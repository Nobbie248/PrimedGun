// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef ENABLE_VR

#define XR_USE_GRAPHICS_API_D3D11

#include <cstdint>
#include <vector>

#include <d3d11.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

namespace DX11
{
struct XRPrimedGunOverlaySwapchain
{
  XrSwapchain swapchain = XR_NULL_HANDLE;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t content_kind = 0;
  uint32_t generation = 0;
  bool texture_ready = false;
  std::vector<XrSwapchainImageD3D11KHR> images;
};

struct XRPrimedGunLaserSwapchain
{
  XrSwapchain swapchain = XR_NULL_HANDLE;
  bool texture_ready = false;
  std::vector<XrSwapchainImageD3D11KHR> images;
};

class D3DPrimedGunOverlay
{
public:
  D3DPrimedGunOverlay() = default;
  ~D3DPrimedGunOverlay();

  D3DPrimedGunOverlay(const D3DPrimedGunOverlay&) = delete;
  D3DPrimedGunOverlay& operator=(const D3DPrimedGunOverlay&) = delete;

  void Shutdown();
  bool AppendLayers(std::vector<XrCompositionLayerBaseHeader*>* layers);

private:
  bool EnsureOverlaySwapchain(XRPrimedGunOverlaySwapchain* overlay, uint32_t content_kind,
                              uint32_t generation, uint32_t width, uint32_t height,
                              const std::vector<uint32_t>& pixels);
  void DestroyOverlaySwapchain(XRPrimedGunOverlaySwapchain* overlay);
  bool EnsureLaserSwapchain();
  void DestroyLaserSwapchain();

  XRPrimedGunOverlaySwapchain m_overlay_swapchain{};
  XRPrimedGunOverlaySwapchain m_position_marker_swapchain{};
  XRPrimedGunLaserSwapchain m_laser_swapchain{};
  XrCompositionLayerQuad m_overlay_layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
  XrCompositionLayerQuad m_position_marker_layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
  XrCompositionLayerQuad m_laser_layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
};
}  // namespace DX11

#endif  // ENABLE_VR
