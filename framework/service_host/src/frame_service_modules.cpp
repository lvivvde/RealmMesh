#include "realmmesh/service_host/frame_service_modules.hpp"

#include "realmmesh/cluster/budget_publisher.hpp"
#include "realmmesh/cluster/service_bootstrap.hpp"
#include "realmmesh/edge/edge_runtime.hpp"
#include "realmmesh/game/common/admission_grant.hpp"
#include "realmmesh/game/common/compact_jws.hpp"
#include "realmmesh/game/common/identity_token.hpp"
#include "realmmesh/game/common/player_data_config.hpp"
#include "realmmesh/game/common/player_data_store.hpp"
#include "realmmesh/game/gateway/account_fetch_port.hpp"
#include "realmmesh/game/gateway/admission_consumption_store.hpp"
#include "realmmesh/game/gateway/gateway_admission.hpp"
#include "realmmesh/game/gateway/gateway_config_lua.hpp"
#include "realmmesh/game/gateway/gateway_login_pipeline.hpp"
#include "realmmesh/game/gateway/gateway_primary_transport.hpp"
#include "realmmesh/game/realm/realm_config_lua.hpp"
#include "realmmesh/game/realm/realm_sessions.hpp"
#include "realmmesh/game/realm/training_rule.hpp"
#include "realmmesh/service_host/edge_service_module.hpp"
#include "realmmesh/service_host/layered_config_lua.hpp"
#include "realmmesh/service_host/service_frame.hpp"

#include <chrono>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace realm::service_host {
namespace {

[[nodiscard]] std::string_view required_environment(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        throw std::runtime_error(std::string(name) + " is not set");
    }
    return value;
}

/// Gateway/Realm 共用的业务帧模块。成员声明序即析构序的反序:frame →
/// Realm Session 表 → 其出站缝/训练规则/角色存储 → Gateway Login
/// Pipeline → adapters,最后由基类回收 runtime。
class FrameServiceModule final : public EdgeServiceModule {
public:
    FrameServiceModule(
        const ServiceModuleContext& context,
        game::gateway::GatewayConfig config,
        std::optional<game::realm::RealmConfig> realm_config)
        : EdgeServiceModule(context) {
        const bool is_gateway = !realm_config.has_value();
        const auto& player_data = config.player_data;
        auto login_config = config.login;
        budget_policy_.conn_capacity = login_config.conn_capacity;
        budget_policy_.fetch_capacity =
            is_gateway ? login_config.fetch_capacity : 0;
        if (is_gateway) {
            wire_gateway(context, player_data, std::move(login_config));
        } else {
            wire_realm(player_data, *realm_config);
        }
        frame_ = std::make_unique<ServiceFrame>(
            service_name(),
            config.downstream_address,
            config.downstream_port,
            config.max_events_per_frame,
            config.login.conn_capacity,
            gateway_login_pipeline_.get(),
            realm_sessions_.get());
    }

    void tick(const ServiceTickContext& context) override {
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
    void wire_gateway(
        const ServiceModuleContext& context,
        const game::common::PlayerDataConfig& player_data,
        game::gateway::GatewayLoginConfig login_config) {
        const auto consumption_key =
            game::gateway::parse_admission_consumption_digest_key(
                required_environment(
                    "REALMMESH_ADMISSION_CONSUMPTION_DIGEST_KEY"));
        admission_consumption_store_ =
            std::make_unique<game::gateway::EtcdAdmissionConsumptionStore>(
                game::gateway::AdmissionConsumptionOptions{
                    .key_prefix = login_config.admission_consumption_prefix,
                    .reservation_ttl = login_config.admission_reservation_ttl,
                    .digest_key = consumption_key},
                cluster::make_etcd_http_client(
                    context.config.host().service_discovery.endpoint,
                    std::chrono::milliseconds{500}));
        // 验证键环逐把装载:轮换重叠期允许活动键与退休键同时存在,未知 kid
        // 仍由验证器直接拒绝,不试遍全环。
        std::vector<game::common::AdmissionGrantVerificationKey> admission_keys;
        admission_keys.reserve(login_config.admission_grant_keys.size());
        for (const auto& source : login_config.admission_grant_keys) {
            admission_keys.push_back(
                {.kid = source.kid,
                 .public_key = game::common::parse_ed25519_public_key_hex(
                     required_environment(
                         source.public_key_environment.c_str()))});
        }
        gateway_admission_ = std::make_unique<game::gateway::GatewayAdmission>(
            game::common::IdentityTokenCodec(
                game::common::seed_from_environment("REALMMESH_IDENTITY_KEY_SEED"),
                login_config.identity_kid),
            login_config.identity_issuer,
            game::common::AdmissionGrantVerifier(
                std::move(admission_keys),
                {.issuer = login_config.admission_grant_issuer,
                 .deployment_id = login_config.deployment_id,
                 .grant_window = login_config.admission_grant_window}),
            *admission_consumption_store_);
        gateway_primary_transport_ =
            std::make_unique<game::gateway::EdgeRuntimePrimaryTransport>(
                runtime());
        if (player_data.uri.empty()) {
            account_fetch_ =
                std::make_unique<game::gateway::DelayedAccountFetchPort>(
                    std::chrono::milliseconds{100},
                    login_config.fetch_capacity);
        } else {
            account_fetch_ =
                std::make_unique<game::gateway::PlayerDataAccountFetchPort>(
                    std::make_unique<game::common::MongoPlayerDataStore>(
                        player_data.uri,
                        player_data.database,
                        player_data.options),
                    login_config.fetch_capacity,
                    login_config.fetch_workers);
        }
        gateway_login_pipeline_ =
            std::make_unique<game::gateway::GatewayLoginPipeline>(
                game::gateway::GatewayLoginPipeline::create(
                    std::move(login_config),
                    game::common::parse_ticket_key_hex(required_environment(
                        "REALMMESH_SESSION_TICKET_KEY")),
                    *gateway_admission_,
                    std::string(context.instance_id),
                    *gateway_primary_transport_,
                    *account_fetch_,
                    &logger(),
                    &context.metrics));
    }

    void wire_realm(
        const game::common::PlayerDataConfig& player_data,
        const game::realm::RealmConfig& realm_config) {
        // 选角与训练读写权威角色数据,没有「无存储」的 Realm 装配。训练
        // 规则加载失败即启动失败(#93)。
        if (player_data.uri.empty()) {
            throw std::invalid_argument("realm requires player_data.uri");
        }
        training_rule_ = std::make_unique<game::realm::TrainingRule>(
            realm_config.training_rule_file);
        realm_characters_ = std::make_unique<game::common::MongoPlayerDataStore>(
            player_data.uri, player_data.database, player_data.options);
        realm_outbox_ =
            std::make_unique<game::realm::EdgeRuntimeRealmOutbox>(runtime());
        realm_sessions_ = std::make_unique<game::realm::RealmSessions>(
            *realm_characters_,
            *training_rule_,
            *realm_outbox_,
            realm_config,
            &logger());
    }

    cluster::BudgetPublishPolicy budget_policy_;
    std::unique_ptr<game::gateway::GatewayPrimaryTransport>
        gateway_primary_transport_;
    std::unique_ptr<game::gateway::AccountFetchPort> account_fetch_;
    std::unique_ptr<game::gateway::AdmissionConsumptionStore>
        admission_consumption_store_;
    std::unique_ptr<game::gateway::GatewayAdmission> gateway_admission_;
    std::unique_ptr<game::gateway::GatewayLoginPipeline>
        gateway_login_pipeline_;
    /// 仅 Realm:Session 表最后声明、最先析构,先回收数据访问工作线程,
    /// 再释放它引用的三者。
    std::unique_ptr<game::common::MongoPlayerDataStore> realm_characters_;
    std::unique_ptr<game::realm::TrainingRule> training_rule_;
    std::unique_ptr<game::realm::RealmOutbox> realm_outbox_;
    std::unique_ptr<game::realm::RealmSessions> realm_sessions_;
    std::unique_ptr<ServiceFrame> frame_;
};

}  // namespace

game::gateway::GatewayConfig load_gateway_config(const LayeredConfig& config) {
    auto gateway = game::gateway::parse_gateway_config(lua_root(config));
    game::common::resolve_player_data_paths(
        gateway.player_data, config.config_root());
    gateway.login.conn_capacity = config.edge().session_capacity();
    if (config.service_name() == "gateway") {
        gateway.login.validate();
    }
    return gateway;
}

game::realm::RealmConfig load_realm_config(const LayeredConfig& config) {
    auto realm = game::realm::parse_realm_config(lua_root(config));
    realm.training_rule_file = config.resolve_path(realm.training_rule_file);
    return realm;
}

std::unique_ptr<ServiceModule> make_gateway_module(
    const ServiceModuleContext& context) {
    return std::make_unique<FrameServiceModule>(
        context, load_gateway_config(context.config), std::nullopt);
}

std::unique_ptr<ServiceModule> make_realm_module(
    const ServiceModuleContext& context) {
    return std::make_unique<FrameServiceModule>(
        context,
        load_gateway_config(context.config),
        load_realm_config(context.config));
}

}  // namespace realm::service_host
