// Copyright 2026 PrimedGun Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Key-addressed JNI access to PrimedGun::RuntimeSettings for the Android frontend. The Quest
// build has no Qt host, so this is the only way its settings UI can reach the mod's tuning
// values. Lookups run through the same VisitPersistedSettings() table that loading and saving
// use, so a setting added there becomes reachable here without touching this file.

#include <algorithm>
#include <iterator>
#include <optional>
#include <string>
#include <type_traits>

#include <fmt/format.h>
#include <jni.h>

#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Version.h"

#include "Core/Config/MainSettings.h"
#include "Core/HW/AddressSpace.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PrimedGun/NativeRuntime.h"
#include "Core/PrimedGun/Settings.h"
#include "Core/PrimedGun/SettingsVisitor.h"
#include "Core/System.h"

#include "DiscIO/Enums.h"

#include "jni/AndroidCommon/AndroidCommon.h"
#include "jni/Host.h"

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

// Writes MEM1 and a register snapshot next to it, the way the Qt window's Dump RAM button does,
// so a Quest crash report can carry the same files. Returns the dump path, or an empty string
// when memory is not mapped (no game running) or the files could not be written.
std::string DumpMem1ToUserDirectory()
{
  AddressSpace::Accessors* accessors = AddressSpace::GetAccessors(AddressSpace::Type::Mem1);
  if (!accessors || !accessors->begin())
    return {};

  const std::string path = File::GetUserPath(F_MEM1DUMP_IDX);
  File::CreateFullPath(path);
  File::IOFile file(path, "wb");
  if (!file)
    return {};

  const size_t size = static_cast<size_t>(std::distance(accessors->begin(), accessors->end()));
  if (!file.WriteBytes(accessors->begin(), size))
    return {};

  File::IOFile context_file(path + ".txt", "wb");
  if (context_file)
  {
    const auto& ppc_state = Core::System::GetInstance().GetPPCState();
    std::string context = fmt::format("PrimedGun RAM dump context\n"
                                      "PC={:08X}\n"
                                      "NPC={:08X}\n"
                                      "LR={:08X}\n"
                                      "CTR={:08X}\n"
                                      "SRR0={:08X}\n"
                                      "SRR1={:08X}\n"
                                      "DSISR={:08X}\n"
                                      "DAR={:08X}\n"
                                      "Exceptions={:08X}\n",
                                      ppc_state.pc, ppc_state.npc, ppc_state.spr[SPR_LR],
                                      ppc_state.spr[SPR_CTR], ppc_state.spr[SPR_SRR0],
                                      ppc_state.spr[SPR_SRR1], ppc_state.spr[SPR_DSISR],
                                      ppc_state.spr[SPR_DAR], ppc_state.Exceptions);
    context += "\nGPRs\n";
    for (int reg = 0; reg < 32; reg += 4)
    {
      context += fmt::format("R{:02}={:08X} R{:02}={:08X} R{:02}={:08X} R{:02}={:08X}\n", reg,
                             ppc_state.gpr[reg], reg + 1, ppc_state.gpr[reg + 1], reg + 2,
                             ppc_state.gpr[reg + 2], reg + 3, ppc_state.gpr[reg + 3]);
    }
    context_file.WriteBytes(context.data(), context.size());
  }

  return path;
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

JNIEXPORT void JNICALL
Java_org_dolphinemu_dolphinemu_features_primedgun_model_PrimedGunSettings_resetAll(JNIEnv*, jclass)
{
  // Same as the Qt window's Reset All: every runtime setting back to its struct default, with the
  // legacy offsets cleared as well.
  PrimedGun::RuntimeSettings s{};
  s.offset_x = 0.0f;
  s.offset_y = 0.0f;
  s.offset_z = 0.0f;
  PrimedGun::SetRuntimeSettings(s);
}

JNIEXPORT jstring JNICALL
Java_org_dolphinemu_dolphinemu_features_primedgun_model_PrimedGunSettings_getVersion(JNIEnv* env,
                                                                                     jclass)
{
  return ToJString(env, Common::GetScmDescStr());
}

JNIEXPORT jstring JNICALL
Java_org_dolphinemu_dolphinemu_features_primedgun_model_PrimedGunSettings_getMemoryCardPath(
    JNIEnv* env, jclass)
{
  // Metroid Prime NTSC-U saves to the USA card in slot A; the Qt transfer targets the same file.
  return ToJString(env,
                   Config::GetMemcardPath(ExpansionInterface::Slot::A, DiscIO::Region::NTSC_U));
}

JNIEXPORT jboolean JNICALL
Java_org_dolphinemu_dolphinemu_features_primedgun_model_PrimedGunSettings_importSettings(
    JNIEnv* env, jclass, jstring path)
{
  return PrimedGun::ImportRuntimeSettings(GetJString(env, path)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_org_dolphinemu_dolphinemu_features_primedgun_model_PrimedGunSettings_applyCannonTextureSlot(
    JNIEnv*, jclass, jint slot)
{
  HostThreadLock guard;
  return PrimedGun::ApplyCannonTextureSlot(static_cast<int>(slot)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_org_dolphinemu_dolphinemu_features_primedgun_model_PrimedGunSettings_dumpMem1(JNIEnv* env,
                                                                                   jclass)
{
  HostThreadLock guard;
  return ToJString(env, DumpMem1ToUserDirectory());
}
}
