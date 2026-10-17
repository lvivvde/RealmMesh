#include "realmmesh/game/common/scene_store.hpp"

#include <sodium.h>

#include <array>
#include <cstddef>
#include <string>

namespace realm::game::common {

std::string scene_command_digest(const SceneCommand& command) {
    // 不哈希结构体填充或主机端序；序号用于标识指令，参数摘要不含序号。
    std::array<unsigned char, 17> bytes{};
    bytes[0] = static_cast<unsigned char>(command.type);
    const auto put =
        [&](std::uint64_t value, std::size_t offset, std::size_t size) {
            for (std::size_t i = 0; i < size; ++i) {
                bytes[offset + size - i - 1] =
                    static_cast<unsigned char>(value & 0xFFU);
                value >>= 8U;
            }
        };
    put(static_cast<std::uint32_t>(command.x), 1, 4);
    put(static_cast<std::uint32_t>(command.y), 5, 4);
    put(command.target_shard_id, 9, 8);
    std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
    crypto_hash_sha256(digest.data(), bytes.data(), bytes.size());
    std::array<char, crypto_hash_sha256_BYTES * 2 + 1> hex{};
    sodium_bin2hex(hex.data(), hex.size(), digest.data(), digest.size());
    return hex.data();
}

SceneWriteResolution resolve_scene_write(
    const SceneWrite& write, const std::optional<SceneStateRecord>& observed) {
    if (!observed || observed->character != write.character ||
        !observed->last_result || write.expected_epoch > scene_counter_max ||
        write.command.seq == 0) {
        return SceneWriteResolution::Unknown;
    }
    const bool changes_binding = write.command.type != SceneCommandType::Move;
    if (changes_binding && write.expected_epoch == scene_counter_max) {
        return SceneWriteResolution::Unknown;
    }
    const auto epoch = write.expected_epoch + (changes_binding ? 1U : 0U);
    const auto& result = *observed->last_result;
    return result.command == write.command && result.owner == write.owner &&
                   result.location == write.location &&
                   result.binding_epoch == epoch
               ? SceneWriteResolution::Committed
               : SceneWriteResolution::Unknown;
}

}  // namespace realm::game::common
