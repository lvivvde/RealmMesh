#pragma once
#include <atomic>
#include <future>
#include <mutex>
#include <thread>
#include "realmmesh/concurrency/bounded_queue.hpp"
#include "realmmesh/game/common/scene_internal_protocol.hpp"
#include "realmmesh/game/common/scene_protocol.hpp"
#include "realmmesh/game/realm/realm_config.hpp"
#include "realmmesh/network/client/tls_client_stream.hpp"
#include "realmmesh/network/codec/length_field_codec.hpp"
#include "realmmesh/network/transport/transport_config.hpp"

namespace realm::game::realm::scene_detail {
struct Failure {
    int code;
};
/// 一个实例一条独立的有界 IO 执行器。实例故障只终止自己的 stream。
class Peer {
public:
    using Clock = std::chrono::steady_clock;
    Peer(
        RealmConfig::SceneClient tls,
        RealmConfig::SceneTarget target,
        std::string realm_incarnation,
        concurrency::BoundedQueue<common::ScenePush>& pushes);
    ~Peer();
    std::future<common::SceneReply> exchange(
        common::SceneRequest request, Clock::time_point deadline);
    std::string incarnation() const;
    void endpoint(std::optional<network::TransportEndpoint> value);
    std::uint64_t generation() const { return generation_.load(); }
    const RealmConfig::SceneTarget target;

private:
    struct Call {
        common::SceneRequest request;
        Clock::time_point deadline;
        std::shared_ptr<std::promise<common::SceneReply>> promise;
    };
    void run(std::stop_token stop);
    void connect(Clock::time_point deadline);
    void disconnect();
    void push(common::ScenePush value);
    common::SceneReply roundtrip(
        common::SceneRequest request, Clock::time_point deadline);
    std::optional<std::vector<std::byte>> receive(Clock::time_point deadline);
    RealmConfig::SceneClient tls_;
    std::string realm_incarnation_;
    concurrency::BoundedQueue<common::ScenePush>& pushes_;
    concurrency::BoundedQueue<Call> calls_{64};
    mutable std::mutex mutex_;
    std::optional<network::TransportEndpoint> endpoint_;
    std::string incarnation_;
    std::atomic_uint64_t generation_{0};
    std::atomic_size_t in_flight_{0};
    std::shared_ptr<network::client::TlsClientStream> stream_;
    network::LengthFieldCodec codec_{common::scene_envelope_max};
    network::ByteBuffer input_;
    std::uint64_t next_id_{1};
    std::jthread thread_;  // 首次请求才启动；仅编排线程调用 exchange。
};
}  // namespace realm::game::realm::scene_detail
