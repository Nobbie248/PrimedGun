// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef ENABLE_VR

#include <atomic>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// VulkanLoader.h must come first — it defines VK_NO_PROTOTYPES before vulkan.h.
#include "VideoBackends/Vulkan/VulkanLoader.h"

#define XR_USE_GRAPHICS_API_VULKAN
#if defined(ANDROID)
#include <jni.h>
#define XR_USE_PLATFORM_ANDROID
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "VideoCommon/VR/OpenXRManager.h"

namespace Vulkan
{
class VKTexture;
class VKFramebuffer;

// Holds the swapchain images for one eye.
// The VkImage objects are owned by the OpenXR runtime; VKTexture wraps them
// (without allocation) and VKFramebuffer provides a render pass for rendering.
struct XRVkEyeSwapchain
{
  XRVkEyeSwapchain();
  ~XRVkEyeSwapchain();
  XRVkEyeSwapchain(XRVkEyeSwapchain&&) noexcept;
  XRVkEyeSwapchain& operator=(XRVkEyeSwapchain&&) noexcept;

  XRVkEyeSwapchain(const XRVkEyeSwapchain&) = delete;
  XRVkEyeSwapchain& operator=(const XRVkEyeSwapchain&) = delete;

  XrSwapchain swapchain = XR_NULL_HANDLE;
  uint32_t width = 0;
  uint32_t height = 0;

  // One entry per swapchain image.
  std::vector<std::unique_ptr<VKTexture>> textures;
  std::vector<std::unique_ptr<VKFramebuffer>> framebuffers;
};

struct XRVkLayeredSwapchain
{
  XRVkLayeredSwapchain();
  ~XRVkLayeredSwapchain();
  XRVkLayeredSwapchain(XRVkLayeredSwapchain&&) noexcept;
  XRVkLayeredSwapchain& operator=(XRVkLayeredSwapchain&&) noexcept;

  XRVkLayeredSwapchain(const XRVkLayeredSwapchain&) = delete;
  XRVkLayeredSwapchain& operator=(const XRVkLayeredSwapchain&) = delete;

  XrSwapchain swapchain = XR_NULL_HANDLE;
  uint32_t width = 0;
  uint32_t height = 0;

  std::vector<std::unique_ptr<VKTexture>> textures;
  std::vector<std::unique_ptr<VKFramebuffer>> framebuffers;
};

struct XRPrimedGunVkOverlaySwapchain
{
  XRPrimedGunVkOverlaySwapchain();
  ~XRPrimedGunVkOverlaySwapchain();
  XRPrimedGunVkOverlaySwapchain(XRPrimedGunVkOverlaySwapchain&&) noexcept;
  XRPrimedGunVkOverlaySwapchain& operator=(XRPrimedGunVkOverlaySwapchain&&) noexcept;

  XRPrimedGunVkOverlaySwapchain(const XRPrimedGunVkOverlaySwapchain&) = delete;
  XRPrimedGunVkOverlaySwapchain& operator=(const XRPrimedGunVkOverlaySwapchain&) = delete;

  XrSwapchain swapchain = XR_NULL_HANDLE;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t content_kind = 0;
  uint32_t generation = 0;
  bool texture_ready = false;
  std::vector<XrSwapchainImageVulkanKHR> images;
  std::vector<std::unique_ptr<VKTexture>> textures;
};

struct XRPrimedGunVkLaserSwapchain
{
  XRPrimedGunVkLaserSwapchain();
  ~XRPrimedGunVkLaserSwapchain();
  XRPrimedGunVkLaserSwapchain(XRPrimedGunVkLaserSwapchain&&) noexcept;
  XRPrimedGunVkLaserSwapchain& operator=(XRPrimedGunVkLaserSwapchain&&) noexcept;

  XRPrimedGunVkLaserSwapchain(const XRPrimedGunVkLaserSwapchain&) = delete;
  XRPrimedGunVkLaserSwapchain& operator=(const XRPrimedGunVkLaserSwapchain&) = delete;

  XrSwapchain swapchain = XR_NULL_HANDLE;
  bool texture_ready = false;
  std::vector<XrSwapchainImageVulkanKHR> images;
  std::vector<std::unique_ptr<VKTexture>> textures;
};

// Vulkan-specific OpenXR backend. Implements VR::IOpenXRSwapchain so that
// Presenter::RenderXFBToScreen() can acquire/release eye images and submit
// frames using only VideoCommon-visible types (AbstractFramebuffer*).
// Vulkan extensions required by the OpenXR runtime, queried before VkInstance/VkDevice creation.
struct VulkanExtensionRequirements
{
  std::vector<std::string> instance_extensions;
  std::vector<std::string> device_extensions;
  u32 max_api_version = 0;
};

class VulkanOpenXR : public VR::IOpenXRSwapchain
{
public:
  VulkanOpenXR();
  ~VulkanOpenXR() override;

  VulkanOpenXR(const VulkanOpenXR&) = delete;
  VulkanOpenXR& operator=(const VulkanOpenXR&) = delete;

  // Must be called BEFORE VulkanContext::CreateVulkanInstance().
  // Creates a temporary XrInstance, queries the Vulkan extensions required by the
  // OpenXR runtime, and stores the OpenXRManager in VR::g_openxr for later reuse.
  static bool PreQueryVulkanExtensions(VulkanExtensionRequirements& out_requirements);

  // Full initialization: creates Vulkan-bound XrSession, reference space, and
  // per-eye swapchains. If PreQueryVulkanExtensions() was called, reuses the
  // existing VR::g_openxr; otherwise creates a new one.
  bool Initialize();

  // Tears down swapchains and resets g_openxr.
  void Shutdown();

  // ---- IOpenXRSwapchain ----

  // Acquire the next swapchain image for the given eye.
  // Returns AbstractFramebuffer* (actually VKFramebuffer*) to render into.
  AbstractFramebuffer* AcquireEyeFramebuffer(uint32_t eye_index) override;

  // Release the current swapchain image back to the runtime.
  void ReleaseEyeTexture(uint32_t eye_index) override;

  bool SupportsLayeredRendering() const override { return m_use_layered_swapchain; }
  AbstractFramebuffer* AcquireLayeredFramebuffer() override;
  void ReleaseLayeredTexture() override;
  std::unique_lock<std::mutex> AcquireGraphicsQueueLock() override;
  bool WaitForPendingFrameFinalization(std::string_view reason = {}) override;

  // Build the XrCompositionLayerProjection and call xrEndFrame.
  bool SubmitFrame() override;
  bool SupportsDetachedFrameLoop() const override { return false; }

  // PrimeFrame pacing (PRIMEFRAME_PACING=pipelined|inline|drain, see frame/PACING.md).
  bool UsesPipelinedPacing() const override { return m_pacing == Pacing::Pipelined; }
  bool UsesLazyFrameStart() const override { return m_pacing == Pacing::Cycle && m_lazy_start; }
  bool UsesCycleOverlap() const override { return m_pacing == Pacing::Cycle && m_cycle_overlap; }
  bool CycleBeforeWait() override;
  bool CycleBeginNowAfterWait() override;
  bool SkipsWindowPresent() const override { return m_skip_window_present; }
  bool LocateNextFramePose() override;

  uint32_t GetEyeWidth() const override
  {
    return m_use_layered_swapchain ? m_layered_swapchain.width : m_eye_swapchains[0].width;
  }
  uint32_t GetEyeHeight() const override
  {
    return m_use_layered_swapchain ? m_layered_swapchain.height : m_eye_swapchains[0].height;
  }

  const XRVkEyeSwapchain& GetEyeSwapchain(uint32_t eye) const { return m_eye_swapchains[eye]; }

private:
  enum class Pacing
  {
    Pipelined,  // two eye sets; a pacing thread releases each set after its fence and ends it
    Inline,     // one set; fence wait right before the release, same frame
    Drain,      // one set; the submit waits for the GPU (PRIMEDGUN_XR_WAIT_GPU=1)
    Cycle,      // one set; each game frame in its own xrWaitFrame cycle, begun at its first
                // pose read; a releaser thread releases it after its fence and ends it
  };

  // One eye set handed from the video thread to the pacing thread.
  struct XRFrameItem
  {
    uint64_t id = 0;
    uint32_t set = 0;
    XrTime target = 0;  // the display time its pose was located for
    std::array<bool, 2> eye_acquired{};
    bool released = false;  // PRIMEFRAME_NO_GPU_WAIT: released by the video thread at once
    bool views_valid = false;
    bool cinematic = false;
    XrCompositionLayerFlags layer_flags = 0;
    std::array<XrCompositionLayerProjectionView, 2> views{};
    XrCompositionLayerQuad cinematic_layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    std::vector<XrCompositionLayerQuad> quads;
    VkFence fence = VK_NULL_HANDLE;
    bool complete = false;
    int64_t submit_ns = 0;
    XrTime pose_time = 0;       // the display time its poses were located for
    bool should_render = true;  // cycle: the frame's xrWaitFrame said so
    int plan_slots = 0;  // cycle: refreshes it is planned to land late (held past the on-time limit)
    uint64_t wait_seq = 0;  // cycle: its xrWaitFrame's number (OpenXRManager::FrameClock)
    // Cycle timing (XRC line): the frame's xrWaitFrame return, how long the video thread waited
    // for the last frame's end at this frame's start, its CPU and wall time since the last submit.
    int64_t wake_ns = 0;
    int64_t video_wait_ns = 0;
    int64_t video_cpu_ns = 0;
    int64_t video_wall_ns = 0;
    int64_t video_fence_wait_ns = 0;
  };

  std::array<XRVkEyeSwapchain, 2>& EyeSet(uint32_t set)
  {
    return set == 0 ? m_eye_swapchains : m_eye_swapchains_b;
  }
  VkFence TakeFence();
  bool SetBusyLocked(uint32_t set) const;
  void ReturnFence(VkFence fence);
  bool SubmitFramePipelined();
  bool SubmitFrameCycle();
  void CycleLoop();
  bool ReleaseItem(XRFrameItem& item, bool wait_gpu);
  void PacerLoop();
  void ReleaseFinishedEarly(int shown_set);
  void StartPacer();
  void StopPacer();
  void ReleaseCompletedItems(bool block);
  void NoteLayerEnd(bool fresh, bool late);
  void NoteGpuWait(uint64_t wait_us, bool waited);
  void EmitStats();

  struct PendingXRFrame
  {
    XrTime display_time = 0;
    XrEnvironmentBlendMode environment_blend_mode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    bool should_render = false;
    XrSpace space = XR_NULL_HANDLE;
    XrCompositionLayerFlags layer_flags = 0;
    std::array<XrCompositionLayerProjectionView, 2> projection_views{};
    uint64_t debug_frame_id = 0;
    uint64_t queued_time_us = 0;

    bool layered_acquired = false;
    XrSwapchain layered_swapchain = XR_NULL_HANDLE;
    std::array<bool, 2> eye_acquired{};
    std::array<XrSwapchain, 2> eye_swapchains{XR_NULL_HANDLE, XR_NULL_HANDLE};
  };

  // Creates XrSession with XrGraphicsBindingVulkanKHR.
  bool CreateSessionVulkan();

  // Allocates m_eye_swapchains and wraps images as VKTexture / VKFramebuffer.
  bool CreateSwapchains();
  bool CreateLayeredSwapchain(int64_t swapchain_format);
  bool CreateEyeSwapchains(int64_t swapchain_format);

  void DestroySwapchains();
  bool EnsurePrimedGunOverlaySwapchain(XRPrimedGunVkOverlaySwapchain* overlay,
                                      uint32_t content_kind, uint32_t generation, uint32_t width,
                                      uint32_t height, const std::vector<uint32_t>& pixels);
  void DestroyPrimedGunOverlaySwapchain(XRPrimedGunVkOverlaySwapchain* overlay);
  bool EnsurePrimedGunLaserSwapchain();
  void DestroyPrimedGunLaserSwapchain();
  bool AppendPrimedGunOverlayLayers(std::vector<XrCompositionLayerBaseHeader*>* layers);
  void FinalizePendingXRFrame(PendingXRFrame frame);

  std::array<XRVkEyeSwapchain, 2> m_eye_swapchains{};
  // Second eye set (pipelined pacing: one waited, unreleased image per swapchain).
  std::array<XRVkEyeSwapchain, 2> m_eye_swapchains_b{};
  uint32_t m_set_count = 1;

  Pacing m_pacing = Pacing::Pipelined;
  bool m_no_gpu_wait = false;
  bool m_skip_window_present = true;
  // PRIMEFRAME_REPEATS=1: end a frame every refresh, repeating the last eye set between new ones.
  bool m_repeat_layers = false;
  // Cycle: PRIMEFRAME_CYCLE_LAZY=0 begins the frame at the end of the last present (as the drain).
  bool m_lazy_start = true;
  bool m_cycle_busy = false;  // the releaser thread is ending a frame (under m_pacer_mutex)
  // Cycle overlap (default; PRIMEFRAME_CYCLE_OVERLAP=0 turns it off): the next frame's
  // xrWaitFrame does not wait for the last frame's end; the releaser thread begins the waited
  // frame right after that end (m_cycle_begin_pending, under m_pacer_mutex).
  bool m_cycle_overlap = false;
  bool m_cycle_begin_pending = false;
  // Cycle end (PRIMEFRAME_CYCLE_END=auto|asap|late, with the overlap; releaser thread only):
  // whether a finished frame is ended at once or held until a fixed time after its wake-up, so
  // that every frame of a room whose frames end near SteamVR's on-time limit is shown a period
  // later (see CycleEndHold).
  int m_end_cfg = 2;   // 0 asap, 1 late, 2 auto
  int m_end_mode = 0;  // 0 asap (no overlap: each xrWaitFrame blocks for the wake-up), 1 late
  double m_end_ontime_ms = 19.5;
  double m_end_earliest_ms = 0.0;
  int64_t m_end_since_ns = 0;
  int64_t m_end_dwell_ns = 4'000'000'000LL;
  std::array<float, 600> m_end_ready{};  // the last frames' finish after their wake-up (ms)
  // Late -> asap (CycleEndHold), the last frames' (ms): the video thread's CPU time plus a GPU
  // tail allowance; when after SteamVR's wake-up they would finish had their xrWaitFrame blocked.
  std::array<float, 600> m_end_vcpu{};
  std::array<float, 600> m_end_est{};
  size_t m_end_ready_next = 0;
  size_t m_end_ready_count = 0;
  int64_t CycleEndHold(const XRFrameItem& item, int64_t fence_ns);
  // Cycle timing, video thread: its wait for the last frame's end in this frame, its CPU clock
  // at the last submit.
  int64_t m_cycle_video_wait_ns = 0;
  int64_t m_cycle_last_cpu_ns = 0;
  uint64_t m_cycle_last_fence_wait_us = 0;
  // Pipelined: release a set's images as soon as its fence signalled, before its refresh
  // (PRIMEFRAME_EARLY_RELEASE=0: at its refresh); the margin the pose target leaves before the
  // runtime's wake-up for it (PRIMEFRAME_PACE_MARGIN_MS, default 3).
  bool m_early_release = true;
  double m_pace_margin_ms = 3.0;

  // Video thread side of the pipelined pacing.
  uint32_t m_next_set = 0;
  uint32_t m_frame_set = 0;
  bool m_frame_set_chosen = false;
  uint64_t m_frame_id = 0;
  XrTime m_target_time = 0;
  XrTime m_last_target = 0;
  int64_t m_locate_ns = 0;
  int64_t m_last_submit_ns = 0;
  double m_span_ema_ms = 0.0;
  double m_interval_ema_ms = 0.0;
  int m_slack_frames = 0;

  // Shared with the pacing thread.
  std::mutex m_pacer_mutex;
  std::condition_variable m_pacer_cv;
  std::deque<XRFrameItem> m_pending;
  bool m_video_waiting_for_set = false;
  std::vector<VkFence> m_free_fences;
  std::vector<VkFence> m_all_fences;
  std::thread m_pacer;
  std::atomic<bool> m_pacer_exit{false};
  std::atomic<double> m_gpu_tail_ema_ms{2.0};
  // Pipelined: the smallest spacing of the display times the runtime handed out (its slot; the
  // display period until measured) and the last one handed out.
  std::atomic<int64_t> m_slot_ns{0};
  XrTime m_last_handed = 0;

  // Per-second statistics (XRSTATS), written by either thread.
  std::mutex m_stats_mutex;
  struct Stats
  {
    uint64_t window_start_us = 0;
    uint32_t fresh = 0, layers = 0, late = 0, gpu_waits = 0, set_waits = 0, discards = 0;
    uint64_t gpu_wait_max_us = 0, drain_sum_us = 0, drain_max_us = 0;
    uint64_t gpu_busy_ns = 0;
  } m_stats;
  XRVkLayeredSwapchain m_layered_swapchain{};
  XRPrimedGunVkOverlaySwapchain m_primedgun_overlay_swapchain{};
  XRPrimedGunVkOverlaySwapchain m_primedgun_position_marker_swapchain{};
  XRPrimedGunVkLaserSwapchain m_primedgun_laser_swapchain{};

  // Image index selected by xrAcquireSwapchainImage for the current frame.
  std::array<uint32_t, 2> m_acquired_image_index{0, 0};
  std::array<bool, 2> m_image_acquired{false, false};
  uint32_t m_acquired_layered_image_index = 0;
  bool m_layered_image_acquired = false;
  bool m_use_layered_swapchain = false;
  bool m_frame_uses_layered_swapchain = false;

  // Reused per-frame composition data (avoids per-frame heap allocation).
  std::array<XrCompositionLayerProjectionView, 2> m_projection_views{};
  XrCompositionLayerProjection m_projection_layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  XrCompositionLayerQuad m_cinematic_screen_layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
  XrCompositionLayerQuad m_primedgun_overlay_layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
  XrCompositionLayerQuad m_primedgun_position_marker_layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
  XrCompositionLayerQuad m_primedgun_laser_layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
  XrCompositionLayerQuad m_primedgun_laser_hit_layer{XR_TYPE_COMPOSITION_LAYER_QUAD};

  std::atomic<bool> m_async_frame_finalization_in_flight{false};
  std::atomic<bool> m_async_frame_finalization_failed{false};
};

// Global Vulkan OpenXR instance — valid between VideoBackend::Initialize() and Shutdown().
extern std::unique_ptr<VulkanOpenXR> g_openxr_vk;

}  // namespace Vulkan

#endif  // ENABLE_VR
