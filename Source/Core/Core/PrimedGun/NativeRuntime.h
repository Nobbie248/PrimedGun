// Copyright 2026 PrimedGun Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Core
{
class CPUThreadGuard;
class System;
}  // namespace Core

namespace PrimedGun
{
struct RuntimeSettings
{
  bool enabled = true;
  bool builtin_patches_enabled = true;
  bool patch_disable_frustum_culling = true;
  bool patch_no_idle_sway = true;
  bool patch_disable_arm_cannon_idle_fidget = true;
  bool patch_beam_projectile_timing = true;
  bool patch_xr_visor_dpad_timing = true;
  bool patch_cannon_rotation = true;
  bool patch_gun_ray_target = true;
  bool patch_reticle = true;
  bool use_right_hand = true;
  float offset_x = 0.0f;
  float offset_y = 0.0f;
  float offset_z = 0.0f;
  float model_offset_x = 0.0f;
  float model_offset_y = 0.0f;
  float model_offset_z = 0.0f;
  float rot_offset_x = 0.0f;
  float rot_offset_y = 0.0f;
  float rot_offset_z = 0.0f;
  float world_scale = 1.50f;
  bool require_trigger = false;
  float trigger_threshold = 0.5f;
  bool rumble_enabled = true;
  float rumble_intensity = 0.35f;
  int rumble_hand_mode = 2;
  bool primegun_grip_inputs_enabled = true;
  bool primegun_grip_inputs_use_trackpad = false;
  float primegun_trackpad_press_threshold = 0.5f;
  bool combat_jump_use_primary_button = false;
  bool gun_targeting_enabled = true;
  float gun_targeting_distance = 60.0f;
  float gun_targeting_radius = 4.0f;
  bool visor_helmet_enabled = false;
  bool vr_overlays_enabled = true;
  bool height_prompt_enabled = true;
  bool vr_menu_hold_left_stick = false;
  bool vr_menu_requires_head_zone = false;
  bool cinematic_screen_enabled = false;
  float metroid_hud_distance = 0.5f;
  float metroid_hud_size = 0.5f;
  float metroid_hud_offset_up = 0.0f;
  float metroid_hud_offset_down = 0.0f;
  float metroid_hud_offset_left = 0.0f;
  float metroid_hud_offset_right = 0.0f;
  bool position_marker_enabled = false;
  bool xr_dpad_enabled = true;
  bool xr_dpad_use_thumbrest_modifier = false;
  float xr_dpad_head_radius = 0.28f;
  float xr_dpad_head_y_below = 0.02f;
  float xr_dpad_deadzone = 0.45f;
  bool directional_movement_enabled = true;
  bool directional_movement_use_right_stick = false;
  bool directional_movement_use_hmd_direction = false;
  float directional_movement_deadzone = 0.25f;
  float directional_movement_speed = 14.0f;
  float directional_movement_accel = 45.0f;
  float directional_movement_air_accel = 8.0f;
  float look_yaw_sensitivity = 1.0f;
  bool snap_turn_enabled = false;
  int snap_turn_degrees = 45;
};

RuntimeSettings GetRuntimeSettings();
void SetRuntimeSettings(const RuntimeSettings& settings);
void ResetCalibrationOffsets();
void ApplySamusArmPreset();
void SetVrStateSlot(int slot);
bool ConsumeVrSettingsSaveRequest();
void MarkVrSettingsSaved();
int ConsumeVrStateSlotSelectRequest();
bool ConsumeVrStateLoadRequest();
bool ConsumeVrStateSaveRequest();
bool ConsumeVrStateLoadNewestRequest();
bool ConsumeVrStateSaveOldestRequest();
bool IsGameplayInputActive();
bool IsOrbitLockActive();
void OnFrameEnd(Core::System& system, const Core::CPUThreadGuard& guard);
void ResetNativeRuntime();
}  // namespace PrimedGun
