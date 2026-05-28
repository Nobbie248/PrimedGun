// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef ENABLE_VR

#define XR_USE_GRAPHICS_API_D3D11

#include "VideoCommon/VR/OpenXRD3D11Common.h"

#include <array>
#include <vector>

#include <dxgi.h>
#include <openxr/openxr_platform.h>

#include "Common/Logging/Log.h"

namespace VR::D3D11OpenXR
{
bool QueryGraphicsRequirements(XrInstance instance, XrSystemId system_id,
                               XrGraphicsRequirementsD3D11KHR* requirements)
{
  PFN_xrGetD3D11GraphicsRequirementsKHR get_requirements = nullptr;
  const XrResult proc_result = xrGetInstanceProcAddr(
      instance, "xrGetD3D11GraphicsRequirementsKHR",
      reinterpret_cast<PFN_xrVoidFunction*>(&get_requirements));
  if (XR_FAILED(proc_result) || !get_requirements)
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR: xrGetD3D11GraphicsRequirementsKHR unavailable ({}).",
                  static_cast<int>(proc_result));
    return false;
  }

  const XrResult result = get_requirements(instance, system_id, requirements);
  if (XR_FAILED(result))
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR: xrGetD3D11GraphicsRequirementsKHR failed ({}).",
                  static_cast<int>(result));
    return false;
  }

  return true;
}

bool CreateSessionFromRequirements(XrInstance instance, XrSystemId system_id,
                                   const XrGraphicsRequirementsD3D11KHR& requirements,
                                   ID3D11Device* device, XrSession* session)
{
  XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
  binding.device = device;

  XrSessionCreateInfo create_info{XR_TYPE_SESSION_CREATE_INFO};
  create_info.next = &binding;
  create_info.systemId = system_id;

  const XrResult result = xrCreateSession(instance, &create_info, session);
  if (XR_FAILED(result))
  {
    ERROR_LOG_FMT(VIDEO,
                  "OpenXR: xrCreateSession(D3D11) failed ({}) with min feature level {:#x}.",
                  static_cast<int>(result), static_cast<int>(requirements.minFeatureLevel));
    return false;
  }

  return true;
}

bool SelectSwapchainFormat(XrSession session, int64_t* format)
{
  uint32_t count = 0;
  XrResult result = xrEnumerateSwapchainFormats(session, 0, &count, nullptr);
  if (XR_FAILED(result) || count == 0)
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR: xrEnumerateSwapchainFormats failed ({}).",
                  static_cast<int>(result));
    return false;
  }

  std::vector<int64_t> formats(count);
  result = xrEnumerateSwapchainFormats(session, count, &count, formats.data());
  if (XR_FAILED(result))
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR: xrEnumerateSwapchainFormats failed ({}).",
                  static_cast<int>(result));
    return false;
  }

  constexpr std::array<int64_t, 4> preferred_formats = {
      DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
      DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM};

  for (const int64_t preferred : preferred_formats)
  {
    for (const int64_t available : formats)
    {
      if (available == preferred)
      {
        *format = preferred;
        return true;
      }
    }
  }

  *format = formats.front();
  return true;
}
}  // namespace VR::D3D11OpenXR

#endif
