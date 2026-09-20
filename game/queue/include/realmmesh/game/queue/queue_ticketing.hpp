#pragma once

#include "realmmesh/game/common/admission_grant.hpp"
#include "realmmesh/game/common/identity_token.hpp"
#include "realmmesh/game/common/queue_number_v2.hpp"
#include "realmmesh/game/queue/queue_core.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace realm::game::queue {

struct IssuedQueueTicket final {
    std::string queue_number_token;
    std::uint64_t number{0};
    std::uint64_t estimated_wait_seconds{0};
};

enum class QueueTicketQueryStatus {
    Queued,
    Admitted,
    InvalidQueueNumber,
    ReleaseExpired,
};

/// `/v1/queue/tickets/me` 新响应的域结果。HTTP 映射只需把 status、
/// position、estimated_wait_seconds 与可选 Admission Grant 序列化；
/// 凭据验证、release ledger 查询和确定性签发都留在模块内部。
struct QueueTicketQuery final {
    QueueTicketQueryStatus status{QueueTicketQueryStatus::InvalidQueueNumber};
    std::uint64_t number{0};
    std::uint64_t position{0};
    std::uint64_t estimated_wait_seconds{0};
    std::optional<std::string> admission_grant;
};

/// Queue Scheduler 的新凭据模块。它是未来 HTTP handler 的单一 seam：
/// 从已验证 Identity Token 发 Queue Number v2，并从 Queue Number +
/// 持久化 release batch 确定性地产生 Admission Grant。当前旧路由尚未
/// 接入本模块，外部协议激活仍由最终 cutover 负责。
class QueueTicketing final {
public:
    QueueTicketing(
        QueueCore& core,
        common::QueueNumberV2Codec queue_numbers,
        common::AdmissionGrantIssuer admission_grants);

    [[nodiscard]] IssuedQueueTicket issue(
        const common::IdentityClaims& identity,
        std::chrono::system_clock::time_point now);

    [[nodiscard]] QueueTicketQuery query(
        std::string_view queue_number_token,
        std::chrono::system_clock::time_point now) const;

private:
    QueueCore* core_;
    common::QueueNumberV2Codec queue_numbers_;
    common::AdmissionGrantIssuer admission_grants_;
};

}  // namespace realm::game::queue
