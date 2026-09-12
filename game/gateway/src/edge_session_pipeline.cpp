#include "realmmesh/game/gateway/edge_session_pipeline.hpp"

#include <stdexcept>

namespace realm::game::gateway {

EdgeSessionPipeline::EdgeSessionPipeline(
    std::uint64_t conn_capacity, std::uint64_t fetch_capacity)
    : conn_capacity_(conn_capacity),
      fetch_capacity_(fetch_capacity) {
    if (conn_capacity_ == 0 || fetch_capacity_ == 0) {
        throw std::invalid_argument(
            "edge session pipeline requires positive capacities");
    }
}

void EdgeSessionPipeline::on_session_opened(EdgeSessionId session_id) {
    if (conn_used_ == conn_capacity_) {
        // conn 容量取启用传输 max_sessions 之和(ServiceHost 同源注入):
        // 管线满即传输满,此分支仅防御超量开表。
        return;
    }
    if (sessions_.emplace(session_id, Entry{}).second) {
        ++conn_used_;
    }
}

std::optional<ClosedPipelineSession> EdgeSessionPipeline::on_session_closed(
    EdgeSessionId session_id) {
    const auto found = sessions_.find(session_id);
    if (found == sessions_.end()) {
        return std::nullopt;
    }
    const auto stage = found->second.stage;
    sessions_.erase(found);
    --conn_used_;
    if (stage == EdgeSessionStage::Fetching) {
        --fetch_used_;  // 归还双预算(spec §5.3:fetching 关闭路径)
    }
    return ClosedPipelineSession{session_id, stage};
}

EnterFetchingResult EdgeSessionPipeline::try_enter_fetching(
    EdgeSessionId session_id) {
    const auto found = sessions_.find(session_id);
    if (found == sessions_.end()) {
        return EnterFetchingResult::UnknownSession;
    }
    if (found->second.stage != EdgeSessionStage::Pending) {
        return EnterFetchingResult::NotPending;
    }
    if (fetch_used_ == fetch_capacity_) {
        // 额度外拒绝:直接迁入 closed,连接占用由随后的关闭事件归还。
        found->second.stage = EdgeSessionStage::Closed;
        return EnterFetchingResult::AtCapacity;
    }
    found->second.stage = EdgeSessionStage::Fetching;
    ++fetch_used_;
    return EnterFetchingResult::Entered;
}

bool EdgeSessionPipeline::mark_handed_off(EdgeSessionId session_id) {
    const auto found = sessions_.find(session_id);
    if (found == sessions_.end() ||
        found->second.stage != EdgeSessionStage::Fetching) {
        return false;
    }
    found->second.stage = EdgeSessionStage::HandedOff;
    --fetch_used_;  // 拉取完成:并发槽即还(连接保留到收尾关闭)
    return true;
}

EdgeSessionStage EdgeSessionPipeline::stage(EdgeSessionId session_id) const {
    const auto found = sessions_.find(session_id);
    return found == sessions_.end() ? EdgeSessionStage::Closed
                                    : found->second.stage;
}

}  // namespace realm::game::gateway
