#include "ring_buffer.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

using blackbox::RingBuffer;

namespace {

std::vector<int> contents(const RingBuffer<int>& buffer)
{
    std::vector<int> out;
    for (std::size_t i = 0; i < buffer.size(); ++i) out.push_back(buffer[i]);
    return out;
}

} // namespace

TEST(RingBuffer, StartsEmpty)
{
    RingBuffer<int> buffer(3);
    EXPECT_EQ(buffer.size(), 0u);
    EXPECT_EQ(buffer.capacity(), 3u);
    EXPECT_EQ(buffer.total_pushed(), 0u);
}

TEST(RingBuffer, KeepsEverythingBelowCapacity)
{
    RingBuffer<int> buffer(5);
    buffer.push(1);
    buffer.push(2);
    buffer.push(3);
    EXPECT_EQ(contents(buffer), (std::vector<int>{1, 2, 3}));
}

TEST(RingBuffer, DropsOldestWhenFull)
{
    RingBuffer<int> buffer(3);
    for (int i = 1; i <= 3; ++i) buffer.push(i);
    EXPECT_EQ(contents(buffer), (std::vector<int>{1, 2, 3}));

    buffer.push(4);
    EXPECT_EQ(contents(buffer), (std::vector<int>{2, 3, 4}));
}

TEST(RingBuffer, StaysOrderedAfterManyWraparounds)
{
    RingBuffer<int> buffer(4);
    for (int i = 1; i <= 1003; ++i) buffer.push(i);
    EXPECT_EQ(contents(buffer), (std::vector<int>{1000, 1001, 1002, 1003}));
}

TEST(RingBuffer, CapacityZeroOnlyCounts)
{
    RingBuffer<int> buffer(0);
    buffer.push(1);
    buffer.push(2);
    EXPECT_EQ(buffer.size(), 0u);
    EXPECT_EQ(buffer.total_pushed(), 2u);
}

TEST(RingBuffer, CapacityOneHoldsLatest)
{
    RingBuffer<int> buffer(1);
    buffer.push(1);
    buffer.push(2);
    buffer.push(3);
    EXPECT_EQ(contents(buffer), (std::vector<int>{3}));
}

TEST(RingBuffer, TotalPushedCountsOverwrittenElements)
{
    RingBuffer<int> buffer(2);
    for (int i = 0; i < 10; ++i) buffer.push(i);
    EXPECT_EQ(buffer.size(), 2u);
    EXPECT_EQ(buffer.total_pushed(), 10u);
}

TEST(RingBuffer, WorksWithMoveOnlyTypes)
{
    RingBuffer<std::unique_ptr<int>> buffer(2);
    for (int i = 0; i < 3; ++i) buffer.push(std::make_unique<int>(i));
    EXPECT_EQ(*buffer[0], 1);
    EXPECT_EQ(*buffer[1], 2);
}
