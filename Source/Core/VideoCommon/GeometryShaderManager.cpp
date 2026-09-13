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
#include "Common/VR/OpenXRInputState.h"
#include "VideoCommon/OpenXROpcodeReplay.h"
#include "VideoCommon/VR/OpenXRManager.h"
#endif

static constexpr int LINE_PT_TEX_OFFSETS[8] = {0, 16, 8, 4, 2, 1, 1, 1};

namespace
{
static constexpr float METROID_PERSPECTIVE_HUD_FORWARD_OFFSET = 1.0f;
static constexpr int METROID_HUD_REFERENCE_CONTEXT_COMBAT = 1;
static constexpr float METROID_COMBAT_HUD_REFERENCE_FAR_Z = -28.5f;
static constexpr float METROID_COMBAT_HUD_REFERENCE_NEAR_Z = -18.0f;
static constexpr float METROID_PERSPECTIVE_HUD_FALLBACK_REFERENCE_Z = -20.0f;
static constexpr float PRIMEDGUN_CINEMA_SINGLE_LAYER = 0.30f;

struct PerspectiveHudTransform
{
  std::array<float, 3> scale{};
  std::array<float, 3> position{};
  float distance = 0.0f;
  bool valid = false;
};

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

bool IsFinite(float value)
{
  return std::isfinite(value);
}

bool IsMetroidHudStableReferenceAllowed(int context, float reference_view_z)
{
  if (context != METROID_HUD_REFERENCE_CONTEXT_COMBAT)
    return true;

  return reference_view_z >= METROID_COMBAT_HUD_REFERENCE_FAR_Z &&
         reference_view_z <= METROID_COMBAT_HUD_REFERENCE_NEAR_Z;
}

PerspectiveHudTransform CalculatePerspectiveHudTransform(const Projection::Raw& projection,
                                                         float units_per_meter,
                                                         float reference_view_z,
                                                         float screen_distance,
                                                         float screen_size)
{
  PerspectiveHudTransform result;

  if (projection[0] == 0.0f || projection[2] == 0.0f || reference_view_z >= 0.0f)
    return result;

  const float zobj = -reference_view_z;
  if (!IsFinite(zobj) || zobj < 1.0e-4f)
    return result;

  const float left = (-(projection[1] + 1.0f) / projection[0]) * zobj;
  const float right = left + (2.0f / projection[0]) * zobj;
  const float bottom = (-(projection[3] + 1.0f) / projection[2]) * zobj;
  const float top = bottom + (2.0f / projection[2]) * zobj;
  const float width = right - left;
  const float height = top - bottom;
  if (width == 0.0f || height == 0.0f || !IsFinite(width) || !IsFinite(height))
    return result;

  result.distance = units_per_meter * screen_distance;
  const float size_reference = units_per_meter * screen_size;
  const float hud_width = std::abs((2.0f / projection[0]) * size_reference);
  const float hud_height = std::abs((2.0f / projection[2]) * size_reference);

  result.scale[0] = hud_width / width;
  result.scale[1] = hud_height / height;
  result.scale[2] = result.scale[0];
  result.position[0] = result.scale[0] * (-(right + left) * 0.5f);
  result.position[1] = result.scale[1] * (-(top + bottom) * 0.5f);
  result.position[2] = result.scale[2] * (zobj + METROID_PERSPECTIVE_HUD_FORWARD_OFFSET) -
                       result.distance;

  result.valid = IsFinite(result.scale[0]) && IsFinite(result.scale[1]) &&
                 IsFinite(result.scale[2]) && IsFinite(result.position[0]) &&
                 IsFinite(result.position[1]) && IsFinite(result.position[2]) &&
                 IsFinite(result.distance);
  return result;
}
}  // namespace

void GeometryShaderManager::Init()
{
  constants = {};
  m_vr_hud_shared_reference_valid = false;
  m_vr_hud_shared_reference_context = 0;
  m_vr_hud_stable_reference_valid = false;
  m_vr_hud_stable_reference_context = 0;
  m_vr_hud_frame_anchor_candidate_valid = false;
  m_vr_hud_frame_anchor_candidate_context = 0;

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
        const float headlocked_projection_scale_x = vr_headlocked_projection_scale_x;
        const float headlocked_projection_scale_y = vr_headlocked_projection_scale_y;
        const float headlocked_projection_offset_x = vr_headlocked_projection_offset_x;
        const float headlocked_projection_offset_y = vr_headlocked_projection_offset_y;
        const bool metroid_hud_self_center = vr_metroid_hud_self_center;
        vr_metroid_hud_self_center = false;  // consume
        const bool metroid_hud_anchor_candidate = vr_metroid_hud_anchor_candidate;
        vr_metroid_hud_anchor_candidate = false;  // consume
        const int metroid_hud_reference_context = vr_metroid_hud_reference_context;
        vr_metroid_hud_reference_context = 0;  // consume
        const float perspective_hud_distance_override = vr_perspective_hud_distance_override;
        const float perspective_hud_size_override = vr_perspective_hud_size_override;
        vr_perspective_hud_distance_override = -1.0f;  // consume
        vr_perspective_hud_size_override = -1.0f;       // consume
        vr_headlocked_projection_scale_x = 1.0f;
        vr_headlocked_projection_scale_y = 1.0f;
        vr_headlocked_projection_offset_x = 0.0f;
        vr_headlocked_projection_offset_y = 0.0f;
        const auto primedgun_overlay = Common::VR::OpenXRInputState::GetPrimedGunOverlay();
        const bool primedgun_cinematic_screen = primedgun_overlay.cinematic_screen_active;
        const bool primedgun_game_menu_screen = primedgun_overlay.game_menu_screen_enabled &&
                                                primedgun_overlay.game_menu_screen_active;
        const bool suppress_hud_calibration =
            primedgun_cinematic_screen && primedgun_game_menu_screen;
        const float upm = std::max(
            !suppress_hud_calibration && upm_override > 0.0f ?
                upm_override :
                g_ActiveConfig.vr_units_per_meter,
            0.0001f);

        if (VR::g_openxr && VR::g_openxr->IsSessionRunning())
        {
          // When the head-pose lock is on, only re-fetch the head pose from OpenXR when
          // we've been explicitly invalidated (at the XFB-copy frame boundary).  This
          // prevents mid-frame LocateViews() updates from desynchronising different draw
          // calls within the same game frame.  When OFF, follow every LocateViews update
          // (legacy behavior — kept as an escape hatch); the eye-view generation says when
          // one happened, so draws in between reuse the cached rows for their scale.
          // During opcode replay, never refresh: the real frame's cache is already
          // correct, and BPStructs' XFB-copy LocateViews has mutated m_eye_views
          // between real-frame draws and replay-frame draws.  Refreshing here would
          // render the replay with a different pose than the real frame it pairs
          // with, producing alternating-frame flicker on head rotation.
          const bool is_replay = VideoCommon::OpenXROpcodeReplay::IsReplaying();
          if (!is_replay)
          {
            const u64 eye_views_generation = VR::g_openxr->GetEyeViewsGeneration();
            if (m_vr_pose_needs_refresh || (!g_ActiveConfig.VRLockHeadPoseEffective() &&
                                            eye_views_generation != m_cached_eye_views_generation))
            {
              InvalidateEyeProjectionEntries();
              m_cached_eye_views_generation = eye_views_generation;
              m_vr_pose_needs_refresh = false;
            }
            // OpenXR stereo path bypasses the classic cproj path, so apply freelook here too.
            m_current_eye_projection_entry =
                LookupEyeProjection(upm, perspective && g_freelook_camera.IsActive(), true);
          }

          static const EyeProjectionEntry s_empty_eye_projection{};
          const EyeProjectionEntry& eye_projection =
              m_current_eye_projection_entry >= 0 ?
                  m_eye_projection_entries[m_current_eye_projection_entry] :
                  s_empty_eye_projection;
          constants.eye_projection[0] = eye_projection.eye_projection[0];
          constants.eye_projection[1] = eye_projection.eye_projection[1];
          constants.eye_projection[2] = eye_projection.eye_projection[2];
          constants.eye_projection[3] = eye_projection.eye_projection[3];
          constants.eye_z_row[0] = eye_projection.eye_z_row[0];
          constants.eye_z_row[1] = eye_projection.eye_z_row[1];

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
          constants.head_projection[0] = eye_projection.head_projection[0];
          constants.head_projection[1] = eye_projection.head_projection[1];
          constants.head_projection[2] = eye_projection.head_projection[2];
          constants.head_projection[3] = eye_projection.head_projection[3];
          for (u32 eye = 0; eye < 2; ++eye)
          {
            auto& row0 = constants.head_projection[eye * 2 + 0];
            auto& row1 = constants.head_projection[eye * 2 + 1];
            if (!suppress_hud_calibration)
            {
              row0[0] *= headlocked_projection_scale_x;
              row0[3] *= headlocked_projection_scale_x;
              row0[2] -= headlocked_projection_offset_x;
              row1[1] *= headlocked_projection_scale_y;
              row1[3] *= headlocked_projection_scale_y;
              row1[2] -= headlocked_projection_offset_y;
            }
          }
          constants.head_locked_params = {g_ActiveConfig.vr_head_locked_curvature, 0.0f, 0.0f,
                                          0.0f};
          constants.pixel_center_correction = vertex_shader_manager.constants.pixelcentercorrection;

          // Virtual screen params (for ortho draws: menus, FMV, HUD).
          const float dist = upm * g_ActiveConfig.vr_screen_distance;
          const float half_h = upm * g_ActiveConfig.vr_screen_size * 0.5f;
          constexpr float virtual_screen_aspect = 4.0f / 3.0f;
          const float half_w = half_h * virtual_screen_aspect;
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

            bool is_skybox = false;
            if (g_ActiveConfig.vr_detect_skybox)
            {
              const auto& pnm = vertex_shader_manager.constants.posnormalmatrix;
              is_skybox = pnm[0][3] == 0.0f && pnm[1][3] == 0.0f &&
                          pnm[2][3] == 0.0f && pnm[0][0] != 1.0f;
            }
            constants.stereoparams[2] = is_skybox ? 0.0f : 1.0f;
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

        // PrimedGun cinema mode renders unmatched game draws unchanged and duplicates them
        // to both EFB layers; SubmitFrame then presents layer 0 as an OpenXR quad screen.
        // Keep explicit element overrides during cutscenes so deliberately matched loading text
        // remains correctly placed. Pause/map screens force the entire composition to mono because
        // their HUD overrides otherwise retain per-eye offsets inside the detached cinema quad.
        if (primedgun_cinematic_screen && (primedgun_game_menu_screen || !had_override))
        {
          constants.stereoparams[3] = PRIMEDGUN_CINEMA_SINGLE_LAYER;
        }

        const bool perspective_hud =
            perspective && VR::g_openxr && VR::g_openxr->IsSessionRunning() &&
            constants.stereoparams[3] < -2.5f;
        const float perspective_hud_distance =
            !suppress_hud_calibration && perspective_hud_distance_override > 0.0f ?
                perspective_hud_distance_override :
                g_ActiveConfig.vr_screen_distance;
        const float perspective_hud_size =
            !suppress_hud_calibration && perspective_hud_size_override > 0.0f ?
                perspective_hud_size_override :
                g_ActiveConfig.vr_screen_size;
        const float perspective_hud_offset_x =
            suppress_hud_calibration ?
                0.0f :
                std::clamp(primedgun_overlay.metroid_hud_offset_right -
                               primedgun_overlay.metroid_hud_offset_left,
                           -1.0f, 1.0f);
        const float perspective_hud_offset_y =
            suppress_hud_calibration ?
                0.0f :
                std::clamp(primedgun_overlay.metroid_hud_offset_up -
                               primedgun_overlay.metroid_hud_offset_down,
                           -1.0f, 1.0f);
        if (perspective_hud)
        {
          const float reference_view_z = vertex_shader_manager.constants.posnormalmatrix[2][3];
          const bool self_center_layer = metroid_hud_self_center;
          const bool valid_reference =
              std::isfinite(reference_view_z) && reference_view_z < 0.0f;
          if (metroid_hud_anchor_candidate && !self_center_layer && valid_reference)
          {
            const bool stable_reference_allowed = IsMetroidHudStableReferenceAllowed(
                metroid_hud_reference_context, reference_view_z);
            const bool replace_candidate =
                stable_reference_allowed &&
                (!m_vr_hud_frame_anchor_candidate_valid ||
                 metroid_hud_reference_context != m_vr_hud_frame_anchor_candidate_context ||
                 reference_view_z < m_vr_hud_frame_anchor_candidate_z);
            if (replace_candidate)
            {
              m_vr_hud_frame_anchor_candidate_z = reference_view_z;
              m_vr_hud_frame_anchor_candidate_valid = true;
              m_vr_hud_frame_anchor_candidate_context = metroid_hud_reference_context;
            }
          }

          const bool stable_reference_matches =
              m_vr_hud_stable_reference_valid &&
              m_vr_hud_stable_reference_context == metroid_hud_reference_context;
          if ((!m_vr_hud_shared_reference_valid ||
               m_vr_hud_shared_reference_context != metroid_hud_reference_context) &&
              !self_center_layer)
          {
            if (stable_reference_matches)
            {
              m_vr_hud_shared_reference_z = m_vr_hud_stable_reference_z;
              m_vr_hud_shared_reference_valid = true;
              m_vr_hud_shared_reference_context = metroid_hud_reference_context;
            }
            else if (valid_reference)
            {
              m_vr_hud_shared_reference_z = reference_view_z;
              m_vr_hud_shared_reference_valid = true;
              m_vr_hud_shared_reference_context = metroid_hud_reference_context;
            }
          }

          const float shared_reference_z =
              m_vr_hud_shared_reference_valid ? m_vr_hud_shared_reference_z : reference_view_z;
          bool depth_outlier = false;
          if (std::isfinite(reference_view_z) && reference_view_z < 0.0f &&
              shared_reference_z < 0.0f)
          {
            const float depth_ratio = reference_view_z / shared_reference_z;
            depth_outlier = depth_ratio < 0.5f || depth_ratio > 2.0f;
          }

          const float ref_for_transform =
              (self_center_layer || depth_outlier) ? reference_view_z : shared_reference_z;
          PerspectiveHudTransform transform = CalculatePerspectiveHudTransform(
              xfmem.projection.rawProjection, upm, ref_for_transform, perspective_hud_distance,
              perspective_hud_size);
          if (!transform.valid && ref_for_transform != METROID_PERSPECTIVE_HUD_FALLBACK_REFERENCE_Z)
          {
            transform = CalculatePerspectiveHudTransform(
                xfmem.projection.rawProjection, upm, METROID_PERSPECTIVE_HUD_FALLBACK_REFERENCE_Z,
                perspective_hud_distance, perspective_hud_size);
          }
          if (transform.valid)
          {
            constants.vr_screen[0] = transform.position[0] + upm * perspective_hud_offset_x;
            constants.vr_screen[1] = transform.position[1] + upm * perspective_hud_offset_y;
            constants.vr_screen[2] = transform.position[2];
            constants.head_locked_params[1] = transform.scale[0];
            constants.head_locked_params[2] = transform.scale[1];
            constants.head_locked_params[3] = transform.scale[2];
            constants.depth_params[3] = transform.distance;
          }
          else
          {
            constants.stereoparams[3] = -2.0f;
          }
        }

        // For ortho/screen draws, pass depth params via depth_params
        // (depth_params is otherwise unused for non-perspective draws).
        // .x = layer offset (between draw calls), .y = element depth (within draw call),
        // .z = HUD thickness in game units.
        if (constants.stereoparams[3] < -0.5f)
        {
          constants.depth_params[0] = g_ActiveConfig.vr_layer_offset;
          // Per-override element depth if set, otherwise global
          constants.depth_params[1] = (vr_element_depth_override >= 0.0f)
                                          ? vr_element_depth_override
                                          : g_ActiveConfig.vr_element_depth;
          vr_element_depth_override = -1.0f;  // consume
          constants.depth_params[2] = upm * g_ActiveConfig.vr_hud_thickness;
          if (perspective_hud)
            constants.depth_params[3] = constants.depth_params[3] > 0.0f ?
                                            constants.depth_params[3] :
                                            upm * perspective_hud_distance;
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
            constants.stereoparams[3] < -0.5f &&
            constants.stereoparams[3] >= -2.5f;  // Screen (-1), HeadLocked (-2/-2.25)
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
            // Cached per layer scale; the width/height hacks below are applied to copies.
            const EyeProjectionEntry& layer_entry =
                m_eye_projection_entries[LookupEyeProjection(layer_upm, false, false)];
            std::array<std::array<float, 4>, 4> layer_eye_proj = layer_entry.eye_projection;
            std::array<std::array<float, 4>, 2> layer_eye_z = layer_entry.eye_z_row;
            std::array<std::array<float, 4>, 4> layer_head_proj = layer_entry.head_projection;
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

#ifdef ENABLE_VR
void GeometryShaderManager::InvalidateEyeProjectionEntries()
{
  for (EyeProjectionEntry& entry : m_eye_projection_entries)
    entry.valid = false;
  m_eye_projection_next_entry = 0;
  m_current_eye_projection_entry = -1;
}

int GeometryShaderManager::LookupEyeProjection(float units_per_meter, bool apply_freelook,
                                               bool record_rendered_views)
{
  for (size_t i = 0; i < NUM_EYE_PROJECTION_ENTRIES; ++i)
  {
    const EyeProjectionEntry& entry = m_eye_projection_entries[i];
    if (entry.valid && entry.freelook_applied == apply_freelook &&
        std::abs(entry.units_per_meter - units_per_meter) <= 0.0001f)
    {
      return static_cast<int>(i);
    }
  }

  // Round-robin replacement that never evicts the entry the current frame's draws use, so a
  // layer lookup cannot invalidate what opcode replay will re-read.
  size_t slot = m_eye_projection_next_entry;
  if (static_cast<int>(slot) == m_current_eye_projection_entry)
    slot = (slot + 1) % NUM_EYE_PROJECTION_ENTRIES;
  m_eye_projection_next_entry = (slot + 1) % NUM_EYE_PROJECTION_ENTRIES;

  EyeProjectionEntry& entry = m_eye_projection_entries[slot];
  entry.valid = true;
  entry.freelook_applied = apply_freelook;
  entry.units_per_meter = units_per_meter;
  VR::g_openxr->GetEyeProjectionRows(units_per_meter, entry.eye_projection, entry.eye_z_row);
  if (apply_freelook)
  {
    const Common::Matrix44 freelook_view = g_freelook_camera.GetView();
    ApplyRowTransform(&entry.eye_projection, freelook_view);
    ApplyRowTransform(&entry.eye_z_row, freelook_view);
  }
  // Unrotated per-eye projection rows for head-locked content.
  VR::g_openxr->GetRawEyeProjectionRows(units_per_meter, entry.head_projection);
  if (record_rendered_views)
  {
    // Snapshot the pose these rows were built from. SubmitFrame uses this snapshot so
    // render_pose == submit_pose regardless of any later LocateViews before xrEndFrame.
    VR::g_openxr->RecordRenderedEyeViews();
  }
  return static_cast<int>(slot);
}
#endif

void GeometryShaderManager::InvalidateVRHeadPose()
{
  m_vr_pose_needs_refresh = true;
  if (m_vr_hud_frame_anchor_candidate_valid)
  {
    const bool stable_reference_allowed = IsMetroidHudStableReferenceAllowed(
        m_vr_hud_frame_anchor_candidate_context, m_vr_hud_frame_anchor_candidate_z);
    const bool accept_candidate =
        stable_reference_allowed &&
        (!m_vr_hud_stable_reference_valid ||
         m_vr_hud_frame_anchor_candidate_context != m_vr_hud_stable_reference_context ||
         m_vr_hud_frame_anchor_candidate_z < m_vr_hud_stable_reference_z);
    if (accept_candidate)
    {
      m_vr_hud_stable_reference_z = m_vr_hud_frame_anchor_candidate_z;
      m_vr_hud_stable_reference_valid = true;
      m_vr_hud_stable_reference_context = m_vr_hud_frame_anchor_candidate_context;
    }
    m_vr_hud_frame_anchor_candidate_valid = false;
  }
  m_vr_hud_shared_reference_valid = false;
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
