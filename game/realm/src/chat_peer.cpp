#include "chat_peer.hpp"
#include <array>
#include <stdexcept>
#include "realmmesh/cluster/etcd_service_registry.hpp"
namespace realm::game::realm::chat_detail {
namespace wire = ::realmmesh::protocol::chat::v1;
namespace {
bool same_link(const wire::Link& a, const wire::Link& b) {
    return a.SerializeAsString() == b.SerializeAsString();
}
}  // namespace
Peer::Peer(
    RealmConfig::ChatClient config,
    RealmConfig::ChatTarget destination,
    std::string incarnation)
    : target(std::move(destination)),
      config_(std::move(config)),
      incarnation_(std::move(incarnation)) {}
Peer::~Peer() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}
std::optional<std::future<Completion>> Peer::submit(
    common::ChatInternalRequest request, Clock::time_point deadline) {
    if (outstanding_ >= common::chat_pending_requests_max) return {};
    Call call{std::move(request), deadline, revision(), {}};
    auto future = call.promise.get_future();
    if (!calls_.try_push(std::move(call))) return {};
    ++outstanding_;
    if (!thread_.joinable())
        thread_ = std::jthread([this](std::stop_token stop) {
            run(stop);
        });
    return std::move(future);
}
std::vector<common::ChatInternalPush> Peer::drain(std::size_t max) {
    std::vector<common::ChatInternalPush> result;
    const std::scoped_lock lock(mutex_);
    while (!pushes_.empty() && result.size() < max) {
        push_bytes_ -= pushes_.front().ByteSizeLong();
        result.push_back(std::move(pushes_.front()));
        pushes_.pop_front();
    }
    return result;
}
void Peer::disconnect() {
    if (stream_) stream_->shutdown();
    stream_.reset();
    input_.clear();
    link_.Clear();
    ++revision_;
    const std::scoped_lock lock(mutex_);
    pushes_.clear();
    push_bytes_ = 0;
}
void Peer::push(common::ChatInternalPush value) {
    if (value.link().SerializeAsString() != link_.SerializeAsString())
        throw std::runtime_error("stale Chat link");
    const auto bytes = value.ByteSizeLong();
    const std::scoped_lock lock(mutex_);
    if (!common::chat_budget_allows(
            {pushes_.size(), push_bytes_},
            {1, bytes},
            {common::chat_pending_pushes_max,
             common::chat_pending_push_bytes_max}))
        throw std::runtime_error("Chat push capacity");
    push_bytes_ += bytes;
    pushes_.push_back(std::move(value));
}
std::optional<std::vector<std::byte>> Peer::receive(
    Clock::time_point deadline) {
    while (stream_ && Clock::now() < deadline) {
        auto frame = codec_.try_decode(input_);
        if (frame.status == network::DecodeStatus::FrameReady)
            return std::move(frame.payload);
        if (frame.status == network::DecodeStatus::FrameTooLarge)
            throw std::runtime_error("Chat frame size");
        std::array<std::byte, 8192> buffer{};
        const auto n = stream_->read_some(buffer, deadline);
        if (!n) return {};
        if (*n == 0) throw std::runtime_error("Chat closed");
        input_.append(std::span<const std::byte>(buffer.data(), *n));
    }
    return {};
}
Completion Peer::exchange(
    common::ChatInternalRequest request, Clock::time_point deadline) {
    if (!stream_ || Clock::now() >= deadline ||
        next_id_ >= common::chat_counter_max)
        return {};
    *request.mutable_link() = link_;
    const auto id = next_id_++;
    auto bytes = common::encode(request, id);
    Completion completion{.dispatched = true};  // 部分 write 也可能已到达对端。
    try {
        const auto frame = codec_.encode(bytes);
        if (!stream_->write_all(frame, deadline)) return completion;
        bool retried = request.operation() == wire::OPERATION_HELLO;
        const auto retry_at = Clock::now() + (deadline - Clock::now()) / 2;
        while (Clock::now() < deadline) {
            const auto payload = receive(retried ? deadline : retry_at);
            if (!payload) {
                if (retried || Clock::now() >= deadline) break;
                // 同一总截止内，仅用原号/原参数再发一次。JOIN 保留原 W，SEND
                // 不重广播。
                retried = true;
                if (!stream_->write_all(frame, deadline)) break;
                continue;
            }
            if (auto value = common::decode_chat_internal_push(*payload)) {
                push(std::move(*value));
                continue;
            }
            auto reply = common::decode_chat_internal_reply(*payload);
            if (reply &&
                common::chat_internal_request_id(*payload).value_or(id) < id &&
                same_link(reply->link(), link_))
                continue;
            if (!reply || common::chat_internal_request_id(*payload) != id ||
                reply->operation() != request.operation())
                break;
            const auto& l = reply->link();
            if (l.realm_id() != 1 ||
                l.realm_instance_id() != config_.realm_instance_id ||
                l.realm_incarnation() != incarnation_ ||
                l.chat_instance_id() != target.instance_id)
                break;
            if (request.operation() != wire::OPERATION_HELLO &&
                l.SerializeAsString() != link_.SerializeAsString())
                break;
            if (reply->status() == wire::STATUS_OK &&
                request.has_membership() &&
                reply->membership().SerializeAsString() !=
                    request.membership().SerializeAsString())
                break;
            completion.reply = std::move(reply);
            return completion;
        }
    } catch (...) {
    }
    return completion;
}
void Peer::connect(Clock::time_point deadline) {
    if (stream_) return;
    if (!config_.enabled || config_.ca_file.empty() ||
        config_.certificate_file.empty() || config_.private_key_file.empty())
        throw std::runtime_error("Chat authentication not configured");
    network::client::TlsClientOptions options;
    options.alpn = common::chat_internal_alpn;
    options.ca_file = config_.ca_file;
    options.certificate_chain_file = config_.certificate_file;
    options.private_key_file = config_.private_key_file;
    options.server_identity = target.server_identity;
    auto host = target.host;
    auto port = target.port;
    if (config_.discovery_enabled) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - Clock::now());
        if (remaining.count() <= 0)
            throw std::runtime_error("Chat discovery deadline");
        cluster::EtcdRegistryOptions registry_options;
        registry_options.endpoint = config_.discovery_endpoint;
        registry_options.key_prefix = config_.discovery_prefix;
        registry_options.background_maintenance = false;
        registry_options.request_timeout =
            std::min(remaining, std::chrono::milliseconds(500));
        cluster::EtcdServiceRegistry registry(registry_options);
        bool found = false;
        for (const auto& instance : registry.discover("chat")) {
            if (instance.instance_id != target.instance_id) continue;
            for (const auto& endpoint : instance.endpoints)
                if (endpoint.protocol == network::TransportProtocol::TlsTcp) {
                    host = endpoint.address;
                    port = endpoint.port;
                    found = true;
                    break;
                }
        }
        if (!found)
            throw std::runtime_error("configured Chat instance not discovered");
    }
    const auto dial =
        network::client::TlsClientStream::dial(host, port, options, deadline);
    if (!dial.ok() ||
        dial.stream->negotiated_alpn() != common::chat_internal_alpn)
        throw std::runtime_error("Chat authentication unavailable");
    stream_ = dial.stream;
    link_.set_realm_id(1);
    link_.set_realm_instance_id(config_.realm_instance_id);
    link_.set_realm_incarnation(incarnation_);
    link_.set_chat_instance_id(target.instance_id);
    common::ChatInternalRequest hello;
    hello.set_operation(wire::OPERATION_HELLO);
    const auto completed = exchange(hello, deadline);
    if (!completed.reply || completed.reply->status() != wire::STATUS_OK)
        throw std::runtime_error("Chat HELLO failed");
    link_ = completed.reply->link();
}
void Peer::run(std::stop_token stop) {
    while (!stop.stop_requested()) {
        if (reset_.exchange(false)) disconnect();
        if (auto call = calls_.try_pop()) {
            Completion result;
            try {
                if (call->revision == revision() &&
                    Clock::now() < call->deadline) {
                    // 只有 JOIN 可以新建链路；旧 SEND/LEAVE
                    // 永远不能在新链路复活。
                    if (!stream_ &&
                        call->request.operation() == wire::OPERATION_JOIN)
                        connect(call->deadline);
                    result = exchange(std::move(call->request), call->deadline);
                    if (!result.reply) disconnect();
                }
            } catch (...) {
                disconnect();
            }
            call->promise.set_value(std::move(result));
        } else if (stream_) {
            try {
                if (auto payload =
                        receive(Clock::now() + std::chrono::milliseconds(5))) {
                    if (auto value =
                            common::decode_chat_internal_push(*payload))
                        push(std::move(*value));
                    else if (const auto reply =
                                 common::decode_chat_internal_reply(*payload);
                             !reply || !same_link(reply->link(), link_) ||
                             common::chat_internal_request_id(*payload)
                                     .value_or(next_id_) >= next_id_)
                        disconnect();
                }
            } catch (...) {
                disconnect();
            }
        } else
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    disconnect();
    while (auto call = calls_.try_pop()) call->promise.set_value({});
}
}  // namespace realm::game::realm::chat_detail
