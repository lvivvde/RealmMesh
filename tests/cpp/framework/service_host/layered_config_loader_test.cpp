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

/// 扁平加载路径(LayeredConfigLoader::load)的服务名用 realm:login_verify
/// 与 queue 走各自的 load_* 重载,拿它们做 fixture 会读成同一路径。
TEST_F(LayeredConfigTest, ServiceLayerOverridesCommon) {
    write(
        root_ / "services" / "realm.lua",
        "return { logging = { level = \"debug\" }, tick_rate = 30 }");
    const auto config = LayeredConfigLoader::load(root_, "realm");
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
        root_ / "services" / "realm.lua",
        "return { service_discovery = { instance_id = \"file-id\", node_id = \"file-node\" } }");
    const auto config = LayeredConfigLoader::load(
        root_,
        "realm",
        CliOverrides{.instance_id = "cli-id", .node_id = "cli-node"});
    EXPECT_EQ(config.service_discovery.instance_id, "cli-id");
    EXPECT_EQ(config.service_discovery.node_id, "cli-node");
    /// file_path 按实例生成:<root>/logs/realm/realm-cli-id.jsonl
    EXPECT_NE(
        config.logging.file_path.string().find("realm-cli-id.jsonl"),
        std::string::npos);
}

TEST_F(LayeredConfigTest, ResolvesSharedPlayerDataDatabaseFromConfigRoot) {
    write(
        root_ / "common" / "player_data.lua",
        "return { player_data = { database_file = \"data/players.sqlite\", "
        "bootstrap_accounts_file = \"common/accounts.lua\", "
        "credential_hash_cost = \"minimum\", busy_timeout_ms = 750 } }");
    write(root_ / "services" / "realm.lua", "return {}");

    const auto config = LayeredConfigLoader::load(root_, "realm");

    EXPECT_EQ(
        config.player_data.database_file,
        root_ / "data" / "players.sqlite");
    EXPECT_EQ(
        config.player_data.options.busy_timeout,
        std::chrono::milliseconds{750});
    EXPECT_EQ(
        config.player_data.options.bootstrap_accounts_file,
        root_ / "common" / "accounts.lua");
    EXPECT_EQ(
        config.player_data.options.credential_hash_cost,
        game::common::CredentialHashCost::Minimum);
}

TEST_F(LayeredConfigTest, RejectsUnknownCredentialHashCost) {
    write(
        root_ / "common" / "player_data.lua",
        "return { player_data = { database_file = \"data/players.sqlite\", "
        "credential_hash_cost = \"cheap\" } }");
    write(root_ / "services" / "realm.lua", "return {}");

    EXPECT_THROW(
        static_cast<void>(LayeredConfigLoader::load(root_, "realm")),
        std::invalid_argument);
}

TEST_F(LayeredConfigTest, MissingServiceFileThrows) {
    EXPECT_THROW(LayeredConfigLoader::load(root_, "ghost"), std::runtime_error);
}

}  // namespace
}  // namespace realm::service_host
