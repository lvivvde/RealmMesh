#include "realmmesh/game/gateway/edge_attach_chain.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

namespace realm::game::gateway {
namespace {

using common::IdentityClaims;
using common::IdentityTokenCodec;

constexpr std::string_view identity_seed_hex =
    "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
constexpr std::string_view number_seed_hex =
    "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb";

const auto t0 = std::chrono::system_clock::time_point{
    std::chrono::seconds{1'700'000'000}};

constexpr std::string_view jti_a =
    "000102030405060708090a0b0c0d0e0f";
constexpr std::string_view jti_b =
    "101112131415161718191a1b1c1d1e1f";
constexpr std::string_view jti_c =
    "202122232425262728292a2b2c2d2e2f";

class EdgeAttachChainTest : public ::testing::Test {
protected:
    void SetUp() override {
        identity_codec_.emplace(
            common::parse_identity_seed_hex(identity_seed_hex),
            "identity-kid");
        number_codec_.emplace(
            common::parse_identity_seed_hex(number_seed_hex), "number-kid");
        pipeline_.emplace(4U, 2U);
    }

    [[nodiscard]] std::string identity_token(
        std::string_view jti,
        std::string issuer = "realmmesh/login-verify",
        std::chrono::system_clock::time_point expires_at =
            t0 + std::chrono::minutes(30)) {
        return identity_codec_->issue(IdentityClaims{
            .issuer = std::string(issuer),
            .account_id = 42,
            .jti = std::string(jti),
            .issued_at = t0,
            .expires_at = expires_at});
    }

    [[nodiscard]] std::string number_token(
        std::uint64_t number, bool admitted) {
        return number_codec_->issue(common::QueueNumberClaims{
            .number = number,
            .admitted = admitted,
            .issued_at = t0,
            .expires_at = t0 + std::chrono::seconds(300)});
    }

    [[nodiscard]] EdgeAttachVerdict attach(
        EdgeSessionId session,
        std::string_view jti,
        bool admitted = true) {
        return chain().handle(
            session,
            identity_token(jti),
            number_token(7, admitted),
            t0 + std::chrono::seconds(1));
    }

    [[nodiscard]] EdgeAttachChain& chain() {
        if (!chain_.has_value()) {
            chain_.emplace(
                *identity_codec_, *number_codec_, *pipeline_,
                "realmmesh/login-verify");
        }
        return *chain_;
    }

    [[nodiscard]] EdgeSessionId open_session() {
        const EdgeSessionId session{next_session_id_++};
        pipeline_->on_session_opened(session);
        return session;
    }

    std::optional<IdentityTokenCodec> identity_codec_;
    std::optional<common::QueueNumberCodec> number_codec_;
    std::optional<EdgeSessionPipeline> pipeline_;
    std::optional<EdgeAttachChain> chain_;
    std::uint64_t next_session_id_{1};
};

TEST_F(EdgeAttachChainTest, ValidCredentialsEnterFetching) {
    const auto session = open_session();
    EXPECT_EQ(attach(session, jti_a), EdgeAttachVerdict::Accepted);
    EXPECT_EQ(pipeline_->stage(session), EdgeSessionStage::Fetching);
    EXPECT_EQ(chain().last_account_id(), 42U);
    EXPECT_EQ(pipeline_->fetch_used(), 1U);
}

TEST_F(EdgeAttachChainTest, InvalidCredentialsAndNumbersAreRejected) {
    const auto session = open_session();

    // 身份 Token 垃圾数据
    EXPECT_EQ(
        chain().handle(session, "not-a-token", number_token(7, true), t0),
        EdgeAttachVerdict::InvalidCredentials);
    // 身份 Token iss 不符
    EXPECT_EQ(
        chain().handle(
            session, identity_token(jti_a, "realmmesh/queue"),
            number_token(7, true), t0),
        EdgeAttachVerdict::InvalidCredentials);
    // 号牌垃圾数据
    EXPECT_EQ(
        chain().handle(session, identity_token(jti_a), "not-a-token", t0),
        EdgeAttachVerdict::InvalidNumber);
    // 号牌未放行(admitted=false,签名有效)
    EXPECT_EQ(
        chain().handle(
            session, identity_token(jti_a), number_token(7, false), t0),
        EdgeAttachVerdict::InvalidNumber);

    EXPECT_EQ(pipeline_->stage(session), EdgeSessionStage::Pending);
    EXPECT_EQ(pipeline_->fetch_used(), 0U);
}

TEST_F(EdgeAttachChainTest, ReplayedJtiIsRejectedOnceConsumed) {
    const auto first = open_session();
    ASSERT_EQ(attach(first, jti_a), EdgeAttachVerdict::Accepted);

    const auto second = open_session();
    EXPECT_EQ(attach(second, jti_a), EdgeAttachVerdict::InvalidCredentials);
    EXPECT_EQ(pipeline_->stage(second), EdgeSessionStage::Pending);
    EXPECT_EQ(pipeline_->stage(first), EdgeSessionStage::Fetching);

    // #47 观测缝:重放拒绝被累计(其余裁决不计入)。
    EXPECT_EQ(chain().replay_rejections(), 1U);

    // 不同 jti 各自独立
    EXPECT_EQ(attach(second, jti_b), EdgeAttachVerdict::Accepted);
    EXPECT_EQ(pipeline_->stage(second), EdgeSessionStage::Fetching);
    EXPECT_EQ(chain().replay_rejections(), 1U);
}

TEST_F(EdgeAttachChainTest, AtCapacityRejectsWithoutBurningCredentials) {
    const auto first = open_session();
    const auto second = open_session();
    const auto third = open_session();
    ASSERT_EQ(attach(first, jti_a), EdgeAttachVerdict::Accepted);
    ASSERT_EQ(attach(second, jti_b), EdgeAttachVerdict::Accepted);
    ASSERT_EQ(pipeline_->fetch_free(), 0U);

    EXPECT_EQ(attach(third, jti_c), EdgeAttachVerdict::OutOfBudget);
    EXPECT_EQ(pipeline_->stage(third), EdgeSessionStage::Pending);

    // 满额拒绝未烧 jti:腾出拉取额度后同一凭据原样重试成功
    ASSERT_TRUE(pipeline_->mark_handed_off(first));
    EXPECT_EQ(attach(third, jti_c), EdgeAttachVerdict::Accepted);
    EXPECT_EQ(pipeline_->stage(third), EdgeSessionStage::Fetching);
}

TEST_F(EdgeAttachChainTest, NonPendingAndUnknownSessionsAreNotAttachable) {
    const auto session = open_session();
    ASSERT_EQ(attach(session, jti_a), EdgeAttachVerdict::Accepted);

    // 已迁离 pending 的会话不能重复 attach
    EXPECT_EQ(attach(session, jti_b), EdgeAttachVerdict::NotPending);
    // 未知会话
    EXPECT_EQ(
        chain().handle(
            EdgeSessionId{999}, identity_token(jti_b),
            number_token(7, true), t0),
        EdgeAttachVerdict::NotPending);
    EXPECT_EQ(pipeline_->fetch_used(), 1U);
}

}  // namespace
}  // namespace realm::game::gateway
