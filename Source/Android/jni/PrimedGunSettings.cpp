// Copyright 2026 PrimedGun Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Key-addressed JNI access to PrimedGun::RuntimeSettings for the Android frontend. The Quest
// build has no Qt host, so this is the only way its settings UI can reach the mod's tuning
// values. Lookups run through the same VisitPersistedSettings() table that loading and saving
// use, so a setting added there becomes reachable here without touching this file.

#include <algorithm>
#include <string>
#include <type_traits>

#include <jni.h>

#include "Core/PrimedGun/NativeRuntime.h"
#include "Core/PrimedGun/Settings.h"
#include "Core/PrimedGun/SettingsVisitor.h"

#include "jni/AndroidCommon/AndroidCommon.h"

namespace
{
// The HUD offsets are stored as four one-sided distances, but both the Qt window and this UI
// expose them as two signed axes. These keys are resolved before the table lookup and map onto
// the underlying pairs the same way MainWindow's HUD rows do.
constexpr char HUD_VERTICAL_KEY[] = "hud_offset_vertical";
constexpr char HUD_HORIZONTAL_KEY[] = "hud_offset_horizontal";

// Copies the member named by `key` into `out`. Returns false when the key names a setting of a
// different type or no setting at all, so the caller can fall back to the Kotlin-side default
// instead of silently reporting zero.
template <typename T>
bool ReadSetting(const std::string& key, T* out)
{
  PrimedGun::RuntimeSettings settings = PrimedGun::GetRuntimeSettings();
  bool found = false;
  PrimedGun::VisitPersistedSettings(settings, [&](const char* name, auto& member) {
    if (found || key != name)
      return;
    if constexpr (std::is_same_v<std::decay_t<decltype(member)>, T>)
    {
      *out = member;
      found = true;
    }
  });
  return found;
}

// Writes `value` into the member named by `key` and publishes the result. Returns false without
// publishing anything when the key does not name a setting of that type.
template <typename T>
bool WriteSetting(const std::string& key, T value)
{
  PrimedGun::RuntimeSettings settings = PrimedGun::GetRuntimeSettings();
  bool found = false;
  PrimedGun::VisitPersistedSettings(settings, [&](const char* name, auto& member) {
    if (found || key != name)
      return;
    if constexpr (std::is_same_v<std::decay_t<decltype(member)>, T>)
    {
      member = value;
      found = true;
    }
  });
  if (found)
    PrimedGun::SetRuntimeSettings(settings);
  return found;
}

// Splits a signed axis back into the positive/negative pair the runtime stores.
void SetSignedAxis(float value, float* positive, float* negative)
{
  *positive = std::max(value, 0.0f);
  *negative = std::max(-value, 0.0f);
}

bool ReadDerivedFloat(const std::string& key, float* out)
{
  const PrimedGun::RuntimeSettings settings = PrimedGun::GetRuntimeSettings();
  if (key == HUD_VERTICAL_KEY)
  {
    *out = settings.metroid_hud_offset_up - settings.metroid_hud_offset_down;
    return true;
  }
  if (key == HUD_HORIZONTAL_KEY)
  {
    *out = settings.metroid_hud_offset_right - settings.metroid_hud_offset_left;
    return true;
  }
  return false;
}

bool WriteDerivedFloat(const std::string& key, float value)
{
  PrimedGun::RuntimeSettings settings = PrimedGun::GetRuntimeSettings();
  if (key == HUD_VERTICAL_KEY)
  {
    SetSignedAxis(value, &settings.metroid_hud_offset_up, &settings.metroid_hud_offset_down);
  }
  else if (key == HUD_HORIZONTAL_KEY)
  {
    SetSignedAxis(value, &settings.metroid_hud_offset_right, &settings.metroid_hud_offset_left);
  }
  else
  {
    return false;
  }
  PrimedGun::SetRuntimeSettings(settings);
  return true;
}
}  // namespace

extern "C" {

JNIEXPORT jboolean JNICALL
Java_org_dolphinemu_dolphinemu_features_primedgun_model_PrimedGunSettings_getBoolean(
    JNIEnv* env, jclass, jstring key, jboolean default_value)
{
  bool value = default_value == JNI_TRUE;
  ReadSetting(GetJString(env, key), &value);
  return value ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_org_dolphinemu_dolphinemu_features_primedgun_model_PrimedGunSettings_setBoolean(
    JNIEnv* env, jclass, jstring key, jboolean value)
{
  WriteSetting(GetJString(env, key), value == JNI_TRUE);
}

JNIEXPORT jfloat JNICALL
Java_org_dolphinemu_dolphinemu_features_primedgun_model_PrimedGunSettings_getFloat(
    JNIEnv* env, jclass, jstring key, jfloat default_value)
{
  const std::string name = GetJString(env, key);
  float value = default_value;
  if (!ReadDerivedFloat(name, &value))
    ReadSetting(name, &value);
  return value;
}

JNIEXPORT void JNICALL
Java_org_dolphinemu_dolphinemu_features_primedgun_model_PrimedGunSettings_setFloat(
    JNIEnv* env, jclass, jstring key, jfloat value)
{
  const std::string name = GetJString(env, key);
  if (!WriteDerivedFloat(name, value))
    WriteSetting(name, static_cast<float>(value));
}

JNIEXPORT jint JNICALL
Java_org_dolphinemu_dolphinemu_features_primedgun_model_PrimedGunSettings_getInt(
    JNIEnv* env, jclass, jstring key, jint default_value)
{
  int value = default_value;
  ReadSetting(GetJString(env, key), &value);
  return value;
}

JNIEXPORT void JNICALL
Java_org_dolphinemu_dolphinemu_features_primedgun_model_PrimedGunSettings_setInt(
    JNIEnv* env, jclass, jstring key, jint value)
{
  WriteSetting(GetJString(env, key), static_cast<int>(value));
}

JNIEXPORT jboolean JNICALL
Java_org_dolphinemu_dolphinemu_features_primedgun_model_PrimedGunSettings_save(JNIEnv*, jclass)
{
  return PrimedGun::SaveRuntimeSettings(PrimedGun::GetRuntimeSettings()) ? JNI_TRUE : JNI_FALSE;
}

// The reset actions below mirror the buttons on the Qt window so both frontends restore the same
// values. They deliberately assign literals rather than reaching for RuntimeSettings{} wholesale,
// because each button owns only its own section.

JNIEXPORT void JNICALL
Java_org_dolphinemu_dolphinemu_features_primedgun_model_PrimedGunSettings_resetController(JNIEnv*,
                                                                                          jclass)
{
  PrimedGun::RuntimeSettings s = PrimedGun::GetRuntimeSettings();
  s.use_right_hand = true;
  s.vr_overlays_enabled = true;
  s.vr_menu_hold_left_stick = false;
  s.vr_menu_requires_head_zone = false;
  s.vr_menu_floating = false;
  s.cinematic_screen_enabled = false;
  s.game_menu_screen_enabled = false;
  s.rumble_enabled = true;
  s.rumble_intensity = 0.35f;
  s.rumble_hand_mode = 2;
  s.xr_dpad_enabled = true;
  s.combat_jump_use_primary_button = false;
  s.primedgun_grip_inputs_enabled = true;
  s.primedgun_grip_inputs_use_trackpad = false;
  s.primedgun_trackpad_press_threshold = 0.5f;
  s.primedgun_index_grip_press_threshold = 0.5f;
  s.directional_movement_enabled = true;
  s.directional_movement_use_right_stick = false;
  s.directional_movement_use_hmd_direction = false;
  s.xr_dpad_head_radius = 0.28f;
  s.xr_dpad_head_y_below = 0.02f;
  s.xr_dpad_deadzone = 0.45f;
  s.directional_movement_deadzone = 0.25f;
  s.directional_movement_speed = 14.0f;
  s.directional_movement_accel = 45.0f;
  s.directional_movement_air_accel = 8.0f;
  s.look_yaw_sensitivity = 1.0f;
  s.snap_turn_enabled = false;
  s.snap_turn_degrees = 45;
  PrimedGun::SetRuntimeSettings(s);
}

JNIEXPORT void JNICALL
Java_org_dolphinemu_dolphinemu_features_primedgun_model_PrimedGunSettings_resetHud(JNIEnv*, jclass)
{
  const PrimedGun::RuntimeSettings defaults{};
  PrimedGun::RuntimeSettings s = PrimedGun::GetRuntimeSettings();
  s.metroid_hud_distance = defaults.metroid_hud_distance;
  s.metroid_hud_size = defaults.metroid_hud_size;
  s.metroid_hud_offset_up = defaults.metroid_hud_offset_up;
  s.metroid_hud_offset_down = defaults.metroid_hud_offset_down;
  s.metroid_hud_offset_left = defaults.metroid_hud_offset_left;
  s.metroid_hud_offset_right = defaults.metroid_hud_offset_right;
  PrimedGun::SetRuntimeSettings(s);
}

JNIEXPORT void JNICALL
Java_org_dolphinemu_dolphinemu_features_primedgun_model_PrimedGunSettings_resetTargeting(JNIEnv*,
                                                                                         jclass)
{
  PrimedGun::RuntimeSettings s = PrimedGun::GetRuntimeSettings();
  s.gun_targeting_enabled = true;
  s.gun_targeting_distance = 60.0f;
  s.gun_targeting_radius = 4.0f;
  s.visor_helmet_enabled = false;
  PrimedGun::SetRuntimeSettings(s);
}

JNIEXPORT void JNICALL
Java_org_dolphinemu_dolphinemu_features_primedgun_model_PrimedGunSettings_resetCalibration(JNIEnv*,
                                                                                           jclass)
{
  PrimedGun::RuntimeSettings s = PrimedGun::GetRuntimeSettings();
  s.model_offset_x = 0.0f;
  s.model_offset_y = 0.0f;
  s.model_offset_z = 0.0f;
  s.rot_offset_x = 0.0f;
  s.rot_offset_y = 0.0f;
  s.rot_offset_z = 0.0f;
  PrimedGun::SetRuntimeSettings(s);
}

JNIEXPORT void JNICALL
Java_org_dolphinemu_dolphinemu_features_primedgun_model_PrimedGunSettings_applySamusArmPreset(
    JNIEnv*, jclass)
{
  PrimedGun::RuntimeSettings s = PrimedGun::GetRuntimeSettings();
  s.model_offset_x = 0.0f;
  s.model_offset_y = -0.300f;
  s.model_offset_z = 0.0f;
  s.rot_offset_x = 0.0f;
  s.rot_offset_y = 20.0f;
  s.rot_offset_z = -90.0f;
  PrimedGun::SetRuntimeSettings(s);
}
}
