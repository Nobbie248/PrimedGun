// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cmath>
#include <limits>

#include "Common/CommonTypes.h"
#include "VideoCommon/ConstantManager.h"
#include "VideoCommon/MetroidElementClassifier.h"

class PointerWrap;
enum class PrimitiveType : u32;

// The non-API dependent parts.
class GeometryShaderManager
{
public:
  void Init();
  void Dirty();
  void DoState(PointerWrap& p);

  void SetConstants(PrimitiveType prim);
  void SetViewportChanged();
  void SetProjectionChanged();
  void SetLinePtWidthChanged();
  void SetTexCoordChanged(u8 texmapid);

  // VR head-pose cache: marks the cached eye projection as stale so the next
  // SetConstants() call re-fetches it from OpenXR.  Called from BPStructs at the
  // XFB-copy boundary so a single game frame's draws all see one consistent pose.
  void InvalidateVRHeadPose();
  // Projection for the VR head-cone CPU cull (four row-major float4 rows), rebuilt on the same
  // pose refresh as the eye projection entries so a draw is culled against the pose it is
  // rendered with. Returns nullptr when no OpenXR pose is available.
  const float* GetVrCullProjection(float cone_degrees);
  float GetVrCullEffectiveDegrees() const { return m_vr_cull_effective_degrees; }

  GeometryShaderConstants constants{};
  bool dirty = false;

  // Per-draw VR stereo mode override (set before RenderDrawCall, consumed in SetConstants).
  // NaN = no override; -3.0 = force head-locked perspective HUD; -2.0 = force head-locked;
  // -1.0 = force screen; 0.0 = force fullscreen; 0.25 = force fullscreen mono;
  // 1.0 = force perspective.
  float vr_stereo_override = std::numeric_limits<float>::quiet_NaN();

  // Per-frame counter for ortho/screen draws — used to spread depth and avoid Z-fighting.
  // Incremented in Flush() for each ortho draw, reset in OnEndFrame().
  int vr_ortho_draw_counter = 0;

  // Per-draw manual layer override from shader overrides (-1 = use auto counter).
  int vr_ortho_layer_override = -1;

  // Per-draw element depth override from shader overrides (-1 = use global setting).
  float vr_element_depth_override = -1.0f;

  // Per-draw UPM override from shader overrides (-1 = use global setting).
  float vr_units_per_meter_override = -1.0f;

  // Per-draw headlocked projection tuning for Hydra-style HUD layers.
  float vr_headlocked_projection_scale_x = 1.0f;
  float vr_headlocked_projection_scale_y = 1.0f;
  float vr_headlocked_projection_offset_x = 0.0f;
  float vr_headlocked_projection_offset_y = 0.0f;
  float vr_perspective_hud_distance_override = -1.0f;
  float vr_perspective_hud_size_override = -1.0f;

  // Set by VertexManagerBase for the current -3 (perspective HUD) draw.
  bool vr_metroid_hud_self_center = false;
  bool vr_metroid_hud_anchor_candidate = false;
  int vr_metroid_hud_reference_context = 0;

  // Per-draw Metroid HUD layer (set when a profile-classified draw matches a
  // perspective Screen/HeadLocked override).  GeometryShaderManager looks this up in
  // a Hydra-ported LUT to apply per-layer scale/width/height hacks to the eye
  // projection — gives 3D-looking helmet/visor/HUD instead of flat sticker.
  // Unknown = no layer-specific treatment.
  MetroidElementLayer vr_metroid_layer = MetroidElementLayer::Unknown;

private:
  void SetVSExpand(VSExpand expand);

  bool m_projection_changed = false;
  bool m_viewport_changed = false;

  // Cached OpenXR eye projection rows keyed by units-per-meter. Every entry is dropped at the
  // frame boundary (XFB copy) and, unless the head pose is locked per frame, whenever
  // LocateViews publishes new eye views. Draws that alternate between HUD and world scales
  // reuse an entry instead of redoing the projection trig on each draw.
  struct EyeProjectionEntry
  {
    bool valid = false;
    bool freelook_applied = false;
    float units_per_meter = 0.0f;
    std::array<std::array<float, 4>, 4> eye_projection{};
    std::array<std::array<float, 4>, 2> eye_z_row{};
    std::array<std::array<float, 4>, 4> head_projection{};
  };
  static constexpr size_t NUM_EYE_PROJECTION_ENTRIES = 4;
  std::array<EyeProjectionEntry, NUM_EYE_PROJECTION_ENTRIES> m_eye_projection_entries{};
  size_t m_eye_projection_next_entry = 0;
  int m_current_eye_projection_entry = -1;
  u64 m_cached_eye_views_generation = 0;
  bool m_vr_pose_needs_refresh = true;
  std::array<std::array<float, 4>, 4> m_vr_cull_projection{};
  float m_vr_cull_projection_degrees = 0.0f;
  float m_vr_cull_effective_degrees = 0.0f;
  bool m_vr_cull_projection_valid = false;

  void InvalidateEyeProjectionEntries();
  // Returns the entry index for this scale, computing the rows on a miss.
  int LookupEyeProjection(float units_per_meter, bool apply_freelook, bool record_rendered_views);

  // Shared reference depth for the headlocked perspective HUD (-3) path. Stable body layers choose
  // the next frame's anchor; all coherent draws in a frame reuse one anchor so they share one
  // transform and keep relative scene depth.
  float m_vr_hud_shared_reference_z = 0.0f;
  bool m_vr_hud_shared_reference_valid = false;
  int m_vr_hud_shared_reference_context = 0;
  float m_vr_hud_stable_reference_z = 0.0f;
  bool m_vr_hud_stable_reference_valid = false;
  int m_vr_hud_stable_reference_context = 0;
  float m_vr_hud_frame_anchor_candidate_z = 0.0f;
  bool m_vr_hud_frame_anchor_candidate_valid = false;
  int m_vr_hud_frame_anchor_candidate_context = 0;
};
