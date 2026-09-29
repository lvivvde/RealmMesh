/// 排队调度服进程内集成测试:真实 TLS loopback,服务绑内核临时端口、
/// std::jthread 驱动 tick;主线程用共享测试客户端走完整 HTTPS 请求。
/// etcd 存取用 fake store 注入,验证放行阀门帧与确定性故障边界；真实
/// etcd/真实 Queue 进程重启分别由独立 integration 目标覆盖。

#include "realmmesh/game/queue/queue_service.hpp"

#include "realmmesh/game/common/admission_grant.hpp"
#include "realmmesh/game/common/identity_token.hpp"
#include "realmmesh/game/common/json.hpp"
#include "realmmesh/game/common/queue_number_v2.hpp"
#include "realmmesh/observability/metrics_registry.hpp"
#include "realmmesh/test_support/legacy_queue_number.hpp"

#include "queue_test_https.hpp"
#include "queue_test_store.hpp"

#include <gtest/gtest.h>

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
using test_https::https_exchange;

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

using FakeStore = TestQueueStateStore;

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
                "REALMMESH_QUEUE_NUMBER_KEY_SEED",
                std::string{kSeedHex}.c_str(), 1),
            0);
        // 凭据角色不得共用签名材料:Admission Grant 用独立种子,
        // 否则 QueueAdmissionSecurityConfig::validate() 拒绝启动。
        ASSERT_EQ(
            ::setenv(
                "REALMMESH_ADMISSION_GRANT_KEY_SEED",
                std::string{kGrantSeedHex}.c_str(), 1),
            0);
    }

    static void TearDownTestSuite() {
        static_cast<void>(::unsetenv("REALMMESH_IDENTITY_KEY_SEED"));
        static_cast<void>(::unsetenv("REALMMESH_QUEUE_NUMBER_KEY_SEED"));
        static_cast<void>(::unsetenv("REALMMESH_ADMISSION_GRANT_KEY_SEED"));
    }

    void SetUp() override {
        store_ = std::make_shared<FakeStore>();
        QueueConfig config;
        config.listen_address = "127.0.0.1";
        config.listen_port = 0;
        config.queue_number_kid = std::string{kKid};
        config.admission_grant_kid = std::string{kGrantKid};
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

    /// 服务侧 Queue Number v2 验签键的测试侧对偶(同种子同 kid)。
    [[nodiscard]] common::QueueNumberV2Codec queue_number_codec() const {
        const auto seed = common::parse_identity_seed_hex(kSeedHex);
        return common::QueueNumberV2Codec(
            common::QueueNumberV2SigningKey{
                .kid = std::string{kKid}, .seed = seed},
            {{.kid = std::string{kKid},
              .public_key = common::ed25519_public_key_from_seed(seed)}},
            std::chrono::seconds{3600});
    }

    /// 服务侧 Admission Grant 验签键环的测试侧对偶(独立种子,角色不共材)。
    [[nodiscard]] common::AdmissionGrantVerifier admission_grant_verifier()
        const {
        const auto seed = common::parse_identity_seed_hex(kGrantSeedHex);
        return common::AdmissionGrantVerifier(
            {{.kid = std::string{kGrantKid},
              .public_key = common::ed25519_public_key_from_seed(seed)}},
            {.issuer = "realmmesh/queue",
             .deployment_id = "development",
             .grant_window = std::chrono::seconds{300}});
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
        const auto issued = post_number_with_identity(identity_token(jti(suffix)));
        if (!issued.has_value()) return std::nullopt;
        return issued->first;
    }

    [[nodiscard]] std::optional<std::pair<std::string, std::uint64_t>>
    post_number_with_identity(std::string_view identity) const {
        std::string body;
        body += "POST /v1/queue/tickets HTTP/1.1\r\n";
        body += "Host: localhost\r\n";
        body += "Authorization: Bearer " + std::string(identity) + "\r\n";
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
        const auto* number = std::get_if<std::int64_t>(&payload->at("number"));
        if (number == nullptr || *number <= 0) return std::nullopt;
        return std::pair{
            *token, static_cast<std::uint64_t>(*number)};
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
    static constexpr std::string_view kGrantSeedHex =
        "4ccd089b28ff96da9db6c346ec114e0f"
        "5b8a319f35aba624da8cf6ed4fb8a6fb";
    static constexpr std::string_view kKid = "queue-test-1";
    static constexpr std::string_view kGrantKid = "grant-test-1";
    static constexpr std::string_view kIdentityKid = "login-verify-test-1";
    /// 与 identity_token() 的 exp 上界一致;Grant 验签要求
    /// exp ≤ identity_expires_at,测试侧只用它做上界,不重复计时。
    static constexpr std::chrono::seconds kIdentityTtl{1800};

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

    // 号牌是 v2 排位凭据:绑定签发时的 identity jti,且只证明位次。
    const auto identity_now = std::chrono::system_clock::now();
    const auto number_claims = queue_number_codec().validate(
        *token, jti("01"), identity_now + kIdentityTtl, identity_now);
    ASSERT_TRUE(number_claims.has_value());
    EXPECT_EQ(number_claims->identity_jti, jti("01"));
    EXPECT_EQ(number_claims->number, 1U);

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

    // 放行后:查询返回 Admission Grant(spec:凭据嵌套 admit_grant 对象,
    // 外层只带状态/位次/估时;JsonCodec 只编扁平对象,以尾段与嵌套截取
    // 核验)。#79 起准入凭据是独立的 Grant,不再是重签 admitted 号牌。
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
    EXPECT_EQ(std::get<std::int64_t>(grant_payload->at("number")), 1);
    const auto* grant = std::get_if<std::string>(
        &grant_payload->at("admission_grant"));
    ASSERT_NE(grant, nullptr);
    const auto grant_now = std::chrono::system_clock::now();
    const auto grant_claims = admission_grant_verifier().validate(
        *grant, jti("01"), grant_now + kIdentityTtl, grant_now);
    ASSERT_TRUE(grant_claims.has_value());
    // Grant 绑定同一身份与来源号值:跨身份拼接在签发侧就不可能。
    EXPECT_EQ(grant_claims->identity_jti, jti("01"));
    EXPECT_EQ(grant_claims->queue_number, 1U);

    const auto progress = https_exchange(port_, get_request("/v1/queue/progress"));
    ASSERT_TRUE(progress.has_value());
    const auto progress_payload = JsonCodec::decode(body_of(*progress));
    ASSERT_TRUE(progress_payload.has_value());
    EXPECT_EQ(
        std::get<std::int64_t>(progress_payload->at("released_number")), 1);
}

/// 旧世界的 admitted 号牌(#82 已从生产库删除,夹具见 test_support)必须
/// 在 Queue HTTP 边界被拒:问位次只认 v2 排位凭据。
TEST_F(QueueServiceTest, RejectsLegacyAdmittedQueueNumberAtQueryBoundary) {
    const test_support::LegacyQueueNumberCodec legacy(
        common::parse_identity_seed_hex(kSeedHex), std::string{kKid});
    const auto now = std::chrono::system_clock::now();
    const auto legacy_token = legacy.issue(
        test_support::LegacyQueueNumberClaims{
            .number = 1,
            .admitted = true,
            .issued_at = now,
            .expires_at = now + std::chrono::seconds{300}});

    const auto response = https_exchange(
        port_, get_request("/v1/queue/tickets/me", legacy_token));
    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(status_of(*response), 401);
    const auto payload = JsonCodec::decode(body_of(*response));
    ASSERT_TRUE(payload.has_value());
    EXPECT_EQ(
        std::get<std::int64_t>(payload->at("code")),
        QueueHandler::error_invalid_number);
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
    const auto claims_now = std::chrono::system_clock::now();
    const auto claims = queue_number_codec().validate(
        *token, jti("01"), claims_now + kIdentityTtl, claims_now);
    ASSERT_TRUE(claims.has_value());
    EXPECT_EQ(claims->number, 6U);
}

TEST_F(QueueServiceTest, RestartRecoversAcknowledgedIssueAndReleasePosition) {
    const auto identity = identity_token(jti("01"));
    const auto first = post_number_with_identity(identity);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->second, 1U);

    store_->set_budgets(
        BudgetAggregate{.gateway_admission = 100, .realm_connections = 100});
    wait_for([this] {
        const auto saved = store_->saved();
        return !saved.empty() && saved.back().released_number == 1U;
    });

    stopping_.store(true);
    if (driver_.joinable()) driver_.join();
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

    const auto replay = post_number_with_identity(identity);
    ASSERT_TRUE(replay.has_value());
    EXPECT_EQ(replay->second, first->second);
    EXPECT_EQ(replay->first, first->first);
    const auto query = https_exchange(
        port_, get_request("/v1/queue/tickets/me", replay->first));
    ASSERT_TRUE(query.has_value());
    EXPECT_EQ(status_of(*query), 200);
    EXPECT_NE(body_of(*query).find("\"status\":\"admitted\""),
              std::string_view::npos);
}

}  // namespace
}  // namespace realm::game::queue
