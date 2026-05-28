// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>

#include "Common/CommonTypes.h"
#include "VideoCommon/BPMemory.h"

namespace VideoCommon::OpenXROpcodeReplay
{
bool IsCaptureEnabled();
bool IsReplaying();
bool IsReplayFrameActive();
bool IsReplayLogFrameActive();
bool IsCaptureArmed();
u64 GetCaptureFrameIndex();
void EnableCaptureForNextFrame(double display_period_ms);

void CaptureCommand(bool is_preprocess, const u8* data, u32 size);
void NotifyFrameBoundary();
void Clear();

bool HasReplayData();
u64 GetReplayFrameIndex();
int GetReplayCount(double display_period_ms);

bool BeginReplayIteration();
void EndReplayIteration();
void DiscardReplayFrame();

void ApplyReplayClearState(bool frame_just_rendered, bool* color_enable, bool* alpha_enable,
                           bool* z_enable, PixelFormat* pixel_format, u32* color,
                           u32* clear_z_value);

void RecordReplayImmediateSwap(u32 xfb_addr, u32 fb_width, u32 fb_stride, u32 fb_height,
                               bool frame_just_rendered);
bool ConsumeReplayImmediateSwap(u32* xfb_addr, u32* fb_width, u32* fb_stride, u32* fb_height);
u32 GetReplayXFBCount();

std::span<const u8> GetReplayCommands(bool is_preprocess);
}  // namespace VideoCommon::OpenXROpcodeReplay
