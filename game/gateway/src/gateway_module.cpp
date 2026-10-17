#include "realmmesh/game/gateway/gateway_module.hpp"

#include "realmmesh/cluster/budget_publisher.hpp"
#include "realmmesh/cluster/service_bootstrap.hpp"
#include "realmmesh/game/common/admission_grant.hpp"
#include "realmmesh/game/common/compact_jws.hpp"
#include "realmmesh/game/common/identity_token.hpp"
#include "realmmesh/game/common/player_data_store.hpp"
#include "realmmesh/game/common/session_ticket.hpp"
#include "realmmesh/game/gateway/account_fetch_port.hpp"
#include "realmmesh/game/gateway/admission_consumption_store.hpp"
#include "realmmesh/game/gateway/gateway_admission.hpp"
#include "realmmesh/game/gateway/gateway_config_lua.hpp"
#include "realmmesh/game/gateway/gateway_frame.hpp"
#include "realmmesh/game/gateway/gateway_login_pipeline.hpp"
#include "realmmesh/game/gateway/gateway_primary_transport.hpp"
#include "realmmesh/service_host/edge_service_module.hpp"
#include "realmmesh/service_host/layered_config_lua.hpp"

#include <chrono>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace realm::game::gateway {
namespace {

[[nodiscard]] std::string_view required_environment(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        throw std::runtime_error(std::string(name) + " is not set");
    }
    return value;
}

/// 成员声明序即析构序的反序:帧 → 登录管线 → 其依赖的 adapters,最后由
/// 基类回收 runtime。
class GatewayModule final : public service_host::EdgeServiceModule {
public:
    GatewayModule(
        const service_host::ServiceModuleContext& context, GatewayConfig config)
        : EdgeServiceModule(context) {
        auto login_config = std::move(config.login);
        budget_policy_.conn_capacity = login_config.conn_capacity;
        budget_policy_.fetch_capacity = login_config.fetch_capacity;
        const auto consumption_key = parse_admission_consumption_digest_key(
            required_environment("REALMMESH_ADMISSION_CONSUMPTION_DIGEST_KEY"));
        admission_consumption_store_ =
            std::make_unique<EtcdAdmissionConsumptionStore>(
                AdmissionConsumptionOptions{
                    .key_prefix = login_config.admission_consumption_prefix,
                    .reservation_ttl = login_config.admission_reservation_ttl,
                    .digest_key = consumption_key},
                cluster::make_etcd_http_client(
                    context.config.host().service_discovery.endpoint,
                    std::chrono::milliseconds{500}));
        // 验证键环逐把装载:轮换重叠期允许活动键与退休键同时存在,未知 kid
        // 仍由验证器直接拒绝,不试遍全环。
        std::vector<common::AdmissionGrantVerificationKey> admission_keys;
        admission_keys.reserve(login_config.admission_grant_keys.size());
        for (const auto& source : login_config.admission_grant_keys) {
            admission_keys.push_back(
                {.kid = source.kid,
                 .public_key = common::parse_ed25519_public_key_hex(
                     required_environment(
                         source.public_key_environment.c_str()))});
        }
        admission_ = std::make_unique<GatewayAdmission>(
            common::IdentityTokenCodec(
                common::seed_from_environment("REALMMESH_IDENTITY_KEY_SEED"),
                login_config.identity_kid),
            login_config.identity_issuer,
            common::AdmissionGrantVerifier(
                std::move(admission_keys),
                {.issuer = login_config.admission_grant_issuer,
                 .deployment_id = login_config.deployment_id,
                 .grant_window = login_config.admission_grant_window}),
            *admission_consumption_store_);
        primary_transport_ =
            std::make_unique<EdgeRuntimePrimaryTransport>(runtime());
        const auto& player_data = config.player_data;
        if (player_data.uri.empty()) {
            account_fetch_ = std::make_unique<DelayedAccountFetchPort>(
                std::chrono::milliseconds{100}, login_config.fetch_capacity);
        } else {
            account_fetch_ = std::make_unique<PlayerDataAccountFetchPort>(
                std::make_unique<common::MongoPlayerDataStore>(
                    player_data.uri, player_data.database, player_data.options),
                login_config.fetch_capacity,
                login_config.fetch_workers);
        }
        pipeline_ = std::make_unique<GatewayLoginPipeline>(
            GatewayLoginPipeline::create(
                std::move(login_config),
                common::parse_ticket_key_hex(
                    required_environment("REALMMESH_SESSION_TICKET_KEY")),
                *admission_,
                std::string(context.instance_id),
                *primary_transport_,
                *account_fetch_,
                &logger(),
                &context.metrics));
        frame_ = std::make_unique<GatewayFrame>(*pipeline_);
    }

    void tick(const service_host::ServiceTickContext& context) override {
        frame_->tick(
            logger(), runtime(), context.resolver, context.budget_reporter);
    }

    void started() override { frame_->started(logger(), runtime()); }

    void stopped() override { frame_->stopped(logger(), runtime()); }

    [[nodiscard]] bool ready() const noexcept override {
        return frame_->ready();
    }

    [[nodiscard]] std::optional<cluster::BudgetPublishPolicy> budget_policy()
        const override {
        return budget_policy_;
    }

private:
    cluster::BudgetPublishPolicy budget_policy_;
    std::unique_ptr<GatewayPrimaryTransport> primary_transport_;
    std::unique_ptr<AccountFetchPort> account_fetch_;
    std::unique_ptr<AdmissionConsumptionStore> admission_consumption_store_;
    std::unique_ptr<GatewayAdmission> admission_;
    std::unique_ptr<GatewayLoginPipeline> pipeline_;
    std::unique_ptr<GatewayFrame> frame_;
};

}  // namespace

GatewayConfig load_gateway_config(const service_host::LayeredConfig& config) {
    auto gateway = parse_gateway_config(service_host::lua_root(config));
    common::resolve_player_data_paths(gateway.player_data, config.config_root());
    gateway.login.conn_capacity = config.edge().session_capacity();
    gateway.login.validate();
    return gateway;
}

std::unique_ptr<service_host::ServiceModule> make_gateway_module(
    const service_host::ServiceModuleContext& context) {
    return std::make_unique<GatewayModule>(
        context, load_gateway_config(context.config));
}

}  // namespace realm::game::gateway
