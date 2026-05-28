// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef ENABLE_VR

#include <array>
#include <cstdint>
#include <mutex>

namespace Common::VR
{
struct OpenXRPoseState
{
  bool valid = false;
  std::array<float, 3> position{};
  std::array<float, 4> orientation{0.0f, 0.0f, 0.0f, 1.0f};
};

struct OpenXRVelocityState
{
  bool linear_valid = false;
  bool angular_valid = false;
  std::array<float, 3> linear{};
  std::array<float, 3> angular{};
};

struct OpenXRControllerState
{
  bool connected = false;
  bool primary_button = false;
  bool secondary_button = false;
  bool menu_button = false;
  bool trigger_button = false;
  bool squeeze_button = false;
  bool thumbstick_button = false;
  float trigger_value = 0.0f;
  float squeeze_value = 0.0f;
  float thumbstick_x = 0.0f;
  float thumbstick_y = 0.0f;
  OpenXRPoseState aim_pose;
  OpenXRPoseState grip_pose;
  OpenXRVelocityState grip_velocity;
};

struct OpenXRInputSnapshot
{
  std::array<OpenXRControllerState, 2> controllers{};
  OpenXRPoseState head_pose;  // HMD head orientation for IR pointer reference
  bool runtime_active = false;
  uint64_t generation = 0;
};

struct OpenXRHapticsState
{
  std::array<float, 2> amplitude{};
};

class OpenXRInputState final
{
public:
  static OpenXRInputSnapshot GetSnapshot()
  {
    std::lock_guard lk(s_state_mutex);
    return s_state;
  }

  static void SetControllers(const std::array<OpenXRControllerState, 2>& controllers,
                             bool runtime_active,
                             const OpenXRPoseState& head_pose = {})
  {
    std::lock_guard lk(s_state_mutex);
    s_state.controllers = controllers;
    s_state.head_pose = head_pose;
    s_state.runtime_active = runtime_active;
    ++s_state.generation;
  }

  static void Reset()
  {
    std::lock_guard lk(s_state_mutex);
    s_state = {};
    s_haptics = {};
    ++s_state.generation;
  }

  static OpenXRHapticsState GetHaptics()
  {
    std::lock_guard lk(s_state_mutex);
    return s_haptics;
  }

  static void SetRumble(float amplitude)
  {
    SetRumble(amplitude, amplitude);
  }

  static void SetRumble(float left_amplitude, float right_amplitude)
  {
    std::lock_guard lk(s_state_mutex);
    s_haptics.amplitude[0] = Clamp01(left_amplitude);
    s_haptics.amplitude[1] = Clamp01(right_amplitude);
  }

  static void SetRumbleForHand(std::size_t hand_index, float amplitude)
  {
    if (hand_index >= s_haptics.amplitude.size())
      return;

    std::lock_guard lk(s_state_mutex);
    s_haptics.amplitude[hand_index] = Clamp01(amplitude);
  }

private:
  static float Clamp01(float value)
  {
    if (value < 0.0f)
      return 0.0f;
    if (value > 1.0f)
      return 1.0f;
    return value;
  }

  static inline std::mutex s_state_mutex;
  static inline OpenXRInputSnapshot s_state{};
  static inline OpenXRHapticsState s_haptics{};
};
}  // namespace Common::VR

#endif  // ENABLE_VR
