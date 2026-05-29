// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/GCPadEmu.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "Common/Common.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#ifdef ENABLE_VR
#include "Common/VR/OpenXRInputState.h"
#endif

#include "Core/HW/GCPad.h"
#ifdef ENABLE_VR
#include "Core/PrimeGun/NativeRuntime.h"
#endif

#include "InputCommon/ControllerEmu/ControlGroup/AnalogStick.h"
#include "InputCommon/ControllerEmu/ControlGroup/Buttons.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/ControlGroup/MixedTriggers.h"
#include "InputCommon/ControllerEmu/StickGate.h"
#include "InputCommon/GCPadStatus.h"

static const u16 button_bitmasks[] = {
    PAD_BUTTON_A,
    PAD_BUTTON_B,
    PAD_BUTTON_X,
    PAD_BUTTON_Y,
    PAD_TRIGGER_Z,
    PAD_BUTTON_START,
    0  // MIC HAX
};

static const u16 trigger_bitmasks[] = {
    PAD_TRIGGER_L,
    PAD_TRIGGER_R,
};

static const u16 dpad_bitmasks[] = {PAD_BUTTON_UP, PAD_BUTTON_DOWN, PAD_BUTTON_LEFT,
                                    PAD_BUTTON_RIGHT};
static const u8 triforce_bitmask[] = {SWITCH_TEST, SWITCH_SERVICE, SWITCH_COIN};

#ifdef ENABLE_VR
static u8 PrimeGunAxisToPadByte(float value, u8 center)
{
  value = std::clamp(value, -1.0f, 1.0f);
  return static_cast<u8>(std::clamp<int>(static_cast<int>(center + std::lround(value * 127.0f)),
                                         0, 255));
}

struct PrimeGunQuat
{
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float w = 1.0f;
};

static PrimeGunQuat PrimeGunNormalizeQuat(PrimeGunQuat q)
{
  const float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (len <= 0.00001f || !std::isfinite(len))
    return {};

  const float inv_len = 1.0f / len;
  q.x *= inv_len;
  q.y *= inv_len;
  q.z *= inv_len;
  q.w *= inv_len;
  return q;
}

static PrimeGunQuat PrimeGunPoseQuat(const Common::VR::OpenXRPoseState& pose)
{
  return PrimeGunNormalizeQuat(
      {pose.orientation[0], pose.orientation[1], pose.orientation[2], pose.orientation[3]});
}

static PrimeGunQuat PrimeGunQuatFromBasis(float right_x, float right_y, float right_z, float up_x,
                                          float up_y, float up_z, float back_x, float back_y,
                                          float back_z)
{
  const float trace = right_x + up_y + back_z;
  PrimeGunQuat q{};
  if (trace > 0.0f)
  {
    const float s = std::sqrt(trace + 1.0f) * 2.0f;
    q.w = 0.25f * s;
    q.x = (up_z - back_y) / s;
    q.y = (back_x - right_z) / s;
    q.z = (right_y - up_x) / s;
  }
  else if (right_x > up_y && right_x > back_z)
  {
    const float s = std::sqrt(1.0f + right_x - up_y - back_z) * 2.0f;
    q.w = (up_z - back_y) / s;
    q.x = 0.25f * s;
    q.y = (up_x + right_y) / s;
    q.z = (back_x + right_z) / s;
  }
  else if (up_y > back_z)
  {
    const float s = std::sqrt(1.0f + up_y - right_x - back_z) * 2.0f;
    q.w = (back_x - right_z) / s;
    q.x = (up_x + right_y) / s;
    q.y = 0.25f * s;
    q.z = (back_y + up_z) / s;
  }
  else
  {
    const float s = std::sqrt(1.0f + back_z - right_x - up_y) * 2.0f;
    q.w = (right_y - up_x) / s;
    q.x = (back_x + right_z) / s;
    q.y = (back_y + up_z) / s;
    q.z = 0.25f * s;
  }
  return PrimeGunNormalizeQuat(q);
}

static PrimeGunQuat PrimeGunConjugateQuat(PrimeGunQuat q)
{
  return {-q.x, -q.y, -q.z, q.w};
}

static void PrimeGunRotateByInverse(PrimeGunQuat q, float x, float y, float z, float* out_x,
                                    float* out_y, float* out_z)
{
  q = PrimeGunConjugateQuat(q);
  const float tx = 2.0f * (q.y * z - q.z * y);
  const float ty = 2.0f * (q.z * x - q.x * z);
  const float tz = 2.0f * (q.x * y - q.y * x);
  *out_x = x + q.w * tx + (q.y * tz - q.z * ty);
  *out_y = y + q.w * ty + (q.z * tx - q.x * tz);
  *out_z = z + q.w * tz + (q.x * ty - q.y * tx);
}

static void PrimeGunRotate(PrimeGunQuat q, float x, float y, float z, float* out_x, float* out_y,
                           float* out_z)
{
  const float tx = 2.0f * (q.y * z - q.z * y);
  const float ty = 2.0f * (q.z * x - q.x * z);
  const float tz = 2.0f * (q.x * y - q.y * x);
  *out_x = x + q.w * tx + (q.y * tz - q.z * ty);
  *out_y = y + q.w * ty + (q.z * tx - q.x * tz);
  *out_z = z + q.w * tz + (q.x * ty - q.y * tx);
}

static float PrimeGunDot(float ax, float ay, float az, float bx, float by, float bz)
{
  return ax * bx + ay * by + az * bz;
}

static PrimeGunQuat PrimeGunRollFreeQuat(PrimeGunQuat q)
{
  float forward_x = 0.0f;
  float forward_y = 0.0f;
  float forward_z = 0.0f;
  PrimeGunRotate(q, 0.0f, 0.0f, -1.0f, &forward_x, &forward_y, &forward_z);

  float right_x = -forward_z;
  float right_y = 0.0f;
  float right_z = forward_x;
  float right_len = std::sqrt(right_x * right_x + right_z * right_z);
  if (right_len < 0.0001f || !std::isfinite(right_len))
  {
    right_x = 1.0f;
    right_z = 0.0f;
    right_len = 1.0f;
  }
  right_x /= right_len;
  right_z /= right_len;

  const float up_x = -forward_y * right_z;
  const float up_y = -forward_z * right_x + forward_x * right_z;
  const float up_z = forward_y * right_x;
  return PrimeGunQuatFromBasis(right_x, right_y, right_z, up_x, up_y, up_z, -forward_x,
                               -forward_y, -forward_z);
}

static void ApplyPrimeGunClassicMenuControls(const Common::VR::OpenXRControllerState& left,
                                             const Common::VR::OpenXRControllerState& right,
                                             GCPadStatus* pad, bool suppress_left_stick,
                                             bool consume_left_secondary)
{
  pad->stickX =
      left.connected && !suppress_left_stick ?
          PrimeGunAxisToPadByte(left.thumbstick_x, GCPadStatus::MAIN_STICK_CENTER_X) :
          GCPadStatus::MAIN_STICK_CENTER_X;
  pad->stickY =
      left.connected && !suppress_left_stick ?
          PrimeGunAxisToPadByte(left.thumbstick_y, GCPadStatus::MAIN_STICK_CENTER_Y) :
          GCPadStatus::MAIN_STICK_CENTER_Y;
  pad->substickX =
      right.connected ? PrimeGunAxisToPadByte(right.thumbstick_x, GCPadStatus::C_STICK_CENTER_X) :
                        GCPadStatus::C_STICK_CENTER_X;
  pad->substickY =
      right.connected ? PrimeGunAxisToPadByte(right.thumbstick_y, GCPadStatus::C_STICK_CENTER_Y) :
                        GCPadStatus::C_STICK_CENTER_Y;

  if (left.connected)
  {
    if (left.primary_button)
      pad->button |= PAD_BUTTON_X;
    if (left.secondary_button && !consume_left_secondary)
      pad->button |= PAD_BUTTON_START;
    if (left.trigger_button)
    {
      pad->button |= PAD_TRIGGER_L;
      pad->triggerLeft = 0xFF;
    }
  }

  if (right.connected)
  {
    if (right.primary_button)
      pad->button |= PAD_BUTTON_A;
    if (right.secondary_button)
      pad->button |= PAD_BUTTON_B;
    if (right.trigger_button)
    {
      pad->button |= PAD_TRIGGER_R;
      pad->triggerRight = 0xFF;
    }
  }
}

enum class PrimeGunBeamSlot
{
  None,
  Power,
  Wave,
  Ice,
  Plasma,
};

static void SetPrimeGunBeamCStick(PrimeGunBeamSlot slot, GCPadStatus* pad)
{
  switch (slot)
  {
  case PrimeGunBeamSlot::Power:
    pad->substickX = GCPadStatus::C_STICK_CENTER_X;
    pad->substickY = PrimeGunAxisToPadByte(1.0f, GCPadStatus::C_STICK_CENTER_Y);
    break;
  case PrimeGunBeamSlot::Wave:
    pad->substickX = PrimeGunAxisToPadByte(1.0f, GCPadStatus::C_STICK_CENTER_X);
    pad->substickY = GCPadStatus::C_STICK_CENTER_Y;
    break;
  case PrimeGunBeamSlot::Ice:
    pad->substickX = GCPadStatus::C_STICK_CENTER_X;
    pad->substickY = PrimeGunAxisToPadByte(-1.0f, GCPadStatus::C_STICK_CENTER_Y);
    break;
  case PrimeGunBeamSlot::Plasma:
    pad->substickX = PrimeGunAxisToPadByte(-1.0f, GCPadStatus::C_STICK_CENTER_X);
    pad->substickY = GCPadStatus::C_STICK_CENTER_Y;
    break;
  case PrimeGunBeamSlot::None:
    pad->substickX = GCPadStatus::C_STICK_CENTER_X;
    pad->substickY = GCPadStatus::C_STICK_CENTER_Y;
    break;
  }
}

static const char* PrimeGunBeamSlotName(PrimeGunBeamSlot slot)
{
  switch (slot)
  {
  case PrimeGunBeamSlot::Power:
    return "power";
  case PrimeGunBeamSlot::Wave:
    return "wave";
  case PrimeGunBeamSlot::Ice:
    return "ice";
  case PrimeGunBeamSlot::Plasma:
    return "plasma";
  case PrimeGunBeamSlot::None:
    return "none";
  }
  return "none";
}

static uint32_t PrimeGunBeamSlotIndex(PrimeGunBeamSlot slot)
{
  switch (slot)
  {
  case PrimeGunBeamSlot::Power:
    return 1;
  case PrimeGunBeamSlot::Wave:
    return 2;
  case PrimeGunBeamSlot::Ice:
    return 3;
  case PrimeGunBeamSlot::Plasma:
    return 4;
  case PrimeGunBeamSlot::None:
    return 0;
  }
  return 0;
}

static void PublishPrimeGunWeaponPanel(bool visible, PrimeGunBeamSlot slot,
                                       const Common::VR::OpenXRPoseState* anchor_pose)
{
  auto overlay = Common::VR::OpenXRInputState::GetPrimeGunOverlay();
  overlay.weapon_panel_visible = visible;
  overlay.weapon_selected_index = PrimeGunBeamSlotIndex(slot);
  if (visible && anchor_pose && anchor_pose->valid)
  {
    overlay.weapon_panel_position = anchor_pose->position;
    overlay.weapon_panel_orientation = anchor_pose->orientation;
  }
  Common::VR::OpenXRInputState::SetPrimeGunOverlay(overlay);
}

static void UpdatePrimeGunWeaponSelect(const Common::VR::OpenXRControllerState& right,
                                       GCPadStatus* pad)
{
  static bool s_gesture_active = false;
  static float s_base_x = 0.0f;
  static float s_base_y = 0.0f;
  static float s_base_z = 0.0f;
  static PrimeGunQuat s_base_orientation = {};
  static PrimeGunBeamSlot s_selected_slot = PrimeGunBeamSlot::None;
  static PrimeGunBeamSlot s_last_logged_slot = PrimeGunBeamSlot::None;
  static PrimeGunBeamSlot s_pulse_slot = PrimeGunBeamSlot::None;
  static int s_pulse_frames = 0;
  static Common::VR::OpenXRPoseState s_anchor_pose = {};
  static PrimeGunQuat s_anchor_orientation = {};
  static bool s_have_selection_center = false;
  static float s_selection_center_x = 0.0f;
  static float s_selection_center_y = 0.0f;

  if (s_pulse_frames > 0)
  {
    SetPrimeGunBeamCStick(s_pulse_slot, pad);
    --s_pulse_frames;
    if (s_pulse_frames == 0)
      s_pulse_slot = PrimeGunBeamSlot::None;
  }

  const auto& pose = right.aim_pose.valid ? right.aim_pose : right.grip_pose;
  if (!right.secondary_button || !pose.valid)
  {
    if (s_gesture_active && s_selected_slot != PrimeGunBeamSlot::None)
    {
      s_pulse_slot = s_selected_slot;
      s_pulse_frames = 8;
      SetPrimeGunBeamCStick(s_pulse_slot, pad);
      NOTICE_LOG_FMT(CORE, "PrimeGun weapon select commit={}", PrimeGunBeamSlotName(s_pulse_slot));
    }
    s_gesture_active = false;
    s_selected_slot = PrimeGunBeamSlot::None;
    s_last_logged_slot = PrimeGunBeamSlot::None;
    s_have_selection_center = false;
    s_selection_center_x = 0.0f;
    s_selection_center_y = 0.0f;
    PublishPrimeGunWeaponPanel(false, PrimeGunBeamSlot::None, nullptr);
    return;
  }

  if (!s_gesture_active)
  {
    s_base_x = pose.position[0];
    s_base_y = pose.position[1];
    s_base_z = pose.position[2];
    s_base_orientation = PrimeGunRollFreeQuat(PrimeGunPoseQuat(pose));
    s_anchor_orientation = s_base_orientation;
    s_anchor_pose = pose;
    s_anchor_pose.orientation = {s_anchor_orientation.x, s_anchor_orientation.y,
                                 s_anchor_orientation.z, s_anchor_orientation.w};
    s_gesture_active = true;
    s_selected_slot = PrimeGunBeamSlot::None;
    s_last_logged_slot = PrimeGunBeamSlot::None;
    s_have_selection_center = false;
    s_selection_center_x = 0.0f;
    s_selection_center_y = 0.0f;
    NOTICE_LOG_FMT(CORE, "PrimeGun weapon select open");
  }

  float panel_right_x = 0.0f;
  float panel_right_y = 0.0f;
  float panel_right_z = 0.0f;
  float panel_up_x = 0.0f;
  float panel_up_y = 0.0f;
  float panel_up_z = 0.0f;
  float panel_forward_x = 0.0f;
  float panel_forward_y = 0.0f;
  float panel_forward_z = 0.0f;
  float panel_offset_x = 0.0f;
  float panel_offset_y = 0.0f;
  float panel_offset_z = 0.0f;
  PrimeGunRotate(s_anchor_orientation, 1.0f, 0.0f, 0.0f, &panel_right_x, &panel_right_y,
                 &panel_right_z);
  PrimeGunRotate(s_anchor_orientation, 0.0f, 1.0f, 0.0f, &panel_up_x, &panel_up_y, &panel_up_z);
  PrimeGunRotate(s_anchor_orientation, 0.0f, 0.0f, -1.0f, &panel_forward_x, &panel_forward_y,
                 &panel_forward_z);
  PrimeGunRotate(s_anchor_orientation, 0.0f, 0.055f, -0.26f, &panel_offset_x, &panel_offset_y,
                 &panel_offset_z);

  const float panel_x = s_base_x + panel_offset_x;
  const float panel_y = s_base_y + panel_offset_y;
  const float panel_z = s_base_z + panel_offset_z;
  const PrimeGunQuat current_orientation = PrimeGunRollFreeQuat(PrimeGunPoseQuat(pose));
  float ray_x = 0.0f;
  float ray_y = 0.0f;
  float ray_z = 0.0f;
  PrimeGunRotate(current_orientation, 0.0f, 0.0f, -1.0f, &ray_x, &ray_y, &ray_z);

  const float denom =
      PrimeGunDot(ray_x, ray_y, ray_z, panel_forward_x, panel_forward_y, panel_forward_z);
  float x = 0.0f;
  float y = 0.0f;
  bool hit_panel = false;
  if (std::fabs(denom) > 0.025f)
  {
    const float to_panel_x = panel_x - pose.position[0];
    const float to_panel_y = panel_y - pose.position[1];
    const float to_panel_z = panel_z - pose.position[2];
    const float t = PrimeGunDot(to_panel_x, to_panel_y, to_panel_z, panel_forward_x,
                                panel_forward_y, panel_forward_z) /
                    denom;
    if (t > 0.02f && t < 2.0f)
    {
      const float hit_x = pose.position[0] + ray_x * t;
      const float hit_y = pose.position[1] + ray_y * t;
      const float hit_z = pose.position[2] + ray_z * t;
      x = PrimeGunDot(hit_x - panel_x, hit_y - panel_y, hit_z - panel_z, panel_right_x,
                      panel_right_y, panel_right_z) /
          0.21f;
      y = PrimeGunDot(hit_x - panel_x, hit_y - panel_y, hit_z - panel_z, panel_up_x,
                      panel_up_y, panel_up_z) /
          0.21f;
      hit_panel = std::fabs(x) <= 1.8f && std::fabs(y) <= 1.8f;
    }
  }

  if (!hit_panel)
  {
    float local_x = 0.0f;
    float local_y = 0.0f;
    float local_z = 0.0f;
    PrimeGunRotateByInverse(s_base_orientation, pose.position[0] - s_base_x,
                            pose.position[1] - s_base_y, pose.position[2] - s_base_z, &local_x,
                            &local_y, &local_z);
    x = local_x / 0.075f;
    y = local_y / 0.075f;
  }

  if (!s_have_selection_center)
  {
    s_selection_center_x = x;
    s_selection_center_y = y;
    s_have_selection_center = true;
  }
  x -= s_selection_center_x;
  y -= s_selection_center_y;

  x = std::clamp(x, -1.0f, 1.0f);
  y = std::clamp(y, -1.0f, 1.0f);
  if (std::fabs(x) < 0.25f && std::fabs(y) < 0.25f)
  {
    s_selected_slot = PrimeGunBeamSlot::None;
    if (s_last_logged_slot != s_selected_slot)
    {
      NOTICE_LOG_FMT(CORE, "PrimeGun weapon select hover={}", PrimeGunBeamSlotName(s_selected_slot));
      s_last_logged_slot = s_selected_slot;
    }
    PublishPrimeGunWeaponPanel(true, s_selected_slot, &s_anchor_pose);
    return;
  }

  if (std::fabs(x) >= std::fabs(y))
  {
    s_selected_slot = x < 0.0f ? PrimeGunBeamSlot::Plasma : PrimeGunBeamSlot::Wave;
  }
  else
  {
    s_selected_slot = y < 0.0f ? PrimeGunBeamSlot::Ice : PrimeGunBeamSlot::Power;
  }

  if (s_last_logged_slot != s_selected_slot)
  {
    NOTICE_LOG_FMT(CORE, "PrimeGun weapon select hover={}", PrimeGunBeamSlotName(s_selected_slot));
    s_last_logged_slot = s_selected_slot;
  }
  PublishPrimeGunWeaponPanel(true, s_selected_slot, &s_anchor_pose);
}

static bool ApplyPrimeGunModernControls(GCPadStatus* pad)
{
  static u64 s_primegun_pad_sample = 0;
  static bool s_last_gameplay = false;
  static bool s_last_overlay_visible = false;
  static bool s_last_left_primary = false;
  static bool s_last_left_secondary = false;
  static bool s_last_left_menu = false;
  static bool s_last_left_thumbstick = false;
  static bool s_last_right_primary = false;
  static bool s_last_right_secondary = false;
  static bool s_last_right_menu = false;
  static bool s_last_right_thumbstick = false;
  static bool s_last_orbit_lock = false;

  const auto snapshot = Common::VR::OpenXRInputState::GetSnapshot();
  const auto overlay = Common::VR::OpenXRInputState::GetPrimeGunOverlay();
  if (!snapshot.runtime_active)
    return false;

  const auto& left = snapshot.controllers[0];
  const auto& right = snapshot.controllers[1];
  if (!left.connected && !right.connected)
    return false;

  auto game_right = right;
  if (overlay.menu_visible)
  {
    game_right.primary_button = false;
    game_right.secondary_button = false;
  }

  *pad = {};
  pad->isConnected = true;

  const bool left_vr_menu_button = left.connected && (left.thumbstick_button || left.menu_button);
  const bool suppress_left_stick = false;

  const bool gameplay = PrimeGun::IsGameplayInputActive();
  const bool orbit_lock_active = gameplay && PrimeGun::IsOrbitLockActive();
  const bool log_now =
      (++s_primegun_pad_sample % 120) == 0 || gameplay != s_last_gameplay ||
      orbit_lock_active != s_last_orbit_lock ||
      overlay.menu_visible != s_last_overlay_visible || left.primary_button != s_last_left_primary ||
      left.secondary_button != s_last_left_secondary || left.menu_button != s_last_left_menu ||
      left.thumbstick_button != s_last_left_thumbstick ||
      right.primary_button != s_last_right_primary ||
      right.secondary_button != s_last_right_secondary || right.menu_button != s_last_right_menu ||
      right.thumbstick_button != s_last_right_thumbstick;
  if (log_now)
  {
    NOTICE_LOG_FMT(CORE,
                   "PrimeGun pad mode={} overlay={} L[p={} s={} menu={} stick_click={} x={:.2f} y={:.2f}] "
                   "R[p={} s={} menu={} stick_click={} trig={} grip={}]",
                   orbit_lock_active ? "lock-on" : (gameplay ? "combat" : "classic"),
                   overlay.menu_visible, left.primary_button,
                   left.secondary_button, left.menu_button, left.thumbstick_button,
                   left.thumbstick_x, left.thumbstick_y, right.primary_button,
                   right.secondary_button, right.menu_button,
                   right.thumbstick_button, right.trigger_button, right.squeeze_button);
    s_last_gameplay = gameplay;
    s_last_orbit_lock = orbit_lock_active;
    s_last_overlay_visible = overlay.menu_visible;
    s_last_left_primary = left.primary_button;
    s_last_left_secondary = left.secondary_button;
    s_last_left_menu = left.menu_button;
    s_last_left_thumbstick = left.thumbstick_button;
    s_last_right_primary = right.primary_button;
    s_last_right_secondary = right.secondary_button;
    s_last_right_menu = right.menu_button;
    s_last_right_thumbstick = right.thumbstick_button;
  }

  if (!gameplay)
  {
    ApplyPrimeGunClassicMenuControls(left, game_right, pad, suppress_left_stick,
                                     false);
    if (pad->button & PAD_BUTTON_A)
      pad->analogA = 0xFF;
    if (pad->button & PAD_BUTTON_B)
      pad->analogB = 0xFF;
    return true;
  }

  const bool weapon_modifier = game_right.connected && game_right.secondary_button;
  const bool fire_pressed = game_right.connected && game_right.trigger_button;
  constexpr float stick_button_threshold = 0.55f;

  // PrimeGun owns the first-person locomotion layout:
  // left stick Y = forward/back, left stick X = runtime strafe, right stick X = turn.
  pad->stickX = orbit_lock_active && left.connected ?
                    PrimeGunAxisToPadByte(left.thumbstick_x, GCPadStatus::MAIN_STICK_CENTER_X) :
                    game_right.connected && !weapon_modifier ?
          PrimeGunAxisToPadByte(game_right.thumbstick_x, GCPadStatus::MAIN_STICK_CENTER_X) :
          GCPadStatus::MAIN_STICK_CENTER_X;
  pad->stickY =
      left.connected && !suppress_left_stick ?
          PrimeGunAxisToPadByte(left.thumbstick_y, GCPadStatus::MAIN_STICK_CENTER_Y) :
                       GCPadStatus::MAIN_STICK_CENTER_Y;

  if (weapon_modifier && game_right.connected)
  {
    UpdatePrimeGunWeaponSelect(game_right, pad);
  }
  else
  {
    pad->substickX = GCPadStatus::C_STICK_CENTER_X;
    pad->substickY = GCPadStatus::C_STICK_CENTER_Y;
    if (game_right.connected)
      UpdatePrimeGunWeaponSelect(game_right, pad);
  }

  if (left.connected)
  {
    if (left.primary_button)
      pad->button |= PAD_BUTTON_X;
    if (left.secondary_button && !left_vr_menu_button)
      pad->button |= PAD_BUTTON_START;
    if (left.squeeze_button)
      pad->button |= PAD_TRIGGER_Z;
    if (left.trigger_button)
    {
      pad->button |= PAD_TRIGGER_L;
      pad->triggerLeft = 0xFF;
    }
  }

  if (game_right.connected)
  {
    if ((!gameplay && game_right.primary_button) || fire_pressed)
      pad->button |= PAD_BUTTON_A;
    if (game_right.squeeze_button)
      pad->button |= PAD_BUTTON_Y;
    if (!weapon_modifier && game_right.thumbstick_y > stick_button_threshold)
      pad->button |= PAD_BUTTON_B;
  }

  if (overlay.menu_visible)
  {
    pad->button &= ~(PAD_BUTTON_A | PAD_BUTTON_B);
    pad->analogA = 0;
    pad->analogB = 0;
  }

  if (pad->button & PAD_BUTTON_A)
    pad->analogA = 0xFF;
  if (pad->button & PAD_BUTTON_B)
    pad->analogB = 0xFF;

  return true;
}
#endif

GCPad::GCPad(const unsigned int index) : m_index(index)
{
  using Translatability = ControllerEmu::Translatability;

  // buttons
  groups.emplace_back(m_buttons = new ControllerEmu::Buttons(BUTTONS_GROUP));
  for (const char* named_button : {A_BUTTON, B_BUTTON, X_BUTTON, Y_BUTTON, Z_BUTTON})
  {
    m_buttons->AddInput(Translatability::DoNotTranslate, named_button);
  }
  // i18n: The START/PAUSE button on GameCube controllers
  m_buttons->AddInput(Translatability::Translate, START_BUTTON, _trans("START"));

  // sticks
  groups.emplace_back(m_main_stick = new ControllerEmu::OctagonAnalogStick(
                          MAIN_STICK_GROUP, _trans("Control Stick"), MAIN_STICK_GATE_RADIUS));
  groups.emplace_back(m_c_stick = new ControllerEmu::OctagonAnalogStick(
                          C_STICK_GROUP, _trans("C Stick"), C_STICK_GATE_RADIUS));

  // triggers
  groups.emplace_back(m_triggers = new ControllerEmu::MixedTriggers(TRIGGERS_GROUP));
  for (const char* named_trigger : {L_DIGITAL, R_DIGITAL, L_ANALOG, R_ANALOG})
  {
    m_triggers->AddInput(Translatability::Translate, named_trigger);
  }

  // dpad
  groups.emplace_back(m_dpad = new ControllerEmu::Buttons(DPAD_GROUP));
  for (const char* named_direction : named_directions)
  {
    m_dpad->AddInput(Translatability::Translate, named_direction);
  }

  // triforce
  groups.emplace_back(m_triforce = new ControllerEmu::Buttons(TRIFORCE_GROUP));
  for (const char* named_button : {TEST_BUTTON, SERVICE_BUTTON, COIN_BUTTON})
  {
    m_triforce->AddInput(Translatability::Translate, named_button);
  }

  // Microphone
  groups.emplace_back(m_mic = new ControllerEmu::Buttons(MIC_GROUP));
  m_mic->AddInput(Translatability::Translate, _trans("Button"));

  // rumble
  groups.emplace_back(m_rumble = new ControllerEmu::ControlGroup(RUMBLE_GROUP));
  m_rumble->AddOutput(Translatability::Translate, _trans("Motor"));

  // options
  groups.emplace_back(m_options = new ControllerEmu::ControlGroup(OPTIONS_GROUP));
  m_options->AddSetting(
      &m_always_connected_setting,
      // i18n: Treat a controller as always being connected regardless of what
      // devices the user actually has plugged in
      {_trans("Always Connected"), nullptr,
       _trans("If checked, the emulated controller is always connected.\n"
              "If unchecked, the connection state of the emulated controller is linked\n"
              "to the connection state of the real default device (if there is one).")},
      false);
}

std::string GCPad::GetName() const
{
  return std::string("GCPad") + char('1' + m_index);
}

InputConfig* GCPad::GetConfig() const
{
  return Pad::GetConfig();
}

ControllerEmu::ControlGroup* GCPad::GetGroup(PadGroup group)
{
  switch (group)
  {
  case PadGroup::Buttons:
    return m_buttons;
  case PadGroup::MainStick:
    return m_main_stick;
  case PadGroup::CStick:
    return m_c_stick;
  case PadGroup::DPad:
    return m_dpad;
  case PadGroup::Triggers:
    return m_triggers;
  case PadGroup::Rumble:
    return m_rumble;
  case PadGroup::Mic:
    return m_mic;
  case PadGroup::Options:
    return m_options;
  case PadGroup::Triforce:
    return m_triforce;
  default:
    return nullptr;
  }
}

GCPadStatus GCPad::GetInput() const
{
  using ControllerEmu::MapFloat;

  const auto lock = GetStateLock();
  GCPadStatus pad = {};

#ifdef ENABLE_VR
  if (m_index == 0 && ApplyPrimeGunModernControls(&pad))
    return pad;
#endif

  if (!(m_always_connected_setting.GetValue() || IsDefaultDeviceConnected() ||
        m_input_override_function))
  {
    pad.isConnected = false;
    return pad;
  }

  // buttons
  m_buttons->GetState(&pad.button, button_bitmasks, m_input_override_function);

  // set analog A/B analog to full or w/e, prolly not needed
  if (pad.button & PAD_BUTTON_A)
    pad.analogA = 0xFF;
  if (pad.button & PAD_BUTTON_B)
    pad.analogB = 0xFF;

  // dpad
  m_dpad->GetState(&pad.button, dpad_bitmasks, m_input_override_function);

  // triforce
  m_triforce->GetState(&pad.switches, triforce_bitmask, m_input_override_function);

  // sticks
  const auto main_stick_state = m_main_stick->GetState(m_input_override_function);
  pad.stickX = MapFloat<u8>(main_stick_state.x, GCPadStatus::MAIN_STICK_CENTER_X, 1);
  pad.stickY = MapFloat<u8>(main_stick_state.y, GCPadStatus::MAIN_STICK_CENTER_Y, 1);

  const auto c_stick_state = m_c_stick->GetState(m_input_override_function);
  pad.substickX = MapFloat<u8>(c_stick_state.x, GCPadStatus::C_STICK_CENTER_X, 1);
  pad.substickY = MapFloat<u8>(c_stick_state.y, GCPadStatus::C_STICK_CENTER_Y, 1);

  // triggers
  std::array<ControlState, 2> triggers;
  m_triggers->GetState(&pad.button, trigger_bitmasks, triggers.data(), m_input_override_function);
  pad.triggerLeft = MapFloat<u8>(triggers[0], 0);
  pad.triggerRight = MapFloat<u8>(triggers[1], 0);

  return pad;
}

void GCPad::SetOutput(const ControlState strength)
{
  const auto lock = GetStateLock();
  m_rumble->controls[0]->control_ref->State(strength);
}

void GCPad::LoadDefaults(const ControllerInterface& ciface)
{
  EmulatedController::LoadDefaults(ciface);

#ifdef ANDROID
  // Rumble
  m_rumble->SetControlExpression(0, "`Android/0/Device Sensors:Motor 0`");

  // Triforce Coin
  m_triforce->SetControlExpression(2, "pulse(`Android/0/Device Sensors:Accel Down` > 15, 0.1)");
#else
  // Buttons: A, B, X, Y, Z
  m_buttons->SetControlExpression(0, "`X`");
  m_buttons->SetControlExpression(1, "`Z`");
  m_buttons->SetControlExpression(2, "`C`");
  m_buttons->SetControlExpression(3, "`S`");
  m_buttons->SetControlExpression(4, "`D`");
#ifdef _WIN32
  m_buttons->SetControlExpression(5, "`RETURN`");  // Start
#else
  // OS X/Linux
  // Start
  m_buttons->SetControlExpression(5, "`Return`");
#endif

  // D-Pad
  m_dpad->SetControlExpression(0, "`T`");  // Up
  m_dpad->SetControlExpression(1, "`G`");  // Down
  m_dpad->SetControlExpression(2, "`F`");  // Left
  m_dpad->SetControlExpression(3, "`H`");  // Right

  // Triforce
  m_triforce->SetControlExpression(0, "`1`");  // Test
  m_triforce->SetControlExpression(1, "`2`");  // Service
  m_triforce->SetControlExpression(2, "`3`");  // Coin

  // C Stick
  m_c_stick->SetControlExpression(0, "`I`");  // Up
  m_c_stick->SetControlExpression(1, "`K`");  // Down
  m_c_stick->SetControlExpression(2, "`J`");  // Left
  m_c_stick->SetControlExpression(3, "`L`");  // Right
  // Modifier
  m_c_stick->SetControlExpression(4, "`Ctrl`");

  // Control Stick
#ifdef _WIN32
  m_main_stick->SetControlExpression(0, "`UP`");     // Up
  m_main_stick->SetControlExpression(1, "`DOWN`");   // Down
  m_main_stick->SetControlExpression(2, "`LEFT`");   // Left
  m_main_stick->SetControlExpression(3, "`RIGHT`");  // Right
#elif __APPLE__
  m_main_stick->SetControlExpression(0, "`Up Arrow`");     // Up
  m_main_stick->SetControlExpression(1, "`Down Arrow`");   // Down
  m_main_stick->SetControlExpression(2, "`Left Arrow`");   // Left
  m_main_stick->SetControlExpression(3, "`Right Arrow`");  // Right
#else
  m_main_stick->SetControlExpression(0, "`Up`");     // Up
  m_main_stick->SetControlExpression(1, "`Down`");   // Down
  m_main_stick->SetControlExpression(2, "`Left`");   // Left
  m_main_stick->SetControlExpression(3, "`Right`");  // Right
#endif
  // Modifier
  m_main_stick->SetControlExpression(4, "`Shift`");

  // Because our defaults use keyboard input, set calibration shapes to squares.
  m_c_stick->SetCalibrationFromGate(ControllerEmu::SquareStickGate(1.0));
  m_main_stick->SetCalibrationFromGate(ControllerEmu::SquareStickGate(1.0));

  // Triggers
  m_triggers->SetControlExpression(0, "`Q`");  // L
  m_triggers->SetControlExpression(1, "`W`");  // R
#endif
}

bool GCPad::GetMicButton() const
{
  const auto lock = GetStateLock();
  return m_mic->controls.back()->GetState<bool>();
}
