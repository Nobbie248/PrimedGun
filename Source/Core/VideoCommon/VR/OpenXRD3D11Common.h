// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef ENABLE_VR

#define XR_USE_GRAPHICS_API_D3D11

#include <d3d11.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

namespace VR::D3D11OpenXR
{
bool QueryGraphicsRequirements(XrInstance instance, XrSystemId system_id,
                               XrGraphicsRequirementsD3D11KHR* requirements);
bool CreateSessionFromRequirements(XrInstance instance, XrSystemId system_id,
                                   const XrGraphicsRequirementsD3D11KHR& requirements,
                                   ID3D11Device* device, XrSession* session);
void SetIntelCompatibilityMode(bool enabled);
bool IsIntelCompatibilityModeEnabled();
bool SelectSwapchainFormat(XrSession session, int64_t* format);
bool IsSRGBSwapchainFormat(int64_t format);
}  // namespace VR::D3D11OpenXR

#endif
