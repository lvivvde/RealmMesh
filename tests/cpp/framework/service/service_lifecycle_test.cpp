#include "realmmesh/service/service_lifecycle.hpp"

#include <gtest/gtest.h>

namespace realm::service {
namespace {

TEST(ServiceLifecycleTest, StartsInCreatedState) {
    const ServiceLifecycle lifecycle;

    EXPECT_EQ(lifecycle.state(), ServiceState::Created);
}

TEST(ServiceLifecycleTest, FollowsNormalStartupAndShutdownSequence) {
    ServiceLifecycle lifecycle;

    EXPECT_TRUE(lifecycle.transition_to(ServiceState::Initializing));
    EXPECT_TRUE(lifecycle.transition_to(ServiceState::Ready));
    EXPECT_TRUE(lifecycle.transition_to(ServiceState::Stopping));
    EXPECT_TRUE(lifecycle.transition_to(ServiceState::Stopped));
    EXPECT_EQ(lifecycle.state(), ServiceState::Stopped);
}

TEST(ServiceLifecycleTest, RejectsSkippedAndBackwardTransitions) {
    ServiceLifecycle lifecycle;

    EXPECT_FALSE(lifecycle.transition_to(ServiceState::Ready));
    EXPECT_EQ(lifecycle.state(), ServiceState::Created);

    ASSERT_TRUE(lifecycle.transition_to(ServiceState::Initializing));
    EXPECT_FALSE(lifecycle.transition_to(ServiceState::Created));
    EXPECT_EQ(lifecycle.state(), ServiceState::Initializing);
}

TEST(ServiceLifecycleTest, AllowsShutdownWhileInitializing) {
    ServiceLifecycle lifecycle;

    ASSERT_TRUE(lifecycle.transition_to(ServiceState::Initializing));
    EXPECT_TRUE(lifecycle.transition_to(ServiceState::Stopping));
    EXPECT_TRUE(lifecycle.transition_to(ServiceState::Stopped));
}

TEST(ServiceLifecycleTest, StoppedStateIsTerminal) {
    ServiceLifecycle lifecycle;

    ASSERT_TRUE(lifecycle.transition_to(ServiceState::Initializing));
    ASSERT_TRUE(lifecycle.transition_to(ServiceState::Ready));
    ASSERT_TRUE(lifecycle.transition_to(ServiceState::Stopping));
    ASSERT_TRUE(lifecycle.transition_to(ServiceState::Stopped));

    EXPECT_FALSE(lifecycle.transition_to(ServiceState::Initializing));
    EXPECT_EQ(lifecycle.state(), ServiceState::Stopped);
}

}  // namespace
}  // namespace realm::service
