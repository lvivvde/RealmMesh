#include "realmmesh/network/kcp/kcp_security.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iterator>
#include <string_view>
#include <vector>

namespace realm::network {
namespace {

KcpSecurityKey test_key() {
    KcpSecurityKey key{};
    for (std::size_t index = 0; index < key.size(); ++index) {
        key[index] = static_cast<std::byte>(index + 1);
    }
    return key;
}

std::vector<std::byte> bytes(std::string_view text) {
    std::vector<std::byte> result;
    std::ranges::transform(text, std::back_inserter(result), [](char value) {
        return static_cast<std::byte>(value);
    });
    return result;
}

TEST(KcpTicketCodecTest, IssuesAuthenticShortLivedCredentials) {
    using namespace std::chrono_literals;
    const auto now = std::chrono::system_clock::time_point(1000s);
    const KcpTicketCodec codec(test_key());

    const auto credentials = codec.issue(now, 30s);
    const auto claims = codec.validate(credentials.ticket, now + 29s);

    ASSERT_TRUE(claims.has_value());
    EXPECT_EQ(claims->token_id, credentials.token_id);
    EXPECT_EQ(claims->expires_at, credentials.expires_at);
    EXPECT_EQ(claims->session_secret, credentials.session_secret);
    EXPECT_FALSE(codec.validate(credentials.ticket, now + 31s).has_value());
}

TEST(KcpTicketCodecTest, RejectsTamperedTicket) {
    using namespace std::chrono_literals;
    const auto now = std::chrono::system_clock::time_point(1000s);
    const KcpTicketCodec codec(test_key());
    auto credentials = codec.issue(now, 30s);
    credentials.ticket.back() ^= std::byte{0x01};

    EXPECT_FALSE(codec.validate(credentials.ticket, now).has_value());
}

TEST(KcpTicketCodecTest, RejectsAnAllZeroMasterKey) {
    EXPECT_THROW(
        static_cast<void>(KcpTicketCodec(KcpSecurityKey{})),
        std::invalid_argument);
}

TEST(KcpReplayWindowTest, RejectsDuplicatesAndPacketsOlderThanWindow) {
    KcpReplayWindow window;

    EXPECT_TRUE(window.accept(100));
    EXPECT_TRUE(window.accept(102));
    EXPECT_TRUE(window.accept(101));
    EXPECT_FALSE(window.accept(101));
    EXPECT_TRUE(window.accept(170));
    EXPECT_FALSE(window.accept(100));
}

TEST(KcpSessionCipherTest, AuthenticatesCiphertextAndAssociatedHeader) {
    KcpSecurityNonce nonce{};
    nonce[0] = std::byte{0x42};
    const KcpSessionCipher cipher(test_key(), nonce);
    const auto plaintext = bytes("kcp-packet");
    const auto header = bytes("session-header");
    auto encrypted = cipher.encrypt(7, plaintext, header);

    EXPECT_EQ(cipher.decrypt(7, encrypted, header), plaintext);

    encrypted.front() ^= std::byte{0x01};
    EXPECT_FALSE(cipher.decrypt(7, encrypted, header).has_value());
    EXPECT_FALSE(cipher.decrypt(8, encrypted, header).has_value());
}

}  // namespace
}  // namespace realm::network
