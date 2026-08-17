#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace realm::game::common {

enum class EdgeOpcode : std::uint8_t {
    LoginRequest = 1,
    LoginSucceeded = 2,
    RealmAuthenticate = 3,
    CharacterList = 4,
    SelectCharacter = 5,
    EnterGameIssued = 6,
    EnterGame = 7,
    EnterGameAccepted = 8,
    Error = 255,
};

struct ServiceEndpoint {
    std::string address;
    std::uint16_t port{0};
    bool operator==(const ServiceEndpoint&) const = default;
};

struct LoginRequest {
    std::string account;
    std::string credential;
    bool operator==(const LoginRequest&) const = default;
};

struct LoginSucceeded {
    std::uint64_t account_id{0};
    std::vector<std::byte> login_ticket;
    ServiceEndpoint realm_endpoint;
    bool operator==(const LoginSucceeded&) const = default;
};

struct RealmAuthenticate {
    std::vector<std::byte> login_ticket;
    bool operator==(const RealmAuthenticate&) const = default;
};

struct CharacterSummary {
    std::uint64_t id{0};
    std::string name;
    bool operator==(const CharacterSummary&) const = default;
};

struct CharacterList {
    std::vector<CharacterSummary> characters;
    bool operator==(const CharacterList&) const = default;
};

struct SelectCharacter {
    std::uint64_t character_id{0};
    bool operator==(const SelectCharacter&) const = default;
};

struct EnterGameIssued {
    std::vector<std::byte> enter_game_ticket;
    ServiceEndpoint gateway_endpoint;
    bool operator==(const EnterGameIssued&) const = default;
};

struct EnterGame {
    std::vector<std::byte> enter_game_ticket;
    bool operator==(const EnterGame&) const = default;
};

struct EnterGameAccepted {
    std::uint64_t account_id{0};
    std::uint64_t character_id{0};
    bool operator==(const EnterGameAccepted&) const = default;
};

struct EdgeError {
    std::uint16_t code{0};
    std::string message;
    bool operator==(const EdgeError&) const = default;
};

[[nodiscard]] std::optional<EdgeOpcode> edge_opcode(
    std::span<const std::byte> payload);

[[nodiscard]] std::vector<std::byte> encode(const LoginRequest& message);
[[nodiscard]] std::vector<std::byte> encode(const LoginSucceeded& message);
[[nodiscard]] std::vector<std::byte> encode(const RealmAuthenticate& message);
[[nodiscard]] std::vector<std::byte> encode(const CharacterList& message);
[[nodiscard]] std::vector<std::byte> encode(const SelectCharacter& message);
[[nodiscard]] std::vector<std::byte> encode(const EnterGameIssued& message);
[[nodiscard]] std::vector<std::byte> encode(const EnterGame& message);
[[nodiscard]] std::vector<std::byte> encode(const EnterGameAccepted& message);
[[nodiscard]] std::vector<std::byte> encode(const EdgeError& message);

[[nodiscard]] std::optional<LoginRequest> decode_login_request(
    std::span<const std::byte> payload);
[[nodiscard]] std::optional<LoginSucceeded> decode_login_succeeded(
    std::span<const std::byte> payload);
[[nodiscard]] std::optional<RealmAuthenticate> decode_realm_authenticate(
    std::span<const std::byte> payload);
[[nodiscard]] std::optional<CharacterList> decode_character_list(
    std::span<const std::byte> payload);
[[nodiscard]] std::optional<SelectCharacter> decode_select_character(
    std::span<const std::byte> payload);
[[nodiscard]] std::optional<EnterGameIssued> decode_enter_game_issued(
    std::span<const std::byte> payload);
[[nodiscard]] std::optional<EnterGame> decode_enter_game(
    std::span<const std::byte> payload);
[[nodiscard]] std::optional<EnterGameAccepted> decode_enter_game_accepted(
    std::span<const std::byte> payload);
[[nodiscard]] std::optional<EdgeError> decode_edge_error(
    std::span<const std::byte> payload);

}  // namespace realm::game::common
