// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef ENABLE_VR

// VulkanLoader.h must come first — it defines VK_NO_PROTOTYPES before vulkan.h.
#include "VideoBackends/Vulkan/VulkanLoader.h"

#define XR_USE_GRAPHICS_API_VULKAN

#include "VideoBackends/Vulkan/VulkanOpenXR.h"

#include <cstdlib>
#include <ctime>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Common/Assert.h"
#include "Common/Logging/Log.h"
#include "Common/ScopeGuard.h"
#include "Common/Thread.h"
#include "Common/Timer.h"

#include "VideoBackends/Vulkan/CommandBufferManager.h"
#include "VideoBackends/Vulkan/StateTracker.h"
#include "VideoBackends/Vulkan/VKTexture.h"
#include "VideoBackends/Vulkan/VulkanContext.h"
#include "VideoCommon/PerformanceMetrics.h"
#include "VideoCommon/TextureConfig.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/VR/OpenXRManager.h"
#include "VideoCommon/VR/PrimedGunOverlayCommon.h"

#ifdef _WIN32
#include <windows.h>  // for SEH __try/__except
#endif

#if defined(ANDROID)
#include <android/log.h>
#endif

namespace Vulkan
{
std::unique_ptr<VulkanOpenXR> g_openxr_vk;
namespace PGO = PrimedGun::Overlay;

namespace
{
struct CinematicScreenAnchor
{
  bool valid = false;
  uint32_t generation = 0;
  XrQuaternionf orientation{0.0f, 0.0f, 0.0f, 1.0f};
  XrVector3f position{};
};

static CinematicScreenAnchor s_cinematic_screen_anchor;

bool ReadCinematicScreenViewerPose(XrQuaternionf* orientation, XrVector3f* position)
{
  const Common::VR::OpenXRInputSnapshot snapshot = Common::VR::OpenXRInputState::GetSnapshot();

  if (snapshot.head_pose.valid)
  {
    *orientation = {snapshot.head_pose.orientation[0], snapshot.head_pose.orientation[1],
                    snapshot.head_pose.orientation[2], snapshot.head_pose.orientation[3]};
    *position = {snapshot.head_pose.position[0] + snapshot.tracking_origin_position[0],
                 snapshot.head_pose.position[1] + snapshot.tracking_origin_position[1],
                 snapshot.head_pose.position[2] + snapshot.tracking_origin_position[2]};
    return true;
  }

  if (VR::g_openxr->AreSubmittedEyeViewsValid())
  {
    const auto& eyes = VR::g_openxr->GetSubmittedEyeViews();
    *orientation = eyes[0].pose.orientation;
    *position = {0.5f * (eyes[0].pose.position.x + eyes[1].pose.position.x),
                 0.5f * (eyes[0].pose.position.y + eyes[1].pose.position.y),
                 0.5f * (eyes[0].pose.position.z + eyes[1].pose.position.z)};
    return true;
  }

  return false;
}

void ResetCinematicScreenAnchor()
{
  s_cinematic_screen_anchor = {};
}

bool BuildCinematicScreenLayer(const std::array<XRVkEyeSwapchain, 2>& eye_swapchains,
                               uint32_t generation, XrCompositionLayerQuad* layer)
{
  if (!VR::g_openxr || !layer || eye_swapchains[0].swapchain == XR_NULL_HANDLE)
    return false;

  if (!s_cinematic_screen_anchor.valid || s_cinematic_screen_anchor.generation != generation)
  {
    XrQuaternionf orientation{};
    XrVector3f position{};
    if (!ReadCinematicScreenViewerPose(&orientation, &position))
      return false;

    s_cinematic_screen_anchor.valid = true;
    s_cinematic_screen_anchor.generation = generation;
    s_cinematic_screen_anchor.orientation = PGO::YawOnlyQuaternion(orientation);
    s_cinematic_screen_anchor.position = position;
  }

  const XrVector3f offset =
      PGO::RotateVector(s_cinematic_screen_anchor.orientation, {0.0f, 0.0f, -2.0f});
  *layer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
  layer->space = VR::g_openxr->GetReferenceSpace();
  layer->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
  layer->subImage.swapchain = eye_swapchains[0].swapchain;
  layer->subImage.imageRect.offset = {0, 0};
  layer->subImage.imageRect.extent = {static_cast<int32_t>(eye_swapchains[0].width),
                                      static_cast<int32_t>(eye_swapchains[0].height)};
  layer->pose.orientation = s_cinematic_screen_anchor.orientation;
  layer->pose.position = {s_cinematic_screen_anchor.position.x + offset.x,
                          s_cinematic_screen_anchor.position.y + offset.y,
                          s_cinematic_screen_anchor.position.z + offset.z};
  layer->size = {2.2f, 1.65f};
  return true;
}

bool SelectPrimedGunOverlaySwapchainFormat(XrSession session, int64_t* out_format)
{
  uint32_t format_count = 0;
  XrResult result = xrEnumerateSwapchainFormats(session, 0, &format_count, nullptr);
  if (XR_FAILED(result) || format_count == 0)
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR: xrEnumerateSwapchainFormats for PrimedGun overlay failed ({}).",
                  static_cast<int>(result));
    return false;
  }

  std::vector<int64_t> runtime_formats(format_count);
  result = xrEnumerateSwapchainFormats(session, format_count, &format_count, runtime_formats.data());
  if (XR_FAILED(result))
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR: xrEnumerateSwapchainFormats for PrimedGun overlay failed ({}).",
                  static_cast<int>(result));
    return false;
  }

  // PrimedGun's overlay builder matches the existing D3D path, which uploads these bytes into an
  // R8G8B8A8 swapchain. Prefer the same Vulkan format and only swizzle when the runtime requires
  // BGRA.
  static constexpr std::array<VkFormat, 4> preferred_formats = {
      VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM,
      VK_FORMAT_B8G8R8A8_SRGB};

  for (const VkFormat preferred : preferred_formats)
  {
    const int64_t wanted = static_cast<int64_t>(preferred);
    if (std::find(runtime_formats.begin(), runtime_formats.end(), wanted) != runtime_formats.end())
    {
      *out_format = wanted;
      return true;
    }
  }

  WARN_LOG_FMT(VIDEO, "OpenXR: No RGBA/BGRA format available for PrimedGun overlay.");
  return false;
}

bool PrimedGunOverlayFormatIsBgra(VkFormat format)
{
  return format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
}

std::vector<uint32_t> ConvertPrimedGunOverlayPixelsForVkFormat(const uint32_t* pixels,
                                                              size_t pixel_count,
                                                              VkFormat format)
{
  std::vector<uint32_t> converted(pixel_count);
  if (!PrimedGunOverlayFormatIsBgra(format))
  {
    std::copy_n(pixels, pixel_count, converted.begin());
    return converted;
  }

  for (size_t i = 0; i < pixel_count; ++i)
  {
    const uint32_t argb = pixels[i];
    const uint32_t a = argb & 0xFF000000u;
    const uint32_t r = (argb >> 16) & 0xFFu;
    const uint32_t g = (argb >> 8) & 0xFFu;
    const uint32_t b = argb & 0xFFu;
    converted[i] = a | (b << 16) | (g << 8) | r;
  }
  return converted;
}

uint64_t ElapsedUs(uint64_t start_us, uint64_t end_us)
{
  if (start_us == 0 || end_us <= start_us)
    return 0;

  return end_us - start_us;
}

#if !defined(ANDROID)
int64_t ThreadCpuNs()
{
  timespec ts{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

int64_t SteadyNs()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// GPU busy time of this process from the kernel (DRM fdinfo, "drm-engine-gpu" in ns; the msm
// driver keeps it per client). 0 when the kernel does not report it.
uint64_t ReadGpuBusyNs()
{
  uint64_t total = 0;
  std::set<std::string> clients;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd", ec))
  {
    std::error_code lec;
    const auto target = std::filesystem::read_symlink(entry.path(), lec);
    if (lec || target.native().rfind("/dev/dri/", 0) != 0)
      continue;
    std::ifstream info("/proc/self/fdinfo/" + entry.path().filename().native());
    std::string line, client;
    uint64_t busy = 0;
    while (std::getline(info, line))
    {
      if (line.rfind("drm-client-id:", 0) == 0)
        client = line.substr(14);
      else if (line.rfind("drm-engine-gpu:", 0) == 0)
        busy = std::strtoull(line.c_str() + 15, nullptr, 10);
    }
    if (!client.empty() && !clients.insert(client).second)
      continue;
    total += busy;
  }
  return total;
}

bool EnvIs(const char* name, const char* value)
{
  const char* env = std::getenv(name);
  return env && std::strcmp(env, value) == 0;
}
#endif

static void AppendOptionalOpenXRExtensions(std::vector<const char*>* extensions)
{
  if (VR::OpenXRManager::IsRuntimeExtensionSupported(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME))
  {
    extensions->push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    INFO_LOG_FMT(VIDEO, "OpenXR: Enabling XR_FB_display_refresh_rate.");
  }
}
}  // namespace

#if defined(ANDROID)
static void AppendOptionalAndroidOpenXRExtensions(std::vector<const char*>* extensions)
{
  if (VR::OpenXRManager::IsRuntimeExtensionSupported(
          XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME))
  {
    extensions->push_back(XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME);
    INFO_LOG_FMT(VIDEO, "OpenXR: Enabling XR_KHR_android_thread_settings.");
  }
  if (VR::OpenXRManager::IsRuntimeExtensionSupported(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME))
  {
    extensions->push_back(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
    INFO_LOG_FMT(VIDEO, "OpenXR: Enabling XR_EXT_performance_settings.");
  }
}
#endif

XRVkEyeSwapchain::XRVkEyeSwapchain() = default;
XRVkEyeSwapchain::~XRVkEyeSwapchain() = default;
XRVkEyeSwapchain::XRVkEyeSwapchain(XRVkEyeSwapchain&&) noexcept = default;
XRVkEyeSwapchain& XRVkEyeSwapchain::operator=(XRVkEyeSwapchain&&) noexcept = default;

XRVkLayeredSwapchain::XRVkLayeredSwapchain() = default;
XRVkLayeredSwapchain::~XRVkLayeredSwapchain() = default;
XRVkLayeredSwapchain::XRVkLayeredSwapchain(XRVkLayeredSwapchain&&) noexcept = default;
XRVkLayeredSwapchain& XRVkLayeredSwapchain::operator=(XRVkLayeredSwapchain&&) noexcept = default;

XRPrimedGunVkOverlaySwapchain::XRPrimedGunVkOverlaySwapchain() = default;
XRPrimedGunVkOverlaySwapchain::~XRPrimedGunVkOverlaySwapchain() = default;
XRPrimedGunVkOverlaySwapchain::XRPrimedGunVkOverlaySwapchain(
    XRPrimedGunVkOverlaySwapchain&&) noexcept = default;
XRPrimedGunVkOverlaySwapchain& XRPrimedGunVkOverlaySwapchain::operator=(
    XRPrimedGunVkOverlaySwapchain&&) noexcept = default;

XRPrimedGunVkLaserSwapchain::XRPrimedGunVkLaserSwapchain() = default;
XRPrimedGunVkLaserSwapchain::~XRPrimedGunVkLaserSwapchain() = default;
XRPrimedGunVkLaserSwapchain::XRPrimedGunVkLaserSwapchain(
    XRPrimedGunVkLaserSwapchain&&) noexcept = default;
XRPrimedGunVkLaserSwapchain& XRPrimedGunVkLaserSwapchain::operator=(
    XRPrimedGunVkLaserSwapchain&&) noexcept = default;

static const char* VkFormatToString(int64_t format)
{
  switch (static_cast<VkFormat>(format))
  {
  case VK_FORMAT_R8G8B8A8_UNORM:
    return "VK_FORMAT_R8G8B8A8_UNORM";
  case VK_FORMAT_R8G8B8A8_SRGB:
    return "VK_FORMAT_R8G8B8A8_SRGB";
  case VK_FORMAT_B8G8R8A8_UNORM:
    return "VK_FORMAT_B8G8R8A8_UNORM";
  case VK_FORMAT_B8G8R8A8_SRGB:
    return "VK_FORMAT_B8G8R8A8_SRGB";
  case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    return "VK_FORMAT_A2B10G10R10_UNORM_PACK32";
  case VK_FORMAT_R16G16B16A16_SFLOAT:
    return "VK_FORMAT_R16G16B16A16_SFLOAT";
  default:
    return "UNKNOWN_VK_FORMAT";
  }
}

static AbstractTextureFormat VkFormatToAbstractFormat(VkFormat format)
{
  switch (format)
  {
  case VK_FORMAT_R8G8B8A8_UNORM:
  case VK_FORMAT_R8G8B8A8_SRGB:
    return AbstractTextureFormat::RGBA8;
  case VK_FORMAT_B8G8R8A8_UNORM:
  case VK_FORMAT_B8G8R8A8_SRGB:
    return AbstractTextureFormat::BGRA8;
  case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    return AbstractTextureFormat::RGB10_A2;
  case VK_FORMAT_R16G16B16A16_SFLOAT:
    return AbstractTextureFormat::RGBA16F;
  default:
    return AbstractTextureFormat::RGBA8;
  }
}

static bool SelectSwapchainFormat(XrSession session, int64_t* out_format)
{
  uint32_t format_count = 0;
  XrResult result = xrEnumerateSwapchainFormats(session, 0, &format_count, nullptr);
  if (XR_FAILED(result) || format_count == 0)
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR: xrEnumerateSwapchainFormats (count) failed ({}).",
                  static_cast<int>(result));
    return false;
  }

  std::vector<int64_t> runtime_formats(format_count);
  result = xrEnumerateSwapchainFormats(session, format_count, &format_count, runtime_formats.data());
  if (XR_FAILED(result))
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR: xrEnumerateSwapchainFormats failed ({}).",
                  static_cast<int>(result));
    return false;
  }

  for (const int64_t format : runtime_formats)
  {
    INFO_LOG_FMT(VIDEO, "OpenXR: Runtime swapchain format {} ({})", static_cast<long long>(format),
                 VkFormatToString(format));
  }

#if defined(ANDROID)
  // Quest's compositor expects sRGB swapchains for Dolphin's gamma-encoded XFB. We render through
  // a UNORM view alias below to avoid a double gamma encode.
  static constexpr std::array<VkFormat, 6> preferred_formats = {
      VK_FORMAT_R8G8B8A8_SRGB,            VK_FORMAT_B8G8R8A8_SRGB,
      VK_FORMAT_R8G8B8A8_UNORM,           VK_FORMAT_B8G8R8A8_UNORM,
      VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_R16G16B16A16_SFLOAT};
#else
  // Prefer sRGB on PC so the OpenXR compositor decodes Dolphin's gamma-encoded XFB correctly.
  static constexpr std::array<VkFormat, 6> preferred_formats = {
      VK_FORMAT_R8G8B8A8_SRGB,            VK_FORMAT_B8G8R8A8_SRGB,
      VK_FORMAT_R8G8B8A8_UNORM,           VK_FORMAT_B8G8R8A8_UNORM,
      VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_R16G16B16A16_SFLOAT};
#endif

  for (const VkFormat preferred : preferred_formats)
  {
    const int64_t wanted = static_cast<int64_t>(preferred);
    if (std::find(runtime_formats.begin(), runtime_formats.end(), wanted) != runtime_formats.end())
    {
      *out_format = wanted;
      INFO_LOG_FMT(VIDEO, "OpenXR: Selected swapchain format {} ({}).",
                   static_cast<long long>(*out_format), VkFormatToString(*out_format));
      return true;
    }
  }

  *out_format = runtime_formats.front();
  WARN_LOG_FMT(VIDEO,
               "OpenXR: No preferred Vulkan swapchain format found; falling back to runtime "
               "format {} ({}).",
               static_cast<long long>(*out_format), VkFormatToString(*out_format));
  return true;
}

// Separate function for SEH protection — __try cannot be used in functions
// that have C++ objects requiring unwinding.
#ifdef _WIN32
static XrResult SafeCreateSession(XrInstance instance, const XrSessionCreateInfo* info,
                                   XrSession* session)
{
  __try
  {
    return xrCreateSession(instance, info, session);
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    ERROR_LOG_FMT(VIDEO,
                  "OpenXR: xrCreateSession CRASHED (exception {:#010x}). "
                  "The OpenXR runtime may require Vulkan extensions that Dolphin did not enable. "
                  "Check the 'Required Vulkan instance/device extensions' log lines above.",
                  static_cast<unsigned>(GetExceptionCode()));
    return XR_ERROR_RUNTIME_FAILURE;
  }
}
#else
static XrResult SafeCreateSession(XrInstance instance, const XrSessionCreateInfo* info,
                                   XrSession* session)
{
  return xrCreateSession(instance, info, session);
}
#endif

VulkanOpenXR::VulkanOpenXR() = default;

VulkanOpenXR::~VulkanOpenXR()
{
  Shutdown();
}

std::unique_lock<std::mutex> VulkanOpenXR::AcquireGraphicsQueueLock()
{
  if (g_command_buffer_mgr)
    return g_command_buffer_mgr->AcquireQueueLock();

  return {};
}

bool VulkanOpenXR::WaitForPendingFrameFinalization(std::string_view reason)
{
#if defined(ANDROID)
  if (m_async_frame_finalization_in_flight.load(std::memory_order_acquire))
  {
    static uint32_t s_async_wait_log_count = 0;
    if (!g_command_buffer_mgr)
    {
      ERROR_LOG_FMT(VIDEO,
                    "OpenXR Vulkan: pending async final XR submit cannot be waited because the "
                    "command buffer manager is gone.");
      return false;
    }

    const uint64_t wait_start_us = Common::Timer::NowUs();
    g_command_buffer_mgr->WaitForWorkerThreadIdle();
    const uint64_t wait_us = ElapsedUs(wait_start_us, Common::Timer::NowUs());
    if (s_async_wait_log_count < 20 || wait_us >= 500)
    {
      INFO_LOG_FMT(VIDEO,
                   "OpenXR Vulkan: waited {} us for pending async final XR submit ({}).",
                   wait_us, reason.empty() ? "frame loop" : reason);
      __android_log_print(ANDROID_LOG_INFO, "DolphinXR",
                          "OpenXR Vulkan: waited %llu us for pending async final XR submit (%.*s)",
                          static_cast<unsigned long long>(wait_us),
                          static_cast<int>(reason.empty() ? std::string_view{"frame loop"}.size() :
                                                            reason.size()),
                          reason.empty() ? "frame loop" : reason.data());
      s_async_wait_log_count++;
    }
  }

  if (m_async_frame_finalization_failed.exchange(false, std::memory_order_acq_rel))
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR Vulkan: previous async final XR submit failed.");
    return false;
  }
#else
  // Cycle: the next frame begins (and its eye images are acquired) only once the releaser
  // thread has ended the last one.
  if (m_pacing == Pacing::Cycle && m_pacer.joinable() &&
      std::this_thread::get_id() != m_pacer.get_id())
  {
    const int64_t wait_start = SteadyNs();
    std::unique_lock<std::mutex> lock(m_pacer_mutex);
    if (!m_pacer_cv.wait_for(lock, std::chrono::seconds(2), [&] {
          return (m_pending.empty() && !m_cycle_busy) || m_pacer_exit.load();
        }))
    {
      ERROR_LOG_FMT(OPENXR, "PrimeFrame cycle: the last frame was not ended within 2 s ({}).",
                    reason);
      return false;
    }
    m_cycle_video_wait_ns += SteadyNs() - wait_start;
  }
#endif

  return true;
}

#if !defined(ANDROID)
// Cycle overlap, video thread: before the next xrWaitFrame, the frame waited last must have been
// begun (the releaser thread begins it right after the end of the frame before it).
bool VulkanOpenXR::CycleBeforeWait()
{
  if (!m_pacer.joinable())
    return true;
  const int64_t wait_start = SteadyNs();
  std::unique_lock<std::mutex> lock(m_pacer_mutex);
  if (!m_pacer_cv.wait_for(lock, std::chrono::seconds(2), [&] {
        return !m_cycle_begin_pending || m_pacer_exit.load();
      }))
  {
    ERROR_LOG_FMT(OPENXR, "PrimeFrame cycle: the waited frame was not begun within 2 s.");
    return false;
  }
  m_cycle_video_wait_ns += SteadyNs() - wait_start;
  return !m_cycle_begin_pending;
}

// Cycle overlap, video thread, right after xrWaitFrame: begin the frame now when no frame is left
// to end, else leave the xrBeginFrame to the releaser thread (after that frame's xrEndFrame).
bool VulkanOpenXR::CycleBeginNowAfterWait()
{
  if (!m_pacer.joinable())
    return true;
  std::lock_guard<std::mutex> lock(m_pacer_mutex);
  if (m_pending.empty() && !m_cycle_busy)
    return true;
  m_cycle_begin_pending = true;
  return false;
}
#else
bool VulkanOpenXR::CycleBeforeWait()
{
  return true;
}
bool VulkanOpenXR::CycleBeginNowAfterWait()
{
  return true;
}
#endif

void VulkanOpenXR::FinalizePendingXRFrame(PendingXRFrame frame)
{
  const uint64_t finalize_start_us = Common::Timer::NowUs();
  uint64_t release_total_us = 0;
  uint64_t end_frame_us = 0;
  bool success = true;
  XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};

  const auto release_swapchain = [&](XrSwapchain swapchain, std::string_view name) {
    if (swapchain == XR_NULL_HANDLE)
      return;

    const uint64_t release_start_us = Common::Timer::NowUs();
    XrResult result = XR_SUCCESS;
    {
      auto queue_lock = AcquireGraphicsQueueLock();
      result = xrReleaseSwapchainImage(swapchain, &release_info);
    }
    const uint64_t release_us = ElapsedUs(release_start_us, Common::Timer::NowUs());
    release_total_us += release_us;
    if (XR_FAILED(result))
    {
      WARN_LOG_FMT(VIDEO, "OpenXR: async xrReleaseSwapchainImage failed for {} ({}).", name,
                   static_cast<int>(result));
      success = false;
    }
  };

  if (frame.layered_acquired)
    release_swapchain(frame.layered_swapchain, "layered swapchain");

  for (uint32_t eye = 0; eye < 2; ++eye)
  {
    if (frame.eye_acquired[eye])
      release_swapchain(frame.eye_swapchains[eye], eye == 0 ? "eye 0" : "eye 1");
  }

  XrCompositionLayerProjection projection_layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  projection_layer.layerFlags = frame.layer_flags;
  projection_layer.space = frame.space;
  projection_layer.viewCount = static_cast<uint32_t>(frame.projection_views.size());
  projection_layer.views = frame.projection_views.data();

  const std::vector<XrCompositionLayerBaseHeader*> layers = {
      reinterpret_cast<XrCompositionLayerBaseHeader*>(&projection_layer)};

  const uint64_t end_frame_start_us = Common::Timer::NowUs();
  if (!VR::g_openxr ||
      !VR::g_openxr->EndFrameDetached(frame.display_time, frame.environment_blend_mode,
                                      frame.should_render, layers))
  {
    success = false;
  }
  end_frame_us = ElapsedUs(end_frame_start_us, Common::Timer::NowUs());

  if (!success)
    m_async_frame_finalization_failed.store(true, std::memory_order_release);

  const uint64_t finalize_end_us = Common::Timer::NowUs();
  const uint64_t queue_delay_us = ElapsedUs(frame.queued_time_us, finalize_start_us);
  const uint64_t finalize_us = ElapsedUs(finalize_start_us, finalize_end_us);
  if (frame.debug_frame_id <= 20 || frame.debug_frame_id % 300 == 0 || queue_delay_us >= 1000 ||
      finalize_us >= 1000)
  {
    INFO_LOG_FMT(VIDEO,
                 "OpenXR Vulkan: async final XR submit #{} timing queue_delay={}us "
                 "release={}us end_frame={}us finalize={}us success={}.",
                 frame.debug_frame_id, queue_delay_us, release_total_us, end_frame_us, finalize_us,
                 success);
#if defined(ANDROID)
    __android_log_print(ANDROID_LOG_INFO, "DolphinXR",
                        "OpenXR Vulkan: async final XR submit #%llu timing queue_delay=%lluus "
                        "release=%lluus end_frame=%lluus finalize=%lluus success=%d",
                        static_cast<unsigned long long>(frame.debug_frame_id),
                        static_cast<unsigned long long>(queue_delay_us),
                        static_cast<unsigned long long>(release_total_us),
                        static_cast<unsigned long long>(end_frame_us),
                        static_cast<unsigned long long>(finalize_us), static_cast<int>(success));
#endif
  }

  m_async_frame_finalization_in_flight.store(false, std::memory_order_release);
}

// static
bool VulkanOpenXR::PreQueryVulkanExtensions(VulkanExtensionRequirements& out)
{
  INFO_LOG_FMT(VIDEO, "OpenXR Vulkan: Pre-querying required Vulkan extensions...");

  auto mgr = std::make_unique<VR::OpenXRManager>();

  std::vector<const char*> extensions = {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME};
  AppendOptionalOpenXRExtensions(&extensions);
#if defined(ANDROID)
  extensions.push_back(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME);
  AppendOptionalAndroidOpenXRExtensions(&extensions);
#endif
  // Required by the OpenXR spec when XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT is used
  // on Vulkan. The sRGB swapchain path uses a UNORM view alias to avoid double gamma.
  if (VR::OpenXRManager::IsRuntimeExtensionSupported(
          XR_KHR_VULKAN_SWAPCHAIN_FORMAT_LIST_EXTENSION_NAME))
  {
    extensions.push_back(XR_KHR_VULKAN_SWAPCHAIN_FORMAT_LIST_EXTENSION_NAME);
  }
  const auto controller_exts = VR::OpenXRManager::GetAvailableControllerExtensions();
  extensions.insert(extensions.end(), controller_exts.begin(), controller_exts.end());
  if (!mgr->CreateInstance(extensions))
    return false;

  if (!mgr->InitializeSystem())
    return false;

  if (!mgr->EnumerateViewConfigurations())
    return false;

  const XrInstance xr_instance = mgr->GetInstance();
  const XrSystemId xr_system = mgr->GetSystemId();

  PFN_xrGetVulkanGraphicsRequirementsKHR pfnGetVulkanRequirements = nullptr;
  xrGetInstanceProcAddr(xr_instance, "xrGetVulkanGraphicsRequirementsKHR",
                        reinterpret_cast<PFN_xrVoidFunction*>(&pfnGetVulkanRequirements));
  if (pfnGetVulkanRequirements)
  {
    XrGraphicsRequirementsVulkanKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    const XrResult result = pfnGetVulkanRequirements(xr_instance, xr_system, &requirements);
    if (XR_SUCCEEDED(result))
    {
      const u32 reported_max =
          VK_MAKE_API_VERSION(0, XR_VERSION_MAJOR(requirements.maxApiVersionSupported),
                              XR_VERSION_MINOR(requirements.maxApiVersionSupported),
                              XR_VERSION_PATCH(requirements.maxApiVersionSupported));
      INFO_LOG_FMT(VIDEO, "OpenXR: Pre-query Vulkan API range min {}.{}.{}, max {}.{}.{}",
                   XR_VERSION_MAJOR(requirements.minApiVersionSupported),
                   XR_VERSION_MINOR(requirements.minApiVersionSupported),
                   XR_VERSION_PATCH(requirements.minApiVersionSupported),
                   XR_VERSION_MAJOR(requirements.maxApiVersionSupported),
                   XR_VERSION_MINOR(requirements.maxApiVersionSupported),
                   XR_VERSION_PATCH(requirements.maxApiVersionSupported));

      // XR_KHR_vulkan_enable v1 always reports max=1.0.0 (Meta's Oculus runtime quirk),
      // even though the runtime actually supports newer Vulkan. Clamping the instance to
      // 1.0 there breaks Dolphin's multiview path (a 1.1 feature) and crashes vkCreateDevice.
      // Treat 1.0.0 as "unknown" so Dolphin keeps its negotiated 1.1/1.2 instance.
      if (reported_max <= VK_API_VERSION_1_0)
      {
        WARN_LOG_FMT(VIDEO,
                     "OpenXR: Runtime reported max Vulkan 1.0.0 (v1 extension quirk); "
                     "ignoring and using Dolphin's instance version.");
        out.max_api_version = 0;
      }
      else
      {
        out.max_api_version = reported_max;
      }
    }
  }

  // Query required Vulkan instance extensions.
  PFN_xrGetVulkanInstanceExtensionsKHR pfnGetInstanceExts = nullptr;
  xrGetInstanceProcAddr(xr_instance, "xrGetVulkanInstanceExtensionsKHR",
                        reinterpret_cast<PFN_xrVoidFunction*>(&pfnGetInstanceExts));
  if (pfnGetInstanceExts)
  {
    uint32_t ext_len = 0;
    pfnGetInstanceExts(xr_instance, xr_system, 0, &ext_len, nullptr);
    if (ext_len > 0)
    {
      std::string ext_str(ext_len, '\0');
      pfnGetInstanceExts(xr_instance, xr_system, ext_len, &ext_len, ext_str.data());
      INFO_LOG_FMT(VIDEO, "OpenXR: Required Vulkan instance extensions: {}", ext_str);
      // Parse space-separated extension list.
      std::istringstream iss(ext_str);
      std::string ext;
      while (iss >> ext)
        out.instance_extensions.push_back(ext);
    }
  }

  // Query required Vulkan device extensions.
  PFN_xrGetVulkanDeviceExtensionsKHR pfnGetDeviceExts = nullptr;
  xrGetInstanceProcAddr(xr_instance, "xrGetVulkanDeviceExtensionsKHR",
                        reinterpret_cast<PFN_xrVoidFunction*>(&pfnGetDeviceExts));
  if (pfnGetDeviceExts)
  {
    uint32_t ext_len = 0;
    pfnGetDeviceExts(xr_instance, xr_system, 0, &ext_len, nullptr);
    if (ext_len > 0)
    {
      std::string ext_str(ext_len, '\0');
      pfnGetDeviceExts(xr_instance, xr_system, ext_len, &ext_len, ext_str.data());
      INFO_LOG_FMT(VIDEO, "OpenXR: Required Vulkan device extensions: {}", ext_str);
      std::istringstream iss(ext_str);
      std::string ext;
      while (iss >> ext)
        out.device_extensions.push_back(ext);
    }
  }

  // Keep the OpenXRManager alive — Initialize() will reuse it.
  VR::g_openxr = std::move(mgr);

  INFO_LOG_FMT(VIDEO, "OpenXR Vulkan: Pre-query complete ({} instance, {} device extensions).",
               out.instance_extensions.size(), out.device_extensions.size());
  return true;
}

bool VulkanOpenXR::Initialize()
{
  INFO_LOG_FMT(VIDEO, "OpenXR Vulkan: Starting initialization...");

  // If PreQueryVulkanExtensions() was called, VR::g_openxr already exists.
  if (!VR::g_openxr)
  {
    auto mgr = std::make_unique<VR::OpenXRManager>();

    std::vector<const char*> extensions = {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME};
    AppendOptionalOpenXRExtensions(&extensions);
#if defined(ANDROID)
    extensions.push_back(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME);
    AppendOptionalAndroidOpenXRExtensions(&extensions);
#endif
    if (VR::OpenXRManager::IsRuntimeExtensionSupported(
            XR_KHR_VULKAN_SWAPCHAIN_FORMAT_LIST_EXTENSION_NAME))
    {
      extensions.push_back(XR_KHR_VULKAN_SWAPCHAIN_FORMAT_LIST_EXTENSION_NAME);
    }
    const auto controller_exts = VR::OpenXRManager::GetAvailableControllerExtensions();
    extensions.insert(extensions.end(), controller_exts.begin(), controller_exts.end());
    if (!mgr->CreateInstance(extensions))
      return false;

    if (!mgr->InitializeSystem())
      return false;

    if (!mgr->EnumerateViewConfigurations())
      return false;

    VR::g_openxr = std::move(mgr);
  }

  INFO_LOG_FMT(VIDEO, "OpenXR Vulkan: Creating session...");
  if (!CreateSessionVulkan())
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR Vulkan: Session creation failed — disabling VR.");
    VR::g_openxr.reset();
    return false;
  }

  INFO_LOG_FMT(VIDEO, "OpenXR Vulkan: Creating reference space...");
  if (!VR::g_openxr->CreateReferenceSpace())
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR Vulkan: Reference space creation failed — disabling VR.");
    VR::g_openxr.reset();
    return false;
  }

#if !defined(ANDROID)
  // PrimeFrame pacing (frame/PACING.md). PRIMEDGUN_XR_WAIT_GPU=1 keeps meaning the drain.
  m_pacing = Pacing::Cycle;
  if (EnvIs("PRIMEFRAME_PACING", "pipelined"))
    m_pacing = Pacing::Pipelined;
  else if (EnvIs("PRIMEFRAME_PACING", "inline"))
    m_pacing = Pacing::Inline;
  else if (EnvIs("PRIMEFRAME_PACING", "drain") ||
           (!std::getenv("PRIMEFRAME_PACING") && EnvIs("PRIMEDGUN_XR_WAIT_GPU", "1")))
    m_pacing = Pacing::Drain;
  m_no_gpu_wait = EnvIs("PRIMEFRAME_NO_GPU_WAIT", "1");
  m_skip_window_present = !EnvIs("PRIMEFRAME_WINDOW_PRESENT", "1");
  m_repeat_layers = EnvIs("PRIMEFRAME_REPEATS", "1");
  m_lazy_start = !EnvIs("PRIMEFRAME_CYCLE_LAZY", "0");
  m_cycle_overlap = !EnvIs("PRIMEFRAME_CYCLE_OVERLAP", "0");
  m_end_cfg = EnvIs("PRIMEFRAME_CYCLE_END", "asap") ? 0 : EnvIs("PRIMEFRAME_CYCLE_END", "late") ? 1 : 2;
  m_end_mode = m_end_cfg == 1 ? 1 : 0;
  if (const char* ontime = std::getenv("PRIMEFRAME_PREDICT_ONTIME_MS"))
    m_end_ontime_ms = std::atof(ontime);
  if (const char* earliest = std::getenv("PRIMEFRAME_CYCLE_EARLIEST_MS"))
    m_end_earliest_ms = std::atof(earliest);
  m_early_release = !EnvIs("PRIMEFRAME_EARLY_RELEASE", "0");
  if (const char* margin = std::getenv("PRIMEFRAME_PACE_MARGIN_MS"))
    m_pace_margin_ms = std::clamp(std::atof(margin), 0.0, 20.0);
  m_set_count = m_pacing == Pacing::Pipelined ? 2 : 1;
  // The pipelined loop picks the refresh its poses are shown on itself.
  VR::g_openxr->SetPredictExtraAllowed(m_pacing != Pacing::Pipelined);
  INFO_LOG_FMT(OPENXR,
               "PrimeFrame pacing: mode={} eye_sets={} no_gpu_wait={} window_present={} "
               "repeats={} cycle_lazy={} cycle_overlap={} cycle_end={} early_release={} "
               "margin_ms={:.1f}",
               m_pacing == Pacing::Pipelined ? "pipelined" :
               m_pacing == Pacing::Inline    ? "inline" :
               m_pacing == Pacing::Cycle     ? "cycle" :
                                               "drain",
               m_set_count, m_no_gpu_wait, !m_skip_window_present, m_repeat_layers, m_lazy_start,
               m_cycle_overlap,
               m_end_cfg == 0 ? "asap" : m_end_cfg == 1 ? "late" : "auto", m_early_release,
               m_pace_margin_ms);
#endif

  INFO_LOG_FMT(VIDEO, "OpenXR Vulkan: Creating swapchains...");
  if (!CreateSwapchains())
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR Vulkan: Swapchain creation failed — disabling VR.");
    VR::g_openxr.reset();
    return false;
  }

  // Register this object as the swapchain provider so Presenter can acquire eye images.
  VR::g_openxr->SetSwapchain(this);
#if !defined(ANDROID)
  if (m_pacing == Pacing::Pipelined || m_pacing == Pacing::Cycle)
    StartPacer();
#endif

  INFO_LOG_FMT(VIDEO, "OpenXR Vulkan: Initialization complete.");
  return true;
}

void VulkanOpenXR::Shutdown()
{
  WaitForPendingFrameFinalization("during shutdown");
#if !defined(ANDROID)
  StopPacer();
#endif

  // Clear swapchain pointer before destroying swapchains so no dangling use occurs.
  if (VR::g_openxr)
    VR::g_openxr->SetSwapchain(nullptr);

  DestroySwapchains();
  VR::g_openxr.reset();

  // Wait for all GPU work to finish before the Vulkan device is destroyed.
  if (g_vulkan_context)
  {
    vkDeviceWaitIdle(g_vulkan_context->GetDevice());
    for (VkFence fence : m_all_fences)
      vkDestroyFence(g_vulkan_context->GetDevice(), fence, nullptr);
  }
  m_all_fences.clear();
  m_free_fences.clear();

  INFO_LOG_FMT(VIDEO, "OpenXR Vulkan: Shut down.");
}

bool VulkanOpenXR::CreateSessionVulkan()
{
  ASSERT(g_vulkan_context != nullptr);
  ASSERT(VR::g_openxr != nullptr);

  const XrInstance xr_instance = VR::g_openxr->GetInstance();
  const XrSystemId xr_system = VR::g_openxr->GetSystemId();

  // --- Query Vulkan graphics requirements (mandatory before session creation) ---
  INFO_LOG_FMT(VIDEO, "OpenXR Vulkan: Querying graphics requirements...");
  PFN_xrGetVulkanGraphicsRequirementsKHR pfnGetVulkanRequirements = nullptr;
  XrResult result = xrGetInstanceProcAddr(
      xr_instance, "xrGetVulkanGraphicsRequirementsKHR",
      reinterpret_cast<PFN_xrVoidFunction*>(&pfnGetVulkanRequirements));

  if (XR_FAILED(result) || pfnGetVulkanRequirements == nullptr)
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR: Could not load xrGetVulkanGraphicsRequirementsKHR.");
    return false;
  }

  XrGraphicsRequirementsVulkanKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
  result = pfnGetVulkanRequirements(xr_instance, xr_system, &requirements);
  if (XR_FAILED(result))
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR: xrGetVulkanGraphicsRequirementsKHR failed ({}).",
                  static_cast<int>(result));
    return false;
  }

  INFO_LOG_FMT(VIDEO, "OpenXR: Vulkan requirements — min API {}.{}.{}, max API {}.{}.{}",
               XR_VERSION_MAJOR(requirements.minApiVersionSupported),
               XR_VERSION_MINOR(requirements.minApiVersionSupported),
               XR_VERSION_PATCH(requirements.minApiVersionSupported),
               XR_VERSION_MAJOR(requirements.maxApiVersionSupported),
               XR_VERSION_MINOR(requirements.maxApiVersionSupported),
               XR_VERSION_PATCH(requirements.maxApiVersionSupported));

  // Log Dolphin's Vulkan API version for comparison.
  const u32 dolphin_api = g_vulkan_context->GetDeviceInfo().apiVersion;
  INFO_LOG_FMT(VIDEO, "OpenXR: Dolphin VkPhysicalDevice API version {}.{}.{}",
               VK_VERSION_MAJOR(dolphin_api), VK_VERSION_MINOR(dolphin_api),
               VK_VERSION_PATCH(dolphin_api));

  // --- Query required Vulkan instance extensions ---
  INFO_LOG_FMT(VIDEO, "OpenXR Vulkan: Querying required Vulkan instance extensions...");
  PFN_xrGetVulkanInstanceExtensionsKHR pfnGetInstanceExts = nullptr;
  result = xrGetInstanceProcAddr(xr_instance, "xrGetVulkanInstanceExtensionsKHR",
                                 reinterpret_cast<PFN_xrVoidFunction*>(&pfnGetInstanceExts));
  if (XR_SUCCEEDED(result) && pfnGetInstanceExts != nullptr)
  {
    uint32_t ext_len = 0;
    pfnGetInstanceExts(xr_instance, xr_system, 0, &ext_len, nullptr);
    if (ext_len > 0)
    {
      std::string ext_str(ext_len, '\0');
      pfnGetInstanceExts(xr_instance, xr_system, ext_len, &ext_len, ext_str.data());
      INFO_LOG_FMT(VIDEO, "OpenXR: Required Vulkan instance extensions: {}", ext_str);
    }
    else
    {
      INFO_LOG_FMT(VIDEO, "OpenXR: No additional Vulkan instance extensions required.");
    }
  }

  // --- Query required Vulkan device extensions ---
  INFO_LOG_FMT(VIDEO, "OpenXR Vulkan: Querying required Vulkan device extensions...");
  PFN_xrGetVulkanDeviceExtensionsKHR pfnGetDeviceExts = nullptr;
  result = xrGetInstanceProcAddr(xr_instance, "xrGetVulkanDeviceExtensionsKHR",
                                 reinterpret_cast<PFN_xrVoidFunction*>(&pfnGetDeviceExts));
  if (XR_SUCCEEDED(result) && pfnGetDeviceExts != nullptr)
  {
    uint32_t ext_len = 0;
    pfnGetDeviceExts(xr_instance, xr_system, 0, &ext_len, nullptr);
    if (ext_len > 0)
    {
      std::string ext_str(ext_len, '\0');
      pfnGetDeviceExts(xr_instance, xr_system, ext_len, &ext_len, ext_str.data());
      INFO_LOG_FMT(VIDEO, "OpenXR: Required Vulkan device extensions: {}", ext_str);
    }
    else
    {
      INFO_LOG_FMT(VIDEO, "OpenXR: No additional Vulkan device extensions required.");
    }
  }

  // --- Verify the physical device matches what the runtime expects ---
  INFO_LOG_FMT(VIDEO, "OpenXR Vulkan: Checking physical device...");
  PFN_xrGetVulkanGraphicsDeviceKHR pfnGetVulkanDevice = nullptr;
  result = xrGetInstanceProcAddr(xr_instance, "xrGetVulkanGraphicsDeviceKHR",
                                 reinterpret_cast<PFN_xrVoidFunction*>(&pfnGetVulkanDevice));
  if (XR_SUCCEEDED(result) && pfnGetVulkanDevice != nullptr)
  {
    VkPhysicalDevice xr_physical_device = VK_NULL_HANDLE;
    result = pfnGetVulkanDevice(xr_instance, xr_system,
                                g_vulkan_context->GetVulkanInstance(), &xr_physical_device);
    if (XR_SUCCEEDED(result))
    {
      if (xr_physical_device != g_vulkan_context->GetPhysicalDevice())
      {
        WARN_LOG_FMT(VIDEO,
                     "OpenXR: Runtime wants a different VkPhysicalDevice than Dolphin selected. "
                     "VR may not work correctly.");
      }
      else
      {
        INFO_LOG_FMT(VIDEO, "OpenXR: VkPhysicalDevice matches runtime expectation.");
      }
    }
  }

  // --- Create XrSession bound to the active Vulkan device ---
  INFO_LOG_FMT(VIDEO, "OpenXR Vulkan: Creating XrSession with Vulkan binding...");
  INFO_LOG_FMT(VIDEO, "  VkInstance={}, VkPhysicalDevice={}, VkDevice={}, queueFamily={}, "
                       "queueIndex=0",
               reinterpret_cast<void*>(g_vulkan_context->GetVulkanInstance()),
               reinterpret_cast<void*>(g_vulkan_context->GetPhysicalDevice()),
               reinterpret_cast<void*>(g_vulkan_context->GetDevice()),
               g_vulkan_context->GetGraphicsQueueFamilyIndex());

  XrGraphicsBindingVulkanKHR vk_binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
  vk_binding.instance = g_vulkan_context->GetVulkanInstance();
  vk_binding.physicalDevice = g_vulkan_context->GetPhysicalDevice();
  vk_binding.device = g_vulkan_context->GetDevice();
  vk_binding.queueFamilyIndex = g_vulkan_context->GetGraphicsQueueFamilyIndex();
  vk_binding.queueIndex = 0;

  XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
  session_info.next = &vk_binding;
  session_info.systemId = xr_system;

  XrSession session = XR_NULL_HANDLE;
  result = SafeCreateSession(xr_instance, &session_info, &session);

  if (XR_FAILED(result))
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR: xrCreateSession failed ({}).", static_cast<int>(result));
    return false;
  }

  VR::g_openxr->SetSession(session);
  INFO_LOG_FMT(VIDEO, "OpenXR Vulkan: Session created successfully.");
  return true;
}

bool VulkanOpenXR::CreateSwapchains()
{
  ASSERT(VR::g_openxr != nullptr);

  int64_t swapchain_format = 0;
  if (!SelectSwapchainFormat(VR::g_openxr->GetSession(), &swapchain_format))
    return false;

  m_use_layered_swapchain = false;
  m_frame_uses_layered_swapchain = false;

#if defined(ANDROID)
  const auto& view_cfgs = VR::g_openxr->GetViewConfigViews();
  const bool matching_eye_sizes =
      view_cfgs[0].recommendedImageRectWidth == view_cfgs[1].recommendedImageRectWidth &&
      view_cfgs[0].recommendedImageRectHeight == view_cfgs[1].recommendedImageRectHeight;
  if (g_vulkan_context->SupportsMultiview() && g_ActiveConfig.vr_use_vulkan_multiview &&
      g_ActiveConfig.stereo_mode == StereoMode::OpenXR)
  {
    if (matching_eye_sizes)
    {
      if (CreateLayeredSwapchain(swapchain_format))
      {
        m_use_layered_swapchain = true;
      }
      else
      {
        WARN_LOG_FMT(VIDEO,
                     "OpenXR: Layered Vulkan swapchain creation failed; falling back to "
                     "two per-eye swapchains.");
      }
    }
    else
    {
      WARN_LOG_FMT(VIDEO,
                   "OpenXR: Layered Vulkan swapchain disabled because eye sizes differ "
                   "({}x{} vs {}x{}).",
                   view_cfgs[0].recommendedImageRectWidth,
                   view_cfgs[0].recommendedImageRectHeight,
                   view_cfgs[1].recommendedImageRectWidth,
                   view_cfgs[1].recommendedImageRectHeight);
    }
  }
#endif

  if (!CreateEyeSwapchains(swapchain_format))
  {
    DestroySwapchains();
    return false;
  }

  return true;
}

bool VulkanOpenXR::CreateLayeredSwapchain(int64_t swapchain_format)
{
  ASSERT(VR::g_openxr != nullptr);

  const auto& view_cfgs = VR::g_openxr->GetViewConfigViews();
  const AbstractTextureFormat abstract_format =
      VkFormatToAbstractFormat(static_cast<VkFormat>(swapchain_format));

  auto& sc = m_layered_swapchain;
  sc.width = view_cfgs[0].recommendedImageRectWidth;
  sc.height = view_cfgs[0].recommendedImageRectHeight;

  auto cleanup = [&sc]() {
    sc.framebuffers.clear();
    sc.textures.clear();
    if (sc.swapchain != XR_NULL_HANDLE)
    {
      xrDestroySwapchain(sc.swapchain);
      sc.swapchain = XR_NULL_HANDLE;
    }
    sc.width = 0;
    sc.height = 0;
  };

  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  info.arraySize = 2;
  info.format = swapchain_format;
  info.width = sc.width;
  info.height = sc.height;
  info.mipCount = 1;
  info.faceCount = 1;
  info.sampleCount = 1;
  info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;

  const VkFormat vk_sc_format = static_cast<VkFormat>(swapchain_format);
  const VkFormat vk_view_format = VKTexture::GetLinearFormat(vk_sc_format);
  const std::array<VkFormat, 2> view_formats = {vk_sc_format, vk_view_format};
  XrVulkanSwapchainFormatListCreateInfoKHR format_list{
      XR_TYPE_VULKAN_SWAPCHAIN_FORMAT_LIST_CREATE_INFO_KHR};
  if (vk_view_format != vk_sc_format)
  {
    info.usageFlags |= XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;

    if (VR::g_openxr->IsExtensionEnabled(XR_KHR_VULKAN_SWAPCHAIN_FORMAT_LIST_EXTENSION_NAME))
    {
      format_list.viewFormatCount = static_cast<uint32_t>(view_formats.size());
      format_list.viewFormats = view_formats.data();
      info.next = &format_list;
    }
  }

  XrResult result = xrCreateSwapchain(VR::g_openxr->GetSession(), &info, &sc.swapchain);
  if (XR_FAILED(result))
  {
    WARN_LOG_FMT(VIDEO, "OpenXR: xrCreateSwapchain failed for layered Vulkan swapchain ({}).",
                 static_cast<int>(result));
    cleanup();
    return false;
  }

  uint32_t image_count = 0;
  result = xrEnumerateSwapchainImages(sc.swapchain, 0, &image_count, nullptr);
  if (XR_FAILED(result) || image_count == 0)
  {
    WARN_LOG_FMT(VIDEO,
                 "OpenXR: xrEnumerateSwapchainImages failed for layered Vulkan swapchain ({}).",
                 static_cast<int>(result));
    cleanup();
    return false;
  }

  std::vector<XrSwapchainImageVulkanKHR> images(image_count,
                                                 {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
  result = xrEnumerateSwapchainImages(
      sc.swapchain, image_count, &image_count,
      reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
  if (XR_FAILED(result))
  {
    WARN_LOG_FMT(VIDEO,
                 "OpenXR: xrEnumerateSwapchainImages data failed for layered Vulkan swapchain "
                 "({}).",
                 static_cast<int>(result));
    cleanup();
    return false;
  }

  sc.textures.resize(image_count);
  sc.framebuffers.resize(image_count);

  for (uint32_t i = 0; i < image_count; ++i)
  {
    TextureConfig tex_config(sc.width, sc.height, /*levels=*/1, /*layers=*/2, /*samples=*/1,
                             abstract_format, AbstractTextureFlag_RenderTarget,
                             AbstractTextureType::Texture_2DArray);

    sc.textures[i] = VKTexture::CreateAdopted(tex_config, images[i].image,
                                              VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                                              VK_IMAGE_LAYOUT_UNDEFINED, vk_view_format);
    if (!sc.textures[i])
    {
      WARN_LOG_FMT(VIDEO,
                   "OpenXR: VKTexture::CreateAdopted failed for layered Vulkan image {}.", i);
      cleanup();
      return false;
    }

    sc.framebuffers[i] = VKFramebuffer::CreateMultiview(sc.textures[i].get(), nullptr, {});
    if (!sc.framebuffers[i])
    {
      WARN_LOG_FMT(VIDEO,
                   "OpenXR: VKFramebuffer::CreateMultiview failed for layered Vulkan image {}.",
                   i);
      cleanup();
      return false;
    }
  }

  INFO_LOG_FMT(VIDEO, "OpenXR: Layered Vulkan swapchain ready: {}x{}, {} images, arraySize=2.",
               sc.width, sc.height, image_count);
  return true;
}

bool VulkanOpenXR::CreateEyeSwapchains(int64_t swapchain_format)
{
  ASSERT(VR::g_openxr != nullptr);

  const auto& view_cfgs = VR::g_openxr->GetViewConfigViews();
  const AbstractTextureFormat abstract_format =
      VkFormatToAbstractFormat(static_cast<VkFormat>(swapchain_format));

  for (uint32_t set_eye = 0; set_eye < 2 * m_set_count; ++set_eye)
  {
    const uint32_t eye = set_eye % 2;
    const uint32_t set = set_eye / 2;
    auto& sc = EyeSet(set)[eye];
    sc.width = view_cfgs[eye].recommendedImageRectWidth;
    sc.height = view_cfgs[eye].recommendedImageRectHeight;

    // Format must come from xrEnumerateSwapchainFormats.
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.arraySize = 1;
    info.format = swapchain_format;
    info.width = sc.width;
    info.height = sc.height;
    info.mipCount = 1;
    info.faceCount = 1;
    info.sampleCount = 1;
    info.usageFlags =
        XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;

    const VkFormat vk_sc_format = static_cast<VkFormat>(swapchain_format);
    const VkFormat vk_view_format = VKTexture::GetLinearFormat(vk_sc_format);
    const std::array<VkFormat, 2> view_formats = {vk_sc_format, vk_view_format};
    XrVulkanSwapchainFormatListCreateInfoKHR format_list{
        XR_TYPE_VULKAN_SWAPCHAIN_FORMAT_LIST_CREATE_INFO_KHR};
    if (vk_view_format != vk_sc_format)
    {
      info.usageFlags |= XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;

      if (VR::g_openxr->IsExtensionEnabled(XR_KHR_VULKAN_SWAPCHAIN_FORMAT_LIST_EXTENSION_NAME))
      {
        format_list.viewFormatCount = static_cast<uint32_t>(view_formats.size());
        format_list.viewFormats = view_formats.data();
        info.next = &format_list;
      }
    }

    XrResult result = xrCreateSwapchain(VR::g_openxr->GetSession(), &info, &sc.swapchain);
    if (XR_FAILED(result))
    {
      ERROR_LOG_FMT(VIDEO, "OpenXR: xrCreateSwapchain failed for eye {} ({}).", eye,
                    static_cast<int>(result));
      return false;
    }

    // Enumerate the Vulkan images backing this swapchain.
    uint32_t image_count = 0;
    xrEnumerateSwapchainImages(sc.swapchain, 0, &image_count, nullptr);

    std::vector<XrSwapchainImageVulkanKHR> images(image_count,
                                                   {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    xrEnumerateSwapchainImages(sc.swapchain, image_count, &image_count,
                               reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));

    sc.textures.resize(image_count);
    sc.framebuffers.resize(image_count);

    for (uint32_t i = 0; i < image_count; ++i)
    {
      // Build a TextureConfig matching the swapchain image properties.
      TextureConfig tex_config(sc.width, sc.height, /*levels=*/1, /*layers=*/1, /*samples=*/1,
                               abstract_format, AbstractTextureFlag_RenderTarget,
                               AbstractTextureType::Texture_2D);

      // Adopt the runtime-owned VkImage. For sRGB swapchains, use a UNORM view alias so
      // BlitFromTexture writes raw sRGB-encoded bytes without a second sRGB encode.
      sc.textures[i] =
          VKTexture::CreateAdopted(tex_config, images[i].image, VK_IMAGE_VIEW_TYPE_2D,
                                   VK_IMAGE_LAYOUT_UNDEFINED, vk_view_format);
      if (!sc.textures[i])
      {
        ERROR_LOG_FMT(VIDEO, "OpenXR: VKTexture::CreateAdopted failed for eye {}, image {}.", eye,
                      i);
        return false;
      }

      sc.framebuffers[i] = VKFramebuffer::Create(sc.textures[i].get(), nullptr, {});
      if (!sc.framebuffers[i])
      {
        ERROR_LOG_FMT(VIDEO, "OpenXR: VKFramebuffer::Create failed for eye {}, image {}.", eye, i);
        return false;
      }
    }

    INFO_LOG_FMT(VIDEO, "OpenXR: Eye {} swapchain (set {}) ready: {}x{}, {} images.", eye, set,
                 sc.width, sc.height, image_count);
  }

  return true;
}

void VulkanOpenXR::DestroySwapchains()
{
  // Wait for the GPU to finish all pending work before destroying resources.
  if (g_vulkan_context)
    vkDeviceWaitIdle(g_vulkan_context->GetDevice());

  DestroyPrimedGunOverlaySwapchain(&m_primedgun_overlay_swapchain);
  DestroyPrimedGunOverlaySwapchain(&m_primedgun_position_marker_swapchain);
  DestroyPrimedGunLaserSwapchain();

  if (m_layered_image_acquired && m_layered_swapchain.swapchain != XR_NULL_HANDLE)
  {
    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    XrResult release_result = XR_SUCCESS;
    {
      auto queue_lock = AcquireGraphicsQueueLock();
      release_result = xrReleaseSwapchainImage(m_layered_swapchain.swapchain, &release_info);
    }
    if (XR_FAILED(release_result))
    {
      WARN_LOG_FMT(VIDEO,
                   "OpenXR: xrReleaseSwapchainImage during shutdown failed for layered "
                   "swapchain ({}).",
                   static_cast<int>(release_result));
    }
    m_layered_image_acquired = false;
  }

  m_layered_swapchain.framebuffers.clear();
  m_layered_swapchain.textures.clear();

  if (m_layered_swapchain.swapchain != XR_NULL_HANDLE)
  {
    const XrResult destroy_result = xrDestroySwapchain(m_layered_swapchain.swapchain);
    if (XR_FAILED(destroy_result))
    {
      WARN_LOG_FMT(VIDEO, "OpenXR: xrDestroySwapchain failed for layered swapchain ({}).",
                   static_cast<int>(destroy_result));
    }
    m_layered_swapchain.swapchain = XR_NULL_HANDLE;
  }
  m_layered_swapchain.width = 0;
  m_layered_swapchain.height = 0;
  m_use_layered_swapchain = false;
  m_frame_uses_layered_swapchain = false;

  for (uint32_t set_eye = 0; set_eye < 4; ++set_eye)
  {
    const uint32_t eye = set_eye % 2;
    const uint32_t set = set_eye / 2;
    auto& sc = EyeSet(set)[eye];

    if (set == m_frame_set && m_image_acquired[eye] && sc.swapchain != XR_NULL_HANDLE)
    {
      XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
      XrResult release_result = XR_SUCCESS;
      {
        auto queue_lock = AcquireGraphicsQueueLock();
        release_result = xrReleaseSwapchainImage(sc.swapchain, &release_info);
      }
      if (XR_FAILED(release_result))
      {
        WARN_LOG_FMT(VIDEO,
                     "OpenXR: xrReleaseSwapchainImage during shutdown failed for eye {} ({}).",
                     eye, static_cast<int>(release_result));
      }
      m_image_acquired[eye] = false;
    }

    // Release Dolphin wrappers before destroying the swapchain so the
    // runtime's VkImages are only freed after our views are gone.
    sc.framebuffers.clear();
    sc.textures.clear();

    if (sc.swapchain != XR_NULL_HANDLE)
    {
      const XrResult destroy_result = xrDestroySwapchain(sc.swapchain);
      if (XR_FAILED(destroy_result))
      {
        WARN_LOG_FMT(VIDEO, "OpenXR: xrDestroySwapchain failed for eye {} ({}).", eye,
                     static_cast<int>(destroy_result));
      }
      sc.swapchain = XR_NULL_HANDLE;
    }
  }
}

void VulkanOpenXR::DestroyPrimedGunOverlaySwapchain(XRPrimedGunVkOverlaySwapchain* overlay)
{
  if (!overlay)
    return;

  overlay->textures.clear();
  overlay->images.clear();
  overlay->width = 0;
  overlay->height = 0;
  overlay->content_kind = 0;
  overlay->generation = 0;
  overlay->texture_ready = false;

  if (overlay->swapchain != XR_NULL_HANDLE)
  {
    const XrResult result = xrDestroySwapchain(overlay->swapchain);
    if (XR_FAILED(result))
      WARN_LOG_FMT(VIDEO, "OpenXR: PrimedGun Vulkan overlay xrDestroySwapchain failed ({}).",
                   static_cast<int>(result));
    overlay->swapchain = XR_NULL_HANDLE;
  }
}

void VulkanOpenXR::DestroyPrimedGunLaserSwapchain()
{
  auto& laser = m_primedgun_laser_swapchain;
  laser.textures.clear();
  laser.images.clear();
  laser.texture_ready = false;

  if (laser.swapchain != XR_NULL_HANDLE)
  {
    const XrResult result = xrDestroySwapchain(laser.swapchain);
    if (XR_FAILED(result))
      WARN_LOG_FMT(VIDEO, "OpenXR: PrimedGun Vulkan laser xrDestroySwapchain failed ({}).",
                   static_cast<int>(result));
    laser.swapchain = XR_NULL_HANDLE;
  }
}

bool VulkanOpenXR::EnsurePrimedGunLaserSwapchain()
{
  auto& laser = m_primedgun_laser_swapchain;
  if (laser.texture_ready && laser.swapchain != XR_NULL_HANDLE)
    return true;

  DestroyPrimedGunLaserSwapchain();

  int64_t swapchain_format = 0;
  if (!SelectPrimedGunOverlaySwapchainFormat(VR::g_openxr->GetSession(), &swapchain_format))
    return false;

  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  info.arraySize = 1;
  info.format = swapchain_format;
  info.width = 32;
  info.height = 30;
  info.mipCount = 1;
  info.faceCount = 1;
  info.sampleCount = 1;
  info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;

  XrResult result = xrCreateSwapchain(VR::g_openxr->GetSession(), &info, &laser.swapchain);
  if (XR_FAILED(result) || laser.swapchain == XR_NULL_HANDLE)
    return false;

  uint32_t image_count = 0;
  result = xrEnumerateSwapchainImages(laser.swapchain, 0, &image_count, nullptr);
  if (XR_FAILED(result) || image_count == 0)
  {
    DestroyPrimedGunLaserSwapchain();
    return false;
  }

  laser.images.assign(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
  result = xrEnumerateSwapchainImages(
      laser.swapchain, image_count, &image_count,
      reinterpret_cast<XrSwapchainImageBaseHeader*>(laser.images.data()));
  if (XR_FAILED(result))
  {
    DestroyPrimedGunLaserSwapchain();
    return false;
  }

  const VkFormat vk_format = static_cast<VkFormat>(swapchain_format);
  const AbstractTextureFormat abstract_format = VkFormatToAbstractFormat(vk_format);
  TextureConfig tex_config(32, 30, 1, 1, 1, abstract_format, 0, AbstractTextureType::Texture_2D);
  laser.textures.resize(image_count);
  for (uint32_t i = 0; i < image_count; ++i)
  {
    laser.textures[i] = VKTexture::CreateAdopted(tex_config, laser.images[i].image,
                                                 VK_IMAGE_VIEW_TYPE_2D,
                                                 VK_IMAGE_LAYOUT_UNDEFINED, vk_format);
    if (!laser.textures[i])
    {
      DestroyPrimedGunLaserSwapchain();
      return false;
    }
  }

  std::array<uint32_t, 960> pixels{};
  for (int y = 0; y < 30; ++y)
    pixels[static_cast<size_t>(y * 32)] = 0xE080D8FFu;
  for (int y = 3; y < 27; ++y)
  {
    for (int x = 4; x < 28; ++x)
    {
      const float dx = static_cast<float>(x) - 15.5f;
      const float dy = static_cast<float>(y) - 14.5f;
      if (dx * dx + dy * dy <= 144.0f)
        pixels[static_cast<size_t>(y * 32 + x)] = 0xE080D8FFu;
    }
  }
  const std::vector<uint32_t> upload_pixels =
      ConvertPrimedGunOverlayPixelsForVkFormat(pixels.data(), pixels.size(), vk_format);

  const uint64_t perf_upload_start_us = Common::Timer::NowUs();
  for (uint32_t i = 0; i < image_count; ++i)
  {
    uint32_t acquired = 0;
    XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    {
      auto queue_lock = AcquireGraphicsQueueLock();
      result = xrAcquireSwapchainImage(laser.swapchain, &acquire_info, &acquired);
    }
    if (XR_FAILED(result))
    {
      DestroyPrimedGunLaserSwapchain();
      return false;
    }

    XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait_info.timeout = XR_INFINITE_DURATION;
    result = xrWaitSwapchainImage(laser.swapchain, &wait_info);
    if (XR_SUCCEEDED(result) && acquired < laser.textures.size())
    {
      laser.textures[acquired]->OverrideImageLayout(VK_IMAGE_LAYOUT_UNDEFINED);
      laser.textures[acquired]->Load(0, 32, 30, 32,
                                     reinterpret_cast<const u8*>(upload_pixels.data()),
                                     upload_pixels.size() * sizeof(uint32_t), 0);
      g_command_buffer_mgr->SubmitCommandBuffer(false, true);
    }

    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    XrResult release_result = XR_SUCCESS;
    {
      auto queue_lock = AcquireGraphicsQueueLock();
      release_result = xrReleaseSwapchainImage(laser.swapchain, &release_info);
    }
    if (XR_FAILED(result) || XR_FAILED(release_result))
    {
      DestroyPrimedGunLaserSwapchain();
      return false;
    }
  }
  g_vulkan_context->GetPerfCounters().overlay_upload_us.fetch_add(
      Common::Timer::NowUs() - perf_upload_start_us, std::memory_order_relaxed);

  laser.texture_ready = true;
  return true;
}

bool VulkanOpenXR::EnsurePrimedGunOverlaySwapchain(XRPrimedGunVkOverlaySwapchain* overlay,
                                                  uint32_t content_kind, uint32_t generation,
                                                  uint32_t width, uint32_t height,
                                                  const std::vector<uint32_t>& pixels)
{
  if (!overlay)
    return false;

  if (overlay->texture_ready && overlay->swapchain != XR_NULL_HANDLE &&
      overlay->content_kind == content_kind && overlay->generation == generation &&
      overlay->width == width && overlay->height == height)
  {
    return true;
  }

  DestroyPrimedGunOverlaySwapchain(overlay);
  overlay->width = width;
  overlay->height = height;

  int64_t swapchain_format = 0;
  if (!SelectPrimedGunOverlaySwapchainFormat(VR::g_openxr->GetSession(), &swapchain_format))
    return false;

  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  info.arraySize = 1;
  info.format = swapchain_format;
  info.width = width;
  info.height = height;
  info.mipCount = 1;
  info.faceCount = 1;
  info.sampleCount = 1;
  info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;

  XrResult result = xrCreateSwapchain(VR::g_openxr->GetSession(), &info, &overlay->swapchain);
  if (XR_FAILED(result) || overlay->swapchain == XR_NULL_HANDLE)
  {
    WARN_LOG_FMT(VIDEO, "OpenXR: PrimedGun Vulkan overlay xrCreateSwapchain failed ({}).",
                 static_cast<int>(result));
    return false;
  }

  uint32_t image_count = 0;
  result = xrEnumerateSwapchainImages(overlay->swapchain, 0, &image_count, nullptr);
  if (XR_FAILED(result) || image_count == 0)
  {
    DestroyPrimedGunOverlaySwapchain(overlay);
    return false;
  }

  overlay->images.assign(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
  result = xrEnumerateSwapchainImages(
      overlay->swapchain, image_count, &image_count,
      reinterpret_cast<XrSwapchainImageBaseHeader*>(overlay->images.data()));
  if (XR_FAILED(result))
  {
    DestroyPrimedGunOverlaySwapchain(overlay);
    return false;
  }

  const VkFormat vk_format = static_cast<VkFormat>(swapchain_format);
  const AbstractTextureFormat abstract_format = VkFormatToAbstractFormat(vk_format);
  TextureConfig tex_config(width, height, 1, 1, 1, abstract_format, 0,
                           AbstractTextureType::Texture_2D);
  overlay->textures.resize(image_count);
  for (uint32_t i = 0; i < image_count; ++i)
  {
    overlay->textures[i] = VKTexture::CreateAdopted(tex_config, overlay->images[i].image,
                                                    VK_IMAGE_VIEW_TYPE_2D,
                                                    VK_IMAGE_LAYOUT_UNDEFINED, vk_format);
    if (!overlay->textures[i])
    {
      DestroyPrimedGunOverlaySwapchain(overlay);
      return false;
    }
  }

  const std::vector<uint32_t> upload_pixels =
      ConvertPrimedGunOverlayPixelsForVkFormat(pixels.data(), pixels.size(), vk_format);

  const uint64_t perf_upload_start_us = Common::Timer::NowUs();
  for (uint32_t i = 0; i < image_count; ++i)
  {
    uint32_t acquired = 0;
    XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    {
      auto queue_lock = AcquireGraphicsQueueLock();
      result = xrAcquireSwapchainImage(overlay->swapchain, &acquire_info, &acquired);
    }
    if (XR_FAILED(result))
    {
      DestroyPrimedGunOverlaySwapchain(overlay);
      return false;
    }

    XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait_info.timeout = XR_INFINITE_DURATION;
    result = xrWaitSwapchainImage(overlay->swapchain, &wait_info);
    if (XR_SUCCEEDED(result) && acquired < overlay->textures.size())
    {
      overlay->textures[acquired]->OverrideImageLayout(VK_IMAGE_LAYOUT_UNDEFINED);
      overlay->textures[acquired]->Load(0, width, height, width,
                                        reinterpret_cast<const u8*>(upload_pixels.data()),
                                        upload_pixels.size() * sizeof(uint32_t), 0);
      g_command_buffer_mgr->SubmitCommandBuffer(false, true);
    }

    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    XrResult release_result = XR_SUCCESS;
    {
      auto queue_lock = AcquireGraphicsQueueLock();
      release_result = xrReleaseSwapchainImage(overlay->swapchain, &release_info);
    }
    if (XR_FAILED(result) || XR_FAILED(release_result))
    {
      DestroyPrimedGunOverlaySwapchain(overlay);
      return false;
    }
  }
  g_vulkan_context->GetPerfCounters().overlay_upload_us.fetch_add(
      Common::Timer::NowUs() - perf_upload_start_us, std::memory_order_relaxed);

  overlay->content_kind = content_kind;
  overlay->generation = generation;
  overlay->texture_ready = true;
  return true;
}

bool VulkanOpenXR::AppendPrimedGunOverlayLayers(std::vector<XrCompositionLayerBaseHeader*>* layers)
{
  if (!VR::g_openxr || !layers)
    return false;

  namespace PGO = PrimedGun::Overlay;
  const auto overlay = Common::VR::OpenXRInputState::GetPrimedGunOverlay();
  // Before the early return below, so the anchor is always released when the compile ends.
  XrPosef compile_anchor{};
  const bool compile_anchored = PGO::CompileProgressPanelPose(overlay, &compile_anchor);
  if (!overlay.menu_visible && !PGO::PromptVisible(overlay) && !overlay.weapon_panel_visible &&
      !overlay.position_marker_visible)
    return false;

  const Common::VR::OpenXRInputSnapshot snapshot = Common::VR::OpenXRInputState::GetSnapshot();
  if (!snapshot.runtime_active)
    return false;

  bool appended_layer = false;
  if (overlay.position_marker_visible)
  {
    constexpr uint32_t marker_width = 512;
    constexpr uint32_t marker_height = 512;
    const std::vector<uint32_t> marker_pixels =
        PGO::BuildPositionMarkerPixels(marker_width, marker_height);
    if (EnsurePrimedGunOverlaySwapchain(&m_primedgun_position_marker_swapchain, 4u, 1u,
                                        marker_width, marker_height, marker_pixels))
    {
      m_primedgun_position_marker_layer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
      m_primedgun_position_marker_layer.layerFlags =
          XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
          XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
      m_primedgun_position_marker_layer.space = VR::g_openxr->GetReferenceSpace();
      m_primedgun_position_marker_layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
      m_primedgun_position_marker_layer.subImage.swapchain =
          m_primedgun_position_marker_swapchain.swapchain;
      m_primedgun_position_marker_layer.subImage.imageRect.offset = {0, 0};
      m_primedgun_position_marker_layer.subImage.imageRect.extent = {
          static_cast<int32_t>(marker_width), static_cast<int32_t>(marker_height)};
      m_primedgun_position_marker_layer.pose.orientation = {-0.70710678f, 0.0f, 0.0f,
                                                           0.70710678f};
      m_primedgun_position_marker_layer.pose.position = {snapshot.tracking_origin_position[0],
                                                        0.005f,
                                                        snapshot.tracking_origin_position[2]};
      m_primedgun_position_marker_layer.size = {0.356f, 0.356f};
      layers->push_back(
          reinterpret_cast<XrCompositionLayerBaseHeader*>(&m_primedgun_position_marker_layer));
      appended_layer = true;
    }
  }

  if (!overlay.menu_visible && !PGO::PromptVisible(overlay) && !overlay.weapon_panel_visible)
    return appended_layer;

  const bool menu = overlay.menu_visible;
  const bool weapon_panel = !menu && overlay.weapon_panel_visible;
  const uint32_t content_kind = menu ? 2u : weapon_panel ? 3u : 1u;
  const uint32_t width = menu ? 1024 : weapon_panel ? 512 : 1024;
  const uint32_t height = menu ? 512 : weapon_panel ? 512 : 384;
  const uint32_t generation = menu ? overlay.generation :
                              weapon_panel ? (100u + overlay.weapon_selected_index) :
                                             PGO::PromptGeneration(overlay);
  const std::vector<uint32_t> pixels = menu        ? PGO::BuildMenuPixels(width, height, overlay) :
                                       weapon_panel ? PGO::BuildWeaponPanelPixels(width, height, overlay) :
                                                      PGO::BuildPromptPixels(width, height, overlay);
  if (!EnsurePrimedGunOverlaySwapchain(&m_primedgun_overlay_swapchain, content_kind, generation,
                                       width, height, pixels))
    return appended_layer;

  m_primedgun_overlay_layer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
  m_primedgun_overlay_layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                        XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
  m_primedgun_overlay_layer.space = VR::g_openxr->GetReferenceSpace();
  m_primedgun_overlay_layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
  m_primedgun_overlay_layer.subImage.swapchain = m_primedgun_overlay_swapchain.swapchain;
  m_primedgun_overlay_layer.subImage.imageRect.offset = {0, 0};
  m_primedgun_overlay_layer.subImage.imageRect.extent = {static_cast<int32_t>(width),
                                                        static_cast<int32_t>(height)};

  PGO::HybridControllerPose left_grip_pose = PGO::MakeGripPose(snapshot.controllers[0]);
  PGO::HybridControllerPose right_grip_pose = PGO::MakeGripPose(snapshot.controllers[1]);
  PGO::HybridControllerPose left_aim_pose = PGO::MakeAimPose(snapshot.controllers[0]);
  PGO::HybridControllerPose right_aim_pose = PGO::MakeAimPose(snapshot.controllers[1]);
  PGO::AddTrackingOrigin(&left_grip_pose, snapshot);
  PGO::AddTrackingOrigin(&right_grip_pose, snapshot);
  PGO::AddTrackingOrigin(&left_aim_pose, snapshot);
  PGO::AddTrackingOrigin(&right_aim_pose, snapshot);
  const PGO::HybridControllerPose& panel_pose =
      overlay.use_right_hand ? left_grip_pose : right_grip_pose;
  const PGO::HybridControllerPose& laser_pose =
      overlay.use_right_hand ? right_aim_pose : left_aim_pose;

  if (menu && overlay.vr_menu_floating && overlay.floating_menu_pose_valid)
  {
    m_primedgun_overlay_layer.pose.orientation = {overlay.floating_menu_orientation[0],
                                                  overlay.floating_menu_orientation[1],
                                                  overlay.floating_menu_orientation[2],
                                                  overlay.floating_menu_orientation[3]};
    m_primedgun_overlay_layer.pose.position = {
        overlay.floating_menu_position[0] + snapshot.tracking_origin_position[0],
        overlay.floating_menu_position[1] + snapshot.tracking_origin_position[1],
        overlay.floating_menu_position[2] + snapshot.tracking_origin_position[2]};
    m_primedgun_overlay_layer.size = {overlay.menu_size[0], overlay.menu_size[1]};
  }
  else if (menu && panel_pose.valid)
  {
    const XrQuaternionf q = panel_pose.orientation;
    m_primedgun_overlay_layer.pose.orientation = PGO::MulQuat(
        q, {-0.70710678f, 0.0f, 0.0f, 0.70710678f});
    const XrVector3f offset = PGO::RotateVector(m_primedgun_overlay_layer.pose.orientation,
                                                {0.0f, 0.10f, -0.18f});
    m_primedgun_overlay_layer.pose.position = {panel_pose.position.x + offset.x,
                                              panel_pose.position.y + offset.y,
                                              panel_pose.position.z + offset.z};
    m_primedgun_overlay_layer.size = {1.05f, 0.72f};
  }
  else if (weapon_panel)
  {
    m_primedgun_overlay_layer.pose.orientation = {overlay.weapon_panel_orientation[0],
                                                 overlay.weapon_panel_orientation[1],
                                                 overlay.weapon_panel_orientation[2],
                                                 overlay.weapon_panel_orientation[3]};
    const XrVector3f offset =
        PGO::RotateVector(m_primedgun_overlay_layer.pose.orientation, {0.0f, 0.055f, -0.26f});
    m_primedgun_overlay_layer.pose.position = {
        overlay.weapon_panel_position[0] + snapshot.tracking_origin_position[0] + offset.x,
        overlay.weapon_panel_position[1] + snapshot.tracking_origin_position[1] + offset.y,
        overlay.weapon_panel_position[2] + snapshot.tracking_origin_position[2] + offset.z};
    m_primedgun_overlay_layer.size = {0.42f, 0.42f};
  }
  else if (compile_anchored)
  {
    m_primedgun_overlay_layer.pose = compile_anchor;
    m_primedgun_overlay_layer.size = {PGO::COMPILE_PANEL_WIDTH, PGO::COMPILE_PANEL_HEIGHT};
  }
  else if (snapshot.head_pose.valid)
  {
    const auto& head = snapshot.head_pose;
    m_primedgun_overlay_layer.pose.orientation = {head.orientation[0], head.orientation[1],
                                                 head.orientation[2], head.orientation[3]};
    const XrVector3f offset =
        PGO::RotateVector(m_primedgun_overlay_layer.pose.orientation, {0.0f, 0.0f, -1.35f});
    m_primedgun_overlay_layer.pose.position = {
        head.position[0] + snapshot.tracking_origin_position[0] + offset.x,
        head.position[1] + snapshot.tracking_origin_position[1] + offset.y,
        head.position[2] + snapshot.tracking_origin_position[2] + offset.z};
    m_primedgun_overlay_layer.size = {0.675f, 0.25f};
  }
  else
  {
    return appended_layer;
  }

  layers->push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&m_primedgun_overlay_layer));
  appended_layer = true;

  if (menu && laser_pose.valid && EnsurePrimedGunLaserSwapchain())
  {
    const XrQuaternionf q = laser_pose.orientation;
    const XrVector3f forward = PGO::RotateVector(q, {0.0f, 0.0f, -1.0f});
    m_primedgun_laser_layer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    m_primedgun_laser_layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                        XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    m_primedgun_laser_layer.space = VR::g_openxr->GetReferenceSpace();
    m_primedgun_laser_layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    m_primedgun_laser_layer.subImage.swapchain = m_primedgun_laser_swapchain.swapchain;
    m_primedgun_laser_layer.subImage.imageRect.offset = {0, 0};
    m_primedgun_laser_layer.subImage.imageRect.extent = {1, 30};
    m_primedgun_laser_layer.pose.orientation =
        PGO::MulQuat(q, {-0.70710678f, 0.0f, 0.0f, 0.70710678f});
    const float laser_length = overlay.menu_pointer_active && overlay.pointer_distance > 0.02f ?
                                   overlay.pointer_distance :
                                   8.0f;
    m_primedgun_laser_layer.pose.position = {
        laser_pose.position.x + forward.x * laser_length * 0.5f,
        laser_pose.position.y + forward.y * laser_length * 0.5f,
        laser_pose.position.z + forward.z * laser_length * 0.5f};
    m_primedgun_laser_layer.size = {0.008f, laser_length};
    layers->push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&m_primedgun_laser_layer));

    if (overlay.menu_pointer_active && overlay.pointer_distance > 0.02f)
    {
      const XrVector3f panel_normal =
          PGO::RotateVector(m_primedgun_overlay_layer.pose.orientation, {0.0f, 0.0f, 1.0f});
      m_primedgun_laser_hit_layer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
      m_primedgun_laser_hit_layer.layerFlags =
          XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
          XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
      m_primedgun_laser_hit_layer.space = VR::g_openxr->GetReferenceSpace();
      m_primedgun_laser_hit_layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
      m_primedgun_laser_hit_layer.subImage.swapchain = m_primedgun_laser_swapchain.swapchain;
      m_primedgun_laser_hit_layer.subImage.imageRect.offset = {4, 3};
      m_primedgun_laser_hit_layer.subImage.imageRect.extent = {24, 24};
      m_primedgun_laser_hit_layer.pose.orientation = m_primedgun_overlay_layer.pose.orientation;
      m_primedgun_laser_hit_layer.pose.position = {
          laser_pose.position.x + forward.x * overlay.pointer_distance + panel_normal.x * 0.002f,
          laser_pose.position.y + forward.y * overlay.pointer_distance + panel_normal.y * 0.002f,
          laser_pose.position.z + forward.z * overlay.pointer_distance + panel_normal.z * 0.002f};
      m_primedgun_laser_hit_layer.size = {0.02f, 0.02f};
      layers->push_back(
          reinterpret_cast<XrCompositionLayerBaseHeader*>(&m_primedgun_laser_hit_layer));
    }
  }

  return appended_layer;
}

AbstractFramebuffer* VulkanOpenXR::AcquireEyeFramebuffer(uint32_t eye_index)
{
  ASSERT(eye_index < 2);
  static unsigned int s_openxr_vk_acquire_log_count = 0;
  const uint64_t perf_start_us = Common::Timer::NowUs();
  Common::ScopeGuard perf_guard([perf_start_us] {
    g_vulkan_context->GetPerfCounters().xr_swapchain_us.fetch_add(
        Common::Timer::NowUs() - perf_start_us, std::memory_order_relaxed);
  });
#if !defined(ANDROID)
  // Pipelined: the frame takes the next eye set once the pacing thread released that set's
  // last image (one waited, unreleased image per swapchain).
  if (m_pacing == Pacing::Cycle && !m_image_acquired[0] && !m_image_acquired[1])
    WaitForPendingFrameFinalization("eye acquire");
  if (!m_frame_set_chosen)
  {
    m_frame_set = m_set_count == 2 ? m_next_set : 0;
    m_frame_set_chosen = true;
    if (m_pacing == Pacing::Pipelined)
    {
      std::unique_lock<std::mutex> lock(m_pacer_mutex);
      if (SetBusyLocked(m_frame_set))
      {
        m_video_waiting_for_set = true;
        m_pacer_cv.notify_all();
        const bool free = m_pacer_cv.wait_for(lock, std::chrono::seconds(2), [&] {
          return !SetBusyLocked(m_frame_set) || m_pacer_exit.load();
        });
        m_video_waiting_for_set = false;
        if (!free || SetBusyLocked(m_frame_set))
        {
          // The pacing thread did not give the set back (session ending): skip the eyes.
          m_frame_set_chosen = false;
          return nullptr;
        }
        lock.unlock();
        // The sets are held too long for this frame rate: no slack in the next target.
        m_slack_frames = 1000;
        std::lock_guard<std::mutex> stats_lock(m_stats_mutex);
        m_stats.set_waits++;
      }
    }
  }
  auto& sc = EyeSet(m_frame_set)[eye_index];
#else
  auto& sc = m_eye_swapchains[eye_index];
#endif
  m_frame_uses_layered_swapchain = false;

  XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  XrResult acquire_result = XR_SUCCESS;
  {
    auto queue_lock = AcquireGraphicsQueueLock();
    acquire_result =
        xrAcquireSwapchainImage(sc.swapchain, &acquire_info, &m_acquired_image_index[eye_index]);
  }
  if (XR_FAILED(acquire_result))
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR: xrAcquireSwapchainImage failed for eye {}.", eye_index);
    return nullptr;
  }
  m_image_acquired[eye_index] = true;

  // Block until the acquired image is safe to write.
  XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wait_info.timeout = XR_INFINITE_DURATION;
  if (XR_FAILED(xrWaitSwapchainImage(sc.swapchain, &wait_info)))
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR: xrWaitSwapchainImage failed for eye {}.", eye_index);

    // Ensure we don't leak an acquired image if waiting fails.
    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    XrResult release_result = XR_SUCCESS;
    {
      auto queue_lock = AcquireGraphicsQueueLock();
      release_result = xrReleaseSwapchainImage(sc.swapchain, &release_info);
    }
    if (XR_FAILED(release_result))
    {
      WARN_LOG_FMT(VIDEO,
                   "OpenXR: xrReleaseSwapchainImage after wait failure failed for eye {} ({}).",
                   eye_index, static_cast<int>(release_result));
    }
    m_image_acquired[eye_index] = false;
    return nullptr;
  }

  // Reset the image layout to UNDEFINED — the runtime may have changed it since last release,
  // and we're about to clear the image anyway via SetAndClearFramebuffer.
  const uint32_t idx = m_acquired_image_index[eye_index];
  sc.textures[idx]->OverrideImageLayout(VK_IMAGE_LAYOUT_UNDEFINED);

  if (s_openxr_vk_acquire_log_count < 12)
  {
    INFO_LOG_FMT(OPENXR,
                 "OpenXR VK acquire #{}: eye={} image={} format={} size={}x{} layout={} "
                 "gfx_queue_family={}",
                 s_openxr_vk_acquire_log_count + 1, eye_index, idx,
                 static_cast<int>(sc.textures[idx]->GetFormat()), sc.width, sc.height,
                 static_cast<int>(sc.textures[idx]->GetLayout()),
                 g_vulkan_context->GetGraphicsQueueFamilyIndex());
    s_openxr_vk_acquire_log_count++;
  }

  return sc.framebuffers[idx].get();
}

AbstractFramebuffer* VulkanOpenXR::AcquireLayeredFramebuffer()
{
  static unsigned int s_openxr_vk_layered_acquire_log_count = 0;
  auto& sc = m_layered_swapchain;
  if (!m_use_layered_swapchain || sc.swapchain == XR_NULL_HANDLE)
    return nullptr;

  XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  XrResult acquire_result = XR_SUCCESS;
  {
    auto queue_lock = AcquireGraphicsQueueLock();
    acquire_result =
        xrAcquireSwapchainImage(sc.swapchain, &acquire_info, &m_acquired_layered_image_index);
  }
  if (XR_FAILED(acquire_result))
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR: xrAcquireSwapchainImage failed for layered swapchain.");
    m_frame_uses_layered_swapchain = false;
    return nullptr;
  }
  m_layered_image_acquired = true;

  XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wait_info.timeout = 5'000'000;  // 5 ms; fallback to per-eye rendering instead of hanging.
  const XrResult wait_result = xrWaitSwapchainImage(sc.swapchain, &wait_info);
  if (wait_result != XR_SUCCESS)
  {
    WARN_LOG_FMT(VIDEO, "OpenXR: xrWaitSwapchainImage failed for layered swapchain ({}).",
                 static_cast<int>(wait_result));

    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    XrResult release_result = XR_SUCCESS;
    {
      auto queue_lock = AcquireGraphicsQueueLock();
      release_result = xrReleaseSwapchainImage(sc.swapchain, &release_info);
    }
    if (XR_FAILED(release_result))
    {
      WARN_LOG_FMT(VIDEO,
                   "OpenXR: xrReleaseSwapchainImage after layered wait failure failed ({}).",
                   static_cast<int>(release_result));
    }
    m_layered_image_acquired = false;
    m_frame_uses_layered_swapchain = false;
    return nullptr;
  }

  const uint32_t idx = m_acquired_layered_image_index;
  sc.textures[idx]->OverrideImageLayout(VK_IMAGE_LAYOUT_UNDEFINED);
  m_frame_uses_layered_swapchain = true;

  if (s_openxr_vk_layered_acquire_log_count < 12)
  {
    INFO_LOG_FMT(OPENXR,
                 "OpenXR VK layered acquire #{}: image={} format={} size={}x{} layers={} "
                 "layout={} gfx_queue_family={}",
                 s_openxr_vk_layered_acquire_log_count + 1, idx,
                 static_cast<int>(sc.textures[idx]->GetFormat()), sc.width, sc.height,
                 sc.textures[idx]->GetLayers(), static_cast<int>(sc.textures[idx]->GetLayout()),
                 g_vulkan_context->GetGraphicsQueueFamilyIndex());
    s_openxr_vk_layered_acquire_log_count++;
  }

  return sc.framebuffers[idx].get();
}

#if !defined(ANDROID)
// PRIMEFRAME_FRAME_STAMP=1 (tests only): each eye image gets a 256x20 pixel block in its top left
// corner naming the locates (display times, XrTime) whose poses the draws of the XFB shown used:
// the oldest and the newest. The mock runtime (MOCK_XR_STAMP) reads it back from the image a
// projection layer submits and compares both with the locate of the layer's poses. Layout, 4x4
// pixel cells, white = 1: row 0 the 64 bits of the oldest (bit 0 first), row 1 their complement,
// row 2: cell 0 the eye (1 = right), cell 1 its complement, cells 2-5 the marker 1 0 1 0, row 3 the
// newest, row 4 its complement. 0 means the XFB shown was not stamped at its copy.
static void WriteFrameStamp(uint32_t eye_index)
{
  const auto& pose = VR::g_openxr->GetPresentPose();
  const uint64_t oldest = pose.valid ? static_cast<uint64_t>(pose.draw_oldest) : 0;
  const uint64_t newest = pose.valid ? static_cast<uint64_t>(pose.draw_newest) : 0;
  constexpr uint32_t cell_size = 4;
  std::vector<VkClearRect> ones, zeros;
  const auto cell = [&](uint32_t column, uint32_t row, bool one) {
    const VkClearRect rect{{{static_cast<int32_t>(column * cell_size),
                             static_cast<int32_t>(row * cell_size)},
                            {cell_size, cell_size}},
                           0,
                           1};
    (one ? ones : zeros).push_back(rect);
  };
  for (uint32_t bit = 0; bit < 64; ++bit)
  {
    const bool old_one = ((oldest >> bit) & 1u) != 0;
    const bool new_one = ((newest >> bit) & 1u) != 0;
    cell(bit, 0, old_one);
    cell(bit, 1, !old_one);
    cell(bit, 3, new_one);
    cell(bit, 4, !new_one);
  }
  cell(0, 2, eye_index == 1);
  cell(1, 2, eye_index != 1);
  for (uint32_t k = 0; k < 4; ++k)
    cell(2 + k, 2, (k & 1u) == 0);
  StateTracker::GetInstance()->EndClearRenderPass();
  StateTracker::GetInstance()->BeginRenderPass();
  VkClearAttachment white{VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
  white.clearValue.color = {{1.0f, 1.0f, 1.0f, 1.0f}};
  VkClearAttachment black{VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
  black.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
  const VkCommandBuffer cmd = g_command_buffer_mgr->GetCurrentCommandBuffer();
  vkCmdClearAttachments(cmd, 1, &white, static_cast<uint32_t>(ones.size()), ones.data());
  vkCmdClearAttachments(cmd, 1, &black, static_cast<uint32_t>(zeros.size()), zeros.data());
}
#endif

void VulkanOpenXR::ReleaseEyeTexture(uint32_t eye_index)
{
  ASSERT(eye_index < 2);
  if (!m_image_acquired[eye_index])
    return;
#if !defined(ANDROID)
  static const bool s_frame_stamp = EnvIs("PRIMEFRAME_FRAME_STAMP", "1");
  if (s_frame_stamp)
    WriteFrameStamp(eye_index);
#endif

#if defined(ANDROID)
  // End any active render pass so the eye image is no longer bound for rendering.
  // We defer the Vulkan submit/xrReleaseSwapchainImage pairing until SubmitFrame()
  // so both eyes stay within one command-buffer lifetime.
  StateTracker::GetInstance()->EndRenderPass();
#else
  // PC OpenXR runtimes are sensitive to holding runtime-owned Vulkan swapchain images
  // across Dolphin's later frame-submit work. Preserve the original PC path: submit the
  // blit for this eye, then release the image back to the runtime.
  //
  // The spec only requires the image writes to be *submitted* before
  // xrReleaseSwapchainImage; GPU-side ordering is the runtime's job. Virtual Desktop
  // does that with a timeline semaphore it creates on our device at session creation —
  // which silently did nothing until the timelineSemaphore feature was enabled at
  // device creation, so this path used to drain the GPU per eye as a workaround
  // (serializing CPU/GPU/compositor every frame). Keep the drain only as a fallback for
  // VD/Quest-class runtimes when the feature could not be enabled — it is a property of
  // the runtime's strictness, not of which projection path is active (SteamVR never
  // needed it).
  StateTracker::GetInstance()->EndRenderPass();
#endif
}

void VulkanOpenXR::ReleaseLayeredTexture()
{
  if (!m_layered_image_acquired)
    return;

#if defined(ANDROID)
  StateTracker::GetInstance()->EndRenderPass();
#else
  // Same contract as ReleaseEyeTexture: submit before release; only drain the GPU as a
  // fallback for VD/Quest-class runtimes when timeline semaphores could not be enabled.
  StateTracker::GetInstance()->EndRenderPass();
#endif
}

bool VulkanOpenXR::SubmitFrame()
{
  ASSERT(VR::g_openxr != nullptr);

#if defined(ANDROID)
  static unsigned int s_openxr_vk_submit_frame_log_count = 0;
  static uint64_t s_openxr_vk_async_frame_id = 0;
  bool has_acquired_images = false;
  has_acquired_images |= m_layered_image_acquired;
  for (bool acquired : m_image_acquired)
  {
    has_acquired_images |= acquired;
  }

  if (has_acquired_images)
  {
    if (!WaitForPendingFrameFinalization("before queuing next async XR submit"))
      return false;

    PendingXRFrame pending_frame;
    pending_frame.debug_frame_id = ++s_openxr_vk_async_frame_id;
    pending_frame.queued_time_us = Common::Timer::NowUs();
    pending_frame.display_time = VR::g_openxr->GetPredictedDisplayTime();
    pending_frame.environment_blend_mode = VR::g_openxr->GetActiveBlendMode();
    pending_frame.should_render = VR::g_openxr->ShouldRender();
    pending_frame.space = VR::g_openxr->GetReferenceSpace();

    if (pending_frame.environment_blend_mode == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND)
    {
      pending_frame.layer_flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                  XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    }

    const auto& eye_views = VR::g_openxr->GetSubmittedEyeViews();
    if (!VR::g_openxr->AreSubmittedEyeViewsValid())
    {
      m_frame_uses_layered_swapchain = false;
      return VR::g_openxr->EndFrame({});
    }

    const bool submit_layered =
        m_frame_uses_layered_swapchain && m_use_layered_swapchain &&
        m_layered_swapchain.swapchain != XR_NULL_HANDLE;

    for (uint32_t eye = 0; eye < 2; ++eye)
    {
      auto& pv = pending_frame.projection_views[eye];
      pv = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
      pv.pose = eye_views[eye].pose;
      pv.fov = eye_views[eye].fov;
      if (submit_layered)
      {
        pv.subImage.swapchain = m_layered_swapchain.swapchain;
        pv.subImage.imageArrayIndex = eye;
        pv.subImage.imageRect = {{0, 0},
                                 {static_cast<int32_t>(m_layered_swapchain.width),
                                  static_cast<int32_t>(m_layered_swapchain.height)}};
      }
      else
      {
        pv.subImage.swapchain = m_eye_swapchains[eye].swapchain;
        pv.subImage.imageArrayIndex = 0;
        pv.subImage.imageRect = {{0, 0},
                                 {static_cast<int32_t>(m_eye_swapchains[eye].width),
                                  static_cast<int32_t>(m_eye_swapchains[eye].height)}};
      }
    }

    pending_frame.layered_acquired = m_layered_image_acquired;
    pending_frame.layered_swapchain = m_layered_swapchain.swapchain;
    m_layered_image_acquired = false;
    for (uint32_t eye = 0; eye < 2; ++eye)
    {
      pending_frame.eye_acquired[eye] = m_image_acquired[eye];
      pending_frame.eye_swapchains[eye] = m_eye_swapchains[eye].swapchain;
      m_image_acquired[eye] = false;
    }
    m_frame_uses_layered_swapchain = false;

    if (s_openxr_vk_submit_frame_log_count < 60)
    {
      INFO_LOG_FMT(VIDEO,
                   "OpenXR Vulkan: queued async final XR submit #{} "
                   "(layered_acquired={} eye0_acquired={} eye1_acquired={} "
                   "submit_layered={} layered_swapchain_enabled={}).",
                   pending_frame.debug_frame_id, pending_frame.layered_acquired,
                   pending_frame.eye_acquired[0], pending_frame.eye_acquired[1], submit_layered,
                   m_use_layered_swapchain);
#if defined(ANDROID)
      __android_log_print(
          ANDROID_LOG_INFO, "DolphinXR",
          "OpenXR Vulkan: queued async final XR submit #%llu "
          "(layered_acquired=%d eye0_acquired=%d eye1_acquired=%d submit_layered=%d "
          "layered_swapchain_enabled=%d)",
          static_cast<unsigned long long>(pending_frame.debug_frame_id),
          static_cast<int>(pending_frame.layered_acquired),
          static_cast<int>(pending_frame.eye_acquired[0]),
          static_cast<int>(pending_frame.eye_acquired[1]), static_cast<int>(submit_layered),
          static_cast<int>(m_use_layered_swapchain));
#endif
      s_openxr_vk_submit_frame_log_count++;
    }

    // Submit the eye rendering work once per frame before releasing the swapchain
    // images back to the runtime. Do not wait for GPU completion here; waiting
    // serializes the emulator, GPU, and Quest compositor every frame. We still
    // advance the Vulkan frame resources because the Quest direct-to-HMD path
    // skips PresentBackbuffer(), which normally resets descriptor pools.
    StateTracker::GetInstance()->EndRenderPass();
    m_async_frame_finalization_in_flight.store(true, std::memory_order_release);
    g_command_buffer_mgr->SubmitCommandBuffer(
        true, false, true, VK_NULL_HANDLE, 0xFFFFFFFF,
        [this, frame = std::move(pending_frame)]() mutable {
          FinalizePendingXRFrame(std::move(frame));
        });
    StateTracker::GetInstance()->InvalidateCachedState();

    return true;
  }
#endif

#if !defined(ANDROID)
  if (m_pacing == Pacing::Pipelined)
    return SubmitFramePipelined();
  if (m_pacing == Pacing::Cycle)
    return SubmitFrameCycle();

  bool desktop_has_acquired_images = m_layered_image_acquired;
  for (bool acquired : m_image_acquired)
    desktop_has_acquired_images |= acquired;
  m_frame_set_chosen = false;

  if (desktop_has_acquired_images)
  {
    StateTracker::GetInstance()->EndRenderPass();

    // Submit all eye rendering once per frame before releasing OpenXR swapchain images.
    // Drain (PRIMEFRAME_PACING=drain or PRIMEDGUN_XR_WAIT_GPU=1): the submit waits for the GPU.
    // Inline: a fence after the submit is waited for right before the release. SteamVR on the
    // Steam Frame shows a released image at once, finished or not (ghosting in the last eye).
    // PRIMEFRAME_NO_GPU_WAIT=1 skips both (the negative test of the mock's release check).
    // When the window present is skipped this submit also advances Dolphin's frame resources.
    const bool wait_for_completion =
        !m_no_gpu_wait &&
        (m_pacing == Pacing::Drain ||
         (!g_vulkan_context->SupportsTimelineSemaphores() && VR::g_openxr &&
          VR::g_openxr->IsQuestOrVirtualDesktopRuntime()));
    const uint64_t drain_start_us = Common::Timer::NowUs();
    g_command_buffer_mgr->SubmitCommandBuffer(false, wait_for_completion, m_skip_window_present);
    uint64_t gpu_wait_us = wait_for_completion ? ElapsedUs(drain_start_us, Common::Timer::NowUs()) : 0;
    bool gpu_waited = gpu_wait_us > 50;
    if (m_pacing == Pacing::Inline && !m_no_gpu_wait)
    {
      VkFence fence = TakeFence();
      {
        auto queue_lock = AcquireGraphicsQueueLock();
        vkQueueSubmit(g_vulkan_context->GetGraphicsQueue(), 0, nullptr, fence);
      }
      const uint64_t wait_start_us = Common::Timer::NowUs();
      gpu_waited = vkGetFenceStatus(g_vulkan_context->GetDevice(), fence) != VK_SUCCESS;
      vkWaitForFences(g_vulkan_context->GetDevice(), 1, &fence, VK_TRUE, UINT64_MAX);
      gpu_wait_us = ElapsedUs(wait_start_us, Common::Timer::NowUs());
      ReturnFence(fence);
    }
    {
      std::lock_guard<std::mutex> lock(m_stats_mutex);
      m_stats.drain_sum_us += gpu_wait_us;
      m_stats.drain_max_us = std::max(m_stats.drain_max_us, gpu_wait_us);
    }
    NoteGpuWait(gpu_wait_us, gpu_waited);
    StateTracker::GetInstance()->InvalidateCachedState();

    const uint64_t perf_release_start_us = Common::Timer::NowUs();
    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};

    if (m_layered_image_acquired)
    {
      XrResult release_result = XR_SUCCESS;
      {
        auto queue_lock = AcquireGraphicsQueueLock();
        release_result = xrReleaseSwapchainImage(m_layered_swapchain.swapchain, &release_info);
      }
      if (XR_FAILED(release_result))
      {
        WARN_LOG_FMT(VIDEO, "OpenXR: xrReleaseSwapchainImage failed for layered swapchain ({}).",
                     static_cast<int>(release_result));
      }
      m_layered_image_acquired = false;
    }

    for (uint32_t eye = 0; eye < 2; ++eye)
    {
      if (!m_image_acquired[eye])
        continue;

      XrResult release_result = XR_SUCCESS;
      {
        auto queue_lock = AcquireGraphicsQueueLock();
        release_result = xrReleaseSwapchainImage(m_eye_swapchains[eye].swapchain, &release_info);
      }
      if (XR_FAILED(release_result))
      {
        WARN_LOG_FMT(VIDEO, "OpenXR: xrReleaseSwapchainImage failed for eye {} ({}).", eye,
                     static_cast<int>(release_result));
      }
      m_image_acquired[eye] = false;
    }

    g_vulkan_context->GetPerfCounters().xr_release_us.fetch_add(
        Common::Timer::NowUs() - perf_release_start_us, std::memory_order_relaxed);
  }
  else if (m_skip_window_present)
  {
    // Nothing for the eyes this frame, but the frame's work still has to go out.
    StateTracker::GetInstance()->EndRenderPass();
    g_command_buffer_mgr->SubmitCommandBuffer(false, false, true);
    StateTracker::GetInstance()->InvalidateCachedState();
  }
  // Late: xrEndFrame more than a refresh after its xrWaitFrame returned (the frame misses the
  // display time it was predicted and located for).
  const auto inline_clock = VR::g_openxr->GetFrameClock();
  const bool inline_late = inline_clock.period_ns > 0 &&
                           SteadyNs() - inline_clock.wake_ns > inline_clock.period_ns;
#endif

  const auto overlay = Common::VR::OpenXRInputState::GetPrimedGunOverlay();
  const bool cinematic_screen_active = overlay.cinematic_screen_active;
#if !defined(ANDROID)
  if (cinematic_screen_active &&
      BuildCinematicScreenLayer(m_eye_swapchains, overlay.cinematic_screen_generation,
                                &m_cinematic_screen_layer))
  {
    std::vector<XrCompositionLayerBaseHeader*> layers = {
        reinterpret_cast<XrCompositionLayerBaseHeader*>(&m_cinematic_screen_layer)};
    AppendPrimedGunOverlayLayers(&layers);
    const bool result = VR::g_openxr->EndFrame(layers);
    m_frame_uses_layered_swapchain = false;
    NoteLayerEnd(desktop_has_acquired_images, inline_late);
    EmitStats();
    return result;
  }
  if (!cinematic_screen_active)
    ResetCinematicScreenAnchor();
#endif

  const auto& eye_views = VR::g_openxr->GetSubmittedEyeViews();
  if (!VR::g_openxr->AreSubmittedEyeViewsValid())
  {
    m_frame_uses_layered_swapchain = false;
    return VR::g_openxr->EndFrame({});
  }

  const bool submit_layered =
      m_frame_uses_layered_swapchain && m_use_layered_swapchain &&
      m_layered_swapchain.swapchain != XR_NULL_HANDLE;

  for (uint32_t eye = 0; eye < 2; ++eye)
  {
    auto& pv = m_projection_views[eye];
    pv = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    pv.pose = eye_views[eye].pose;
    pv.fov = eye_views[eye].fov;
    if (submit_layered)
    {
      pv.subImage.swapchain = m_layered_swapchain.swapchain;
      pv.subImage.imageArrayIndex = eye;
      pv.subImage.imageRect = {{0, 0},
                               {static_cast<int32_t>(m_layered_swapchain.width),
                                static_cast<int32_t>(m_layered_swapchain.height)}};
    }
    else
    {
      pv.subImage.swapchain = m_eye_swapchains[eye].swapchain;
      pv.subImage.imageArrayIndex = 0;
      pv.subImage.imageRect = {
          {0, 0},
          {static_cast<int32_t>(m_eye_swapchains[eye].width),
           static_cast<int32_t>(m_eye_swapchains[eye].height)}};
    }
  }

  m_projection_layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  m_projection_layer.space = VR::g_openxr->GetReferenceSpace();
  m_projection_layer.viewCount = 2;
  m_projection_layer.views = m_projection_views.data();

  if (VR::g_openxr->GetActiveBlendMode() == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND)
  {
    m_projection_layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                    XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
  }

  std::vector<XrCompositionLayerBaseHeader*> layers = {
      reinterpret_cast<XrCompositionLayerBaseHeader*>(&m_projection_layer)};
  AppendPrimedGunOverlayLayers(&layers);

  const bool result = VR::g_openxr->EndFrame(layers);
  m_frame_uses_layered_swapchain = false;
#if !defined(ANDROID)
  NoteLayerEnd(desktop_has_acquired_images, inline_late);
  EmitStats();
#endif
  return result;
}


#if !defined(ANDROID)
// Pipelined: a set is busy while an item of it is pending with its images unreleased (one waited,
// unreleased image per swapchain). Once released (at its fence, before its refresh) the game may
// render into the set's next images: the pacing thread releases and ends items in order, so the
// older layer is ended before the newer images are released. Under m_pacer_mutex.
bool VulkanOpenXR::SetBusyLocked(uint32_t set) const
{
  for (const XRFrameItem& item : m_pending)
  {
    if (item.set == set && !item.released)
      return true;
  }
  return false;
}

VkFence VulkanOpenXR::TakeFence()
{
  {
    std::lock_guard<std::mutex> lock(m_pacer_mutex);
    if (!m_free_fences.empty())
    {
      VkFence fence = m_free_fences.back();
      m_free_fences.pop_back();
      return fence;
    }
  }
  VkFenceCreateInfo info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence = VK_NULL_HANDLE;
  vkCreateFence(g_vulkan_context->GetDevice(), &info, nullptr, &fence);
  std::lock_guard<std::mutex> lock(m_pacer_mutex);
  m_all_fences.push_back(fence);
  return fence;
}

void VulkanOpenXR::ReturnFence(VkFence fence)
{
  if (fence == VK_NULL_HANDLE)
    return;
  vkResetFences(g_vulkan_context->GetDevice(), 1, &fence);
  std::lock_guard<std::mutex> lock(m_pacer_mutex);
  m_free_fences.push_back(fence);
}

void VulkanOpenXR::NoteLayerEnd(bool fresh, bool late)
{
  std::lock_guard<std::mutex> lock(m_stats_mutex);
  m_stats.layers++;
  if (fresh)
  {
    m_stats.fresh++;
    if (late)
      m_stats.late++;
  }
}

void VulkanOpenXR::NoteGpuWait(uint64_t wait_us, bool waited)
{
  std::lock_guard<std::mutex> lock(m_stats_mutex);
  if (waited)
  {
    m_stats.gpu_waits++;
    m_stats.gpu_wait_max_us = std::max(m_stats.gpu_wait_max_us, wait_us);
  }
}

// One INFO line a second (OpenXR log category) on the video thread (Dolphin's frame counters
// are folded in there): pacing mode; eye sets submitted by the game; layer stacks ended and new
// eye sets among them; new sets late (shown after the refresh their pose was located for, or
// ended more than a refresh after their xrWaitFrame); releases that had to wait for the GPU and
// the longest wait; the time the video thread waited for the GPU itself (drain or inline wait);
// GPU busy ms per submitted set (kernel DRM fdinfo); xrWaitFrame blocking time; waits for a free
// eye set; eye size; the game's frame and vblank rates.
void VulkanOpenXR::EmitStats()
{
  static uint64_t s_submitted = 0;
  static uint64_t s_last_gpu_ns = 0;
  s_submitted++;
  const uint64_t now_us = Common::Timer::NowUs();
  std::lock_guard<std::mutex> lock(m_stats_mutex);
  if (m_stats.window_start_us == 0)
  {
    m_stats.window_start_us = now_us;
    s_last_gpu_ns = ReadGpuBusyNs();
    s_submitted = 0;
    return;
  }
  if (now_us - m_stats.window_start_us < 1000000)
    return;
  g_perf_metrics.UpdateCounters();
  const uint64_t gpu_ns = ReadGpuBusyNs();
  const double gpu_ms = s_submitted ? (gpu_ns - s_last_gpu_ns) / 1e6 / s_submitted : 0.0;
  s_last_gpu_ns = gpu_ns;
  uint32_t waits = 0;
  uint64_t wait_total_us = 0, wait_max_us = 0;
  VR::OpenXRManager::FrameTimingStats timing{};
  if (VR::g_openxr)
  {
    VR::g_openxr->TakeWaitFrameStats(&waits, &wait_total_us, &wait_max_us);
    VR::g_openxr->TakeFrameTimingStats(&timing);
  }
  const double ended = std::max<uint32_t>(timing.frames, 1);
  INFO_LOG_FMT(OPENXR,
               "XRSTATS mode={} submitted={} layers={} shown={} late={} gpu_waits={} "
               "gpu_wait_max_ms={:.2f} drain_avg_ms={:.2f} drain_max_ms={:.2f} gpu_ms={:.2f} "
               "xrwait_avg_ms={:.2f} xrwait_max_ms={:.2f} set_waits={} discards={} "
               "end_avg_ms={:.2f} end_max_ms={:.2f} end_late={} age_avg_ms={:.2f} "
               "age_max_ms={:.2f} lead_avg_ms={:.2f} pose_off={} extra_avg_ms={:.2f} extra_frames={} "
               "eye={}x{} game_fps={:.1f} game_vps={:.1f}",
               m_pacing == Pacing::Pipelined ? (m_no_gpu_wait ? "pipelined-nowait" : "pipelined") :
               m_pacing == Pacing::Inline    ? (m_no_gpu_wait ? "inline-nowait" : "inline") :
               m_pacing == Pacing::Cycle     ? (m_no_gpu_wait ? "cycle-nowait" : "cycle") :
                                               (m_no_gpu_wait ? "drain-nowait" : "drain"),
               s_submitted, m_stats.layers, m_stats.fresh, m_stats.late, m_stats.gpu_waits,
               m_stats.gpu_wait_max_us / 1000.0,
               s_submitted ? m_stats.drain_sum_us / 1000.0 / s_submitted : 0.0,
               m_stats.drain_max_us / 1000.0, gpu_ms,
               waits ? wait_total_us / 1000.0 / waits : 0.0, wait_max_us / 1000.0,
               m_stats.set_waits, m_stats.discards, timing.end_sum_ms / ended, timing.end_max_ms,
               timing.ended_late,
               timing.age_count ? timing.age_sum_ms / timing.age_count : 0.0, timing.age_max_ms,
               timing.lead_sum_ms / ended, timing.pose_mismatch, timing.extra_sum_ms / ended,
               timing.extra_frames, m_eye_swapchains[0].width,
               m_eye_swapchains[0].height,
               g_perf_metrics.GetFPS(), g_perf_metrics.GetVPS());
  m_stats = {};
  m_stats.window_start_us = now_us;
  s_submitted = 0;
}

bool VulkanOpenXR::SubmitFramePipelined()
{
  const bool any_acquired = m_image_acquired[0] || m_image_acquired[1];
  if (!any_acquired)
  {
    // Nothing for the eyes (not rendering); the pacing thread repeats what it has.
    m_frame_set_chosen = false;
    if (m_skip_window_present)
    {
      StateTracker::GetInstance()->EndRenderPass();
      g_command_buffer_mgr->SubmitCommandBuffer(false, false, true);
      StateTracker::GetInstance()->InvalidateCachedState();
    }
    return true;
  }

  // Submit the frame's work (game tail and eye blits), then mark the queue with a fence: the
  // pacing thread releases the images once it signalled, and the work runs while the next game
  // frame is emulated.
  StateTracker::GetInstance()->EndRenderPass();
  g_command_buffer_mgr->SubmitCommandBuffer(false, false, m_skip_window_present);
  StateTracker::GetInstance()->InvalidateCachedState();

  XRFrameItem item;
  item.id = ++m_frame_id;
  item.set = m_frame_set;
  item.eye_acquired = m_image_acquired;
  item.submit_ns = SteadyNs();
  item.target = m_target_time;
  if (item.target == 0)
    item.target = VR::g_openxr->GetFrameClock().display_time;
  if (m_no_gpu_wait)
  {
    // Negative test: released right after the submit, no wait of any kind.
    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    for (uint32_t eye = 0; eye < 2; ++eye)
    {
      if (!item.eye_acquired[eye])
        continue;
      auto queue_lock = AcquireGraphicsQueueLock();
      xrReleaseSwapchainImage(EyeSet(item.set)[eye].swapchain, &release_info);
    }
    item.released = true;
    item.complete = true;
  }
  else
  {
    item.fence = TakeFence();
    auto queue_lock = AcquireGraphicsQueueLock();
    vkQueueSubmit(g_vulkan_context->GetGraphicsQueue(), 0, nullptr, item.fence);
  }

  // The poses the frame was rendered with (both eyes from one xrLocateViews).
  const auto& eye_views = VR::g_openxr->GetSubmittedEyeViews();
  item.views_valid = VR::g_openxr->AreSubmittedEyeViewsValid();
  auto& set = EyeSet(item.set);
  for (uint32_t eye = 0; eye < 2; ++eye)
  {
    auto& pv = item.views[eye];
    pv = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    pv.pose = eye_views[eye].pose;
    pv.fov = eye_views[eye].fov;
    pv.subImage.swapchain = set[eye].swapchain;
    pv.subImage.imageArrayIndex = 0;
    pv.subImage.imageRect = {{0, 0},
                             {static_cast<int32_t>(set[eye].width),
                              static_cast<int32_t>(set[eye].height)}};
  }
  if (VR::g_openxr->GetActiveBlendMode() == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND)
  {
    item.layer_flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                       XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
  }
  const auto overlay = Common::VR::OpenXRInputState::GetPrimedGunOverlay();
  if (overlay.cinematic_screen_active)
  {
    item.cinematic = BuildCinematicScreenLayer(set, overlay.cinematic_screen_generation,
                                               &item.cinematic_layer);
  }
  else
  {
    ResetCinematicScreenAnchor();
  }
  std::vector<XrCompositionLayerBaseHeader*> quads;
  AppendPrimedGunOverlayLayers(&quads);
  for (XrCompositionLayerBaseHeader* quad : quads)
    item.quads.push_back(*reinterpret_cast<XrCompositionLayerQuad*>(quad));

  m_image_acquired = {false, false};
  const uint32_t used_set = item.set;
  {
    std::lock_guard<std::mutex> lock(m_pacer_mutex);
    m_pending.push_back(std::move(item));
  }
  m_pacer_cv.notify_all();
  if (m_set_count == 2)
    m_next_set = used_set ^ 1;
  m_frame_set_chosen = false;

  // How long the game takes from the pose to the submit, and from submit to submit.
  const int64_t now = SteadyNs();
  if (m_locate_ns)
  {
    const double span_ms = (now - m_locate_ns) / 1e6;
    m_span_ema_ms = m_span_ema_ms == 0.0 ? span_ms : m_span_ema_ms * 0.9 + span_ms * 0.1;
  }
  if (m_last_submit_ns)
  {
    const double interval_ms = std::min((now - m_last_submit_ns) / 1e6, 100.0);
    m_interval_ema_ms =
        m_interval_ema_ms == 0.0 ? interval_ms : m_interval_ema_ms * 0.9 + interval_ms * 0.1;
  }
  m_last_submit_ns = now;
  EmitStats();
  return true;
}

// Cycle: the frame was begun (xrWaitFrame, xrBeginFrame, xrLocateViews) at its first pose read;
// submit its work, put a fence behind it and hand it to the releaser thread, which releases the
// eye images once the fence signalled and ends the frame with the display time of its own
// xrWaitFrame and the poses it was drawn with. The video thread goes on with the next game frame;
// the next frame begins only after this one ended (WaitForPendingFrameFinalization).
bool VulkanOpenXR::SubmitFrameCycle()
{
  const bool any_acquired = m_image_acquired[0] || m_image_acquired[1];
  StateTracker::GetInstance()->EndRenderPass();
  g_command_buffer_mgr->SubmitCommandBuffer(false, false, m_skip_window_present);
  StateTracker::GetInstance()->InvalidateCachedState();

  XRFrameItem item;
  item.id = ++m_frame_id;
  item.set = 0;
  item.eye_acquired = m_image_acquired;
  item.submit_ns = SteadyNs();
  item.target = VR::g_openxr->GetPredictedDisplayTime();
  item.should_render = VR::g_openxr->ShouldRender();
  {
    const int64_t cpu = ThreadCpuNs();
    item.wake_ns = VR::g_openxr->GetFrameClock().wake_ns;
    item.video_wait_ns = m_cycle_video_wait_ns;
    item.video_cpu_ns = m_cycle_last_cpu_ns ? cpu - m_cycle_last_cpu_ns : 0;
    item.video_wall_ns = m_last_submit_ns ? item.submit_ns - m_last_submit_ns : 0;
    m_cycle_video_wait_ns = 0;
    m_cycle_last_cpu_ns = cpu;
    m_last_submit_ns = item.submit_ns;
  }
  if (any_acquired && m_no_gpu_wait)
  {
    // Negative test: released right after the submit, no wait of any kind.
    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    for (uint32_t eye = 0; eye < 2; ++eye)
    {
      if (!item.eye_acquired[eye])
        continue;
      auto queue_lock = AcquireGraphicsQueueLock();
      xrReleaseSwapchainImage(m_eye_swapchains[eye].swapchain, &release_info);
    }
    item.released = true;
    item.complete = true;
  }
  else if (any_acquired)
  {
    item.fence = TakeFence();
    auto queue_lock = AcquireGraphicsQueueLock();
    vkQueueSubmit(g_vulkan_context->GetGraphicsQueue(), 0, nullptr, item.fence);
  }
  else
  {
    item.released = true;
    item.complete = true;
  }

  const auto& eye_views = VR::g_openxr->GetSubmittedEyeViews();
  item.views_valid = any_acquired && VR::g_openxr->AreSubmittedEyeViewsValid();
  item.pose_time = item.views_valid ? VR::g_openxr->GetSubmittedLocateTime() : 0;
  for (uint32_t eye = 0; eye < 2; ++eye)
  {
    auto& pv = item.views[eye];
    pv = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    pv.pose = eye_views[eye].pose;
    pv.fov = eye_views[eye].fov;
    pv.subImage.swapchain = m_eye_swapchains[eye].swapchain;
    pv.subImage.imageArrayIndex = 0;
    pv.subImage.imageRect = {{0, 0},
                             {static_cast<int32_t>(m_eye_swapchains[eye].width),
                              static_cast<int32_t>(m_eye_swapchains[eye].height)}};
  }
  if (VR::g_openxr->GetActiveBlendMode() == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND)
  {
    item.layer_flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                       XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
  }
  const auto overlay = Common::VR::OpenXRInputState::GetPrimedGunOverlay();
  if (overlay.cinematic_screen_active)
  {
    item.cinematic = BuildCinematicScreenLayer(m_eye_swapchains,
                                               overlay.cinematic_screen_generation,
                                               &item.cinematic_layer);
  }
  else
  {
    ResetCinematicScreenAnchor();
  }
  std::vector<XrCompositionLayerBaseHeader*> quads;
  AppendPrimedGunOverlayLayers(&quads);
  for (XrCompositionLayerBaseHeader* quad : quads)
    item.quads.push_back(*reinterpret_cast<XrCompositionLayerQuad*>(quad));

  m_image_acquired = {false, false};
  m_frame_set_chosen = false;
  m_frame_uses_layered_swapchain = false;
  {
    std::lock_guard<std::mutex> lock(m_pacer_mutex);
    m_pending.push_back(std::move(item));
  }
  m_pacer_cv.notify_all();
  EmitStats();
  return true;
}

// Cycle: the releaser thread. One frame at a time: wait for its fence, release its eye images,
// end it. Late: ended more than a refresh after its xrWaitFrame returned.
void VulkanOpenXR::CycleLoop()
{
  Common::SetCurrentThreadName("PrimeFrame XR releaser");
  const VkDevice device = g_vulkan_context->GetDevice();
  while (!m_pacer_exit.load())
  {
    XRFrameItem item;
    {
      std::unique_lock<std::mutex> lock(m_pacer_mutex);
      m_pacer_cv.wait_for(lock, std::chrono::milliseconds(100),
                          [&] { return m_pacer_exit.load() || !m_pending.empty(); });
      if (m_pending.empty())
        continue;
      item = std::move(m_pending.front());
      m_pending.pop_front();
      m_cycle_busy = true;
    }
    const int64_t picked_ns = SteadyNs();
    if (item.fence != VK_NULL_HANDLE && !item.complete)
    {
      const int64_t start = SteadyNs();
      const bool waited = vkGetFenceStatus(device, item.fence) != VK_SUCCESS;
      vkWaitForFences(device, 1, &item.fence, VK_TRUE, UINT64_MAX);
      item.complete = true;
      NoteGpuWait(static_cast<uint64_t>((SteadyNs() - start) / 1000), waited);
    }
    const int64_t fence_ns = SteadyNs();
    const int64_t hold_ns = CycleEndHold(item, fence_ns);
    ReleaseItem(item, true);

    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    std::vector<XrCompositionLayerBaseHeader*> layers;
    if (item.cinematic)
    {
      layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&item.cinematic_layer));
    }
    else if (item.views_valid)
    {
      projection.layerFlags = item.layer_flags;
      projection.space = VR::g_openxr->GetReferenceSpace();
      projection.viewCount = 2;
      projection.views = item.views.data();
      layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&projection));
    }
    for (XrCompositionLayerQuad& quad : item.quads)
      layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad));
    VR::g_openxr->EndFrameDetached(item.target, VR::g_openxr->GetActiveBlendMode(),
                                   item.should_render && !layers.empty(), layers,
                                   item.views_valid && !item.cinematic ? item.pose_time : 0);
    const auto clock = VR::g_openxr->GetFrameClock();
    const int64_t end_ns = SteadyNs();
    NoteLayerEnd(item.views_valid,
                 clock.period_ns > 0 && end_ns - item.wake_ns > clock.period_ns);
    // Overlap: the next frame was waited for while this one was ending; begin it now. Decided
    // under the lock together with leaving the busy state, so a wait that comes later sees this
    // frame ended and begins its frame itself.
    bool begin_next = false;
    {
      std::lock_guard<std::mutex> lock(m_pacer_mutex);
      begin_next = m_cycle_begin_pending;
      if (!begin_next)
        m_cycle_busy = false;
    }
    if (begin_next && !VR::g_openxr->BeginFrame())
      ERROR_LOG_FMT(OPENXR, "PrimeFrame cycle: xrBeginFrame on the releaser thread failed.");
    static const bool s_frame_log = !EnvIs("PRIMEFRAME_FRAME_LOG", "0");
    if (s_frame_log && item.wake_ns != 0)
    {
      // Times in ms from the frame's xrWaitFrame return: sub its submit, gpu its fence seen
      // signalled, end its xrEndFrame; vw the video thread's wait for the last frame's end in
      // this frame, vcpu / vwall the video thread's CPU / wall time since the last submit, q the
      // releaser picking the frame up after its submit.
      INFO_LOG_FMT(OPENXR,
                   "XRC n={} sub={:.2f} gpu={:.2f} end={:.2f} vw={:.2f} vcpu={:.2f} vwall={:.2f} "
                   "q={:.2f} hold={:.2f}",
                   item.id, (item.submit_ns - item.wake_ns) / 1e6,
                   (fence_ns - item.wake_ns) / 1e6, (end_ns - item.wake_ns) / 1e6,
                   item.video_wait_ns / 1e6, item.video_cpu_ns / 1e6, item.video_wall_ns / 1e6,
                   (picked_ns - item.submit_ns) / 1e6, hold_ns / 1e6);
    }
    {
      std::lock_guard<std::mutex> lock(m_pacer_mutex);
      m_cycle_busy = false;
      if (begin_next)
        m_cycle_begin_pending = false;
    }
    m_pacer_cv.notify_all();
  }
  ReleaseCompletedItems(true);
  {
    std::lock_guard<std::mutex> lock(m_pacer_mutex);
    m_cycle_busy = false;
  }
  m_pacer_cv.notify_all();
  INFO_LOG_FMT(OPENXR, "PrimeFrame releaser thread exiting.");
}

// Cycle end (with the overlap), releaser thread, after the frame's fence. SteamVR on the Frame
// (framesToThrottle 1) shows a frame on its display time when it is ready up to about 19.5 ms
// (PRIMEFRAME_PREDICT_ONTIME_MS) after the wake-up of its xrWaitFrame, a whole period later when
// it is ready later. The game runs at exactly 60 Hz against SteamVR's 60 Hz slots, so its frames
// start at a fixed phase after the wake-up, which a launch cannot choose; a room whose frames end
// near that limit would mix both (an image held 4 refreshes, the next never shown). asap: end at
// once, but not before PRIMEFRAME_CYCLE_EARLIEST_MS (16) after the wake-up (0: no minimum): the
// frames the headset showed clean ended 16 to 19.5 ms after it, the ones it doubled within 12 ms
// (a light room draws a frame in a few ms once its xrWaitFrame returned). late: hold the release and the end until 2.5 ms past the limit, so every frame is shown a
// period later (PRIMEFRAME_PREDICT_EXTRA_MS=auto locates the poses for that refresh). auto: asap
// while at most 2 of the last 120 frames finished past the limit, else late; back to asap when
// the last 600 frames all finished 1 ms inside it, at the earliest 10 s after the switch (twice as
// long after each return to asap that lasted under 30 s, up to 160 s). Returns the hold.
int64_t VulkanOpenXR::CycleEndHold(const XRFrameItem& item, int64_t fence_ns)
{
  if (!m_cycle_overlap || !item.views_valid || item.target == 0)
    return 0;
  const auto clock = VR::g_openxr->GetFrameClock();
  if (clock.grid_offset_ns == 0)
    return 0;
  // The runtime's wake-up for the frame's display time (its lead from the last call that blocked
  // for it), on the steady clock.
  const int64_t wake_ns = item.target - clock.grid_offset_ns;
  const float ready_ms = static_cast<float>((fence_ns - wake_ns) / 1e6);
  m_end_ready[m_end_ready_next] = ready_ms;
  m_end_ready_next = (m_end_ready_next + 1) % m_end_ready.size();
  m_end_ready_count++;

  if (m_end_cfg == 2)
  {
    const auto count_last = [&](size_t n, auto&& pred) {
      size_t hits = 0;
      for (size_t i = 1; i <= n; ++i)
        hits += pred(m_end_ready[(m_end_ready_next + m_end_ready.size() - i) % m_end_ready.size()]);
      return hits;
    };
    const int64_t dwell = fence_ns - m_end_since_ns;
    int mode = m_end_mode;
    size_t late = 0;
    if (mode == 0 && m_end_ready_count >= 120)
    {
      late = count_last(120, [&](float v) { return v > m_end_ontime_ms; });
      if (late > 2)
      {
        mode = 1;
        m_end_dwell_ns = dwell < 30'000'000'000LL ?
                             std::min<int64_t>(m_end_dwell_ns * 2, 160'000'000'000LL) :
                             10'000'000'000LL;
      }
    }
    else if (mode == 1 && m_end_ready_count >= m_end_ready.size() && dwell > m_end_dwell_ns)
    {
      late = count_last(m_end_ready.size(), [&](float v) { return v > m_end_ontime_ms - 1.0f; });
      if (late == 0)
        mode = 0;
    }
    if (mode != m_end_mode)
    {
      INFO_LOG_FMT(OPENXR,
                   "PrimeFrame cycle end: {} ({} frames finished past {:.1f} ms after the "
                   "wake-up; next return to asap after {} s at the earliest)",
                   mode == 1 ? "late (held)" : "asap", late, mode == 1 ? m_end_ontime_ms :
                                                             m_end_ontime_ms - 1.0,
                   m_end_dwell_ns / 1'000'000'000LL);
      m_end_mode = mode;
      m_end_since_ns = fence_ns;
      m_end_ready_count = 0;
    }
  }
  const double end_ms = m_end_mode == 1 ? m_end_ontime_ms + 2.5 : m_end_earliest_ms;
  if (end_ms <= 0)
    return 0;
  const int64_t target = wake_ns + static_cast<int64_t>(end_ms * 1e6);
  const int64_t now = SteadyNs();
  if (target <= now)
    return 0;
  std::this_thread::sleep_for(std::chrono::nanoseconds(target - now));
  return target - now;
}

bool VulkanOpenXR::ReleaseItem(XRFrameItem& item, bool wait_gpu)
{
  if (item.fence != VK_NULL_HANDLE && wait_gpu && !item.complete)
  {
    vkWaitForFences(g_vulkan_context->GetDevice(), 1, &item.fence, VK_TRUE, UINT64_MAX);
    item.complete = true;
  }
  bool ok = true;
  if (!item.released)
  {
    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    for (uint32_t eye = 0; eye < 2; ++eye)
    {
      if (!item.eye_acquired[eye])
        continue;
      XrResult result;
      {
        auto queue_lock = AcquireGraphicsQueueLock();
        result = xrReleaseSwapchainImage(EyeSet(item.set)[eye].swapchain, &release_info);
      }
      if (XR_FAILED(result))
      {
        WARN_LOG_FMT(VIDEO, "OpenXR: xrReleaseSwapchainImage failed for set {} eye {} ({}).",
                     item.set, eye, static_cast<int>(result));
        ok = false;
      }
    }
    item.released = true;
  }
  ReturnFence(item.fence);
  item.fence = VK_NULL_HANDLE;
  return ok;
}

void VulkanOpenXR::ReleaseCompletedItems(bool block)
{
  std::vector<XRFrameItem> done;
  {
    std::lock_guard<std::mutex> lock(m_pacer_mutex);
    while (!m_pending.empty())
    {
      XRFrameItem& front = m_pending.front();
      if (!block && !front.complete && front.fence != VK_NULL_HANDLE &&
          vkGetFenceStatus(g_vulkan_context->GetDevice(), front.fence) != VK_SUCCESS)
      {
        break;
      }
      done.push_back(std::move(front));
      m_pending.pop_front();
    }
  }
  for (XRFrameItem& item : done)
    ReleaseItem(item, true);
  if (!done.empty())
    m_pacer_cv.notify_all();
}

void VulkanOpenXR::StartPacer()
{
  if (m_pacer.joinable())
    return;
  m_pacer_exit.store(false);
  if (m_pacing == Pacing::Cycle)
    m_pacer = std::thread(&VulkanOpenXR::CycleLoop, this);
  else
    m_pacer = std::thread(&VulkanOpenXR::PacerLoop, this);
  INFO_LOG_FMT(OPENXR, "PrimeFrame {} thread started.",
               m_pacing == Pacing::Cycle ? "releaser" : "pacing");
}

void VulkanOpenXR::StopPacer()
{
  if (!m_pacer.joinable())
    return;
  m_pacer_exit.store(true);
  m_pacer_cv.notify_all();
  m_pacer.join();
  INFO_LOG_FMT(OPENXR, "PrimeFrame pacing thread stopped.");
}

// Pipelined: release the images of finished sets before their slot, in order, so that only
// xrBeginFrame and xrEndFrame are left between a slot's xrWaitFrame and its end, and the game
// gets the set back as soon as the GPU is done with it. A set's new images are released only
// once the layer that last used the set was ended and is no longer the one on show (shown_set:
// the set of the last ended layer, -1 none): SteamVR shows a released image at once.
void VulkanOpenXR::ReleaseFinishedEarly(int shown_set)
{
  const VkDevice device = g_vulkan_context->GetDevice();
  while (true)
  {
    XRFrameItem* item = nullptr;
    {
      std::lock_guard<std::mutex> lock(m_pacer_mutex);
      std::array<bool, 2> used{shown_set == 0, shown_set == 1};
      for (XRFrameItem& pending : m_pending)
      {
        if (pending.released)
        {
          used[pending.set] = true;
          continue;
        }
        if (used[pending.set] || pending.fence == VK_NULL_HANDLE ||
            (!pending.complete && vkGetFenceStatus(device, pending.fence) != VK_SUCCESS))
        {
          break;
        }
        item = &pending;
        break;
      }
      if (!item)
        return;
    }
    // Only the pacing thread releases and removes items, so the item stays where it is.
    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    for (uint32_t eye = 0; eye < 2; ++eye)
    {
      if (!item->eye_acquired[eye])
        continue;
      auto queue_lock = AcquireGraphicsQueueLock();
      xrReleaseSwapchainImage(EyeSet(item->set)[eye].swapchain, &release_info);
    }
    VkFence done_fence = VK_NULL_HANDLE;
    {
      std::lock_guard<std::mutex> lock(m_pacer_mutex);
      item->released = true;
      item->complete = true;
      done_fence = item->fence;
      item->fence = VK_NULL_HANDLE;
    }
    ReturnFence(done_fence);
    // The game may draw into the set's next images now.
    m_pacer_cv.notify_all();
  }
}

// The pacing thread: one xrWaitFrame/xrBeginFrame/xrEndFrame per display refresh. A new eye set
// is shown on the refresh its pose was located for (its target) once the GPU finished it: its
// images are released then (never before the fence signalled) and the projection layer ends
// with exactly the poses it was rendered with. Refreshes without a new set repeat the last one.
void VulkanOpenXR::PacerLoop()
{
  Common::SetCurrentThreadName("PrimeFrame XR pacing");
  const VkDevice device = g_vulkan_context->GetDevice();
  XRFrameItem current;
  bool have_current = false;

  while (!m_pacer_exit.load())
  {
    if (!VR::g_openxr->PollEvents() || !VR::g_openxr->IsSessionRunning())
    {
      ReleaseCompletedItems(false);
      have_current = false;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }

    // Unless PRIMEFRAME_REPEATS=1, a frame is ended only on a refresh that shows a new eye set,
    // and that is the refresh its poses were located for: every layer goes out with the display
    // time of its poses. On the refreshes in between the runtime shows the last frame again
    // itself, reprojected from the pose and the display time it was ended with. (Ending a repeat
    // on the next refresh with the previous refresh's poses gave SteamVR, at 60 fps, every second
    // layer with poses a refresh older than its display time.)
    int64_t due_wake = 0;
    if (!m_repeat_layers && have_current)
    {
      XrTime due = 0;
      {
        std::unique_lock<std::mutex> lock(m_pacer_mutex);
        // Without a new set for 100 ms (loading, paused, not rendering) a frame repeats the last.
        m_pacer_cv.wait_for(lock, std::chrono::milliseconds(100),
                            [&] { return m_pacer_exit.load() || !m_pending.empty(); });
        if (!m_pending.empty())
          due = m_pending.front().target;
      }
      if (m_pacer_exit.load())
        break;
      const auto clock = VR::g_openxr->GetFrameClock();
      if (due != 0 && clock.period_ns > 0)
        due_wake = due - clock.grid_offset_ns;
    }

    // Until the xrWaitFrame call: release finished sets as they finish, and measure the GPU tail.
    // The call: when the due set is for the very next slot the runtime hands out (SteamVR
    // throttling the app to every second refresh: consecutive slots) 1 ms before the estimated
    // wake-up, and xrWaitFrame blocks until the runtime's own wake-up; when slots are skipped
    // (an xrWaitFrame called after skipped slots returns at once with the slot that is running:
    // SteamVR on the Frame, the mock's MOCK_XR_SKIP=late) just after the due slot's wake-up.
    // Without a due set (repeats, start): before the next refresh.
    {
      int64_t call_at = 0;
      if (due_wake != 0)
      {
        const int64_t slot =
            std::max(m_slot_ns.load(), VR::g_openxr->GetFrameClock().period_ns);
        XrTime due = 0;
        {
          std::lock_guard<std::mutex> lock(m_pacer_mutex);
          if (!m_pending.empty())
            due = m_pending.front().target;
        }
        const bool next_slot =
            m_last_handed != 0 && due != 0 && due <= m_last_handed + slot + slot / 2;
        call_at = next_slot ? due_wake - 1'000'000 : due_wake + 250'000;
      }
      else
      {
        const auto clock = VR::g_openxr->GetFrameClock();
        if (clock.period_ns > 0)
          call_at = clock.wake_ns + clock.period_ns - 500'000;
      }
      // A finished set: its GPU tail (submit to finished) sets the margin the next targets allow.
      const auto note_done = [&](VkFence, int64_t submit_ns, uint64_t id) {
        const double tail_ms = (SteadyNs() - submit_ns) / 1e6;
        const double ema = m_gpu_tail_ema_ms.load();
        m_gpu_tail_ema_ms.store(tail_ms > ema ? ema * 0.7 + tail_ms * 0.3 :
                                                ema * 0.95 + tail_ms * 0.05);
        std::lock_guard<std::mutex> lock(m_pacer_mutex);
        for (XRFrameItem& item : m_pending)
        {
          if (item.id == id)
            item.complete = true;
        }
      };
      while (!m_pacer_exit.load())
      {
        // Not within 3 ms of the call: a release takes the queue lock, which the game's submits
        // may hold for a while, and the runtime's own work (the mock copies the image) (the due
        // set is released at its slot then).
        if (!m_no_gpu_wait && m_early_release && (call_at == 0 || SteadyNs() < call_at - 3'000'000))
          ReleaseFinishedEarly(have_current ? static_cast<int>(current.set) : -1);
        VkFence fence = VK_NULL_HANDLE;
        int64_t submit_ns = 0;
        uint64_t id = 0, newest = 0;
        {
          std::lock_guard<std::mutex> lock(m_pacer_mutex);
          for (const XRFrameItem& item : m_pending)
          {
            if (fence == VK_NULL_HANDLE && !item.complete && item.fence != VK_NULL_HANDLE)
            {
              fence = item.fence;
              submit_ns = item.submit_ns;
              id = item.id;
            }
            newest = item.id;
          }
        }
        const int64_t now = SteadyNs();
        if (call_at == 0 || now >= call_at)
          break;
        // A fence wait's timeout may overshoot by a few ms (the kernel's wait), so the last 3 ms
        // before the call are a plain timed sleep.
        if (fence != VK_NULL_HANDLE && now < call_at - 3'000'000)
        {
          if (vkWaitForFences(device, 1, &fence, VK_TRUE,
                              static_cast<uint64_t>(call_at - 3'000'000 - now)) == VK_SUCCESS)
            note_done(fence, submit_ns, id);
        }
        else
        {
          // Sleep until the call, or until the game hands over a new set (or, with a set on the
          // GPU, 0.5 ms to look at its fence again).
          const int64_t until =
              fence != VK_NULL_HANDLE ? std::min(call_at, now + 500'000) : call_at;
          const auto deadline =
              std::chrono::steady_clock::time_point(std::chrono::nanoseconds(until));
          {
            std::unique_lock<std::mutex> lock(m_pacer_mutex);
            m_pacer_cv.wait_until(lock, deadline, [&] {
              return m_pacer_exit.load() || (!m_pending.empty() && m_pending.back().id != newest);
            });
          }
          if (fence != VK_NULL_HANDLE && vkGetFenceStatus(device, fence) == VK_SUCCESS)
            note_done(fence, submit_ns, id);
        }
      }
    }

    if (!VR::g_openxr->WaitFrame())
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      continue;
    }
    const auto clock = VR::g_openxr->GetFrameClock();
    const XrTime display_time = clock.display_time;
    // The runtime's slot: the smallest spacing of the display times it handed out (the pacing
    // thread asks for every slot at the start, so at the full refresh rate this is one refresh;
    // with SteamVR throttling the app it is two).
    if (m_last_handed != 0)
    {
      const int64_t spacing = display_time - m_last_handed;
      const int64_t slot = m_slot_ns.load();
      if (spacing > 5'000'000 && (slot == 0 || spacing < slot))
        m_slot_ns.store(spacing);
    }
    m_last_handed = display_time;
    const int64_t period = std::max<int64_t>(clock.period_ns, 1);
    const bool should_render = VR::g_openxr->ShouldRender();

    // The newest set due on this refresh (or the oldest when the game waits for a free set).
    int chosen = -1;
    VkFence chosen_fence = VK_NULL_HANDLE;
    bool chosen_complete = false;
    {
      std::lock_guard<std::mutex> lock(m_pacer_mutex);
      for (size_t i = 0; i < m_pending.size(); ++i)
      {
        if (m_pending[i].target <= display_time + period / 2)
          chosen = static_cast<int>(i);
      }
      if (chosen >= 0)
      {
        chosen_fence = m_pending[chosen].fence;
        chosen_complete = m_pending[chosen].complete;
      }
    }
    if (chosen < 0 && due_wake != 0 && !m_repeat_layers)
    {
      // Woken for a set, but the runtime handed out an earlier refresh than its poses were
      // located for: begin that refresh's frame and leave it unended (the next xrBeginFrame
      // discards it; the runtime shows the last frame again) rather than end the last set with a
      // display time its poses were not located for.
      VR::g_openxr->BeginFrame();
      std::lock_guard<std::mutex> lock(m_stats_mutex);
      m_stats.discards++;
      continue;
    }
    bool waited = false;
    uint64_t wait_us = 0;
    if (chosen >= 0 && !chosen_complete && chosen_fence != VK_NULL_HANDLE)
    {
      // Due but not finished: wait for it up to half a refresh, else show what is finished.
      if (vkGetFenceStatus(device, chosen_fence) != VK_SUCCESS)
      {
        waited = true;
        const int64_t start = SteadyNs();
        const int64_t timeout = clock.wake_ns + period / 2 - start;
        const VkResult r = timeout > 0 ? vkWaitForFences(device, 1, &chosen_fence, VK_TRUE,
                                                         static_cast<uint64_t>(timeout)) :
                                         VK_TIMEOUT;
        wait_us = static_cast<uint64_t>((SteadyNs() - start) / 1000);
        if (r != VK_SUCCESS)
        {
          std::lock_guard<std::mutex> lock(m_pacer_mutex);
          int fallback = -1;
          for (int i = chosen - 1; i >= 0; --i)
          {
            if (m_pending[i].complete || m_pending[i].fence == VK_NULL_HANDLE ||
                vkGetFenceStatus(device, m_pending[i].fence) == VK_SUCCESS)
            {
              fallback = i;
              break;
            }
          }
          chosen = fallback;
        }
      }
    }

    VR::g_openxr->BeginFrame();

    bool fresh = false;
    bool late = false;
    if (chosen >= 0)
    {
      std::vector<XRFrameItem> items;
      {
        std::lock_guard<std::mutex> lock(m_pacer_mutex);
        for (int i = 0; i <= chosen; ++i)
        {
          items.push_back(std::move(m_pending.front()));
          m_pending.pop_front();
        }
      }
      for (XRFrameItem& item : items)
        ReleaseItem(item, !m_no_gpu_wait);
      m_pacer_cv.notify_all();
      current = std::move(items.back());
      have_current = true;
      fresh = true;
      late = current.target + period / 2 < display_time;
    }
    if (waited)
      NoteGpuWait(wait_us, true);

    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    std::vector<XrCompositionLayerBaseHeader*> layers;
    if (have_current)
    {
      if (current.cinematic)
      {
        layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&current.cinematic_layer));
      }
      else if (current.views_valid)
      {
        projection.layerFlags = current.layer_flags;
        projection.space = VR::g_openxr->GetReferenceSpace();
        projection.viewCount = 2;
        projection.views = current.views.data();
        layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&projection));
      }
      for (XrCompositionLayerQuad& quad : current.quads)
        layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad));
    }
    VR::g_openxr->EndFrameDetached(display_time, VR::g_openxr->GetActiveBlendMode(),
                                   should_render && !layers.empty(), layers,
                                   have_current && current.views_valid && !current.cinematic ?
                                       current.target :
                                       0);
    if (SteadyNs() - clock.wake_ns > period)
      late = true;
    NoteLayerEnd(fresh, late);
  }

  ReleaseCompletedItems(true);
  INFO_LOG_FMT(OPENXR, "PrimeFrame pacing thread exiting.");
}

// Video thread, after a game frame was handed over: locate the pose for the refresh the next
// game frame will be shown on. That refresh is the first one the frame can make (now + the time
// the game takes from pose to submit + the GPU tail + 1 ms), kept a steady number of refreshes
// after the previous target (2 for a 60 fps game at 120 Hz) when that is not earlier.
bool VulkanOpenXR::LocateNextFramePose()
{
  const auto clock = VR::g_openxr->GetFrameClock();
  if (clock.display_time == 0 || clock.period_ns <= 0)
    return false;
  // The runtime's slot (two refreshes when SteamVR throttles the app to half the refresh rate).
  const int64_t period = std::max(clock.period_ns, m_slot_ns.load());
  const int64_t now = SteadyNs();
  const double span_ms = m_span_ema_ms > 0.0 ? m_span_ema_ms : 15.0;
  const double interval_ms = m_interval_ema_ms > 0.0 ? m_interval_ema_ms : 16.7;
  const int64_t ready =
      now + static_cast<int64_t>((span_ms + m_gpu_tail_ema_ms.load() + m_pace_margin_ms) * 1e6);
  // The runtime's wake-up for clock.display_time (the last xrWaitFrame may have returned late).
  const int64_t grid_wake =
      clock.grid_offset_ns != 0 ? clock.display_time - clock.grid_offset_ns : clock.wake_ns;
  int64_t slots = (ready - grid_wake + period - 1) / period;
  if (slots < 1)
    slots = 1;
  const XrTime t_ready = clock.display_time + slots * period;
  XrTime target = t_ready;
  if (m_last_target != 0)
  {
    const int64_t step = std::max<int64_t>(1, std::llround(interval_ms * 1e6 / period));
    const XrTime t_lock = m_last_target + step * period;
    if (t_lock < t_ready - period / 2)
    {
      // The next frame cannot make the cadence refresh: it is shown on the first it can make.
      m_slack_frames = 0;
    }
    else if (t_lock <= t_ready + period / 2 || (m_slack_frames < 1000 && ++m_slack_frames <= 30))
    {
      // The cadence refresh (2:2 at 60 fps / 120 Hz). With a refresh or more of slack it is kept
      // for 30 frames before the target moves earlier (one shorter hold, less latency after).
      target = t_lock;
      if (t_lock <= t_ready + period / 2)
        m_slack_frames = 0;
    }
    else
    {
      m_slack_frames = 0;
    }
  }
  m_last_target = target;
  m_target_time = target;
  m_locate_ns = now;
  return VR::g_openxr->LocateViewsAt(target);
}
#else
bool VulkanOpenXR::LocateNextFramePose()
{
  return false;
}
#endif

}  // namespace Vulkan

#endif  // ENABLE_VR
