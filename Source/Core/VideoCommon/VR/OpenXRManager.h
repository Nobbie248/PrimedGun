// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef ENABLE_VR

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
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
  virtual XrSwapchain GetFlatSwapchain() const { return XR_NULL_HANDLE; }
  virtual bool SupportsDetachedFrameLoop() const { return true; }
  virtual std::unique_lock<std::mutex> AcquireGraphicsQueueLock() { return {}; }
  virtual bool WaitForPendingFrameFinalization(std::string_view reason = {}) { return true; }

  // PrimeFrame pacing (Vulkan). Pipelined: a pacing thread owns xrWaitFrame/xrBeginFrame/
  // xrEndFrame and the releases; the video thread only renders, hands each eye set over and
  // locates the pose for the next game frame (LocateNextFramePose). The desktop window present
  // is skipped while the session runs when SkipsWindowPresent() (nobody sees it in the headset).
  virtual bool UsesPipelinedPacing() const { return false; }
  virtual bool SkipsWindowPresent() const { return false; }
  virtual bool LocateNextFramePose() { return false; }
  // Cycle pacing: each game frame runs in its own xrWaitFrame cycle (as the drain), started at
  // the frame's first draw that needs the head pose (the presenter arms it, OpenXRManager starts
  // it), and a releaser thread ends it once the GPU has finished.
  virtual bool UsesLazyFrameStart() const { return false; }
  // Cycle overlap (default, PRIMEFRAME_CYCLE_OVERLAP=0 off; frame/PACING.md): the next frame's
  // xrWaitFrame does not wait for the last frame's end; its xrBeginFrame follows that end (on
  // the releaser thread when the end comes later). CycleBeforeWait blocks until the last waited
  // frame was begun; CycleBeginNowAfterWait says whether the caller begins the frame it just
  // waited for (nothing left to end) or the releaser thread does it after its end.
  virtual bool UsesCycleOverlap() const { return false; }
  virtual bool CycleBeforeWait() { return true; }
  virtual bool CycleBeginNowAfterWait() { return true; }
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
  bool CreateInstance(const std::vector<const char*>& extra_extensions = {});

  // Step 2: Locate the HMD system.
  bool InitializeSystem();

  // Step 2b: Query recommended per-eye render resolution.
  // Must be called before the backend creates swapchains.
  bool EnumerateViewConfigurations();

  // Step 3 (called by backend after creating session with graphics binding).
  void SetSession(XrSession session);

  // Step 3b: Register the backend swapchain implementation for use by the presenter.
  // Raw pointer — the backend object outlives the manager.
  void SetSwapchain(IOpenXRSwapchain* swapchain);
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
  // pose_time: the display time the projection layer's poses were located for (0: none or
  // unknown), for the per-frame timing.
  bool EndFrameDetached(XrTime display_time, XrEnvironmentBlendMode environment_blend_mode,
                        bool should_render,
                        const std::vector<XrCompositionLayerBaseHeader*>& layers,
                        XrTime pose_time = 0);

  // Dedicated OpenXR pacing loop. The pacing thread owns the frame protocol and
  // re-submits the most recently published layers while the game renders its next frame.
  void StartFrameThread();
  void StopFrameThread();
  bool IsFrameThreadActive() const
  {
    return m_frame_thread_running.load(std::memory_order_acquire);
  }
  void PublishFrame(const std::array<XrCompositionLayerProjectionView, 2>& views,
                    XrCompositionLayerFlags layer_flags);
  void PublishLayers(const std::vector<XrCompositionLayerBaseHeader*>& layers);

  // Prevent the pacing thread from submitting a newly released swapchain image
  // before the video thread has published the matching composition-layer pose.
  void BeginVideoFrameHandoff() { m_video_handoff_active.fetch_add(1, std::memory_order_release); }
  void EndVideoFrameHandoff() { m_video_handoff_active.fetch_sub(1, std::memory_order_release); }
  class ScopedVideoFrameHandoff
  {
  public:
    explicit ScopedVideoFrameHandoff(OpenXRManager* manager) : m_manager(manager)
    {
      if (m_manager)
        m_manager->BeginVideoFrameHandoff();
    }
    ~ScopedVideoFrameHandoff()
    {
      if (m_manager)
        m_manager->EndVideoFrameHandoff();
    }

    ScopedVideoFrameHandoff(const ScopedVideoFrameHandoff&) = delete;
    ScopedVideoFrameHandoff& operator=(const ScopedVideoFrameHandoff&) = delete;

  private:
    OpenXRManager* m_manager;
  };

  // xrLocateViews — fills m_eye_views with the predicted head pose for each eye.
  // Call between BeginFrame and rendering.
  bool LocateViews();
  // xrLocateViews for an explicit display time (the pipelined pacing picks the refresh).
  // extra_ns: how far past display_time the frame is predicted to be shown (the views are
  // located for display_time; the frame itself still ends with display_time - extra_ns).
  bool LocateViewsAt(XrTime display_time, int64_t extra_ns = 0);
  void RecordRenderedEyeViews();

  // Prediction for the real display time (PRIMEFRAME_PREDICT_EXTRA_MS=<ms>|auto, frame/PACING.md):
  // the head views and the controller poses of a frame are located that much after the
  // predicted display time its xrWaitFrame returned; xrEndFrame keeps that display time. Only
  // where the frame is located right after its own xrWaitFrame (cycle, drain, inline).
  void SetPredictExtraAllowed(bool allowed) { m_predict_allowed = allowed; }

  // Cycle pacing (frame/PACING.md): the presenter arms the next frame's start at the end of a
  // present; the first pose read of the next game frame on that thread (a draw) then waits for
  // the previous frame's end, calls xrWaitFrame and xrBeginFrame and locates the views for the
  // display time it got. TakeLazyFrameStarted() tells the presenter whether that happened.
  void ArmLazyFrameStart();
  void DisarmLazyFrameStart() { m_lazy_armed.store(false, std::memory_order_relaxed); }
  bool TakeLazyFrameStarted();
  // Cycle pacing: xrWaitFrame, xrBeginFrame (or, with the overlap, leave it to the releaser
  // thread), xrLocateViews when locate_views and the runtime wants the frame rendered.
  bool StartCycleFrame(bool locate_views);
  XrTime GetSubmittedLocateTime() const { return m_submitted_locate_time; }

  // Per-frame timing (all pacing modes), noted at every xrEndFrame: how long the frame's
  // xrWaitFrame blocked, the time from its return to the xrEndFrame, the age of the poses at the
  // end, and whether the poses were located for the display time the frame ends with. One
  // "XRF" line per frame unless PRIMEFRAME_FRAME_LOG=0; per-second sums for XRSTATS.
  struct FrameTimingStats
  {
    uint32_t frames = 0;           // xrEndFrame calls with layers
    uint32_t pose_mismatch = 0;    // poses located for another display time than the end's
    uint32_t ended_late = 0;       // ended more than PRIMEFRAME_ONTIME_MS after the wake-up
    double end_sum_ms = 0, end_max_ms = 0;  // xrWaitFrame return (or the grid wake) to end
    double age_sum_ms = 0, age_max_ms = 0;  // the poses' xrLocateViews call to end
    double lead_sum_ms = 0;                 // predicted display time minus xrWaitFrame return
    uint32_t age_count = 0;
    double extra_sum_ms = 0;  // how far past their display time the poses were located
    uint32_t extra_frames = 0;  // frames located past their display time
    uint32_t early_frames = 0;  // frames whose xrWaitFrame returned before the runtime's wake-up
    // Cycle landing plan (frame/PACING.md "One landing"): frames planned to land 0, 1, 2 or more
    // refreshes after their display time; frames that had to land a refresh later than the one
    // after the last frame's (an image shown twice); frames planned onto the last frame's refresh
    // after a hitch (catch-up: the last frame is not shown).
    std::array<uint32_t, 3> plan_slots{};
    uint32_t plan_breaks = 0;
    uint32_t plan_catchups = 0;
    uint32_t plan_drops = 0;  // frames landed on the last frame's refresh: holding them would cost frames
  };
  void TakeFrameTimingStats(FrameTimingStats* out);

  // The last xrWaitFrame: its predicted display time, the steady clock (ns) when it returned,
  // the display period. Lets another thread place a future refresh on the runtime's grid.
  struct FrameClock
  {
    XrTime display_time = 0;
    int64_t wake_ns = 0;
    int64_t period_ns = 0;
    // display_time minus the steady clock at the runtime's wake-up for it: the runtime's lead
    // (XrTime, from the last call that blocked; before one did, the largest seen) plus the
    // XrTime clock's offset to the steady clock now (SteamVR's XrTime is CLOCK_MONOTONIC_RAW).
    int64_t grid_offset_ns = 0;
    // The frame begun by the last xrWaitFrame: returned before the runtime's wake-up for its
    // display time (SteamVR hands out the next refresh early while a frame is still being
    // ended); the refreshes it is planned to land after its display time (cycle end late, see
    // SetCycleEndLate: held past the on-time limit, its poses located for the landing refresh).
    bool early = false;
    int plan_slots = 0;
    uint64_t wait_seq = 0;  // xrWaitFrame calls returned so far, this one included
  };
  FrameClock GetFrameClock() const
  {
    std::lock_guard<std::mutex> lock(m_clock_mutex);
    return m_clock;
  }
  // xrWaitFrame calls since the last call: count, total and longest blocking time (us).
  void TakeWaitFrameStats(uint32_t* count, uint64_t* total_us, uint64_t* max_us);
  // The runtime's lead (wake-up to display time, ms; exact once a call blocked) and the spacing of
  // the refreshes it hands out (ms; framesToThrottle 1 at 120 Hz: 16.67, whatever
  // predictedDisplayPeriod says).
  void GetWakeGrid(double* lead_ms, bool* lead_exact, double* slot_ms) const;
  // Cycle pacing (frame/PACING.md "One landing"): SetCycleEndLate(true) turns the landing plan on
  // (PRIMEFRAME_CYCLE_END=plan, the default, with the overlap): every frame is planned at its
  // xrWaitFrame to land on the refresh after the last frame's (whichever refresh SteamVR handed
  // it), later only when it cannot make that one, held past the on-time limit when it lands
  // after its display time, its poses located for the landing refresh. false: asap (lane L's
  // A/B: the next xrWaitFrame waits for the last frame's end). NoteCycleFinish: the frame's GPU
  // work finished (its xrWaitFrame return to its fence, for the expected finish of the next
  // frames; slack_ms: how long before the on-time limit of its landing refresh).
  void SetCycleEndLate(bool late) { m_cycle_end_late.store(late, std::memory_order_relaxed); }
  void NoteCycleFinish(uint64_t wait_seq, int64_t submit_ns, int64_t finish_ns, double slack_ms);
  // No exact lead yet: the next cycle frame start ends whatever is pending and, unless its
  // xrWaitFrame blocks, spends one empty frame so that the next one does (once every 2 s, at
  // most five times).
  bool WantsLeadCalibration() const;
  // Cycle end late, releaser thread: wait until an xrWaitFrame after the seq-th has returned (so
  // the next frame was handed its refresh while this one was pending) or the steady clock passes
  // deadline_ns. Returns whether it returned.
  bool WaitForWaitAfter(uint64_t seq, int64_t deadline_ns);

  // ---- Accessors ----

  XrInstance GetInstance() const { return m_instance; }
  XrSystemId GetSystemId() const { return m_system_id; }
  XrSession GetSession() const { return m_session; }
  XrSpace GetReferenceSpace() const { return m_reference_space; }
  // A VIEW reference space (head-locked), created at the first call; XR_NULL_HANDLE if the
  // runtime refuses it. For the PRIMEFRAME_HUD_TEST quad (frame/PACING.md "The HUD").
  XrSpace GetViewSpace();

  XrTime GetPredictedDisplayTime() const
  {
    return m_predicted_display_time_snapshot.load(std::memory_order_acquire);
  }
  bool IsSessionRunning() const { return m_session_running.load(std::memory_order_acquire); }
  bool ShouldRender() const { return m_should_render_snapshot.load(std::memory_order_acquire); }
  bool IsExtensionEnabled(const char* extension_name) const;
  bool ShouldUseVulkanLegacyProjectionFallback() const;
  bool IsQuestOrVirtualDesktopRuntime() const;
  XrEnvironmentBlendMode GetActiveBlendMode() const { return m_active_blend_mode; }
  XrCompositionLayerFlags GetProjectionLayerExtraFlags() const;
  double GetNativeDisplayPeriodMs() const
  {
    return m_native_display_period_ms.load(std::memory_order_acquire);
  }
  float GetStartupDisplayRefreshRateHz() const
  {
    const double period_ms = m_native_display_period_ms.load(std::memory_order_acquire);
    return period_ms > 0.0 ? static_cast<float>(1000.0 / period_ms) : 0.0f;
  }

  const std::array<XREyeView, 2>& GetEyeViews() const { return m_eye_views; }
  const std::array<XREyeView, 2>& GetSubmittedEyeViews() const { return m_submitted_eye_views; }
  void StampXFBPose(uint32_t xfb_addr);
  void SelectPresentPoseForXFB(uint32_t xfb_addr);

  // PrimeFrame pose bookkeeping (video thread). Every xrLocateViews is named by the display time
  // it was made for. The draws note the locate their constants came from; an XFB copy keeps the
  // oldest and newest of its draws with the XFB (StampXFBPose), and the present picks them up
  // with the XFB it shows (SelectPresentPoseForXFB).
  XrTime GetEyeViewsLocateTime() const { return m_eye_views_locate_time; }
  void NoteDrawPose(XrTime t)
  {
    if (t == 0)
      return;
    if (m_draw_pose_min == 0 || t < m_draw_pose_min)
      m_draw_pose_min = t;
    if (t > m_draw_pose_max)
      m_draw_pose_max = t;
  }
  // The pose of the geometry shader's cache (RecordRenderedEyeViews).
  void NoteCachedDrawPose() { NoteDrawPose(m_submitted_locate_time); }
  struct PresentPose
  {
    bool valid = false;        // the XFB shown was stamped at its copy
    XrTime locate_time = 0;    // the locate of its pose (the newest its draws used)
    XrTime draw_oldest = 0;    // the oldest and newest locate its draws used
    XrTime draw_newest = 0;
  };
  const PresentPose& GetPresentPose() const { return m_present_pose; }
  const std::array<XREyeView, 2>& GetPresentEyeViews() const
  {
    return m_present_eye_views_valid ? m_present_eye_views : m_submitted_eye_views;
  }
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
  // Symmetric perspective projection of `cone_degrees` on both axes that follows the head the
  // same way GetEyeProjectionRows does, padded to enclose both eye origins. The
  // cone never narrows below the rendered per-eye FOV plus a margin. Canted views are skipped.
  // Row-major, clip = M * viewPos with
  // w = -z_eye; only the x, y and w rows matter, the z row is eye-space depth. Used by the VR
  // head-cone CPU cull. Returns false until eye views and the home position exist.
  bool GetHeadCullProjection(float cone_degrees, float units_per_meter,
                             std::array<std::array<float, 4>, 4>* out_matrix,
                             float* out_effective_degrees = nullptr) const;
  bool GetLegacyViewMatrix(float units_per_meter, Common::Matrix44* out_view_matrix) const;
  void GetLegacyProjectionAdjustments(
      float units_per_meter, float game_projection_x_scale, float game_projection_x_offset,
      float game_projection_y_scale, float game_projection_y_offset,
      std::array<std::array<float, 4>, 2>& out_x_rows,
      std::array<std::array<float, 4>, 2>& out_y_rows) const;
  bool RegisterCurrentAndroidThread(const char* thread_name) { return true; }

  // Request a height-only recenter of the VR home position.
  // Applied on the OpenXR render thread during LocateViews.
  void RequestRecenter();

private:
  bool InitializeInputActions();
  void DestroyInputActions();
  void UpdateInputActions();
  void UpdateHaptics();
  void ResetInputActionsState();

  void HandleSessionStateChange(XrSessionState new_state);
  void FrameThreadLoop();
  // Cycle pacing: called by every pose read; starts the armed frame on the arming thread.
  void StartLazyFrameIfArmed() const;
  void NoteFrameEnd(XrTime display_time, XrTime pose_time, bool has_layers);
  void UpdateWakeGrid(uint64_t waited_us);

  struct PublishedXRLayer
  {
    enum class Kind : uint8_t
    {
      Projection,
      Quad
    };

    Kind kind = Kind::Projection;
    XrCompositionLayerFlags layer_flags = 0;
    XrSpace space = XR_NULL_HANDLE;
    std::array<XrCompositionLayerProjectionView, 2> projection_views{};
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
  };

  struct PublishedXRFrame
  {
    std::vector<PublishedXRLayer> layers;
  };

  XrInstance m_instance = XR_NULL_HANDLE;
  std::string m_runtime_name;
  XrSystemId m_system_id = XR_NULL_SYSTEM_ID;
  XrSession m_session = XR_NULL_HANDLE;
  XrSpace m_reference_space = XR_NULL_HANDLE;

  // Non-owning pointer; lifetime managed by the backend (D3DOpenXR).
  IOpenXRSwapchain* m_swapchain = nullptr;
  std::vector<std::string> m_enabled_extensions;

  XrSessionState m_session_state = XR_SESSION_STATE_UNKNOWN;
  std::atomic<bool> m_session_running{false};
  bool m_exit_render_loop = false;

  XrFrameState m_frame_state{XR_TYPE_FRAME_STATE};
  mutable std::mutex m_clock_mutex;
  FrameClock m_clock;
  std::atomic<uint32_t> m_wait_count{0};
  uint64_t m_wait_seq = 0;  // under m_wait_seq_mutex
  XrSpace m_view_space = XR_NULL_HANDLE;
  bool m_view_space_tried = false;
  std::atomic<uint64_t> m_begun_frames{0};
  std::atomic<uint64_t> m_ended_frames{0};
  std::mutex m_wait_seq_mutex;
  std::condition_variable m_wait_seq_cv;
  std::atomic<uint64_t> m_wait_total_us{0};
  std::atomic<uint64_t> m_wait_max_us{0};
  std::atomic<XrTime> m_predicted_display_time_snapshot{0};
  std::atomic<int64_t> m_predicted_display_period_snapshot{0};
  std::atomic<bool> m_should_render_snapshot{false};
  XrEnvironmentBlendMode m_active_blend_mode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  std::atomic<double> m_native_display_period_ms{0.0};

  std::thread m_frame_thread;
  std::atomic<bool> m_frame_thread_running{false};
  std::atomic<bool> m_frame_thread_should_exit{false};
  std::mutex m_publish_mutex;
  std::condition_variable m_publish_cv;
  PublishedXRFrame m_published_frame;
  uint64_t m_publish_serial = 0;
  std::atomic<int> m_video_handoff_active{0};

  // OpenXR input action set used to expose VR controller input to Dolphin's
  // regular controller mapping UI.
  XrActionSet m_input_action_set = XR_NULL_HANDLE;
  std::array<XrPath, 2> m_input_hand_paths{XR_NULL_PATH, XR_NULL_PATH};
  XrAction m_action_primary_click = XR_NULL_HANDLE;
  XrAction m_action_secondary_click = XR_NULL_HANDLE;
  XrAction m_action_menu_click = XR_NULL_HANDLE;
  XrAction m_action_thumbstick_click = XR_NULL_HANDLE;
  XrAction m_action_thumbrest_touch = XR_NULL_HANDLE;
  XrAction m_action_trigger_click = XR_NULL_HANDLE;
  XrAction m_action_squeeze_click = XR_NULL_HANDLE;
  XrAction m_action_trigger_value = XR_NULL_HANDLE;
  XrAction m_action_squeeze_value = XR_NULL_HANDLE;
  XrAction m_action_squeeze_force = XR_NULL_HANDLE;
  std::array<bool, 2> m_index_grip_pressed{};
  std::array<bool, 2> m_index_trackpad_pressed{};
  XrAction m_action_thumbstick_x = XR_NULL_HANDLE;
  XrAction m_action_thumbstick_y = XR_NULL_HANDLE;
  XrAction m_action_trackpad_click = XR_NULL_HANDLE;
  XrAction m_action_trackpad_touch = XR_NULL_HANDLE;
  XrAction m_action_trackpad_x = XR_NULL_HANDLE;
  XrAction m_action_trackpad_y = XR_NULL_HANDLE;
  XrAction m_action_trackpad_force = XR_NULL_HANDLE;
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

  struct XFBPoseStamp
  {
    uint32_t xfb_addr = 0;
    uint64_t serial = 0;
    std::array<XREyeView, 2> views{};
    XrTime locate_time = 0;
    XrTime draw_oldest = 0;
    XrTime draw_newest = 0;
  };
  std::array<XFBPoseStamp, 8> m_xfb_pose_stamps{};
  size_t m_xfb_pose_stamp_next = 0;
  uint64_t m_xfb_pose_stamp_serial = 0;
  std::array<XREyeView, 2> m_present_eye_views{};
  bool m_present_eye_views_valid = false;
  PresentPose m_present_pose{};
  XrTime m_eye_views_locate_time = 0;
  XrTime m_submitted_locate_time = 0;
  XrTime m_draw_pose_min = 0;
  XrTime m_draw_pose_max = 0;

  // Cycle pacing: the armed frame start (video thread).
  std::atomic<bool> m_lazy_armed{false};
  std::thread::id m_lazy_thread{};
  bool m_lazy_started = false;

  // Per-frame timing. The last xrWaitFrame (call, return, display time) and the recent
  // xrLocateViews calls (display time, call time) by any thread, under m_clock_mutex.
  int64_t m_wait_call_ns = 0;
  int64_t m_wait_return_ns = 0;
  XrTime m_wait_display_time = 0;
  // The last few xrWaitFrame calls (a frame may end after the next one's xrWaitFrame).
  struct WaitNote
  {
    XrTime display_time = 0;
    int64_t call_ns = 0;
    int64_t return_ns = 0;
    bool early = false;
    int plan_slots = 0;
    uint64_t seq = 0;  // the xrWaitFrame call's number (FrameClock::wait_seq)
  };
  std::array<WaitNote, 4> m_wait_notes{};
  size_t m_wait_note_next = 0;
  XrTime m_prev_end_display_time = 0;
  struct LocateNote
  {
    XrTime display_time = 0;
    int64_t call_ns = 0;
    int64_t extra_ns = 0;  // located this far past the frame's display time
  };
  std::array<LocateNote, 16> m_locate_notes{};
  size_t m_locate_note_next = 0;
  uint64_t m_frame_log_count = 0;
  FrameTimingStats m_frame_timing{};

  bool m_eye_views_valid = false;
  bool m_submitted_eye_views_valid = false;

  // Prediction for the real display time (SetPredictExtraAllowed). The extra of the frame begun
  // by the last xrWaitFrame; the xrWaitFrame return to finish time (average) of frames handed out
  // on time [0] and early [1]. Under m_clock_mutex, except m_frame_extra_ns (the thread calling
  // xrWaitFrame).
  bool m_predict_allowed = false;
  int64_t m_frame_extra_ns = 0;
  std::array<double, 2> m_finish_avg_ms{};
  bool m_cycle_finish_seen = false;
  std::atomic<bool> m_cycle_end_late{false};
  // Cycle landing plan (under m_clock_mutex): the refresh the last waited frame is planned to land
  // on; the last frames' xrWaitFrame return to finish (ms) and slack before the on-time limit of
  // their landing refresh (ms); frames since the landing last moved (a hitch, a refresh later or
  // earlier) and how many a catch-up needs (doubled when a refresh later follows one within 60 s).
  XrTime m_prev_land_time = 0;
  std::array<float, 120> m_finish_ring{};
  std::array<float, 120> m_submit_ring{};  // xrWaitFrame return to the frame's submit (ms)
  std::array<float, 1200> m_slack_ring{};
  size_t m_finish_next = 0, m_finish_count = 0, m_slack_next = 0;
  size_t m_plan_stable_frames = 0;
  size_t m_catchup_need_frames = 300;
  int64_t m_last_catchup_ns = 0;
  int64_t m_settle_until_ns = 0;  // catch-ups only until 30 s after the start, a hitch or a break
  uint32_t m_plan_breaks_total = 0, m_plan_catchups_total = 0;
  int64_t m_plan_log_ns = 0;

  // The runtime's wake-up grid (frame/PACING.md "The wake-up"), under m_clock_mutex. XrTime
  // clock: 0 not chosen, 1 CLOCK_MONOTONIC_RAW, 2 the steady clock (CLOCK_MONOTONIC); its offset
  // to the steady clock at the last xrWaitFrame return. The lead in XrTime: the largest of calls
  // that blocked for the wake-up (a call made later returns at once with a shorter lead, one
  // SteamVR hands out early with a longer one; see UpdateWakeGrid), else the largest seen. The refresh spacing:
  // the smallest step between consecutive display times that recurs.
  int m_xr_clock = 0;
  int64_t m_xr_offset_ns = 0;
  int64_t m_wake_lead_ns = 0;
  bool m_wake_lead_exact = false;
  int64_t m_lead_window_max_ns = 0;
  int64_t m_lead_window_start_ns = 0;
  int m_lead_window_n = 0;
  bool m_last_wait_blocked = false;
  XrTime m_prev_wait_display_time = 0;
  std::array<int64_t, 64> m_display_steps{};
  size_t m_display_step_next = 0;
  size_t m_display_step_count = 0;
  std::array<int64_t, 256> m_periods{};  // the last predictedDisplayPeriod values
  size_t m_period_next = 0;
  size_t m_period_count = 0;
  int64_t m_slot_ns = 0;
  int m_calib_attempts = 0;
  int64_t m_calib_last_ns = 0;

  // "Home" head-center position. With stage space this remains the runtime's play-space origin;
  // local-space fallback records the first usable head-center position.
  bool m_reference_space_is_stage = false;
  mutable bool m_home_set{false};
  mutable XrVector3f m_home_position{0.f, 0.f, 0.f};
  std::atomic<bool> m_recenter_requested{false};
};

// Global instance — created by the backend during VideoBackend::Initialize().
extern std::unique_ptr<OpenXRManager> g_openxr;

}  // namespace VR

#endif  // ENABLE_VR
