// Copyright 2026 PrimedGun Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PrimedGun/NativeRuntime.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/MathUtil.h"
#ifdef ENABLE_VR
#include "Common/VR/OpenXRInputState.h"
#include "VideoCommon/VR/OpenXRManager.h"
#endif

#include "Core/Config/GraphicsSettings.h"
#include "Core/Core.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/MMU.h"
#include "Core/System.h"

#include "Core/PrimedGun/PrimedGunBuiltinPatches.inc"

#include "VideoCommon/AsyncRequests.h"
#include "VideoCommon/HiresTextures.h"
#include "VideoCommon/ShaderHunter.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/VideoConfig.h"

namespace PrimedGun
{
namespace
{
struct PatchWrite
{
  u32 address = 0;
  u32 value = 0;
  u32 group = 0;
};

struct HookWrite
{
  u32 offset = 0;
  u32 value = 0;
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

Pose PoseFromOpenXR(const Common::VR::OpenXRPoseState& xr_pose);
bool LeftControllerNearHead(const Pose& left, const Pose& hmd, const RuntimeSettings& settings);

bool PoseNumbersLookValid(const Pose& pose)
{
  if (!pose.valid || !std::isfinite(pose.px) || !std::isfinite(pose.py) ||
      !std::isfinite(pose.pz) || !std::isfinite(pose.qx) || !std::isfinite(pose.qy) ||
      !std::isfinite(pose.qz) || !std::isfinite(pose.qw))
  {
    return false;
  }

  const float quat_len_sq =
      pose.qx * pose.qx + pose.qy * pose.qy + pose.qz * pose.qz + pose.qw * pose.qw;
  return std::isfinite(quat_len_sq) && quat_len_sq >= 0.25f && quat_len_sq <= 4.0f;
}

bool MatrixNumbersLookValid(const Matrix3x4& mat)
{
  for (float value : mat.m)
  {
    if (!std::isfinite(value))
      return false;
  }

  return true;
}

float ClampFinite(float value, float fallback, float min_value, float max_value)
{
  if (!std::isfinite(value))
    return fallback;
  return std::clamp(value, min_value, max_value);
}

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

constexpr u32 MEM1_BASE = 0x80000000u;
constexpr u32 MEM1_END = 0x81800000u;
constexpr u32 PATCH_CODE_ARENA_BASE = 0x80001C00u;
constexpr u32 PATCH_CODE_ARENA_SIZE = 0xC00u;
constexpr u32 SCRATCH_BASE = 0x80002800u;
constexpr u32 SCRATCH_ARENA_SIZE = 0x800u;
constexpr u32 CANNON_BASIS_SCRATCH = SCRATCH_BASE + 0x000u;
constexpr u32 CANNON_EXPECTED_GUN_SCRATCH = SCRATCH_BASE + 0x038u;
constexpr u32 MODEL_OFFSET_WORLD_SCRATCH = SCRATCH_BASE + 0x040u;
constexpr u32 ADJUSTED_GUN_POS_SCRATCH = SCRATCH_BASE + 0x050u;
constexpr u32 AUDIO_LISTENER_CAVE = SCRATCH_BASE + 0x080u;
constexpr u32 GUN_TARGET_SCRATCH = SCRATCH_BASE + 0x400u;
constexpr u32 RETICLE_BILLBOARD_SCRATCH = SCRATCH_BASE + 0x500u;
constexpr u32 SCAN_RETICLE_TRACE_SCRATCH = SCRATCH_BASE + 0x540u;
constexpr u32 PITCH_ZERO_ENABLE_SCRATCH = SCRATCH_BASE + 0x5A0u;
constexpr u32 DPAD_PRESS_SCRATCH = SCRATCH_BASE + 0x680u;
constexpr u32 DPAD_DISABLE_OWNER_SCRATCH = SCRATCH_BASE + 0x684u;
constexpr u32 DPAD_DISABLE_FLAGS_ADDR_SCRATCH = SCRATCH_BASE + 0x688u;
constexpr u32 DPAD_DISABLE_ORIGINAL_FLAGS_SCRATCH = SCRATCH_BASE + 0x68Cu;
constexpr u32 GAMEFLOW_MENU_SCRATCH = SCRATCH_BASE + 0x690u;
constexpr u32 MORPHBALL_CAMERA_LEVEL_ENABLE_SCRATCH = SCRATCH_BASE + 0x694u;
constexpr u32 FIRST_PERSON_ORBIT_AIM_VECTOR_ENABLE_SCRATCH = SCRATCH_BASE + 0x698u;
constexpr u32 AUDIO_LISTENER_SCRATCH = SCRATCH_BASE + 0x700u;
constexpr u32 DPAD_DISABLE_OWNER_MAGIC = 0x50474450u;  // PGDP

constexpr u32 FIRST_PERSON_PITCH_LOAD_CAVE = PATCH_CODE_ARENA_BASE + 0x000u;
constexpr u32 RENDER_MODEL_OFFSET_CAVE = PATCH_CODE_ARENA_BASE + 0x080u;
constexpr u32 FIRST_PERSON_ELEVATION_PITCH_CAVE = PATCH_CODE_ARENA_BASE + 0x100u;
constexpr u32 COMBAT_PITCH_CAVE_0 = PATCH_CODE_ARENA_BASE + 0x140u;
constexpr u32 COMBAT_PITCH_CAVE_1 = PATCH_CODE_ARENA_BASE + 0x180u;
constexpr u32 COMBAT_PITCH_CAVE_2 = PATCH_CODE_ARENA_BASE + 0x1C0u;
constexpr u32 COMBAT_ELEVATION_PITCH_CAVE = PATCH_CODE_ARENA_BASE + 0x200u;
constexpr u32 WAVE_PROJECTILE_TRANSFORM_CAVE = PATCH_CODE_ARENA_BASE + 0x280u;
constexpr u32 MISSILE_PROJECTILE_TRANSFORM_CAVE = PATCH_CODE_ARENA_BASE + 0x340u;
constexpr u32 GAMEFLOW_UNPAUSE_CAVE = PATCH_CODE_ARENA_BASE + 0x400u;
constexpr u32 GAMEFLOW_MESSAGE_CAVE = PATCH_CODE_ARENA_BASE + 0x430u;
constexpr u32 GAMEFLOW_SAVE_CAVE = PATCH_CODE_ARENA_BASE + 0x460u;
constexpr u32 GAMEFLOW_LOGBOOK_CAVE = PATCH_CODE_ARENA_BASE + 0x490u;
constexpr u32 GAMEFLOW_PAUSE_CAVE = PATCH_CODE_ARENA_BASE + 0x4C0u;
constexpr u32 GAMEFLOW_MAP_CAVE = PATCH_CODE_ARENA_BASE + 0x4F0u;
constexpr u32 SCAN_RETICLE_TRACE_CAVE = PATCH_CODE_ARENA_BASE + 0x540u;
constexpr u32 SCAN_RETICLE_TRACE_CURR_CAVE = PATCH_CODE_ARENA_BASE + 0x580u;
constexpr u32 SCAN_INDICATOR_UPDATE_TRACE_CAVE = PATCH_CODE_ARENA_BASE + 0x5C0u;
constexpr u32 SCAN_INDICATOR_VIEW_BASIS_CAVE = PATCH_CODE_ARENA_BASE + 0x600u;
constexpr u32 FIRST_PERSON_ORBIT_AIM_VECTOR_CAVE = PATCH_CODE_ARENA_BASE + 0x6C0u;
constexpr u32 LEGACY_MORPHBALL_CAMERA_RETURN_ADDRESS = 0x8000A9B4u;
constexpr u32 LEGACY_MORPHBALL_CAMERA_RETURN_CAVE = PATCH_CODE_ARENA_BASE + 0x680u;
constexpr u32 LEGACY_MORPHBALL_CAMERA_RETURN_ORIGINAL = 0x80010054u;
constexpr u32 BALL_CAMERA_LEVEL_PATCH_ADDRESS = 0x800830A0u;
constexpr u32 BALL_CAMERA_LEVEL_PATCH_ORIGINAL = 0x387F0034u;
constexpr u32 BALL_CAMERA_LEVEL_CAVE = PATCH_CODE_ARENA_BASE + 0xB40u;
constexpr u32 INTERPOLATION_CAMERA_LEVEL_PATCH_ADDRESS = 0x8026529Cu;
constexpr u32 INTERPOLATION_CAMERA_LEVEL_PATCH_ORIGINAL = 0x887E00E4u;
constexpr u32 INTERPOLATION_CAMERA_LEVEL_CAVE = PATCH_CODE_ARENA_BASE + 0xB80u;
constexpr u32 AUDIO_LISTENER_PATCH_ADDRESS = 0x8000BA88u;
constexpr u32 AUDIO_LISTENER_PATCH_ORIGINAL = 0xD1010008u;
constexpr u32 AUDIO_LISTENER_LEGACY_BAD_CAVE = 0x80002300u;
constexpr u32 AUDIO_LISTENER_LEGACY_HIGH_CAVE = 0x817F8000u;
constexpr u32 FIRST_PERSON_ORBIT_AIM_VECTOR_ADDRESS = 0x8000E71Cu;
constexpr u32 FIRST_PERSON_ORBIT_AIM_VECTOR_ORIGINAL = 0x801E0304u;
constexpr u32 FIRST_PERSON_ORBIT_AIM_VECTOR_SKIP_ADDRESS = 0x8000E9C4u;
constexpr u32 PPC_BLR = 0x4E800020u;
constexpr u32 LOAD_ZERO_TO_F1 = 0xC02280B0u;
constexpr u32 LOAD_ZERO_TO_F31 = 0xC3E280B0u;
constexpr u32 DRAW_NEXT_LOCK_ON_GROUP = 0x800BD808u;
constexpr u32 DRAW_CURR_LOCK_ON_GROUP = 0x800BE25Cu;
constexpr u32 UPDATE_SCAN_OBJECT_INDICATORS = 0x80112508u;
constexpr u32 DRAW_SCAN_INDICATOR_MODEL_BASIS = 0x801122CCu;
constexpr u32 DRAW_SCAN_INDICATOR_MODEL_BASIS_ORIGINAL = 0xC0410074u;

constexpr u32 VR_MENU_TAB_COUNT = 6;
constexpr u32 VR_MENU_LAYOUT_TAB = 0;
constexpr u32 VR_MENU_CALIBRATION_TAB = 1;
constexpr float VR_MENU_TEXTURE_WIDTH = 1024.0f;
constexpr float VR_MENU_TEXTURE_HEIGHT = 512.0f;
constexpr u32 VR_MENU_CALIBRATION_FIRST_PAGE_ITEMS = 10;
constexpr u32 VR_MENU_CALIBRATION_TOTAL_ITEMS = 20;
constexpr u32 VR_MENU_CALIBRATION_PAGE_COUNT = 2;
constexpr u32 VR_MENU_CONTROL_TAB = 2;
constexpr u32 VR_MENU_MOVEMENT_TAB = 3;
constexpr u32 VR_MENU_CANNON_TAB = 4;
constexpr u32 VR_MENU_STATE_TAB = 5;
constexpr u32 VR_MENU_CONTROL_FIRST_PAGE_ITEMS = 8;
constexpr u32 VR_MENU_CONTROL_TOTAL_ITEMS = 17;
constexpr u32 VR_MENU_CONTROL_PAGE_COUNT = 2;
constexpr std::array<int, 4> SNAP_TURN_DEGREES_CHOICES = {30, 45, 60, 90};
constexpr float VR_MENU_ROW_TEXT_Y = 146.0f;
constexpr float VR_MENU_ROW_STEP_Y = 22.0f;
constexpr float VR_MENU_ROW_HIT_HALF_HEIGHT = 13.0f;
constexpr float VR_MENU_STATE_ROW_GAP_Y = 18.0f;
constexpr u64 VR_MENU_STATE_CONFIRM_FRAMES = 360;
constexpr u64 VR_MENU_RESET_CONFIRM_FRAMES = 360;
constexpr u64 VR_MENU_LONG_PRESS_FRAMES = 60;
constexpr u32 VR_MENU_STATE_SLOT_COUNT = 10;
constexpr u32 VR_MENU_STATE_ACTION_LOAD = 1;
constexpr u32 VR_MENU_STATE_ACTION_SAVE = 2;
constexpr u32 VR_MENU_STATE_ACTION_LOAD_NEWEST = 3;
constexpr u32 VR_MENU_STATE_ACTION_SAVE_OLDEST = 4;
constexpr u32 VR_MENU_STATE_ACTION_ROW_COUNT = 4;
constexpr u32 VR_MENU_RESET_ALL_ACTION = 1;
constexpr u32 VR_MENU_RESET_TARGETING_ACTION = 2;
constexpr u32 VR_MENU_RESET_CALIBRATION_ACTION = 3;
constexpr u32 VR_MENU_RESET_CONTROLLER_ACTION = 4;
constexpr u32 VR_MENU_RESET_MOVEMENT_ACTION = 5;
constexpr const char* PRIMEGUN_CANNON_GAME_ID = "GM8E01";
constexpr const char* PRIMEGUN_CANNON_PACK_FOLDER = "000_PrimedGunCannon";
constexpr const char* PRIMEGUN_CANNON_LIBRARY_FOLDER = "PrimedGun" DIR_SEP "CannonTextures";
constexpr std::array<const char*, 3> PRIMEGUN_CANNON_TEXTURE_NAMES = {
    "tex1_128x128_m_3c6ded49d64d30f2_14",
    "tex1_128x128_m_bec6d78ea7dd739e_14",
    "tex1_64x64_m_c7625e7ecd9cd5c2_14",
};
constexpr bool ENABLE_PRIMEDGUN_RUNTIME_LOGGING = false;
constexpr bool ENABLE_PRIMEDGUN_LOCK_LOGGING = false;

constexpr u32 FINAL_INPUT_OFFSET = 0xB54u;
constexpr u32 FINAL_INPUT_RIGHT_STICK_X = FINAL_INPUT_OFFSET + 0x10u;
constexpr u32 FINAL_INPUT_RIGHT_STICK_Y = FINAL_INPUT_OFFSET + 0x14u;
constexpr u32 FINAL_INPUT_RIGHT_STICK_X_PRESS = FINAL_INPUT_OFFSET + 0x22u;
constexpr u32 FINAL_INPUT_RIGHT_STICK_Y_PRESS = FINAL_INPUT_OFFSET + 0x23u;
constexpr u32 FINAL_INPUT_DPAD_HELD_0 = FINAL_INPUT_OFFSET + 0x2Cu;
constexpr u32 FINAL_INPUT_DPAD_HELD_1 = FINAL_INPUT_OFFSET + 0x2Du;
constexpr u32 STATE_MANAGER_PLAYER_STATE_OFFSET = 0x8B8u;
constexpr u32 STATE_MANAGER_WORLD_TRANS_MANAGER_REF_OFFSET = 0x8C4u;
constexpr u32 RSTL_REF_DATA_OBJECT_OFFSET = 0x00u;
constexpr u32 WORLD_TRANS_MANAGER_TYPE_OFFSET = 0x30u;
constexpr u32 WORLD_TRANS_MANAGER_TYPE_ELEVATOR = 1u;
constexpr u64 CINEMATIC_SCREEN_SIGNAL_LOSS_GRACE_FRAMES = 10u;
constexpr u32 PLAYER_STATE_CURRENT_VISOR_OFFSET = 0x14u;
constexpr u32 PLAYER_STATE_TRANSITION_VISOR_OFFSET = 0x18u;
constexpr u32 PLAYER_STATE_MAX_VISOR = 3u;
constexpr u32 PLAYER_STATE_SCAN_VISOR = 2u;
constexpr u32 FINAL_INPUT_DPAD_PRESSED_0 = FINAL_INPUT_OFFSET + 0x2Eu;
constexpr u32 PLAYER_DISABLE_INPUT_FLAGS_OFFSET = 0x9C6u;
constexpr u8 PLAYER_DISABLE_INPUT_MASK = 0x04u;
constexpr u32 PLAYER_VELOCITY_OFFSET = 0x138u;
constexpr u32 PLAYER_MOVEMENT_STATE_OFFSET = 0x258u;
constexpr u32 PLAYER_ORBIT_STATE_OFFSET = 0x304u;
constexpr u32 PLAYER_ORBIT_TARGET_ID_OFFSET = 0x310u;
constexpr u32 PLAYER_ORBIT_LOCK_ID_OFFSET = 0x33Cu;
constexpr u32 PLAYER_NEARBY_ORBIT_OBJECTS_OFFSET = 0x344u;
constexpr u32 PLAYER_ONSCREEN_ORBIT_OBJECTS_OFFSET = 0x354u;
constexpr u32 PLAYER_FREE_LOOK_STATE_OFFSET = 0x3DCu;
constexpr u32 PLAYER_AIM_TARGET_ID_OFFSET = 0x3F4u;
constexpr u32 PLAYER_FREE_LOOK_CENTER_TIME_OFFSET = 0x3E0u;
constexpr u32 PLAYER_FREE_LOOK_YAW_ANGLE_OFFSET = 0x3E4u;
constexpr u32 PLAYER_FREE_LOOK_YAW_VEL_OFFSET = 0x3E8u;
constexpr u32 PLAYER_FREE_LOOK_PITCH_ANGLE_OFFSET = 0x3ECu;
constexpr u32 PLAYER_VISOR_STATE_OFFSET = 0x330u;
constexpr u32 PLAYER_VISOR_SCAN_TARGETS_OFFSET = 0x13Cu;
constexpr u32 PLAYER_VISOR_SCAN_TARGET_DATA_OFFSET = PLAYER_VISOR_SCAN_TARGETS_OFFSET + 4u;
constexpr u32 PLAYER_VISOR_SCAN_TARGET_SLOT_SIZE = 0x10u;
constexpr u32 PLAYER_VISOR_SCAN_TARGET_CAPACITY = 64u;
constexpr u32 PLAYER_VISOR_SCAN_FRAME_COLOR_INTERP_OFFSET = 0x54Cu;
constexpr u32 PLAYER_VISOR_SCAN_FRAME_COLOR_IMPULSE_OFFSET = 0x550u;
constexpr float PLAYER_VISOR_SCAN_TARGET_VALID_TIMER = 1.0f;
constexpr u32 GP_GAME_STATE = 0x805A8C40u;
constexpr u32 GAME_OPTIONS_HELMET_ALPHA_OFFSET = 0x17Cu + 0x64u;
constexpr u64 GAMEPLAY_INPUT_LOSS_HOLD_FRAMES = 18u;

bool RuntimeLoggingEnabled()
{
  return ENABLE_PRIMEDGUN_RUNTIME_LOGGING;
}

bool LockLoggingEnabled()
{
  return ENABLE_PRIMEDGUN_LOCK_LOGGING;
}

void AppendScanDebugLine(std::string_view line)
{
  if (!RuntimeLoggingEnabled())
    return;

  File::CreateFullPath(File::GetUserPath(D_LOGS_IDX));
  File::IOFile file(File::GetUserPath(D_LOGS_IDX) + "PrimedGunScan.log", "ab");
  if (!file)
    return;

  file.WriteBytes(line.data(), line.size());
  static constexpr char newline = '\n';
  file.WriteBytes(&newline, 1);
}

void AppendLockDebugLine(std::string_view line)
{
  if (!LockLoggingEnabled())
    return;

  File::CreateFullPath(File::GetUserPath(D_LOGS_IDX));
  File::IOFile file(File::GetUserPath(D_LOGS_IDX) + "PrimedGunLock.log", "ab");
  if (!file)
    return;

  file.WriteBytes(line.data(), line.size());
  static constexpr char newline = '\n';
  file.WriteBytes(&newline, 1);
}

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
bool s_cinematic_screen_active = false;
u64 s_cinematic_screen_hold_until_frame = 0;
u32 s_cinematic_screen_generation = 0;
bool s_snap_turn_ready = true;
u64 s_snap_turn_cooldown_until_frame = 0;
u32 s_vr_menu_tab = 0;
u32 s_vr_menu_selected_index = 0;
u32 s_vr_menu_calibration_page = 0;
u32 s_vr_menu_control_page = 0;
u32 s_vr_menu_generation = 1;
u64 s_vr_menu_saved_notice_until_frame = 0;
u64 s_vr_menu_input_suppress_until_frame = 0;
u64 s_vr_menu_long_press_start_frame = 0;
bool s_vr_menu_long_press_consumed = false;
u32 s_vr_cannon_texture_slot = 0;
u64 s_vr_cannon_texture_notice_until_frame = 0;
bool s_vr_settings_save_requested = false;
std::atomic_bool s_vr_state_load_requested{false};
std::atomic_bool s_vr_state_save_requested{false};
std::atomic_bool s_vr_state_load_newest_requested{false};
std::atomic_bool s_vr_state_save_oldest_requested{false};
std::atomic_int s_vr_state_slot_select_requested{0};
std::atomic_int s_vr_state_slot_from_ui{0};
u32 s_vr_state_slot = 1;
u32 s_vr_state_confirm_action = 0;
u64 s_vr_state_confirm_until_frame = 0;
u32 s_vr_reset_confirm_action = 0;
u64 s_vr_reset_confirm_until_frame = 0;
u64 s_height_prompt_until_frame = 0;
u64 s_prompt_gameplay_ready_since_frame = 0;
u64 s_prompt_first_ready_timeout_frame = 0;
bool s_prompt_skipped_first_ready = false;
bool s_prompt_waiting_for_second_ready = false;
bool s_prompt_shown_this_session = false;
bool s_dpad_forced_input_disabled = false;
bool s_dpad_input_was_disabled = false;
u32 s_dpad_input_player = 0;
u32 s_dpad_input_flags_addr = 0;
u64 s_dpad_last_disable_refresh_frame = 0;
float s_directional_move_speed = 0.0f;
std::atomic_bool s_gameplay_input_active{false};
std::atomic_bool s_orbit_lock_active{false};
u64 s_gameplay_input_hold_until_frame = 0;
bool s_last_logged_gameplay_input_active = false;
bool s_have_logged_gameplay_input_active = false;
u64 s_last_mode_probe_frame = 0;
u64 s_last_camera_watchdog_frame = 0;
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
u64 s_last_scan_reticle_trace_frame = 0;
bool s_have_dumped_scan_indicator_code = false;
u32 s_last_validated_gun = 0;
u32 s_last_patch_player = 0;
u64 s_patch_reapply_until_frame = 0;
u64 s_last_cannon_feed_watchdog_frame = 0;
u32 s_cannon_feed_stall_frames = 0;
u64 s_last_scan_reticle_watchdog_frame = 0;
u32 s_scan_reticle_bad_samples = 0;
bool s_cannon_hand_pose_ready = false;
bool s_smooth_matrix_valid = false;
u64 s_cannon_smoothing_bypass_until_frame = 0;
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

DynamicPpcPatch s_scan_reticle_trace_patch{
    DRAW_NEXT_LOCK_ON_GROUP, 0, 0, SCAN_RETICLE_TRACE_CAVE, false};
DynamicPpcPatch s_scan_reticle_trace_curr_patch{
    DRAW_CURR_LOCK_ON_GROUP, 0, 0, SCAN_RETICLE_TRACE_CURR_CAVE, false};
DynamicPpcPatch s_scan_indicator_update_trace_patch{
    UPDATE_SCAN_OBJECT_INDICATORS, 0, 0, SCAN_INDICATOR_UPDATE_TRACE_CAVE, false};
DynamicPpcPatch s_scan_indicator_view_basis_patch{
    DRAW_SCAN_INDICATOR_MODEL_BASIS, DRAW_SCAN_INDICATOR_MODEL_BASIS_ORIGINAL, 0,
    SCAN_INDICATOR_VIEW_BASIS_CAVE, false};

DynamicPpcPatch s_ball_camera_level_patch{
    BALL_CAMERA_LEVEL_PATCH_ADDRESS, BALL_CAMERA_LEVEL_PATCH_ORIGINAL, 0,
    BALL_CAMERA_LEVEL_CAVE, false};
DynamicPpcPatch s_interpolation_camera_level_patch{
    INTERPOLATION_CAMERA_LEVEL_PATCH_ADDRESS, INTERPOLATION_CAMERA_LEVEL_PATCH_ORIGINAL, 0,
    INTERPOLATION_CAMERA_LEVEL_CAVE, false};
DynamicPpcPatch s_first_person_orbit_aim_vector_patch{
    FIRST_PERSON_ORBIT_AIM_VECTOR_ADDRESS, FIRST_PERSON_ORBIT_AIM_VECTOR_ORIGINAL, 0,
    FIRST_PERSON_ORBIT_AIM_VECTOR_CAVE, false};
DynamicPpcPatch s_audio_listener_patch{
    AUDIO_LISTENER_PATCH_ADDRESS, AUDIO_LISTENER_PATCH_ORIGINAL, 0, AUDIO_LISTENER_CAVE, false};

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
  std::string_view text{kPrimedGunBuiltinPatches};
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

    // PrimedGun's built-in patch block uses AR-style dword writes.
    const u8 command_page = static_cast<u8>(command >> 24);
    if (command_page != 0x04)
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

bool TryReadU16(const Core::CPUThreadGuard& guard, u32 address, u16* out)
{
  const auto value = PowerPC::MMU::HostTryRead<u16>(guard, address);
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

bool IsWritablePrimeMem1Address(u32 address, u32 size)
{
  return address >= MEM1_BASE && address < MEM1_END && size <= (MEM1_END - address);
}

bool TryWriteFloat(const Core::CPUThreadGuard& guard, u32 address, float value)
{
  return std::isfinite(value) && IsWritablePrimeMem1Address(address, sizeof(value)) &&
         PowerPC::MMU::HostTryWrite<float>(guard, value, address).has_value();
}

bool TryWriteU32(const Core::CPUThreadGuard& guard, u32 address, u32 value)
{
  return IsWritablePrimeMem1Address(address, sizeof(value)) &&
         PowerPC::MMU::HostTryWrite<u32>(guard, value, address).has_value();
}

bool TryWriteU16(const Core::CPUThreadGuard& guard, u32 address, u16 value)
{
  return IsWritablePrimeMem1Address(address, sizeof(value)) &&
         PowerPC::MMU::HostTryWrite<u16>(guard, value, address).has_value();
}

bool TryWriteU8(const Core::CPUThreadGuard& guard, u32 address, u8 value)
{
  return IsWritablePrimeMem1Address(address, sizeof(value)) &&
         PowerPC::MMU::HostTryWrite<u8>(guard, value, address).has_value();
}

bool TryWriteInstruction(const Core::CPUThreadGuard& guard, u32 address, u32 value)
{
  return IsWritablePrimeMem1Address(address, sizeof(value)) &&
         PowerPC::MMU::HostTryWrite<u32>(guard, value, address).has_value();
}

bool PrimePointerLooksValid(u32 address, u32 size)
{
  return (address & 3u) == 0 && IsWritablePrimeMem1Address(address, size);
}

bool RangeOverlaps(u32 address, u32 size, u32 range_start, u32 range_end)
{
  if (size == 0)
    return false;

  const u64 start = address;
  const u64 end = start + size;
  return start < range_end && end > range_start;
}

bool OverlapsPrimedGunRuntimeArena(u32 address, u32 size)
{
  return RangeOverlaps(address, size, PATCH_CODE_ARENA_BASE,
                      PATCH_CODE_ARENA_BASE + PATCH_CODE_ARENA_SIZE) ||
         RangeOverlaps(address, size, SCRATCH_BASE, SCRATCH_BASE + SCRATCH_ARENA_SIZE);
}

bool PrimeGameObjectPointerLooksValid(u32 address, u32 size)
{
  return PrimePointerLooksValid(address, size) && !OverlapsPrimedGunRuntimeArena(address, size);
}

bool PrimeDataPointerLooksValid(u32 address, u32 size)
{
  return IsWritablePrimeMem1Address(address, size) &&
         !OverlapsPrimedGunRuntimeArena(address, size);
}

bool PlayerObjectLooksValid(const Core::CPUThreadGuard& guard, u32 player)
{
  if (!PrimeGameObjectPointerLooksValid(player, PLAYER_DISABLE_INPUT_FLAGS_OFFSET + 1u))
    return false;

  u32 camera_state = 0xffffffffu;
  u32 morph_state = 0xffffffffu;
  u32 movement_state = 0xffffffffu;
  u32 visor_state = 0xffffffffu;
  u32 holster_state = 0xffffffffu;
  float gun_alpha = 0.0f;
  float vx = 0.0f;
  float vy = 0.0f;
  float vz = 0.0f;
  if (!TryReadU32(guard, player + 0x2F4u, &camera_state) ||
      !TryReadU32(guard, player + 0x2F8u, &morph_state) ||
      !TryReadU32(guard, player + PLAYER_MOVEMENT_STATE_OFFSET, &movement_state) ||
      !TryReadU32(guard, player + PLAYER_VISOR_STATE_OFFSET, &visor_state) ||
      !TryReadFloat(guard, player + 0x494u, &gun_alpha) ||
      !TryReadU32(guard, player + 0x498u, &holster_state) ||
      !TryReadFloat(guard, player + PLAYER_VELOCITY_OFFSET + 0x00u, &vx) ||
      !TryReadFloat(guard, player + PLAYER_VELOCITY_OFFSET + 0x04u, &vy) ||
      !TryReadFloat(guard, player + PLAYER_VELOCITY_OFFSET + 0x08u, &vz))
  {
    return false;
  }

  return camera_state <= 8 && morph_state <= 8 && movement_state <= 16 && visor_state <= 8 &&
         holster_state <= 8 && std::isfinite(gun_alpha) && gun_alpha >= -0.05f &&
         gun_alpha <= 1.05f && std::isfinite(vx) && std::isfinite(vy) && std::isfinite(vz);
}

u32 PpcBranch(u32 from, u32 to);

bool TryWriteInstructionBlock(const Core::CPUThreadGuard& guard, u32 base,
                              std::initializer_list<HookWrite> writes)
{
  for (const HookWrite& write : writes)
  {
    if (!TryWriteInstruction(guard, base + write.offset, write.value))
      return false;
  }
  return true;
}

bool InstructionBlockMatches(const Core::CPUThreadGuard& guard, u32 base,
                             std::initializer_list<HookWrite> writes);

bool RefreshInstructionBlock(const Core::CPUThreadGuard& guard, u32 base,
                             std::initializer_list<HookWrite> writes)
{
  if (InstructionBlockMatches(guard, base, writes))
    return true;
  return TryWriteInstructionBlock(guard, base, writes);
}

bool InstructionBlockMatches(const Core::CPUThreadGuard& guard, u32 base,
                             std::initializer_list<HookWrite> writes)
{
  for (const HookWrite& write : writes)
  {
    u32 current = 0;
    if (!TryReadU32(guard, base + write.offset, &current) || current != write.value)
      return false;
  }
  return true;
}

void InvalidatePrimedGunPatchICache(Core::System& system)
{
  auto& jit = system.GetJitInterface();
  jit.InvalidateICache(0x80000000u, 0x00200000u, true);
  jit.InvalidateICache(PATCH_CODE_ARENA_BASE, PATCH_CODE_ARENA_SIZE, true);
}

bool InstallBranchAfterCaveWrite(const Core::CPUThreadGuard& guard, u32 patch_address,
                                 u32 cave_address, std::initializer_list<HookWrite> cave_writes)
{
  const u32 branch = PpcBranch(patch_address, cave_address);
  u32 current = 0;
  const bool branch_installed = TryReadU32(guard, patch_address, &current) && current == branch;
  if (branch_installed && InstructionBlockMatches(guard, cave_address, cave_writes))
    return false;

  const bool cave_matches = InstructionBlockMatches(guard, cave_address, cave_writes);
  if (!cave_matches && !TryWriteInstructionBlock(guard, cave_address, cave_writes))
    return false;

  if (branch_installed)
    return !cave_matches;

  return TryWriteInstruction(guard, patch_address, branch);
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

void ClearCannonRuntimeScratch(const Core::CPUThreadGuard& guard)
{
  for (u32 offset = 0; offset <= 0x38u; offset += 4u)
    TryWriteU32(guard, CANNON_BASIS_SCRATCH + offset, 0);

  for (u32 offset = 0; offset < 0x0Cu; offset += 4u)
  {
    TryWriteU32(guard, MODEL_OFFSET_WORLD_SCRATCH + offset, 0);
    TryWriteU32(guard, ADJUSTED_GUN_POS_SCRATCH + offset, 0);
  }
}

bool WriteBasisScratch(const Core::CPUThreadGuard& guard, const Matrix3x4& mat)
{
  if (!MatrixNumbersLookValid(mat))
    return false;

  int index = 0;
  for (int row = 0; row < 3; ++row)
  {
    for (int col = 0; col < 3; ++col)
    {
      if (!TryWriteFloat(guard, CANNON_BASIS_SCRATCH + static_cast<u32>(index * 4),
                         mat.At(row, col)))
      {
        return false;
      }
      ++index;
    }
  }
  return true;
}

bool WriteBasis9(const Core::CPUThreadGuard& guard, u32 address, const Matrix3x4& mat)
{
  if (!MatrixNumbersLookValid(mat))
    return false;

  int index = 0;
  for (int row = 0; row < 3; ++row)
  {
    for (int col = 0; col < 3; ++col)
    {
      if (!TryWriteFloat(guard, address + static_cast<u32>(index * 4), mat.At(row, col)))
        return false;
      ++index;
    }
  }
  return true;
}

Matrix3x4 CannonModelMatrixForHand(const Matrix3x4& mat, const RuntimeSettings& settings)
{
  Matrix3x4 model_mat = mat;
  if (!settings.use_right_hand)
  {
    for (int row = 0; row < 3; ++row)
      model_mat.At(row, 0) = -model_mat.At(row, 0);
  }

  return model_mat;
}

struct ScanVisorInfo
{
  bool active = false;
  bool real_state = false;
  u32 player_state = 0;
  u32 current_visor = 0xffffffffu;
  u32 transition_visor = 0xffffffffu;
  u32 proxy_visor = 0xffffffffu;
};

ScanVisorInfo ReadScanVisorInfo(const Core::CPUThreadGuard& guard, u32 player)
{
  ScanVisorInfo info{};
  if (player < 0x80000000u)
    return info;

  if (TryReadU32(guard, ADDRESS.state_manager + STATE_MANAGER_PLAYER_STATE_OFFSET,
                 &info.player_state) &&
      info.player_state >= 0x80000000u &&
      TryReadU32(guard, info.player_state + PLAYER_STATE_CURRENT_VISOR_OFFSET,
                 &info.current_visor))
  {
    TryReadU32(guard, info.player_state + PLAYER_STATE_TRANSITION_VISOR_OFFSET,
               &info.transition_visor);

    if (info.current_visor == PLAYER_STATE_SCAN_VISOR ||
        info.transition_visor == PLAYER_STATE_SCAN_VISOR)
    {
      info.active = true;
      info.real_state = true;
      return info;
    }
  }

  if (TryReadU32(guard, player + PLAYER_VISOR_STATE_OFFSET, &info.proxy_visor) &&
      info.proxy_visor == 1)
  {
    info.active = true;
  }

  return info;
}

bool ScanVisorActive(const Core::CPUThreadGuard& guard, u32 player)
{
  return ReadScanVisorInfo(guard, player).active;
}

bool PlayerIsInMenuMapOrMorphball(const Core::CPUThreadGuard& guard, u32 player)
{
  u32 gameflow_menu = 0;
  if (TryReadU32(guard, GAMEFLOW_MENU_SCRATCH, &gameflow_menu) && gameflow_menu == 1)
    return true;

  u32 morph_state = 0xffffffffu;
  if (!TryReadU32(guard, player + 0x2F8u, &morph_state))
    return true;

  return morph_state != 0;
}

bool InputDisableIsOwnedByPrimedGunDpad(u32 player)
{
  return s_dpad_forced_input_disabled && !s_dpad_input_was_disabled &&
         s_dpad_input_player == player &&
         s_dpad_input_flags_addr == player + PLAYER_DISABLE_INPUT_FLAGS_OFFSET;
}

bool PlayerIsFirstPersonUnmorphed(const Core::CPUThreadGuard& guard, u32 player)
{
  if (!PlayerObjectLooksValid(guard, player))
    return false;

  if (PlayerIsInMenuMapOrMorphball(guard, player))
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
           movement_state <= 16;
  }

  u8 input_flags = 0;
  if (!TryReadU8(guard, player + PLAYER_DISABLE_INPUT_FLAGS_OFFSET, &input_flags) ||
      (((input_flags & PLAYER_DISABLE_INPUT_MASK) != 0) &&
       !InputDisableIsOwnedByPrimedGunDpad(player)))
  {
    return false;
  }

  u32 movement_state = 0xffffffffu;
  return TryReadU32(guard, player + PLAYER_MOVEMENT_STATE_OFFSET, &movement_state) &&
         movement_state <= 6;
}

bool PlayerIsChangingVisorsInFirstPerson(const Core::CPUThreadGuard& guard, u32 player)
{
  if (!PlayerObjectLooksValid(guard, player))
    return false;

  if (PlayerIsInMenuMapOrMorphball(guard, player))
    return false;

  u32 player_state = 0;
  u32 current_visor = 0xffffffffu;
  u32 transition_visor = 0xffffffffu;
  if (!TryReadU32(guard, ADDRESS.state_manager + STATE_MANAGER_PLAYER_STATE_OFFSET,
                  &player_state) ||
      player_state < 0x80000000u ||
      !TryReadU32(guard, player_state + PLAYER_STATE_CURRENT_VISOR_OFFSET, &current_visor) ||
      !TryReadU32(guard, player_state + PLAYER_STATE_TRANSITION_VISOR_OFFSET,
                  &transition_visor) ||
      current_visor > PLAYER_STATE_MAX_VISOR || transition_visor > PLAYER_STATE_MAX_VISOR ||
      current_visor == transition_visor)
  {
    return false;
  }

  u32 camera_state = 0xffffffffu;
  u32 morph_state = 0xffffffffu;
  u32 movement_state = 0xffffffffu;
  if (!TryReadU32(guard, player + 0x2F4u, &camera_state) ||
      !TryReadU32(guard, player + 0x2F8u, &morph_state) ||
      !TryReadU32(guard, player + PLAYER_MOVEMENT_STATE_OFFSET, &movement_state))
  {
    return false;
  }

  return camera_state == 0 && morph_state == 0 && movement_state <= 8;
}

bool PlayerIsFirstPersonGunReady(const Core::CPUThreadGuard& guard, u32 player)
{
  if (!PlayerObjectLooksValid(guard, player))
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
  if (!RuntimeLoggingEnabled())
    return;

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
                 "PrimedGun mode_probe player={:08X} menu={} camera={} morph={} input_flags={:02X} "
                 "move={} visor={} gun_alpha={:.3f} holster={} game_state={:08X} camera_mgr={:08X}",
                 player, gameflow_menu, camera_state, morph_state, input_flags, movement_state,
                 visor_state, gun_alpha, holster_state, game_state, camera_manager);
  NOTICE_LOG_FMT(CORE,
                 "PrimedGun mode_probe player+2E0: {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                 player_words[0], player_words[1], player_words[2], player_words[3],
                 player_words[4], player_words[5], player_words[6], player_words[7]);
  NOTICE_LOG_FMT(CORE,
                 "PrimedGun mode_probe state+800: {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                 state_words[0], state_words[1], state_words[2], state_words[3], state_words[4],
                 state_words[5], state_words[6], state_words[7]);
  NOTICE_LOG_FMT(CORE,
                 "PrimedGun mode_probe game+000: {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                 game_words_0[0], game_words_0[1], game_words_0[2], game_words_0[3],
                 game_words_0[4], game_words_0[5], game_words_0[6], game_words_0[7]);
  NOTICE_LOG_FMT(CORE,
                 "PrimedGun mode_probe game+080: {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                 game_words_80[0], game_words_80[1], game_words_80[2], game_words_80[3],
                 game_words_80[4], game_words_80[5], game_words_80[6], game_words_80[7]);
  NOTICE_LOG_FMT(CORE,
                 "PrimedGun mode_probe game+100: {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                 game_words_100[0], game_words_100[1], game_words_100[2], game_words_100[3],
                 game_words_100[4], game_words_100[5], game_words_100[6], game_words_100[7]);
  NOTICE_LOG_FMT(CORE,
                 "PrimedGun mode_probe camera+000: {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                 camera_words[0], camera_words[1], camera_words[2], camera_words[3],
                 camera_words[4], camera_words[5], camera_words[6], camera_words[7]);
}

void LogScanReticleTrace(const Core::CPUThreadGuard& guard, u32 player)
{
  if (!RuntimeLoggingEnabled())
    return;

  if (player < 0x80000000u || !ScanVisorActive(guard, player))
    return;

  if (s_frame_counter < s_last_scan_reticle_trace_frame + 20)
    return;

  u32 next_hit = 0;
  u32 curr_hit = 0;
  u32 next_reticle = 0;
  u32 curr_reticle = 0;
  u32 next_rot = 0;
  u32 curr_rot = 0;
  u32 visor = 0;
  TryReadU32(guard, SCAN_RETICLE_TRACE_SCRATCH + 0x08u, &next_hit);
  TryReadU32(guard, SCAN_RETICLE_TRACE_SCRATCH + 0x18u, &curr_hit);
  TryReadU32(guard, SCAN_RETICLE_TRACE_SCRATCH + 0x00u, &next_reticle);
  TryReadU32(guard, SCAN_RETICLE_TRACE_SCRATCH + 0x04u, &next_rot);
  TryReadU32(guard, SCAN_RETICLE_TRACE_SCRATCH + 0x10u, &curr_reticle);
  TryReadU32(guard, SCAN_RETICLE_TRACE_SCRATCH + 0x14u, &curr_rot);
  TryReadU32(guard, SCAN_RETICLE_TRACE_SCRATCH + 0x20u, &visor);

  if ((next_hit == 0 || next_reticle < 0x80000000u || next_rot < 0x80000000u) &&
      (curr_hit == 0 || curr_reticle < 0x80000000u || curr_rot < 0x80000000u))
  {
    return;
  }

  s_last_scan_reticle_trace_frame = s_frame_counter;
  TryWriteU32(guard, SCAN_RETICLE_TRACE_SCRATCH + 8u, 0);
  TryWriteU32(guard, SCAN_RETICLE_TRACE_SCRATCH + 0x18u, 0);

  auto read_vec3 = [&](u32 address, float* x, float* y, float* z) {
    *x = 0.0f;
    *y = 0.0f;
    *z = 0.0f;
    TryReadFloat(guard, address + 0x00u, x);
    TryReadFloat(guard, address + 0x04u, y);
    TryReadFloat(guard, address + 0x08u, z);
  };

  float p_right_x = 0.0f;
  float p_right_y = 0.0f;
  float p_right_z = 0.0f;
  float p_up_x = 0.0f;
  float p_up_y = 0.0f;
  float p_up_z = 0.0f;
  float p_fwd_x = 0.0f;
  float p_fwd_y = 0.0f;
  float p_fwd_z = 0.0f;
  read_vec3(player + ADDRESS.transform_offset + 0x00u, &p_right_x, &p_right_y, &p_right_z);
  read_vec3(player + ADDRESS.transform_offset + 0x10u, &p_up_x, &p_up_y, &p_up_z);
  read_vec3(player + ADDRESS.transform_offset + 0x20u, &p_fwd_x, &p_fwd_y, &p_fwd_z);

  u32 camera_manager = 0;
  u32 camera = 0;
  TryReadU32(guard, ADDRESS.state_manager + ADDRESS.camera_manager_offset, &camera_manager);
  if (camera_manager >= 0x80000000u)
    TryReadU32(guard, camera_manager + 0x10u, &camera);

  float c_right_x = 0.0f;
  float c_right_y = 0.0f;
  float c_right_z = 0.0f;
  float c_up_x = 0.0f;
  float c_up_y = 0.0f;
  float c_up_z = 0.0f;
  float c_fwd_x = 0.0f;
  float c_fwd_y = 0.0f;
  float c_fwd_z = 0.0f;
  if (camera >= 0x80000000u)
  {
    read_vec3(camera + ADDRESS.transform_offset + 0x00u, &c_right_x, &c_right_y, &c_right_z);
    read_vec3(camera + ADDRESS.transform_offset + 0x10u, &c_up_x, &c_up_y, &c_up_z);
    read_vec3(camera + ADDRESS.transform_offset + 0x20u, &c_fwd_x, &c_fwd_y, &c_fwd_z);
  }

  auto log_reticle_state = [&](const char* name, u32 reticle, u32 rot, u32 state_offset) {
    if (reticle < 0x80000000u || rot < 0x80000000u)
      return;

    const u32 state = reticle + state_offset;
    u32 target_id_word = 0;
    float radius = 0.0f;
    float tx = 0.0f;
    float ty = 0.0f;
    float tz = 0.0f;
    float factor = 0.0f;
    float min_vp = 0.0f;
    u8 orbit_idle = 0;
    TryReadU32(guard, state + 0x00u, &target_id_word);
    TryReadFloat(guard, state + 0x04u, &radius);
    TryReadFloat(guard, state + 0x08u, &tx);
    TryReadFloat(guard, state + 0x0Cu, &ty);
    TryReadFloat(guard, state + 0x10u, &tz);
    TryReadFloat(guard, state + 0x14u, &factor);
    TryReadFloat(guard, state + 0x18u, &min_vp);
    TryReadU8(guard, state + 0x1Cu, &orbit_idle);

    float r0x = 0.0f;
    float r0y = 0.0f;
    float r0z = 0.0f;
    float r1x = 0.0f;
    float r1y = 0.0f;
    float r1z = 0.0f;
    float r2x = 0.0f;
    float r2y = 0.0f;
    float r2z = 0.0f;
    read_vec3(rot + 0x00u, &r0x, &r0y, &r0z);
    read_vec3(rot + 0x0Cu, &r1x, &r1y, &r1z);
    read_vec3(rot + 0x18u, &r2x, &r2y, &r2z);

    const u16 target_id = static_cast<u16>(target_id_word >> 16);
    NOTICE_LOG_FMT(CORE,
                   "PrimedGun scan_reticle_trace {} reticle={:08X} rot={:08X} target={:04X} "
                   "radius={:.3f} pos=({:.2f},{:.2f},{:.2f}) factor={:.3f} min_vp={:.3f} "
                   "orbit_idle={} rot_rows=({:.3f},{:.3f},{:.3f})/({:.3f},{:.3f},{:.3f})/"
                   "({:.3f},{:.3f},{:.3f})",
                   name, reticle, rot, target_id, radius, tx, ty, tz, factor, min_vp,
                   orbit_idle != 0, r0x, r0y, r0z, r1x, r1y, r1z, r2x, r2y, r2z);
  };

  NOTICE_LOG_FMT(CORE,
                 "PrimedGun scan_basis player={:08X} camera={:08X} p_r=({:.3f},{:.3f},{:.3f}) "
                 "p_u=({:.3f},{:.3f},{:.3f}) p_f=({:.3f},{:.3f},{:.3f}) "
                 "c_r=({:.3f},{:.3f},{:.3f}) c_u=({:.3f},{:.3f},{:.3f}) "
                 "c_f=({:.3f},{:.3f},{:.3f})",
                 player, camera, p_right_x, p_right_y, p_right_z, p_up_x, p_up_y, p_up_z,
                 p_fwd_x, p_fwd_y, p_fwd_z, c_right_x, c_right_y, c_right_z, c_up_x, c_up_y,
                 c_up_z, c_fwd_x, c_fwd_y, c_fwd_z);

  log_reticle_state("next", next_reticle, next_rot, 0x174u);
  log_reticle_state("curr", curr_reticle, curr_rot, 0x10Cu);

  if (visor >= 0x80000000u)
  {
    std::string entries;
    for (int i = 0; i < 8; ++i)
    {
      const u32 entry = visor + 0x140u + static_cast<u32>(i * 0x10);
      u32 target_id_word = 0;
      float f4 = 0.0f;
      float f8 = 0.0f;
      u8 visible = 0;
      TryReadU32(guard, entry + 0x00u, &target_id_word);
      TryReadFloat(guard, entry + 0x04u, &f4);
      TryReadFloat(guard, entry + 0x08u, &f8);
      TryReadU8(guard, entry + 0x0Cu, &visible);
      const u16 target_id = static_cast<u16>(target_id_word >> 16);
      entries += fmt::format("{}#{:d}:id={:04X} f4={:.3f} f8={:.3f} vis={}",
                             entries.empty() ? "" : " ", i, target_id, f4, f8, visible != 0);
    }
    NOTICE_LOG_FMT(CORE, "PrimedGun scan_indicator_table visor={:08X} {}", visor, entries);
  }
}

void DumpScanIndicatorCodeOnce(const Core::CPUThreadGuard& guard)
{
  if (!RuntimeLoggingEnabled())
    return;

  if (s_have_dumped_scan_indicator_code)
    return;

  s_have_dumped_scan_indicator_code = true;

  auto dump_range = [&](const char* name, u32 start, u32 end) {
    std::string words;
    for (u32 address = start; address < end; address += 4)
    {
      u32 instruction = 0;
      if (!TryReadU32(guard, address, &instruction))
        instruction = 0xffffffffu;
      words += fmt::format("{}{:08X}:{:08X}", words.empty() ? "" : " ", address, instruction);
    }
    NOTICE_LOG_FMT(CORE, "PrimedGun scan_code_dump {} {}", name, words);
  };

  dump_range("FindEmptyInactiveScanTarget", 0x80111E20u, 0x80111E5Cu);
  dump_range("FindCachedInactiveScanTarget", 0x80111E5Cu, 0x80111EA8u);
  dump_range("DrawScanObjectIndicators_full", 0x80111EA8u, 0x80112508u);
  dump_range("UpdateScanObjectIndicators_head", 0x80112508u, 0x80112880u);
  dump_range("UpdateScanWindow_head", 0x80112948u, 0x80112B00u);
}

void ApplyHelmetOpacityZero(const Core::CPUThreadGuard& guard, const RuntimeSettings& settings)
{
  if (settings.visor_helmet_enabled)
    return;

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
  bool has_target = false;
  bool has_character = false;
  bool has_orbit = false;
  bool has_scan = false;
  bool targetable = false;
  bool grapple_point = false;
  bool suppress_orbit_hook = false;
};

struct ScanTargetCandidate
{
  u16 uid = 0xffffu;
  u32 obj = 0;
};

struct ActorAabb
{
  float min_x = 0.0f;
  float min_y = 0.0f;
  float min_z = 0.0f;
  float max_x = 0.0f;
  float max_y = 0.0f;
  float max_z = 0.0f;
};

struct RayCandidateMetrics
{
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float along = 0.0f;
  float perp = 0.0f;
  float radius = 0.0f;
  bool valid = false;
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

bool LooksLikeTransformMatrix(const Core::CPUThreadGuard& guard, u32 transform);

bool ReadAabb(const Core::CPUThreadGuard& guard, u32 address, ActorAabb* box)
{
  return address >= 0x80000000u &&
         TryReadFloat(guard, address + 0x00u, &box->min_x) &&
         TryReadFloat(guard, address + 0x04u, &box->min_y) &&
         TryReadFloat(guard, address + 0x08u, &box->min_z) &&
         TryReadFloat(guard, address + 0x0cu, &box->max_x) &&
         TryReadFloat(guard, address + 0x10u, &box->max_y) &&
         TryReadFloat(guard, address + 0x14u, &box->max_z);
}

bool AabbLooksUsable(const ActorAabb& box)
{
  const float width = box.max_x - box.min_x;
  const float height = box.max_y - box.min_y;
  const float depth = box.max_z - box.min_z;
  const float center_x = (box.min_x + box.max_x) * 0.5f;
  const float center_y = (box.min_y + box.max_y) * 0.5f;
  const float center_z = (box.min_z + box.max_z) * 0.5f;
  return std::isfinite(box.min_x) && std::isfinite(box.min_y) && std::isfinite(box.min_z) &&
         std::isfinite(box.max_x) && std::isfinite(box.max_y) && std::isfinite(box.max_z) &&
         std::isfinite(width) && std::isfinite(height) && std::isfinite(depth) &&
         width >= 0.0f && height >= 0.0f && depth >= 0.0f &&
         width <= 1000.0f && height <= 1000.0f && depth <= 1000.0f &&
         std::fabs(center_x) <= 100000.0f && std::fabs(center_y) <= 100000.0f &&
         std::fabs(center_z) <= 100000.0f;
}

float AabbMaxExtent(const ActorAabb& box)
{
  return std::max({box.max_x - box.min_x, box.max_y - box.min_y, box.max_z - box.min_z});
}

bool ReadActorRenderAabb(const Core::CPUThreadGuard& guard, u32 obj, ActorAabb* box)
{
  return PrimeGameObjectPointerLooksValid(obj, 0xb4u) && ReadAabb(guard, obj + 0x9cu, box) &&
         AabbLooksUsable(*box);
}

bool TransformAabbByMatrix(const Core::CPUThreadGuard& guard, u32 transform, const ActorAabb& local,
                           ActorAabb* world)
{
  if (!LooksLikeTransformMatrix(guard, transform))
    return false;

  float m[12] = {};
  for (int i = 0; i < 12; ++i)
  {
    if (!TryReadFloat(guard, transform + static_cast<u32>(i * 4), &m[i]) ||
        !std::isfinite(m[i]))
    {
      return false;
    }
  }

  world->min_x = world->min_y = world->min_z = std::numeric_limits<float>::max();
  world->max_x = world->max_y = world->max_z = std::numeric_limits<float>::lowest();

  for (int ix = 0; ix < 2; ++ix)
  {
    const float x = ix == 0 ? local.min_x : local.max_x;
    for (int iy = 0; iy < 2; ++iy)
    {
      const float y = iy == 0 ? local.min_y : local.max_y;
      for (int iz = 0; iz < 2; ++iz)
      {
        const float z = iz == 0 ? local.min_z : local.max_z;
        const float wx = m[0] * x + m[1] * y + m[2] * z + m[3];
        const float wy = m[4] * x + m[5] * y + m[6] * z + m[7];
        const float wz = m[8] * x + m[9] * y + m[10] * z + m[11];
        if (!std::isfinite(wx) || !std::isfinite(wy) || !std::isfinite(wz))
          return false;

        world->min_x = std::min(world->min_x, wx);
        world->min_y = std::min(world->min_y, wy);
        world->min_z = std::min(world->min_z, wz);
        world->max_x = std::max(world->max_x, wx);
        world->max_y = std::max(world->max_y, wy);
        world->max_z = std::max(world->max_z, wz);
      }
    }
  }

  return AabbLooksUsable(*world);
}

bool ReadActorPhysicsAabb(const Core::CPUThreadGuard& guard, u32 obj, ActorAabb* box)
{
  ActorAabb base = {};
  float tx = 0.0f;
  float ty = 0.0f;
  float tz = 0.0f;
  float off_x = 0.0f;
  float off_y = 0.0f;
  float off_z = 0.0f;
  if (!PrimeGameObjectPointerLooksValid(obj, 0x1f4u) || !ReadAabb(guard, obj + 0x1a4u, &base) ||
      !AabbLooksUsable(base))
  {
    return false;
  }

  if (TransformAabbByMatrix(guard, obj + 0x1e8u, base, box))
    return true;

  if (!ReadTransformTranslation(guard, obj + ADDRESS.transform_offset, &tx, &ty, &tz) ||
      !TryReadFloat(guard, obj + 0x1e8u, &off_x) || !TryReadFloat(guard, obj + 0x1ecu, &off_y) ||
      !TryReadFloat(guard, obj + 0x1f0u, &off_z) || !std::isfinite(off_x) ||
      !std::isfinite(off_y) || !std::isfinite(off_z))
  {
    return false;
  }

  const float x = tx + off_x;
  const float y = ty + off_y;
  const float z = tz + off_z;
  box->min_x = base.min_x + x;
  box->min_y = base.min_y + y;
  box->min_z = base.min_z + z;
  box->max_x = base.max_x + x;
  box->max_y = base.max_y + y;
  box->max_z = base.max_z + z;
  return AabbLooksUsable(*box);
}

bool RayIntersectsAabb(float ray_x, float ray_y, float ray_z, float dir_x, float dir_y,
                       float dir_z, const ActorAabb& box, float max_along, float* hit_along)
{
  float t_min = 0.0f;
  float t_max = max_along;
  bool starts_inside = ray_x >= box.min_x && ray_x <= box.max_x &&
                       ray_y >= box.min_y && ray_y <= box.max_y &&
                       ray_z >= box.min_z && ray_z <= box.max_z;
  const auto test_axis = [](float ray, float dir, float min_v, float max_v, float* min_t,
                            float* max_t) {
    if (std::fabs(dir) < 0.00001f)
      return ray >= min_v && ray <= max_v;

    float t0 = (min_v - ray) / dir;
    float t1 = (max_v - ray) / dir;
    if (t0 > t1)
      std::swap(t0, t1);
    *min_t = std::max(*min_t, t0);
    *max_t = std::min(*max_t, t1);
    return *min_t <= *max_t;
  };

  if (!test_axis(ray_x, dir_x, box.min_x, box.max_x, &t_min, &t_max) ||
      !test_axis(ray_y, dir_y, box.min_y, box.max_y, &t_min, &t_max) ||
      !test_axis(ray_z, dir_z, box.min_z, box.max_z, &t_min, &t_max))
  {
    return false;
  }

  *hit_along = starts_inside ? 0.0f : std::max(t_min, 0.0f);
  return std::isfinite(*hit_along) && *hit_along <= max_along;
}

bool RayMetricsForPoint(float ray_x, float ray_y, float ray_z, float dir_x, float dir_y,
                        float dir_z, float x, float y, float z, float max_along,
                        RayCandidateMetrics* metrics)
{
  const float vx = x - ray_x;
  const float vy = y - ray_y;
  const float vz = z - ray_z;
  const float along = vx * dir_x + vy * dir_y + vz * dir_z;
  if (along < -0.5f || along > max_along)
    return false;

  const float clamped_along = std::max(along, 0.0f);

  const float nearest_x = ray_x + dir_x * clamped_along;
  const float nearest_y = ray_y + dir_y * clamped_along;
  const float nearest_z = ray_z + dir_z * clamped_along;
  const float dx = x - nearest_x;
  const float dy = y - nearest_y;
  const float dz = z - nearest_z;
  const float perp_sq = dx * dx + dy * dy + dz * dz;
  if (!std::isfinite(perp_sq))
    return false;

  metrics->x = x;
  metrics->y = y;
  metrics->z = z;
  metrics->along = clamped_along;
  metrics->perp = std::sqrt(perp_sq);
  metrics->radius = 0.0f;
  metrics->valid = true;
  return true;
}

bool RayMetricsForAabb(float ray_x, float ray_y, float ray_z, float dir_x, float dir_y,
                       float dir_z, const ActorAabb& box, float max_along,
                       RayCandidateMetrics* metrics)
{
  if (!AabbLooksUsable(box))
    return false;

  const float center_x = (box.min_x + box.max_x) * 0.5f;
  const float center_y = (box.min_y + box.max_y) * 0.5f;
  const float center_z = (box.min_z + box.max_z) * 0.5f;
  const float half_x = (box.max_x - box.min_x) * 0.5f;
  const float half_y = (box.max_y - box.min_y) * 0.5f;
  const float half_z = (box.max_z - box.min_z) * 0.5f;
  const float projected_radius_sq =
      half_x * half_x * std::max(0.0f, 1.0f - dir_x * dir_x) +
      half_y * half_y * std::max(0.0f, 1.0f - dir_y * dir_y) +
      half_z * half_z * std::max(0.0f, 1.0f - dir_z * dir_z);
  const float projected_radius = std::sqrt(std::max(0.0f, projected_radius_sq));

  float hit_along = 0.0f;
  if (RayIntersectsAabb(ray_x, ray_y, ray_z, dir_x, dir_y, dir_z, box, max_along, &hit_along))
  {
    metrics->x = ray_x + dir_x * hit_along;
    metrics->y = ray_y + dir_y * hit_along;
    metrics->z = ray_z + dir_z * hit_along;
    metrics->along = hit_along;
    metrics->perp = 0.0f;
    metrics->radius = projected_radius;
    metrics->valid = true;
    return true;
  }

  const float vx = center_x - ray_x;
  const float vy = center_y - ray_y;
  const float vz = center_z - ray_z;
  const float along = vx * dir_x + vy * dir_y + vz * dir_z;
  if (!std::isfinite(along) || along < -projected_radius ||
      along > max_along + projected_radius)
  {
    return false;
  }

  const float clamped_along = std::clamp(along, 0.0f, max_along);
  const float nearest_x = ray_x + dir_x * clamped_along;
  const float nearest_y = ray_y + dir_y * clamped_along;
  const float nearest_z = ray_z + dir_z * clamped_along;
  const float dx = center_x - nearest_x;
  const float dy = center_y - nearest_y;
  const float dz = center_z - nearest_z;
  const float perp_sq = dx * dx + dy * dy + dz * dz;
  if (!std::isfinite(perp_sq))
    return false;

  metrics->x = nearest_x;
  metrics->y = nearest_y;
  metrics->z = nearest_z;
  metrics->along = clamped_along;
  metrics->perp = std::max(0.0f, std::sqrt(perp_sq) - projected_radius);
  metrics->radius = projected_radius;
  metrics->valid = true;
  return true;
}

bool PreferRayMetrics(const RayCandidateMetrics& candidate, const RayCandidateMetrics& current)
{
  if (!candidate.valid)
    return false;
  if (!current.valid)
    return true;
  const float candidate_score = candidate.perp + candidate.along * 0.001f;
  const float current_score = current.perp + current.along * 0.001f;
  return candidate_score < current_score;
}

RayCandidateMetrics ResolveActorRayMetrics(const Core::CPUThreadGuard& guard, u32 obj, float ray_x,
                                           float ray_y, float ray_z, float dir_x, float dir_y,
                                           float dir_z, float max_along, bool allow_physics_aabb)
{
  RayCandidateMetrics best = {};
  float ox = 0.0f;
  float oy = 0.0f;
  float oz = 0.0f;
  RayCandidateMetrics metrics = {};
  if (ReadTransformTranslation(guard, obj + ADDRESS.transform_offset, &ox, &oy, &oz) &&
      RayMetricsForPoint(ray_x, ray_y, ray_z, dir_x, dir_y, dir_z, ox, oy, oz, max_along,
                         &metrics) &&
      PreferRayMetrics(metrics, best))
  {
    best = metrics;
  }

  ActorAabb box = {};
  if (ReadActorRenderAabb(guard, obj, &box) &&
      RayMetricsForAabb(ray_x, ray_y, ray_z, dir_x, dir_y, dir_z, box, max_along, &metrics) &&
      PreferRayMetrics(metrics, best))
  {
    best = metrics;
  }

  if (allow_physics_aabb && ReadActorPhysicsAabb(guard, obj, &box) &&
      RayMetricsForAabb(ray_x, ray_y, ray_z, dir_x, dir_y, dir_z, box, max_along, &metrics) &&
      PreferRayMetrics(metrics, best))
  {
    best = metrics;
  }

  return best;
}

bool ScanAabbUsableForAim(const ActorAabb& box, const RuntimeSettings& settings,
                          float min_extent, float max_extent_cap)
{
  // Some scan actors use large trigger volumes, so scan aim only trusts bounded object shapes.
  const float targeting_radius = ClampFinite(settings.gun_targeting_radius, 4.0f, 0.1f, 50.0f);
  const float max_extent = std::clamp(targeting_radius * 2.0f, min_extent, max_extent_cap);
  return AabbMaxExtent(box) <= max_extent;
}

bool ScanRenderAabbUsableForAim(const ActorAabb& box, const RuntimeSettings& settings)
{
  return ScanAabbUsableForAim(box, settings, 8.0f, 22.0f);
}

bool ScanPhysicsAabbUsableForAim(const ActorAabb& box, const RuntimeSettings& settings)
{
  return ScanAabbUsableForAim(box, settings, 6.0f, 10.0f);
}

RayCandidateMetrics ResolveScanActorRayMetrics(const Core::CPUThreadGuard& guard, u32 obj,
                                               float ray_x, float ray_y, float ray_z, float dir_x,
                                               float dir_y, float dir_z, float max_along,
                                               const RuntimeSettings& settings)
{
  RayCandidateMetrics best = {};
  float ox = 0.0f;
  float oy = 0.0f;
  float oz = 0.0f;
  RayCandidateMetrics metrics = {};
  if (ReadTransformTranslation(guard, obj + ADDRESS.transform_offset, &ox, &oy, &oz) &&
      RayMetricsForPoint(ray_x, ray_y, ray_z, dir_x, dir_y, dir_z, ox, oy, oz, max_along,
                         &metrics) &&
      PreferRayMetrics(metrics, best))
  {
    best = metrics;
  }

  ActorAabb box = {};
  if (ReadActorRenderAabb(guard, obj, &box) && ScanRenderAabbUsableForAim(box, settings) &&
      RayMetricsForAabb(ray_x, ray_y, ray_z, dir_x, dir_y, dir_z, box, max_along, &metrics) &&
      PreferRayMetrics(metrics, best))
  {
    best = metrics;
  }

  if (ReadActorPhysicsAabb(guard, obj, &box) && ScanPhysicsAabbUsableForAim(box, settings) &&
      RayMetricsForAabb(ray_x, ray_y, ray_z, dir_x, dir_y, dir_z, box, max_along, &metrics) &&
      PreferRayMetrics(metrics, best))
  {
    best = metrics;
  }

  return best;
}

float ScanTargetAimConePerp(const RuntimeSettings& settings, const RayCandidateMetrics& metrics)
{
  const float targeting_radius = ClampFinite(settings.gun_targeting_radius, 4.0f, 0.1f, 50.0f);
  const float base_perp = std::clamp(targeting_radius * 0.45f, 0.75f, 2.0f);
  const float distance_slack = std::clamp(metrics.along - 3.0f, 0.0f, 60.0f) * 0.18f;
  const float bounds_slack = std::min(metrics.radius, 3.0f) * 0.35f;
  const float max_perp = std::clamp(targeting_radius * 2.75f, 3.0f, 14.0f);
  return std::min(base_perp + distance_slack + bounds_slack, max_perp);
}

float ScanVisualAimConePerp(const RuntimeSettings& settings, const RayCandidateMetrics& metrics)
{
  const float targeting_radius = ClampFinite(settings.gun_targeting_radius, 4.0f, 0.1f, 50.0f);
  const float base_perp = std::clamp(targeting_radius * 3.0f, 3.0f, 14.0f);
  const float distance_slack = std::clamp(metrics.along, 0.0f, 80.0f) * 0.55f;
  const float bounds_slack = std::min(metrics.radius, 2.0f) * 0.35f;
  const float max_perp = std::clamp(targeting_radius * 8.0f, 18.0f, 36.0f);
  return std::min(base_perp + distance_slack + bounds_slack, max_perp);
}

float ScoreScanTarget(const RayCandidateMetrics& metrics, float aim_cone_perp)
{
  const float cone_fraction = metrics.perp / std::max(aim_cone_perp, 0.001f);
  const float radius_penalty = std::min(metrics.radius, 1.0f) * 0.02f;
  return cone_fraction * 2.0f + metrics.perp * 0.20f + metrics.along * 0.003f + radius_penalty;
}

float ScoreScanVisualTarget(const RayCandidateMetrics& metrics, float aim_cone_perp)
{
  const float cone_fraction = metrics.perp / std::max(aim_cone_perp, 0.001f);
  return cone_fraction + metrics.along * 0.001f + std::min(metrics.radius, 1.0f) * 0.02f;
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
  if (!PrimeGameObjectPointerLooksValid(obj, 0x70u) || bit < 0 || bit >= 64)
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
  return PrimeGameObjectPointerLooksValid(obj, 0xe8u) && TryReadU8(guard, obj + 0xe7u, &flags) &&
         (flags & 0x01u) != 0;
}

bool ActorIsGrapplePoint(const Core::CPUThreadGuard& guard, u32 obj)
{
  u32 vtable = 0;
  return PrimeGameObjectPointerLooksValid(obj, sizeof(u32)) && TryReadU32(guard, obj, &vtable) &&
         vtable >= 0x803E0D00u && vtable < 0x803E0D70u;
}

bool GunAimDirectionFromMatrix(const Matrix3x4& mat, float* x, float* y, float* z)
{
  *x = mat.m[1];
  *y = mat.m[5];
  *z = mat.m[9];
  return Normalize3(*x, *y, *z);
}

bool RemoveRollFromAimBasis(Matrix3x4* mat)
{
  float forward_x = mat->m[1];
  float forward_y = mat->m[5];
  float forward_z = mat->m[9];
  if (!Normalize3(forward_x, forward_y, forward_z))
    return false;

  constexpr float world_up_x = 0.0f;
  constexpr float world_up_y = 0.0f;
  constexpr float world_up_z = 1.0f;

  float right_x = forward_y * world_up_z - forward_z * world_up_y;
  float right_y = forward_z * world_up_x - forward_x * world_up_z;
  float right_z = forward_x * world_up_y - forward_y * world_up_x;
  if (!Normalize3(right_x, right_y, right_z))
  {
    right_x = -1.0f;
    right_y = 0.0f;
    right_z = 0.0f;
  }

  float up_x = right_y * forward_z - right_z * forward_y;
  float up_y = right_z * forward_x - right_x * forward_z;
  float up_z = right_x * forward_y - right_y * forward_x;
  if (!Normalize3(up_x, up_y, up_z))
    return false;

  mat->At(0, 0) = right_x;
  mat->At(1, 0) = right_y;
  mat->At(2, 0) = right_z;
  mat->At(0, 1) = forward_x;
  mat->At(1, 1) = forward_y;
  mat->At(2, 1) = forward_z;
  mat->At(0, 2) = up_x;
  mat->At(1, 2) = up_y;
  mat->At(2, 2) = up_z;
  return true;
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
         PrimeGameObjectPointerLooksValid(obj, ADDRESS.transform_offset + 0x30u) &&
         (expected_obj < 0x80000000u || obj == expected_obj) &&
         TryReadU32(guard, obj + 0x8u, &live_uid_word) &&
         static_cast<u16>(live_uid_word >> 16) == uid;
}

void WriteGunTargetScratch(const Core::CPUThreadGuard& guard, u32 player, u16 uid)
{
  TryWriteU32(guard, GUN_TARGET_SCRATCH, player);
  TryWriteU32(guard, GUN_TARGET_SCRATCH + 4u, 0);
  TryWriteU16(guard, GUN_TARGET_SCRATCH + 4u, uid);
}

void ClearScanReticleScratch(const Core::CPUThreadGuard& guard, u32 player)
{
  WriteGunTargetScratch(guard, player, 0xffffu);
  TryWriteU32(guard, RETICLE_BILLBOARD_SCRATCH, 0);
  for (u32 offset = 4u; offset <= 0x24u; offset += 4u)
    TryWriteU32(guard, RETICLE_BILLBOARD_SCRATCH + offset, 0);
}

bool ObjectByUid(const Core::CPUThreadGuard& guard, u32 state_manager, u16 uid, u32* obj)
{
  if (uid == 0xffffu || state_manager < 0x80000000u || obj == nullptr)
    return false;

  u32 object_list = 0;
  if (!TryReadU32(guard, state_manager + 0x810u, &object_list) || object_list < 0x80000000u)
    return false;

  u32 live_uid_word = 0;
  return TryReadU32(guard, object_list + ((uid & 0x03ffu) << 3) + 4u, obj) &&
         PrimeGameObjectPointerLooksValid(*obj, ADDRESS.transform_offset + 0x30u) &&
         TryReadU32(guard, *obj + 0x8u, &live_uid_word) &&
         static_cast<u16>(live_uid_word >> 16) == uid;
}

bool ReadPlayerTargetUid(const Core::CPUThreadGuard& guard, u32 player, u32 offset, u16* uid)
{
  u32 word = 0;
  if (uid == nullptr || !TryReadU32(guard, player + offset, &word))
    return false;

  *uid = static_cast<u16>(word >> 16);
  return *uid != 0xffffu;
}

bool ObjectLooksLikeVanillaTarget(const Core::CPUThreadGuard& guard, u32 obj)
{
  return ActorIsTargetable(guard, obj) &&
         (ActorHasMaterial(guard, obj, 40) || ActorHasMaterial(guard, obj, 41) ||
          ActorHasMaterial(guard, obj, 39) || ActorIsGrapplePoint(guard, obj));
}

bool EntityLooksActive(const Core::CPUThreadGuard& guard, u32 obj)
{
  u8 flags = 0;
  return PrimeGameObjectPointerLooksValid(obj, 0x31u) && TryReadU8(guard, obj + 0x30u, &flags) &&
         (flags & 0x80u) != 0;
}

bool ActorHasScanInfo(const Core::CPUThreadGuard& guard, u32 obj)
{
  u32 scan_info = 0;
  return PrimeGameObjectPointerLooksValid(obj, 0x9cu) &&
         TryReadU32(guard, obj + 0x98u, &scan_info) && scan_info >= 0x80000000u;
}

bool ReadVanillaTargetUid(const Core::CPUThreadGuard& guard, u32 state_manager, u32 player,
                          u32 gun, u16* uid)
{
  if (uid == nullptr)
    return false;

  constexpr std::array<u32, 3> target_offsets = {
      PLAYER_ORBIT_TARGET_ID_OFFSET,
      PLAYER_ORBIT_LOCK_ID_OFFSET,
      PLAYER_AIM_TARGET_ID_OFFSET,
  };

  for (const u32 offset : target_offsets)
  {
    u16 candidate_uid = 0xffffu;
    u32 obj = 0;
    if (!ReadPlayerTargetUid(guard, player, offset, &candidate_uid) ||
        !ObjectByUid(guard, state_manager, candidate_uid, &obj) || obj == player || obj == gun ||
        !ObjectLooksLikeVanillaTarget(guard, obj))
    {
      continue;
    }

    *uid = candidate_uid;
    return true;
  }

  return false;
}

bool PlayerHasVanillaTarget(const Core::CPUThreadGuard& guard, u32 state_manager, u32 player)
{
  constexpr std::array<u32, 3> target_offsets = {
      PLAYER_ORBIT_TARGET_ID_OFFSET,
      PLAYER_ORBIT_LOCK_ID_OFFSET,
      PLAYER_AIM_TARGET_ID_OFFSET,
  };

  for (const u32 offset : target_offsets)
  {
    u16 candidate_uid = 0xffffu;
    u32 obj = 0;
    if (ReadPlayerTargetUid(guard, player, offset, &candidate_uid) &&
        ObjectByUid(guard, state_manager, candidate_uid, &obj) && obj != player &&
        ObjectLooksLikeVanillaTarget(guard, obj))
    {
      return true;
    }
  }

  return false;
}

bool PlayerHasPrimedGunTarget(const Core::CPUThreadGuard& guard, u32 state_manager, u32 player)
{
  u32 scratch_player = 0;
  u16 uid = 0xffffu;
  return TryReadU32(guard, GUN_TARGET_SCRATCH, &scratch_player) && scratch_player == player &&
         TryReadU16(guard, GUN_TARGET_SCRATCH + 4u, &uid) &&
         TargetUidStillExists(guard, state_manager, uid, 0);
}

bool PlayerHasOrbitControlTarget(const Core::CPUThreadGuard& guard, u32 state_manager, u32 player)
{
  return PlayerHasVanillaTarget(guard, state_manager, player) ||
         PlayerHasPrimedGunTarget(guard, state_manager, player);
}

bool ResolveActiveCameraTransform(const Core::CPUThreadGuard& guard, u32* transform);

bool RefreshOrbitTargetCandidates(const Core::CPUThreadGuard& guard, u32 state_manager, u32 player,
                                  u32 vector_offset, std::vector<ScanTargetCandidate>* candidates)
{
  if (candidates == nullptr)
    return false;

  candidates->clear();

  u32 count = 0;
  u32 items = 0;
  if (!TryReadU32(guard, player + vector_offset + 4u, &count) ||
      !TryReadU32(guard, player + vector_offset + 12u, &items) ||
      count > 128u || !PrimeDataPointerLooksValid(items, count * sizeof(u16)))
  {
    return false;
  }

  candidates->reserve(count);
  for (u32 i = 0; i < count; ++i)
  {
    u16 uid = 0xffffu;
    if (!TryReadU16(guard, items + i * 2u, &uid) || uid == 0xffffu)
      continue;

    u32 obj = 0;
    if (!ObjectByUid(guard, state_manager, uid, &obj) || obj == player)
      continue;

    candidates->push_back({uid, obj});
  }

  return !candidates->empty();
}

bool RefreshScanTargetCandidates(const Core::CPUThreadGuard& guard, u32 state_manager, u32 player,
                                 std::vector<ScanTargetCandidate>* candidates)
{
  return RefreshOrbitTargetCandidates(guard, state_manager, player,
                                      PLAYER_NEARBY_ORBIT_OBJECTS_OFFSET, candidates);
}

bool CandidateListContainsUid(const std::vector<ScanTargetCandidate>& candidates, u16 uid)
{
  for (const ScanTargetCandidate& candidate : candidates)
  {
    if (candidate.uid == uid)
      return true;
  }
  return false;
}

void SetScanVisorValidDirectionPhases(const Core::CPUThreadGuard& guard, u32 visor)
{
  if (visor < 0x80000000u ||
      !PrimeDataPointerLooksValid(visor + PLAYER_VISOR_SCAN_FRAME_COLOR_INTERP_OFFSET, 8u))
  {
    return;
  }

  TryWriteFloat(guard, visor + PLAYER_VISOR_SCAN_FRAME_COLOR_INTERP_OFFSET, 1.0f);
  TryWriteFloat(guard, visor + PLAYER_VISOR_SCAN_FRAME_COLOR_IMPULSE_OFFSET, 1.0f);
}

void EnsureUidInScanVisorTargets(const Core::CPUThreadGuard& guard, u32 visor, u16 uid)
{
  if (visor < 0x80000000u || uid == 0xffffu ||
      !PrimeDataPointerLooksValid(visor + PLAYER_VISOR_SCAN_TARGETS_OFFSET,
                                  4u + PLAYER_VISOR_SCAN_TARGET_CAPACITY *
                                           PLAYER_VISOR_SCAN_TARGET_SLOT_SIZE))
  {
    return;
  }

  u32 count = 0;
  if (!TryReadU32(guard, visor + PLAYER_VISOR_SCAN_TARGETS_OFFSET, &count) ||
      count > PLAYER_VISOR_SCAN_TARGET_CAPACITY)
  {
    return;
  }

  const u32 data = visor + PLAYER_VISOR_SCAN_TARGET_DATA_OFFSET;
  u32 inactive_entry = 0;
  for (u32 i = 0; i < count; ++i)
  {
    const u32 entry = data + i * PLAYER_VISOR_SCAN_TARGET_SLOT_SIZE;
    u16 existing_uid = 0xffffu;
    float timer = 0.0f;
    const bool have_uid = TryReadU16(guard, entry, &existing_uid);
    const bool have_timer = TryReadFloat(guard, entry + 0x04u, &timer) && std::isfinite(timer);
    if (inactive_entry == 0 && (!have_uid || existing_uid == 0xffffu || !have_timer || timer <= 0.0f))
      inactive_entry = entry;

    if (!have_uid || existing_uid != uid)
      continue;

    if (!have_timer || timer < PLAYER_VISOR_SCAN_TARGET_VALID_TIMER)
    {
      TryWriteFloat(guard, entry + 0x04u, PLAYER_VISOR_SCAN_TARGET_VALID_TIMER);
    }
    TryWriteFloat(guard, entry + 0x08u, 0.0f);
    TryWriteU8(guard, entry + 0x0Cu, 1);
    SetScanVisorValidDirectionPhases(guard, visor);
    return;
  }

  if (inactive_entry != 0)
  {
    TryWriteU16(guard, inactive_entry, uid);
    TryWriteFloat(guard, inactive_entry + 0x04u, PLAYER_VISOR_SCAN_TARGET_VALID_TIMER);
    TryWriteFloat(guard, inactive_entry + 0x08u, 0.0f);
    TryWriteU8(guard, inactive_entry + 0x0Cu, 1);
    SetScanVisorValidDirectionPhases(guard, visor);
    return;
  }

  if (count >= PLAYER_VISOR_SCAN_TARGET_CAPACITY)
    return;

  const u32 entry = data + count * PLAYER_VISOR_SCAN_TARGET_SLOT_SIZE;
  TryWriteU16(guard, entry, uid);
  TryWriteFloat(guard, entry + 0x04u, PLAYER_VISOR_SCAN_TARGET_VALID_TIMER);
  TryWriteFloat(guard, entry + 0x08u, 0.0f);
  TryWriteU8(guard, entry + 0x0Cu, 1);
  TryWriteU32(guard, visor + PLAYER_VISOR_SCAN_TARGETS_OFFSET, count + 1u);
  SetScanVisorValidDirectionPhases(guard, visor);
}

bool SeedScanIndicatorTargetsFromHmd(const Core::CPUThreadGuard& guard, u32 state_manager,
                                     u32 player, u32 visor, const Matrix3x4& hmd,
                                     const RuntimeSettings& settings, GunTargetPick* pick)
{
  if (state_manager < 0x80000000u || !PlayerObjectLooksValid(guard, player) ||
      visor < 0x80000000u)
  {
    return false;
  }

  u32 object_list = 0;
  u32 player_uid_word = 0;
  u32 player_area = 0xffffffffu;
  if (!TryReadU32(guard, state_manager + 0x810u, &object_list) ||
      object_list < 0x80000000u || !TryReadU32(guard, player + 0x8u, &player_uid_word) ||
      !TryReadU32(guard, player + 0x4u, &player_area))
  {
    return false;
  }

  float ray_x = 0.0f;
  float ray_y = 0.0f;
  float ray_z = 0.0f;
  u32 camera_transform = 0;
  if (!ResolveActiveCameraTransform(guard, &camera_transform) ||
      !ReadTransformTranslation(guard, camera_transform, &ray_x, &ray_y, &ray_z))
  {
    if (!ReadTransformTranslation(guard, player + ADDRESS.transform_offset, &ray_x, &ray_y, &ray_z))
      return false;
  }

  float dir_x = 0.0f;
  float dir_y = 0.0f;
  float dir_z = 0.0f;
  if (!GunAimDirectionFromMatrix(hmd, &dir_x, &dir_y, &dir_z))
    return false;

  struct Candidate
  {
    u16 uid = 0xffffu;
    u32 obj = 0;
    RayCandidateMetrics metrics = {};
    float score = std::numeric_limits<float>::infinity();
  };
  std::array<Candidate, 16> visual_best{};
  std::array<Candidate, 8> target_best{};
  const auto insert_candidate = [](auto& candidates, u16 uid, u32 obj,
                                   const RayCandidateMetrics& metrics, float score) {
    size_t slot = candidates.size();
    float worst = -1.0f;
    for (size_t i = 0; i < candidates.size(); ++i)
    {
      if (candidates[i].uid == uid)
        return;
      if (candidates[i].uid == 0xffffu)
      {
        slot = i;
        break;
      }
      if (candidates[i].score > worst)
      {
        worst = candidates[i].score;
        slot = i;
      }
    }

    if (slot >= candidates.size() ||
        (candidates[slot].uid != 0xffffu && score >= candidates[slot].score))
      return;

    candidates[slot].uid = uid;
    candidates[slot].obj = obj;
    candidates[slot].metrics = metrics;
    candidates[slot].score = score;
  };

  const u16 player_uid = static_cast<u16>(player_uid_word >> 16);
  const float max_along = std::clamp(settings.gun_targeting_distance, 15.0f, 100.0f);

  for (u32 i = 0; i < 1024u; ++i)
  {
    u32 obj = 0;
    if (!TryReadU32(guard, object_list + (i << 3) + 4u, &obj) ||
        !PrimeGameObjectPointerLooksValid(obj, ADDRESS.transform_offset + 0x30u) ||
        obj == player || !LooksLikeTransformMatrix(guard, obj + ADDRESS.transform_offset))
    {
      continue;
    }

    u32 uid_word = 0;
    u32 owner_uid_word = 0;
    u32 obj_area = 0xffffffffu;
    if (!TryReadU32(guard, obj + 0x8u, &uid_word) ||
        !TryReadU32(guard, obj + 0xecu, &owner_uid_word) ||
        !TryReadU32(guard, obj + 0x4u, &obj_area))
    {
      continue;
    }

    const u16 uid = static_cast<u16>(uid_word >> 16);
    const u16 owner_uid = static_cast<u16>(owner_uid_word >> 16);
    if (uid == 0xffffu || uid == player_uid || owner_uid == player_uid || obj_area != player_area ||
        !EntityLooksActive(guard, obj) || !ActorIsTargetable(guard, obj) ||
        !ActorHasMaterial(guard, obj, 39) || !ActorHasScanInfo(guard, obj))
    {
      continue;
    }

    const RayCandidateMetrics metrics =
        ResolveScanActorRayMetrics(guard, obj, ray_x, ray_y, ray_z, dir_x, dir_y, dir_z,
                                   max_along, settings);
    if (!metrics.valid)
      continue;

    const float visual_perp = ScanVisualAimConePerp(settings, metrics);
    if (metrics.perp <= visual_perp)
      insert_candidate(visual_best, uid, obj, metrics, ScoreScanVisualTarget(metrics, visual_perp));

    const float target_perp = ScanTargetAimConePerp(settings, metrics);
    if (metrics.perp <= target_perp)
      insert_candidate(target_best, uid, obj, metrics, ScoreScanTarget(metrics, target_perp));
  }

  for (const Candidate& candidate : visual_best)
  {
    if (candidate.uid != 0xffffu)
      EnsureUidInScanVisorTargets(guard, visor, candidate.uid);
  }

  const Candidate* selected = nullptr;
  for (const Candidate& candidate : target_best)
  {
    if (candidate.uid != 0xffffu)
    {
      if (selected == nullptr || candidate.score < selected->score)
        selected = &candidate;
    }
  }

  if (selected == nullptr || pick == nullptr)
    return selected != nullptr;

  EnsureUidInScanVisorTargets(guard, visor, selected->uid);

  pick->uid = selected->uid;
  pick->obj = selected->obj;
  pick->ray_x = ray_x;
  pick->ray_y = ray_y;
  pick->ray_z = ray_z;
  pick->dir_x = dir_x;
  pick->dir_y = dir_y;
  pick->dir_z = dir_z;
  pick->x = selected->metrics.x;
  pick->y = selected->metrics.y;
  pick->z = selected->metrics.z;
  pick->along = selected->metrics.along;
  pick->perp = selected->metrics.perp;
  pick->score = selected->score;
  pick->has_scan = true;
  pick->targetable = true;
  return true;
}

bool PickScanTargetFromHmd(const Core::CPUThreadGuard& guard, u32 state_manager, u32 player,
                           const Matrix3x4& hmd, const RuntimeSettings& settings,
                           GunTargetPick* pick)
{
  if (!settings.gun_targeting_enabled || state_manager < 0x80000000u ||
      !PlayerObjectLooksValid(guard, player))
  {
    return false;
  }

  std::vector<ScanTargetCandidate> candidates;
  if (!RefreshScanTargetCandidates(guard, state_manager, player, &candidates))
    return false;

  float ray_x = 0.0f;
  float ray_y = 0.0f;
  float ray_z = 0.0f;
  u32 camera_transform = 0;
  if (!ResolveActiveCameraTransform(guard, &camera_transform) ||
      !ReadTransformTranslation(guard, camera_transform, &ray_x, &ray_y, &ray_z))
  {
    if (!ReadTransformTranslation(guard, player + ADDRESS.transform_offset, &ray_x, &ray_y, &ray_z))
      return false;
  }

  float dir_x = 0.0f;
  float dir_y = 0.0f;
  float dir_z = 0.0f;
  if (!GunAimDirectionFromMatrix(hmd, &dir_x, &dir_y, &dir_z))
    return false;

  const float max_along = settings.gun_targeting_distance;
  bool found = false;

  for (const ScanTargetCandidate& candidate : candidates)
  {
    if (!TargetUidStillExists(guard, state_manager, candidate.uid, candidate.obj))
      continue;

    const RayCandidateMetrics metrics =
        ResolveScanActorRayMetrics(guard, candidate.obj, ray_x, ray_y, ray_z, dir_x, dir_y, dir_z,
                                   max_along, settings);
    if (!metrics.valid)
      continue;

    const float hmd_view_perp = ScanTargetAimConePerp(settings, metrics);
    if (metrics.perp > hmd_view_perp)
      continue;

    const float score = ScoreScanTarget(metrics, hmd_view_perp);
    if (!found || score < pick->score)
    {
      found = true;
      pick->uid = candidate.uid;
      pick->obj = candidate.obj;
      pick->ray_x = ray_x;
      pick->ray_y = ray_y;
      pick->ray_z = ray_z;
      pick->dir_x = dir_x;
      pick->dir_y = dir_y;
      pick->dir_z = dir_z;
      pick->x = metrics.x;
      pick->y = metrics.y;
      pick->z = metrics.z;
      pick->along = metrics.along;
      pick->perp = metrics.perp;
      pick->score = score;
      pick->has_scan = true;
    }
  }

  return found;
}

bool TryPreferPairedGrapplePoint(const Core::CPUThreadGuard& guard, u32 object_list, u32 player,
                                 u32 gun, float ray_x, float ray_y, float ray_z, float dir_x,
                                 float dir_y, float dir_z, float max_along, float max_perp,
                                 bool strict_lock_aim, GunTargetPick* pick)
{
  if (pick == nullptr || pick->grapple_point || pick->obj < 0x80000000u ||
      object_list < 0x80000000u)
  {
    return false;
  }

  float target_x = 0.0f;
  float target_y = 0.0f;
  float target_z = 0.0f;
  if (!ReadTransformTranslation(guard, pick->obj + ADDRESS.transform_offset, &target_x, &target_y,
                                &target_z))
  {
    return false;
  }

  bool found = false;
  GunTargetPick grapple_pick = {};
  grapple_pick.score = std::numeric_limits<float>::max();

  for (u32 i = 0; i < 1024u; ++i)
  {
    u32 obj = 0;
    if (!TryReadU32(guard, object_list + (i << 3) + 4u, &obj) || obj == player || obj == gun ||
        obj == pick->obj || !PrimeGameObjectPointerLooksValid(obj, ADDRESS.transform_offset + 0x30u))
    {
      continue;
    }

    if (!ActorIsGrapplePoint(guard, obj) || !ActorHasMaterial(guard, obj, 41) ||
        !LooksLikeTransformMatrix(guard, obj + ADDRESS.transform_offset))
    {
      continue;
    }

    float grapple_x = 0.0f;
    float grapple_y = 0.0f;
    float grapple_z = 0.0f;
    if (!ReadTransformTranslation(guard, obj + ADDRESS.transform_offset, &grapple_x, &grapple_y,
                                  &grapple_z))
    {
      continue;
    }

    const float dx = grapple_x - target_x;
    const float dy = grapple_y - target_y;
    const float dz = grapple_z - target_z;
    const float paired_dist_sq = dx * dx + dy * dy + dz * dz;
    if (!std::isfinite(paired_dist_sq) || paired_dist_sq > 2.25f)
      continue;

    const RayCandidateMetrics metrics =
        ResolveActorRayMetrics(guard, obj, ray_x, ray_y, ray_z, dir_x, dir_y, dir_z, max_along,
                               true);
    if (!metrics.valid || metrics.perp > max_perp)
      continue;

    float aim_cone_perp = max_perp;
    if (strict_lock_aim)
    {
      const float strict_cone = 0.45f + metrics.along * 0.075f;
      const float bounds_slack = std::min(metrics.radius * 0.60f, max_perp * 0.50f);
      aim_cone_perp = std::min(max_perp, strict_cone * 1.30f + bounds_slack);
      if (metrics.perp > aim_cone_perp)
        continue;
    }

    const float score = metrics.perp + metrics.along * 0.001f + paired_dist_sq * 0.25f;
    if (found && score >= grapple_pick.score)
      continue;

    u32 uid_word = 0;
    if (!TryReadU32(guard, obj + 0x8u, &uid_word))
      continue;

    const u16 uid = static_cast<u16>(uid_word >> 16);
    if (uid == 0xffffu)
      continue;

    found = true;
    grapple_pick = *pick;
    grapple_pick.uid = uid;
    grapple_pick.obj = obj;
    grapple_pick.x = metrics.x;
    grapple_pick.y = metrics.y;
    grapple_pick.z = metrics.z;
    grapple_pick.along = metrics.along;
    grapple_pick.perp = metrics.perp;
    grapple_pick.score = score;
    grapple_pick.has_target = false;
    grapple_pick.has_character = false;
    grapple_pick.has_orbit = true;
    grapple_pick.has_scan = false;
    grapple_pick.targetable = ActorIsTargetable(guard, obj);
    grapple_pick.grapple_point = true;
    grapple_pick.suppress_orbit_hook = false;
  }

  if (!found)
    return false;

  *pick = grapple_pick;
  return true;
}

bool PickGunRayTarget(const Core::CPUThreadGuard& guard, u32 state_manager, u32 player, u32 gun,
                      u32 world_xf, const Matrix3x4& mat, const RuntimeSettings& settings,
                      bool strict_lock_aim, GunTargetPick* pick)
{
  if (!settings.gun_targeting_enabled || state_manager < 0x80000000u ||
      !PlayerObjectLooksValid(guard, player) || gun < 0x80000000u)
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
  constexpr float combat_pick_radius_multiplier = 1.0f;
  constexpr float vanilla_orbit_pick_radius_multiplier = 1.75f;
  constexpr float scan_pick_radius_multiplier = 3.0f;
  const float grapple_max_perp = max_perp * 1.30f;
  const bool scan_mode = ScanVisorActive(guard, player);
  std::vector<ScanTargetCandidate> nearby_orbit_candidates;
  const bool have_nearby_orbit_candidates =
      !scan_mode && RefreshOrbitTargetCandidates(guard, state_manager, player,
                                                 PLAYER_NEARBY_ORBIT_OBJECTS_OFFSET,
                                                 &nearby_orbit_candidates);
  std::vector<ScanTargetCandidate> onscreen_orbit_candidates;
  const bool have_onscreen_orbit_candidates =
      !scan_mode && RefreshOrbitTargetCandidates(guard, state_manager, player,
                                                 PLAYER_ONSCREEN_ORBIT_OBJECTS_OFFSET,
                                                 &onscreen_orbit_candidates);
  bool found = false;

  for (u32 i = 0; i < 1024u; ++i)
  {
    u32 obj = 0;
    if (!TryReadU32(guard, object_list + (i << 3) + 4u, &obj) ||
        !PrimeGameObjectPointerLooksValid(obj, ADDRESS.transform_offset + 0x30u) ||
        obj == player || obj == gun)
    {
      continue;
    }

    if (!LooksLikeTransformMatrix(guard, obj + ADDRESS.transform_offset))
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
    const bool nearby_orbit_candidate =
        have_nearby_orbit_candidates && CandidateListContainsUid(nearby_orbit_candidates, uid);
    const bool onscreen_orbit_candidate =
        have_onscreen_orbit_candidates && CandidateListContainsUid(onscreen_orbit_candidates, uid);
    const bool orbit_candidate = has_orbit && targetable;
    const bool scan_candidate = has_scan;
    const bool grapple_candidate = grapple_point && has_orbit;
    const bool vanilla_orbit_candidate =
        !scan_mode && onscreen_orbit_candidate && orbit_candidate;
    const bool cannon_orbit_candidate =
        !scan_mode && orbit_candidate &&
        (has_scan || has_character || vanilla_orbit_candidate || nearby_orbit_candidate);

    const bool has_any_target_hint =
        has_target || has_orbit || scan_candidate || grapple_candidate;
    if (!has_any_target_hint)
      continue;

    const bool combat_candidate = has_target && (targetable || nearby_orbit_candidate);

    if (scan_mode && !scan_candidate)
      continue;

    if (!scan_mode && !combat_candidate && !cannon_orbit_candidate && !grapple_candidate)
      continue;

    if (scan_mode && !combat_candidate && !orbit_candidate && !scan_candidate && !grapple_candidate)
      continue;

    ActorAabb render_box = {};
    const bool has_render_bounds = ReadActorRenderAabb(guard, obj, &render_box);
    const bool allow_physics_aabb =
        scan_mode || grapple_candidate || cannon_orbit_candidate || has_render_bounds;
    if (!scan_mode && !grapple_candidate && !cannon_orbit_candidate && !has_render_bounds)
      continue;

    const bool target_only_helper_point =
        !scan_mode && combat_candidate && !has_character && !has_orbit && !grapple_candidate &&
        !nearby_orbit_candidate && !onscreen_orbit_candidate && has_render_bounds &&
        AabbMaxExtent(render_box) <= 0.05f;
    if (target_only_helper_point)
      continue;

    const RayCandidateMetrics metrics =
        ResolveActorRayMetrics(guard, obj, ray_x, ray_y, ray_z, dir_x, dir_y, dir_z, max_along,
                               allow_physics_aabb);
    if (!metrics.valid)
      continue;

    const float candidate_max_perp =
        scan_mode && scan_candidate ? max_perp * scan_pick_radius_multiplier :
        combat_candidate             ? max_perp * combat_pick_radius_multiplier :
        cannon_orbit_candidate       ? max_perp * vanilla_orbit_pick_radius_multiplier :
        grapple_candidate             ? grapple_max_perp :
                                       max_perp;
    if (metrics.perp > candidate_max_perp)
      continue;

    float aim_cone_perp = candidate_max_perp;
    if (strict_lock_aim && !scan_mode)
    {
      const float strict_cone = 0.45f + metrics.along * 0.075f;
      const float bounds_slack = std::min(metrics.radius * 0.60f, candidate_max_perp * 0.50f);
      const float strict_candidate_cone =
          grapple_candidate       ? strict_cone * 1.30f + bounds_slack :
          cannon_orbit_candidate  ? strict_cone * vanilla_orbit_pick_radius_multiplier + bounds_slack :
                                    strict_cone + bounds_slack;
      aim_cone_perp = std::min(candidate_max_perp, strict_candidate_cone);
      if (metrics.perp > aim_cone_perp)
        continue;
    }

    const float cone_fraction = metrics.perp / std::max(aim_cone_perp, 0.001f);
    float score = cone_fraction * 1.50f + metrics.perp * 0.35f +
                  metrics.along * 0.003f - metrics.radius * 0.015f;
    if (grapple_candidate)
      score -= 0.35f;
    if (vanilla_orbit_candidate)
      score -= 0.08f;
    if (scan_mode)
    {
      score -= 0.10f;
    }
    else
    {
      if (has_target)
        score -= 0.10f;
      else if (has_scan)
        score += 0.10f;
      else if (!has_character && !grapple_candidate)
        score += 0.15f;
      if (nearby_orbit_candidate)
        score -= 0.12f;
    }

    if (!found || score < pick->score)
    {
      found = true;
      pick->uid = uid;
      pick->obj = obj;
      pick->x = metrics.x;
      pick->y = metrics.y;
      pick->z = metrics.z;
      pick->along = metrics.along;
      pick->perp = metrics.perp;
      pick->score = score;
      pick->has_target = has_target;
      pick->has_character = has_character;
      pick->has_orbit = has_orbit;
      pick->has_scan = has_scan;
      pick->targetable = targetable;
      pick->grapple_point = grapple_point;
      pick->suppress_orbit_hook = false;
    }
  }

  if (found && !scan_mode && pick->has_target)
  {
    TryPreferPairedGrapplePoint(guard, object_list, player, gun, ray_x, ray_y, ray_z, dir_x, dir_y,
                                dir_z, max_along, grapple_max_perp, strict_lock_aim, pick);
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
  static u64 s_last_lock_log_frame = 0;
  static u16 s_last_lock_log_uid = 0xffffu;
  static int s_last_lock_log_source = -1;
  static bool s_last_lock_log_held = false;

  if (!settings.gun_targeting_enabled || !settings.patch_gun_ray_target ||
      !PlayerObjectLooksValid(guard, player))
  {
    WriteGunTargetScratch(guard, 0, 0xffffu);
    return;
  }

  const bool lock_held = OrbitLockButtonHeld(guard, state_manager);

  GunTargetPick pick = {};
  const bool found_live =
      PickGunRayTarget(guard, state_manager, player, gun, world_xf, mat, settings, lock_held, &pick);
  bool found = found_live;
  int lock_source = found_live ? 1 : 0;

  const auto log_lock_state = [&](int source, u16 uid, u32 obj, bool vanilla_found,
                                  bool wrote_target) {
    if (!LockLoggingEnabled())
      return;

    const bool transition = lock_held != s_last_lock_log_held ||
                            source != s_last_lock_log_source ||
                            uid != s_last_lock_log_uid;
    const bool periodic = lock_held && s_frame_counter >= s_last_lock_log_frame + 30u;
    if (!transition && !periodic)
      return;

    const char* source_name =
        source == 1 ? "live" :
        source == 3 ? "vanilla_fallback" :
        source == 4 ? "suppressed" :
                      "none";
    AppendLockDebugLine(fmt::format(
        "frame={} lock={} source={} wrote={} uid={:04X} obj={:08X} player={:08X} gun={:08X} "
        "found_live={} vanilla={} suppress={}",
        s_frame_counter, lock_held, source_name, wrote_target, uid, obj, player, gun, found_live,
        vanilla_found, pick.suppress_orbit_hook));

    s_last_lock_log_frame = s_frame_counter;
    s_last_lock_log_uid = uid;
    s_last_lock_log_source = source;
    s_last_lock_log_held = lock_held;
  };

  if (!found || pick.suppress_orbit_hook)
  {
    u16 vanilla_uid = 0xffffu;
    const bool vanilla_found = lock_held && ReadVanillaTargetUid(guard, state_manager, player, gun,
                                                                 &vanilla_uid);
    if (vanilla_found)
    {
      WriteGunTargetScratch(guard, player, vanilla_uid);
      log_lock_state(3, vanilla_uid, 0, true, true);
    }
    else
    {
      WriteGunTargetScratch(guard, player, 0xffffu);
      log_lock_state(pick.suppress_orbit_hook ? 4 : 0, 0xffffu, 0, false, false);
    }
    return;
  }

  WriteGunTargetScratch(guard, player, pick.uid);
  log_lock_state(lock_source, pick.uid, pick.obj, false, true);
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

float WrapDegrees(float angle)
{
  while (angle > 180.0f)
    angle -= 360.0f;
  while (angle < -180.0f)
    angle += 360.0f;
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
  u32 camera_manager = 0;
  u32 object_list = 0;
  if (!TryReadU32(guard, ADDRESS.state_manager + ADDRESS.player_offset, &player) ||
      !PlayerObjectLooksValid(guard, player) ||
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

void LogCameraWatchdog(const Core::CPUThreadGuard& guard, u32 player, bool force)
{
  if (!RuntimeLoggingEnabled())
    return;

  if (!force && s_frame_counter < s_last_camera_watchdog_frame + 60)
    return;
  s_last_camera_watchdog_frame = s_frame_counter;

  u32 camera_state = 0xffffffffu;
  u32 morph_state = 0xffffffffu;
  u32 movement_state = 0xffffffffu;
  u32 camera_manager = 0;
  u32 object_list = 0;
  u32 camera_uid_word = 0;
  u32 active_camera = 0;
  u32 active_transform = 0;
  float active_yaw = 0.0f;
  bool active_transform_ok = false;

  if (player >= 0x80000000u)
  {
    TryReadU32(guard, player + 0x2F4u, &camera_state);
    TryReadU32(guard, player + 0x2F8u, &morph_state);
    TryReadU32(guard, player + PLAYER_MOVEMENT_STATE_OFFSET, &movement_state);
  }

  TryReadU32(guard, ADDRESS.state_manager + ADDRESS.camera_manager_offset, &camera_manager);
  TryReadU32(guard, ADDRESS.state_manager + 0x810u, &object_list);
  if (camera_manager >= 0x80000000u)
    TryReadU32(guard, camera_manager, &camera_uid_word);
  if (camera_uid_word != 0 && object_list >= 0x80000000u)
  {
    const u32 camera_uid = (camera_uid_word >> 16) & 0xffffu;
    if (camera_uid != 0xffffu)
      TryReadU32(guard, object_list + ((camera_uid & 0x03ffu) << 3) + 4u, &active_camera);
  }
  if (ResolveActiveCameraTransform(guard, &active_transform))
    active_transform_ok = ReadYawFromTransform2D(guard, active_transform, &active_yaw);

  std::array<u32, 12> manager_words{};
  std::array<u32, 6> camera_slots{};
  if (camera_manager >= 0x80000000u)
  {
    for (u32 i = 0; i < manager_words.size(); ++i)
      TryReadU32(guard, camera_manager + i * 4u, &manager_words[i]);

    constexpr std::array<u32, 6> slot_offsets{0x10u, 0x80u, 0x84u, 0x88u, 0x8Cu, 0x90u};
    for (u32 i = 0; i < slot_offsets.size(); ++i)
      TryReadU32(guard, camera_manager + slot_offsets[i], &camera_slots[i]);
  }

  u32 active_vtable = 0;
  std::array<u32, 8> active_vtable_entries{};
  if (active_camera >= 0x80000000u && TryReadU32(guard, active_camera, &active_vtable) &&
      active_vtable >= 0x80000000u)
  {
    for (u32 i = 0; i < active_vtable_entries.size(); ++i)
      TryReadU32(guard, active_vtable + i * 4u, &active_vtable_entries[i]);
  }

  std::array<float, 12> active_xf{};
  if (active_transform >= 0x80000000u)
  {
    for (u32 i = 0; i < active_xf.size(); ++i)
      TryReadFloat(guard, active_transform + i * 4u, &active_xf[i]);
  }

  u32 legacy_camera_return = 0;
  u32 first_person_pitch_site = 0;
  u32 combat_pitch0_site = 0;
  u32 combat_pitch1_site = 0;
  u32 combat_pitch2_site = 0;
  TryReadU32(guard, LEGACY_MORPHBALL_CAMERA_RETURN_ADDRESS, &legacy_camera_return);
  TryReadU32(guard, 0x8000E548u, &first_person_pitch_site);
  TryReadU32(guard, 0x8000E7B4u, &combat_pitch0_site);
  TryReadU32(guard, 0x8000E808u, &combat_pitch1_site);
  TryReadU32(guard, 0x8000E83Cu, &combat_pitch2_site);

  NOTICE_LOG_FMT(CORE,
                 "PrimedGun camera_watch player={:08X} camera_state={} morph_state={} "
                 "move_state={} camera_mgr={:08X} uid_word={:08X} object_list={:08X} "
                 "active_camera={:08X} active_xf={:08X} xf_ok={} yaw={:.3f}",
                 player, camera_state, morph_state, movement_state, camera_manager,
                 camera_uid_word, object_list, active_camera, active_transform, active_transform_ok,
                 active_yaw);
  NOTICE_LOG_FMT(CORE,
                 "PrimedGun camera_watch manager+000 {:08X} {:08X} {:08X} {:08X} {:08X} "
                 "{:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                 manager_words[0], manager_words[1], manager_words[2], manager_words[3],
                 manager_words[4], manager_words[5], manager_words[6], manager_words[7],
                 manager_words[8], manager_words[9], manager_words[10], manager_words[11]);
  NOTICE_LOG_FMT(CORE,
                 "PrimedGun camera_watch slots +10={:08X} +80={:08X} +84={:08X} "
                 "+88={:08X} +8C={:08X} +90={:08X} active_vtable={:08X}",
                 camera_slots[0], camera_slots[1], camera_slots[2], camera_slots[3],
                 camera_slots[4], camera_slots[5], active_vtable);
  NOTICE_LOG_FMT(CORE,
                 "PrimedGun camera_watch active_vtable {:08X} {:08X} {:08X} {:08X} "
                 "{:08X} {:08X} {:08X} {:08X}",
                 active_vtable_entries[0], active_vtable_entries[1], active_vtable_entries[2],
                 active_vtable_entries[3], active_vtable_entries[4], active_vtable_entries[5],
                 active_vtable_entries[6], active_vtable_entries[7]);
  NOTICE_LOG_FMT(CORE,
                 "PrimedGun camera_watch active_xf rows ({:.4f},{:.4f},{:.4f},{:.4f}) "
                 "({:.4f},{:.4f},{:.4f},{:.4f}) ({:.4f},{:.4f},{:.4f},{:.4f})",
                 active_xf[0], active_xf[1], active_xf[2], active_xf[3], active_xf[4],
                 active_xf[5], active_xf[6], active_xf[7], active_xf[8], active_xf[9],
                 active_xf[10], active_xf[11]);
  NOTICE_LOG_FMT(CORE,
                 "PrimedGun camera_watch asm camera_return={:08X} first_pitch={:08X} "
                 "combat_pitch={:08X}/{:08X}/{:08X}",
                 legacy_camera_return, first_person_pitch_site, combat_pitch0_site,
                 combat_pitch1_site, combat_pitch2_site);
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

bool RotateTransformYaw2D(const Core::CPUThreadGuard& guard, u32 transform, float yaw_delta_rad)
{
  if (std::fabs(yaw_delta_rad) < 0.000001f)
    return false;

  float m00 = 0.0f;
  float m01 = 0.0f;
  float m10 = 0.0f;
  float m11 = 0.0f;
  if (!TryReadFloat(guard, transform + 0x00u, &m00) ||
      !TryReadFloat(guard, transform + 0x04u, &m01) ||
      !TryReadFloat(guard, transform + 0x10u, &m10) ||
      !TryReadFloat(guard, transform + 0x14u, &m11))
  {
    return false;
  }

  if (!std::isfinite(m00) || !std::isfinite(m01) || !std::isfinite(m10) || !std::isfinite(m11))
    return false;

  const float c = std::cos(yaw_delta_rad);
  const float s = std::sin(yaw_delta_rad);
  const float new_m00 = c * m00 - s * m01;
  const float new_m01 = s * m00 + c * m01;
  const float new_m10 = c * m10 - s * m11;
  const float new_m11 = s * m10 + c * m11;
  return TryWriteFloat(guard, transform + 0x00u, new_m00) &&
         TryWriteFloat(guard, transform + 0x04u, new_m01) &&
         TryWriteFloat(guard, transform + 0x10u, new_m10) &&
         TryWriteFloat(guard, transform + 0x14u, new_m11);
}

bool FlattenCameraTransformPitch(const Core::CPUThreadGuard& guard, u32 transform)
{
  if (!PrimeDataPointerLooksValid(transform, 0x30u))
  {
    return false;
  }

  float right_x = 0.0f;
  float right_y = 0.0f;
  float right_z = 0.0f;
  float forward_z = 0.0f;
  float up_x = 0.0f;
  float up_y = 0.0f;
  float up_z = 0.0f;
  if (!TryReadFloat(guard, transform + 0x00u, &right_x) ||
      !TryReadFloat(guard, transform + 0x04u, &right_y) ||
      !TryReadFloat(guard, transform + 0x08u, &right_z) ||
      !TryReadFloat(guard, transform + 0x18u, &forward_z) ||
      !TryReadFloat(guard, transform + 0x20u, &up_x) ||
      !TryReadFloat(guard, transform + 0x24u, &up_y) ||
      !TryReadFloat(guard, transform + 0x28u, &up_z))
  {
    return false;
  }

  if (!std::isfinite(right_x) || !std::isfinite(right_y) || !std::isfinite(right_z) ||
      !std::isfinite(forward_z) || !std::isfinite(up_x) || !std::isfinite(up_y) ||
      !std::isfinite(up_z))
  {
    return false;
  }

  const float right_len = std::sqrt(right_x * right_x + right_y * right_y);
  if (!std::isfinite(right_len) || right_len < 0.25f || right_len > 1.25f)
    return false;

  if (std::fabs(right_z) < 0.0005f && std::fabs(forward_z) < 0.0005f &&
      std::fabs(up_x) < 0.0005f && std::fabs(up_y) < 0.0005f &&
      std::fabs(up_z - 1.0f) < 0.0005f)
  {
    return false;
  }

  right_x /= right_len;
  right_y /= right_len;
  const float forward_x = -right_y;
  const float forward_y = right_x;

  return TryWriteFloat(guard, transform + 0x00u, right_x) &&
         TryWriteFloat(guard, transform + 0x04u, right_y) &&
         TryWriteFloat(guard, transform + 0x08u, 0.0f) &&
         TryWriteFloat(guard, transform + 0x10u, forward_x) &&
         TryWriteFloat(guard, transform + 0x14u, forward_y) &&
         TryWriteFloat(guard, transform + 0x18u, 0.0f) &&
         TryWriteFloat(guard, transform + 0x20u, 0.0f) &&
         TryWriteFloat(guard, transform + 0x24u, 0.0f) &&
         TryWriteFloat(guard, transform + 0x28u, 1.0f);
}

bool FlattenCameraObjectPitch(const Core::CPUThreadGuard& guard, u32 camera)
{
  if (!PrimeGameObjectPointerLooksValid(camera, ADDRESS.transform_offset + 0x30u))
    return false;

  return FlattenCameraTransformPitch(guard, camera + ADDRESS.transform_offset);
}

bool FlattenActiveMorphballCameraTransform(const Core::CPUThreadGuard& guard, u32 player)
{
  u32 morph_state = 0xffffffffu;
  if (player < 0x80000000u || !TryReadU32(guard, player + 0x2F8u, &morph_state) ||
      morph_state == 0)
  {
    return false;
  }

  bool wrote = false;
  u32 active_transform = 0;
  if (ResolveActiveCameraTransform(guard, &active_transform))
    wrote = FlattenCameraTransformPitch(guard, active_transform) || wrote;

  u32 camera_manager = 0;
  if (!TryReadU32(guard, ADDRESS.state_manager + ADDRESS.camera_manager_offset, &camera_manager) ||
      camera_manager < 0x80000000u)
  {
    return wrote;
  }

  for (const u32 slot_offset : {0x80u, 0x88u})
  {
    u32 camera = 0;
    if (TryReadU32(guard, camera_manager + slot_offset, &camera))
      wrote = FlattenCameraObjectPitch(guard, camera) || wrote;
  }

  return wrote;
}

void ClearFreeLookYawCarry(const Core::CPUThreadGuard& guard, u32 player)
{
  if (player < 0x80000000u)
    return;

  TryWriteFloat(guard, player + PLAYER_FREE_LOOK_CENTER_TIME_OFFSET, 0.0f);
  TryWriteFloat(guard, player + PLAYER_FREE_LOOK_YAW_ANGLE_OFFSET, 0.0f);
  TryWriteFloat(guard, player + PLAYER_FREE_LOOK_YAW_VEL_OFFSET, 0.0f);
}

void ApplyLookYawSensitivityBoost(const Core::CPUThreadGuard& guard,
                                  const Common::VR::OpenXRInputSnapshot& snapshot,
                                  const RuntimeSettings& settings, u32 player)
{
  if (player < 0x80000000u || settings.snap_turn_enabled ||
      !PlayerIsFirstPersonUnmorphed(guard, player))
  {
    return;
  }

  const bool orbit_lock_active = OrbitLockButtonHeld(guard, ADDRESS.state_manager) &&
                                 PlayerHasOrbitControlTarget(guard, ADDRESS.state_manager, player);
  if (orbit_lock_active)
  {
    ClearFreeLookYawCarry(guard, player);
    return;
  }

  if (settings.look_yaw_sensitivity <= 1.0f)
    return;

  const int look_hand = settings.directional_movement_use_right_stick ? 0 : 1;
  const Common::VR::OpenXRControllerState& look = snapshot.controllers[look_hand];
  if (!look.connected)
    return;

  const float stick_x = std::clamp(look.thumbstick_x, -1.0f, 1.0f);
  if (std::fabs(stick_x) < 0.05f)
    return;

  constexpr float max_extra_yaw_deg_per_second = 125.0f;
  constexpr float frame_dt = 1.0f / 60.0f;
  const float yaw_scale = std::clamp(settings.look_yaw_sensitivity - 1.0f, 0.0f, 2.0f);
  const float yaw_delta_rad = stick_x * yaw_scale * max_extra_yaw_deg_per_second * frame_dt *
                              (static_cast<float>(MathUtil::PI) / 180.0f);
  RotateTransformYaw2D(guard, player + ADDRESS.transform_offset, yaw_delta_rad);
}

int SnapTurnStepDegrees(int current, int direction)
{
  int closest_index = 1;
  int closest_delta = std::numeric_limits<int>::max();
  for (int i = 0; i < static_cast<int>(SNAP_TURN_DEGREES_CHOICES.size()); ++i)
  {
    const int delta = std::abs(current - SNAP_TURN_DEGREES_CHOICES[i]);
    if (delta < closest_delta)
    {
      closest_delta = delta;
      closest_index = i;
    }
  }

  const int next_index = std::clamp(closest_index + direction, 0,
                                    static_cast<int>(SNAP_TURN_DEGREES_CHOICES.size()) - 1);
  return SNAP_TURN_DEGREES_CHOICES[next_index];
}

bool UpdateSnapTurn(const Core::CPUThreadGuard& guard,
                    const Common::VR::OpenXRInputSnapshot& snapshot,
                    const RuntimeSettings& settings, float* applied_yaw_deg)
{
  if (applied_yaw_deg != nullptr)
    *applied_yaw_deg = 0.0f;

  if (!settings.snap_turn_enabled || !snapshot.runtime_active ||
      s_frame_counter < s_vr_menu_input_suppress_until_frame || s_vr_menu_visible)
  {
    if (!settings.snap_turn_enabled)
      s_snap_turn_ready = true;
    return false;
  }

  constexpr size_t right_hand_index = 1;
  const Common::VR::OpenXRControllerState& controller = snapshot.controllers[right_hand_index];
  if (!controller.connected)
  {
    s_snap_turn_ready = true;
    s_snap_turn_cooldown_until_frame = 0;
    return false;
  }

  constexpr float snap_enter_threshold = 0.72f;
  constexpr float snap_exit_threshold = 0.32f;
  const float stick_x = std::clamp(controller.thumbstick_x, -1.0f, 1.0f);
  if (std::fabs(stick_x) <= snap_exit_threshold)
  {
    s_snap_turn_ready = true;
    return false;
  }

  if (snapshot.head_pose.valid && controller.aim_pose.valid &&
      LeftControllerNearHead(PoseFromOpenXR(controller.aim_pose), PoseFromOpenXR(snapshot.head_pose),
                             settings))
  {
    s_snap_turn_ready = false;
    return false;
  }

  if (!s_snap_turn_ready || std::fabs(stick_x) < snap_enter_threshold ||
      s_frame_counter < s_snap_turn_cooldown_until_frame)
  {
    return false;
  }

  // Consume the flick even if gameplay is currently unavailable, so holding the stick through
  // a menu, loading transition, or invalid player state cannot snap on the next valid frame.
  s_snap_turn_ready = false;

  u32 player = 0;
  if (!TryReadU32(guard, ADDRESS.state_manager + ADDRESS.player_offset, &player) ||
      player < 0x80000000u || !PlayerIsFirstPersonUnmorphed(guard, player) ||
      PlayerIsChangingVisorsInFirstPerson(guard, player))
  {
    return false;
  }

  const int degrees =
      std::clamp(settings.snap_turn_degrees, SNAP_TURN_DEGREES_CHOICES.front(),
                 SNAP_TURN_DEGREES_CHOICES.back());
  const float signed_degrees = (stick_x > 0.0f ? 1.0f : -1.0f) * static_cast<float>(degrees);
  const float yaw_delta_rad = signed_degrees * (static_cast<float>(MathUtil::PI) / 180.0f);
  if (RotateTransformYaw2D(guard, player + ADDRESS.transform_offset, yaw_delta_rad))
  {
    s_snap_turn_cooldown_until_frame = s_frame_counter + 8;
    s_cannon_smoothing_bypass_until_frame = s_frame_counter + 10;
    s_smooth_matrix_valid = false;
    if (applied_yaw_deg != nullptr)
      *applied_yaw_deg = signed_degrees;
    return true;
  }
  return false;
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

bool IsQuestTouchPlusProfile(std::string_view profile)
{
  return profile.find("/interaction_profiles/meta/touch_controller_plus") != std::string_view::npos;
}

bool QuestTouchPlusThumbrestModifierActive(const Common::VR::OpenXRInputSnapshot& snapshot)
{
  for (std::size_t hand = 0; hand < snapshot.controllers.size(); ++hand)
  {
    if (snapshot.controllers[hand].thumbrest_touch &&
        IsQuestTouchPlusProfile(snapshot.interaction_profiles[hand]))
    {
      return true;
    }
  }

  return false;
}

void ClearDpadDisableOwnershipScratch(const Core::CPUThreadGuard& guard)
{
  TryWriteU32(guard, DPAD_DISABLE_OWNER_SCRATCH, 0);
  TryWriteU32(guard, DPAD_DISABLE_FLAGS_ADDR_SCRATCH, 0);
  TryWriteU32(guard, DPAD_DISABLE_ORIGINAL_FLAGS_SCRATCH, 0);
}

void MarkDpadDisableOwnershipScratch(const Core::CPUThreadGuard& guard, u32 flags_addr,
                                     u8 original_flags)
{
  TryWriteU32(guard, DPAD_DISABLE_OWNER_SCRATCH, DPAD_DISABLE_OWNER_MAGIC);
  TryWriteU32(guard, DPAD_DISABLE_FLAGS_ADDR_SCRATCH, flags_addr);
  TryWriteU32(guard, DPAD_DISABLE_ORIGINAL_FLAGS_SCRATCH, original_flags);
}

bool RestoreDpadDisableAt(const Core::CPUThreadGuard& guard, u32 flags_addr, u8 original_flags)
{
  if (!IsWritablePrimeMem1Address(flags_addr, sizeof(u8)))
    return false;

  if (flags_addr < PLAYER_DISABLE_INPUT_FLAGS_OFFSET ||
      !PlayerObjectLooksValid(guard, flags_addr - PLAYER_DISABLE_INPUT_FLAGS_OFFSET))
  {
    return false;
  }

  u8 flags = 0;
  if (!TryReadU8(guard, flags_addr, &flags))
    return false;

  if ((original_flags & PLAYER_DISABLE_INPUT_MASK) != 0)
    return true;

  return TryWriteU8(guard, flags_addr, flags & static_cast<u8>(~PLAYER_DISABLE_INPUT_MASK));
}

void ClearDpadDisableTracking()
{
  s_dpad_forced_input_disabled = false;
  s_dpad_input_was_disabled = false;
  s_dpad_input_player = 0;
  s_dpad_input_flags_addr = 0;
  s_dpad_last_disable_refresh_frame = 0;
}

void RestoreTrackedDpadDisable(const Core::CPUThreadGuard& guard)
{
  if (s_dpad_forced_input_disabled && s_dpad_input_flags_addr != 0)
  {
    RestoreDpadDisableAt(guard, s_dpad_input_flags_addr,
                         s_dpad_input_was_disabled ? PLAYER_DISABLE_INPUT_MASK : 0);
  }

  ClearDpadDisableTracking();
  ClearDpadDisableOwnershipScratch(guard);
}

void RestorePersistedDpadDisableIfIdle(const Core::CPUThreadGuard& guard)
{
  u32 owner = 0;
  u32 flags_addr = 0;
  u32 original_flags = 0;
  if (!TryReadU32(guard, DPAD_DISABLE_OWNER_SCRATCH, &owner) ||
      owner != DPAD_DISABLE_OWNER_MAGIC ||
      !TryReadU32(guard, DPAD_DISABLE_FLAGS_ADDR_SCRATCH, &flags_addr) ||
      !TryReadU32(guard, DPAD_DISABLE_ORIGINAL_FLAGS_SCRATCH, &original_flags))
  {
    return;
  }

  RestoreDpadDisableAt(guard, flags_addr, static_cast<u8>(original_flags));
  ClearDpadDisableOwnershipScratch(guard);
}

void SetPlayerInputDisabledForDpad(const Core::CPUThreadGuard& guard, u32 state_manager,
                                   bool disabled)
{
  if (!disabled)
    RestorePersistedDpadDisableIfIdle(guard);

  u32 player = 0;
  const bool have_player =
      TryReadU32(guard, state_manager + ADDRESS.player_offset, &player) &&
      PlayerObjectLooksValid(guard, player);

  if (disabled && s_dpad_forced_input_disabled && have_player &&
      player != s_dpad_input_player)
  {
    RestoreTrackedDpadDisable(guard);
  }

  if (disabled && s_dpad_forced_input_disabled && have_player &&
      player == s_dpad_input_player && s_dpad_input_flags_addr == player + PLAYER_DISABLE_INPUT_FLAGS_OFFSET)
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

  if (!have_player)
  {
    if (!disabled)
      RestoreTrackedDpadDisable(guard);
    return;
  }

  const u32 flags_addr = player + PLAYER_DISABLE_INPUT_FLAGS_OFFSET;
  u8 flags = 0;
  if (!TryReadU8(guard, flags_addr, &flags))
  {
    if (!disabled)
      RestoreTrackedDpadDisable(guard);
    return;
  }

  if (disabled)
  {
    if (!s_dpad_forced_input_disabled)
    {
      s_dpad_input_was_disabled = (flags & PLAYER_DISABLE_INPUT_MASK) != 0;
      s_dpad_forced_input_disabled = true;
    }

    TryWriteU8(guard, flags_addr, flags | PLAYER_DISABLE_INPUT_MASK);
    s_dpad_input_player = player;
    s_dpad_input_flags_addr = flags_addr;
    s_dpad_last_disable_refresh_frame = s_frame_counter;
    MarkDpadDisableOwnershipScratch(guard, flags_addr, flags);
    return;
  }

  RestoreTrackedDpadDisable(guard);
}

void SuppressCStickForDpad(const Core::CPUThreadGuard& guard, u32 state_manager)
{
  u32 player = 0;
  if (!TryReadU32(guard, state_manager + ADDRESS.player_offset, &player) ||
      !PlayerObjectLooksValid(guard, player) || ScanVisorActive(guard, player))
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
  Pose pose{
      xr_pose.position[0],
      xr_pose.position[1],
      xr_pose.position[2],
      xr_pose.orientation[0],
      xr_pose.orientation[1],
      xr_pose.orientation[2],
      xr_pose.orientation[3],
      xr_pose.valid,
  };
  pose.valid = PoseNumbersLookValid(pose);
  return pose;
}

void UpdateReticleBillboard(const Core::CPUThreadGuard& guard,
                            const Common::VR::OpenXRInputSnapshot& snapshot, u32 player,
                            float yaw_delta_deg)
{
  const ScanVisorInfo scan_info = ReadScanVisorInfo(guard, player);
  if (!snapshot.head_pose.valid)
  {
    TryWriteU32(guard, RETICLE_BILLBOARD_SCRATCH, 0);
    return;
  }

  Pose adjusted_hmd = PoseFromOpenXR(snapshot.head_pose);
  ApplyTrackingWorldYaw(adjusted_hmd, yaw_delta_deg);

  const Matrix3x4 hmd =
      PoseToPrimeMatrix(adjusted_hmd, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
  Matrix3x4 billboard_basis = hmd;
  if (scan_info.active)
    RemoveRollFromAimBasis(&billboard_basis);

  if (!WriteBasis9(guard, RETICLE_BILLBOARD_SCRATCH + 4, billboard_basis))
  {
    TryWriteU32(guard, RETICLE_BILLBOARD_SCRATCH, 0);
    return;
  }

  TryWriteU32(guard, RETICLE_BILLBOARD_SCRATCH, 1);
}

bool HmdPitchFromSnapshot(const Common::VR::OpenXRInputSnapshot& snapshot, float yaw_delta_deg,
                          float* pitch_out)
{
  if (!snapshot.head_pose.valid)
    return false;

  Pose adjusted_hmd = PoseFromOpenXR(snapshot.head_pose);
  ApplyTrackingWorldYaw(adjusted_hmd, yaw_delta_deg);

  const Matrix3x4 hmd =
      PoseToPrimeMatrix(adjusted_hmd, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
  return GunPitchFromMatrix(hmd, pitch_out);
}

bool MatrixFromHmdSnapshot(const Common::VR::OpenXRInputSnapshot& snapshot, float yaw_delta_deg,
                           Matrix3x4* hmd)
{
  if (!snapshot.head_pose.valid)
    return false;

  Pose adjusted_hmd = PoseFromOpenXR(snapshot.head_pose);
  ApplyTrackingWorldYaw(adjusted_hmd, yaw_delta_deg);
  *hmd = PoseToPrimeMatrix(adjusted_hmd, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
  return true;
}

void ClearAudioListenerScratch(const Core::CPUThreadGuard& guard)
{
  for (u32 offset = 0; offset <= 0x18u; offset += 4u)
    TryWriteU32(guard, AUDIO_LISTENER_SCRATCH + offset, 0);
}

bool WriteAudioListenerScratch(const Core::CPUThreadGuard& guard, const Matrix3x4& hmd)
{
  if (!MatrixNumbersLookValid(hmd))
    return false;

  const bool wrote_vectors =
      // Prime's UpdateAudioListener passes camera column Y as listener heading.
      TryWriteFloat(guard, AUDIO_LISTENER_SCRATCH + 0x04u, hmd.At(0, 1)) &&
      TryWriteFloat(guard, AUDIO_LISTENER_SCRATCH + 0x08u, hmd.At(1, 1)) &&
      TryWriteFloat(guard, AUDIO_LISTENER_SCRATCH + 0x0Cu, hmd.At(2, 1)) &&
      // Prime passes camera column Z as listener up.
      TryWriteFloat(guard, AUDIO_LISTENER_SCRATCH + 0x10u, hmd.At(0, 2)) &&
      TryWriteFloat(guard, AUDIO_LISTENER_SCRATCH + 0x14u, hmd.At(1, 2)) &&
      TryWriteFloat(guard, AUDIO_LISTENER_SCRATCH + 0x18u, hmd.At(2, 2));
  if (!wrote_vectors)
  {
    TryWriteU32(guard, AUDIO_LISTENER_SCRATCH, 0);
    return false;
  }

  return TryWriteU32(guard, AUDIO_LISTENER_SCRATCH, 1);
}

void UpdateAudioListenerScratchFromHmd(const Core::CPUThreadGuard& guard,
                                       const Common::VR::OpenXRInputSnapshot& snapshot,
                                       float yaw_delta_deg)
{
  Matrix3x4 hmd{};
  if (!snapshot.runtime_active || !snapshot.head_pose.valid ||
      !MatrixFromHmdSnapshot(snapshot, yaw_delta_deg, &hmd) ||
      !WriteAudioListenerScratch(guard, hmd))
  {
    ClearAudioListenerScratch(guard);
  }
}

bool ScanPitchFieldsWritable(const Core::CPUThreadGuard& guard, u32 player)
{
  if (!PlayerObjectLooksValid(guard, player))
    return false;

  float pitch = 0.0f;
  float pitch_vel = 0.0f;
  u8 free_look_state = 0;
  u8 free_look_pitch_state = 0;
  return TryReadFloat(guard, player + PLAYER_FREE_LOOK_PITCH_ANGLE_OFFSET, &pitch) &&
         TryReadFloat(guard, player + 0x3F0u, &pitch_vel) &&
         TryReadU8(guard, player + PLAYER_FREE_LOOK_STATE_OFFSET, &free_look_state) &&
         TryReadU8(guard, player + 0x3DEu, &free_look_pitch_state);
}

void ClearScanPitchState(const Core::CPUThreadGuard& guard, u32 player)
{
  if (!ScanPitchFieldsWritable(guard, player))
    return;

  TryWriteU8(guard, player + PLAYER_FREE_LOOK_STATE_OFFSET + 2, 0);
  TryWriteFloat(guard, player + PLAYER_FREE_LOOK_CENTER_TIME_OFFSET, 0.0f);
  TryWriteFloat(guard, player + PLAYER_FREE_LOOK_PITCH_ANGLE_OFFSET, 0.0f);
  TryWriteFloat(guard, player + 0x3F0u, 0.0f);
}

void UpdateScanPitch(const Core::CPUThreadGuard& guard,
                     const Common::VR::OpenXRInputSnapshot& snapshot, u32 player,
                     float yaw_delta_deg)
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
    if (!HmdPitchFromSnapshot(snapshot, yaw_delta_deg, &pitch))
      return;

    constexpr float max_scan_pitch = 1.2217305f;  // 70 degrees
    pitch = std::clamp(pitch, -max_scan_pitch, max_scan_pitch);
    constexpr float pitch_smooth = 0.45f;
    s_smooth_scan_pitch = s_smooth_scan_pitch * pitch_smooth + pitch * (1.0f - pitch_smooth);
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
    ++s_scan_normal_frames;
  }

  s_scan_was_active = scan_active;
}

void UpdateScanTargetingFromHmd(const Core::CPUThreadGuard& guard,
                                const Common::VR::OpenXRInputSnapshot& snapshot, u32 state_manager,
                                u32 player, const RuntimeSettings& settings, float yaw_delta_deg)
{
  static u64 s_last_scan_candidate_log_frame = 0;
  static u16 s_last_scan_candidate_uid = 0xffffu;
  static bool s_last_scan_candidate_found = false;

  const ScanVisorInfo scan_info = ReadScanVisorInfo(guard, player);
  if (!scan_info.active)
  {
    WriteGunTargetScratch(guard, player, 0xffffu);
    return;
  }

  Matrix3x4 hmd = {};
  if (!MatrixFromHmdSnapshot(snapshot, yaw_delta_deg, &hmd))
  {
    WriteGunTargetScratch(guard, player, 0xffffu);
    return;
  }

  u32 visor = 0;
  u32 visor_valid = 0;
  const bool have_visor =
      TryReadU32(guard, SCAN_RETICLE_TRACE_SCRATCH + 0x20u, &visor) &&
      TryReadU32(guard, SCAN_RETICLE_TRACE_SCRATCH + 0x24u, &visor_valid) && visor_valid == 1u;
  GunTargetPick marker_pick = {};
  const bool marker_found =
      have_visor &&
      SeedScanIndicatorTargetsFromHmd(guard, state_manager, player, visor, hmd, settings,
                                      &marker_pick);

  GunTargetPick pick = {};
  bool found = false;
  if (marker_found)
  {
    pick = marker_pick;
    found = true;
  }
  else
  {
    found = PickScanTargetFromHmd(guard, state_manager, player, hmd, settings, &pick);
  }

  if (found)
  {
    if (RuntimeLoggingEnabled() &&
        (pick.uid != s_last_scan_candidate_uid || !s_last_scan_candidate_found ||
         s_frame_counter >= s_last_scan_candidate_log_frame + 30u))
    {
      AppendScanDebugLine(fmt::format(
          "PrimedGun scan_candidate frame={} player={:08X} state={:08X} current_visor={} "
          "transition_visor={} proxy_visor={} real_state={} uid={:04X} obj={:08X} score={:.3f} "
          "along={:.3f} perp={:.3f} scan={} target={} orbit={} character={} targetable={} grapple={}",
          s_frame_counter, player, scan_info.player_state, scan_info.current_visor,
          scan_info.transition_visor, scan_info.proxy_visor, scan_info.real_state, pick.uid,
          pick.obj, pick.score, pick.along, pick.perp, pick.has_scan, pick.has_target,
          pick.has_orbit, pick.has_character, pick.targetable, pick.grapple_point));
      NOTICE_LOG_FMT(CORE,
                     "PrimedGun scan_candidate selected uid={:04X} obj={:08X} score={:.3f} "
                     "along={:.3f} perp={:.3f} pos=({:.3f},{:.3f},{:.3f}) "
                     "scan={} target={} orbit={} character={} targetable={} grapple={}",
                     pick.uid, pick.obj, pick.score, pick.along, pick.perp, pick.x, pick.y,
                     pick.z, pick.has_scan, pick.has_target, pick.has_orbit, pick.has_character,
                     pick.targetable, pick.grapple_point);
      s_last_scan_candidate_log_frame = s_frame_counter;
      s_last_scan_candidate_uid = pick.uid;
      s_last_scan_candidate_found = true;
    }
    WriteGunTargetScratch(guard, player, pick.uid);
    if (have_visor)
      EnsureUidInScanVisorTargets(guard, visor, pick.uid);
  }
  else
  {
    if (RuntimeLoggingEnabled() &&
        (s_last_scan_candidate_found || s_frame_counter >= s_last_scan_candidate_log_frame + 30u))
    {
      AppendScanDebugLine(fmt::format(
          "PrimedGun scan_candidate frame={} player={:08X} state={:08X} current_visor={} "
          "transition_visor={} proxy_visor={} real_state={} uid=FFFF obj=00000000 none",
          s_frame_counter, player, scan_info.player_state, scan_info.current_visor,
          scan_info.transition_visor, scan_info.proxy_visor, scan_info.real_state));
      NOTICE_LOG_FMT(CORE, "PrimedGun scan_candidate selected none");
      s_last_scan_candidate_log_frame = s_frame_counter;
      s_last_scan_candidate_uid = 0xffffu;
      s_last_scan_candidate_found = false;
    }
    WriteGunTargetScratch(guard, player, 0xffffu);
  }
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

  u32 player = 0;
  if (!TryReadU32(guard, ADDRESS.state_manager + ADDRESS.player_offset, &player) ||
      !PlayerIsFirstPersonUnmorphed(guard, player))
  {
    disarm();
    return;
  }

  const float deadzone = std::min(settings.xr_dpad_deadzone, 0.25f);
  DpadDir dir = DpadNone;
  bool dpad_modifier_active = false;
  const Common::VR::OpenXRControllerState& dpad_hand =
      snapshot.controllers[settings.use_right_hand ? 0 : 1];

  if (settings.xr_dpad_use_thumbrest_modifier)
  {
    if (!dpad_hand.connected || !QuestTouchPlusThumbrestModifierActive(snapshot))
    {
      disarm();
      return;
    }

    dpad_modifier_active = true;
    dir = StickToDpad(dpad_hand.thumbstick_x, dpad_hand.thumbstick_y, deadzone, s_last_dir);
  }
  else
  {
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

    dpad_modifier_active = in_head_zone;
    dir = StickToDpad(dpad_hand.thumbstick_x, dpad_hand.thumbstick_y, deadzone, s_last_dir);
  }

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

  SetPlayerInputDisabledForDpad(guard, ADDRESS.state_manager, dpad_modifier_active);
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
  if (settings.xr_dpad_enabled && settings.xr_dpad_use_thumbrest_modifier &&
      QuestTouchPlusThumbrestModifierActive(snapshot))
  {
    s_directional_move_speed = 0.0f;
    return;
  }

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

  if (OrbitLockButtonHeld(guard, ADDRESS.state_manager) &&
      PlayerHasOrbitControlTarget(guard, ADDRESS.state_manager, player))
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
std::string PrimedGunCannonActiveFolder()
{
  return File::GetUserPath(D_HIRESTEXTURES_IDX) + PRIMEGUN_CANNON_PACK_FOLDER + DIR_SEP;
}

std::string PrimedGunCannonLibraryFolderForSlot(u32 slot)
{
  return File::GetUserPath(D_LOAD_IDX) + PRIMEGUN_CANNON_LIBRARY_FOLDER + DIR_SEP + "slot_" +
         std::to_string(slot) + DIR_SEP;
}

std::string PrimedGunCannonAppLibraryFolderForSlot(u32 slot)
{
  return (std::filesystem::path(File::GetSysDirectory()) / ".." / PORTABLE_USER_DIR / LOAD_DIR /
          PRIMEGUN_CANNON_LIBRARY_FOLDER / ("slot_" + std::to_string(slot)))
             .lexically_normal()
             .string() +
         DIR_SEP;
}

std::string PrimedGunCannonActivePath(std::string_view texture_name, std::string_view extension)
{
  return PrimedGunCannonActiveFolder() + std::string(texture_name) + std::string(extension);
}

std::string PrimedGunCannonSourcePath(u32 slot, std::string_view texture_name)
{
  for (const std::string& slot_folder :
       {PrimedGunCannonLibraryFolderForSlot(slot), PrimedGunCannonAppLibraryFolderForSlot(slot)})
  {
    const std::string dds_path = slot_folder + std::string(texture_name) + ".dds";
    if (File::Exists(dds_path))
      return dds_path;

    const std::string png_path = slot_folder + std::string(texture_name) + ".png";
    if (File::Exists(png_path))
      return png_path;
  }

  return {};
}

void PrimedGunCannonRegisterPack()
{
  const std::string active_folder = PrimedGunCannonActiveFolder();
  File::CreateDirs(active_folder);
  const std::string gameids_folder = active_folder + "gameids" DIR_SEP;
  File::CreateDirs(gameids_folder);
  const std::string gameids_path =
      gameids_folder + std::string(PRIMEGUN_CANNON_GAME_ID) + ".txt";
  if (!File::Exists(gameids_path))
    File::CreateEmptyFile(gameids_path);
}

void PrimedGunCannonRefreshHiresMap(
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

bool ApplyPrimedGunCannonTextureSlot(u32 slot)
{
  PrimedGunCannonRegisterPack();

  for (const char* texture_name : PRIMEGUN_CANNON_TEXTURE_NAMES)
  {
    File::Delete(PrimedGunCannonActivePath(texture_name, ".dds"),
                 File::IfAbsentBehavior::NoConsoleWarning);
    File::Delete(PrimedGunCannonActivePath(texture_name, ".png"),
                 File::IfAbsentBehavior::NoConsoleWarning);
  }

  std::array<std::string, PRIMEGUN_CANNON_TEXTURE_NAMES.size()> active_paths{};
  if (slot != 0)
  {
    bool copied_any = false;
    for (size_t i = 0; i < PRIMEGUN_CANNON_TEXTURE_NAMES.size(); ++i)
    {
      const std::string source_path =
          PrimedGunCannonSourcePath(slot, PRIMEGUN_CANNON_TEXTURE_NAMES[i]);
      if (source_path.empty())
        continue;

      const std::filesystem::path source_fs(source_path);
      const std::string extension = source_fs.extension().string();
      const std::string dest_path =
          PrimedGunCannonActivePath(PRIMEGUN_CANNON_TEXTURE_NAMES[i], extension);
      File::CreateFullPath(dest_path);
      if (!File::Copy(source_path, dest_path, true))
      {
        WARN_LOG_FMT(VIDEO, "PrimedGun: Failed to apply cannon texture '{}' from '{}'.",
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
  PrimedGunCannonRefreshHiresMap(active_paths);
  return true;
}

u32 VrMenuItemCountForTab(u32 tab)
{
  switch (tab)
  {
  case VR_MENU_LAYOUT_TAB:
    return 0;
  case VR_MENU_CALIBRATION_TAB:
    return s_vr_menu_calibration_page == 0 ?
               VR_MENU_CALIBRATION_FIRST_PAGE_ITEMS + 1 :
               VR_MENU_CALIBRATION_TOTAL_ITEMS - VR_MENU_CALIBRATION_FIRST_PAGE_ITEMS + 1;
  case VR_MENU_CONTROL_TAB:
    return s_vr_menu_control_page == 0 ? VR_MENU_CONTROL_FIRST_PAGE_ITEMS + 1 :
                                         VR_MENU_CONTROL_TOTAL_ITEMS -
                                             VR_MENU_CONTROL_FIRST_PAGE_ITEMS + 1;
  case VR_MENU_MOVEMENT_TAB:
    return 11;
  case VR_MENU_CANNON_TAB:
    return 6;
  case VR_MENU_STATE_TAB:
    return VR_MENU_STATE_ACTION_ROW_COUNT + VR_MENU_STATE_SLOT_COUNT;
  default:
    return 0;
  }
}

float VrMenuRowCenterY(u32 tab, u32 index)
{
  float y = VR_MENU_ROW_TEXT_Y + static_cast<float>(index) * VR_MENU_ROW_STEP_Y;
  if (tab == VR_MENU_STATE_TAB && index >= VR_MENU_STATE_ACTION_ROW_COUNT)
    y += VR_MENU_STATE_ROW_GAP_Y;
  return y;
}

int VrMenuRowFromTextureY(u32 tab, float texture_y, u32 item_count)
{
  int best_index = -1;
  float best_distance = VR_MENU_ROW_HIT_HALF_HEIGHT;
  for (u32 i = 0; i < item_count; ++i)
  {
    const float distance = std::fabs(texture_y - VrMenuRowCenterY(tab, i));
    if (distance <= best_distance)
    {
      best_distance = distance;
      best_index = static_cast<int>(i);
    }
  }
  return best_index;
}

u32 ClampVrStateSlot(int slot)
{
  return static_cast<u32>(
      std::clamp(slot, 1, static_cast<int>(VR_MENU_STATE_SLOT_COUNT)));
}

void ClearVrStateConfirmation()
{
  if (s_vr_state_confirm_action == 0)
    return;

  s_vr_state_confirm_action = 0;
  s_vr_state_confirm_until_frame = 0;
  ++s_vr_menu_generation;
}

void ApplyPendingVrStateSlotFromUi()
{
  const int requested_slot = s_vr_state_slot_from_ui.exchange(0, std::memory_order_acq_rel);
  if (requested_slot <= 0)
    return;

  const u32 slot = ClampVrStateSlot(requested_slot);
  if (s_vr_state_slot == slot)
    return;

  s_vr_state_slot = slot;
  ClearVrStateConfirmation();
  ++s_vr_menu_generation;
}

void ClearVrResetConfirmation()
{
  if (s_vr_reset_confirm_action == 0)
    return;

  s_vr_reset_confirm_action = 0;
  s_vr_reset_confirm_until_frame = 0;
  ++s_vr_menu_generation;
}

void ClearVrMenuConfirmations()
{
  ClearVrStateConfirmation();
  ClearVrResetConfirmation();
}

void ResetVrMenuToLayoutTab()
{
  s_vr_menu_tab = VR_MENU_LAYOUT_TAB;
  s_vr_menu_selected_index = 0;
}

void RefreshVrStateConfirmation()
{
  if (s_vr_state_confirm_action != 0 && s_frame_counter >= s_vr_state_confirm_until_frame)
    ClearVrStateConfirmation();
}

void RefreshVrResetConfirmation()
{
  if (s_vr_reset_confirm_action != 0 && s_frame_counter >= s_vr_reset_confirm_until_frame)
    ClearVrResetConfirmation();
}

bool ConfirmVrResetAction(u32 action)
{
  if (action == 0)
    return true;

  RefreshVrResetConfirmation();
  if (s_vr_reset_confirm_action == action)
  {
    s_vr_reset_confirm_action = 0;
    s_vr_reset_confirm_until_frame = 0;
    ClearVrStateConfirmation();
    return true;
  }

  ClearVrStateConfirmation();
  s_vr_reset_confirm_action = action;
  s_vr_reset_confirm_until_frame = s_frame_counter + VR_MENU_RESET_CONFIRM_FRAMES;
  ++s_vr_menu_generation;
  return false;
}

int VrMenuControlActualIndex(u32 local_index)
{
  if (local_index == 0)
    return -1;

  return s_vr_menu_control_page == 0 ?
             static_cast<int>(local_index - 1) :
             static_cast<int>(VR_MENU_CONTROL_FIRST_PAGE_ITEMS + local_index - 1);
}

int VrMenuCalibrationActualIndex(u32 local_index)
{
  if (local_index == 0)
    return -1;

  return s_vr_menu_calibration_page == 0 ?
             static_cast<int>(local_index - 1) :
             static_cast<int>(VR_MENU_CALIBRATION_FIRST_PAGE_ITEMS + local_index - 1);
}

u32 VrMenuResetActionForSelection()
{
  if (s_vr_menu_tab == VR_MENU_CALIBRATION_TAB &&
      VrMenuCalibrationActualIndex(s_vr_menu_selected_index) == 9)
    return VR_MENU_RESET_TARGETING_ACTION;
  if (s_vr_menu_tab == VR_MENU_CALIBRATION_TAB &&
      VrMenuCalibrationActualIndex(s_vr_menu_selected_index) == 17)
    return VR_MENU_RESET_CALIBRATION_ACTION;
  if (s_vr_menu_tab == VR_MENU_CONTROL_TAB &&
      VrMenuControlActualIndex(s_vr_menu_selected_index) == 16)
  {
    return VR_MENU_RESET_CONTROLLER_ACTION;
  }
  if (s_vr_menu_tab == VR_MENU_MOVEMENT_TAB && s_vr_menu_selected_index == 10)
    return VR_MENU_RESET_MOVEMENT_ACTION;
  return 0;
}

bool VrMenuRowIsNumeric(u32 tab, u32 index)
{
  switch (tab)
  {
  case VR_MENU_CALIBRATION_TAB:
  {
    if (index == 0)
      return true;

    const int actual_index = VrMenuCalibrationActualIndex(index);
    return (actual_index >= 1 && actual_index <= 6) ||
           (actual_index >= 10 && actual_index <= 15);
  }
  case VR_MENU_CONTROL_TAB:
  {
    if (index == 0)
      return true;

    const int actual_index = VrMenuControlActualIndex(index);
    return actual_index == 3 || actual_index == 9 || actual_index == 13 ||
           actual_index == 14 || actual_index == 15;
  }
  case VR_MENU_MOVEMENT_TAB:
    return (index >= 3 && index <= 7) || index == 9;
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

void SetVrMenuControlPage(u32 page)
{
  s_vr_menu_control_page = page % VR_MENU_CONTROL_PAGE_COUNT;
  ClampVrMenuSelection();
}

void SetVrMenuCalibrationPage(u32 page)
{
  s_vr_menu_calibration_page = page % VR_MENU_CALIBRATION_PAGE_COUNT;
  ClampVrMenuSelection();
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
  settings->rumble_enabled = true;
  settings->rumble_intensity = 0.35f;
  settings->rumble_hand_mode = 2;
  settings->primegun_grip_inputs_enabled = true;
  settings->primegun_grip_inputs_use_trackpad = false;
  settings->primegun_trackpad_press_threshold = 0.5f;
  settings->combat_jump_use_primary_button = false;
  settings->vr_menu_hold_left_stick = false;
  settings->vr_menu_requires_head_zone = false;
  settings->xr_dpad_enabled = true;
  settings->xr_dpad_use_thumbrest_modifier = false;
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
  settings->look_yaw_sensitivity = 1.0f;
  settings->snap_turn_enabled = false;
  settings->snap_turn_degrees = 45;
}

bool CinematicCameraActive(const Core::CPUThreadGuard& guard)
{
  u32 camera_manager = 0;
  u32 cine_camera_count = 0;
  return TryReadU32(guard, ADDRESS.state_manager + ADDRESS.camera_manager_offset, &camera_manager) &&
         camera_manager >= 0x80000000u &&
         TryReadU32(guard, camera_manager + 0x08u, &cine_camera_count) &&
         cine_camera_count > 0 && cine_camera_count < 16;
}

bool ElevatorWorldTransitionActive(const Core::CPUThreadGuard& guard)
{
  u32 trans_ref_data = 0;
  u32 trans_manager = 0;
  if (!TryReadU32(guard, ADDRESS.state_manager + STATE_MANAGER_WORLD_TRANS_MANAGER_REF_OFFSET,
                  &trans_ref_data) ||
      !PrimeDataPointerLooksValid(trans_ref_data, RSTL_REF_DATA_OBJECT_OFFSET + sizeof(u32)) ||
      !TryReadU32(guard, trans_ref_data + RSTL_REF_DATA_OBJECT_OFFSET, &trans_manager) ||
      !PrimeDataPointerLooksValid(trans_manager, WORLD_TRANS_MANAGER_TYPE_OFFSET + sizeof(u32)))
  {
    return false;
  }

  u32 trans_type = 0;
  return TryReadU32(guard, trans_manager + WORLD_TRANS_MANAGER_TYPE_OFFSET, &trans_type) &&
         trans_type == WORLD_TRANS_MANAGER_TYPE_ELEVATOR;
}

void SetCinematicScreenActive(bool active)
{
  if (active && !s_cinematic_screen_active)
    ++s_cinematic_screen_generation;
  s_cinematic_screen_active = active;
}

void ClearCinematicScreenState(bool enabled)
{
  SetCinematicScreenActive(false);
  s_cinematic_screen_hold_until_frame = 0;
#ifdef ENABLE_VR
  Common::VR::PrimedGunVrOverlayState overlay =
      Common::VR::OpenXRInputState::GetPrimedGunOverlay();
  overlay.cinematic_screen_enabled = enabled;
  overlay.cinematic_screen_active = false;
  overlay.cinematic_screen_generation = s_cinematic_screen_generation;
  Common::VR::OpenXRInputState::SetPrimedGunOverlay(overlay);
#endif
}

void UpdateCinematicScreenState(const Core::CPUThreadGuard& guard, const RuntimeSettings& settings)
{
  if (!settings.cinematic_screen_enabled)
  {
    ClearCinematicScreenState(false);
    return;
  }

  if (CinematicCameraActive(guard) || ElevatorWorldTransitionActive(guard))
  {
    SetCinematicScreenActive(true);
    s_cinematic_screen_hold_until_frame =
        s_frame_counter + CINEMATIC_SCREEN_SIGNAL_LOSS_GRACE_FRAMES;
    return;
  }

  if (s_frame_counter >= s_cinematic_screen_hold_until_frame)
    SetCinematicScreenActive(false);
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
  case VR_MENU_CALIBRATION_TAB:
  {
    if (s_vr_menu_selected_index == 0)
    {
      SetVrMenuCalibrationPage(direction < 0 ?
                                   s_vr_menu_calibration_page + VR_MENU_CALIBRATION_PAGE_COUNT - 1 :
                                   s_vr_menu_calibration_page + 1);
      break;
    }

    switch (VrMenuCalibrationActualIndex(s_vr_menu_selected_index))
    {
    case 1:
      settings->metroid_hud_distance =
          std::clamp(settings->metroid_hud_distance + sign * 0.05f, 0.1f, 3.0f);
      break;
    case 2:
      settings->metroid_hud_size =
          std::clamp(settings->metroid_hud_size + sign * 0.05f, 0.1f, 3.0f);
      break;
    case 3:
    {
      const float value = std::clamp(settings->metroid_hud_offset_up -
                                         settings->metroid_hud_offset_down + sign * 0.01f,
                                     -1.0f, 1.0f);
      settings->metroid_hud_offset_up = std::max(value, 0.0f);
      settings->metroid_hud_offset_down = std::max(-value, 0.0f);
      break;
    }
    case 4:
    {
      const float value = std::clamp(settings->metroid_hud_offset_right -
                                         settings->metroid_hud_offset_left + sign * 0.01f,
                                     -1.0f, 1.0f);
      settings->metroid_hud_offset_right = std::max(value, 0.0f);
      settings->metroid_hud_offset_left = std::max(-value, 0.0f);
      break;
    }
    case 5:
      settings->gun_targeting_distance =
          std::clamp(settings->gun_targeting_distance + sign * 1.0f, 1.0f, 200.0f);
      break;
    case 6:
      settings->gun_targeting_radius =
          std::clamp(settings->gun_targeting_radius + sign * 0.1f, 0.1f, 25.0f);
      break;
    case 10:
      settings->model_offset_x += sign * 0.01f;
      break;
    case 11:
      settings->model_offset_y += sign * 0.01f;
      break;
    case 12:
      settings->model_offset_z += sign * 0.01f;
      break;
    case 13:
      settings->rot_offset_x += sign * 1.0f;
      break;
    case 14:
      settings->rot_offset_y += sign * 1.0f;
      break;
    case 15:
      settings->rot_offset_z += sign * 1.0f;
      break;
    default:
      break;
    }
    break;
  }
  case VR_MENU_CONTROL_TAB:
  {
    if (s_vr_menu_selected_index == 0)
    {
      SetVrMenuControlPage(direction < 0 ? s_vr_menu_control_page + VR_MENU_CONTROL_PAGE_COUNT - 1 :
                                           s_vr_menu_control_page + 1);
      break;
    }

    switch (VrMenuControlActualIndex(s_vr_menu_selected_index))
    {
    case 3:
      settings->rumble_intensity =
          std::clamp(settings->rumble_intensity + sign * 0.05f, 0.0f, 1.0f);
      break;
    case 9:
      settings->primegun_trackpad_press_threshold =
          std::clamp(settings->primegun_trackpad_press_threshold + sign * 0.05f, 0.05f, 1.0f);
      break;
    case 13:
      settings->xr_dpad_head_radius =
          std::clamp(settings->xr_dpad_head_radius + sign * 0.01f, 0.05f, 0.60f);
      break;
    case 14:
      settings->xr_dpad_head_y_below =
          std::clamp(settings->xr_dpad_head_y_below + sign * 0.01f, 0.0f, 0.60f);
      break;
    case 15:
      settings->xr_dpad_deadzone =
          std::clamp(settings->xr_dpad_deadzone + sign * 0.05f, 0.0f, 0.95f);
      break;
    default:
      break;
    }
    break;
  }
  case VR_MENU_MOVEMENT_TAB:
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
    case 7:
      settings->look_yaw_sensitivity =
          std::clamp(settings->look_yaw_sensitivity + sign * 0.05f, 0.20f, 3.00f);
      break;
    case 9:
      settings->snap_turn_degrees =
          SnapTurnStepDegrees(settings->snap_turn_degrees, direction < 0 ? -1 : 1);
      break;
    default:
      break;
    }
    break;
  default:
    break;
  }
}

void ActivateVrMenuSelection(RuntimeSettings* settings)
{
  const u32 reset_action = VrMenuResetActionForSelection();
  if (reset_action == 0)
    ClearVrResetConfirmation();

  if (s_vr_menu_tab == VR_MENU_CALIBRATION_TAB)
  {
    if (s_vr_menu_selected_index == 0)
    {
      SetVrMenuCalibrationPage(s_vr_menu_calibration_page + 1);
      return;
    }

    const int actual_index = VrMenuCalibrationActualIndex(s_vr_menu_selected_index);
    if (actual_index == 0)
      settings->cinematic_screen_enabled = !settings->cinematic_screen_enabled;
    else if (actual_index == 7)
      settings->visor_helmet_enabled = !settings->visor_helmet_enabled;
    else if (actual_index == 8)
      settings->height_prompt_enabled = !settings->height_prompt_enabled;
    else if (actual_index == 9)
    {
      if (!ConfirmVrResetAction(reset_action))
        return;

      settings->gun_targeting_enabled = true;
      settings->gun_targeting_distance = 60.0f;
      settings->gun_targeting_radius = 4.0f;
      settings->visor_helmet_enabled = false;
    }
    else if (actual_index == 16)
      settings->position_marker_enabled = !settings->position_marker_enabled;
    else if (actual_index == 17)
    {
      if (!ConfirmVrResetAction(reset_action))
        return;

      settings->model_offset_x = DEFAULT_MODEL_OFFSET_X;
      settings->model_offset_y = DEFAULT_MODEL_OFFSET_Y;
      settings->model_offset_z = DEFAULT_MODEL_OFFSET_Z;
      settings->rot_offset_x = DEFAULT_ROT_OFFSET_X;
      settings->rot_offset_y = DEFAULT_ROT_OFFSET_Y;
      settings->rot_offset_z = DEFAULT_ROT_OFFSET_Z;
    }
    else if (actual_index == 18)
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
    else if (actual_index == 19)
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
    return;
  }

  if (s_vr_menu_tab == VR_MENU_CONTROL_TAB)
  {
    if (s_vr_menu_selected_index == 0)
    {
      SetVrMenuControlPage(s_vr_menu_control_page + 1);
      return;
    }

    const int actual_index = VrMenuControlActualIndex(s_vr_menu_selected_index);
    if (actual_index == 0)
      settings->use_right_hand = !settings->use_right_hand;
    else if (actual_index == 1)
      settings->rumble_enabled = !settings->rumble_enabled;
    else if (actual_index == 2)
      settings->rumble_hand_mode = (std::clamp(settings->rumble_hand_mode, 0, 2) + 1) % 3;
    else if (actual_index == 4)
      settings->primegun_grip_inputs_enabled = !settings->primegun_grip_inputs_enabled;
    else if (actual_index == 5)
      settings->combat_jump_use_primary_button = !settings->combat_jump_use_primary_button;
    else if (actual_index == 6)
      settings->vr_menu_hold_left_stick = !settings->vr_menu_hold_left_stick;
    else if (actual_index == 7)
      settings->vr_menu_requires_head_zone = !settings->vr_menu_requires_head_zone;
    else if (actual_index == 8)
      settings->primegun_grip_inputs_use_trackpad = !settings->primegun_grip_inputs_use_trackpad;
    else if (actual_index == 10 || actual_index == 11)
      settings->xr_dpad_enabled = !settings->xr_dpad_enabled;
    else if (actual_index == 12)
      settings->xr_dpad_use_thumbrest_modifier = !settings->xr_dpad_use_thumbrest_modifier;
    else if (actual_index == 16)
    {
      if (!ConfirmVrResetAction(reset_action))
        return;

      ResetControllerSettings(settings);
    }
    return;
  }

  if (s_vr_menu_tab == VR_MENU_MOVEMENT_TAB)
  {
    if (s_vr_menu_selected_index == 0)
      settings->directional_movement_enabled = !settings->directional_movement_enabled;
    else if (s_vr_menu_selected_index == 1)
      settings->directional_movement_use_right_stick = !settings->directional_movement_use_right_stick;
    else if (s_vr_menu_selected_index == 2)
      settings->directional_movement_use_hmd_direction =
          !settings->directional_movement_use_hmd_direction;
    else if (s_vr_menu_selected_index == 8)
      settings->snap_turn_enabled = !settings->snap_turn_enabled;
    else if (s_vr_menu_selected_index == 10)
    {
      if (!ConfirmVrResetAction(reset_action))
        return;

      ResetMovementSettings(settings);
    }
    return;
  }

  if (s_vr_menu_tab == VR_MENU_CANNON_TAB)
  {
    if (s_vr_menu_selected_index <= 5)
    {
      if (ApplyPrimedGunCannonTextureSlot(s_vr_menu_selected_index))
      {
        s_vr_cannon_texture_slot = s_vr_menu_selected_index;
        s_vr_cannon_texture_notice_until_frame = s_frame_counter + 180;
      }
      return;
    }
  }

  if (s_vr_menu_tab == VR_MENU_STATE_TAB)
  {
    if (s_vr_menu_selected_index >= VR_MENU_STATE_ACTION_ROW_COUNT)
    {
      const u32 slot = s_vr_menu_selected_index - VR_MENU_STATE_ACTION_ROW_COUNT + 1;
      if (slot >= 1 && slot <= VR_MENU_STATE_SLOT_COUNT)
      {
        s_vr_state_slot = slot;
        s_vr_state_slot_select_requested.store(static_cast<int>(slot), std::memory_order_release);
        ClearVrStateConfirmation();
      }
      return;
    }

    const u32 requested_action = s_vr_menu_selected_index + 1;
    if (requested_action < VR_MENU_STATE_ACTION_LOAD ||
        requested_action > VR_MENU_STATE_ACTION_SAVE_OLDEST)
      return;

    RefreshVrStateConfirmation();
    if (s_vr_state_confirm_action == requested_action)
    {
      if (requested_action == VR_MENU_STATE_ACTION_LOAD)
        s_vr_state_load_requested.store(true, std::memory_order_release);
      else if (requested_action == VR_MENU_STATE_ACTION_SAVE)
        s_vr_state_save_requested.store(true, std::memory_order_release);
      else if (requested_action == VR_MENU_STATE_ACTION_LOAD_NEWEST)
        s_vr_state_load_newest_requested.store(true, std::memory_order_release);
      else if (requested_action == VR_MENU_STATE_ACTION_SAVE_OLDEST)
        s_vr_state_save_oldest_requested.store(true, std::memory_order_release);
      s_vr_state_confirm_action = 0;
      s_vr_state_confirm_until_frame = 0;
    }
    else
    {
      s_vr_state_confirm_action = requested_action;
      s_vr_state_confirm_until_frame = s_frame_counter + VR_MENU_STATE_CONFIRM_FRAMES;
    }
    return;
  }
}

void SaveVrMenuSettingsNotice()
{
  ClearVrMenuConfirmations();
  s_vr_settings_save_requested = true;
  s_vr_menu_saved_notice_until_frame = s_frame_counter + 180;
  ++s_vr_menu_generation;
}

void PublishVrOverlayState(const RuntimeSettings& settings, bool prompt_visible)
{
  RefreshVrStateConfirmation();
  RefreshVrResetConfirmation();
  const Common::VR::PrimedGunVrOverlayState previous =
      Common::VR::OpenXRInputState::GetPrimedGunOverlay();
  Common::VR::PrimedGunVrOverlayState overlay{};
  overlay.prompt_visible = settings.vr_overlays_enabled && prompt_visible;
  overlay.menu_visible = settings.vr_overlays_enabled && s_vr_menu_visible;
  overlay.menu_pointer_active = settings.vr_overlays_enabled && s_vr_menu_visible;
  overlay.saved_notice = s_frame_counter < s_vr_menu_saved_notice_until_frame;
  overlay.generation = s_vr_menu_generation;
  overlay.tab = s_vr_menu_tab;
  overlay.selected_index = s_vr_menu_selected_index;
  overlay.item_count = VrMenuItemCountForTab(s_vr_menu_tab);
  overlay.calibration_page = s_vr_menu_calibration_page;
  overlay.control_page = s_vr_menu_control_page;
  overlay.cannon_texture_slot = s_vr_cannon_texture_slot;
  overlay.cannon_texture_notice = s_frame_counter < s_vr_cannon_texture_notice_until_frame;
  overlay.state_slot = s_vr_state_slot;
  overlay.state_confirm_action = s_vr_state_confirm_action;
  overlay.reset_confirm_action = s_vr_reset_confirm_action;
  overlay.weapon_panel_visible = settings.vr_overlays_enabled && previous.weapon_panel_visible;
  overlay.weapon_selected_index = previous.weapon_selected_index;
  overlay.weapon_panel_position = previous.weapon_panel_position;
  overlay.weapon_panel_orientation = previous.weapon_panel_orientation;
  overlay.world_scale = settings.world_scale;
  overlay.use_right_hand = settings.use_right_hand;
  overlay.require_trigger = settings.require_trigger;
  overlay.trigger_threshold = settings.trigger_threshold;
  overlay.rumble_enabled = settings.rumble_enabled;
  overlay.rumble_intensity = settings.rumble_intensity;
  overlay.rumble_hand_mode = settings.rumble_hand_mode;
  overlay.primegun_grip_inputs_enabled = settings.primegun_grip_inputs_enabled;
  overlay.primegun_grip_inputs_use_trackpad = settings.primegun_grip_inputs_use_trackpad;
  overlay.primegun_trackpad_press_threshold = settings.primegun_trackpad_press_threshold;
  overlay.combat_jump_use_primary_button = settings.combat_jump_use_primary_button;
  overlay.vr_menu_hold_left_stick = settings.vr_menu_hold_left_stick;
  overlay.vr_menu_requires_head_zone = settings.vr_menu_requires_head_zone;
  overlay.gun_targeting_enabled = settings.gun_targeting_enabled;
  overlay.gun_targeting_distance = settings.gun_targeting_distance;
  overlay.gun_targeting_radius = settings.gun_targeting_radius;
  overlay.visor_helmet_enabled = settings.visor_helmet_enabled;
  overlay.vr_overlays_enabled = settings.vr_overlays_enabled;
  overlay.height_prompt_enabled = settings.height_prompt_enabled;
  overlay.position_marker_visible = settings.vr_overlays_enabled && settings.position_marker_enabled;
  overlay.xr_dpad_enabled = settings.xr_dpad_enabled;
  overlay.xr_dpad_use_thumbrest_modifier = settings.xr_dpad_use_thumbrest_modifier;
  overlay.cinematic_screen_enabled = settings.cinematic_screen_enabled;
  overlay.cinematic_screen_active = settings.cinematic_screen_enabled && s_cinematic_screen_active;
  overlay.cinematic_screen_generation = s_cinematic_screen_generation;
  overlay.metroid_hud_distance = settings.metroid_hud_distance;
  overlay.metroid_hud_size = settings.metroid_hud_size;
  overlay.metroid_hud_offset_up = settings.metroid_hud_offset_up;
  overlay.metroid_hud_offset_down = settings.metroid_hud_offset_down;
  overlay.metroid_hud_offset_left = settings.metroid_hud_offset_left;
  overlay.metroid_hud_offset_right = settings.metroid_hud_offset_right;
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
  overlay.look_yaw_sensitivity = settings.look_yaw_sensitivity;
  overlay.snap_turn_enabled = settings.snap_turn_enabled;
  overlay.snap_turn_degrees = settings.snap_turn_degrees;
  overlay.offset_x = settings.offset_x;
  overlay.offset_y = settings.offset_y;
  overlay.offset_z = settings.offset_z;
  overlay.model_offset_x = settings.model_offset_x;
  overlay.model_offset_y = settings.model_offset_y;
  overlay.model_offset_z = settings.model_offset_z;
  overlay.rot_offset_x = settings.rot_offset_x;
  overlay.rot_offset_y = settings.rot_offset_y;
  overlay.rot_offset_z = settings.rot_offset_z;
  Common::VR::OpenXRInputState::SetPrimedGunOverlay(overlay);
}

void UpdateVrMenu(const Common::VR::OpenXRInputSnapshot& snapshot, RuntimeSettings* settings,
                  bool prompt_visible)
{
  ApplyPendingVrStateSlotFromUi();

  if (!settings->vr_overlays_enabled)
  {
    if (s_vr_menu_visible)
    {
      s_vr_menu_visible = false;
      ClearVrStateConfirmation();
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

  const u32 panel_hand_index = settings->use_right_hand ? 0 : 1;
  const u32 pointer_hand_index = settings->use_right_hand ? 1 : 0;
  const auto& panel_hand = snapshot.controllers[panel_hand_index];
  const auto& pointer_hand = snapshot.controllers[pointer_hand_index];
  const bool menu_input_raw =
      panel_hand.connected && (panel_hand.thumbstick_button || panel_hand.menu_button);
  const bool menu_hand_near_head =
      panel_hand.connected && panel_hand.aim_pose.valid && snapshot.head_pose.valid &&
      LeftControllerNearHead(PoseFromOpenXR(panel_hand.aim_pose),
                             PoseFromOpenXR(snapshot.head_pose), *settings);
  const bool menu_input =
      menu_input_raw && (!settings->vr_menu_requires_head_zone || s_vr_menu_visible ||
                         menu_hand_near_head);
  if (settings->vr_menu_hold_left_stick)
  {
    if (s_vr_menu_visible)
    {
      if (menu_input && !s_last_vr_menu_thumbstick)
      {
        s_vr_menu_visible = false;
        s_vr_menu_long_press_start_frame = 0;
        s_vr_menu_long_press_consumed = true;
        ClearVrMenuConfirmations();
        ++s_vr_menu_generation;
      }
      else if (!menu_input)
      {
        s_vr_menu_long_press_start_frame = 0;
        s_vr_menu_long_press_consumed = false;
      }
    }
    else if (!menu_input)
    {
      s_vr_menu_long_press_start_frame = 0;
      s_vr_menu_long_press_consumed = false;
    }
    else
    {
      if (s_vr_menu_long_press_start_frame == 0)
        s_vr_menu_long_press_start_frame = s_frame_counter == 0 ? 1 : s_frame_counter;

      if (!s_vr_menu_long_press_consumed &&
          s_frame_counter >= s_vr_menu_long_press_start_frame + VR_MENU_LONG_PRESS_FRAMES)
      {
        s_vr_menu_visible = !s_vr_menu_visible;
        s_vr_menu_long_press_consumed = true;
        if (s_vr_menu_visible)
          ResetVrMenuToLayoutTab();
        if (!s_vr_menu_visible)
          ClearVrMenuConfirmations();
        ++s_vr_menu_generation;
      }
    }
  }
  else if (menu_input && !s_last_vr_menu_thumbstick)
  {
    s_vr_menu_visible = !s_vr_menu_visible;
    if (s_vr_menu_visible)
      ResetVrMenuToLayoutTab();
    if (!s_vr_menu_visible)
      ClearVrMenuConfirmations();
    ++s_vr_menu_generation;
  }
  else if (!menu_input)
  {
    s_vr_menu_long_press_start_frame = 0;
    s_vr_menu_long_press_consumed = false;
  }
  if (!settings->vr_menu_hold_left_stick && menu_input)
  {
    s_vr_menu_long_press_start_frame = 0;
    s_vr_menu_long_press_consumed = false;
  }
  s_last_vr_menu_thumbstick = menu_input;

  float pointer_x = 0.5f;
  float pointer_y = 0.5f;
  bool pointer_active = false;
  if (s_vr_menu_visible)
  {
    const Pose panel_pose = GripControllerPose(panel_hand);
    const Pose pointer_pose = PoseFromOpenXR(pointer_hand.aim_pose);
    pointer_active = VrMenuPointerHit(panel_pose, pointer_pose, &pointer_x, &pointer_y);
    if (pointer_active)
    {
      const u32 item_count = VrMenuItemCountForTab(s_vr_menu_tab);
      if (item_count > 0)
      {
        const int hovered =
            VrMenuRowFromTextureY(s_vr_menu_tab,
                                  std::clamp(pointer_y, 0.0f, 1.0f) * VR_MENU_TEXTURE_HEIGHT,
                                  item_count);
        if (hovered >= 0 && s_vr_menu_selected_index != static_cast<u32>(hovered))
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

    const bool primary =
        pointer_hand.connected && (pointer_hand.primary_button || pointer_hand.trigger_button);
    const bool secondary = pointer_hand.connected && pointer_hand.secondary_button;
    if (primary && !s_last_vr_menu_primary)
    {
      const float texture_x = std::clamp(pointer_x, 0.0f, 1.0f) * VR_MENU_TEXTURE_WIDTH;
      const float texture_y = std::clamp(pointer_y, 0.0f, 1.0f) * VR_MENU_TEXTURE_HEIGHT;
      if (pointer_active && texture_y >= 64.0f && texture_y <= 102.0f)
      {
        constexpr float tab_start_x = 22.0f;
        constexpr float tab_step = 166.0f;
        constexpr float tab_width = 150.0f;
        const int tab = static_cast<int>((texture_x - tab_start_x) / tab_step);
        const float tab_local_x =
            texture_x - (tab_start_x + static_cast<float>(tab) * tab_step);
        if (tab >= 0 && tab < static_cast<int>(VR_MENU_TAB_COUNT) && tab_local_x >= 0.0f &&
            tab_local_x <= tab_width &&
            s_vr_menu_tab != static_cast<u32>(tab))
        {
          ClearVrMenuConfirmations();
          s_vr_menu_tab = static_cast<u32>(tab);
          s_vr_menu_selected_index = 0;
          ++s_vr_menu_generation;
        }
      }
      else if (s_vr_menu_tab != VR_MENU_LAYOUT_TAB && pointer_active && texture_y >= 108.0f &&
               texture_y <= 136.0f)
      {
        if (texture_x >= 52.0f && texture_x <= 272.0f)
          SaveVrMenuSettingsNotice();
        else if (texture_x >= 300.0f && texture_x <= 520.0f)
        {
          if (ConfirmVrResetAction(VR_MENU_RESET_ALL_ACTION))
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
        const int row_hit =
            VrMenuRowFromTextureY(s_vr_menu_tab, texture_y, VrMenuItemCountForTab(s_vr_menu_tab));
        if (row_hit >= 0)
        {
          if (s_vr_menu_selected_index != static_cast<u32>(row_hit))
            s_vr_menu_selected_index = static_cast<u32>(row_hit);

          if (texture_x >= value_box_x && texture_x <= value_box_x + value_width &&
              VrMenuRowIsNumeric(s_vr_menu_tab, s_vr_menu_selected_index))
          {
            AdjustVrMenuSetting(settings,
                                texture_x >= value_box_x + value_width * 0.5f ? 1 : -1);
          }
          else
          {
            ActivateVrMenuSelection(settings);
          }
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
  auto overlay = Common::VR::OpenXRInputState::GetPrimedGunOverlay();
  overlay.menu_pointer_active = s_vr_menu_visible && pointer_active;
  overlay.pointer_x = pointer_x;
  overlay.pointer_y = pointer_y;
  Common::VR::OpenXRInputState::SetPrimedGunOverlay(overlay);
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
    if (VR::g_openxr)
      VR::g_openxr->RequestRecenter();
    s_height_prompt_until_frame = 0;
  }
  s_last_height_reset_thumbstick = pressed;
}

bool UpdateHeightPromptGate(const Core::CPUThreadGuard& guard,
                            const Common::VR::OpenXRInputSnapshot& snapshot,
                            const RuntimeSettings& settings, u32 player)
{
  if (!settings.height_prompt_enabled)
  {
    s_prompt_gameplay_ready_since_frame = 0;
    s_height_prompt_until_frame = 0;
    return false;
  }

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
    ClearAudioListenerScratch(guard);
    PublishVrOverlayState(settings, false);
    return;
  }

  u32 prompt_player = 0;
  if (!TryReadU32(guard, ADDRESS.state_manager + ADDRESS.player_offset, &prompt_player))
    prompt_player = 0;
  const bool prompt_visible = UpdateHeightPromptGate(guard, snapshot, settings, prompt_player);

  UpdateVrMenu(snapshot, &settings, prompt_visible);
  SetRuntimeSettings(settings);

  float yaw_delta_deg = GetPlayerYawDeltaDegrees(guard);
  if (settings.builtin_patches_enabled)
    UpdateAudioListenerScratchFromHmd(guard, snapshot, yaw_delta_deg);
  else
    ClearAudioListenerScratch(guard);
  UpdateDirectionalMovement(guard, snapshot, settings, yaw_delta_deg);
  UpdateXrDpad(guard, snapshot, settings);
  float snap_turn_yaw_deg = 0.0f;
  const bool snap_turn_applied = UpdateSnapTurn(guard, snapshot, settings, &snap_turn_yaw_deg);
  if (snap_turn_applied)
    yaw_delta_deg = WrapDegrees(yaw_delta_deg - snap_turn_yaw_deg);
  if (!snap_turn_applied)
    ApplyLookYawSensitivityBoost(guard, snapshot, settings, prompt_player);

  const Common::VR::OpenXRControllerState& hand =
      snapshot.controllers[settings.use_right_hand ? 1 : 0];
  const bool hand_pose_ready = hand.connected && hand.aim_pose.valid;
  if (!hand_pose_ready)
  {
    s_cannon_hand_pose_ready = false;
    return;
  }

  if (!s_cannon_hand_pose_ready)
  {
    s_cannon_hand_pose_ready = true;
    s_smooth_matrix_valid = false;
    s_last_validated_gun = 0;
    ClearCannonRuntimeScratch(guard);
    WriteGunTargetScratch(guard, 0, 0xffffu);
    TryWriteU32(guard, RETICLE_BILLBOARD_SCRATCH, 0);
  }

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
  if (!MatrixNumbersLookValid(controller_mat_no_offset) || !MatrixNumbersLookValid(mat))
    return;

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
  const bool bypass_cannon_smoothing = s_frame_counter < s_cannon_smoothing_bypass_until_frame;
  if (bypass_cannon_smoothing || !s_smooth_matrix_valid ||
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
  if (!MatrixNumbersLookValid(mat))
  {
    ClearCannonRuntimeScratch(guard);
    TryWriteU32(guard, RETICLE_BILLBOARD_SCRATCH, 0);
    return;
  }

  auto clear_active_cannon_feed = [&] {
    ClearCannonRuntimeScratch(guard);
    TryWriteU32(guard, RETICLE_BILLBOARD_SCRATCH, 0);
  };

  u32 player = 0;
  if (!TryReadU32(guard, ADDRESS.state_manager + ADDRESS.player_offset, &player) ||
      !PlayerObjectLooksValid(guard, player))
  {
    s_smooth_matrix_valid = false;
    s_last_validated_gun = 0;
    clear_active_cannon_feed();
    WriteGunTargetScratch(guard, 0, 0xffffu);
    return;
  }

  UpdateScanPitch(guard, snapshot, player, yaw_delta_deg);
  const bool scan_active = ScanVisorActive(guard, player);
  if (scan_active)
    UpdateScanTargetingFromHmd(guard, snapshot, ADDRESS.state_manager, player, settings,
                               yaw_delta_deg);

  u32 gun = 0;
  if (!PlayerIsFirstPersonGunReady(guard, player) ||
      !TryReadU32(guard, player + ADDRESS.cannon_offset, &gun) || gun < 0x80000000u)
  {
    if (scan_active)
    {
      UpdateReticleBillboard(guard, snapshot, player, yaw_delta_deg);
    }

    if (scan_active)
      return;

    s_smooth_matrix_valid = false;
    s_last_validated_gun = 0;
    clear_active_cannon_feed();
    WriteGunTargetScratch(guard, 0, 0xffffu);
    return;
  }

  const u32 world_xf = gun + ADDRESS.world_xf_offset;
  const u32 local_xf = gun + ADDRESS.local_xf_offset;
  const bool gun_chain_valid =
      LooksLikeTransformMatrix(guard, world_xf) && LooksLikeTransformMatrix(guard, local_xf);
  if (!gun_chain_valid)
  {
    s_smooth_matrix_valid = false;
    s_last_validated_gun = 0;
    clear_active_cannon_feed();
    WriteGunTargetScratch(guard, 0, 0xffffu);
    return;
  }
  s_last_validated_gun = gun;

  const Matrix3x4 model_mat = CannonModelMatrixForHand(mat, settings);
  if (!WriteBasisScratch(guard, model_mat))
  {
    s_smooth_matrix_valid = false;
    s_last_validated_gun = 0;
    clear_active_cannon_feed();
    WriteGunTargetScratch(guard, 0, 0xffffu);
    return;
  }
  TryWriteFloat(guard, ADDRESS.gun_pos + 0, mat.At(0, 3));
  TryWriteFloat(guard, ADDRESS.gun_pos + 4, mat.At(1, 3));
  TryWriteFloat(guard, ADDRESS.gun_pos + 8, mat.At(2, 3));

  const float model_local_x = settings.model_offset_x * settings.world_scale;
  const float model_local_y =
      (BASE_MODEL_FORWARD_BACK_OFFSET + settings.model_offset_y) * settings.world_scale;
  const float model_local_z = settings.model_offset_z * settings.world_scale;
  const float model_world_x =
      model_mat.m[0] * model_local_x + model_mat.m[1] * model_local_y +
      model_mat.m[2] * model_local_z;
  const float model_world_y =
      model_mat.m[4] * model_local_x + model_mat.m[5] * model_local_y +
      model_mat.m[6] * model_local_z;
  const float model_world_z =
      model_mat.m[8] * model_local_x + model_mat.m[9] * model_local_y +
      model_mat.m[10] * model_local_z;
  if (!std::isfinite(model_world_x) || !std::isfinite(model_world_y) ||
      !std::isfinite(model_world_z))
  {
    clear_active_cannon_feed();
    return;
  }
  TryWriteFloat(guard, MODEL_OFFSET_WORLD_SCRATCH + 0, model_world_x);
  TryWriteFloat(guard, MODEL_OFFSET_WORLD_SCRATCH + 4, model_world_y);
  TryWriteFloat(guard, MODEL_OFFSET_WORLD_SCRATCH + 8, model_world_z);
  TryWriteU32(guard, CANNON_EXPECTED_GUN_SCRATCH, gun);

  UpdateReticleBillboard(guard, snapshot, player, yaw_delta_deg);
  if (scan_active)
    return;

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
  constexpr u32 flag_hi = (PITCH_ZERO_ENABLE_SCRATCH >> 16) & 0xffffu;
  constexpr u32 flag_lo = PITCH_ZERO_ENABLE_SCRATCH & 0xffffu;
  const u32 original_label = patch.cave + 0x18u;
  const u32 zero_label = patch.cave + 0x20u;
  const u32 return_addr = patch.address + 4u;
  const auto write_cave = [&] {
    return RefreshInstructionBlock(
        guard, patch.cave,
        {{0x00u, 0x3D800000u | flag_hi},
         {0x04u, 0x618C0000u | flag_lo},
         {0x08u, 0x818C0000u},
         {0x0Cu, 0x2C0C0000u},
         {0x10u, PpcBeq(patch.cave + 0x10u, original_label)},
         {0x14u, PpcBranch(patch.cave + 0x14u, zero_label)},
         {0x18u, patch.original},
         {0x1Cu, PpcBranch(patch.cave + 0x1Cu, return_addr)},
         {0x20u, LOAD_ZERO_TO_F31},
         {0x24u, PpcBranch(patch.cave + 0x24u, return_addr)}});
  };

  if (current == branch)
  {
    patch.applied = write_cave();
    return false;
  }

  if (current != patch.original)
  {
    patch.applied = false;
    return false;
  }

  patch.applied = write_cave() && TryWriteInstruction(guard, patch.address, branch);
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
    if (patch.original == 0)
    {
      patch.applied = false;
      return false;
    }
    current = patch.original;
  }
  else if ((current & 0xFFFF0000u) != 0x94210000u)
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

  patch.applied = InstallBranchAfterCaveWrite(
      guard, patch.address, patch.cave,
      {{0x00u, 0x3D800000u | pos_scratch_hi},
       {0x04u, 0x618C0000u | pos_scratch_lo},
       {0x08u, 0x3D600000u | offset_scratch_hi},
       {0x0Cu, 0x616B0000u | offset_scratch_lo},
       {0x10u, 0xC0050000u},
       {0x14u, 0xC02B0000u},
       {0x18u, 0xEC00082Au},
       {0x1Cu, 0xD00C0000u},
       {0x20u, 0xC0450004u},
       {0x24u, 0xC06B0004u},
       {0x28u, 0xEC42182Au},
       {0x2Cu, 0xD04C0004u},
       {0x30u, 0xC0850008u},
       {0x34u, 0xC0AB0008u},
       {0x38u, 0xEC84282Au},
       {0x3Cu, 0xD08C0008u},
       {0x40u, 0x7D856378u},
       {0x44u, patch.original},
       {0x48u, PpcBranch(patch.cave + 0x48u, return_addr)}});
  return patch.applied;
}

bool ApplyScanIndicatorPointerPatch(const Core::CPUThreadGuard& guard)
{
  auto& patch = s_scan_indicator_update_trace_patch;
  u32 current = 0;
  if (!TryReadU32(guard, patch.address, &current))
    return false;

  const u32 branch = PpcBranch(patch.address, patch.cave);
  if (current == branch)
  {
    if (patch.original == 0)
      TryReadU32(guard, patch.cave + 0x14u, &patch.original);
    if (patch.original == 0)
    {
      patch.applied = false;
      return false;
    }
    current = patch.original;
  }
  else if ((current & 0xFFFF0000u) != 0x94210000u)
  {
    patch.applied = false;
    return false;
  }

  patch.original = current;
  const u32 trace_hi = (SCAN_RETICLE_TRACE_SCRATCH >> 16) & 0xffffu;
  const u32 trace_lo = SCAN_RETICLE_TRACE_SCRATCH & 0xffffu;
  const u32 return_addr = patch.address + 4u;

  patch.applied = InstallBranchAfterCaveWrite(
      guard, patch.address, patch.cave,
      {{0x00u, 0x3D800000u | trace_hi},
       {0x04u, 0x618C0000u | trace_lo},
       {0x08u, 0x906C0020u},
       {0x0Cu, 0x38000001u},
       {0x10u, 0x900C0024u},
       {0x14u, patch.original},
       {0x18u, PpcBranch(patch.cave + 0x18u, return_addr)}});
  return patch.applied;
}

bool ApplyScanReticleTracePatch(const Core::CPUThreadGuard& guard)
{
  auto apply_one = [&](DynamicPpcPatch& patch, u32 scratch_offset) {
    u32 current = 0;
    if (!TryReadU32(guard, patch.address, &current))
      return false;

    const u32 branch = PpcBranch(patch.address, patch.cave);
    if (current == branch)
    {
      if (patch.original == 0)
        TryReadU32(guard, patch.cave + 0x18u, &patch.original);
      if (patch.original == 0)
      {
        patch.applied = false;
        return false;
      }
      current = patch.original;
    }
    else if ((current & 0xFFFF0000u) != 0x94210000u)
    {
      patch.applied = false;
      return false;
    }

    patch.original = current;
    const u32 scratch = SCAN_RETICLE_TRACE_SCRATCH + scratch_offset;
    const u32 trace_hi = (scratch >> 16) & 0xffffu;
    const u32 trace_lo = scratch & 0xffffu;
    const u32 return_addr = patch.address + 4u;

    patch.applied = InstallBranchAfterCaveWrite(
        guard, patch.address, patch.cave,
        {{0x00u, 0x3D800000u | trace_hi},
         {0x04u, 0x618C0000u | trace_lo},
         {0x08u, 0x906C0000u},
         {0x0Cu, 0x908C0004u},
         {0x10u, 0x38000001u},
         {0x14u, 0x900C0008u},
         {0x18u, patch.original},
         {0x1Cu, PpcBranch(patch.cave + 0x1Cu, return_addr)}});
    return patch.applied;
  };

  bool wrote = false;
  wrote = apply_one(s_scan_reticle_trace_patch, 0x00u) || wrote;
  wrote = apply_one(s_scan_reticle_trace_curr_patch, 0x10u) || wrote;

  return wrote;
}

bool RestoreTracePatchIfInstalled(const Core::CPUThreadGuard& guard, DynamicPpcPatch& patch,
                                  u32 original_cave_offset)
{
  const u32 branch = PpcBranch(patch.address, patch.cave);
  u32 current = 0;
  if (!TryReadU32(guard, patch.address, &current) || current != branch)
    return false;

  u32 original = patch.original;
  if (original == 0)
    TryReadU32(guard, patch.cave + original_cave_offset, &original);
  if (original == 0)
    return false;

  const bool restored = TryWriteInstruction(guard, patch.address, original);
  if (restored)
  {
    patch.original = original;
    patch.applied = false;
  }
  return restored;
}

bool RestoreScanReticleTracePatches(const Core::CPUThreadGuard& guard)
{
  bool restored = false;
  restored = RestoreTracePatchIfInstalled(guard, s_scan_reticle_trace_patch, 0x18u) || restored;
  restored = RestoreTracePatchIfInstalled(guard, s_scan_reticle_trace_curr_patch, 0x18u) || restored;
  return restored;
}

bool ApplyScanIndicatorViewBasisPatch(const Core::CPUThreadGuard& guard)
{
  auto& patch = s_scan_indicator_view_basis_patch;
  u32 current = 0;
  if (!TryReadU32(guard, patch.address, &current))
    return false;

  const u32 branch = PpcBranch(patch.address, patch.cave);
  constexpr u32 basis_hi = (RETICLE_BILLBOARD_SCRATCH >> 16) & 0xffffu;
  constexpr u32 basis_lo = RETICLE_BILLBOARD_SCRATCH & 0xffffu;
  const u32 return_addr = patch.address + 4u;

  const std::initializer_list<HookWrite> cave_writes{
      {0x00u, 0x3D800000u | basis_hi},
      {0x04u, 0x618C0000u | basis_lo},

      // Copy only the 3x3 orientation rows into the per-indicator model transform.
      // Translation remains the scan indicator position the game already selected.
      {0x08u, 0xC00C0004u},  // lfs f0, 4(r12)
      {0x0Cu, 0xD0010148u},  // stfs f0, 0x148(r1)
      {0x10u, 0xC00C0008u},
      {0x14u, 0xD001014Cu},
      {0x18u, 0xC00C000Cu},
      {0x1Cu, 0xD0010150u},

      {0x20u, 0xC00C0010u},
      {0x24u, 0xD0010158u},
      {0x28u, 0xC00C0014u},
      {0x2Cu, 0xD001015Cu},
      {0x30u, 0xC00C0018u},
      {0x34u, 0xD0010160u},

      {0x38u, 0xC00C001Cu},
      {0x3Cu, 0xD0010168u},
      {0x40u, 0xC00C0020u},
      {0x44u, 0xD001016Cu},
      {0x48u, 0xC00C0024u},
      {0x4Cu, 0xD0010170u},

      {0x50u, patch.original},
      {0x54u, PpcBranch(patch.cave + 0x54u, return_addr)}};

  if (current == branch)
  {
    const bool cave_matches = InstructionBlockMatches(guard, patch.cave, cave_writes);
    patch.applied = cave_matches || TryWriteInstructionBlock(guard, patch.cave, cave_writes);
    return !cave_matches && patch.applied;
  }

  if (current != patch.original)
  {
    patch.applied = false;
    return false;
  }

  patch.original = current;
  patch.applied =
      InstallBranchAfterCaveWrite(guard, patch.address, patch.cave, cave_writes);
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
    if (patch.original == 0)
      TryReadU32(guard, patch.cave + 0x80u, &patch.original);
    if (patch.original == 0)
    {
      patch.applied = false;
      return false;
    }
    current = patch.original;
  }
  else if ((current & 0xFFFF0000u) != 0x94210000u)
  {
    patch.applied = false;
    return false;
  }

  patch.original = current;
  constexpr u32 transform_scratch = SCRATCH_BASE + 0x600u;
  constexpr u32 transform_scratch_hi = (transform_scratch >> 16) & 0xffffu;
  constexpr u32 transform_scratch_lo = transform_scratch & 0xffffu;
  constexpr u32 cannon_scratch_hi = (CANNON_BASIS_SCRATCH >> 16) & 0xffffu;
  constexpr u32 cannon_scratch_lo = CANNON_BASIS_SCRATCH & 0xffffu;
  const u32 original_label = patch.cave + 0x80u;
  const u32 return_addr = patch.address + 4u;
  const u32 src = patch.transform_register;

  patch.applied = InstallBranchAfterCaveWrite(
      guard, patch.address, patch.cave,
      {{0x00u, 0x3D800000u | transform_scratch_hi},
       {0x04u, 0x618C0000u | transform_scratch_lo},
       {0x08u, 0x3D600000u | cannon_scratch_hi},
       {0x0Cu, 0x616B0000u | cannon_scratch_lo},
       {0x10u, 0x800B0038u},
       {0x14u, 0x2C000000u},
       {0x18u, PpcBeq(patch.cave + 0x18u, original_label)},
       {0x1Cu, 0x800B0000u},
       {0x20u, 0x900C0000u},
       {0x24u, 0x800B0004u},
       {0x28u, 0x900C0004u},
       {0x2Cu, 0x800B0008u},
       {0x30u, 0x900C0008u},
       {0x34u, 0x800B000Cu},
       {0x38u, 0x900C0010u},
       {0x3Cu, 0x800B0010u},
       {0x40u, 0x900C0014u},
       {0x44u, 0x800B0014u},
       {0x48u, 0x900C0018u},
       {0x4Cu, 0x800B0018u},
       {0x50u, 0x900C0020u},
       {0x54u, 0x800B001Cu},
       {0x58u, 0x900C0024u},
       {0x5Cu, 0x800B0020u},
       {0x60u, 0x900C0028u},
       {0x64u, PpcLwzR0(src, 0x0Cu)},
       {0x68u, 0x900C000Cu},
       {0x6Cu, PpcLwzR0(src, 0x1Cu)},
       {0x70u, 0x900C001Cu},
       {0x74u, PpcLwzR0(src, 0x2Cu)},
       {0x78u, 0x900C002Cu},
       {0x7Cu, PpcMr(src, 12)},
       {0x80u, patch.original},
       {0x84u, PpcBranch(patch.cave + 0x84u, return_addr)}});
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
    if (patch.original == 0)
      TryReadU32(guard, patch.cave + 0x10u, &patch.original);
    if (patch.original == 0)
    {
      patch.applied = false;
      return false;
    }
    current = patch.original;
  }

  patch.original = current;
  constexpr u32 menu_scratch_hi = (GAMEFLOW_MENU_SCRATCH >> 16) & 0xffffu;
  constexpr u32 menu_scratch_lo = GAMEFLOW_MENU_SCRATCH & 0xffffu;
  const u32 return_addr = patch.address + 4u;

  patch.applied = InstallBranchAfterCaveWrite(
      guard, patch.address, patch.cave,
      {{0x00u, 0x3D800000u | menu_scratch_hi},
       {0x04u, 0x618C0000u | menu_scratch_lo},
       {0x08u, 0x39600000u | (patch.menu_flag & 0xffffu)},
       {0x0Cu, 0x916C0000u},
       {0x10u, patch.original},
       {0x14u, PpcBranch(patch.cave + 0x14u, return_addr)}});
  return patch.applied;
}

bool ApplyGameFlowFlagPatches(const Core::CPUThreadGuard& guard)
{
  bool wrote = false;
  for (GameFlowFlagPatch& patch : s_game_flow_flag_patches)
    wrote = ApplyGameFlowFlagPatch(guard, patch) || wrote;
  return wrote;
}

bool RestoreGameFlowFlagPatch(const Core::CPUThreadGuard& guard, GameFlowFlagPatch& patch)
{
  const u32 branch = PpcBranch(patch.address, patch.cave);
  u32 current = 0;
  if (!TryReadU32(guard, patch.address, &current) || current != branch)
  {
    patch.applied = false;
    return false;
  }

  u32 original = patch.original;
  if (original == 0)
    TryReadU32(guard, patch.cave + 0x10u, &original);
  if (original == 0)
    return false;

  const bool restored = TryWriteInstruction(guard, patch.address, original);
  if (restored)
  {
    patch.original = original;
    patch.applied = false;
  }
  return restored;
}

bool RestoreGameFlowFlagPatches(const Core::CPUThreadGuard& guard)
{
  bool restored = false;
  for (GameFlowFlagPatch& patch : s_game_flow_flag_patches)
    restored = RestoreGameFlowFlagPatch(guard, patch) || restored;
  return restored;
}

bool ApplyConditionalCombatPitchPatch(const Core::CPUThreadGuard& guard, DynamicPpcPatch& patch)
{
  u32 current = 0;
  if (!TryReadU32(guard, patch.address, &current))
    return false;

  const u32 branch = PpcBranch(patch.address, patch.cave);
  if (current != branch && current != patch.original && current != patch.replacement)
  {
    patch.applied = false;
    return false;
  }

  constexpr u32 flag_hi = (PITCH_ZERO_ENABLE_SCRATCH >> 16) & 0xffffu;
  constexpr u32 flag_lo = PITCH_ZERO_ENABLE_SCRATCH & 0xffffu;
  const u32 original_label = patch.cave + 0x18u;
  const u32 zero_label = patch.cave + 0x20u;
  const u32 return_addr = patch.address + 4u;

  const std::initializer_list<HookWrite> cave_writes{
      {0x00u, 0x3D800000u | flag_hi},
      {0x04u, 0x618C0000u | flag_lo},
      {0x08u, 0x818C0000u},
      {0x0Cu, 0x2C0C0000u},
      {0x10u, PpcBeq(patch.cave + 0x10u, original_label)},
      {0x14u, PpcBranch(patch.cave + 0x14u, zero_label)},
      {0x18u, patch.original},
      {0x1Cu, PpcBranch(patch.cave + 0x1Cu, return_addr)},
      {0x20u, patch.replacement},
      {0x24u, PpcBranch(patch.cave + 0x24u, return_addr)}};
  const bool cave_matches = InstructionBlockMatches(guard, patch.cave, cave_writes);
  if (!cave_matches && !TryWriteInstructionBlock(guard, patch.cave, cave_writes))
  {
    patch.applied = false;
    return false;
  }

  if (current == branch)
  {
    patch.applied = true;
    return !cave_matches;
  }

  patch.applied = TryWriteInstruction(guard, patch.address, branch);
  return patch.applied;
}

bool ApplyConditionalElevationPitchPatch(const Core::CPUThreadGuard& guard, DynamicPpcPatch& patch)
{
  u32 current = 0;
  if (!TryReadU32(guard, patch.address, &current))
    return false;

  const u32 branch = PpcBranch(patch.address, patch.cave);
  if (current != branch && current != patch.original)
  {
    patch.applied = false;
    return false;
  }

  constexpr u32 flag_hi = (PITCH_ZERO_ENABLE_SCRATCH >> 16) & 0xffffu;
  constexpr u32 flag_lo = PITCH_ZERO_ENABLE_SCRATCH & 0xffffu;
  const u32 original_label = patch.cave + 0x18u;
  const u32 zero_label = patch.cave + 0x20u;
  const u32 return_addr = patch.address + 4u;

  const std::initializer_list<HookWrite> cave_writes{
      {0x00u, 0x3D800000u | flag_hi},
      {0x04u, 0x618C0000u | flag_lo},
      {0x08u, 0x818C0000u},
      {0x0Cu, 0x2C0C0000u},
      {0x10u, PpcBeq(patch.cave + 0x10u, original_label)},
      {0x14u, PpcBranch(patch.cave + 0x14u, zero_label)},
      {0x18u, patch.original},
      {0x1Cu, PpcBranch(patch.cave + 0x1Cu, return_addr)},
      {0x20u, 0xC00280B0u},
      {0x24u, patch.original},
      {0x28u, PpcBranch(patch.cave + 0x28u, return_addr)}};
  const bool cave_matches = InstructionBlockMatches(guard, patch.cave, cave_writes);
  if (!cave_matches && !TryWriteInstructionBlock(guard, patch.cave, cave_writes))
  {
    patch.applied = false;
    return false;
  }

  if (current == branch)
  {
    patch.applied = true;
    return !cave_matches;
  }

  patch.applied = TryWriteInstruction(guard, patch.address, branch);
  return patch.applied;
}

bool RestoreLegacyMorphBallCameraReturnPatch(const Core::CPUThreadGuard& guard)
{
  u32 current = 0;
  if (!TryReadU32(guard, LEGACY_MORPHBALL_CAMERA_RETURN_ADDRESS, &current))
    return false;

  const u32 legacy_branch =
      PpcBranch(LEGACY_MORPHBALL_CAMERA_RETURN_ADDRESS, LEGACY_MORPHBALL_CAMERA_RETURN_CAVE);
  if (current != legacy_branch)
    return false;

  return TryWriteInstruction(guard, LEGACY_MORPHBALL_CAMERA_RETURN_ADDRESS,
                             LEGACY_MORPHBALL_CAMERA_RETURN_ORIGINAL);
}

bool ApplyBallCameraLevelPatch(const Core::CPUThreadGuard& guard)
{
  auto& patch = s_ball_camera_level_patch;
  u32 current = 0;
  if (!TryReadU32(guard, patch.address, &current))
    return false;

  const u32 branch = PpcBranch(patch.address, patch.cave);
  if (current != branch && current != patch.original)
  {
    patch.applied = false;
    return false;
  }

  const u32 return_addr = patch.address + 4u;

  const std::initializer_list<HookWrite> cave_writes{
      // copyXf lives at r1+0x80. Keep yaw from the right vector, flatten pitch,
      // set world up, and leave translation untouched. Use existing r2 small-data
      // constants so the inline hook does not leak a scratch GPR into game code.
      {0x00u, 0xC0010090u},  // lfs f0, 0x90(r1)  ; right.y
      {0x04u, 0xFC000050u},  // fneg f0, f0
      {0x08u, 0xD0010084u},  // stfs f0, 0x84(r1) ; forward.x = -right.y
      {0x0Cu, 0xC0010080u},  // lfs f0, 0x80(r1)  ; right.x
      {0x10u, 0xD0010094u},  // stfs f0, 0x94(r1) ; forward.y = right.x

      {0x14u, 0xC00280B0u},  // lfs f0, -0x7F50(r2) ; 0.0f
      {0x18u, 0xD0010088u},  // up.x
      {0x1Cu, 0xD0010098u},  // up.y
      {0x20u, 0xD00100A0u},  // right.z
      {0x24u, 0xD00100A4u},  // forward.z

      {0x28u, 0xC00280B4u},  // lfs f0, -0x7F4C(r2) ; 1.0f
      {0x2Cu, 0xD00100A8u},  // up.z

      {0x30u, patch.original},
      {0x34u, PpcBranch(patch.cave + 0x34u, return_addr)},
      {0x38u, 0x60000000u},
      {0x3Cu, 0x60000000u}};

  const bool patch_wrote = InstallBranchAfterCaveWrite(guard, patch.address, patch.cave, cave_writes);
  patch.applied = InstructionBlockMatches(guard, patch.cave, cave_writes) &&
                  TryReadU32(guard, patch.address, &current) && current == branch;
  return patch_wrote;
}

bool ApplyInterpolationCameraLevelPatch(const Core::CPUThreadGuard& guard)
{
  auto& patch = s_interpolation_camera_level_patch;
  u32 current = 0;
  if (!TryReadU32(guard, patch.address, &current))
    return false;

  const u32 branch = PpcBranch(patch.address, patch.cave);
  if (current != branch && current != patch.original)
  {
    patch.applied = false;
    return false;
  }

  constexpr u32 flag_hi = (MORPHBALL_CAMERA_LEVEL_ENABLE_SCRATCH >> 16) & 0xffffu;
  constexpr u32 flag_lo = MORPHBALL_CAMERA_LEVEL_ENABLE_SCRATCH & 0xffffu;
  const u32 original_label = patch.cave + 0x44u;
  const u32 return_addr = patch.address + 4u;

  const std::initializer_list<HookWrite> cave_writes{
      // CInterpolationCamera::Think writes the transition transform to this+0x34
      // immediately before this patch site. Flatten only while the player is
      // morphed so first-person/cinematic transitions keep their original pitch.
      {0x00u, 0x3D800000u | flag_hi},
      {0x04u, 0x618C0000u | flag_lo},
      {0x08u, 0x818C0000u},
      {0x0Cu, 0x2C0C0000u},  // cmpwi r12, 0
      {0x10u, PpcBeq(patch.cave + 0x10u, original_label)},

      {0x14u, 0xC01E0038u},  // lfs f0, 0x38(r30)  ; right.y
      {0x18u, 0xFC000050u},  // fneg f0, f0
      {0x1Cu, 0xD01E0044u},  // stfs f0, 0x44(r30) ; forward.x = -right.y
      {0x20u, 0xC01E0034u},  // lfs f0, 0x34(r30)  ; right.x
      {0x24u, 0xD01E0048u},  // stfs f0, 0x48(r30) ; forward.y = right.x

      {0x28u, 0xC00280B0u},  // lfs f0, -0x7F50(r2) ; 0.0f
      {0x2Cu, 0xD01E003Cu},  // right.z
      {0x30u, 0xD01E004Cu},  // forward.z
      {0x34u, 0xD01E0054u},  // up.x
      {0x38u, 0xD01E0058u},  // up.y

      {0x3Cu, 0xC00280B4u},  // lfs f0, -0x7F4C(r2) ; 1.0f
      {0x40u, 0xD01E005Cu},  // up.z

      {0x44u, patch.original},
      {0x48u, PpcBranch(patch.cave + 0x48u, return_addr)}};

  const bool patch_wrote = InstallBranchAfterCaveWrite(guard, patch.address, patch.cave, cave_writes);
  patch.applied = InstructionBlockMatches(guard, patch.cave, cave_writes) &&
                  TryReadU32(guard, patch.address, &current) && current == branch;
  return patch_wrote;
}

bool ApplyFirstPersonOrbitAimVectorPatch(const Core::CPUThreadGuard& guard)
{
  auto& patch = s_first_person_orbit_aim_vector_patch;
  u32 current = 0;
  if (!TryReadU32(guard, patch.address, &current))
    return false;

  const u32 branch = PpcBranch(patch.address, patch.cave);
  if (current != branch && current != patch.original)
  {
    patch.applied = false;
    return false;
  }

  const u32 return_addr = patch.address + 4u;
  constexpr u32 flag_hi = (FIRST_PERSON_ORBIT_AIM_VECTOR_ENABLE_SCRATCH >> 16) & 0xffffu;
  constexpr u32 flag_lo = FIRST_PERSON_ORBIT_AIM_VECTOR_ENABLE_SCRATCH & 0xffffu;
  const std::initializer_list<HookWrite> cave_writes{
      // CFirstPersonCamera::UpdateTransform normally replaces rVec with the
      // orbit/scan target vector here. Keep the hook installed permanently and
      // gate the scan-only yaw lock through a scratch flag so changing visors
      // does not rewrite code or invalidate JIT.
      {0x00u, 0x3D800000u | flag_hi},  // lis r12, flag@ha
      {0x04u, 0x818C0000u | flag_lo},  // lwz r12, flag@l(r12)
      {0x08u, 0x2C0C0000u},            // cmpwi r12, 0
      {0x0Cu, PpcBeq(patch.cave + 0x0Cu, patch.cave + 0x20u)},

      {0x10u, patch.original},  // lwz r0, 0x304(r30)
      {0x14u, 0x2C000000u},     // cmpwi r0, 0
      {0x18u, PpcBeq(patch.cave + 0x18u, patch.cave + 0x24u)},
      {0x1Cu, PpcBranch(patch.cave + 0x1Cu, FIRST_PERSON_ORBIT_AIM_VECTOR_SKIP_ADDRESS)},

      {0x20u, patch.original},
      {0x24u, PpcBranch(patch.cave + 0x24u, return_addr)}};

  const bool patch_wrote = InstallBranchAfterCaveWrite(guard, patch.address, patch.cave, cave_writes);
  patch.applied = InstructionBlockMatches(guard, patch.cave, cave_writes) &&
                  TryReadU32(guard, patch.address, &current) && current == branch;
  return patch_wrote;
}

bool RestoreFirstPersonOrbitAimVectorPatch(const Core::CPUThreadGuard& guard)
{
  auto& patch = s_first_person_orbit_aim_vector_patch;
  u32 current = 0;
  if (!TryReadU32(guard, patch.address, &current))
    return false;

  const u32 branch = PpcBranch(patch.address, patch.cave);
  if (current != branch)
  {
    patch.applied = false;
    return false;
  }

  const bool restored = TryWriteInstruction(guard, patch.address, patch.original);
  if (restored)
    patch.applied = false;
  return restored;
}

bool ApplyAudioListenerPatch(const Core::CPUThreadGuard& guard)
{
  auto& patch = s_audio_listener_patch;
  u32 current = 0;
  if (!TryReadU32(guard, patch.address, &current))
    return false;

  const u32 branch = PpcBranch(patch.address, patch.cave);
  const u32 legacy_bad_branch = PpcBranch(patch.address, AUDIO_LISTENER_LEGACY_BAD_CAVE);
  const u32 legacy_high_branch = PpcBranch(patch.address, AUDIO_LISTENER_LEGACY_HIGH_CAVE);
  if (current != branch && current != patch.original && current != legacy_bad_branch &&
      current != legacy_high_branch)
  {
    patch.applied = false;
    return false;
  }

  constexpr u32 scratch_hi = (AUDIO_LISTENER_SCRATCH >> 16) & 0xffffu;
  constexpr u32 scratch_lo = AUDIO_LISTENER_SCRATCH & 0xffffu;
  const u32 original_label = patch.cave + 0x60u;
  const u32 return_addr = patch.address + 0x28u;
  const std::initializer_list<HookWrite> cave_writes{
      {0x00u, 0x3D800000u | scratch_hi},  // lis r12, scratch@ha
      {0x04u, 0x618C0000u | scratch_lo},  // ori r12, r12, scratch@l
      {0x08u, 0x800C0000u},               // lwz r0, 0(r12)
      {0x0Cu, 0x2C000000u},               // cmpwi r0, 0
      {0x10u, PpcBeq(patch.cave + 0x10u, original_label)},

      {0x14u, 0x88E2CC40u},  // lbz r7, -0x33c0(r2)
      {0x18u, 0xC10C0010u},  // up.x
      {0x1Cu, 0xD1010008u},
      {0x20u, 0xC10C0014u},  // up.y
      {0x24u, 0xD101000Cu},
      {0x28u, 0xC10C0018u},  // up.z
      {0x2Cu, 0xD1010010u},
      {0x30u, 0xC10C0004u},  // heading.x
      {0x34u, 0xD1010014u},
      {0x38u, 0xC10C0008u},  // heading.y
      {0x3Cu, 0xD1010018u},
      {0x40u, 0xC10C000Cu},  // heading.z
      {0x44u, 0xD101001Cu},
      {0x48u, 0xD0410020u},  // original translation column
      {0x4Cu, 0xD0210024u},
      {0x50u, 0xD0010028u},
      {0x54u, PpcBranch(patch.cave + 0x54u, return_addr)},
      {0x58u, 0x60000000u},
      {0x5Cu, 0x60000000u},

      {0x60u, patch.original},
      {0x64u, 0x88E2CC40u},
      {0x68u, 0xD0E1000Cu},
      {0x6Cu, 0xD0C10010u},
      {0x70u, 0xD0A10014u},
      {0x74u, 0xD0810018u},
      {0x78u, 0xD061001Cu},
      {0x7Cu, 0xD0410020u},
      {0x80u, 0xD0210024u},
      {0x84u, 0xD0010028u},
      {0x88u, PpcBranch(patch.cave + 0x88u, return_addr)}};

  const bool patch_wrote = InstallBranchAfterCaveWrite(guard, patch.address, patch.cave, cave_writes);
  patch.applied = InstructionBlockMatches(guard, patch.cave, cave_writes) &&
                  TryReadU32(guard, patch.address, &current) && current == branch;
  return patch_wrote;
}

void UpdatePitchZeroHookEnabled(const Core::CPUThreadGuard& guard, bool scan_active)
{
  // Keep the original pitch-zero behavior active in scan mode too; otherwise Prime's
  // scan/orbit path recenters pitch when the reticle resolves a target.
  (void)scan_active;
  TryWriteU32(guard, PITCH_ZERO_ENABLE_SCRATCH, 1u);
}

void UpdateMorphballCameraLevelHookEnabled(const Core::CPUThreadGuard& guard, bool have_player,
                                           u32 player)
{
  u32 morph_state = 0;
  const bool enabled = have_player && player >= 0x80000000u &&
                       TryReadU32(guard, player + 0x2F8u, &morph_state) && morph_state != 0;
  TryWriteU32(guard, MORPHBALL_CAMERA_LEVEL_ENABLE_SCRATCH, enabled ? 1u : 0u);
}

bool ApplyCombatPitchPatches(const Core::CPUThreadGuard& guard)
{
  bool wrote = false;
  u32 player = 0;
  const bool scan_active =
      TryReadU32(guard, ADDRESS.state_manager + ADDRESS.player_offset, &player) &&
      player >= 0x80000000u && ScanVisorActive(guard, player);

  wrote = ApplyGameFlowFlagPatches(guard) || wrote;
  wrote = ApplyRenderModelOffsetPatch(guard) || wrote;
  wrote = RuntimeLoggingEnabled() ? (ApplyScanReticleTracePatch(guard) || wrote) :
                                    (RestoreScanReticleTracePatches(guard) || wrote);
  wrote = ApplyScanIndicatorPointerPatch(guard) || wrote;
  wrote = ApplyScanIndicatorViewBasisPatch(guard) || wrote;
  wrote = ApplyFirstPersonPitchLoadPatch(guard) || wrote;
  wrote = ApplyBallCameraLevelPatch(guard) || wrote;
  wrote = ApplyInterpolationCameraLevelPatch(guard) || wrote;
  wrote = ApplyFirstPersonOrbitAimVectorPatch(guard) || wrote;
  TryWriteU32(guard, FIRST_PERSON_ORBIT_AIM_VECTOR_ENABLE_SCRATCH, scan_active ? 1u : 0u);
  wrote = ApplyAudioListenerPatch(guard) || wrote;

  UpdatePitchZeroHookEnabled(guard, scan_active);

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

    if (!TryWriteInstruction(guard, patch.address, patch.value))
      continue;

    wrote_instruction = true;
  }

  if (wrote_instruction)
    InvalidatePrimedGunPatchICache(system);
}

bool BuiltinPatchGroupApplied(const Core::CPUThreadGuard& guard, const RuntimeSettings& settings,
                              u32 group)
{
  std::call_once(s_parse_builtin_patches_once, ParseBuiltinPatches);
  if (!PatchGroupEnabled(settings, group))
    return true;

  bool saw_group = false;
  for (const PatchWrite& patch : s_builtin_patches)
  {
    if (patch.group != group)
      continue;

    saw_group = true;
    const auto current = PowerPC::MMU::HostTryRead<u32>(guard, patch.address);
    if (!current || current->value != patch.value)
      return false;
  }

  return saw_group;
}

void VerifyCriticalBuiltinPatches(Core::System& system, const Core::CPUThreadGuard& guard,
                                  const RuntimeSettings& settings, bool have_player)
{
  if (!settings.builtin_patches_enabled || !have_player)
    return;

  if (BuiltinPatchGroupApplied(guard, settings, PatchCannonRotation) &&
      BuiltinPatchGroupApplied(guard, settings, PatchGunRayTarget) &&
      BuiltinPatchGroupApplied(guard, settings, PatchReticle))
  {
    return;
  }

  s_patches_applied_this_boot = false;
  ApplyBuiltinPatches(system, guard);
  InvalidatePrimedGunPatchICache(system);
}

void ResetCannonTrackingFeedState(const Core::CPUThreadGuard& guard)
{
  s_cannon_hand_pose_ready = false;
  s_smooth_matrix_valid = false;
  s_last_validated_gun = 0;
  ClearCannonRuntimeScratch(guard);
  WriteGunTargetScratch(guard, 0, 0xffffu);
  TryWriteU32(guard, RETICLE_BILLBOARD_SCRATCH, 0);
}

bool CannonFeedScratchLooksEmpty(const Core::CPUThreadGuard& guard)
{
  u32 expected_gun = 0;
  if (!TryReadU32(guard, CANNON_EXPECTED_GUN_SCRATCH, &expected_gun) || expected_gun != 0)
    return false;

  for (u32 offset = 0; offset < 0x24u; offset += 4u)
  {
    u32 word = 0;
    if (!TryReadU32(guard, CANNON_BASIS_SCRATCH + offset, &word) || word != 0)
      return false;
  }

  return true;
}

bool ReticleBillboardScratchLooksValid(const Core::CPUThreadGuard& guard)
{
  u32 enabled = 0;
  if (!TryReadU32(guard, RETICLE_BILLBOARD_SCRATCH, &enabled) || enabled != 1)
    return false;

  float m[9] = {};
  for (int i = 0; i < 9; ++i)
  {
    if (!TryReadFloat(guard, RETICLE_BILLBOARD_SCRATCH + 4u + static_cast<u32>(i * 4), &m[i]) ||
        !std::isfinite(m[i]))
    {
      return false;
    }
  }

  const float row0_len = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
  const float row1_len = std::sqrt(m[3] * m[3] + m[4] * m[4] + m[5] * m[5]);
  const float row2_len = std::sqrt(m[6] * m[6] + m[7] * m[7] + m[8] * m[8]);
  if (row0_len < 0.5f || row0_len > 1.5f || row1_len < 0.5f || row1_len > 1.5f ||
      row2_len < 0.5f || row2_len > 1.5f)
  {
    return false;
  }

  const float dot01 = std::fabs(m[0] * m[3] + m[1] * m[4] + m[2] * m[5]);
  const float dot02 = std::fabs(m[0] * m[6] + m[1] * m[7] + m[2] * m[8]);
  const float dot12 = std::fabs(m[3] * m[6] + m[4] * m[7] + m[5] * m[8]);
  return dot01 <= 0.35f && dot02 <= 0.35f && dot12 <= 0.35f;
}

bool ScanTargetScratchLooksCurrent(const Core::CPUThreadGuard& guard, u32 player)
{
  u32 scratch_player = 0;
  u16 uid = 0xffffu;
  return TryReadU32(guard, GUN_TARGET_SCRATCH, &scratch_player) && scratch_player == player &&
         TryReadU16(guard, GUN_TARGET_SCRATCH + 4u, &uid) && (uid == 0xffffu || uid <= 0x03ffu);
}

void VerifyScanReticleWatchdog(const Core::CPUThreadGuard& guard,
                               const RuntimeSettings& settings, bool have_player, u32 player,
                               bool scan_active)
{
  if (!settings.enabled || !settings.builtin_patches_enabled || !have_player || !scan_active)
  {
    s_scan_reticle_bad_samples = 0;
    return;
  }

  if (s_frame_counter < s_last_scan_reticle_watchdog_frame + 30u)
    return;
  s_last_scan_reticle_watchdog_frame = s_frame_counter;

  const bool scratch_ok = ScanTargetScratchLooksCurrent(guard, player) &&
                          ReticleBillboardScratchLooksValid(guard);
  if (scratch_ok)
  {
    s_scan_reticle_bad_samples = 0;
    return;
  }

  ++s_scan_reticle_bad_samples;
  if (s_scan_reticle_bad_samples < 2)
    return;

  s_scan_reticle_bad_samples = 0;
  ClearScanReticleScratch(guard, player);
  s_smooth_scan_pitch = 0.0f;
}

void VerifyCannonFeedWatchdog(Core::System& system, const Core::CPUThreadGuard& guard,
                              const RuntimeSettings& settings, bool have_player, u32 player)
{
  (void)system;

  if (!settings.enabled || !settings.builtin_patches_enabled || !have_player)
  {
    s_cannon_feed_stall_frames = 0;
    return;
  }

  if (ScanVisorActive(guard, player))
  {
    s_cannon_feed_stall_frames = 0;
    return;
  }

  if (s_frame_counter < s_last_cannon_feed_watchdog_frame + 180u)
    return;
  s_last_cannon_feed_watchdog_frame = s_frame_counter;

  u32 gun = 0;
  const bool cannon_ready = PlayerIsFirstPersonGunReady(guard, player) &&
                            TryReadU32(guard, player + ADDRESS.cannon_offset, &gun) &&
                            gun >= 0x80000000u;
  if (!cannon_ready)
  {
    s_cannon_feed_stall_frames = 0;
    return;
  }

  if (!CannonFeedScratchLooksEmpty(guard))
  {
    s_cannon_feed_stall_frames = 0;
    return;
  }

  ++s_cannon_feed_stall_frames;
  if (s_cannon_feed_stall_frames < 3)
    return;

  s_cannon_feed_stall_frames = 0;
  ResetCannonTrackingFeedState(guard);
}

void UpdateShaderHunterGameFlowFlags(const Core::CPUThreadGuard& guard)
{
  u32 player = 0;
  if (TryReadU32(guard, ADDRESS.state_manager + ADDRESS.player_offset, &player) &&
      PlayerObjectLooksValid(guard, player) && PlayerIsInMenuMapOrMorphball(guard, player))
  {
    ShaderHunter::GetInstance().RegisterExternalFlag("primedgun_map_or_pause");
  }
}
}  // namespace

void OnFrameEnd(Core::System& system, const Core::CPUThreadGuard& guard)
{
  ++s_frame_counter;

  const RuntimeSettings settings = GetRuntimeSettings();
  if (!settings.enabled)
  {
    TryWriteU32(guard, MORPHBALL_CAMERA_LEVEL_ENABLE_SCRATCH, 0);
    TryWriteU32(guard, FIRST_PERSON_ORBIT_AIM_VECTOR_ENABLE_SCRATCH, 0);
    ClearAudioListenerScratch(guard);
    ClearCinematicScreenState(settings.cinematic_screen_enabled);
    s_gameplay_input_hold_until_frame = 0;
    s_gameplay_input_active.store(false, std::memory_order_relaxed);
    s_orbit_lock_active.store(false, std::memory_order_relaxed);
    return;
  }

  const bool game_active = Core::IsRunning(system) && IsMetroidPrimeRev0(guard);
  if (!game_active)
  {
    TryWriteU32(guard, MORPHBALL_CAMERA_LEVEL_ENABLE_SCRATCH, 0);
    TryWriteU32(guard, FIRST_PERSON_ORBIT_AIM_VECTOR_ENABLE_SCRATCH, 0);
    ClearAudioListenerScratch(guard);
    ClearCinematicScreenState(settings.cinematic_screen_enabled);
    s_gameplay_input_hold_until_frame = 0;
    s_gameplay_input_active.store(false, std::memory_order_relaxed);
    s_orbit_lock_active.store(false, std::memory_order_relaxed);
    if (s_game_was_active)
      ResetNativeRuntime();
    s_game_was_active = false;
    return;
  }

  s_game_was_active = true;
  UpdateCinematicScreenState(guard, settings);
  UpdateShaderHunterGameFlowFlags(guard);
  if (settings.builtin_patches_enabled && RestoreLegacyMorphBallCameraReturnPatch(guard))
    InvalidatePrimedGunPatchICache(system);

  u32 player = 0;
  const bool have_player =
      TryReadU32(guard, ADDRESS.state_manager + ADDRESS.player_offset, &player) &&
      PlayerObjectLooksValid(guard, player);
  if (!have_player)
  {
    TryWriteU32(guard, MORPHBALL_CAMERA_LEVEL_ENABLE_SCRATCH, 0);
    TryWriteU32(guard, FIRST_PERSON_ORBIT_AIM_VECTOR_ENABLE_SCRATCH, 0);
    ClearAudioListenerScratch(guard);
    if (s_last_patch_player != 0)
      s_patch_reapply_until_frame = s_frame_counter + 180u;
    s_last_patch_player = 0;
    s_patches_applied_this_boot = false;
    const bool held_gameplay_input_active = s_frame_counter < s_gameplay_input_hold_until_frame;
    s_gameplay_input_active.store(held_gameplay_input_active, std::memory_order_relaxed);
    s_orbit_lock_active.store(false, std::memory_order_relaxed);
#ifdef ENABLE_VR
    PublishVrOverlayState(settings, false);
#endif
    return;
  }

  if (player != s_last_patch_player)
  {
    s_last_patch_player = player;
    s_patches_applied_this_boot = false;
    s_patch_reapply_until_frame = s_frame_counter + 180u;
    TryWriteU32(guard, GAMEFLOW_MENU_SCRATCH, 0);
  }
  UpdateMorphballCameraLevelHookEnabled(guard, have_player, player);

  bool dynamic_patch_applied = false;
  if (settings.builtin_patches_enabled)
  {
    dynamic_patch_applied = ApplyCombatPitchPatches(guard) || dynamic_patch_applied;
    if (settings.patch_beam_projectile_timing)
      dynamic_patch_applied = ApplyProjectileTransformPatches(guard) || dynamic_patch_applied;
  }
  if (dynamic_patch_applied)
    InvalidatePrimedGunPatchICache(system);

  // Recheck occasionally so user/loaded-state changes cannot silently undo PrimedGun patches.
  if (settings.builtin_patches_enabled &&
      (!s_patches_applied_this_boot || (s_frame_counter % 60) == 0))
  {
    ApplyBuiltinPatches(system, guard);
    s_patches_applied_this_boot = true;
  }

  if ((s_frame_counter % 60) == 1)
    ApplyHelmetOpacityZero(guard, settings);

  const bool patch_reapply_window = have_player && s_frame_counter < s_patch_reapply_until_frame;
  if (settings.builtin_patches_enabled &&
      (patch_reapply_window || !s_patches_applied_this_boot || (s_frame_counter % 60) == 0))
  {
    ApplyBuiltinPatches(system, guard);
    s_patches_applied_this_boot = true;
  }

  const bool default_controls_active = have_player && PlayerIsInMenuMapOrMorphball(guard, player);
  if (default_controls_active)
    s_gameplay_input_hold_until_frame = 0;

  const bool scan_active = have_player && !default_controls_active && ScanVisorActive(guard, player);
  if (settings.builtin_patches_enabled && have_player)
    FlattenActiveMorphballCameraTransform(guard, player);

  const bool raw_gameplay_input_active =
      have_player && !default_controls_active && PlayerIsFirstPersonUnmorphed(guard, player);
  const bool visor_transition_input_active =
      !raw_gameplay_input_active && have_player && !default_controls_active &&
      PlayerIsChangingVisorsInFirstPerson(guard, player);
  const bool direct_gameplay_input_active =
      raw_gameplay_input_active || visor_transition_input_active;
  if (direct_gameplay_input_active)
    s_gameplay_input_hold_until_frame = s_frame_counter + GAMEPLAY_INPUT_LOSS_HOLD_FRAMES;
  const bool gameplay_input_active =
      direct_gameplay_input_active ||
      (!default_controls_active && s_frame_counter < s_gameplay_input_hold_until_frame);
  UpdatePitchZeroHookEnabled(guard, scan_active);
  const bool orbit_lock_active =
      gameplay_input_active && OrbitLockButtonHeld(guard, ADDRESS.state_manager) &&
      PlayerHasOrbitControlTarget(guard, ADDRESS.state_manager, player);
  s_gameplay_input_active.store(gameplay_input_active, std::memory_order_relaxed);
  s_orbit_lock_active.store(orbit_lock_active, std::memory_order_relaxed);
  if (!have_player || (!scan_active && !gameplay_input_active))
    WriteGunTargetScratch(guard, 0, 0xffffu);

  if (have_player && RuntimeLoggingEnabled())
  {
    LogModeProbe(guard, player, gameplay_input_active != s_last_logged_gameplay_input_active);
    LogCameraWatchdog(guard, player, default_controls_active);
    if (scan_active)
      DumpScanIndicatorCodeOnce(guard);
    LogScanReticleTrace(guard, player);
  }
  VerifyCriticalBuiltinPatches(system, guard, settings, have_player);
  VerifyScanReticleWatchdog(guard, settings, have_player, player, scan_active);
  VerifyCannonFeedWatchdog(system, guard, settings, have_player, player);
  if (RuntimeLoggingEnabled())
  {
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
                     "PrimedGun gameplay_input={} player={:08X} camera={} morph={} "
                     "input_flags={:02X} move={} visor={} gun_alpha={:.3f} holster={}",
                     gameplay_input_active, player, camera_state, morph_state, input_flags,
                     movement_state, visor_state, gun_alpha, holster_state);
      s_have_logged_gameplay_input_active = true;
      s_last_logged_gameplay_input_active = gameplay_input_active;
    }
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
  s_dpad_input_player = 0;
  s_dpad_input_flags_addr = 0;
  s_dpad_last_disable_refresh_frame = 0;
  s_directional_move_speed = 0.0f;
  s_gameplay_input_hold_until_frame = 0;
  s_gameplay_input_active.store(false, std::memory_order_relaxed);
  s_orbit_lock_active.store(false, std::memory_order_relaxed);
  s_last_logged_gameplay_input_active = false;
  s_have_logged_gameplay_input_active = false;
  s_last_mode_probe_frame = 0;
  s_last_camera_watchdog_frame = 0;
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
  s_last_scan_reticle_trace_frame = 0;
  s_have_dumped_scan_indicator_code = false;
  s_last_validated_gun = 0;
  s_last_patch_player = 0;
  s_patch_reapply_until_frame = 0;
  s_last_cannon_feed_watchdog_frame = 0;
  s_cannon_feed_stall_frames = 0;
  s_cinematic_screen_generation = 0;
  s_cinematic_screen_active = false;
  s_cinematic_screen_hold_until_frame = 0;
  s_last_scan_reticle_watchdog_frame = 0;
  s_scan_reticle_bad_samples = 0;
  s_cannon_hand_pose_ready = false;
  s_smooth_matrix_valid = false;
  s_cannon_smoothing_bypass_until_frame = 0;
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
  s_snap_turn_ready = true;
  s_snap_turn_cooldown_until_frame = 0;
  s_vr_menu_tab = 0;
  s_vr_menu_selected_index = 0;
  s_vr_menu_calibration_page = 0;
  s_vr_menu_control_page = 0;
  ++s_vr_menu_generation;
  s_vr_menu_saved_notice_until_frame = 0;
  s_vr_menu_long_press_start_frame = 0;
  s_vr_menu_long_press_consumed = false;
  s_vr_cannon_texture_notice_until_frame = 0;
  s_vr_state_load_requested.store(false, std::memory_order_release);
  s_vr_state_save_requested.store(false, std::memory_order_release);
  s_vr_state_load_newest_requested.store(false, std::memory_order_release);
  s_vr_state_save_oldest_requested.store(false, std::memory_order_release);
  s_vr_state_slot_select_requested.store(0, std::memory_order_release);
  s_vr_state_slot_from_ui.store(0, std::memory_order_release);
  s_vr_state_slot = 1;
  s_vr_state_confirm_action = 0;
  s_vr_state_confirm_until_frame = 0;
  s_vr_reset_confirm_action = 0;
  s_vr_reset_confirm_until_frame = 0;
  s_height_prompt_until_frame = 0;
  s_prompt_gameplay_ready_since_frame = 0;
  s_prompt_first_ready_timeout_frame = 0;
  s_prompt_skipped_first_ready = false;
  s_prompt_waiting_for_second_ready = false;
#ifdef ENABLE_VR
  Common::VR::OpenXRInputState::SetPrimedGunOverlay({});
#endif
  s_first_person_pitch_load_patch.applied = false;
  s_render_model_offset_patch.applied = false;
  s_render_model_offset_patch.original = 0;
  s_scan_reticle_trace_patch.applied = false;
  s_scan_reticle_trace_patch.original = 0;
  s_scan_reticle_trace_curr_patch.applied = false;
  s_scan_reticle_trace_curr_patch.original = 0;
  s_scan_indicator_update_trace_patch.applied = false;
  s_scan_indicator_update_trace_patch.original = 0;
  s_scan_indicator_view_basis_patch.applied = false;
  s_scan_indicator_view_basis_patch.original = DRAW_SCAN_INDICATOR_MODEL_BASIS_ORIGINAL;
  s_first_person_orbit_aim_vector_patch.applied = false;
  s_audio_listener_patch.applied = false;
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
  const RuntimeSettings defaults{};
  s_settings.offset_x = ClampFinite(s_settings.offset_x, defaults.offset_x, -10.0f, 10.0f);
  s_settings.offset_y = ClampFinite(s_settings.offset_y, defaults.offset_y, -10.0f, 10.0f);
  s_settings.offset_z = ClampFinite(s_settings.offset_z, defaults.offset_z, -10.0f, 10.0f);
  s_settings.model_offset_x =
      ClampFinite(s_settings.model_offset_x, defaults.model_offset_x, -10.0f, 10.0f);
  s_settings.model_offset_y =
      ClampFinite(s_settings.model_offset_y, defaults.model_offset_y, -10.0f, 10.0f);
  s_settings.model_offset_z =
      ClampFinite(s_settings.model_offset_z, defaults.model_offset_z, -10.0f, 10.0f);
  s_settings.rot_offset_x =
      ClampFinite(s_settings.rot_offset_x, defaults.rot_offset_x, -360.0f, 360.0f);
  s_settings.rot_offset_y =
      ClampFinite(s_settings.rot_offset_y, defaults.rot_offset_y, -360.0f, 360.0f);
  s_settings.rot_offset_z =
      ClampFinite(s_settings.rot_offset_z, defaults.rot_offset_z, -360.0f, 360.0f);
  s_settings.world_scale = ClampFinite(s_settings.world_scale, defaults.world_scale, 0.1f, 10.0f);
  s_settings.require_trigger = false;
  s_settings.trigger_threshold = defaults.trigger_threshold;
  s_settings.primegun_trackpad_press_threshold =
      ClampFinite(s_settings.primegun_trackpad_press_threshold,
                  defaults.primegun_trackpad_press_threshold, 0.05f, 1.0f);
  s_settings.rumble_intensity =
      ClampFinite(s_settings.rumble_intensity, defaults.rumble_intensity, 0.0f, 1.0f);
  s_settings.rumble_hand_mode = std::clamp(s_settings.rumble_hand_mode, 0, 2);
  s_settings.gun_targeting_distance =
      ClampFinite(s_settings.gun_targeting_distance, defaults.gun_targeting_distance, 1.0f, 500.0f);
  s_settings.gun_targeting_radius =
      ClampFinite(s_settings.gun_targeting_radius, defaults.gun_targeting_radius, 0.1f, 50.0f);
  s_settings.metroid_hud_distance =
      ClampFinite(s_settings.metroid_hud_distance, defaults.metroid_hud_distance, 0.1f, 3.0f);
  s_settings.metroid_hud_size =
      ClampFinite(s_settings.metroid_hud_size, defaults.metroid_hud_size, 0.1f, 3.0f);
  s_settings.metroid_hud_offset_up =
      ClampFinite(s_settings.metroid_hud_offset_up, defaults.metroid_hud_offset_up, 0.0f, 1.0f);
  s_settings.metroid_hud_offset_down = ClampFinite(s_settings.metroid_hud_offset_down,
                                                   defaults.metroid_hud_offset_down, 0.0f, 1.0f);
  s_settings.metroid_hud_offset_left = ClampFinite(s_settings.metroid_hud_offset_left,
                                                   defaults.metroid_hud_offset_left, 0.0f, 1.0f);
  s_settings.metroid_hud_offset_right = ClampFinite(s_settings.metroid_hud_offset_right,
                                                    defaults.metroid_hud_offset_right, 0.0f, 1.0f);
  s_settings.xr_dpad_head_radius =
      ClampFinite(s_settings.xr_dpad_head_radius, defaults.xr_dpad_head_radius, 0.02f, 2.0f);
  s_settings.xr_dpad_head_y_below =
      ClampFinite(s_settings.xr_dpad_head_y_below, defaults.xr_dpad_head_y_below, 0.0f, 2.0f);
  s_settings.xr_dpad_deadzone =
      ClampFinite(s_settings.xr_dpad_deadzone, defaults.xr_dpad_deadzone, 0.0f, 1.0f);
  s_settings.directional_movement_deadzone =
      ClampFinite(s_settings.directional_movement_deadzone,
                  defaults.directional_movement_deadzone, 0.0f, 1.0f);
  s_settings.directional_movement_speed =
      ClampFinite(s_settings.directional_movement_speed, defaults.directional_movement_speed, 0.0f,
                  100.0f);
  s_settings.directional_movement_accel =
      ClampFinite(s_settings.directional_movement_accel, defaults.directional_movement_accel, 0.0f,
                  500.0f);
  s_settings.directional_movement_air_accel =
      ClampFinite(s_settings.directional_movement_air_accel,
                  defaults.directional_movement_air_accel, 0.0f, 500.0f);
  s_settings.look_yaw_sensitivity =
      ClampFinite(s_settings.look_yaw_sensitivity, defaults.look_yaw_sensitivity, 0.1f, 5.0f);
  s_settings.snap_turn_degrees =
      SnapTurnStepDegrees(std::clamp(s_settings.snap_turn_degrees,
                                     SNAP_TURN_DEGREES_CHOICES.front(),
                                     SNAP_TURN_DEGREES_CHOICES.back()),
                          0);
#ifdef ENABLE_VR
  Common::VR::OpenXRInputState::SetRumbleConfig(s_settings.rumble_enabled,
                                                s_settings.rumble_intensity,
                                                s_settings.rumble_hand_mode);
#endif
  if (!s_settings.enabled || !s_settings.cinematic_screen_enabled)
    ClearCinematicScreenState(s_settings.cinematic_screen_enabled);
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

void SetVrStateSlot(int slot)
{
  s_vr_state_slot_from_ui.store(static_cast<int>(ClampVrStateSlot(slot)), std::memory_order_release);
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

int ConsumeVrStateSlotSelectRequest()
{
  return s_vr_state_slot_select_requested.exchange(0, std::memory_order_acq_rel);
}

bool ConsumeVrStateLoadRequest()
{
  return s_vr_state_load_requested.exchange(false, std::memory_order_acq_rel);
}

bool ConsumeVrStateSaveRequest()
{
  return s_vr_state_save_requested.exchange(false, std::memory_order_acq_rel);
}

bool ConsumeVrStateLoadNewestRequest()
{
  return s_vr_state_load_newest_requested.exchange(false, std::memory_order_acq_rel);
}

bool ConsumeVrStateSaveOldestRequest()
{
  return s_vr_state_save_oldest_requested.exchange(false, std::memory_order_acq_rel);
}
}  // namespace PrimedGun
