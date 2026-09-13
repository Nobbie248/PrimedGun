// Copyright 2026 PrimedGun Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <unordered_map>

#include <gtest/gtest.h>

#include "VideoCommon/GenerationCounterTable.h"

TEST(GenerationCounterTable, CountsOccurrencesPerKey)
{
  VideoCommon::GenerationCounterTable table;
  EXPECT_EQ(table.Increment(0x1234), 0);
  EXPECT_EQ(table.Increment(0x1234), 1);
  EXPECT_EQ(table.Increment(0x1234), 2);
  EXPECT_EQ(table.Increment(0), 0);
  EXPECT_EQ(table.Increment(0), 1);
  EXPECT_EQ(table.Increment(0x1234), 3);
  EXPECT_EQ(table.GetLiveCount(), 2u);
}

TEST(GenerationCounterTable, ClearResetsCountsWithoutReallocating)
{
  VideoCommon::GenerationCounterTable table;
  for (u64 key = 0; key < 100; ++key)
    table.Increment(key * 0x9e3779b97f4a7c15ULL);
  const size_t capacity = table.GetCapacity();
  table.Clear();
  EXPECT_EQ(table.GetLiveCount(), 0u);
  EXPECT_EQ(table.GetCapacity(), capacity);
  for (u64 key = 0; key < 100; ++key)
    EXPECT_EQ(table.Increment(key * 0x9e3779b97f4a7c15ULL), 0);
  EXPECT_EQ(table.GetCapacity(), capacity);
}

TEST(GenerationCounterTable, MatchesUnorderedMapAcrossGrowthAndFrames)
{
  VideoCommon::GenerationCounterTable table;
  std::unordered_map<u64, int> reference;
  u64 state = 0x243f6a8885a308d3ULL;
  for (int frame = 0; frame < 6; ++frame)
  {
    // Many more distinct keys than the initial capacity, with repeats.
    for (int i = 0; i < 5000; ++i)
    {
      state ^= state << 13;
      state ^= state >> 7;
      state ^= state << 17;
      const u64 key = (state % 1500) * 0x100000001b3ULL;
      EXPECT_EQ(table.Increment(key), reference[key]++);
    }
    EXPECT_EQ(table.GetLiveCount(), reference.size());
    table.Clear();
    reference.clear();
  }
}

TEST(GenerationCounterTable, SurvivesManyClears)
{
  VideoCommon::GenerationCounterTable table;
  table.Increment(7);
  for (int i = 0; i < 100000; ++i)
    table.Clear();
  EXPECT_EQ(table.Increment(7), 0);
  EXPECT_EQ(table.Increment(7), 1);
}
