// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef ENABLE_VR

#include <array>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>

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
  bool trackpad_button = false;
  bool trackpad_touch = false;
  float trigger_value = 0.0f;
  float squeeze_value = 0.0f;
  float squeeze_force = 0.0f;
  float thumbstick_x = 0.0f;
  float thumbstick_y = 0.0f;
  float trackpad_x = 0.0f;
  float trackpad_y = 0.0f;
  float trackpad_force = 0.0f;
  OpenXRPoseState aim_pose;
  OpenXRPoseState grip_pose;
  OpenXRVelocityState grip_velocity;
};

struct OpenXRInputSnapshot
{
  std::array<OpenXRControllerState, 2> controllers{};
  std::array<std::string, 2> interaction_profiles{};
  OpenXRPoseState head_pose;  // HMD head orientation for IR pointer reference
  std::array<float, 3> tracking_origin_position{};  // Offset removed from poses for game tracking.
  bool runtime_active = false;
  bool session_focused = false;
  uint64_t generation = 0;
};

struct OpenXRHapticsState
{
  std::array<float, 2> amplitude{};
};

struct PrimedGunVrOverlayState
{
  bool prompt_visible = false;
  bool menu_visible = false;
  bool weapon_panel_visible = false;
  bool menu_pointer_active = false;
  bool saved_notice = false;
  uint32_t generation = 0;
  uint32_t tab = 0;
  uint32_t selected_index = 0;
  uint32_t item_count = 0;
  uint32_t cannon_texture_slot = 0;
  bool cannon_texture_notice = false;
  uint32_t weapon_selected_index = 0;
  std::array<float, 3> weapon_panel_position{};
  std::array<float, 4> weapon_panel_orientation{0.0f, 0.0f, 0.0f, 1.0f};
  float pointer_x = 0.5f;
  float pointer_y = 0.5f;
  float world_scale = 1.5f;
  bool use_right_hand = true;
  bool require_trigger = false;
  float trigger_threshold = 0.5f;
  bool primegun_grip_inputs_enabled = true;
  bool primegun_grip_inputs_use_trackpad = false;
  float primegun_trackpad_press_threshold = 0.5f;
  bool gun_targeting_enabled = true;
  float gun_targeting_distance = 60.0f;
  float gun_targeting_radius = 4.0f;
  bool visor_helmet_enabled = false;
  bool xr_dpad_enabled = true;
  float xr_dpad_head_radius = 0.18f;
  float xr_dpad_head_y_below = 0.14f;
  float xr_dpad_deadzone = 0.45f;
  bool directional_movement_enabled = true;
  bool directional_movement_use_right_stick = false;
  bool directional_movement_use_hmd_direction = false;
  float directional_movement_deadzone = 0.25f;
  float directional_movement_speed = 14.0f;
  float directional_movement_accel = 45.0f;
  float directional_movement_air_accel = 8.0f;
  float look_yaw_sensitivity = 1.0f;
  float offset_x = 0.0f;
  float offset_y = 0.0f;
  float offset_z = 0.0f;
  float model_offset_x = 0.0f;
  float model_offset_y = 0.0f;
  float model_offset_z = 0.0f;
  float rot_offset_x = 0.0f;
  float rot_offset_y = 0.0f;
  float rot_offset_z = 0.0f;
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
                             const OpenXRPoseState& head_pose = {},
                             const std::array<float, 3>& tracking_origin_position = {})
  {
    std::lock_guard lk(s_state_mutex);
    s_state.controllers = controllers;
    s_state.head_pose = head_pose;
    s_state.tracking_origin_position = tracking_origin_position;
    s_state.runtime_active = runtime_active;
    s_state.session_focused = runtime_active;
    ++s_state.generation;
  }

  static void SetInteractionProfiles(const std::array<std::string, 2>& profiles)
  {
    std::lock_guard lk(s_state_mutex);
    s_state.interaction_profiles = profiles;
    ++s_state.generation;
  }

  static void SetSessionFocused(bool focused)
  {
    std::lock_guard lk(s_state_mutex);
    s_state.session_focused = focused;
    ++s_state.generation;
  }

  static std::string GetDiagnosticString()
  {
    std::lock_guard lk(s_state_mutex);
    std::ostringstream out;
    out << "runtime_active=" << (s_state.runtime_active ? "true" : "false")
        << " session_focused=" << (s_state.session_focused ? "true" : "false")
        << " generation=" << s_state.generation << '\n';
    for (std::size_t i = 0; i < s_state.controllers.size(); ++i)
    {
      const OpenXRControllerState& controller = s_state.controllers[i];
      out << (i == 0 ? "left" : "right") << ": connected="
          << (controller.connected ? "true" : "false") << " profile="
          << s_state.interaction_profiles[i] << " trigger=" << controller.trigger_value
          << " squeeze=" << controller.squeeze_value << " stick=(" << controller.thumbstick_x
          << ',' << controller.thumbstick_y << ")\n";
    }
    return out.str();
  }

  static void Reset()
  {
    std::lock_guard lk(s_state_mutex);
    s_state = {};
    s_haptics = {};
    s_primegun_overlay = {};
    ++s_state.generation;
  }

  static OpenXRHapticsState GetHaptics()
  {
    std::lock_guard lk(s_state_mutex);
    return s_haptics;
  }

  static PrimedGunVrOverlayState GetPrimedGunOverlay()
  {
    std::lock_guard lk(s_state_mutex);
    return s_primegun_overlay;
  }

  static void SetPrimedGunOverlay(const PrimedGunVrOverlayState& overlay)
  {
    std::lock_guard lk(s_state_mutex);
    s_primegun_overlay = overlay;
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
  static inline PrimedGunVrOverlayState s_primegun_overlay{};
};
}  // namespace Common::VR

#endif  // ENABLE_VR
