// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/Wiimote.h"

#include <chrono>
#include <cmath>
#include <limits>
#include <optional>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/Logging/Log.h"
#include "Common/MathUtil.h"
#include "Common/Matrix.h"
#ifdef ENABLE_VR
#include "Common/VR/OpenXRInputState.h"
#endif

#include "Core/Config/WiimoteSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"
#include "Core/HW/WiimoteEmu/Camera.h"
#include "Core/HW/WiimoteReal/WiimoteReal.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/USB/Bluetooth/BTEmu.h"
#include "Core/IOS/USB/Bluetooth/WiimoteDevice.h"
#include "Core/Movie.h"
#include "Core/System.h"
#include "Core/WiiUtils.h"

#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/ControlGroup/Cursor.h"
#include "InputCommon/ControllerEmu/ControlGroup/Attachments.h"
#include "InputCommon/ControllerEmu/StickGate.h"
#include "InputCommon/InputConfig.h"

// Limit the amount of wiimote connect requests, when a button is pressed in disconnected state
static std::array<u8, MAX_BBMOTES> s_last_connect_request_counter;

namespace
{
static std::array<std::atomic<WiimoteSource>, MAX_BBMOTES> s_wiimote_sources;
static std::optional<Config::ConfigChangedCallbackID> s_config_callback_id = std::nullopt;

bool IsEmulatedSource(WiimoteSource source)
{
  return source == WiimoteSource::Emulated || source == WiimoteSource::OpenXR;
}

#ifdef ENABLE_VR
Common::Quaternion ToQuaternion(const std::array<float, 4>& quat)
{
  return {quat[3], quat[0], quat[1], quat[2]};
}

Common::Vec3 ToVec3(const std::array<float, 3>& vec)
{
  return {vec[0], vec[1], vec[2]};
}

struct OpenXRVelocityHistory
{
  bool has_pose_sample = false;
  bool has_velocity_sample = false;
  Common::Vec3 previous_position{};
  Common::Vec3 previous_velocity{};
  std::chrono::steady_clock::time_point previous_time{};
};

struct OpenXRWiimoteState
{
  u64 generation = std::numeric_limits<u64>::max();
  Common::Vec3 acceleration{0.0f, 0.0f, float(MathUtil::GRAVITY_ACCELERATION)};
  Common::Vec3 angular_velocity{};
  float ir_x = std::numeric_limits<float>::quiet_NaN();
  float ir_y = 0.0f;
  float ir_z = 0.0f;  // Forward/backward distance offset (-1 to +1)
  // Raw aim orientation for reference-based IR computation in ApplyOpenXRIRCenter
  Common::Quaternion aim_orientation{1.0f, 0.0f, 0.0f, 0.0f};
  Common::Vec3 aim_position{};  // Aim pose world position for Z computation
  bool has_aim = false;
};

struct OpenXRIRCenter
{
  bool initialized = false;
  // Reference orientation from the HMD — "where the user is looking" becomes cursor center.
  // Controller aim deviation from this direction maps to cursor position.
  Common::Quaternion reference_orientation;
  Common::Vec3 reference_position{};  // Controller reference position for forward/backward
};

struct OpenXRCursorRanges
{
  float yaw_half_range = float(MathUtil::PI * 25.0 / 180.0 / 2.0);
  float pitch_half_range = float(MathUtil::PI * 20.0 / 180.0 / 2.0);
};

OpenXRCursorRanges GetOpenXRCursorRanges(unsigned int wiimote_index)
{
  OpenXRCursorRanges ranges;

  if (wiimote_index >= MAX_WIIMOTES)
    return ranges;

  auto* config = ::Wiimote::GetConfig();
  if (!config)
    return ranges;

  auto* wiimote = static_cast<WiimoteEmu::Wiimote*>(config->GetController(wiimote_index));
  if (!wiimote)
    return ranges;

  auto* ir_group = static_cast<ControllerEmu::Cursor*>(
      wiimote->GetWiimoteGroup(WiimoteEmu::WiimoteGroup::Point));
  if (!ir_group)
    return ranges;

  ranges.yaw_half_range = std::max(static_cast<float>(ir_group->GetTotalYaw() / 2), 0.001f);
  ranges.pitch_half_range = std::max(static_cast<float>(ir_group->GetTotalPitch() / 2), 0.001f);
  return ranges;
}

OpenXRWiimoteState BuildOpenXRState(const Common::VR::OpenXRControllerState& controller,
                                    OpenXRVelocityHistory* velocity_history)
{
  OpenXRWiimoteState out;

  const bool has_grip_pose = controller.grip_pose.valid;
  const bool has_aim_pose = controller.aim_pose.valid;
  if (!has_grip_pose && !has_aim_pose)
    return out;

  // Use aim pose for the motion reference frame. The aim pose orientation determines how
  // world-space gravity maps to Wiimote-local accelerometer axes. Using grip would produce
  // wrong accel values because grip/aim can differ by 30-60 degrees on many controllers.
  const Common::Quaternion reference_orientation =
      (has_aim_pose ? ToQuaternion(controller.aim_pose.orientation) :
                      ToQuaternion(controller.grip_pose.orientation))
          .Normalized();
  const Common::Matrix33 world_to_local =
      Common::Matrix33::FromQuaternion(reference_orientation).Transposed();

  const auto get_matrix = [&world_to_local](int row, int col) { return world_to_local.data[row * 3 + col]; };

  Common::Vec3 relative_acceleration{};
  const auto now = std::chrono::steady_clock::now();
  const float dt =
      velocity_history->has_pose_sample
          ? std::chrono::duration_cast<std::chrono::duration<float>>(now -
                                                                      velocity_history->previous_time)
                .count()
          : 0.0f;

  Common::Vec3 current_position{};
  std::optional<Common::Vec3> pose_velocity;
  if (has_grip_pose)
  {
    current_position = ToVec3(controller.grip_pose.position);
    if (velocity_history->has_pose_sample && dt > 0.001f)
      pose_velocity = (current_position - velocity_history->previous_position) / dt;
  }

  std::optional<Common::Vec3> current_velocity;
  if (controller.grip_velocity.linear_valid)
    current_velocity = ToVec3(controller.grip_velocity.linear);

  if (pose_velocity)
  {
    // Blend runtime velocity with pose-derived velocity to improve push/pull response on
    // runtimes that heavily smooth linear velocity.
    if (current_velocity)
      current_velocity = (*current_velocity + *pose_velocity) * 0.5f;
    else
      current_velocity = *pose_velocity;
  }

  if (current_velocity && velocity_history->has_velocity_sample && dt > 0.001f)
  {
    const Common::Vec3 world_accel = (*current_velocity - velocity_history->previous_velocity) / dt;
    relative_acceleration = world_to_local * world_accel;
  }

  if (has_grip_pose)
  {
    velocity_history->previous_position = current_position;
    velocity_history->previous_time = now;
    velocity_history->has_pose_sample = true;
  }
  else
  {
    velocity_history->has_pose_sample = false;
    velocity_history->has_velocity_sample = false;
  }

  if (current_velocity)
  {
    velocity_history->previous_velocity = *current_velocity;
    velocity_history->has_velocity_sample = true;
  }
  else if (!velocity_history->has_pose_sample)
  {
    velocity_history->has_velocity_sample = false;
  }

  float gx = -get_matrix(0, 1);
  float gz = get_matrix(1, 1);
  float gy = get_matrix(2, 1);

  gx -= relative_acceleration.x / float(MathUtil::GRAVITY_ACCELERATION);
  gz += relative_acceleration.y / float(MathUtil::GRAVITY_ACCELERATION);
  gy += relative_acceleration.z / float(MathUtil::GRAVITY_ACCELERATION);

  out.acceleration = Common::Vec3(gx, gy, gz) * float(MathUtil::GRAVITY_ACCELERATION);

  if (controller.grip_velocity.angular_valid)
  {
    const Common::Vec3 world_angular_velocity = ToVec3(controller.grip_velocity.angular);
    const Common::Vec3 local_angular_velocity = world_to_local * world_angular_velocity;
    out.angular_velocity =
        Common::Vec3(local_angular_velocity.x, local_angular_velocity.z, local_angular_velocity.y);
  }

  // Store raw aim orientation and position — IR angles and distance are computed
  // in ApplyOpenXRIRCenter relative to the captured reference, making the mapping
  // headset-independent.
  if (has_aim_pose)
  {
    out.aim_orientation = ToQuaternion(controller.aim_pose.orientation).Normalized();
    out.aim_position = ToVec3(controller.aim_pose.position);
    out.has_aim = true;
  }
  else
  {
    out.aim_orientation = reference_orientation;
    out.has_aim = false;
  }

  // IR x/y/z left as NaN/0/0 — will be filled by ApplyOpenXRIRCenter
  return out;
}

std::array<OpenXRIRCenter, MAX_BBMOTES> s_openxr_ir_centers{};

void ApplyOpenXRIRCenter(unsigned int wiimote_index, OpenXRWiimoteState* state,
                         const Common::VR::OpenXRPoseState& head_pose)
{
  if (wiimote_index >= MAX_BBMOTES || !state->has_aim)
    return;

  auto& center = s_openxr_ir_centers[wiimote_index];

  // On first valid frame or after recenter, capture the HMD forward direction as reference.
  // The cursor center corresponds to where the user is looking, and controller aim
  // deviation from that direction moves the cursor.
  if (!center.initialized)
  {
    if (head_pose.valid)
      center.reference_orientation = ToQuaternion(head_pose.orientation).Normalized();
    else
      center.reference_orientation = state->aim_orientation;
    center.reference_position = state->aim_position;
    center.initialized = true;
  }

  // Compute the relative rotation: how much the controller aim deviates from the
  // HMD forward direction. conjugate(reference) * current_aim
  const Common::Quaternion relative =
      center.reference_orientation.Conjugate() * state->aim_orientation;

  // Transform the forward vector (-Z) by the relative rotation to get the
  // local-frame pointing direction
  const Common::Vec3 local_forward = relative * Common::Vec3{0.0f, 0.0f, -1.0f};

  // Extract yaw and pitch from the local forward vector
  // In OpenXR space: X=right, Y=up, Z=back (right-handed)
  // local_forward.x = left/right deviation (yaw)
  // local_forward.y = up/down deviation (pitch)
  // local_forward.z = forward component (should be near -1 when pointing straight)
  const float forward_len =
      std::sqrt(local_forward.x * local_forward.x + local_forward.z * local_forward.z);
  const float yaw = std::atan2(local_forward.x, -local_forward.z);
  const float pitch = std::atan2(local_forward.y, std::max(forward_len, 0.001f));

  // Get per-wiimote cursor ranges for normalization
  const OpenXRCursorRanges ranges = GetOpenXRCursorRanges(wiimote_index);

  // Normalize to [-1, 1] range based on configured total yaw/pitch
  // Positive yaw = pointing right = positive ir_x (moves cursor right)
  // Positive pitch = pointing up = positive ir_y (moves cursor up)
  state->ir_x = yaw / std::max(ranges.yaw_half_range, 0.001f);
  state->ir_y = pitch / std::max(ranges.pitch_half_range, 0.001f);

  // Compute forward/backward distance (ir_z) from position delta projected onto
  // the reference aim direction. This changes the spacing between the two virtual
  // IR sensor bar dots — closer = dots farther apart, farther = dots closer together.
  // Scale: 0.3m of forward movement = ir_z of -1 (moves 1m closer to sensor bar).
  // Matches the old Hydra behavior (300mm per unit for Z axis).
  constexpr float Z_METERS_PER_UNIT = 0.3f;
  const Common::Vec3 ref_forward =
      center.reference_orientation * Common::Vec3{0.0f, 0.0f, -1.0f};
  const Common::Vec3 pos_delta = state->aim_position - center.reference_position;
  const float forward_displacement = pos_delta.Dot(ref_forward);
  state->ir_z = std::clamp(-forward_displacement / Z_METERS_PER_UNIT, -1.0f, 1.0f);
}

ControllerEmu::InputOverrideFunction CreateOpenXRInputOverrideFunction(unsigned int wiimote_index,
                                                                       bool prefer_left_hand)
{
  OpenXRWiimoteState cached_state;
  OpenXRVelocityHistory left_velocity_history;
  OpenXRVelocityHistory right_velocity_history;

  return [wiimote_index, prefer_left_hand, cached_state, left_velocity_history,
          right_velocity_history](std::string_view group_name, std::string_view control_name,
                                  ControlState) mutable -> std::optional<ControlState> {
    if (s_wiimote_sources[wiimote_index].load() != WiimoteSource::OpenXR)
      return std::nullopt;

    const Common::VR::OpenXRInputSnapshot snapshot = Common::VR::OpenXRInputState::GetSnapshot();
    if (!snapshot.runtime_active)
      return std::nullopt;

    if (cached_state.generation != snapshot.generation)
    {
      const auto& left = snapshot.controllers[0];
      const auto& right = snapshot.controllers[1];

      const auto right_valid = right.grip_pose.valid || right.aim_pose.valid;
      const auto left_valid = left.grip_pose.valid || left.aim_pose.valid;

      if (!prefer_left_hand && right_valid)
      {
        cached_state =
            BuildOpenXRState(right, &right_velocity_history);
        ApplyOpenXRIRCenter(wiimote_index, &cached_state, snapshot.head_pose);
      }
      else if (left_valid)
      {
        cached_state =
            BuildOpenXRState(left, &left_velocity_history);
        ApplyOpenXRIRCenter(wiimote_index, &cached_state, snapshot.head_pose);
      }
      else
      {
        cached_state = {};
      }

      cached_state.generation = snapshot.generation;
    }

    if (group_name == WiimoteEmu::Wiimote::ACCELEROMETER_GROUP)
    {
      if (control_name == ControllerEmu::ReshapableInput::X_INPUT_OVERRIDE)
        return cached_state.acceleration.x;
      if (control_name == ControllerEmu::ReshapableInput::Y_INPUT_OVERRIDE)
        return cached_state.acceleration.y;
      if (control_name == ControllerEmu::ReshapableInput::Z_INPUT_OVERRIDE)
        return cached_state.acceleration.z;
    }
    else if (group_name == WiimoteEmu::Wiimote::GYROSCOPE_GROUP)
    {
      if (control_name == ControllerEmu::ReshapableInput::X_INPUT_OVERRIDE)
        return cached_state.angular_velocity.x;
      if (control_name == ControllerEmu::ReshapableInput::Y_INPUT_OVERRIDE)
        return cached_state.angular_velocity.y;
      if (control_name == ControllerEmu::ReshapableInput::Z_INPUT_OVERRIDE)
        return cached_state.angular_velocity.z;
    }
    else if (group_name == WiimoteEmu::Wiimote::IR_GROUP)
    {
      // Handle recenter signal from EmulatePoint — reset the reference orientation
      // so the current aim direction becomes the new center.
      if (control_name == "Recenter")
      {
        if (wiimote_index < MAX_BBMOTES)
          s_openxr_ir_centers[wiimote_index].initialized = false;
        return std::nullopt;
      }
      if (control_name == ControllerEmu::ReshapableInput::X_INPUT_OVERRIDE)
        return std::isnan(cached_state.ir_x) ? 0.0 : cached_state.ir_x;
      if (control_name == ControllerEmu::ReshapableInput::Y_INPUT_OVERRIDE)
        return static_cast<double>(cached_state.ir_y);
      if (control_name == ControllerEmu::ReshapableInput::Z_INPUT_OVERRIDE)
        return static_cast<double>(cached_state.ir_z);
    }

    return std::nullopt;
  };
}

std::array<bool, MAX_BBMOTES> s_openxr_overrides_enabled{};

void UpdateOpenXRInputOverride(unsigned int index, WiimoteSource source)
{
  auto* wiimote =
      static_cast<WiimoteEmu::Wiimote*>(::Wiimote::GetConfig()->GetController(index));
  if (!wiimote)
    return;

  auto apply_to_attachments = [wiimote](const ControllerEmu::InputOverrideFunction& override_func) {
    auto* attachments_group = static_cast<ControllerEmu::Attachments*>(
        wiimote->GetWiimoteGroup(WiimoteEmu::WiimoteGroup::Attachments));
    if (!attachments_group)
      return;

    for (const auto& attachment : attachments_group->GetAttachmentList())
    {
      if (!attachment)
        continue;

      if (override_func)
        attachment->SetInputOverrideFunction(override_func);
      else
        attachment->ClearInputOverrideFunction();
    }
  };

  if (source == WiimoteSource::OpenXR)
  {
    s_openxr_ir_centers[index] = {};
    wiimote->SetInputOverrideFunction(CreateOpenXRInputOverrideFunction(index, false));
    apply_to_attachments(CreateOpenXRInputOverrideFunction(index, true));
    s_openxr_overrides_enabled[index] = true;
  }
  else if (s_openxr_overrides_enabled[index])
  {
    s_openxr_ir_centers[index] = {};
    wiimote->ClearInputOverrideFunction();
    apply_to_attachments({});
    s_openxr_overrides_enabled[index] = false;
  }
}
#else
void UpdateOpenXRInputOverride(unsigned int, WiimoteSource)
{
}
#endif

WiimoteSource GetSource(unsigned int index)
{
  return s_wiimote_sources[index];
}

void OnSourceChanged(unsigned int index, WiimoteSource source)
{
  const WiimoteSource previous_source = s_wiimote_sources[index].exchange(source);

  if (previous_source == source)
  {
    // No change. Do nothing.
    UpdateOpenXRInputOverride(index, source);
    return;
  }

  UpdateOpenXRInputOverride(index, source);

  WiimoteReal::HandleWiimoteSourceChange(index);

  const Core::CPUThreadGuard guard(Core::System::GetInstance());
  WiimoteCommon::UpdateSource(index);
}

void RefreshConfig()
{
  for (int i = 0; i < MAX_BBMOTES; ++i)
    OnSourceChanged(i, Config::Get(Config::GetInfoForWiimoteSource(i)));
}

}  // namespace

namespace WiimoteCommon
{
void UpdateSource(unsigned int index)
{
  const auto bluetooth = WiiUtils::GetBluetoothEmuDevice();
  if (bluetooth == nullptr)
    return;

  bluetooth->AccessWiimoteByIndex(index)->SetSource(GetHIDWiimoteSource(index));
}

HIDWiimote* GetHIDWiimoteSource(unsigned int index)
{
  HIDWiimote* hid_source = nullptr;

  switch (GetSource(index))
  {
  case WiimoteSource::Emulated:
  case WiimoteSource::OpenXR:
    hid_source = static_cast<WiimoteEmu::Wiimote*>(::Wiimote::GetConfig()->GetController(index));
    break;

  case WiimoteSource::Real:
    hid_source = WiimoteReal::g_wiimotes[index].get();
    break;

  default:
    break;
  }

  return hid_source;
}

}  // namespace WiimoteCommon

namespace Wiimote
{
static InputConfig s_config(WIIMOTE_INI_NAME, _trans("Wii Remote"), "Wiimote", "Wiimote");

InputConfig* GetConfig()
{
  return &s_config;
}

std::optional<OpenXRWiiRemoteState> GetOpenXRWiiRemoteState(unsigned int index)
{
#ifdef ENABLE_VR
  if (index >= MAX_WIIMOTES || s_wiimote_sources[index].load() != WiimoteSource::OpenXR)
    return std::nullopt;

  if (const auto right = GetOpenXRHandState(false))
    return right;
  return GetOpenXRHandState(true);
#else
  return std::nullopt;
#endif
}

std::optional<OpenXRWiiRemoteState> GetOpenXRHandState(bool left_hand)
{
#ifdef ENABLE_VR
  const Common::VR::OpenXRInputSnapshot snapshot = Common::VR::OpenXRInputState::GetSnapshot();
  if (!snapshot.runtime_active)
    return std::nullopt;

  const auto& controller = snapshot.controllers[left_hand ? 0 : 1];
  if (!controller.grip_pose.valid && !controller.aim_pose.valid)
    return std::nullopt;

  OpenXRVelocityHistory throwaway_velocity_history;
  OpenXRWiimoteState state =
      BuildOpenXRState(controller, &throwaway_velocity_history);
  // This path doesn't have a wiimote index, use 0 for reference-based IR
  ApplyOpenXRIRCenter(0, &state, snapshot.head_pose);

  OpenXRWiiRemoteState out;
  out.acceleration = {state.acceleration.x, state.acceleration.y, state.acceleration.z};
  out.angular_velocity = {state.angular_velocity.x, state.angular_velocity.y, state.angular_velocity.z};
  out.ir_x = state.ir_x;
  out.ir_y = state.ir_y;
  out.ir_visible = !std::isnan(state.ir_x);
  return out;
#else
  return std::nullopt;
#endif
}

ControllerEmu::ControlGroup* GetWiimoteGroup(int number, WiimoteEmu::WiimoteGroup group)
{
  return static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(number))->GetWiimoteGroup(group);
}

ControllerEmu::ControlGroup* GetNunchukGroup(int number, WiimoteEmu::NunchukGroup group)
{
  return static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(number))->GetNunchukGroup(group);
}

ControllerEmu::ControlGroup* GetClassicGroup(int number, WiimoteEmu::ClassicGroup group)
{
  return static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(number))->GetClassicGroup(group);
}

ControllerEmu::ControlGroup* GetGuitarGroup(int number, WiimoteEmu::GuitarGroup group)
{
  return static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(number))->GetGuitarGroup(group);
}

ControllerEmu::ControlGroup* GetDrumsGroup(int number, WiimoteEmu::DrumsGroup group)
{
  return static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(number))->GetDrumsGroup(group);
}

ControllerEmu::ControlGroup* GetTurntableGroup(int number, WiimoteEmu::TurntableGroup group)
{
  return static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(number))
      ->GetTurntableGroup(group);
}

ControllerEmu::ControlGroup* GetUDrawTabletGroup(int number, WiimoteEmu::UDrawTabletGroup group)
{
  return static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(number))
      ->GetUDrawTabletGroup(group);
}

ControllerEmu::ControlGroup* GetDrawsomeTabletGroup(int number,
                                                    WiimoteEmu::DrawsomeTabletGroup group)
{
  return static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(number))
      ->GetDrawsomeTabletGroup(group);
}

ControllerEmu::ControlGroup* GetTaTaConGroup(int number, WiimoteEmu::TaTaConGroup group)
{
  return static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(number))->GetTaTaConGroup(group);
}

ControllerEmu::ControlGroup* GetShinkansenGroup(int number, WiimoteEmu::ShinkansenGroup group)
{
  return static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(number))
      ->GetShinkansenGroup(group);
}

void Shutdown()
{
  s_config.UnregisterHotplugCallback();

  s_config.ClearControllers();

  WiimoteReal::Stop();

  if (s_config_callback_id)
  {
    Config::RemoveConfigChangedCallback(*s_config_callback_id);
    s_config_callback_id = std::nullopt;
  }
}

void Initialize(InitializeMode init_mode)
{
  if (s_config.ControllersNeedToBeCreated())
  {
    for (unsigned int i = WIIMOTE_CHAN_0; i < MAX_BBMOTES; ++i)
      s_config.CreateController<WiimoteEmu::Wiimote>(i);
  }

  s_config.RegisterHotplugCallback();

  LoadConfig();

  if (!s_config_callback_id)
    s_config_callback_id = Config::AddConfigChangedCallback(RefreshConfig);
  RefreshConfig();

  WiimoteReal::Initialize(init_mode);

  // Reload Wiimotes with our settings
  auto& movie = Core::System::GetInstance().GetMovie();
  if (movie.IsMovieActive())
    movie.ChangeWiiPads();
}

void ResetAllWiimotes()
{
  for (int i = WIIMOTE_CHAN_0; i < MAX_BBMOTES; ++i)
    static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(i))->Reset();
}

void LoadConfig()
{
  s_config.LoadConfig();
  s_last_connect_request_counter.fill(0);
}

void GenerateDynamicInputTextures()
{
  s_config.GenerateControllerTextures();
}

void Resume()
{
  WiimoteReal::Resume();
}

void Pause()
{
  WiimoteReal::Pause();
}

void DoState(PointerWrap& p)
{
  for (int i = 0; i < MAX_BBMOTES; ++i)
  {
    const WiimoteSource source = GetSource(i);
    auto state_wiimote_source = u8(source);
    p.Do(state_wiimote_source);

    if (IsEmulatedSource(WiimoteSource(state_wiimote_source)))
    {
      // Sync complete state of emulated wiimotes.
      static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(i))->DoState(p);
    }

    if (p.IsReadMode())
    {
      // If using a real wiimote or the save-state source does not match the current source,
      // then force a reconnection on load.
      if (source == WiimoteSource::Real || source != WiimoteSource(state_wiimote_source))
        WiimoteCommon::UpdateSource(i);
    }
  }
}

void RecenterOpenXRPointer(unsigned int wiimote_index)
{
#ifdef ENABLE_VR
  if (wiimote_index < MAX_BBMOTES)
    s_openxr_ir_centers[wiimote_index].initialized = false;
#endif
}

}  // namespace Wiimote
