#include "realmmesh/service_host/mesh_host.hpp"

#include "realmmesh/edge/edge_runtime.hpp"
#include "test_service_modules.hpp"

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

using test_modules::edge_runtime;

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

class MeshHostTest : public ::testing::Test {
protected:
    std::filesystem::path root_;

    void SetUp() override {
        root_ =
            std::filesystem::temp_directory_path() /
            ("mesh-host-" +
             std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
        std::filesystem::create_directories(root_ / "common");
        std::filesystem::create_directories(root_ / "services");
        write(
            root_ / "common" / "discovery.lua",
            "return { service_discovery = { enabled = false, instance_id = \"mesh-test-01\", endpoint = \"http://etcd:2379\", lease_ttl_seconds = 9 } }");
        write(
            root_ / "services" / "a.lua", "return { " + transport_lua() + " }");
        write(
            root_ / "services" / "b.lua", "return { " + transport_lua() + " }");
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    /// 单个 tls_tcp transport(listen_port = 0 随机,证书走环境变量)。
    static std::string transport_lua() {
        return "transports = { { name = \"client_tls_tcp\", protocol = "
               "\"tls_tcp\", enabled = true, listen_address = "
               "\"127.0.0.1\", listen_port = 0, "
               "certificate_chain_file_environment = "
               "\"REALMMESH_TLS_CERTIFICATE_FILE\", "
               "private_key_file_environment = "
               "\"REALMMESH_TLS_PRIVATE_KEY_FILE\" } }";
    }

    /// 用例里的服务名都挂空转的接入面模块。
    static ServiceModuleRegistry modules() {
        return test_modules::test_service_modules({"a", "b", "entry"});
    }

    static void write(
        const std::filesystem::path& path, std::string_view text) {
        std::ofstream output(path);
        output << text;
    }
};

TEST_F(MeshHostTest, StartsWavesAndGatesEntryUntilAllReady) {
    const ScopedTlsEnvironment tls_environment;
    const std::vector<ServiceSpec> specs{
        {"a", {}, false}, {"b", {"a"}, true},  // b 为 entry
    };
    MeshHost mesh(root_, specs, modules());
    // 启动前门禁关闭。
    EXPECT_FALSE(mesh.entry_ready());
    ASSERT_TRUE(mesh.start_all());
    EXPECT_TRUE(mesh.entry_ready());
    EXPECT_TRUE(edge_runtime(mesh.service("a")).running());
    EXPECT_TRUE(edge_runtime(mesh.service("b")).running());
    mesh.tick();  // 发现禁用时为无害轮询。
    mesh.shutdown();
    EXPECT_FALSE(edge_runtime(mesh.service("a")).running());
    EXPECT_FALSE(edge_runtime(mesh.service("b")).running());
    EXPECT_FALSE(mesh.entry_ready());
}

TEST_F(MeshHostTest, FailureRecyclesStartedServicesInReverse) {
    const ScopedTlsEnvironment tls_environment;
    // b 的 runtime 节为非表值,使 LayeredConfigLoader 抛异常。
    write(
        root_ / "services" / "b.lua",
        "return { " + transport_lua() + ", runtime = \"broken\" }");
    const std::vector<ServiceSpec> specs{
        {"a", {}, false},
        {"b", {"a"}, true},
    };
    MeshHost mesh(root_, specs, modules());
    EXPECT_FALSE(mesh.start_all());
    // a 已启动但被反序回收。
    EXPECT_FALSE(edge_runtime(mesh.service("a")).running());
    // b 从未装配成功,不可访问。
    EXPECT_THROW(static_cast<void>(mesh.service("b")), std::out_of_range);
}

TEST_F(MeshHostTest, FailureBeforeFinalWaveNeverConstructsEntry) {
    const ScopedTlsEnvironment tls_environment;
    write(
        root_ / "services" / "b.lua",
        "return { " + transport_lua() + ", runtime = \"broken\" }");
    write(
        root_ / "services" / "entry.lua", "return { " + transport_lua() + " }");
    const std::vector<ServiceSpec> specs{
        {"a", {}, false},
        {"entry", {}, true},
        {"b", {"a"}, false},
    };

    MeshHost mesh(root_, specs, modules());
    EXPECT_FALSE(mesh.start_all());
    EXPECT_THROW(static_cast<void>(mesh.service("entry")), std::out_of_range);
}

TEST_F(MeshHostTest, ServiceLookupThrowsForUnknownName) {
    const ScopedTlsEnvironment tls_environment;
    MeshHost mesh(
        root_, std::vector<ServiceSpec>{{"a", {}, false}}, modules());
    EXPECT_THROW(static_cast<void>(mesh.service("ghost")), std::out_of_range);
    ASSERT_TRUE(mesh.start_all());
    EXPECT_NO_THROW(static_cast<void>(mesh.service("a")));
}

}  // namespace
}  // namespace realm::service_host
