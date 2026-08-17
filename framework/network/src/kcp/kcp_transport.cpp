#include "realmmesh/network/kcp/kcp_transport.hpp"

#include "realmmesh/network/reactor/epoll_event_loop.hpp"

extern "C" {
#include <ikcp.h>
}

#include <arpa/inet.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <system_error>
#include <unordered_map>
#include <unistd.h>
#include <utility>

namespace realm::network {
namespace {

constexpr std::size_t kcp_header_size = 24;
constexpr std::size_t aead_tag_size = 16;
constexpr std::array<std::byte, 4> secure_magic{
    std::byte{'R'}, std::byte{'M'}, std::byte{'K'}, std::byte{'1'}};
constexpr std::byte client_hello_kind{1};
constexpr std::byte client_data_kind{2};
constexpr std::byte server_data_kind{3};
constexpr std::size_t data_header_size = 4 + 1 + 8 + 8;
constexpr std::size_t hello_header_size = 4 + 1 + 8 + kcp_access_ticket_size;

[[noreturn]] void throw_socket_error(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

[[nodiscard]] IUINT32 current_time_ms() noexcept {
    const auto value = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return static_cast<IUINT32>(value);
}

void write_u64(std::span<std::byte, 8> output, std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < output.size(); ++index) {
        const auto shift = static_cast<unsigned>((output.size() - index - 1) * 8);
        output[index] = static_cast<std::byte>((value >> shift) & 0xffU);
    }
}

std::uint64_t read_u64(const std::byte* input) noexcept {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value = (value << 8U) |
                static_cast<std::uint64_t>(std::to_integer<unsigned>(input[index]));
    }
    return value;
}

KcpSecurityKey direction_key(
    const KcpSecurityKey& secret,
    std::string_view context) {
    const KcpSecurityNonce empty{};
    return derive_kcp_session_key(
        secret, empty, empty, std::as_bytes(std::span(context)));
}

KcpSecurityNonce direction_nonce(std::byte direction) {
    KcpSecurityNonce nonce{};
    nonce[0] = direction;
    return nonce;
}

bool has_magic(std::span<const std::byte> packet) {
    return packet.size() >= secure_magic.size() &&
           std::ranges::equal(packet.first(secure_magic.size()), secure_magic);
}

}  // namespace

class KcpTransport::Implementation final {
public:
    struct Session final {
        Session(
            Implementation* owner_value,
            SessionId id_value,
            const sockaddr_in& peer_value,
            const KcpSecurityKey& secret)
            : owner(owner_value),
              id(id_value),
              peer(peer_value),
              receive_cipher(
                  direction_key(secret, "client-to-server-v1"),
                  direction_nonce(std::byte{1})),
              send_cipher(
                  direction_key(secret, "server-to-client-v1"),
                  direction_nonce(std::byte{2})),
              last_activity(std::chrono::steady_clock::now()) {}

        Implementation* owner;
        SessionId id;
        sockaddr_in peer{};
        ikcpcb* control{nullptr};
        KcpSessionCipher receive_cipher;
        KcpSessionCipher send_cipher;
        KcpReplayWindow receive_window;
        std::uint64_t next_send_sequence{0};
        std::chrono::steady_clock::time_point last_activity;

        ~Session() {
            if (control != nullptr) {
                ikcp_release(control);
            }
        }
    };

    explicit Implementation(TransportConfig config) : config_(std::move(config)) {
        if (!config_.kcp_ticket_key.has_value()) {
            throw std::invalid_argument("KCP requires a ticket encryption key");
        }
        if (std::ranges::all_of(
                *config_.kcp_ticket_key,
                [](std::byte value) { return value == std::byte{0}; })) {
            throw std::invalid_argument("KCP ticket encryption key cannot be all zero");
        }
        if (config_.idle_timeout <= std::chrono::milliseconds::zero()) {
            throw std::invalid_argument("KCP idle timeout must be positive");
        }
        ticket_codec_ = std::make_unique<KcpTicketCodec>(*config_.kcp_ticket_key);
        descriptor_ = ::socket(
            AF_INET,
            SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
            0);
        if (descriptor_ < 0) {
            throw_socket_error("socket(kcp)");
        }

        try {
            const int reuse_address = 1;
            if (::setsockopt(
                    descriptor_, SOL_SOCKET, SO_REUSEADDR,
                    &reuse_address, sizeof(reuse_address)) < 0) {
                throw_socket_error("setsockopt(SO_REUSEADDR)");
            }
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(config_.listen_port);
            if (::inet_pton(
                    AF_INET, config_.listen_address.c_str(),
                    &address.sin_addr) != 1) {
                throw std::invalid_argument("invalid IPv4 KCP listen address");
            }
            if (::bind(
                    descriptor_, reinterpret_cast<const sockaddr*>(&address),
                    sizeof(address)) < 0) {
                throw_socket_error("bind(kcp)");
            }
            socklen_t size = sizeof(address);
            if (::getsockname(
                    descriptor_, reinterpret_cast<sockaddr*>(&address), &size) < 0) {
                throw_socket_error("getsockname(kcp)");
            }
            local_port_ = ntohs(address.sin_port);
            event_loop_.add(descriptor_, EventInterest::Read);
        } catch (...) {
            close_socket();
            throw;
        }
    }

    ~Implementation() {
        sessions_.clear();
        close_socket();
    }

    [[nodiscard]] std::vector<TransportEvent> poll_once(
        std::chrono::milliseconds timeout) {
        std::vector<TransportEvent> events;
        const auto wait_time = std::min(timeout, std::chrono::milliseconds(10));
        const auto ready = event_loop_.wait(wait_time);
        if (!ready.empty() && ready.front().readable) {
            receive_datagrams(events);
        }

        const IUINT32 now = current_time_ms();
        const auto steady_now = std::chrono::steady_clock::now();
        std::vector<SessionId> expired;
        for (auto& [id, session] : sessions_) {
            if (steady_now - session->last_activity >= config_.idle_timeout) {
                expired.push_back(id);
                continue;
            }
            ikcp_update(session->control, now);
            drain_messages(*session, events);
        }
        for (const SessionId id : expired) {
            static_cast<void>(close(id));
            events.push_back({
                .kind = TransportEventKind::SessionClosed,
                .session_id = id,
                .payload = {},
            });
        }
        const auto system_now = std::chrono::system_clock::now();
        std::erase_if(used_tokens_, [system_now](const auto& entry) {
            return entry.second < system_now;
        });
        return events;
    }

    [[nodiscard]] bool send(
        SessionId session_id,
        std::span<const std::byte> payload) {
        const auto iterator = sessions_.find(session_id);
        if (iterator == sessions_.end() ||
            payload.size() > config_.max_payload_size ||
            payload.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            return false;
        }
        auto* control = iterator->second->control;
        if (ikcp_send(
                control,
                reinterpret_cast<const char*>(payload.data()),
                static_cast<int>(payload.size())) < 0) {
            return false;
        }
        ikcp_update(control, current_time_ms());
        ikcp_flush(control);
        return true;
    }

    [[nodiscard]] bool close(SessionId session_id) {
        const auto iterator = sessions_.find(session_id);
        if (iterator == sessions_.end()) {
            return false;
        }
        sessions_.erase(iterator);
        return true;
    }

    [[nodiscard]] TransportEndpoint endpoint() const {
        return {
            .name = config_.name,
            .protocol = TransportProtocol::Kcp,
            .address = config_.listen_address,
            .port = local_port_,
        };
    }

    [[nodiscard]] const TransportConfig& config() const noexcept { return config_; }
    [[nodiscard]] std::size_t session_count() const noexcept { return sessions_.size(); }

private:
    static int output(
        const char* data,
        int size,
        ikcpcb*,
        void* user) {
        auto& session = *static_cast<Session*>(user);
        try {
            if (session.next_send_sequence ==
                std::numeric_limits<std::uint64_t>::max()) {
                return -1;
            }
            std::vector<std::byte> packet(data_header_size);
            std::ranges::copy(secure_magic, packet.begin());
            packet[4] = server_data_kind;
            write_u64(std::span<std::byte, 8>(packet.data() + 5, 8), session.id);
            write_u64(
                std::span<std::byte, 8>(packet.data() + 13, 8),
                session.next_send_sequence);
            const auto plaintext = std::as_bytes(
                std::span(data, static_cast<std::size_t>(size)));
            auto encrypted = session.send_cipher.encrypt(
                session.next_send_sequence++, plaintext, packet);
            packet.insert(packet.end(), encrypted.begin(), encrypted.end());
            const auto sent = ::sendto(
                session.owner->descriptor_, packet.data(), packet.size(), MSG_NOSIGNAL,
                reinterpret_cast<const sockaddr*>(&session.peer), sizeof(session.peer));
            return sent >= 0 && static_cast<std::size_t>(sent) == packet.size() ? 0 : -1;
        } catch (...) {
            return -1;
        }
    }

    Session* create_session(
        const sockaddr_in& peer,
        IUINT32 conversation,
        const KcpTicketClaims& claims) {
        if (sessions_.size() >= config_.max_sessions) {
            return nullptr;
        }
        const SessionId id = claims.token_id;
        auto session = std::make_unique<Session>(
            this, id, peer, claims.session_secret);
        session->control = ikcp_create(conversation, session.get());
        if (session->control == nullptr) {
            throw std::runtime_error("failed to create KCP control block");
        }
        ikcp_setoutput(session->control, &Implementation::output);
        static_cast<void>(ikcp_nodelay(session->control, 1, 10, 2, 1));
        static_cast<void>(ikcp_wndsize(session->control, 128, 128));

        Session* result = session.get();
        sessions_.emplace(id, std::move(session));
        return result;
    }

    void receive_datagrams(std::vector<TransportEvent>& events) {
        std::byte buffer[65536];
        while (true) {
            sockaddr_in peer{};
            socklen_t peer_size = sizeof(peer);
            const auto received = ::recvfrom(
                descriptor_, buffer, sizeof(buffer), 0,
                reinterpret_cast<sockaddr*>(&peer), &peer_size);
            if (received < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return;
                }
                throw_socket_error("recvfrom(kcp)");
            }
            const auto packet = std::span<const std::byte>(
                buffer, static_cast<std::size_t>(received));
            if (!has_magic(packet) || packet.size() < 5) {
                continue;
            }
            if (packet[4] == client_hello_kind) {
                receive_client_hello(peer, packet, events);
            } else if (packet[4] == client_data_kind) {
                receive_client_data(peer, packet, events);
            }
        }
    }

    void receive_client_hello(
        const sockaddr_in& peer,
        std::span<const std::byte> packet,
        std::vector<TransportEvent>& events) {
        if (packet.size() < hello_header_size + aead_tag_size) {
            return;
        }
        const std::uint64_t sequence = read_u64(packet.data() + 5);
        const auto ticket = packet.subspan(13, kcp_access_ticket_size);
        const auto claims = ticket_codec_->validate(
            ticket, std::chrono::system_clock::now());
        if (!claims.has_value()) {
            return;
        }

        Session* session = nullptr;
        auto iterator = sessions_.find(claims->token_id);
        bool created = false;
        std::optional<std::vector<std::byte>> plaintext;
        if (iterator != sessions_.end()) {
            session = iterator->second.get();
            plaintext = session->receive_cipher.decrypt(
                sequence,
                packet.subspan(hello_header_size),
                packet.first(hello_header_size));
            if (!plaintext.has_value() ||
                !session->receive_window.accept(sequence)) {
                return;
            }
        } else {
            if (used_tokens_.contains(claims->token_id)) {
                return;
            }
            KcpSessionCipher cipher(
                direction_key(claims->session_secret, "client-to-server-v1"),
                direction_nonce(std::byte{1}));
            plaintext = cipher.decrypt(
                sequence,
                packet.subspan(hello_header_size),
                packet.first(hello_header_size));
            if (!plaintext.has_value() || plaintext->size() < kcp_header_size) {
                return;
            }
            const IUINT32 conversation = ikcp_getconv(
                reinterpret_cast<const char*>(plaintext->data()));
            session = create_session(peer, conversation, *claims);
            if (session == nullptr || !session->receive_window.accept(sequence)) {
                return;
            }
            used_tokens_[claims->token_id] = claims->expires_at;
            created = true;
        }

        if (!accept_authenticated_packet(*session, peer, *plaintext)) {
            if (created) {
                static_cast<void>(close(session->id));
            }
            return;
        }
        if (created) {
            events.push_back({
                .kind = TransportEventKind::SessionOpened,
                .session_id = session->id,
                .payload = {},
            });
        }
        drain_messages(*session, events);
    }

    void receive_client_data(
        const sockaddr_in& peer,
        std::span<const std::byte> packet,
        std::vector<TransportEvent>& events) {
        if (packet.size() < data_header_size + aead_tag_size) {
            return;
        }
        const SessionId session_id = read_u64(packet.data() + 5);
        const std::uint64_t sequence = read_u64(packet.data() + 13);
        const auto iterator = sessions_.find(session_id);
        if (iterator == sessions_.end()) {
            return;
        }
        auto& session = *iterator->second;
        auto plaintext = session.receive_cipher.decrypt(
            sequence,
            packet.subspan(data_header_size),
            packet.first(data_header_size));
        if (!plaintext.has_value() || !session.receive_window.accept(sequence)) {
            return;
        }
        if (accept_authenticated_packet(session, peer, *plaintext)) {
            drain_messages(session, events);
        }
    }

    [[nodiscard]] bool accept_authenticated_packet(
        Session& session,
        const sockaddr_in& peer,
        std::span<const std::byte> plaintext) {
        if (plaintext.size() < kcp_header_size ||
            ikcp_input(
                session.control,
                reinterpret_cast<const char*>(plaintext.data()),
                static_cast<long>(plaintext.size())) < 0) {
            return false;
        }
        session.peer = peer;
        session.last_activity = std::chrono::steady_clock::now();
        return true;
    }

    void drain_messages(Session& session, std::vector<TransportEvent>& events) {
        while (true) {
            const int size = ikcp_peeksize(session.control);
            if (size < 0) {
                return;
            }
            std::vector<std::byte> payload(static_cast<std::size_t>(size));
            const int received = ikcp_recv(
                session.control,
                reinterpret_cast<char*>(payload.data()),
                size);
            if (received < 0) {
                return;
            }
            if (payload.size() <= config_.max_payload_size) {
                events.push_back({
                    .kind = TransportEventKind::MessageReceived,
                    .session_id = session.id,
                    .payload = std::move(payload),
                });
            }
        }
    }

    void close_socket() noexcept {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
            descriptor_ = -1;
        }
        local_port_ = 0;
    }

    TransportConfig config_;
    std::unique_ptr<KcpTicketCodec> ticket_codec_;
    int descriptor_{-1};
    std::uint16_t local_port_{0};
    EpollEventLoop event_loop_;
    std::unordered_map<SessionId, std::unique_ptr<Session>> sessions_;
    std::unordered_map<SessionId, std::chrono::system_clock::time_point> used_tokens_;
};

KcpTransport::KcpTransport(TransportConfig config)
    : implementation_(std::make_unique<Implementation>(std::move(config))) {}

KcpTransport::~KcpTransport() = default;

std::string_view KcpTransport::name() const noexcept {
    return implementation_->config().name;
}

TransportProtocol KcpTransport::protocol() const noexcept {
    return TransportProtocol::Kcp;
}

TransportEndpoint KcpTransport::local_endpoint() const {
    return implementation_->endpoint();
}

std::size_t KcpTransport::session_count() const noexcept {
    return implementation_->session_count();
}

std::vector<TransportEvent> KcpTransport::poll_once(
    std::chrono::milliseconds timeout) {
    return implementation_->poll_once(timeout);
}

bool KcpTransport::send(
    SessionId session_id,
    std::span<const std::byte> payload) {
    return implementation_->send(session_id, payload);
}

bool KcpTransport::close(SessionId session_id) {
    return implementation_->close(session_id);
}

}  // namespace realm::network
