#include "realmmesh/loadgen/robot.hpp"

#include "realmmesh/game/common/edge_protocol.hpp"
#include "realmmesh/loadgen/edge_client.hpp"
#include "realmmesh/loadgen/http_client.hpp"
#include "realmmesh/loadgen/json_field.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace realm::loadgen {
namespace {

namespace common = ::realm::game::common;
namespace edge_v1 = ::realmmesh::protocol::edge::v1;

[[nodiscard]] double elapsed_ms(std::chrono::steady_clock::time_point from) {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - from)
        .count();
}

/// verify 相位:返回 identity_token;失败计入 verify 计数器。
[[nodiscard]] std::optional<std::string> run_verify(
    const RobotOptions& options,
    PhaseCounters& verify) {
    const auto started = std::chrono::steady_clock::now();
    auto connection = TlsHttpConnection::dial(
        options.endpoints.login_verify, options.deadline);
    if (connection == nullptr) {
        verify.record_failure(FailureKind::ConnectionError, elapsed_ms(started));
        return std::nullopt;
    }
    const std::string body = "{\"account\":\"" + options.account +
                             "\",\"credential\":\"" + options.credential +
                             "\"}";
    const auto response = connection->request(
        "POST", "/v1/login/verify", std::nullopt, body, options.deadline);
    if (!response.has_value()) {
        verify.record_failure(FailureKind::ConnectionError, elapsed_ms(started));
        return std::nullopt;
    }
    const auto token =
        extract_json_string_field(response->body, "identity_token");
    if (response->status != 200 || !token.has_value()) {
        verify.record_failure(FailureKind::VerifyRejected, elapsed_ms(started));
        return std::nullopt;
    }
    verify.record_success(elapsed_ms(started));
    return token;
}

/// 取号相位:返回 (number, queue_number_token)。连接由调用方拨好并
/// 跨相位复用(keep-alive):万级短连洪流的握手次数减半。
[[nodiscard]] std::optional<std::pair<std::uint64_t, std::string>> run_tickets(
    const RobotOptions& options,
    TlsHttpConnection& connection,
    const std::string& identity_token,
    PhaseCounters& tickets) {
    const auto started = std::chrono::steady_clock::now();
    const auto response = connection.request(
        "POST", "/v1/queue/tickets", identity_token, "", options.deadline);
    if (!response.has_value()) {
        tickets.record_failure(FailureKind::ConnectionError, elapsed_ms(started));
        return std::nullopt;
    }
    const auto token =
        extract_json_string_field(response->body, "queue_number_token");
    const auto number = extract_json_int_field(response->body, "number");
    if (response->status != 202 || !token.has_value() || !number.has_value() ||
        *number < 0) {
        tickets.record_failure(
            FailureKind::TicketsRejected, elapsed_ms(started));
        return std::nullopt;
    }
    tickets.record_success(elapsed_ms(started));
    return std::pair{static_cast<std::uint64_t>(*number), *token};
}

/// 轮询相位:progress 轮询(客户端以 released_number 本地判定,
/// ADR-0006)+ 放行兑换(admitted 号牌 grant,嵌套在 admit_grant 内,
/// 是响应体首个 queue_number_token)。每个请求记一次时延。
[[nodiscard]] std::optional<std::string> run_poll(
    const RobotOptions& options,
    TlsHttpConnection& connection,
    std::uint64_t number,
    const std::string& number_token,
    PhaseCounters& poll) {
    for (;;) {
        const auto started = std::chrono::steady_clock::now();
        const auto response = connection.request(
            "GET", "/v1/queue/progress", std::nullopt, "", options.deadline);
        const auto released =
            response.has_value()
                ? extract_json_int_field(response->body, "released_number")
                : std::nullopt;
        if (!response.has_value() || response->status != 200 ||
            !released.has_value() || *released < 0) {
            poll.record_failure(
                response.has_value() ? FailureKind::PollFailed
                                     : FailureKind::ConnectionError,
                elapsed_ms(started));
            return std::nullopt;
        }
        poll.record_success(elapsed_ms(started));
        if (static_cast<std::uint64_t>(*released) >= number) {
            break;
        }
        // 截止前最后一个窗口也要留出轮询本身的时间。
        if (std::chrono::steady_clock::now() + options.poll_interval >=
            options.deadline) {
            poll.record_failure(FailureKind::AdmitTimeout, 0);
            return std::nullopt;
        }
        std::this_thread::sleep_for(options.poll_interval);
    }

    const auto started = std::chrono::steady_clock::now();
    const auto response = connection.request(
        "GET", "/v1/queue/tickets/me", number_token, "", options.deadline);
    if (!response.has_value()) {
        poll.record_failure(FailureKind::ConnectionError, elapsed_ms(started));
        return std::nullopt;
    }
    const auto status = extract_json_string_field(response->body, "status");
    const auto grant =
        extract_json_string_field(response->body, "queue_number_token");
    if (response->status != 200 || !status.has_value() ||
        *status != "admitted" || !grant.has_value() || grant->empty()) {
        poll.record_failure(FailureKind::PollFailed, elapsed_ms(started));
        return std::nullopt;
    }
    poll.record_success(elapsed_ms(started));
    return grant;
}

/// 建连相位:发 attach 帧 + 等 1302 受理(连接由调用方拨好)。
[[nodiscard]] bool run_attach(
    const RobotOptions& options,
    TlsEdgeConnection& connection,
    const std::string& identity_token,
    const std::string& grant_token,
    PhaseCounters& attach) {
    const auto started = std::chrono::steady_clock::now();
    common::EdgeAttach message;
    message.set_identity_token(identity_token);
    message.set_queue_number_token(grant_token);
    if (!connection.send_frame(common::encode(message, 0), options.deadline)) {
        attach.record_failure(FailureKind::ConnectionError, elapsed_ms(started));
        return false;
    }
    for (;;) {
        const auto payload = connection.receive_frame(options.deadline);
        if (!payload.has_value()) {
            attach.record_failure(
                FailureKind::AttachTimeout, elapsed_ms(started));
            return false;
        }
        const auto message_id = common::edge_message_id(*payload);
        if (!message_id.has_value()) {
            attach.record_failure(
                FailureKind::AttachRejected, elapsed_ms(started));
            return false;
        }
        if (*message_id == edge_v1::MESSAGE_ID_S2C_EDGE_ATTACH_ACCEPTED) {
            attach.record_success(elapsed_ms(started));
            return true;
        }
        if (*message_id == edge_v1::MESSAGE_ID_S2C_ERROR) {
            attach.record_failure(
                FailureKind::AttachRejected, elapsed_ms(started));
            return false;
        }
        // 其余帧(不应出现):忽略继续等受理。
    }
}

/// 交付相位:等服务器推送 1303 EnterRealmGranted(handed-off 的
/// 客户端可见事件,#45)。
[[nodiscard]] bool run_handoff(
    const RobotOptions& options,
    TlsEdgeConnection& connection,
    PhaseCounters& handoff) {
    const auto started = std::chrono::steady_clock::now();
    for (;;) {
        const auto payload = connection.receive_frame(options.deadline);
        if (!payload.has_value()) {
            handoff.record_failure(
                FailureKind::HandoffTimeout, elapsed_ms(started));
            return false;
        }
        const auto message_id = common::edge_message_id(*payload);
        if (!message_id.has_value()) {
            handoff.record_failure(
                FailureKind::HandoffRejected, elapsed_ms(started));
            return false;
        }
        if (*message_id == edge_v1::MESSAGE_ID_S2C_ENTER_REALM_GRANTED) {
            handoff.record_success(elapsed_ms(started));
            return true;
        }
        if (*message_id == edge_v1::MESSAGE_ID_S2C_ERROR) {
            handoff.record_failure(
                FailureKind::HandoffRejected, elapsed_ms(started));
            return false;
        }
    }
}

}  // namespace

std::optional<RobotPhase> parse_robot_phase(std::string_view text) {
    if (text == "verify") return RobotPhase::Verify;
    if (text == "tickets") return RobotPhase::Tickets;
    if (text == "poll") return RobotPhase::Poll;
    if (text == "gateway") return RobotPhase::Gateway;
    if (text == "all") return RobotPhase::All;
    return std::nullopt;
}

RobotOutcome run_robot(
    const RobotOptions& options,
    RobotCounters counters) {
    PhaseCounters& verify = counters.verify;
    PhaseCounters& tickets = counters.tickets;
    PhaseCounters& poll = counters.poll;
    PhaseCounters& attach = counters.attach;
    PhaseCounters& handoff = counters.handoff;
    // Gateway = 单趟链路到 handed-off;All = handed-off 后保持连接到
    // 保持截止(soak 语境:会话停在 handed-off 段撑在线水位,由服务端
    // 宽限或保持到点收尾),单趟即返回。
    std::string number_token;
    // 队列连接按机器人复用(keep-alive):取号、轮询、兑换同连接,
    // 万级短连下的握手总量减半;连接随机器人结束关闭(RST)。
    std::unique_ptr<TlsHttpConnection> queue_connection;
    for (;;) {
        auto identity = run_verify(options, verify);
        if (!identity.has_value()) {
            return {.completed = false,
                    .failure = verify.last_failure,
                    .number_token = std::move(number_token)};
        }
        if (options.phase == RobotPhase::Verify) {
            return {.completed = true,
                    .failure = FailureKind::None,
                    .number_token = std::move(number_token)};
        }

        if (queue_connection == nullptr) {
            queue_connection = TlsHttpConnection::dial(
                options.endpoints.queue, options.deadline);
            if (queue_connection == nullptr) {
                tickets.record_failure(
                    FailureKind::ConnectionError,
                    elapsed_ms(std::chrono::steady_clock::now()));
                return {.completed = false,
                        .failure = tickets.last_failure,
                        .number_token = std::move(number_token)};
            }
        }
        const auto issued =
            run_tickets(options, *queue_connection, *identity, tickets);
        if (!issued.has_value()) {
            return {.completed = false,
                    .failure = tickets.last_failure,
                    .number_token = std::move(number_token)};
        }
        if (options.collect_artifacts) {
            number_token = issued->second;
        }
        if (options.phase == RobotPhase::Tickets) {
            return {.completed = true,
                    .failure = FailureKind::None,
                    .number_token = std::move(number_token)};
        }

        const auto grant = run_poll(
            options, *queue_connection, issued->first, issued->second, poll);
        if (!grant.has_value()) {
            return {.completed = false,
                    .failure = poll.last_failure,
                    .number_token = std::move(number_token)};
        }
        if (options.phase == RobotPhase::Poll) {
            return {.completed = true,
                    .failure = FailureKind::None,
                    .number_token = std::move(number_token)};
        }

        auto connection = TlsEdgeConnection::dial(
            options.endpoints.gateway, options.deadline);
        if (connection == nullptr) {
            attach.record_failure(
                FailureKind::ConnectionError,
                elapsed_ms(std::chrono::steady_clock::now()));
            return {.completed = false,
                    .failure = attach.last_failure,
                    .number_token = std::move(number_token)};
        }
        if (!run_attach(options, *connection, *identity, *grant, attach)) {
            return {.completed = false,
                    .failure = attach.last_failure,
                    .number_token = std::move(number_token)};
        }
        if (!run_handoff(options, *connection, handoff)) {
            return {.completed = false,
                    .failure = handoff.last_failure,
                    .number_token = std::move(number_token)};
        }
        if (options.phase == RobotPhase::Gateway) {
            return {.completed = true,
                    .failure = FailureKind::None,
                    .number_token = std::move(number_token)};
        }
        // All(soak 语境):handed-off 后按住连接到保持截止(缺省总
        // 截止),会话保持在服务端 handed-off 段(在线水位);到点关闭
        // 即完成。
        const auto hold_horizon =
            options.hold_until.value_or(options.deadline);
        while (std::chrono::steady_clock::now() < hold_horizon &&
               std::chrono::steady_clock::now() < options.deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
        return {.completed = true,
                .failure = FailureKind::None,
                .number_token = std::move(number_token)};
    }
}

}  // namespace realm::loadgen
