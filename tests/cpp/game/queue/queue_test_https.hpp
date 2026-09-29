#pragma once

#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace realm::game::queue::test_https {

class Descriptor final {
public:
    explicit Descriptor(int value)
        : value_(value) {}
    ~Descriptor() {
        if (value_ >= 0) ::close(value_);
    }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    [[nodiscard]] int get() const noexcept { return value_; }

private:
    int value_;
};

struct SslContextDeleter {
    void operator()(SSL_CTX* value) const noexcept { SSL_CTX_free(value); }
};

struct SslDeleter {
    void operator()(SSL* value) const noexcept { SSL_free(value); }
};

[[nodiscard]] inline bool write_all(SSL* ssl, std::string_view bytes) {
    const auto* data =
        reinterpret_cast<const unsigned char*>(bytes.data());
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        std::size_t written = 0;
        if (SSL_write_ex(
                ssl, data + offset, bytes.size() - offset, &written) != 1) {
            return false;
        }
        offset += written;
    }
    return true;
}

/// 读完响应头后按 Content-Length 读取；服务端使用 keep-alive 时不会
/// 主动关闭连接。
[[nodiscard]] inline std::string read_response(SSL* ssl) {
    std::string response;
    std::array<char, 4096> buffer{};
    std::size_t header_end = std::string::npos;
    while (header_end == std::string::npos) {
        std::size_t received = 0;
        if (SSL_read_ex(
                ssl, buffer.data(), buffer.size(), &received) != 1) {
            return response;
        }
        response.append(buffer.data(), received);
        header_end = response.find("\r\n\r\n");
    }
    constexpr std::string_view needle = "Content-Length:";
    const auto length_field = response.find(needle);
    if (length_field == std::string::npos) return response;
    const auto value_start =
        response.find_first_not_of(" \t", length_field + needle.size());
    const auto value_end = response.find("\r\n", value_start);
    const auto content_length = static_cast<std::size_t>(std::stoul(
        response.substr(value_start, value_end - value_start)));
    const auto body_start = header_end + 4U;
    while (response.size() < body_start + content_length) {
        std::size_t received = 0;
        if (SSL_read_ex(
                ssl, buffer.data(), buffer.size(), &received) != 1) {
            break;
        }
        response.append(buffer.data(), received);
    }
    return response;
}

/// 一条 TLS 连接上完成一次请求-响应；连接或协议失败返回空。
[[nodiscard]] inline std::optional<std::string> https_exchange(
    std::uint16_t port, std::string_view request) {
    std::unique_ptr<SSL_CTX, SslContextDeleter> context(
        SSL_CTX_new(TLS_client_method()));
    if (context == nullptr) return std::nullopt;
    SSL_CTX_set_verify(context.get(), SSL_VERIFY_NONE, nullptr);
    std::unique_ptr<SSL, SslDeleter> ssl(SSL_new(context.get()));
    if (ssl == nullptr) return std::nullopt;
    static constexpr std::array<unsigned char, 9> alpn{
        8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
    if (SSL_set_alpn_protos(ssl.get(), alpn.data(), alpn.size()) != 0) {
        return std::nullopt;
    }

    const Descriptor socket(::socket(AF_INET, SOCK_STREAM, 0));
    if (socket.get() < 0) return std::nullopt;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(
            socket.get(),
            reinterpret_cast<sockaddr*>(&address),
            sizeof(address)) != 0 ||
        SSL_set_fd(ssl.get(), socket.get()) != 1 ||
        SSL_connect(ssl.get()) != 1 || !write_all(ssl.get(), request)) {
        return std::nullopt;
    }
    auto response = read_response(ssl.get());
    if (response.empty()) return std::nullopt;
    return response;
}

}  // namespace realm::game::queue::test_https
