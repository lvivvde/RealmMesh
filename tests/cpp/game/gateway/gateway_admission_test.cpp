#include "realmmesh/game/gateway/gateway_admission.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace realm::game::gateway {
namespace {

using namespace std::chrono_literals;

constexpr std::string_view identity_seed_hex =
    "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
constexpr std::string_view grant_seed_hex =
    "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb";
constexpr std::string_view digest_key_hex =
    "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7";
constexpr std::string_view identity_a =
    "000102030405060708090a0b0c0d0e0f";
constexpr std::string_view identity_b =
    "101112131415161718191a1b1c1d1e1f";
constexpr std::string_view grant_jti =
    "202122232425262728292a2b2c2d2e2f";

std::chrono::system_clock::time_point at(std::int64_t seconds) {
    return std::chrono::system_clock::time_point{std::chrono::seconds{seconds}};
}

AdmissionConsumptionOptions store_options() {
    return {
        .key_prefix = "/realmmesh/admission/test",
        .reservation_ttl = 10s,
        .digest_key =
            parse_admission_consumption_digest_key(digest_key_hex),
    };
}

common::IdentityTokenCodec identity_codec() {
    return common::IdentityTokenCodec(
        common::parse_identity_seed_hex(identity_seed_hex),
        "login-verify-v1");
}

common::AdmissionGrantPolicy grant_policy(std::string deployment) {
    return {
        .issuer = "realmmesh/queue",
        .deployment_id = std::move(deployment),
        .grant_window = 5min,
    };
}

common::AdmissionGrantIssuer grant_issuer(std::string deployment = "prod-a") {
    return common::AdmissionGrantIssuer(
        {.kid = "grant-v1",
         .seed = common::parse_identity_seed_hex(grant_seed_hex)},
        grant_policy(std::move(deployment)));
}

common::AdmissionGrantVerifier grant_verifier(
    std::string deployment = "prod-a") {
    const auto seed = common::parse_identity_seed_hex(grant_seed_hex);
    return common::AdmissionGrantVerifier(
        {{.kid = "grant-v1",
          .public_key = common::ed25519_public_key_from_seed(seed)}},
        grant_policy(std::move(deployment)));
}

std::string identity_token(
    std::string_view jti, std::uint64_t account_id = 42) {
    return identity_codec().issue(common::IdentityClaims{
        .issuer = "realmmesh/login-verify",
        .account_id = account_id,
        .jti = std::string{jti},
        .issued_at = at(100),
        .expires_at = at(1'000),
    });
}

std::string grant_token(std::string_view bound_identity = identity_a) {
    return grant_issuer().issue(common::AdmissionGrantIssue{
        .grant_jti = std::string{grant_jti},
        .identity_jti = std::string{bound_identity},
        .queue_number = 7,
        .released_at = at(100),
        .issued_at = at(100),
        .identity_expires_at = at(1'000),
    });
}

GatewayAdmission admission(
    AdmissionConsumptionStore& store,
    std::string deployment = "prod-a") {
    return GatewayAdmission(
        identity_codec(),
        "realmmesh/login-verify",
        grant_verifier(std::move(deployment)),
        store);
}

TEST(GatewayAdmissionTest, TwoInstancesCommitOneIdentityAtMostOnce) {
    InMemoryAdmissionConsumptionStore store(store_options());
    auto gateway_a = admission(store);
    auto gateway_b = admission(store);
    const auto identity = identity_token(identity_a);
    const auto grant = grant_token();

    auto first = gateway_a.reserve(
        identity, grant, "gateway-a/attempt-1", at(110));
    ASSERT_EQ(first.status, GatewayAdmissionStartStatus::Reserved);
    ASSERT_TRUE(first.reservation.has_value());
    EXPECT_EQ(first.reservation->account_id(), 42U);

    auto competing = gateway_b.reserve(
        identity, grant, "gateway-b/attempt-1", at(111));
    EXPECT_EQ(competing.status, GatewayAdmissionStartStatus::InProgress);

    EXPECT_EQ(
        gateway_a.abandon(*first.reservation, at(112)),
        GatewayAdmissionTransitionStatus::Applied);
    auto replacement = gateway_b.reserve(
        identity, grant, "gateway-b/attempt-1", at(112));
    ASSERT_EQ(replacement.status, GatewayAdmissionStartStatus::Reserved);
    ASSERT_TRUE(replacement.reservation.has_value());
    EXPECT_EQ(
        gateway_b.on_accept_result(
            *replacement.reservation,
            PrimaryTransportResult::Queued,
            at(113)),
        GatewayAdmissionTransitionStatus::Applied);

    const auto replay = gateway_a.reserve(
        identity, grant, "gateway-a/attempt-2", at(114));
    EXPECT_EQ(replay.status, GatewayAdmissionStartStatus::Consumed);
}

TEST(GatewayAdmissionTest, RejectsBindingAndDeploymentBeforeStoreWork) {
    InMemoryAdmissionConsumptionStore store(store_options());
    store.set_available(false);
    auto gateway = admission(store);

    EXPECT_EQ(
        gateway
            .reserve(
                identity_token(identity_b),
                grant_token(identity_a),
                "gateway-a/attempt-1",
                at(110))
            .status,
        GatewayAdmissionStartStatus::InvalidCredentials);

    auto wrong_deployment = admission(store, "prod-b");
    EXPECT_EQ(
        wrong_deployment
            .reserve(
                identity_token(identity_a),
                grant_token(identity_a),
                "gateway-a/attempt-2",
                at(110))
            .status,
        GatewayAdmissionStartStatus::InvalidCredentials);
}

TEST(GatewayAdmissionTest, StoreOutageIsRetryableAndAffectsAvailability) {
    InMemoryAdmissionConsumptionStore store(store_options());
    store.set_available(false);
    auto gateway = admission(store);

    EXPECT_EQ(
        gateway
            .reserve(
                identity_token(identity_a),
                grant_token(),
                "gateway-a/attempt-1",
                at(110))
            .status,
        GatewayAdmissionStartStatus::StoreUnavailable);
    EXPECT_FALSE(gateway.available());
}

TEST(GatewayAdmissionTest, FullKeepsReservationAndStoppedReleasesIt) {
    InMemoryAdmissionConsumptionStore store(store_options());
    auto gateway_a = admission(store);
    auto gateway_b = admission(store);
    const auto identity = identity_token(identity_a);
    const auto grant = grant_token();
    auto first = gateway_a.reserve(
        identity, grant, "gateway-a/attempt-1", at(110));
    ASSERT_TRUE(first.reservation.has_value());

    EXPECT_EQ(
        gateway_a.on_accept_result(
            *first.reservation, PrimaryTransportResult::Full, at(111)),
        GatewayAdmissionTransitionStatus::Pending);
    EXPECT_EQ(
        gateway_b
            .reserve(identity, grant, "gateway-b/attempt-1", at(111))
            .status,
        GatewayAdmissionStartStatus::InProgress);

    EXPECT_EQ(
        gateway_a.on_accept_result(
            *first.reservation, PrimaryTransportResult::Stopped, at(112)),
        GatewayAdmissionTransitionStatus::Applied);
    auto replacement = gateway_b.reserve(
        identity, grant, "gateway-b/attempt-1", at(112));
    EXPECT_EQ(replacement.status, GatewayAdmissionStartStatus::Reserved);
}

class AmbiguousCommitStore final : public AdmissionConsumptionStore {
public:
    AdmissionReserveResult reserve(
        const AdmissionReserveRequest& request) override {
        return {
            .status = AdmissionReserveStatus::Reserved,
            .reservation = AdmissionReservation{
                .identity_jti = request.identity_jti,
                .grant_jti = request.grant_jti,
                .owner = request.owner,
                .fencing = 1,
                .lease_expires_at = request.now + 10s,
                .consume_until = request.consume_until,
            },
        };
    }

    AdmissionMutationStatus commit(
        const AdmissionReservation&,
        std::chrono::system_clock::time_point) override {
        ++commit_calls;
        return AdmissionMutationStatus::Unavailable;
    }

    AdmissionMutationStatus release(
        const AdmissionReservation&,
        std::chrono::system_clock::time_point) override {
        ++release_calls;
        return AdmissionMutationStatus::Applied;
    }

    bool available() const noexcept override { return false; }

    int commit_calls{0};
    int release_calls{0};
};

TEST(GatewayAdmissionTest, AmbiguousCommitCanNeverReleaseReservation) {
    AmbiguousCommitStore store;
    auto gateway = admission(store);
    auto started = gateway.reserve(
        identity_token(identity_a),
        grant_token(),
        "gateway-a/attempt-1",
        at(110));
    ASSERT_TRUE(started.reservation.has_value());

    EXPECT_EQ(
        gateway.on_accept_result(
            *started.reservation, PrimaryTransportResult::Queued, at(111)),
        GatewayAdmissionTransitionStatus::StoreUnavailable);
    EXPECT_EQ(
        gateway.abandon(*started.reservation, at(112)),
        GatewayAdmissionTransitionStatus::Lost);
    EXPECT_EQ(store.commit_calls, 1);
    EXPECT_EQ(store.release_calls, 0);
}

}  // namespace
}  // namespace realm::game::gateway
