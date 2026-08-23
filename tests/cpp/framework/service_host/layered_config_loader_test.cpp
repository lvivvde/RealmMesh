#include "realmmesh/service_host/layered_config_loader.hpp"
#include "realmmesh/observability/logger.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace realm::service_host {
namespace {

class LayeredConfigTest : public ::testing::Test {
protected:
    std::filesystem::path root_;

    void SetUp() override {
        root_ =
            std::filesystem::temp_directory_path() /
            ("layered-cfg-" +
             std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
        std::filesystem::create_directories(root_ / "common");
        std::filesystem::create_directories(root_ / "services");
        write(
            root_ / "common" / "logging.lua",
            "return { logging = { level = \"warn\", environment = \"test\", cluster = \"ci\", region = \"cn\", service_name = \"common\", console = false, metrics_port = 0 } }");
        write(
            root_ / "common" / "discovery.lua",
            "return { service_discovery = { enabled = false, endpoint = \"http://etcd:2379\", lease_ttl_seconds = 9 } }");
    }
    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }
    static void write(
        const std::filesystem::path& path, std::string_view text) {
        std::ofstream output(path);
        output << text;
    }
};

TEST_F(LayeredConfigTest, ServiceLayerOverridesCommon) {
    write(
        root_ / "services" / "login.lua",
        "return { logging = { level = \"debug\" }, tick_rate = 30 }");
    const auto config = LayeredConfigLoader::load(root_, "login");
    EXPECT_EQ(
        config.logging.min_severity,
        observability::Severity::Debug);  // 服务层覆盖
    EXPECT_EQ(
        config.service_discovery.lease_ttl,
        std::chrono::seconds(9));  // 公共层保留
    EXPECT_EQ(config.tick_rate, std::uint32_t{30});
}

TEST_F(LayeredConfigTest, CliOverridesInstanceIdentity) {
    write(
        root_ / "services" / "login.lua",
        "return { service_discovery = { instance_id = \"file-id\", node_id = \"file-node\" } }");
    const auto config = LayeredConfigLoader::load(
        root_,
        "login",
        CliOverrides{.instance_id = "cli-id", .node_id = "cli-node"});
    EXPECT_EQ(config.service_discovery.instance_id, "cli-id");
    EXPECT_EQ(config.service_discovery.node_id, "cli-node");
    /// file_path 按实例生成:<root>/logs/login/login-cli-id.jsonl
    EXPECT_NE(
        config.logging.file_path.string().find("login-cli-id.jsonl"),
        std::string::npos);
}

TEST_F(LayeredConfigTest, MissingServiceFileThrows) {
    EXPECT_THROW(LayeredConfigLoader::load(root_, "ghost"), std::runtime_error);
}

}  // namespace
}  // namespace realm::service_host
