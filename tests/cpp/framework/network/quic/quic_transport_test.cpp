#include "realmmesh/network/transport/transport_factory.hpp"
#include "realmmesh/observability/logger.hpp"

#include <gtest/gtest.h>
#include <msquic.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

namespace realm::network {
namespace {

class TemporaryLogFile final {
public:
    TemporaryLogFile()
        : path_(
              std::filesystem::temp_directory_path() /
              ("realmmesh-quic-transport-" +
               std::to_string(std::chrono::steady_clock::now()
                                  .time_since_epoch()
                                  .count()) +
               ".jsonl")) {}

    ~TemporaryLogFile() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] std::vector<nlohmann::json> read_log_events(
    const std::filesystem::path& path) {
    std::ifstream input(path);
    std::vector<nlohmann::json> events;
    std::string line;
    while (std::getline(input, line)) {
        events.push_back(nlohmann::json::parse(line));
    }
    return events;
}

[[nodiscard]] const nlohmann::json* find_log_event(
    const std::vector<nlohmann::json>& events, std::string_view event_name) {
    const auto iterator =
        std::ranges::find_if(events, [event_name](const auto& event) {
            return event.at("event_name") == event_name;
        });
    return iterator == events.end() ? nullptr : &*iterator;
}

struct ClientState {
    const QUIC_API_TABLE* api{nullptr};
    HQUIC connection{nullptr};
    HQUIC stream{nullptr};
    std::mutex mutex;
    std::condition_variable ready;
    bool connected{false};
    bool shutdown{false};
    QUIC_STATUS transport_status{QUIC_STATUS_SUCCESS};
    std::vector<std::byte> received;
};

struct ClientSend {
    std::vector<std::byte> bytes;
    QUIC_BUFFER buffer{};
};

QUIC_STATUS QUIC_API
client_stream_callback(HQUIC stream, void* context, QUIC_STREAM_EVENT* event) {
    auto* state = static_cast<ClientState*>(context);
    switch (event->Type) {
    case QUIC_STREAM_EVENT_RECEIVE: {
        std::lock_guard lock(state->mutex);
        for (std::uint32_t index = 0; index < event->RECEIVE.BufferCount;
             ++index) {
            const auto& buffer = event->RECEIVE.Buffers[index];
            const auto* begin =
                reinterpret_cast<const std::byte*>(buffer.Buffer);
            state->received.insert(
                state->received.end(), begin, begin + buffer.Length);
        }
        state->ready.notify_all();
        break;
    }
    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        delete static_cast<ClientSend*>(event->SEND_COMPLETE.ClientContext);
        break;
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        if (!event->SHUTDOWN_COMPLETE.AppCloseInProgress) {
            state->api->StreamClose(stream);
        }
        break;
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API
client_connection_callback(HQUIC, void* context, QUIC_CONNECTION_EVENT* event) {
    auto* state = static_cast<ClientState*>(context);
    switch (event->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED: {
        {
            std::lock_guard lock(state->mutex);
            state->connected = true;
        }
        state->ready.notify_all();
        if (QUIC_FAILED(state->api->StreamOpen(
                state->connection,
                QUIC_STREAM_OPEN_FLAG_NONE,
                client_stream_callback,
                state,
                &state->stream))) {
            return QUIC_STATUS_ABORTED;
        }
        return state->api->StreamStart(
            state->stream, QUIC_STREAM_START_FLAG_IMMEDIATE);
    }
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE: {
        std::lock_guard lock(state->mutex);
        state->shutdown = true;
    }
        state->ready.notify_all();
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT: {
        std::lock_guard lock(state->mutex);
        state->transport_status = event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status;
    }
        state->ready.notify_all();
        break;
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

[[nodiscard]] std::unique_ptr<ClientSend> make_frame(
    std::span<const std::byte> payload) {
    auto frame = std::make_unique<ClientSend>();
    const auto size = static_cast<std::uint32_t>(payload.size());
    frame->bytes = {
        static_cast<std::byte>((size >> 24U) & 0xFFU),
        static_cast<std::byte>((size >> 16U) & 0xFFU),
        static_cast<std::byte>((size >> 8U) & 0xFFU),
        static_cast<std::byte>(size & 0xFFU),
    };
    frame->bytes.insert(frame->bytes.end(), payload.begin(), payload.end());
    frame->buffer = {
        .Length = static_cast<std::uint32_t>(frame->bytes.size()),
        .Buffer = reinterpret_cast<std::uint8_t*>(frame->bytes.data()),
    };
    return frame;
}

TEST(QuicTransportTest, ExchangesAFramedMessageOverOneVerifiedStream) {
    TemporaryLogFile log_file;
    observability::LoggerConfig logger_config;
    logger_config.file_path = log_file.path();
    observability::Logger logger(
        logger_config,
        observability::ServiceIdentity{.service_name = "gateway"});
    const std::vector<TransportConfig> configs{{
        .name = "client_quic",
        .protocol = TransportProtocol::Quic,
        .listen_address = "127.0.0.1",
        .listen_port = 0,
        .tls =
            TransportConfig::TlsServerIdentity{
                .certificate_chain_file = REALMMESH_TEST_TLS_CERTIFICATE,
                .private_key_file = REALMMESH_TEST_TLS_PRIVATE_KEY,
            },
    }};
    auto transports = TransportFactory::create_enabled(configs, &logger);
    ASSERT_EQ(transports.size(), 1U);
    auto& transport = *transports.front();
    EXPECT_EQ(transport.protocol(), TransportProtocol::Quic);
    EXPECT_NE(transport.local_endpoint().port, 0U);

    std::jthread server([&] {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            for (auto& event :
                 transport.poll_once(std::chrono::milliseconds(20))) {
                if (event.kind == TransportEventKind::MessageReceived) {
                    EXPECT_TRUE(
                        transport.send(event.session_id, event.payload));
                    return;
                }
            }
        }
    });

    const QUIC_API_TABLE* api = nullptr;
    ASSERT_FALSE(QUIC_FAILED(MsQuicOpen2(&api)));
    HQUIC registration = nullptr;
    HQUIC configuration = nullptr;
    const QUIC_REGISTRATION_CONFIG registration_config{
        "realmmesh-quic-test",
        QUIC_EXECUTION_PROFILE_LOW_LATENCY,
    };
    ASSERT_FALSE(QUIC_FAILED(
        api->RegistrationOpen(&registration_config, &registration)));
    const std::array<std::uint8_t, 16> alpn_bytes{
        'r',
        'e',
        'a',
        'l',
        'm',
        'm',
        'e',
        's',
        'h',
        '-',
        'e',
        'd',
        'g',
        'e',
        '/',
        '1'};
    const QUIC_BUFFER alpn{
        .Length = static_cast<std::uint32_t>(alpn_bytes.size()),
        .Buffer = const_cast<std::uint8_t*>(alpn_bytes.data()),
    };
    QUIC_SETTINGS settings{};
    settings.HandshakeIdleTimeoutMs = 3000;
    settings.IsSet.HandshakeIdleTimeoutMs = TRUE;
    ASSERT_FALSE(QUIC_FAILED(api->ConfigurationOpen(
        registration,
        &alpn,
        1,
        &settings,
        sizeof(settings),
        nullptr,
        &configuration)));
    QUIC_CREDENTIAL_CONFIG credential{};
    credential.Type = QUIC_CREDENTIAL_TYPE_NONE;
    credential.Flags = static_cast<QUIC_CREDENTIAL_FLAGS>(
        QUIC_CREDENTIAL_FLAG_CLIENT |
        QUIC_CREDENTIAL_FLAG_SET_CA_CERTIFICATE_FILE);
    credential.CaCertificateFile = REALMMESH_TEST_TLS_CERTIFICATE;
    ASSERT_FALSE(QUIC_FAILED(
        api->ConfigurationLoadCredential(configuration, &credential)));

    ClientState client{
        .api = api,
    };
    ASSERT_FALSE(QUIC_FAILED(api->ConnectionOpen(
        registration,
        client_connection_callback,
        &client,
        &client.connection)));
    ASSERT_FALSE(QUIC_FAILED(api->ConnectionStart(
        client.connection,
        configuration,
        QUIC_ADDRESS_FAMILY_UNSPEC,
        "127.0.0.1",
        transport.local_endpoint().port)));

    {
        std::unique_lock lock(client.mutex);
        ASSERT_TRUE(client.ready.wait_for(
            lock,
            std::chrono::seconds(5),
            [&] {
                return client.connected || client.shutdown;
            }))
            << "MsQuic status=" << client.transport_status
            << " server sessions=" << transport.session_count();
        ASSERT_TRUE(client.connected)
            << "MsQuic status=" << client.transport_status;
    }
    const std::array<std::byte, 4> payload{
        std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}};
    auto frame = make_frame(payload);
    ASSERT_FALSE(QUIC_FAILED(api->StreamSend(
        client.stream, &frame->buffer, 1, QUIC_SEND_FLAG_NONE, frame.get())));
    static_cast<void>(frame.release());

    {
        std::unique_lock lock(client.mutex);
        ASSERT_TRUE(client.ready.wait_for(lock, std::chrono::seconds(5), [&] {
            return client.received.size() >= 8U;
        }));
        ASSERT_GE(client.received.size(), 8U);
        EXPECT_TRUE(std::equal(
            payload.begin(), payload.end(), client.received.begin() + 4));
    }

    api->ConnectionShutdown(
        client.connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
    {
        std::unique_lock lock(client.mutex);
        ASSERT_TRUE(client.ready.wait_for(lock, std::chrono::seconds(5), [&] {
            return client.shutdown;
        }));
    }
    server.join();
    const auto close_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (transport.session_count() != 0U &&
           std::chrono::steady_clock::now() < close_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_EQ(transport.session_count(), 0U);
    ASSERT_TRUE(logger.flush(std::chrono::seconds(2)));

    const auto log_events = read_log_events(log_file.path());
    const auto* accepted = find_log_event(log_events, "connection_accepted");
    ASSERT_NE(accepted, nullptr);
    EXPECT_EQ(accepted->at("attributes").at("protocol"), "quic");
    EXPECT_EQ(accepted->at("attributes").at("peer_address"), "127.0.0.1");
    EXPECT_GT(accepted->at("attributes").at("peer_port").get<int>(), 0);

    const auto* handshake =
        find_log_event(log_events, "tls_handshake_completed");
    ASSERT_NE(handshake, nullptr);
    EXPECT_EQ(handshake->at("attributes").at("alpn"), "realmmesh-edge/1");

    const auto* closed = find_log_event(log_events, "connection_closed");
    ASSERT_NE(closed, nullptr);
    EXPECT_EQ(closed->at("attributes").at("reason"), "peer_requested");

    api->ConnectionClose(client.connection);
    api->ConfigurationClose(configuration);
    api->RegistrationClose(registration);
    MsQuicClose(api);
}

}  // namespace
}  // namespace realm::network
