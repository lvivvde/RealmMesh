#include "realmmesh/service_host/mesh_host.hpp"

#include "realmmesh/game/gateway/gateway_runtime.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
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
    }
    ~ScopedTlsEnvironment() {
        static_cast<void>(::unsetenv("REALMMESH_TLS_CERTIFICATE_FILE"));
        static_cast<void>(::unsetenv("REALMMESH_TLS_PRIVATE_KEY_FILE"));
        static_cast<void>(::unsetenv("REALMMESH_SESSION_TICKET_KEY"));
    }
};

/// 临时目录:进程内唯一名,析构时递归清理。
class TemporaryDirectory final {
public:
    TemporaryDirectory()
        : path_(
              std::filesystem::temp_directory_path() /
              ("mode2-test-" +
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

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

/// 拷贝真实 configs 到临时目录后确保服务发现关闭(与 E2E 同模式):
/// 本环境无 etcd,而发现开启时 ServiceHost 的 ready 语义要求注册成功,
/// start_all 会整体失败;源配置默认 enabled = false,替换仅为幂等兜底,
/// 其余配置保持真实值。
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

[[nodiscard]] std::vector<ServiceSpec> full_topology() {
    return {
        {"realm", {}, false},
        {"login", {"realm"}, false},
        {"gateway", {"login"}, true},
    };
}

/// 模式 2 语义 = realm_mesh --service <name> 的收窄规则:
/// 拓扑收窄为仅该服务,depends_on 清空;未知服务名抛 invalid_argument。
TEST(Mode2Test, SingleServiceNarrowsTopologyAndAppliesOverrides) {
    const auto specs =
        MeshHost::narrow_single_service(full_topology(), "login");
    ASSERT_EQ(specs.size(), std::size_t{1});
    EXPECT_EQ(specs.at(0).name, "login");
    EXPECT_TRUE(specs.at(0).depends_on.empty());

    EXPECT_THROW(
        static_cast<void>(
            MeshHost::narrow_single_service(full_topology(), "unknown")),
        std::invalid_argument);
}

/// 单服务 MeshHost(temp configs,服务 login,discovery off):
/// start_all 成功、entry_ready 放行、runtime 运行、shutdown 干净;
/// CliOverrides.instance_id 经 prometheus 指标的 service_instance 标签生效。
TEST(Mode2Test, SingleServiceMeshStartsAndRuns) {
    const ScopedTlsEnvironment tls_environment;
    const std::filesystem::path source = REALMMESH_SOURCE_DIR "/configs";
    const TemporaryDirectory scratch;
    ASSERT_TRUE(copy_configs_with_discovery_disabled(source, scratch.path()));

    CliOverrides overrides;
    overrides.instance_id = "login-mode2-77";
    MeshHost mesh(
        scratch.path(),
        MeshHost::narrow_single_service(full_topology(), "login"),
        overrides);
    ASSERT_TRUE(mesh.start_all());
    EXPECT_TRUE(mesh.entry_ready());
    EXPECT_TRUE(mesh.service("login").runtime().running());
    EXPECT_NE(
        mesh.service("login").prometheus_metrics().find(
            "service_instance=\"login-mode2-77\""),
        std::string::npos);
    mesh.shutdown();
    EXPECT_FALSE(mesh.service("login").runtime().running());
}

}  // namespace
}  // namespace realm::service_host
