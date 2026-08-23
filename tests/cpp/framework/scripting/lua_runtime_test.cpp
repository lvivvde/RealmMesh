#include "realmmesh/scripting/lua_runtime.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>

namespace realm::scripting {
namespace {

class TemporaryLuaFile final {
public:
    TemporaryLuaFile()
        : path_(
              std::filesystem::temp_directory_path() /
              ("realmmesh_lua_runtime_" +
               std::to_string(std::chrono::steady_clock::now()
                                  .time_since_epoch()
                                  .count()) +
               ".lua")) {}

    ~TemporaryLuaFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    void write(std::string_view source) const {
        std::ofstream output(path_, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(output.good());
        output << source;
        ASSERT_TRUE(output.good());
    }

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

TEST(LuaRuntimeTest, CallsLuaAndBoundCppFunctions) {
    LuaRuntime runtime;
    runtime.set_function("cpp_double", [](int value) {
        return value * 2;
    });
    std::string error;

    ASSERT_TRUE(runtime.load_module_source(
        "combat",
        "return { damage = function(base) return cpp_double(base) + 1 end }",
        &error))
        << error;

    EXPECT_EQ(runtime.call<int>("combat", "damage", 20), 41);
}

TEST(LuaRuntimeTest, KeepsTheOldModuleWhenAHotReloadFails) {
    LuaRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.load_module_source(
        "rules", "return { value = function() return 1 end }", &error));

    EXPECT_FALSE(runtime.load_module_source(
        "rules", "return { value = function( }", &error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(runtime.call<int>("rules", "value"), 1);

    ASSERT_TRUE(runtime.load_module_source(
        "rules", "return { value = function() return 2 end }", &error));
    EXPECT_EQ(runtime.call<int>("rules", "value"), 2);
}

TEST(LuaRuntimeTest, ReloadsChangedFilesAndRollsBackInvalidChanges) {
    TemporaryLuaFile file;
    file.write("return { value = function() return 1 end }");

    LuaRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.load_module("rules", file.path(), &error)) << error;
    const auto initial_write_time =
        std::filesystem::last_write_time(file.path());

    file.write("return { value = function() return 2 end }");
    std::filesystem::last_write_time(
        file.path(), initial_write_time + std::chrono::seconds(2));
    auto report = runtime.reload_changed();
    EXPECT_EQ(report.reloaded, std::vector<std::string>{"rules"});
    EXPECT_TRUE(report.failures.empty());
    EXPECT_EQ(runtime.call<int>("rules", "value"), 2);

    file.write("return { value = function( }");
    std::filesystem::last_write_time(
        file.path(), initial_write_time + std::chrono::seconds(4));
    report = runtime.reload_changed();
    EXPECT_TRUE(report.reloaded.empty());
    ASSERT_EQ(report.failures.size(), 1U);
    EXPECT_EQ(report.failures.front().module, "rules");
    EXPECT_FALSE(report.failures.front().error.empty());
    EXPECT_EQ(runtime.call<int>("rules", "value"), 2);

    report = runtime.reload_changed();
    EXPECT_TRUE(report.reloaded.empty());
    EXPECT_TRUE(report.failures.empty());
}

TEST(LuaRuntimeTest, RejectsAccessFromAnotherThread) {
    LuaRuntime runtime;
    auto result = std::async(std::launch::async, [&runtime] {
        try {
            static_cast<void>(runtime.has_module("anything"));
            return false;
        } catch (const std::logic_error&) {
            return true;
        }
    });

    EXPECT_TRUE(result.get());
}

TEST(LuaRuntimeTest, DoesNotExposeFileLoadingLibraries) {
    LuaRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.load_module_source(
        "sandbox",
        "return { has_os = os ~= nil, has_io = io ~= nil, "
        "has_package = package ~= nil, has_dofile = dofile ~= nil, "
        "has_loadfile = loadfile ~= nil }",
        &error))
        << error;
    const auto exports = runtime.module("sandbox");

    EXPECT_FALSE(exports.get<bool>("has_os"));
    EXPECT_FALSE(exports.get<bool>("has_io"));
    EXPECT_FALSE(exports.get<bool>("has_package"));
    EXPECT_FALSE(exports.get<bool>("has_dofile"));
    EXPECT_FALSE(exports.get<bool>("has_loadfile"));
}

}  // namespace
}  // namespace realm::scripting
