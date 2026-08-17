#pragma once

namespace realm::network {

class TcpListener;

class TcpSocket final {
public:
    ~TcpSocket();

    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;

    [[nodiscard]] int native_handle() const noexcept;
    [[nodiscard]] bool valid() const noexcept;

private:
    friend class TcpListener;

    explicit TcpSocket(int descriptor) noexcept;
    void close() noexcept;

    int descriptor_{-1};
};

}  // namespace realm::network
