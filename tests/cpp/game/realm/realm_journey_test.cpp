#include "journey_fixture.hpp"

namespace realm::game::realm {
namespace {

template <typename T>
void expect_rejected(
    const client::RealmReply<T>& reply, int code, std::string_view step) {
    EXPECT_EQ(reply.status, RealmCallStatus::Rejected) << step;
    EXPECT_EQ(reply.error_code, code) << step;
}

[[nodiscard]] const client::RealmCharacterView* find_character(
    const client::RealmRoster& roster, std::uint64_t character_id) {
    for (const auto& character : roster.characters) {
        if (character.character_id == character_id) return &character;
    }
    return nullptr;
}

TEST_F(RealmJourneyTest, CharacterLifecycleSurvivesReloginAndRealmRestart) {
    // 1. 新账号走完 Login Chain:Realm 里没有角色,也没有上次选择。
    auto first = login();
    ASSERT_NE(first, nullptr);
    {
        const auto roster = first->list_characters(soon());
        ASSERT_TRUE(roster.ok());
        EXPECT_TRUE(roster.value.characters.empty());
        EXPECT_EQ(roster.value.last_selected_character_id, 0U);
    }

    // 2. 建角:新角色经验 0、1 级。
    const auto hero = first->create_character("Hero", soon());
    ASSERT_TRUE(hero.ok());
    EXPECT_EQ(hero.value.name, "Hero");
    EXPECT_EQ(hero.value.exp, 0U);
    EXPECT_EQ(hero.value.level, 1U);
    const auto hero_id = hero.value.character_id;

    // 8. 重名(本 Realm 内唯一,含他人角色)与每账号 3 个上限。
    expect_rejected(
        first->create_character("Hero", soon()), 3005, "same-account name");
    expect_rejected(
        first->create_character(other_account_character_name, soon()),
        3005,
        "other account's name");
    const auto mage = first->create_character("Mage", soon());
    ASSERT_TRUE(mage.ok());
    const auto mage_id = mage.value.character_id;
    ASSERT_TRUE(first->create_character("Rogue", soon()).ok());
    expect_rejected(
        first->create_character("Bard", soon()), 3006, "fourth character");

    // 5. 选他人的角色被拒(3007),会话仍在选角阶段。
    expect_rejected(
        first->select_character(other_account_character_id, soon()),
        3007,
        "other account's character");

    // 9. 选角阶段训练是阶段错位(3003),会话存活。
    expect_rejected(first->train(1, soon()), 3003, "train while selecting");

    // 2. 选中自己的角色,进入游戏中。
    {
        const auto selected = first->select_character(hero_id, soon());
        ASSERT_TRUE(selected.ok());
        EXPECT_EQ(selected.value.character.character_id, hero_id);
        EXPECT_EQ(selected.value.training_seq, 0U);
    }

    // 9. 游戏中再列表/建角同样是阶段错位,会话存活。
    expect_rejected(first->list_characters(soon()), 3003, "list in game");
    expect_rejected(
        first->create_character("Late", soon()), 3003, "create in game");
    EXPECT_TRUE(first->heartbeat(soon()));

    // 3. 训练 3 次:每次 +10 经验,仍是 1 级。
    for (std::uint64_t seq = 1; seq <= 3; ++seq) {
        const auto trained = first->train(seq, soon());
        ASSERT_TRUE(trained.ok()) << "seq " << seq;
        EXPECT_EQ(trained.value.exp, seq * 10);
        EXPECT_EQ(trained.value.level, 1U);
        EXPECT_EQ(trained.value.seq, seq);
        EXPECT_FALSE(trained.value.replayed);
    }

    // 6. 重放上一个序号得到幂等结果;跳号被拒(3009)。
    {
        const auto replayed = first->train(3, soon());
        ASSERT_TRUE(replayed.ok());
        EXPECT_TRUE(replayed.value.replayed);
        EXPECT_EQ(replayed.value.exp, 30U);
        EXPECT_EQ(replayed.value.seq, 3U);
    }
    expect_rejected(first->train(5, soon()), 3009, "skipped seq");
    expect_rejected(first->train(2, soon()), 3009, "stale seq");

    // 4. 断线后重跑 Login Chain:读到 exp 30 与上次选择。
    first->close();
    first.reset();
    auto second = login();
    ASSERT_NE(second, nullptr);
    {
        const auto roster = second->list_characters(soon());
        ASSERT_TRUE(roster.ok());
        EXPECT_EQ(roster.value.characters.size(), 3U);
        EXPECT_EQ(roster.value.last_selected_character_id, hero_id);
        const auto* stored = find_character(roster.value, hero_id);
        ASSERT_NE(stored, nullptr);
        EXPECT_EQ(stored->exp, 30U);
        EXPECT_EQ(stored->level, 1U);
    }

    // 7. 练满:经验 900 即 10 级,再训练被拒(3008)。
    {
        const auto selected = second->select_character(mage_id, soon());
        ASSERT_TRUE(selected.ok());
        EXPECT_EQ(selected.value.training_seq, 0U);
    }
    for (std::uint64_t seq = 1; seq <= 90; ++seq) {
        const auto trained = second->train(seq, soon());
        ASSERT_TRUE(trained.ok()) << "seq " << seq;
        EXPECT_EQ(trained.value.exp, seq * 10);
        EXPECT_EQ(trained.value.level, seq * 10 / 100 + 1);
    }
    expect_rejected(second->train(91, soon()), 3008, "beyond max level");

    // 10. 顶替:新会话入场后旧会话收到 1409 并断开;新会话从选角开始。
    auto third = login();
    ASSERT_NE(third, nullptr);
    {
        const auto displaced = second->train(91, soon());
        EXPECT_EQ(displaced.status, RealmCallStatus::Displaced);
        EXPECT_FALSE(second->heartbeat(client::Clock::now() + 200ms));
    }
    expect_rejected(third->train(1, soon()), 3003, "new session selecting");
    {
        const auto roster = third->list_characters(soon());
        ASSERT_TRUE(roster.ok());
        EXPECT_EQ(roster.value.last_selected_character_id, mage_id);
    }

    // 11. Realm 重启:在线会话断开,重新登录读到已确认的状态。
    restart_realm();
    EXPECT_FALSE(third->heartbeat(client::Clock::now() + 500ms));
    third.reset();
    second.reset();
    auto fourth = login();
    ASSERT_NE(fourth, nullptr);
    {
        const auto roster = fourth->list_characters(soon());
        ASSERT_TRUE(roster.ok());
        EXPECT_EQ(roster.value.characters.size(), 3U);
        EXPECT_EQ(roster.value.last_selected_character_id, mage_id);
        const auto* stored_hero = find_character(roster.value, hero_id);
        ASSERT_NE(stored_hero, nullptr);
        EXPECT_EQ(stored_hero->exp, 30U);
        const auto* stored_mage = find_character(roster.value, mage_id);
        ASSERT_NE(stored_mage, nullptr);
        EXPECT_EQ(stored_mage->exp, 900U);
        EXPECT_EQ(stored_mage->level, 10U);
    }
    {
        const auto selected = fourth->select_character(hero_id, soon());
        ASSERT_TRUE(selected.ok());
        EXPECT_EQ(selected.value.training_seq, 3U);
        EXPECT_EQ(selected.value.character.exp, 30U);
    }
    const auto resumed = fourth->train(4, soon());
    ASSERT_TRUE(resumed.ok());
    EXPECT_EQ(resumed.value.exp, 40U);
    fourth->close();
}

}  // namespace
}  // namespace realm::game::realm
