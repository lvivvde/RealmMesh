#include <iostream>
#include <nlohmann/json.hpp>
#include "realmmesh/game/client/scene_session.hpp"
#include "realmmesh/game/client/wire_login_transport.hpp"

namespace client = realm::game::client;
using Json = nlohmann::json;
using namespace std::chrono_literals;

template <class T>
T checked(client::RealmReply<T> reply) {
    if (!reply.ok())
        throw std::runtime_error(
            "Realm 请求失败：" + std::to_string(reply.error_code));
    return std::move(reply.value);
}

int main(int argc, char** argv) {
    if (argc != 4) return 2;
    client::WireEndpoints endpoints;
    endpoints.login_verify_port = std::stoi(argv[1]);
    endpoints.queue_port = std::stoi(argv[2]);
    endpoints.verify_peer =
        std::getenv("REALMMESH_DEMO_SELF_SIGNED") == nullptr;
    client::WireEnterRealmRedeemer redeemer;
    client::WireLoginTransport transport(endpoints, redeemer);
    client::LoginChainConfig config;
    config.on_queue_position = [](std::uint64_t position) {
        std::cout << Json{{"position", position}}.dump() << std::endl;
    };
    config.gateway_endpoints.push_back(
        {.protocol = realm::network::TransportProtocol::TlsTcp,
         .host = "127.0.0.1",
         .port = static_cast<std::uint16_t>(std::stoi(argv[3])),
         .priority = 1});
    std::unique_ptr<client::RealmSession> session;
    std::string self;
    realm::game::common::SceneState state;
    auto next_refresh = client::Clock::now();
    for (std::string line; std::getline(std::cin, line);) try {
            auto req = Json::parse(line);
            auto op = req.at("op").get<std::string>();
            auto deadline = client::Clock::now() + 5s;
            Json out;
            if (op == "login" && !session) {
                client::LoginChain chain(transport, config);
                auto result = chain.run(
                    client::LoginRun::full(
                        req.at("account"),
                        req.at("password"),
                        client::Clock::now() + 60s));
                if (!result.succeeded())
                    throw std::runtime_error(
                        "登录失败：" + result.failure()->detail);
                session = std::move(
                    std::get<client::FullSuccess>(*result.success()).session);
                op = "list";
                deadline = client::Clock::now() + 5s;
            }
            if (!session) throw std::runtime_error("请先登录");
            if (op == "create") {
                checked(session->create_character(
                    req.at("name").get<std::string>(), deadline));
                op = "list";
            }
            if (op == "list") {
                out["characters"] = Json::array();
                for (const auto& role :
                     checked(session->list_characters(deadline)).characters)
                    out["characters"].push_back(
                        {{"id", std::to_string(role.character_id)},
                         {"name", role.name}});
            } else {
                auto* scene = session->scene();
                if (!scene) throw std::runtime_error("场景不可用");
                if (op == "select") {
                    auto id = req.at("id").get<std::string>();
                    checked(
                        session->select_character(std::stoull(id), deadline));
                    state = checked(scene->get_scene_state(deadline));
                    checked(scene->enter_scene(
                        state.last_scene_seq() + 1, deadline));
                    self = id;
                } else if (self.empty() || (op != "move" && op != "state"))
                    throw std::runtime_error("无效操作");
                if (op == "select" || client::Clock::now() >= next_refresh) {
                    state = checked(scene->get_scene_state(deadline));
                    next_refresh = client::Clock::now() + 5s;
                }
                if (op == "move") {
                    auto dir = req.at("dir").get<std::string>();
                    auto pos = state.confirmed_location();
                    if (dir != "left" && dir != "right" && dir != "up" &&
                        dir != "down")
                        throw std::runtime_error("无效方向");
                    auto reply = scene->move_in_scene(
                        state.last_scene_seq() + 1,
                        pos.x() + (dir == "right") - (dir == "left"),
                        pos.y() + (dir == "down") - (dir == "up"),
                        deadline);
                    if (!reply.ok()) next_refresh = client::Clock::now();
                    auto moved = checked(std::move(reply));
                    state.set_last_scene_seq(moved.scene_seq());
                    *state.mutable_confirmed_location() = moved.location();
                }
                while (auto notice =
                           scene->poll_scene(client::Clock::now() + 2ms)) {
                    auto* event =
                        std::get_if<realm::game::common::SceneEvent>(&*notice);
                    auto* snapshot = state.mutable_snapshot();
                    if (!event ||
                        event->shard_incarnation() !=
                            snapshot->shard_incarnation() ||
                        event->event_seq() > snapshot->event_seq() + 1) {
                        state = checked(scene->get_scene_state(deadline));
                        break;
                    }
                    if (event->event_seq() <= snapshot->event_seq()) continue;
                    snapshot->set_event_seq(event->event_seq());
                    auto* members = snapshot->mutable_members();
                    auto it = std::find_if(
                        members->begin(),
                        members->end(),
                        [&](const auto& member) {
                            return member.character_id() ==
                                   event->member().character_id();
                        });
                    if (event->kind() ==
                        realm::game::common::SceneEvent::KIND_LEAVE) {
                        if (it != members->end() &&
                            it->binding_epoch() ==
                                event->member().binding_epoch())
                            members->erase(it);
                    } else if (it == members->end())
                        *members->Add() = event->member();
                    else if (
                        it->binding_epoch() <= event->member().binding_epoch())
                        *it = event->member();
                }
                if (!state.has_snapshot())
                    throw std::runtime_error("场景快照不可用");
                out["self"] = self;
                out["members"] = Json::array();
                for (const auto& member : state.snapshot().members())
                    out["members"].push_back(
                        {{"id", std::to_string(member.character_id())},
                         {"x", member.location().x()},
                         {"y", member.location().y()}});
            }
            std::cout << out.dump() << std::endl;
        } catch (const std::exception& error) {
            std::cout << Json{{"error", error.what()}}.dump() << std::endl;
        }
}
