#pragma once
#include <atomic>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
#include "realmmesh/concurrency/bounded_queue.hpp"
#include "realmmesh/game/common/chat_protocol.hpp"
#include "realmmesh/game/realm/realm_config.hpp"
#include "realmmesh/network/client/tls_client_stream.hpp"
#include "realmmesh/network/codec/length_field_codec.hpp"
namespace realm::game::realm::chat_detail {
using Clock = std::chrono::steady_clock;
struct Completion {
    std::optional<common::ChatInternalReply> reply;
    bool dispatched{false};
};
struct Call {
    common::ChatInternalRequest request;
    Clock::time_point deadline;
    std::uint64_t revision;
    std::promise<Completion> promise;
};
/// 帧线程拥有计数（含未取完成），IO 线程独占 stream/link。队列最多8个。
class Peer final {
public:
    Peer(
        RealmConfig::ChatClient,
        RealmConfig::ChatTarget,
        std::string incarnation);
    ~Peer();
    std::optional<std::future<Completion>> submit(
        common::ChatInternalRequest, Clock::time_point);
    std::vector<common::ChatInternalPush> drain(std::size_t max);
    void collected() { --outstanding_; }
    std::uint64_t revision() const { return revision_.load(); }
    void invalidate() { reset_ = true; }
    const RealmConfig::ChatTarget target;

private:
    void run(std::stop_token);
    void disconnect();
    void connect(Clock::time_point);
    Completion exchange(common::ChatInternalRequest, Clock::time_point);
    std::optional<std::vector<std::byte>> receive(Clock::time_point);
    void push(common::ChatInternalPush);
    RealmConfig::ChatClient config_;
    std::string incarnation_;
    concurrency::BoundedQueue<Call> calls_{
        common::chat_queued_requests_per_link_max};
    std::size_t outstanding_{0};  // 仅帧线程；完成未取走不归还。
    std::atomic_uint64_t revision_{0};
    std::atomic_bool reset_{false};
    std::mutex mutex_;
    std::deque<common::ChatInternalPush> pushes_;
    std::size_t push_bytes_{0};
    std::shared_ptr<network::client::TlsClientStream> stream_;
    network::LengthFieldCodec codec_{common::chat_envelope_max};
    network::ByteBuffer input_;
    ::realmmesh::protocol::chat::v1::Link link_;
    std::uint64_t next_id_{1};
    std::jthread thread_;
};
}  // namespace realm::game::realm::chat_detail
