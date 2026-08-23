#pragma once

#include <cstdint>
#include <string>

namespace realm::network {

class TcpListener;

/// 解析后的对端地址,失败时 host 为空、port 为 0。
struct TcpPeerEndpoint {
    std::string host;
    std::uint16_t port{0};
};

class TcpSocket final {
public:
    ~TcpSocket();

    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;

    [[nodiscard]] int native_handle() const noexcept;
    [[nodiscard]] bool valid() const noexcept;
    /// 读取对端地址(getpeername);失败时 host 为空、port 为 0。
    [[nodiscard]] TcpPeerEndpoint peer_endpoint() const noexcept;

private:
    friend class TcpListener;

    explicit TcpSocket(int descriptor) noexcept;
    void close() noexcept;

    int descriptor_{-1};
};

}  // namespace realm::network
