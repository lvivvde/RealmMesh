#include "realmmesh/game/realm/training_rule.hpp"

#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace realm::game::realm {
namespace {

constexpr std::string_view module_name = "realm_training_rule";

[[nodiscard]] lua_Integer lua_experience(std::uint64_t exp) {
    if (exp > static_cast<std::uint64_t>(
                  std::numeric_limits<lua_Integer>::max())) {
        throw std::runtime_error("experience is out of Lua integer range");
    }
    return static_cast<lua_Integer>(exp);
}

}  // namespace

TrainingRule::TrainingRule(const std::filesystem::path& file)
    : file_(file.string()) {
    std::ifstream input(file, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot read training rule " + file_);
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    source_ = std::move(contents).str();
    build();
    // 启动时以经验 0 试调一次,返回值形状不对即启动失败,而不是等到
    // 第一个玩家训练时才暴露。
    try {
        static_cast<void>(train(0));
        static_cast<void>(level(0));
    } catch (const std::exception& error) {
        throw std::runtime_error(
            "training rule " + file_ + " is invalid: " + error.what());
    }
}

TrainingRule::~TrainingRule() = default;

void TrainingRule::build() {
    auto runtime = std::make_unique<scripting::LuaRuntime>();
    std::string error;
    if (!runtime->load_module_source(
            std::string(module_name), source_, &error)) {
        throw std::runtime_error(
            "failed to load training rule " + file_ + ": " + error);
    }
    const auto exports = runtime->module(module_name);
    for (const char* function : {"train", "level"}) {
        if (!exports[function].get<sol::object>().is<sol::protected_function>()) {
            throw std::runtime_error(
                "training rule " + file_ + " must export " + function);
        }
    }
    runtime_ = std::move(runtime);
    owner_ = std::this_thread::get_id();
}

scripting::LuaRuntime& TrainingRule::runtime() {
    if (std::this_thread::get_id() != owner_) build();
    return *runtime_;
}

std::optional<std::uint64_t> TrainingRule::train(std::uint64_t exp) {
    const auto result = runtime().call<sol::object>(
        module_name, "train", lua_experience(exp));
    if (result == sol::lua_nil) return std::nullopt;
    if (!result.is<lua_Integer>() || result.as<lua_Integer>() < 0 ||
        static_cast<std::uint64_t>(result.as<lua_Integer>()) <= exp) {
        throw std::runtime_error(
            "training rule must return nil or an integer above the current "
            "experience");
    }
    return static_cast<std::uint64_t>(result.as<lua_Integer>());
}

std::uint32_t TrainingRule::level(std::uint64_t exp) {
    const auto result = runtime().call<sol::object>(
        module_name, "level", lua_experience(exp));
    if (!result.is<lua_Integer>() || result.as<lua_Integer>() < 1 ||
        result.as<lua_Integer>() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("training rule level must be a positive integer");
    }
    return static_cast<std::uint32_t>(result.as<lua_Integer>());
}

}  // namespace realm::game::realm
