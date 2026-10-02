#pragma once

#include "realmmesh/concurrency/bounded_work_pool.hpp"
#include "realmmesh/game/common/player_data_store.hpp"
#include "realmmesh/game/common/realm_protocol.hpp"
#include "realmmesh/game/gateway/edge_session_table.hpp"
#include "realmmesh/game/realm/realm_config.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace realm::game::gateway {
class GatewayRuntime;
}  // namespace realm::game::gateway

namespace realm::observability {
class Logger;
}  // namespace realm::observability

namespace realm::game::realm {

class TrainingRule;

/// Realm Session 的出站缝:回包与关闭。生产实现转发到 GatewayRuntime。
class RealmOutbox {
public:
    virtual ~RealmOutbox() = default;
    virtual void send(
        gateway::EdgeSessionId session, std::vector<std::byte> payload) = 0;
    virtual void close(gateway::EdgeSessionId session) = 0;
};

class GatewayRuntimeRealmOutbox final : public RealmOutbox {
public:
    explicit GatewayRuntimeRealmOutbox(gateway::GatewayRuntime& runtime)
        : runtime_(runtime) {}

    void send(
        gateway::EdgeSessionId session,
        std::vector<std::byte> payload) override;
    void close(gateway::EdgeSessionId session) override;

private:
    gateway::GatewayRuntime& runtime_;
};

enum class RealmSessionPhase : std::uint8_t {
    /// 选角:只受理列表、创建、选择。
    Selecting,
    /// 游戏中:只受理训练。不可回到选角。
    InGame,
};

/// Realm Session 表与选角、训练处理(#93)。全部方法在帧线程调用;
/// 角色数据访问在有界工作线程上执行(满额回 429,数据源故障回 3010,
/// 会话都保持)。在线表按账号唯一:同账号新会话入场时,旧会话收到
/// RealmSessionDisplaced(1409)后关闭;旧会话在途的写仍会落库,回包丢弃。
/// 同一会话的请求按到达顺序逐个处理。
class RealmSessions final {
public:
    static constexpr std::uint32_t realm_id = 1;
    static constexpr std::size_t max_characters_per_account = 3;

    RealmSessions(
        common::RealmCharacterStore& store,
        TrainingRule& rule,
        RealmOutbox& outbox,
        const RealmConfig& config,
        observability::Logger* logger = nullptr);
    ~RealmSessions();

    RealmSessions(const RealmSessions&) = delete;
    RealmSessions& operator=(const RealmSessions&) = delete;

    /// EnterRealm 受理后登记会话(选角阶段)。
    void enter(gateway::EdgeSessionId session, std::uint64_t account_id);
    /// 连接关闭:移出在线表,未决请求的回包随之丢弃。
    void closed(gateway::EdgeSessionId session);
    [[nodiscard]] bool contains(gateway::EdgeSessionId session) const;
    [[nodiscard]] std::optional<RealmSessionPhase> phase(
        gateway::EdgeSessionId session) const;

    /// 处理已入场会话的客户端 Realm 消息(1401/1403/1405/1407)。不是这
    /// 四种消息、解码失败或会话未入场时返回 false,交由调用方按未知消息
    /// 处理。
    [[nodiscard]] bool handle(
        gateway::EdgeSessionId session, std::span<const std::byte> payload);
    /// 帧尾取回已完成的数据访问并回包。
    void complete(std::size_t max_completions);

    [[nodiscard]] std::size_t online() const noexcept {
        return sessions_.size();
    }

private:
    struct ListRequest {};
    struct CreateRequest {
        std::string name;
    };
    struct SelectRequest {
        std::uint64_t character_id{0};
    };
    struct TrainRequest {
        std::uint64_t seq{0};
    };
    using RequestBody =
        std::variant<ListRequest, CreateRequest, SelectRequest, TrainRequest>;
    struct Request {
        std::uint64_t request_id{0};
        RequestBody body;
    };

    struct Session {
        std::uint64_t account_id{0};
        RealmSessionPhase phase{RealmSessionPhase::Selecting};
        std::optional<common::RealmCharacter> character;
        bool busy{false};
        std::deque<Request> waiting;
    };

    using DataValue = std::variant<
        common::CharacterRoster,
        common::CreateCharacterResult,
        std::optional<common::RealmCharacter>,
        std::optional<common::TrainingWriteResult>>;
    struct DataResult {
        bool ok{false};
        std::string error;
        DataValue value;
    };
    struct PendingData {
        gateway::EdgeSessionId session;
        std::uint64_t account_id{0};
        Request request;
    };

    void process_next(gateway::EdgeSessionId id, Session& session);
    /// 返回 true 表示已提交数据访问(会话进入忙碌,等待完成)。
    [[nodiscard]] bool process(
        gateway::EdgeSessionId id, Session& session, const Request& request);
    /// 执行一步处理;抛异常时回 3010 并返回 false(不占用会话)。
    template <typename Step>
    [[nodiscard]] bool guarded(
        gateway::EdgeSessionId id, const Request& request, Step&& step);
    [[nodiscard]] bool submit(
        gateway::EdgeSessionId id,
        const Session& session,
        const Request& request,
        concurrency::BoundedWorkPool<DataResult>::Job job);
    void finish(
        gateway::EdgeSessionId id,
        Session& session,
        const Request& request,
        const DataResult& result);
    void finish_train(
        gateway::EdgeSessionId id,
        Session& session,
        const Request& request,
        std::uint64_t seq,
        const std::optional<common::TrainingWriteResult>& write);
    void send_error(
        gateway::EdgeSessionId id,
        std::uint64_t request_id,
        int code,
        std::string_view message,
        std::uint32_t retry_after_seconds = 0);
    void send_train_result(
        gateway::EdgeSessionId id,
        std::uint64_t request_id,
        const common::RealmCharacter& character,
        bool replayed);
    [[nodiscard]] common::CharacterSummary summary(
        const common::RealmCharacter& character);

    common::RealmCharacterStore& store_;
    TrainingRule& rule_;
    RealmOutbox& outbox_;
    RealmConfig config_;
    observability::Logger* logger_{nullptr};
    std::unordered_map<gateway::EdgeSessionId, Session> sessions_;
    std::unordered_map<std::uint64_t, gateway::EdgeSessionId> online_;
    std::unordered_map<std::uint64_t, PendingData> pending_;
    std::uint64_t next_data_id_{1};
    /// 最后声明、最先析构:先回收工作线程,再释放 store 之外的簿记。
    /// 关闭会话不取消在途访问,写照常落库,只丢回包。
    concurrency::BoundedWorkPool<DataResult> pool_;
};

}  // namespace realm::game::realm
