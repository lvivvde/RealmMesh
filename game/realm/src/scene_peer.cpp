#include "scene_peer.hpp"
#include <array>
#include "realmmesh/game/common/edge_protocol.hpp"

namespace realm::game::realm::scene_detail {
namespace internal = ::realmmesh::protocol::scene_internal::v1;
Peer::Peer(
    RealmConfig::SceneClient tls,
    RealmConfig::SceneTarget destination,
    std::string realm_incarnation,
    concurrency::BoundedQueue<common::ScenePush>& pushes)
    : target(std::move(destination)),
      tls_(std::move(tls)),
      realm_incarnation_(std::move(realm_incarnation)),
      pushes_(pushes) {}
Peer::~Peer() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}
std::string Peer::incarnation() const {
    const std::scoped_lock lock(mutex_);
    return incarnation_;
}
void Peer::endpoint(std::optional<network::TransportEndpoint> value) {
    const std::scoped_lock lock(mutex_);
    endpoint_ = std::move(value);
}
std::future<common::SceneReply> Peer::exchange(
    common::SceneRequest request, Clock::time_point deadline) {
    auto promise = std::make_shared<std::promise<common::SceneReply>>();
    auto result = promise->get_future();
    if (in_flight_.fetch_add(1) >= 64) {
        --in_flight_;
        promise->set_exception(
            std::make_exception_ptr(Failure{common::edge_error_throttled}));
    } else if (!calls_.try_push({std::move(request), deadline, promise})) {
        --in_flight_;
        promise->set_exception(
            std::make_exception_ptr(Failure{common::edge_error_throttled}));
    } else if (!thread_.joinable()) {
        thread_ = std::jthread([this](std::stop_token stop) {
            run(stop);
        });
    }
    return result;
}
void Peer::disconnect() {
    if (stream_) stream_->shutdown();
    stream_.reset();
    input_.clear();
    {
        const std::scoped_lock lock(mutex_);
        incarnation_.clear();
    }
    ++generation_;
}
void Peer::push(common::ScenePush value) {
    if (value.realm_incarnation() != realm_incarnation_ ||
        value.scene_instance_id() != target.instance_id ||
        value.scene_incarnation() != incarnation())
        return;
    if (!pushes_.try_push(std::move(value))) disconnect();
}
std::optional<std::vector<std::byte>> Peer::receive(
    Clock::time_point deadline) {
    while (stream_) {
        auto decoded = codec_.try_decode(input_);
        if (decoded.status == network::DecodeStatus::FrameReady)
            return std::move(decoded.payload);
        if (decoded.status == network::DecodeStatus::FrameTooLarge) {
            disconnect();
            return {};
        }
        std::array<std::byte, 8192> bytes{};
        const auto count = stream_->read_some(bytes, deadline);
        if (!count) return {};
        if (*count == 0) {
            disconnect();
            return {};
        }
        input_.append(std::span<const std::byte>(bytes.data(), *count));
    }
    return {};
}
common::SceneReply Peer::roundtrip(
    common::SceneRequest request, Clock::time_point deadline) {
    if (!stream_ || Clock::now() >= deadline)
        throw Failure{common::scene_error_unavailable};
    const auto id = next_id_++;
    if (!stream_->write_all(
            codec_.encode(common::encode(request, id)), deadline)) {
        disconnect();
        throw Failure{common::scene_error_unavailable};
    }
    while (Clock::now() < deadline) {
        const auto bytes = receive(deadline);
        if (!bytes) break;
        if (auto event = common::decode_scene_push(*bytes)) {
            push(std::move(*event));
            continue;
        }
        const auto reply = common::decode_scene_reply(*bytes);
        if (!reply || common::scene_internal_request_id(*bytes) != id ||
            reply->operation() != request.operation() ||
            reply->realm_incarnation() != realm_incarnation_ ||
            reply->scene_instance_id() != target.instance_id ||
            (request.scene_incarnation() != "discover" &&
             reply->scene_incarnation() != request.scene_incarnation()))
            break;
        if (reply->status() != internal::STATUS_OK) {
            if (reply->status() == internal::STATUS_BUSY)
                throw Failure{common::edge_error_throttled};
            if (reply->status() == internal::STATUS_INVALID)
                throw Failure{common::scene_error_invalid_target};
            throw Failure{common::scene_error_stale_binding};
        }
        if (request.has_route() && request.route().SerializeAsString() !=
                                       reply->route().SerializeAsString())
            break;
        if (reply->scene_seq() != request.scene_seq()) break;
        return *reply;
    }
    disconnect();
    throw Failure{common::scene_error_unavailable};
}
void Peer::connect(Clock::time_point deadline) {
    if (stream_) return;
    if (!tls_.enabled) throw Failure{common::scene_error_unavailable};
    auto host = target.host;
    auto port = target.port;
    {
        const std::scoped_lock lock(mutex_);
        if (endpoint_) {
            host = endpoint_->address;
            port = endpoint_->port;
        }
    }
    network::client::TlsClientOptions options;
    options.alpn = common::scene_internal_alpn;
    options.ca_file = tls_.ca_file;
    options.certificate_chain_file = tls_.certificate_file;
    options.private_key_file = tls_.private_key_file;
    options.server_identity = target.server_identity;
    const auto dial =
        network::client::TlsClientStream::dial(host, port, options, deadline);
    if (!dial.ok() ||
        dial.stream->negotiated_alpn() != common::scene_internal_alpn)
        throw Failure{common::scene_error_unavailable};
    stream_ = dial.stream;
    common::SceneRequest probe;
    probe.set_operation(internal::OPERATION_SNAPSHOT);
    probe.set_realm_incarnation(realm_incarnation_);
    probe.set_scene_instance_id(target.instance_id);
    probe.set_scene_incarnation("discover");
    probe.set_shard_id(target.shard_id);
    try {
        const auto reply = roundtrip(std::move(probe), deadline);
        const std::scoped_lock lock(mutex_);
        incarnation_ = reply.scene_incarnation();
    } catch (...) {
        disconnect();
        throw;
    }
}
void Peer::run(std::stop_token stop) {
    while (!stop.stop_requested()) {
        if (auto call = calls_.try_pop()) {
            try {
                if (Clock::now() >= call->deadline)
                    throw Failure{common::scene_error_unavailable};
                connect(call->deadline);
                if (call->request.scene_incarnation().empty())
                    call->request.set_scene_incarnation(incarnation());
                if (call->request.scene_incarnation() != incarnation())
                    throw Failure{common::scene_error_stale_binding};
                call->promise->set_value(
                    roundtrip(std::move(call->request), call->deadline));
            } catch (const Failure&) {
                call->promise->set_exception(std::current_exception());
            } catch (...) {
                disconnect();
                call->promise->set_exception(
                    std::make_exception_ptr(
                        Failure{common::scene_error_unavailable}));
            }
            --in_flight_;
        } else if (stream_) {
            try {
                if (const auto bytes =
                        receive(Clock::now() + std::chrono::milliseconds(5))) {
                    if (auto event = common::decode_scene_push(*bytes))
                        push(std::move(*event));
                    else
                        disconnect();
                }
            } catch (...) {
                disconnect();
            }
        } else
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    disconnect();
    while (auto call = calls_.try_pop())
        call->promise->set_exception(
            std::make_exception_ptr(Failure{common::scene_error_unavailable}));
}
}  // namespace realm::game::realm::scene_detail
