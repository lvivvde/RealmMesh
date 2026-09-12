#include "realmmesh/game/common/edge_protocol.hpp"
#include "realmmesh/network/codec/length_field_codec.hpp"
#include "realmmesh/network/tcp/tcp_listener.hpp"

#include <gtest/gtest.h>
#include <openssl/ssl.h>
#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
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
        const char* executable, const std::filesystem::path& config_root) {
        pid_ = ::fork();
        if (pid_ < 0) throw std::runtime_error("fork failed");
        if (pid_ == 0) {
            ::execl(
                executable,
                executable,
                "--config",
                config_root.c_str(),
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

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

[[nodiscard]] std::uint16_t environment_port(
    const char* name, std::uint16_t fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr) return fallback;
    const auto parsed = std::stoul(value);
    if (parsed == 0 || parsed > 65535) {
        throw std::invalid_argument(std::string(name) + " is not a valid port");
    }
    return static_cast<std::uint16_t>(parsed);
}

[[nodiscard]] bool write_file(
    const std::filesystem::path& path, std::string_view contents) {
    std::ofstream output(path, std::ios::trunc);
    output << contents;
    return static_cast<bool>(output);
}

[[nodiscard]] std::string replace_all(
    std::string contents, std::string_view from, std::string_view to) {
    for (auto position = contents.find(from); position != std::string::npos;
         position = contents.find(from, position + to.size())) {
        contents.replace(position, from.size(), to);
    }
    return contents;
}

/// 同时打开 count 个监听套接字后再取端口,保证返回的端口互不相同:逐个
/// 取端口时操作系统可能把刚释放的同一个临时端口再分回来。
[[nodiscard]] std::vector<std::uint16_t> unused_tcp_ports(std::size_t count) {
    std::vector<std::unique_ptr<network::TcpListener>> listeners;
    std::vector<std::uint16_t> ports;
    listeners.reserve(count);
    ports.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        listeners.push_back(
            std::make_unique<network::TcpListener>("127.0.0.1", 0));
        ports.push_back(listeners.back()->local_port());
    }
    return ports;
}

/// 把服务配置里的固定端口改写成一组空闲端口。login → realm → gateway 的
/// downstream 指向必须同步改写,否则依赖探活会连到错误的服务。
void use_free_ports(
    const std::filesystem::path& root,
    std::uint16_t login_port,
    std::uint16_t realm_port,
    std::uint16_t gateway_port) {
    const auto login_path = root / "services" / "login.lua";
    auto login = read_file(login_path);
    login = replace_all(login, "listen_port = 7000",
                        "listen_port = " + std::to_string(login_port));
    login = replace_all(login, "downstream_port = 7100",
                        "downstream_port = " + std::to_string(realm_port));
    login = replace_all(login, "metrics_port = 9101", "metrics_port = 0");
    if (!write_file(login_path, login)) {
        throw std::runtime_error("cannot rewrite login.lua");
    }

    const auto realm_path = root / "services" / "realm.lua";
    auto realm = read_file(realm_path);
    realm = replace_all(realm, "listen_port = 7100",
                        "listen_port = " + std::to_string(realm_port));
    realm = replace_all(realm, "downstream_port = 8000",
                        "downstream_port = " + std::to_string(gateway_port));
    realm = replace_all(realm, "metrics_port = 9102", "metrics_port = 0");
    if (!write_file(realm_path, realm)) {
        throw std::runtime_error("cannot rewrite realm.lua");
    }

    // gateway.lua 里 listen_port = 8000 出现两次(TCP 与其伴随传输),
    // 两处都要指向同一个空闲端口。
    const auto gateway_path = root / "services" / "gateway.lua";
    auto gateway = read_file(gateway_path);
    gateway = replace_all(gateway, "listen_port = 8000",
                          "listen_port = " + std::to_string(gateway_port));
    gateway = replace_all(gateway, "metrics_port = 9103", "metrics_port = 0");
    if (!write_file(gateway_path, gateway)) {
        throw std::runtime_error("cannot rewrite gateway.lua");
    }
}

/// 自起进程的用例使用的临时配置树:拷贝权威 configs/ 后按需改写端口,
/// 析构时递归清理。这样既不占用开发机上的固定端口(macOS 的 7000 常被
/// AirPlay Receiver 占用),也不把日志写进源码树的 configs/logs/。
class ScratchConfigRoot final {
public:
    explicit ScratchConfigRoot(const std::filesystem::path& source) {
        path_ = std::filesystem::temp_directory_path() /
                ("three-stage-flow-" +
                 std::to_string(static_cast<long long>(::getpid())));
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        std::filesystem::create_directories(path_, error);
        if (error) throw std::runtime_error("cannot create config scratch dir");
        std::filesystem::copy(
            source,
            path_,
            std::filesystem::copy_options::recursive |
                std::filesystem::copy_options::overwrite_existing,
            error);
        if (error) throw std::runtime_error("cannot copy config tree");
    }
    ~ScratchConfigRoot() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    ScratchConfigRoot(const ScratchConfigRoot&) = delete;
    ScratchConfigRoot& operator=(const ScratchConfigRoot&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] std::optional<std::string> correlation_for_event(
    std::string_view contents, std::string_view event_name) {
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

/// 统计日志内容中某事件名的出现条数(每行一条 JSON 事件)。
[[nodiscard]] std::size_t count_events(
    const std::string& contents, std::string_view event_name) {
    const std::string needle =
        "\"event_name\":\"" + std::string(event_name) + "\"";
    std::size_t count = 0;
    for (std::size_t position = contents.find(needle);
         position != std::string::npos;
         position = contents.find(needle, position + needle.size())) {
        ++count;
    }
    return count;
}

void wait_for_event(
    const std::filesystem::path& path, std::string_view event_name) {
    using namespace std::chrono_literals;
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (count_events(read_file(path), event_name) != 0) return;
        std::this_thread::sleep_for(10ms);
    }
    throw std::runtime_error(
        "event was not written before timeout: " + std::string(event_name));
}

/// 分层加载器生成的日志文件:<config_root>/logs/<service>/<service>-<instance>.jsonl。
[[nodiscard]] std::filesystem::path service_log_path(
    const std::filesystem::path& config_root, std::string_view service) {
    const std::string name(service);
    return config_root / "logs" / name / (name + "-" + name + "-dev-01.jsonl");
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
        : descriptor_(descriptor),
          context_(context),
          ssl_(ssl) {}
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
    const int descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
    if (descriptor < 0) return std::nullopt;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(
            descriptor,
            reinterpret_cast<const sockaddr*>(&address),
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
        16,
        'r',
        'e',
        'a',
        'l',
        'm',
        'm',
        'e',
        's',
        'h',
        '-',
        'e',
        'd',
        'g',
        'e',
        '/',
        '1'};
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

/// TCP 探活:裸 connect 确认端口已被监听,用于等待 realm_mesh 全链路就绪。
void wait_for_tcp_ready(std::uint16_t port) {
    using namespace std::chrono_literals;
    for (int attempt = 0; attempt < 500; ++attempt) {
        const int descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
        if (descriptor >= 0) {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(port);
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (::connect(
                    descriptor,
                    reinterpret_cast<const sockaddr*>(&address),
                    sizeof(address)) == 0) {
                ::close(descriptor);
                return;
            }
            ::close(descriptor);
        }
        std::this_thread::sleep_for(10ms);
    }
    throw std::runtime_error("realm_mesh did not open expected port");
}

void send_all(SSL* ssl, std::span<const std::byte> bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        std::size_t sent = 0;
        if (SSL_write_ex(
                ssl, bytes.data() + offset, bytes.size() - offset, &sent) !=
            1) {
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
    const bool external_service_group =
        std::getenv("REALMMESH_THREE_STAGE_EXTERNAL") != nullptr;
    const char* external_config_root =
        std::getenv("REALMMESH_THREE_STAGE_CONFIG_ROOT");

    // 自起进程时用临时配置树 + 当空闲端口:固定端口不再成为测试前提
    // (7000 在 macOS 上常被 AirPlay Receiver 占用),日志也不再写进源码树。
    std::optional<ScratchConfigRoot> scratch;
    std::filesystem::path config_root;
    std::uint16_t login_port = 0;
    std::uint16_t realm_port = 0;
    std::uint16_t gateway_port = 0;
    if (external_service_group) {
        login_port = environment_port("REALMMESH_THREE_STAGE_LOGIN_PORT", 7000);
        realm_port = environment_port("REALMMESH_THREE_STAGE_REALM_PORT", 7100);
        gateway_port =
            environment_port("REALMMESH_THREE_STAGE_GATEWAY_PORT", 8000);
        config_root = external_config_root == nullptr
                          ? std::filesystem::path(REALMMESH_TEST_SOURCE_DIR) /
                                "configs"
                          : std::filesystem::path(external_config_root);
    } else {
        scratch.emplace(
            std::filesystem::path(REALMMESH_TEST_SOURCE_DIR) / "configs");
        config_root = scratch->path();
        const auto ports = unused_tcp_ports(3);
        login_port = ports.at(0);
        realm_port = ports.at(1);
        gateway_port = ports.at(2);
        use_free_ports(config_root, login_port, realm_port, gateway_port);
        // 自行启动进程时先清理上次日志;外部服务组已经打开当前日志文件,
        // 此时 unlink 会让后续事件只写入已删除的 inode。
        for (const std::string_view service : {"login", "realm", "gateway"}) {
            std::error_code error;
            std::filesystem::remove(
                service_log_path(config_root, service), error);
        }
    }
    constexpr auto key =
        "0102030405060708090a0b0c0d0e0f10"
        "1112131415161718191a1b1c1d1e1f20";
    ASSERT_EQ(::setenv("REALMMESH_SESSION_TICKET_KEY", key, 1), 0);
    ASSERT_EQ(
        ::setenv(
            "REALMMESH_IDENTITY_KEY_SEED",
            "00112233445566778899aabbccddeeff"
            "00112233445566778899aabbccddeeff",
            1),
        0);
    // queue 节点自 mode-1 拓扑起随组启动,号牌签名种子与身份种子同批注入。
    ASSERT_EQ(
        ::setenv(
            "REALMMESH_QUEUE_KEY_SEED",
            "00112233445566778899aabbccddeeff"
            "00112233445566778899aabbccddeeff",
            1),
        0);
    ASSERT_EQ(
        ::setenv(
            "REALMMESH_TLS_CERTIFICATE_FILE",
            REALMMESH_TEST_TLS_CERTIFICATE,
            1),
        0);
    ASSERT_EQ(
        ::setenv(
            "REALMMESH_TLS_PRIVATE_KEY_FILE",
            REALMMESH_TEST_TLS_PRIVATE_KEY,
            1),
        0);

    std::unique_ptr<ChildProcess> mesh;
    if (!external_service_group) {
        mesh = std::make_unique<ChildProcess>(
            REALMMESH_MESH_EXECUTABLE, config_root);
    }
    wait_for_tcp_ready(login_port);
    wait_for_tcp_ready(realm_port);
    wait_for_tcp_ready(gateway_port);

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

    // Login 票据一次性:同一票据在第二条连接上再次兑换必须被拒(2001)并断开。
    auto replay_socket = connect_when_ready(
        static_cast<std::uint16_t>(login_response->realm_endpoints(0).port()));
    send_message(replay_socket, encode(authenticate, 6));
    const auto replay_wire = receive_message(replay_socket);
    EXPECT_EQ(edge_request_id(replay_wire), 6);
    const auto replay_error = decode_edge_error(replay_wire);
    ASSERT_TRUE(replay_error.has_value());
    EXPECT_EQ(replay_error->code(), 2001);

    HeartbeatRequest heartbeat;
    send_message(realm_socket, encode(heartbeat, 5));
    const auto heartbeat_wire = receive_message(realm_socket);
    EXPECT_EQ(edge_request_id(heartbeat_wire), 5);
    EXPECT_TRUE(decode_heartbeat_response(heartbeat_wire).has_value());

    SelectCharacter select_character;
    select_character.set_character_id(characters->characters(0).id());
    send_message(realm_socket, encode(select_character, 3));
    const auto enter_wire = receive_message(realm_socket);
    EXPECT_EQ(edge_request_id(enter_wire), 3);
    const auto enter = decode_enter_game_issued(enter_wire);
    ASSERT_TRUE(enter.has_value());
    ASSERT_EQ(enter->gateway_endpoints_size(), 2);
    const auto& tcp_gateway = enter->gateway_endpoints(1);
    EXPECT_EQ(
        tcp_gateway.protocol(),
        ::realmmesh::protocol::edge::v1::TRANSPORT_PROTOCOL_TLS_TCP);
    EXPECT_EQ(tcp_gateway.port(), gateway_port);

    auto gateway_socket =
        connect_when_ready(static_cast<std::uint16_t>(tcp_gateway.port()));
    EnterGame enter_game;
    enter_game.set_enter_game_ticket(enter->enter_game_ticket());
    send_message(gateway_socket, encode(enter_game, 4));
    const auto accepted_wire = receive_message(gateway_socket);
    EXPECT_EQ(edge_request_id(accepted_wire), 4);
    const auto accepted = decode_enter_game_accepted(accepted_wire);
    ASSERT_TRUE(accepted.has_value());
    EXPECT_EQ(accepted->account_id(), login_response->account_id());
    EXPECT_EQ(accepted->character_id(), characters->characters(0).id());

    if (mesh != nullptr) mesh->stop();

    if (external_service_group) {
        for (const std::string_view service : {"login", "realm", "gateway"}) {
            wait_for_event(
                service_log_path(config_root, service),
                "player_session_established");
        }
    }

    const auto login_log = read_file(service_log_path(config_root, "login"));
    const auto realm_log = read_file(service_log_path(config_root, "realm"));
    const auto gateway_log =
        read_file(service_log_path(config_root, "gateway"));
    // 关停幂等:MeshHost::shutdown() 与 ServiceHost 析构双停只生效首次,
    // 每服务恰好一条 service_started 配对一条 service_stopped。
    EXPECT_EQ(count_events(login_log, "service_started"), 1);
    EXPECT_EQ(
        count_events(login_log, "service_stopped"),
        external_service_group ? 0 : 1);
    EXPECT_EQ(count_events(realm_log, "service_started"), 1);
    EXPECT_EQ(
        count_events(realm_log, "service_stopped"),
        external_service_group ? 0 : 1);
    EXPECT_EQ(count_events(gateway_log, "service_started"), 1);
    EXPECT_EQ(
        count_events(gateway_log, "service_stopped"),
        external_service_group ? 0 : 1);
    const auto login_correlation =
        correlation_for_event(login_log, "player_session_established");
    const auto realm_correlation =
        correlation_for_event(realm_log, "player_session_established");
    const auto gateway_correlation =
        correlation_for_event(gateway_log, "player_session_established");
    ASSERT_TRUE(login_correlation.has_value());
    ASSERT_TRUE(realm_correlation.has_value());
    ASSERT_TRUE(gateway_correlation.has_value());
    EXPECT_EQ(*realm_correlation, *login_correlation);
    EXPECT_EQ(*gateway_correlation, *login_correlation);
    static_cast<void>(::unsetenv("REALMMESH_SESSION_TICKET_KEY"));
    static_cast<void>(::unsetenv("REALMMESH_IDENTITY_KEY_SEED"));
    static_cast<void>(::unsetenv("REALMMESH_TLS_CERTIFICATE_FILE"));
    static_cast<void>(::unsetenv("REALMMESH_TLS_PRIVATE_KEY_FILE"));
}

}  // namespace
}  // namespace realm::game::common
