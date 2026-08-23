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

}  // namespace

game::gateway::GatewayConfig LayeredConfigLoader::load(
    const std::filesystem::path& config_root,
    std::string_view service_name,
    const CliOverrides& overrides) {
    scripting::LuaRuntime runtime;
    std::string error;
    if (!runtime.load_module_source("config_merger", merge_script, &error)) {
        throw std::runtime_error("failed to load merge script: " + error);
    }

    // 服务层表:services/<name>.lua 缺失或语法错误 → runtime_error。
    const std::filesystem::path service_file =
        config_root / "services" / (std::string(service_name) + ".lua");
    if (!runtime.load_module("service_config", service_file, &error)) {
        throw std::runtime_error(
            "failed to load service configuration: " + error);
    }
    sol::table service_table = runtime.module("service_config");

    // 公共层按文件名序相互深合并(后文件胜),服务层最后并入(服务层值胜)。
    sol::table root;
    bool has_common = false;
    for (const auto& file : lua_files(config_root / "common")) {
        if (!runtime.load_module("common_config", file, &error)) {
            throw std::runtime_error(
                "failed to load common configuration: " + error);
        }
        if (has_common) {
            call_merger<sol::table>(
                runtime, "merge", root, runtime.module("common_config"));
        } else {
            root = runtime.module("common_config");
            has_common = true;
        }
    }
    if (has_common) {
        root = call_merger<sol::table>(runtime, "merge", root, service_table);
    } else {
        root = service_table;
    }

    // CLI 覆盖:非空项写入 service_discovery,优先级最高。
    sol::table discovery = call_merger<sol::table>(
        runtime, "ensure_table", root, "service_discovery");
    if (overrides.instance_id.has_value()) {
        discovery["instance_id"] = *overrides.instance_id;
    }
    if (overrides.node_id.has_value()) {
        discovery["node_id"] = *overrides.node_id;
    }
    if (overrides.zone.has_value()) {
        discovery["zone"] = *overrides.zone;
    }

    // 实例身份:CLI 覆盖 > 服务表 instance_id > 默认 "<svc>-01"。
    std::string instance = string_field(discovery, "instance_id");
    if (instance.empty()) {
        instance = std::string(service_name) + "-01";
    }

    // 日志身份:file_path 按实例生成,service_name 固定为服务名。
    sol::table logging =
        call_merger<sol::table>(runtime, "ensure_table", root, "logging");
    logging["service_name"] = std::string(service_name);
    logging["file_path"] =
        (config_root / "logs" / std::string(service_name) /
         (std::string(service_name) + "-" + instance + ".jsonl"))
            .string();

    // parse 要求 transports 为表(可为空):未配置时补空表。
    call_merger<sol::table>(runtime, "ensure_table", root, "transports");

    return game::gateway::GatewayConfigLoader::parse(root);
}

}  // namespace realm::service_host
