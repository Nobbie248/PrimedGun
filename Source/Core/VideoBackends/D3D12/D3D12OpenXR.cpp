// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef ENABLE_VR

// Must define the D3D12 platform before any OpenXR includes.
#define XR_USE_GRAPHICS_API_D3D12

#include "VideoBackends/D3D12/D3D12OpenXR.h"

#include <algorithm>
#include <array>
#include <vector>

#include "Common/Assert.h"
#include "Common/Logging/Log.h"
#include "Common/VR/OpenXRInputState.h"

#include "VideoBackends/D3D12/D3D12Gfx.h"
#include "VideoBackends/D3D12/DX12Context.h"
#include "VideoBackends/D3D12/DX12Texture.h"
#include "VideoCommon/TextureConfig.h"
#include "VideoCommon/VideoConfig.h"
// Only used for the DXGI swapchain-format helpers, which are shared between the D3D11 and
// D3D12 bindings (both enumerate DXGI_FORMAT values). D3D12OpenXR.h enables both OpenXR
// platform blocks so this header's D3D11-typed declarations compile here.
#include "VideoCommon/VR/OpenXRD3D11Common.h"
#include "VideoCommon/VR/OpenXRManager.h"
#include "VideoCommon/VR/PrimedGunOverlayCommon.h"

namespace DX12
{
std::unique_ptr<D3D12OpenXR> g_openxr_d3d12;
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

CinematicScreenAnchor s_cinematic_screen_anchor;

bool ReadCinematicScreenViewerPose(XrQuaternionf* orientation, XrVector3f* position)
{
  const Common::VR::OpenXRInputSnapshot snapshot =
      Common::VR::OpenXRInputState::GetSnapshot();
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

  if (!s_cinematic_screen_anchor.valid ||
      s_cinematic_screen_anchor.generation != generation)
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

XRLayeredSwapchain::XRLayeredSwapchain() = default;
XRLayeredSwapchain::~XRLayeredSwapchain() = default;
XRLayeredSwapchain::XRLayeredSwapchain(XRLayeredSwapchain&&) noexcept = default;
XRLayeredSwapchain& XRLayeredSwapchain::operator=(XRLayeredSwapchain&&) noexcept = default;

D3D12OpenXR::D3D12OpenXR() = default;

D3D12OpenXR::~D3D12OpenXR()
{
  Shutdown();
}

bool D3D12OpenXR::Initialize()
{
  INFO_LOG_FMT(VIDEO, "OpenXR D3D12: Starting initialization...");
  ASSERT_MSG(VIDEO, !VR::g_openxr, "OpenXRManager already initialized.");

  auto mgr = std::make_unique<VR::OpenXRManager>();

  // The D3D12 graphics binding extension is mandatory.
  // Also enable optional controller profile extensions (Meta, Pico, etc.) when available.
  std::vector<const char*> extensions = {XR_KHR_D3D12_ENABLE_EXTENSION_NAME};
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

  // Publish the manager globally so CreateSessionD3D12 can access instance/system IDs.
  VR::g_openxr = std::move(mgr);

  if (!CreateSessionD3D12())
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
    // Destroy any partially-created swapchains while the session still exists.
    DestroySwapchains();
    VR::g_openxr.reset();
    return false;
  }

  // Register this object as the swapchain provider so Presenter can acquire eye images.
  VR::g_openxr->SetSwapchain(this);

  INFO_LOG_FMT(VIDEO, "OpenXR D3D12: Initialization complete.");
  return true;
}

void D3D12OpenXR::Shutdown()
{
  // Clear swapchain pointer before destroying swapchains so no dangling use occurs.
  if (VR::g_openxr)
    VR::g_openxr->SetSwapchain(nullptr);

  m_primedgun_overlay.Shutdown();
  DestroySwapchains();
  VR::g_openxr.reset();
  INFO_LOG_FMT(VIDEO, "OpenXR D3D12: Shut down.");
}

bool D3D12OpenXR::CreateSessionD3D12()
{
  ASSERT(g_dx_context != nullptr && g_dx_context->GetDevice() != nullptr);
  ASSERT(VR::g_openxr != nullptr);

  PFN_xrGetD3D12GraphicsRequirementsKHR get_requirements = nullptr;
  const XrResult addr_result =
      xrGetInstanceProcAddr(VR::g_openxr->GetInstance(), "xrGetD3D12GraphicsRequirementsKHR",
                            reinterpret_cast<PFN_xrVoidFunction*>(&get_requirements));
  if (XR_FAILED(addr_result) || get_requirements == nullptr)
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR: Could not load xrGetD3D12GraphicsRequirementsKHR.");
    return false;
  }

  XrGraphicsRequirementsD3D12KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
  const XrResult req_result =
      get_requirements(VR::g_openxr->GetInstance(), VR::g_openxr->GetSystemId(), &requirements);
  if (XR_FAILED(req_result))
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR: xrGetD3D12GraphicsRequirementsKHR failed ({}).",
                  static_cast<int>(req_result));
    return false;
  }

  INFO_LOG_FMT(VIDEO,
               "OpenXR: D3D12 requirements — adapter LUID {:#010x}{:08x}, "
               "min feature level {:#x}",
               requirements.adapterLuid.HighPart, requirements.adapterLuid.LowPart,
               static_cast<int>(requirements.minFeatureLevel));

  // Runtimes require the session's device to live on the HMD's adapter. Dolphin created its
  // device from the adapter selected in the graphics settings, so warn when they differ —
  // xrCreateSession is then expected to fail.
  const LUID device_luid = g_dx_context->GetDevice()->GetAdapterLuid();
  if (device_luid.HighPart != requirements.adapterLuid.HighPart ||
      device_luid.LowPart != requirements.adapterLuid.LowPart)
  {
    WARN_LOG_FMT(VIDEO,
                 "OpenXR: Dolphin's D3D12 device is on adapter LUID {:#010x}{:08x}, but the "
                 "runtime wants {:#010x}{:08x}. Select the headset's GPU as the adapter in the "
                 "graphics settings.",
                 device_luid.HighPart, device_luid.LowPart, requirements.adapterLuid.HighPart,
                 requirements.adapterLuid.LowPart);
  }

  XrGraphicsBindingD3D12KHR d3d12_binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
  d3d12_binding.device = g_dx_context->GetDevice();
  d3d12_binding.queue = g_dx_context->GetCommandQueue();

  XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
  session_info.next = &d3d12_binding;
  session_info.systemId = VR::g_openxr->GetSystemId();

  XrSession session = XR_NULL_HANDLE;
  const XrResult result = xrCreateSession(VR::g_openxr->GetInstance(), &session_info, &session);
  if (XR_FAILED(result))
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR: xrCreateSession (D3D12) failed ({}).",
                  static_cast<int>(result));
    return false;
  }

  VR::g_openxr->SetSession(session);
  INFO_LOG_FMT(VIDEO, "OpenXR D3D12: Session created successfully.");
  return true;
}

bool D3D12OpenXR::CreateSwapchains()
{
  ASSERT(VR::g_openxr != nullptr);

  // Both the D3D11 and D3D12 bindings enumerate DXGI_FORMAT values, so the D3D11-named
  // format-selection helper applies unchanged.
  int64_t swapchain_format = 0;
  if (!VR::D3D11OpenXR::SelectSwapchainFormat(VR::g_openxr->GetSession(), &swapchain_format))
    return false;

  m_use_layered_swapchain = false;
  m_frame_uses_layered_swapchain = false;

  // Layered fast path: one arraySize=2 swapchain the presenter fills in a single
  // GS-expanded blit, halving the full-res blits and acquire/release pairs per frame.
  // Only the stereo projection path benefits (the flat panel is mono), and the shared
  // image size requires both eyes to agree.
  if (g_ActiveConfig.stereo_mode == StereoMode::OpenXR && g_backend_info.bSupportsGeometryShaders)
  {
    const auto& view_cfgs = VR::g_openxr->GetViewConfigViews();
    const bool matching_eye_sizes =
        view_cfgs[0].recommendedImageRectWidth == view_cfgs[1].recommendedImageRectWidth &&
        view_cfgs[0].recommendedImageRectHeight == view_cfgs[1].recommendedImageRectHeight;
    if (matching_eye_sizes)
    {
      if (CreateLayeredSwapchain(swapchain_format))
      {
        m_use_layered_swapchain = true;
      }
      else
      {
        WARN_LOG_FMT(VIDEO, "OpenXR: Layered D3D12 swapchain creation failed; falling back to "
                            "two per-eye swapchains.");
      }
    }
    else
    {
      WARN_LOG_FMT(VIDEO,
                   "OpenXR: Layered D3D12 swapchain disabled because eye sizes differ "
                   "({}x{} vs {}x{}).",
                   view_cfgs[0].recommendedImageRectWidth,
                   view_cfgs[0].recommendedImageRectHeight,
                   view_cfgs[1].recommendedImageRectWidth,
                   view_cfgs[1].recommendedImageRectHeight);
    }
  }

  return CreateEyeSwapchains(swapchain_format);
}

bool D3D12OpenXR::CreateLayeredSwapchain(int64_t swapchain_format)
{
  ASSERT(VR::g_openxr != nullptr);

  const auto& view_cfgs = VR::g_openxr->GetViewConfigViews();

  auto& sc = m_layered_swapchain;
  sc.width = view_cfgs[0].recommendedImageRectWidth;
  sc.height = view_cfgs[0].recommendedImageRectHeight;

  const auto cleanup = [&sc]() {
    // Same ordering as DestroySwapchains: drop the DXTexture wrappers (their resource
    // references are released on a fence), flush so the references are really gone,
    // then let the runtime free its images.
    const bool had_wrappers = !sc.textures.empty() || !sc.framebuffers.empty();
    sc.framebuffers.clear();
    sc.textures.clear();
    if (had_wrappers && g_dx_context)
    {
      if (g_gfx)
        Gfx::GetInstance()->ExecuteCommandList(true);
      else
        g_dx_context->ExecuteCommandList(true);
    }
    if (sc.swapchain != XR_NULL_HANDLE)
    {
      xrDestroySwapchain(sc.swapchain);
      sc.swapchain = XR_NULL_HANDLE;
    }
    sc.width = 0;
    sc.height = 0;
  };

  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  info.arraySize = 2;
  info.format = swapchain_format;
  info.width = sc.width;
  info.height = sc.height;
  info.mipCount = 1;
  info.faceCount = 1;
  info.sampleCount = 1;
  // Same usage as the per-eye swapchains: color target only, MUTABLE_FORMAT so a UNORM
  // RTV can alias the sRGB-declared texture (avoids a double gamma encode).
  info.usageFlags =
      XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;

  XrResult result = xrCreateSwapchain(VR::g_openxr->GetSession(), &info, &sc.swapchain);
  if (XR_FAILED(result))
  {
    WARN_LOG_FMT(VIDEO, "OpenXR: xrCreateSwapchain failed for layered D3D12 swapchain ({}).",
                 static_cast<int>(result));
    cleanup();
    return false;
  }

  uint32_t image_count = 0;
  result = xrEnumerateSwapchainImages(sc.swapchain, 0, &image_count, nullptr);
  if (XR_FAILED(result) || image_count == 0)
  {
    WARN_LOG_FMT(VIDEO,
                 "OpenXR: xrEnumerateSwapchainImages failed for layered D3D12 swapchain ({}).",
                 static_cast<int>(result));
    cleanup();
    return false;
  }

  std::vector<XrSwapchainImageD3D12KHR> images(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
  result = xrEnumerateSwapchainImages(sc.swapchain, image_count, &image_count,
                                      reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
  if (XR_FAILED(result))
  {
    WARN_LOG_FMT(VIDEO,
                 "OpenXR: xrEnumerateSwapchainImages data failed for layered D3D12 swapchain "
                 "({}).",
                 static_cast<int>(result));
    cleanup();
    return false;
  }

  sc.textures.resize(image_count);
  sc.framebuffers.resize(image_count);

  for (uint32_t i = 0; i < image_count; ++i)
  {
    // CreateAdopted reads the resource descriptor, so the wrapper picks up both array
    // slices (layers=2) and DXFramebuffer's RTV spans them.
    sc.textures[i] =
        DXTexture::CreateAdopted(images[i].texture, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (!sc.textures[i])
    {
      WARN_LOG_FMT(VIDEO, "OpenXR: DXTexture::CreateAdopted failed for layered image {}.", i);
      cleanup();
      return false;
    }
    if (sc.textures[i]->GetLayers() < 2)
    {
      // A single-slice resource would silently leave the right eye black — fall back.
      WARN_LOG_FMT(VIDEO,
                   "OpenXR: Layered D3D12 swapchain image {} has {} layer(s), expected 2.", i,
                   sc.textures[i]->GetLayers());
      cleanup();
      return false;
    }

    sc.framebuffers[i] = DXFramebuffer::Create(sc.textures[i].get(), nullptr, {});
    if (!sc.framebuffers[i])
    {
      WARN_LOG_FMT(VIDEO, "OpenXR: DXFramebuffer::Create failed for layered image {}.", i);
      cleanup();
      return false;
    }
  }

  INFO_LOG_FMT(VIDEO, "OpenXR: Layered D3D12 swapchain ready: {}x{}, {} images, arraySize=2.",
               sc.width, sc.height, image_count);
  return true;
}

bool D3D12OpenXR::CreateEyeSwapchains(int64_t swapchain_format)
{
  ASSERT(VR::g_openxr != nullptr);

  const auto& view_cfgs = VR::g_openxr->GetViewConfigViews();

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

    // Enumerate the D3D12 resources backing this swapchain.
    uint32_t image_count = 0;
    xrEnumerateSwapchainImages(sc.swapchain, 0, &image_count, nullptr);

    std::vector<XrSwapchainImageD3D12KHR> images(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    xrEnumerateSwapchainImages(sc.swapchain, image_count, &image_count,
                               reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));

    sc.textures.resize(image_count);
    sc.framebuffers.resize(image_count);

    for (uint32_t i = 0; i < image_count; ++i)
    {
      // Adopt the runtime-owned resource. DXTexture::CreateAdopted reads the resource
      // descriptor to build the TextureConfig and AddRefs the underlying resource.
      // The runtime guarantees acquired color images are in a RENDER_TARGET-compatible
      // state and expects them back in that state on release, so start tracking there.
      sc.textures[i] =
          DXTexture::CreateAdopted(images[i].texture, D3D12_RESOURCE_STATE_RENDER_TARGET);
      if (!sc.textures[i])
      {
        ERROR_LOG_FMT(VIDEO, "OpenXR: DXTexture::CreateAdopted failed for eye {}, image {}.",
                      eye, i);
        return false;
      }

      // No depth attachment; the compositor only consumes the color layer.
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

void D3D12OpenXR::DestroySwapchains()
{
  bool had_wrappers = false;

  if (m_layered_image_acquired && m_layered_swapchain.swapchain != XR_NULL_HANDLE)
  {
    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    const XrResult release_result =
        xrReleaseSwapchainImage(m_layered_swapchain.swapchain, &release_info);
    if (XR_FAILED(release_result))
    {
      WARN_LOG_FMT(VIDEO,
                   "OpenXR: xrReleaseSwapchainImage during shutdown failed for layered "
                   "swapchain ({}).",
                   static_cast<int>(release_result));
    }
    m_layered_image_acquired = false;
  }

  had_wrappers |=
      !m_layered_swapchain.textures.empty() || !m_layered_swapchain.framebuffers.empty();
  m_layered_swapchain.framebuffers.clear();
  m_layered_swapchain.textures.clear();

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

    // Release Dolphin wrappers before destroying the swapchain. The DXTexture destructor
    // defers the release of its reference until the current command list's fence passes,
    // so an explicit flush below drops them before the runtime frees the resources.
    had_wrappers |= !sc.textures.empty() || !sc.framebuffers.empty();
    sc.framebuffers.clear();
    sc.textures.clear();
  }

  // Wait for the GPU and destroy the deferred references on the runtime-owned resources.
  if (had_wrappers && g_dx_context)
  {
    if (g_gfx)
      Gfx::GetInstance()->ExecuteCommandList(true);
    else
      g_dx_context->ExecuteCommandList(true);
  }

  if (m_layered_swapchain.swapchain != XR_NULL_HANDLE)
  {
    const XrResult destroy_result = xrDestroySwapchain(m_layered_swapchain.swapchain);
    if (XR_FAILED(destroy_result))
    {
      WARN_LOG_FMT(VIDEO, "OpenXR: xrDestroySwapchain failed for layered swapchain ({}).",
                   static_cast<int>(destroy_result));
    }
    m_layered_swapchain.swapchain = XR_NULL_HANDLE;
  }
  m_layered_swapchain.width = 0;
  m_layered_swapchain.height = 0;
  m_use_layered_swapchain = false;
  m_frame_uses_layered_swapchain = false;

  for (uint32_t eye = 0; eye < 2; ++eye)
  {
    auto& sc = m_eye_swapchains[eye];
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

AbstractFramebuffer* D3D12OpenXR::AcquireEyeFramebuffer(uint32_t eye_index)
{
  ASSERT(eye_index < 2);
  auto& sc = m_eye_swapchains[eye_index];
  m_frame_uses_layered_swapchain = false;

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

  // The runtime hands acquired color images over in a RENDER_TARGET-compatible state;
  // resync our tracking without recording a barrier.
  const uint32_t idx = m_acquired_image_index[eye_index];
  sc.textures[idx]->OverrideState(D3D12_RESOURCE_STATE_RENDER_TARGET);

  return sc.framebuffers[idx].get();
}

void D3D12OpenXR::ReleaseEyeTexture(uint32_t eye_index)
{
  ASSERT(eye_index < 2);
  if (!m_image_acquired[eye_index])
    return;

  auto& sc = m_eye_swapchains[eye_index];

  // The runtime interprets released color images as RENDER_TARGET; record a barrier if the
  // blit left the image in another state.
  sc.textures[m_acquired_image_index[eye_index]]->TransitionToState(
      D3D12_RESOURCE_STATE_RENDER_TARGET);

  // The spec only requires the image writes to be *submitted* to the queue in the graphics
  // binding before xrReleaseSwapchainImage; GPU-side ordering is the runtime's job. Dolphin
  // batches commands into open command lists, so kick the current one now.
  Gfx::GetInstance()->ExecuteCommandList(false);

  XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  const XrResult result = xrReleaseSwapchainImage(sc.swapchain, &release_info);
  if (XR_FAILED(result))
  {
    WARN_LOG_FMT(VIDEO, "OpenXR: xrReleaseSwapchainImage failed for eye {} ({}).", eye_index,
                 static_cast<int>(result));
  }
  m_image_acquired[eye_index] = false;
}

AbstractFramebuffer* D3D12OpenXR::AcquireLayeredFramebuffer()
{
  auto& sc = m_layered_swapchain;
  if (!m_use_layered_swapchain || sc.swapchain == XR_NULL_HANDLE)
    return nullptr;

  XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  if (XR_FAILED(
          xrAcquireSwapchainImage(sc.swapchain, &acquire_info, &m_acquired_layered_image_index)))
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR: xrAcquireSwapchainImage failed for layered swapchain.");
    m_frame_uses_layered_swapchain = false;
    return nullptr;
  }
  m_layered_image_acquired = true;

  XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wait_info.timeout = XR_INFINITE_DURATION;
  if (XR_FAILED(xrWaitSwapchainImage(sc.swapchain, &wait_info)))
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR: xrWaitSwapchainImage failed for layered swapchain.");

    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    const XrResult release_result = xrReleaseSwapchainImage(sc.swapchain, &release_info);
    if (XR_FAILED(release_result))
    {
      WARN_LOG_FMT(VIDEO,
                   "OpenXR: xrReleaseSwapchainImage after layered wait failure failed ({}).",
                   static_cast<int>(release_result));
    }
    m_layered_image_acquired = false;
    m_frame_uses_layered_swapchain = false;
    return nullptr;
  }

  // The runtime hands acquired color images over in a RENDER_TARGET-compatible state;
  // resync our tracking without recording a barrier.
  const uint32_t idx = m_acquired_layered_image_index;
  sc.textures[idx]->OverrideState(D3D12_RESOURCE_STATE_RENDER_TARGET);
  m_frame_uses_layered_swapchain = true;

  return sc.framebuffers[idx].get();
}

void D3D12OpenXR::ReleaseLayeredTexture()
{
  if (!m_layered_image_acquired)
    return;

  // Same contract as ReleaseEyeTexture: hand the image back in RENDER_TARGET state and
  // make sure the blit is submitted to the queue before releasing.
  m_layered_swapchain.textures[m_acquired_layered_image_index]->TransitionToState(
      D3D12_RESOURCE_STATE_RENDER_TARGET);
  Gfx::GetInstance()->ExecuteCommandList(false);

  XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  const XrResult result = xrReleaseSwapchainImage(m_layered_swapchain.swapchain, &release_info);
  if (XR_FAILED(result))
  {
    WARN_LOG_FMT(VIDEO, "OpenXR: xrReleaseSwapchainImage failed for layered swapchain ({}).",
                 static_cast<int>(result));
  }
  m_layered_image_acquired = false;
}

bool D3D12OpenXR::SubmitFrame()
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
    m_primedgun_overlay.AppendLayers(&layers);
    m_frame_uses_layered_swapchain = false;
    if (VR::g_openxr->IsFrameThreadActive())
    {
      VR::g_openxr->PublishLayers(layers);
      return true;
    }
    return VR::g_openxr->EndFrame(layers);
  }
  if (!cinematic_screen_active)
    ResetCinematicScreenAnchor();

  // Use the pose the presented XFB was rendered with (stamped at its XFB copy and
  // selected in FetchXFB). Live m_eye_views — or even the live submit snapshot — can
  // already belong to the NEXT frame when presentation is deferred to VI time, and a
  // pose/content mismatch makes ATW reproject the frame to the wrong place.
  const auto& eye_views = VR::g_openxr->GetPresentEyeViews();
  const bool submit_layered = m_frame_uses_layered_swapchain && m_use_layered_swapchain &&
                              m_layered_swapchain.swapchain != XR_NULL_HANDLE;

  for (uint32_t eye = 0; eye < 2; ++eye)
  {
    auto& pv = m_projection_views[eye];
    pv = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    pv.pose = eye_views[eye].pose;
    pv.fov = eye_views[eye].fov;
    if (submit_layered)
    {
      pv.subImage.swapchain = m_layered_swapchain.swapchain;
      pv.subImage.imageArrayIndex = eye;
      pv.subImage.imageRect = {{0, 0},
                               {static_cast<int32_t>(m_layered_swapchain.width),
                                static_cast<int32_t>(m_layered_swapchain.height)}};
    }
    else
    {
      pv.subImage.swapchain = m_eye_swapchains[eye].swapchain;
      pv.subImage.imageArrayIndex = 0;
      pv.subImage.imageRect = {
          {0, 0},
          {static_cast<int32_t>(m_eye_swapchains[eye].width),
           static_cast<int32_t>(m_eye_swapchains[eye].height)}};
    }
  }

  m_projection_layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  m_projection_layer.space = VR::g_openxr->GetReferenceSpace();
  m_projection_layer.viewCount = 2;
  m_projection_layer.views = m_projection_views.data();
  m_projection_layer.layerFlags = VR::g_openxr->GetProjectionLayerExtraFlags();

  std::vector<XrCompositionLayerBaseHeader*> layers = {
      reinterpret_cast<XrCompositionLayerBaseHeader*>(&m_projection_layer)};
  m_primedgun_overlay.AppendLayers(&layers);

  if (VR::g_openxr->IsFrameThreadActive())
  {
    // The pacing thread owns xrEndFrame. PublishLayers deep-copies projection and
    // PrimeGun quad data before this function returns.
    VR::g_openxr->PublishLayers(layers);
    m_frame_uses_layered_swapchain = false;
    return true;
  }

  const bool result = VR::g_openxr->EndFrame(layers);
  m_frame_uses_layered_swapchain = false;
  return result;
}

}  // namespace DX12

#endif  // ENABLE_VR
