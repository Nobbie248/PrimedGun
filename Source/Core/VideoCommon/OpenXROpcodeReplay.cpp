// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/OpenXROpcodeReplay.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <vector>

#include "Common/Logging/Log.h"
#include "Core/Config/GraphicsSettings.h"
#include "VideoCommon/VideoConfig.h"

namespace VideoCommon::OpenXROpcodeReplay
{
namespace
{
constexpr u64 INVALID_FRAME_INDEX = std::numeric_limits<u64>::max();

struct ReplayClearState
{
  bool color_enable = false;
  bool alpha_enable = false;
  bool z_enable = false;
  PixelFormat pixel_format = PixelFormat::RGBA6_Z24;
  u32 color = 0;
  u32 clear_z_value = 0;
  bool valid = false;
};

struct ReplayState
{
  std::mutex mutex;
  std::vector<u8> capture_preprocess;
  std::vector<u8> capture_main;
  std::vector<u8> replay_preprocess;
  std::vector<u8> replay_main;
  u64 capture_frame_index = INVALID_FRAME_INDEX;
  u64 scheduled_replay_frame_index = INVALID_FRAME_INDEX;
  u64 replay_frame_index = INVALID_FRAME_INDEX;
  u64 last_decided_frame_index = INVALID_FRAME_INDEX;
  u64 next_frame_index = 0;
  int scheduled_replay_count = 0;
  int replay_count = 0;
  bool capture_enabled = false;
  bool replaying = false;
  bool replay_frame_active = false;
  bool replay_log_frame_active = false;
  bool capture_window_armed = false;
  bool pending_immediate_swap = false;
  bool pending_immediate_swap_is_frame_boundary = false;
  u32 pending_xfb_addr = 0;
  u32 pending_fb_width = 0;
  u32 pending_fb_stride = 0;
  u32 pending_fb_height = 0;
  u32 replay_xfb_count = 0;
  ReplayClearState clear_states[2];
};

ReplayState s_state;

int GetConfiguredInputRefreshRateUnlocked()
{
  switch (g_ActiveConfig.vr_opcode_replay_mode)
  {
  case OpenXROpcodeReplayMode::Input25Hz:
    return 25;
  case OpenXROpcodeReplayMode::Input30Hz:
    return 30;
  case OpenXROpcodeReplayMode::Input50Hz:
    return 50;
  case OpenXROpcodeReplayMode::Input60Hz:
    return 60;
  case OpenXROpcodeReplayMode::Off:
  default:
    return 0;
  }
}

int GetConfiguredTargetRefreshRateUnlocked(double display_period_ms)
{
  const int configured_refresh_rate = Config::NormalizeVROpcodeReplayTargetRefreshRate(
      g_ActiveConfig.vr_opcode_replay_target_refresh_rate);
  if (configured_refresh_rate == Config::GFX_VR_OPCODE_REPLAY_TARGET_REFRESH_RATE_AUTO)
  {
    if (display_period_ms <= 0.0)
      return 0;

    const float refresh_rate_hz = static_cast<float>(1000.0 / display_period_ms);
    return Config::ChooseClosestVRForcedVBIFrequency(refresh_rate_hz);
  }

  switch (configured_refresh_rate)
  {
  case Config::GFX_VR_OPCODE_REPLAY_TARGET_REFRESH_RATE_72:
  case Config::GFX_VR_OPCODE_REPLAY_TARGET_REFRESH_RATE_90:
  case Config::GFX_VR_OPCODE_REPLAY_TARGET_REFRESH_RATE_120:
    return configured_refresh_rate;
  default:
    return 90;
  }
}

bool IsConfiguredUnlocked()
{
  return g_ActiveConfig.stereo_mode == StereoMode::OpenXR &&
         GetConfiguredInputRefreshRateUnlocked() > 0;
}

void ClearUnlocked(ReplayState& state)
{
  state.capture_preprocess.clear();
  state.capture_main.clear();
  state.replay_preprocess.clear();
  state.replay_main.clear();
  state.capture_frame_index = INVALID_FRAME_INDEX;
  state.scheduled_replay_frame_index = INVALID_FRAME_INDEX;
  state.replay_frame_index = INVALID_FRAME_INDEX;
  state.last_decided_frame_index = INVALID_FRAME_INDEX;
  state.next_frame_index = 0;
  state.scheduled_replay_count = 0;
  state.replay_count = 0;
  state.capture_enabled = false;
  state.replaying = false;
  state.replay_frame_active = false;
  state.replay_log_frame_active = false;
  state.capture_window_armed = false;
  state.pending_immediate_swap = false;
  state.pending_immediate_swap_is_frame_boundary = false;
  state.pending_xfb_addr = 0;
  state.pending_fb_width = 0;
  state.pending_fb_stride = 0;
  state.pending_fb_height = 0;
  state.replay_xfb_count = 0;
  state.clear_states[0] = {};
  state.clear_states[1] = {};
}

int CalculateReplayCountForFrameUnlocked(double display_period_ms, u64 frame_index)
{
  const int input_refresh_rate = GetConfiguredInputRefreshRateUnlocked();
  const int target_refresh_rate = GetConfiguredTargetRefreshRateUnlocked(display_period_ms);
  if (input_refresh_rate <= 0 || target_refresh_rate <= input_refresh_rate)
    return 0;

  const double target_period_ms = 1000.0 / static_cast<double>(target_refresh_rate);
  constexpr double tolerance_ms = 0.8;
  if (!(display_period_ms > 0.0 &&
        std::abs(display_period_ms - target_period_ms) <= tolerance_ms))
  {
    return 0;
  }

  const u64 start_slot =
      (frame_index * static_cast<u64>(target_refresh_rate)) / static_cast<u64>(input_refresh_rate);
  const u64 end_slot = ((frame_index + 1) * static_cast<u64>(target_refresh_rate)) /
                       static_cast<u64>(input_refresh_rate);
  if (end_slot <= start_slot)
    return 0;

  return std::max(0, static_cast<int>(end_slot - start_slot) - 1);
}
}  // namespace

bool IsCaptureEnabled()
{
  std::scoped_lock lock(s_state.mutex);
  s_state.capture_enabled = IsConfiguredUnlocked();
  return s_state.capture_enabled;
}

bool IsReplaying()
{
  std::scoped_lock lock(s_state.mutex);
  return s_state.replaying;
}

bool IsReplayFrameActive()
{
  std::scoped_lock lock(s_state.mutex);
  return s_state.replay_frame_active;
}

bool IsReplayLogFrameActive()
{
  std::scoped_lock lock(s_state.mutex);
  return s_state.replay_log_frame_active;
}

bool IsCaptureArmed()
{
  std::scoped_lock lock(s_state.mutex);
  return s_state.capture_window_armed;
}

u64 GetCaptureFrameIndex()
{
  std::scoped_lock lock(s_state.mutex);
  return s_state.capture_frame_index;
}

void EnableCaptureForNextFrame(double display_period_ms)
{
  std::scoped_lock lock(s_state.mutex);
  s_state.capture_enabled = IsConfiguredUnlocked();
  const int scheduled_replay_count =
      s_state.capture_enabled && !s_state.replaying ?
          CalculateReplayCountForFrameUnlocked(display_period_ms, s_state.next_frame_index) :
          0;
  const bool set_active = scheduled_replay_count > 0;
  if (set_active)
  {
    s_state.capture_preprocess.clear();
    s_state.capture_main.clear();
    s_state.capture_frame_index = s_state.next_frame_index;
    s_state.scheduled_replay_frame_index = s_state.next_frame_index;
    s_state.scheduled_replay_count = scheduled_replay_count;
    s_state.replay_log_frame_active = true;
    s_state.capture_window_armed = true;
  }
  else
  {
    s_state.capture_preprocess.clear();
    s_state.capture_main.clear();
    s_state.replay_log_frame_active = false;
    s_state.capture_window_armed = false;
    s_state.capture_frame_index = INVALID_FRAME_INDEX;
    s_state.scheduled_replay_frame_index = INVALID_FRAME_INDEX;
    s_state.scheduled_replay_count = 0;
  }
}

void CaptureCommand(bool is_preprocess, const u8* data, u32 size)
{
  if (data == nullptr || size == 0)
    return;

  std::scoped_lock lock(s_state.mutex);
  s_state.capture_enabled = IsConfiguredUnlocked();
  if (!s_state.capture_enabled || s_state.replaying || !s_state.replay_log_frame_active)
  {
    static int skipped = 0;
    if (++skipped % 10000 == 1)
    {
      WARN_LOG_FMT(VIDEO,
                   "OpcodeReplay CaptureCommand skipped (count={}) capture_enabled={} "
                   "replaying={} log_active={}",
                   skipped, s_state.capture_enabled, s_state.replaying,
                   s_state.replay_log_frame_active);
    }
    return;
  }

  auto& stream = is_preprocess ? s_state.capture_preprocess : s_state.capture_main;
  stream.insert(stream.end(), data, data + size);
}

void NotifyFrameBoundary()
{
  std::scoped_lock lock(s_state.mutex);

  if (s_state.replaying)
  {
    WARN_LOG_FMT(VIDEO, "OpcodeReplay NotifyFrameBoundary: early-out (replaying=true)");
    return;
  }

  s_state.capture_enabled = IsConfiguredUnlocked();
  if (!s_state.capture_enabled)
  {
    ClearUnlocked(s_state);
    return;
  }

  if (!s_state.capture_window_armed)
  {
    s_state.next_frame_index++;
    return;
  }

  if (s_state.capture_frame_index != s_state.scheduled_replay_frame_index ||
      s_state.capture_frame_index != s_state.next_frame_index)
  {
    WARN_LOG_FMT(VIDEO,
                 "OpcodeReplay NotifyFrameBoundary: capture scheduling mismatch capture_idx={} "
                 "scheduled_idx={} expected_idx={}",
                 s_state.capture_frame_index, s_state.scheduled_replay_frame_index,
                 s_state.next_frame_index);
    s_state.capture_preprocess.clear();
    s_state.capture_main.clear();
    s_state.replay_preprocess.clear();
    s_state.replay_main.clear();
    s_state.replay_frame_index = INVALID_FRAME_INDEX;
    s_state.replay_count = 0;
    s_state.replay_log_frame_active = false;
    s_state.capture_window_armed = false;
    s_state.capture_frame_index = INVALID_FRAME_INDEX;
    s_state.scheduled_replay_frame_index = INVALID_FRAME_INDEX;
    s_state.scheduled_replay_count = 0;
    s_state.next_frame_index++;
    return;
  }

  const size_t cap_pre = s_state.capture_preprocess.size();
  const size_t cap_main = s_state.capture_main.size();

  if (cap_pre == 0 && cap_main == 0)
  {
    static int empty_count = 0;
    if (++empty_count % 30 == 1)
    {
      WARN_LOG_FMT(VIDEO,
                   "OpcodeReplay NotifyFrameBoundary: EMPTY capture (count={}) "
                   "replay_log_frame_active={} capture_enabled={} capture_idx={}",
                   empty_count, s_state.replay_log_frame_active, s_state.capture_enabled,
                   s_state.capture_frame_index);
    }
    s_state.replay_preprocess.clear();
    s_state.replay_main.clear();
    s_state.replay_frame_index = INVALID_FRAME_INDEX;
    s_state.replay_count = 0;
    s_state.replay_log_frame_active = false;
    s_state.capture_window_armed = false;
    s_state.capture_frame_index = INVALID_FRAME_INDEX;
    s_state.scheduled_replay_frame_index = INVALID_FRAME_INDEX;
    s_state.scheduled_replay_count = 0;
    s_state.next_frame_index++;
    return;
  }

  s_state.replay_preprocess = std::move(s_state.capture_preprocess);
  s_state.replay_main = std::move(s_state.capture_main);
  s_state.capture_preprocess.clear();
  s_state.capture_main.clear();
  s_state.replay_frame_index = s_state.capture_frame_index;
  s_state.replay_count = s_state.scheduled_replay_count;
  s_state.next_frame_index = s_state.replay_frame_index + 1;
  s_state.replay_log_frame_active = false;
  s_state.capture_window_armed = false;
  s_state.capture_frame_index = INVALID_FRAME_INDEX;
  s_state.scheduled_replay_frame_index = INVALID_FRAME_INDEX;
  s_state.scheduled_replay_count = 0;
  static_cast<void>(cap_pre);
  static_cast<void>(cap_main);
}

void Clear()
{
  std::scoped_lock lock(s_state.mutex);
  ClearUnlocked(s_state);
}

bool HasReplayData()
{
  std::scoped_lock lock(s_state.mutex);
  return s_state.replay_frame_index != INVALID_FRAME_INDEX &&
         (!s_state.replay_preprocess.empty() || !s_state.replay_main.empty());
}

u64 GetReplayFrameIndex()
{
  std::scoped_lock lock(s_state.mutex);
  return s_state.replay_frame_index;
}

int GetReplayCount(double display_period_ms)
{
  std::scoped_lock lock(s_state.mutex);
  static_cast<void>(display_period_ms);

  s_state.capture_enabled = IsConfiguredUnlocked();
  if (!s_state.capture_enabled || s_state.replaying ||
      s_state.replay_frame_index == INVALID_FRAME_INDEX ||
      s_state.replay_count <= 0 ||
      (s_state.replay_preprocess.empty() && s_state.replay_main.empty()))
  {
    return 0;
  }

  if (s_state.last_decided_frame_index == s_state.replay_frame_index)
    return 0;

  s_state.last_decided_frame_index = s_state.replay_frame_index;
  return s_state.replay_count;
}

bool BeginReplayIteration()
{
  std::scoped_lock lock(s_state.mutex);
  if (s_state.replaying || s_state.replay_frame_index == INVALID_FRAME_INDEX ||
      (s_state.replay_preprocess.empty() && s_state.replay_main.empty()))
    return false;

  s_state.replaying = true;
  s_state.capture_enabled = IsConfiguredUnlocked();
  s_state.replay_frame_active = true;
  s_state.replay_log_frame_active = false;
  s_state.pending_immediate_swap = false;
  s_state.pending_immediate_swap_is_frame_boundary = false;
  s_state.pending_xfb_addr = 0;
  s_state.pending_fb_width = 0;
  s_state.pending_fb_stride = 0;
  s_state.pending_fb_height = 0;
  s_state.replay_xfb_count = 0;
  return true;
}

void EndReplayIteration()
{
  std::scoped_lock lock(s_state.mutex);
  s_state.replaying = false;
  s_state.replay_frame_active = false;
  s_state.replay_log_frame_active = false;
  s_state.pending_immediate_swap = false;
  s_state.pending_immediate_swap_is_frame_boundary = false;
  s_state.pending_xfb_addr = 0;
  s_state.pending_fb_width = 0;
  s_state.pending_fb_stride = 0;
  s_state.pending_fb_height = 0;
  s_state.replay_xfb_count = 0;
}

void DiscardReplayFrame()
{
  std::scoped_lock lock(s_state.mutex);
  const u64 discarded_frame_index = s_state.replay_frame_index;
  s_state.replay_preprocess.clear();
  s_state.replay_main.clear();
  s_state.replay_frame_index = INVALID_FRAME_INDEX;
  s_state.replay_count = 0;
  s_state.pending_immediate_swap = false;
  s_state.pending_immediate_swap_is_frame_boundary = false;
  s_state.pending_xfb_addr = 0;
  s_state.pending_fb_width = 0;
  s_state.pending_fb_stride = 0;
  s_state.pending_fb_height = 0;
  s_state.replay_xfb_count = 0;
  static_cast<void>(discarded_frame_index);
}

void ApplyReplayClearState(bool frame_just_rendered, bool* color_enable, bool* alpha_enable,
                           bool* z_enable, PixelFormat* pixel_format, u32* color,
                           u32* clear_z_value)
{
  std::scoped_lock lock(s_state.mutex);
  s_state.capture_enabled = IsConfiguredUnlocked();
  if (!s_state.capture_enabled || color_enable == nullptr || alpha_enable == nullptr ||
      z_enable == nullptr || pixel_format == nullptr || color == nullptr ||
      clear_z_value == nullptr)
    return;

  ReplayClearState current = {
      .color_enable = *color_enable,
      .alpha_enable = *alpha_enable,
      .z_enable = *z_enable,
      .pixel_format = *pixel_format,
      .color = *color,
      .clear_z_value = *clear_z_value,
      .valid = true,
  };

  if (s_state.replay_frame_active && frame_just_rendered)
  {
    s_state.clear_states[0] = current;
    return;
  }

  if (!s_state.replay_frame_active && !frame_just_rendered)
  {
    s_state.clear_states[1] = current;
    return;
  }

  const ReplayClearState& selected =
      !s_state.replay_frame_active && frame_just_rendered ? s_state.clear_states[0] :
                                                            s_state.clear_states[1];
  if (!selected.valid)
    return;

  *color_enable = selected.color_enable;
  *alpha_enable = selected.alpha_enable;
  *z_enable = selected.z_enable;
  *pixel_format = selected.pixel_format;
  *color = selected.color;
  *clear_z_value = selected.clear_z_value;
}

void RecordReplayImmediateSwap(u32 xfb_addr, u32 fb_width, u32 fb_stride, u32 fb_height,
                               bool frame_just_rendered)
{
  std::scoped_lock lock(s_state.mutex);
  if (!s_state.replaying)
    return;

  s_state.replay_xfb_count++;

  if (s_state.pending_immediate_swap && (s_state.pending_immediate_swap_is_frame_boundary ||
                                         !frame_just_rendered))
  {
    return;
  }

  s_state.pending_immediate_swap = true;
  s_state.pending_immediate_swap_is_frame_boundary = frame_just_rendered;
  s_state.pending_xfb_addr = xfb_addr;
  s_state.pending_fb_width = fb_width;
  s_state.pending_fb_stride = fb_stride;
  s_state.pending_fb_height = fb_height;
}

bool ConsumeReplayImmediateSwap(u32* xfb_addr, u32* fb_width, u32* fb_stride, u32* fb_height)
{
  std::scoped_lock lock(s_state.mutex);
  if (!s_state.replaying || !s_state.pending_immediate_swap)
    return false;

  if (xfb_addr)
    *xfb_addr = s_state.pending_xfb_addr;
  if (fb_width)
    *fb_width = s_state.pending_fb_width;
  if (fb_stride)
    *fb_stride = s_state.pending_fb_stride;
  if (fb_height)
    *fb_height = s_state.pending_fb_height;

  s_state.pending_immediate_swap = false;
  s_state.pending_immediate_swap_is_frame_boundary = false;
  s_state.pending_xfb_addr = 0;
  s_state.pending_fb_width = 0;
  s_state.pending_fb_stride = 0;
  s_state.pending_fb_height = 0;
  return true;
}

u32 GetReplayXFBCount()
{
  std::scoped_lock lock(s_state.mutex);
  return s_state.replay_xfb_count;
}

std::span<const u8> GetReplayCommands(bool is_preprocess)
{
  std::scoped_lock lock(s_state.mutex);
  const auto& stream = is_preprocess ? s_state.replay_preprocess : s_state.replay_main;
  return std::span<const u8>(stream.data(), stream.size());
}
}  // namespace VideoCommon::OpenXROpcodeReplay
