#include "realmmesh/service_host/mesh_host.hpp"

#include "realmmesh/game/gateway/gateway_runtime.hpp"
#include "realmmesh/network/tcp/tcp_listener.hpp"
#include "realmmesh/test_support/temporary_directory.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace realm::service_host {
namespace {

/// TLS 证书/会话票据环境变量守护:指向 CMake 预生成的自签证书与固定
/// 测试密钥,析构时还原。
class ScopedTlsEnvironment final {
public:
    ScopedTlsEnvironment() {
        EXPECT_EQ(
            ::setenv(
                "REALMMESH_TLS_CERTIFICATE_FILE",
                REALMMESH_TEST_TLS_CERTIFICATE,
                1),
            0);
        EXPECT_EQ(
            ::setenv(
                "REALMMESH_TLS_PRIVATE_KEY_FILE",
                REALMMESH_TEST_TLS_PRIVATE_KEY,
                1),
            0);
        EXPECT_EQ(
            ::setenv(
                "REALMMESH_SESSION_TICKET_KEY",
                "0102030405060708090a0b0c0d0e0f10"
                "1112131415161718191a1b1c1d1e1f20",
                1),
            0);
        EXPECT_EQ(
            ::setenv(
                "REALMMESH_IDENTITY_KEY_SEED",
                "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
                1),
            0);
        EXPECT_EQ(
            ::setenv(
                "REALMMESH_QUEUE_KEY_SEED",
                "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
                1),
            0);
    }
    ~ScopedTlsEnvironment() {
        static_cast<void>(::unsetenv("REALMMESH_TLS_CERTIFICATE_FILE"));
        static_cast<void>(::unsetenv("REALMMESH_TLS_PRIVATE_KEY_FILE"));
        static_cast<void>(::unsetenv("REALMMESH_SESSION_TICKET_KEY"));
        static_cast<void>(::unsetenv("REALMMESH_IDENTITY_KEY_SEED"));
        static_cast<void>(::unsetenv("REALMMESH_QUEUE_KEY_SEED"));
    }
};

/// TCP 探活:能连上 127.0.0.1:<port> 即视为监听中。
[[nodiscard]] bool tcp_port_accepts_connections(std::uint16_t port) {
    const int descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
    if (descriptor < 0) return false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool connected = ::connect(
                               descriptor,
                               reinterpret_cast<const sockaddr*>(&address),
                               sizeof(address)) == 0;
    ::close(descriptor);
    return connected;
}

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
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

/// 把服务配置里的固定端口改写成一组空闲端口。两处静态下游指向(realm →
/// gateway、gateway → realm)必须随监听端口同步改写,否则按配置直连的
/// 路径会落到别的进程或空端口。
void use_free_ports(
    const std::filesystem::path& root,
    std::uint16_t realm_port,
    std::uint16_t gateway_port) {
    const auto realm_path = root / "services" / "realm.lua";
    auto realm = read_file(realm_path);
    realm = replace_all(realm, "listen_port = 7100",
                        "listen_port = " + std::to_string(realm_port));
    realm = replace_all(realm, "downstream_port = 8000",
                        "downstream_port = " + std::to_string(gateway_port));
    realm = replace_all(realm, "metrics_port = 9102", "metrics_port = 0");
    ASSERT_TRUE(write_file(realm_path, realm));

    // gateway.lua 里 listen_port = 8000 出现两次(TCP 与其伴随传输),
    // 两处都要指向同一个空闲端口。
    const auto gateway_path = root / "services" / "gateway.lua";
    auto gateway = read_file(gateway_path);
    gateway = replace_all(gateway, "listen_port = 8000",
                          "listen_port = " + std::to_string(gateway_port));
    gateway = replace_all(gateway, "downstream_port = 7100",
                          "downstream_port = " + std::to_string(realm_port));
    gateway = replace_all(gateway, "metrics_port = 9103", "metrics_port = 0");
    ASSERT_TRUE(write_file(gateway_path, gateway));
}

/// 拷贝真实 configs 到临时目录后确保服务发现关闭:
/// - LayeredConfigLoader 会把日志写进 <root>/logs/,拷贝避免污染源码树;
/// - 本环境无 etcd,而发现开启时 ServiceHost 的 ready 语义要求注册成功
///   (Task 4 契约),start_all 会整体失败;源配置默认 enabled = false,
///   替换仅为幂等兜底。端口由调用方随后改写成空闲端口(见 use_free_ports)。
[[nodiscard]] bool copy_configs_with_discovery_disabled(
    const std::filesystem::path& source, const std::filesystem::path& target) {
    std::error_code error;
    std::filesystem::create_directories(target / "common", error);
    if (error) return false;
    std::filesystem::create_directories(target / "services", error);
    if (error) return false;
    std::filesystem::copy(
        source / "common",
        target / "common",
        std::filesystem::copy_options::recursive,
        error);
    if (error) return false;
    std::filesystem::copy(
        source / "services",
        target / "services",
        std::filesystem::copy_options::recursive,
        error);
    if (error) return false;
    std::filesystem::copy_file(
        source / "main.config", target / "main.config", error);
    if (error) return false;

    auto contents = read_file(target / "common" / "discovery.lua");
    constexpr std::string_view enabled_true = "enabled = true";
    for (auto position = contents.find(enabled_true);
         position != std::string::npos;
         position = contents.find(enabled_true)) {
        contents.replace(position, enabled_true.size(), "enabled = false");
    }
    if (contents.find("enabled = false") == std::string::npos) return false;
    std::ofstream output(target / "common" / "discovery.lua", std::ios::trunc);
    output << contents;
    return static_cast<bool>(output);
}

/// 模式 1(全拓扑一体)E2E:真实配置起 realm → gateway,
/// entry 放行、依赖端口可连、整体关停后全部停止。
TEST(MeshHostE2ETest, AllInOneStartsAndStopsCleanly) {
    const ScopedTlsEnvironment tls_environment;
    const std::filesystem::path source = REALMMESH_SOURCE_DIR "/configs";
    const test_support::TemporaryDirectory scratch("mesh-host-e2e-");
    ASSERT_TRUE(copy_configs_with_discovery_disabled(source, scratch.path()));

    // 配置里的固定端口在本机可能被占用(macOS ControlCenter 的 AirPlay
    // Receiver 就常占 7000 附近),所以改用一组当前空闲的端口,而不是依赖
    // 配置里的默认值。
    const auto ports = unused_tcp_ports(2);
    const auto realm_port = ports.at(0);
    const auto gateway_port = ports.at(1);
    use_free_ports(scratch.path(), realm_port, gateway_port);

    const std::vector<ServiceSpec> specs{
        {"realm", {}, false},
        {"gateway", {"realm"}, true},
    };
    MeshHost mesh(scratch.path(), specs);
    ASSERT_TRUE(mesh.start_all());
    EXPECT_TRUE(mesh.entry_ready());
    // 依赖服务端口可连:realm 的监听端口 TCP 探活成功。
    EXPECT_TRUE(tcp_port_accepts_connections(realm_port));

    // /metrics 组装(#47):帧尾发布依赖 tick 节拍(MeshHost 不自转
    // 线程,由外部驱动),注册表渲染的 edge_* 段落先于既有 service_ready
    // gauge(拼接顺序不变)。
    std::string metrics_text;
    for (int attempt = 0; attempt < 1000; ++attempt) {
        mesh.tick();
        metrics_text = mesh.service("gateway").prometheus_metrics();
        if (metrics_text.find("edge_sessions{stage=") != std::string::npos) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    const auto sessions_at = metrics_text.find("edge_sessions{stage=");
    const auto ready_at =
        metrics_text.find("# TYPE realmmesh_service_ready gauge");
    ASSERT_NE(ready_at, std::string::npos);
    ASSERT_NE(sessions_at, std::string::npos);
    EXPECT_TRUE(sessions_at < ready_at);
    EXPECT_NE(
        metrics_text.find("edge_budget{kind=\"conn_free\"}"),
        std::string::npos);

    mesh.shutdown();
    EXPECT_FALSE(mesh.service("realm").runtime().running());
}

}  // namespace
}  // namespace realm::service_host
