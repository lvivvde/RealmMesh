#pragma once

#include "realmmesh/network/tcp/tcp_socket.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace realm::network {

class TcpListener final {
public:
    TcpListener(std::string_view address, std::uint16_t port, int backlog = 128);
    ~TcpListener();

    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    TcpListener(TcpListener&& other) noexcept;
    TcpListener& operator=(TcpListener&& other) noexcept;

    [[nodiscard]] int native_handle() const noexcept;
    [[nodiscard]] std::uint16_t local_port() const noexcept;
    [[nodiscard]] std::optional<TcpSocket> accept();

private:
    void close() noexcept;

    int descriptor_{-1};
    std::uint16_t local_port_{0};
};

}  // namespace realm::network
