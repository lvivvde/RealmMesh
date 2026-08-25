#include "realmmesh/service_host/startup_topology.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

namespace realm::service_host {
namespace {

TEST(StartupTopologyTest, IndependentServicesShareFirstWave) {
    const StartupTopology topology(std::vector<ServiceSpec>{
        {"realm", {}, false},
        {"friends", {}, false},
        {"gateway", {"realm", "friends"}, true},
    });
    EXPECT_EQ(
        topology.waves().at(0), (std::vector<std::string>{"realm", "friends"}));
    EXPECT_EQ(topology.waves().at(1), std::vector<std::string>{"gateway"});
    EXPECT_EQ(
        topology.shutdown_order(),
        (std::vector<std::string>{"gateway", "friends", "realm"}));
}

TEST(StartupTopologyTest, ChainSplitsIntoWaves) {
    const StartupTopology topology(std::vector<ServiceSpec>{
        {"login", {"realm"}, false},
        {"realm", {}, false},
        {"gateway", {"login"}, true},
    });
    EXPECT_EQ(topology.waves().size(), std::size_t{3});
    EXPECT_EQ(topology.waves().at(2), std::vector<std::string>{"gateway"});
}

TEST(StartupTopologyTest, EntryServiceIsAlwaysInTheFinalWave) {
    const StartupTopology topology(std::vector<ServiceSpec>{
        {"realm", {}, false},
        {"login", {"realm"}, false},
        {"gateway", {"login"}, true},
        {"a", {}, false},
        {"b", {"a"}, false},
        {"c", {"b"}, false},
        {"d", {"c"}, false},
    });

    ASSERT_EQ(topology.waves().size(), std::size_t{5});
    EXPECT_EQ(topology.waves().back(), std::vector<std::string>{"gateway"});
    EXPECT_EQ(topology.waves().at(3), std::vector<std::string>{"d"});
}

TEST(StartupTopologyTest, RejectsServicesDependingOnAnEntry) {
    EXPECT_THROW(
        StartupTopology(std::vector<ServiceSpec>{
            {"gateway", {}, true},
            {"late", {"gateway"}, false},
        }),
        std::invalid_argument);
}

TEST(StartupTopologyTest, CycleThrows) {
    EXPECT_THROW(
        StartupTopology(
            std::vector<ServiceSpec>{{"a", {"b"}, false}, {"b", {"a"}, false}}),
        std::invalid_argument);
}

TEST(StartupTopologyTest, UnknownDependencyThrows) {
    EXPECT_THROW(
        StartupTopology(std::vector<ServiceSpec>{{"a", {"ghost"}, false}}),
        std::invalid_argument);
}

}  // namespace
}  // namespace realm::service_host
