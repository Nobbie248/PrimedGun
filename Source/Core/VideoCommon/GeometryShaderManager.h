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

  GeometryShaderConstants constants{};
  bool dirty = false;

  // Per-draw VR stereo mode override (set before RenderDrawCall, consumed in SetConstants).
  // NaN = no override; -2.0 = force head-locked; -1.0 = force screen;
  // 0.0 = force fullscreen; 0.25 = force fullscreen mono; 1.0 = force perspective.
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

  // Cached OpenXR head pose data — refreshed only at frame boundaries (XFB copy)
  // when vr_lock_head_pose is enabled, otherwise refreshed every SetConstants call.
  std::array<std::array<float, 4>, 4> m_cached_eye_projection{};
  std::array<std::array<float, 4>, 2> m_cached_eye_z_row{};
  std::array<std::array<float, 4>, 4> m_cached_head_projection{};
  bool m_vr_pose_needs_refresh = true;
};
