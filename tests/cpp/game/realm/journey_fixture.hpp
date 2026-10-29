#pragma once
#include "realmmesh/cluster/etcd_service_registry.hpp"
#include "realmmesh/game/client/login_chain.hpp"
#include "realmmesh/game/client/wire_login_transport.hpp"
#include "realmmesh/game/common/edge_protocol.hpp"
#include "realmmesh/game/common/player_data_store.hpp"
#include "realmmesh/test_support/etcd_process.hpp"
#include "realmmesh/test_support/mongod_process.hpp"
#include "realmmesh/test_support/temporary_directory.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>

/// Realm 最小业务闭环的端到端旅程(#93):客户端库的生产 wire(Login
/// Chain + Realm API)对真实 realm_mesh 进程、真 etcd 与隔离 mongod。
/// 只经公共接口观察;期望值全部来自规格数字(+10 经验、exp/100+1 级、
/// 10 级封顶、每账号 3 个角色)。
namespace realm::game::realm {
namespace {

using namespace std::chrono_literals;
using client::RealmCallStatus;

constexpr std::uint64_t journey_account_id = 7001;
constexpr std::string_view journey_account = "journey";
constexpr std::string_view journey_credential = "journey-secret";
/// configs/common/accounts.lua 的 "pinned" 账号:Lua 首次导入给它一个
/// 同名默认角色,角色 id 即账号 id。
constexpr std::uint64_t other_account_character_id = 4242;
constexpr std::string_view other_account_character_name = "pinned";

/// 拉起 realm_mesh --config <root>,析构时 SIGINT 优雅收尾并回收。
class MeshProcess final {
public:
    explicit MeshProcess(const std::filesystem::path& config_root) {
        pid_ = ::fork();
        if (pid_ < 0) throw std::runtime_error("fork failed");
        if (pid_ == 0) {
            ::execl(
                REALMMESH_MESH_EXECUTABLE,
                REALMMESH_MESH_EXECUTABLE,
                "--config",
                config_root.c_str(),
                static_cast<char*>(nullptr));
            _exit(127);
        }
    }
    ~MeshProcess() { stop(); }
    MeshProcess(const MeshProcess&) = delete;
    MeshProcess& operator=(const MeshProcess&) = delete;

    void crash() noexcept {
        if (pid_ <= 0) return;
        static_cast<void>(::kill(pid_, SIGKILL));
        int status = 0;
        while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
        }
        pid_ = -1;
    }

    void stop() noexcept {
        if (pid_ <= 0) return;
        static_cast<void>(::kill(pid_, SIGINT));
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        int status = 0;
        for (;;) {
            const auto reaped = ::waitpid(pid_, &status, WNOHANG);
            if (reaped == pid_ || (reaped < 0 && errno == ECHILD)) break;
            if (std::chrono::steady_clock::now() >= deadline) {
                static_cast<void>(::kill(pid_, SIGKILL));
                while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
                }
                break;
            }
            std::this_thread::sleep_for(5ms);
        }
        pid_ = -1;
    }

private:
    pid_t pid_{-1};
};

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

void rewrite(
    const std::filesystem::path& path,
    std::string_view from,
    const std::string& to) {
    auto contents = read_file(path);
    const auto position = contents.find(from);
    if (position == std::string::npos) {
        throw std::runtime_error(
            "missing '" + std::string(from) + "' in " + path.string());
    }
    for (auto at = position; at != std::string::npos;
         at = contents.find(from, at + to.size())) {
        contents.replace(at, from.size(), to);
    }
    std::ofstream output(path, std::ios::trunc);
    output << contents;
    if (!output) throw std::runtime_error("cannot rewrite " + path.string());
}

void copy_configs(
    const std::filesystem::path& target, std::string_view main_config) {
    std::filesystem::copy(
        std::filesystem::path(REALMMESH_TEST_SOURCE_DIR) / "configs",
        target,
        std::filesystem::copy_options::recursive);
    std::ofstream output(target / "main.config", std::ios::trunc);
    output << main_config;
}

struct Ports final {
    std::uint16_t login_verify{0};
    std::uint16_t queue{0};
    std::uint16_t gateway{0};
    std::uint16_t realm{0};
};

/// 两棵配置树指向同一组空闲端口、同一个 etcd 与同一个隔离库:
/// 入口树跑 login_verify/queue/gateway,Realm 树单独跑 realm,
/// 这样 Realm 可以独立重启而 Gateway 不受影响。
void prepare_config(
    const std::filesystem::path& root,
    const Ports& ports,
    const std::string& etcd_endpoint,
    const std::string& database,
    const test_support::MongodProcess& mongod) {
    const auto services = root / "services";
    rewrite(
        services / "login_verify.lua",
        "listen_port = 0,",
        "listen_port = " + std::to_string(ports.login_verify) + ",");
    rewrite(
        services / "login_verify.lua",
        "metrics_port = 9104",
        "metrics_port = 0");
    rewrite(
        services / "queue.lua",
        "listen_port = 0,",
        "listen_port = " + std::to_string(ports.queue) + ",");
    rewrite(services / "queue.lua", "metrics_port = 9105", "metrics_port = 0");
    // 放行一次放完、帧间隔收到 1s:旅程要连续登录多次,不测排队节拍。
    rewrite(
        services / "queue.lua", "release_step = 3000", "release_step = 100000");
    rewrite(
        services / "queue.lua",
        "release_interval_seconds = 2",
        "release_interval_seconds = 1");
    rewrite(services / "queue.lua", "http://127.0.0.1:2379", etcd_endpoint);
    rewrite(
        services / "gateway.lua",
        "listen_port = 8000",
        "listen_port = " + std::to_string(ports.gateway));
    rewrite(
        services / "gateway.lua",
        "downstream_port = 7100",
        "downstream_port = " + std::to_string(ports.realm));
    rewrite(
        services / "gateway.lua", "metrics_port = 9103", "metrics_port = 0");
    rewrite(
        services / "realm.lua",
        "listen_port = 7100",
        "listen_port = " + std::to_string(ports.realm));
    rewrite(services / "realm.lua", "metrics_port = 9102", "metrics_port = 0");
    rewrite(
        root / "common" / "discovery.lua",
        "http://127.0.0.1:2379",
        etcd_endpoint);
    test_support::point_player_data_at(root, mongod, database);
}

void set_environment(const char* name, std::string_view value) {
    if (::setenv(name, std::string(value).c_str(), 1) != 0) {
        throw std::runtime_error(std::string("cannot set ") + name);
    }
}

/// 被拉起进程继承的密钥材料,取值与 new_chain_flow_test 同一组。
class ScopedServiceEnvironment final {
public:
    ScopedServiceEnvironment() {
        set_environment(
            "REALMMESH_IDENTITY_KEY_SEED",
            "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
        set_environment(
            "REALMMESH_QUEUE_NUMBER_KEY_SEED",
            "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb");
        set_environment(
            "REALMMESH_ADMISSION_GRANT_KEY_SEED",
            "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
        set_environment(
            "REALMMESH_ADMISSION_GRANT_PUBLIC_KEY",
            "207a067892821e25d770f1fba0c47c11ff4b813e54162ece9eb839e076231ab6");
        set_environment(
            "REALMMESH_ADMISSION_CONSUMPTION_DIGEST_KEY",
            "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210");
        set_environment(
            "REALMMESH_SESSION_TICKET_KEY",
            "0102030405060708090a0b0c0d0e0f10"
            "1112131415161718191a1b1c1d1e1f20");
        set_environment(
            "REALMMESH_TLS_CERTIFICATE_FILE", REALMMESH_TEST_TLS_CERTIFICATE);
        set_environment(
            "REALMMESH_TLS_PRIVATE_KEY_FILE", REALMMESH_TEST_TLS_PRIVATE_KEY);
    }
    ~ScopedServiceEnvironment() {
        for (const char* name :
             {"REALMMESH_IDENTITY_KEY_SEED",
              "REALMMESH_QUEUE_NUMBER_KEY_SEED",
              "REALMMESH_ADMISSION_GRANT_KEY_SEED",
              "REALMMESH_ADMISSION_GRANT_PUBLIC_KEY",
              "REALMMESH_ADMISSION_CONSUMPTION_DIGEST_KEY",
              "REALMMESH_SESSION_TICKET_KEY",
              "REALMMESH_TLS_CERTIFICATE_FILE",
              "REALMMESH_TLS_PRIVATE_KEY_FILE"}) {
            static_cast<void>(::unsetenv(name));
        }
    }
    ScopedServiceEnvironment(const ScopedServiceEnvironment&) = delete;
    ScopedServiceEnvironment& operator=(const ScopedServiceEnvironment&) =
        delete;
};

[[nodiscard]] std::string base64(std::string_view input) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    for (std::size_t offset = 0; offset < input.size(); offset += 3U) {
        std::uint32_t value = 0;
        for (std::size_t index = 0; index < 3U; ++index) {
            value <<= 8U;
            if (offset + index < input.size()) {
                value |= static_cast<unsigned char>(input[offset + index]);
            }
        }
        const auto remaining = input.size() - offset;
        for (std::size_t index = 0; index < 4U; ++index) {
            output.push_back(
                index <= remaining
                    ? alphabet[(value >> (18U - 6U * index)) & 0x3FU]
                    : '=');
        }
    }
    return output;
}

/// 队列按 etcd 里的服务额度放行:没有额度键就一个也不放。给本拓扑的
/// gateway 与 realm 实例写足额度(取值同 loadgen 集成用例)。
void seed_admission_budgets(const std::string& endpoint) {
    const auto client = cluster::make_etcd_http_client(endpoint, 500ms);
    const std::pair<std::string_view, std::string_view> budgets[] = {
        {"/realmmesh/budgets/service/gateway/gateway-dev-01",
         R"({"conn_free":100000,"fetch_free":100000})"},
        {"/realmmesh/budgets/service/realm/realm-dev-01",
         R"({"conn_free":100000})"},
    };
    for (const auto& [key, value] : budgets) {
        const auto deadline = std::chrono::steady_clock::now() + 15s;
        std::string error;
        while (true) {
            const auto response = client->post(
                "/v3/kv/put",
                nlohmann::json{{"key", base64(key)}, {"value", base64(value)}}
                    .dump(),
                &error);
            if (response.has_value() &&
                response->find("\"error\"") == std::string::npos) {
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error(
                    "cannot seed " + std::string(key) + ": " + error);
            }
            std::this_thread::sleep_for(50ms);
        }
    }
}

void wait_for_port(std::uint16_t port, std::string_view description) {
    for (int attempt = 0; attempt < 1000; ++attempt) {
        if (test_support::loopback_port_open(port)) return;
        std::this_thread::sleep_for(10ms);
    }
    throw std::runtime_error(
        std::string(description) + " did not open its port");
}

[[nodiscard]] client::TimePoint soon() { return client::Clock::now() + 5s; }

/// retry_unavailable 单次尝试的截止。共享副本集上服务端最慢的回包是
/// 一次写关注超时(4.5 秒)后 with_transaction 自动重试提交，或一次读
/// 卡满套接字超时(7 秒);截止须晚于两者，服务端的 3010 或成功回包才
/// 先于客户端的 Disconnected 到达。层级见 point_player_data_at。
[[nodiscard]] client::TimePoint data_call_deadline() {
    return client::Clock::now() + 12s;
}

/// 3010 表示玩家数据暂不可用、可重试：全量 CTest 下共享 mongod 偶发的
/// 写关注等待超时不是业务失败。在一个 soon() 窗口内以同一请求重放,
/// 每次尝试各自拿 data_call_deadline() 作截止。call(deadline) 必须按 seq
/// 幂等：首次写若已落库，重放命中同一 seq,服务回 replayed 结果而不是再
/// 推进一次。saw_unavailable 非空时记下是否至少观察到一次 3010。
template <typename Call>
[[nodiscard]] auto retry_unavailable(
    Call&& call, bool* saw_unavailable = nullptr) {
    const auto unavailable = [](const auto& reply) {
        return reply.status == RealmCallStatus::Rejected &&
               reply.error_code == common::edge_error_realm_data_unavailable;
    };
    const auto window = soon();
    auto reply = call(data_call_deadline());
    if (saw_unavailable != nullptr) *saw_unavailable = false;
    while (unavailable(reply) && client::Clock::now() < window) {
        if (saw_unavailable != nullptr) *saw_unavailable = true;
        std::this_thread::sleep_for(50ms);
        reply = call(data_call_deadline());
    }
    if (saw_unavailable != nullptr && unavailable(reply))
        *saw_unavailable = true;
    return reply;
}

class RealmJourneyTest : public ::testing::Test {
protected:
    RealmJourneyTest()
        : mesh_root_("realm-journey-mesh-"),
          realm_root_("realm-journey-realm-") {}

    /// 派生夹具的服务进程此时都已回收，再删本例的库；独占 mongod 随
    /// 夹具一起销毁，不必清理。shared_mongod_ 只在 SetUp 时确认共享后
    /// 才记下：共享副本集是静态对象，派生夹具成员(如 fault_mongo_)
    /// 先于这里析构也不会悬空。
    ~RealmJourneyTest() override {
        if (shared_mongod_ != nullptr)
            shared_mongod_->drop_database(journey_database_);
    }

    void SetUp() override {
        etcd_.wait_ready();
        seed_admission_budgets(etcd_.endpoint());
        const auto ports = test_support::unused_loopback_ports(4);
        ports_ = Ports{ports.at(0), ports.at(1), ports.at(2), ports.at(3)};
        const auto database = test_support::MongodProcess::fresh_database();
        if (player_data().is_shared()) shared_mongod_ = &player_data();
        journey_database_ = database;
        copy_configs(
            mesh_root_.path(),
            "return { services = { { name = \"login_verify\" }, "
            "{ name = \"queue\" }, "
            "{ name = \"gateway\", entry = true } } }");
        copy_configs(
            realm_root_.path(),
            "return { services = { { name = \"realm\" } } }");
        prepare_config(
            mesh_root_.path(),
            ports_,
            etcd_.endpoint(),
            database,
            player_data());
        prepare_config(
            realm_root_.path(),
            ports_,
            etcd_.endpoint(),
            database,
            player_data());

        // Provisioning:先完成开发账号的 Lua 首次导入(给 "pinned" 一个默认
        // 角色),再建一个没有角色的旅程账号。
        game::common::MongoPlayerDataOptions options;
        options.bootstrap_accounts_file =
            std::filesystem::path(REALMMESH_TEST_SOURCE_DIR) / "configs" /
            "common" / "accounts.lua";
        game::common::MongoPlayerDataStore provisioning(
            player_data().uri(), database, options);
        provisioning.provision_account(
            game::common::AccountProvisioning{
                .account_id = journey_account_id,
                .account_name = std::string(journey_account),
                .credential = std::string(journey_credential),
                .banned = false,
                .whitelisted = true});

        prepare_services(database);
        start_realm();
        mesh_.emplace(mesh_root_.path());
        wait_for_port(ports_.gateway, "gateway");

        client::WireEndpoints endpoints;
        endpoints.login_verify_port = ports_.login_verify;
        endpoints.queue_port = ports_.queue;
        endpoints.verify_peer = false;
        transport_.emplace(
            endpoints, redeemer_, client::WireTransportOptions{});
    }

    void TearDown() override {
        transport_.reset();
        mesh_.reset();
        realm_.reset();
    }

    virtual const test_support::MongodProcess& player_data() const {
        return test_support::MongodProcess::shared();
    }
    virtual void prepare_services(const std::string&) {}

    void start_realm() {
        realm_.emplace(realm_root_.path());
        wait_for_port(ports_.realm, "realm");
    }

    /// 优雅停掉 Realm 进程(waitpid 回收后端口已释放),再在同一端口拉起。
    void restart_realm() {
        realm_.reset();
        start_realm();
    }

    /// 走完整 Login Chain 直到 Realm 入场,返回已认证的 Realm Session。
    [[nodiscard]] std::unique_ptr<client::RealmSession> login(
        std::string_view account = journey_account,
        std::string_view credential = journey_credential,
        client::LoginChainTransport* isolated_transport = nullptr) {
        client::LoginChainConfig config;
        config.gateway_endpoints.push_back(
            network::client::EndpointCandidate{
                .protocol = network::TransportProtocol::TlsTcp,
                .host = "127.0.0.1",
                .port = ports_.gateway,
                .priority = 1,
            });
        client::LoginChain chain(
            isolated_transport ? *isolated_transport : *transport_,
            std::move(config));
        auto result = chain.run(
            client::LoginRun::full(
                std::string(account),
                std::string(credential),
                client::Clock::now() + 20s));
        if (!result.succeeded()) {
            ADD_FAILURE() << "login failed at stage "
                          << static_cast<int>(result.failure()->stage) << ": "
                          << result.failure()->detail;
            return nullptr;
        }
        auto* full = std::get_if<client::FullSuccess>(result.success());
        if (full == nullptr) {
            ADD_FAILURE() << "login did not reach the Realm";
            return nullptr;
        }
        return std::move(full->session);
    }

    test_support::EtcdProcess etcd_;
    test_support::TemporaryDirectory mesh_root_;
    test_support::TemporaryDirectory realm_root_;
    Ports ports_;
    ScopedServiceEnvironment environment_;
    std::optional<MeshProcess> realm_;
    std::optional<MeshProcess> mesh_;
    client::WireEnterRealmRedeemer redeemer_;
    std::optional<client::WireLoginTransport> transport_;
    const test_support::MongodProcess* shared_mongod_{nullptr};
    std::string journey_database_;
};

}  // namespace
}  // namespace realm::game::realm
