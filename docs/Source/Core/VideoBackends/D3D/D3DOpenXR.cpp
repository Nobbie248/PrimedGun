// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef ENABLE_VR

// Must define the D3D11 platform before any OpenXR includes.
#define XR_USE_GRAPHICS_API_D3D11

#include "VideoBackends/D3D/D3DOpenXR.h"

#include <algorithm>
#include <array>
#include <vector>

#include "Common/Assert.h"
#include "Common/Logging/Log.h"

#include "VideoBackends/D3D/D3DBase.h"
#include "VideoBackends/D3D/DXTexture.h"
#include "VideoCommon/TextureConfig.h"
#include "VideoCommon/VR/OpenXRManager.h"

namespace DX11
{
std::unique_ptr<D3DOpenXR> g_openxr_d3d;

static const char* DXGIFormatToString(int64_t format)
{
  switch (static_cast<DXGI_FORMAT>(format))
  {
  case DXGI_FORMAT_R8G8B8A8_UNORM:
    return "DXGI_FORMAT_R8G8B8A8_UNORM";
  case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    return "DXGI_FORMAT_R8G8B8A8_UNORM_SRGB";
  case DXGI_FORMAT_B8G8R8A8_UNORM:
    return "DXGI_FORMAT_B8G8R8A8_UNORM";
  case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    return "DXGI_FORMAT_B8G8R8A8_UNORM_SRGB";
  case DXGI_FORMAT_R16G16B16A16_FLOAT:
    return "DXGI_FORMAT_R16G16B16A16_FLOAT";
  default:
    return "UNKNOWN_DXGI_FORMAT";
  }
}

static bool SelectSwapchainFormat(XrSession session, int64_t* out_format)
{
  uint32_t format_count = 0;
  XrResult result = xrEnumerateSwapchainFormats(session, 0, &format_count, nullptr);
  if (XR_FAILED(result) || format_count == 0)
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR: xrEnumerateSwapchainFormats (count) failed ({}).",
                  static_cast<int>(result));
    return false;
  }

  std::vector<int64_t> runtime_formats(format_count);
  result = xrEnumerateSwapchainFormats(session, format_count, &format_count, runtime_formats.data());
  if (XR_FAILED(result))
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR: xrEnumerateSwapchainFormats failed ({}).",
                  static_cast<int>(result));
    return false;
  }

  for (const int64_t format : runtime_formats)
  {
    INFO_LOG_FMT(VIDEO, "OpenXR: Runtime swapchain format {} ({})", static_cast<long long>(format),
                 DXGIFormatToString(format));
  }

  static constexpr std::array<DXGI_FORMAT, 5> preferred_formats = {
      DXGI_FORMAT_R8G8B8A8_UNORM,      DXGI_FORMAT_B8G8R8A8_UNORM,
      DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
      DXGI_FORMAT_R16G16B16A16_FLOAT};

  for (const DXGI_FORMAT preferred : preferred_formats)
  {
    const int64_t wanted = static_cast<int64_t>(preferred);
    if (std::find(runtime_formats.begin(), runtime_formats.end(), wanted) != runtime_formats.end())
    {
      *out_format = wanted;
      INFO_LOG_FMT(VIDEO, "OpenXR: Selected swapchain format {} ({}).",
                   static_cast<long long>(*out_format), DXGIFormatToString(*out_format));
      return true;
    }
  }

  *out_format = runtime_formats.front();
  WARN_LOG_FMT(VIDEO,
               "OpenXR: No preferred D3D11 swapchain format found; falling back to runtime format "
               "{} ({}).",
               static_cast<long long>(*out_format), DXGIFormatToString(*out_format));
  return true;
}

D3DOpenXR::D3DOpenXR() = default;

D3DOpenXR::~D3DOpenXR()
{
  Shutdown();
}

bool D3DOpenXR::Initialize()
{
  INFO_LOG_FMT(VIDEO, "OpenXR D3D11: Starting initialization...");
  ASSERT_MSG(VIDEO, !VR::g_openxr, "OpenXRManager already initialized.");

  auto mgr = std::make_unique<VR::OpenXRManager>();

  // The D3D11 graphics binding extension is mandatory.
  const std::vector<const char*> extensions = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
  if (!mgr->CreateInstance(extensions))
    return false;

  if (!mgr->InitializeSystem())
    return false;

  if (!mgr->EnumerateViewConfigurations())
    return false;

  // Publish the manager globally so CreateSessionD3D11 can access instance/system IDs.
  VR::g_openxr = std::move(mgr);

  if (!CreateSessionD3D11())
  {
    VR::g_openxr.reset();
    return false;
  }

  if (!VR::g_openxr->CreateReferenceSpace())
  {
    VR::g_openxr.reset();
    return false;
  }

  if (!CreateSwapchains())
  {
    VR::g_openxr.reset();
    return false;
  }

  // Register this object as the swapchain provider so Presenter can acquire eye images.
  VR::g_openxr->SetSwapchain(this);

  INFO_LOG_FMT(VIDEO, "OpenXR D3D11: Initialization complete.");
  return true;
}

void D3DOpenXR::Shutdown()
{
  // Clear swapchain pointer before destroying swapchains so no dangling use occurs.
  if (VR::g_openxr)
    VR::g_openxr->SetSwapchain(nullptr);

  DestroySwapchains();
  VR::g_openxr.reset();
  if (D3D::context)
    D3D::context->Flush();
  INFO_LOG_FMT(VIDEO, "OpenXR D3D11: Shut down.");
}

bool D3DOpenXR::CreateSessionD3D11()
{
  ASSERT(D3D::device != nullptr);
  ASSERT(VR::g_openxr != nullptr);

  // --- Query D3D11 graphics requirements (mandatory before session creation) ---
  PFN_xrGetD3D11GraphicsRequirementsKHR pfnGetD3D11Requirements = nullptr;
  XrResult result = xrGetInstanceProcAddr(
      VR::g_openxr->GetInstance(), "xrGetD3D11GraphicsRequirementsKHR",
      reinterpret_cast<PFN_xrVoidFunction*>(&pfnGetD3D11Requirements));

  if (XR_FAILED(result) || pfnGetD3D11Requirements == nullptr)
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR: Could not load xrGetD3D11GraphicsRequirementsKHR.");
    return false;
  }

  XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
  result = pfnGetD3D11Requirements(VR::g_openxr->GetInstance(), VR::g_openxr->GetSystemId(),
                                    &requirements);
  if (XR_FAILED(result))
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR: xrGetD3D11GraphicsRequirementsKHR failed ({}).",
                  static_cast<int>(result));
    return false;
  }

  INFO_LOG_FMT(VIDEO,
               "OpenXR: D3D11 requirements — adapter LUID {:#010x}{:08x}, "
               "min feature level {:#x}",
               requirements.adapterLuid.HighPart, requirements.adapterLuid.LowPart,
               static_cast<int>(requirements.minFeatureLevel));

  // --- Create XrSession bound to the active D3D11 device ---
  XrGraphicsBindingD3D11KHR d3d11_binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
  d3d11_binding.device = D3D::device.Get();

  XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
  session_info.next = &d3d11_binding;
  session_info.systemId = VR::g_openxr->GetSystemId();

  XrSession session = XR_NULL_HANDLE;
  result = xrCreateSession(VR::g_openxr->GetInstance(), &session_info, &session);
  if (XR_FAILED(result))
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR: xrCreateSession failed ({}).", static_cast<int>(result));
    return false;
  }

  VR::g_openxr->SetSession(session);
  INFO_LOG_FMT(VIDEO, "OpenXR D3D11: Session created successfully.");
  return true;
}

bool D3DOpenXR::CreateSwapchains()
{
  ASSERT(VR::g_openxr != nullptr);

  const auto& view_cfgs = VR::g_openxr->GetViewConfigViews();
  int64_t swapchain_format = 0;
  if (!SelectSwapchainFormat(VR::g_openxr->GetSession(), &swapchain_format))
    return false;

  for (uint32_t eye = 0; eye < 2; ++eye)
  {
    auto& sc = m_eye_swapchains[eye];
    sc.width = view_cfgs[eye].recommendedImageRectWidth;
    sc.height = view_cfgs[eye].recommendedImageRectHeight;

    // Format must come from xrEnumerateSwapchainFormats.
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.arraySize = 1;
    info.format = swapchain_format;
    info.width = sc.width;
    info.height = sc.height;
    info.mipCount = 1;
    info.faceCount = 1;
    info.sampleCount = 1;
    // We render into this swapchain image as a color target, but never sample from it.
    // Some runtimes reject the extra SAMPLED usage for certain formats.
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;

    XrResult result = xrCreateSwapchain(VR::g_openxr->GetSession(), &info, &sc.swapchain);
    if (XR_FAILED(result))
    {
      ERROR_LOG_FMT(VIDEO, "OpenXR: xrCreateSwapchain failed for eye {} ({}).", eye,
                    static_cast<int>(result));
      return false;
    }

    // Enumerate the D3D11 textures backing this swapchain.
    uint32_t image_count = 0;
    xrEnumerateSwapchainImages(sc.swapchain, 0, &image_count, nullptr);

    std::vector<XrSwapchainImageD3D11KHR> images(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    xrEnumerateSwapchainImages(sc.swapchain, image_count, &image_count,
                               reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));

    sc.textures.resize(image_count);
    sc.framebuffers.resize(image_count);

    for (uint32_t i = 0; i < image_count; ++i)
    {
      // Adopt the runtime-owned texture. DXTexture::CreateAdopted reads the D3D11 texture
      // descriptor to build the TextureConfig and AddRefs the underlying resource.
      sc.textures[i] = DXTexture::CreateAdopted(ComPtr<ID3D11Texture2D>(images[i].texture));
      if (!sc.textures[i])
      {
        ERROR_LOG_FMT(VIDEO, "OpenXR: DXTexture::CreateAdopted failed for eye {}, image {}.",
                      eye, i);
        return false;
      }

      // No depth attachment for now; depth will be added in Phase 3 when the render path
      // is fully integrated with the EFB.
      sc.framebuffers[i] = DXFramebuffer::Create(sc.textures[i].get(), nullptr, {});
      if (!sc.framebuffers[i])
      {
        ERROR_LOG_FMT(VIDEO, "OpenXR: DXFramebuffer::Create failed for eye {}, image {}.",
                      eye, i);
        return false;
      }
    }

    INFO_LOG_FMT(VIDEO, "OpenXR: Eye {} swapchain ready: {}x{}, {} images.", eye, sc.width,
                 sc.height, image_count);
  }

  return true;
}

void D3DOpenXR::DestroySwapchains()
{
  for (uint32_t eye = 0; eye < 2; ++eye)
  {
    auto& sc = m_eye_swapchains[eye];

    if (m_image_acquired[eye] && sc.swapchain != XR_NULL_HANDLE)
    {
      XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
      const XrResult release_result = xrReleaseSwapchainImage(sc.swapchain, &release_info);
      if (XR_FAILED(release_result))
      {
        WARN_LOG_FMT(VIDEO,
                     "OpenXR: xrReleaseSwapchainImage during shutdown failed for eye {} ({}).",
                     eye, static_cast<int>(release_result));
      }
      m_image_acquired[eye] = false;
    }

    // Release Dolphin wrappers before destroying the swapchain so the
    // runtime's textures are only freed after our references are gone.
    sc.framebuffers.clear();
    sc.textures.clear();

    if (sc.swapchain != XR_NULL_HANDLE)
    {
      const XrResult destroy_result = xrDestroySwapchain(sc.swapchain);
      if (XR_FAILED(destroy_result))
      {
        WARN_LOG_FMT(VIDEO, "OpenXR: xrDestroySwapchain failed for eye {} ({}).", eye,
                     static_cast<int>(destroy_result));
      }
      sc.swapchain = XR_NULL_HANDLE;
    }
  }
}

AbstractFramebuffer* D3DOpenXR::AcquireEyeFramebuffer(uint32_t eye_index)
{
  ASSERT(eye_index < 2);
  auto& sc = m_eye_swapchains[eye_index];

  XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  if (XR_FAILED(xrAcquireSwapchainImage(sc.swapchain, &acquire_info,
                                         &m_acquired_image_index[eye_index])))
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR: xrAcquireSwapchainImage failed for eye {}.", eye_index);
    return nullptr;
  }
  m_image_acquired[eye_index] = true;

  // Block until the acquired image is safe to write.
  XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wait_info.timeout = XR_INFINITE_DURATION;
  if (XR_FAILED(xrWaitSwapchainImage(sc.swapchain, &wait_info)))
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR: xrWaitSwapchainImage failed for eye {}.", eye_index);

    // Ensure we don't leak an acquired image if waiting fails.
    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    const XrResult release_result = xrReleaseSwapchainImage(sc.swapchain, &release_info);
    if (XR_FAILED(release_result))
    {
      WARN_LOG_FMT(VIDEO,
                   "OpenXR: xrReleaseSwapchainImage after wait failure failed for eye {} ({}).",
                   eye_index, static_cast<int>(release_result));
    }
    m_image_acquired[eye_index] = false;
    return nullptr;
  }

  return sc.framebuffers[m_acquired_image_index[eye_index]].get();
}

void D3DOpenXR::ReleaseEyeTexture(uint32_t eye_index)
{
  ASSERT(eye_index < 2);
  if (!m_image_acquired[eye_index])
    return;

  XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  const XrResult result = xrReleaseSwapchainImage(m_eye_swapchains[eye_index].swapchain,
                                                   &release_info);
  if (XR_FAILED(result))
  {
    WARN_LOG_FMT(VIDEO, "OpenXR: xrReleaseSwapchainImage failed for eye {} ({}).", eye_index,
                 static_cast<int>(result));
  }
  m_image_acquired[eye_index] = false;
}

bool D3DOpenXR::SubmitFrame()
{
  ASSERT(VR::g_openxr != nullptr);

  const auto& eye_views = VR::g_openxr->GetEyeViews();

  for (uint32_t eye = 0; eye < 2; ++eye)
  {
    auto& pv = m_projection_views[eye];
    pv = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    pv.pose = eye_views[eye].pose;
    pv.fov = eye_views[eye].fov;
    pv.subImage.swapchain = m_eye_swapchains[eye].swapchain;
    pv.subImage.imageArrayIndex = 0;
    pv.subImage.imageRect = {
        {0, 0},
        {static_cast<int32_t>(m_eye_swapchains[eye].width),
         static_cast<int32_t>(m_eye_swapchains[eye].height)}};
  }

  m_projection_layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  m_projection_layer.space = VR::g_openxr->GetReferenceSpace();
  m_projection_layer.viewCount = 2;
  m_projection_layer.views = m_projection_views.data();

  const std::vector<XrCompositionLayerBaseHeader*> layers = {
      reinterpret_cast<XrCompositionLayerBaseHeader*>(&m_projection_layer)};

  return VR::g_openxr->EndFrame(layers);
}

}  // namespace DX11

#endif  // ENABLE_VR
