#include "realmmesh/network/quic/quic_transport.hpp"

#include "realmmesh/network/codec/length_field_codec.hpp"
#include "realmmesh/network/core/byte_buffer.hpp"

#include <msquic.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace realm::network {
namespace {

void require_success(QUIC_STATUS status, const char* operation) {
    if (QUIC_FAILED(status)) {
        throw std::runtime_error(
            std::string(operation) + " failed with MsQuic status " +
            std::to_string(status));
    }
}

}  // namespace

class QuicTransport::Impl final {
public:
    explicit Impl(TransportConfig config)
        : config_(std::move(config)),
          event_capacity_(std::max<std::size_t>(config_.max_sessions * 4U, 64U)) {
        require_success(MsQuicOpen2(&api_), "MsQuicOpen2");
        try {
            open_registration();
            open_configuration();
            open_listener();
        } catch (...) {
            close_handles();
            throw;
        }
    }

    ~Impl() { close_handles(); }

    [[nodiscard]] std::string_view name() const noexcept { return config_.name; }
    [[nodiscard]] TransportEndpoint local_endpoint() const {
        return {
            .name = config_.name,
            .protocol = TransportProtocol::Quic,
            .address = config_.listen_address,
            .port = local_port_,
        };
    }
    [[nodiscard]] std::size_t session_count() const noexcept {
        std::lock_guard lock(connections_mutex_);
        return connections_.size();
    }

    [[nodiscard]] std::vector<TransportEvent> poll_once(
        std::chrono::milliseconds timeout) {
        std::unique_lock lock(events_mutex_);
        if (events_.empty() && timeout > std::chrono::milliseconds::zero()) {
            events_ready_.wait_for(lock, timeout, [this] { return !events_.empty(); });
        }
        std::vector<TransportEvent> result;
        result.reserve(events_.size());
        while (!events_.empty()) {
            result.push_back(std::move(events_.front()));
            events_.pop_front();
        }
        return result;
    }

    [[nodiscard]] bool send(
        SessionId session_id,
        std::span<const std::byte> payload) {
        const auto state = find_connection(session_id);
        if (!state) {
            return false;
        }
        auto frame = state->codec.encode(payload);
        if (frame.size() > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }

        auto context = std::make_unique<SendContext>();
        context->state = state.get();
        context->bytes = std::move(frame);
        context->buffer = {
            .Length = static_cast<std::uint32_t>(context->bytes.size()),
            .Buffer = reinterpret_cast<std::uint8_t*>(context->bytes.data()),
        };

        std::lock_guard state_lock(state->mutex);
        if (!state->connected || state->stream == nullptr || state->closing ||
            context->bytes.size() >
                config_.max_pending_output_bytes -
                    std::min(
                        config_.max_pending_output_bytes,
                        state->pending_output_bytes)) {
            return false;
        }
        state->pending_output_bytes += context->bytes.size();
        const auto status = api_->StreamSend(
            state->stream,
            &context->buffer,
            1,
            QUIC_SEND_FLAG_NONE,
            context.get());
        if (QUIC_FAILED(status)) {
            state->pending_output_bytes -= context->bytes.size();
            return false;
        }
        static_cast<void>(context.release());
        return true;
    }

    [[nodiscard]] bool close(SessionId session_id) {
        const auto state = find_connection(session_id);
        if (!state) {
            return false;
        }
        {
            std::lock_guard state_lock(state->mutex);
            if (state->closing) {
                return false;
            }
            state->closing = true;
        }
        api_->ConnectionShutdown(
            state->connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
        return true;
    }

    [[nodiscard]] bool reload_credentials() {
        HQUIC replacement = nullptr;
        try {
            replacement = create_configuration();
        } catch (...) {
            return false;
        }
        HQUIC previous = nullptr;
        {
            std::lock_guard lock(configuration_mutex_);
            previous = std::exchange(configuration_, replacement);
        }
        api_->ConfigurationClose(previous);
        return true;
    }

private:
    struct ConnectionState {
        ConnectionState(
            Impl* owner_value,
            SessionId session_id_value,
            HQUIC connection_value,
            std::size_t max_payload_size)
            : owner(owner_value),
              session_id(session_id_value),
              connection(connection_value),
              codec(max_payload_size) {}

        Impl* owner;
        SessionId session_id;
        HQUIC connection;
        HQUIC stream{nullptr};
        LengthFieldCodec codec;
        ByteBuffer input;
        std::mutex mutex;
        std::size_t pending_output_bytes{0};
        bool connected{false};
        bool closing{false};
    };

    struct SendContext {
        ConnectionState* state{nullptr};
        std::vector<std::byte> bytes;
        QUIC_BUFFER buffer{};
    };

    void open_registration() {
        const QUIC_REGISTRATION_CONFIG registration_config{
            "realmmesh-gateway",
            QUIC_EXECUTION_PROFILE_LOW_LATENCY,
        };
        require_success(
            api_->RegistrationOpen(&registration_config, &registration_),
            "RegistrationOpen");
    }

    [[nodiscard]] HQUIC create_configuration() {
        const auto& identity = config_.tls.value();
        alpn_bytes_.assign(identity.alpn.begin(), identity.alpn.end());
        const QUIC_BUFFER alpn{
            .Length = static_cast<std::uint32_t>(alpn_bytes_.size()),
            .Buffer = alpn_bytes_.data(),
        };

        QUIC_SETTINGS settings{};
        settings.HandshakeIdleTimeoutMs =
            static_cast<std::uint64_t>(config_.handshake_timeout.count());
        settings.IdleTimeoutMs =
            static_cast<std::uint64_t>(config_.idle_timeout.count());
        settings.KeepAliveIntervalMs = 0;
        settings.PeerBidiStreamCount = 1;
        settings.PeerUnidiStreamCount = 0;
        settings.MigrationEnabled = TRUE;
        settings.DatagramReceiveEnabled = FALSE;
        settings.ServerResumptionLevel = QUIC_SERVER_NO_RESUME;
        settings.IsSet.HandshakeIdleTimeoutMs = TRUE;
        settings.IsSet.IdleTimeoutMs = TRUE;
        settings.IsSet.KeepAliveIntervalMs = TRUE;
        settings.IsSet.PeerBidiStreamCount = TRUE;
        settings.IsSet.PeerUnidiStreamCount = TRUE;
        settings.IsSet.MigrationEnabled = TRUE;
        settings.IsSet.DatagramReceiveEnabled = TRUE;
        settings.IsSet.ServerResumptionLevel = TRUE;

        HQUIC configuration = nullptr;
        require_success(
            api_->ConfigurationOpen(
                registration_,
                &alpn,
                1,
                &settings,
                sizeof(settings),
                nullptr,
                &configuration),
            "ConfigurationOpen");

        QUIC_CERTIFICATE_FILE certificate{
            .PrivateKeyFile = identity.private_key_file.c_str(),
            .CertificateFile = identity.certificate_chain_file.c_str(),
        };
        QUIC_CREDENTIAL_CONFIG credential{};
        credential.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
        credential.Flags = QUIC_CREDENTIAL_FLAG_NONE;
        credential.CertificateFile = &certificate;
        const auto credential_status =
            api_->ConfigurationLoadCredential(configuration, &credential);
        if (QUIC_FAILED(credential_status)) {
            api_->ConfigurationClose(configuration);
            require_success(credential_status, "ConfigurationLoadCredential");
        }
        return configuration;
    }

    void open_configuration() {
        configuration_ = create_configuration();
    }

    void open_listener() {
        require_success(
            api_->ListenerOpen(
                registration_, listener_callback, this, &listener_),
            "ListenerOpen");
        QUIC_ADDR address{};
        if (!QuicAddrFromString(
                config_.listen_address.c_str(), config_.listen_port, &address)) {
            throw std::invalid_argument("invalid QUIC listen address");
        }
        const QUIC_BUFFER alpn{
            .Length = static_cast<std::uint32_t>(alpn_bytes_.size()),
            .Buffer = alpn_bytes_.data(),
        };
        require_success(
            api_->ListenerStart(listener_, &alpn, 1, &address),
            "ListenerStart");

        QUIC_ADDR local_address{};
        std::uint32_t local_address_size = sizeof(local_address);
        require_success(
            api_->GetParam(
                listener_,
                QUIC_PARAM_LISTENER_LOCAL_ADDRESS,
                &local_address_size,
                &local_address),
            "GetParam(QUIC_PARAM_LISTENER_LOCAL_ADDRESS)");
        local_port_ = QuicAddrGetPort(&local_address);
    }

    void close_handles() noexcept {
        if (api_ == nullptr) {
            return;
        }
        if (listener_ != nullptr) {
            api_->ListenerClose(listener_);
            listener_ = nullptr;
        }
        if (registration_ != nullptr) {
            api_->RegistrationShutdown(
                registration_, QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT, 0);
        }
        if (configuration_ != nullptr) {
            api_->ConfigurationClose(configuration_);
            configuration_ = nullptr;
        }
        if (registration_ != nullptr) {
            api_->RegistrationClose(registration_);
            registration_ = nullptr;
        }
        connections_.clear();
        MsQuicClose(api_);
        api_ = nullptr;
    }

    [[nodiscard]] std::shared_ptr<ConnectionState> find_connection(
        SessionId session_id) {
        std::lock_guard lock(connections_mutex_);
        const auto iterator = connections_.find(session_id);
        return iterator == connections_.end() ? nullptr : iterator->second;
    }

    [[nodiscard]] bool push_event(TransportEvent event) {
        {
            std::lock_guard lock(events_mutex_);
            if (events_.size() >= event_capacity_) {
                return false;
            }
            events_.push_back(std::move(event));
        }
        events_ready_.notify_one();
        return true;
    }

    void reject_overloaded(ConnectionState* state) {
        {
            std::lock_guard lock(state->mutex);
            if (state->closing) {
                return;
            }
            state->closing = true;
        }
        api_->ConnectionShutdown(
            state->connection, QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT, 1);
    }

    QUIC_STATUS on_new_connection(HQUIC connection) {
        std::shared_ptr<ConnectionState> state;
        {
            std::lock_guard lock(connections_mutex_);
            if (connections_.size() >= config_.max_sessions) {
                return QUIC_STATUS_CONNECTION_REFUSED;
            }
            const SessionId session_id = next_session_id_++;
            state = std::make_shared<ConnectionState>(
                this, session_id, connection, config_.max_payload_size);
            api_->SetCallbackHandler(
                connection,
                reinterpret_cast<void*>(connection_callback),
                state.get());
            connections_.emplace(session_id, state);
        }
        QUIC_STATUS status = QUIC_STATUS_INVALID_STATE;
        {
            std::lock_guard lock(configuration_mutex_);
            status = api_->ConnectionSetConfiguration(
                connection, configuration_);
        }
        if (QUIC_FAILED(status)) {
            std::lock_guard lock(connections_mutex_);
            for (auto iterator = connections_.begin();
                 iterator != connections_.end(); ++iterator) {
                if (iterator->second->connection == connection) {
                    connections_.erase(iterator);
                    break;
                }
            }
        }
        return status;
    }

    QUIC_STATUS on_connection_event(
        ConnectionState* state,
        HQUIC connection,
        QUIC_CONNECTION_EVENT* event) {
        const auto state_guard = find_connection(state->session_id);
        if (!state_guard) {
            return QUIC_STATUS_INVALID_STATE;
        }
        switch (event->Type) {
        case QUIC_CONNECTION_EVENT_CONNECTED: {
            {
                std::lock_guard lock(state->mutex);
                state->connected = true;
            }
            if (!push_event({
                    .kind = TransportEventKind::SessionOpened,
                    .session_id = state->session_id,
                    .payload = {},
                })) {
                reject_overloaded(state);
            }
            break;
        }
        case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
            const bool unidirectional =
                (event->PEER_STREAM_STARTED.Flags &
                 QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL) != 0;
            bool reject = unidirectional;
            {
                std::lock_guard lock(state->mutex);
                reject = reject || state->stream != nullptr;
                if (!reject) {
                    state->stream = event->PEER_STREAM_STARTED.Stream;
                }
            }
            if (reject) {
                reject_overloaded(state);
            } else {
                api_->SetCallbackHandler(
                    event->PEER_STREAM_STARTED.Stream,
                    reinterpret_cast<void*>(stream_callback),
                    state);
            }
            break;
        }
        case QUIC_CONNECTION_EVENT_PEER_ADDRESS_CHANGED:
            if (!push_event({
                    .kind = TransportEventKind::PeerAddressChanged,
                    .session_id = state->session_id,
                    .payload = {},
                })) {
                reject_overloaded(state);
            }
            break;
        case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE: {
            bool was_connected = false;
            {
                std::lock_guard lock(state->mutex);
                was_connected = state->connected;
                state->closing = true;
            }
            if (was_connected) {
                static_cast<void>(push_event({
                    .kind = TransportEventKind::SessionClosed,
                    .session_id = state->session_id,
                    .payload = {},
                }));
            }
            const SessionId session_id = state->session_id;
            api_->ConnectionClose(connection);
            std::lock_guard lock(connections_mutex_);
            connections_.erase(session_id);
            break;
        }
        default:
            break;
        }
        return QUIC_STATUS_SUCCESS;
    }

    QUIC_STATUS on_stream_event(
        ConnectionState* state,
        HQUIC stream,
        QUIC_STREAM_EVENT* event) {
        const auto state_guard = find_connection(state->session_id);
        if (!state_guard) {
            return QUIC_STATUS_INVALID_STATE;
        }
        switch (event->Type) {
        case QUIC_STREAM_EVENT_RECEIVE:
            for (std::uint32_t index = 0;
                 index < event->RECEIVE.BufferCount; ++index) {
                const auto& buffer = event->RECEIVE.Buffers[index];
                state->input.append(std::span<const std::byte>{
                    reinterpret_cast<const std::byte*>(buffer.Buffer),
                    buffer.Length,
                });
            }
            while (true) {
                auto decoded = state->codec.try_decode(state->input);
                if (decoded.status == DecodeStatus::NeedMoreData) {
                    break;
                }
                if (decoded.status == DecodeStatus::FrameTooLarge) {
                    reject_overloaded(state);
                    break;
                }
                if (!push_event({
                        .kind = TransportEventKind::MessageReceived,
                        .session_id = state->session_id,
                        .payload = std::move(decoded.payload),
                    })) {
                    reject_overloaded(state);
                    break;
                }
            }
            break;
        case QUIC_STREAM_EVENT_SEND_COMPLETE: {
            std::unique_ptr<SendContext> context(
                static_cast<SendContext*>(event->SEND_COMPLETE.ClientContext));
            if (context) {
                std::lock_guard lock(state->mutex);
                state->pending_output_bytes -= std::min(
                    state->pending_output_bytes, context->bytes.size());
            }
            break;
        }
        case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
        case QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED:
            reject_overloaded(state);
            break;
        case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
            {
                std::lock_guard lock(state->mutex);
                if (state->stream == stream) {
                    state->stream = nullptr;
                }
            }
            if (!event->SHUTDOWN_COMPLETE.AppCloseInProgress) {
                api_->StreamClose(stream);
            }
            break;
        default:
            break;
        }
        return QUIC_STATUS_SUCCESS;
    }

    static QUIC_STATUS QUIC_API listener_callback(
        HQUIC,
        void* context,
        QUIC_LISTENER_EVENT* event) {
        auto* self = static_cast<Impl*>(context);
        if (event->Type != QUIC_LISTENER_EVENT_NEW_CONNECTION) {
            return QUIC_STATUS_NOT_SUPPORTED;
        }
        return self->on_new_connection(event->NEW_CONNECTION.Connection);
    }

    static QUIC_STATUS QUIC_API connection_callback(
        HQUIC connection,
        void* context,
        QUIC_CONNECTION_EVENT* event) {
        auto* state = static_cast<ConnectionState*>(context);
        return state->owner->on_connection_event(state, connection, event);
    }

    static QUIC_STATUS QUIC_API stream_callback(
        HQUIC stream,
        void* context,
        QUIC_STREAM_EVENT* event) {
        auto* state = static_cast<ConnectionState*>(context);
        return state->owner->on_stream_event(state, stream, event);
    }

    TransportConfig config_;
    const QUIC_API_TABLE* api_{nullptr};
    HQUIC registration_{nullptr};
    HQUIC configuration_{nullptr};
    std::mutex configuration_mutex_;
    HQUIC listener_{nullptr};
    std::vector<std::uint8_t> alpn_bytes_;
    std::uint16_t local_port_{0};
    std::atomic<SessionId> next_session_id_{1};

    mutable std::mutex connections_mutex_;
    std::unordered_map<SessionId, std::shared_ptr<ConnectionState>> connections_;

    const std::size_t event_capacity_;
    std::mutex events_mutex_;
    std::condition_variable events_ready_;
    std::deque<TransportEvent> events_;
};

QuicTransport::QuicTransport(TransportConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}
QuicTransport::~QuicTransport() = default;
std::string_view QuicTransport::name() const noexcept { return impl_->name(); }
TransportProtocol QuicTransport::protocol() const noexcept {
    return TransportProtocol::Quic;
}
TransportEndpoint QuicTransport::local_endpoint() const {
    return impl_->local_endpoint();
}
std::size_t QuicTransport::session_count() const noexcept {
    return impl_->session_count();
}
std::vector<TransportEvent> QuicTransport::poll_once(
    std::chrono::milliseconds timeout) {
    return impl_->poll_once(timeout);
}
bool QuicTransport::send(
    SessionId session_id,
    std::span<const std::byte> payload) {
    return impl_->send(session_id, payload);
}
bool QuicTransport::close(SessionId session_id) {
    return impl_->close(session_id);
}
bool QuicTransport::reload_credentials() {
    return impl_->reload_credentials();
}

}  // namespace realm::network
