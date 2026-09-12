#include "realmmesh/game/gateway/edge_attach_chain.hpp"

namespace realm::game::gateway {

EdgeAttachChain::EdgeAttachChain(
    const common::IdentityTokenCodec& identity_codec,
    const common::QueueNumberCodec& number_codec,
    EdgeSessionPipeline& pipeline,
    std::string_view identity_issuer)
    : identity_codec_(&identity_codec),
      number_codec_(&number_codec),
      pipeline_(&pipeline),
      identity_issuer_(identity_issuer) {}

EdgeAttachVerdict EdgeAttachChain::handle(
    EdgeSessionId session,
    std::string_view identity_token,
    std::string_view number_token,
    std::chrono::system_clock::time_point now) {
    if (pipeline_->stage(session) != EdgeSessionStage::Pending) {
        return EdgeAttachVerdict::NotPending;
    }
    // 探针先于一切凭据消费:满额时 jti 尚未烧掉,客户端可在额度恢复后
    // 重开连接原样重试。
    if (pipeline_->fetch_free() == 0U) {
        return EdgeAttachVerdict::OutOfBudget;
    }

    const auto identity =
        identity_codec_->validate(identity_token, identity_issuer_, now);
    if (!identity.has_value()) {
        return EdgeAttachVerdict::InvalidCredentials;
    }
    const auto number = number_codec_->validate(number_token, now);
    if (!number.has_value() || !number->admitted) {
        return EdgeAttachVerdict::InvalidNumber;
    }
    if (!replay_guard_.consume(*identity, now)) {
        return EdgeAttachVerdict::InvalidCredentials;
    }

    // 探针已过且本链单线程,此处迁移不应失败;兜底按额度拒绝(该路径
    // 只在阶段表被并发改动时可达,jti 已消费属可接受的防御代价)。
    if (pipeline_->try_enter_fetching(session) !=
        EnterFetchingResult::Entered) {
        return EdgeAttachVerdict::OutOfBudget;
    }
    last_account_id_ = identity->account_id;
    return EdgeAttachVerdict::Accepted;
}

}  // namespace realm::game::gateway
