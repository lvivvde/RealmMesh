#include "realmmesh/game/login_verify/login_verify_dispatcher.hpp"

#include "realmmesh/game/common/account_store.hpp"
#include "realmmesh/game/common/identity_token.hpp"
#include "realmmesh/game/common/json.hpp"
#include "realmmesh/game/login_verify/login_verify_handler.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

namespace realm::game::login_verify {
namespace {

using namespace std::chrono_literals;
using common::JsonCodec;

/// 认定被挡住的账号源:模拟 Argon2 + 存储查询的慢路径。
class BlockingAccountStore final : public common::AccountStore {
public:
    [[nodiscard]] std::optional<common::AccountRecord> authenticate(
        std::string_view, std::string_view) const override {
        std::unique_lock lock(mutex_);
        released_condition_.wait_for(lock, 5s, [this] { return released_; });
        return common::AccountRecord{
            .account_id = 42, .banned = false, .whitelisted = true};
    }

    void release() {
        {
            const std::scoped_lock lock(mutex_);
            released_ = true;
        }
        released_condition_.notify_all();
    }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable released_condition_;
    bool released_{false};
};

[[nodiscard]] network::Http1Request request(
    std::string method, std::string target, std::string body = {}) {
    network::Http1Request value;
    value.method = std::move(method);
    value.target = std::move(target);
    value.body = std::move(body);
    return value;
}

[[nodiscard]] network::Http1Request verify_request() {
    return request(
        "POST", "/v1/login/verify", R"({"account":"player","credential":"dev"})");
}

[[nodiscard]] std::vector<LoginVerifyDispatcher::Completion> wait_for_completions(
    LoginVerifyDispatcher& dispatcher, std::size_t count) {
    std::vector<LoginVerifyDispatcher::Completion> completions;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (completions.size() < count &&
           std::chrono::steady_clock::now() < deadline) {
        for (auto& completion : dispatcher.drain(16)) {
            completions.push_back(std::move(completion));
        }
        std::this_thread::sleep_for(1ms);
    }
    return completions;
}

class LoginVerifyDispatcherTest : public ::testing::Test {
protected:
    BlockingAccountStore store_;
    common::IdentityTokenCodec codec_{
        common::parse_identity_seed_hex(
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
        "login-verify-test-1"};
    LoginVerifyHandler handler_{
        store_, codec_, [] { return std::chrono::system_clock::now(); }};
};

/// 验签挂起到工作者上,loop 线程上的其他路由照常即时应答(#98)。
TEST_F(LoginVerifyDispatcherTest, SlowVerificationDoesNotBlockOtherRoutes) {
    LoginVerifyDispatcher dispatcher(handler_, {.workers = 1, .capacity = 4});

    const auto deferred = dispatcher.dispatch(verify_request(), 7);
    EXPECT_TRUE(std::holds_alternative<network::HttpDeferred>(deferred));

    const auto health = dispatcher.dispatch(request("GET", "/healthz"), 8);
    ASSERT_TRUE(std::holds_alternative<network::Http1Response>(health));
    EXPECT_EQ(std::get<network::Http1Response>(health).status, 200);
    EXPECT_TRUE(dispatcher.drain(16).empty());

    store_.release();
    const auto completions = wait_for_completions(dispatcher, 1);
    ASSERT_EQ(completions.size(), 1U);
    EXPECT_EQ(completions[0].token, 7U);
    EXPECT_EQ(completions[0].response.status, 200);
    const auto payload = JsonCodec::decode(completions[0].response.body);
    ASSERT_TRUE(payload.has_value());
    EXPECT_TRUE(payload->contains("identity_token"));
}

/// 工作者满额:验签即时回 503 + Retry-After,不排进无界队列;
/// 满额不影响其他路由。
TEST_F(LoginVerifyDispatcherTest, FullCapacityAnswersServiceUnavailableWithRetryAfter) {
    LoginVerifyDispatcher dispatcher(handler_, {.workers = 1, .capacity = 1});
    ASSERT_TRUE(std::holds_alternative<network::HttpDeferred>(
        dispatcher.dispatch(verify_request(), 1)));

    const auto busy = dispatcher.dispatch(verify_request(), 2);
    ASSERT_TRUE(std::holds_alternative<network::Http1Response>(busy));
    const auto& response = std::get<network::Http1Response>(busy);
    EXPECT_EQ(response.status, 503);
    ASSERT_NE(response.header("Retry-After"), nullptr);
    EXPECT_EQ(*response.header("Retry-After"), "1");
    const auto payload = JsonCodec::decode(response.body);
    ASSERT_TRUE(payload.has_value());
    EXPECT_EQ(std::get<std::int64_t>(payload->at("code")), error_verifier_busy);
    EXPECT_EQ(std::get<std::int64_t>(payload->at("retry_after_seconds")), 1);

    const auto health = dispatcher.dispatch(request("GET", "/healthz"), 3);
    ASSERT_TRUE(std::holds_alternative<network::Http1Response>(health));
    EXPECT_EQ(std::get<network::Http1Response>(health).status, 200);

    store_.release();
    ASSERT_EQ(wait_for_completions(dispatcher, 1).size(), 1U);
    // 结果取走即释放槽位,下一次验签重新挂起。
    EXPECT_TRUE(std::holds_alternative<network::HttpDeferred>(
        dispatcher.dispatch(verify_request(), 4)));
    ASSERT_EQ(wait_for_completions(dispatcher, 1).size(), 1U);
}

/// 非 POST 的验签路由不占工作者:405 即时应答。
TEST_F(LoginVerifyDispatcherTest, WrongMethodOnVerifyRouteAnswersImmediately) {
    LoginVerifyDispatcher dispatcher(handler_, {.workers = 1, .capacity = 1});
    const auto result = dispatcher.dispatch(request("GET", "/v1/login/verify"), 1);
    ASSERT_TRUE(std::holds_alternative<network::Http1Response>(result));
    EXPECT_EQ(std::get<network::Http1Response>(result).status, 405);
}

TEST_F(LoginVerifyDispatcherTest, RejectsZeroWorkersAndZeroCapacity) {
    EXPECT_THROW(
        LoginVerifyDispatcher(handler_, {.workers = 0, .capacity = 1}),
        std::invalid_argument);
    EXPECT_THROW(
        LoginVerifyDispatcher(handler_, {.workers = 1, .capacity = 0}),
        std::invalid_argument);
}

}  // namespace
}  // namespace realm::game::login_verify
