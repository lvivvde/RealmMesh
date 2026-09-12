#include "realmmesh/service_host/layered_config_loader.hpp"

#include "realmmesh/scripting/lua_runtime.hpp"

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace realm::service_host {
namespace {

/// 深合并脚本:merge(base, overlay) 将 overlay 递归并入 base(同名键 overlay
/// 值胜); ensure_table(root, key) 保证 root[key] 为表并返回,供补默认配置节。
constexpr std::string_view merge_script = R"lua(
local function merge(base, overlay)
    for key, value in pairs(overlay) do
        if type(base[key]) == "table" and type(value) == "table" then
            merge(base[key], value)
        else
            base[key] = value
        end
    end
    return base
end
local function ensure_table(root, key)
    if type(root[key]) ~= "table" then
        root[key] = {}
    end
    return root[key]
end
return { merge = merge, ensure_table = ensure_table }
)lua";

/// 收集目录下全部 .lua 文件并按文件名排序,保证公共层合并次序确定。
[[nodiscard]] std::vector<std::filesystem::path> lua_files(
    const std::filesystem::path& directory) {
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) return {};

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() == ".lua" && entry.is_regular_file()) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

/// 读表内字符串字段,缺失或非字符串时返回空串。
[[nodiscard]] std::string string_field(
    const sol::table& table, std::string_view field) {
    const sol::object value = table.raw_get<sol::object>(std::string(field));
    return value.is<std::string>() ? value.as<std::string>() : std::string{};
}

/// 取合并脚本的导出函数(脚本为内部常量,加载成功即必然存在)。
[[nodiscard]] sol::protected_function merger_function(
    const scripting::LuaRuntime& runtime, std::string_view function_name) {
    return runtime.module("config_merger")
        .raw_get<sol::protected_function>(std::string(function_name));
}

/// 调用合并脚本导出函数,Lua 出错时抛 runtime_error。
template <typename Return, typename... Arguments>
Return call_merger(
    const scripting::LuaRuntime& runtime,
    std::string_view function_name,
    Arguments&&... arguments) {
    const sol::protected_function function =
        merger_function(runtime, function_name);
    const sol::protected_function_result result =
        function(std::forward<Arguments>(arguments)...);
    if (!result.valid()) {
        const sol::error lua_error = result;
        throw std::runtime_error(
            "config merger call failed: " + std::string(lua_error.what()));
    }
    return result.get<Return>();
}

/// 合并后的分层根表:runtime 必须与 root 同生命周期(root 引用其 Lua 态)。
struct MergedLayers {
    scripting::LuaRuntime runtime;
    sol::table root;
};

/// 公共装载管线(出参形式,避免移动持 Lua 态的对象):服务层 + 公共层
/// 深合并(公共层先互并,服务层最后并入,服务层值胜)、CLI 覆盖、实例
/// 身份与日志身份注入。parse 前置工作全在这里,各 load_* 只剩解析分派。
void merged_layers(
    MergedLayers& layers,
    const std::filesystem::path& config_root,
    std::string_view service_name,
    const CliOverrides& overrides) {
    std::string error;
    if (!layers.runtime.load_module_source(
            "config_merger", merge_script, &error)) {
        throw std::runtime_error("failed to load merge script: " + error);
    }

    // 服务层表:services/<name>.lua 缺失或语法错误 → runtime_error。
    const std::filesystem::path service_file =
        config_root / "services" / (std::string(service_name) + ".lua");
    if (!layers.runtime.load_module("service_config", service_file, &error)) {
        throw std::runtime_error(
            "failed to load service configuration: " + error);
    }
    layers.root = layers.runtime.module("service_config");

    // 公共层按文件名序相互深合并(后文件胜),服务层最后并入(服务层值胜)。
    sol::table common_root;
    bool has_common = false;
    for (const auto& file : lua_files(config_root / "common")) {
        if (!layers.runtime.load_module("common_config", file, &error)) {
            throw std::runtime_error(
                "failed to load common configuration: " + error);
        }
        if (has_common) {
            common_root = call_merger<sol::table>(
                layers.runtime, "merge", common_root,
                layers.runtime.module("common_config"));
        } else {
            common_root = layers.runtime.module("common_config");
            has_common = true;
        }
    }
    if (has_common) {
        layers.root = call_merger<sol::table>(
            layers.runtime, "merge", common_root, layers.root);
    }

    // CLI 覆盖:非空项写入 service_discovery,优先级最高。
    sol::table discovery = call_merger<sol::table>(
        layers.runtime, "ensure_table", layers.root, "service_discovery");
    if (overrides.instance_id.has_value()) {
        discovery["instance_id"] = *overrides.instance_id;
    }
    if (overrides.node_id.has_value()) {
        discovery["node_id"] = *overrides.node_id;
    }
    if (overrides.zone.has_value()) {
        discovery["zone"] = *overrides.zone;
    }

    // 实例身份默认值与日志身份:file_path 按实例生成,service_name 固定。
    std::string instance = string_field(discovery, "instance_id");
    if (instance.empty()) {
        instance = std::string(service_name) + "-01";
    }
    sol::table logging = call_merger<sol::table>(
        layers.runtime, "ensure_table", layers.root, "logging");
    logging["service_name"] = std::string(service_name);
    logging["file_path"] =
        (config_root / "logs" / std::string(service_name) /
         (std::string(service_name) + "-" + instance + ".jsonl"))
            .string();
}

}  // namespace

game::gateway::GatewayConfig LayeredConfigLoader::load(
    const std::filesystem::path& config_root,
    std::string_view service_name,
    const CliOverrides& overrides) {
    MergedLayers layers;
    merged_layers(layers, config_root, service_name, overrides);

    // parse 要求 transports 为表(可为空):未配置时补空表。
    call_merger<sol::table>(
        layers.runtime, "ensure_table", layers.root, "transports");

    return game::gateway::GatewayConfigLoader::parse(layers.root);
}

LayeredConfigLoader::LoginVerifyServiceConfig
LayeredConfigLoader::load_login_verify(
    const std::filesystem::path& config_root,
    std::string_view service_name,
    const CliOverrides& overrides) {
    MergedLayers layers;
    merged_layers(layers, config_root, service_name, overrides);

    LoginVerifyServiceConfig config;
    // 宿主级节段经由既有解析器(logging/service_discovery/metrics);
    // parse 要求 transports 为表(可为空):未配置时补空表。
    call_merger<sol::table>(
        layers.runtime, "ensure_table", layers.root, "transports");
    config.host = game::gateway::GatewayConfigLoader::parse(layers.root);
    config.login_verify =
        game::login_verify::LoginVerifyConfigLoader::parse(layers.root);

    if (config.login_verify.accounts_file.is_relative()) {
        config.login_verify.accounts_file =
            config_root / config.login_verify.accounts_file;
    }
    return config;
}

LayeredConfigLoader::QueueServiceConfig
LayeredConfigLoader::load_queue(
    const std::filesystem::path& config_root,
    std::string_view service_name,
    const CliOverrides& overrides) {
    MergedLayers layers;
    merged_layers(layers, config_root, service_name, overrides);

    QueueServiceConfig config;
    // 宿主级节段经由既有解析器(logging/service_discovery/metrics);
    // parse 要求 transports 为表(可为空):未配置时补空表。
    call_merger<sol::table>(
        layers.runtime, "ensure_table", layers.root, "transports");
    config.host = game::gateway::GatewayConfigLoader::parse(layers.root);
    config.queue = game::queue::QueueConfigLoader::parse(layers.root);
    return config;
}

}  // namespace realm::service_host
