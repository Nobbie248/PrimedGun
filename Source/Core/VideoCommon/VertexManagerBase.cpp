// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VertexManagerBase.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Contains.h"
#include "Common/EnumMap.h"
#include "Common/FileUtil.h"
#include "Common/Hash.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/MathUtil.h"
#include "Common/SmallVector.h"
#ifdef ENABLE_VR
#include "Common/VR/OpenXRInputState.h"
#endif

#include "Core/DolphinAnalytics.h"
#include "Core/HW/SystemTimers.h"
#include "Core/System.h"

#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/BoundingBox.h"
#include "VideoCommon/DataReader.h"
#include "VideoCommon/ElementsGroupManager.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/GeometryShaderManager.h"
#include "VideoCommon/GraphicsModSystem/Runtime/CustomShaderCache.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModActionData.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModManager.h"
#include "VideoCommon/IndexGenerator.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/OpcodeDecoding.h"
#include "VideoCommon/PerfQueryBase.h"
#include "VideoCommon/PixelShaderGen.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VertexShaderManager.h"
#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"
#include "VideoCommon/CullingCodeFinder.h"
#include "VideoCommon/HideObjectEngine.h"
#include "VideoCommon/ShaderHunter.h"
#include "VideoCommon/TextureElementManager.h"
#include "VideoCommon/XFStateManager.h"

#include "Core/ConfigManager.h"

std::unique_ptr<VertexManagerBase> g_vertex_manager;

using OpcodeDecoder::Primitive;

namespace
{
constexpr float VR_STEREO_OVERRIDE_HEAD_LOCKED = -2.0f;
constexpr float VR_STEREO_OVERRIDE_SCREEN = -1.0f;
constexpr float VR_STEREO_OVERRIDE_FULLSCREEN = 0.0f;
// Uses the same geometry shader path as fullscreen, but RenderDrawCall disables the
// OpenXR legacy view so the original fullscreen projection is emitted identically to both eyes.
constexpr float VR_STEREO_OVERRIDE_FULLSCREEN_MONO = 0.25f;
// Uses the headlocked path, but asks the D3D geometry shader to copy the current eye into tex0.z
// for texture-array layer selection in the pixel shader.
constexpr float VR_STEREO_OVERRIDE_HEAD_LOCKED_TEX0_LAYER = -2.25f;
constexpr int VR_TEXTURE_LAYER_FROM_D3D_FULLSCREEN_TEX0_Z = -2;
constexpr u32 METROID_PRIME1_THERMAL_COLOR_MASK_SOURCE_STAGE = 7;
constexpr int METROID_PRIME1_XRAY_ORTHO_RIGHT_X100 = 64000;
constexpr int METROID_PRIME1_XRAY_ORTHO_TOP_X100 = 44800;
constexpr int METROID_PRIME1_THERMAL_HEAT_HFOV_X100 = 7;
constexpr int METROID_PRIME1_THERMAL_HEAT_VFOV_X100 = 5;
constexpr int METROID_PRIME1_THERMAL_HEAT_NEAR_X1000 = 200;
constexpr int METROID_PRIME1_THERMAL_HEAT_FAR_X100 = 75;
constexpr u64 METROID_PRIME1_THERMAL_MASK_STAGE7_HASH = 0x8e31231cae45574c;
constexpr u32 METROID_PRIME1_FULLSCREEN_EFB_EFFECT_PS_HASH = 0x667d9a41;
constexpr u32 METROID_PRIME1_FULLSCREEN_EFB_EFFECT_SOURCE_STAGE = 7;
constexpr u64 PRIMEGUN_CANNON_PROBE_PS_HASH = 0x9e0b32f0;
constexpr u32 PRIMEGUN_CANNON_PROBE_MAX_LOGS = 240;
constexpr u32 PRIMEGUN_THERMAL_HUD_MAX_LOGS = 240;
constexpr u32 PRIMEGUN_ELEMENT_HANDLING_MAX_LOGS = 800;
constexpr int METROID_HUD_CONTEXT_DEFAULT = 0;
constexpr int METROID_HUD_CONTEXT_COMBAT = 1;
constexpr int METROID_HUD_CONTEXT_MENU = 2;
constexpr float METROID_PRIME1_PERSPECTIVE_HUD_DEFAULT_DISTANCE = 0.5f;
constexpr float METROID_PRIME1_PERSPECTIVE_HUD_DEFAULT_SIZE = 0.5f;

struct MetroidHydraHudSettings
{
  bool enabled = false;
  bool perspective_hud = false;
  float scale = 1.0f;
  float width = 1.0f;
  float height = 1.0f;
  float up = 0.0f;
  float right = 0.0f;
};

struct MetroidLayerBehavior
{
  bool skip = false;
  ShaderHunter::HandlingType handling = ShaderHunter::HandlingType::Skip;
};

u32 s_primegun_cannon_probe_log_count = 0;
bool s_primegun_cannon_probe_suppressed_notice = false;
u32 s_primegun_thermal_hud_log_count = 0;
u32 s_primegun_element_handling_log_count = 0;

int GetFullscreenMonoPerEyeTextureLayer()
{
  return g_backend_info.api_type == APIType::D3D ? VR_TEXTURE_LAYER_FROM_D3D_FULLSCREEN_TEX0_Z :
                                                   -1;
}

bool WithinElementSignatureTolerance(int value, int expected, int tolerance)
{
  return value >= expected - tolerance && value <= expected + tolerance;
}

float PositionMatrixDeterminant(u32 matrix_index)
{
  if (matrix_index + 2 >= XFMEM_POSMATRICES_END / 4)
    return 1.0f;

  const float* m = &xfmem.posMatrices[matrix_index * 4];
  return m[0] * (m[5] * m[10] - m[6] * m[9]) -
         m[1] * (m[4] * m[10] - m[6] * m[8]) +
         m[2] * (m[4] * m[9] - m[5] * m[8]);
}

bool IsMirroredPositionMatrix(u32 matrix_index)
{
  return PositionMatrixDeterminant(matrix_index) < -0.001f;
}

bool DrawUsesMirroredPositionMatrix(const NativeVertexFormat* format)
{
  if (format == nullptr)
    return false;

  const PortableVertexDeclaration& vertex_declaration = format->GetVertexDeclaration();
  if (vertex_declaration.posmtx.enable)
  {
    for (const u32 matrix_index : VertexLoaderManager::position_matrix_index_cache)
    {
      if (IsMirroredPositionMatrix(matrix_index))
        return true;
    }
    return false;
  }

  return IsMirroredPositionMatrix(g_main_cp_state.matrix_index_a.PosNormalMtxIdx);
}

const char* HandlingToDebugName(ShaderHunter::HandlingType handling)
{
  switch (handling)
  {
  case ShaderHunter::HandlingType::Skip:
    return "skip";
  case ShaderHunter::HandlingType::Screen:
    return "screen";
  case ShaderHunter::HandlingType::Fullscreen:
    return "fullscreen";
  case ShaderHunter::HandlingType::FullscreenMono:
    return "fullscreen_mono";
  case ShaderHunter::HandlingType::HeadLocked:
    return "headlocked";
  case ShaderHunter::HandlingType::Flag:
    return "flag";
  case ShaderHunter::HandlingType::UnitsPerMeter:
    return "units_per_meter";
  default:
    return "unknown";
  }
}

std::string PrimedGunCannonProbeLogPath()
{
  const std::string game_id = SConfig::GetInstance().GetGameID();
  const std::string dump_dir = File::GetUserPath(D_DUMP_IDX) + "Shaders/" + game_id + "/";
  if (!File::IsDirectory(dump_dir))
    File::CreateFullPath(dump_dir);
  return dump_dir + "primegun_cannon_probe.log";
}

void AppendPrimedGunCannonProbeLog(std::string_view text)
{
  const std::string path = PrimedGunCannonProbeLogPath();
  File::IOFile file(path, "ab");
  if (!file)
  {
    WARN_LOG_FMT(VIDEO, "PrimedGun cannon probe: failed to open '{}'", path);
    return;
  }
  file.WriteString(text);
}

void LogPrimedGunCannonProbeDraw(u32 draw_sequence, u64 vs_hash, u64 ps_hash, u64 gs_hash,
                                const std::array<u64, 8>& texture_hashes,
                                const std::array<std::string, 8>& texture_names,
                                const ShaderHunter::RuntimeElementSignature& signature)
{
  if (ps_hash != PRIMEGUN_CANNON_PROBE_PS_HASH)
    return;

  if (s_primegun_cannon_probe_log_count >= PRIMEGUN_CANNON_PROBE_MAX_LOGS)
  {
    if (!s_primegun_cannon_probe_suppressed_notice)
    {
      s_primegun_cannon_probe_suppressed_notice = true;
      AppendPrimedGunCannonProbeLog(fmt::format(
          "\nPrimedGun cannon probe: suppressing further draws after {} matches.\n",
          PRIMEGUN_CANNON_PROBE_MAX_LOGS));
      INFO_LOG_FMT(VIDEO,
                   "PrimedGun cannon probe: suppressing further draws after {} matches. Log: {}",
                   PRIMEGUN_CANNON_PROBE_MAX_LOGS, PrimedGunCannonProbeLogPath());
    }
    return;
  }

  ++s_primegun_cannon_probe_log_count;
  std::string out;
  out += fmt::format(
      "\n[PrimedGun cannon probe #{:03}] draw={} VS={:08x} PS={:08x} GS={:08x}\n",
      s_primegun_cannon_probe_log_count, draw_sequence, static_cast<u32>(vs_hash),
      static_cast<u32>(ps_hash), static_cast<u32>(gs_hash));
  out += fmt::format(
      "projection={} hfov={} vfov={} near={} far={} ortho=({}, {}, {}, {}) viewport=({}, {}, "
      "{}x{}) scissor=({}, {}, {}, {}) alpha={:08x} ztest={} zupdate={} zfunc={} blend_color={} "
      "blend_alpha={}\n",
      signature.perspective ? "perspective" : "ortho", signature.perspective_hfov_x100,
      signature.perspective_vfov_x100, signature.perspective_near_x1000,
      signature.perspective_far_x100, signature.ortho_left_x100, signature.ortho_right_x100,
      signature.ortho_top_x100, signature.ortho_bottom_x100, signature.viewport_x,
      signature.viewport_y, signature.viewport_width, signature.viewport_height,
      signature.scissor_left, signature.scissor_top, signature.scissor_right,
      signature.scissor_bottom, signature.alpha_test_hex, signature.ztest, signature.zupdate,
      signature.zfunc, signature.blend_color_update, signature.blend_alpha_update);

  out += fmt::format("genmode texgens={} tev_stages={} ind_stages={} cull={}\n",
                     bpmem.genMode.numtexgens.Value(), bpmem.genMode.numtevstages.Value() + 1,
                     bpmem.genMode.numindstages.Value(),
                     static_cast<int>(bpmem.genMode.cull_mode.Value()));
  out += fmt::format(
      "zmode test={} update={} func={} alpha_test hex={:08x} ref0={} ref1={} comp0={} comp1={} "
      "logic={}\n",
      bpmem.zmode.test_enable.Value(), bpmem.zmode.update_enable.Value(),
      bpmem.zmode.func.Value(), bpmem.alpha_test.hex, bpmem.alpha_test.ref0.Value(),
      bpmem.alpha_test.ref1.Value(), bpmem.alpha_test.comp0.Value(),
      bpmem.alpha_test.comp1.Value(), bpmem.alpha_test.logic.Value());
  out += fmt::format(
      "blend enable={} logic={} color_update={} alpha_update={} src={} dst={} subtract={} "
      "logic_mode={} dstalpha enable={} alpha={}\n",
      bpmem.blendmode.blend_enable.Value(), bpmem.blendmode.logic_op_enable.Value(),
      bpmem.blendmode.color_update.Value(), bpmem.blendmode.alpha_update.Value(),
      static_cast<int>(bpmem.blendmode.src_factor.Value()),
      static_cast<int>(bpmem.blendmode.dst_factor.Value()), bpmem.blendmode.subtract.Value(),
      static_cast<int>(bpmem.blendmode.logic_mode.Value()),
      bpmem.dstalpha.enable.Value(), bpmem.dstalpha.alpha.Value());

  out += "textures:\n";
  for (u32 i = 0; i < texture_hashes.size(); ++i)
  {
    if (texture_hashes[i] == 0 && texture_names[i].empty())
      continue;
    out += fmt::format("  stage{} hash={:016x} name={}\n", i, texture_hashes[i],
                       texture_names[i].empty() ? "(unknown)" : texture_names[i]);
  }

  out += "tev orders/selectors:\n";
  const u32 tev_stage_count = bpmem.genMode.numtevstages.Value() + 1;
  for (u32 stage = 0; stage < tev_stage_count; ++stage)
  {
    const auto& order = bpmem.tevorders[stage / 2];
    out += fmt::format(
        "  stage{} enabled={} texmap={} texcoord={} colorchan={} konst_color={} konst_alpha={} "
        "color_hex={:06x} alpha_hex={:06x}\n",
        stage, order.getEnable(stage & 1), order.getTexMap(stage & 1),
        order.getTexCoord(stage & 1), static_cast<int>(order.getColorChan(stage & 1)),
        bpmem.tevksel.GetKonstColor(stage), bpmem.tevksel.GetKonstAlpha(stage),
        bpmem.combiners[stage].colorC.hex, bpmem.combiners[stage].alphaC.hex);
  }

  out += "tev regs / konst colors:\n";
  for (u32 i = 0; i < 4; ++i)
  {
    const auto& reg = bpmem.tevregs[i];
    out += fmt::format(
        "  reg{} ra_type={} bg_type={} r={} g={} b={} a={} raw_ra={:06x} raw_bg={:06x}\n", i,
        reg.ra.type.Value(), reg.bg.type.Value(), reg.ra.red.Value(), reg.bg.green.Value(),
        reg.bg.blue.Value(), reg.ra.alpha.Value(), reg.ra.hex, reg.bg.hex);
  }

  out += "xf material/lighting:\n";
  for (u32 i = 0; i < NUM_XF_COLOR_CHANNELS; ++i)
  {
    out += fmt::format(
        "  chan{} amb={:08x} mat={:08x} color_hex={:08x} alpha_hex={:08x} color_lighting={} "
        "alpha_lighting={} color_lights={:02x} alpha_lights={:02x}\n",
        i, xfmem.ambColor[i], xfmem.matColor[i], xfmem.color[i].hex, xfmem.alpha[i].hex,
        xfmem.color[i].enablelighting.Value(), xfmem.alpha[i].enablelighting.Value(),
        xfmem.color[i].GetFullLightMask(), xfmem.alpha[i].GetFullLightMask());
  }

  AppendPrimedGunCannonProbeLog(out);
  if (s_primegun_cannon_probe_log_count == 1)
  {
    INFO_LOG_FMT(VIDEO, "PrimedGun cannon probe logging PS {:08x} to {}",
                 static_cast<u32>(PRIMEGUN_CANNON_PROBE_PS_HASH), PrimedGunCannonProbeLogPath());
  }
}

bool ShouldDisableOpenXRLegacyView(float vr_override)
{
  return !std::isnan(vr_override) &&
         (vr_override < -0.5f ||
          (vr_override > VR_STEREO_OVERRIDE_FULLSCREEN &&
           vr_override < 0.5f));
}

void AppendPrimedGunThermalHudLog(std::string_view text)
{
  if (s_primegun_thermal_hud_log_count >= PRIMEGUN_THERMAL_HUD_MAX_LOGS)
    return;

  ++s_primegun_thermal_hud_log_count;
  File::CreateFullPath(File::GetUserPath(D_LOGS_IDX));
  File::IOFile file(File::GetUserPath(D_LOGS_IDX) + "PrimedGunThermalHud.log", "ab");
  if (!file)
    return;
  file.WriteString(std::string(text));
}

void AppendPrimedGunElementHandlingLog(std::string_view text)
{
  if (s_primegun_element_handling_log_count >= PRIMEGUN_ELEMENT_HANDLING_MAX_LOGS)
    return;

  ++s_primegun_element_handling_log_count;
  File::CreateFullPath(File::GetUserPath(D_LOGS_IDX));
  File::IOFile file(File::GetUserPath(D_LOGS_IDX) + "PrimedGunElementHandling.log", "ab");
  if (!file)
    return;
  file.WriteString(std::string(text));
}

bool IsMetroidPrime1Profile(MetroidElementProfile profile)
{
  return profile == MetroidElementProfile::Prime1GC ||
         profile == MetroidElementProfile::Prime1Wii;
}

float GetPrimedGunPerspectiveHudDistance()
{
#ifdef ENABLE_VR
  return std::clamp(Common::VR::OpenXRInputState::GetPrimedGunOverlay().metroid_hud_distance,
                    0.1f, 3.0f);
#else
  return METROID_PRIME1_PERSPECTIVE_HUD_DEFAULT_DISTANCE;
#endif
}

float GetPrimedGunPerspectiveHudSize()
{
#ifdef ENABLE_VR
  return std::clamp(Common::VR::OpenXRInputState::GetPrimedGunOverlay().metroid_hud_size, 0.1f,
                    3.0f);
#else
  return METROID_PRIME1_PERSPECTIVE_HUD_DEFAULT_SIZE;
#endif
}

MetroidHydraHudSettings GetMetroidHydraHudSettings(MetroidElementProfile profile,
                                                   MetroidElementLayer layer)
{
  if (!IsMetroidPrime1Profile(profile))
    return {};

  switch (layer)
  {
  case MetroidElementLayer::HUD:
  case MetroidElementLayer::UnknownHUD:
  case MetroidElementLayer::MapOrHint:
  case MetroidElementLayer::Map:
    return {.enabled = true,
            .perspective_hud = true,
            .scale = 30.0f,
            .width = 0.79f,
            .height = 0.79f};

  case MetroidElementLayer::XRayHUD:
    return {.enabled = true,
            .perspective_hud = true,
            .scale = 30.0f,
            .width = 0.79f,
            .height = 0.79f};

  case MetroidElementLayer::DarkVisorHUD:
    return {.enabled = true,
            .perspective_hud = true,
            .scale = 5.0f,
            .width = 0.79f,
            .height = 0.79f};

  case MetroidElementLayer::VisorRadarHint:
    return {.enabled = true,
            .perspective_hud = true,
            .scale = 32.0f,
            .width = 0.79f,
            .height = 0.79f};

  case MetroidElementLayer::RadarDot:
    return {.enabled = true,
            .perspective_hud = true,
            .scale = 36.0f,
            .width = 0.79f,
            .height = 0.79f};

  case MetroidElementLayer::Visor:
    return {.enabled = true, .perspective_hud = true, .scale = 90.0f};

  case MetroidElementLayer::Helmet:
    return {.enabled = true,
            .perspective_hud = true,
            .scale = 100.0f,
            .width = 1.7f,
            .height = 1.7f};

  case MetroidElementLayer::ScanText:
    return {.enabled = true,
            .perspective_hud = true,
            .scale = 32.0f,
            .width = 0.8f,
            .height = 0.8f};

  case MetroidElementLayer::ScanHologram:
    return {.enabled = true,
            .perspective_hud = true,
            .scale = 40.0f,
            .width = 1.2f,
            .height = 1.2f};

  case MetroidElementLayer::ScanReticle:
    return {.enabled = true, .width = 1.0f, .height = 1.0f, .up = 0.14f, .right = 0.01f};

  default:
    return {};
  }
}

bool IsMetroidPerspectiveHudAnchorLayer(MetroidElementLayer layer)
{
  switch (layer)
  {
  case MetroidElementLayer::HUD:
  case MetroidElementLayer::UnknownHUD:
  case MetroidElementLayer::MapOrHint:
  case MetroidElementLayer::Map:
  case MetroidElementLayer::XRayHUD:
  case MetroidElementLayer::DarkVisorHUD:
  case MetroidElementLayer::Helmet:
    return true;

  default:
    return false;
  }
}

bool IsMetroidPrime1CombatContextLayer(MetroidElementLayer layer)
{
  switch (layer)
  {
  case MetroidElementLayer::Gun:
  case MetroidElementLayer::World:
  case MetroidElementLayer::Reticle:
  case MetroidElementLayer::WiiReticle:
  case MetroidElementLayer::XRayWorld:
  case MetroidElementLayer::ThermalGunAndDoor:
    return true;

  default:
    return false;
  }
}

bool IsMetroidPrime1MenuContextLayer(MetroidElementLayer layer)
{
  switch (layer)
  {
  case MetroidElementLayer::Map0:
  case MetroidElementLayer::Map1:
  case MetroidElementLayer::Map2:
  case MetroidElementLayer::Dialog:
  case MetroidElementLayer::MapMap:
  case MetroidElementLayer::MapLegend:
  case MetroidElementLayer::InventorySamus:
  case MetroidElementLayer::InventorySamusOutline:
  case MetroidElementLayer::MapNorth:
    return true;

  default:
    return false;
  }
}

bool IsMetroidPrime1CombatHudAnchorCandidate(MetroidElementLayer layer, float reference_view_z,
                                             int hud_context)
{
  return hud_context == METROID_HUD_CONTEXT_COMBAT && layer == MetroidElementLayer::ScanText &&
         std::isfinite(reference_view_z) && reference_view_z >= -22.0f &&
         reference_view_z <= -18.0f;
}

bool IsMetroidPrime1ScannerPreviewMask(const ShaderHunter::RuntimeElementSignature& signature)
{
  return signature.valid && !signature.perspective && signature.ortho_left_x100 == -32000 &&
         signature.ortho_right_x100 == 32000 && signature.ortho_top_x100 == 22400 &&
         signature.ortho_bottom_x100 == -22400 && signature.ortho_layer == 0 &&
         signature.viewport_x == 662 && signature.viewport_y == 566 &&
         signature.viewport_width == 320 && signature.viewport_height == 224 &&
         signature.scissor_left == 342 && signature.scissor_top == 342 &&
         signature.scissor_right == 981 && signature.scissor_bottom == 789 &&
         signature.alpha_test_hex == 0x007f0000 && signature.ztest && signature.zupdate &&
         signature.zfunc == 3 && signature.blend_color_update && signature.blend_alpha_update;
}

bool IsMetroidPrime1ThermalContextLayer(MetroidElementLayer layer)
{
  switch (layer)
  {
  case MetroidElementLayer::ThermalEffect:
  case MetroidElementLayer::ThermalEffectGun:
  case MetroidElementLayer::ThermalGunAndDoor:
    return true;

  default:
    return false;
  }
}

bool IsMetroidPrime1ThermalChargeBeamMaskDraw(
    const std::optional<ElementsGroupManager::DrawRecord>& draw)
{
  if (!draw || draw->profile_id != MetroidElementProfile::Prime1GC ||
      draw->profile_layer != MetroidElementLayer::ChargeBeamEffect)
  {
    return false;
  }

  const ShaderHunter::RuntimeElementSignature& signature = draw->signature;
  return signature.valid && !signature.perspective && signature.ortho_left_x100 == 0 &&
         signature.ortho_right_x100 == 64000 && signature.ortho_top_x100 == 0 &&
         signature.ortho_bottom_x100 == 44800 && signature.ortho_layer == 0 &&
         signature.viewport_x == 662 && signature.viewport_y == 566 &&
         signature.viewport_width == 320 && signature.viewport_height == 224 &&
         signature.scissor_left == 342 && signature.scissor_top == 342 &&
         signature.scissor_right == 981 && signature.scissor_bottom == 789 &&
         signature.alpha_test_hex == 0x007f0000 && !signature.ztest && !signature.zupdate &&
         signature.zfunc == 7 && signature.blend_color_update && signature.blend_alpha_update &&
         draw->textures[7] == METROID_PRIME1_THERMAL_MASK_STAGE7_HASH;
}

MetroidLayerBehavior GetMetroidLayerBehavior(MetroidElementLayer layer)
{
  switch (layer)
  {
  case MetroidElementLayer::EFBCopy:
  case MetroidElementLayer::BlackBars:
  case MetroidElementLayer::ScanVisor:
  case MetroidElementLayer::ScanDarken:
  case MetroidElementLayer::ScanHighlighter:
  case MetroidElementLayer::ScanBox:
  case MetroidElementLayer::ScanCircle:
  case MetroidElementLayer::ScanReticle:
    return {.skip = true};

  case MetroidElementLayer::Helmet:
  case MetroidElementLayer::HUD:
  case MetroidElementLayer::MorphballHUD:
  case MetroidElementLayer::XRayHUD:
  case MetroidElementLayer::DarkVisorHUD:
  case MetroidElementLayer::UnknownHUD:
  case MetroidElementLayer::VisorRadarHint:
  case MetroidElementLayer::RadarDot:
  case MetroidElementLayer::MorphballMapOrHint:
  case MetroidElementLayer::MapOrHint:
  case MetroidElementLayer::MorphballMap:
  case MetroidElementLayer::Map:
  case MetroidElementLayer::Map0:
  case MetroidElementLayer::Map1:
  case MetroidElementLayer::Map2:
  case MetroidElementLayer::Dialog:
  case MetroidElementLayer::MapMap:
  case MetroidElementLayer::MapLegend:
  case MetroidElementLayer::InventorySamus:
  case MetroidElementLayer::InventorySamusOutline:
  case MetroidElementLayer::MapNorth:
  case MetroidElementLayer::ScanText:
  case MetroidElementLayer::ScanHologram:
  case MetroidElementLayer::Visor:
  case MetroidElementLayer::VisorBootup:
  case MetroidElementLayer::UnknownVisor:
    return {.handling = ShaderHunter::HandlingType::HeadLocked};

  default:
    return {};
  }
}

bool ShouldSampleMetroidFullscreenEffectPerEye(
    const std::optional<ElementsGroupManager::DrawRecord>& draw,
    int thermal_effect_gun_draws_seen)
{
  if (!draw)
    return false;

  switch (draw->profile_layer)
  {
  case MetroidElementLayer::XRayEffect:
  case MetroidElementLayer::ThermalEffect:
  case MetroidElementLayer::ChargeBeamEffect:
    return true;
  case MetroidElementLayer::ThermalEffectGun:
    // The first ThermalEffectGun pass carries the live heat source. Later fullscreen
    // compositing passes freeze on the right eye if they sample with the active eye,
    // so keep their texture lookups pinned to layer 0.
    return thermal_effect_gun_draws_seen <= 1;
  default:
    return false;
  }
}

bool IsMetroidThermalBackdropDraw(const std::optional<ElementsGroupManager::DrawRecord>& draw)
{
  if (!draw || draw->profile_layer != MetroidElementLayer::ThermalEffectGun)
    return false;

  const ShaderHunter::RuntimeElementSignature& signature = draw->signature;
  return signature.valid && !signature.perspective && signature.viewport_x == 662 &&
         signature.viewport_y == 566 && signature.viewport_width == 320 &&
         signature.viewport_height == 224 && signature.scissor_left == 342 &&
         signature.scissor_top == 342 && signature.scissor_right == 981 &&
         signature.scissor_bottom == 789 && signature.alpha_test_hex == 0x003f0000 &&
         !signature.ztest && !signature.zupdate && signature.zfunc == 7 &&
         signature.blend_color_update && signature.blend_alpha_update;
}

bool IsMetroidThermalColorMaskDraw(
    const std::optional<ElementsGroupManager::DrawRecord>& draw)
{
  if (!draw || draw->profile_layer != MetroidElementLayer::ThermalEffectGun)
    return false;

  const ShaderHunter::RuntimeElementSignature& signature = draw->signature;
  return signature.valid && !signature.perspective && signature.viewport_x == 662 &&
         signature.viewport_y == 566 && signature.viewport_width == 320 &&
         signature.viewport_height == 224 && signature.scissor_left == 342 &&
         signature.scissor_top == 342 && signature.scissor_right == 981 &&
         signature.scissor_bottom == 789 && signature.alpha_test_hex == 0x003f0000 &&
         !signature.ztest && !signature.zupdate && signature.zfunc == 3 &&
         signature.blend_color_update && !signature.blend_alpha_update;
}

bool HasMetroidPrime1XRayEffectSignature(const ShaderHunter::RuntimeElementSignature& signature)
{
  const bool xray_alpha_test = signature.alpha_test_hex == 0x007f0000 ||
                               signature.alpha_test_hex == 0x003f0000;
  return signature.valid && !signature.perspective &&
         WithinElementSignatureTolerance(signature.ortho_left_x100, 0, 1) &&
         WithinElementSignatureTolerance(signature.ortho_right_x100,
                                         METROID_PRIME1_XRAY_ORTHO_RIGHT_X100, 100) &&
         WithinElementSignatureTolerance(signature.ortho_top_x100,
                                         METROID_PRIME1_XRAY_ORTHO_TOP_X100, 100) &&
         WithinElementSignatureTolerance(signature.ortho_bottom_x100, 0, 1) &&
         signature.ortho_layer == 0 && signature.viewport_x == 662 && signature.viewport_y == 566 &&
         signature.viewport_width == 320 && signature.viewport_height == 224 &&
         signature.scissor_left == 342 && signature.scissor_top == 342 &&
         signature.scissor_right == 981 && signature.scissor_bottom == 789 &&
         xray_alpha_test && !signature.ztest && !signature.zupdate && signature.zfunc == 3 &&
         signature.blend_color_update && signature.blend_alpha_update;
}

bool HasMetroidPrime1ThermalScissor(const ShaderHunter::RuntimeElementSignature& signature)
{
  return signature.scissor_left == 342 && signature.scissor_top == 342 &&
         signature.scissor_right == 981 && signature.scissor_bottom == 789;
}

bool HasMetroidPrime1MainViewport(const ShaderHunter::RuntimeElementSignature& signature)
{
  return signature.viewport_x == 662 && signature.viewport_y == 566 &&
         signature.viewport_width == 320 && signature.viewport_height == 224 &&
         HasMetroidPrime1ThermalScissor(signature);
}

bool HasBoundEfbCopyTexture()
{
  if (!g_texture_cache)
    return false;

  for (u32 stage = 0; stage < 8; ++stage)
  {
    if (g_texture_cache->IsBoundTextureEfbCopy(stage))
      return true;
  }

  return false;
}

bool HasBoundEfbCopyTextureAtStage(u32 stage)
{
  return g_texture_cache && g_texture_cache->IsBoundTextureEfbCopy(stage);
}

bool IsMetroidPrime1FramebufferEffectDraw(
    const std::optional<ElementsGroupManager::DrawRecord>& draw)
{
  if (!draw || !IsMetroidPrime1Profile(draw->profile_id))
    return false;

  const ShaderHunter::RuntimeElementSignature& signature = draw->signature;
  const bool fullscreen_efb_effect_shader =
      static_cast<u32>(draw->ps_hash) == METROID_PRIME1_FULLSCREEN_EFB_EFFECT_PS_HASH;
  if (!fullscreen_efb_effect_shader)
    return false;

  const bool efb_copy_source =
      HasBoundEfbCopyTextureAtStage(METROID_PRIME1_FULLSCREEN_EFB_EFFECT_SOURCE_STAGE) ||
      HasBoundEfbCopyTexture();
  return signature.valid && !signature.perspective && HasMetroidPrime1MainViewport(signature) &&
         signature.blend_color_update && efb_copy_source;
}

bool HasMetroidPrime1ThermalHeatProjection(
    const ShaderHunter::RuntimeElementSignature& signature)
{
  return signature.perspective &&
         WithinElementSignatureTolerance(signature.perspective_hfov_x100,
                                         METROID_PRIME1_THERMAL_HEAT_HFOV_X100, 1) &&
         WithinElementSignatureTolerance(signature.perspective_vfov_x100,
                                         METROID_PRIME1_THERMAL_HEAT_VFOV_X100, 1) &&
         WithinElementSignatureTolerance(signature.perspective_near_x1000,
                                         METROID_PRIME1_THERMAL_HEAT_NEAR_X1000, 1) &&
         WithinElementSignatureTolerance(signature.perspective_far_x100,
                                         METROID_PRIME1_THERMAL_HEAT_FAR_X100, 1);
}

bool IsMetroidPrime1ThermalColorMaskStereoSourceBound()
{
  return g_texture_cache &&
         g_texture_cache->GetBoundTextureNativeWidth(
             METROID_PRIME1_THERMAL_COLOR_MASK_SOURCE_STAGE) == 640 &&
         g_texture_cache->GetBoundTextureNativeHeight(
             METROID_PRIME1_THERMAL_COLOR_MASK_SOURCE_STAGE) == 448 &&
         g_texture_cache->GetBoundTextureLayers(METROID_PRIME1_THERMAL_COLOR_MASK_SOURCE_STAGE) >=
             2;
}

bool IsMetroidPrime1ThermalHeatEmitterDraw(
    const std::optional<ElementsGroupManager::DrawRecord>& draw)
{
  if (!draw || draw->profile_id != MetroidElementProfile::Prime1GC)
    return false;

  const ShaderHunter::RuntimeElementSignature& signature = draw->signature;
  return signature.valid && HasMetroidPrime1ThermalHeatProjection(signature) &&
         HasMetroidPrime1ThermalScissor(signature) && signature.alpha_test_hex == 0x003f0000 &&
         signature.ztest && signature.zupdate && signature.zfunc == 3 &&
         signature.blend_color_update && signature.blend_alpha_update;
}

bool IsMetroidPrime1XRayEffectDraw(const std::optional<ElementsGroupManager::DrawRecord>& draw)
{
  if (!draw || draw->profile_id != MetroidElementProfile::Prime1GC)
    return false;

  if (draw->profile_layer == MetroidElementLayer::XRayEffect)
    return true;

  return HasMetroidPrime1XRayEffectSignature(draw->signature);
}

bool IsMetroidPrime1XRayHUDDraw(const std::optional<ElementsGroupManager::DrawRecord>& draw)
{
  return draw && draw->profile_id == MetroidElementProfile::Prime1GC &&
         draw->profile_layer == MetroidElementLayer::XRayHUD;
}

bool ShouldLogMetroidPrime1XRayDraw(const std::optional<ElementsGroupManager::DrawRecord>& draw)
{
  if (!draw)
    return false;

  if (draw->profile_layer == MetroidElementLayer::XRayEffect ||
      draw->profile_layer == MetroidElementLayer::XRayHUD)
    return true;

  return draw->profile_id == MetroidElementProfile::Prime1GC &&
         HasMetroidPrime1XRayEffectSignature(draw->signature);
}

void LogMetroidPrime1XRayDraw(u32 draw_counter,
                              const std::optional<ElementsGroupManager::DrawRecord>& draw,
                              ShaderHunter::HandlingType handling, u64 vs_hash, u64 ps_hash,
                              u64 gs_hash, int forced_texture_layer,
                              bool signature_fallback, bool d3d_tex0_layer_fallback)
{
  if (!ShouldLogMetroidPrime1XRayDraw(draw))
    return;

  const ShaderHunter::RuntimeElementSignature& signature = draw->signature;
  INFO_LOG_FMT(
      VIDEO,
      "VR_XRAY_DBG #{}: backend={} profile={} profile_layer={} handling={} tex_layer={} "
      "signature_fallback={} d3d_tex0_layer_fallback={} xray_sig_match={} proj={} "
      "persp(h={} v={} n={} f={}) ortho(l={} r={} t={} b={}) VP({},{} {}x{}) "
      "SC({},{} {},{}) alpha={:08x} zt={} zupd={} zf={} blend(col={},alpha={}) "
      "VS={:08x} PS={:08x} GS={:08x} "
      "t0({}x{} layers={} hash={:016x}) t1({}x{} layers={} hash={:016x}) "
      "t2({}x{} layers={} hash={:016x}) t3({}x{} layers={} hash={:016x}) "
      "t7({}x{} layers={} hash={:016x})",
      draw_counter, g_backend_info.api_type == APIType::D3D ? "d3d" : "other",
      draw->profile_id == MetroidElementProfile::Prime1GC ? "mp1gc" : "other",
      draw->profile_layer_name, HandlingToDebugName(handling), forced_texture_layer,
      signature_fallback, d3d_tex0_layer_fallback,
      HasMetroidPrime1XRayEffectSignature(signature),
      signature.perspective ? "persp" : "ortho", signature.perspective_hfov_x100,
      signature.perspective_vfov_x100, signature.perspective_near_x1000,
      signature.perspective_far_x100, signature.ortho_left_x100, signature.ortho_right_x100,
      signature.ortho_top_x100, signature.ortho_bottom_x100, signature.viewport_x,
      signature.viewport_y, signature.viewport_width, signature.viewport_height,
      signature.scissor_left, signature.scissor_top, signature.scissor_right,
      signature.scissor_bottom, signature.alpha_test_hex, signature.ztest, signature.zupdate,
      signature.zfunc, signature.blend_color_update, signature.blend_alpha_update, vs_hash,
      ps_hash, gs_hash, g_texture_cache ? g_texture_cache->GetBoundTextureNativeWidth(0) : 0,
      g_texture_cache ? g_texture_cache->GetBoundTextureNativeHeight(0) : 0,
      g_texture_cache ? g_texture_cache->GetBoundTextureLayers(0) : 0,
      g_texture_cache ? g_texture_cache->GetBoundTextureHash(0) : 0,
      g_texture_cache ? g_texture_cache->GetBoundTextureNativeWidth(1) : 0,
      g_texture_cache ? g_texture_cache->GetBoundTextureNativeHeight(1) : 0,
      g_texture_cache ? g_texture_cache->GetBoundTextureLayers(1) : 0,
      g_texture_cache ? g_texture_cache->GetBoundTextureHash(1) : 0,
      g_texture_cache ? g_texture_cache->GetBoundTextureNativeWidth(2) : 0,
      g_texture_cache ? g_texture_cache->GetBoundTextureNativeHeight(2) : 0,
      g_texture_cache ? g_texture_cache->GetBoundTextureLayers(2) : 0,
      g_texture_cache ? g_texture_cache->GetBoundTextureHash(2) : 0,
      g_texture_cache ? g_texture_cache->GetBoundTextureNativeWidth(3) : 0,
      g_texture_cache ? g_texture_cache->GetBoundTextureNativeHeight(3) : 0,
      g_texture_cache ? g_texture_cache->GetBoundTextureLayers(3) : 0,
      g_texture_cache ? g_texture_cache->GetBoundTextureHash(3) : 0,
      g_texture_cache ? g_texture_cache->GetBoundTextureNativeWidth(7) : 0,
      g_texture_cache ? g_texture_cache->GetBoundTextureNativeHeight(7) : 0,
      g_texture_cache ? g_texture_cache->GetBoundTextureLayers(7) : 0,
      g_texture_cache ? g_texture_cache->GetBoundTextureHash(7) : 0);
}

void LogPrimedGunElementHandling(u32 draw_counter,
                                 const std::optional<ElementsGroupManager::DrawRecord>& draw,
                                 ShaderHunter::HandlingType handling, u64 vs_hash, u64 ps_hash,
                                  u64 gs_hash, const std::array<u64, 8>& texture_hashes,
                                  int forced_texture_layer, float stereo_override)
{
  if (!draw || draw->profile_id != MetroidElementProfile::Prime1GC)
    return;

  const bool interesting_handling = handling == ShaderHunter::HandlingType::Screen ||
                                    handling == ShaderHunter::HandlingType::Fullscreen ||
                                    handling == ShaderHunter::HandlingType::FullscreenMono ||
                                    handling == ShaderHunter::HandlingType::HeadLocked;
  const bool interesting_layer =
      draw->profile_layer == MetroidElementLayer::ScreenFade ||
      draw->profile_layer == MetroidElementLayer::ScreenOverlay ||
      draw->profile_layer == MetroidElementLayer::XRayEffect ||
      draw->profile_layer == MetroidElementLayer::VisorDirt ||
      draw->profile_layer == MetroidElementLayer::ThermalEffect ||
      draw->profile_layer == MetroidElementLayer::ThermalEffectGun ||
      draw->profile_layer == MetroidElementLayer::ChargeBeamEffect ||
      draw->profile_layer == MetroidElementLayer::ScanDarken ||
      draw->profile_layer == MetroidElementLayer::ScanHighlighter ||
      draw->profile_layer == MetroidElementLayer::ScanReticle ||
      draw->profile_layer == MetroidElementLayer::ScanText ||
      draw->profile_layer == MetroidElementLayer::XRayHUD ||
      draw->profile_layer == MetroidElementLayer::HUD ||
      draw->profile_layer == MetroidElementLayer::UnknownHUD;
  if (!interesting_layer && handling == ShaderHunter::HandlingType::Screen)
    return;
  if (!interesting_handling && !interesting_layer)
    return;

  const auto& s = draw->signature;
  AppendPrimedGunElementHandlingLog(fmt::format(
      "element-handling draw={} layer={} handling={} tex_layer={} proj={} "
      "override={} "
      "persp(h={} v={} n={} f={}) ortho(l={} r={} t={} b={} layer={}) "
      "vp=({},{} {}x{}) sc=({},{} {},{}) alpha={:08x} ztest={} zupdate={} zfunc={} "
      "blend(col={} alpha={}) VS={:08x} PS={:08x} GS={:08x} "
      "t0={:016x} t1={:016x} t2={:016x} t3={:016x} t7={:016x}\n",
      draw_counter, draw->profile_layer_name, HandlingToDebugName(handling),
      forced_texture_layer, s.perspective ? "perspective" : "ortho",
      std::isnan(stereo_override) ? 999.0f : stereo_override,
      s.perspective_hfov_x100, s.perspective_vfov_x100, s.perspective_near_x1000,
      s.perspective_far_x100, s.ortho_left_x100, s.ortho_right_x100,
      s.ortho_top_x100, s.ortho_bottom_x100, s.ortho_layer, s.viewport_x, s.viewport_y,
      s.viewport_width, s.viewport_height, s.scissor_left, s.scissor_top, s.scissor_right,
      s.scissor_bottom, s.alpha_test_hex, s.ztest, s.zupdate, s.zfunc,
      s.blend_color_update, s.blend_alpha_update, static_cast<u32>(vs_hash),
      static_cast<u32>(ps_hash), static_cast<u32>(gs_hash), texture_hashes[0],
      texture_hashes[1], texture_hashes[2], texture_hashes[3], texture_hashes[7]));
}

ShaderHunter::RuntimeElementSignature BuildRuntimeElementSignature(const XFMemory& xf_memory,
                                                                   const BPMemory& bp_memory,
                                                                   int ortho_layer)
{
  ShaderHunter::RuntimeElementSignature signature;
  signature.valid = true;
  signature.perspective = xf_memory.projection.type == ProjectionType::Perspective;

  const float* projection = xf_memory.projection.rawProjection.data();
  if (signature.perspective)
  {
    const float hfov = 2.0f * std::atan(1.0f / projection[0]) * 180.0f / 3.14159265f;
    const float vfov = 2.0f * std::atan(1.0f / projection[2]) * 180.0f / 3.14159265f;
    const float far_plane = projection[5] / projection[4];
    const float near_plane = far_plane * projection[4] / (projection[4] - 1.0f);
    signature.perspective_hfov_x100 = static_cast<int>(std::lround(hfov * 100.0f));
    signature.perspective_vfov_x100 = static_cast<int>(std::lround(vfov * 100.0f));
    signature.perspective_near_x1000 = static_cast<int>(std::lround(near_plane * 1000.0f));
    signature.perspective_far_x100 = static_cast<int>(std::lround(far_plane * 100.0f));
  }
  else
  {
    const float left = -(projection[1] + 1.0f) / projection[0];
    const float right = left + 2.0f / projection[0];
    const float bottom = -(projection[3] + 1.0f) / projection[2];
    const float top = bottom + 2.0f / projection[2];
    signature.ortho_left_x100 = static_cast<int>(std::lround(left * 100.0f));
    signature.ortho_right_x100 = static_cast<int>(std::lround(right * 100.0f));
    signature.ortho_top_x100 = static_cast<int>(std::lround(top * 100.0f));
    signature.ortho_bottom_x100 = static_cast<int>(std::lround(bottom * 100.0f));
    signature.ortho_layer = ortho_layer;
  }

  const auto& viewport = xf_memory.viewport;
  signature.viewport_x = static_cast<int>(std::lround(viewport.xOrig));
  signature.viewport_y = static_cast<int>(std::lround(viewport.yOrig));
  signature.viewport_width = static_cast<int>(std::lround(std::abs(viewport.wd)));
  signature.viewport_height = static_cast<int>(std::lround(std::abs(viewport.ht)));
  signature.scissor_left = bp_memory.scissorTL.x;
  signature.scissor_top = bp_memory.scissorTL.y;
  signature.scissor_right = bp_memory.scissorBR.x;
  signature.scissor_bottom = bp_memory.scissorBR.y;
  signature.alpha_test_hex = bp_memory.alpha_test.hex;
  signature.ztest = bp_memory.zmode.test_enable != 0;
  signature.zupdate = bp_memory.zmode.update_enable != 0;
  signature.zfunc = static_cast<int>(bp_memory.zmode.func.Value());
  signature.blend_color_update = bp_memory.blendmode.color_update != 0;
  signature.blend_alpha_update = bp_memory.blendmode.alpha_update != 0;
  return signature;
}

MetroidProjectionMetrics BuildMetroidProjectionMetrics(const XFMemory& xf_memory,
                                                       u32 projection_sequence)
{
  MetroidProjectionMetrics metrics;
  metrics.projection_sequence = projection_sequence;
  metrics.perspective = xf_memory.projection.type == ProjectionType::Perspective;

  const float* projection = xf_memory.projection.rawProjection.data();
  if (metrics.perspective)
  {
    metrics.hfov = 2.0f * std::atan(1.0f / projection[0]) * 180.0f / 3.14159265f;
    metrics.vfov = 2.0f * std::atan(1.0f / projection[2]) * 180.0f / 3.14159265f;
    metrics.zfar = projection[5] / projection[4];
    metrics.znear = metrics.zfar * projection[4] / (projection[4] - 1.0f);
  }
  else
  {
    metrics.left = -(projection[1] + 1.0f) / projection[0];
    metrics.right = metrics.left + 2.0f / projection[0];
    metrics.bottom = -(projection[3] + 1.0f) / projection[2];
    metrics.top = metrics.bottom + 2.0f / projection[2];
    metrics.zfar = projection[5] / projection[4];
    metrics.znear = (1.0f + projection[4] * metrics.zfar) / projection[4];
  }
  return metrics;
}
}  // namespace

// GX primitive -> RenderState primitive, no primitive restart
constexpr Common::EnumMap<PrimitiveType, Primitive::GX_DRAW_POINTS> primitive_from_gx{
    PrimitiveType::Triangles,  // GX_DRAW_QUADS
    PrimitiveType::Triangles,  // GX_DRAW_QUADS_2
    PrimitiveType::Triangles,  // GX_DRAW_TRIANGLES
    PrimitiveType::Triangles,  // GX_DRAW_TRIANGLE_STRIP
    PrimitiveType::Triangles,  // GX_DRAW_TRIANGLE_FAN
    PrimitiveType::Lines,      // GX_DRAW_LINES
    PrimitiveType::Lines,      // GX_DRAW_LINE_STRIP
    PrimitiveType::Points,     // GX_DRAW_POINTS
};

// GX primitive -> RenderState primitive, using primitive restart
constexpr Common::EnumMap<PrimitiveType, Primitive::GX_DRAW_POINTS> primitive_from_gx_pr{
    PrimitiveType::TriangleStrip,  // GX_DRAW_QUADS
    PrimitiveType::TriangleStrip,  // GX_DRAW_QUADS_2
    PrimitiveType::TriangleStrip,  // GX_DRAW_TRIANGLES
    PrimitiveType::TriangleStrip,  // GX_DRAW_TRIANGLE_STRIP
    PrimitiveType::TriangleStrip,  // GX_DRAW_TRIANGLE_FAN
    PrimitiveType::Lines,          // GX_DRAW_LINES
    PrimitiveType::Lines,          // GX_DRAW_LINE_STRIP
    PrimitiveType::Points,         // GX_DRAW_POINTS
};

// Due to the BT.601 standard which the GameCube is based on being a compromise
// between PAL and NTSC, neither standard gets square pixels. They are each off
// by ~9% in opposite directions.
// Just in case any game decides to take this into account, we do both these
// tests with a large amount of slop.

static float CalculateProjectionViewportRatio(const Projection::Raw& projection,
                                              const Viewport& viewport)
{
  const float projection_ar = projection[2] / projection[0];
  const float viewport_ar = viewport.wd / viewport.ht;

  return std::abs(projection_ar / viewport_ar);
}

static bool IsAnamorphicProjection(const Projection::Raw& projection, const Viewport& viewport,
                                   const VideoConfig& config)
{
  // If ratio between our projection and viewport aspect ratios is similar to 16:9 / 4:3
  // we have an anamorphic projection. This value can be overridden by a GameINI.
  // Game cheats that change the aspect ratio to natively unsupported ones
  // won't be automatically recognized here.

  return std::abs(CalculateProjectionViewportRatio(projection, viewport) -
                  config.widescreen_heuristic_widescreen_ratio) <
         config.widescreen_heuristic_aspect_ratio_slop;
}

static bool IsNormalProjection(const Projection::Raw& projection, const Viewport& viewport,
                               const VideoConfig& config)
{
  return std::abs(CalculateProjectionViewportRatio(projection, viewport) -
                  config.widescreen_heuristic_standard_ratio) <
         config.widescreen_heuristic_aspect_ratio_slop;
}

VertexManagerBase::VertexManagerBase()
    : m_cpu_vertex_buffer(MAXVBUFFERSIZE), m_cpu_index_buffer(MAXIBUFFERSIZE)
{
}

VertexManagerBase::~VertexManagerBase() = default;

bool VertexManagerBase::Initialize()
{
  auto& video_events = GetVideoEvents();

  m_frame_end_event =
      video_events.after_frame_event.Register([this](Core::System&) { OnEndFrame(); });
  m_after_present_event = video_events.after_present_event.Register(
      [this](const PresentInfo& pi) { m_ticks_elapsed = pi.emulated_timestamp; });
  m_index_generator.Init();
  m_custom_shader_cache = std::make_unique<CustomShaderCache>();
  m_cpu_cull.Init();
  return true;
}

u32 VertexManagerBase::GetRemainingSize() const
{
  return static_cast<u32>(m_end_buffer_pointer - m_cur_buffer_pointer);
}

void VertexManagerBase::AddIndices(OpcodeDecoder::Primitive primitive, u32 num_vertices)
{
  m_index_generator.AddIndices(primitive, num_vertices);
}

bool VertexManagerBase::AreAllVerticesCulled(VertexLoaderBase* loader,
                                             OpcodeDecoder::Primitive primitive, const u8* src,
                                             u32 count)
{
  return m_cpu_cull.AreAllVerticesCulled(loader, primitive, src, count);
}

DataReader VertexManagerBase::PrepareForAdditionalData(OpcodeDecoder::Primitive primitive,
                                                       u32 count, u32 stride, bool cullall)
{
  // Flush all EFB pokes. Since the buffer is shared, we can't draw pokes+primitives concurrently.
  g_framebuffer_manager->FlushEFBPokes();

  // The SSE vertex loader can write up to 4 bytes past the end
  u32 const needed_vertex_bytes = count * stride + 4;

  // We can't merge different kinds of primitives, so we have to flush here
  PrimitiveType new_primitive_type = g_backend_info.bSupportsPrimitiveRestart ?
                                         primitive_from_gx_pr[primitive] :
                                         primitive_from_gx[primitive];
  if (m_current_primitive_type != new_primitive_type) [[unlikely]]
  {
    Flush();

    // Have to update the rasterization state for point/line cull modes.
    m_current_primitive_type = new_primitive_type;
    SetRasterizationStateChanged();
  }

  u32 remaining_indices = GetRemainingIndices(primitive);
  u32 remaining_index_generator_indices = m_index_generator.GetRemainingIndices(primitive);

  // Check for size in buffer, if the buffer gets full, call Flush()
  if (!m_is_flushed && (count > remaining_index_generator_indices || count > remaining_indices ||
                        needed_vertex_bytes > GetRemainingSize())) [[unlikely]]
  {
    Flush();
  }

  m_cull_all = cullall;

  // need to alloc new buffer
  if (m_is_flushed) [[unlikely]]
  {
    if (cullall)
    {
      // This buffer isn't getting sent to the GPU. Just allocate it on the cpu.
      m_cur_buffer_pointer = m_base_buffer_pointer = m_cpu_vertex_buffer.data();
      m_end_buffer_pointer = m_base_buffer_pointer + m_cpu_vertex_buffer.size();
      m_index_generator.Start(m_cpu_index_buffer.data());
    }
    else
    {
      ResetBuffer(stride);
    }

    remaining_index_generator_indices = m_index_generator.GetRemainingIndices(primitive);
    remaining_indices = GetRemainingIndices(primitive);
    m_is_flushed = false;
  }

  // Now that we've reset the buffer, there should be enough space. It's possible that we still
  // won't have enough space in a few rare cases, such as vertex shader line/point expansion with a
  // ton of lines in one draw command, in which case we will either need to add support for
  // splitting a single draw command into multiple draws or using bigger indices.
  ASSERT_MSG(VIDEO, count <= remaining_index_generator_indices,
             "VertexManager: Too few remaining index values ({} > {}). "
             "32-bit indices or primitive breaking needed.",
             count, remaining_index_generator_indices);
  ASSERT_MSG(VIDEO, count <= remaining_indices,
             "VertexManager: Buffer not large enough for all indices! ({} > {}) "
             "Increase MAXIBUFFERSIZE or we need primitive breaking after all.",
             count, remaining_indices);
  ASSERT_MSG(VIDEO, needed_vertex_bytes <= GetRemainingSize(),
             "VertexManager: Buffer not large enough for all vertices! ({} > {}) "
             "Increase MAXVBUFFERSIZE or we need primitive breaking after all.",
             needed_vertex_bytes, GetRemainingSize());

  return DataReader(m_cur_buffer_pointer, m_end_buffer_pointer);
}

DataReader VertexManagerBase::DisableCullAll(u32 stride)
{
  if (m_cull_all)
  {
    m_cull_all = false;
    ResetBuffer(stride);
  }
  return DataReader(m_cur_buffer_pointer, m_end_buffer_pointer);
}

void VertexManagerBase::FlushData(u32 count, u32 stride)
{
  m_cur_buffer_pointer += count * stride;
}

u32 VertexManagerBase::GetRemainingIndices(OpcodeDecoder::Primitive primitive) const
{
  const u32 index_len = MAXIBUFFERSIZE - m_index_generator.GetIndexLen();

  if (primitive >= Primitive::GX_DRAW_LINES)
  {
    if (g_Config.UseVSForLinePointExpand())
    {
      if (g_backend_info.bSupportsPrimitiveRestart)
      {
        switch (primitive)
        {
        case Primitive::GX_DRAW_LINES:
          return index_len / 5 * 2;
        case Primitive::GX_DRAW_LINE_STRIP:
          return index_len / 5 + 1;
        case Primitive::GX_DRAW_POINTS:
          return index_len / 5;
        default:
          return 0;
        }
      }
      else
      {
        switch (primitive)
        {
        case Primitive::GX_DRAW_LINES:
          return index_len / 6 * 2;
        case Primitive::GX_DRAW_LINE_STRIP:
          return index_len / 6 + 1;
        case Primitive::GX_DRAW_POINTS:
          return index_len / 6;
        default:
          return 0;
        }
      }
    }
    else
    {
      switch (primitive)
      {
      case Primitive::GX_DRAW_LINES:
        return index_len;
      case Primitive::GX_DRAW_LINE_STRIP:
        return index_len / 2 + 1;
      case Primitive::GX_DRAW_POINTS:
        return index_len;
      default:
        return 0;
      }
    }
  }
  else if (g_backend_info.bSupportsPrimitiveRestart)
  {
    switch (primitive)
    {
    case Primitive::GX_DRAW_QUADS:
    case Primitive::GX_DRAW_QUADS_2:
      return index_len / 5 * 4;
    case Primitive::GX_DRAW_TRIANGLES:
      return index_len / 4 * 3;
    case Primitive::GX_DRAW_TRIANGLE_STRIP:
      return index_len / 1 - 1;
    case Primitive::GX_DRAW_TRIANGLE_FAN:
      return index_len / 6 * 4 + 1;
    default:
      return 0;
    }
  }
  else
  {
    switch (primitive)
    {
    case Primitive::GX_DRAW_QUADS:
    case Primitive::GX_DRAW_QUADS_2:
      return index_len / 6 * 4;
    case Primitive::GX_DRAW_TRIANGLES:
      return index_len;
    case Primitive::GX_DRAW_TRIANGLE_STRIP:
      return index_len / 3 + 2;
    case Primitive::GX_DRAW_TRIANGLE_FAN:
      return index_len / 3 + 2;
    default:
      return 0;
    }
  }
}

auto VertexManagerBase::ResetFlushAspectRatioCount() -> FlushStatistics
{
  const auto result = m_flush_statistics;
  m_flush_statistics = {};
  return result;
}

void VertexManagerBase::ResetBuffer(u32 vertex_stride)
{
  m_base_buffer_pointer = m_cpu_vertex_buffer.data();
  m_cur_buffer_pointer = m_cpu_vertex_buffer.data();
  m_end_buffer_pointer = m_base_buffer_pointer + m_cpu_vertex_buffer.size();
  m_index_generator.Start(m_cpu_index_buffer.data());
}

void VertexManagerBase::CommitBuffer(u32 num_vertices, u32 vertex_stride, u32 num_indices,
                                     u32* out_base_vertex, u32* out_base_index)
{
  *out_base_vertex = 0;
  *out_base_index = 0;
}

void VertexManagerBase::DrawCurrentBatch(u32 base_index, u32 num_indices, u32 base_vertex)
{
  // If bounding box is enabled, we need to flush any changes first, then invalidate what we have.
  if (g_bounding_box->IsEnabled() && g_ActiveConfig.bBBoxEnable && g_backend_info.bSupportsBBox)
  {
    g_bounding_box->Flush();
  }

  g_gfx->DrawIndexed(base_index, num_indices, base_vertex);
}

void VertexManagerBase::UploadUniforms()
{
}

void VertexManagerBase::InvalidateConstants()
{
  auto& system = Core::System::GetInstance();
  auto& vertex_shader_manager = system.GetVertexShaderManager();
  auto& geometry_shader_manager = system.GetGeometryShaderManager();
  auto& pixel_shader_manager = system.GetPixelShaderManager();
  vertex_shader_manager.dirty = true;
  geometry_shader_manager.dirty = true;
  pixel_shader_manager.dirty = true;
}

void VertexManagerBase::UploadUtilityUniforms(const void* uniforms, u32 uniforms_size)
{
}

void VertexManagerBase::UploadUtilityVertices(const void* vertices, u32 vertex_stride,
                                              u32 num_vertices, const u16* indices, u32 num_indices,
                                              u32* out_base_vertex, u32* out_base_index)
{
  // The GX vertex list should be flushed before any utility draws occur.
  ASSERT(m_is_flushed);

  // Copy into the buffers usually used for GX drawing.
  ResetBuffer(std::max(vertex_stride, 1u));
  if (vertices)
  {
    const u32 copy_size = vertex_stride * num_vertices;
    ASSERT((m_cur_buffer_pointer + copy_size) <= m_end_buffer_pointer);
    std::memcpy(m_cur_buffer_pointer, vertices, copy_size);
    m_cur_buffer_pointer += copy_size;
  }
  if (indices)
    m_index_generator.AddExternalIndices(indices, num_indices, num_vertices);

  CommitBuffer(num_vertices, vertex_stride, num_indices, out_base_vertex, out_base_index);
}

u32 VertexManagerBase::GetTexelBufferElementSize(TexelBufferFormat buffer_format)
{
  // R8 - 1, R16 - 2, RGBA8 - 4, R32G32 - 8
  return 1u << static_cast<u32>(buffer_format);
}

bool VertexManagerBase::UploadTexelBuffer(const void* data, u32 data_size, TexelBufferFormat format,
                                          u32* out_offset)
{
  return false;
}

bool VertexManagerBase::UploadTexelBuffer(const void* data, u32 data_size, TexelBufferFormat format,
                                          u32* out_offset, const void* palette_data,
                                          u32 palette_size, TexelBufferFormat palette_format,
                                          u32* palette_offset)
{
  return false;
}

BitSet32 VertexManagerBase::UsedTextures() const
{
  BitSet32 usedtextures;
  for (u32 i = 0; i < bpmem.genMode.numtevstages + 1u; ++i)
    if (bpmem.tevorders[i / 2].getEnable(i & 1))
      usedtextures[bpmem.tevorders[i / 2].getTexMap(i & 1)] = true;

  if (bpmem.genMode.numindstages > 0)
    for (unsigned int i = 0; i < bpmem.genMode.numtevstages + 1u; ++i)
      if (bpmem.tevind[i].IsActive() && bpmem.tevind[i].bt < bpmem.genMode.numindstages)
        usedtextures[bpmem.tevindref.getTexMap(bpmem.tevind[i].bt)] = true;

  return usedtextures;
}

void VertexManagerBase::Flush()
{
  if (m_is_flushed)
    return;

  m_is_flushed = true;

  if (m_draw_counter == 0)
  {
    // This is more or less the start of the Frame
    GetVideoEvents().before_frame_event.Trigger();
  }

  if (xfmem.numTexGen.numTexGens != bpmem.genMode.numtexgens ||
      xfmem.numChan.numColorChans != bpmem.genMode.numcolchans)
  {
    ERROR_LOG_FMT(
        VIDEO,
        "Mismatched configuration between XF and BP stages - {}/{} texgens, {}/{} colors. "
        "Skipping draw. Please report on the issue tracker.",
        xfmem.numTexGen.numTexGens, bpmem.genMode.numtexgens.Value(), xfmem.numChan.numColorChans,
        bpmem.genMode.numcolchans.Value());

    // Analytics reporting so we can discover which games have this problem, that way when we
    // eventually simulate the behavior we have test cases for it.
    if (xfmem.numTexGen.numTexGens != bpmem.genMode.numtexgens)
    {
      DolphinAnalytics::Instance().ReportGameQuirk(GameQuirk::MismatchedGPUTexGensBetweenXFAndBP);
    }
    if (xfmem.numChan.numColorChans != bpmem.genMode.numcolchans)
    {
      DolphinAnalytics::Instance().ReportGameQuirk(GameQuirk::MismatchedGPUColorsBetweenXFAndBP);
    }

    HideObjectEngine::Engine::GetInstance().DiscardPendingCapturedPrefixes();
    return;
  }

#if defined(_DEBUG) || defined(DEBUGFAST)
  PRIM_LOG("frame{}:\n texgen={}, numchan={}, dualtex={}, ztex={}, cole={}, alpe={}, ze={}",
           g_ActiveConfig.iSaveTargetId, xfmem.numTexGen.numTexGens, xfmem.numChan.numColorChans,
           xfmem.dualTexTrans.enabled, bpmem.ztex2.op.Value(), bpmem.blendmode.color_update.Value(),
           bpmem.blendmode.alpha_update.Value(), bpmem.zmode.update_enable.Value());

  for (u32 i = 0; i < xfmem.numChan.numColorChans; ++i)
  {
    LitChannel* ch = &xfmem.color[i];
    PRIM_LOG("colchan{}: matsrc={}, light={:#x}, ambsrc={}, diffunc={}, attfunc={}", i,
             ch->matsource.Value(), ch->GetFullLightMask(), ch->ambsource.Value(),
             ch->diffusefunc.Value(), ch->attnfunc.Value());
    ch = &xfmem.alpha[i];
    PRIM_LOG("alpchan{}: matsrc={}, light={:#x}, ambsrc={}, diffunc={}, attfunc={}", i,
             ch->matsource.Value(), ch->GetFullLightMask(), ch->ambsource.Value(),
             ch->diffusefunc.Value(), ch->attnfunc.Value());
  }

  for (u32 i = 0; i < xfmem.numTexGen.numTexGens; ++i)
  {
    TexMtxInfo tinfo = xfmem.texMtxInfo[i];
    if (tinfo.texgentype != TexGenType::EmbossMap)
      tinfo.hex &= 0x7ff;
    if (tinfo.texgentype != TexGenType::Regular)
      tinfo.projection = TexSize::ST;

    PRIM_LOG("txgen{}: proj={}, input={}, gentype={}, srcrow={}, embsrc={}, emblght={}, "
             "postmtx={}, postnorm={}",
             i, tinfo.projection.Value(), tinfo.inputform.Value(), tinfo.texgentype.Value(),
             tinfo.sourcerow.Value(), tinfo.embosssourceshift.Value(),
             tinfo.embosslightshift.Value(), xfmem.postMtxInfo[i].index.Value(),
             xfmem.postMtxInfo[i].normalize.Value());
  }

  PRIM_LOG("pixel: tev={}, ind={}, texgen={}, dstalpha={}, alphatest={:#x}",
           bpmem.genMode.numtevstages.Value() + 1, bpmem.genMode.numindstages.Value(),
           bpmem.genMode.numtexgens.Value(), bpmem.dstalpha.enable.Value(),
           (bpmem.alpha_test.hex >> 16) & 0xff);
#endif

  // Track some stats used elsewhere by the anamorphic widescreen heuristic.
  auto& system = Core::System::GetInstance();
  if (!system.IsWii())
  {
    const bool is_perspective = xfmem.projection.type == ProjectionType::Perspective;

    auto& counts =
        is_perspective ? m_flush_statistics.perspective : m_flush_statistics.orthographic;

    const auto& projection = xfmem.projection.rawProjection;
    // TODO: Potentially the viewport size could be used as weight for the flush count average.
    // This way a small minimap would have less effect than a fullscreen projection.
    const auto& viewport = xfmem.viewport;

    // FYI: This average is based on flushes.
    // It doesn't look at vertex counts like the heuristic does.
    counts.average_ratio.Push(CalculateProjectionViewportRatio(projection, viewport));

    if (IsAnamorphicProjection(projection, viewport, g_ActiveConfig))
    {
      ++counts.anamorphic_flush_count;
      counts.anamorphic_vertex_count += m_index_generator.GetIndexLen();
    }
    else if (IsNormalProjection(projection, viewport, g_ActiveConfig))
    {
      ++counts.normal_flush_count;
      counts.normal_vertex_count += m_index_generator.GetIndexLen();
    }
    else
    {
      ++counts.other_flush_count;
      counts.other_vertex_count += m_index_generator.GetIndexLen();
    }
  }

  auto& pixel_shader_manager = system.GetPixelShaderManager();
  auto& geometry_shader_manager = system.GetGeometryShaderManager();
  auto& vertex_shader_manager = system.GetVertexShaderManager();
  auto& xf_state_manager = system.GetXFStateManager();
  bool committed_hide_object_capture = false;

  if (g_ActiveConfig.bGraphicMods)
  {
    const double seconds_elapsed =
        static_cast<double>(m_ticks_elapsed) / system.GetSystemTimers().GetTicksPerSecond();
    pixel_shader_manager.constants.time_ms = seconds_elapsed * 1000;
  }

  NativeVertexFormat* current_vertex_format = VertexLoaderManager::GetCurrentVertexFormat();
  CalculateNormals(current_vertex_format);
  // Calculate ZSlope for zfreeze
  const auto used_textures = UsedTextures();
  std::vector<std::string> texture_names;
  Common::SmallVector<u32, 8> texture_units;
  std::array<SamplerState, 8> samplers;
  if (!m_cull_all)
  {
    if (!g_ActiveConfig.bGraphicMods)
    {
      for (const u32 i : used_textures)
      {
        const auto cache_entry = g_texture_cache->Load(i);
        if (!cache_entry)
          continue;
        const float custom_tex_scale = cache_entry->GetWidth() / float(cache_entry->native_width);
        samplers[i] = TextureCacheBase::GetSamplerState(
            i, custom_tex_scale, cache_entry->is_custom_tex, cache_entry->has_arbitrary_mips);
      }
    }
    else
    {
      for (const u32 i : used_textures)
      {
        const auto cache_entry = g_texture_cache->Load(i);
        if (cache_entry)
        {
          if (!Common::Contains(texture_names, cache_entry->texture_info_name))
          {
            texture_names.push_back(cache_entry->texture_info_name);
            texture_units.push_back(i);
          }

          const float custom_tex_scale = cache_entry->GetWidth() / float(cache_entry->native_width);
          samplers[i] = TextureCacheBase::GetSamplerState(
              i, custom_tex_scale, cache_entry->is_custom_tex, cache_entry->has_arbitrary_mips);
        }
      }
    }
  }
  vertex_shader_manager.SetConstants(texture_names, xf_state_manager);
  if (!bpmem.genMode.zfreeze)
  {
    // Must be done after VertexShaderManager::SetConstants()
    CalculateZSlope(current_vertex_format);
  }
  else if (m_zslope.dirty && !m_cull_all)  // or apply any dirty ZSlopes
  {
    pixel_shader_manager.SetZSlope(m_zslope.dfdx, m_zslope.dfdy, m_zslope.f0);
    m_zslope.dirty = false;
  }

  if (!m_cull_all)
  {
    CustomPixelShaderContents custom_pixel_shader_contents;
    std::optional<CustomPixelShader> custom_pixel_shader;
    std::vector<std::string> custom_pixel_texture_names;
    std::span<u8> custom_pixel_shader_uniforms;
    bool skip = false;
    for (size_t i = 0; i < texture_names.size(); i++)
    {
      GraphicsModActionData::DrawStarted draw_started{texture_units, &skip, &custom_pixel_shader,
                                                      &custom_pixel_shader_uniforms};
      for (const auto& action : g_graphics_mod_manager->GetDrawStartedActions(texture_names[i]))
      {
        action->OnDrawStarted(&draw_started);
        if (custom_pixel_shader)
        {
          custom_pixel_shader_contents.shaders.push_back(*custom_pixel_shader);
          custom_pixel_texture_names.push_back(texture_names[i]);
        }
        custom_pixel_shader = std::nullopt;
      }
    }

    // Now the vertices can be flushed to the GPU. Everything following the CommitBuffer() call
    // must be careful to not upload any utility vertices, as the binding will be lost otherwise.
    const u32 num_indices = m_index_generator.GetIndexLen();
    if (num_indices == 0)
    {
      HideObjectEngine::Engine::GetInstance().DiscardPendingCapturedPrefixes();
      return;
    }

    // Texture loading can cause palettes to be applied (-> uniforms -> draws).
    // Palette application does not use vertices, only a full-screen quad, so this is okay.
    // Same with GPU texture decoding, which uses compute shaders.
    g_texture_cache->BindTextures(used_textures, samplers);

    if (!skip)
    {
#ifdef ENABLE_VR
      const bool primegun_left_hand_mirrored_position_draw =
          g_ActiveConfig.stereo_mode == StereoMode::OpenXR &&
          !Common::VR::OpenXRInputState::GetPrimedGunOverlay().use_right_hand &&
          DrawUsesMirroredPositionMatrix(current_vertex_format);
#else
      constexpr bool primegun_left_hand_mirrored_position_draw = false;
#endif

      UpdatePipelineConfig();
      UpdatePipelineObject();
      if (primegun_left_hand_mirrored_position_draw)
      {
        m_current_pipeline_config.rasterization_state.cull_mode = CullMode::None;
        m_current_uber_pipeline_config.rasterization_state.cull_mode = CullMode::None;
        m_pipeline_config_changed = true;
        UpdatePipelineObject();
      }
      bool shader_hunter_force_pink = false;
      if (m_current_pipeline_object)
      {
        pixel_shader_manager.SetVRTextureLayerOverride(-1);
        int forced_texture_layer = -1;

        // Shader Hunter: register shader hashes and check for skip.
        // Also check persistent overrides (always active, even when hunting is disabled).
        bool hunter_skip = false;
        bool elements_skip = false;
        bool texmgr_skip = false;
        auto& hunter = ShaderHunter::GetInstance();
        auto& elements = ElementsGroupManager::GetInstance();
        auto& texmgr = TextureElementManager::GetInstance();
        const bool primegun_cannon_probe_enabled = false;
        const bool hunter_enabled = hunter.IsEnabled();
        const bool hunter_debug_logging = hunter.IsDebugLogging();
        const bool hunter_has_overrides = hunter.HasOverrides();
        const bool elements_popup_open = elements.IsPopupOpen();
        const bool elements_has_overrides = elements.HasOverrides();
        const bool elements_runtime_active = elements_popup_open || elements_has_overrides;
        const bool texmgr_has_overrides = texmgr.HasOverrides();
        const bool hunter_needs_families = hunter.NeedsShaderFamilySignatures();
        const bool hunter_needs_textures = hunter.NeedsTextureHashes();
        const bool hunter_needs_counters = hunter.NeedsOverrideDrawCounters();
        if (primegun_cannon_probe_enabled || hunter_enabled || hunter_has_overrides ||
            hunter_debug_logging || elements_runtime_active || texmgr_has_overrides)
        {
          const auto& vs = m_current_pipeline_config.vs_uid;
          const auto& ps = m_current_pipeline_config.ps_uid;
          const auto& gs = m_current_pipeline_config.gs_uid;
          const u64 vs_hash =
              Common::ComputeCRC32(vs.GetUidDataRaw(), static_cast<u32>(vs.GetUidDataSize()));
          const u64 ps_hash =
              Common::ComputeCRC32(ps.GetUidDataRaw(), static_cast<u32>(ps.GetUidDataSize()));
          const u64 gs_hash =
              Common::ComputeCRC32(gs.GetUidDataRaw(), static_cast<u32>(gs.GetUidDataSize()));

          std::array<u64, 8> tex_hashes{};
          std::array<std::string, 8> tex_names{};
          const bool needs_texture_hashes =
              primegun_cannon_probe_enabled || hunter_enabled || hunter_needs_textures ||
              elements_runtime_active || texmgr_has_overrides;
          const bool needs_texture_names =
              primegun_cannon_probe_enabled || hunter_enabled || elements_popup_open;
          if (needs_texture_hashes || needs_texture_names)
          {
            for (u32 i = 0; i < 8; i++)
            {
              if (needs_texture_hashes)
                tex_hashes[i] = g_texture_cache->GetBoundTextureHash(i);
              if (needs_texture_names)
                tex_names[i] = g_texture_cache->GetBoundTextureName(i);
            }
          }

          if (hunter_enabled || hunter_needs_textures)
            hunter.SetCurrentDrawTextures(tex_hashes, tex_names);

          u64 vs_family = 0;
          u64 ps_family = 0;
          u64 gs_family = 0;
          if (hunter_enabled || hunter_needs_families || elements_runtime_active)
          {
            vs_family = hunter.RegisterShader(ShaderHunter::ShaderType::Vertex, vs_hash,
                                              vs.GetUidDataRaw(), vs.GetUidDataSize());
            ps_family = hunter.RegisterShader(ShaderHunter::ShaderType::Pixel, ps_hash,
                                              ps.GetUidDataRaw(), ps.GetUidDataSize());
            gs_family = hunter.RegisterShader(ShaderHunter::ShaderType::Geometry, gs_hash,
                                              gs.GetUidDataRaw(), gs.GetUidDataSize());
            hunter.SetCurrentDrawShaderFamilies(vs_family, ps_family, gs_family);
          }

          ShaderHunter::RuntimeElementSignature draw_signature{};
          if (primegun_cannon_probe_enabled || hunter_enabled || elements_runtime_active)
          {
            draw_signature = BuildRuntimeElementSignature(
                xfmem, bpmem, system.GetGeometryShaderManager().vr_ortho_draw_counter);
            if (hunter_enabled)
              hunter.SetCurrentDrawSignature(draw_signature);
          }

          if (primegun_cannon_probe_enabled)
          {
            LogPrimedGunCannonProbeDraw(m_draw_counter + 1, vs_hash, ps_hash, gs_hash, tex_hashes,
                                       tex_names, draw_signature);
          }

          if (hunter_enabled)
          {
            hunter.RegisterDrawCombination(vs_hash, ps_hash, gs_hash);
            hunter_skip = hunter.ShouldSkipDraw(vs_hash, ps_hash, gs_hash);
            shader_hunter_force_pink = hunter.ShouldHighlightSelectedDraw();
          }

          std::optional<ElementsGroupManager::DrawRecord> element_draw;
          if (elements_runtime_active)
          {
            const bool needs_profile_classification = elements.NeedsProfileClassification();
            MetroidProjectionMetrics profile_metrics;
            if (needs_profile_classification)
              profile_metrics = BuildMetroidProjectionMetrics(xfmem, m_draw_counter);

            element_draw.emplace(ElementsGroupManager::DrawRecord{
                .draw_index = -1,
                .draw_sequence = m_draw_counter + 1,
                .vs_hash = vs_hash,
                .ps_hash = ps_hash,
                .gs_hash = gs_hash,
                .vs_family = vs_family,
                .ps_family = ps_family,
                .gs_family = gs_family,
                .signature = draw_signature,
                .textures = tex_hashes,
                .texture_names = tex_names});
            if (needs_profile_classification)
              elements.ClassifyProfileDraw(&*element_draw, profile_metrics);
            if (needs_profile_classification && IsMetroidPrime1Profile(element_draw->profile_id))
            {
              if (IsMetroidPrime1ScannerPreviewMask(draw_signature))
              {
                element_draw->profile_layer = MetroidElementLayer::ScanDarken;
                element_draw->profile_layer_name =
                    std::string(MetroidElementLayerToDisplayName(element_draw->profile_layer));
              }

              if (IsMetroidPrime1ThermalChargeBeamMaskDraw(element_draw))
              {
                element_draw->profile_layer = MetroidElementLayer::ThermalEffectGun;
                element_draw->profile_layer_name =
                    std::string(MetroidElementLayerToDisplayName(element_draw->profile_layer));
                AppendPrimedGunThermalHudLog(fmt::format(
                    "thermal-chargebeam-remap draw={} active_prev={} seen_this_frame={}\n",
                    element_draw->draw_sequence, m_metroid_prime1_thermal_context_active,
                    m_metroid_prime1_thermal_context_seen));
              }

              if (IsMetroidPrime1MenuContextLayer(element_draw->profile_layer))
              {
                m_metroid_prime1_menu_context_seen = true;
                m_metroid_prime1_combat_context_seen = false;
              }
              else if (!m_metroid_prime1_menu_context_seen &&
                       IsMetroidPrime1CombatContextLayer(element_draw->profile_layer))
              {
                m_metroid_prime1_combat_context_seen = true;
              }
              if (IsMetroidPrime1ThermalContextLayer(element_draw->profile_layer))
              {
                AppendPrimedGunThermalHudLog(fmt::format(
                    "thermal-context draw={} layer={} perspective={} active_prev={}\n",
                    element_draw->draw_sequence, element_draw->profile_layer_name,
                    element_draw->signature.perspective, m_metroid_prime1_thermal_context_active));
                m_metroid_prime1_thermal_context_seen = true;
              }
            }
            if (g_ActiveConfig.vr_metroid_visor_fix && g_texture_cache &&
                needs_profile_classification &&
                elements.IsMetroidPrime1GCProfileActive() &&
                element_draw->profile_id == MetroidElementProfile::Prime1GC)
            {
              // Texture loads and EFB-copy lookups happen before this draw is classified, so
              // opening the window here arms the next matching thermal EFB copy.
              if (IsMetroidThermalBackdropDraw(element_draw))
                g_texture_cache->BeginMetroidPrime1ThermalSourceWindow(
                    "METROID_THERMAL_HEAT_GEOMETRY");
              if (IsMetroidPrime1ThermalHeatEmitterDraw(element_draw))
              {
                g_texture_cache->BeginMetroidPrime1ThermalSourceWindow(
                    "METROID_THERMAL_HEAT_GEOMETRY");
              }
            }

            HideObjectEngine::Engine::GetInstance().CommitCapturedPrefixesForDraw(
                element_draw->draw_sequence);
            committed_hide_object_capture = true;

            const auto preview_action = elements.RegisterDraw(*element_draw);
            elements_skip = preview_action == ElementsGroupManager::PreviewAction::Skip;
            if (preview_action == ElementsGroupManager::PreviewAction::Pink)
              shader_hunter_force_pink = true;
          }

          // Register flag shaders (must be before skip/handling checks)
          if (hunter_has_overrides)
            hunter.RegisterFlags(vs_hash, ps_hash, gs_hash);
          if (elements_runtime_active)
            elements.RegisterFlagsForDraw(*element_draw);

          if (hunter_needs_counters)
            hunter.AdvanceOverrideDrawCounters(vs_hash, ps_hash, gs_hash);
          if (elements_runtime_active)
            elements.AdvanceOverrideDrawCounters(*element_draw);

          if (!hunter_skip && !elements_skip && elements_has_overrides)
            elements_skip = elements.ShouldSkipByOverride(*element_draw);

          if (!hunter_skip && !elements_skip && element_draw)
          {
            const MetroidLayerBehavior layer_behavior =
                GetMetroidLayerBehavior(element_draw->profile_layer);
            if (layer_behavior.skip)
            {
              LogPrimedGunElementHandling(m_draw_counter, element_draw,
                                          ShaderHunter::HandlingType::Skip, vs_hash, ps_hash,
                                            gs_hash, tex_hashes, -1,
                                            std::numeric_limits<float>::quiet_NaN());
                elements_skip = true;
              }
          }

          if (!hunter_skip && !elements_skip && hunter_has_overrides)
            hunter_skip = hunter.ShouldSkipByOverride(vs_hash, ps_hash, gs_hash);

          if (!hunter_skip && !elements_skip && texmgr_has_overrides)
            texmgr_skip = texmgr.ShouldSkipByTexture(tex_hashes);

          // Check for screen/fullscreen handling overrides (VR stereo mode override)
          if (!hunter_skip && !elements_skip && !texmgr_skip)
          {
            auto handling = ShaderHunter::HandlingType::Skip;
            int manual_layer = -1;
            float element_depth = -1.0f;
            float units_per_meter = -1.0f;
            const MetroidHydraHudSettings metroid_hydra_hud =
                element_draw ? GetMetroidHydraHudSettings(element_draw->profile_id,
                                                          element_draw->profile_layer) :
                               MetroidHydraHudSettings{};

            if (elements_has_overrides)
              handling = elements.GetOverrideHandling(*element_draw);
            if (handling != ShaderHunter::HandlingType::Skip)
            {
              if (handling == ShaderHunter::HandlingType::Screen ||
                  handling == ShaderHunter::HandlingType::HeadLocked)
              {
                manual_layer = elements.GetOverrideLayer(*element_draw);
                element_depth = elements.GetOverrideElementDepth(*element_draw);
              }
              else if (handling == ShaderHunter::HandlingType::UnitsPerMeter)
              {
                units_per_meter = elements.GetOverrideUnitsPerMeter(*element_draw);
              }
            }
            else if (hunter_has_overrides)
            {
              handling = hunter.GetOverrideHandling(vs_hash, ps_hash, gs_hash);
              if (handling == ShaderHunter::HandlingType::Screen ||
                  handling == ShaderHunter::HandlingType::HeadLocked)
              {
                manual_layer = hunter.GetOverrideLayer(vs_hash, ps_hash, gs_hash);
                element_depth = hunter.GetOverrideElementDepth(vs_hash, ps_hash, gs_hash);
              }
              else if (handling == ShaderHunter::HandlingType::UnitsPerMeter)
              {
                units_per_meter = hunter.GetOverrideUnitsPerMeter(vs_hash, ps_hash, gs_hash);
              }
            }
            if (handling == ShaderHunter::HandlingType::Skip && element_draw)
              handling = GetMetroidLayerBehavior(element_draw->profile_layer).handling;
            if (handling == ShaderHunter::HandlingType::Skip && texmgr_has_overrides)
            {
              handling =
                  texmgr.GetHandlingForTextures(tex_hashes, &manual_layer, &element_depth,
                                                &units_per_meter);
            }
            const bool metroid_visor_fix_active = g_ActiveConfig.vr_metroid_visor_fix;
            const bool xray_effect_draw =
                metroid_visor_fix_active && IsMetroidPrime1XRayEffectDraw(element_draw);
            const bool xray_signature_match =
                xray_effect_draw && element_draw &&
                element_draw->profile_layer != MetroidElementLayer::XRayEffect;
            const bool xray_signature_fallback = xray_signature_match;
            const bool thermal_hud_context_active =
                m_metroid_prime1_thermal_context_active || m_metroid_prime1_thermal_context_seen;
            const bool d3d_xray_hud_tex0_layer_fallback =
                metroid_visor_fix_active && g_backend_info.api_type == APIType::D3D &&
                !thermal_hud_context_active && IsMetroidPrime1XRayHUDDraw(element_draw);
            if (xray_signature_fallback && handling == ShaderHunter::HandlingType::Skip)
            {
              handling = ShaderHunter::HandlingType::FullscreenMono;
            }
            const bool framebuffer_effect_draw =
                metroid_visor_fix_active && IsMetroidPrime1FramebufferEffectDraw(element_draw);
            if (framebuffer_effect_draw &&
                (handling == ShaderHunter::HandlingType::Skip ||
                 handling == ShaderHunter::HandlingType::Screen ||
                 handling == ShaderHunter::HandlingType::HeadLocked))
            {
              handling = ShaderHunter::HandlingType::FullscreenMono;
              manual_layer = -1;
              element_depth = -1.0f;
            }

            if (handling == ShaderHunter::HandlingType::Screen)
            {
              geometry_shader_manager.vr_stereo_override = VR_STEREO_OVERRIDE_SCREEN;
              if (manual_layer >= 0)
                geometry_shader_manager.vr_ortho_layer_override = manual_layer;
              if (element_depth >= 0.0f)
                geometry_shader_manager.vr_element_depth_override = element_depth;
              if (element_draw)
                geometry_shader_manager.vr_metroid_layer = element_draw->profile_layer;
            }
            else if (handling == ShaderHunter::HandlingType::Fullscreen)
            {
              geometry_shader_manager.vr_stereo_override = VR_STEREO_OVERRIDE_FULLSCREEN;
            }
            else if (handling == ShaderHunter::HandlingType::FullscreenMono)
            {
              geometry_shader_manager.vr_stereo_override = VR_STEREO_OVERRIDE_FULLSCREEN_MONO;
              const bool thermal_backdrop =
                  metroid_visor_fix_active && IsMetroidThermalBackdropDraw(element_draw);
              const bool thermal_color_mask =
                  metroid_visor_fix_active && IsMetroidThermalColorMaskDraw(element_draw);
              if (element_draw &&
                  element_draw->profile_layer == MetroidElementLayer::ThermalEffectGun)
              {
                ++m_metroid_thermal_effect_gun_draws;
              }

              // Backdrop/purple overlay: stage 7 holds the 2-layer EFB copy of the per-eye
              // heat-intensity buffer. When that layered source is present, sample per-eye so
              // the right eye gets its own heat data. Fall back to layer 0 only if the source
              // is somehow not layered (e.g. game-side path, EFB-copy disabled).
              if (thermal_backdrop)
              {
                if (IsMetroidPrime1ThermalColorMaskStereoSourceBound())
                {
                  forced_texture_layer = GetFullscreenMonoPerEyeTextureLayer();
                  pixel_shader_manager.SetVRTextureLayerOverride(forced_texture_layer);
                }
                else
                {
                  forced_texture_layer = 0;
                  pixel_shader_manager.SetVRTextureLayerOverride(forced_texture_layer);
                }
              }
              else if (thermal_color_mask)
              {
                // Color-mask/yellow-heat pass: stage 7 holds the palette-decoded 2-layer
                // texture (left-eye-correct heat colors in layer 0, right-eye-correct in
                // layer 1). The layered palette pipeline preserves per-eye content from the
                // upstream EFB copy. Sample per-eye so each eye gets its own colors.
                if (IsMetroidPrime1ThermalColorMaskStereoSourceBound())
                {
                  forced_texture_layer = GetFullscreenMonoPerEyeTextureLayer();
                  pixel_shader_manager.SetVRTextureLayerOverride(forced_texture_layer);
                }
                else
                {
                  forced_texture_layer = 0;
                  pixel_shader_manager.SetVRTextureLayerOverride(forced_texture_layer);
                }
              }
              else if ((!metroid_visor_fix_active ||
                        !ShouldSampleMetroidFullscreenEffectPerEye(
                            element_draw, m_metroid_thermal_effect_gun_draws)) &&
                       !xray_effect_draw)
              {
                forced_texture_layer = 0;
                pixel_shader_manager.SetVRTextureLayerOverride(forced_texture_layer);
              }
              else
              {
                forced_texture_layer = GetFullscreenMonoPerEyeTextureLayer();
                pixel_shader_manager.SetVRTextureLayerOverride(forced_texture_layer);
              }
            }
            else if (handling == ShaderHunter::HandlingType::HeadLocked)
            {
              const bool perspective_metroid_hud =
                  metroid_hydra_hud.perspective_hud && element_draw &&
                  element_draw->signature.perspective;
              geometry_shader_manager.vr_stereo_override =
                  d3d_xray_hud_tex0_layer_fallback ? VR_STEREO_OVERRIDE_HEAD_LOCKED_TEX0_LAYER :
                  perspective_metroid_hud ? -3.0f :
                                           VR_STEREO_OVERRIDE_HEAD_LOCKED;
              if (metroid_hydra_hud.enabled && element_draw)
              {
                const auto& pnm = vertex_shader_manager.constants.posnormalmatrix;
                const float metroid_hud_ref_z = pnm[2][3];
                int metroid_hud_context = METROID_HUD_CONTEXT_DEFAULT;
                if (IsMetroidPrime1Profile(element_draw->profile_id))
                {
                  if (m_metroid_prime1_menu_context_seen)
                    metroid_hud_context = METROID_HUD_CONTEXT_MENU;
                  else if (m_metroid_prime1_combat_context_seen)
                    metroid_hud_context = METROID_HUD_CONTEXT_COMBAT;
                }
                const std::string_view layer_name =
                    MetroidElementLayerToININame(element_draw->profile_layer);
                geometry_shader_manager.vr_metroid_hud_self_center =
                    layer_name.find("RADAR") != std::string_view::npos;
                geometry_shader_manager.vr_metroid_hud_anchor_candidate =
                    perspective_metroid_hud &&
                    (IsMetroidPerspectiveHudAnchorLayer(element_draw->profile_layer) ||
                     IsMetroidPrime1CombatHudAnchorCandidate(element_draw->profile_layer,
                                                             metroid_hud_ref_z,
                                                             metroid_hud_context));
                geometry_shader_manager.vr_metroid_hud_reference_context = metroid_hud_context;
              }
              if (manual_layer >= 0)
                geometry_shader_manager.vr_ortho_layer_override = manual_layer;
              if (element_depth >= 0.0f)
                geometry_shader_manager.vr_element_depth_override = element_depth;
              // Plumb classified Metroid layer through to the GS uniforms so it can apply
              // Hydra-style per-layer hacks (Helmet/Visor/HUD scale + FOV mods).
              if (element_draw)
                geometry_shader_manager.vr_metroid_layer = element_draw->profile_layer;
              if (metroid_hydra_hud.enabled)
              {
                if (metroid_hydra_hud.scale > 0.0f)
                {
                  geometry_shader_manager.vr_units_per_meter_override =
                      g_ActiveConfig.vr_units_per_meter * metroid_hydra_hud.scale;
                }
                geometry_shader_manager.vr_headlocked_projection_scale_x =
                    metroid_hydra_hud.width;
                geometry_shader_manager.vr_headlocked_projection_scale_y =
                    metroid_hydra_hud.height;
                geometry_shader_manager.vr_headlocked_projection_offset_x =
                    metroid_hydra_hud.right;
                geometry_shader_manager.vr_headlocked_projection_offset_y = metroid_hydra_hud.up;
                if (metroid_hydra_hud.perspective_hud)
                {
                  geometry_shader_manager.vr_perspective_hud_distance_override =
                      GetPrimedGunPerspectiveHudDistance();
                  geometry_shader_manager.vr_perspective_hud_size_override =
                      GetPrimedGunPerspectiveHudSize();
                }
              }
              if (d3d_xray_hud_tex0_layer_fallback)
              {
                forced_texture_layer = VR_TEXTURE_LAYER_FROM_D3D_FULLSCREEN_TEX0_Z;
                pixel_shader_manager.SetVRTextureLayerOverride(forced_texture_layer);
              }
            }
            else if (handling == ShaderHunter::HandlingType::UnitsPerMeter)
            {
              if (units_per_meter > 0.0f)
                geometry_shader_manager.vr_units_per_meter_override = units_per_meter;
            }
            else
            {
              // No override matched — log for debugging if relevant
              hunter.DebugLogUnmatched(vs_hash, ps_hash, gs_hash);
            }
            LogPrimedGunElementHandling(m_draw_counter, element_draw, handling, vs_hash, ps_hash,
                                        gs_hash, tex_hashes, forced_texture_layer,
                                        geometry_shader_manager.vr_stereo_override);
            LogMetroidPrime1XRayDraw(m_draw_counter, element_draw, handling, vs_hash, ps_hash,
                                     gs_hash, forced_texture_layer, xray_signature_fallback,
                                     d3d_xray_hud_tex0_layer_fallback);
          }

          // ClearEFB is an independent flag, checked regardless of handling type.
          // This lets a shader be e.g. Skip+ClearEFB or Screen+ClearEFB.
          if (hunter_has_overrides)
            hunter.CheckClearEFBForDraw(vs_hash, ps_hash, gs_hash);

          // VR Draw Debug Logging: log every draw call's projection, viewport, scissor, and
          // shader hashes so we can identify how specific visual elements (e.g. cinematic bars)
          // are drawn.
          if (hunter.IsDebugLogging()) [[unlikely]]
          {
            const auto& proj = xfmem.projection;
            const auto& vp = xfmem.viewport;
            const auto& scTL = bpmem.scissorTL;
            const auto& scBR = bpmem.scissorBR;
            const u32 nidx = m_index_generator.GetIndexLen();
            const bool z_test = bpmem.zmode.test_enable != 0;
            const bool color_update = bpmem.blendmode.color_update != 0;
            const bool alpha_update = bpmem.blendmode.alpha_update != 0;
            const bool z_update = bpmem.zmode.update_enable != 0;
            const int ortho_layer_counter = geometry_shader_manager.vr_ortho_draw_counter;
            const int ortho_layer_override = geometry_shader_manager.vr_ortho_layer_override;
            if (proj.type == ProjectionType::Orthographic)
            {
              const float* p = proj.rawProjection.data();
              const float left = -(p[1] + 1) / p[0];
              const float right = left + 2 / p[0];
              const float bottom = -(p[3] + 1) / p[2];
              const float top = bottom + 2 / p[2];
              const float zfar = p[5] / p[4];
              const float znear = (1 + p[4] * zfar) / p[4];
              INFO_LOG_FMT(VIDEO,
                           "VR_DRAW #{}: ORTHO l={:.1f} r={:.1f} t={:.1f} b={:.1f} n={:.1f} "
                           "f={:.1f} | VP({:.0f},{:.0f} {:.0f}x{:.0f}) | SC({},{} {},{})"
                           " | VS={:08x} PS={:08x} GS={:08x} | idx={} col={} alpha={} zt={} "
                           "z={} zf={} | layer_ctr={} layer_ovr={} | skip={}",
                           m_draw_counter, left, right, top, bottom, znear, zfar, vp.xOrig,
                           vp.yOrig, vp.wd, vp.ht, scTL.x, scTL.y, scBR.x, scBR.y, vs_hash,
                           ps_hash, gs_hash, nidx, color_update, alpha_update, z_test, z_update,
                           bpmem.zmode.func, ortho_layer_counter, ortho_layer_override, hunter_skip);
            }
            else
            {
              const float* p = proj.rawProjection.data();
              const float hfov = 2 * std::atan(1.0f / p[0]) * 180.0f / 3.14159265f;
              const float vfov = 2 * std::atan(1.0f / p[2]) * 180.0f / 3.14159265f;
              const float f = p[5] / p[4];
              const float n = f * p[4] / (p[4] - 1);
              INFO_LOG_FMT(VIDEO,
                           "VR_DRAW #{}: PERSP hfov={:.2f} vfov={:.2f} n={:.2f} f={:.2f}"
                           " | VP({:.0f},{:.0f} {:.0f}x{:.0f}) | SC({},{} {},{})"
                           " | VS={:08x} PS={:08x} GS={:08x} | idx={} col={} alpha={} zt={} "
                           "z={} zf={} | layer_ctr={} layer_ovr={} | skip={}",
                           m_draw_counter, hfov, vfov, n, f, vp.xOrig, vp.yOrig, vp.wd, vp.ht,
                           scTL.x, scTL.y, scBR.x, scBR.y, vs_hash, ps_hash, gs_hash, nidx,
                           color_update, alpha_update, z_test, z_update, bpmem.zmode.func,
                           ortho_layer_counter, ortho_layer_override, hunter_skip);
            }
          }
        }

        // Increment ortho draw counter for depth layering on the VR virtual screen.
        // Applies to natural ortho draws and draws forced to screen/head-locked by override.
        // Keep read-only EQUAL passes on the previous layer to preserve multi-pass HUD rendering.
        if (g_ActiveConfig.vr_auto_layer_spread)
        {
          const bool is_ortho = xfmem.projection.type != ProjectionType::Perspective;
          const bool forced_screen_or_headlocked =
              !std::isnan(geometry_shader_manager.vr_stereo_override) &&
              geometry_shader_manager.vr_stereo_override < -0.5f;
          if (is_ortho || forced_screen_or_headlocked)
          {
            const bool z_equal_read_only =
                bpmem.zmode.test_enable && !bpmem.zmode.update_enable &&
                bpmem.zmode.func == CompareMode::Equal;

            if (z_equal_read_only && geometry_shader_manager.vr_ortho_layer_override < 0 &&
                geometry_shader_manager.vr_ortho_draw_counter > 0)
            {
              geometry_shader_manager.vr_ortho_layer_override =
                  geometry_shader_manager.vr_ortho_draw_counter - 1;
            }

            if (!z_equal_read_only)
              geometry_shader_manager.vr_ortho_draw_counter++;
          }
        }

        if (!hunter_skip && !elements_skip && !texmgr_skip)
        {
          if (shader_hunter_force_pink && !m_pink_pixel_shader)
          {
            // Simple PS that outputs solid magenta, like 3DMigoto's hunting mode.
            // No #version or #defines — the backend prepends its own SHADER_HEADER
            // (with #version 450, type aliases, etc.) before SPIRV compilation.
            // Outputs both ocol0 and ocol1 (dual-source blending index 1) so shaders
            // using SRC1_ALPHA blend modes still render visibly.
            constexpr std::string_view pink_source =
                "FRAGMENT_OUTPUT_LOCATION_INDEXED(0, 0) out float4 ocol0;\n"
                "FRAGMENT_OUTPUT_LOCATION_INDEXED(0, 1) out float4 ocol1;\n"
                "void main() {\n"
                "  ocol0 = float4(1.0, 0.0, 1.0, 1.0);\n"
                "  ocol1 = float4(0.0, 0.0, 0.0, 1.0);\n"
                "}\n";
            m_pink_pixel_shader = g_gfx->CreateShaderFromSource(
                ShaderStage::Pixel, pink_source, nullptr, "Pink Highlight");
            if (!m_pink_pixel_shader)
              WARN_LOG_FMT(VIDEO, "Failed to compile pink highlight pixel shader");
          }
          m_force_pink_ps = shader_hunter_force_pink;

          const AbstractPipeline* pipeline_object = m_current_pipeline_object;
          if (!custom_pixel_shader_contents.shaders.empty())
          {
            const AbstractPipeline* custom_pipeline = nullptr;
            if (const auto async_pipeline =
                    GetCustomPipeline(custom_pixel_shader_contents, m_current_pipeline_config,
                                      m_current_uber_pipeline_config, m_current_pipeline_object))
            {
              custom_pipeline = async_pipeline;
            }

            if (custom_pipeline)
            {
              pipeline_object = custom_pipeline;
            }
          }
          RenderDrawCall(pixel_shader_manager, geometry_shader_manager,
                         custom_pixel_shader_contents, custom_pixel_shader_uniforms,
                         m_current_primitive_type, pipeline_object);
          m_force_pink_ps = false;
        }
      }
    }

    if (!committed_hide_object_capture)
      HideObjectEngine::Engine::GetInstance().DiscardPendingCapturedPrefixes();

    // Even if we skip the draw, emulated state should still be impacted
    OnDraw();

    // The EFB cache is now potentially stale.
    g_framebuffer_manager->FlagPeekCacheAsOutOfDate();
  }
  else
  {
    HideObjectEngine::Engine::GetInstance().DiscardPendingCapturedPrefixes();
  }

  if (xfmem.numTexGen.numTexGens != bpmem.genMode.numtexgens)
  {
    ERROR_LOG_FMT(VIDEO,
                  "xf.numtexgens ({}) does not match bp.numtexgens ({}). Error in command stream.",
                  xfmem.numTexGen.numTexGens, bpmem.genMode.numtexgens.Value());
  }
}

void VertexManagerBase::DoState(PointerWrap& p)
{
  if (p.IsReadMode())
  {
    // Flush old vertex data before loading state.
    Flush();
  }

  p.Do(m_zslope);
  p.Do(VertexLoaderManager::normal_cache);
  p.Do(VertexLoaderManager::tangent_cache);
  p.Do(VertexLoaderManager::binormal_cache);
}

void VertexManagerBase::CalculateZSlope(NativeVertexFormat* format)
{
  float out[12];
  float viewOffset[2] = {xfmem.viewport.xOrig - bpmem.scissorOffset.x * 2,
                         xfmem.viewport.yOrig - bpmem.scissorOffset.y * 2};

  if (m_current_primitive_type != PrimitiveType::Triangles &&
      m_current_primitive_type != PrimitiveType::TriangleStrip)
  {
    return;
  }

  // Global matrix ID.
  u32 mtxIdx = g_main_cp_state.matrix_index_a.PosNormalMtxIdx;
  const PortableVertexDeclaration vert_decl = format->GetVertexDeclaration();

  // Make sure the buffer contains at least 3 vertices.
  if ((m_cur_buffer_pointer - m_base_buffer_pointer) < (vert_decl.stride * 3))
    return;

  // Lookup vertices of the last rendered triangle and software-transform them
  // This allows us to determine the depth slope, which will be used if z-freeze
  // is enabled in the following flush.
  auto& system = Core::System::GetInstance();
  auto& vertex_shader_manager = system.GetVertexShaderManager();
  for (unsigned int i = 0; i < 3; ++i)
  {
    // If this vertex format has per-vertex position matrix IDs, look it up.
    if (vert_decl.posmtx.enable)
      mtxIdx = VertexLoaderManager::position_matrix_index_cache[2 - i];

    if (vert_decl.position.components == 2)
      VertexLoaderManager::position_cache[2 - i][2] = 0;

    vertex_shader_manager.TransformToClipSpace(&VertexLoaderManager::position_cache[2 - i][0],
                                               &out[i * 4], mtxIdx);

    // Transform to Screenspace
    float inv_w = 1.0f / out[3 + i * 4];

    out[0 + i * 4] = out[0 + i * 4] * inv_w * xfmem.viewport.wd + viewOffset[0];
    out[1 + i * 4] = out[1 + i * 4] * inv_w * xfmem.viewport.ht + viewOffset[1];
    out[2 + i * 4] = out[2 + i * 4] * inv_w * xfmem.viewport.zRange + xfmem.viewport.farZ;
  }

  float dx31 = out[8] - out[0];
  float dx12 = out[0] - out[4];
  float dy12 = out[1] - out[5];
  float dy31 = out[9] - out[1];

  float DF31 = out[10] - out[2];
  float DF21 = out[6] - out[2];
  float a = DF31 * -dy12 - DF21 * dy31;
  float b = dx31 * DF21 + dx12 * DF31;
  float c = -dx12 * dy31 - dx31 * -dy12;

  // Sometimes we process de-generate triangles. Stop any divide by zeros
  if (c == 0)
    return;

  m_zslope.dfdx = -a / c;
  m_zslope.dfdy = -b / c;
  m_zslope.f0 = out[2] - (out[0] * m_zslope.dfdx + out[1] * m_zslope.dfdy);
  m_zslope.dirty = true;
}

void VertexManagerBase::CalculateNormals(NativeVertexFormat* format)
{
  const PortableVertexDeclaration vert_decl = format->GetVertexDeclaration();

  // Only update the binormal/tangent vertex shader constants if the vertex format lacks binormals
  // (VertexLoaderManager::binormal_cache gets updated by the vertex loader when binormals are
  // present, though)
  if (vert_decl.normals[1].enable)
    return;

  VertexLoaderManager::tangent_cache[3] = 0;
  VertexLoaderManager::binormal_cache[3] = 0;

  auto& system = Core::System::GetInstance();
  auto& vertex_shader_manager = system.GetVertexShaderManager();
  if (vertex_shader_manager.constants.cached_tangent != VertexLoaderManager::tangent_cache)
  {
    vertex_shader_manager.constants.cached_tangent = VertexLoaderManager::tangent_cache;
    vertex_shader_manager.dirty = true;
  }
  if (vertex_shader_manager.constants.cached_binormal != VertexLoaderManager::binormal_cache)
  {
    vertex_shader_manager.constants.cached_binormal = VertexLoaderManager::binormal_cache;
    vertex_shader_manager.dirty = true;
  }

  if (vert_decl.normals[0].enable)
    return;

  VertexLoaderManager::normal_cache[3] = 0;
  if (vertex_shader_manager.constants.cached_normal != VertexLoaderManager::normal_cache)
  {
    vertex_shader_manager.constants.cached_normal = VertexLoaderManager::normal_cache;
    vertex_shader_manager.dirty = true;
  }
}

void VertexManagerBase::UpdatePipelineConfig()
{
  NativeVertexFormat* vertex_format = VertexLoaderManager::GetCurrentVertexFormat();
  if (vertex_format != m_current_pipeline_config.vertex_format)
  {
    m_current_pipeline_config.vertex_format = vertex_format;
    m_current_uber_pipeline_config.vertex_format =
        VertexLoaderManager::GetUberVertexFormat(vertex_format->GetVertexDeclaration());
    m_pipeline_config_changed = true;
  }

  VertexShaderUid vs_uid = GetVertexShaderUid();
  if (vs_uid != m_current_pipeline_config.vs_uid)
  {
    m_current_pipeline_config.vs_uid = vs_uid;
    m_current_uber_pipeline_config.vs_uid = UberShader::GetVertexShaderUid();
    m_pipeline_config_changed = true;
  }

  PixelShaderUid ps_uid = GetPixelShaderUid();
  if (ps_uid != m_current_pipeline_config.ps_uid)
  {
    m_current_pipeline_config.ps_uid = ps_uid;
    m_current_uber_pipeline_config.ps_uid = UberShader::GetPixelShaderUid();
    m_pipeline_config_changed = true;
  }

  GeometryShaderUid gs_uid = GetGeometryShaderUid(GetCurrentPrimitiveType());
  if (gs_uid != m_current_pipeline_config.gs_uid)
  {
    m_current_pipeline_config.gs_uid = gs_uid;
    m_current_uber_pipeline_config.gs_uid = gs_uid;
    m_pipeline_config_changed = true;
  }

  if (m_rasterization_state_changed)
  {
    m_rasterization_state_changed = false;

    RasterizationState new_rs = {};
    new_rs.Generate(bpmem, m_current_primitive_type);
    if (new_rs != m_current_pipeline_config.rasterization_state)
    {
      m_current_pipeline_config.rasterization_state = new_rs;
      m_current_uber_pipeline_config.rasterization_state = new_rs;
      m_pipeline_config_changed = true;
    }
  }

  if (m_depth_state_changed)
  {
    m_depth_state_changed = false;

    DepthState new_ds = {};
    new_ds.Generate(bpmem);
    if (new_ds != m_current_pipeline_config.depth_state)
    {
      m_current_pipeline_config.depth_state = new_ds;
      m_current_uber_pipeline_config.depth_state = new_ds;
      m_pipeline_config_changed = true;
    }
  }

  if (m_blending_state_changed)
  {
    m_blending_state_changed = false;

    BlendingState new_bs = {};
    new_bs.Generate(bpmem);
    if (new_bs != m_current_pipeline_config.blending_state)
    {
      m_current_pipeline_config.blending_state = new_bs;
      m_current_uber_pipeline_config.blending_state = new_bs;
      m_pipeline_config_changed = true;
    }
  }
}

void VertexManagerBase::UpdatePipelineObject()
{
  if (!m_pipeline_config_changed)
    return;

  m_current_pipeline_object = nullptr;
  m_pipeline_config_changed = false;

  switch (g_ActiveConfig.iShaderCompilationMode)
  {
  case ShaderCompilationMode::Synchronous:
  {
    // Ubershaders disabled? Block and compile the specialized shader.
    m_current_pipeline_object = g_shader_cache->GetPipelineForUid(m_current_pipeline_config);
  }
  break;

  case ShaderCompilationMode::SynchronousUberShaders:
  {
    // Exclusive ubershader mode, always use ubershaders.
    m_current_pipeline_object =
        g_shader_cache->GetUberPipelineForUid(m_current_uber_pipeline_config);
  }
  break;

  case ShaderCompilationMode::AsynchronousUberShaders:
  case ShaderCompilationMode::AsynchronousSkipRendering:
  {
    // Can we background compile shaders? If so, get the pipeline asynchronously.
    auto res = g_shader_cache->GetPipelineForUidAsync(m_current_pipeline_config);
    if (res)
    {
      // Specialized shaders are ready, prefer these.
      m_current_pipeline_object = *res;
      return;
    }

    if (g_ActiveConfig.iShaderCompilationMode == ShaderCompilationMode::AsynchronousUberShaders)
    {
      // Specialized shaders not ready, use the ubershaders.
      m_current_pipeline_object =
          g_shader_cache->GetUberPipelineForUid(m_current_uber_pipeline_config);
    }
    else
    {
      // Ensure we try again next draw. Otherwise, if no registers change between frames, the
      // object will never be drawn, even when the shader is ready.
      m_pipeline_config_changed = true;
    }
  }
  break;
  }
}

void VertexManagerBase::OnConfigChange()
{
  // Reload index generator function tables in case VS expand config changed
  m_index_generator.Init();
}

void VertexManagerBase::OnDraw()
{
  m_draw_counter++;

  // If the last efb copy was too close to the one before it, don't forget about it until the next
  // efb copy happens (which might not be for a long time)
  u32 diff = m_draw_counter - m_last_efb_copy_draw_counter;
  if (m_unflushed_efb_copy && diff > MINIMUM_DRAW_CALLS_PER_COMMAND_BUFFER_FOR_READBACK)
  {
    g_gfx->Flush();
    m_unflushed_efb_copy = false;
    m_last_efb_copy_draw_counter = m_draw_counter;
  }

  // If we didn't have any CPU access last frame, do nothing.
  if (m_scheduled_command_buffer_kicks.empty() || !m_allow_background_execution)
    return;

  // Check if this draw is scheduled to kick a command buffer.
  // The draw counters will always be sorted so a binary search is possible here.
  if (std::ranges::binary_search(m_scheduled_command_buffer_kicks, m_draw_counter))
  {
    // Kick a command buffer on the background thread.
    g_gfx->Flush();
    m_unflushed_efb_copy = false;
    m_last_efb_copy_draw_counter = m_draw_counter;
  }
}

void VertexManagerBase::OnCPUEFBAccess()
{
  // Check this isn't another access without any draws in between.
  if (!m_cpu_accesses_this_frame.empty() && m_cpu_accesses_this_frame.back() == m_draw_counter)
    return;

  // Store the current draw counter for scheduling in OnEndFrame.
  m_cpu_accesses_this_frame.emplace_back(m_draw_counter);
}

void VertexManagerBase::OnEFBCopyToRAM()
{
  // If we're not deferring, try to preempt it next frame.
  if (!g_ActiveConfig.bDeferEFBCopies)
  {
    OnCPUEFBAccess();
    return;
  }

  // Otherwise, only execute if we have at least 10 objects between us and the last copy.
  const u32 diff = m_draw_counter - m_last_efb_copy_draw_counter;
  m_last_efb_copy_draw_counter = m_draw_counter;
  if (diff < MINIMUM_DRAW_CALLS_PER_COMMAND_BUFFER_FOR_READBACK)
  {
    m_unflushed_efb_copy = true;
    return;
  }

  m_unflushed_efb_copy = false;
  g_gfx->Flush();
}

void VertexManagerBase::OnEndFrame()
{
  auto& hunter = ShaderHunter::GetInstance();
  // Lazy-load shader overrides and hide object codes when game ID becomes available or changes
  const std::string game_id = SConfig::GetInstance().GetGameID();
  if (!game_id.empty())
  {
    hunter.LoadOverridesIfNeeded(game_id);
    ElementsGroupManager::GetInstance().LoadOverridesIfNeeded(game_id);
    TextureElementManager::GetInstance().LoadOverridesIfNeeded(game_id);
    HideObjectEngine::Engine::GetInstance().LoadCodesIfNeeded(game_id);
  }
  hunter.OnFrameEnd();
  CullingCodeFinder::GetInstance().OnFrameEnd();
  ElementsGroupManager::GetInstance().OnFrameEnd();
  TextureElementManager::GetInstance().OnFrameEnd();
  HideObjectEngine::Engine::GetInstance().OnFrameEnd();
  auto& system = Core::System::GetInstance();
  system.GetGeometryShaderManager().vr_ortho_draw_counter = 0;
  m_draw_counter = 0;
  m_last_efb_copy_draw_counter = 0;
  m_metroid_thermal_effect_gun_draws = 0;
  m_metroid_prime1_combat_context_seen = false;
  m_metroid_prime1_menu_context_seen = false;
  m_metroid_prime1_thermal_context_active = m_metroid_prime1_thermal_context_seen;
  m_metroid_prime1_thermal_context_seen = false;
  m_scheduled_command_buffer_kicks.clear();

  // If we have no CPU access at all, leave everything in the one command buffer for maximum
  // parallelism between CPU/GPU, at the cost of slightly higher latency.
  if (m_cpu_accesses_this_frame.empty())
    return;

  // In order to reduce CPU readback latency, we want to kick a command buffer roughly halfway
  // between the draw counters that invoked the readback, or every 250 draws, whichever is
  // smaller.
  if (g_ActiveConfig.iCommandBufferExecuteInterval > 0)
  {
    u32 last_draw_counter = 0;
    u32 interval = static_cast<u32>(g_ActiveConfig.iCommandBufferExecuteInterval);
    for (u32 draw_counter : m_cpu_accesses_this_frame)
    {
      // We don't want to waste executing command buffers for only a few draws, so set a minimum.
      // Leave last_draw_counter as-is, so we get the correct number of draws between submissions.
      u32 draw_count = draw_counter - last_draw_counter;
      if (draw_count < MINIMUM_DRAW_CALLS_PER_COMMAND_BUFFER_FOR_READBACK)
        continue;

      if (draw_count <= interval)
      {
        u32 mid_point = draw_count / 2;
        m_scheduled_command_buffer_kicks.emplace_back(last_draw_counter + mid_point);
      }
      else
      {
        u32 counter = interval;
        while (counter < draw_count)
        {
          m_scheduled_command_buffer_kicks.emplace_back(last_draw_counter + counter);
          counter += interval;
        }
      }

      last_draw_counter = draw_counter;
    }
  }

  m_cpu_accesses_this_frame.clear();

  // We invalidate the pipeline object at the start of the frame.
  // This is for the rare case where only a single pipeline configuration is used,
  // and hybrid ubershaders have compiled the specialized shader, but without any
  // state changes the specialized shader will not take over.
  InvalidatePipelineObject();
}

void VertexManagerBase::NotifyCustomShaderCacheOfHostChange(const ShaderHostConfig& host_config)
{
  m_custom_shader_cache->SetHostConfig(host_config);
  m_custom_shader_cache->Reload();
}

void VertexManagerBase::RenderDrawCall(
    PixelShaderManager& pixel_shader_manager, GeometryShaderManager& geometry_shader_manager,
    const CustomPixelShaderContents& custom_pixel_shader_contents,
    std::span<u8> custom_pixel_shader_uniforms, PrimitiveType primitive_type,
    const AbstractPipeline* current_pipeline)
{
  auto& system = Core::System::GetInstance();
  auto& vertex_shader_manager = system.GetVertexShaderManager();
  auto& xf_state_manager = system.GetXFStateManager();

  if (g_ActiveConfig.stereo_mode == StereoMode::OpenXR)
  {
    const float vr_override = geometry_shader_manager.vr_stereo_override;
    vertex_shader_manager.SetProjectionMatrix(xf_state_manager,
                                              !ShouldDisableOpenXRLegacyView(vr_override), true);
  }

  // Now we can upload uniforms, as nothing else will override them.
  geometry_shader_manager.SetConstants(primitive_type);
  pixel_shader_manager.SetConstants();
  if (!custom_pixel_shader_uniforms.empty() &&
      pixel_shader_manager.custom_constants.data() != custom_pixel_shader_uniforms.data())
  {
    pixel_shader_manager.custom_constants_dirty = true;
  }
  pixel_shader_manager.custom_constants = custom_pixel_shader_uniforms;
  UploadUniforms();

  g_gfx->SetPipeline(current_pipeline);

  // Shader Hunter pink highlight: override PS after pipeline is set (like 3DMigoto).
  if (m_force_pink_ps && m_pink_pixel_shader)
    g_gfx->SetForcePixelShader(m_pink_pixel_shader.get());

  u32 base_vertex, base_index;
  CommitBuffer(m_index_generator.GetNumVerts(),
               VertexLoaderManager::GetCurrentVertexFormat()->GetVertexStride(),
               m_index_generator.GetIndexLen(), &base_vertex, &base_index);

  if (g_backend_info.api_type != APIType::D3D && g_ActiveConfig.UseVSForLinePointExpand() &&
      (primitive_type == PrimitiveType::Points || primitive_type == PrimitiveType::Lines))
  {
    // VS point/line expansion puts the vertex id at gl_VertexID << 2
    // That means the base vertex has to be adjusted to match
    // (The shader adds this after shifting right on D3D, so no need to do this)
    base_vertex <<= 2;
  }

  if (PerfQueryBase::ShouldEmulate())
    g_perf_query->EnableQuery(bpmem.zcontrol.early_ztest ? PQG_ZCOMP_ZCOMPLOC : PQG_ZCOMP);

  DrawCurrentBatch(base_index, m_index_generator.GetIndexLen(), base_vertex);

  // Track the total emulated state draws
  INCSTAT(g_stats.this_frame.num_draw_calls);

  if (PerfQueryBase::ShouldEmulate())
    g_perf_query->DisableQuery(bpmem.zcontrol.early_ztest ? PQG_ZCOMP_ZCOMPLOC : PQG_ZCOMP);
}

const AbstractPipeline* VertexManagerBase::GetCustomPipeline(
    const CustomPixelShaderContents& custom_pixel_shader_contents,
    const VideoCommon::GXPipelineUid& current_pipeline_config,
    const VideoCommon::GXUberPipelineUid& current_uber_pipeline_config,
    const AbstractPipeline* current_pipeline) const
{
  if (current_pipeline)
  {
    if (!custom_pixel_shader_contents.shaders.empty())
    {
      CustomShaderInstance custom_shaders;
      custom_shaders.pixel_contents = custom_pixel_shader_contents;
      switch (g_ActiveConfig.iShaderCompilationMode)
      {
      case ShaderCompilationMode::Synchronous:
      case ShaderCompilationMode::AsynchronousSkipRendering:
      {
        if (auto pipeline = m_custom_shader_cache->GetPipelineAsync(
                current_pipeline_config, custom_shaders, current_pipeline->m_config))
        {
          return *pipeline;
        }
      }
      break;
      case ShaderCompilationMode::SynchronousUberShaders:
      {
        // Custom pixel shader injection is only supported by specialized pixel shader generation.
        // Avoid custom ubershader fallback in this mode so behavior is consistent across backends.
        if (!custom_pixel_shader_contents.shaders.empty())
        {
          if (auto pipeline = m_custom_shader_cache->GetPipelineAsync(
                  current_pipeline_config, custom_shaders, current_pipeline->m_config))
          {
            return *pipeline;
          }
        }
        else
        {
          // D3D has issues compiling large custom ubershaders, use specialized shaders instead.
          if (g_backend_info.api_type == APIType::D3D)
          {
            if (auto pipeline = m_custom_shader_cache->GetPipelineAsync(
                    current_pipeline_config, custom_shaders, current_pipeline->m_config))
            {
              return *pipeline;
            }
          }
          else
          {
            if (auto pipeline = m_custom_shader_cache->GetPipelineAsync(
                    current_uber_pipeline_config, custom_shaders, current_pipeline->m_config))
            {
              return *pipeline;
            }
          }
        }
      }
      break;
      case ShaderCompilationMode::AsynchronousUberShaders:
      {
        if (auto pipeline = m_custom_shader_cache->GetPipelineAsync(
                current_pipeline_config, custom_shaders, current_pipeline->m_config))
        {
          return *pipeline;
        }
        // Custom pixel shader injection is only supported by specialized shaders.
        else if (custom_pixel_shader_contents.shaders.empty())
        {
          if (auto uber_pipeline = m_custom_shader_cache->GetPipelineAsync(
                  current_uber_pipeline_config, custom_shaders, current_pipeline->m_config))
          {
            return *uber_pipeline;
          }
        }
      }
      break;
      };
    }
  }

  return nullptr;
}
