#include "realmmesh/game/scene/scene_module.hpp"
#include "realmmesh/edge/edge_runtime.hpp"
#include "realmmesh/game/common/session_ticket.hpp"
#include "realmmesh/game/scene/scene_shard.hpp"
#include "realmmesh/observability/metrics_registry.hpp"
#include "realmmesh/service_host/edge_service_module.hpp"
#include "scene_dynamic.hpp"

#include <map>
#include <sol/sol.hpp>
#include <stdexcept>
#include <unordered_map>
#include "realmmesh/service_host/layered_config_lua.hpp"

namespace realm::game::scene {
namespace internal = ::realmmesh::protocol::scene_internal::v1;
namespace {
std::uint64_t configured_shard(const service_host::LayeredConfig& config) {
    const auto section =
        service_host::lua_root(config).raw_get<sol::object>("scene");
    if (section == sol::lua_nil) return 1;
    if (!section.is<sol::table>())
        throw std::invalid_argument("scene must be table");
    const auto shard =
        section.as<sol::table>().raw_get<sol::object>("shard_id");
    if (!shard.is<lua_Integer>() || shard.as<lua_Integer>() < 1 ||
        shard.as<lua_Integer>() > 2)
        throw std::invalid_argument(
            "S3 Scene requires configured shard 1 or 2");
    return static_cast<std::uint64_t>(shard.as<lua_Integer>());
}
class SceneModule final : public service_host::EdgeServiceModule {
public:
    explicit SceneModule(const service_host::ServiceModuleContext& context)
        : EdgeServiceModule(context),
          instance_id_(context.instance_id),
          metrics_(context.metrics),
          shard_(
              instance_id_,
              common::correlation_id_hex(common::make_correlation_id()),
              configured_shard(context.config)) {
        std::size_t enabled = 0;
        for (const auto& transport : context.config.edge().transports) {
            if (!transport.enabled) continue;
            ++enabled;
            if (transport.protocol != network::TransportProtocol::TlsTcp ||
                !transport.tls ||
                transport.tls->alpn != common::scene_internal_alpn ||
                transport.tls->client_ca_file.empty() ||
                transport.tls->allowed_client_identities.empty() ||
                transport.max_payload_size > common::scene_envelope_max ||
                transport.max_sessions > 64) {
                throw std::invalid_argument(
                    "Scene requires bounded dedicated mutual TLS transport");
            }
        }
        if (enabled != 1)
            throw std::invalid_argument("Scene requires one TLS listener");
    }
    void tick(const service_host::ServiceTickContext&) override {
        shard_.expire();
        for (auto& event : runtime().drain_events(256)) {
            const auto id = event.connection_id;
            if (event.kind == edge::EdgeEventKind::ConnectionClosed) {
                peers_.erase(id);
                shard_.closed(id.value);
            } else if (event.kind == edge::EdgeEventKind::MessageReceived) {
                const auto request =
                    common::decode_scene_request(event.payload);
                const auto correlation =
                    common::scene_internal_request_id(event.payload);
                const auto peer = peers_.find(id);
                if (!request || !correlation ||
                    (peer != peers_.end() &&
                     peer->second != request->realm_incarnation()) ||
                    (peer == peers_.end() &&
                     request->operation() != internal::OPERATION_SNAPSHOT)) {
                    static_cast<void>(runtime().try_close(id));
                    continue;
                }
                const auto reply = shard_.handle(id.value, *request);
                const auto payload = common::encode(reply, *correlation);
                const auto result = event.established
                                        ? runtime().try_send(id, payload)
                                        : runtime().try_accept(id, payload);
                if (result != edge::QueueResult::Queued) {
                    static_cast<void>(runtime().try_close(id));
                } else {
                    peers_[id] = request->realm_incarnation();
                }
            }
            publish();
        }
        metrics_.gauge_set(
            "scene_shard_members",
            static_cast<double>(shard_.active_members()));
        metrics_.gauge_set(
            "scene_prepared_members",
            static_cast<double>(shard_.prepared_members()));
        metrics_.gauge_set("scene_draining", shard_.draining() ? 1 : 0);
    }

private:
    void publish() {
        for (const auto& event : shard_.drain_events()) {
            for (const auto& [id, incarnation] : peers_) {
                common::ScenePush push;
                push.set_realm_incarnation(incarnation);
                push.set_scene_instance_id(instance_id_);
                push.set_scene_incarnation(shard_.incarnation());
                *push.mutable_event() = event;
                if (runtime().try_send(id, common::encode(push)) !=
                    edge::QueueResult::Queued) {
                    static_cast<void>(runtime().try_close(id));
                }
            }
        }
    }
    std::string instance_id_;
    observability::MetricsRegistry& metrics_;
    SceneShard shard_;
    std::unordered_map<edge::EdgeConnectionId, std::string> peers_;
};
}  // namespace
std::unique_ptr<service_host::ServiceModule> make_scene_module(
    const service_host::ServiceModuleContext& context) {
    const auto section =
        service_host::lua_root(context.config).raw_get<sol::object>("scene");
    if (section.is<sol::table>()) {
        const auto dynamic =
            section.as<sol::table>().raw_get<sol::object>("dynamic");
        if (dynamic != sol::lua_nil && !dynamic.is<bool>())
            throw std::invalid_argument("scene.dynamic must be boolean");
        if (section.as<sol::table>().get_or("dynamic", false))
            return make_dynamic_scene_module(context);
    }
    return std::make_unique<SceneModule>(context);
}
}  // namespace realm::game::scene
