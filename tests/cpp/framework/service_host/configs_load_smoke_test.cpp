#include "realmmesh/scripting/lua_runtime.hpp"
#include "realmmesh/game/common/admission_grant.hpp"
#include "realmmesh/game/queue/queue_config.hpp"
#include "realmmesh/service_host/layered_config_loader.hpp"
#include "realmmesh/service_host/startup_topology.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace realm::service_host {
namespace {

// 真实加载路径要求 TLS 证书路径环境变量;惯用法同 mesh_host_test.cpp。
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

// 真实 configs/ 树的生产加载路径冒烟:全部 services/<name>.lua 都能经
// LayeredConfigLoader 加载(与 mesh_host 同路径),全部 .lua 都能被 LuaRuntime
// 编译执行。纯 Lua 侧不做这件事——沙箱外的解释器与宿主环境不一致,
// 边界划分见地图 #15 的调研结论(调研票 #16)。
class ConfigsLoadSmokeTest : public ::testing::Test {
protected:
    ScopedTlsEnvironment tls_;

    static std::filesystem::path configs_root() {
        return std::filesystem::path(REALMMESH_SOURCE_DIR) / "configs";
    }

    static std::vector<std::string> service_names() {
        std::vector<std::string> names;
        for (const auto& entry :
             std::filesystem::directory_iterator(configs_root() / "services")) {
            if (entry.is_regular_file() && entry.path().extension() == ".lua") {
                names.push_back(entry.path().stem().string());
            }
        }
        return names;
    }

    static std::string read_file(const std::filesystem::path& path) {
        std::ifstream input(path);
        return std::string(
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>());
    }
};

/// 旧 Login 身份已退役:生产配置树里没有它的服务配置,加载 `login` 在
/// 启动期明确失败,而不是落回默认值静默起来;启动拓扑也不再声明它。
TEST_F(ConfigsLoadSmokeTest, LoginServiceConfigIsGone) {
    EXPECT_FALSE(
        std::filesystem::exists(configs_root() / "services" / "login.lua"));
    EXPECT_THROW(
        static_cast<void>(LayeredConfigLoader::load(configs_root(), "login")),
        std::runtime_error);
    const auto topology = read_file(configs_root() / "main.config");
    EXPECT_EQ(topology.find("\"login\""), std::string::npos);
    EXPECT_NE(topology.find("\"login_verify\""), std::string::npos);
}

/// 随包的 main.config 经装载入口按声明序给出四个服务，gateway 是唯一入口(#127)。
TEST_F(ConfigsLoadSmokeTest, MainConfigTopologyLoadsInDeclarationOrder) {
    const auto specs = load_topology(configs_root());

    ASSERT_EQ(specs.size(), std::size_t{4});
    EXPECT_EQ(specs[0].name, "login_verify");
    EXPECT_EQ(specs[1].name, "queue");
    EXPECT_EQ(specs[2].name, "realm");
    EXPECT_TRUE(specs[2].depends_on.empty());
    EXPECT_FALSE(specs[2].entry);
    EXPECT_EQ(specs[3].name, "gateway");
    EXPECT_EQ(specs[3].depends_on, std::vector<std::string>{"realm"});
    EXPECT_TRUE(specs[3].entry);
}

TEST_F(ConfigsLoadSmokeTest, EveryServiceConfigLoadsThroughLayeredLoader) {
    const auto names = service_names();
    ASSERT_FALSE(names.empty());
    for (const auto& name : names) {
        // 冒烟只关心生产加载路径不抛;结果配置由 mesh_host 消费,这里不展开。
        EXPECT_NO_THROW(static_cast<void>(
            LayeredConfigLoader::load(configs_root(), name))) << name;
    }
}

TEST_F(ConfigsLoadSmokeTest, LoginVerifyConfigLoadsThroughDedicatedLoader) {
    const auto config =
        LayeredConfigLoader::load_login_verify(configs_root(), "login_verify");
    // TLS 路径经环境变量解析(ScopedTlsEnvironment 已指向测试证书),
    // 账号集相对路径按 config_root 落为绝对路径。
    EXPECT_FALSE(config.login_verify.tls.certificate_chain_file.empty());
    EXPECT_FALSE(config.login_verify.tls.private_key_file.empty());
    EXPECT_TRUE(config.login_verify.accounts_file.is_absolute());
    EXPECT_TRUE(std::filesystem::exists(config.login_verify.accounts_file));
    EXPECT_EQ(config.login_verify.kid, "login-verify-v1");
}

TEST_F(ConfigsLoadSmokeTest, QueueConfigLoadsThroughDedicatedLoader) {
    const auto config = LayeredConfigLoader::load_queue(configs_root(), "queue");
    // TLS 路径经环境变量解析(ScopedTlsEnvironment 已指向测试证书)。
    EXPECT_FALSE(config.queue.tls.certificate_chain_file.empty());
    EXPECT_FALSE(config.queue.tls.private_key_file.empty());
    // 两种凭据角色各自一把 kid,绝不共用(#79 / ADR-0009)。
    EXPECT_EQ(config.queue.queue_number_kid, "queue-number-v2");
    EXPECT_EQ(config.queue.admission_grant_kid, "admission-grant-v1");
    EXPECT_NE(config.queue.queue_number_kid, config.queue.admission_grant_kid);
    EXPECT_EQ(config.queue.identity_kid, "login-verify-v1");
    EXPECT_EQ(config.queue.identity_issuer, "realmmesh/login-verify");
    EXPECT_TRUE(config.queue.release_step > 0);
    EXPECT_GE(config.queue.release_interval, std::chrono::seconds{2});
    EXPECT_GE(config.queue.budget_interval, std::chrono::seconds{1});
    // 快照/额度 key 契约(§5.2)与生产 etcd 路径默认值在配置中可见。
    EXPECT_EQ(config.queue.budget_prefix, "/realmmesh/budgets/service");
    EXPECT_EQ(config.queue.snapshot_key, "/realmmesh/queue/snapshot");
    EXPECT_EQ(config.queue.issuance_prefix, "/realmmesh/queue/issuance");
    // 发号权威依赖 etcd，配置层不提供绕过 fail-closed 的开关。
}

/// 出厂的 gateway.lua 必须真的能过 #79 的准入配置校验:键环非空、kid 唯一、
/// 每条都指名公钥环境变量、窗口在协议上限内,且与 Queue 的 admit_grace 一致
/// —— 两边窗口不一致会让合法 Grant 在网关侧被误判为过期。
TEST_F(ConfigsLoadSmokeTest, GatewayAdmissionConfigLoadsAndValidates) {
    const auto config = LayeredConfigLoader::load(configs_root(), "gateway");
    ASSERT_FALSE(config.login.admission_grant_keys.empty());
    EXPECT_EQ(
        config.login.admission_grant_keys.front().kid, "admission-grant-v1");
    EXPECT_EQ(
        config.login.admission_grant_keys.front().public_key_environment,
        "REALMMESH_ADMISSION_GRANT_PUBLIC_KEY");
    EXPECT_EQ(config.login.admission_grant_issuer, "realmmesh/queue");
    EXPECT_EQ(config.login.deployment_id, "development");
    EXPECT_GT(config.login.admission_grant_window, std::chrono::seconds{0});
    EXPECT_LE(
        config.login.admission_grant_window,
        game::common::admission_grant_max_window);
    EXPECT_NO_THROW(config.login.validate());

    const auto queue = LayeredConfigLoader::load_queue(configs_root(), "queue");
    EXPECT_EQ(config.login.admission_grant_window, queue.queue.admit_grace);
    EXPECT_EQ(config.login.admission_grant_issuer, queue.queue.admission_grant_issuer);
    EXPECT_EQ(config.login.deployment_id, queue.queue.deployment_id);
}

TEST_F(ConfigsLoadSmokeTest, EveryConfigFileCompilesInLuaRuntime) {
    const std::vector<std::filesystem::path> layers = {
        configs_root() / "common",
        configs_root() / "services",
    };
    for (const auto& layer : layers) {
        for (const auto& entry : std::filesystem::directory_iterator(layer)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".lua") {
                continue;
            }
            scripting::LuaRuntime runtime;
            std::string error;
            EXPECT_TRUE(runtime.load_module(
                "configs_smoke_" + entry.path().stem().string(),
                entry.path(),
                &error))
                << entry.path() << ": " << error;
        }
    }
}

}  // namespace
}  // namespace realm::service_host
