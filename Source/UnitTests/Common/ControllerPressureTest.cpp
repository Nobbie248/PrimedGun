// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <limits>

#include <gtest/gtest.h>

#include "Common/VR/ControllerPressure.h"

TEST(ControllerPressure, RequiresPressureAndReleasesAtRest)
{
  for (const float threshold : {0.05f, 0.5f, 1.0f})
  {
    EXPECT_FALSE(Common::VR::UpdatePressureButton(0.0f, threshold, false));
    EXPECT_FALSE(Common::VR::UpdatePressureButton(0.0f, threshold, true));
    EXPECT_FALSE(Common::VR::UpdatePressureButton(threshold - 0.01f, threshold, false));
    EXPECT_TRUE(Common::VR::UpdatePressureButton(threshold, threshold, false));
  }
}

TEST(ControllerPressure, HysteresisPreventsRepeatedPresses)
{
  bool pressed = Common::VR::UpdatePressureButton(0.6f, 0.5f, false);
  ASSERT_TRUE(pressed);
  for (const float force : {0.49f, 0.51f, 0.45f, 0.40f})
  {
    pressed = Common::VR::UpdatePressureButton(force, 0.5f, pressed);
    EXPECT_TRUE(pressed);
  }
  pressed = Common::VR::UpdatePressureButton(0.39f, 0.5f, pressed);
  EXPECT_FALSE(pressed);
  EXPECT_FALSE(Common::VR::UpdatePressureButton(0.45f, 0.5f, pressed));
}

TEST(ControllerPressure, ValidatesSavedThresholdsAndInvalidSamples)
{
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  for (const float threshold : {-1.0f, 0.0f, 2.0f, nan, inf})
  {
    EXPECT_FALSE(Common::VR::UpdatePressureButton(0.0f, threshold, false));
    EXPECT_TRUE(Common::VR::UpdatePressureButton(1.0f, threshold, false));
  }
  EXPECT_FALSE(Common::VR::UpdatePressureButton(0.49f, nan, false));
  EXPECT_TRUE(Common::VR::UpdatePressureButton(0.5f, nan, false));
  for (const float force : {-1.0f, nan, inf})
    EXPECT_FALSE(Common::VR::UpdatePressureButton(force, 0.5f, true));
}

TEST(ControllerPressure, HandsAndLiveThresholdsAreIndependent)
{
  const bool left_pressed = Common::VR::UpdatePressureButton(0.6f, 0.5f, false);
  const bool right_pressed = Common::VR::UpdatePressureButton(0.1f, 0.5f, false);
  EXPECT_TRUE(left_pressed);
  EXPECT_FALSE(right_pressed);
  EXPECT_FALSE(Common::VR::UpdatePressureButton(0.6f, 0.9f, left_pressed));
  EXPECT_TRUE(Common::VR::UpdatePressureButton(0.2f, 0.15f, right_pressed));
}
