// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef ENABLE_VR

#include "VideoBackends/D3D/D3DPrimedGunOverlay.h"

#include <array>
#include <vector>

#include "Common/Logging/Log.h"
#include "Common/VR/OpenXRInputState.h"

#include "VideoBackends/D3D/D3DBase.h"
#include "VideoCommon/VR/OpenXRD3D11Common.h"
#include "VideoCommon/VR/OpenXRManager.h"
#include "VideoCommon/VR/PrimedGunOverlayCommon.h"

namespace DX11
{
namespace PGO = PrimedGun::Overlay;

D3DPrimedGunOverlay::~D3DPrimedGunOverlay()
{
  Shutdown();
}

void D3DPrimedGunOverlay::Shutdown()
{
  DestroyOverlaySwapchain(&m_overlay_swapchain);
  DestroyOverlaySwapchain(&m_position_marker_swapchain);
  DestroyLaserSwapchain();
}

void D3DPrimedGunOverlay::DestroyOverlaySwapchain(XRPrimedGunOverlaySwapchain* overlay)
{
  if (!overlay)
    return;

  overlay->images.clear();
  overlay->texture_ready = false;
  overlay->content_kind = 0;
  overlay->generation = 0;
  if (overlay->swapchain != XR_NULL_HANDLE)
  {
    const XrResult result = xrDestroySwapchain(overlay->swapchain);
    if (XR_FAILED(result))
      WARN_LOG_FMT(VIDEO, "OpenXR: PrimedGun overlay xrDestroySwapchain failed ({}).",
                   static_cast<int>(result));
    overlay->swapchain = XR_NULL_HANDLE;
  }
}

void D3DPrimedGunOverlay::DestroyLaserSwapchain()
{
  auto& laser = m_laser_swapchain;
  laser.images.clear();
  laser.texture_ready = false;
  if (laser.swapchain != XR_NULL_HANDLE)
  {
    const XrResult result = xrDestroySwapchain(laser.swapchain);
    if (XR_FAILED(result))
      WARN_LOG_FMT(VIDEO, "OpenXR: PrimedGun laser xrDestroySwapchain failed ({}).",
                   static_cast<int>(result));
    laser.swapchain = XR_NULL_HANDLE;
  }
}

bool D3DPrimedGunOverlay::EnsureLaserSwapchain()
{
  auto& laser = m_laser_swapchain;
  if (laser.texture_ready && laser.swapchain != XR_NULL_HANDLE)
    return true;

  if (!VR::g_openxr)
    return false;

  DestroyLaserSwapchain();
  int64_t swapchain_format = DXGI_FORMAT_R8G8B8A8_UNORM;
  VR::D3D11OpenXR::SelectSwapchainFormat(VR::g_openxr->GetSession(), &swapchain_format);

  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  info.arraySize = 1;
  info.format = swapchain_format;
  info.width = 16;
  info.height = 10;
  info.mipCount = 1;
  info.faceCount = 1;
  info.sampleCount = 1;
  info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT;

  XrResult result = xrCreateSwapchain(VR::g_openxr->GetSession(), &info, &laser.swapchain);
  if (XR_FAILED(result) || laser.swapchain == XR_NULL_HANDLE)
    return false;

  uint32_t image_count = 0;
  result = xrEnumerateSwapchainImages(laser.swapchain, 0, &image_count, nullptr);
  if (XR_FAILED(result) || image_count == 0)
  {
    DestroyLaserSwapchain();
    return false;
  }

  laser.images.assign(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
  result = xrEnumerateSwapchainImages(
      laser.swapchain, image_count, &image_count,
      reinterpret_cast<XrSwapchainImageBaseHeader*>(laser.images.data()));
  if (XR_FAILED(result))
  {
    DestroyLaserSwapchain();
    return false;
  }

  std::array<uint32_t, 160> pixels{};
  for (int y = 3; y < 7; ++y)
    for (int x = 0; x < 16; ++x)
      pixels[static_cast<size_t>(y * 16 + x)] = 0xE080D8FFu;

  for (uint32_t i = 0; i < image_count; ++i)
  {
    uint32_t acquired = 0;
    XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    result = xrAcquireSwapchainImage(laser.swapchain, &acquire_info, &acquired);
    if (XR_FAILED(result))
    {
      DestroyLaserSwapchain();
      return false;
    }

    XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait_info.timeout = XR_INFINITE_DURATION;
    result = xrWaitSwapchainImage(laser.swapchain, &wait_info);
    if (XR_SUCCEEDED(result) && acquired < laser.images.size() && laser.images[acquired].texture)
    {
      D3D::context->UpdateSubresource(laser.images[acquired].texture, 0, nullptr, pixels.data(),
                                      16 * sizeof(uint32_t), 0);
      D3D::context->Flush();
    }

    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(laser.swapchain, &release_info);
    if (XR_FAILED(result))
    {
      DestroyLaserSwapchain();
      return false;
    }
  }

  laser.texture_ready = true;
  return true;
}

bool D3DPrimedGunOverlay::EnsureOverlaySwapchain(XRPrimedGunOverlaySwapchain* overlay,
                                                uint32_t content_kind, uint32_t generation,
                                                uint32_t width, uint32_t height,
                                                const std::vector<uint32_t>& pixels)
{
  if (!overlay)
    return false;

  if (overlay->texture_ready && overlay->swapchain != XR_NULL_HANDLE &&
      overlay->content_kind == content_kind && overlay->generation == generation &&
      overlay->width == width && overlay->height == height)
  {
    return true;
  }

  if (!VR::g_openxr)
    return false;

  DestroyOverlaySwapchain(overlay);
  overlay->width = width;
  overlay->height = height;

  int64_t swapchain_format = DXGI_FORMAT_R8G8B8A8_UNORM;
  VR::D3D11OpenXR::SelectSwapchainFormat(VR::g_openxr->GetSession(), &swapchain_format);

  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  info.arraySize = 1;
  info.format = swapchain_format;
  info.width = width;
  info.height = height;
  info.mipCount = 1;
  info.faceCount = 1;
  info.sampleCount = 1;
  info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT;

  XrResult result = xrCreateSwapchain(VR::g_openxr->GetSession(), &info, &overlay->swapchain);
  if (XR_FAILED(result) || overlay->swapchain == XR_NULL_HANDLE)
  {
    WARN_LOG_FMT(VIDEO, "OpenXR: PrimedGun overlay xrCreateSwapchain failed ({}).",
                 static_cast<int>(result));
    return false;
  }

  uint32_t image_count = 0;
  result = xrEnumerateSwapchainImages(overlay->swapchain, 0, &image_count, nullptr);
  if (XR_FAILED(result) || image_count == 0)
  {
    DestroyOverlaySwapchain(overlay);
    return false;
  }

  overlay->images.assign(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
  result = xrEnumerateSwapchainImages(
      overlay->swapchain, image_count, &image_count,
      reinterpret_cast<XrSwapchainImageBaseHeader*>(overlay->images.data()));
  if (XR_FAILED(result))
  {
    DestroyOverlaySwapchain(overlay);
    return false;
  }

  for (uint32_t i = 0; i < image_count; ++i)
  {
    uint32_t acquired = 0;
    XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    result = xrAcquireSwapchainImage(overlay->swapchain, &acquire_info, &acquired);
    if (XR_FAILED(result))
    {
      DestroyOverlaySwapchain(overlay);
      return false;
    }

    XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait_info.timeout = XR_INFINITE_DURATION;
    result = xrWaitSwapchainImage(overlay->swapchain, &wait_info);
    if (XR_SUCCEEDED(result) && acquired < overlay->images.size() &&
        overlay->images[acquired].texture)
    {
      D3D::context->UpdateSubresource(overlay->images[acquired].texture, 0, nullptr, pixels.data(),
                                      width * sizeof(uint32_t), 0);
      D3D::context->Flush();
    }

    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(overlay->swapchain, &release_info);
    if (XR_FAILED(result))
    {
      DestroyOverlaySwapchain(overlay);
      return false;
    }
  }

  overlay->content_kind = content_kind;
  overlay->generation = generation;
  overlay->texture_ready = true;
  return true;
}

bool D3DPrimedGunOverlay::AppendLayers(std::vector<XrCompositionLayerBaseHeader*>* layers)
{
  if (!VR::g_openxr || !layers)
    return false;

  const auto overlay = Common::VR::OpenXRInputState::GetPrimedGunOverlay();
  if (!overlay.menu_visible && !overlay.prompt_visible && !overlay.weapon_panel_visible &&
      !overlay.position_marker_visible)
    return false;

  const Common::VR::OpenXRInputSnapshot snapshot = Common::VR::OpenXRInputState::GetSnapshot();
  if (!snapshot.runtime_active)
    return false;

  bool appended_layer = false;
  if (overlay.position_marker_visible)
  {
    constexpr uint32_t marker_width = 512;
    constexpr uint32_t marker_height = 512;
    const std::vector<uint32_t> marker_pixels =
        PGO::BuildPositionMarkerPixels(marker_width, marker_height);
    if (EnsureOverlaySwapchain(&m_position_marker_swapchain, 4u, 1u, marker_width, marker_height,
                               marker_pixels))
    {
      m_position_marker_layer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
      m_position_marker_layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                           XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
      m_position_marker_layer.space = VR::g_openxr->GetReferenceSpace();
      m_position_marker_layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
      m_position_marker_layer.subImage.swapchain = m_position_marker_swapchain.swapchain;
      m_position_marker_layer.subImage.imageRect.offset = {0, 0};
      m_position_marker_layer.subImage.imageRect.extent = {static_cast<int32_t>(marker_width),
                                                           static_cast<int32_t>(marker_height)};
      m_position_marker_layer.pose.orientation = {-0.70710678f, 0.0f, 0.0f, 0.70710678f};
      m_position_marker_layer.pose.position = {snapshot.tracking_origin_position[0], 0.005f,
                                               snapshot.tracking_origin_position[2]};
      m_position_marker_layer.size = {0.356f, 0.356f};
      layers->push_back(
          reinterpret_cast<XrCompositionLayerBaseHeader*>(&m_position_marker_layer));
      appended_layer = true;
    }
  }

  if (!overlay.menu_visible && !overlay.prompt_visible && !overlay.weapon_panel_visible)
    return appended_layer;

  const bool menu = overlay.menu_visible;
  const bool weapon_panel = !menu && overlay.weapon_panel_visible;
  const uint32_t content_kind = menu ? 2u : weapon_panel ? 3u : 1u;
  const uint32_t width = menu ? 1024 : weapon_panel ? 512 : 1024;
  const uint32_t height = menu ? 512 : weapon_panel ? 512 : 384;
  const uint32_t generation =
      menu ? overlay.generation : weapon_panel ? (100u + overlay.weapon_selected_index) : 1u;
  const std::vector<uint32_t> pixels =
      menu ? PGO::BuildMenuPixels(width, height, overlay) :
             weapon_panel ? PGO::BuildWeaponPanelPixels(width, height, overlay) :
                            PGO::BuildPromptPixels(width, height);
  if (!EnsureOverlaySwapchain(&m_overlay_swapchain, content_kind, generation, width, height,
                              pixels))
    return appended_layer;

  m_overlay_layer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
  m_overlay_layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                               XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
  m_overlay_layer.space = VR::g_openxr->GetReferenceSpace();
  m_overlay_layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
  m_overlay_layer.subImage.swapchain = m_overlay_swapchain.swapchain;
  m_overlay_layer.subImage.imageRect.offset = {0, 0};
  m_overlay_layer.subImage.imageRect.extent = {static_cast<int32_t>(width),
                                               static_cast<int32_t>(height)};

  PGO::HybridControllerPose left_grip_pose = PGO::MakeGripPose(snapshot.controllers[0]);
  PGO::HybridControllerPose right_grip_pose = PGO::MakeGripPose(snapshot.controllers[1]);
  PGO::HybridControllerPose left_aim_pose = PGO::MakeAimPose(snapshot.controllers[0]);
  PGO::HybridControllerPose right_aim_pose = PGO::MakeAimPose(snapshot.controllers[1]);
  PGO::AddTrackingOrigin(&left_grip_pose, snapshot);
  PGO::AddTrackingOrigin(&right_grip_pose, snapshot);
  PGO::AddTrackingOrigin(&left_aim_pose, snapshot);
  PGO::AddTrackingOrigin(&right_aim_pose, snapshot);
  const PGO::HybridControllerPose& panel_pose =
      overlay.use_right_hand ? left_grip_pose : right_grip_pose;
  const PGO::HybridControllerPose& laser_pose =
      overlay.use_right_hand ? right_aim_pose : left_aim_pose;

  if (menu && panel_pose.valid)
  {
    const XrQuaternionf q = panel_pose.orientation;
    m_overlay_layer.pose.orientation =
        PGO::MulQuat(q, {-0.70710678f, 0.0f, 0.0f, 0.70710678f});
    const XrVector3f offset =
        PGO::RotateVector(m_overlay_layer.pose.orientation, {0.0f, 0.10f, -0.18f});
    m_overlay_layer.pose.position = {panel_pose.position.x + offset.x,
                                     panel_pose.position.y + offset.y,
                                     panel_pose.position.z + offset.z};
    m_overlay_layer.size = {1.05f, 0.72f};
  }
  else if (weapon_panel)
  {
    m_overlay_layer.pose.orientation = {overlay.weapon_panel_orientation[0],
                                        overlay.weapon_panel_orientation[1],
                                        overlay.weapon_panel_orientation[2],
                                        overlay.weapon_panel_orientation[3]};
    const XrVector3f offset =
        PGO::RotateVector(m_overlay_layer.pose.orientation, {0.0f, 0.055f, -0.26f});
    m_overlay_layer.pose.position = {
        overlay.weapon_panel_position[0] + snapshot.tracking_origin_position[0] + offset.x,
        overlay.weapon_panel_position[1] + snapshot.tracking_origin_position[1] + offset.y,
        overlay.weapon_panel_position[2] + snapshot.tracking_origin_position[2] + offset.z};
    m_overlay_layer.size = {0.42f, 0.42f};
  }
  else if (snapshot.head_pose.valid)
  {
    const auto& head = snapshot.head_pose;
    m_overlay_layer.pose.orientation = {head.orientation[0], head.orientation[1],
                                        head.orientation[2], head.orientation[3]};
    const XrVector3f offset =
        PGO::RotateVector(m_overlay_layer.pose.orientation, {0.0f, 0.0f, -1.35f});
    m_overlay_layer.pose.position = {
        head.position[0] + snapshot.tracking_origin_position[0] + offset.x,
        head.position[1] + snapshot.tracking_origin_position[1] + offset.y,
        head.position[2] + snapshot.tracking_origin_position[2] + offset.z};
    m_overlay_layer.size = {0.675f, 0.25f};
  }
  else
  {
    return appended_layer;
  }

  layers->push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&m_overlay_layer));
  appended_layer = true;

  if (menu && laser_pose.valid && EnsureLaserSwapchain())
  {
    const XrQuaternionf q = laser_pose.orientation;
    const XrVector3f forward = PGO::RotateVector(q, {0.0f, 0.0f, -1.0f});
    m_laser_layer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    m_laser_layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                               XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    m_laser_layer.space = VR::g_openxr->GetReferenceSpace();
    m_laser_layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    m_laser_layer.subImage.swapchain = m_laser_swapchain.swapchain;
    m_laser_layer.subImage.imageRect.offset = {0, 0};
    m_laser_layer.subImage.imageRect.extent = {16, 10};
    m_laser_layer.pose.orientation =
        PGO::MulQuat(q, {-0.70710678f, 0.0f, 0.0f, 0.70710678f});
    m_laser_layer.pose.position = {laser_pose.position.x + forward.x * 0.40f,
                                   laser_pose.position.y + forward.y * 0.40f,
                                   laser_pose.position.z + forward.z * 0.40f};
    m_laser_layer.size = {0.008f, 0.80f};
    layers->push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&m_laser_layer));
  }

  return appended_layer;
}
}  // namespace DX11

#endif  // ENABLE_VR
