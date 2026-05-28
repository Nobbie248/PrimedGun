// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cmath>
#include <limits>

#include "Common/CommonTypes.h"
#include "VideoCommon/ConstantManager.h"

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

  GeometryShaderConstants constants{};
  bool dirty = false;

  // Per-draw VR stereo mode override (set before RenderDrawCall, consumed in SetConstants).
  // NaN = no override; -2.0 = head locked; -1.0 = screen; 0.0 = fullscreen; 1.0 = perspective.
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

  // Per-draw override for head-locked content: use full eye-FOV coverage sizing
  // instead of the configurable virtual-screen size.
  bool vr_head_locked_full_view_override = false;

  // Optional extra right-eye clip-space X offset for specific head-locked full-view draws.
  float vr_head_locked_right_eye_x_offset_override = 0.0f;

private:
  void SetVSExpand(VSExpand expand);

  bool m_projection_changed = false;
  bool m_viewport_changed = false;
};
