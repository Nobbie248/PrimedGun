// Copyright 2026 PrimedGun Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PrimedGun/RuntimeSettingsFile.h"

#include <charconv>
#include <mutex>
#include <utility>

#include "Common/IniFile.h"
#include "Common/Logging/Log.h"

namespace PrimedGun
{
namespace
{
constexpr const char* SECTION = "primedgun";

// Every key the Qt frontend saves, in its order (B bool, F float, I int).
#define PRIMEDGUN_RS_KEYS(B, F, I)                                                                 \
  B(enabled)                                                                                       \
  B(builtin_patches_enabled)                                                                       \
  B(patch_disable_frustum_culling)                                                                 \
  B(patch_no_idle_sway)                                                                            \
  B(patch_disable_arm_cannon_idle_fidget)                                                          \
  B(patch_beam_projectile_timing)                                                                  \
  B(patch_xr_visor_dpad_timing)                                                                    \
  B(patch_cannon_rotation)                                                                         \
  B(patch_gun_ray_target)                                                                          \
  B(patch_reticle)                                                                                 \
  B(use_right_hand)                                                                                \
  F(model_offset_x)                                                                                \
  F(model_offset_y)                                                                                \
  F(model_offset_z)                                                                                \
  F(rot_offset_x)                                                                                  \
  F(rot_offset_y)                                                                                  \
  F(rot_offset_z)                                                                                  \
  F(world_scale)                                                                                   \
  B(require_trigger)                                                                               \
  F(trigger_threshold)                                                                             \
  B(rumble_enabled)                                                                                \
  F(rumble_intensity)                                                                              \
  I(rumble_hand_mode)                                                                              \
  B(primedgun_grip_inputs_enabled)                                                                 \
  B(primedgun_grip_inputs_use_trackpad)                                                            \
  F(primedgun_trackpad_press_threshold)                                                            \
  F(primedgun_index_grip_press_threshold)                                                          \
  B(combat_jump_use_primary_button)                                                                \
  B(vr_menu_hold_left_stick)                                                                       \
  B(vr_menu_requires_head_zone)                                                                    \
  B(vr_menu_floating)                                                                              \
  B(cinematic_screen_enabled)                                                                      \
  B(game_menu_screen_enabled)                                                                      \
  B(frustum_culling_enabled)                                                                       \
  F(frustum_culling_degrees)                                                                       \
  F(metroid_hud_distance)                                                                          \
  F(metroid_hud_size)                                                                              \
  F(metroid_hud_offset_up)                                                                         \
  F(metroid_hud_offset_down)                                                                       \
  F(metroid_hud_offset_left)                                                                       \
  F(metroid_hud_offset_right)                                                                      \
  B(gun_targeting_enabled)                                                                         \
  F(gun_targeting_distance)                                                                        \
  F(gun_targeting_radius)                                                                          \
  B(visor_helmet_enabled)                                                                          \
  B(vr_overlays_enabled)                                                                           \
  B(height_prompt_enabled)                                                                         \
  B(position_marker_enabled)                                                                       \
  B(xr_dpad_enabled)                                                                               \
  F(xr_dpad_head_radius)                                                                           \
  F(xr_dpad_head_y_below)                                                                          \
  F(xr_dpad_deadzone)                                                                              \
  B(directional_movement_enabled)                                                                  \
  B(directional_movement_use_right_stick)                                                          \
  B(directional_movement_use_hmd_direction)                                                        \
  F(directional_movement_deadzone)                                                                 \
  F(directional_movement_speed)                                                                    \
  F(directional_movement_accel)                                                                    \
  F(directional_movement_air_accel)                                                                \
  F(look_yaw_sensitivity)                                                                          \
  B(snap_turn_enabled)                                                                             \
  I(snap_turn_degrees)

// Shortest text that reads back as the same float ("1", "0.08", "1.15"), like the Qt frontend writes.
std::string FloatText(float value)
{
  char buffer[32];
  const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  return std::string(buffer, result.ptr);
}

std::mutex s_persist_mutex;
std::string s_persist_path;
std::function<void()> s_exit_game;
}  // namespace

bool LoadRuntimeSettingsFile(const std::string& path, RuntimeSettings* settings)
{
  Common::IniFile ini;
  const bool loaded = ini.Load(path) && ini.Exists(SECTION);
  if (loaded)
  {
#define PG_LOAD(name) ini.GetIfExists(SECTION, #name, &settings->name);
    PRIMEDGUN_RS_KEYS(PG_LOAD, PG_LOAD, PG_LOAD)
#undef PG_LOAD
  }

  // The same overrides the Qt frontend applies after loading.
  settings->builtin_patches_enabled = true;
  settings->patch_disable_frustum_culling = false;
  settings->patch_no_idle_sway = true;
  settings->patch_disable_arm_cannon_idle_fidget = true;
  settings->patch_beam_projectile_timing = true;
  settings->patch_xr_visor_dpad_timing = true;
  settings->patch_cannon_rotation = true;
  settings->patch_gun_ray_target = true;
  settings->patch_reticle = true;
  settings->offset_x = 0.0f;
  settings->offset_y = 0.0f;
  settings->offset_z = 0.0f;
  settings->require_trigger = false;
  settings->trigger_threshold = 0.5f;
  return loaded;
}

bool SaveRuntimeSettingsFile(const std::string& path, const RuntimeSettings& settings)
{
  Common::IniFile ini;
  ini.Load(path);  // keep unrelated sections and keys; a missing file is fine
  Common::IniFile::Section* section = ini.GetOrCreateSection(SECTION);
#define PG_SAVE_B(name) section->Set(#name, std::string(settings.name ? "true" : "false"));
#define PG_SAVE_F(name) section->Set(#name, FloatText(settings.name));
#define PG_SAVE_I(name) section->Set(#name, settings.name);
  PRIMEDGUN_RS_KEYS(PG_SAVE_B, PG_SAVE_F, PG_SAVE_I)
#undef PG_SAVE_B
#undef PG_SAVE_F
#undef PG_SAVE_I
  return ini.Save(path);
}

void EnableHostPersistence(const std::string& path, std::function<void()> exit_game)
{
  std::lock_guard lock{s_persist_mutex};
  s_persist_path = path;
  s_exit_game = std::move(exit_game);
}

bool HostPersistenceEnabled()
{
  std::lock_guard lock{s_persist_mutex};
  return !s_persist_path.empty();
}

bool SaveHostPersistence(const char* reason)
{
  std::string path;
  {
    std::lock_guard lock{s_persist_mutex};
    path = s_persist_path;
  }
  if (path.empty())
    return false;
  const bool ok = SaveRuntimeSettingsFile(path, GetRuntimeSettings());
  if (ok)
    NOTICE_LOG_FMT(CORE, "PrimedGun: settings saved to {} ({})", path, reason);
  else
    ERROR_LOG_FMT(CORE, "PrimedGun: could not save settings to {} ({})", path, reason);
  return ok;
}

void RequestHostExit()
{
  std::function<void()> exit_game;
  {
    std::lock_guard lock{s_persist_mutex};
    exit_game = s_exit_game;
  }
  if (exit_game)
    exit_game();
}
}  // namespace PrimedGun
