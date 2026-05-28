// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GeometryShaderManager.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/System.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/FreeLookCamera.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/VertexShaderManager.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"

#ifdef ENABLE_VR
#include "VideoCommon/OpenXROpcodeReplay.h"
#include "VideoCommon/VR/OpenXRManager.h"
#endif

static constexpr int LINE_PT_TEX_OFFSETS[8] = {0, 16, 8, 4, 2, 1, 1, 1};

namespace
{
// Hydra-ported per-layer HUD treatment for Metroid Prime.  Each entry mirrors
// MetroidVR.cpp:2055 (GetMetroidPrimeValues) — fScaleHack is a divisor on UnitsPerMetre
// (makes the layer appear larger/closer when bigger), fWidthHack/fHeightHack are direct
// multipliers on the per-eye projection's X/Y scale (FOV).  has_hack=false → no per-layer
// treatment (use default eye projection).
struct MetroidLayerHack
{
  bool has_hack;
  float scale_hack;
  float width_hack;
  float height_hack;
};

MetroidLayerHack GetMetroidLayerHack(MetroidElementLayer layer)
{
  switch (layer)
  {
  // ── Hydra MetroidVR.cpp:2285-2289 ──
  case MetroidElementLayer::Helmet:
    return {true, 100.0f, 1.7f, 1.7f};
  // ── Hydra MetroidVR.cpp:2272-2276 ──
  case MetroidElementLayer::Visor:
  case MetroidElementLayer::UnknownVisor:
  case MetroidElementLayer::VisorBootup:
    return {true, 90.0f, 1.0f, 1.0f};
  // ── Hydra MetroidVR.cpp:2249-2252 (METROID_HUD; visor/cannon selector) ──
  case MetroidElementLayer::HUD:
  case MetroidElementLayer::MorphballHUD:
  case MetroidElementLayer::XRayHUD:
  case MetroidElementLayer::DarkVisorHUD:
  case MetroidElementLayer::UnknownHUD:
    return {true, 30.0f, 0.79f, 0.79f};
  // Radar dot, map — same as HUD treatment in Hydra
  case MetroidElementLayer::RadarDot:
  case MetroidElementLayer::VisorRadarHint:
    return {true, 36.0f, 0.79f, 0.79f};
  case MetroidElementLayer::Map:
  case MetroidElementLayer::Map0:
  case MetroidElementLayer::Map1:
  case MetroidElementLayer::Map2:
  case MetroidElementLayer::MapOrHint:
  case MetroidElementLayer::MorphballMap:
  case MetroidElementLayer::MorphballMapOrHint:
    return {true, 30.0f, 0.79f, 0.79f};
  default:
    return {false, 1.0f, 1.0f, 1.0f};
  }
}

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
        auto& system = Core::System::GetInstance();
        auto& vertex_shader_manager = system.GetVertexShaderManager();
        const bool perspective = xfmem.projection.type == ProjectionType::Perspective;
        constants.stereoparams = {0.0f, 0.0f, 0.0f, 0.0f};
        constants.eye_projection = {};
        constants.legacy_eye_projection_x = {};
        constants.legacy_eye_projection_y = {};
        constants.legacy_center_projection = {};
        constants.eye_z_row = {};
        constants.depth_params = {};
        constants.vr_screen = {};
        constants.head_projection = {};
        constants.head_locked_params = {};
        constants.pixel_center_correction = {};
        const float upm_override = vr_units_per_meter_override;
        vr_units_per_meter_override = -1.0f;  // consume

        if (VR::g_openxr && VR::g_openxr->IsSessionRunning())
        {
          const float upm = std::max(upm_override > 0.0f ? upm_override :
                                                          g_ActiveConfig.vr_units_per_meter,
                                     0.0001f);

          // When vr_lock_head_pose is ON, only re-fetch the head pose from OpenXR when
          // we've been explicitly invalidated (at the XFB-copy frame boundary).  This
          // prevents mid-frame LocateViews() updates from desynchronising different draw
          // calls within the same game frame.  When OFF, refetch every call (legacy
          // behavior — kept as an escape hatch).
          // During opcode replay, never refresh: the real frame's cache is already
          // correct, and BPStructs' XFB-copy LocateViews has mutated m_eye_views
          // between real-frame draws and replay-frame draws.  Refreshing here would
          // render the replay with a different pose than the real frame it pairs
          // with, producing alternating-frame flicker on head rotation.
          const bool is_replay = VideoCommon::OpenXROpcodeReplay::IsReplaying();
          const bool need_refresh =
              !is_replay && (!g_ActiveConfig.vr_lock_head_pose || m_vr_pose_needs_refresh);
          if (need_refresh)
          {
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

            m_cached_eye_projection = eye_projection_rows;
            m_cached_eye_z_row = eye_z_rows;

            // Unrotated per-eye projection rows for head-locked content.
            std::array<std::array<float, 4>, 4> head_proj_rows{};
            VR::g_openxr->GetRawEyeProjectionRows(upm, head_proj_rows);
            m_cached_head_projection = head_proj_rows;

            // Snapshot the pose the cache was built from.  SubmitFrame will use
            // this snapshot so render_pose == submit_pose regardless of any
            // later LocateViews that may clobber m_eye_views before xrEndFrame.
            VR::g_openxr->RecordRenderedEyeViews();

            m_vr_pose_needs_refresh = false;
          }

          constants.eye_projection[0] = m_cached_eye_projection[0];
          constants.eye_projection[1] = m_cached_eye_projection[1];
          constants.eye_projection[2] = m_cached_eye_projection[2];
          constants.eye_projection[3] = m_cached_eye_projection[3];
          constants.eye_z_row[0] = m_cached_eye_z_row[0];
          constants.eye_z_row[1] = m_cached_eye_z_row[1];

          // The Vulkan legacy projected-space fallback is only safe for runtimes/headsets that
          // tolerate approximating the runtime frustum in projected space.
#if !defined(ANDROID)
          if (perspective && g_backend_info.api_type == APIType::Vulkan &&
              VR::g_openxr->ShouldUseVulkanLegacyProjectionFallback())
          {
            const Common::Vec2 fov_multiplier = g_freelook_camera.IsActive() ?
                                                    g_freelook_camera.GetFieldOfViewMultiplier() :
                                                    Common::Vec2{1.0f, 1.0f};
            const float game_projection_x_scale =
                xfmem.projection.rawProjection[0] * g_ActiveConfig.fAspectRatioHackW *
                fov_multiplier.x;
            const float game_projection_x_offset =
                xfmem.projection.rawProjection[1] * g_ActiveConfig.fAspectRatioHackW *
                fov_multiplier.x;
            const float game_projection_y_scale =
                xfmem.projection.rawProjection[2] * g_ActiveConfig.fAspectRatioHackH *
                fov_multiplier.y;
            const float game_projection_y_offset =
                xfmem.projection.rawProjection[3] * g_ActiveConfig.fAspectRatioHackH *
                fov_multiplier.y;
            constants.legacy_center_projection[0] = {game_projection_x_scale, 0.0f,
                                                     game_projection_x_offset, 0.0f};
            constants.legacy_center_projection[1] = {0.0f, game_projection_y_scale,
                                                     game_projection_y_offset, 0.0f};
            std::array<std::array<float, 4>, 2> legacy_eye_projection_x_rows{};
            std::array<std::array<float, 4>, 2> legacy_eye_projection_y_rows{};
            VR::g_openxr->GetLegacyProjectionAdjustments(
                upm, game_projection_x_scale, game_projection_x_offset, game_projection_y_scale,
                game_projection_y_offset, legacy_eye_projection_x_rows,
                legacy_eye_projection_y_rows);
            // The Vulkan projected-space fallback consumes eye indices opposite to the
            // OpenXR per-eye rows used by the full path, so swap them here.
            constants.legacy_eye_projection_x[0] = legacy_eye_projection_x_rows[1];
            constants.legacy_eye_projection_x[1] = legacy_eye_projection_x_rows[0];
            constants.legacy_eye_projection_y[0] = legacy_eye_projection_y_rows[1];
            constants.legacy_eye_projection_y[1] = legacy_eye_projection_y_rows[0];
            constants.stereoparams[1] = 1.0f;
          }
#endif

          // Unrotated per-eye projection rows for head-locked content (cached above).
          constants.head_projection[0] = m_cached_head_projection[0];
          constants.head_projection[1] = m_cached_head_projection[1];
          constants.head_projection[2] = m_cached_head_projection[2];
          constants.head_projection[3] = m_cached_head_projection[3];
          constants.head_locked_params = {g_ActiveConfig.vr_head_locked_curvature, 0.0f, 0.0f,
                                          0.0f};
          constants.pixel_center_correction = vertex_shader_manager.constants.pixelcentercorrection;

          // Virtual screen params (for ortho draws: menus, FMV, HUD).
          const float dist = upm * g_ActiveConfig.vr_screen_distance;
          const float half_h = upm * g_ActiveConfig.vr_screen_size * 0.5f;
          const float half_w = half_h * (16.0f / 9.0f);
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
        const bool had_override = !std::isnan(vr_stereo_override);
        if (had_override)
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

        // ─── Metroid HUD per-layer hacks (Hydra port) ──────────────────────────────────
        // When a Screen/HeadLocked override fires on a Metroid HUD layer (Helmet, Visor,
        // HUD/cannon-selector, etc.), rebuild the eye projection with a layer-specific
        // UnitsPerMetre and apply Hydra's per-layer FOV scale (fWidthHack/fHeightHack).
        // Only applies when the toggle is enabled AND we have a recognized layer with
        // a hack entry.  Falls back to the default projection otherwise.
        const MetroidElementLayer layer_for_hack = vr_metroid_layer;
        vr_metroid_layer = MetroidElementLayer::Unknown;  // consume
        const bool stereo_override_is_screen_family =
            constants.stereoparams[3] < -0.5f;  // Screen (-1), HeadLocked (-2/-2.25)
        if (g_ActiveConfig.vr_hud_3d_enable && stereo_override_is_screen_family &&
            layer_for_hack != MetroidElementLayer::Unknown &&
            VR::g_openxr && VR::g_openxr->IsSessionRunning())
        {
          const MetroidLayerHack hack = GetMetroidLayerHack(layer_for_hack);
          if (hack.has_hack && hack.scale_hack > 0.0f)
          {
            // Hydra: UPM_layer = base_upm * scale_hack / Scale.  Larger scale_hack →
            // game-units feel smaller, so the HUD layer appears closer/bigger in HMD space.
            const float base_upm = std::max(g_ActiveConfig.vr_units_per_meter, 0.0001f);
            const float layer_upm = base_upm * hack.scale_hack;
            // Rebuild rotated (eye_proj) and unrotated (head_proj) per-eye projection
            // rows with the layer-specific UPM.  Both are consumed by the GS — Screen
            // path uses eye_proj (rotated), HeadLocked uses head_proj (unrotated).
            std::array<std::array<float, 4>, 4> layer_eye_proj{};
            std::array<std::array<float, 4>, 2> layer_eye_z{};
            std::array<std::array<float, 4>, 4> layer_head_proj{};
            VR::g_openxr->GetEyeProjectionRows(layer_upm, layer_eye_proj, layer_eye_z);
            VR::g_openxr->GetRawEyeProjectionRows(layer_upm, layer_head_proj);
            // Apply fWidthHack / fHeightHack to projection rows (mirrors Hydra
            // VertexShaderManager.cpp:1530-1549).  Row 0 is X (multiply by width_hack),
            // row 1 is Y (multiply by height_hack).
            for (int eye = 0; eye < 2; ++eye)
            {
              for (int col = 0; col < 4; ++col)
              {
                layer_eye_proj[eye * 2 + 0][col] *= hack.width_hack;
                layer_eye_proj[eye * 2 + 1][col] *= hack.height_hack;
                layer_head_proj[eye * 2 + 0][col] *= hack.width_hack;
                layer_head_proj[eye * 2 + 1][col] *= hack.height_hack;
              }
            }
            constants.eye_projection[0] = layer_eye_proj[0];
            constants.eye_projection[1] = layer_eye_proj[1];
            constants.eye_projection[2] = layer_eye_proj[2];
            constants.eye_projection[3] = layer_eye_proj[3];
            constants.eye_z_row[0] = layer_eye_z[0];
            constants.eye_z_row[1] = layer_eye_z[1];
            constants.head_projection[0] = layer_head_proj[0];
            constants.head_projection[1] = layer_head_proj[1];
            constants.head_projection[2] = layer_head_proj[2];
            constants.head_projection[3] = layer_head_proj[3];

            // Diagnostic log: rate-limited per session, lets us see which layers fire.
            if (had_override)
            {
              static int s_metroid_log_count = 0;
              if (s_metroid_log_count < 60)
              {
                ++s_metroid_log_count;
                INFO_LOG_FMT(VIDEO,
                             "[MetroidHUD #{}] layer={} scale={:.1f} w={:.2f} h={:.2f} "
                             "upm_base={:.2f} upm_layer={:.2f}",
                             s_metroid_log_count,
                             MetroidElementLayerToDisplayName(layer_for_hack),
                             hack.scale_hack, hack.width_hack, hack.height_hack,
                             base_upm, layer_upm);
              }
            }
          }
        }
        (void)had_override;
        // ───────────────────────────────────────────────────────────────────────────────

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

void GeometryShaderManager::InvalidateVRHeadPose()
{
  m_vr_pose_needs_refresh = true;
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
