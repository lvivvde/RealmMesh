#include "realmmesh/scripting/lua_runtime.hpp"
#include "realmmesh/service_host/layered_config_loader.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
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

// 真实 configs/ 树的生产加载路径冒烟:三个 services/<name>.lua 都能经
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
};

TEST_F(ConfigsLoadSmokeTest, EveryServiceConfigLoadsThroughLayeredLoader) {
    const auto names = service_names();
    ASSERT_FALSE(names.empty());
    for (const auto& name : names) {
        // 冒烟只关心生产加载路径不抛;结果配置由 mesh_host 消费,这里不展开。
        EXPECT_NO_THROW(static_cast<void>(
            LayeredConfigLoader::load(configs_root(), name))) << name;
    }
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
