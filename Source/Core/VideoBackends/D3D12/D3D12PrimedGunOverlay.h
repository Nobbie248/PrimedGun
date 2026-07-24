// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef ENABLE_VR

#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12

#include <cstdint>
#include <memory>
#include <vector>

#include <d3d11.h>
#include <d3d12.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

namespace DX12
{
class DXTexture;

struct XRPrimedGunOverlaySwapchain
{
  XRPrimedGunOverlaySwapchain();
  ~XRPrimedGunOverlaySwapchain();
  XRPrimedGunOverlaySwapchain(XRPrimedGunOverlaySwapchain&&) noexcept;
  XRPrimedGunOverlaySwapchain& operator=(XRPrimedGunOverlaySwapchain&&) noexcept;

  XRPrimedGunOverlaySwapchain(const XRPrimedGunOverlaySwapchain&) = delete;
  XRPrimedGunOverlaySwapchain& operator=(const XRPrimedGunOverlaySwapchain&) = delete;

  XrSwapchain swapchain = XR_NULL_HANDLE;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t content_kind = 0;
  uint32_t generation = 0;
  bool texture_ready = false;
  std::vector<XrSwapchainImageD3D12KHR> images;
  std::vector<std::unique_ptr<DXTexture>> textures;
};

struct XRPrimedGunLaserSwapchain
{
  XRPrimedGunLaserSwapchain();
  ~XRPrimedGunLaserSwapchain();
  XRPrimedGunLaserSwapchain(XRPrimedGunLaserSwapchain&&) noexcept;
  XRPrimedGunLaserSwapchain& operator=(XRPrimedGunLaserSwapchain&&) noexcept;

  XRPrimedGunLaserSwapchain(const XRPrimedGunLaserSwapchain&) = delete;
  XRPrimedGunLaserSwapchain& operator=(const XRPrimedGunLaserSwapchain&) = delete;

  XrSwapchain swapchain = XR_NULL_HANDLE;
  bool texture_ready = false;
  std::vector<XrSwapchainImageD3D12KHR> images;
  std::vector<std::unique_ptr<DXTexture>> textures;
};

class D3D12PrimedGunOverlay
{
public:
  D3D12PrimedGunOverlay() = default;
  ~D3D12PrimedGunOverlay();

  D3D12PrimedGunOverlay(const D3D12PrimedGunOverlay&) = delete;
  D3D12PrimedGunOverlay& operator=(const D3D12PrimedGunOverlay&) = delete;

  void Shutdown();
  bool AppendLayers(std::vector<XrCompositionLayerBaseHeader*>* layers);

private:
  bool EnsureOverlaySwapchain(XRPrimedGunOverlaySwapchain* overlay, uint32_t content_kind,
                              uint32_t generation, uint32_t width, uint32_t height,
                              const std::vector<uint32_t>& pixels);
  void DestroyOverlaySwapchain(XRPrimedGunOverlaySwapchain* overlay);
  bool EnsureLaserSwapchain();
  void DestroyLaserSwapchain();
  void FlushDeferredResources(bool had_textures);

  XRPrimedGunOverlaySwapchain m_overlay_swapchain{};
  XRPrimedGunOverlaySwapchain m_position_marker_swapchain{};
  XRPrimedGunLaserSwapchain m_laser_swapchain{};
  XrCompositionLayerQuad m_overlay_layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
  XrCompositionLayerQuad m_position_marker_layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
  XrCompositionLayerQuad m_laser_layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
};
}  // namespace DX12

#endif  // ENABLE_VR
