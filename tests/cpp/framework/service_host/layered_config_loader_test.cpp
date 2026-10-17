#include "realmmesh/service_host/layered_config_loader.hpp"
#include "realmmesh/observability/logger.hpp"
#include "realmmesh/game/common/player_data_config.hpp"
#include "realmmesh/game/common/player_data_config_lua.hpp"
#include "realmmesh/game/realm/realm_module.hpp"
#include "realmmesh/service_host/layered_config_lua.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
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
    /// 经分层装载后从合并根表解析共享 player_data 节段(与各服务模块
    /// 装载入口同一路径),相对路径按 config_root 落为绝对路径。
    [[nodiscard]] game::common::PlayerDataConfig load_player_data() const {
        const auto layered = LayeredConfigLoader::load(root_, "realm");
        auto config = game::common::parse_player_data_config(lua_root(layered));
        game::common::resolve_player_data_paths(config, layered.config_root());
        return config;
    }
};

/// 宿主层节段经 LayeredConfig::host() 读取;服务专属节段(此处借 realm 的
/// max_events_per_frame)由模块装载入口从同一合并根表解析。
TEST_F(LayeredConfigTest, ServiceLayerOverridesCommon) {
    write(
        root_ / "services" / "realm.lua",
        "return { logging = { level = \"debug\" }, max_events_per_frame = 30, "
        "realm = { training_rule_file = \"services/realm/training.lua\" } }");
    const auto layered = LayeredConfigLoader::load(root_, "realm");
    EXPECT_EQ(layered.service_name(), "realm");
    EXPECT_EQ(
        layered.host().logging.min_severity,
        observability::Severity::Debug);  // 服务层覆盖
    EXPECT_EQ(
        layered.host().service_discovery.lease_ttl,
        std::chrono::seconds(9));  // 公共层保留
    EXPECT_EQ(
        game::realm::load_realm_config(layered).max_events_per_frame,
        std::size_t{30});
}

TEST_F(LayeredConfigTest, CliOverridesInstanceIdentity) {
    write(
        root_ / "services" / "realm.lua",
        "return { service_discovery = { instance_id = \"file-id\", node_id = \"file-node\" } }");
    const auto layered = LayeredConfigLoader::load(
        root_,
        "realm",
        CliOverrides{.instance_id = "cli-id", .node_id = "cli-node"});
    const auto& config = layered.host();
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

    const auto config = load_player_data();

    EXPECT_EQ(config.uri, "mongodb://127.0.0.1:27017/?replicaSet=rs0");
    EXPECT_EQ(config.database, "realmmesh");
    EXPECT_EQ(
        config.options.server_selection_timeout,
        std::chrono::milliseconds{750});
    EXPECT_EQ(
        config.options.socket_timeout,
        std::chrono::milliseconds{1500});
    EXPECT_EQ(
        config.options.bootstrap_accounts_file,
        root_ / "common" / "accounts.lua");
    EXPECT_EQ(
        config.options.credential_hash_cost,
        game::common::CredentialHashCost::Minimum);
}

TEST_F(LayeredConfigTest, RequiresDatabaseWhenUriIsSet) {
    write(
        root_ / "common" / "player_data.lua",
        "return { player_data = { "
        "uri = \"mongodb://127.0.0.1:27017/?replicaSet=rs0\" } }");
    write(root_ / "services" / "realm.lua", "return {}");

    EXPECT_THROW(
        static_cast<void>(load_player_data()),
        std::invalid_argument);
}

TEST_F(LayeredConfigTest, PrivatePlayerDataUriOverridesDefaultWithoutLuaOsAccess) {
    const ScopedPlayerDataUri environment(
        "mongodb://test_user:test_secret@127.0.0.1:27018/?directConnection=true");
    write(root_ / "common" / "player_data.lua",
          "return { player_data = { uri = 'mongodb://127.0.0.1:27017/', "
          "uri_environment = 'REALMMESH_TEST_MONGODB_URI', database = 'realmmesh' } }");
    write(root_ / "services" / "realm.lua", "return {}");
    EXPECT_EQ(load_player_data().uri,
              std::getenv(ScopedPlayerDataUri::name));
}

TEST_F(LayeredConfigTest, UnsetPlayerDataUriEnvironmentPreservesLocalDefault) {
    const ScopedPlayerDataUri environment(nullptr);
    write(root_ / "common" / "player_data.lua",
          "return { player_data = { uri = 'mongodb://127.0.0.1:27017/', "
          "uri_environment = 'REALMMESH_TEST_MONGODB_URI', database = 'realmmesh' } }");
    write(root_ / "services" / "realm.lua", "return {}");
    EXPECT_EQ(load_player_data().uri,
              "mongodb://127.0.0.1:27017/");
}

TEST_F(LayeredConfigTest, MissingPrivatePlayerDataUriFailsWithoutDefault) {
    const ScopedPlayerDataUri environment(nullptr);
    write(root_ / "common" / "player_data.lua",
          "return { player_data = { uri_environment = 'REALMMESH_TEST_MONGODB_URI', "
          "database = 'realmmesh' } }");
    write(root_ / "services" / "realm.lua", "return {}");
    EXPECT_THROW(static_cast<void>(load_player_data()),
                 std::invalid_argument);
}

TEST_F(LayeredConfigTest, EmptyPrivatePlayerDataUriFailsInsteadOfConnectingLocally) {
    const ScopedPlayerDataUri environment("");
    write(root_ / "common" / "player_data.lua",
          "return { player_data = { uri = 'mongodb://127.0.0.1:27017/', "
          "uri_environment = 'REALMMESH_TEST_MONGODB_URI', database = 'realmmesh' } }");
    write(root_ / "services" / "realm.lua", "return {}");
    EXPECT_THROW(static_cast<void>(load_player_data()),
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
        static_cast<void>(load_player_data()),
        std::invalid_argument);
}

TEST_F(LayeredConfigTest, RejectsUnknownCredentialHashCost) {
    write(
        root_ / "common" / "player_data.lua",
        "return { player_data = { credential_hash_cost = \"cheap\" } }");
    write(root_ / "services" / "realm.lua", "return {}");

    EXPECT_THROW(
        static_cast<void>(load_player_data()),
        std::invalid_argument);
}

TEST_F(LayeredConfigTest, MissingServiceFileThrows) {
    EXPECT_THROW(static_cast<void>(LayeredConfigLoader::load(root_, "ghost")),
                 std::runtime_error);
}

}  // namespace
}  // namespace realm::service_host
