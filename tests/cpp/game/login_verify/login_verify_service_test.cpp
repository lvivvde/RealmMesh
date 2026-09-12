/// 健全服集成测试(唯一动 socket 的缝):真实 TLS loopback,服务绑内核
/// 临时端口、std::jthread 驱动 tick;主线程自建 OpenSSL 客户端(先例:
/// http_server_test)走完整 HTTPS 请求,断言签发/JWKS/健康与拒绝档位。

#include "realmmesh/game/login_verify/login_verify_service.hpp"

#include "realmmesh/game/common/identity_token.hpp"
#include "realmmesh/game/common/json.hpp"
#include "realmmesh/test_support/temporary_directory.hpp"

#include <gtest/gtest.h>
#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace realm::game::login_verify {
namespace {

using common::JsonCodec;

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

using SslContextPtr = std::unique_ptr<SSL_CTX, SslContextDeleter>;
using SslPtr = std::unique_ptr<SSL, SslDeleter>;

void write_all(SSL* ssl, std::string_view bytes) {
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        std::size_t written = 0;
        ASSERT_EQ(SSL_write_ex(ssl, data + offset, bytes.size() - offset, &written), 1);
        offset += written;
    }
}

/// 读完响应头后按 Content-Length 读取(keep-alive 服务端不会主动关连接)。
[[nodiscard]] std::string read_response(SSL* ssl) {
    std::string response;
    char buffer[4096];
    std::size_t header_end = std::string::npos;
    while (header_end == std::string::npos) {
        std::size_t received = 0;
        if (SSL_read_ex(ssl, buffer, sizeof(buffer), &received) != 1) {
            return response;
        }
        response.append(buffer, received);
        header_end = response.find("\r\n\r\n");
    }
    const std::string needle = "Content-Length:";
    std::size_t length_field = response.find(needle);
    if (length_field == std::string::npos) return response;
    const std::size_t value_start =
        response.find_first_not_of(" \t", length_field + needle.size());
    const std::size_t value_end = response.find("\r\n", value_start);
    const std::size_t content_length =
        static_cast<std::size_t>(std::stoul(response.substr(
            value_start, value_end - value_start)));
    const std::size_t body_start = header_end + 4;
    while (response.size() < body_start + content_length) {
        std::size_t received = 0;
        if (SSL_read_ex(ssl, buffer, sizeof(buffer), &received) != 1) break;
        response.append(buffer, received);
    }
    return response;
}

/// 一条 TLS 连接上完成一次请求-响应;失败返回空(用例侧断言有值)。
[[nodiscard]] std::optional<std::string> https_exchange(
    std::uint16_t port, std::string_view request) {
    SslContextPtr context(SSL_CTX_new(TLS_client_method()));
    SSL_CTX_set_verify(context.get(), SSL_VERIFY_NONE, nullptr);
    SslPtr ssl(SSL_new(context.get()));
    // 服务端握手要求协商出 ALPN(http/1.1),不带即被回绝关闭。
    static constexpr std::array<unsigned char, 9> alpn{
        8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
    EXPECT_EQ(SSL_set_alpn_protos(ssl.get(), alpn.data(), alpn.size()), 0);

    const Descriptor socket(::socket(AF_INET, SOCK_STREAM, 0));
    EXPECT_GE(socket.get(), 0);
    if (socket.get() < 0) return std::nullopt;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const int connected = ::connect(
        socket.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address));
    EXPECT_EQ(connected, 0);
    if (connected != 0) return std::nullopt;
    const bool attached = SSL_set_fd(ssl.get(), socket.get()) == 1;
    EXPECT_TRUE(attached);
    if (!attached) return std::nullopt;
    if (SSL_connect(ssl.get()) != 1) {
        ADD_FAILURE() << "TLS handshake failed";
        return std::nullopt;
    }
    write_all(ssl.get(), request);
    return read_response(ssl.get());
}

[[nodiscard]] std::string post_request(
    std::string_view target, std::string_view body) {
    std::string request;
    request += "POST ";
    request += target;
    request += " HTTP/1.1\r\n";
    request += "Host: localhost\r\n";
    request += "Content-Type: application/json\r\n";
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    request += "Connection: close\r\n";
    request += "\r\n";
    request += body;
    return request;
}

[[nodiscard]] std::string get_request(std::string_view target) {
    std::string request;
    request += "GET ";
    request += target;
    request += " HTTP/1.1\r\n";
    request += "Host: localhost\r\n";
    request += "Connection: close\r\n";
    request += "\r\n";
    return request;
}

[[nodiscard]] std::string_view body_of(const std::string& response) {
    const auto header_end = response.find("\r\n\r\n");
    return header_end == std::string::npos
               ? std::string_view{}
               : std::string_view{response}.substr(header_end + 4);
}

[[nodiscard]] int status_of(const std::string& response) {
    return std::stoi(response.substr(response.find(' ') + 1, 3));
}

class LoginVerifyServiceTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        ASSERT_EQ(
            ::setenv(
                "REALMMESH_IDENTITY_KEY_SEED",
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                1),
            0);
    }

    static void TearDownTestSuite() {
        static_cast<void>(::unsetenv("REALMMESH_IDENTITY_KEY_SEED"));
    }

    void SetUp() override {
        const auto accounts = directory_.path() / "accounts.lua";
        std::ofstream stream(accounts, std::ios::binary);
        stream << R"lua(
return {
    accounts = {
        { account = "pinned", credential = "dev", whitelisted = true, account_id = 4242 },
    },
}
)lua";
        stream.close();

        LoginVerifyConfig config;
        config.listen_address = "127.0.0.1";
        config.listen_port = 0;
        config.kid = std::string{kKid};
        config.accounts_file = accounts;
        config.tls = network::TransportConfig::TlsServerIdentity{
            .certificate_chain_file = REALMMESH_TEST_TLS_CERTIFICATE,
            .private_key_file = REALMMESH_TEST_TLS_PRIVATE_KEY,
            .alpn = "http/1.1"};
        service_ = std::make_unique<LoginVerifyService>(std::move(config));
        service_->start();
        driver_ = std::jthread([this] {
            while (!stopping_.load()) {
                service_->tick();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        port_ = service_->local_endpoints().at(0).port;
    }

    void TearDown() override {
        stopping_.store(true);
        if (driver_.joinable()) driver_.join();
        service_->stop();
        service_.reset();
    }

    [[nodiscard]] common::IdentityTokenCodec local_codec() const {
        return common::IdentityTokenCodec(
            common::parse_identity_seed_hex(
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
            std::string{kKid});
    }

    static constexpr std::string_view kKid = "login-verify-test-1";

    test_support::TemporaryDirectory directory_{"login-verify-service-test-"};
    std::unique_ptr<LoginVerifyService> service_;
    std::jthread driver_;
    std::atomic_bool stopping_{false};
    std::uint16_t port_{0};
};

TEST_F(LoginVerifyServiceTest, VerifiesAccountOverTlsAndIssuesToken) {
    const auto exchanged = https_exchange(
        port_,
        post_request("/v1/login/verify", R"({"account":"pinned","credential":"dev"})"));
    ASSERT_TRUE(exchanged.has_value());
    const std::string& response = *exchanged;
    EXPECT_EQ(status_of(response), 200);
    const auto payload = JsonCodec::decode(body_of(response));
    ASSERT_TRUE(payload.has_value());
    const auto& token =
        std::get<std::string>(payload->at("identity_token"));
    EXPECT_EQ(std::get<std::string>(payload->at("account_id")), "4242");

    const auto claims = local_codec().validate(
        token, identity_token_issuer, std::chrono::system_clock::now());
    ASSERT_TRUE(claims.has_value());
    EXPECT_EQ(claims->account_id, 4242ULL);
}

TEST_F(LoginVerifyServiceTest, WrongCredentialIsRejectedOverTls) {
    const auto exchanged = https_exchange(
        port_,
        post_request("/v1/login/verify", R"({"account":"pinned","credential":"x"})"));
    ASSERT_TRUE(exchanged.has_value());
    const std::string& response = *exchanged;
    EXPECT_EQ(status_of(response), 401);
    const auto payload = JsonCodec::decode(body_of(response));
    ASSERT_TRUE(payload.has_value());
    EXPECT_EQ(std::get<std::int64_t>(payload->at("code")), 1001);
}

TEST_F(LoginVerifyServiceTest, ServesJwksOverTls) {
    const auto exchanged = https_exchange(port_, get_request("/.well-known/jwks.json"));
    ASSERT_TRUE(exchanged.has_value());
    const std::string& response = *exchanged;
    EXPECT_EQ(status_of(response), 200);
    EXPECT_EQ(body_of(response), local_codec().jwks());
    // JWKS 的 kid 与服务配置一致(即自签 token 使用的 kid)。
    EXPECT_NE(body_of(response).find(std::string{kKid}), std::string_view::npos);
}

TEST_F(LoginVerifyServiceTest, ServesHealthzOverTls) {
    const auto exchanged = https_exchange(port_, get_request("/healthz"));
    ASSERT_TRUE(exchanged.has_value());
    const std::string& response = *exchanged;
    EXPECT_EQ(status_of(response), 200);
    EXPECT_EQ(body_of(response), "ok");
}

}  // namespace
}  // namespace realm::game::login_verify
