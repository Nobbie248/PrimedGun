// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef ENABLE_VR

// Must define the D3D11 platform before any OpenXR includes.
#define XR_USE_GRAPHICS_API_D3D11

#include "VideoBackends/D3D/D3DOpenXR.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Common/Assert.h"
#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/Image.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"

#include "VideoBackends/D3D/D3DBase.h"
#include "VideoBackends/D3D/DXTexture.h"
#include "Common/VR/OpenXRInputState.h"
#include "VideoCommon/TextureConfig.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/VR/OpenXRD3D11Common.h"
#include "VideoCommon/VR/OpenXRManager.h"
#include "VideoCommon/VR/PrimedGunOverlayCommon.h"

namespace DX11
{
std::unique_ptr<D3DOpenXR> g_openxr_d3d;
namespace PGO = PrimedGun::Overlay;

namespace
{
struct CinematicScreenAnchor
{
  bool valid = false;
  uint32_t generation = 0;
  XrQuaternionf orientation{0.0f, 0.0f, 0.0f, 1.0f};
  XrVector3f position{};
};

static CinematicScreenAnchor s_cinematic_screen_anchor;

bool ReadCinematicScreenViewerPose(XrQuaternionf* orientation, XrVector3f* position)
{
  const Common::VR::OpenXRInputSnapshot snapshot = Common::VR::OpenXRInputState::GetSnapshot();

  if (snapshot.head_pose.valid)
  {
    *orientation = {snapshot.head_pose.orientation[0], snapshot.head_pose.orientation[1],
                    snapshot.head_pose.orientation[2], snapshot.head_pose.orientation[3]};
    *position = {snapshot.head_pose.position[0] + snapshot.tracking_origin_position[0],
                 snapshot.head_pose.position[1] + snapshot.tracking_origin_position[1],
                 snapshot.head_pose.position[2] + snapshot.tracking_origin_position[2]};
    return true;
  }

  if (VR::g_openxr->AreSubmittedEyeViewsValid())
  {
    const auto& eyes = VR::g_openxr->GetSubmittedEyeViews();
    *orientation = eyes[0].pose.orientation;
    *position = {0.5f * (eyes[0].pose.position.x + eyes[1].pose.position.x),
                 0.5f * (eyes[0].pose.position.y + eyes[1].pose.position.y),
                 0.5f * (eyes[0].pose.position.z + eyes[1].pose.position.z)};
    return true;
  }

  return false;
}

void ResetCinematicScreenAnchor()
{
  s_cinematic_screen_anchor = {};
}

bool BuildCinematicScreenLayer(const std::array<XREyeSwapchain, 2>& eye_swapchains,
                               uint32_t generation, XrCompositionLayerQuad* layer)
{
  if (!VR::g_openxr || !layer || eye_swapchains[0].swapchain == XR_NULL_HANDLE)
    return false;

  if (!s_cinematic_screen_anchor.valid || s_cinematic_screen_anchor.generation != generation)
  {
    XrQuaternionf orientation{};
    XrVector3f position{};
    if (!ReadCinematicScreenViewerPose(&orientation, &position))
      return false;

    s_cinematic_screen_anchor.valid = true;
    s_cinematic_screen_anchor.generation = generation;
    s_cinematic_screen_anchor.orientation = PGO::YawOnlyQuaternion(orientation);
    s_cinematic_screen_anchor.position = position;
  }

  const XrVector3f offset =
      PGO::RotateVector(s_cinematic_screen_anchor.orientation, {0.0f, 0.0f, -2.0f});
  *layer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
  layer->space = VR::g_openxr->GetReferenceSpace();
  layer->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
  layer->subImage.swapchain = eye_swapchains[0].swapchain;
  layer->subImage.imageRect.offset = {0, 0};
  layer->subImage.imageRect.extent = {static_cast<int32_t>(eye_swapchains[0].width),
                                      static_cast<int32_t>(eye_swapchains[0].height)};
  layer->pose.orientation = s_cinematic_screen_anchor.orientation;
  layer->pose.position = {s_cinematic_screen_anchor.position.x + offset.x,
                          s_cinematic_screen_anchor.position.y + offset.y,
                          s_cinematic_screen_anchor.position.z + offset.z};
  layer->size = {2.2f, 1.65f};
  return true;
}
}  // namespace

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
  // Also enable optional controller profile extensions (Meta, Pico, etc.) when available.
  std::vector<const char*> extensions = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
  if (VR::OpenXRManager::IsRuntimeExtensionSupported(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME))
  {
    extensions.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    INFO_LOG_FMT(VIDEO, "OpenXR: Enabling XR_FB_display_refresh_rate.");
  }
  const auto controller_exts = VR::OpenXRManager::GetAvailableControllerExtensions();
  extensions.insert(extensions.end(), controller_exts.begin(), controller_exts.end());
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

  XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
  if (!VR::D3D11OpenXR::QueryGraphicsRequirements(VR::g_openxr->GetInstance(),
                                                  VR::g_openxr->GetSystemId(), &requirements))
    return false;

  INFO_LOG_FMT(VIDEO,
               "OpenXR: D3D11 requirements — adapter LUID {:#010x}{:08x}, "
               "min feature level {:#x}",
               requirements.adapterLuid.HighPart, requirements.adapterLuid.LowPart,
               static_cast<int>(requirements.minFeatureLevel));

  XrSession session = XR_NULL_HANDLE;
  if (!VR::D3D11OpenXR::CreateSessionFromRequirements(VR::g_openxr->GetInstance(),
                                                      VR::g_openxr->GetSystemId(), requirements,
                                                      D3D::device.Get(), &session))
    return false;

  VR::g_openxr->SetSession(session);
  INFO_LOG_FMT(VIDEO, "OpenXR D3D11: Session created successfully.");
  return true;
}

bool D3DOpenXR::CreateSwapchains()
{
  ASSERT(VR::g_openxr != nullptr);

  const auto& view_cfgs = VR::g_openxr->GetViewConfigViews();
  int64_t swapchain_format = 0;
  if (!VR::D3D11OpenXR::SelectSwapchainFormat(VR::g_openxr->GetSession(), &swapchain_format))
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
    //
    // MUTABLE_FORMAT lets us keep the swapchain declared as sRGB (so the compositor
    // decodes correctly) while creating a UNORM RTV on the underlying texture, so
    // BlitFromTexture writes raw sRGB-encoded bytes without a double gamma encode.
    // With this flag the runtime must back the texture with a DXGI _TYPELESS format.
    info.usageFlags =
        XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;

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
  m_primegun_overlay.Shutdown();

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

  D3D::context->Flush();

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

  const auto overlay = Common::VR::OpenXRInputState::GetPrimedGunOverlay();
  const bool cinematic_screen_active =
      overlay.cinematic_screen_enabled && overlay.cinematic_screen_active;
  if (cinematic_screen_active &&
      BuildCinematicScreenLayer(m_eye_swapchains, overlay.cinematic_screen_generation,
                                &m_cinematic_screen_layer))
  {
    std::vector<XrCompositionLayerBaseHeader*> layers = {
        reinterpret_cast<XrCompositionLayerBaseHeader*>(&m_cinematic_screen_layer)};
    m_primegun_overlay.AppendLayers(&layers);
    return VR::g_openxr->EndFrame(layers);
  }
  if (!cinematic_screen_active)
    ResetCinematicScreenAnchor();

  // Use the submit snapshot captured when the GS pose cache was last refreshed. Using
  // live m_eye_views here would pick up LocateViews that ran between the last draw and
  // xrEndFrame, causing ATW to reproject against the wrong pose.
  const auto& eye_views = VR::g_openxr->GetSubmittedEyeViews();
  if (!VR::g_openxr->AreSubmittedEyeViewsValid())
    return VR::g_openxr->EndFrame({});

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

  if (VR::g_openxr->GetActiveBlendMode() == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND)
  {
    m_projection_layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                    XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
  }

  std::vector<XrCompositionLayerBaseHeader*> layers = {
      reinterpret_cast<XrCompositionLayerBaseHeader*>(&m_projection_layer)};
  m_primegun_overlay.AppendLayers(&layers);

  return VR::g_openxr->EndFrame(layers);
}

}  // namespace DX11

#endif  // ENABLE_VR
