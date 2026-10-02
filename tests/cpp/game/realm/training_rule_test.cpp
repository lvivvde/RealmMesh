#include "realmmesh/game/realm/training_rule.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using realm::game::realm::TrainingRule;

const std::filesystem::path shipped_rule =
    std::filesystem::path(REALMMESH_TEST_SOURCE_DIR) / "configs" / "services" /
    "realm" / "training.lua";

class TrainingRuleFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        directory_ = std::filesystem::temp_directory_path() /
                     ("realm_training_rule_" +
                      std::string(::testing::UnitTest::GetInstance()
                                      ->current_test_info()
                                      ->name()));
        std::filesystem::create_directories(directory_);
    }
    void TearDown() override { std::filesystem::remove_all(directory_); }

    [[nodiscard]] std::filesystem::path write(const std::string& source) const {
        const auto path = directory_ / "rule.lua";
        std::ofstream(path) << source;
        return path;
    }

    std::filesystem::path directory_;
};

TEST(TrainingRuleTest, ShippedRuleTrainsAndCapsAtLevelTen) {
    TrainingRule rule(shipped_rule);
    EXPECT_EQ(rule.train(0), 10U);
    EXPECT_EQ(rule.train(890), 900U);
    EXPECT_FALSE(rule.train(900).has_value());
    EXPECT_EQ(rule.level(0), 1U);
    EXPECT_EQ(rule.level(30), 1U);
    EXPECT_EQ(rule.level(100), 2U);
    EXPECT_EQ(rule.level(900), 10U);
}

/// 帧线程可以不是构造线程(MeshHost 在测试线程构造、驱动线程 tick)。
TEST(TrainingRuleTest, CallableFromFrameThreadOtherThanConstructor) {
    TrainingRule rule(shipped_rule);
    std::optional<std::uint64_t> trained;
    std::thread frame([&] { trained = rule.train(10); });
    frame.join();
    EXPECT_EQ(trained, 20U);
    EXPECT_EQ(rule.level(100), 2U);
}

TEST(TrainingRuleTest, MissingFileFailsStartup) {
    EXPECT_THROW(
        TrainingRule(std::filesystem::path("/nonexistent/training.lua")),
        std::runtime_error);
}

TEST_F(TrainingRuleFileTest, SyntaxErrorFailsStartup) {
    EXPECT_THROW(TrainingRule(write("return {")), std::runtime_error);
}

TEST_F(TrainingRuleFileTest, MissingFunctionsFailStartup) {
    EXPECT_THROW(
        TrainingRule(write("return { train = function(exp) return exp end }")),
        std::runtime_error);
}

TEST_F(TrainingRuleFileTest, RuleWithoutIoAccess) {
    // 沙箱不提供 io/os:规则里触碰它们即报错,启动试调时就暴露。
    EXPECT_THROW(
        TrainingRule(write(R"lua(
return {
    level = function(exp) return 1 end,
    train = function(exp) return io.open("x") end,
}
)lua")),
        std::runtime_error);
}

TEST_F(TrainingRuleFileTest, MalformedResultsFailStartup) {
    EXPECT_THROW(
        TrainingRule(write(R"lua(
return {
    level = function(exp) return 1 end,
    train = function(exp) return exp end,
}
)lua")),
        std::runtime_error);
    EXPECT_THROW(
        TrainingRule(write(R"lua(
return {
    level = function(exp) return 0 end,
    train = function(exp) return exp + 1 end,
}
)lua")),
        std::runtime_error);
}

/// 只在部分经验区间出错的规则能通过启动试调,调用时仍抛异常,由
/// RealmSessions 收敛为 3010。
TEST_F(TrainingRuleFileTest, LaterRuleErrorsThrowAtCallTime) {
    TrainingRule rule(write(R"lua(
return {
    level = function(exp) return 1 end,
    train = function(exp)
        if exp >= 10 then error("boom") end
        return exp + 10
    end,
}
)lua"));
    EXPECT_EQ(rule.train(0), 10U);
    EXPECT_THROW(static_cast<void>(rule.train(10)), std::runtime_error);
}

}  // namespace
