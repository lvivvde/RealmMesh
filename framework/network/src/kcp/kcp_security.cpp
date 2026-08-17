#include "realmmesh/network/kcp/kcp_security.hpp"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace realm::network {
namespace {

constexpr std::size_t token_plaintext_size = 1 + 8 + 8 + kcp_security_key_size;
constexpr std::byte token_version{1};
constexpr std::string_view ticket_context = "RealmMesh-KCP-ticket-v1";
constexpr std::array<std::byte, 4> secure_magic{
    std::byte{'R'}, std::byte{'M'}, std::byte{'K'}, std::byte{'1'}};
constexpr std::byte client_hello_kind{1};
constexpr std::byte client_data_kind{2};
constexpr std::byte server_data_kind{3};
constexpr std::size_t data_header_size = secure_magic.size() + 1 + 8 + 8;
constexpr std::size_t hello_header_size = secure_magic.size() + 1 + 8 + kcp_access_ticket_size;

void initialize_sodium() {
    static std::once_flag flag;
    static int result = -1;
    std::call_once(flag, [] { result = sodium_init(); });
    if (result < 0) {
        throw std::runtime_error("failed to initialize libsodium");
    }
}

void write_u64(std::span<std::byte, 8> output, std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < output.size(); ++index) {
        const auto shift = static_cast<unsigned>((output.size() - index - 1) * 8);
        output[index] = static_cast<std::byte>((value >> shift) & 0xffU);
    }
}

std::uint64_t read_u64(std::span<const std::byte, 8> input) noexcept {
    std::uint64_t value = 0;
    for (const std::byte item : input) {
        value = (value << 8U) | static_cast<std::uint64_t>(std::to_integer<unsigned>(item));
    }
    return value;
}

void append_u64(std::vector<std::byte>& output, std::uint64_t value) {
    const std::size_t offset = output.size();
    output.resize(offset + 8);
    write_u64(std::span<std::byte, 8>(output.data() + offset, 8), value);
}

bool has_magic(std::span<const std::byte> packet) noexcept {
    return packet.size() >= secure_magic.size() &&
           std::ranges::equal(packet.first(secure_magic.size()), secure_magic);
}

KcpSecurityKey direction_key(
    const KcpSecurityKey& secret,
    std::string_view context) {
    const KcpSecurityNonce empty_nonce{};
    return derive_kcp_session_key(
        secret,
        empty_nonce,
        empty_nonce,
        std::as_bytes(std::span(context)));
}

KcpSecurityNonce direction_nonce(std::byte direction) {
    KcpSecurityNonce nonce{};
    nonce[0] = direction;
    return nonce;
}

std::span<const unsigned char> as_unsigned(std::span<const std::byte> value) {
    return {
        reinterpret_cast<const unsigned char*>(value.data()),
        value.size(),
    };
}

unsigned char* as_unsigned(std::byte* value) {
    return reinterpret_cast<unsigned char*>(value);
}

std::uint64_t time_to_milliseconds(
    std::chrono::system_clock::time_point value) {
    const auto count = std::chrono::duration_cast<std::chrono::milliseconds>(
        value.time_since_epoch()).count();
    if (count < 0) {
        throw std::invalid_argument("KCP ticket time cannot be before Unix epoch");
    }
    return static_cast<std::uint64_t>(count);
}

}  // namespace

KcpTicketCodec::KcpTicketCodec(KcpSecurityKey master_key)
    : master_key_(master_key) {
    initialize_sodium();
    if (std::ranges::all_of(
            master_key_,
            [](std::byte value) { return value == std::byte{0}; })) {
        throw std::invalid_argument("KCP ticket key cannot be all zero");
    }
}

KcpAccessCredentials KcpTicketCodec::issue(
    std::chrono::system_clock::time_point now,
    std::chrono::seconds ttl) const {
    if (ttl <= std::chrono::seconds::zero()) {
        throw std::invalid_argument("KCP ticket TTL must be positive");
    }

    KcpAccessCredentials credentials{};
    randombytes_buf(&credentials.token_id, sizeof(credentials.token_id));
    if (credentials.token_id == 0) {
        credentials.token_id = 1;
    }
    credentials.expires_at = now + ttl;
    randombytes_buf(credentials.session_secret.data(), credentials.session_secret.size());

    std::array<std::byte, token_plaintext_size> plaintext{};
    plaintext[0] = token_version;
    write_u64(
        std::span<std::byte, 8>(plaintext.data() + 1, 8),
        credentials.token_id);
    write_u64(
        std::span<std::byte, 8>(plaintext.data() + 9, 8),
        time_to_milliseconds(credentials.expires_at));
    std::ranges::copy(credentials.session_secret, plaintext.begin() + 17);

    KcpSecurityNonce nonce{};
    randombytes_buf(nonce.data(), nonce.size());
    credentials.ticket.resize(
        nonce.size() + plaintext.size() +
        crypto_aead_xchacha20poly1305_ietf_ABYTES);
    std::ranges::copy(nonce, credentials.ticket.begin());

    unsigned long long encrypted_size = 0;
    const auto context = as_unsigned(std::as_bytes(std::span(ticket_context)));
    const int result = crypto_aead_xchacha20poly1305_ietf_encrypt(
        as_unsigned(credentials.ticket.data() + nonce.size()),
        &encrypted_size,
        as_unsigned(plaintext).data(),
        plaintext.size(),
        context.data(),
        context.size(),
        nullptr,
        as_unsigned(nonce).data(),
        as_unsigned(master_key_).data());
    if (result != 0 || encrypted_size !=
            plaintext.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES) {
        throw std::runtime_error("failed to encrypt KCP access ticket");
    }
    return credentials;
}

std::optional<KcpTicketClaims> KcpTicketCodec::validate(
    std::span<const std::byte> ticket,
    std::chrono::system_clock::time_point now) const {
    if (ticket.size() != kcp_access_ticket_size) {
        return std::nullopt;
    }

    const auto nonce = ticket.first<kcp_security_nonce_size>();
    const auto ciphertext = ticket.subspan(kcp_security_nonce_size);
    std::array<std::byte, token_plaintext_size> plaintext{};
    unsigned long long plaintext_size = 0;
    const auto context = as_unsigned(std::as_bytes(std::span(ticket_context)));
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            as_unsigned(plaintext.data()),
            &plaintext_size,
            nullptr,
            as_unsigned(ciphertext).data(),
            ciphertext.size(),
            context.data(),
            context.size(),
            as_unsigned(nonce).data(),
            as_unsigned(master_key_).data()) != 0 ||
        plaintext_size != plaintext.size() ||
        plaintext[0] != token_version) {
        return std::nullopt;
    }

    KcpTicketClaims claims{};
    claims.token_id = read_u64(std::span<const std::byte, 8>(plaintext.data() + 1, 8));
    const auto expiry_ms = read_u64(
        std::span<const std::byte, 8>(plaintext.data() + 9, 8));
    if (expiry_ms > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return std::nullopt;
    }
    claims.expires_at = std::chrono::system_clock::time_point(
        std::chrono::milliseconds(static_cast<std::int64_t>(expiry_ms)));
    std::ranges::copy(plaintext.begin() + 17, plaintext.end(), claims.session_secret.begin());
    if (claims.token_id == 0 || now > claims.expires_at) {
        return std::nullopt;
    }
    return claims;
}

bool KcpReplayWindow::accept(std::uint64_t sequence) noexcept {
    if (!initialized_) {
        initialized_ = true;
        highest_sequence_ = sequence;
        seen_mask_ = 1;
        return true;
    }
    if (sequence > highest_sequence_) {
        const std::uint64_t advance = sequence - highest_sequence_;
        seen_mask_ = advance >= 64 ? 1 : (seen_mask_ << advance) | 1;
        highest_sequence_ = sequence;
        return true;
    }

    const std::uint64_t distance = highest_sequence_ - sequence;
    if (distance >= 64) {
        return false;
    }
    const std::uint64_t bit = std::uint64_t{1} << distance;
    if ((seen_mask_ & bit) != 0) {
        return false;
    }
    seen_mask_ |= bit;
    return true;
}

KcpSessionCipher::KcpSessionCipher(
    KcpSecurityKey key,
    KcpSecurityNonce nonce_prefix)
    : key_(key), nonce_prefix_(nonce_prefix) {
    initialize_sodium();
}

std::vector<std::byte> KcpSessionCipher::encrypt(
    std::uint64_t sequence,
    std::span<const std::byte> plaintext,
    std::span<const std::byte> associated_data) const {
    std::vector<std::byte> ciphertext(
        plaintext.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES);
    unsigned long long ciphertext_size = 0;
    const auto nonce = nonce_for(sequence);
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(
            as_unsigned(ciphertext.data()),
            &ciphertext_size,
            as_unsigned(plaintext).data(),
            plaintext.size(),
            as_unsigned(associated_data).data(),
            associated_data.size(),
            nullptr,
            as_unsigned(nonce).data(),
            as_unsigned(key_).data()) != 0) {
        throw std::runtime_error("failed to encrypt KCP session packet");
    }
    ciphertext.resize(static_cast<std::size_t>(ciphertext_size));
    return ciphertext;
}

std::optional<std::vector<std::byte>> KcpSessionCipher::decrypt(
    std::uint64_t sequence,
    std::span<const std::byte> ciphertext,
    std::span<const std::byte> associated_data) const {
    if (ciphertext.size() < crypto_aead_xchacha20poly1305_ietf_ABYTES) {
        return std::nullopt;
    }
    std::vector<std::byte> plaintext(
        ciphertext.size() - crypto_aead_xchacha20poly1305_ietf_ABYTES);
    unsigned long long plaintext_size = 0;
    const auto nonce = nonce_for(sequence);
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            as_unsigned(plaintext.data()),
            &plaintext_size,
            nullptr,
            as_unsigned(ciphertext).data(),
            ciphertext.size(),
            as_unsigned(associated_data).data(),
            associated_data.size(),
            as_unsigned(nonce).data(),
            as_unsigned(key_).data()) != 0) {
        return std::nullopt;
    }
    plaintext.resize(static_cast<std::size_t>(plaintext_size));
    return plaintext;
}

KcpSecurityNonce KcpSessionCipher::nonce_for(std::uint64_t sequence) const noexcept {
    KcpSecurityNonce nonce = nonce_prefix_;
    write_u64(
        std::span<std::byte, 8>(nonce.data() + nonce.size() - 8, 8),
        sequence);
    return nonce;
}

KcpClientSecurity::KcpClientSecurity(KcpAccessCredentials credentials)
    : credentials_(std::move(credentials)),
      send_cipher_(
          direction_key(credentials_.session_secret, "client-to-server-v1"),
          direction_nonce(std::byte{1})),
      receive_cipher_(
          direction_key(credentials_.session_secret, "server-to-client-v1"),
          direction_nonce(std::byte{2})) {
    if (credentials_.token_id == 0 ||
        credentials_.ticket.size() != kcp_access_ticket_size) {
        throw std::invalid_argument("invalid KCP client credentials");
    }
}

std::uint64_t KcpClientSecurity::session_id() const noexcept {
    return credentials_.token_id;
}

bool KcpClientSecurity::confirmed() const noexcept {
    return confirmed_;
}

std::vector<std::byte> KcpClientSecurity::protect(
    std::span<const std::byte> kcp_datagram) {
    if (next_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("KCP send sequence exhausted");
    }

    std::vector<std::byte> header;
    header.insert(header.end(), secure_magic.begin(), secure_magic.end());
    if (confirmed_) {
        header.push_back(client_data_kind);
        append_u64(header, credentials_.token_id);
        append_u64(header, next_sequence_);
    } else {
        header.push_back(client_hello_kind);
        append_u64(header, next_sequence_);
        header.insert(
            header.end(),
            credentials_.ticket.begin(),
            credentials_.ticket.end());
    }

    auto ciphertext = send_cipher_.encrypt(
        next_sequence_++, kcp_datagram, header);
    header.insert(header.end(), ciphertext.begin(), ciphertext.end());
    return header;
}

std::optional<std::vector<std::byte>> KcpClientSecurity::unprotect(
    std::span<const std::byte> protected_datagram) {
    if (protected_datagram.size() <
            data_header_size + crypto_aead_xchacha20poly1305_ietf_ABYTES ||
        !has_magic(protected_datagram) ||
        protected_datagram[secure_magic.size()] != server_data_kind) {
        return std::nullopt;
    }
    const auto session = read_u64(std::span<const std::byte, 8>(
        protected_datagram.data() + secure_magic.size() + 1, 8));
    const auto sequence = read_u64(std::span<const std::byte, 8>(
        protected_datagram.data() + secure_magic.size() + 1 + 8, 8));
    if (session != credentials_.token_id) {
        return std::nullopt;
    }

    const auto header = protected_datagram.first(data_header_size);
    auto plaintext = receive_cipher_.decrypt(
        sequence,
        protected_datagram.subspan(data_header_size),
        header);
    if (!plaintext.has_value() || !receive_window_.accept(sequence)) {
        return std::nullopt;
    }
    confirmed_ = true;
    return plaintext;
}

KcpSecurityKey derive_kcp_session_key(
    const KcpSecurityKey& session_secret,
    const KcpSecurityNonce& client_nonce,
    const KcpSecurityNonce& server_nonce,
    std::span<const std::byte> context) {
    initialize_sodium();
    std::vector<std::byte> input;
    input.reserve(client_nonce.size() + server_nonce.size() + context.size());
    input.insert(input.end(), client_nonce.begin(), client_nonce.end());
    input.insert(input.end(), server_nonce.begin(), server_nonce.end());
    input.insert(input.end(), context.begin(), context.end());

    KcpSecurityKey result{};
    if (crypto_generichash(
            as_unsigned(result.data()),
            result.size(),
            as_unsigned(input).data(),
            input.size(),
            as_unsigned(session_secret).data(),
            session_secret.size()) != 0) {
        throw std::runtime_error("failed to derive KCP session key");
    }
    return result;
}

}  // namespace realm::network
