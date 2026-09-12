// Copyright 2026 PrimedGun Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PrimedGun/Settings.h"

#include <mutex>
#include <string>

#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Common/Logging/Log.h"

#include "Core/PrimedGun/NativeRuntime.h"

namespace PrimedGun
{
namespace
{
constexpr char SETTINGS_FILE_NAME[] = "PrimedGun.ini";
constexpr char SETTINGS_SECTION[] = "Runtime";

// DolphinQt stored these in Qt.ini's [primedgun] group before persistence moved into Core, and
// the key names are unchanged so that file can still be imported. Nothing else reads it.
constexpr char LEGACY_SETTINGS_FILE_NAME[] = "Qt.ini";
constexpr char LEGACY_SETTINGS_SECTION[] = "primedgun";
// Even older builds used this group name, before the project was renamed.
constexpr char OLDEST_SETTINGS_SECTION[] = "primegun";

std::mutex s_load_mutex;
bool s_loaded = false;

// The single list of persisted settings, shared by loading and saving so a new setting only has
// to be added here to persist on every platform. Members left out are deliberately not user
// settings: the patch_* / builtin_patches_enabled flags are internal and no UI exposes them (the
// DolphinQt loader read them and then pinned them back to these same defaults), offset_x/y/z are
// legacy calibration superseded by model_offset_*, and require_trigger/trigger_threshold are
// force-reset by SetRuntimeSettings().
template <typename Visitor>
void VisitPersistedSettings(RuntimeSettings& s, Visitor&& visit)
{
  visit("enabled", s.enabled);
  visit("use_right_hand", s.use_right_hand);
  visit("model_offset_x", s.model_offset_x);
  visit("model_offset_y", s.model_offset_y);
  visit("model_offset_z", s.model_offset_z);
  visit("rot_offset_x", s.rot_offset_x);
  visit("rot_offset_y", s.rot_offset_y);
  visit("rot_offset_z", s.rot_offset_z);
  visit("world_scale", s.world_scale);
  visit("rumble_enabled", s.rumble_enabled);
  visit("rumble_intensity", s.rumble_intensity);
  visit("rumble_hand_mode", s.rumble_hand_mode);
  visit("primedgun_grip_inputs_enabled", s.primedgun_grip_inputs_enabled);
  visit("primedgun_grip_inputs_use_trackpad", s.primedgun_grip_inputs_use_trackpad);
  visit("primedgun_trackpad_press_threshold", s.primedgun_trackpad_press_threshold);
  visit("primedgun_index_grip_press_threshold", s.primedgun_index_grip_press_threshold);
  visit("combat_jump_use_primary_button", s.combat_jump_use_primary_button);
  visit("vr_menu_hold_left_stick", s.vr_menu_hold_left_stick);
  visit("vr_menu_requires_head_zone", s.vr_menu_requires_head_zone);
  visit("vr_menu_floating", s.vr_menu_floating);
  visit("cinematic_screen_enabled", s.cinematic_screen_enabled);
  visit("game_menu_screen_enabled", s.game_menu_screen_enabled);
  visit("frustum_culling_enabled", s.frustum_culling_enabled);
  visit("frustum_culling_degrees", s.frustum_culling_degrees);
  visit("metroid_hud_distance", s.metroid_hud_distance);
  visit("metroid_hud_size", s.metroid_hud_size);
  visit("metroid_hud_offset_up", s.metroid_hud_offset_up);
  visit("metroid_hud_offset_down", s.metroid_hud_offset_down);
  visit("metroid_hud_offset_left", s.metroid_hud_offset_left);
  visit("metroid_hud_offset_right", s.metroid_hud_offset_right);
  visit("gun_targeting_enabled", s.gun_targeting_enabled);
  visit("gun_targeting_distance", s.gun_targeting_distance);
  visit("gun_targeting_radius", s.gun_targeting_radius);
  visit("visor_helmet_enabled", s.visor_helmet_enabled);
  visit("vr_overlays_enabled", s.vr_overlays_enabled);
  visit("height_prompt_enabled", s.height_prompt_enabled);
  visit("position_marker_enabled", s.position_marker_enabled);
  visit("xr_dpad_enabled", s.xr_dpad_enabled);
  visit("xr_dpad_head_radius", s.xr_dpad_head_radius);
  visit("xr_dpad_head_y_below", s.xr_dpad_head_y_below);
  visit("xr_dpad_deadzone", s.xr_dpad_deadzone);
  visit("directional_movement_enabled", s.directional_movement_enabled);
  visit("directional_movement_use_right_stick", s.directional_movement_use_right_stick);
  visit("directional_movement_use_hmd_direction", s.directional_movement_use_hmd_direction);
  visit("directional_movement_deadzone", s.directional_movement_deadzone);
  visit("directional_movement_speed", s.directional_movement_speed);
  visit("directional_movement_accel", s.directional_movement_accel);
  visit("directional_movement_air_accel", s.directional_movement_air_accel);
  visit("look_yaw_sensitivity", s.look_yaw_sensitivity);
  visit("snap_turn_enabled", s.snap_turn_enabled);
  visit("snap_turn_degrees", s.snap_turn_degrees);
}

// Reads one INI section into a RuntimeSettings, leaving members without a stored key alone so
// they keep the struct default. Returns false when the section is missing entirely.
bool ApplySectionToSettings(const Common::IniFile& ini, std::string_view section_name,
                            RuntimeSettings* settings)
{
  const Common::IniFile::Section* section = ini.GetSection(section_name);
  if (section == nullptr)
    return false;

  VisitPersistedSettings(*settings, [section](const char* key, auto& value) {
    std::remove_reference_t<decltype(value)> parsed{};
    if (section->Get(key, &parsed, value))
      value = parsed;
  });
  return true;
}

// Reads whichever section an INI happens to use, so a file written by any past build still loads.
bool ApplyAnyKnownSectionToSettings(const Common::IniFile& ini, RuntimeSettings* settings)
{
  for (const char* section : {SETTINGS_SECTION, LEGACY_SETTINGS_SECTION, OLDEST_SETTINGS_SECTION})
  {
    if (ApplySectionToSettings(ini, section, settings))
      return true;
  }
  return false;
}
}  // namespace

std::string GetSettingsFilePath()
{
  return File::GetUserPath(D_CONFIG_IDX) + SETTINGS_FILE_NAME;
}

void LoadRuntimeSettings()
{
  {
    std::lock_guard lock{s_load_mutex};
    s_loaded = true;
  }

  RuntimeSettings settings = GetRuntimeSettings();

  const std::string path = GetSettingsFilePath();
  Common::IniFile ini;
  if (ini.Load(path) && ApplySectionToSettings(ini, SETTINGS_SECTION, &settings))
  {
    INFO_LOG_FMT(CORE, "PrimedGun: loaded VR settings from {}", path);
  }
  else
  {
    // First run on this install, or an upgrade from a build that kept these in Qt.ini.
    const std::string legacy_path = File::GetUserPath(D_CONFIG_IDX) + LEGACY_SETTINGS_FILE_NAME;
    Common::IniFile legacy_ini;
    if (legacy_ini.Load(legacy_path) && ApplyAnyKnownSectionToSettings(legacy_ini, &settings))
    {
      INFO_LOG_FMT(CORE, "PrimedGun: imported VR settings from {}", legacy_path);
      SetRuntimeSettings(settings);
      SaveRuntimeSettings(GetRuntimeSettings());
      return;
    }

    INFO_LOG_FMT(CORE, "PrimedGun: no saved VR settings, using defaults");
  }

  SetRuntimeSettings(settings);
}

void EnsureRuntimeSettingsLoaded()
{
  {
    std::lock_guard lock{s_load_mutex};
    if (s_loaded)
      return;
  }

  LoadRuntimeSettings();
}

bool SaveRuntimeSettings(const RuntimeSettings& settings)
{
  const std::string path = GetSettingsFilePath();

  // Keep any unrelated sections a future version may add to this file.
  Common::IniFile ini;
  ini.Load(path);

  RuntimeSettings values = settings;
  Common::IniFile::Section* section = ini.GetOrCreateSection(SETTINGS_SECTION);
  VisitPersistedSettings(values,
                         [section](const char* key, auto& value) { section->Set(key, value); });

  if (!ini.Save(path))
  {
    ERROR_LOG_FMT(CORE, "PrimedGun: failed to save VR settings to {}", path);
    return false;
  }

  INFO_LOG_FMT(CORE, "PrimedGun: saved VR settings to {}", path);
  return true;
}

bool ImportRuntimeSettings(const std::string& path)
{
  Common::IniFile ini;
  if (!ini.Load(path))
  {
    WARN_LOG_FMT(CORE, "PrimedGun: could not read VR settings to import from {}", path);
    return false;
  }

  RuntimeSettings settings = GetRuntimeSettings();
  const bool was_enabled = settings.enabled;
  if (!ApplyAnyKnownSectionToSettings(ini, &settings))
  {
    WARN_LOG_FMT(CORE, "PrimedGun: no VR settings found in {}", path);
    return false;
  }

  settings.enabled = was_enabled;
  SetRuntimeSettings(settings);
  SaveRuntimeSettings(GetRuntimeSettings());
  INFO_LOG_FMT(CORE, "PrimedGun: imported VR settings from {}", path);
  return true;
}

void ProcessPendingVrSettingsSave()
{
  if (!ConsumeVrSettingsSaveRequest())
    return;

  SaveRuntimeSettings(GetRuntimeSettings());
  MarkVrSettingsSaved();
}
}  // namespace PrimedGun
