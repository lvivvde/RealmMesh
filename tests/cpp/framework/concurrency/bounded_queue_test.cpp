#include "realmmesh/concurrency/bounded_queue.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

namespace realm::concurrency {
namespace {

TEST(BoundedQueueTest, RejectsZeroCapacity) {
    EXPECT_THROW(BoundedQueue<int>(0), std::invalid_argument);
}

TEST(BoundedQueueTest, PreservesFifoOrderAndRejectsOverflow) {
    BoundedQueue<std::string> queue(2);

    EXPECT_TRUE(queue.try_push("first"));
    EXPECT_TRUE(queue.try_push("second"));
    EXPECT_FALSE(queue.try_push("overflow"));
    EXPECT_EQ(queue.size(), 2U);

    const auto first = queue.try_pop();
    const auto second = queue.try_pop();
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*first, "first");
    EXPECT_EQ(*second, "second");
    EXPECT_FALSE(queue.try_pop().has_value());
}

}  // namespace
}  // namespace realm::concurrency
