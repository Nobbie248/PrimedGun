// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cmath>

namespace Common::VR
{
inline bool UpdatePressureButton(float force, float press_threshold, bool was_pressed)
{
  if (!std::isfinite(force))
    return false;

  press_threshold = std::isfinite(press_threshold) ?
                        std::clamp(press_threshold, 0.05f, 1.0f) :
                        0.5f;
  // A lower release threshold prevents noisy pressure readings from repeating a press.
  const float threshold = was_pressed ? press_threshold * 0.8f : press_threshold;
  return std::clamp(force, 0.0f, 1.0f) >= threshold;
}
}  // namespace Common::VR
