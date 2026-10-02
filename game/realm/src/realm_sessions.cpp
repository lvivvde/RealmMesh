#include "realmmesh/game/realm/realm_sessions.hpp"

#include "realmmesh/game/common/edge_protocol.hpp"
#include "realmmesh/game/gateway/gateway_runtime.hpp"
#include "realmmesh/game/realm/character_name.hpp"
#include "realmmesh/game/realm/training_rule.hpp"
#include "realmmesh/observability/logger.hpp"

#include <exception>
#include <type_traits>
#include <utility>

namespace realm::game::realm {
namespace {

template <typename... Visitors>
struct Overloaded : Visitors... {
    using Visitors::operator()...;
};
template <typename... Visitors>
Overloaded(Visitors...) -> Overloaded<Visitors...>;

}  // namespace

void GatewayRuntimeRealmOutbox::send(
    gateway::EdgeSessionId session, std::vector<std::byte> payload) {
    // 出站队列满时回包丢失,客户端按请求超时处理;会话不因此关闭。
    static_cast<void>(runtime_.try_send(session, payload));
}

void GatewayRuntimeRealmOutbox::close(gateway::EdgeSessionId session) {
    static_cast<void>(runtime_.try_close(session));
}

RealmSessions::RealmSessions(
    common::RealmCharacterStore& store,
    TrainingRule& rule,
    RealmOutbox& outbox,
    const RealmConfig& config,
    observability::Logger* logger)
    : store_(store),
      rule_(rule),
      outbox_(outbox),
      config_(config),
      logger_(logger),
      pool_(config.data_workers, config.data_capacity) {}

RealmSessions::~RealmSessions() = default;

void RealmSessions::enter(
    gateway::EdgeSessionId session, std::uint64_t account_id) {
    if (const auto online = online_.find(account_id);
        online != online_.end() && online->second != session) {
        const auto displaced = online->second;
        outbox_.send(displaced, common::encode(common::RealmSessionDisplaced{}));
        outbox_.close(displaced);
        sessions_.erase(displaced);
    }
    online_[account_id] = session;
    sessions_[session] = Session{.account_id = account_id};
}

void RealmSessions::closed(gateway::EdgeSessionId session) {
    const auto found = sessions_.find(session);
    if (found == sessions_.end()) return;
    if (const auto online = online_.find(found->second.account_id);
        online != online_.end() && online->second == session) {
        online_.erase(online);
    }
    sessions_.erase(found);
}

bool RealmSessions::contains(gateway::EdgeSessionId session) const {
    return sessions_.contains(session);
}

std::optional<RealmSessionPhase> RealmSessions::phase(
    gateway::EdgeSessionId session) const {
    const auto found = sessions_.find(session);
    if (found == sessions_.end()) return std::nullopt;
    return found->second.phase;
}

bool RealmSessions::handle(
    gateway::EdgeSessionId id, std::span<const std::byte> payload) {
    const auto message = common::realm_message_id(payload);
    const auto found = sessions_.find(id);
    if (!message.has_value() || found == sessions_.end()) return false;

    using common::RealmMessageId;
    std::optional<Request> request;
    switch (*message) {
        case RealmMessageId::MESSAGE_ID_C2S_LIST_CHARACTERS:
            if (const auto decoded = common::decode_list_characters(payload)) {
                request = Request{.body = ListRequest{}};
            }
            break;
        case RealmMessageId::MESSAGE_ID_C2S_CREATE_CHARACTER:
            if (const auto decoded = common::decode_create_character(payload)) {
                request = Request{.body = CreateRequest{.name = decoded->name()}};
            }
            break;
        case RealmMessageId::MESSAGE_ID_C2S_SELECT_CHARACTER:
            if (const auto decoded = common::decode_select_character(payload)) {
                request = Request{
                    .body = SelectRequest{
                        .character_id = decoded->character_id()}};
            }
            break;
        case RealmMessageId::MESSAGE_ID_C2S_TRAIN:
            if (const auto decoded = common::decode_train(payload)) {
                request = Request{.body = TrainRequest{.seq = decoded->seq()}};
            }
            break;
        default:
            return false;
    }
    if (!request.has_value()) return false;
    request->request_id = common::realm_request_id(payload).value_or(0);

    auto& session = found->second;
    const auto pending =
        session.waiting.size() + (session.busy ? std::size_t{1} : 0);
    if (pending >= config_.max_pending_per_session) {
        send_error(
            id,
            request->request_id,
            common::edge_error_throttled,
            "too many pending realm requests",
            static_cast<std::uint32_t>(config_.retry_after.count()));
        return true;
    }
    session.waiting.push_back(std::move(*request));
    process_next(id, session);
    return true;
}

void RealmSessions::complete(std::size_t max_completions) {
    for (auto& completion : pool_.drain(max_completions)) {
        const auto pending = pending_.find(completion.id);
        if (pending == pending_.end()) continue;
        const auto data = std::move(pending->second);
        pending_.erase(pending);

        // 会话已关闭或被顶替:写已落库,回包丢弃。
        const auto found = sessions_.find(data.session);
        if (found == sessions_.end() ||
            found->second.account_id != data.account_id) {
            continue;
        }
        auto& session = found->second;
        session.busy = false;
        static_cast<void>(guarded(data.session, data.request, [&] {
            finish(data.session, session, data.request, completion.result);
            return false;
        }));
        process_next(data.session, session);
    }
}

void RealmSessions::process_next(gateway::EdgeSessionId id, Session& session) {
    while (!session.busy && !session.waiting.empty()) {
        const auto request = std::move(session.waiting.front());
        session.waiting.pop_front();
        session.busy = guarded(
            id, request, [&] { return process(id, session, request); });
    }
}

template <typename Step>
bool RealmSessions::guarded(
    gateway::EdgeSessionId id, const Request& request, Step&& step) {
    // 训练规则在帧线程执行,Lua 报错或返回值形状不对时抛异常;
    // 收敛为该请求的 3010,会话与进程都保持。
    try {
        return step();
    } catch (const std::exception& error) {
        if (logger_ != nullptr) {
            static_cast<void>(logger_->error(
                "realm_training_rule_failed",
                "realm training rule raised an error",
                {observability::field("session_id", id.value),
                 observability::field("error", std::string(error.what()))}));
        }
        send_error(
            id,
            request.request_id,
            common::edge_error_realm_data_unavailable,
            "realm request failed");
        return false;
    }
}

bool RealmSessions::process(
    gateway::EdgeSessionId id, Session& session, const Request& request) {
    const bool selecting = session.phase == RealmSessionPhase::Selecting;
    const auto account_id = session.account_id;
    auto& store = store_;
    const auto mismatch = [&] {
        send_error(
            id,
            request.request_id,
            common::edge_error_phase_mismatch,
            selecting ? "character not selected" : "character already selected");
        return false;
    };

    return std::visit(
        Overloaded{
            [&](const ListRequest&) {
                if (!selecting) return mismatch();
                return submit(id, session, request, [&store, account_id] {
                    return DataResult{
                        .ok = true,
                        .value = store.list_characters(account_id, realm_id)};
                });
            },
            [&](const CreateRequest& create) {
                if (!selecting) return mismatch();
                if (!is_valid_character_name(create.name)) {
                    send_error(
                        id,
                        request.request_id,
                        common::edge_error_invalid_character_name,
                        "invalid character name");
                    return false;
                }
                return submit(
                    id, session, request, [&store, account_id, name = create.name] {
                        return DataResult{
                            .ok = true,
                            .value = store.create_character(
                                account_id,
                                realm_id,
                                name,
                                max_characters_per_account)};
                    });
            },
            [&](const SelectRequest& select) {
                if (!selecting) return mismatch();
                if (select.character_id == 0) {
                    send_error(
                        id,
                        request.request_id,
                        common::edge_error_character_not_owned,
                        "character not owned");
                    return false;
                }
                return submit(
                    id,
                    session,
                    request,
                    [&store, account_id, character_id = select.character_id] {
                        return DataResult{
                            .ok = true,
                            .value = store.choose_character(
                                account_id, realm_id, character_id)};
                    });
            },
            [&](const TrainRequest& train) {
                if (selecting || !session.character.has_value()) {
                    return mismatch();
                }
                const auto& character = *session.character;
                if (train.seq == character.last_training_seq) {
                    send_train_result(id, request.request_id, character, true);
                    return false;
                }
                if (train.seq != character.last_training_seq + 1) {
                    send_error(
                        id,
                        request.request_id,
                        common::edge_error_invalid_training_seq,
                        "invalid training seq");
                    return false;
                }
                const auto exp = rule_.train(character.exp);
                if (!exp.has_value()) {
                    send_error(
                        id,
                        request.request_id,
                        common::edge_error_max_level,
                        "character is at max level");
                    return false;
                }
                const common::TrainingWrite write{
                    .account_id = account_id,
                    .realm_id = realm_id,
                    .character_id = character.character_id,
                    .seq = train.seq,
                    .exp = *exp,
                };
                return submit(id, session, request, [&store, write] {
                    return DataResult{
                        .ok = true, .value = store.record_training(write)};
                });
            },
        },
        request.body);
}

bool RealmSessions::submit(
    gateway::EdgeSessionId id,
    const Session& session,
    const Request& request,
    concurrency::BoundedWorkPool<DataResult>::Job job) {
    const auto throttle = [&] {
        send_error(
            id,
            request.request_id,
            common::edge_error_throttled,
            "realm data access is saturated",
            static_cast<std::uint32_t>(config_.retry_after.count()));
        return false;
    };
    if (pool_.in_flight() >= pool_.capacity()) return throttle();

    const auto data_id = next_data_id_++;
    // 任务不得抛异常:数据源故障收敛为 ok=false,帧线程回 3010。
    auto guarded = [job = std::move(job)]() noexcept {
        try {
            return job();
        } catch (const std::exception& error) {
            return DataResult{.ok = false, .error = error.what()};
        } catch (...) {
            return DataResult{.ok = false, .error = "unknown error"};
        }
    };
    switch (pool_.try_submit(data_id, std::move(guarded))) {
        case concurrency::WorkSubmitResult::Submitted:
            pending_.emplace(
                data_id,
                PendingData{
                    .session = id,
                    .account_id = session.account_id,
                    .request = request});
            return true;
        case concurrency::WorkSubmitResult::Full:
            return throttle();
        case concurrency::WorkSubmitResult::Stopped:
            break;
    }
    send_error(
        id,
        request.request_id,
        common::edge_error_realm_data_unavailable,
        "realm data unavailable");
    return false;
}

void RealmSessions::finish(
    gateway::EdgeSessionId id,
    Session& session,
    const Request& request,
    const DataResult& result) {
    if (!result.ok) {
        if (logger_ != nullptr) {
            static_cast<void>(logger_->warn(
                "realm_data_unavailable",
                "realm character data access failed",
                {observability::field("session_id", id.value),
                 observability::field("error", result.error)}));
        }
        send_error(
            id,
            request.request_id,
            common::edge_error_realm_data_unavailable,
            "realm data unavailable");
        return;
    }

    std::visit(
        Overloaded{
            [&](const ListRequest&) {
                const auto& roster =
                    std::get<common::CharacterRoster>(result.value);
                common::CharacterList list;
                for (const auto& character : roster.characters) {
                    *list.add_characters() = summary(character);
                }
                list.set_last_selected_character_id(
                    roster.last_selected_character_id);
                outbox_.send(id, common::encode(list, request.request_id));
            },
            [&](const CreateRequest&) {
                const auto& created =
                    std::get<common::CreateCharacterResult>(result.value);
                switch (created.outcome) {
                    case common::CreateCharacterOutcome::Created: {
                        common::CharacterCreated reply;
                        *reply.mutable_character() = summary(created.character);
                        outbox_.send(
                            id, common::encode(reply, request.request_id));
                        return;
                    }
                    case common::CreateCharacterOutcome::NameTaken:
                        send_error(
                            id,
                            request.request_id,
                            common::edge_error_character_name_taken,
                            "character name taken");
                        return;
                    case common::CreateCharacterOutcome::LimitReached:
                        send_error(
                            id,
                            request.request_id,
                            common::edge_error_character_limit_reached,
                            "character limit reached");
                        return;
                }
            },
            [&](const SelectRequest&) {
                const auto& chosen =
                    std::get<std::optional<common::RealmCharacter>>(result.value);
                if (!chosen.has_value()) {
                    send_error(
                        id,
                        request.request_id,
                        common::edge_error_character_not_owned,
                        "character not owned");
                    return;
                }
                common::CharacterSelected reply;
                *reply.mutable_character() = summary(*chosen);
                reply.set_training_seq(chosen->last_training_seq);
                session.character = *chosen;
                session.phase = RealmSessionPhase::InGame;
                outbox_.send(id, common::encode(reply, request.request_id));
            },
            [&](const TrainRequest& train) {
                finish_train(
                    id,
                    session,
                    request,
                    train.seq,
                    std::get<std::optional<common::TrainingWriteResult>>(
                        result.value));
            },
        },
        request.body);
}

void RealmSessions::finish_train(
    gateway::EdgeSessionId id,
    Session& session,
    const Request& request,
    std::uint64_t seq,
    const std::optional<common::TrainingWriteResult>& write) {
    if (!write.has_value()) {
        send_error(
            id,
            request.request_id,
            common::edge_error_character_not_owned,
            "character not owned");
        return;
    }
    // 条件写未命中说明库里的序号已被别处推进(如被顶替会话的在途写):
    // 以库为准刷新缓存,恰好等于本 seq 视为重放。
    session.character = write->character;
    if (write->committed || write->character.last_training_seq == seq) {
        send_train_result(
            id, request.request_id, write->character, !write->committed);
        return;
    }
    send_error(
        id,
        request.request_id,
        common::edge_error_invalid_training_seq,
        "invalid training seq");
}

void RealmSessions::send_error(
    gateway::EdgeSessionId id,
    std::uint64_t request_id,
    int code,
    std::string_view message,
    std::uint32_t retry_after_seconds) {
    common::EdgeError error;
    error.set_code(static_cast<std::uint32_t>(code));
    error.set_message(std::string(message));
    error.set_retry_after_seconds(retry_after_seconds);
    outbox_.send(id, common::encode(error, request_id));
}

void RealmSessions::send_train_result(
    gateway::EdgeSessionId id,
    std::uint64_t request_id,
    const common::RealmCharacter& character,
    bool replayed) {
    common::TrainResult result;
    result.set_character_id(character.character_id);
    result.set_exp(character.exp);
    result.set_level(rule_.level(character.exp));
    result.set_seq(character.last_training_seq);
    result.set_replayed(replayed);
    outbox_.send(id, common::encode(result, request_id));
}

common::CharacterSummary RealmSessions::summary(
    const common::RealmCharacter& character) {
    common::CharacterSummary summary;
    summary.set_character_id(character.character_id);
    summary.set_name(character.name);
    summary.set_exp(character.exp);
    summary.set_level(rule_.level(character.exp));
    return summary;
}

}  // namespace realm::game::realm
