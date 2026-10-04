#include "realmmesh/service_host/startup_topology.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace realm::service_host {
namespace {

/// 每个用例一个临时配置根，只写 main.config。
class LoadTopologyFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path() /
                ("realm_load_topology_" +
                 std::string(::testing::UnitTest::GetInstance()
                                 ->current_test_info()
                                 ->name()));
        std::filesystem::create_directories(root_);
    }
    void TearDown() override { std::filesystem::remove_all(root_); }

    [[nodiscard]] const std::filesystem::path& write(
        const std::string& source) const {
        std::ofstream(root_ / "main.config") << source;
        return root_;
    }

    std::filesystem::path root_;
};

TEST(LoadTopologyTest, ShippedMainConfigListsServicesInDeclarationOrder) {
    const auto specs = load_topology(
        std::filesystem::path(REALMMESH_SOURCE_DIR) / "configs");

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

TEST_F(LoadTopologyFileTest, OmittedFieldsDefaultToNoDependenciesAndNotEntry) {
    const auto specs = load_topology(write(R"(return {
        services = {
            { name = "realm" },
            { name = "gateway", depends_on = { "realm", "queue" }, entry = true },
        },
    })"));

    ASSERT_EQ(specs.size(), std::size_t{2});
    EXPECT_EQ(specs[0].name, "realm");
    EXPECT_TRUE(specs[0].depends_on.empty());
    EXPECT_FALSE(specs[0].entry);
    EXPECT_EQ(
        specs[1].depends_on, (std::vector<std::string>{"realm", "queue"}));
    EXPECT_TRUE(specs[1].entry);
}

TEST_F(LoadTopologyFileTest, MissingFileIsALoadFailure) {
    EXPECT_THROW(
        static_cast<void>(load_topology(root_ / "absent")),
        std::runtime_error);
}

TEST_F(LoadTopologyFileTest, LuaErrorIsALoadFailure) {
    EXPECT_THROW(
        static_cast<void>(load_topology(write("error('boom')"))),
        std::runtime_error);
}

TEST_F(LoadTopologyFileTest, RejectsMissingOrEmptyServicesTable) {
    EXPECT_THROW(
        static_cast<void>(load_topology(write("return {}"))),
        std::invalid_argument);
    EXPECT_THROW(
        static_cast<void>(load_topology(write("return { services = {} }"))),
        std::invalid_argument);
    EXPECT_THROW(
        static_cast<void>(load_topology(write("return { services = 1 }"))),
        std::invalid_argument);
}

TEST_F(LoadTopologyFileTest, RejectsMalformedEntries) {
    for (const char* services : {
             R"({ "realm" })",
             R"({ {} })",
             R"({ { name = 7 } })",
             R"({ { name = "gateway", depends_on = "realm" } })",
             R"({ { name = "gateway", depends_on = { 1 } } })",
             R"({ { name = "gateway", entry = "yes" } })",
         }) {
        SCOPED_TRACE(services);
        EXPECT_THROW(
            static_cast<void>(load_topology(
                write(std::string("return { services = ") + services + " }"))),
            std::invalid_argument);
    }
}

}  // namespace
}  // namespace realm::service_host
