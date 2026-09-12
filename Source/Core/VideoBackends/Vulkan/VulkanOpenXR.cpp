// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef ENABLE_VR

// VulkanLoader.h must come first — it defines VK_NO_PROTOTYPES before vulkan.h.
#include "VideoBackends/Vulkan/VulkanLoader.h"

#define XR_USE_GRAPHICS_API_VULKAN

#include "VideoBackends/Vulkan/VulkanOpenXR.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Common/Assert.h"
#include "Common/Logging/Log.h"
#include "Common/ScopeGuard.h"
#include "Common/Timer.h"

#include "VideoBackends/Vulkan/CommandBufferManager.h"
#include "VideoBackends/Vulkan/StateTracker.h"
#include "VideoBackends/Vulkan/VKTexture.h"
#include "VideoBackends/Vulkan/VulkanContext.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/TextureConfig.h"
#include "VideoCommon/VR/OpenXRManager.h"
#include "VideoCommon/VR/PrimedGunOverlayCommon.h"
#include "VideoCommon/VideoConfig.h"

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
  result =
      xrEnumerateSwapchainFormats(session, format_count, &format_count, runtime_formats.data());
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
                                                               size_t pixel_count, VkFormat format)
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

template <typename T>
void DestroySwapchainVulkanObjects(T& swapchain)
{
  const VkDevice device = g_vulkan_context->GetDevice();

  // VKFramebuffer/VKTexture normally defer destruction until a command buffer is recycled.
  // OpenXR owns the VkImages, however, and xrDestroySwapchain may destroy them immediately.
  // Destroy our dependent Vulkan objects synchronously before returning ownership to OpenXR.
  for (auto& framebuffer : swapchain.framebuffers)
  {
    if (framebuffer)
    {
      const VkFramebuffer handle = framebuffer->ReleaseHandle();
      if (handle != VK_NULL_HANDLE)
        vkDestroyFramebuffer(device, handle, nullptr);
    }
  }
  swapchain.framebuffers.clear();

  for (auto& texture : swapchain.textures)
  {
    if (texture)
    {
      const VkImageView view = texture->ReleaseView();
      if (view != VK_NULL_HANDLE)
        vkDestroyImageView(device, view, nullptr);
    }
  }
  swapchain.textures.clear();

  for (VkImageView view : swapchain.fdm_views)
    vkDestroyImageView(device, view, nullptr);
  swapchain.fdm_views.clear();
}

static void AppendOptionalOpenXRExtensions(std::vector<const char*>* extensions)
{
  if (VR::OpenXRManager::IsRuntimeExtensionSupported(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME))
  {
    extensions->push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    INFO_LOG_FMT(VIDEO, "OpenXR: Enabling XR_FB_display_refresh_rate.");
  }

  // Fixed foveated rendering: on Vulkan the runtime hands us fragment density map images
  // (XR_FB_foveation_vulkan) that our swapchain render passes read.
  const auto foveation_exts = VR::OpenXRManager::GetAvailableFoveationExtensions(true);
  if (!foveation_exts.empty())
  {
    extensions->insert(extensions->end(), foveation_exts.begin(), foveation_exts.end());
    INFO_LOG_FMT(VIDEO, "OpenXR: Enabling XR_FB_foveation (+configuration, vulkan, "
                        "swapchain_update_state).");
  }
}
}  // namespace

#if defined(ANDROID)
static void AppendOptionalAndroidOpenXRExtensions(std::vector<const char*>* extensions)
{
  if (VR::OpenXRManager::IsRuntimeExtensionSupported(XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME))
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
XRPrimedGunVkOverlaySwapchain&
XRPrimedGunVkOverlaySwapchain::operator=(XRPrimedGunVkOverlaySwapchain&&) noexcept = default;

XRPrimedGunVkLaserSwapchain::XRPrimedGunVkLaserSwapchain() = default;
XRPrimedGunVkLaserSwapchain::~XRPrimedGunVkLaserSwapchain() = default;
XRPrimedGunVkLaserSwapchain::XRPrimedGunVkLaserSwapchain(XRPrimedGunVkLaserSwapchain&&) noexcept =
    default;
XRPrimedGunVkLaserSwapchain&
XRPrimedGunVkLaserSwapchain::operator=(XRPrimedGunVkLaserSwapchain&&) noexcept = default;

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
  result =
      xrEnumerateSwapchainFormats(session, format_count, &format_count, runtime_formats.data());
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
  static constexpr std::array<VkFormat, 6> preferred_formats = {VK_FORMAT_R8G8B8A8_SRGB,
                                                                VK_FORMAT_B8G8R8A8_SRGB,
                                                                VK_FORMAT_R8G8B8A8_UNORM,
                                                                VK_FORMAT_B8G8R8A8_UNORM,
                                                                VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                                                                VK_FORMAT_R16G16B16A16_SFLOAT};
#else
  // Prefer sRGB on PC so the OpenXR compositor decodes Dolphin's gamma-encoded XFB correctly.
  static constexpr std::array<VkFormat, 6> preferred_formats = {VK_FORMAT_R8G8B8A8_SRGB,
                                                                VK_FORMAT_B8G8R8A8_SRGB,
                                                                VK_FORMAT_R8G8B8A8_UNORM,
                                                                VK_FORMAT_B8G8R8A8_UNORM,
                                                                VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                                                                VK_FORMAT_R16G16B16A16_SFLOAT};
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
      INFO_LOG_FMT(VIDEO, "OpenXR Vulkan: waited {} us for pending async final XR submit ({}).",
                   wait_us, reason.empty() ? "frame loop" : reason);
      __android_log_print(
          ANDROID_LOG_INFO, "DolphinXR",
          "OpenXR Vulkan: waited %llu us for pending async final XR submit (%.*s)",
          static_cast<unsigned long long>(wait_us),
          static_cast<int>(reason.empty() ? std::string_view{"frame loop"}.size() : reason.size()),
          reason.empty() ? "frame loop" : reason.data());
      s_async_wait_log_count++;
    }
  }

  if (m_async_frame_finalization_failed.exchange(false, std::memory_order_acq_rel))
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR Vulkan: previous async final XR submit failed.");
    return false;
  }
#endif

  return true;
}

void VulkanOpenXR::FinalizePendingXRFrame(PendingXRFrame frame)
{
  const uint64_t finalize_start_us = Common::Timer::NowUs();
  uint64_t release_total_us = 0;
  uint64_t end_frame_us = 0;
  bool success = true;
  XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};

  // The pacing thread holds this lock from its layer snapshot through xrEndFrame.
  // Release every image and publish its matching pose as one indivisible handoff.
  auto queue_lock = AcquireGraphicsQueueLock();

  const auto release_swapchain = [&](XrSwapchain swapchain, std::string_view name) {
    if (swapchain == XR_NULL_HANDLE)
      return;

    const uint64_t release_start_us = Common::Timer::NowUs();
    const XrResult result = xrReleaseSwapchainImage(swapchain, &release_info);
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

  std::vector<XrCompositionLayerBaseHeader*> layers;
  if (frame.has_projection)
    layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&projection_layer));
  for (auto& quad : frame.quad_layers)
    layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad));

  const uint64_t end_frame_start_us = Common::Timer::NowUs();
  if (!VR::g_openxr)
  {
    success = false;
  }
  else if (frame.publish_to_pacing_thread)
  {
    // Only the pacing thread drives xrWaitFrame/xrBeginFrame/xrEndFrame.
    // Clear content after a failed release rather than pairing it with an old image.
    VR::g_openxr->PublishLayers(success ? layers : std::vector<XrCompositionLayerBaseHeader*>{});
  }
  else if (!VR::g_openxr->EndFrameDetached(frame.display_time, frame.environment_blend_mode,
                                           frame.should_render && success, layers, false))
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
        WARN_LOG_FMT(VIDEO, "OpenXR: Runtime reported max Vulkan 1.0.0 (v1 extension quirk); "
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

  INFO_LOG_FMT(VIDEO, "OpenXR Vulkan: Creating swapchains...");
  if (!CreateSwapchains())
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR Vulkan: Swapchain creation failed — disabling VR.");
    VR::g_openxr.reset();
    return false;
  }

  // Register this object as the swapchain provider so Presenter can acquire eye images.
  VR::g_openxr->SetSwapchain(this);

  INFO_LOG_FMT(VIDEO, "OpenXR Vulkan: Initialization complete.");
  return true;
}

void VulkanOpenXR::Shutdown()
{
  WaitForPendingFrameFinalization("during shutdown");

  // Clear swapchain pointer before destroying swapchains so no dangling use occurs.
  if (VR::g_openxr)
    VR::g_openxr->SetSwapchain(nullptr);

  DestroySwapchains();
  VR::g_openxr.reset();

  // Wait for all GPU work to finish before the Vulkan device is destroyed.
  if (g_vulkan_context)
    vkDeviceWaitIdle(g_vulkan_context->GetDevice());

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
  XrResult result =
      xrGetInstanceProcAddr(xr_instance, "xrGetVulkanGraphicsRequirementsKHR",
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
    result = pfnGetVulkanDevice(xr_instance, xr_system, g_vulkan_context->GetVulkanInstance(),
                                &xr_physical_device);
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
  INFO_LOG_FMT(VIDEO,
               "  VkInstance={}, VkPhysicalDevice={}, VkDevice={}, queueFamily={}, "
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

bool VulkanOpenXR::ShouldUseFoveation() const
{
  // Foveate only the stereo projection path: the flat panel reuses eye swapchain #0,
  // and foveating a world-locked quad would just blur its edges for no gain.
  return g_ActiveConfig.stereo_mode == StereoMode::OpenXR && VR::g_openxr &&
         VR::g_openxr->IsFoveationUsable() && g_vulkan_context->SupportsFragmentDensityMap();
}

// static
bool VulkanOpenXR::PrepareFoveationImages(
    const std::vector<XrSwapchainImageFoveationVulkanFB>& fdm_images,
    std::vector<VkImageView>* out_views)
{
  out_views->clear();
  out_views->reserve(fdm_images.size());

  const auto cleanup = [out_views]() {
    for (VkImageView view : *out_views)
      vkDestroyImageView(g_vulkan_context->GetDevice(), view, nullptr);
    out_views->clear();
  };

  for (const auto& fdm : fdm_images)
  {
    if (fdm.image == VK_NULL_HANDLE)
    {
      WARN_LOG_FMT(VIDEO, "OpenXR: Runtime returned no fragment density map image.");
      cleanup();
      return false;
    }

    VkImageViewCreateInfo view_info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = fdm.image;
    // 2D_ARRAY covers both per-eye (1 layer) and multiview (2 layer) density maps.
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    view_info.format = VK_FORMAT_R8G8_UNORM;
    view_info.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};

    VkImageView view = VK_NULL_HANDLE;
    const VkResult res =
        vkCreateImageView(g_vulkan_context->GetDevice(), &view_info, nullptr, &view);
    if (res != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkCreateImageView (fragment density map) failed: ");
      cleanup();
      return false;
    }
    out_views->push_back(view);
  }

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
        WARN_LOG_FMT(VIDEO, "OpenXR: Layered Vulkan swapchain creation failed; falling back to "
                            "two per-eye swapchains.");
      }
    }
    else
    {
      WARN_LOG_FMT(VIDEO,
                   "OpenXR: Layered Vulkan swapchain disabled because eye sizes differ "
                   "({}x{} vs {}x{}).",
                   view_cfgs[0].recommendedImageRectWidth, view_cfgs[0].recommendedImageRectHeight,
                   view_cfgs[1].recommendedImageRectWidth, view_cfgs[1].recommendedImageRectHeight);
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

bool VulkanOpenXR::CreateLayeredSwapchain(int64_t swapchain_format, bool allow_foveation)
{
  ASSERT(VR::g_openxr != nullptr);

  const auto& view_cfgs = VR::g_openxr->GetViewConfigViews();
  const AbstractTextureFormat abstract_format =
      VkFormatToAbstractFormat(static_cast<VkFormat>(swapchain_format));

  auto& sc = m_layered_swapchain;
  sc.width = view_cfgs[0].recommendedImageRectWidth;
  sc.height = view_cfgs[0].recommendedImageRectHeight;

  auto cleanup = [&sc]() {
    DestroySwapchainVulkanObjects(sc);
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

  // Fixed foveated rendering: ask the runtime to allocate fragment density maps.
  XrSwapchainCreateInfoFoveationFB foveation_info{XR_TYPE_SWAPCHAIN_CREATE_INFO_FOVEATION_FB};
  bool foveated = allow_foveation && ShouldUseFoveation();
  if (foveated)
  {
    foveation_info.flags = XR_SWAPCHAIN_CREATE_FOVEATION_FRAGMENT_DENSITY_MAP_BIT_FB;
    // XrSwapchainCreateInfoFoveationFB::next is (non-const) void*.
    foveation_info.next = const_cast<void*>(info.next);
    info.next = &foveation_info;
  }

  XrResult result = xrCreateSwapchain(VR::g_openxr->GetSession(), &info, &sc.swapchain);
  if (XR_FAILED(result) && foveated)
  {
    WARN_LOG_FMT(VIDEO,
                 "OpenXR: Foveated layered swapchain creation failed ({}); retrying without "
                 "foveation.",
                 static_cast<int>(result));
    info.next = foveation_info.next;
    foveated = false;
    result = xrCreateSwapchain(VR::g_openxr->GetSession(), &info, &sc.swapchain);
  }
  if (XR_FAILED(result))
  {
    WARN_LOG_FMT(VIDEO, "OpenXR: xrCreateSwapchain failed for layered Vulkan swapchain ({}).",
                 static_cast<int>(result));
    cleanup();
    return foveated ? CreateLayeredSwapchain(swapchain_format, false) : false;
  }

  uint32_t image_count = 0;
  result = xrEnumerateSwapchainImages(sc.swapchain, 0, &image_count, nullptr);
  if (XR_FAILED(result) || image_count == 0)
  {
    WARN_LOG_FMT(VIDEO,
                 "OpenXR: xrEnumerateSwapchainImages failed for layered Vulkan swapchain ({}).",
                 static_cast<int>(result));
    cleanup();
    return foveated ? CreateLayeredSwapchain(swapchain_format, false) : false;
  }

  std::vector<XrSwapchainImageVulkanKHR> images(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
  std::vector<XrSwapchainImageFoveationVulkanFB> fdm_images;
  if (foveated)
  {
    fdm_images.resize(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_FOVEATION_VULKAN_FB});
    for (uint32_t i = 0; i < image_count; ++i)
      images[i].next = &fdm_images[i];
  }
  result = xrEnumerateSwapchainImages(sc.swapchain, image_count, &image_count,
                                      reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
  if (XR_FAILED(result))
  {
    WARN_LOG_FMT(VIDEO,
                 "OpenXR: xrEnumerateSwapchainImages data failed for layered Vulkan swapchain "
                 "({}).",
                 static_cast<int>(result));
    cleanup();
    return foveated ? CreateLayeredSwapchain(swapchain_format, false) : false;
  }

  if (foveated && !PrepareFoveationImages(fdm_images, &sc.fdm_views))
  {
    WARN_LOG_FMT(VIDEO, "OpenXR: Retrying layered swapchain without foveation.");
    cleanup();
    return CreateLayeredSwapchain(swapchain_format, false);
  }

  sc.textures.resize(image_count);
  sc.framebuffers.resize(image_count);

  for (uint32_t i = 0; i < image_count; ++i)
  {
    TextureConfig tex_config(sc.width, sc.height, /*levels=*/1, /*layers=*/2, /*samples=*/1,
                             abstract_format, AbstractTextureFlag_RenderTarget,
                             AbstractTextureType::Texture_2DArray);

    sc.textures[i] =
        VKTexture::CreateAdopted(tex_config, images[i].image, VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                                 VK_IMAGE_LAYOUT_UNDEFINED, vk_view_format);
    if (!sc.textures[i])
    {
      WARN_LOG_FMT(VIDEO, "OpenXR: VKTexture::CreateAdopted failed for layered Vulkan image {}.",
                   i);
      cleanup();
      return foveated ? CreateLayeredSwapchain(swapchain_format, false) : false;
    }

    sc.framebuffers[i] = VKFramebuffer::CreateMultiview(
        sc.textures[i].get(), nullptr, {}, foveated ? sc.fdm_views[i] : VK_NULL_HANDLE);
    if (!sc.framebuffers[i])
    {
      WARN_LOG_FMT(VIDEO,
                   "OpenXR: VKFramebuffer::CreateMultiview failed for layered Vulkan image {}.", i);
      cleanup();
      return foveated ? CreateLayeredSwapchain(swapchain_format, false) : false;
    }
  }

  if (foveated)
  {
    if (!VR::g_openxr->ApplyFoveationToSwapchain(sc.swapchain))
    {
      cleanup();
      return CreateLayeredSwapchain(swapchain_format, false);
    }
    for (const auto& fdm : fdm_images)
    {
      // Record transitions only after setup succeeds, so failure cleanup cannot leave
      // command buffers referencing destroyed runtime images.
      VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      barrier.srcAccessMask = 0;
      barrier.dstAccessMask = VK_ACCESS_FRAGMENT_DENSITY_MAP_READ_BIT_EXT;
      barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      barrier.newLayout = VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT;
      barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.image = fdm.image;
      barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
      vkCmdPipelineBarrier(g_command_buffer_mgr->GetCurrentInitCommandBuffer(),
                           VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_DENSITY_PROCESS_BIT_EXT, 0, 0, nullptr, 0,
                           nullptr, 1, &barrier);
    }
    m_foveated = true;
  }

  INFO_LOG_FMT(VIDEO, "OpenXR: Layered Vulkan swapchain ready: {}x{}, {} images, arraySize=2{}.",
               sc.width, sc.height, image_count, foveated ? ", foveated" : "");
  return true;
}

bool VulkanOpenXR::CreateEyeSwapchains(int64_t swapchain_format)
{
  ASSERT(VR::g_openxr != nullptr);

  const auto& view_cfgs = VR::g_openxr->GetViewConfigViews();
  const AbstractTextureFormat abstract_format =
      VkFormatToAbstractFormat(static_cast<VkFormat>(swapchain_format));

  for (uint32_t eye = 0; eye < 2; ++eye)
  {
    auto& sc = m_eye_swapchains[eye];
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
      sc.textures[i] = VKTexture::CreateAdopted(tex_config, images[i].image, VK_IMAGE_VIEW_TYPE_2D,
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

    INFO_LOG_FMT(VIDEO, "OpenXR: Eye {} swapchain ready: {}x{}, {} images.", eye, sc.width,
                 sc.height, image_count);
  }

  return true;
}

void VulkanOpenXR::DestroySwapchains()
{
  // Submit recorded work as well as waiting for the queue before freeing runtime images.
  if (g_gfx)
    g_gfx->WaitForGPUIdle();
  else if (g_command_buffer_mgr)
    g_command_buffer_mgr->SubmitCommandBuffer(false, true);
  else if (g_vulkan_context)
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

  DestroySwapchainVulkanObjects(m_layered_swapchain);
  m_foveated = false;

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

  for (uint32_t eye = 0; eye < 2; ++eye)
  {
    auto& sc = m_eye_swapchains[eye];

    if (m_image_acquired[eye] && sc.swapchain != XR_NULL_HANDLE)
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
                     "OpenXR: xrReleaseSwapchainImage during shutdown failed for eye {} ({}).", eye,
                     static_cast<int>(release_result));
      }
      m_image_acquired[eye] = false;
    }

    // Release Dolphin wrappers before destroying the swapchain so the
    // runtime's VkImages are only freed after our views are gone.
    DestroySwapchainVulkanObjects(sc);

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
    laser.textures[i] =
        VKTexture::CreateAdopted(tex_config, laser.images[i].image, VK_IMAGE_VIEW_TYPE_2D,
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
    overlay->textures[i] =
        VKTexture::CreateAdopted(tex_config, overlay->images[i].image, VK_IMAGE_VIEW_TYPE_2D,
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
  if (!overlay.menu_visible && !overlay.prompt_visible && !overlay.weapon_panel_visible &&
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
      m_primedgun_position_marker_layer.pose.orientation = {-0.70710678f, 0.0f, 0.0f, 0.70710678f};
      m_primedgun_position_marker_layer.pose.position = {
          snapshot.tracking_origin_position[0], 0.005f, snapshot.tracking_origin_position[2]};
      m_primedgun_position_marker_layer.size = {0.356f, 0.356f};
      layers->push_back(
          reinterpret_cast<XrCompositionLayerBaseHeader*>(&m_primedgun_position_marker_layer));
      appended_layer = true;
    }
  }

  if (!overlay.menu_visible && !overlay.prompt_visible && !overlay.weapon_panel_visible)
    return appended_layer;

  const bool menu = overlay.menu_visible;
  const bool weapon_panel = !menu && overlay.weapon_panel_visible;
  const uint32_t content_kind = menu ? 2u : weapon_panel ? 3u : 1u;
  const uint32_t width = menu ? 1024 : weapon_panel ? 512 : 1024;
  const uint32_t height = menu ? 512 : weapon_panel ? 512 : 384;
  const uint32_t generation = menu         ? overlay.generation :
                              weapon_panel ? (100u + overlay.weapon_selected_index) :
                                             1u;
  const std::vector<uint32_t> pixels = menu ? PGO::BuildMenuPixels(width, height, overlay) :
                                       weapon_panel ?
                                              PGO::BuildWeaponPanelPixels(width, height, overlay) :
                                              PGO::BuildPromptPixels(width, height);
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
    m_primedgun_overlay_layer.pose.orientation = {
        overlay.floating_menu_orientation[0], overlay.floating_menu_orientation[1],
        overlay.floating_menu_orientation[2], overlay.floating_menu_orientation[3]};
    m_primedgun_overlay_layer.pose.position = {
        overlay.floating_menu_position[0] + snapshot.tracking_origin_position[0],
        overlay.floating_menu_position[1] + snapshot.tracking_origin_position[1],
        overlay.floating_menu_position[2] + snapshot.tracking_origin_position[2]};
    m_primedgun_overlay_layer.size = {overlay.menu_size[0], overlay.menu_size[1]};
  }
  else if (menu && panel_pose.valid)
  {
    const XrQuaternionf q = panel_pose.orientation;
    m_primedgun_overlay_layer.pose.orientation =
        PGO::MulQuat(q, {-0.70710678f, 0.0f, 0.0f, 0.70710678f});
    const XrVector3f offset =
        PGO::RotateVector(m_primedgun_overlay_layer.pose.orientation, {0.0f, 0.10f, -0.18f});
    m_primedgun_overlay_layer.pose.position = {panel_pose.position.x + offset.x,
                                               panel_pose.position.y + offset.y,
                                               panel_pose.position.z + offset.z};
    m_primedgun_overlay_layer.size = {1.05f, 0.72f};
  }
  else if (weapon_panel)
  {
    m_primedgun_overlay_layer.pose.orientation = {
        overlay.weapon_panel_orientation[0], overlay.weapon_panel_orientation[1],
        overlay.weapon_panel_orientation[2], overlay.weapon_panel_orientation[3]};
    const XrVector3f offset =
        PGO::RotateVector(m_primedgun_overlay_layer.pose.orientation, {0.0f, 0.055f, -0.26f});
    m_primedgun_overlay_layer.pose.position = {
        overlay.weapon_panel_position[0] + snapshot.tracking_origin_position[0] + offset.x,
        overlay.weapon_panel_position[1] + snapshot.tracking_origin_position[1] + offset.y,
        overlay.weapon_panel_position[2] + snapshot.tracking_origin_position[2] + offset.z};
    m_primedgun_overlay_layer.size = {0.42f, 0.42f};
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
      m_primedgun_laser_hit_layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
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
  auto& sc = m_eye_swapchains[eye_index];
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

  if (!WaitForPendingFrameFinalization("before acquiring the next layered image"))
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
  // A timeout does not relinquish the acquired image: releasing it before a successful
  // wait violates the OpenXR call order. In particular, do not alternate between stale
  // layered images and the per-eye fallback when GPU work takes longer than 5 ms.
  wait_info.timeout = XR_INFINITE_DURATION;
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
      WARN_LOG_FMT(VIDEO, "OpenXR: xrReleaseSwapchainImage after layered wait failure failed ({}).",
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

void VulkanOpenXR::ReleaseEyeTexture(uint32_t eye_index)
{
  ASSERT(eye_index < 2);
  if (!m_image_acquired[eye_index])
    return;

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

  if (!WaitForPendingFrameFinalization("before publishing the next XR frame"))
    return false;

  static uint64_t s_frame_id = 0;
  PendingXRFrame frame;
  frame.debug_frame_id = ++s_frame_id;
  frame.queued_time_us = Common::Timer::NowUs();
  frame.publish_to_pacing_thread = VR::g_openxr->IsFrameThreadActive();
  frame.display_time = VR::g_openxr->GetPredictedDisplayTime();
  frame.environment_blend_mode = VR::g_openxr->GetActiveBlendMode();
  frame.should_render = VR::g_openxr->ShouldRender();
  frame.space = VR::g_openxr->GetReferenceSpace();
  frame.layer_flags = VR::g_openxr->GetProjectionLayerExtraFlags();

  const bool submit_layered = m_frame_uses_layered_swapchain && m_use_layered_swapchain &&
                              m_layered_swapchain.swapchain != XR_NULL_HANDLE;
  const auto overlay = Common::VR::OpenXRInputState::GetPrimedGunOverlay();
  std::vector<XrCompositionLayerBaseHeader*> quad_layers;
  if (overlay.cinematic_screen_active && !submit_layered &&
      BuildCinematicScreenLayer(m_eye_swapchains, overlay.cinematic_screen_generation,
                                &m_cinematic_screen_layer))
  {
    quad_layers.push_back(
        reinterpret_cast<XrCompositionLayerBaseHeader*>(&m_cinematic_screen_layer));
  }
  else
  {
    if (!overlay.cinematic_screen_active)
      ResetCinematicScreenAnchor();

    // Use the pose stamped on this XFB, not the pose of the next emulated frame.
    const auto& eye_views = VR::g_openxr->GetPresentEyeViews();
    const auto valid_orientation = [](const XrQuaternionf& q) {
      return q.x != 0.0f || q.y != 0.0f || q.z != 0.0f || q.w != 0.0f;
    };
    frame.has_projection = valid_orientation(eye_views[0].pose.orientation) &&
                           valid_orientation(eye_views[1].pose.orientation);
    for (uint32_t eye = 0; eye < 2; ++eye)
    {
      auto& pv = frame.projection_views[eye];
      pv = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
      pv.pose = eye_views[eye].pose;
      pv.fov = eye_views[eye].fov;
      pv.subImage.swapchain =
          submit_layered ? m_layered_swapchain.swapchain : m_eye_swapchains[eye].swapchain;
      pv.subImage.imageArrayIndex = submit_layered ? eye : 0;
      pv.subImage.imageRect = {
          {0, 0},
          {static_cast<int32_t>(submit_layered ? m_layered_swapchain.width :
                                                 m_eye_swapchains[eye].width),
           static_cast<int32_t>(submit_layered ? m_layered_swapchain.height :
                                                 m_eye_swapchains[eye].height)}};
    }
  }

  // Overlay uploads use the video thread's command buffers. Copy the layer data so
  // the submit worker never reads mutable render-thread composition storage.
  AppendPrimedGunOverlayLayers(&quad_layers);
  for (const auto* layer : quad_layers)
    frame.quad_layers.push_back(*reinterpret_cast<const XrCompositionLayerQuad*>(layer));

  frame.layered_acquired = std::exchange(m_layered_image_acquired, false);
  frame.layered_swapchain = m_layered_swapchain.swapchain;
  bool has_acquired_images = frame.layered_acquired;
  for (uint32_t eye = 0; eye < 2; ++eye)
  {
    frame.eye_acquired[eye] = std::exchange(m_image_acquired[eye], false);
    frame.eye_swapchains[eye] = m_eye_swapchains[eye].swapchain;
    has_acquired_images |= frame.eye_acquired[eye];
  }
  m_frame_uses_layered_swapchain = false;

  // Even an invalid pose must release every acquired image. An empty layer stack
  // clears stale content without taking ownership of the pacing frame protocol.
#if defined(ANDROID)
  const bool advance_to_next_frame =
      g_ActiveConfig.vr_android_direct_to_hmd && VR::g_openxr->IsSessionRunning();
#else
  constexpr bool advance_to_next_frame = false;
#endif
  if (!has_acquired_images && !advance_to_next_frame)
  {
    FinalizePendingXRFrame(std::move(frame));
    return !m_async_frame_finalization_failed.exchange(false, std::memory_order_acq_rel);
  }

  StateTracker::GetInstance()->EndRenderPass();
#if defined(ANDROID)
  // Recycle once per frame: direct mode owns this here; the mirror path already
  // advanced in PresentBackbuffer. Even when XR skips rendering, submit recorded
  // game work in direct mode so command buffers and transient resources can retire.
  // The callback runs after vkQueueSubmit, without waiting for GPU completion.
  m_async_frame_finalization_in_flight.store(true, std::memory_order_release);
  g_command_buffer_mgr->SubmitCommandBuffer(
      true, false, advance_to_next_frame, VK_NULL_HANDLE, 0xFFFFFFFF,
      [this, frame = std::move(frame)]() mutable { FinalizePendingXRFrame(std::move(frame)); });
  StateTracker::GetInstance()->InvalidateCachedState();
  return true;
#else
  const bool wait_for_completion = !g_vulkan_context->SupportsTimelineSemaphores() &&
                                   VR::g_openxr->IsQuestOrVirtualDesktopRuntime();
  g_command_buffer_mgr->SubmitCommandBuffer(false, wait_for_completion);
  StateTracker::GetInstance()->InvalidateCachedState();
  FinalizePendingXRFrame(std::move(frame));
  return !m_async_frame_finalization_failed.exchange(false, std::memory_order_acq_rel);
#endif
}

}  // namespace Vulkan

#endif  // ENABLE_VR
