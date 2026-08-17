#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace realm::network {

inline constexpr std::size_t kcp_security_key_size = 32;
inline constexpr std::size_t kcp_security_nonce_size = 24;
inline constexpr std::size_t kcp_access_ticket_size = 89;

using KcpSecurityKey = std::array<std::byte, kcp_security_key_size>;
using KcpSecurityNonce = std::array<std::byte, kcp_security_nonce_size>;

struct KcpAccessCredentials {
    std::uint64_t token_id;
    std::chrono::system_clock::time_point expires_at;
    KcpSecurityKey session_secret;
    std::vector<std::byte> ticket;
};

struct KcpTicketClaims {
    std::uint64_t token_id;
    std::chrono::system_clock::time_point expires_at;
    KcpSecurityKey session_secret;
};

class KcpTicketCodec final {
public:
    explicit KcpTicketCodec(KcpSecurityKey master_key);

    [[nodiscard]] KcpAccessCredentials issue(
        std::chrono::system_clock::time_point now,
        std::chrono::seconds ttl) const;
    [[nodiscard]] std::optional<KcpTicketClaims> validate(
        std::span<const std::byte> ticket,
        std::chrono::system_clock::time_point now) const;

private:
    KcpSecurityKey master_key_;
};

class KcpReplayWindow final {
public:
    [[nodiscard]] bool accept(std::uint64_t sequence) noexcept;

private:
    std::uint64_t highest_sequence_{0};
    std::uint64_t seen_mask_{0};
    bool initialized_{false};
};

class KcpSessionCipher final {
public:
    KcpSessionCipher(KcpSecurityKey key, KcpSecurityNonce nonce_prefix);

    [[nodiscard]] std::vector<std::byte> encrypt(
        std::uint64_t sequence,
        std::span<const std::byte> plaintext,
        std::span<const std::byte> associated_data) const;
    [[nodiscard]] std::optional<std::vector<std::byte>> decrypt(
        std::uint64_t sequence,
        std::span<const std::byte> ciphertext,
        std::span<const std::byte> associated_data) const;

private:
    [[nodiscard]] KcpSecurityNonce nonce_for(std::uint64_t sequence) const noexcept;

    KcpSecurityKey key_;
    KcpSecurityNonce nonce_prefix_;
};

class KcpClientSecurity final {
public:
    explicit KcpClientSecurity(KcpAccessCredentials credentials);

    [[nodiscard]] std::uint64_t session_id() const noexcept;
    [[nodiscard]] bool confirmed() const noexcept;
    [[nodiscard]] std::vector<std::byte> protect(
        std::span<const std::byte> kcp_datagram);
    [[nodiscard]] std::optional<std::vector<std::byte>> unprotect(
        std::span<const std::byte> protected_datagram);

private:
    KcpAccessCredentials credentials_;
    KcpSessionCipher send_cipher_;
    KcpSessionCipher receive_cipher_;
    KcpReplayWindow receive_window_;
    std::uint64_t next_sequence_{0};
    bool confirmed_{false};
};

[[nodiscard]] KcpSecurityKey derive_kcp_session_key(
    const KcpSecurityKey& session_secret,
    const KcpSecurityNonce& client_nonce,
    const KcpSecurityNonce& server_nonce,
    std::span<const std::byte> context);

}  // namespace realm::network
