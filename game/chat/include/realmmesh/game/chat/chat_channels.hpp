#pragma once
#include "realmmesh/game/chat/chat_config.hpp"
#include "realmmesh/game/common/chat_protocol.hpp"
#include <chrono>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>
namespace realm::game::chat {
namespace wire = ::realmmesh::protocol::chat::v1;
[[nodiscard]] common::ChatInternalReply chat_reply(
    const common::ChatInternalRequest& request, wire::Status status);
// 单线程属主；传输未处理的字节不是已接纳请求。回复取走前仍占请求名额。
class ChatChannels final {
public:
    using Clock = std::chrono::steady_clock;
    using Peer = std::uint64_t;
    struct Reply {
        Peer peer;
        std::uint64_t request_id;
        common::ChatInternalReply message;
        double duration_seconds;
    };
    struct Push { Peer peer; std::vector<std::byte> payload; };
    struct Stats {
        std::uint64_t accepted{}, rejected{}, replayed{}, queue_rejected{}, expired{};
        std::uint64_t rate_rejected{}, stale_rejected{}, resource_rejected{};
    };
    ChatChannels(ChatConfig config, std::string instance_id, std::string incarnation);
    [[nodiscard]] bool opened(Peer peer);
    void closed(Peer peer);
    // 只接纳已由 common 编解码校验的请求；Full 不执行任何业务变更。
    [[nodiscard]] bool submit(Peer peer, const common::ChatInternalRequest& request,
                              std::uint64_t request_id, Clock::time_point now = Clock::now());
    void run_round(Clock::time_point now = Clock::now());
    [[nodiscard]] std::optional<Reply> take_reply();
    [[nodiscard]] std::vector<Push> drain_pushes();
    [[nodiscard]] std::vector<Peer> take_closed_peers();
    [[nodiscard]] std::size_t active_members() const noexcept;
    [[nodiscard]] std::size_t pending_requests() const noexcept { return pending_; }
    [[nodiscard]] std::size_t pending_pushes() const noexcept { return pushes_.size(); }
    [[nodiscard]] std::size_t pending_push_bytes() const noexcept { return push_bytes_; }
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    [[nodiscard]] static std::optional<std::uint64_t> next_counter(std::uint64_t value) noexcept;
private:
    struct Pending {
        common::ChatInternalRequest request;
        std::uint64_t id;
        Clock::time_point deadline;
    };
    struct LinkState {
        std::optional<wire::Link> link;
        std::uint64_t highest_generation{};
        std::deque<Pending> requests;
    };
    struct Member {
        Peer peer;
        wire::Membership membership;
        common::ChatState state;
        std::string last_text;
        double tokens{static_cast<double>(common::chat_send_burst)};
        Clock::time_point refill;
    };
    struct Channel {
        wire::Epoch epoch;
        std::uint64_t sequence{};
        std::map<std::uint64_t, Member> members;
    };
    struct PendingPush {
        Peer peer;
        wire::Membership membership;
        std::vector<std::byte> payload;
    };
    [[nodiscard]] common::ChatInternalReply handle(Peer peer, const Pending& request, Clock::time_point now);
    void send(Channel& channel, Member& member, const common::SendChat& send,
              common::ChatSendResult& result, Clock::time_point now);
    ChatConfig config_;
    std::string instance_id_, incarnation_;
    std::map<std::uint64_t, Channel> channels_;
    std::map<Peer, LinkState> links_;
    std::deque<Reply> replies_;
    std::deque<PendingPush> pushes_;
    std::vector<Peer> closed_peers_;
    std::size_t pending_{}, push_bytes_{};
    std::uint64_t link_generation_{};
    Peer next_peer_{};
    Stats stats_;
};
}  // namespace realm::game::chat
