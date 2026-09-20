/// 排队调度服集成测试(唯一动 socket 的缝):真实 TLS loopback,服务绑
/// 内核临时端口、std::jthread 驱动 tick;主线程自建 OpenSSL 客户端(先例:
/// login_verify_service_test)走完整 HTTPS 请求。etcd 存取用 fake store
/// 注入,验证放行阀门帧、fail-closed 与冷备恢复;真实 etcd 路径由
/// queue_store_test 覆盖。

#include "realmmesh/game/queue/queue_service.hpp"

#include "realmmesh/game/common/identity_token.hpp"
#include "realmmesh/game/common/json.hpp"
#include "realmmesh/observability/metrics_registry.hpp"

#include <gtest/gtest.h>
#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace realm::game::queue {
namespace {

using common::IdentityClaims;
using common::IdentityTokenCodec;
using common::JsonCodec;

class Descriptor final {
public:
    explicit Descriptor(int value)
        : value_(value) {}
    ~Descriptor() {
        if (value_ >= 0) ::close(value_);
    }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    [[nodiscard]] int get() const noexcept { return value_; }

private:
    int value_;
};

struct SslContextDeleter {
    void operator()(SSL_CTX* value) const noexcept { SSL_CTX_free(value); }
};
struct SslDeleter {
    void operator()(SSL* value) const noexcept { SSL_free(value); }
};

using SslContextPtr = std::unique_ptr<SSL_CTX, SslContextDeleter>;
using SslPtr = std::unique_ptr<SSL, SslDeleter>;

void write_all(SSL* ssl, std::string_view bytes) {
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        std::size_t written = 0;
        ASSERT_EQ(SSL_write_ex(ssl, data + offset, bytes.size() - offset, &written), 1);
        offset += written;
    }
}

/// 读完响应头后按 Content-Length 读取(keep-alive 服务端不会主动关连接)。
[[nodiscard]] std::string read_response(SSL* ssl) {
    std::string response;
    char buffer[4096];
    std::size_t header_end = std::string::npos;
    while (header_end == std::string::npos) {
        std::size_t received = 0;
        if (SSL_read_ex(ssl, buffer, sizeof(buffer), &received) != 1) {
            return response;
        }
        response.append(buffer, received);
        header_end = response.find("\r\n\r\n");
    }
    const std::string needle = "Content-Length:";
    std::size_t length_field = response.find(needle);
    if (length_field == std::string::npos) return response;
    const std::size_t value_start =
        response.find_first_not_of(" \t", length_field + needle.size());
    const std::size_t value_end = response.find("\r\n", value_start);
    const std::size_t content_length =
        static_cast<std::size_t>(std::stoul(response.substr(
            value_start, value_end - value_start)));
    const std::size_t body_start = header_end + 4;
    while (response.size() < body_start + content_length) {
        std::size_t received = 0;
        if (SSL_read_ex(ssl, buffer, sizeof(buffer), &received) != 1) break;
        response.append(buffer, received);
    }
    return response;
}

/// 一条 TLS 连接上完成一次请求-响应;失败返回空(用例侧断言有值)。
[[nodiscard]] std::optional<std::string> https_exchange(
    std::uint16_t port, std::string_view request) {
    SslContextPtr context(SSL_CTX_new(TLS_client_method()));
    SSL_CTX_set_verify(context.get(), SSL_VERIFY_NONE, nullptr);
    SslPtr ssl(SSL_new(context.get()));
    // 服务端握手要求协商出 ALPN(http/1.1),不带即被回绝关闭。
    static constexpr std::array<unsigned char, 9> alpn{
        8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
    EXPECT_EQ(SSL_set_alpn_protos(ssl.get(), alpn.data(), alpn.size()), 0);

    const Descriptor socket(::socket(AF_INET, SOCK_STREAM, 0));
    EXPECT_GE(socket.get(), 0);
    if (socket.get() < 0) return std::nullopt;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const int connected = ::connect(
        socket.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address));
    EXPECT_EQ(connected, 0);
    if (connected != 0) return std::nullopt;
    const bool attached = SSL_set_fd(ssl.get(), socket.get()) == 1;
    EXPECT_TRUE(attached);
    if (!attached) return std::nullopt;
    if (SSL_connect(ssl.get()) != 1) {
        ADD_FAILURE() << "TLS handshake failed";
        return std::nullopt;
    }
    write_all(ssl.get(), request);
    return read_response(ssl.get());
}

[[nodiscard]] std::string get_request(
    std::string_view target, const std::optional<std::string>& bearer = std::nullopt) {
    std::string request;
    request += "GET ";
    request += target;
    request += " HTTP/1.1\r\n";
    request += "Host: localhost\r\n";
    if (bearer.has_value()) {
        request += "Authorization: Bearer " + *bearer + "\r\n";
    }
    request += "Connection: close\r\n";
    request += "\r\n";
    return request;
}

[[nodiscard]] std::string_view body_of(const std::string& response) {
    const auto header_end = response.find("\r\n\r\n");
    return header_end == std::string::npos
               ? std::string_view{}
               : std::string_view{response}.substr(header_end + 4);
}

[[nodiscard]] int status_of(const std::string& response) {
    return std::stoi(response.substr(response.find(' ') + 1, 3));
}

/// 测试注缝:额度/快照由用例直控;save 调用按序记录供断言。tick 线程
/// 与主线程并发访问,全部经互斥锁。
class FakeStore final : public QueueStateStore {
public:
    void set_budgets(std::optional<BudgetAggregate> value) {
        const std::scoped_lock lock(mutex_);
        budgets_ = std::move(value);
    }

    void set_snapshot(std::optional<QueueSnapshot> value) {
        const std::scoped_lock lock(mutex_);
        snapshot_ = std::move(value);
    }

    void set_save_success(bool value) {
        const std::scoped_lock lock(mutex_);
        save_success_ = value;
    }

    [[nodiscard]] std::size_t save_attempts() const {
        const std::scoped_lock lock(mutex_);
        return save_attempts_;
    }

    [[nodiscard]] std::vector<QueueSnapshot> saved() const {
        const std::scoped_lock lock(mutex_);
        return saved_;
    }

    [[nodiscard]] std::optional<BudgetAggregate> refresh_budgets() const override {
        const std::scoped_lock lock(mutex_);
        return budgets_;
    }

    [[nodiscard]] std::optional<QueueSnapshot> load_snapshot() const override {
        const std::scoped_lock lock(mutex_);
        return snapshot_;
    }

    [[nodiscard]] bool save_snapshot(
        const QueueSnapshot& snapshot_value,
        std::int64_t updated_at_seconds) const override {
        const std::scoped_lock lock(mutex_);
        static_cast<void>(updated_at_seconds);
        ++save_attempts_;
        if (!save_success_) return false;
        saved_.push_back(snapshot_value);
        return true;
    }

private:
    mutable std::mutex mutex_;
    std::optional<BudgetAggregate> budgets_;
    std::optional<QueueSnapshot> snapshot_;
    mutable std::vector<QueueSnapshot> saved_;
    mutable std::size_t save_attempts_{0};
    bool save_success_{true};
};

class QueueServiceTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        ASSERT_EQ(
            ::setenv(
                "REALMMESH_IDENTITY_KEY_SEED",
                std::string{kSeedHex}.c_str(), 1),
            0);
        ASSERT_EQ(
            ::setenv(
                "REALMMESH_QUEUE_KEY_SEED",
                std::string{kSeedHex}.c_str(), 1),
            0);
    }

    static void TearDownTestSuite() {
        static_cast<void>(::unsetenv("REALMMESH_IDENTITY_KEY_SEED"));
        static_cast<void>(::unsetenv("REALMMESH_QUEUE_KEY_SEED"));
    }

    void SetUp() override {
        store_ = std::make_shared<FakeStore>();
        QueueConfig config;
        config.listen_address = "127.0.0.1";
        config.listen_port = 0;
        config.kid = std::string{kKid};
        config.identity_kid = std::string{kIdentityKid};
        config.release_step = 100;
        config.release_interval = std::chrono::milliseconds{20};
        config.budget_interval = std::chrono::milliseconds{20};
        config.tls = network::TransportConfig::TlsServerIdentity{
            .certificate_chain_file = REALMMESH_TEST_TLS_CERTIFICATE,
            .private_key_file = REALMMESH_TEST_TLS_PRIVATE_KEY,
            .alpn = "http/1.1"};
        service_ = std::make_unique<QueueService>(
            std::move(config), store_, &metrics_);
        service_->start();
        driver_ = std::jthread([this] {
            while (!stopping_.load()) {
                service_->tick();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        port_ = service_->local_endpoints().at(0).port;
    }

    void TearDown() override {
        stopping_.store(true);
        if (driver_.joinable()) driver_.join();
        service_->stop();
        service_.reset();
    }

    /// 服务侧身份验签键的测试侧对偶(同种子同 kid)。
    [[nodiscard]] IdentityTokenCodec identity_codec() const {
        return IdentityTokenCodec(
            common::parse_identity_seed_hex(kSeedHex),
            std::string{kIdentityKid});
    }

    [[nodiscard]] common::QueueNumberCodec number_codec() const {
        return common::QueueNumberCodec(
            common::parse_identity_seed_hex(kSeedHex), std::string{kKid});
    }

    /// jti 需 32 字符小写 hex;suffix 变体保证不同身份。
    [[nodiscard]] static std::string jti(std::string_view suffix) {
        std::string hex(32 - suffix.size(), 'a');
        hex += suffix;
        return hex;
    }

    [[nodiscard]] std::string identity_token(std::string_view jti_value) const {
        const auto now = std::chrono::system_clock::now();
        return identity_codec().issue(IdentityClaims{
            .issuer = "realmmesh/login-verify",
            .account_id = 4242,
            .jti = std::string{jti_value},
            .issued_at = now,
            .expires_at = now + std::chrono::seconds{1800},
        });
    }

    [[nodiscard]] std::optional<std::string> post_number(
        std::string_view suffix) const {
        std::string body;
        body += "POST /v1/queue/tickets HTTP/1.1\r\n";
        body += "Host: localhost\r\n";
        body += "Authorization: Bearer " + identity_token(jti(suffix)) + "\r\n";
        body += "Content-Length: 0\r\n";
        body += "Connection: close\r\n";
        body += "\r\n";
        const auto exchanged = https_exchange(port_, body);
        if (!exchanged.has_value()) return std::nullopt;
        if (status_of(*exchanged) != 202) return std::nullopt;
        const auto payload = JsonCodec::decode(body_of(*exchanged));
        if (!payload.has_value()) return std::nullopt;
        const auto* token =
            std::get_if<std::string>(&payload->at("queue_number_token"));
        if (token == nullptr) return std::nullopt;
        return *token;
    }

    /// 轮询等待谓词成立(tick 驱动的异步帧不即时)。
    template <typename Predicate>
    static void wait_for(Predicate&& predicate) {
        for (int attempt = 0; attempt < 2000; ++attempt) {
            if (predicate()) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        ADD_FAILURE() << "condition not reached in wait_for";
    }

    static constexpr std::string_view kSeedHex =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    static constexpr std::string_view kKid = "queue-test-1";
    static constexpr std::string_view kIdentityKid = "login-verify-test-1";

    // 注册表声明在首位(成员逆序析构):服务持有的指针最后失效。
    observability::MetricsRegistry metrics_;
    std::shared_ptr<FakeStore> store_;
    std::unique_ptr<QueueService> service_;
    std::jthread driver_;
    std::atomic_bool stopping_{false};
    std::uint16_t port_{0};
};

TEST_F(QueueServiceTest, ServesHealthzOverTls) {
    const auto exchanged = https_exchange(port_, get_request("/healthz"));
    ASSERT_TRUE(exchanged.has_value());
    EXPECT_EQ(status_of(*exchanged), 200);
    EXPECT_EQ(body_of(*exchanged), "ok");
}

TEST_F(QueueServiceTest, IssuesAndReleasesByBudgetFrame) {
    const auto token = post_number("01");
    ASSERT_TRUE(token.has_value());

    // 未放行前:progress 水位 0,号牌查询为排队态。
    const auto before = https_exchange(
        port_, get_request("/v1/queue/tickets/me", token));
    ASSERT_TRUE(before.has_value());
    const auto before_payload = JsonCodec::decode(body_of(*before));
    ASSERT_TRUE(before_payload.has_value());
    EXPECT_EQ(
        std::get<std::string>(before_payload->at("status")), "queued");

    store_->set_budgets(
        BudgetAggregate{.gateway_admission = 100, .realm_connections = 100});
    wait_for([this] { return !store_->saved().empty(); });

    // 放行帧落快照:released 1(只发了 1 号,被已发余量封顶)。
    const auto saved = store_->saved();
    ASSERT_EQ(saved.size(), 1U);
    EXPECT_EQ(saved.at(0).released_number, 1U);
    EXPECT_EQ(saved.at(0).next_number, 2U);
    ASSERT_EQ(saved.at(0).release_batches.size(), 1U);
    EXPECT_EQ(saved.at(0).release_batches.front().first_number, 1U);
    EXPECT_EQ(saved.at(0).release_batches.front().last_number, 1U);

    // 放行后:号牌查询重签放行凭证(spec:凭证嵌套 admit_grant,外层
    // 只带状态/位次/估时;JsonCodec 只编扁平对象,以尾段与嵌套截取核验)。
    const auto after = https_exchange(
        port_, get_request("/v1/queue/tickets/me", token));
    ASSERT_TRUE(after.has_value());
    const std::string_view admitted_body = body_of(*after);
    EXPECT_TRUE(admitted_body.ends_with(R"("status":"admitted"})"));
    static constexpr std::string_view grant_marker = "\"admit_grant\":";
    const auto grant_at = admitted_body.find(grant_marker);
    ASSERT_NE(grant_at, std::string_view::npos);
    const auto grant_start = grant_at + grant_marker.size();
    const auto grant_end = admitted_body.find('}', grant_start);
    ASSERT_NE(grant_end, std::string_view::npos);
    const auto grant_payload = JsonCodec::decode(
        admitted_body.substr(grant_start, grant_end - grant_start + 1));
    ASSERT_TRUE(grant_payload.has_value());
    const auto* grant = std::get_if<std::string>(
        &grant_payload->at("queue_number_token"));
    ASSERT_NE(grant, nullptr);
    const auto grant_claims = number_codec().validate(*grant, std::chrono::system_clock::now());
    ASSERT_TRUE(grant_claims.has_value());
    EXPECT_EQ(grant_claims->number, 1U);
    EXPECT_TRUE(grant_claims->admitted);

    const auto progress = https_exchange(port_, get_request("/v1/queue/progress"));
    ASSERT_TRUE(progress.has_value());
    const auto progress_payload = JsonCodec::decode(body_of(*progress));
    ASSERT_TRUE(progress_payload.has_value());
    EXPECT_EQ(
        std::get<std::int64_t>(progress_payload->at("released_number")), 1);
}

TEST_F(QueueServiceTest, FailedSnapshotWriteDoesNotPublishRelease) {
    const auto token = post_number("01");
    ASSERT_TRUE(token.has_value());
    store_->set_save_success(false);
    store_->set_budgets(
        BudgetAggregate{.gateway_admission = 100, .realm_connections = 100});
    wait_for([this] { return store_->save_attempts() != 0; });

    const auto blocked = https_exchange(
        port_, get_request("/v1/queue/tickets/me", token));
    ASSERT_TRUE(blocked.has_value());
    const auto blocked_payload = JsonCodec::decode(body_of(*blocked));
    ASSERT_TRUE(blocked_payload.has_value());
    EXPECT_EQ(
        std::get<std::string>(blocked_payload->at("status")), "queued");
    EXPECT_TRUE(store_->saved().empty());

    store_->set_save_success(true);
    wait_for([this] { return !store_->saved().empty(); });
    const auto released = https_exchange(
        port_, get_request("/v1/queue/tickets/me", token));
    ASSERT_TRUE(released.has_value());
    EXPECT_NE(body_of(*released).find(R"("status":"admitted")"),
              std::string_view::npos);
}

TEST_F(QueueServiceTest, MetricsTrackIssueAdmitAndProgress) {
    // tick 尾部轮询发布:空闲水位 0 的基线先就绪。
    wait_for([this] {
        return metrics_.render().find("tickets_issued_total 0\n") !=
               std::string::npos;
    });
    const std::string idle = metrics_.render();
    EXPECT_NE(idle.find("released_number 0\n"), std::string::npos);
    EXPECT_NE(idle.find("queue_length_est 0\n"), std::string::npos);
    EXPECT_NE(idle.find("admit_rate "), std::string::npos);

    const auto token = post_number("01");
    ASSERT_TRUE(token.has_value());
    wait_for([this] {
        return metrics_.render().find("tickets_issued_total 1\n") !=
               std::string::npos;
    });
    const std::string issued = metrics_.render();
    EXPECT_NE(issued.find("queue_length_est 1\n"), std::string::npos);
    // 未发生放行:批次计数不出现(计数器只在首次 add/set 后渲染)。
    EXPECT_EQ(issued.find("admit_batches_total"), std::string::npos);

    // 放行帧发生:批次计数与水位随核心外显。
    store_->set_budgets(
        BudgetAggregate{.gateway_admission = 100, .realm_connections = 100});
    wait_for([this] {
        const std::string text = metrics_.render();
        return text.find("admit_batches_total 1\n") != std::string::npos &&
               text.find("released_number 1\n") != std::string::npos;
    });
    const std::string admitted = metrics_.render();
    EXPECT_NE(admitted.find("queue_length_est 0\n"), std::string::npos);

    // progress 请求计数经 handler 上报(告警 6 的 CDN 卸载监测源)。
    ASSERT_TRUE(
        https_exchange(port_, get_request("/v1/queue/progress")).has_value());
    wait_for([this] {
        return metrics_.render().find("progress_requests_total 1\n") !=
               std::string::npos;
    });
}

TEST_F(QueueServiceTest, FailClosedWhenBudgetsUnavailable) {
    const auto token = post_number("01");
    ASSERT_TRUE(token.has_value());
    // 额度保持未知(nullopt):tick 轮询照跑,但不放行、不落快照。
    store_->set_budgets(std::nullopt);
    std::this_thread::sleep_for(std::chrono::milliseconds{60});
    EXPECT_TRUE(store_->saved().empty());

    const auto progress = https_exchange(port_, get_request("/v1/queue/progress"));
    ASSERT_TRUE(progress.has_value());
    const auto progress_payload = JsonCodec::decode(body_of(*progress));
    ASSERT_TRUE(progress_payload.has_value());
    EXPECT_EQ(
        std::get<std::int64_t>(progress_payload->at("released_number")), 0);
}

TEST_F(QueueServiceTest, RestoresWaterLevelsFromColdBackup) {
    // 快照在启动前注入:先停驱动与服务,再带冷备重启(tick 线程不得与
    // stop/start 并发)。
    stopping_.store(true);
    if (driver_.joinable()) driver_.join();
    store_->set_snapshot(QueueSnapshot{
        .released_number = 5,
        .next_number = 6,
        .admit_rate = 0,
        .release_batches_pruned_through = 5,
    });
    service_->stop();
    service_->start();
    port_ = service_->local_endpoints().at(0).port;
    stopping_.store(false);
    driver_ = std::jthread([this] {
        while (!stopping_.load()) {
            service_->tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    // 恢复的水位直接外显:progress 从 5 起算,新发号续 6。
    const auto progress = https_exchange(port_, get_request("/v1/queue/progress"));
    ASSERT_TRUE(progress.has_value());
    const auto progress_payload = JsonCodec::decode(body_of(*progress));
    ASSERT_TRUE(progress_payload.has_value());
    EXPECT_EQ(
        std::get<std::int64_t>(progress_payload->at("released_number")), 5);

    const auto token = post_number("01");
    ASSERT_TRUE(token.has_value());
    const auto claims = number_codec().validate(*token, std::chrono::system_clock::now());
    ASSERT_TRUE(claims.has_value());
    EXPECT_EQ(claims->number, 6U);
}

}  // namespace
}  // namespace realm::game::queue
