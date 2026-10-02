#pragma once

#include "realmmesh/game/common/player_data_store.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace realm::test_support {

/// 内存版 RealmCharacterStore:工作线程调用,内部加锁。gate 关闭时数据
/// 访问停在工作线程里,用来制造「在途」;calls() 计数所有数据访问。
/// 语义对齐 MongoPlayerDataStore:角色名 Realm 内唯一、每账号上限由调用方
/// 传入、训练按 seq 条件写。新角色 id 从 9001 递增(真实实现为随机)。
class InMemoryCharacterStore final : public game::common::RealmCharacterStore {
public:
    using CharacterRoster = game::common::CharacterRoster;
    using CreateCharacterOutcome = game::common::CreateCharacterOutcome;
    using CreateCharacterResult = game::common::CreateCharacterResult;
    using PlayerDataError = game::common::PlayerDataError;
    using RealmCharacter = game::common::RealmCharacter;
    using TrainingWrite = game::common::TrainingWrite;
    using TrainingWriteResult = game::common::TrainingWriteResult;

    struct Stored {
        std::uint64_t account_id{0};
        RealmCharacter character;
    };

    void add(std::uint64_t account_id, RealmCharacter character) {
        const std::scoped_lock lock(mutex_);
        characters_[character.character_id] = {
            account_id, std::move(character)};
    }
    [[nodiscard]] std::optional<RealmCharacter> stored(std::uint64_t id) const {
        const std::scoped_lock lock(mutex_);
        const auto found = characters_.find(id);
        if (found == characters_.end()) return std::nullopt;
        return found->second.character;
    }
    [[nodiscard]] std::uint64_t last_selected(std::uint64_t account_id) const {
        const std::scoped_lock lock(mutex_);
        const auto found = selected_.find(account_id);
        return found == selected_.end() ? 0 : found->second;
    }
    void set_unavailable(bool unavailable) {
        const std::scoped_lock lock(mutex_);
        unavailable_ = unavailable;
    }
    void close_gate() {
        const std::scoped_lock lock(mutex_);
        gate_open_ = false;
    }
    void open_gate() {
        {
            const std::scoped_lock lock(mutex_);
            gate_open_ = true;
        }
        gate_.notify_all();
    }
    [[nodiscard]] int calls() const {
        const std::scoped_lock lock(mutex_);
        return calls_;
    }

    [[nodiscard]] CharacterRoster list_characters(
        std::uint64_t account_id, std::uint32_t) const override {
        std::unique_lock lock(mutex_);
        enter(lock);
        CharacterRoster roster;
        for (const auto& [id, stored] : characters_) {
            if (stored.account_id == account_id) {
                roster.characters.push_back(stored.character);
            }
        }
        if (const auto found = selected_.find(account_id);
            found != selected_.end()) {
            roster.last_selected_character_id = found->second;
        }
        return roster;
    }

    [[nodiscard]] CreateCharacterResult create_character(
        std::uint64_t account_id,
        std::uint32_t,
        std::string_view name,
        std::size_t max_characters) override {
        std::unique_lock lock(mutex_);
        enter(lock);
        std::size_t owned = 0;
        for (const auto& [id, stored] : characters_) {
            if (stored.character.name == name) {
                return {.outcome = CreateCharacterOutcome::NameTaken};
            }
            if (stored.account_id == account_id) ++owned;
        }
        if (owned >= max_characters) {
            return {.outcome = CreateCharacterOutcome::LimitReached};
        }
        RealmCharacter character{
            .character_id = next_id_++, .name = std::string(name)};
        characters_[character.character_id] = {account_id, character};
        return {.outcome = CreateCharacterOutcome::Created,
                .character = character};
    }

    [[nodiscard]] std::optional<RealmCharacter> choose_character(
        std::uint64_t account_id,
        std::uint32_t,
        std::uint64_t character_id) override {
        std::unique_lock lock(mutex_);
        enter(lock);
        const auto found = characters_.find(character_id);
        if (found == characters_.end() ||
            found->second.account_id != account_id) {
            return std::nullopt;
        }
        selected_[account_id] = character_id;
        return found->second.character;
    }

    [[nodiscard]] std::optional<TrainingWriteResult> record_training(
        const TrainingWrite& write) override {
        std::unique_lock lock(mutex_);
        enter(lock);
        const auto found = characters_.find(write.character_id);
        if (found == characters_.end() ||
            found->second.account_id != write.account_id) {
            return std::nullopt;
        }
        auto& character = found->second.character;
        if (character.last_training_seq + 1 != write.seq) {
            return TrainingWriteResult{
                .committed = false, .character = character};
        }
        character.exp = write.exp;
        character.last_training_seq = write.seq;
        return TrainingWriteResult{
            .committed = true, .character = character};
    }

private:
    void enter(std::unique_lock<std::mutex>& lock) const {
        ++calls_;
        gate_.wait(lock, [this] { return gate_open_; });
        if (unavailable_) {
            throw PlayerDataError("player data unavailable");
        }
    }

    mutable std::mutex mutex_;
    mutable std::condition_variable gate_;
    mutable int calls_{0};
    bool gate_open_{true};
    bool unavailable_{false};
    std::uint64_t next_id_{9001};
    std::map<std::uint64_t, Stored> characters_;
    std::map<std::uint64_t, std::uint64_t> selected_;
};

}  // namespace realm::test_support
