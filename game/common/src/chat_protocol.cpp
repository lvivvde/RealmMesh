#include "realmmesh/game/common/chat_protocol.hpp"
#include "envelope_codec.hpp"

#include <algorithm>
#include <stdexcept>

namespace realm::game::common {
namespace {
namespace chat = ::realmmesh::protocol::chat::v1;

bool counter(std::uint64_t value) { return value != 0 && value <= chat_counter_max; }
bool kind(chat::ChannelKind value) {
    return value == chat::CHANNEL_KIND_SCENE || value == chat::CHANNEL_KIND_REALM;
}
bool reason(chat::Reason value) {
    return value >= chat::REASON_NONE && value <= chat::REASON_COUNTER_EXHAUSTED;
}
bool identity(std::string_view value) {
    return !value.empty() && value.size() <= 128 &&
           std::ranges::all_of(value, [](unsigned char c) { return c >= 33 && c <= 126; });
}
bool channel(const chat::Channel& m) {
    return m.realm_id() != 0 && kind(m.kind()) &&
           (m.kind() == chat::CHANNEL_KIND_SCENE ?
                (m.shard_id() == 1 || m.shard_id() == 2) : m.shard_id() == 0);
}
bool same_channel(const chat::Channel& a, const chat::Channel& b) {
    return a.realm_id() == b.realm_id() && a.kind() == b.kind() && a.shard_id() == b.shard_id();
}
bool epoch(const chat::Epoch& m) {
    return m.has_channel() && channel(m.channel()) && identity(m.chat_incarnation());
}
bool same_epoch(const chat::Epoch& a, const chat::Epoch& b) {
    return same_channel(a.channel(), b.channel()) && a.chat_incarnation() == b.chat_incarnation();
}
bool membership(const chat::Membership& m) {
    return m.has_channel() && channel(m.channel()) && m.character_id() != 0 &&
           identity(m.realm_incarnation()) && m.session_binding() != 0 && counter(m.generation()) &&
           (m.channel().kind() == chat::CHANNEL_KIND_SCENE ?
                counter(m.scene_binding_epoch()) : m.scene_binding_epoch() == 0);
}
bool receipt(const chat::Receipt& m) {
    return m.has_epoch() && epoch(m.epoch()) && counter(m.channel_seq()) && counter(m.client_send_seq());
}
bool valid(const GetChatState& m, std::uint64_t id) { return id != 0 && kind(m.kind()); }
bool valid(const SendChat& m, std::uint64_t id) {
    return id != 0 && kind(m.kind()) && counter(m.membership_generation()) &&
           m.has_expected_epoch() && epoch(m.expected_epoch()) &&
           m.expected_epoch().channel().kind() == m.kind() && counter(m.client_send_seq()) && valid_chat_text(m.text());
}
bool valid(const ChatState& m, std::uint64_t id) {
    if (id == 0 || !kind(m.kind()) || !reason(m.reason())) return false;
    if (m.phase() == chat::PHASE_UNAVAILABLE)
        return m.reason() != chat::REASON_NONE && m.membership_generation() == 0 &&
               !m.has_epoch() && m.join_watermark() == 0 && m.last_client_send_seq() == 0 && !m.has_last_receipt();
    if (m.phase() != chat::PHASE_ACTIVE || m.reason() != chat::REASON_NONE ||
        !counter(m.membership_generation()) || !m.has_epoch() || !epoch(m.epoch()) ||
        m.epoch().channel().kind() != m.kind() || m.join_watermark() > chat_counter_max ||
        m.last_client_send_seq() > chat_counter_max) return false;
    if (m.last_client_send_seq() == 0) return !m.has_last_receipt();
    return m.has_last_receipt() && receipt(m.last_receipt()) &&
           m.last_receipt().client_send_seq() == m.last_client_send_seq() &&
           same_epoch(m.last_receipt().epoch(), m.epoch()) &&
           m.last_receipt().channel_seq() > m.join_watermark();
}
bool valid(const ChatSendResult& m, std::uint64_t id) {
    if (id == 0 || !kind(m.kind()) || !counter(m.membership_generation()) ||
        !m.has_epoch() || !epoch(m.epoch()) || m.epoch().channel().kind() != m.kind() ||
        !counter(m.client_send_seq()) || !reason(m.reason())) return false;
    if (m.outcome() == chat::OUTCOME_ACCEPTED)
        return m.reason() == chat::REASON_NONE && m.retry_after_ms() == 0 && m.has_receipt() &&
               receipt(m.receipt()) && same_epoch(m.receipt().epoch(), m.epoch()) &&
               m.receipt().client_send_seq() == m.client_send_seq();
    if (m.has_receipt()) return false;
    if (m.outcome() == chat::OUTCOME_UNKNOWN)
        return m.retry_after_ms() == 0 && (m.reason() == chat::REASON_DEADLINE ||
               m.reason() == chat::REASON_OUTCOME_LOST || m.reason() == chat::REASON_UNAVAILABLE);
    if (m.outcome() != chat::OUTCOME_REJECTED || m.reason() == chat::REASON_NONE ||
        m.reason() == chat::REASON_OUTCOME_LOST || m.reason() == chat::REASON_GAP) return false;
    const bool retry = m.reason() == chat::REASON_RATE_LIMITED ||
                       m.reason() == chat::REASON_BUSY || m.reason() == chat::REASON_RESOURCE_LIMIT;
    return retry ? (m.retry_after_ms() != 0 && m.retry_after_ms() <= chat_retry_after_ms_max) :
                   m.retry_after_ms() == 0;
}
bool valid(const ChatMessage& m, std::uint64_t id) {
    return id == 0 && counter(m.membership_generation()) && m.has_epoch() && epoch(m.epoch()) &&
           counter(m.channel_seq()) && m.sender_character_id() != 0 && valid_chat_text(m.text());
}
bool valid(const ChatUnavailable& m, std::uint64_t id) {
    return id == 0 && kind(m.kind()) && reason(m.reason()) && m.reason() != chat::REASON_NONE;
}
bool link(const chat::Link& m, bool hello) {
    return m.realm_id() != 0 && identity(m.realm_instance_id()) && identity(m.realm_incarnation()) &&
           identity(m.chat_instance_id()) && (hello ?
                (m.chat_incarnation().empty() && m.link_generation() == 0) :
                (identity(m.chat_incarnation()) && counter(m.link_generation())));
}
bool operation(chat::Operation value) {
    return value >= chat::OPERATION_HELLO && value <= chat::OPERATION_STATE;
}
bool matches_link(const chat::Membership& m, const chat::Link& l) {
    return membership(m) && m.channel().realm_id() == l.realm_id() && m.realm_incarnation() == l.realm_incarnation();
}
bool matches_epoch(const chat::Epoch& e, const chat::Membership& m, const chat::Link& l) {
    return same_channel(e.channel(), m.channel()) && e.chat_incarnation() == l.chat_incarnation();
}
bool valid(const ChatInternalRequest& m, std::uint64_t id) {
    if (id == 0 || !operation(m.operation()) || !m.has_link() ||
        !link(m.link(), m.operation() == chat::OPERATION_HELLO)) return false;
    if (m.operation() == chat::OPERATION_HELLO) return !m.has_membership() && !m.has_send();
    if (!m.has_membership() || !matches_link(m.membership(), m.link())) return false;
    if (m.operation() != chat::OPERATION_SEND) return !m.has_send();
    return m.has_send() && valid(m.send(), id) &&
           m.send().membership_generation() == m.membership().generation() &&
           matches_epoch(m.send().expected_epoch(), m.membership(), m.link());
}
bool valid(const ChatInternalReply& m, std::uint64_t id) {
    if (id == 0 || !operation(m.operation()) || !m.has_link() ||
        m.status() < chat::STATUS_OK || m.status() > chat::STATUS_UNAVAILABLE) return false;
    const bool rejected_hello = m.operation() == chat::OPERATION_HELLO && m.status() != chat::STATUS_OK;
    if (!link(m.link(), rejected_hello)) return false;
    if (m.status() != chat::STATUS_OK || m.operation() == chat::OPERATION_HELLO)
        return !m.has_membership() && !m.has_state() && !m.has_result();
    if (!m.has_membership() || !matches_link(m.membership(), m.link())) return false;
    if (m.operation() == chat::OPERATION_LEAVE) return !m.has_state() && !m.has_result();
    if (m.operation() == chat::OPERATION_SEND)
        return !m.has_state() && m.has_result() && valid(m.result(), id) &&
               m.result().membership_generation() == m.membership().generation() &&
               matches_epoch(m.result().epoch(), m.membership(), m.link());
    return !m.has_result() && m.has_state() && valid(m.state(), id) &&
           m.state().phase() == chat::PHASE_ACTIVE &&
           m.state().membership_generation() == m.membership().generation() &&
           matches_epoch(m.state().epoch(), m.membership(), m.link());
}
bool valid(const ChatInternalPush& m, std::uint64_t id) {
    return id == 0 && m.has_link() && link(m.link(), false) && m.has_membership() &&
           matches_link(m.membership(), m.link()) && m.has_message() && valid(m.message(), 0) &&
           m.message().membership_generation() == m.membership().generation() &&
           matches_epoch(m.message().epoch(), m.membership(), m.link());
}

template <typename Message>
std::vector<std::byte> encode_chat(const Message& m, std::uint32_t type, std::uint64_t id) {
    if (m.ByteSizeLong() > chat_envelope_max || !valid(m, id))
        throw std::invalid_argument("invalid chat message");
    auto bytes = detail::encode_message(m, type, id);
    if (bytes.size() > chat_envelope_max) throw std::invalid_argument("chat envelope exceeds 64KiB");
    return bytes;
}
template <typename Message>
std::optional<Message> decode_chat(std::span<const std::byte> bytes, std::uint32_t type) {
    if (bytes.size() > chat_envelope_max) return std::nullopt;
    const auto envelope = detail::parse_envelope(bytes);
    if (!envelope || envelope->message_id() != type) return std::nullopt;
    auto message = detail::decode_message<Message>(bytes, type);
    if (!message || !valid(*message, envelope->request_id())) return std::nullopt;
    return message;
}
std::optional<ChatMessageId> message_id(std::span<const std::byte> bytes, std::uint32_t first, std::uint32_t last) {
    if (bytes.size() > chat_envelope_max) return std::nullopt;
    const auto envelope = detail::parse_envelope(bytes);
    if (!envelope || envelope->message_id() < first || envelope->message_id() > last) return std::nullopt;
    return static_cast<ChatMessageId>(envelope->message_id());
}
}  // namespace

bool valid_chat_text(std::string_view text) noexcept {
    if (text.empty() || text.size() > chat_text_bytes_max) return false;
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first < 0x80) {
            if (first < 0x20 || first == 0x7f) return false;
            continue;
        }
        std::size_t extra{};
        std::uint32_t point{}, minimum{};
        if (first >= 0xc2 && first <= 0xdf) { extra = 1; point = first & 0x1fU; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { extra = 2; point = first & 0x0fU; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { extra = 3; point = first & 0x07U; minimum = 0x10000; }
        else return false;
        if (text.size() - i < extra) return false;
        for (std::size_t j = 0; j < extra; ++j) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0U) != 0x80U) return false;
            point = (point << 6U) | (next & 0x3fU);
        }
        if (point < minimum || point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff)) return false;
    }
    return true;
}
std::optional<ChatMessageId> chat_message_id(std::span<const std::byte> bytes) { return message_id(bytes, 1701, 1706); }
std::optional<ChatMessageId> chat_internal_message_id(std::span<const std::byte> bytes) { return message_id(bytes, 1801, 1803); }
std::optional<std::uint64_t> chat_request_id(std::span<const std::byte> bytes) {
    if (!chat_message_id(bytes)) return std::nullopt;
    return detail::parse_envelope(bytes)->request_id();
}
std::optional<std::uint64_t> chat_internal_request_id(std::span<const std::byte> bytes) {
    if (!chat_internal_message_id(bytes)) return std::nullopt;
    return detail::parse_envelope(bytes)->request_id();
}
std::vector<std::byte> encode(const GetChatState& message, std::uint64_t request_id) {
    return encode_chat(message, 1701, request_id);
}
std::optional<GetChatState> decode_get_chat_state(std::span<const std::byte> payload) {
    return decode_chat<GetChatState>(payload, 1701);
}
std::vector<std::byte> encode(const ChatState& message, std::uint64_t request_id) {
    return encode_chat(message, 1702, request_id);
}
std::optional<ChatState> decode_chat_state(std::span<const std::byte> payload) {
    return decode_chat<ChatState>(payload, 1702);
}
std::vector<std::byte> encode(const SendChat& message, std::uint64_t request_id) {
    return encode_chat(message, 1703, request_id);
}
std::optional<SendChat> decode_send_chat(std::span<const std::byte> payload) {
    return decode_chat<SendChat>(payload, 1703);
}
std::vector<std::byte> encode(const ChatSendResult& message, std::uint64_t request_id) {
    return encode_chat(message, 1704, request_id);
}
std::optional<ChatSendResult> decode_chat_send_result(std::span<const std::byte> payload) {
    return decode_chat<ChatSendResult>(payload, 1704);
}
std::vector<std::byte> encode(const ChatMessage& message, std::uint64_t request_id) {
    return encode_chat(message, 1705, request_id);
}
std::optional<ChatMessage> decode_chat_message(std::span<const std::byte> payload) {
    return decode_chat<ChatMessage>(payload, 1705);
}
std::vector<std::byte> encode(const ChatUnavailable& message, std::uint64_t request_id) {
    return encode_chat(message, 1706, request_id);
}
std::optional<ChatUnavailable> decode_chat_unavailable(std::span<const std::byte> payload) {
    return decode_chat<ChatUnavailable>(payload, 1706);
}
std::vector<std::byte> encode(const ChatInternalRequest& message, std::uint64_t request_id) {
    return encode_chat(message, 1801, request_id);
}
std::optional<ChatInternalRequest> decode_chat_internal_request(std::span<const std::byte> payload) {
    return decode_chat<ChatInternalRequest>(payload, 1801);
}
std::vector<std::byte> encode(const ChatInternalReply& message, std::uint64_t request_id) {
    return encode_chat(message, 1802, request_id);
}
std::optional<ChatInternalReply> decode_chat_internal_reply(std::span<const std::byte> payload) {
    return decode_chat<ChatInternalReply>(payload, 1802);
}
std::vector<std::byte> encode(const ChatInternalPush& message, std::uint64_t request_id) {
    return encode_chat(message, 1803, request_id);
}
std::optional<ChatInternalPush> decode_chat_internal_push(std::span<const std::byte> payload) {
    return decode_chat<ChatInternalPush>(payload, 1803);
}
}  // namespace realm::game::common
