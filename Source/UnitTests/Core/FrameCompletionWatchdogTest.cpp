// Copyright 2026 PrimedGun Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "Core/PrimedGun/FrameCompletionWatchdog.h"

using PrimedGun::FrameCompletionWatchdog;
using Action = FrameCompletionWatchdog::Action;

TEST(FrameCompletionWatchdog, NormalFramesAndIdleNeverTrigger)
{
  FrameCompletionWatchdog watchdog;
  for (u32 i = 0; i < 1000; ++i)
    EXPECT_EQ(watchdog.Update(true, i), Action::None);
  for (int i = 0; i < 1000; ++i)
    EXPECT_EQ(watchdog.Update(false, 999), Action::None);
}

TEST(FrameCompletionWatchdog, RetriesBeforePausing)
{
  FrameCompletionWatchdog watchdog;
  EXPECT_EQ(watchdog.Update(true, 7), Action::None);
  for (int i = 1; i < 360; ++i)
  {
    const auto expected = i >= 120 && i % 60 == 0 ? Action::Retry : Action::None;
    EXPECT_EQ(watchdog.Update(true, 7), expected) << i;
  }
  EXPECT_EQ(watchdog.Update(true, 7), Action::Pause);
  // Resume alone must not permit the game to hit its fatal watchdog.
  for (int i = 0; i < 1000; ++i)
    EXPECT_EQ(watchdog.Update(true, 7), Action::Pause);
}

TEST(FrameCompletionWatchdog, CompletionOrNewFrameClearsStall)
{
  for (const bool completed : {false, true})
  {
    FrameCompletionWatchdog watchdog;
    for (int i = 0; i <= 360; ++i)
      watchdog.Update(true, 7);
    EXPECT_EQ(watchdog.Update(!completed, completed ? 7 : 8), Action::None);
    EXPECT_FALSE(watchdog.IsStalled());
    EXPECT_EQ(watchdog.Update(true, 8), Action::None);
  }
}

TEST(FrameCompletionWatchdog, ResetAndFrameCounterWrapClearStall)
{
  FrameCompletionWatchdog watchdog;
  for (int i = 0; i < 300; ++i)
    watchdog.Update(true, 0xffffffffu);
  EXPECT_EQ(watchdog.Update(true, 0), Action::None);
  EXPECT_FALSE(watchdog.IsStalled());
  for (int i = 0; i < 300; ++i)
    watchdog.Update(true, 0);
  watchdog = {};
  EXPECT_EQ(watchdog.Update(true, 0), Action::None);
  EXPECT_FALSE(watchdog.IsStalled());
}
