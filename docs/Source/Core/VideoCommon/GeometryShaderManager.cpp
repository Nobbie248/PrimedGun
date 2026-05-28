// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GeometryShaderManager.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/FreeLookCamera.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/VertexShaderManager.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"

#ifdef ENABLE_VR
#include "VideoCommon/VR/OpenXRManager.h"
#endif

static constexpr int LINE_PT_TEX_OFFSETS[8] = {0, 16, 8, 4, 2, 1, 1, 1};

namespace
{
void ApplyRowTransform(std::array<std::array<float, 4>, 4>* rows, const Common::Matrix44& matrix)
{
  for (auto& row : *rows)
  {
    const std::array<float, 4> src = row;
    row[0] = src[0] * matrix.data[0] + src[1] * matrix.data[4] + src[2] * matrix.data[8] +
             src[3] * matrix.data[12];
    row[1] = src[0] * matrix.data[1] + src[1] * matrix.data[5] + src[2] * matrix.data[9] +
             src[3] * matrix.data[13];
    row[2] = src[0] * matrix.data[2] + src[1] * matrix.data[6] + src[2] * matrix.data[10] +
             src[3] * matrix.data[14];
    row[3] = src[0] * matrix.data[3] + src[1] * matrix.data[7] + src[2] * matrix.data[11] +
             src[3] * matrix.data[15];
  }
}

void ApplyRowTransform(std::array<std::array<float, 4>, 2>* rows, const Common::Matrix44& matrix)
{
  for (auto& row : *rows)
  {
    const std::array<float, 4> src = row;
    row[0] = src[0] * matrix.data[0] + src[1] * matrix.data[4] + src[2] * matrix.data[8] +
             src[3] * matrix.data[12];
    row[1] = src[0] * matrix.data[1] + src[1] * matrix.data[5] + src[2] * matrix.data[9] +
             src[3] * matrix.data[13];
    row[2] = src[0] * matrix.data[2] + src[1] * matrix.data[6] + src[2] * matrix.data[10] +
             src[3] * matrix.data[14];
    row[3] = src[0] * matrix.data[3] + src[1] * matrix.data[7] + src[2] * matrix.data[11] +
             src[3] * matrix.data[15];
  }
}
}  // namespace

void GeometryShaderManager::Init()
{
  constants = {};

  // Init any initial constants which aren't zero when bpmem is zero.
  SetViewportChanged();
  SetProjectionChanged();

  dirty = true;
}

void GeometryShaderManager::Dirty()
{
  // This function is called after a savestate is loaded.
  // Any constants that can changed based on settings should be re-calculated
  m_projection_changed = true;

  // Uses EFB scale config
  SetLinePtWidthChanged();

  dirty = true;
}

void GeometryShaderManager::SetVSExpand(VSExpand expand)
{
  if (constants.vs_expand != expand)
  {
    constants.vs_expand = expand;
    dirty = true;
  }
}

void GeometryShaderManager::SetConstants(PrimitiveType prim)
{
  if (g_ActiveConfig.stereo_mode != StereoMode::Off)
  {
#ifdef ENABLE_VR
    const bool openxr_mode = (g_ActiveConfig.stereo_mode == StereoMode::OpenXR);
#else
    const bool openxr_mode = false;
#endif

    if (m_projection_changed || openxr_mode)
    {
      m_projection_changed = false;

#ifdef ENABLE_VR
      if (openxr_mode)
      {
        const bool perspective = xfmem.projection.type == ProjectionType::Perspective;
        constants.stereoparams = {0.0f, 0.0f, 0.0f, 0.0f};
        constants.eye_projection = {};
        constants.eye_z_row = {};
        constants.depth_params = {};
        constants.vr_screen = {};
        constants.head_projection = {};
        constants.head_locked_params = {};
        const float upm_override = vr_units_per_meter_override;
        vr_units_per_meter_override = -1.0f;  // consume
        const bool head_locked_full_view = vr_head_locked_full_view_override;
        vr_head_locked_full_view_override = false;  // consume
        const float head_locked_right_eye_x_offset = vr_head_locked_right_eye_x_offset_override;
        vr_head_locked_right_eye_x_offset_override = 0.0f;  // consume

        if (VR::g_openxr && VR::g_openxr->IsSessionRunning())
        {
          const float upm = std::max(upm_override > 0.0f ? upm_override :
                                                          g_ActiveConfig.vr_units_per_meter,
                                     0.0001f);

          // Per-eye projection rows — needed by BOTH perspective and ortho VR paths.
          std::array<std::array<float, 4>, 4> eye_projection_rows{};
          std::array<std::array<float, 4>, 2> eye_z_rows{};
          VR::g_openxr->GetEyeProjectionRows(upm, eye_projection_rows, eye_z_rows);

          // OpenXR stereo path bypasses the classic cproj path, so apply freelook here too.
          if (perspective && g_freelook_camera.IsActive())
          {
            const Common::Matrix44 freelook_view = g_freelook_camera.GetView();
            ApplyRowTransform(&eye_projection_rows, freelook_view);
            ApplyRowTransform(&eye_z_rows, freelook_view);
          }

          constants.eye_projection[0] = eye_projection_rows[0];
          constants.eye_projection[1] = eye_projection_rows[1];
          constants.eye_projection[2] = eye_projection_rows[2];
          constants.eye_projection[3] = eye_projection_rows[3];
          constants.eye_z_row[0] = eye_z_rows[0];
          constants.eye_z_row[1] = eye_z_rows[1];

          // Unrotated per-eye projection rows for head-locked content.
          std::array<std::array<float, 4>, 4> head_proj_rows{};
          VR::g_openxr->GetRawEyeProjectionRows(upm, head_proj_rows);
          constants.head_projection[0] = head_proj_rows[0];
          constants.head_projection[1] = head_proj_rows[1];
          constants.head_projection[2] = head_proj_rows[2];
          constants.head_projection[3] = head_proj_rows[3];
          // head_locked_params:
          //   x = curvature
          //   y = full-view centered mapping flag (for fullscreen-mono ortho overlays)
          constants.head_locked_params = {g_ActiveConfig.vr_head_locked_curvature,
                                          head_locked_full_view ? 1.0f : 0.0f,
                                          head_locked_right_eye_x_offset, 0.0f};

          // Virtual screen params (for ortho draws: menus, FMV, HUD).
          const float dist = upm * g_ActiveConfig.vr_screen_distance;
          float half_h = upm * g_ActiveConfig.vr_screen_size * 0.5f;
          float half_w = half_h * (16.0f / 9.0f);

          if (head_locked_full_view)
          {
            float max_tan_x = 0.0f;
            float max_tan_y = 0.0f;
            const auto& eye_views = VR::g_openxr->GetEyeViews();
            for (const auto& eye_view : eye_views)
            {
              const float tan_left = std::abs(std::tan(eye_view.fov.angleLeft));
              const float tan_right = std::abs(std::tan(eye_view.fov.angleRight));
              const float tan_up = std::abs(std::tan(eye_view.fov.angleUp));
              const float tan_down = std::abs(std::tan(eye_view.fov.angleDown));
              max_tan_x = std::max({max_tan_x, tan_left, tan_right});
              max_tan_y = std::max({max_tan_y, tan_up, tan_down});
            }

            if (std::isfinite(max_tan_x) && std::isfinite(max_tan_y) && max_tan_x > 0.0f &&
                max_tan_y > 0.0f)
            {
              half_w = dist * max_tan_x;
              half_h = dist * max_tan_y;
            }
          }
          // Layer index: use manual override if set, otherwise auto counter.
          const int layer = (vr_ortho_layer_override >= 0) ? vr_ortho_layer_override
                                                           : vr_ortho_draw_counter;
          vr_ortho_layer_override = -1;  // consume
          constants.vr_screen = {half_w, half_h, dist, static_cast<float>(layer)};

          if (perspective)
          {
            // Detect if the game's projection flips the X axis (e.g. mirror mode in
            // Mario Kart).  The VR GS path replaces the game's projection entirely with
            // the VR eye projection, which always has a positive X scale.  If the game's
            // projection has a negative X scale, triangle winding reverses and the game
            // adjusts its cull mode accordingly — but our VR projection doesn't reproduce
            // the flip, so the cull mode becomes wrong and back-faces are shown instead of
            // front-faces.  Pass the sign to the GS so it can negate the output X when
            // needed, restoring correct winding AND the intended mirrored view.
            const float proj_x_sign =
                (xfmem.projection.rawProjection[0] < 0.0f) ? -1.0f : 1.0f;
            constants.stereoparams[0] = proj_x_sign;

            float depth_scale = 1.0f;
            float depth_offset = 0.0f;
            if (VertexShaderManager::UseVertexDepthRange())
            {
              if (g_backend_info.bSupportsReversedDepthRange)
              {
                depth_scale = std::fabs(xfmem.viewport.zRange) / 16777215.0f;
                if (xfmem.viewport.zRange < 0.0f)
                  depth_offset = xfmem.viewport.farZ / 16777215.0f;
                else
                  depth_offset = 1.0f - xfmem.viewport.farZ / 16777215.0f;
              }
              else
              {
                depth_scale = xfmem.viewport.zRange / 16777215.0f;
                depth_offset = 1.0f - xfmem.viewport.farZ / 16777215.0f;
              }
            }
            constants.depth_params = {xfmem.projection.rawProjection[4],
                                      xfmem.projection.rawProjection[5],
                                      depth_scale, depth_offset};

            // Perspective flag consumed in the OpenXR GS path.
            constants.stereoparams[3] = 1.0f;
          }
          else if (g_ActiveConfig.vr_virtual_screen)
          {
            // Orthographic VR flag — signals GS to use virtual screen path.
            constants.stereoparams[3] = -1.0f;
          }

        }

        // Per-draw VR stereo override (screen/fullscreen handling from shader overrides)
        if (!std::isnan(vr_stereo_override))
        {
          constants.stereoparams[3] = vr_stereo_override;
          vr_stereo_override = std::numeric_limits<float>::quiet_NaN();
        }

        // For ortho/screen draws, pass depth params via depth_params
        // (depth_params is otherwise unused for non-perspective draws).
        // .x = layer offset (between draw calls), .y = element depth (within draw call)
        if (constants.stereoparams[3] < -0.5f)
        {
          constants.depth_params[0] = g_ActiveConfig.vr_layer_offset;
          // Per-override element depth if set, otherwise global
          constants.depth_params[1] = (vr_element_depth_override >= 0.0f)
                                          ? vr_element_depth_override
                                          : g_ActiveConfig.vr_element_depth;
          vr_element_depth_override = -1.0f;  // consume
        }

        dirty = true;
      }
      else
#endif
      if (xfmem.projection.type == ProjectionType::Perspective)
      {
        const float offset = g_ActiveConfig.stereo_depth;
        constants.stereoparams[0] = g_ActiveConfig.bStereoSwapEyes ? offset : -offset;
        constants.stereoparams[1] = g_ActiveConfig.bStereoSwapEyes ? -offset : offset;
        constants.stereoparams[2] = g_ActiveConfig.stereo_convergence;
        constants.stereoparams[3] = 0.0f;
        dirty = true;
      }
      else
      {
        constants.stereoparams[0] = 0.0f;
        constants.stereoparams[1] = 0.0f;
        constants.stereoparams[2] = 0.0f;
        constants.stereoparams[3] = 0.0f;
        dirty = true;
      }
    }
  }

  if (g_ActiveConfig.UseVSForLinePointExpand())
  {
    if (prim == PrimitiveType::Points)
      SetVSExpand(VSExpand::Point);
    else if (prim == PrimitiveType::Lines)
      SetVSExpand(VSExpand::Line);
    else
      SetVSExpand(VSExpand::None);
  }

  if (m_viewport_changed)
  {
    m_viewport_changed = false;

    constants.lineptparams[0] = 2.0f * xfmem.viewport.wd;
    constants.lineptparams[1] = -2.0f * xfmem.viewport.ht;

    dirty = true;
  }
}

void GeometryShaderManager::SetViewportChanged()
{
  m_viewport_changed = true;
}

void GeometryShaderManager::SetProjectionChanged()
{
  m_projection_changed = true;
}

void GeometryShaderManager::SetLinePtWidthChanged()
{
  constants.lineptparams[2] = bpmem.lineptwidth.linesize / 6.f;
  constants.lineptparams[3] = bpmem.lineptwidth.pointsize / 6.f;
  constants.texoffset[2] = LINE_PT_TEX_OFFSETS[bpmem.lineptwidth.lineoff];
  constants.texoffset[3] = LINE_PT_TEX_OFFSETS[bpmem.lineptwidth.pointoff];
  dirty = true;
}

void GeometryShaderManager::SetTexCoordChanged(u8 texmapid)
{
  TCoordInfo& tc = bpmem.texcoords[texmapid];
  int bitmask = 1 << texmapid;
  constants.texoffset[0] &= ~bitmask;
  constants.texoffset[0] |= tc.s.line_offset << texmapid;
  constants.texoffset[1] &= ~bitmask;
  constants.texoffset[1] |= tc.s.point_offset << texmapid;
  dirty = true;
}

void GeometryShaderManager::DoState(PointerWrap& p)
{
  p.Do(m_projection_changed);
  p.Do(m_viewport_changed);

  p.Do(constants);

  if (p.IsReadMode())
  {
    // Fixup the current state from global GPU state
    // NOTE: This requires that all GPU memory has been loaded already.
    Dirty();
  }
}
