/// 兑换口的帧级单测:不起 socket,只喂罐头字节,钉住 `WireEnterRealmRedeemer`
/// 对「一帧请求 + 一帧应答」的处理——1305 成功、1999 拒绝、坏帧、读空各自
/// 落到哪个 `ChainFailure`、detail 是什么。真实服务端的对撞在集成档
/// (wire_login_transport_integration_test)。

#include "realmmesh/client/wire_login_transport.hpp"
#include "realmmesh/common/v1/envelope.pb.h"
#include "realmmesh/game/common/edge_protocol.hpp"
#include "realmmesh/network/client/edge_client_connection.hpp"
#include "realmmesh/network/codec/length_field_codec.hpp"
#include "realmmesh/network/core/byte_buffer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace realm::client {
namespace {

namespace common = ::realm::game::common;
namespace edge_v1 = ::realmmesh::protocol::edge::v1;
namespace net_client = ::realm::network::client;

/// 帧级替身:记录写出的字节,并按需回放罐头帧(含长度前缀)。不起 socket,
/// 只验兑换口对「一帧一答」的处理;canned 空 = 对端无数据(超时/断开路径)。
class CannedStream final : public net_client::ISecureByteStream {
public:
    std::vector<std::byte> canned;
    std::vector<std::byte> written;

    [[nodiscard]] bool write_all(std::span<const std::byte> data,
                                 net_client::StreamDeadline deadline) override {
        static_cast<void>(deadline);
        written.insert(written.end(), data.begin(), data.end());
        return true;
    }

    [[nodiscard]] std::optional<std::size_t> read_some(
        std::span<std::byte> out, net_client::StreamDeadline deadline) override {
        static_cast<void>(deadline);
        if (canned.empty()) {
            return std::nullopt;
        }
        const std::size_t count = std::min(out.size(), canned.size());
        std::copy_n(canned.begin(), count, out.begin());
        canned.erase(canned.begin(),
                     canned.begin() + static_cast<std::ptrdiff_t>(count));
        return count;
    }

    void shutdown() override {}
};

/// 一帧的线上字节(补长度前缀,与 EdgeClientConnection 的收发同款)。
[[nodiscard]] std::vector<std::byte> framed(
    std::span<const std::byte> payload) {
    return network::LengthFieldCodec{net_client::kMaxEdgeFramePayload}.encode(
        payload);
}

/// 反解兑换口写出的请求:去前缀 → 信封 → 1304 请求体。取不到即 nullopt。
[[nodiscard]] std::optional<common::EnterRealm> parse_enter_realm_request(
    std::span<const std::byte> written) {
    network::ByteBuffer buffer;
    buffer.append(written);
    const auto decoded = network::LengthFieldCodec{
        net_client::kMaxEdgeFramePayload}.try_decode(buffer);
    if (decoded.status != network::DecodeStatus::FrameReady) {
        return std::nullopt;
    }
    const auto message_id = common::edge_message_id(decoded.payload);
    if (!message_id.has_value() ||
        *message_id != edge_v1::MESSAGE_ID_C2S_ENTER_REALM) {
        return std::nullopt;
    }
    return common::decode_enter_realm(decoded.payload);
}

[[nodiscard]] PortStatus redeem_with(CannedStream& stream) {
    WireEnterRealmRedeemer redeemer;
    return redeemer.redeem(stream, "ert-1",
                           Clock::now() + std::chrono::seconds{1});
}

/// 写出的必须是一帧可解析的 1304(票据原样进 payload);收到 1305 即成功。
TEST(WireEnterRealmRedeemerTest, Sends1304AndAccepts1305) {
    common::EnterRealmAccepted accepted;
    accepted.set_account_id(42);
    CannedStream stream;
    stream.canned = framed(common::encode(accepted));

    const auto status = redeem_with(stream);

    ASSERT_TRUE(status.ok) << status.detail;
    const auto request = parse_enter_realm_request(stream.written);
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->enter_realm_ticket(), "ert-1");
}

/// 1999 EdgeError(3002 无效票据)→ EnterRealmRejected,detail 带服务端错误码。
TEST(WireEnterRealmRedeemerTest, MapsEdgeErrorToEnterRealmRejected) {
    common::EdgeError error;
    error.set_code(static_cast<std::uint32_t>(
        common::edge_error_invalid_enter_realm_ticket));
    error.set_message("invalid enter realm ticket");
    CannedStream stream;
    stream.canned = framed(common::encode(error));

    const auto status = redeem_with(stream);

    EXPECT_FALSE(status.ok);
    EXPECT_EQ(status.failure, ChainFailure::EnterRealmRejected);
    EXPECT_NE(status.detail.find("3002"), std::string::npos) << status.detail;
}

/// 帧取不到信封(不可解析)→ 坏帧,同样是 EnterRealmRejected。
TEST(WireEnterRealmRedeemerTest, MapsBadFrameToEnterRealmRejected) {
    const std::vector<std::byte> garbage{std::byte{0xFF}};
    CannedStream stream;
    stream.canned = framed(garbage);

    const auto status = redeem_with(stream);

    EXPECT_FALSE(status.ok);
    EXPECT_EQ(status.failure, ChainFailure::EnterRealmRejected);
    EXPECT_NE(status.detail.find("坏帧"), std::string::npos) << status.detail;
}

/// 信封解得出 1305、但帧体不是合法 EnterRealmAccepted → 同样是坏帧:
/// 消息号对不代表内容可信,不得当成功收下。
TEST(WireEnterRealmRedeemerTest, MapsUndecodableAcceptedToEnterRealmRejected) {
    ::realmmesh::protocol::common::v1::Envelope envelope;
    envelope.set_protocol_version(common::kEdgeProtocolVersion);
    envelope.set_message_id(
        static_cast<std::uint32_t>(edge_v1::MESSAGE_ID_S2C_ENTER_REALM_ACCEPTED));
    envelope.set_request_id(0);
    envelope.set_payload("\xFF");  // 非法 wire type:EnterRealmAccepted 解不出
    std::string bytes;
    ASSERT_TRUE(envelope.SerializeToString(&bytes));
    const auto* begin = reinterpret_cast<const std::byte*>(bytes.data());

    CannedStream stream;
    stream.canned = framed({begin, begin + bytes.size()});

    const auto status = redeem_with(stream);

    EXPECT_FALSE(status.ok);
    EXPECT_EQ(status.failure, ChainFailure::EnterRealmRejected);
    EXPECT_NE(status.detail.find("坏帧"), std::string::npos) << status.detail;
}

/// 对端无数据(超时/断开)→ EnterRealmRejected:不新增超时分型,请求仍然
/// 被写出(证明确实尝试过兑换)。
TEST(WireEnterRealmRedeemerTest, MapsTimeoutToEnterRealmRejected) {
    CannedStream stream;  // canned 空:读即刻判空

    const auto status = redeem_with(stream);

    EXPECT_FALSE(status.ok);
    EXPECT_EQ(status.failure, ChainFailure::EnterRealmRejected);
    EXPECT_TRUE(parse_enter_realm_request(stream.written).has_value())
        << status.detail;
}

}  // namespace
}  // namespace realm::client
