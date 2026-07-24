// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef ENABLE_VR

#include <array>
#include <memory>
#include <vector>
// Must define the D3D11 platform before including openxr_platform.h.
#define XR_USE_GRAPHICS_API_D3D11
#include <d3d11.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "VideoBackends/D3D/D3DPrimedGunOverlay.h"
#include "VideoCommon/VR/OpenXRManager.h"

namespace DX11
{
class DXTexture;
class DXFramebuffer;

// Holds the swapchain images for one eye.
// The ID3D11Texture2D objects are owned by the OpenXR runtime; DXTexture wraps them
// (with AddRef) and DXFramebuffer provides an RTV for rendering.
struct XREyeSwapchain
{
  XrSwapchain swapchain = XR_NULL_HANDLE;
  uint32_t width = 0;
  uint32_t height = 0;

  // One entry per swapchain image.
  std::vector<std::unique_ptr<DXTexture>> textures;
  std::vector<std::unique_ptr<DXFramebuffer>> framebuffers;
};

// D3D11-specific OpenXR backend. Implements VR::IOpenXRSwapchain so that
// Presenter::RenderXFBToScreen() can acquire/release eye images and submit
// frames using only VideoCommon-visible types (AbstractFramebuffer*).
class D3DOpenXR : public VR::IOpenXRSwapchain
{
public:
  D3DOpenXR();
  ~D3DOpenXR() override;

  D3DOpenXR(const D3DOpenXR&) = delete;
  D3DOpenXR& operator=(const D3DOpenXR&) = delete;

  // Full initialization: creates XrInstance + system, D3D11-bound XrSession,
  // reference space, and per-eye swapchains.
  bool Initialize();

  // Tears down swapchains and resets g_openxr.
  void Shutdown();

  // ---- IOpenXRSwapchain ----

  // Acquire the next swapchain image for the given eye.
  // Returns AbstractFramebuffer* (actually DXFramebuffer*) to render into.
  AbstractFramebuffer* AcquireEyeFramebuffer(uint32_t eye_index) override;

  // Release the current swapchain image back to the runtime.
  void ReleaseEyeTexture(uint32_t eye_index) override;

  // Build the XrCompositionLayerProjection and call xrEndFrame.
  bool SubmitFrame() override;

  // The D3D11 immediate context is owned by the video thread. Running xrEndFrame
  // from the detached pacing thread can race swapchain rendering and shutdown.
  bool SupportsDetachedFrameLoop() const override { return false; }

  uint32_t GetEyeWidth() const override { return m_eye_swapchains[0].width; }
  uint32_t GetEyeHeight() const override { return m_eye_swapchains[0].height; }

  const XREyeSwapchain& GetEyeSwapchain(uint32_t eye) const { return m_eye_swapchains[eye]; }

private:
  // Creates XrSession with XrGraphicsBindingD3D11KHR.
  bool CreateSessionD3D11();

  // Allocates m_eye_swapchains and wraps images as DXTexture / DXFramebuffer.
  bool CreateSwapchains();

  void DestroySwapchains();

  std::array<XREyeSwapchain, 2> m_eye_swapchains{};
  D3DPrimedGunOverlay m_primedgun_overlay{};

  // Image index selected by xrAcquireSwapchainImage for the current frame.
  std::array<uint32_t, 2> m_acquired_image_index{0, 0};
  std::array<bool, 2> m_image_acquired{false, false};

  // Reused per-frame composition data (avoids per-frame heap allocation).
  std::array<XrCompositionLayerProjectionView, 2> m_projection_views{};
  XrCompositionLayerProjection m_projection_layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  XrCompositionLayerQuad m_cinematic_screen_layer{XR_TYPE_COMPOSITION_LAYER_QUAD};

};

// Global D3D11 OpenXR instance — valid between VideoBackend::Initialize() and Shutdown().
extern std::unique_ptr<D3DOpenXR> g_openxr_d3d;

}  // namespace DX11

#endif  // ENABLE_VR
