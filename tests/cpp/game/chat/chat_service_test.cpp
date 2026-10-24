#include "chat_test_messages.hpp"
#include "realmmesh/cluster/etcd_service_registry.hpp"
#include "realmmesh/network/client/edge_client_connection.hpp"
#include "realmmesh/test_support/etcd_process.hpp"
#include "realmmesh/test_support/temporary_directory.hpp"
#include <gtest/gtest.h>
#include <httplib.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <chrono>
#include <fstream>
#include <map>
#include <thread>
using namespace std::chrono_literals;
namespace realm::game::chat {
namespace {
namespace net = ::realm::network::client;
using namespace test;
std::string certificate(const char* file) {
    return (std::filesystem::path(REALMMESH_TEST_SERVICE_TLS_DIRECTORY) / file).string();
}
void rewrite(const std::filesystem::path& file, std::string_view from, std::string_view to) {
    std::ifstream in(file); std::string data((std::istreambuf_iterator<char>(in)), {});
    const auto at = data.find(from);
    if (at == std::string::npos) throw std::runtime_error("missing test configuration key");
    data.replace(at, from.size(), to); std::ofstream(file) << data;
}
class Process {
public:
    explicit Process(const std::filesystem::path& config) {
        const auto log = config / "process.log";
        pid_ = ::fork();
        if (pid_ < 0) throw std::runtime_error("fork failed");
        if (pid_ == 0) {
            const auto out = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (out >= 0) { ::dup2(out, STDOUT_FILENO); ::dup2(out, STDERR_FILENO); ::close(out); }
            ::setenv("REALMMESH_CHAT_CERTIFICATE_FILE", certificate("chat.pem").c_str(), 1);
            ::setenv("REALMMESH_CHAT_PRIVATE_KEY_FILE", certificate("chat.key").c_str(), 1);
            ::setenv("REALMMESH_SERVICE_CA_FILE", certificate("ca.pem").c_str(), 1);
            ::execl(REALMMESH_MESH_EXECUTABLE, REALMMESH_MESH_EXECUTABLE,
                "--config", config.c_str(), "--service", "chat", static_cast<char*>(nullptr));
            _exit(127);
        }
    }
    ~Process() { stop(); }
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    void crash() {
        if (pid_ <= 0) return;
        ::kill(pid_, SIGKILL); int status{};
        while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
        pid_ = -1;
    }
    bool stop() {
        if (pid_ <= 0) return true;
        ::kill(pid_, SIGINT);
        const auto until = ChatChannels::Clock::now() + 3s;
        int status{};
        while (ChatChannels::Clock::now() < until) {
            const auto result = ::waitpid(pid_, &status, WNOHANG);
            if (result == pid_ || (result < 0 && errno == ECHILD)) {
                pid_ = -1; return WIFEXITED(status) && WEXITSTATUS(status) == 0;
            }
            std::this_thread::sleep_for(5ms);
        }
        crash(); return false;
    }
private:
    pid_t pid_{-1};
};
class ChatServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto ports = test_support::unused_loopback_ports(2); port_ = ports[0]; metrics_ = ports[1];
        std::filesystem::copy(std::filesystem::path(REALMMESH_TEST_SOURCE_DIR) / "configs", root_.path(),
            std::filesystem::copy_options::recursive);
        std::ofstream(root_.path() / "main.config") << "return {services = {{name = 'chat'}}}";
        rewrite(root_.path() / "services/chat.lua", "listen_port = 7300", "listen_port = " + std::to_string(port_));
        rewrite(root_.path() / "services/chat.lua", "metrics_port = 9107", "metrics_port = " + std::to_string(metrics_));
    }
    void TearDown() override {
        if (process_) EXPECT_TRUE(process_->stop()); process_.reset();
        EXPECT_FALSE(test_support::loopback_port_open(port_)); EXPECT_FALSE(test_support::loopback_port_open(metrics_));
    }
    void start() {
        process_ = std::make_unique<Process>(root_.path());
        const auto until = ChatChannels::Clock::now() + 5s;
        while (ChatChannels::Clock::now() < until) {
            if (test_support::loopback_port_open(port_)) return;
            std::this_thread::sleep_for(10ms);
        }
        std::ifstream log(root_.path() / "process.log");
        throw std::runtime_error("Chat startup failed: " + std::string((std::istreambuf_iterator<char>(log)), {}));
    }
    net::TlsClientOptions options() {
        net::TlsClientOptions c; c.alpn = common::chat_internal_alpn; c.ca_file = certificate("ca.pem");
        c.server_identity = "chat-dev-01.chat.service";
        c.certificate_chain_file = certificate("realm.pem"); c.private_key_file = certificate("realm.key");
        return c;
    }
    std::shared_ptr<net::ISecureByteStream> dial(net::TlsClientOptions c) {
        return net::TlsClientStream::dial("127.0.0.1", port_, c, ChatChannels::Clock::now() + 1s).stream;
    }
    common::ChatInternalReply call(net::EdgeClientConnection& client, const common::ChatInternalRequest& r) {
        const auto deadline = ChatChannels::Clock::now() + 2s;
        if (!client.send_frame(common::encode(r, ++request_id_), deadline)) throw std::runtime_error("send failed");
        // 调用时必须先消费此前的通知，不能将回执关联与发送序号混淆。
        const auto bytes = client.receive_frame(deadline);
        if (!bytes) throw std::runtime_error("missing Chat reply");
        const auto reply = common::decode_chat_internal_reply(*bytes);
        if (!reply || common::chat_internal_request_id(*bytes) != request_id_) throw std::runtime_error("invalid Chat reply");
        return *reply;
    }
    std::string metrics() {
        httplib::Client client("127.0.0.1", metrics_);
        client.set_connection_timeout(1, 0); client.set_read_timeout(1, 0);
        const auto r = client.Get("/metrics"); return r ? r->body : "";
    }
    test_support::TemporaryDirectory root_{"realmmesh-chat-c2-"};
    std::unique_ptr<Process> process_;
    std::uint16_t port_{}, metrics_{};
    std::uint64_t request_id_{};
};
TEST_F(ChatServiceTest, ProductionModuleOrdersTwoCharactersReplaysOnceAndPublishesFiniteMetrics) {
    start(); const auto stream = dial(options()); ASSERT_TRUE(stream); net::EdgeClientConnection client(stream);
    const auto link = call(client, hello()).link(); const auto a = member(1), b = member(2, 8);
    ASSERT_EQ(call(client, request(link, wire::OPERATION_JOIN, a)).status(), wire::STATUS_OK);
    ASSERT_EQ(call(client, request(link, wire::OPERATION_JOIN, b)).status(), wire::STATUS_OK);
    const auto first = call(client, send(link, a)); EXPECT_EQ(first.result().receipt().channel_seq(), 1);
    for (int i = 0; i < 2; ++i) {
        const auto bytes = client.receive_frame(ChatChannels::Clock::now() + 1s); ASSERT_TRUE(bytes);
        const auto push = common::decode_chat_internal_push(*bytes); ASSERT_TRUE(push);
        EXPECT_EQ(push->message().channel_seq(), 1); EXPECT_EQ(push->message().sender_character_id(), 7);
    }
    EXPECT_EQ(call(client, send(link, a)).result().receipt().channel_seq(), 1);
    const auto state = call(client, request(link, wire::OPERATION_STATE, a));
    EXPECT_EQ(state.state().join_watermark(), 0); EXPECT_EQ(state.state().last_client_send_seq(), 1);
    EXPECT_EQ(call(client, send(link, b)).result().receipt().channel_seq(), 2);
    for (int i = 0; i < 2; ++i) {
        const auto bytes = client.receive_frame(ChatChannels::Clock::now() + 1s);
        ASSERT_TRUE(bytes); ASSERT_TRUE(common::decode_chat_internal_push(*bytes));
    }
    EXPECT_EQ(call(client, send(link, member(99))).status(), wire::STATUS_STALE_MEMBERSHIP);
    std::string body;
    const auto until = ChatChannels::Clock::now() + 2s;
    do { body = metrics(); if (body.find("chat_receipt_replays_total 1") != std::string::npos) break; } while (ChatChannels::Clock::now() < until);
    EXPECT_NE(body.find("chat_receipt_replays_total 1"), std::string::npos);
    EXPECT_NE(body.find("chat_queue_rejected_total 0"), std::string::npos);
    EXPECT_NE(body.find("chat_send_total{outcome=\"rejected\"} 1"), std::string::npos);
    EXPECT_NE(body.find("chat_send_duration_seconds_count{outcome=\"accepted\"} 3"), std::string::npos);
    EXPECT_EQ(body.find("你好"), std::string::npos);
}
TEST_F(ChatServiceTest, WrongCertificateCaServerIdentityAndAlpnNeverReachHello) {
    start(); const auto good = options();
    std::vector<net::TlsClientOptions> invalid;
    auto c = good; c.certificate_chain_file.clear(); c.private_key_file.clear(); invalid.push_back(c);
    c = good; c.certificate_chain_file = certificate("other.pem"); c.private_key_file = certificate("other.key"); invalid.push_back(c);
    c = good; c.ca_file = REALMMESH_TEST_TLS_CERTIFICATE; invalid.push_back(c);
    c = good; c.server_identity = "scene-dev-01.scene.service"; invalid.push_back(c);
    c = good; c.alpn = "realmmesh-edge/1"; invalid.push_back(c);
    c = good; c.certificate_chain_file = REALMMESH_TEST_TLS_CERTIFICATE; c.private_key_file = REALMMESH_TEST_TLS_PRIVATE_KEY; invalid.push_back(c);
    for (const auto& option : invalid) {
        const auto stream = dial(option); if (!stream) continue;
        net::EdgeClientConnection client(stream); const auto deadline = ChatChannels::Clock::now() + 500ms;
        if (!client.send_frame(common::encode(hello(), 1), deadline)) continue;
        EXPECT_FALSE(client.receive_frame(deadline));
    }
    const auto stream = dial(good); ASSERT_TRUE(stream); net::EdgeClientConnection client(stream);
    EXPECT_EQ(call(client, hello()).status(), wire::STATUS_OK);
}
TEST_F(ChatServiceTest, AuthenticatedWrongRealmInstanceAndClientWireAreRejected) {
    start();
    for (int mode = 0; mode < 3; ++mode) {
        const auto stream = dial(options()); ASSERT_TRUE(stream); net::EdgeClientConnection client(stream);
        auto r = hello();
        if (mode == 0) r.mutable_link()->set_realm_id(2);
        if (mode == 1) r.mutable_link()->set_realm_instance_id("another-realm");
        if (mode == 2) r.mutable_link()->set_chat_instance_id("another-chat");
        EXPECT_EQ(call(client, r).status(), wire::STATUS_FORBIDDEN);
    }
    const auto stream = dial(options()); ASSERT_TRUE(stream); net::EdgeClientConnection client(stream);
    common::GetChatState get; get.set_kind(wire::CHANNEL_KIND_REALM);
    ASSERT_TRUE(client.send_frame(common::encode(get, 1), ChatChannels::Clock::now() + 1s));
    EXPECT_FALSE(client.receive_frame(ChatChannels::Clock::now() + 1s));
}
TEST_F(ChatServiceTest, NewAuthenticatedLinkRejectsOldQualificationAndSurvivesLateDisconnect) {
    start(); auto old_stream = dial(options()); ASSERT_TRUE(old_stream); net::EdgeClientConnection old(old_stream);
    const auto old_link = call(old, hello()).link();
    ASSERT_EQ(call(old, request(old_link, wire::OPERATION_JOIN, member(1))).status(), wire::STATUS_OK);
    const auto stream = dial(options()); ASSERT_TRUE(stream); net::EdgeClientConnection client(stream);
    const auto link = call(client, hello()).link(); EXPECT_GT(link.link_generation(), old_link.link_generation());
    EXPECT_FALSE(old.receive_frame(ChatChannels::Clock::now() + 1s)); old_stream->shutdown();
    EXPECT_EQ(call(client, send(old_link, member(1))).status(), wire::STATUS_STALE_LINK);
    EXPECT_EQ(call(client, request(link, wire::OPERATION_JOIN, member(1))).status(), wire::STATUS_OK);
    EXPECT_EQ(call(client, send(link, member(1))).result().receipt().channel_seq(), 1);
}
TEST_F(ChatServiceTest, ActualTlsOutboundRejectionClosesLinkWithoutRollingBackAcceptedReceipt) {
    rewrite(root_.path() / "services/chat.lua", "max_pending_output_bytes = 1048576", "max_pending_output_bytes = 512");
    start(); const auto stream = dial(options()); ASSERT_TRUE(stream); net::EdgeClientConnection client(stream);
    const auto link = call(client, hello()).link();
    ASSERT_EQ(call(client, request(link, wire::OPERATION_JOIN, member(1))).status(), wire::STATUS_OK);
    EXPECT_EQ(call(client, send(link, member(1), 1, std::string(512, 'x'))).result().outcome(), wire::OUTCOME_ACCEPTED);
    EXPECT_FALSE(client.receive_frame(ChatChannels::Clock::now() + 1s));
    const auto fresh_stream = dial(options()); ASSERT_TRUE(fresh_stream); net::EdgeClientConnection fresh(fresh_stream);
    const auto fresh_link = call(fresh, hello()).link();
    const auto joined = call(fresh, request(fresh_link, wire::OPERATION_JOIN, member(1)));
    EXPECT_EQ(joined.state().join_watermark(), 1); EXPECT_FALSE(joined.state().has_last_receipt());
}
TEST_F(ChatServiceTest, RealDiscoveryRegistrationCrashLeaseExpiryAndSameNameRestartGetFreshEpoch) {
    test_support::EtcdProcess etcd; etcd.wait_ready();
    rewrite(root_.path() / "common/discovery.lua", "enabled = false", "enabled = true");
    rewrite(root_.path() / "common/discovery.lua", "required = false", "required = true");
    rewrite(root_.path() / "common/discovery.lua", "http://127.0.0.1:2379", etcd.endpoint());
    start(); cluster::EtcdServiceRegistry registry({.endpoint = etcd.endpoint(), .background_maintenance = false});
    auto until = ChatChannels::Clock::now() + 5s;
    while (registry.discover("chat").empty() && ChatChannels::Clock::now() < until) std::this_thread::sleep_for(10ms);
    const auto discovered = registry.discover("chat"); ASSERT_EQ(discovered.size(), 1);
    EXPECT_EQ(discovered[0].instance_id, "chat-dev-01"); ASSERT_EQ(discovered[0].endpoints.size(), 1);
    EXPECT_EQ(discovered[0].endpoints[0].port, port_);
    const auto stream = dial(options()); ASSERT_TRUE(stream); net::EdgeClientConnection client(stream);
    const auto old = call(client, hello()).link();
    ASSERT_EQ(call(client, request(old, wire::OPERATION_JOIN, member(1))).status(), wire::STATUS_OK);
    ASSERT_EQ(call(client, send(old, member(1))).result().outcome(), wire::OUTCOME_ACCEPTED);
    process_->crash(); EXPECT_FALSE(test_support::loopback_port_open(port_));
    until = ChatChannels::Clock::now() + 10s;
    while (!registry.discover("chat").empty() && ChatChannels::Clock::now() < until) std::this_thread::sleep_for(20ms);
    ASSERT_TRUE(registry.discover("chat").empty());
    start(); const auto new_stream = dial(options()); ASSERT_TRUE(new_stream); net::EdgeClientConnection fresh(new_stream);
    const auto current = call(fresh, hello()).link(); EXPECT_NE(current.chat_incarnation(), old.chat_incarnation());
    EXPECT_EQ(call(fresh, send(old, member(1))).status(), wire::STATUS_STALE_LINK);
    const auto joined = call(fresh, request(current, wire::OPERATION_JOIN, member(1)));
    EXPECT_EQ(joined.state().join_watermark(), 0); EXPECT_FALSE(joined.state().has_last_receipt());
    EXPECT_TRUE(process_->stop()); EXPECT_TRUE(registry.discover("chat").empty());
    etcd.stop();
}
TEST_F(ChatServiceTest, MissingHelloExpiresAndNormalStopReapsProcessAndListeningPorts) {
    start(); const auto stream = dial(options()); ASSERT_TRUE(stream);
    std::array<std::byte, 16> data{};
    const auto before = ChatChannels::Clock::now();
    const auto read = stream->read_some(data, before + 3s);
    EXPECT_TRUE(!read || *read == 0);
    EXPECT_GE(ChatChannels::Clock::now() - before, 500ms);
    EXPECT_LT(ChatChannels::Clock::now() - before, 2500ms);
    EXPECT_TRUE(process_->stop()); EXPECT_FALSE(test_support::loopback_port_open(port_));
    EXPECT_FALSE(test_support::loopback_port_open(metrics_));
}
}  // namespace
}  // namespace realm::game::chat
