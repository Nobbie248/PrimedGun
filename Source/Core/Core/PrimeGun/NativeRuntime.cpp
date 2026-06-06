// Copyright 2026 PrimeGun Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PrimeGun/NativeRuntime.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/MathUtil.h"
#ifdef ENABLE_VR
#include "Common/VR/OpenXRInputState.h"
#endif

#include "Core/Config/GraphicsSettings.h"
#include "Core/Core.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/MMU.h"
#include "Core/System.h"

#include "Core/PrimeGun/PrimeGunBuiltinPatches.inc"

#include "VideoCommon/AsyncRequests.h"
#include "VideoCommon/HiresTextures.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/VideoConfig.h"

namespace PrimeGun
{
namespace
{
struct PatchWrite
{
  u32 address = 0;
  u32 value = 0;
  u32 group = 0;
};

struct Pose
{
  float px = 0.0f;
  float py = 0.0f;
  float pz = 0.0f;
  float qx = 0.0f;
  float qy = 0.0f;
  float qz = 0.0f;
  float qw = 1.0f;
  bool valid = false;
};

struct Matrix3x4
{
  float m[12] = {};

  float& At(int row, int col) { return m[row * 4 + col]; }
  const float& At(int row, int col) const { return m[row * 4 + col]; }
};

struct GameAddresses
{
  u32 state_manager = 0x8045A1A8;
  u32 player_offset = 0x84C;
  u32 camera_manager_offset = 0x86C;
  u32 gun_pos = 0x8045BCE8;
  u32 transform_offset = 0x34;
  u32 cannon_offset = 0x490;
  u32 gun_xf_offset = 0x3E8;
  u32 beam_xf_offset = 0x418;
  u32 world_xf_offset = 0x4A8;
  u32 local_xf_offset = 0x4D8;
};

constexpr GameAddresses ADDRESS{};
constexpr float DEFAULT_WORLD_SCALE = 1.50f;
constexpr float DEFAULT_MODEL_OFFSET_X = 0.0f;
constexpr float DEFAULT_MODEL_OFFSET_Y = 0.0f;
constexpr float DEFAULT_MODEL_OFFSET_Z = 0.0f;
constexpr float BASE_MODEL_FORWARD_BACK_OFFSET = -0.080f;
constexpr float DEFAULT_ROT_OFFSET_X = 0.0f;
constexpr float DEFAULT_ROT_OFFSET_Y = 0.0f;
constexpr float DEFAULT_ROT_OFFSET_Z = 0.0f;

constexpr u32 SCRATCH_BASE = 0x817FE000u;
constexpr u32 CANNON_BASIS_SCRATCH = SCRATCH_BASE + 0x000u;
constexpr u32 CANNON_EXPECTED_GUN_SCRATCH = SCRATCH_BASE + 0x038u;
constexpr u32 MODEL_OFFSET_WORLD_SCRATCH = SCRATCH_BASE + 0x040u;
constexpr u32 ADJUSTED_GUN_POS_SCRATCH = SCRATCH_BASE + 0x050u;
constexpr u32 GUN_TARGET_SCRATCH = SCRATCH_BASE + 0x400u;
constexpr u32 RETICLE_BILLBOARD_SCRATCH = SCRATCH_BASE + 0x500u;
constexpr u32 DPAD_PRESS_SCRATCH = SCRATCH_BASE + 0x680u;
constexpr u32 GAMEFLOW_MENU_SCRATCH = SCRATCH_BASE + 0x690u;

constexpr u32 FIRST_PERSON_PITCH_LOAD_CAVE = 0x80001B70u;
constexpr u32 RENDER_MODEL_OFFSET_CAVE = 0x80001C00u;
constexpr u32 FIRST_PERSON_ELEVATION_PITCH_CAVE = 0x80001C80u;
constexpr u32 COMBAT_PITCH_CAVE_0 = 0x80001E00u;
constexpr u32 COMBAT_PITCH_CAVE_1 = 0x80001E40u;
constexpr u32 COMBAT_PITCH_CAVE_2 = 0x80001E80u;
constexpr u32 COMBAT_ELEVATION_PITCH_CAVE = 0x80001EC0u;
constexpr u32 WAVE_PROJECTILE_TRANSFORM_CAVE = 0x80001F00u;
constexpr u32 MISSILE_PROJECTILE_TRANSFORM_CAVE = 0x80001FA0u;
constexpr u32 GAMEFLOW_UNPAUSE_CAVE = 0x80002040u;
constexpr u32 GAMEFLOW_MESSAGE_CAVE = 0x80002070u;
constexpr u32 GAMEFLOW_SAVE_CAVE = 0x800020A0u;
constexpr u32 GAMEFLOW_LOGBOOK_CAVE = 0x800020D0u;
constexpr u32 GAMEFLOW_PAUSE_CAVE = 0x80002100u;
constexpr u32 GAMEFLOW_MAP_CAVE = 0x80002130u;
constexpr u32 LOAD_ZERO_TO_F1 = 0xC02280B0u;
constexpr u32 LOAD_ZERO_TO_F31 = 0xC3E280B0u;

constexpr u32 VR_MENU_TAB_COUNT = 6;
constexpr u32 VR_MENU_CANNON_TAB = 5;
constexpr const char* PRIMEGUN_CANNON_GAME_ID = "GM8E01";
constexpr const char* PRIMEGUN_CANNON_PACK_FOLDER = "000_PrimedGunCannon";
constexpr const char* PRIMEGUN_CANNON_LIBRARY_FOLDER = "PrimedGun" DIR_SEP "CannonTextures";
constexpr std::array<const char*, 3> PRIMEGUN_CANNON_TEXTURE_NAMES = {
    "tex1_128x128_m_3c6ded49d64d30f2_14",
    "tex1_128x128_m_bec6d78ea7dd739e_14",
    "tex1_64x64_m_c7625e7ecd9cd5c2_14",
};

constexpr u32 FINAL_INPUT_OFFSET = 0xB54u;
constexpr u32 FINAL_INPUT_RIGHT_STICK_X = FINAL_INPUT_OFFSET + 0x10u;
constexpr u32 FINAL_INPUT_RIGHT_STICK_Y = FINAL_INPUT_OFFSET + 0x14u;
constexpr u32 FINAL_INPUT_RIGHT_STICK_X_PRESS = FINAL_INPUT_OFFSET + 0x22u;
constexpr u32 FINAL_INPUT_RIGHT_STICK_Y_PRESS = FINAL_INPUT_OFFSET + 0x23u;
constexpr u32 FINAL_INPUT_DPAD_HELD_0 = FINAL_INPUT_OFFSET + 0x2Cu;
constexpr u32 FINAL_INPUT_DPAD_HELD_1 = FINAL_INPUT_OFFSET + 0x2Du;
constexpr u32 FINAL_INPUT_DPAD_PRESSED_0 = FINAL_INPUT_OFFSET + 0x2Eu;
constexpr u32 PLAYER_DISABLE_INPUT_FLAGS_OFFSET = 0x9C6u;
constexpr u8 PLAYER_DISABLE_INPUT_MASK = 0x04u;
constexpr u32 PLAYER_VELOCITY_OFFSET = 0x138u;
constexpr u32 PLAYER_MOVEMENT_STATE_OFFSET = 0x258u;
constexpr u32 PLAYER_FREE_LOOK_STATE_OFFSET = 0x3DCu;
constexpr u32 PLAYER_FREE_LOOK_CENTER_TIME_OFFSET = 0x3E0u;
constexpr u32 PLAYER_FREE_LOOK_YAW_ANGLE_OFFSET = 0x3E4u;
constexpr u32 PLAYER_FREE_LOOK_YAW_VEL_OFFSET = 0x3E8u;
constexpr u32 PLAYER_FREE_LOOK_PITCH_ANGLE_OFFSET = 0x3ECu;
constexpr u32 PLAYER_VISOR_STATE_OFFSET = 0x330u;
constexpr u32 GP_GAME_STATE = 0x805A8C40u;
constexpr u32 GAME_OPTIONS_HELMET_ALPHA_OFFSET = 0x17Cu + 0x64u;

enum PatchGroup : u32
{
  PatchDisableFrustumCulling = 0,
  PatchNoIdleSway,
  PatchDisableArmCannonIdleFidget,
  PatchBeamProjectileTiming,
  PatchXrVisorDpadTiming,
  PatchCannonRotation,
  PatchGunRayTarget,
  PatchReticle,
  PatchUnknown,
};

std::once_flag s_parse_builtin_patches_once;
std::vector<PatchWrite> s_builtin_patches;
std::mutex s_settings_mutex;
RuntimeSettings s_settings;
u64 s_frame_counter = 0;
bool s_game_was_active = false;
bool s_patches_applied_this_boot = false;
bool s_scan_was_active = false;
u32 s_scan_last_player = 0;
u32 s_scan_normal_frames = 0;
float s_smooth_scan_pitch = 0.0f;
bool s_translation_base_valid = false;
float s_controller_base_x = 0.0f;
float s_controller_base_y = 0.0f;
float s_controller_base_z = 0.0f;
bool s_last_height_reset_thumbstick = false;
bool s_last_vr_menu_thumbstick = false;
bool s_last_vr_menu_trigger = false;
bool s_last_vr_menu_primary = false;
bool s_last_vr_menu_secondary = false;
bool s_last_vr_menu_stick_up = false;
bool s_last_vr_menu_stick_down = false;
bool s_last_vr_menu_stick_left = false;
bool s_last_vr_menu_stick_right = false;
bool s_vr_menu_visible = false;
u32 s_vr_menu_tab = 0;
u32 s_vr_menu_selected_index = 0;
u32 s_vr_menu_generation = 1;
u64 s_vr_menu_saved_notice_until_frame = 0;
u64 s_vr_menu_input_suppress_until_frame = 0;
u32 s_vr_cannon_texture_slot = 0;
u64 s_vr_cannon_texture_notice_until_frame = 0;
bool s_vr_settings_save_requested = false;
u64 s_height_prompt_until_frame = 0;
u64 s_prompt_gameplay_ready_since_frame = 0;
u64 s_prompt_first_ready_timeout_frame = 0;
bool s_prompt_skipped_first_ready = false;
bool s_prompt_waiting_for_second_ready = false;
bool s_prompt_shown_this_session = false;
bool s_dpad_forced_input_disabled = false;
bool s_dpad_input_was_disabled = false;
u32 s_dpad_input_flags_addr = 0;
u64 s_dpad_last_disable_refresh_frame = 0;
float s_directional_move_speed = 0.0f;
std::atomic_bool s_gameplay_input_active{false};
std::atomic_bool s_orbit_lock_active{false};
bool s_last_logged_gameplay_input_active = false;
bool s_have_logged_gameplay_input_active = false;
u64 s_last_mode_probe_frame = 0;
u32 s_last_mode_probe_camera_state = 0xffffffffu;
u32 s_last_mode_probe_morph_state = 0xffffffffu;
u32 s_last_mode_probe_movement_state = 0xffffffffu;
u32 s_last_mode_probe_visor_state = 0xffffffffu;
u32 s_last_mode_probe_holster_state = 0xffffffffu;
u8 s_last_mode_probe_input_flags = 0xffu;
float s_last_mode_probe_gun_alpha = -1.0f;
std::array<u32, 16> s_last_mode_probe_state_words{};
std::array<u32, 16> s_last_mode_probe_game_words{};
bool s_have_mode_probe_words = false;
u32 s_last_validated_gun = 0;
bool s_smooth_matrix_valid = false;
float s_smooth_matrix[12] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
                             0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};

struct DynamicPpcPatch
{
  u32 address = 0;
  u32 original = 0;
  u32 replacement = 0;
  u32 cave = 0;
  bool applied = false;
};

struct ConditionalPitchLoadPatch
{
  u32 address = 0;
  u32 original = 0;
  u32 cave = 0;
  bool applied = false;
};

struct ProjectileTransformPatch
{
  u32 address = 0;
  u32 original = 0;
  u32 cave = 0;
  u32 transform_register = 0;
  bool applied = false;
};

struct GameFlowFlagPatch
{
  u32 address = 0;
  u32 original = 0;
  u32 cave = 0;
  u32 menu_flag = 0;
  bool applied = false;
};

ConditionalPitchLoadPatch s_first_person_pitch_load_patch{
    0x8000E548u, 0xC3FE03ECu, FIRST_PERSON_PITCH_LOAD_CAVE, false};

DynamicPpcPatch s_render_model_offset_patch{
    0x80041A8Cu, 0, 0, RENDER_MODEL_OFFSET_CAVE, false};

DynamicPpcPatch s_combat_pitch_patches[] = {
    {0x8000E7B4u, 0xEC21E828u, LOAD_ZERO_TO_F1, COMBAT_PITCH_CAVE_0, false},
    {0x8000E808u, 0xEC21E828u, LOAD_ZERO_TO_F1, COMBAT_PITCH_CAVE_1, false},
    {0x8000E83Cu, 0xEC21E828u, LOAD_ZERO_TO_F1, COMBAT_PITCH_CAVE_2, false},
};

DynamicPpcPatch s_combat_elevation_pitch_patch{
    0x8000FA50u, 0xD01D01C0u, LOAD_ZERO_TO_F1, COMBAT_ELEVATION_PITCH_CAVE, false};

ProjectileTransformPatch s_projectile_transform_patches[] = {
    {0x800E0434u, 0, WAVE_PROJECTILE_TRANSFORM_CAVE, 6, false},
    {0x801B9070u, 0, MISSILE_PROJECTILE_TRANSFORM_CAVE, 8, false},
};

GameFlowFlagPatch s_game_flow_flag_patches[] = {
    {0x800243CCu, 0, GAMEFLOW_UNPAUSE_CAVE, 0, false},
    {0x80024414u, 0, GAMEFLOW_MESSAGE_CAVE, 1, false},
    {0x80024450u, 0, GAMEFLOW_SAVE_CAVE, 1, false},
    {0x8002448Cu, 0, GAMEFLOW_LOGBOOK_CAVE, 1, false},
    {0x800244C8u, 0, GAMEFLOW_PAUSE_CAVE, 1, false},
    {0x80024504u, 0, GAMEFLOW_MAP_CAVE, 1, false},
};

enum DpadDir
{
  DpadNone = 0,
  DpadUp,
  DpadRight,
  DpadDown,
  DpadLeft,
};

std::string_view Trim(std::string_view text)
{
  const size_t first = text.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos)
    return {};

  const size_t last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

bool ParseHexU32(std::string_view text, u32* out)
{
  text = Trim(text);
  if (text.starts_with("0x") || text.starts_with("0X"))
    text.remove_prefix(2);

  u32 value = 0;
  const auto* begin = text.data();
  const auto* end = begin + text.size();
  const auto result = std::from_chars(begin, end, value, 16);
  if (result.ec != std::errc{} || result.ptr != end)
    return false;

  *out = value;
  return true;
}

u32 PatchGroupFromHeader(std::string_view line)
{
  if (line.find("Disable Frustum Culling") != std::string_view::npos)
    return PatchDisableFrustumCulling;
  if (line.find("No Idle Sway") != std::string_view::npos)
    return PatchNoIdleSway;
  if (line.find("Disable Arm Cannon Idle Fidget") != std::string_view::npos)
    return PatchDisableArmCannonIdleFidget;
  if (line.find("Beam Projectile Timing Hook") != std::string_view::npos)
    return PatchBeamProjectileTiming;
  if (line.find("XR Visor D-Pad Timing Hook") != std::string_view::npos)
    return PatchXrVisorDpadTiming;
  if (line.find("Cannon Rotation Hook") != std::string_view::npos)
    return PatchCannonRotation;
  if (line.find("Gun Ray Lock/Scan Target Hook") != std::string_view::npos)
    return PatchGunRayTarget;
  if (line.find("Reticle Hook") != std::string_view::npos)
    return PatchReticle;
  return PatchUnknown;
}

bool PatchGroupEnabled(const RuntimeSettings& settings, u32 group)
{
  switch (group)
  {
  case PatchDisableFrustumCulling:
    return settings.patch_disable_frustum_culling;
  case PatchNoIdleSway:
    return settings.patch_no_idle_sway;
  case PatchDisableArmCannonIdleFidget:
    return settings.patch_disable_arm_cannon_idle_fidget;
  case PatchBeamProjectileTiming:
    return settings.patch_beam_projectile_timing;
  case PatchXrVisorDpadTiming:
    return settings.patch_xr_visor_dpad_timing;
  case PatchCannonRotation:
    return settings.patch_cannon_rotation;
  case PatchGunRayTarget:
    return settings.patch_gun_ray_target;
  case PatchReticle:
    return settings.patch_reticle;
  default:
    return true;
  }
}

void ParseBuiltinPatches()
{
  std::string_view text{kPrimeGunBuiltinPatches};
  u32 current_group = PatchUnknown;
  while (!text.empty())
  {
    const size_t newline = text.find('\n');
    std::string_view line = newline == std::string_view::npos ? text : text.substr(0, newline);
    text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);

    line = Trim(line);
    if (line.starts_with('$'))
    {
      current_group = PatchGroupFromHeader(line);
      continue;
    }
    if (line.empty() || line.starts_with('#') || line.starts_with('['))
      continue;

    const size_t separator = line.find_first_of(" \t");
    if (separator == std::string_view::npos)
      continue;

    u32 command = 0;
    u32 value = 0;
    if (!ParseHexU32(line.substr(0, separator), &command) ||
        !ParseHexU32(line.substr(separator + 1), &value))
    {
      continue;
    }

    // PrimeGun's built-in patch block currently uses AR 04 dword writes only.
    if (static_cast<u8>(command >> 24) != 0x04)
      continue;

    s_builtin_patches.push_back({0x80000000u | (command & 0x01ff'ffffu), value, current_group});
  }
}

bool IsMetroidPrimeRev0(const Core::CPUThreadGuard& guard)
{
  constexpr std::array<u8, 6> game_id = {'G', 'M', '8', 'E', '0', '1'};
  for (size_t i = 0; i < game_id.size(); ++i)
  {
    const auto byte = PowerPC::MMU::HostTryRead<u8>(guard, 0x80000000u + static_cast<u32>(i));
    if (!byte || byte->value != game_id[i])
      return false;
  }

  return true;
}

Matrix3x4 PoseToPrimeMatrix(const Pose& pose, float ox, float oy, float oz, float rx_deg,
                            float ry_deg, float rz_deg, float scale)
{
  auto quat_mul = [](float ax, float ay, float az, float aw, float bx, float by, float bz,
                     float bw, float& ox_out, float& oy_out, float& oz_out, float& ow_out) {
    ox_out = aw * bx + ax * bw + ay * bz - az * by;
    oy_out = aw * by - ax * bz + ay * bw + az * bx;
    oz_out = aw * bz + ax * by - ay * bx + az * bw;
    ow_out = aw * bw - ax * bx - ay * by - az * bz;
  };

  float qx = pose.qx;
  float qy = pose.qy;
  float qz = pose.qz;
  float qw = pose.qw;

  constexpr float deg_to_rad = static_cast<float>(MathUtil::PI / 180.0);
  const float local_pitch = rx_deg * deg_to_rad;
  const float local_yaw = ry_deg * deg_to_rad;
  const float local_roll = rz_deg * deg_to_rad;

  const float hp = local_pitch * 0.5f;
  const float hy = local_yaw * 0.5f;
  const float hr = local_roll * 0.5f;

  const float sx = std::sin(hp);
  const float cx = std::cos(hp);
  const float sy = std::sin(hy);
  const float cy = std::cos(hy);
  const float sz = std::sin(hr);
  const float cz = std::cos(hr);

  float cq_x, cq_y, cq_z, cq_w;
  quat_mul(0.0f, sy, 0.0f, cy, sx, 0.0f, 0.0f, cx, cq_x, cq_y, cq_z, cq_w);
  float cr_x, cr_y, cr_z, cr_w;
  quat_mul(cq_x, cq_y, cq_z, cq_w, 0.0f, 0.0f, sz, cz, cr_x, cr_y, cr_z, cr_w);

  quat_mul(qx, qy, qz, qw, cr_x, cr_y, cr_z, cr_w, qx, qy, qz, qw);

  const float r00 = 1 - 2 * (qy * qy + qz * qz);
  const float r01 = 2 * (qx * qy - qw * qz);
  const float r02 = 2 * (qx * qz + qw * qy);
  const float r10 = 2 * (qx * qy + qw * qz);
  const float r11 = 1 - 2 * (qx * qx + qz * qz);
  const float r12 = 2 * (qy * qz - qw * qx);
  const float r20 = 2 * (qx * qz - qw * qy);
  const float r21 = 2 * (qy * qz + qw * qx);
  const float r22 = 1 - 2 * (qx * qx + qy * qy);

  Matrix3x4 mat = {};
  mat.At(0, 0) = -r00;
  mat.At(0, 1) = r02;
  mat.At(0, 2) = -r01;
  mat.At(1, 0) = r20;
  mat.At(1, 1) = -r22;
  mat.At(1, 2) = r21;
  mat.At(2, 0) = r10;
  mat.At(2, 1) = -r12;
  mat.At(2, 2) = r11;
  mat.At(0, 3) = (pose.px + ox) * scale;
  mat.At(1, 3) = -(pose.pz + oz) * scale;
  mat.At(2, 3) = (pose.py + oy) * scale;
  return mat;
}

bool TryReadU32(const Core::CPUThreadGuard& guard, u32 address, u32* out)
{
  const auto value = PowerPC::MMU::HostTryRead<u32>(guard, address);
  if (!value)
    return false;

  *out = value->value;
  return true;
}

bool TryReadU8(const Core::CPUThreadGuard& guard, u32 address, u8* out)
{
  const auto value = PowerPC::MMU::HostTryRead<u8>(guard, address);
  if (!value)
    return false;

  *out = value->value;
  return true;
}

bool TryReadFloat(const Core::CPUThreadGuard& guard, u32 address, float* out)
{
  const auto value = PowerPC::MMU::HostTryRead<float>(guard, address);
  if (!value)
    return false;

  *out = value->value;
  return true;
}

void TryWriteFloat(const Core::CPUThreadGuard& guard, u32 address, float value)
{
  PowerPC::MMU::HostTryWrite<float>(guard, value, address);
}

void TryWriteU32(const Core::CPUThreadGuard& guard, u32 address, u32 value)
{
  PowerPC::MMU::HostTryWrite<u32>(guard, value, address);
}

void TryWriteU16(const Core::CPUThreadGuard& guard, u32 address, u16 value)
{
  PowerPC::MMU::HostTryWrite<u16>(guard, value, address);
}

void TryWriteU8(const Core::CPUThreadGuard& guard, u32 address, u8 value)
{
  PowerPC::MMU::HostTryWrite<u8>(guard, value, address);
}

bool TryWriteInstruction(const Core::CPUThreadGuard& guard, u32 address, u32 value)
{
  return PowerPC::MMU::HostTryWrite<u32>(guard, value, address).has_value();
}

u32 PpcBranch(u32 from, u32 to)
{
  const s32 offset = static_cast<s32>(to - from);
  return 0x48000000u | (static_cast<u32>(offset) & 0x03FFFFFCu);
}

u32 PpcBeq(u32 from, u32 to)
{
  const s32 offset = static_cast<s32>(to - from);
  return 0x41820000u | (static_cast<u32>(offset) & 0x0000FFFCu);
}

u32 PpcMr(u32 dest, u32 src)
{
  return 0x7C000378u | ((src & 31u) << 21) | ((dest & 31u) << 16) | ((src & 31u) << 11);
}

u32 PpcLwzR0(u32 base, u32 offset)
{
  return 0x80000000u | ((base & 31u) << 16) | (offset & 0xffffu);
}

void WriteBasisScratch(const Core::CPUThreadGuard& guard, const Matrix3x4& mat)
{
  int index = 0;
  for (int row = 0; row < 3; ++row)
  {
    for (int col = 0; col < 3; ++col)
    {
      TryWriteFloat(guard, CANNON_BASIS_SCRATCH + static_cast<u32>(index * 4),
                    mat.At(row, col));
      ++index;
    }
  }
}

void WriteBasis9(const Core::CPUThreadGuard& guard, u32 address, const Matrix3x4& mat)
{
  int index = 0;
  for (int row = 0; row < 3; ++row)
  {
    for (int col = 0; col < 3; ++col)
    {
      TryWriteFloat(guard, address + static_cast<u32>(index * 4), mat.At(row, col));
      ++index;
    }
  }
}

bool ScanVisorActive(const Core::CPUThreadGuard& guard, u32 player)
{
  if (player < 0x80000000u)
    return false;

  u32 visor = 0;
  return TryReadU32(guard, player + PLAYER_VISOR_STATE_OFFSET, &visor) && visor == 1;
}

bool PlayerIsFirstPersonUnmorphed(const Core::CPUThreadGuard& guard, u32 player)
{
  if (player < 0x80000000u)
    return false;

  u32 gameflow_menu = 0;
  if (TryReadU32(guard, GAMEFLOW_MENU_SCRATCH, &gameflow_menu) && gameflow_menu == 1)
    return false;

  u32 camera_state = 0xffffffffu;
  u32 morph_state = 0xffffffffu;
  if (!TryReadU32(guard, player + 0x2F4u, &camera_state) ||
      !TryReadU32(guard, player + 0x2F8u, &morph_state) ||
      camera_state != 0 || morph_state != 0)
  {
    return false;
  }

  const bool scan_active = ScanVisorActive(guard, player);
  if (scan_active)
  {
    u32 movement_state = 0xffffffffu;
    return TryReadU32(guard, player + PLAYER_MOVEMENT_STATE_OFFSET, &movement_state) &&
           movement_state <= 6;
  }

  u8 input_flags = 0;
  if (!TryReadU8(guard, player + PLAYER_DISABLE_INPUT_FLAGS_OFFSET, &input_flags) ||
      (input_flags & PLAYER_DISABLE_INPUT_MASK) != 0)
  {
    return false;
  }

  u32 movement_state = 0xffffffffu;
  return TryReadU32(guard, player + PLAYER_MOVEMENT_STATE_OFFSET, &movement_state) &&
         movement_state <= 6;
}

bool PlayerIsFirstPersonGunReady(const Core::CPUThreadGuard& guard, u32 player)
{
  if (player < 0x80000000u)
    return false;

  u32 camera_state = 0xffffffffu;
  u32 morph_state = 0xffffffffu;
  float gun_alpha = 0.0f;
  u32 holster_state = 0xffffffffu;
  if (!TryReadU32(guard, player + 0x2F4u, &camera_state) ||
      !TryReadU32(guard, player + 0x2F8u, &morph_state) ||
      !TryReadFloat(guard, player + 0x494u, &gun_alpha) ||
      !TryReadU32(guard, player + 0x498u, &holster_state))
  {
    return false;
  }

  return camera_state == 0 && morph_state == 0 && std::isfinite(gun_alpha) &&
         gun_alpha >= 0.95f && holster_state == 2;
}

void LogModeProbe(const Core::CPUThreadGuard& guard, u32 player, bool force)
{
  u32 camera_state = 0xffffffffu;
  u32 morph_state = 0xffffffffu;
  u32 movement_state = 0xffffffffu;
  u32 visor_state = 0xffffffffu;
  u32 holster_state = 0xffffffffu;
  u8 input_flags = 0xffu;
  float gun_alpha = -1.0f;
  u32 gameflow_menu = 0xffffffffu;

  if (player >= 0x80000000u)
  {
    TryReadU32(guard, player + 0x2F4u, &camera_state);
    TryReadU32(guard, player + 0x2F8u, &morph_state);
    TryReadU32(guard, player + PLAYER_MOVEMENT_STATE_OFFSET, &movement_state);
    TryReadU32(guard, player + PLAYER_VISOR_STATE_OFFSET, &visor_state);
    TryReadU32(guard, player + 0x498u, &holster_state);
    TryReadU8(guard, player + PLAYER_DISABLE_INPUT_FLAGS_OFFSET, &input_flags);
    TryReadFloat(guard, player + 0x494u, &gun_alpha);
  }

  u32 game_state = 0;
  u32 camera_manager = 0;
  TryReadU32(guard, GAMEFLOW_MENU_SCRATCH, &gameflow_menu);
  TryReadU32(guard, GP_GAME_STATE, &game_state);
  TryReadU32(guard, ADDRESS.state_manager + ADDRESS.camera_manager_offset, &camera_manager);

  std::array<u32, 8> player_words{};
  std::array<u32, 8> state_words{};
  std::array<u32, 8> game_words_0{};
  std::array<u32, 8> game_words_80{};
  std::array<u32, 8> game_words_100{};
  std::array<u32, 8> camera_words{};

  if (player >= 0x80000000u)
  {
    for (u32 i = 0; i < player_words.size(); ++i)
      TryReadU32(guard, player + 0x2E0u + i * 4u, &player_words[i]);
  }

  for (u32 i = 0; i < state_words.size(); ++i)
    TryReadU32(guard, ADDRESS.state_manager + 0x800u + i * 4u, &state_words[i]);

  if (game_state >= 0x80000000u)
  {
    for (u32 i = 0; i < game_words_0.size(); ++i)
      TryReadU32(guard, game_state + i * 4u, &game_words_0[i]);
    for (u32 i = 0; i < game_words_80.size(); ++i)
      TryReadU32(guard, game_state + 0x80u + i * 4u, &game_words_80[i]);
    for (u32 i = 0; i < game_words_100.size(); ++i)
      TryReadU32(guard, game_state + 0x100u + i * 4u, &game_words_100[i]);
  }

  if (camera_manager >= 0x80000000u)
  {
    for (u32 i = 0; i < camera_words.size(); ++i)
      TryReadU32(guard, camera_manager + i * 4u, &camera_words[i]);
  }

  bool changed = camera_state != s_last_mode_probe_camera_state ||
                 morph_state != s_last_mode_probe_morph_state ||
                 movement_state != s_last_mode_probe_movement_state ||
                 visor_state != s_last_mode_probe_visor_state ||
                 holster_state != s_last_mode_probe_holster_state ||
                 input_flags != s_last_mode_probe_input_flags ||
                 std::fabs(gun_alpha - s_last_mode_probe_gun_alpha) > 0.001f;
  if (!s_have_mode_probe_words)
    changed = true;

  if (!force && !changed && s_frame_counter < s_last_mode_probe_frame + 60)
    return;

  s_last_mode_probe_frame = s_frame_counter;
  s_last_mode_probe_camera_state = camera_state;
  s_last_mode_probe_morph_state = morph_state;
  s_last_mode_probe_movement_state = movement_state;
  s_last_mode_probe_visor_state = visor_state;
  s_last_mode_probe_holster_state = holster_state;
  s_last_mode_probe_input_flags = input_flags;
  s_last_mode_probe_gun_alpha = gun_alpha;
  s_have_mode_probe_words = true;

  NOTICE_LOG_FMT(CORE,
                 "PrimeGun mode_probe player={:08X} menu={} camera={} morph={} input_flags={:02X} "
                 "move={} visor={} gun_alpha={:.3f} holster={} game_state={:08X} camera_mgr={:08X}",
                 player, gameflow_menu, camera_state, morph_state, input_flags, movement_state,
                 visor_state, gun_alpha, holster_state, game_state, camera_manager);
  NOTICE_LOG_FMT(CORE,
                 "PrimeGun mode_probe player+2E0: {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                 player_words[0], player_words[1], player_words[2], player_words[3],
                 player_words[4], player_words[5], player_words[6], player_words[7]);
  NOTICE_LOG_FMT(CORE,
                 "PrimeGun mode_probe state+800: {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                 state_words[0], state_words[1], state_words[2], state_words[3], state_words[4],
                 state_words[5], state_words[6], state_words[7]);
  NOTICE_LOG_FMT(CORE,
                 "PrimeGun mode_probe game+000: {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                 game_words_0[0], game_words_0[1], game_words_0[2], game_words_0[3],
                 game_words_0[4], game_words_0[5], game_words_0[6], game_words_0[7]);
  NOTICE_LOG_FMT(CORE,
                 "PrimeGun mode_probe game+080: {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                 game_words_80[0], game_words_80[1], game_words_80[2], game_words_80[3],
                 game_words_80[4], game_words_80[5], game_words_80[6], game_words_80[7]);
  NOTICE_LOG_FMT(CORE,
                 "PrimeGun mode_probe game+100: {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                 game_words_100[0], game_words_100[1], game_words_100[2], game_words_100[3],
                 game_words_100[4], game_words_100[5], game_words_100[6], game_words_100[7]);
  NOTICE_LOG_FMT(CORE,
                 "PrimeGun mode_probe camera+000: {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                 camera_words[0], camera_words[1], camera_words[2], camera_words[3],
                 camera_words[4], camera_words[5], camera_words[6], camera_words[7]);
}

void ApplyHelmetOpacityZero(const Core::CPUThreadGuard& guard)
{
  u32 game_state = 0;
  if (!TryReadU32(guard, GP_GAME_STATE, &game_state) || game_state < 0x80000000u)
    return;

  const u32 helmet_alpha_addr = game_state + GAME_OPTIONS_HELMET_ALPHA_OFFSET;
  u32 current = 0;
  if (TryReadU32(guard, helmet_alpha_addr, &current) && current != 0)
    TryWriteU32(guard, helmet_alpha_addr, 0);
}

bool Normalize3(float& x, float& y, float& z)
{
  const float len = std::sqrt(x * x + y * y + z * z);
  if (!std::isfinite(len) || len < 0.00001f)
    return false;
  x /= len;
  y /= len;
  z /= len;
  return true;
}

struct GunTargetPick
{
  u16 uid = 0xffffu;
  u32 obj = 0;
  float ray_x = 0.0f;
  float ray_y = 0.0f;
  float ray_z = 0.0f;
  float dir_x = 0.0f;
  float dir_y = 0.0f;
  float dir_z = 0.0f;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float along = 0.0f;
  float perp = 0.0f;
  float score = 0.0f;
  bool suppress_orbit_hook = false;
};

bool ReadTransformTranslation(const Core::CPUThreadGuard& guard, u32 transform, float* x,
                              float* y, float* z)
{
  return transform >= 0x80000000u &&
         TryReadFloat(guard, transform + 0x0cu, x) &&
         TryReadFloat(guard, transform + 0x1cu, y) &&
         TryReadFloat(guard, transform + 0x2cu, z) &&
         std::isfinite(*x) && std::isfinite(*y) && std::isfinite(*z);
}

bool LooksLikeTransformMatrix(const Core::CPUThreadGuard& guard, u32 transform)
{
  float m[12] = {};
  if (transform < 0x80000000u)
    return false;

  for (int i = 0; i < 12; ++i)
  {
    if (!TryReadFloat(guard, transform + static_cast<u32>(i * 4), &m[i]) ||
        !std::isfinite(m[i]))
    {
      return false;
    }
  }

  const float row0_len = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
  const float row1_len = std::sqrt(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
  const float row2_len = std::sqrt(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
  if (row0_len < 0.5f || row0_len > 1.5f || row1_len < 0.5f || row1_len > 1.5f ||
      row2_len < 0.5f || row2_len > 1.5f)
  {
    return false;
  }

  const float dot01 = std::fabs(m[0] * m[4] + m[1] * m[5] + m[2] * m[6]);
  const float dot02 = std::fabs(m[0] * m[8] + m[1] * m[9] + m[2] * m[10]);
  const float dot12 = std::fabs(m[4] * m[8] + m[5] * m[9] + m[6] * m[10]);
  return dot01 <= 0.25f && dot02 <= 0.25f && dot12 <= 0.25f;
}

bool ActorHasMaterial(const Core::CPUThreadGuard& guard, u32 obj, int bit)
{
  if (obj < 0x80000000u || bit < 0 || bit >= 64)
    return false;

  u32 hi = 0;
  u32 lo = 0;
  if (!TryReadU32(guard, obj + 0x68u, &hi) || !TryReadU32(guard, obj + 0x6cu, &lo))
    return false;

  const u64 bits = (static_cast<u64>(hi) << 32) | lo;
  return (bits & (u64{1} << static_cast<u64>(bit))) != 0;
}

bool ActorIsTargetable(const Core::CPUThreadGuard& guard, u32 obj)
{
  u8 flags = 0;
  return obj >= 0x80000000u && TryReadU8(guard, obj + 0xe7u, &flags) &&
         (flags & 0x01u) != 0;
}

bool ActorIsGrapplePoint(const Core::CPUThreadGuard& guard, u32 obj)
{
  u32 vtable = 0;
  return obj >= 0x80000000u && TryReadU32(guard, obj, &vtable) &&
         vtable >= 0x803E0D00u && vtable < 0x803E0D70u;
}

bool GunAimDirectionFromMatrix(const Matrix3x4& mat, float* x, float* y, float* z)
{
  *x = mat.m[1];
  *y = mat.m[5];
  *z = mat.m[9];
  return Normalize3(*x, *y, *z);
}

bool TargetUidStillExists(const Core::CPUThreadGuard& guard, u32 state_manager, u16 uid,
                          u32 expected_obj)
{
  if (uid == 0xffffu)
    return false;

  u32 object_list = 0;
  if (!TryReadU32(guard, state_manager + 0x810u, &object_list) || object_list < 0x80000000u)
    return false;

  u32 obj = 0;
  u32 live_uid_word = 0;
  return TryReadU32(guard, object_list + ((uid & 0x03ffu) << 3) + 4u, &obj) &&
         obj >= 0x80000000u && (expected_obj < 0x80000000u || obj == expected_obj) &&
         TryReadU32(guard, obj + 0x8u, &live_uid_word) &&
         static_cast<u16>(live_uid_word >> 16) == uid;
}

void WriteGunTargetScratch(const Core::CPUThreadGuard& guard, u32 player, u16 uid)
{
  TryWriteU32(guard, GUN_TARGET_SCRATCH, player);
  TryWriteU32(guard, GUN_TARGET_SCRATCH + 4u, 0);
  TryWriteU16(guard, GUN_TARGET_SCRATCH + 4u, uid);
}

bool PickGunRayTarget(const Core::CPUThreadGuard& guard, u32 state_manager, u32 player, u32 gun,
                      u32 world_xf, const Matrix3x4& mat, const RuntimeSettings& settings,
                      bool strict_lock_aim, GunTargetPick* pick)
{
  if (!settings.gun_targeting_enabled || state_manager < 0x80000000u ||
      player < 0x80000000u || gun < 0x80000000u)
  {
    return false;
  }

  float ray_x = 0.0f;
  float ray_y = 0.0f;
  float ray_z = 0.0f;
  if (!ReadTransformTranslation(guard, world_xf, &ray_x, &ray_y, &ray_z))
    return false;

  float dir_x = 0.0f;
  float dir_y = 0.0f;
  float dir_z = 0.0f;
  if (!GunAimDirectionFromMatrix(mat, &dir_x, &dir_y, &dir_z))
    return false;
  pick->ray_x = ray_x;
  pick->ray_y = ray_y;
  pick->ray_z = ray_z;
  pick->dir_x = dir_x;
  pick->dir_y = dir_y;
  pick->dir_z = dir_z;

  u32 object_list = 0;
  u32 player_uid_word = 0;
  if (!TryReadU32(guard, state_manager + 0x810u, &object_list) ||
      object_list < 0x80000000u || !TryReadU32(guard, player + 0x8u, &player_uid_word))
  {
    return false;
  }

  const u16 player_uid = static_cast<u16>(player_uid_word >> 16);
  const float max_along = settings.gun_targeting_distance;
  const float max_perp = settings.gun_targeting_radius;
  const float grapple_max_perp = max_perp * 1.30f;
  const bool scan_mode = ScanVisorActive(guard, player);
  bool found = false;

  for (u32 i = 0; i < 1024u; ++i)
  {
    u32 obj = 0;
    if (!TryReadU32(guard, object_list + (i << 3) + 4u, &obj) ||
        obj < 0x80000000u || obj == player || obj == gun)
    {
      continue;
    }

    float ox = 0.0f;
    float oy = 0.0f;
    float oz = 0.0f;
    if (!ReadTransformTranslation(guard, obj + ADDRESS.transform_offset, &ox, &oy, &oz))
      continue;

    const float vx = ox - ray_x;
    const float vy = oy - ray_y;
    const float vz = oz - ray_z;
    const float along = vx * dir_x + vy * dir_y + vz * dir_z;
    if (along < 0.5f || along > max_along)
      continue;

    const float nearest_x = ray_x + dir_x * along;
    const float nearest_y = ray_y + dir_y * along;
    const float nearest_z = ray_z + dir_z * along;
    const float dx = ox - nearest_x;
    const float dy = oy - nearest_y;
    const float dz = oz - nearest_z;
    const float perp_sq = dx * dx + dy * dy + dz * dz;
    if (!std::isfinite(perp_sq) || perp_sq > grapple_max_perp * grapple_max_perp ||
        !LooksLikeTransformMatrix(guard, obj + ADDRESS.transform_offset))
    {
      continue;
    }

    u32 uid_word = 0;
    u32 owner_uid_word = 0;
    if (!TryReadU32(guard, obj + 0x8u, &uid_word) || !TryReadU32(guard, obj + 0xecu, &owner_uid_word))
      continue;

    const u16 uid = static_cast<u16>(uid_word >> 16);
    const u16 owner_uid = static_cast<u16>(owner_uid_word >> 16);
    if (uid == 0xffffu || uid == player_uid || owner_uid == player_uid)
      continue;

    const bool has_target = ActorHasMaterial(guard, obj, 40);
    const bool has_character = ActorHasMaterial(guard, obj, 33) || ActorHasMaterial(guard, obj, 55);
    const bool has_orbit = ActorHasMaterial(guard, obj, 41);
    const bool has_scan = ActorHasMaterial(guard, obj, 39);
    const bool targetable = ActorIsTargetable(guard, obj);
    const bool grapple_point = ActorIsGrapplePoint(guard, obj);
    const bool combat_candidate = has_target && targetable;
    const bool orbit_candidate = has_orbit && targetable;
    const bool scan_candidate = has_scan;
    const bool grapple_candidate = grapple_point && has_orbit;
    if (!combat_candidate && !orbit_candidate && !scan_candidate && !grapple_candidate)
      continue;

    const float perp = std::sqrt(perp_sq);
    const float candidate_max_perp = grapple_candidate ? grapple_max_perp : max_perp;
    if (perp > candidate_max_perp)
      continue;

    float aim_cone_perp = candidate_max_perp;
    if (strict_lock_aim)
    {
      const float strict_cone = 0.45f + along * (scan_mode ? 0.085f : 0.075f);
      aim_cone_perp = std::min(candidate_max_perp, grapple_candidate ? strict_cone * 1.30f : strict_cone);
      if (perp > aim_cone_perp)
        continue;
    }

    const float cone_fraction = perp / std::max(aim_cone_perp, 0.001f);
    float score = cone_fraction * 1.50f + perp * 0.35f + along * 0.003f;
    if (grapple_candidate)
      score -= 0.15f;
    if (scan_mode)
    {
      if (has_scan)
        score -= 0.10f;
      else if (!has_character)
        score += 0.10f;
      if (!has_scan && !has_target)
        score += 0.15f;
    }
    else
    {
      if (has_target)
        score -= 0.10f;
      else if (has_scan)
        score += 0.10f;
      else if (!has_character)
        score += 0.15f;
    }

    if (!found || score < pick->score)
    {
      found = true;
      pick->uid = uid;
      pick->obj = obj;
      pick->x = ox;
      pick->y = oy;
      pick->z = oz;
      pick->along = along;
      pick->perp = perp;
      pick->score = score;
      pick->suppress_orbit_hook = false;
    }
  }

  return found;
}

bool OrbitLockButtonHeld(const Core::CPUThreadGuard& guard, u32 state_manager)
{
  u8 held0 = 0;
  return TryReadU8(guard, state_manager + FINAL_INPUT_DPAD_HELD_0, &held0) &&
         (held0 & 0x04u) != 0;
}

void UpdateGunTargeting(const Core::CPUThreadGuard& guard, u32 state_manager, u32 player, u32 gun,
                        u32 world_xf, const Matrix3x4& mat, const RuntimeSettings& settings)
{
  static GunTargetPick s_last_pick = {};
  static bool s_last_pick_valid = false;
  static u64 s_last_pick_frame = 0;
  static u64 s_last_success_frame = 0;
  static u32 s_last_pick_player = 0;
  static u32 s_last_pick_gun = 0;

  if (!settings.gun_targeting_enabled || !settings.patch_gun_ray_target || player < 0x80000000u ||
      ScanVisorActive(guard, player))
  {
    s_last_pick_valid = false;
    WriteGunTargetScratch(guard, 0, 0xffffu);
    return;
  }

  const bool same_context = s_last_pick_valid && s_last_pick_player == player &&
                            s_last_pick_gun == gun;
  const bool lock_held = OrbitLockButtonHeld(guard, state_manager);
  const bool throttle_scan = same_context && s_frame_counter - s_last_pick_frame < 4;

  GunTargetPick pick = {};
  bool found = false;
  if (throttle_scan)
  {
    pick = s_last_pick;
    found = true;
  }
  else
  {
    const GunTargetPick previous = s_last_pick;
    const bool previous_valid = s_last_pick_valid;
    found =
        PickGunRayTarget(guard, state_manager, player, gun, world_xf, mat, settings, lock_held, &pick);
    s_last_pick_frame = s_frame_counter;
    s_last_pick_player = player;
    s_last_pick_gun = gun;

    if (found)
    {
      s_last_pick = pick;
      s_last_pick_valid = true;
      s_last_success_frame = s_frame_counter;
    }
    else if (same_context && previous_valid &&
             s_frame_counter - s_last_success_frame <= (lock_held ? 21u : 11u) &&
             TargetUidStillExists(guard, state_manager, previous.uid, previous.obj))
    {
      pick = previous;
      found = true;
      s_last_pick = pick;
      s_last_pick_valid = true;
    }
    else
    {
      s_last_pick_valid = false;
    }
  }

  if (!found || pick.suppress_orbit_hook)
  {
    WriteGunTargetScratch(guard, player, 0xffffu);
    return;
  }

  WriteGunTargetScratch(guard, player, pick.uid);
}

bool GunPitchFromMatrix(const Matrix3x4& mat, float* pitch_out)
{
  float dir_x = -mat.m[4];
  float dir_y = mat.m[5];
  float dir_flat_z = 0.0f;
  if (!Normalize3(dir_x, dir_y, dir_flat_z))
    return false;

  const float dir_z = mat.m[6] * -dir_y + -mat.m[2] * dir_x;
  if (!std::isfinite(dir_z))
    return false;

  *pitch_out = std::asin(std::clamp(dir_z, -0.98f, 0.98f));
  return std::isfinite(*pitch_out);
}

float WrapRadians(float angle)
{
  constexpr float pi = static_cast<float>(MathUtil::PI);
  constexpr float two_pi = pi * 2.0f;
  while (angle > pi)
    angle -= two_pi;
  while (angle < -pi)
    angle += two_pi;
  return angle;
}

bool ReadYawFromTransform2D(const Core::CPUThreadGuard& guard, u32 transform, float* yaw_deg)
{
  float m00 = 0.0f;
  float m01 = 0.0f;
  float m10 = 0.0f;
  float m11 = 0.0f;
  if (!TryReadFloat(guard, transform + 0x00, &m00) ||
      !TryReadFloat(guard, transform + 0x04, &m01) ||
      !TryReadFloat(guard, transform + 0x10, &m10) ||
      !TryReadFloat(guard, transform + 0x14, &m11))
  {
    return false;
  }

  if (!std::isfinite(m00) || !std::isfinite(m01) || !std::isfinite(m10) || !std::isfinite(m11))
    return false;

  const float row0_len = std::sqrt(m00 * m00 + m01 * m01);
  const float row1_len = std::sqrt(m10 * m10 + m11 * m11);
  const float det = m00 * m11 - m01 * m10;
  if (row0_len < 0.7f || row0_len > 1.3f || row1_len < 0.7f || row1_len > 1.3f ||
      std::fabs(det) < 0.5f)
  {
    return false;
  }

  *yaw_deg = std::atan2(m01, m00) * (180.0f / static_cast<float>(MathUtil::PI));
  return true;
}

bool ResolveActiveCameraTransform(const Core::CPUThreadGuard& guard, u32* transform)
{
  constexpr u32 object_list_offset = 0x810u;

  u32 player = 0;
  u32 gun = 0;
  u32 camera_manager = 0;
  u32 object_list = 0;
  if (!TryReadU32(guard, ADDRESS.state_manager + ADDRESS.player_offset, &player) ||
      player < 0x80000000u || !TryReadU32(guard, player + ADDRESS.cannon_offset, &gun) ||
      gun < 0x80000000u ||
      !TryReadU32(guard, ADDRESS.state_manager + ADDRESS.camera_manager_offset, &camera_manager) ||
      camera_manager < 0x80000000u ||
      !TryReadU32(guard, ADDRESS.state_manager + object_list_offset, &object_list) ||
      object_list < 0x80000000u)
  {
    return false;
  }

  u32 camera_uid_word = 0;
  if (!TryReadU32(guard, camera_manager, &camera_uid_word))
    return false;

  const u16 camera_uid = static_cast<u16>(camera_uid_word >> 16);
  if (camera_uid == 0xffffu)
    return false;

  u32 camera = 0;
  if (!TryReadU32(guard, object_list + ((camera_uid & 0x03ffu) << 3) + 4, &camera) ||
      camera < 0x80000000u)
  {
    return false;
  }

  *transform = camera + ADDRESS.transform_offset;
  return true;
}

float GetPlayerYawDeltaDegrees(const Core::CPUThreadGuard& guard)
{
  u32 camera_transform = 0;
  float yaw_deg = 0.0f;
  if (!ResolveActiveCameraTransform(guard, &camera_transform) ||
      !ReadYawFromTransform2D(guard, camera_transform, &yaw_deg))
  {
    return 0.0f;
  }

  return WrapRadians((yaw_deg + 180.0f) * (static_cast<float>(MathUtil::PI) / 180.0f)) *
         (-180.0f / static_cast<float>(MathUtil::PI));
}

float YawFromPrimeXY(float x, float y)
{
  return std::atan2(x, y);
}

void PrimeXYFromYaw(float yaw, float* x, float* y)
{
  *x = std::sin(yaw);
  *y = std::cos(yaw);
}

void ApplyTrackingWorldYaw(Pose& pose, float yaw_deg)
{
  const float yaw_rad = yaw_deg * (static_cast<float>(MathUtil::PI) / 180.0f);
  const float half = yaw_rad * 0.5f;
  const float sy = std::sin(half);
  const float cy = std::cos(half);

  const float qx = pose.qx;
  const float qy = pose.qy;
  const float qz = pose.qz;
  const float qw = pose.qw;
  pose.qx = cy * qx + sy * qz;
  pose.qy = cy * qy + sy * qw;
  pose.qz = cy * qz - sy * qx;
  pose.qw = cy * qw - sy * qy;
}

bool PosePrimeYaw(Pose pose, float yaw_delta_deg, float* yaw_out)
{
  if (!pose.valid)
    return false;

  ApplyTrackingWorldYaw(pose, yaw_delta_deg);

  const float qx = pose.qx;
  const float qy = pose.qy;
  const float qz = pose.qz;
  const float qw = pose.qw;
  const float sin_yaw = 2.0f * (qw * qy + qx * qz);
  const float cos_yaw = 1.0f - 2.0f * (qy * qy + qz * qz);
  if (!std::isfinite(sin_yaw) || !std::isfinite(cos_yaw))
    return false;

  float prime_forward_x = -sin_yaw;
  float prime_forward_y = cos_yaw;
  const float len =
      std::sqrt(prime_forward_x * prime_forward_x + prime_forward_y * prime_forward_y);
  if (!std::isfinite(len) || len < 0.0001f)
    return false;

  prime_forward_x /= len;
  prime_forward_y /= len;

  const float r12 = 2.0f * (qy * qz - qw * qx);
  const float pitch_up = std::asin(std::clamp(-r12, -1.0f, 1.0f));
  constexpr float allow_backward_flip_pitch =
      90.0f * (static_cast<float>(MathUtil::PI) / 180.0f);
  if (std::fabs(pitch_up) > allow_backward_flip_pitch)
  {
    prime_forward_x = -prime_forward_x;
    prime_forward_y = -prime_forward_y;
  }

  *yaw_out = YawFromPrimeXY(prime_forward_x, prime_forward_y);
  return true;
}

void RotatePrimeXY(float& x, float& y, float yaw_deg)
{
  const float yaw_rad = yaw_deg * (static_cast<float>(MathUtil::PI) / 180.0f);
  const float c = std::cos(yaw_rad);
  const float s = std::sin(yaw_rad);
  const float old_x = x;
  const float old_y = y;
  x = c * old_x - s * old_y;
  y = s * old_x + c * old_y;
}

DpadDir StickToDpad(float x, float y, float deadzone, DpadDir last_dir)
{
  const float mag = std::sqrt(x * x + y * y);
  const float exit_deadzone = std::max(0.05f, deadzone * 0.55f);
  if (mag < exit_deadzone)
    return DpadNone;

  const float ax = std::fabs(x);
  const float ay = std::fabs(y);
  if (ax >= deadzone || ay >= deadzone)
    return ay >= ax ? (y > 0.0f ? DpadUp : DpadDown) :
                      (x > 0.0f ? DpadRight : DpadLeft);

  if (last_dir == DpadNone)
    return DpadNone;

  constexpr float axis_switch_bias = 1.35f;
  switch (last_dir)
  {
  case DpadUp:
  case DpadDown:
    if (ay * axis_switch_bias >= ax)
      return y >= 0.0f ? DpadUp : DpadDown;
    break;
  case DpadLeft:
  case DpadRight:
    if (ax * axis_switch_bias >= ay)
      return x >= 0.0f ? DpadRight : DpadLeft;
    break;
  default:
    break;
  }

  return ax > ay ? (x >= 0.0f ? DpadRight : DpadLeft) :
                   (y >= 0.0f ? DpadUp : DpadDown);
}

u32 DpadCommand(DpadDir dir)
{
  switch (dir)
  {
  case DpadUp:
    return 53u;
  case DpadRight:
    return 50u;
  case DpadDown:
    return 51u;
  case DpadLeft:
    return 52u;
  default:
    return 0xffffffffu;
  }
}

bool LeftControllerNearHead(const Pose& left, const Pose& hmd, const RuntimeSettings& settings)
{
  if (!left.valid || !hmd.valid)
    return false;

  const float dx = left.px - hmd.px;
  const float dy = left.py - hmd.py;
  const float dz = left.pz - hmd.pz;
  const float radius = settings.xr_dpad_head_radius + 0.06f;
  return dx * dx + dy * dy + dz * dz <= radius * radius &&
         dy >= -(settings.xr_dpad_head_y_below + 0.04f) && dy <= 0.28f;
}

void SetPlayerInputDisabledForDpad(const Core::CPUThreadGuard& guard, u32 state_manager,
                                   bool disabled)
{
  if (disabled && s_dpad_forced_input_disabled && s_dpad_input_flags_addr >= 0x80000000u)
  {
    if (s_frame_counter - s_dpad_last_disable_refresh_frame < 15)
      return;

    u8 flags = 0;
    if (TryReadU8(guard, s_dpad_input_flags_addr, &flags))
    {
      TryWriteU8(guard, s_dpad_input_flags_addr, flags | PLAYER_DISABLE_INPUT_MASK);
      s_dpad_last_disable_refresh_frame = s_frame_counter;
    }
    return;
  }

  u32 player = 0;
  if (!TryReadU32(guard, state_manager + ADDRESS.player_offset, &player) ||
      player < 0x80000000u)
  {
    s_dpad_forced_input_disabled = false;
    s_dpad_input_was_disabled = false;
    s_dpad_input_flags_addr = 0;
    s_dpad_last_disable_refresh_frame = 0;
    return;
  }

  const u32 flags_addr = player + PLAYER_DISABLE_INPUT_FLAGS_OFFSET;
  u8 flags = 0;
  if (!TryReadU8(guard, flags_addr, &flags))
    return;

  if (disabled)
  {
    if (!s_dpad_forced_input_disabled)
    {
      s_dpad_input_was_disabled = (flags & PLAYER_DISABLE_INPUT_MASK) != 0;
      s_dpad_forced_input_disabled = true;
    }

    TryWriteU8(guard, flags_addr, flags | PLAYER_DISABLE_INPUT_MASK);
    s_dpad_input_flags_addr = flags_addr;
    s_dpad_last_disable_refresh_frame = s_frame_counter;
    return;
  }

  if (s_dpad_forced_input_disabled && !s_dpad_input_was_disabled)
    TryWriteU8(guard, flags_addr, flags & static_cast<u8>(~PLAYER_DISABLE_INPUT_MASK));

  s_dpad_forced_input_disabled = false;
  s_dpad_input_was_disabled = false;
  s_dpad_input_flags_addr = 0;
  s_dpad_last_disable_refresh_frame = 0;
}

void SuppressCStickForDpad(const Core::CPUThreadGuard& guard, u32 state_manager)
{
  u32 player = 0;
  if (!TryReadU32(guard, state_manager + ADDRESS.player_offset, &player) ||
      player < 0x80000000u || ScanVisorActive(guard, player))
  {
    return;
  }

  TryWriteFloat(guard, state_manager + FINAL_INPUT_RIGHT_STICK_X, 0.0f);
  TryWriteFloat(guard, state_manager + FINAL_INPUT_RIGHT_STICK_Y, 0.0f);
  TryWriteU8(guard, state_manager + FINAL_INPUT_RIGHT_STICK_X_PRESS, 0);
  TryWriteU8(guard, state_manager + FINAL_INPUT_RIGHT_STICK_Y_PRESS, 0);

  TryWriteU8(guard, player + PLAYER_FREE_LOOK_STATE_OFFSET, 0);
  TryWriteU8(guard, player + PLAYER_FREE_LOOK_STATE_OFFSET + 1, 0);
  TryWriteU8(guard, player + PLAYER_FREE_LOOK_STATE_OFFSET + 2, 0);
  TryWriteFloat(guard, player + PLAYER_FREE_LOOK_CENTER_TIME_OFFSET, 0.0f);
  TryWriteFloat(guard, player + PLAYER_FREE_LOOK_YAW_ANGLE_OFFSET, 0.0f);
  TryWriteFloat(guard, player + PLAYER_FREE_LOOK_YAW_VEL_OFFSET, 0.0f);
  TryWriteFloat(guard, player + PLAYER_FREE_LOOK_PITCH_ANGLE_OFFSET, 0.0f);
}

#ifdef ENABLE_VR
Pose PoseFromOpenXR(const Common::VR::OpenXRPoseState& xr_pose)
{
  return {
      xr_pose.position[0],
      xr_pose.position[1],
      xr_pose.position[2],
      xr_pose.orientation[0],
      xr_pose.orientation[1],
      xr_pose.orientation[2],
      xr_pose.orientation[3],
      xr_pose.valid,
  };
}

void UpdateReticleBillboard(const Core::CPUThreadGuard& guard,
                            const Common::VR::OpenXRInputSnapshot& snapshot, u32 player,
                            float yaw_delta_deg)
{
  if (ScanVisorActive(guard, player) || !snapshot.head_pose.valid)
  {
    TryWriteU32(guard, RETICLE_BILLBOARD_SCRATCH, 0);
    return;
  }

  Pose adjusted_hmd = PoseFromOpenXR(snapshot.head_pose);
  ApplyTrackingWorldYaw(adjusted_hmd, yaw_delta_deg);

  const Matrix3x4 hmd =
      PoseToPrimeMatrix(adjusted_hmd, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
  TryWriteU32(guard, RETICLE_BILLBOARD_SCRATCH, 1);
  WriteBasis9(guard, RETICLE_BILLBOARD_SCRATCH + 4, hmd);
}

void UpdateScanPitch(const Core::CPUThreadGuard& guard, u32 player, const Matrix3x4& controller_mat)
{
  const bool scan_active = ScanVisorActive(guard, player);
  if (player != s_scan_last_player)
  {
    s_scan_last_player = player;
    s_scan_was_active = scan_active;
    s_scan_normal_frames = 0;
    s_smooth_scan_pitch = 0.0f;
  }

  if (scan_active)
  {
    s_scan_normal_frames = 0;
    float pitch = 0.0f;
    if (!GunPitchFromMatrix(controller_mat, &pitch))
      return;

    constexpr float max_scan_pitch = 1.2217305f;  // 70 degrees
    pitch = std::clamp(pitch, -max_scan_pitch, max_scan_pitch);
    constexpr float pitch_smooth = 0.45f;
    s_smooth_scan_pitch = s_smooth_scan_pitch * pitch_smooth + pitch * (1.0f - pitch_smooth);
    TryWriteFloat(guard, player + PLAYER_FREE_LOOK_PITCH_ANGLE_OFFSET, s_smooth_scan_pitch);
  }
  else if (s_scan_was_active)
  {
    s_scan_normal_frames = 1;
  }
  else if (s_scan_normal_frames > 0 && s_scan_normal_frames < 8)
  {
    ++s_scan_normal_frames;
  }
  else if (s_scan_normal_frames == 8)
  {
    s_smooth_scan_pitch = 0.0f;
    TryWriteFloat(guard, player + PLAYER_FREE_LOOK_PITCH_ANGLE_OFFSET, 0.0f);
    ++s_scan_normal_frames;
  }

  s_scan_was_active = scan_active;
}

void UpdateXrDpad(const Core::CPUThreadGuard& guard,
                  const Common::VR::OpenXRInputSnapshot& snapshot,
                  const RuntimeSettings& settings)
{
  static DpadDir s_last_dir = DpadNone;
  static DpadDir s_latched_dir = DpadNone;
  static u64 s_latch_until_frame = 0;
  static u64 s_dir_start_frame = 0;
  static u64 s_last_press_frame = 0;
  static u64 s_last_near_head_frame = 0;
  static bool s_c_stick_suppressed = false;

  const auto disarm = [&] {
    SetPlayerInputDisabledForDpad(guard, ADDRESS.state_manager, false);
    TryWriteU32(guard, DPAD_PRESS_SCRATCH, 0xffffffffu);
    s_last_dir = DpadNone;
    s_latched_dir = DpadNone;
    s_latch_until_frame = 0;
    s_dir_start_frame = 0;
    s_last_press_frame = 0;
    s_c_stick_suppressed = false;
  };

  if (!settings.xr_dpad_enabled || !snapshot.runtime_active)
  {
    disarm();
    return;
  }

  const Common::VR::OpenXRControllerState& dpad_hand =
      snapshot.controllers[settings.use_right_hand ? 0 : 1];
  if (!dpad_hand.connected || !dpad_hand.aim_pose.valid || !snapshot.head_pose.valid)
  {
    disarm();
    return;
  }

  const Pose hand_pose = PoseFromOpenXR(dpad_hand.aim_pose);
  const Pose hmd_pose = PoseFromOpenXR(snapshot.head_pose);
  bool in_head_zone = LeftControllerNearHead(hand_pose, hmd_pose, settings);
  if (in_head_zone)
  {
    s_last_near_head_frame = s_frame_counter;
  }
  else if (s_last_near_head_frame != 0 && s_frame_counter - s_last_near_head_frame < 16)
  {
    in_head_zone = true;
  }
  else
  {
    disarm();
    return;
  }

  const float deadzone = std::min(settings.xr_dpad_deadzone, 0.25f);
  DpadDir dir = StickToDpad(dpad_hand.thumbstick_x, dpad_hand.thumbstick_y, deadzone, s_last_dir);
  if (dir != DpadNone)
  {
    s_latched_dir = dir;
    s_latch_until_frame = s_frame_counter + 8;
  }
  else if (s_latched_dir != DpadNone && s_frame_counter < s_latch_until_frame)
  {
    dir = s_latched_dir;
  }

  if (dir == DpadNone)
  {
    disarm();
    return;
  }

  SetPlayerInputDisabledForDpad(guard, ADDRESS.state_manager, in_head_zone);
  if (!s_c_stick_suppressed)
  {
    SuppressCStickForDpad(guard, ADDRESS.state_manager);
    s_c_stick_suppressed = true;
  }

  if (dir != s_last_dir)
  {
    s_dir_start_frame = s_frame_counter;
    s_last_press_frame = 0;
  }

  const bool initial_press = s_dir_start_frame != 0 && s_frame_counter - s_dir_start_frame < 11;
  const bool repeat_press = s_last_press_frame == 0 || s_frame_counter - s_last_press_frame >= 10;
  const bool send_press = initial_press || repeat_press;
  if (send_press)
    s_last_press_frame = s_frame_counter;

  TryWriteU32(guard, DPAD_PRESS_SCRATCH, send_press ? DpadCommand(dir) : 0xffffffffu);
  TryWriteFloat(guard, ADDRESS.state_manager + FINAL_INPUT_RIGHT_STICK_X, 0.0f);
  TryWriteFloat(guard, ADDRESS.state_manager + FINAL_INPUT_RIGHT_STICK_Y, 0.0f);

  u8 held0 = 0;
  u8 held1 = 0;
  u8 pressed0 = 0;
  if (!TryReadU8(guard, ADDRESS.state_manager + FINAL_INPUT_DPAD_HELD_0, &held0) ||
      !TryReadU8(guard, ADDRESS.state_manager + FINAL_INPUT_DPAD_HELD_1, &held1) ||
      !TryReadU8(guard, ADDRESS.state_manager + FINAL_INPUT_DPAD_PRESSED_0, &pressed0))
  {
    disarm();
    return;
  }

  held0 &= static_cast<u8>(~0x01u);
  held1 &= static_cast<u8>(~0xE0u);
  pressed0 &= static_cast<u8>(~0x1Cu);
  switch (dir)
  {
  case DpadUp:
    held0 |= 0x01u;
    if (send_press)
      pressed0 |= 0x10u;
    break;
  case DpadRight:
    held1 |= 0x80u;
    if (send_press)
      pressed0 |= 0x08u;
    break;
  case DpadDown:
    held1 |= 0x40u;
    if (send_press)
      pressed0 |= 0x04u;
    break;
  case DpadLeft:
    held1 |= 0x20u;
    if (send_press)
      pressed0 |= 0x02u;
    break;
  default:
    break;
  }

  TryWriteU8(guard, ADDRESS.state_manager + FINAL_INPUT_DPAD_HELD_0, held0);
  TryWriteU8(guard, ADDRESS.state_manager + FINAL_INPUT_DPAD_HELD_1, held1);
  TryWriteU8(guard, ADDRESS.state_manager + FINAL_INPUT_DPAD_PRESSED_0, pressed0);
  s_last_dir = dir;
}

void UpdateDirectionalMovement(const Core::CPUThreadGuard& guard,
                               const Common::VR::OpenXRInputSnapshot& snapshot,
                               const RuntimeSettings& settings, float yaw_delta_deg)
{
  if (!settings.directional_movement_enabled || !snapshot.runtime_active || !snapshot.head_pose.valid ||
      s_frame_counter < s_vr_menu_input_suppress_until_frame)
  {
    s_directional_move_speed = 0.0f;
    return;
  }

  const int move_hand = settings.directional_movement_use_right_stick ? 1 : 0;
  const Common::VR::OpenXRControllerState& controller = snapshot.controllers[move_hand];
  if (!controller.connected ||
      (!settings.directional_movement_use_hmd_direction && !controller.aim_pose.valid))
  {
    s_directional_move_speed = 0.0f;
    return;
  }

  const Pose hmd_pose = PoseFromOpenXR(snapshot.head_pose);
  const Pose controller_move_pose = PoseFromOpenXR(controller.aim_pose);
  const Pose move_pose =
      settings.directional_movement_use_hmd_direction ? hmd_pose : controller_move_pose;
  const Common::VR::OpenXRControllerState& dpad_hand =
      snapshot.controllers[settings.use_right_hand ? 0 : 1];
  if (dpad_hand.connected && dpad_hand.aim_pose.valid &&
      LeftControllerNearHead(PoseFromOpenXR(dpad_hand.aim_pose), hmd_pose, settings))
  {
    s_directional_move_speed = 0.0f;
    return;
  }

  const float stick_x = controller.thumbstick_x;
  const float stick_y = controller.thumbstick_y;
  const float stick_mag = std::sqrt(stick_x * stick_x + stick_y * stick_y);
  if (stick_mag < settings.directional_movement_deadzone)
  {
    s_directional_move_speed = 0.0f;
    return;
  }

  const float move_mag = std::min(stick_mag, 1.0f);
  if (move_mag < settings.directional_movement_deadzone)
  {
    s_directional_move_speed = 0.0f;
    return;
  }

  u32 player = 0;
  if (!TryReadU32(guard, ADDRESS.state_manager + ADDRESS.player_offset, &player) ||
      !PlayerIsFirstPersonUnmorphed(guard, player))
  {
    s_directional_move_speed = 0.0f;
    return;
  }

  if (OrbitLockButtonHeld(guard, ADDRESS.state_manager))
  {
    s_directional_move_speed = 0.0f;
    return;
  }

  float vx = 0.0f;
  float vy = 0.0f;
  float vz = 0.0f;
  if (!TryReadFloat(guard, player + PLAYER_VELOCITY_OFFSET + 0x00u, &vx) ||
      !TryReadFloat(guard, player + PLAYER_VELOCITY_OFFSET + 0x04u, &vy) ||
      !TryReadFloat(guard, player + PLAYER_VELOCITY_OFFSET + 0x08u, &vz) ||
      !std::isfinite(vx) || !std::isfinite(vy) || !std::isfinite(vz))
  {
    return;
  }

  const float flat_speed = std::sqrt(vx * vx + vy * vy);
  float base_yaw = yaw_delta_deg * (static_cast<float>(MathUtil::PI) / 180.0f);
  if (!PosePrimeYaw(move_pose, yaw_delta_deg, &base_yaw))
  {
    s_directional_move_speed = 0.0f;
    return;
  }
  float forward_x = 0.0f;
  float forward_y = 0.0f;
  float right_x = 0.0f;
  float right_y = 0.0f;
  PrimeXYFromYaw(WrapRadians(base_yaw + static_cast<float>(MathUtil::PI)), &forward_x,
                 &forward_y);
  PrimeXYFromYaw(WrapRadians(base_yaw + static_cast<float>(MathUtil::PI) * 0.5f), &right_x,
                 &right_y);

  float dir_x = forward_x * stick_y - right_x * stick_x;
  float dir_y = forward_y * stick_y - right_y * stick_x;
  const float dir_len = std::sqrt(dir_x * dir_x + dir_y * dir_y);
  if (!std::isfinite(dir_len) || dir_len < 0.0001f)
  {
    s_directional_move_speed = 0.0f;
    return;
  }
  dir_x /= dir_len;
  dir_y /= dir_len;

  u32 movement_state = 0xffffffffu;
  const bool on_ground =
      TryReadU32(guard, player + PLAYER_MOVEMENT_STATE_OFFSET, &movement_state) &&
      movement_state == 0;
  if (s_directional_move_speed <= 0.0f ||
      s_directional_move_speed > settings.directional_movement_speed * move_mag)
  {
    s_directional_move_speed =
        std::min(std::max(flat_speed, 0.0f), settings.directional_movement_speed * move_mag);
  }

  const float accel = on_ground ? settings.directional_movement_accel :
                                  settings.directional_movement_air_accel;
  constexpr float frame_dt = 1.0f / 60.0f;
  const float target_speed = settings.directional_movement_speed * move_mag;
  const float max_delta = accel * frame_dt;
  if (s_directional_move_speed < target_speed)
    s_directional_move_speed = std::min(target_speed, s_directional_move_speed + max_delta);
  else
    s_directional_move_speed = std::max(target_speed, s_directional_move_speed - max_delta);

  TryWriteFloat(guard, player + PLAYER_VELOCITY_OFFSET + 0x00u, dir_x * s_directional_move_speed);
  TryWriteFloat(guard, player + PLAYER_VELOCITY_OFFSET + 0x04u, dir_y * s_directional_move_speed);
  TryWriteFloat(guard, player + PLAYER_VELOCITY_OFFSET + 0x08u, vz);
}
#endif

#ifdef ENABLE_VR
std::string PrimeGunCannonActiveFolder()
{
  return File::GetUserPath(D_HIRESTEXTURES_IDX) + PRIMEGUN_CANNON_PACK_FOLDER + DIR_SEP;
}

std::string PrimeGunCannonLibraryFolderForSlot(u32 slot)
{
  return File::GetUserPath(D_LOAD_IDX) + PRIMEGUN_CANNON_LIBRARY_FOLDER + DIR_SEP + "slot_" +
         std::to_string(slot) + DIR_SEP;
}

std::string PrimeGunCannonActivePath(std::string_view texture_name, std::string_view extension)
{
  return PrimeGunCannonActiveFolder() + std::string(texture_name) + std::string(extension);
}

std::string PrimeGunCannonSourcePath(u32 slot, std::string_view texture_name)
{
  const std::string slot_folder = PrimeGunCannonLibraryFolderForSlot(slot);
  const std::string dds_path = slot_folder + std::string(texture_name) + ".dds";
  if (File::Exists(dds_path))
    return dds_path;

  const std::string png_path = slot_folder + std::string(texture_name) + ".png";
  return File::Exists(png_path) ? png_path : std::string();
}

void PrimeGunCannonRegisterPack()
{
  const std::string active_folder = PrimeGunCannonActiveFolder();
  File::CreateDirs(active_folder);
  const std::string gameids_folder = active_folder + "gameids" DIR_SEP;
  File::CreateDirs(gameids_folder);
  const std::string gameids_path =
      gameids_folder + std::string(PRIMEGUN_CANNON_GAME_ID) + ".txt";
  if (!File::Exists(gameids_path))
    File::CreateEmptyFile(gameids_path);
}

void PrimeGunCannonRefreshHiresMap(
    const std::array<std::string, PRIMEGUN_CANNON_TEXTURE_NAMES.size()>& active_paths)
{
  for (const char* texture_name : PRIMEGUN_CANNON_TEXTURE_NAMES)
    HiresTexture::RemoveAssetPath(texture_name);

  HiresTexture::Update();

  for (size_t i = 0; i < PRIMEGUN_CANNON_TEXTURE_NAMES.size(); ++i)
  {
    if (!active_paths[i].empty())
      HiresTexture::SetAssetPath(PRIMEGUN_CANNON_TEXTURE_NAMES[i], active_paths[i]);
    HiresTexture::MarkDirty(PRIMEGUN_CANNON_TEXTURE_NAMES[i]);
  }

  AsyncRequests::GetInstance()->PushEvent([] {
    if (g_texture_cache)
      g_texture_cache->Invalidate();
  });
}

bool ApplyPrimeGunCannonTextureSlot(u32 slot)
{
  PrimeGunCannonRegisterPack();

  for (const char* texture_name : PRIMEGUN_CANNON_TEXTURE_NAMES)
  {
    File::Delete(PrimeGunCannonActivePath(texture_name, ".dds"),
                 File::IfAbsentBehavior::NoConsoleWarning);
    File::Delete(PrimeGunCannonActivePath(texture_name, ".png"),
                 File::IfAbsentBehavior::NoConsoleWarning);
  }

  std::array<std::string, PRIMEGUN_CANNON_TEXTURE_NAMES.size()> active_paths{};
  if (slot != 0)
  {
    bool copied_any = false;
    for (size_t i = 0; i < PRIMEGUN_CANNON_TEXTURE_NAMES.size(); ++i)
    {
      const std::string source_path =
          PrimeGunCannonSourcePath(slot, PRIMEGUN_CANNON_TEXTURE_NAMES[i]);
      if (source_path.empty())
        continue;

      const std::filesystem::path source_fs(source_path);
      const std::string extension = source_fs.extension().string();
      const std::string dest_path =
          PrimeGunCannonActivePath(PRIMEGUN_CANNON_TEXTURE_NAMES[i], extension);
      File::CreateFullPath(dest_path);
      if (!File::Copy(source_path, dest_path, true))
      {
        WARN_LOG_FMT(VIDEO, "PrimeGun: Failed to apply cannon texture '{}' from '{}'.",
                     PRIMEGUN_CANNON_TEXTURE_NAMES[i], source_path);
        continue;
      }

      active_paths[i] = dest_path;
      copied_any = true;
    }

    if (!copied_any)
      return false;
  }

  Config::SetBaseOrCurrent(Config::GFX_HIRES_TEXTURES, true);
  UpdateActiveConfig();
  PrimeGunCannonRefreshHiresMap(active_paths);
  return true;
}

u32 VrMenuItemCountForTab(u32 tab)
{
  switch (tab)
  {
  case 1:
    return 7;
  case 2:
    return 9;
  case 3:
    return 8;
  case 4:
    return 2;
  case VR_MENU_CANNON_TAB:
    return 6;
  default:
    return 4;
  }
}

bool VrMenuRowIsNumeric(u32 tab, u32 index)
{
  switch (tab)
  {
  case 0:
    return index == 1 || index == 2;
  case 1:
    return index <= 5;
  case 2:
    return index == 2 || index == 5 || index == 6 || index == 7;
  case 3:
    return index >= 3 && index <= 6;
  default:
    return false;
  }
}

void ClampVrMenuSelection()
{
  const u32 count = VrMenuItemCountForTab(s_vr_menu_tab);
  if (count == 0)
    s_vr_menu_selected_index = 0;
  else if (s_vr_menu_selected_index >= count)
    s_vr_menu_selected_index = count - 1;
}

void RotatePoseVector(const Pose& pose, float x, float y, float z, float* out_x, float* out_y,
                      float* out_z)
{
  const float tx = 2.0f * (pose.qy * z - pose.qz * y);
  const float ty = 2.0f * (pose.qz * x - pose.qx * z);
  const float tz = 2.0f * (pose.qx * y - pose.qy * x);
  *out_x = x + pose.qw * tx + (pose.qy * tz - pose.qz * ty);
  *out_y = y + pose.qw * ty + (pose.qz * tx - pose.qx * tz);
  *out_z = z + pose.qw * tz + (pose.qx * ty - pose.qy * tx);
}

Pose HybridControllerPose(const Common::VR::OpenXRControllerState& controller)
{
  Pose pose = PoseFromOpenXR(controller.aim_pose);
  if (controller.grip_pose.valid)
  {
    pose.valid = true;
    pose.px = controller.grip_pose.position[0];
    pose.py = controller.grip_pose.position[1];
    pose.pz = controller.grip_pose.position[2];
    if (!controller.aim_pose.valid)
    {
      pose.qx = controller.grip_pose.orientation[0];
      pose.qy = controller.grip_pose.orientation[1];
      pose.qz = controller.grip_pose.orientation[2];
      pose.qw = controller.grip_pose.orientation[3];
    }
  }
  return pose;
}

Pose GripControllerPose(const Common::VR::OpenXRControllerState& controller)
{
  Pose pose = PoseFromOpenXR(controller.grip_pose);
  if (!pose.valid)
    pose = HybridControllerPose(controller);
  return pose;
}

bool VrMenuPointerHit(const Pose& left, const Pose& right, float* x_out, float* y_out)
{
  if (!left.valid || !right.valid)
    return false;

  Pose panel = left;
  panel.qx = left.qx * 0.70710678f - left.qw * 0.70710678f;
  panel.qy = left.qy * 0.70710678f - left.qz * 0.70710678f;
  panel.qz = left.qz * 0.70710678f + left.qy * 0.70710678f;
  panel.qw = left.qw * 0.70710678f + left.qx * 0.70710678f;

  float ox = 0.0f, oy = 0.0f, oz = 0.0f;
  RotatePoseVector(panel, 0.0f, 0.10f, -0.18f, &ox, &oy, &oz);
  const float cx = left.px + ox;
  const float cy = left.py + oy;
  const float cz = left.pz + oz;

  float rx = 0.0f, ry = 0.0f, rz = 0.0f;
  float ux = 0.0f, uy = 0.0f, uz = 0.0f;
  float nx = 0.0f, ny = 0.0f, nz = 0.0f;
  float dx = 0.0f, dy = 0.0f, dz = 0.0f;
  RotatePoseVector(panel, 1.0f, 0.0f, 0.0f, &rx, &ry, &rz);
  RotatePoseVector(panel, 0.0f, 1.0f, 0.0f, &ux, &uy, &uz);
  RotatePoseVector(panel, 0.0f, 0.0f, 1.0f, &nx, &ny, &nz);
  RotatePoseVector(right, 0.0f, 0.0f, -1.0f, &dx, &dy, &dz);

  const float denom = dx * nx + dy * ny + dz * nz;
  if (std::fabs(denom) < 0.001f)
    return false;

  const float vx = cx - right.px;
  const float vy = cy - right.py;
  const float vz = cz - right.pz;
  const float t = (vx * nx + vy * ny + vz * nz) / denom;
  if (t < 0.02f || t > 3.0f)
    return false;

  const float hx = right.px + dx * t - cx;
  const float hy = right.py + dy * t - cy;
  const float hz = right.pz + dz * t - cz;
  *x_out = 0.5f + (hx * rx + hy * ry + hz * rz) / 1.05f;
  *y_out = 0.5f - (hx * ux + hy * uy + hz * uz) / 0.72f;
  return *x_out >= 0.0f && *x_out <= 1.0f && *y_out >= 0.0f && *y_out <= 1.0f;
}

void ResetControllerSettings(RuntimeSettings* settings)
{
  settings->use_right_hand = true;
  settings->require_trigger = false;
  settings->trigger_threshold = 0.5f;
  settings->xr_dpad_enabled = true;
  settings->xr_dpad_head_radius = 0.28f;
  settings->xr_dpad_head_y_below = 0.02f;
  settings->xr_dpad_deadzone = 0.45f;
}

void ResetMovementSettings(RuntimeSettings* settings)
{
  settings->directional_movement_enabled = true;
  settings->directional_movement_use_right_stick = false;
  settings->directional_movement_use_hmd_direction = false;
  settings->directional_movement_deadzone = 0.25f;
  settings->directional_movement_speed = 14.0f;
  settings->directional_movement_accel = 45.0f;
  settings->directional_movement_air_accel = 8.0f;
}

void ResetVrRuntimeSettings(RuntimeSettings* settings)
{
  const bool enabled = settings->enabled;
  const bool builtin_patches_enabled = settings->builtin_patches_enabled;
  const bool patch_disable_frustum_culling = settings->patch_disable_frustum_culling;
  const bool patch_no_idle_sway = settings->patch_no_idle_sway;
  const bool patch_disable_arm_cannon_idle_fidget = settings->patch_disable_arm_cannon_idle_fidget;
  const bool patch_beam_projectile_timing = settings->patch_beam_projectile_timing;
  const bool patch_xr_visor_dpad_timing = settings->patch_xr_visor_dpad_timing;
  const bool patch_cannon_rotation = settings->patch_cannon_rotation;
  const bool patch_gun_ray_target = settings->patch_gun_ray_target;
  const bool patch_reticle = settings->patch_reticle;
  *settings = RuntimeSettings{};
  settings->enabled = enabled;
  settings->builtin_patches_enabled = builtin_patches_enabled;
  settings->patch_disable_frustum_culling = patch_disable_frustum_culling;
  settings->patch_no_idle_sway = patch_no_idle_sway;
  settings->patch_disable_arm_cannon_idle_fidget = patch_disable_arm_cannon_idle_fidget;
  settings->patch_beam_projectile_timing = patch_beam_projectile_timing;
  settings->patch_xr_visor_dpad_timing = patch_xr_visor_dpad_timing;
  settings->patch_cannon_rotation = patch_cannon_rotation;
  settings->patch_gun_ray_target = patch_gun_ray_target;
  settings->patch_reticle = patch_reticle;
}

void AdjustVrMenuSetting(RuntimeSettings* settings, int direction)
{
  const float sign = direction < 0 ? -1.0f : 1.0f;
  switch (s_vr_menu_tab)
  {
  case 1:
    switch (s_vr_menu_selected_index)
    {
    case 0:
      settings->model_offset_x += sign * 0.01f;
      break;
    case 1:
      settings->model_offset_y += sign * 0.01f;
      break;
    case 2:
      settings->model_offset_z += sign * 0.01f;
      break;
    case 3:
      settings->rot_offset_x += sign * 1.0f;
      break;
    case 4:
      settings->rot_offset_y += sign * 1.0f;
      break;
    case 5:
      settings->rot_offset_z += sign * 1.0f;
      break;
    default:
      break;
    }
    break;
  case 2:
    switch (s_vr_menu_selected_index)
    {
    case 2:
      settings->trigger_threshold =
          std::clamp(settings->trigger_threshold + sign * 0.05f, 0.0f, 1.0f);
      break;
    case 5:
      settings->xr_dpad_head_radius =
          std::clamp(settings->xr_dpad_head_radius + sign * 0.01f, 0.05f, 0.60f);
      break;
    case 6:
      settings->xr_dpad_head_y_below =
          std::clamp(settings->xr_dpad_head_y_below + sign * 0.01f, 0.0f, 0.60f);
      break;
    case 7:
      settings->xr_dpad_deadzone =
          std::clamp(settings->xr_dpad_deadzone + sign * 0.05f, 0.0f, 0.95f);
      break;
    default:
      break;
    }
    break;
  case 3:
    switch (s_vr_menu_selected_index)
    {
    case 3:
      settings->directional_movement_deadzone =
          std::clamp(settings->directional_movement_deadzone + sign * 0.05f, 0.0f, 0.95f);
      break;
    case 4:
      settings->directional_movement_speed =
          std::clamp(settings->directional_movement_speed + sign * 0.5f, 1.0f, 60.0f);
      break;
    case 5:
      settings->directional_movement_accel =
          std::clamp(settings->directional_movement_accel + sign * 1.0f, 1.0f, 120.0f);
      break;
    case 6:
      settings->directional_movement_air_accel =
          std::clamp(settings->directional_movement_air_accel + sign * 1.0f, 1.0f, 120.0f);
      break;
    default:
      break;
    }
    break;
  default:
    if (s_vr_menu_tab == 0)
    {
      if (s_vr_menu_selected_index == 1)
        settings->gun_targeting_distance =
            std::clamp(settings->gun_targeting_distance + sign * 1.0f, 1.0f, 200.0f);
      else if (s_vr_menu_selected_index == 2)
        settings->gun_targeting_radius =
            std::clamp(settings->gun_targeting_radius + sign * 0.1f, 0.1f, 25.0f);
    }
    break;
  }
}

void ActivateVrMenuSelection(RuntimeSettings* settings)
{
  if (s_vr_menu_tab == 0)
  {
    if (s_vr_menu_selected_index == 0)
      settings->gun_targeting_enabled = !settings->gun_targeting_enabled;
    else if (s_vr_menu_selected_index == 3)
    {
      settings->gun_targeting_enabled = true;
      settings->gun_targeting_distance = 60.0f;
      settings->gun_targeting_radius = 4.0f;
    }
    return;
  }

  if (s_vr_menu_tab == 1)
  {
    if (s_vr_menu_selected_index == 6)
    {
      settings->model_offset_x = DEFAULT_MODEL_OFFSET_X;
      settings->model_offset_y = DEFAULT_MODEL_OFFSET_Y;
      settings->model_offset_z = DEFAULT_MODEL_OFFSET_Z;
      settings->rot_offset_x = DEFAULT_ROT_OFFSET_X;
      settings->rot_offset_y = DEFAULT_ROT_OFFSET_Y;
      settings->rot_offset_z = DEFAULT_ROT_OFFSET_Z;
    }
    return;
  }

  if (s_vr_menu_tab == 2)
  {
    if (s_vr_menu_selected_index == 0)
      settings->use_right_hand = !settings->use_right_hand;
    else if (s_vr_menu_selected_index == 1)
      settings->require_trigger = !settings->require_trigger;
    else if (s_vr_menu_selected_index == 3)
      settings->xr_dpad_enabled = !settings->xr_dpad_enabled;
    else if (s_vr_menu_selected_index == 4)
      settings->xr_dpad_enabled = !settings->xr_dpad_enabled;
    else if (s_vr_menu_selected_index == 8)
      ResetControllerSettings(settings);
    return;
  }

  if (s_vr_menu_tab == 3)
  {
    if (s_vr_menu_selected_index == 0)
      settings->directional_movement_enabled = !settings->directional_movement_enabled;
    else if (s_vr_menu_selected_index == 1)
      settings->directional_movement_use_right_stick = !settings->directional_movement_use_right_stick;
    else if (s_vr_menu_selected_index == 2)
      settings->directional_movement_use_hmd_direction =
          !settings->directional_movement_use_hmd_direction;
    else if (s_vr_menu_selected_index == 7)
      ResetMovementSettings(settings);
    return;
  }

  if (s_vr_menu_tab == 4)
  {
    if (s_vr_menu_selected_index == 0)
    {
      settings->offset_x = 0.0f;
      settings->offset_y = 0.0f;
      settings->offset_z = 0.0f;
      settings->model_offset_x = DEFAULT_MODEL_OFFSET_X;
      settings->model_offset_y = DEFAULT_MODEL_OFFSET_Y;
      settings->model_offset_z = DEFAULT_MODEL_OFFSET_Z;
      settings->rot_offset_x = DEFAULT_ROT_OFFSET_X;
      settings->rot_offset_y = DEFAULT_ROT_OFFSET_Y;
      settings->rot_offset_z = DEFAULT_ROT_OFFSET_Z;
    }
    else if (s_vr_menu_selected_index == 1)
    {
      settings->offset_x = 0.0f;
      settings->offset_y = 0.0f;
      settings->offset_z = 0.0f;
      settings->model_offset_x = 0.0f;
      settings->model_offset_y = -0.300f;
      settings->model_offset_z = 0.0f;
      settings->rot_offset_x = 0.0f;
      settings->rot_offset_y = 20.0f;
      settings->rot_offset_z = -90.0f;
    }
  }

  if (s_vr_menu_tab == VR_MENU_CANNON_TAB)
  {
    if (s_vr_menu_selected_index <= 5)
    {
      if (ApplyPrimeGunCannonTextureSlot(s_vr_menu_selected_index))
      {
        s_vr_cannon_texture_slot = s_vr_menu_selected_index;
        s_vr_cannon_texture_notice_until_frame = s_frame_counter + 180;
      }
      return;
    }
  }
}

void SaveVrMenuSettingsNotice()
{
  s_vr_settings_save_requested = true;
  s_vr_menu_saved_notice_until_frame = s_frame_counter + 180;
  ++s_vr_menu_generation;
}

void PublishVrOverlayState(const RuntimeSettings& settings, bool prompt_visible)
{
  const Common::VR::PrimeGunVrOverlayState previous =
      Common::VR::OpenXRInputState::GetPrimeGunOverlay();
  Common::VR::PrimeGunVrOverlayState overlay{};
  overlay.prompt_visible = settings.vr_overlays_enabled && prompt_visible;
  overlay.menu_visible = settings.vr_overlays_enabled && s_vr_menu_visible;
  overlay.menu_pointer_active = settings.vr_overlays_enabled && s_vr_menu_visible;
  overlay.saved_notice = s_frame_counter < s_vr_menu_saved_notice_until_frame;
  overlay.generation = s_vr_menu_generation;
  overlay.tab = s_vr_menu_tab;
  overlay.selected_index = s_vr_menu_selected_index;
  overlay.item_count = VrMenuItemCountForTab(s_vr_menu_tab);
  overlay.cannon_texture_slot = s_vr_cannon_texture_slot;
  overlay.cannon_texture_notice = s_frame_counter < s_vr_cannon_texture_notice_until_frame;
  overlay.weapon_panel_visible = settings.vr_overlays_enabled && previous.weapon_panel_visible;
  overlay.weapon_selected_index = previous.weapon_selected_index;
  overlay.weapon_panel_position = previous.weapon_panel_position;
  overlay.weapon_panel_orientation = previous.weapon_panel_orientation;
  overlay.world_scale = settings.world_scale;
  overlay.use_right_hand = settings.use_right_hand;
  overlay.require_trigger = settings.require_trigger;
  overlay.trigger_threshold = settings.trigger_threshold;
  overlay.gun_targeting_enabled = settings.gun_targeting_enabled;
  overlay.gun_targeting_distance = settings.gun_targeting_distance;
  overlay.gun_targeting_radius = settings.gun_targeting_radius;
  overlay.xr_dpad_enabled = settings.xr_dpad_enabled;
  overlay.xr_dpad_head_radius = settings.xr_dpad_head_radius;
  overlay.xr_dpad_head_y_below = settings.xr_dpad_head_y_below;
  overlay.xr_dpad_deadzone = settings.xr_dpad_deadzone;
  overlay.directional_movement_enabled = settings.directional_movement_enabled;
  overlay.directional_movement_use_right_stick = settings.directional_movement_use_right_stick;
  overlay.directional_movement_use_hmd_direction = settings.directional_movement_use_hmd_direction;
  overlay.directional_movement_deadzone = settings.directional_movement_deadzone;
  overlay.directional_movement_speed = settings.directional_movement_speed;
  overlay.directional_movement_accel = settings.directional_movement_accel;
  overlay.directional_movement_air_accel = settings.directional_movement_air_accel;
  overlay.offset_x = settings.offset_x;
  overlay.offset_y = settings.offset_y;
  overlay.offset_z = settings.offset_z;
  overlay.model_offset_x = settings.model_offset_x;
  overlay.model_offset_y = settings.model_offset_y;
  overlay.model_offset_z = settings.model_offset_z;
  overlay.rot_offset_x = settings.rot_offset_x;
  overlay.rot_offset_y = settings.rot_offset_y;
  overlay.rot_offset_z = settings.rot_offset_z;
  Common::VR::OpenXRInputState::SetPrimeGunOverlay(overlay);
}

void UpdateVrMenu(const Common::VR::OpenXRInputSnapshot& snapshot, RuntimeSettings* settings,
                  bool prompt_visible)
{
  if (!settings->vr_overlays_enabled)
  {
    if (s_vr_menu_visible)
    {
      s_vr_menu_visible = false;
      ++s_vr_menu_generation;
    }
    s_last_vr_menu_thumbstick = false;
    PublishVrOverlayState(*settings, false);
    return;
  }

  if (!snapshot.runtime_active)
  {
    PublishVrOverlayState(*settings, false);
    return;
  }

  const auto& left = snapshot.controllers[0];
  const auto& right = snapshot.controllers[1];
  const bool menu_toggle = left.connected && (left.thumbstick_button || left.menu_button);
  if (menu_toggle && !s_last_vr_menu_thumbstick)
  {
    s_vr_menu_visible = !s_vr_menu_visible;
    ++s_vr_menu_generation;
  }
  s_last_vr_menu_thumbstick = menu_toggle;

  float pointer_x = 0.5f;
  float pointer_y = 0.5f;
  bool pointer_active = false;
  if (s_vr_menu_visible)
  {
    const Pose left_pose = GripControllerPose(snapshot.controllers[0]);
    const Pose right_pose = PoseFromOpenXR(snapshot.controllers[1].aim_pose);
    pointer_active = VrMenuPointerHit(left_pose, right_pose, &pointer_x, &pointer_y);
    if (pointer_active)
    {
      constexpr float first_y = 154.0f / 512.0f;
      constexpr float step_y = 25.0f / 512.0f;
      const u32 item_count = VrMenuItemCountForTab(s_vr_menu_tab);
      if (item_count > 0 && pointer_y >= first_y)
      {
        int hovered = static_cast<int>((pointer_y - first_y + step_y * 0.5f) / step_y);
        hovered = std::clamp(hovered, 0, static_cast<int>(item_count) - 1);
        if (s_vr_menu_selected_index != static_cast<u32>(hovered))
        {
          s_vr_menu_selected_index = static_cast<u32>(hovered);
          ++s_vr_menu_generation;
        }
      }
    }

    s_last_vr_menu_stick_up = false;
    s_last_vr_menu_stick_down = false;
    s_last_vr_menu_stick_left = false;
    s_last_vr_menu_stick_right = false;

    const bool primary = right.connected && right.primary_button;
    const bool secondary = right.connected && right.secondary_button;
    if (primary && !s_last_vr_menu_primary)
    {
      const float texture_x = std::clamp(pointer_x, 0.0f, 1.0f) * 1024.0f;
      const float texture_y = std::clamp(pointer_y, 0.0f, 1.0f) * 512.0f;
      if (pointer_active && texture_y >= 64.0f && texture_y <= 102.0f)
      {
        constexpr float tab_step = 162.0f;
        constexpr float tab_width = 150.0f;
        const int tab = static_cast<int>((texture_x - 36.0f) / tab_step);
        const float tab_local_x = texture_x - (36.0f + static_cast<float>(tab) * tab_step);
        if (tab >= 0 && tab < static_cast<int>(VR_MENU_TAB_COUNT) && tab_local_x >= 0.0f &&
            tab_local_x <= tab_width &&
            s_vr_menu_tab != static_cast<u32>(tab))
        {
          s_vr_menu_tab = static_cast<u32>(tab);
          s_vr_menu_selected_index = 0;
          ++s_vr_menu_generation;
        }
      }
      else if (pointer_active && texture_y >= 108.0f && texture_y <= 136.0f)
      {
        if (texture_x >= 52.0f && texture_x <= 272.0f)
          SaveVrMenuSettingsNotice();
        else if (texture_x >= 300.0f && texture_x <= 520.0f)
        {
          ResetVrRuntimeSettings(settings);
          ++s_vr_menu_generation;
        }
      }
      else if (pointer_active)
      {
        constexpr float row_x = 52.0f;
        constexpr float row_width = 920.0f;
        constexpr float value_width = 170.0f;
        constexpr float value_box_x = row_x + row_width - 190.0f;
        if (texture_y >= 154.0f && texture_x >= value_box_x &&
            texture_x <= value_box_x + value_width)
        {
          if (VrMenuRowIsNumeric(s_vr_menu_tab, s_vr_menu_selected_index))
            AdjustVrMenuSetting(settings, texture_x >= value_box_x + value_width * 0.5f ? 1 : -1);
          else
            ActivateVrMenuSelection(settings);
          ++s_vr_menu_generation;
        }
        else
        {
          ActivateVrMenuSelection(settings);
          ++s_vr_menu_generation;
        }
      }
    }
    if (secondary && !s_last_vr_menu_secondary)
    {
      // B is suppressed from gameplay while the VR menu is open, but it does not close or
      // change the menu. The old menu only changed settings through laser hits.
    }
    s_last_vr_menu_trigger = false;
    s_last_vr_menu_primary = primary;
    s_last_vr_menu_secondary = secondary;
  }

  PublishVrOverlayState(*settings, !s_vr_menu_visible && prompt_visible);
  auto overlay = Common::VR::OpenXRInputState::GetPrimeGunOverlay();
  overlay.menu_pointer_active = s_vr_menu_visible && pointer_active;
  overlay.pointer_x = pointer_x;
  overlay.pointer_y = pointer_y;
  Common::VR::OpenXRInputState::SetPrimeGunOverlay(overlay);
}

void UpdateHeightOnlyReset(const Common::VR::OpenXRInputSnapshot& snapshot,
                           const Common::VR::OpenXRControllerState& hand,
                           const RuntimeSettings& settings)
{
  const bool left_in_visor_zone =
      snapshot.controllers[0].connected && snapshot.controllers[0].aim_pose.valid &&
      snapshot.head_pose.valid &&
      LeftControllerNearHead(PoseFromOpenXR(snapshot.controllers[0].aim_pose),
                             PoseFromOpenXR(snapshot.head_pose), settings);
  const bool pressed = snapshot.runtime_active && hand.connected && hand.thumbstick_button &&
                       snapshot.head_pose.valid && !left_in_visor_zone;
  if (pressed && !s_last_height_reset_thumbstick)
  {
    // Height calibration intentionally changes only Prime's vertical axis.
    // The horizontal baseline remains untouched so the play-space center is not re-zeroed sideways.
    s_controller_base_z = snapshot.head_pose.position[1] * settings.world_scale;
    s_translation_base_valid = true;
    s_height_prompt_until_frame = 0;
  }
  s_last_height_reset_thumbstick = pressed;
}

bool UpdateHeightPromptGate(const Core::CPUThreadGuard& guard,
                            const Common::VR::OpenXRInputSnapshot& snapshot,
                            const RuntimeSettings& settings, u32 player)
{
  if (!snapshot.runtime_active || player < 0x80000000u)
  {
    s_prompt_gameplay_ready_since_frame = 0;
    s_height_prompt_until_frame = 0;
    return false;
  }

  u32 gun = 0;
  const bool gameplay_ready = PlayerIsFirstPersonGunReady(guard, player) &&
                              TryReadU32(guard, player + ADDRESS.cannon_offset, &gun) &&
                              gun >= 0x80000000u;
  if (gameplay_ready)
  {
    if (s_prompt_gameplay_ready_since_frame == 0)
      s_prompt_gameplay_ready_since_frame = s_frame_counter;
  }
  else
  {
    s_prompt_gameplay_ready_since_frame = 0;
    return false;
  }

  const bool stable_ready = s_prompt_gameplay_ready_since_frame != 0 &&
                            s_frame_counter >= s_prompt_gameplay_ready_since_frame + 72;
  if (!stable_ready)
    return false;

  if (!s_prompt_skipped_first_ready)
  {
    s_prompt_skipped_first_ready = true;
    s_prompt_waiting_for_second_ready = true;
    s_prompt_first_ready_timeout_frame = s_frame_counter + 600;
    return false;
  }

  if (s_prompt_waiting_for_second_ready)
  {
    if (s_frame_counter < s_prompt_first_ready_timeout_frame)
      return false;
    s_prompt_waiting_for_second_ready = false;
  }

  if (!s_prompt_shown_this_session)
  {
    s_prompt_shown_this_session = true;
    s_height_prompt_until_frame = s_frame_counter + 600;
  }

  const bool left_in_visor_zone =
      snapshot.controllers[0].connected && snapshot.controllers[0].aim_pose.valid &&
      snapshot.head_pose.valid &&
      LeftControllerNearHead(PoseFromOpenXR(snapshot.controllers[0].aim_pose),
                             PoseFromOpenXR(snapshot.head_pose), settings);
  const bool right_thumb_click = snapshot.controllers[1].connected &&
                                 snapshot.controllers[1].thumbstick_button &&
                                 !left_in_visor_zone;
  if (right_thumb_click)
    s_height_prompt_until_frame = 0;

  return s_height_prompt_until_frame != 0 && s_frame_counter < s_height_prompt_until_frame;
}
#endif

void UpdateCannonTracking(const Core::CPUThreadGuard& guard)
{
#ifdef ENABLE_VR
  RuntimeSettings settings = GetRuntimeSettings();
  if (!settings.enabled)
    return;

  const Common::VR::OpenXRInputSnapshot snapshot = Common::VR::OpenXRInputState::GetSnapshot();
  if (!snapshot.runtime_active)
  {
    PublishVrOverlayState(settings, false);
    return;
  }

  u32 prompt_player = 0;
  if (!TryReadU32(guard, ADDRESS.state_manager + ADDRESS.player_offset, &prompt_player))
    prompt_player = 0;
  const bool prompt_visible = UpdateHeightPromptGate(guard, snapshot, settings, prompt_player);

  UpdateVrMenu(snapshot, &settings, prompt_visible);
  SetRuntimeSettings(settings);

  const float yaw_delta_deg = GetPlayerYawDeltaDegrees(guard);
  UpdateDirectionalMovement(guard, snapshot, settings, yaw_delta_deg);
  UpdateXrDpad(guard, snapshot, settings);

  const Common::VR::OpenXRControllerState& hand =
      snapshot.controllers[settings.use_right_hand ? 1 : 0];
  if (!hand.connected || !hand.aim_pose.valid)
    return;

  UpdateHeightOnlyReset(snapshot, hand, settings);

  if (settings.require_trigger && hand.trigger_value < settings.trigger_threshold)
    return;

  const Pose pose{
      hand.aim_pose.position[0],
      hand.aim_pose.position[1],
      hand.aim_pose.position[2],
      hand.aim_pose.orientation[0],
      hand.aim_pose.orientation[1],
      hand.aim_pose.orientation[2],
      hand.aim_pose.orientation[3],
      true,
  };
  Pose adjusted_pose = pose;
  ApplyTrackingWorldYaw(adjusted_pose, yaw_delta_deg);

  Matrix3x4 controller_mat_no_offset =
      PoseToPrimeMatrix(adjusted_pose, 0.0f, 0.0f, 0.0f, settings.rot_offset_x,
                        settings.rot_offset_y, settings.rot_offset_z, settings.world_scale);

  Matrix3x4 mat =
      PoseToPrimeMatrix(adjusted_pose, 0.0f, 0.0f, 0.0f, settings.rot_offset_x,
                        settings.rot_offset_y, settings.rot_offset_z, settings.world_scale);

  if (!s_translation_base_valid)
  {
    s_controller_base_x = 0.0f;
    s_controller_base_y = 0.0f;
    s_controller_base_z = 0.0f;
    s_translation_base_valid = true;
  }

  float offset_x = settings.offset_x * settings.world_scale;
  float offset_y = -(settings.offset_z * settings.world_scale);
  const float offset_z = settings.offset_y * settings.world_scale;
  RotatePrimeXY(offset_x, offset_y, yaw_delta_deg);

  mat.At(0, 3) = controller_mat_no_offset.At(0, 3) - s_controller_base_x + offset_x;
  mat.At(1, 3) = controller_mat_no_offset.At(1, 3) - s_controller_base_y + offset_y;
  mat.At(2, 3) = controller_mat_no_offset.At(2, 3) - s_controller_base_z + offset_z;

  const float smooth_dx = mat.At(0, 3) - s_smooth_matrix[3];
  const float smooth_dy = mat.At(1, 3) - s_smooth_matrix[7];
  const float smooth_dz = mat.At(2, 3) - s_smooth_matrix[11];
  const float smooth_jump_sq =
      smooth_dx * smooth_dx + smooth_dy * smooth_dy + smooth_dz * smooth_dz;
  constexpr float cannon_smooth_keep = 0.24f;
  constexpr float cannon_smooth_snap_distance = 2.0f;
  if (!s_smooth_matrix_valid ||
      smooth_jump_sq > cannon_smooth_snap_distance * cannon_smooth_snap_distance)
  {
    for (int i = 0; i < 12; ++i)
      s_smooth_matrix[i] = mat.m[i];
    s_smooth_matrix_valid = true;
  }
  else
  {
    for (int i = 0; i < 12; ++i)
      s_smooth_matrix[i] =
          s_smooth_matrix[i] * cannon_smooth_keep + mat.m[i] * (1.0f - cannon_smooth_keep);
  }
  for (int i = 0; i < 12; ++i)
    mat.m[i] = s_smooth_matrix[i];

  u32 player = 0;
  if (!TryReadU32(guard, ADDRESS.state_manager + ADDRESS.player_offset, &player) ||
      player < 0x80000000u)
  {
    s_smooth_matrix_valid = false;
    s_last_validated_gun = 0;
    TryWriteU32(guard, CANNON_EXPECTED_GUN_SCRATCH, 0);
    WriteGunTargetScratch(guard, 0, 0xffffu);
    TryWriteU32(guard, RETICLE_BILLBOARD_SCRATCH, 0);
    return;
  }

  UpdateScanPitch(guard, player, mat);

  u32 gun = 0;
  if (!PlayerIsFirstPersonGunReady(guard, player) ||
      !TryReadU32(guard, player + ADDRESS.cannon_offset, &gun) || gun < 0x80000000u)
  {
    s_smooth_matrix_valid = false;
    s_last_validated_gun = 0;
    TryWriteU32(guard, CANNON_EXPECTED_GUN_SCRATCH, 0);
    WriteGunTargetScratch(guard, 0, 0xffffu);
    TryWriteU32(guard, RETICLE_BILLBOARD_SCRATCH, 0);
    return;
  }

  const u32 gun_xf = gun + ADDRESS.gun_xf_offset;
  const u32 beam_xf = gun + ADDRESS.beam_xf_offset;
  const u32 world_xf = gun + ADDRESS.world_xf_offset;
  const u32 local_xf = gun + ADDRESS.local_xf_offset;
  const bool gun_chain_valid =
      s_last_validated_gun == gun ||
      (LooksLikeTransformMatrix(guard, gun_xf) && LooksLikeTransformMatrix(guard, beam_xf) &&
       LooksLikeTransformMatrix(guard, world_xf) && LooksLikeTransformMatrix(guard, local_xf));
  if (!gun_chain_valid)
  {
    s_smooth_matrix_valid = false;
    s_last_validated_gun = 0;
    TryWriteU32(guard, CANNON_EXPECTED_GUN_SCRATCH, 0);
    WriteGunTargetScratch(guard, 0, 0xffffu);
    TryWriteU32(guard, RETICLE_BILLBOARD_SCRATCH, 0);
    return;
  }
  s_last_validated_gun = gun;

  WriteBasisScratch(guard, mat);
  TryWriteFloat(guard, ADDRESS.gun_pos + 0, mat.At(0, 3));
  TryWriteFloat(guard, ADDRESS.gun_pos + 4, mat.At(1, 3));
  TryWriteFloat(guard, ADDRESS.gun_pos + 8, mat.At(2, 3));

  const float model_local_x = settings.model_offset_x * settings.world_scale;
  const float model_local_y =
      (BASE_MODEL_FORWARD_BACK_OFFSET + settings.model_offset_y) * settings.world_scale;
  const float model_local_z = settings.model_offset_z * settings.world_scale;
  const float model_world_x =
      mat.m[0] * model_local_x + mat.m[1] * model_local_y + mat.m[2] * model_local_z;
  const float model_world_y =
      mat.m[4] * model_local_x + mat.m[5] * model_local_y + mat.m[6] * model_local_z;
  const float model_world_z =
      mat.m[8] * model_local_x + mat.m[9] * model_local_y + mat.m[10] * model_local_z;
  TryWriteFloat(guard, MODEL_OFFSET_WORLD_SCRATCH + 0, model_world_x);
  TryWriteFloat(guard, MODEL_OFFSET_WORLD_SCRATCH + 4, model_world_y);
  TryWriteFloat(guard, MODEL_OFFSET_WORLD_SCRATCH + 8, model_world_z);
  TryWriteU32(guard, CANNON_EXPECTED_GUN_SCRATCH, gun);

  UpdateReticleBillboard(guard, snapshot, player, yaw_delta_deg);
  UpdateGunTargeting(guard, ADDRESS.state_manager, player, gun, world_xf, mat, settings);
#endif
}

bool ApplyFirstPersonPitchLoadPatch(const Core::CPUThreadGuard& guard)
{
  auto& patch = s_first_person_pitch_load_patch;
  u32 current = 0;
  if (!TryReadU32(guard, patch.address, &current))
    return false;

  const u32 branch = PpcBranch(patch.address, patch.cave);
  if (current == branch)
  {
    patch.applied = true;
    return false;
  }

  if (current != patch.original)
  {
    patch.applied = false;
    return false;
  }

  const u32 zero_label = patch.cave + 0x18u;
  const u32 return_addr = patch.address + 4u;
  TryWriteInstruction(guard, patch.cave + 0x00u, 0x819E0330u);
  TryWriteInstruction(guard, patch.cave + 0x04u, 0x2C0C0000u);
  TryWriteInstruction(guard, patch.cave + 0x08u, PpcBeq(patch.cave + 0x08u, zero_label));
  TryWriteInstruction(guard, patch.cave + 0x0Cu, patch.original);
  TryWriteInstruction(guard, patch.cave + 0x10u, PpcBranch(patch.cave + 0x10u, return_addr));
  TryWriteInstruction(guard, patch.cave + 0x14u, 0x60000000u);
  TryWriteInstruction(guard, patch.cave + 0x18u, LOAD_ZERO_TO_F31);
  TryWriteInstruction(guard, patch.cave + 0x1Cu, PpcBranch(patch.cave + 0x1Cu, return_addr));
  patch.applied = TryWriteInstruction(guard, patch.address, branch);
  return patch.applied;
}

bool ApplyRenderModelOffsetPatch(const Core::CPUThreadGuard& guard)
{
  DynamicPpcPatch& patch = s_render_model_offset_patch;
  u32 current = 0;
  if (!TryReadU32(guard, patch.address, &current))
    return false;

  const u32 branch = PpcBranch(patch.address, patch.cave);
  if (current == branch)
  {
    if (patch.original == 0)
      TryReadU32(guard, patch.cave + 0x44u, &patch.original);
    patch.applied = patch.original != 0;
    return false;
  }

  if ((current & 0xFFFF0000u) != 0x94210000u)
  {
    patch.applied = false;
    return false;
  }

  patch.original = current;
  constexpr u32 pos_scratch_hi = (ADJUSTED_GUN_POS_SCRATCH >> 16) & 0xffffu;
  constexpr u32 pos_scratch_lo = ADJUSTED_GUN_POS_SCRATCH & 0xffffu;
  constexpr u32 offset_scratch_hi = (MODEL_OFFSET_WORLD_SCRATCH >> 16) & 0xffffu;
  constexpr u32 offset_scratch_lo = MODEL_OFFSET_WORLD_SCRATCH & 0xffffu;
  const u32 return_addr = patch.address + 4u;

  TryWriteInstruction(guard, patch.cave + 0x00u, 0x3D800000u | pos_scratch_hi);
  TryWriteInstruction(guard, patch.cave + 0x04u, 0x618C0000u | pos_scratch_lo);
  TryWriteInstruction(guard, patch.cave + 0x08u, 0x3D600000u | offset_scratch_hi);
  TryWriteInstruction(guard, patch.cave + 0x0Cu, 0x616B0000u | offset_scratch_lo);
  TryWriteInstruction(guard, patch.cave + 0x10u, 0xC0050000u);
  TryWriteInstruction(guard, patch.cave + 0x14u, 0xC02B0000u);
  TryWriteInstruction(guard, patch.cave + 0x18u, 0xEC00082Au);
  TryWriteInstruction(guard, patch.cave + 0x1Cu, 0xD00C0000u);
  TryWriteInstruction(guard, patch.cave + 0x20u, 0xC0450004u);
  TryWriteInstruction(guard, patch.cave + 0x24u, 0xC06B0004u);
  TryWriteInstruction(guard, patch.cave + 0x28u, 0xEC42182Au);
  TryWriteInstruction(guard, patch.cave + 0x2Cu, 0xD04C0004u);
  TryWriteInstruction(guard, patch.cave + 0x30u, 0xC0850008u);
  TryWriteInstruction(guard, patch.cave + 0x34u, 0xC0AB0008u);
  TryWriteInstruction(guard, patch.cave + 0x38u, 0xEC84282Au);
  TryWriteInstruction(guard, patch.cave + 0x3Cu, 0xD08C0008u);
  TryWriteInstruction(guard, patch.cave + 0x40u, 0x7D856378u);
  TryWriteInstruction(guard, patch.cave + 0x44u, patch.original);
  TryWriteInstruction(guard, patch.cave + 0x48u, PpcBranch(patch.cave + 0x48u, return_addr));
  patch.applied = TryWriteInstruction(guard, patch.address, branch);
  return patch.applied;
}

bool ApplyProjectileTransformPatch(const Core::CPUThreadGuard& guard, ProjectileTransformPatch& patch)
{
  u32 current = 0;
  if (!TryReadU32(guard, patch.address, &current))
    return false;

  const u32 branch = PpcBranch(patch.address, patch.cave);
  if (current == branch)
  {
    patch.applied = true;
    return false;
  }

  if ((current & 0xFFFF0000u) != 0x94210000u)
  {
    patch.applied = false;
    return false;
  }

  patch.original = current;
  constexpr u32 transform_scratch_hi = (0x817FE600u >> 16) & 0xffffu;
  constexpr u32 transform_scratch_lo = 0x817FE600u & 0xffffu;
  constexpr u32 cannon_scratch_hi = (CANNON_BASIS_SCRATCH >> 16) & 0xffffu;
  constexpr u32 cannon_scratch_lo = CANNON_BASIS_SCRATCH & 0xffffu;
  const u32 original_label = patch.cave + 0x80u;
  const u32 return_addr = patch.address + 4u;
  const u32 src = patch.transform_register;

  TryWriteInstruction(guard, patch.cave + 0x00u, 0x3D800000u | transform_scratch_hi);
  TryWriteInstruction(guard, patch.cave + 0x04u, 0x618C0000u | transform_scratch_lo);
  TryWriteInstruction(guard, patch.cave + 0x08u, 0x3D600000u | cannon_scratch_hi);
  TryWriteInstruction(guard, patch.cave + 0x0Cu, 0x616B0000u | cannon_scratch_lo);
  TryWriteInstruction(guard, patch.cave + 0x10u, 0x800B0038u);
  TryWriteInstruction(guard, patch.cave + 0x14u, 0x2C000000u);
  TryWriteInstruction(guard, patch.cave + 0x18u, PpcBeq(patch.cave + 0x18u, original_label));
  TryWriteInstruction(guard, patch.cave + 0x1Cu, 0x800B0000u);
  TryWriteInstruction(guard, patch.cave + 0x20u, 0x900C0000u);
  TryWriteInstruction(guard, patch.cave + 0x24u, 0x800B0004u);
  TryWriteInstruction(guard, patch.cave + 0x28u, 0x900C0004u);
  TryWriteInstruction(guard, patch.cave + 0x2Cu, 0x800B0008u);
  TryWriteInstruction(guard, patch.cave + 0x30u, 0x900C0008u);
  TryWriteInstruction(guard, patch.cave + 0x34u, 0x800B000Cu);
  TryWriteInstruction(guard, patch.cave + 0x38u, 0x900C0010u);
  TryWriteInstruction(guard, patch.cave + 0x3Cu, 0x800B0010u);
  TryWriteInstruction(guard, patch.cave + 0x40u, 0x900C0014u);
  TryWriteInstruction(guard, patch.cave + 0x44u, 0x800B0014u);
  TryWriteInstruction(guard, patch.cave + 0x48u, 0x900C0018u);
  TryWriteInstruction(guard, patch.cave + 0x4Cu, 0x800B0018u);
  TryWriteInstruction(guard, patch.cave + 0x50u, 0x900C0020u);
  TryWriteInstruction(guard, patch.cave + 0x54u, 0x800B001Cu);
  TryWriteInstruction(guard, patch.cave + 0x58u, 0x900C0024u);
  TryWriteInstruction(guard, patch.cave + 0x5Cu, 0x800B0020u);
  TryWriteInstruction(guard, patch.cave + 0x60u, 0x900C0028u);
  TryWriteInstruction(guard, patch.cave + 0x64u, PpcLwzR0(src, 0x0Cu));
  TryWriteInstruction(guard, patch.cave + 0x68u, 0x900C000Cu);
  TryWriteInstruction(guard, patch.cave + 0x6Cu, PpcLwzR0(src, 0x1Cu));
  TryWriteInstruction(guard, patch.cave + 0x70u, 0x900C001Cu);
  TryWriteInstruction(guard, patch.cave + 0x74u, PpcLwzR0(src, 0x2Cu));
  TryWriteInstruction(guard, patch.cave + 0x78u, 0x900C002Cu);
  TryWriteInstruction(guard, patch.cave + 0x7Cu, PpcMr(src, 12));
  TryWriteInstruction(guard, patch.cave + 0x80u, patch.original);
  TryWriteInstruction(guard, patch.cave + 0x84u, PpcBranch(patch.cave + 0x84u, return_addr));
  patch.applied = TryWriteInstruction(guard, patch.address, branch);
  return patch.applied;
}

bool ApplyProjectileTransformPatches(const Core::CPUThreadGuard& guard)
{
  bool wrote = false;
  for (ProjectileTransformPatch& patch : s_projectile_transform_patches)
    wrote = ApplyProjectileTransformPatch(guard, patch) || wrote;
  return wrote;
}

bool ApplyGameFlowFlagPatch(const Core::CPUThreadGuard& guard, GameFlowFlagPatch& patch)
{
  u32 current = 0;
  if (!TryReadU32(guard, patch.address, &current))
    return false;

  const u32 branch = PpcBranch(patch.address, patch.cave);
  if (current == branch)
  {
    patch.applied = true;
    return false;
  }

  patch.original = current;
  constexpr u32 menu_scratch_hi = (GAMEFLOW_MENU_SCRATCH >> 16) & 0xffffu;
  constexpr u32 menu_scratch_lo = GAMEFLOW_MENU_SCRATCH & 0xffffu;
  const u32 return_addr = patch.address + 4u;

  TryWriteInstruction(guard, patch.cave + 0x00u, 0x3D800000u | menu_scratch_hi);
  TryWriteInstruction(guard, patch.cave + 0x04u, 0x618C0000u | menu_scratch_lo);
  TryWriteInstruction(guard, patch.cave + 0x08u, 0x39600000u | (patch.menu_flag & 0xffffu));
  TryWriteInstruction(guard, patch.cave + 0x0Cu, 0x916C0000u);
  TryWriteInstruction(guard, patch.cave + 0x10u, patch.original);
  TryWriteInstruction(guard, patch.cave + 0x14u, PpcBranch(patch.cave + 0x14u, return_addr));
  patch.applied = TryWriteInstruction(guard, patch.address, branch);
  return patch.applied;
}

bool ApplyGameFlowFlagPatches(const Core::CPUThreadGuard& guard)
{
  bool wrote = false;
  for (GameFlowFlagPatch& patch : s_game_flow_flag_patches)
    wrote = ApplyGameFlowFlagPatch(guard, patch) || wrote;
  return wrote;
}

bool ApplyConditionalCombatPitchPatch(const Core::CPUThreadGuard& guard, DynamicPpcPatch& patch)
{
  u32 current = 0;
  if (!TryReadU32(guard, patch.address, &current))
    return false;

  const u32 branch = PpcBranch(patch.address, patch.cave);
  if (current == branch)
  {
    patch.applied = true;
    return false;
  }

  if (current != patch.original && current != patch.replacement)
  {
    patch.applied = false;
    return false;
  }

  const u32 original_label = patch.cave + 0x20u;
  const u32 zero_label = patch.cave + 0x28u;
  const u32 return_addr = patch.address + 4u;
  TryWriteInstruction(guard, patch.cave + 0x00u, 0x3D808045u);
  TryWriteInstruction(guard, patch.cave + 0x04u, 0x618CA1A8u);
  TryWriteInstruction(guard, patch.cave + 0x08u, 0x818C084Cu);
  TryWriteInstruction(guard, patch.cave + 0x0Cu, 0x2C0C0000u);
  TryWriteInstruction(guard, patch.cave + 0x10u, PpcBeq(patch.cave + 0x10u, original_label));
  TryWriteInstruction(guard, patch.cave + 0x14u, 0x818C0330u);
  TryWriteInstruction(guard, patch.cave + 0x18u, 0x2C0C0000u);
  TryWriteInstruction(guard, patch.cave + 0x1Cu, PpcBeq(patch.cave + 0x1Cu, zero_label));
  TryWriteInstruction(guard, patch.cave + 0x20u, patch.original);
  TryWriteInstruction(guard, patch.cave + 0x24u, PpcBranch(patch.cave + 0x24u, return_addr));
  TryWriteInstruction(guard, patch.cave + 0x28u, patch.replacement);
  TryWriteInstruction(guard, patch.cave + 0x2Cu, PpcBranch(patch.cave + 0x2Cu, return_addr));
  patch.applied = TryWriteInstruction(guard, patch.address, branch);
  return patch.applied;
}

bool ApplyConditionalElevationPitchPatch(const Core::CPUThreadGuard& guard, DynamicPpcPatch& patch)
{
  u32 current = 0;
  if (!TryReadU32(guard, patch.address, &current))
    return false;

  const u32 branch = PpcBranch(patch.address, patch.cave);
  if (current == branch)
  {
    patch.applied = true;
    return false;
  }

  if (current != patch.original)
  {
    patch.applied = false;
    return false;
  }

  const u32 original_label = patch.cave + 0x20u;
  const u32 zero_label = patch.cave + 0x28u;
  const u32 return_addr = patch.address + 4u;
  TryWriteInstruction(guard, patch.cave + 0x00u, 0x3D808045u);
  TryWriteInstruction(guard, patch.cave + 0x04u, 0x618CA1A8u);
  TryWriteInstruction(guard, patch.cave + 0x08u, 0x818C084Cu);
  TryWriteInstruction(guard, patch.cave + 0x0Cu, 0x2C0C0000u);
  TryWriteInstruction(guard, patch.cave + 0x10u, PpcBeq(patch.cave + 0x10u, original_label));
  TryWriteInstruction(guard, patch.cave + 0x14u, 0x816C0330u);
  TryWriteInstruction(guard, patch.cave + 0x18u, 0x2C0B0000u);
  TryWriteInstruction(guard, patch.cave + 0x1Cu, PpcBeq(patch.cave + 0x1Cu, zero_label));
  TryWriteInstruction(guard, patch.cave + 0x20u, patch.original);
  TryWriteInstruction(guard, patch.cave + 0x24u, PpcBranch(patch.cave + 0x24u, return_addr));
  TryWriteInstruction(guard, patch.cave + 0x28u, 0xC00280B0u);
  TryWriteInstruction(guard, patch.cave + 0x2Cu, patch.original);
  TryWriteInstruction(guard, patch.cave + 0x30u, PpcBranch(patch.cave + 0x30u, return_addr));
  patch.applied = TryWriteInstruction(guard, patch.address, branch);
  return patch.applied;
}

bool ApplyCombatPitchPatches(const Core::CPUThreadGuard& guard)
{
  bool wrote = false;
  wrote = ApplyGameFlowFlagPatches(guard) || wrote;
  wrote = ApplyRenderModelOffsetPatch(guard) || wrote;
  wrote = ApplyFirstPersonPitchLoadPatch(guard) || wrote;

  for (DynamicPpcPatch& patch : s_combat_pitch_patches)
  {
    wrote = ApplyConditionalCombatPitchPatch(guard, patch) || wrote;
  }

  wrote = ApplyConditionalElevationPitchPatch(guard, s_combat_elevation_pitch_patch) || wrote;
  return wrote;
}

void ApplyBuiltinPatches(Core::System& system, const Core::CPUThreadGuard& guard)
{
  std::call_once(s_parse_builtin_patches_once, ParseBuiltinPatches);
  const RuntimeSettings settings = GetRuntimeSettings();

  bool wrote_instruction = false;
  for (const PatchWrite& patch : s_builtin_patches)
  {
    if (!PatchGroupEnabled(settings, patch.group))
      continue;

    const auto current = PowerPC::MMU::HostTryRead<u32>(guard, patch.address);
    if (current && current->value == patch.value)
      continue;

    if (!PowerPC::MMU::HostTryWrite<u32>(guard, patch.value, patch.address))
      continue;

    wrote_instruction = true;
  }

  if (wrote_instruction)
    system.GetJitInterface().InvalidateICache(0x80000000u, 0x00200000u, true);
}
}  // namespace

void OnFrameEnd(Core::System& system, const Core::CPUThreadGuard& guard)
{
  ++s_frame_counter;

  const RuntimeSettings settings = GetRuntimeSettings();
  if (!settings.enabled)
  {
    s_gameplay_input_active.store(false, std::memory_order_relaxed);
    s_orbit_lock_active.store(false, std::memory_order_relaxed);
    return;
  }

  const bool game_active = Core::IsRunning(system) && IsMetroidPrimeRev0(guard);
  if (!game_active)
  {
    s_gameplay_input_active.store(false, std::memory_order_relaxed);
    s_orbit_lock_active.store(false, std::memory_order_relaxed);
    if (s_game_was_active)
      ResetNativeRuntime();
    s_game_was_active = false;
    return;
  }

  if (!s_game_was_active)
    TryWriteU32(guard, GAMEFLOW_MENU_SCRATCH, 0);
  s_game_was_active = true;

  bool dynamic_patch_applied = false;
  if (settings.builtin_patches_enabled)
  {
    dynamic_patch_applied = ApplyCombatPitchPatches(guard) || dynamic_patch_applied;
    if (settings.patch_beam_projectile_timing)
      dynamic_patch_applied = ApplyProjectileTransformPatches(guard) || dynamic_patch_applied;
  }
  if (dynamic_patch_applied)
    system.GetJitInterface().InvalidateICache(0x80000000u, 0x00200000u, true);

  // Recheck occasionally so user/loaded-state changes cannot silently undo PrimeGun patches.
  if (settings.builtin_patches_enabled &&
      (!s_patches_applied_this_boot || (s_frame_counter % 60) == 0))
  {
    ApplyBuiltinPatches(system, guard);
    s_patches_applied_this_boot = true;
  }

  if ((s_frame_counter % 60) == 1)
    ApplyHelmetOpacityZero(guard);

  u32 player = 0;
  const bool have_player =
      TryReadU32(guard, ADDRESS.state_manager + ADDRESS.player_offset, &player) &&
      player >= 0x80000000u;
  const bool gameplay_input_active = have_player && PlayerIsFirstPersonUnmorphed(guard, player);
  const bool orbit_lock_active = gameplay_input_active && OrbitLockButtonHeld(guard, ADDRESS.state_manager);
  s_gameplay_input_active.store(gameplay_input_active, std::memory_order_relaxed);
  s_orbit_lock_active.store(orbit_lock_active, std::memory_order_relaxed);
  if (have_player)
    LogModeProbe(guard, player, gameplay_input_active != s_last_logged_gameplay_input_active);
  if (!s_have_logged_gameplay_input_active ||
      gameplay_input_active != s_last_logged_gameplay_input_active)
  {
    u32 camera_state = 0xffffffffu;
    u32 morph_state = 0xffffffffu;
    u32 movement_state = 0xffffffffu;
    u32 visor_state = 0xffffffffu;
    u32 holster_state = 0xffffffffu;
    u8 input_flags = 0xffu;
    float gun_alpha = -1.0f;
    if (have_player)
    {
      TryReadU32(guard, player + 0x2F4u, &camera_state);
      TryReadU32(guard, player + 0x2F8u, &morph_state);
      TryReadU32(guard, player + PLAYER_MOVEMENT_STATE_OFFSET, &movement_state);
      TryReadU32(guard, player + PLAYER_VISOR_STATE_OFFSET, &visor_state);
      TryReadU32(guard, player + 0x498u, &holster_state);
      TryReadU8(guard, player + PLAYER_DISABLE_INPUT_FLAGS_OFFSET, &input_flags);
      TryReadFloat(guard, player + 0x494u, &gun_alpha);
    }
    NOTICE_LOG_FMT(CORE,
                   "PrimeGun gameplay_input={} player={:08X} camera={} morph={} "
                   "input_flags={:02X} move={} visor={} gun_alpha={:.3f} holster={}",
                   gameplay_input_active, player, camera_state, morph_state, input_flags,
                   movement_state, visor_state, gun_alpha, holster_state);
    s_have_logged_gameplay_input_active = true;
    s_last_logged_gameplay_input_active = gameplay_input_active;
  }

  UpdateCannonTracking(guard);
}

bool IsGameplayInputActive()
{
  return s_gameplay_input_active.load(std::memory_order_relaxed);
}

bool IsOrbitLockActive()
{
  return s_orbit_lock_active.load(std::memory_order_relaxed);
}

void ResetNativeRuntime()
{
  s_patches_applied_this_boot = false;
  s_frame_counter = 0;
  s_scan_was_active = false;
  s_scan_last_player = 0;
  s_scan_normal_frames = 0;
  s_smooth_scan_pitch = 0.0f;
  s_translation_base_valid = false;
  s_dpad_forced_input_disabled = false;
  s_dpad_input_was_disabled = false;
  s_dpad_input_flags_addr = 0;
  s_dpad_last_disable_refresh_frame = 0;
  s_directional_move_speed = 0.0f;
  s_gameplay_input_active.store(false, std::memory_order_relaxed);
  s_orbit_lock_active.store(false, std::memory_order_relaxed);
  s_last_logged_gameplay_input_active = false;
  s_have_logged_gameplay_input_active = false;
  s_last_mode_probe_frame = 0;
  s_last_mode_probe_camera_state = 0xffffffffu;
  s_last_mode_probe_morph_state = 0xffffffffu;
  s_last_mode_probe_movement_state = 0xffffffffu;
  s_last_mode_probe_visor_state = 0xffffffffu;
  s_last_mode_probe_holster_state = 0xffffffffu;
  s_last_mode_probe_input_flags = 0xffu;
  s_last_mode_probe_gun_alpha = -1.0f;
  s_last_mode_probe_state_words = {};
  s_last_mode_probe_game_words = {};
  s_have_mode_probe_words = false;
  s_last_validated_gun = 0;
  s_smooth_matrix_valid = false;
  s_controller_base_x = 0.0f;
  s_controller_base_y = 0.0f;
  s_controller_base_z = 0.0f;
  s_last_height_reset_thumbstick = false;
  s_last_vr_menu_thumbstick = false;
  s_last_vr_menu_trigger = false;
  s_last_vr_menu_primary = false;
  s_last_vr_menu_secondary = false;
  s_last_vr_menu_stick_up = false;
  s_last_vr_menu_stick_down = false;
  s_last_vr_menu_stick_left = false;
  s_last_vr_menu_stick_right = false;
  s_vr_menu_visible = false;
  s_vr_menu_tab = 0;
  s_vr_menu_selected_index = 0;
  ++s_vr_menu_generation;
  s_vr_menu_saved_notice_until_frame = 0;
  s_vr_cannon_texture_notice_until_frame = 0;
  s_height_prompt_until_frame = 0;
  s_prompt_gameplay_ready_since_frame = 0;
  s_prompt_first_ready_timeout_frame = 0;
  s_prompt_skipped_first_ready = false;
  s_prompt_waiting_for_second_ready = false;
#ifdef ENABLE_VR
  Common::VR::OpenXRInputState::SetPrimeGunOverlay({});
#endif
  s_first_person_pitch_load_patch.applied = false;
  s_render_model_offset_patch.applied = false;
  s_render_model_offset_patch.original = 0;
  for (DynamicPpcPatch& patch : s_combat_pitch_patches)
    patch.applied = false;
  s_combat_elevation_pitch_patch.applied = false;
  for (ProjectileTransformPatch& patch : s_projectile_transform_patches)
  {
    patch.applied = false;
    patch.original = 0;
  }
  for (GameFlowFlagPatch& patch : s_game_flow_flag_patches)
  {
    patch.applied = false;
    patch.original = 0;
  }
  s_smooth_matrix[0] = 1.0f;
  s_smooth_matrix[1] = 0.0f;
  s_smooth_matrix[2] = 0.0f;
  s_smooth_matrix[3] = 0.0f;
  s_smooth_matrix[4] = 0.0f;
  s_smooth_matrix[5] = 1.0f;
  s_smooth_matrix[6] = 0.0f;
  s_smooth_matrix[7] = 0.0f;
  s_smooth_matrix[8] = 0.0f;
  s_smooth_matrix[9] = 0.0f;
  s_smooth_matrix[10] = 1.0f;
  s_smooth_matrix[11] = 0.0f;
}

RuntimeSettings GetRuntimeSettings()
{
  std::lock_guard lock{s_settings_mutex};
  return s_settings;
}

void SetRuntimeSettings(const RuntimeSettings& settings)
{
  std::lock_guard lock{s_settings_mutex};
  s_settings = settings;
}

void ResetCalibrationOffsets()
{
  std::lock_guard lock{s_settings_mutex};
  s_settings.offset_x = 0.0f;
  s_settings.offset_y = 0.0f;
  s_settings.offset_z = 0.0f;
  s_settings.model_offset_x = DEFAULT_MODEL_OFFSET_X;
  s_settings.model_offset_y = DEFAULT_MODEL_OFFSET_Y;
  s_settings.model_offset_z = DEFAULT_MODEL_OFFSET_Z;
  s_settings.rot_offset_x = DEFAULT_ROT_OFFSET_X;
  s_settings.rot_offset_y = DEFAULT_ROT_OFFSET_Y;
  s_settings.rot_offset_z = DEFAULT_ROT_OFFSET_Z;
}

void ApplySamusArmPreset()
{
  std::lock_guard lock{s_settings_mutex};
  s_settings.offset_x = 0.0f;
  s_settings.offset_y = 0.0f;
  s_settings.offset_z = 0.0f;
  s_settings.model_offset_x = 0.0f;
  s_settings.model_offset_y = -0.300f;
  s_settings.model_offset_z = 0.0f;
  s_settings.rot_offset_x = 0.0f;
  s_settings.rot_offset_y = 20.0f;
  s_settings.rot_offset_z = -90.0f;
}

bool ConsumeVrSettingsSaveRequest()
{
  std::lock_guard lock{s_settings_mutex};
  const bool requested = s_vr_settings_save_requested;
  s_vr_settings_save_requested = false;
  return requested;
}

void MarkVrSettingsSaved()
{
  std::lock_guard lock{s_settings_mutex};
  s_vr_menu_saved_notice_until_frame = s_frame_counter + 180;
  ++s_vr_menu_generation;
}
}  // namespace PrimeGun
