#include "realmmesh/game/common/edge_protocol.hpp"
#include "realmmesh/network/codec/length_field_codec.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace realm::game::common {
namespace {

class ChildProcess final {
public:
    ChildProcess(
        const char* executable,
        const std::filesystem::path& working_directory,
        const std::filesystem::path& config_path) {
        pid_ = ::fork();
        if (pid_ < 0) throw std::runtime_error("fork failed");
        if (pid_ == 0) {
            if (::chdir(working_directory.c_str()) != 0) _exit(126);
            ::execl(
                executable,
                executable,
                "--config",
                config_path.c_str(),
                static_cast<char*>(nullptr));
            _exit(127);
        }
    }
    ~ChildProcess() { stop(); }
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    void stop() noexcept {
        if (pid_ <= 0) return;
        static_cast<void>(::kill(pid_, SIGINT));
        int status = 0;
        static_cast<void>(::waitpid(pid_, &status, 0));
        pid_ = -1;
    }

private:
    pid_t pid_{-1};
};

class TemporaryDirectory final {
public:
    TemporaryDirectory()
        : path_(
              std::filesystem::temp_directory_path() /
              ("realmmesh-three-stage-" +
               std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count()))) {
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

[[nodiscard]] std::optional<std::string> correlation_for_event(
    std::string_view contents,
    std::string_view event_name) {
    std::istringstream input{std::string(contents)};
    std::string line;
    while (std::getline(input, line)) {
        const auto event = nlohmann::json::parse(line);
        if (event.value("event_name", "") == event_name &&
            event.contains("correlation_id")) {
            return event.at("correlation_id").get<std::string>();
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::uint16_t unused_tcp_port() {
    const int descriptor = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (descriptor < 0) throw std::runtime_error("socket failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(
            descriptor,
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) != 0) {
        ::close(descriptor);
        throw std::runtime_error("ephemeral port bind failed");
    }
    socklen_t size = sizeof(address);
    if (::getsockname(
            descriptor,
            reinterpret_cast<sockaddr*>(&address),
            &size) != 0) {
        ::close(descriptor);
        throw std::runtime_error("getsockname failed");
    }
    ::close(descriptor);
    return ntohs(address.sin_port);
}

void replace_all(
    std::string& contents,
    std::string_view original,
    std::string_view replacement) {
    std::size_t position = 0;
    while ((position = contents.find(original, position)) != std::string::npos) {
        contents.replace(position, original.size(), replacement);
        position += replacement.size();
    }
}

[[nodiscard]] std::filesystem::path write_test_config(
    const std::filesystem::path& directory,
    std::string_view name,
    std::initializer_list<std::pair<std::string_view, std::string>> replacements) {
    const auto source = std::filesystem::path(REALMMESH_TEST_SOURCE_DIR) /
        "lua/config/services" / (std::string(name) + ".lua");
    auto contents = read_file(source);
    if (contents.empty()) throw std::runtime_error("test config is empty");
    for (const auto& [original, replacement] : replacements) {
        replace_all(contents, original, replacement);
    }
    const auto destination = directory / (std::string(name) + ".lua");
    std::ofstream output(destination);
    output << contents;
    if (!output) throw std::runtime_error("failed to write test config");
    return destination;
}

struct ContextDeleter {
    void operator()(SSL_CTX* value) const noexcept { SSL_CTX_free(value); }
};
struct SslDeleter {
    void operator()(SSL* value) const noexcept { SSL_free(value); }
};

class TlsSocket final {
public:
    TlsSocket(int descriptor, SSL_CTX* context, SSL* ssl)
        : descriptor_(descriptor), context_(context), ssl_(ssl) {}
    ~TlsSocket() {
        ssl_.reset();
        context_.reset();
        if (descriptor_ >= 0) ::close(descriptor_);
    }
    TlsSocket(const TlsSocket&) = delete;
    TlsSocket& operator=(const TlsSocket&) = delete;
    TlsSocket(TlsSocket&& other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1)),
          context_(std::move(other.context_)),
          ssl_(std::move(other.ssl_)) {}
    [[nodiscard]] SSL* ssl() const noexcept { return ssl_.get(); }

private:
    int descriptor_;
    std::unique_ptr<SSL_CTX, ContextDeleter> context_;
    std::unique_ptr<SSL, SslDeleter> ssl_;
};

std::optional<TlsSocket> try_connect(std::uint16_t port) {
    const int descriptor = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (descriptor < 0) return std::nullopt;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(descriptor, reinterpret_cast<const sockaddr*>(&address),
                  sizeof(address)) < 0) {
        ::close(descriptor);
        return std::nullopt;
    }
    timeval timeout{};
    timeout.tv_sec = 2;
    static_cast<void>(::setsockopt(
        descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    auto* context = SSL_CTX_new(TLS_client_method());
    if (context == nullptr ||
        SSL_CTX_load_verify_locations(
            context, REALMMESH_TEST_TLS_CERTIFICATE, nullptr) != 1) {
        SSL_CTX_free(context);
        ::close(descriptor);
        return std::nullopt;
    }
    SSL_CTX_set_verify(context, SSL_VERIFY_PEER, nullptr);
    auto* ssl = SSL_new(context);
    const std::array<unsigned char, 17> alpn{
        16, 'r', 'e', 'a', 'l', 'm', 'm', 'e', 's', 'h', '-', 'e', 'd', 'g', 'e', '/', '1'};
    if (ssl == nullptr || SSL_set_fd(ssl, descriptor) != 1 ||
        SSL_set_tlsext_host_name(ssl, "localhost") != 1 ||
        SSL_set1_host(ssl, "localhost") != 1 ||
        SSL_set_alpn_protos(ssl, alpn.data(), alpn.size()) != 0 ||
        SSL_connect(ssl) != 1) {
        SSL_free(ssl);
        SSL_CTX_free(context);
        ::close(descriptor);
        return std::nullopt;
    }
    return TlsSocket(descriptor, context, ssl);
}

TlsSocket connect_when_ready(std::uint16_t port) {
    using namespace std::chrono_literals;
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (auto socket = try_connect(port); socket.has_value()) {
            return std::move(*socket);
        }
        std::this_thread::sleep_for(10ms);
    }
    throw std::runtime_error("service did not open expected port");
}

void send_all(SSL* ssl, std::span<const std::byte> bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        std::size_t sent = 0;
        if (SSL_write_ex(
                ssl, bytes.data() + offset, bytes.size() - offset, &sent) != 1) {
            throw std::runtime_error("TLS send failed");
        }
        offset += sent;
    }
}

std::vector<std::byte> receive_exactly(SSL* ssl, std::size_t size) {
    std::vector<std::byte> result(size);
    std::size_t offset = 0;
    while (offset < size) {
        std::size_t received = 0;
        if (SSL_read_ex(
                ssl,
                result.data() + offset,
                result.size() - offset,
                &received) != 1) {
            throw std::runtime_error("TLS receive failed");
        }
        offset += received;
    }
    return result;
}

void send_message(TlsSocket& socket, std::span<const std::byte> payload) {
    const network::LengthFieldCodec codec(65536);
    send_all(socket.ssl(), codec.encode(payload));
}

std::vector<std::byte> receive_message(TlsSocket& socket) {
    const auto header = receive_exactly(socket.ssl(), 4);
    std::uint32_t size = 0;
    for (const auto value : header) {
        size = (size << 8U) | std::to_integer<std::uint8_t>(value);
    }
    if (size > 65536) throw std::runtime_error("response frame too large");
    return receive_exactly(socket.ssl(), size);
}

TEST(ThreeStageFlowTest, LogsInSelectsACharacterAndEntersTheGateway) {
    const TemporaryDirectory working_directory;
    const auto login_port = unused_tcp_port();
    const auto realm_port = unused_tcp_port();
    const auto gateway_port = unused_tcp_port();
    const auto login_config = write_test_config(
        working_directory.path(),
        "login",
        {{"7000", std::to_string(login_port)},
         {"7100", std::to_string(realm_port)},
         {"metrics_port = 9101", "metrics_port = 0"},
         {"console = true", "console = false"}});
    const auto realm_config = write_test_config(
        working_directory.path(),
        "realm",
        {{"7100", std::to_string(realm_port)},
         {"8000", std::to_string(gateway_port)},
         {"metrics_port = 9102", "metrics_port = 0"},
         {"console = true", "console = false"}});
    const auto gateway_config = write_test_config(
        working_directory.path(),
        "gateway",
        {{"8000", std::to_string(gateway_port)},
         {"metrics_port = 9103", "metrics_port = 0"},
         {"console = true", "console = false"}});
    constexpr auto key =
        "0102030405060708090a0b0c0d0e0f10"
        "1112131415161718191a1b1c1d1e1f20";
    ASSERT_EQ(::setenv("REALMMESH_SESSION_TICKET_KEY", key, 1), 0);
    ASSERT_EQ(::setenv(
        "REALMMESH_TLS_CERTIFICATE_FILE",
        REALMMESH_TEST_TLS_CERTIFICATE,
        1), 0);
    ASSERT_EQ(::setenv(
        "REALMMESH_TLS_PRIVATE_KEY_FILE",
        REALMMESH_TEST_TLS_PRIVATE_KEY,
        1), 0);

    ChildProcess gateway(
        REALMMESH_GATEWAY_EXECUTABLE,
        working_directory.path(),
        gateway_config);
    ChildProcess realm(
        REALMMESH_REALM_EXECUTABLE,
        working_directory.path(),
        realm_config);
    ChildProcess login(
        REALMMESH_LOGIN_EXECUTABLE,
        working_directory.path(),
        login_config);

    auto login_socket = connect_when_ready(login_port);
    LoginRequest login_request;
    login_request.set_account("alice");
    login_request.set_credential("dev");
    send_message(login_socket, encode(login_request, 1));
    const auto login_wire = receive_message(login_socket);
    EXPECT_EQ(edge_request_id(login_wire), 1);
    const auto login_response = decode_login_succeeded(login_wire);
    ASSERT_TRUE(login_response.has_value());
    ASSERT_EQ(login_response->realm_endpoints_size(), 1);
    EXPECT_EQ(login_response->realm_endpoints(0).port(), realm_port);

    auto realm_socket = connect_when_ready(
        static_cast<std::uint16_t>(login_response->realm_endpoints(0).port()));
    RealmAuthenticate authenticate;
    authenticate.set_login_ticket(login_response->login_ticket());
    send_message(realm_socket, encode(authenticate, 2));
    const auto character_wire = receive_message(realm_socket);
    EXPECT_EQ(edge_request_id(character_wire), 2);
    const auto characters = decode_character_list(character_wire);
    ASSERT_TRUE(characters.has_value());
    ASSERT_EQ(characters->characters_size(), 1);

    SelectCharacter select_character;
    select_character.set_character_id(characters->characters(0).id());
    send_message(realm_socket, encode(select_character, 3));
    const auto enter_wire = receive_message(realm_socket);
    EXPECT_EQ(edge_request_id(enter_wire), 3);
    const auto enter = decode_enter_game_issued(enter_wire);
    ASSERT_TRUE(enter.has_value());
    ASSERT_EQ(enter->gateway_endpoints_size(), 2);
    const auto& tcp_gateway = enter->gateway_endpoints(1);
    EXPECT_EQ(tcp_gateway.protocol(),
              ::realmmesh::protocol::edge::v1::TRANSPORT_PROTOCOL_TLS_TCP);
    EXPECT_EQ(tcp_gateway.port(), gateway_port);

    auto gateway_socket = connect_when_ready(
        static_cast<std::uint16_t>(tcp_gateway.port()));
    EnterGame enter_game;
    enter_game.set_enter_game_ticket(enter->enter_game_ticket());
    send_message(gateway_socket, encode(enter_game, 4));
    const auto accepted_wire = receive_message(gateway_socket);
    EXPECT_EQ(edge_request_id(accepted_wire), 4);
    const auto accepted = decode_enter_game_accepted(accepted_wire);
    ASSERT_TRUE(accepted.has_value());
    EXPECT_EQ(accepted->account_id(), login_response->account_id());
    EXPECT_EQ(accepted->character_id(), characters->characters(0).id());

    login.stop();
    realm.stop();
    gateway.stop();

    const auto login_log = read_file(
        working_directory.path() /
        ".runtime/logs/login/login-dev-01.jsonl");
    const auto realm_log = read_file(
        working_directory.path() /
        ".runtime/logs/realm/realm-dev-01.jsonl");
    const auto gateway_log = read_file(
        working_directory.path() /
        ".runtime/logs/gateway/gateway-dev-01.jsonl");
    EXPECT_NE(login_log.find("\"event_name\":\"service_started\""),
              std::string::npos);
    EXPECT_NE(login_log.find("\"event_name\":\"service_stopped\""),
              std::string::npos);
    EXPECT_NE(realm_log.find("\"event_name\":\"service_started\""),
              std::string::npos);
    EXPECT_NE(gateway_log.find("\"event_name\":\"service_started\""),
              std::string::npos);
    const auto login_correlation = correlation_for_event(
        login_log, "player_session_established");
    const auto realm_correlation = correlation_for_event(
        realm_log, "player_session_established");
    const auto gateway_correlation = correlation_for_event(
        gateway_log, "player_session_established");
    ASSERT_TRUE(login_correlation.has_value());
    ASSERT_TRUE(realm_correlation.has_value());
    ASSERT_TRUE(gateway_correlation.has_value());
    EXPECT_EQ(*realm_correlation, *login_correlation);
    EXPECT_EQ(*gateway_correlation, *login_correlation);
    static_cast<void>(::unsetenv("REALMMESH_SESSION_TICKET_KEY"));
    static_cast<void>(::unsetenv("REALMMESH_TLS_CERTIFICATE_FILE"));
    static_cast<void>(::unsetenv("REALMMESH_TLS_PRIVATE_KEY_FILE"));
}

}  // namespace
}  // namespace realm::game::common
