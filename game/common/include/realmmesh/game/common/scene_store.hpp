#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace realm::game::common {

inline constexpr std::int32_t scene_coordinate_max = 10'000;
inline constexpr std::uint64_t scene_counter_max = 0x7FFF'FFFF'FFFF'FFFFULL;

struct SceneCharacterKey {
    std::uint64_t account_id{0};
    std::uint32_t realm_id{0};
    std::uint64_t character_id{0};
    bool operator==(const SceneCharacterKey&) const = default;
};

struct SceneLocation {
    std::uint64_t shard_id{0};
    std::int32_t x{0};
    std::int32_t y{0};
    bool operator==(const SceneLocation&) const = default;
};

struct SceneOwner {
    std::string realm_incarnation;
    std::uint64_t session_binding{0};
    std::string scene_instance_id;
    std::string scene_incarnation;
    /// 0 仅供静态分片；动态分片写入必须匹配当前分片承载版本。
    std::uint64_t shard_version{0};
    bool operator==(const SceneOwner&) const = default;
};

enum class SceneCommandType : std::uint8_t {
    Enter = 1,
    Move = 2,
    Transfer = 3
};

/// 业务参数的规范形状；Enter 的坐标/目标为 0，Move 无目标分片，Transfer
/// 无坐标。
struct SceneCommand {
    SceneCommandType type{SceneCommandType::Enter};
    std::uint64_t seq{0};
    std::int32_t x{0};
    std::int32_t y{0};
    std::uint64_t target_shard_id{0};
    bool operator==(const SceneCommand&) const = default;
};

struct SceneStoredResult {
    SceneCommand command;
    SceneLocation location;
    std::uint64_t binding_epoch{0};
    SceneOwner owner;
    bool operator==(const SceneStoredResult&) const = default;
};

struct SceneStateRecord {
    SceneCharacterKey character;
    /// 未进入过为 0；撤销保留落点和最后结果、递增版本并清除 owner。
    std::uint64_t binding_epoch{0};
    std::optional<SceneOwner> owner;
    std::optional<SceneLocation> confirmed_location;
    std::optional<SceneStoredResult> last_result;
    bool operator==(const SceneStateRecord&) const = default;
};

struct SceneWrite {
    SceneCharacterKey character;
    std::uint64_t expected_epoch{0};
    std::optional<SceneOwner> expected_owner;
    SceneOwner owner;
    SceneCommand command;
    SceneLocation location;
};

enum class SceneWriteOutcome {
    Committed,
    Replayed,
    NotOwned,
    StaleBinding,
    InvalidSequence,
    InvalidCommand,
};
struct SceneWriteResult {
    SceneWriteOutcome outcome{SceneWriteOutcome::NotOwned};
    std::optional<SceneStateRecord> state;
};

/// 任何发出写之后的驱动故障都必须作为结果不确定处理，不能宣称未提交。
class SceneWriteUncertain final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct SceneShardOwner {
    std::string instance_id;
    std::string incarnation;
    bool operator==(const SceneShardOwner&) const = default;
};
struct SceneInstanceState {
    std::string instance_id;
    std::string incarnation;
    bool draining{false};
    bool operator==(const SceneInstanceState&) const = default;
};
struct SceneShardRecord {
    std::uint32_t realm_id{0};
    std::uint64_t shard_id{0};
    std::uint64_t version{0};
    std::optional<SceneShardOwner> owner;
    bool draining{false};
    bool operator==(const SceneShardRecord&) const = default;
};

/// 分片承载的窄边界。同步调用只在工作线程执行；记录不删除，防止版本复用。
class SceneShardStore {
public:
    virtual ~SceneShardStore() = default;
    [[nodiscard]] virtual std::optional<SceneInstanceState>
    scene_instance_state(const std::string& instance_id) const = 0;
    /// 宿主注册成功后发布新 incarnation；同 incarnation 不恢复接纳。
    /// 标记排空与其全部已承载分片的排空标记在同一事务提交；与分配相互栅栏。
    [[nodiscard]] virtual bool compare_exchange_scene_instance(
        const std::optional<SceneInstanceState>& expected,
        const SceneInstanceState& desired) = 0;
    [[nodiscard]] virtual std::optional<SceneShardRecord> shard_state(
        std::uint32_t realm_id, std::uint64_t shard_id) const = 0;
    /// CAS 匹配完整预期记录。承载变化版本 +1；标记排空不换承载版本。
    /// 不支持同承载者恢复接纳；释放前由编排者确认无成员/准备/在途写。
    /// 发出写后的故障抛 SceneWriteUncertain，不能据此宣称 CAS 未生效。
    [[nodiscard]] virtual bool compare_exchange_shard(
        const std::optional<SceneShardRecord>& expected,
        const SceneShardRecord& desired) = 0;
};

/// 回读只在精确匹配该次结果时证明已提交；旧值、缺记录或已覆盖历史均是 Unknown。
enum class SceneWriteResolution { Committed, Unknown };
[[nodiscard]] SceneWriteResolution resolve_scene_write(
    const SceneWrite& write, const std::optional<SceneStateRecord>& observed);

/// 固定长度的 SHA-256 hex；只涵盖规范业务参数，不受 request_id/连接变化影响。
[[nodiscard]] std::string scene_command_digest(const SceneCommand& command);

/// 同步窄存储接口；只能在工作线程调用。非归属角色无记录也不能初始化。
class RealmSceneStore {
public:
    virtual ~RealmSceneStore() = default;
    [[nodiscard]] virtual std::optional<SceneStateRecord> initialize_scene(
        const SceneCharacterKey& character) = 0;
    [[nodiscard]] virtual std::optional<SceneStateRecord> scene_state(
        const SceneCharacterKey& character) const = 0;
    [[nodiscard]] virtual SceneWriteResult commit_scene(
        const SceneWrite& write) = 0;
    /// 仅匹配 expected_epoch 和完整 owner 时递增版本撤销；不消耗场景指令序号。
    [[nodiscard]] virtual bool revoke_scene(
        const SceneCharacterKey& character,
        std::uint64_t expected_epoch,
        const SceneOwner& expected_owner) = 0;
};

}  // namespace realm::game::common
