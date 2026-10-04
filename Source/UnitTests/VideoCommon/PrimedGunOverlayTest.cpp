// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "VideoCommon/VR/PrimedGunOverlayCommon.h"

TEST(PrimedGunOverlay, CompileProgressVisibilityAndGeneration)
{
  Common::VR::PrimedGunVrOverlayState state{};
  EXPECT_FALSE(PrimedGun::Overlay::PromptVisible(state));
  EXPECT_EQ(PrimedGun::Overlay::PromptGeneration(state), 1u);
  state.compile_progress_visible = true;
  EXPECT_TRUE(PrimedGun::Overlay::PromptVisible(state));
  EXPECT_EQ(PrimedGun::Overlay::PromptGeneration(state), 1000u);
  state.compile_total = 200;
  state.compile_completed = 100;
  EXPECT_EQ(PrimedGun::Overlay::PromptGeneration(state), 1050u);
  state.compile_progress_visible = false;
  EXPECT_FALSE(PrimedGun::Overlay::PromptVisible(state));
  EXPECT_EQ(PrimedGun::Overlay::PromptGeneration(state), 1u);
  state.prompt_visible = true;
  EXPECT_TRUE(PrimedGun::Overlay::PromptVisible(state));
}

TEST(PrimedGunOverlay, CompileProgressPixelsClampToTotal)
{
  const auto empty = PrimedGun::Overlay::BuildCompileProgressPixels(1024, 384, 0, 0);
  const auto half = PrimedGun::Overlay::BuildCompileProgressPixels(1024, 384, 50, 100);
  const auto full = PrimedGun::Overlay::BuildCompileProgressPixels(1024, 384, 100, 100);
  const auto over = PrimedGun::Overlay::BuildCompileProgressPixels(1024, 384, 200, 100);
  EXPECT_EQ(empty.size(), 1024u * 384u);
  EXPECT_NE(empty, half);
  EXPECT_NE(half, full);
  EXPECT_EQ(full, over);
}

TEST(PrimedGunOverlay, CompilePanelRequiresValidTracking)
{
  Common::VR::PrimedGunVrOverlayState state{};
  XrPosef pose{};
  EXPECT_FALSE(PrimedGun::Overlay::CompileProgressPanelPose(state, &pose));
  state.compile_progress_visible = true;
  EXPECT_FALSE(PrimedGun::Overlay::CompileProgressPanelPose(state, &pose));
}
