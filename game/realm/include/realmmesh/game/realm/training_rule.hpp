#pragma once

#include "realmmesh/scripting/lua_runtime.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace realm::game::realm {

/// 训练规则的 Lua 绑定(#93)。Realm 启动时从配置路径读取并校验一次,
/// 读取、编译或形状校验失败即抛 std::runtime_error,令启动失败;不热更。
/// 规则是纯函数,只在帧线程调用,调用方保证不并发。
///
/// LuaRuntime 绑定构造线程,而帧线程不一定是构造线程(MeshHost 可在一个
/// 线程构造、在另一个线程 tick)。换线程调用时用同一份已校验源码在当前
/// 线程重建运行时,不重读文件,行为不变。
class TrainingRule final {
public:
    explicit TrainingRule(const std::filesystem::path& file);
    ~TrainingRule();

    TrainingRule(const TrainingRule&) = delete;
    TrainingRule& operator=(const TrainingRule&) = delete;

    /// 一次训练后的经验;已满级返回 nullopt。规则返回值不是大于 exp 的
    /// 整数时抛 std::runtime_error。
    [[nodiscard]] std::optional<std::uint64_t> train(std::uint64_t exp);
    /// 经验对应的等级(≥ 1)。
    [[nodiscard]] std::uint32_t level(std::uint64_t exp);

private:
    [[nodiscard]] scripting::LuaRuntime& runtime();
    void build();

    std::string file_;
    std::string source_;
    std::thread::id owner_;
    std::unique_ptr<scripting::LuaRuntime> runtime_;
};

}  // namespace realm::game::realm
