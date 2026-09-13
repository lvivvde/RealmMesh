#include "realmmesh/game/gateway/edge_session_pipeline.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace realm::game::gateway {
namespace {

constexpr EdgeSessionId sid(std::uint64_t value) { return {value}; }

class EdgeSessionPipelineTest : public ::testing::Test {
protected:
    void SetUp() override { pipeline_.emplace(4U, 2U); }

    std::optional<EdgeSessionPipeline> pipeline_;
};

TEST_F(EdgeSessionPipelineTest, OpenedSessionsOccupyAndReleaseConnectionSlots) {
    pipeline_->on_session_opened(sid(1));
    pipeline_->on_session_opened(sid(2));
    EXPECT_EQ(pipeline_->conn_used(), 2U);
    EXPECT_EQ(pipeline_->conn_free(), 2U);

    EXPECT_TRUE(pipeline_->on_session_closed(sid(1)).has_value());
    EXPECT_EQ(pipeline_->conn_used(), 1U);

    // 未知会话的关闭是空操作(重复终结通知安全)。
    EXPECT_FALSE(pipeline_->on_session_closed(sid(1)).has_value());
    EXPECT_EQ(pipeline_->conn_used(), 1U);
}

TEST_F(EdgeSessionPipelineTest, AttachMovesPendingToFetchingAndHoldsFetchSlot) {
    pipeline_->on_session_opened(sid(1));
    EXPECT_EQ(pipeline_->stage(sid(1)), EdgeSessionStage::Pending);

    EXPECT_EQ(pipeline_->try_enter_fetching(sid(1)),
              EnterFetchingResult::Entered);
    EXPECT_EQ(pipeline_->stage(sid(1)), EdgeSessionStage::Fetching);
    EXPECT_EQ(pipeline_->fetch_used(), 1U);

    // 拉取完成 → handed-off:fetch 槽释放,连接占用保留到关闭。
    EXPECT_TRUE(pipeline_->mark_handed_off(sid(1)));
    EXPECT_EQ(pipeline_->stage(sid(1)), EdgeSessionStage::HandedOff);
    EXPECT_EQ(pipeline_->fetch_used(), 0U);
    EXPECT_EQ(pipeline_->conn_used(), 1U);
}

TEST_F(EdgeSessionPipelineTest, RejectsIllegalTransitions) {
    pipeline_->on_session_opened(sid(1));

    // 未知会话。
    EXPECT_EQ(pipeline_->try_enter_fetching(sid(9)),
              EnterFetchingResult::UnknownSession);
    EXPECT_FALSE(pipeline_->mark_handed_off(sid(9)));

    // 重复进入 fetching 非法。
    ASSERT_EQ(pipeline_->try_enter_fetching(sid(1)),
              EnterFetchingResult::Entered);
    EXPECT_EQ(pipeline_->try_enter_fetching(sid(1)),
              EnterFetchingResult::NotPending);

    // pending 不能直接 handed-off。
    pipeline_->on_session_opened(sid(2));
    EXPECT_FALSE(pipeline_->mark_handed_off(sid(2)));
}

TEST_F(EdgeSessionPipelineTest, AtCapacityRejectsAttachAndClosesSession) {
    pipeline_->on_session_opened(sid(1));
    pipeline_->on_session_opened(sid(2));
    pipeline_->on_session_opened(sid(3));
    ASSERT_EQ(pipeline_->try_enter_fetching(sid(1)),
              EnterFetchingResult::Entered);
    ASSERT_EQ(pipeline_->try_enter_fetching(sid(2)),
              EnterFetchingResult::Entered);

    // 拉取池满:第三个 attach 被拒,会话直接迁入 closed(额度外拒绝)。
    EXPECT_EQ(pipeline_->try_enter_fetching(sid(3)),
              EnterFetchingResult::AtCapacity);
    EXPECT_EQ(pipeline_->stage(sid(3)), EdgeSessionStage::Closed);
    EXPECT_EQ(pipeline_->fetch_used(), 2U);

    // 已 closed 的会话不再接受迁移。
    EXPECT_EQ(pipeline_->try_enter_fetching(sid(3)),
              EnterFetchingResult::NotPending);
}

TEST_F(EdgeSessionPipelineTest, CloseFromFetchingReturnsBothBudgets) {
    pipeline_->on_session_opened(sid(1));
    pipeline_->on_session_opened(sid(2));
    ASSERT_EQ(pipeline_->try_enter_fetching(sid(1)),
              EnterFetchingResult::Entered);
    ASSERT_EQ(pipeline_->try_enter_fetching(sid(2)),
              EnterFetchingResult::Entered);
    EXPECT_EQ(pipeline_->fetch_used(), 2U);

    const auto closed = pipeline_->on_session_closed(sid(1));
    ASSERT_TRUE(closed.has_value());
    EXPECT_EQ(closed->stage, EdgeSessionStage::Fetching);
    EXPECT_EQ(pipeline_->fetch_used(), 1U);
    EXPECT_EQ(pipeline_->conn_used(), 1U);
}

TEST_F(EdgeSessionPipelineTest, UnknownSessionStageReadsAsClosed) {
    EXPECT_EQ(pipeline_->stage(sid(42)), EdgeSessionStage::Closed);
}

TEST_F(EdgeSessionPipelineTest, StageCountsReflectLiveSessionsPerStage) {
    pipeline_->on_session_opened(sid(1));
    pipeline_->on_session_opened(sid(2));
    pipeline_->on_session_opened(sid(3));
    pipeline_->on_session_opened(sid(4));
    ASSERT_EQ(pipeline_->try_enter_fetching(sid(1)),
              EnterFetchingResult::Entered);
    ASSERT_EQ(pipeline_->mark_handed_off(sid(1)), true);
    ASSERT_EQ(pipeline_->try_enter_fetching(sid(3)),
              EnterFetchingResult::Entered);

    const auto counts = pipeline_->stage_counts();
    EXPECT_EQ(counts.pending, 2U);
    EXPECT_EQ(counts.fetching, 1U);
    EXPECT_EQ(counts.handed_off, 1U);

    // closed 会话不在任何活动阶段计数里:pending 的 sid(4) 迁入
    // fetching 后被关闭,pending 与 fetching 计数双双回落。
    ASSERT_EQ(pipeline_->try_enter_fetching(sid(4)),
              EnterFetchingResult::Entered);
    static_cast<void>(pipeline_->on_session_closed(sid(4)));
    const auto after = pipeline_->stage_counts();
    EXPECT_EQ(after.pending, 1U);
    EXPECT_EQ(after.fetching, 1U);
    EXPECT_EQ(after.handed_off, 1U);
}

}  // namespace
}  // namespace realm::game::gateway
