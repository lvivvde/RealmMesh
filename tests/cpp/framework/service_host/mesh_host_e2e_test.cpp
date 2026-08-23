#include "realmmesh/service_host/mesh_host.hpp"

#include "realmmesh/game/gateway/gateway_runtime.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace realm::service_host {
namespace {

/// TLS 证书环境变量守护:指向 CMake 预生成的自签证书,析构时还原。
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
    }
    ~ScopedTlsEnvironment() {
        static_cast<void>(::unsetenv("REALMMESH_TLS_CERTIFICATE_FILE"));
        static_cast<void>(::unsetenv("REALMMESH_TLS_PRIVATE_KEY_FILE"));
    }
};

/// 临时目录:进程内唯一名,析构时递归清理。
class TemporaryDirectory final {
public:
    TemporaryDirectory()
        : path_(
              std::filesystem::temp_directory_path() /
              ("mesh-host-e2e-" +
               std::to_string(static_cast<long long>(::getpid())))) {
        std::filesystem::create_directories(path_);
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

/// TCP 探活:能连上 127.0.0.1:<port> 即视为监听中。
[[nodiscard]] bool tcp_port_accepts_connections(std::uint16_t port) {
    const int descriptor = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
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

/// 拷贝真实 configs 到临时目录后关闭服务发现:
/// - LayeredConfigLoader 会把日志写进 <root>/logs/,拷贝避免污染源码树;
/// - 本环境无 etcd,而发现开启时 ServiceHost 的 ready 语义要求注册成功
///   (Task 4 契约),start_all 会整体失败,故在拷贝里仅改 discovery.enabled,
///   其余配置(含全部端口)保持真实值。
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
    const auto position = contents.find(enabled_true);
    if (position == std::string::npos) return false;
    contents.replace(position, enabled_true.size(), "enabled = false");
    std::ofstream output(target / "common" / "discovery.lua", std::ios::trunc);
    output << contents;
    return static_cast<bool>(output);
}

/// 模式 1(全拓扑一体)E2E:真实配置起 realm → login → gateway,
/// entry 放行、依赖端口可连、整体关停后全部停止。
TEST(MeshHostE2ETest, AllInOneStartsAndStopsCleanly) {
    const ScopedTlsEnvironment tls_environment;
    const std::filesystem::path source = REALMMESH_SOURCE_DIR "/configs";
    const TemporaryDirectory scratch;
    ASSERT_TRUE(copy_configs_with_discovery_disabled(source, scratch.path()));

    const std::vector<ServiceSpec> specs{
        {"realm", {}, false},
        {"login", {"realm"}, false},
        {"gateway", {"login"}, true},
    };
    MeshHost mesh(scratch.path(), specs);
    ASSERT_TRUE(mesh.start_all());
    EXPECT_TRUE(mesh.entry_ready());
    // 依赖服务端口可连:127.0.0.1:7100(realm 监听)TCP 探活成功。
    EXPECT_TRUE(tcp_port_accepts_connections(7100));
    mesh.shutdown();
    EXPECT_FALSE(mesh.service("realm").runtime().running());
}

}  // namespace
}  // namespace realm::service_host
