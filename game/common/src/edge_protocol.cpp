#include "realmmesh/game/common/edge_protocol.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace realm::game::common {
namespace {

class Writer final {
public:
    explicit Writer(EdgeOpcode opcode) { u8(static_cast<std::uint8_t>(opcode)); }

    void u8(std::uint8_t value) { data_.push_back(static_cast<std::byte>(value)); }
    void u16(std::uint16_t value) {
        data_.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
        data_.push_back(static_cast<std::byte>(value & 0xffU));
    }
    void u64(std::uint64_t value) {
        for (int shift = 56; shift >= 0; shift -= 8) {
            data_.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
        }
    }
    void bytes(std::span<const std::byte> value) {
        if (value.size() > std::numeric_limits<std::uint16_t>::max()) {
            throw std::length_error("edge protocol field exceeds 65535 bytes");
        }
        u16(static_cast<std::uint16_t>(value.size()));
        data_.insert(data_.end(), value.begin(), value.end());
    }
    void text(std::string_view value) { bytes(std::as_bytes(std::span(value))); }
    std::vector<std::byte> finish() { return std::move(data_); }

private:
    std::vector<std::byte> data_;
};

class Reader final {
public:
    explicit Reader(std::span<const std::byte> data) : data_(data) {}

    std::optional<std::uint8_t> u8() {
        if (remaining() < 1) return std::nullopt;
        return std::to_integer<std::uint8_t>(data_[offset_++]);
    }
    std::optional<std::uint16_t> u16() {
        if (remaining() < 2) return std::nullopt;
        const auto value = static_cast<std::uint16_t>(
            (std::to_integer<std::uint16_t>(data_[offset_]) << 8U) |
            std::to_integer<std::uint16_t>(data_[offset_ + 1]));
        offset_ += 2;
        return value;
    }
    std::optional<std::uint64_t> u64() {
        if (remaining() < 8) return std::nullopt;
        std::uint64_t value = 0;
        for (int index = 0; index < 8; ++index) {
            value = (value << 8U) |
                    std::to_integer<std::uint8_t>(data_[offset_++]);
        }
        return value;
    }
    std::optional<std::vector<std::byte>> bytes() {
        const auto size = u16();
        if (!size.has_value() || remaining() < *size) return std::nullopt;
        std::vector<std::byte> result(
            data_.begin() + static_cast<std::ptrdiff_t>(offset_),
            data_.begin() + static_cast<std::ptrdiff_t>(offset_ + *size));
        offset_ += *size;
        return result;
    }
    std::optional<std::string> text() {
        const auto value = bytes();
        if (!value.has_value()) return std::nullopt;
        return std::string(
            reinterpret_cast<const char*>(value->data()), value->size());
    }
    bool done() const { return offset_ == data_.size(); }

private:
    std::size_t remaining() const { return data_.size() - offset_; }
    std::span<const std::byte> data_;
    std::size_t offset_{0};
};

bool opcode(Reader& reader, EdgeOpcode expected) {
    const auto value = reader.u8();
    return value.has_value() && *value == static_cast<std::uint8_t>(expected);
}

void endpoint(Writer& writer, const ServiceEndpoint& value) {
    writer.text(value.address);
    writer.u16(value.port);
}

std::optional<ServiceEndpoint> endpoint(Reader& reader) {
    auto address = reader.text();
    auto port = reader.u16();
    if (!address.has_value() || !port.has_value()) return std::nullopt;
    return ServiceEndpoint{std::move(*address), *port};
}

}  // namespace

std::optional<EdgeOpcode> edge_opcode(std::span<const std::byte> payload) {
    if (payload.empty()) return std::nullopt;
    const auto value = std::to_integer<std::uint8_t>(payload.front());
    switch (static_cast<EdgeOpcode>(value)) {
    case EdgeOpcode::LoginRequest:
    case EdgeOpcode::LoginSucceeded:
    case EdgeOpcode::RealmAuthenticate:
    case EdgeOpcode::CharacterList:
    case EdgeOpcode::SelectCharacter:
    case EdgeOpcode::EnterGameIssued:
    case EdgeOpcode::EnterGame:
    case EdgeOpcode::EnterGameAccepted:
    case EdgeOpcode::Error:
        return static_cast<EdgeOpcode>(value);
    }
    return std::nullopt;
}

std::vector<std::byte> encode(const LoginRequest& message) {
    Writer writer(EdgeOpcode::LoginRequest);
    writer.text(message.account); writer.text(message.credential);
    return writer.finish();
}
std::vector<std::byte> encode(const LoginSucceeded& message) {
    Writer writer(EdgeOpcode::LoginSucceeded);
    writer.u64(message.account_id); writer.bytes(message.login_ticket);
    endpoint(writer, message.realm_endpoint); return writer.finish();
}
std::vector<std::byte> encode(const RealmAuthenticate& message) {
    Writer writer(EdgeOpcode::RealmAuthenticate);
    writer.bytes(message.login_ticket); return writer.finish();
}
std::vector<std::byte> encode(const CharacterList& message) {
    if (message.characters.size() > std::numeric_limits<std::uint16_t>::max()) {
        throw std::length_error("too many characters in edge response");
    }
    Writer writer(EdgeOpcode::CharacterList);
    writer.u16(static_cast<std::uint16_t>(message.characters.size()));
    for (const auto& character : message.characters) {
        writer.u64(character.id); writer.text(character.name);
    }
    return writer.finish();
}
std::vector<std::byte> encode(const SelectCharacter& message) {
    Writer writer(EdgeOpcode::SelectCharacter);
    writer.u64(message.character_id); return writer.finish();
}
std::vector<std::byte> encode(const EnterGameIssued& message) {
    Writer writer(EdgeOpcode::EnterGameIssued);
    writer.bytes(message.enter_game_ticket);
    endpoint(writer, message.gateway_endpoint); return writer.finish();
}
std::vector<std::byte> encode(const EnterGame& message) {
    Writer writer(EdgeOpcode::EnterGame);
    writer.bytes(message.enter_game_ticket); return writer.finish();
}
std::vector<std::byte> encode(const EnterGameAccepted& message) {
    Writer writer(EdgeOpcode::EnterGameAccepted);
    writer.u64(message.account_id); writer.u64(message.character_id);
    return writer.finish();
}
std::vector<std::byte> encode(const EdgeError& message) {
    Writer writer(EdgeOpcode::Error);
    writer.u16(message.code); writer.text(message.message); return writer.finish();
}

std::optional<LoginRequest> decode_login_request(std::span<const std::byte> payload) {
    Reader reader(payload); if (!opcode(reader, EdgeOpcode::LoginRequest)) return std::nullopt;
    auto account = reader.text(); auto credential = reader.text();
    if (!account || !credential || !reader.done()) return std::nullopt;
    return LoginRequest{std::move(*account), std::move(*credential)};
}
std::optional<LoginSucceeded> decode_login_succeeded(std::span<const std::byte> payload) {
    Reader reader(payload); if (!opcode(reader, EdgeOpcode::LoginSucceeded)) return std::nullopt;
    auto account = reader.u64(); auto ticket = reader.bytes(); auto target = endpoint(reader);
    if (!account || !ticket || !target || !reader.done()) return std::nullopt;
    return LoginSucceeded{*account, std::move(*ticket), std::move(*target)};
}
std::optional<RealmAuthenticate> decode_realm_authenticate(std::span<const std::byte> payload) {
    Reader reader(payload); if (!opcode(reader, EdgeOpcode::RealmAuthenticate)) return std::nullopt;
    auto ticket = reader.bytes(); if (!ticket || !reader.done()) return std::nullopt;
    return RealmAuthenticate{std::move(*ticket)};
}
std::optional<CharacterList> decode_character_list(std::span<const std::byte> payload) {
    Reader reader(payload); if (!opcode(reader, EdgeOpcode::CharacterList)) return std::nullopt;
    auto count = reader.u16(); if (!count) return std::nullopt;
    CharacterList result;
    result.characters.reserve(*count);
    for (std::uint16_t index = 0; index < *count; ++index) {
        auto id = reader.u64(); auto name = reader.text();
        if (!id || !name) return std::nullopt;
        result.characters.push_back({*id, std::move(*name)});
    }
    return reader.done() ? std::optional<CharacterList>(std::move(result)) : std::nullopt;
}
std::optional<SelectCharacter> decode_select_character(std::span<const std::byte> payload) {
    Reader reader(payload); if (!opcode(reader, EdgeOpcode::SelectCharacter)) return std::nullopt;
    auto id = reader.u64(); if (!id || !reader.done()) return std::nullopt;
    return SelectCharacter{*id};
}
std::optional<EnterGameIssued> decode_enter_game_issued(std::span<const std::byte> payload) {
    Reader reader(payload); if (!opcode(reader, EdgeOpcode::EnterGameIssued)) return std::nullopt;
    auto ticket = reader.bytes(); auto target = endpoint(reader);
    if (!ticket || !target || !reader.done()) return std::nullopt;
    return EnterGameIssued{std::move(*ticket), std::move(*target)};
}
std::optional<EnterGame> decode_enter_game(std::span<const std::byte> payload) {
    Reader reader(payload); if (!opcode(reader, EdgeOpcode::EnterGame)) return std::nullopt;
    auto ticket = reader.bytes(); if (!ticket || !reader.done()) return std::nullopt;
    return EnterGame{std::move(*ticket)};
}
std::optional<EnterGameAccepted> decode_enter_game_accepted(std::span<const std::byte> payload) {
    Reader reader(payload); if (!opcode(reader, EdgeOpcode::EnterGameAccepted)) return std::nullopt;
    auto account = reader.u64(); auto character = reader.u64();
    if (!account || !character || !reader.done()) return std::nullopt;
    return EnterGameAccepted{*account, *character};
}
std::optional<EdgeError> decode_edge_error(std::span<const std::byte> payload) {
    Reader reader(payload); if (!opcode(reader, EdgeOpcode::Error)) return std::nullopt;
    auto code = reader.u16(); auto message = reader.text();
    if (!code || !message || !reader.done()) return std::nullopt;
    return EdgeError{*code, std::move(*message)};
}

}  // namespace realm::game::common
