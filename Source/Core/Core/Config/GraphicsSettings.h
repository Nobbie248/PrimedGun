// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cmath>
#include <string>

#include "Common/Config/Config.h"

enum class AspectMode : int;
enum class ShaderCompilationMode : int;
enum class StereoMode : int;
enum class StereoPerEyeResolution : int;
enum class TextureFilteringMode : int;
enum class AnisotropicFilteringMode : int;
enum class OutputResamplingMode : int;
enum class ColorCorrectionRegion : int;
enum class TriState : int;
enum class FrameDumpResolutionType : int;
enum class VertexLoaderType : int;
enum class OpenXROpcodeReplayMode : int;
enum class OpenXRMirrorView : int;
enum class OpenXRReferenceSpaceMode : int;
enum class OpenXRTrackingMode : int;

namespace Config
{
// Configuration Information

// Graphics.Hardware

extern const Info<bool> GFX_VSYNC;
extern const Info<int> GFX_ADAPTER;

// Graphics.Settings

extern const Info<bool> GFX_WIDESCREEN_HACK;
extern const Info<AspectMode> GFX_ASPECT_RATIO;
extern const Info<int> GFX_CUSTOM_ASPECT_RATIO_WIDTH;
extern const Info<int> GFX_CUSTOM_ASPECT_RATIO_HEIGHT;
extern const Info<AspectMode> GFX_SUGGESTED_ASPECT_RATIO;
extern const Info<u32> GFX_WIDESCREEN_HEURISTIC_TRANSITION_THRESHOLD;
extern const Info<float> GFX_WIDESCREEN_HEURISTIC_ASPECT_RATIO_SLOP;
extern const Info<float> GFX_WIDESCREEN_HEURISTIC_STANDARD_RATIO;
extern const Info<float> GFX_WIDESCREEN_HEURISTIC_WIDESCREEN_RATIO;
extern const Info<bool> GFX_CROP;
extern const Info<int> GFX_SAFE_TEXTURE_CACHE_COLOR_SAMPLES;
extern const Info<bool> GFX_SHOW_FPS;
extern const Info<bool> GFX_SHOW_FTIMES;
extern const Info<bool> GFX_SHOW_VPS;
extern const Info<bool> GFX_SHOW_VTIMES;
extern const Info<bool> GFX_SHOW_GRAPHS;
extern const Info<bool> GFX_SHOW_SPEED;
extern const Info<bool> GFX_SHOW_SPEED_COLORS;
extern const Info<bool> GFX_MOVABLE_PERFORMANCE_METRICS;
extern const Info<int> GFX_PERF_SAMP_WINDOW;
extern const Info<bool> GFX_SHOW_NETPLAY_PING;
extern const Info<bool> GFX_SHOW_NETPLAY_MESSAGES;
extern const Info<bool> GFX_LOG_RENDER_TIME_TO_FILE;
extern const Info<bool> GFX_OVERLAY_STATS;
extern const Info<bool> GFX_OVERLAY_PROJ_STATS;
extern const Info<bool> GFX_OVERLAY_SCISSOR_STATS;
extern const Info<bool> GFX_OVERLAY_SHADER_FLAGS;
extern const Info<bool> GFX_OVERLAY_SHADER_HUNTING;
extern const Info<bool> GFX_DUMP_TEXTURES;
extern const Info<bool> GFX_DUMP_MIP_TEXTURES;
extern const Info<bool> GFX_DUMP_BASE_TEXTURES;
extern const Info<int> GFX_TEXTURE_PNG_COMPRESSION_LEVEL;
extern const Info<bool> GFX_HIRES_TEXTURES;
extern const Info<bool> GFX_CACHE_HIRES_TEXTURES;
extern const Info<bool> GFX_DUMP_EFB_TARGET;
extern const Info<bool> GFX_DUMP_XFB_TARGET;
extern const Info<bool> GFX_DUMP_FRAMES_AS_IMAGES;
extern const Info<bool> GFX_USE_LOSSLESS;
extern const Info<std::string> GFX_DUMP_FORMAT;
extern const Info<std::string> GFX_DUMP_CODEC;
extern const Info<std::string> GFX_DUMP_PIXEL_FORMAT;
extern const Info<std::string> GFX_DUMP_ENCODER;
extern const Info<std::string> GFX_DUMP_PATH;
extern const Info<int> GFX_BITRATE_KBPS;
extern const Info<FrameDumpResolutionType> GFX_FRAME_DUMPS_RESOLUTION_TYPE;
extern const Info<int> GFX_PNG_COMPRESSION_LEVEL;
extern const Info<bool> GFX_ENABLE_GPU_TEXTURE_DECODING;
extern const Info<bool> GFX_ENABLE_PIXEL_LIGHTING;
extern const Info<bool> GFX_FAST_DEPTH_CALC;
extern const Info<u32> GFX_MSAA;
extern const Info<bool> GFX_SSAA;
extern const Info<int> GFX_EFB_SCALE;
extern const Info<int> GFX_MAX_EFB_SCALE;
extern const Info<bool> GFX_TEXFMT_OVERLAY_ENABLE;
extern const Info<bool> GFX_TEXFMT_OVERLAY_CENTER;
extern const Info<bool> GFX_ENABLE_WIREFRAME;
extern const Info<bool> GFX_DISABLE_FOG;
extern const Info<bool> GFX_BORDERLESS_FULLSCREEN;
extern const Info<bool> GFX_ENABLE_VALIDATION_LAYER;
extern const Info<bool> GFX_BACKEND_MULTITHREADING;
extern const Info<int> GFX_COMMAND_BUFFER_EXECUTE_INTERVAL;
extern const Info<bool> GFX_SHADER_CACHE;
extern const Info<bool> GFX_WAIT_FOR_SHADERS_BEFORE_STARTING;
extern const Info<ShaderCompilationMode> GFX_SHADER_COMPILATION_MODE;
extern const Info<int> GFX_SHADER_COMPILER_THREADS;
extern const Info<int> GFX_SHADER_PRECOMPILER_THREADS;
extern const Info<bool> GFX_SAVE_TEXTURE_CACHE_TO_STATE;
extern const Info<bool> GFX_PREFER_VS_FOR_LINE_POINT_EXPANSION;
extern const Info<bool> GFX_CPU_CULL;

extern const Info<TriState> GFX_MTL_MANUALLY_UPLOAD_BUFFERS;
extern const Info<TriState> GFX_MTL_USE_PRESENT_DRAWABLE;

extern const Info<bool> GFX_SW_DUMP_OBJECTS;
extern const Info<bool> GFX_SW_DUMP_TEV_STAGES;
extern const Info<bool> GFX_SW_DUMP_TEV_TEX_FETCHES;

extern const Info<bool> GFX_PREFER_GLES;

extern const Info<bool> GFX_MODS_ENABLE;

// Graphics.Enhancements

extern const Info<TextureFilteringMode> GFX_ENHANCE_FORCE_TEXTURE_FILTERING;
// NOTE - this is x in (1 << x)
extern const Info<AnisotropicFilteringMode> GFX_ENHANCE_MAX_ANISOTROPY;
extern const Info<OutputResamplingMode> GFX_ENHANCE_OUTPUT_RESAMPLING;
extern const Info<std::string> GFX_ENHANCE_POST_SHADER;
extern const Info<bool> GFX_ENHANCE_FORCE_TRUE_COLOR;
extern const Info<bool> GFX_ENHANCE_DISABLE_COPY_FILTER;
extern const Info<bool> GFX_ENHANCE_ARBITRARY_MIPMAP_DETECTION;
extern const Info<float> GFX_ENHANCE_ARBITRARY_MIPMAP_DETECTION_THRESHOLD;
extern const Info<bool> GFX_ENHANCE_HDR_OUTPUT;

// Color.Correction

static constexpr float GFX_CC_GAME_GAMMA_MIN = 2.2f;
static constexpr float GFX_CC_GAME_GAMMA_MAX = 2.8f;

static constexpr float GFX_CC_DISPLAY_GAMMA_MIN = 2.2f;
static constexpr float GFX_CC_DISPLAY_GAMMA_MAX = 2.4f;

static constexpr float GFX_CC_HDR_PAPER_WHITE_NITS_MIN = 80.f;
static constexpr float GFX_CC_HDR_PAPER_WHITE_NITS_MAX = 500.f;

extern const Info<bool> GFX_CC_CORRECT_COLOR_SPACE;
extern const Info<ColorCorrectionRegion> GFX_CC_GAME_COLOR_SPACE;
extern const Info<bool> GFX_CC_CORRECT_GAMMA;
extern const Info<float> GFX_CC_GAME_GAMMA;
extern const Info<bool> GFX_CC_SDR_DISPLAY_GAMMA_SRGB;
extern const Info<float> GFX_CC_SDR_DISPLAY_CUSTOM_GAMMA;
extern const Info<float> GFX_CC_HDR_PAPER_WHITE_NITS;

// Graphics.Stereoscopy

extern const Info<StereoMode> GFX_STEREO_MODE;
extern const Info<bool> GFX_STEREO_PER_EYE_RESOLUTION_FULL;
extern const Info<float> GFX_STEREO_DEPTH;
extern const Info<float> GFX_STEREO_CONVERGENCE_PERCENTAGE;
extern const Info<bool> GFX_STEREO_SWAP_EYES;
extern const Info<float> GFX_STEREO_CONVERGENCE;
extern const Info<bool> GFX_STEREO_EFB_MONO_DEPTH;
extern const Info<float> GFX_STEREO_DEPTH_PERCENTAGE;

// Stereoscopy pseudo-limits for consistent behavior between enhancements tab and hotkeys.
static constexpr float GFX_STEREO_DEPTH_MAXIMUM = 100;
static constexpr float GFX_STEREO_CONVERGENCE_MAXIMUM = 200;

// Graphics.VR

extern const Info<bool> GFX_VR_ENABLE_OPENXR;
extern const Info<bool> GFX_VR_AUTO_IMMEDIATE_XFB;
extern const Info<float> GFX_VR_UNITS_PER_METER;
extern const Info<float> GFX_VR_LEAN_BACK_ANGLE;
extern const Info<bool> GFX_VR_ENABLE_CAMERA_FORWARD;
extern const Info<float> GFX_VR_CAMERA_FORWARD;
extern const Info<bool> GFX_VR_ENABLE_CAMERA_HEIGHT;
extern const Info<float> GFX_VR_CAMERA_HEIGHT;

static constexpr float GFX_VR_UNITS_PER_METER_MIN = 0.1f;
static constexpr float GFX_VR_UNITS_PER_METER_MAX = 500.0f;
static constexpr float GFX_VR_UNITS_PER_METER_STEP = 0.1f;
static constexpr float GFX_VR_LEAN_BACK_ANGLE_MIN = -45.0f;
static constexpr float GFX_VR_LEAN_BACK_ANGLE_MAX = 45.0f;
static constexpr float GFX_VR_LEAN_BACK_ANGLE_STEP = 0.1f;
static constexpr float GFX_VR_CAMERA_FORWARD_MIN = -20.0f;
static constexpr float GFX_VR_CAMERA_FORWARD_MAX = 20.0f;
static constexpr float GFX_VR_CAMERA_FORWARD_STEP = 0.1f;
static constexpr float GFX_VR_CAMERA_HEIGHT_MIN = -20.0f;
static constexpr float GFX_VR_CAMERA_HEIGHT_MAX = 20.0f;
static constexpr float GFX_VR_CAMERA_HEIGHT_STEP = 0.1f;

extern const Info<bool> GFX_VR_VIRTUAL_SCREEN;
extern const Info<float> GFX_VR_SCREEN_DISTANCE;
extern const Info<float> GFX_VR_SCREEN_SIZE;
extern const Info<float> GFX_VR_HEAD_LOCKED_CURVATURE;
extern const Info<bool> GFX_VR_DONT_CLEAR_SCREEN;
extern const Info<bool> GFX_VR_LOAD_CUSTOM_SHADERS;
extern const Info<bool> GFX_VR_ENABLE_OPENXR_CONFIG_SCENE;
extern const Info<bool> GFX_VR_DISABLE_CPU_CULL;
extern const Info<OpenXROpcodeReplayMode> GFX_VR_OPCODE_REPLAY;
extern const Info<OpenXRMirrorView> GFX_VR_MIRROR_VIEW;
extern const Info<OpenXRReferenceSpaceMode> GFX_VR_REFERENCE_SPACE_MODE;
extern const Info<OpenXRTrackingMode> GFX_VR_TRACKING_MODE;
extern const Info<bool> GFX_VR_USE_OPENXR_PLAY_SPACE_CENTER;
extern const Info<int> GFX_VR_OPCODE_REPLAY_TARGET_REFRESH_RATE;
extern const Info<int> GFX_VR_FORCED_VBI_FREQUENCY;
extern const Info<bool> GFX_VR_AUTO_VBI_FROM_HMD;
extern const Info<bool> GFX_VR_AUTO_LAYER_SPREAD;
extern const Info<float> GFX_VR_LAYER_OFFSET;
extern const Info<float> GFX_VR_ELEMENT_DEPTH;
extern const Info<bool> GFX_VR_HUD_3D_ENABLE;
extern const Info<float> GFX_VR_HUD_3D_CLOSER;
extern const Info<bool> GFX_VR_REMOVE_BARS;
extern const Info<bool> GFX_VR_ORTHO_SCISSOR_FIX;
extern const Info<bool> GFX_VR_METROID_VISOR_FIX;
extern const Info<bool> GFX_VR_LOCK_HEAD_POSE;
extern const Info<bool> GFX_VR_AR_MODE;
extern const Info<bool> GFX_VR_AR_MODE_DEBUG;
extern const Info<float> GFX_VR_AR_BACKGROUND_ALPHA;
static constexpr float GFX_VR_AR_BACKGROUND_ALPHA_MIN = 0.0f;
static constexpr float GFX_VR_AR_BACKGROUND_ALPHA_MAX = 1.0f;
static constexpr float GFX_VR_AR_BACKGROUND_ALPHA_STEP = 0.05f;
extern const Info<float> GFX_VR_GAMMA;
extern const Info<int> GFX_VR_CLEAR_EFB_COPIES;
static constexpr int GFX_VR_CLEAR_EFB_MIN = 0;
static constexpr int GFX_VR_CLEAR_EFB_MAX = 640;
static constexpr int GFX_VR_CLEAR_EFB_STEP = 10;
extern const Info<bool> GFX_VR_USE_VULKAN_MULTIVIEW;
extern const Info<bool> GFX_VR_ANDROID_DIRECT_TO_HMD;
extern const Info<bool> GFX_VR_QUEST_CPU_LEVEL_5_HINT;
extern const Info<int> GFX_VR_FOVEATION_LEVEL;
extern const Info<int> GFX_VR_DISPLAY_REFRESH_RATE;
extern const Info<bool> GFX_VR_DRAW_DEBUG_LOGGING;

// Fixed Foveated Rendering level (XR_FB_foveation). Off/Low/Medium/High map to the FB foveation
// levels; higher levels reclaim more GPU fill at the cost of more peripheral blur.
static constexpr int GFX_VR_FOVEATION_LEVEL_OFF = 0;
static constexpr int GFX_VR_FOVEATION_LEVEL_LOW = 1;
static constexpr int GFX_VR_FOVEATION_LEVEL_MEDIUM = 2;
static constexpr int GFX_VR_FOVEATION_LEVEL_HIGH = 3;

// HMD panel refresh rate requested via XR_FB_display_refresh_rate. Auto = leave the runtime's
// choice. The others are the rates a Quest 3 panel supports (60 Hz is NOT a panel rate).
static constexpr int GFX_VR_DISPLAY_REFRESH_RATE_AUTO = 0;
static constexpr int GFX_VR_DISPLAY_REFRESH_RATE_72 = 72;
static constexpr int GFX_VR_DISPLAY_REFRESH_RATE_90 = 90;
static constexpr int GFX_VR_DISPLAY_REFRESH_RATE_120 = 120;

static constexpr float GFX_VR_SCREEN_DISTANCE_MIN = 0.5f;
static constexpr float GFX_VR_SCREEN_DISTANCE_MAX = 10.0f;
static constexpr float GFX_VR_SCREEN_DISTANCE_STEP = 0.1f;
static constexpr float GFX_VR_SCREEN_SIZE_MIN = 0.5f;
static constexpr float GFX_VR_SCREEN_SIZE_MAX = 5.0f;
static constexpr float GFX_VR_SCREEN_SIZE_STEP = 0.1f;
static constexpr float GFX_VR_HEAD_LOCKED_CURVATURE_MIN = 0.0f;
static constexpr float GFX_VR_HEAD_LOCKED_CURVATURE_MAX = 5.0f;
static constexpr float GFX_VR_HEAD_LOCKED_CURVATURE_STEP = 0.01f;
static constexpr int GFX_VR_FORCED_VBI_FREQUENCY_AUTO = -1;
static constexpr int GFX_VR_FORCED_VBI_FREQUENCY_OFF = 0;
static constexpr int GFX_VR_FORCED_VBI_FREQUENCY_72 = 72;
static constexpr int GFX_VR_FORCED_VBI_FREQUENCY_90 = 90;
static constexpr int GFX_VR_FORCED_VBI_FREQUENCY_120 = 120;
static constexpr int GFX_VR_OPCODE_REPLAY_TARGET_REFRESH_RATE_AUTO = -1;
static constexpr int GFX_VR_OPCODE_REPLAY_TARGET_REFRESH_RATE_72 = 72;
static constexpr int GFX_VR_OPCODE_REPLAY_TARGET_REFRESH_RATE_90 = 90;
static constexpr int GFX_VR_OPCODE_REPLAY_TARGET_REFRESH_RATE_120 = 120;
static constexpr float GFX_VR_LAYER_OFFSET_MIN = 0.0001f;
static constexpr float GFX_VR_LAYER_OFFSET_MAX = 0.01f;
static constexpr float GFX_VR_LAYER_OFFSET_STEP = 0.0001f;
static constexpr float GFX_VR_ELEMENT_DEPTH_MIN = 0.0f;
static constexpr float GFX_VR_ELEMENT_DEPTH_MAX = 10.0f;
static constexpr float GFX_VR_ELEMENT_DEPTH_STEP = 0.0001f;
static constexpr float GFX_VR_HUD_3D_CLOSER_MIN = 0.0f;
static constexpr float GFX_VR_HUD_3D_CLOSER_MAX = 1.0f;
static constexpr float GFX_VR_HUD_3D_CLOSER_STEP = 0.01f;
static constexpr float GFX_VR_GAMMA_MIN = 1.0f;
static constexpr float GFX_VR_GAMMA_MAX = 3.0f;
static constexpr float GFX_VR_GAMMA_STEP = 0.1f;

inline int NormalizeVRForcedVBIFrequency(int frequency)
{
  switch (frequency)
  {
  case GFX_VR_FORCED_VBI_FREQUENCY_AUTO:
  case GFX_VR_FORCED_VBI_FREQUENCY_72:
  case GFX_VR_FORCED_VBI_FREQUENCY_90:
  case GFX_VR_FORCED_VBI_FREQUENCY_120:
    return frequency;
  default:
    return GFX_VR_FORCED_VBI_FREQUENCY_OFF;
  }
}

inline int NormalizeVROpcodeReplayTargetRefreshRate(int refresh_rate)
{
  switch (refresh_rate)
  {
  case GFX_VR_OPCODE_REPLAY_TARGET_REFRESH_RATE_AUTO:
  case 0:  // Legacy value from the previous OutputRefreshRate Auto option.
    return GFX_VR_OPCODE_REPLAY_TARGET_REFRESH_RATE_AUTO;
  case GFX_VR_OPCODE_REPLAY_TARGET_REFRESH_RATE_72:
  case GFX_VR_OPCODE_REPLAY_TARGET_REFRESH_RATE_90:
  case GFX_VR_OPCODE_REPLAY_TARGET_REFRESH_RATE_120:
    return refresh_rate;
  default:
    return GFX_VR_OPCODE_REPLAY_TARGET_REFRESH_RATE_90;
  }
}

inline int NormalizeVRDisplayRefreshRate(int refresh_rate)
{
  switch (refresh_rate)
  {
  case GFX_VR_DISPLAY_REFRESH_RATE_72:
  case GFX_VR_DISPLAY_REFRESH_RATE_90:
  case GFX_VR_DISPLAY_REFRESH_RATE_120:
    return refresh_rate;
  default:
    return GFX_VR_DISPLAY_REFRESH_RATE_AUTO;
  }
}

inline int ChooseClosestVRForcedVBIFrequency(float refresh_rate_hz)
{
  if (refresh_rate_hz <= 0.0f)
    return GFX_VR_FORCED_VBI_FREQUENCY_OFF;

  int closest_frequency = GFX_VR_FORCED_VBI_FREQUENCY_72;
  float closest_distance = std::abs(refresh_rate_hz - GFX_VR_FORCED_VBI_FREQUENCY_72);

  constexpr int frequencies[] = {GFX_VR_FORCED_VBI_FREQUENCY_90,
                                 GFX_VR_FORCED_VBI_FREQUENCY_120};
  for (const int frequency : frequencies)
  {
    const float distance = std::abs(refresh_rate_hz - static_cast<float>(frequency));
    if (distance < closest_distance)
    {
      closest_frequency = frequency;
      closest_distance = distance;
    }
  }

  return closest_frequency;
}

// Graphics.Hacks

extern const Info<bool> GFX_HACK_EFB_ACCESS_ENABLE;
extern const Info<bool> GFX_HACK_EFB_DEFER_INVALIDATION;
extern const Info<int> GFX_HACK_EFB_ACCESS_TILE_SIZE;
extern const Info<bool> GFX_HACK_BBOX_ENABLE;
extern const Info<bool> GFX_HACK_FORCE_PROGRESSIVE;
extern const Info<bool> GFX_HACK_SKIP_EFB_COPY_TO_RAM;
extern const Info<bool> GFX_HACK_SKIP_XFB_COPY_TO_RAM;
extern const Info<bool> GFX_HACK_DISABLE_COPY_TO_VRAM;
extern const Info<bool> GFX_HACK_DEFER_EFB_COPIES;
extern const Info<bool> GFX_HACK_IMMEDIATE_XFB;
extern const Info<bool> GFX_HACK_CAP_IMMEDIATE_XFB;
extern const Info<bool> GFX_HACK_SKIP_DUPLICATE_XFBS;
extern const Info<bool> GFX_HACK_EARLY_XFB_OUTPUT;
extern const Info<bool> GFX_HACK_COPY_EFB_SCALED;
extern const Info<bool> GFX_HACK_EFB_EMULATE_FORMAT_CHANGES;
extern const Info<bool> GFX_HACK_VERTEX_ROUNDING;
extern const Info<bool> GFX_HACK_VI_SKIP;
extern const Info<u32> GFX_HACK_MISSING_COLOR_VALUE;
extern const Info<bool> GFX_HACK_FAST_TEXTURE_SAMPLING;
#ifdef __APPLE__
extern const Info<bool> GFX_HACK_NO_MIPMAPPING;
#endif

// Graphics.GameSpecific

extern const Info<bool> GFX_PERF_QUERIES_ENABLE;

// Android custom GPU drivers

extern const Info<std::string> GFX_DRIVER_LIB_NAME;

// Vertex loader

extern const Info<VertexLoaderType> GFX_VERTEX_LOADER_TYPE;

}  // namespace Config
