#include "realmmesh/service_host/layered_config_loader.hpp"
#include "realmmesh/observability/logger.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>

namespace realm::service_host {
namespace {

class ScopedPlayerDataUri final {
public:
    explicit ScopedPlayerDataUri(const char* value) {
        if (const char* previous = std::getenv(name)) previous_ = previous;
        if (value) {
            EXPECT_EQ(::setenv(name, value, 1), 0);
        } else {
            EXPECT_EQ(::unsetenv(name), 0);
        }
    }
    ~ScopedPlayerDataUri() {
        if (previous_) {
            static_cast<void>(::setenv(name, previous_->c_str(), 1));
        } else {
            static_cast<void>(::unsetenv(name));
        }
    }
    static constexpr const char* name = "REALMMESH_TEST_MONGODB_URI";
private:
    std::optional<std::string> previous_;
};

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

TEST_F(LayeredConfigTest, ParsesSharedPlayerDataDeployment) {
    write(
        root_ / "common" / "player_data.lua",
        "return { player_data = { "
        "uri = \"mongodb://127.0.0.1:27017/?replicaSet=rs0\", "
        "database = \"realmmesh\", server_selection_timeout_ms = 750, "
        "socket_timeout_ms = 1500, "
        "bootstrap_accounts_file = \"common/accounts.lua\", "
        "credential_hash_cost = \"minimum\" } }");
    write(root_ / "services" / "realm.lua", "return {}");

    const auto config = LayeredConfigLoader::load(root_, "realm");

    EXPECT_EQ(
        config.player_data.uri, "mongodb://127.0.0.1:27017/?replicaSet=rs0");
    EXPECT_EQ(config.player_data.database, "realmmesh");
    EXPECT_EQ(
        config.player_data.options.server_selection_timeout,
        std::chrono::milliseconds{750});
    EXPECT_EQ(
        config.player_data.options.socket_timeout,
        std::chrono::milliseconds{1500});
    EXPECT_EQ(
        config.player_data.options.bootstrap_accounts_file,
        root_ / "common" / "accounts.lua");
    EXPECT_EQ(
        config.player_data.options.credential_hash_cost,
        game::common::CredentialHashCost::Minimum);
}

TEST_F(LayeredConfigTest, RequiresDatabaseWhenUriIsSet) {
    write(
        root_ / "common" / "player_data.lua",
        "return { player_data = { "
        "uri = \"mongodb://127.0.0.1:27017/?replicaSet=rs0\" } }");
    write(root_ / "services" / "realm.lua", "return {}");

    EXPECT_THROW(
        static_cast<void>(LayeredConfigLoader::load(root_, "realm")),
        std::invalid_argument);
}

TEST_F(LayeredConfigTest, PrivatePlayerDataUriOverridesDefaultWithoutLuaOsAccess) {
    const ScopedPlayerDataUri environment(
        "mongodb://test_user:test_secret@127.0.0.1:27018/?directConnection=true");
    write(root_ / "common" / "player_data.lua",
          "return { player_data = { uri = 'mongodb://127.0.0.1:27017/', "
          "uri_environment = 'REALMMESH_TEST_MONGODB_URI', database = 'realmmesh' } }");
    write(root_ / "services" / "realm.lua", "return {}");
    EXPECT_EQ(LayeredConfigLoader::load(root_, "realm").player_data.uri,
              std::getenv(ScopedPlayerDataUri::name));
}

TEST_F(LayeredConfigTest, UnsetPlayerDataUriEnvironmentPreservesLocalDefault) {
    const ScopedPlayerDataUri environment(nullptr);
    write(root_ / "common" / "player_data.lua",
          "return { player_data = { uri = 'mongodb://127.0.0.1:27017/', "
          "uri_environment = 'REALMMESH_TEST_MONGODB_URI', database = 'realmmesh' } }");
    write(root_ / "services" / "realm.lua", "return {}");
    EXPECT_EQ(LayeredConfigLoader::load(root_, "realm").player_data.uri,
              "mongodb://127.0.0.1:27017/");
}

TEST_F(LayeredConfigTest, MissingPrivatePlayerDataUriFailsWithoutDefault) {
    const ScopedPlayerDataUri environment(nullptr);
    write(root_ / "common" / "player_data.lua",
          "return { player_data = { uri_environment = 'REALMMESH_TEST_MONGODB_URI', "
          "database = 'realmmesh' } }");
    write(root_ / "services" / "realm.lua", "return {}");
    EXPECT_THROW(static_cast<void>(LayeredConfigLoader::load(root_, "realm")),
                 std::invalid_argument);
}

TEST_F(LayeredConfigTest, EmptyPrivatePlayerDataUriFailsInsteadOfConnectingLocally) {
    const ScopedPlayerDataUri environment("");
    write(root_ / "common" / "player_data.lua",
          "return { player_data = { uri = 'mongodb://127.0.0.1:27017/', "
          "uri_environment = 'REALMMESH_TEST_MONGODB_URI', database = 'realmmesh' } }");
    write(root_ / "services" / "realm.lua", "return {}");
    EXPECT_THROW(static_cast<void>(LayeredConfigLoader::load(root_, "realm")),
                 std::invalid_argument);
}

TEST_F(LayeredConfigTest, RejectsRetiredSqlitePlayerDataKeys) {
    // ADR-0010 的本地文件键在 ADR-0011 后失效；静默忽略会让旧配置误以为
    // 仍指向本地库。
    write(
        root_ / "common" / "player_data.lua",
        "return { player_data = { database_file = \"data/players.sqlite\" } }");
    write(root_ / "services" / "realm.lua", "return {}");

    EXPECT_THROW(
        static_cast<void>(LayeredConfigLoader::load(root_, "realm")),
        std::invalid_argument);
}

TEST_F(LayeredConfigTest, RejectsUnknownCredentialHashCost) {
    write(
        root_ / "common" / "player_data.lua",
        "return { player_data = { credential_hash_cost = \"cheap\" } }");
    write(root_ / "services" / "realm.lua", "return {}");

    EXPECT_THROW(
        static_cast<void>(LayeredConfigLoader::load(root_, "realm")),
        std::invalid_argument);
}

TEST_F(LayeredConfigTest, MissingServiceFileThrows) {
    EXPECT_THROW(static_cast<void>(LayeredConfigLoader::load(root_, "ghost")),
                 std::runtime_error);
}

}  // namespace
}  // namespace realm::service_host
