#include "realmmesh/game/realm/realm_module.hpp"

#include "realmmesh/cluster/budget_publisher.hpp"
#include "realmmesh/cluster/service_resolver.hpp"
#include "realmmesh/game/common/player_data_store.hpp"
#include "realmmesh/game/realm/realm_config_lua.hpp"
#include "realmmesh/game/realm/realm_frame.hpp"
#include "realmmesh/game/realm/realm_scenes.hpp"
#include "realmmesh/game/realm/realm_chat.hpp"
#include "realmmesh/game/realm/realm_sessions.hpp"
#include "realmmesh/game/realm/training_rule.hpp"
#include "realmmesh/service_host/edge_service_module.hpp"
#include "realmmesh/service_host/host_config.hpp"
#include "realmmesh/service_host/layered_config_lua.hpp"

#include <memory>
#include <optional>
#include <stdexcept>

namespace realm::game::realm {
namespace {

/// 成员声明序即析构序的反序:帧 → Realm Session 表 → 其出站缝/训练规则/
/// 角色存储,最后由基类回收 runtime。
class RealmModule final : public service_host::EdgeServiceModule {
public:
    RealmModule(
        const service_host::ServiceModuleContext& context, RealmConfig config)
        : EdgeServiceModule(context) {
        // 选角与训练读写权威角色数据,没有「无存储」的 Realm 装配。
        scene_instances_.push_back(config.scene.instance_id);
        dynamic_scene_ = config.scene.enabled && config.scene.dynamic;
        if (dynamic_scene_ &&
            (!context.config.host().service_discovery.enabled ||
             !context.config.host().service_discovery.required))
            throw std::invalid_argument(
                "dynamic Realm Scene requires mandatory service discovery");
        for (const auto& target : config.scene.targets)
            scene_instances_.push_back(target.instance_id);
        const auto& player_data = config.player_data;
        if (player_data.uri.empty()) {
            throw std::invalid_argument("realm requires player_data.uri");
        }
        const auto conn_capacity = context.config.edge().session_capacity();
        budget_policy_.conn_capacity = conn_capacity;
        budget_policy_.fetch_capacity = 0;
        // 训练规则先于存储加载:规则失败即启动失败(#93)。
        training_rule_ =
            std::make_unique<TrainingRule>(config.training_rule_file);
        characters_ = std::make_unique<common::MongoPlayerDataStore>(
            player_data.uri, player_data.database, player_data.options);
        outbox_ = std::make_unique<EdgeRuntimeRealmOutbox>(runtime());
        scenes_ = std::make_unique<RealmScenes>(
            *characters_, *outbox_, config, context.metrics, characters_.get());
        config.chat.realm_instance_id = context.instance_id;
        const auto& discovery = context.config.host().service_discovery;
        config.chat.discovery_enabled = discovery.enabled;
        config.chat.discovery_endpoint = discovery.endpoint;
        config.chat.discovery_prefix = discovery.key_prefix;
        chat_ = std::make_unique<RealmChat>(*outbox_, config, context.metrics, scenes_.get());
        sessions_ = std::make_unique<RealmSessions>(
            *characters_,
            *training_rule_,
            *outbox_,
            config,
            &logger(),
            scenes_.get(), chat_.get());
        frame_ = std::make_unique<RealmFrame>(
            config.max_events_per_frame, conn_capacity, *sessions_);
    }

    void tick(const service_host::ServiceTickContext& context) override {
        if (dynamic_scene_) {
            scenes_->discovery_ready(false);
            const auto snapshot =
                context.resolver ? context.resolver->endpoints() : std::nullopt;
            if (snapshot) {
                for (const auto& instance : scene_instances_) {
                    const auto found = snapshot->find(instance);
                    scenes_->endpoint(
                        found == snapshot->end() ? std::nullopt
                                                 : std::optional(found->second),
                        instance);
                }
                scenes_->discovery_ready(true);
            }
        } else {
            for (const auto& instance : scene_instances_)
                scenes_->endpoint(
                    context.resolver ? context.resolver->endpoint(instance)
                                     : std::nullopt,
                    instance);
        }
        frame_->tick(logger(), runtime(), context.budget_reporter);
    }

    void started() override { frame_->started(logger(), runtime()); }

    void stopped() override { frame_->stopped(logger()); }

    [[nodiscard]] std::optional<cluster::BudgetPublishPolicy> budget_policy()
        const override {
        return budget_policy_;
    }

private:
    cluster::BudgetPublishPolicy budget_policy_;
    std::vector<std::string> scene_instances_;
    bool dynamic_scene_{false};
    /// Session 表在三者之后声明、先于它们析构:先回收数据访问工作线程,
    /// 再释放它引用的存储、规则与出站缝。
    std::unique_ptr<common::MongoPlayerDataStore> characters_;
    std::unique_ptr<TrainingRule> training_rule_;
    std::unique_ptr<RealmOutbox> outbox_;
    std::unique_ptr<RealmScenes> scenes_;
    std::unique_ptr<RealmChat> chat_;
    std::unique_ptr<RealmSessions> sessions_;
    std::unique_ptr<RealmFrame> frame_;
};

}  // namespace

RealmConfig load_realm_config(const service_host::LayeredConfig& config) {
    auto realm = parse_realm_config(service_host::lua_root(config));
    realm.training_rule_file = config.resolve_path(realm.training_rule_file);
    if (realm.scene.enabled) {
        realm.scene.ca_file = config.resolve_path(realm.scene.ca_file).string();
        realm.scene.certificate_file =
            config.resolve_path(realm.scene.certificate_file).string();
        realm.scene.private_key_file =
            config.resolve_path(realm.scene.private_key_file).string();
    }
    if (realm.chat.enabled) {
        for (auto* path : {&realm.chat.ca_file, &realm.chat.certificate_file, &realm.chat.private_key_file})
            if (!path->empty()) *path = config.resolve_path(*path).string();
    }
    common::resolve_player_data_paths(realm.player_data, config.config_root());
    return realm;
}

std::unique_ptr<service_host::ServiceModule> make_realm_module(
    const service_host::ServiceModuleContext& context) {
    return std::make_unique<RealmModule>(
        context, load_realm_config(context.config));
}

}  // namespace realm::game::realm
