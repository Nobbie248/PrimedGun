// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef ENABLE_VR

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <openxr/openxr.h>

#include "Common/Matrix.h"
#include "VideoCommon/AbstractFramebuffer.h"

// OpenXRManager owns the XrInstance, XrSystemId, XrSession, and reference XrSpace.
// It handles the OpenXR session state machine and per-frame timing.
//
// Backend-specific swapchain creation is handled by the respective backend class
// (D3DOpenXR for D3D11, VulkanOpenXR for Vulkan), which creates the XrSession
// with a graphics binding and then calls SetSession() + SetSwapchain() to
// transfer ownership here.

namespace VR
{
// Per-eye pose and field-of-view returned by xrLocateViews.
struct XREyeView
{
  XrPosef pose;
  XrFovf fov;
};

// Horizon thread-scheduling class for RegisterCurrentAndroidThread (maps to XrAndroidThreadTypeKHR
// on Android). Defined unconditionally so the cross-platform method signature compiles everywhere.
enum class AndroidThreadType
{
  ApplicationMain,  // the Gekko/emulation (CPU-bound) thread
  RendererMain,     // the render/submit thread
};

// Abstract interface for backend-specific per-eye swapchain management.
// Implemented by D3DOpenXR (D3D11) and VulkanOpenXR (Vulkan).
// Used by Presenter::RenderXFBToScreen() to blit game frames into the HMD's
// eye textures without depending on backend-specific types.
class IOpenXRSwapchain
{
public:
  virtual ~IOpenXRSwapchain() = default;

  // Acquire the next swapchain image for the given eye (0 = left, 1 = right).
  // Returns an AbstractFramebuffer* to render into, or nullptr on error.
  // Must be paired with ReleaseEyeTexture() after rendering is complete.
  virtual AbstractFramebuffer* AcquireEyeFramebuffer(uint32_t eye) = 0;

  // Signal the runtime that rendering into the given eye's current image is done.
  virtual void ReleaseEyeTexture(uint32_t eye) = 0;

  // Build the XrCompositionLayerProjection from the current eye poses and submit
  // via xrEndFrame. Call after both eyes have been rendered and released.
  virtual bool SubmitFrame() = 0;

  // Pixel dimensions of the eye render targets (typically HMD recommended resolution).
  virtual uint32_t GetEyeWidth() const = 0;
  virtual uint32_t GetEyeHeight() const = 0;

  virtual bool SupportsLayeredRendering() const { return false; }
  virtual AbstractFramebuffer* AcquireLayeredFramebuffer() { return nullptr; }
  virtual void ReleaseLayeredTexture() {}
  virtual std::unique_lock<std::mutex> AcquireGraphicsQueueLock() { return {}; }
  virtual bool WaitForPendingFrameFinalization(std::string_view reason = {}) { return true; }
};

class OpenXRManager
{
public:
  OpenXRManager();
  ~OpenXRManager();

  OpenXRManager(const OpenXRManager&) = delete;
  OpenXRManager& operator=(const OpenXRManager&) = delete;

  static bool IsRuntimeExtensionSupported(const char* extension_name);
  static std::vector<const char*> GetAvailableControllerExtensions();

  // Step 1: Create XrInstance.
  // extra_extensions must include the graphics API extension (e.g. XR_KHR_D3D11_ENABLE_EXTENSION_NAME).
  // platform_instance_create_next is an optional struct chained into XrInstanceCreateInfo::next
  // (used on Android to supply XrInstanceCreateInfoAndroidKHR).
  bool CreateInstance(const std::vector<const char*>& extra_extensions = {},
                      const void* platform_instance_create_next = nullptr);

  // Step 2: Locate the HMD system.
  bool InitializeSystem();

  // Step 2b: Query recommended per-eye render resolution.
  // Must be called before the backend creates swapchains.
  bool EnumerateViewConfigurations();

  // Step 3 (called by backend after creating session with graphics binding).
  void SetSession(XrSession session);

  // Step 3b: Register the backend swapchain implementation for use by the presenter.
  // Raw pointer — the backend object outlives the manager.
  void SetSwapchain(IOpenXRSwapchain* swapchain) { m_swapchain = swapchain; }
  IOpenXRSwapchain* GetSwapchain() const { return m_swapchain; }

  // Step 4: Create the local reference space used for head tracking.
  bool CreateReferenceSpace();

  // ---- Per-frame interface ----

  // Poll XrEvents and drive the session state machine.
  // Returns false if the render loop should stop.
  bool PollEvents();

  // xrWaitFrame — blocks until the runtime wants a new frame rendered.
  // Call at the start of each rendered frame.
  bool WaitFrame();

  // xrBeginFrame — signals the runtime that rendering has started.
  bool BeginFrame();

  // xrEndFrame — submits the completed layer stack to the compositor.
  bool EndFrame(const std::vector<XrCompositionLayerBaseHeader*>& layers);
  bool EndFrameDetached(XrTime display_time, XrEnvironmentBlendMode environment_blend_mode,
                        bool should_render,
                        const std::vector<XrCompositionLayerBaseHeader*>& layers);

  // xrLocateViews — fills m_eye_views with the predicted head pose for each eye.
  // Call between BeginFrame and rendering.
  bool LocateViews();
  void RecordRenderedEyeViews();

  // ---- Accessors ----

  XrInstance GetInstance() const { return m_instance; }
  XrSystemId GetSystemId() const { return m_system_id; }
  XrSession GetSession() const { return m_session; }
  XrSpace GetReferenceSpace() const { return m_reference_space; }

  XrTime GetPredictedDisplayTime() const { return m_frame_state.predictedDisplayTime; }
  bool IsSessionRunning() const { return m_session_running; }
  bool ShouldRender() const { return m_frame_state.shouldRender == XR_TRUE; }
  bool IsExtensionEnabled(const char* extension_name) const;
  bool ShouldUseVulkanLegacyProjectionFallback() const;
  bool IsQuestOrVirtualDesktopRuntime() const;
  XrEnvironmentBlendMode GetActiveBlendMode() const { return m_active_blend_mode; }
  double GetNativeDisplayPeriodMs() const { return m_native_display_period_ms; }
  float GetStartupDisplayRefreshRateHz() const
  {
    return m_native_display_period_ms > 0.0 ? static_cast<float>(1000.0 / m_native_display_period_ms) :
                                              0.0f;
  }

  const std::array<XREyeView, 2>& GetEyeViews() const { return m_eye_views; }
  const std::array<XREyeView, 2>& GetSubmittedEyeViews() const { return m_submitted_eye_views; }
  bool AreEyeViewsValid() const { return m_eye_views_valid; }
  bool AreSubmittedEyeViewsValid() const { return m_submitted_eye_views_valid; }
  const std::array<XrViewConfigurationView, 2>& GetViewConfigViews() const
  {
    return m_view_config_views;
  }

  // Compute per-eye projection rows with head rotation and eye position baked in.
  //
  // out_proj_rows[0..1] = left-eye rows 0,1; [2..3] = right-eye rows 0,1.
  //   x_clip = dot(row0, viewPos),  y_clip = dot(row1, viewPos)
  //   Head rotation and eye position (scaled by units_per_meter) are baked in.
  //
  // out_z_rows[0..1] = left/right eye z-axis row.
  //   z_eye = dot(z_row, viewPos) gives eye-space depth for perspective divide
  //   (f.pos.w = -z_eye) and depth buffer recomputation.
  void GetEyeProjectionRows(
      float units_per_meter,
      std::array<std::array<float, 4>, 4>& out_proj_rows,
      std::array<std::array<float, 4>, 2>& out_z_rows) const;

  // Compute per-eye projection rows WITHOUT head rotation (for head-locked content).
  // Same layout as GetEyeProjectionRows but only includes the raw asymmetric frustum
  // projection and per-eye IPD offset. Content rendered with these rows follows
  // the user's head movements.
  void GetRawEyeProjectionRows(
      float units_per_meter,
      std::array<std::array<float, 4>, 4>& out_proj_rows) const;
  bool GetLegacyViewMatrix(float units_per_meter, Common::Matrix44* out_view_matrix) const;
  void GetLegacyProjectionAdjustments(
      float units_per_meter, float game_projection_x_scale, float game_projection_x_offset,
      float game_projection_y_scale, float game_projection_y_offset,
      std::array<std::array<float, 4>, 2>& out_x_rows,
      std::array<std::array<float, 4>, 2>& out_y_rows) const;
  // Declare the calling thread to the Horizon scheduler via XR_KHR_android_thread_settings so it
  // can pin the render/submit and emulation threads to the big (Prime/Gold) cores -- and is then
  // free to schedule the unmarked async-shader compile workers onto the little cores. No-op off
  // Android or when the extension wasn't enabled. See quest-gpu-shader-plan.md Step 5 / audit #3.
  bool RegisterCurrentAndroidThread(AndroidThreadType thread_type, const char* thread_name);

  // Apply a fixed-foveation profile (XR_FB_foveation) to a backend eye/layered swapchain so the
  // runtime shades the periphery at a lower rate. Level comes from GFX_VR_FOVEATION_LEVEL. No-op
  // (returns false) unless the FB foveation extensions were enabled (Quest) and the level is above
  // Off. Call once per swapchain after creation, passing the backend's XrSwapchain handle.
  bool ApplyFoveationProfile(XrSwapchain swapchain);

  // Request a height-only recenter of the VR home position.
  // Applied on the OpenXR render thread during LocateViews.
  void RequestRecenter();

  // Request a full recenter: move the player back to the play-space centre, face them forward
  // (yaw) and reset eye height. Fired in response to the system "Reset View" (the Meta-button
  // hold sends XrEventDataReferenceSpaceChangePending). Applied on the OpenXR render thread at
  // the top of LocateViews by recreating the active reference space at the levelled head pose.
  void RequestFullRecenter();

private:
  bool InitializeInputActions();
  void DestroyInputActions();
  void UpdateInputActions();
  void UpdateHaptics();
  void ResetInputActionsState();

  void HandleSessionStateChange(XrSessionState new_state);

  // Tell the runtime this is a heavy app so it schedules the CPU/GPU domains at high clocks
  // (XR_EXT_performance_settings). No-op unless the extension was enabled (Android/Quest only).
  void ApplyPerformanceLevelHints();

  // Request a fixed HMD panel refresh rate (XR_FB_display_refresh_rate) from GFX_VR_DISPLAY_REFRESH_
  // RATE: enumerate the runtime's supported rates and request the closest one to the configured
  // target (or leave it alone on Auto). Records the active rate into m_native_display_period_ms.
  // No-op unless the extension was enabled (Android/Quest). Requires a running session, so it's
  // called from HandleSessionStateChange right after xrBeginSession.
  void ApplyDisplayRefreshRate();

  // Recreate m_reference_space so its origin sits at the current head's levelled pose (play-space
  // X/Z + yaw, height left to m_home_position). Measures the head against the immutable
  // m_recenter_base_space and resets the software height home. Called on the render thread.
  void ApplyFullRecenter();

  XrInstance m_instance = XR_NULL_HANDLE;
  std::string m_runtime_name;
  XrSystemId m_system_id = XR_NULL_SYSTEM_ID;
  XrSession m_session = XR_NULL_HANDLE;
  XrSpace m_reference_space = XR_NULL_HANDLE;
  // Immutable reference space (same type/origin as the initial m_reference_space) used only to
  // measure the head pose when computing a full recenter, so repeated recenters stay absolute.
  XrSpace m_recenter_base_space = XR_NULL_HANDLE;
  // Previous active reference space, kept alive one recenter longer because in-flight composition
  // layers captured its handle; destroyed at the next recenter / teardown.
  XrSpace m_old_reference_space = XR_NULL_HANDLE;

  // Non-owning pointer; lifetime managed by the backend (D3DOpenXR).
  IOpenXRSwapchain* m_swapchain = nullptr;
  std::vector<std::string> m_enabled_extensions;

  XrSessionState m_session_state = XR_SESSION_STATE_UNKNOWN;
  bool m_session_running = false;
  bool m_exit_render_loop = false;

  XrFrameState m_frame_state{XR_TYPE_FRAME_STATE};
  XrEnvironmentBlendMode m_active_blend_mode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  double m_native_display_period_ms = 0.0;

  // OpenXR input action set used to expose VR controller input to Dolphin's
  // regular controller mapping UI.
  XrActionSet m_input_action_set = XR_NULL_HANDLE;
  std::array<XrPath, 2> m_input_hand_paths{XR_NULL_PATH, XR_NULL_PATH};
  XrAction m_action_primary_click = XR_NULL_HANDLE;
  XrAction m_action_secondary_click = XR_NULL_HANDLE;
  XrAction m_action_menu_click = XR_NULL_HANDLE;
  XrAction m_action_thumbstick_click = XR_NULL_HANDLE;
  XrAction m_action_trigger_click = XR_NULL_HANDLE;
  XrAction m_action_squeeze_click = XR_NULL_HANDLE;
  XrAction m_action_trigger_value = XR_NULL_HANDLE;
  XrAction m_action_squeeze_value = XR_NULL_HANDLE;
  XrAction m_action_thumbstick_x = XR_NULL_HANDLE;
  XrAction m_action_thumbstick_y = XR_NULL_HANDLE;
  XrAction m_action_aim_pose = XR_NULL_HANDLE;
  XrAction m_action_grip_pose = XR_NULL_HANDLE;
  XrAction m_action_haptic = XR_NULL_HANDLE;
  std::array<XrSpace, 2> m_aim_spaces{XR_NULL_HANDLE, XR_NULL_HANDLE};
  std::array<XrSpace, 2> m_grip_spaces{XR_NULL_HANDLE, XR_NULL_HANDLE};
  std::array<bool, 2> m_haptics_active{false, false};

  std::array<XrViewConfigurationView, 2> m_view_config_views{};
  std::array<XrView, 2> m_views{};
  std::array<XREyeView, 2> m_eye_views{};
  std::array<XREyeView, 2> m_submitted_eye_views{};
  bool m_eye_views_valid = false;
  bool m_submitted_eye_views_valid = false;

  // "Home" head-center position. With stage space this remains the runtime's play-space origin;
  // local-space fallback records the first usable head-center position.
  bool m_reference_space_is_stage = false;
  mutable bool m_home_set{false};
  mutable XrVector3f m_home_position{0.f, 0.f, 0.f};
  std::atomic<bool> m_recenter_requested{false};
  std::atomic<bool> m_full_recenter_requested{false};
};

// Global instance — created by the backend during VideoBackend::Initialize().
extern std::unique_ptr<OpenXRManager> g_openxr;

}  // namespace VR

#endif  // ENABLE_VR
