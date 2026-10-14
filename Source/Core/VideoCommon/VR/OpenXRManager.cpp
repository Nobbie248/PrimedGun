// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef ENABLE_VR

#include "VideoCommon/VR/OpenXRManager.h"
#include "VideoCommon/VR/HeadCullProjection.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>
#include <thread>

#include "Common/Logging/Log.h"
#include "Common/Matrix.h"
#include "Common/Thread.h"
#include "Common/Timer.h"
#include "Common/VR/ControllerPressure.h"
#include "Common/VR/OpenXRInputState.h"
#include "Core/Config/GraphicsSettings.h"
#include "VideoCommon/VideoConfig.h"

namespace VR
{
std::unique_ptr<OpenXRManager> g_openxr;

namespace
{
constexpr float PRIMEDGUN_DEFAULT_STANDING_HEIGHT_M = 1.2f;

struct EulerDeg
{
  float yaw = 0.0f;
  float pitch = 0.0f;
  float roll = 0.0f;
};

static XrQuaternionf MultiplyQuaternions(const XrQuaternionf& a, const XrQuaternionf& b)
{
  return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
          a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
          a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
          a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

static EulerDeg QuaternionToEulerDeg(const XrQuaternionf& q)
{
  // Yaw(Z), pitch(Y), roll(X) for quick diagnostics in logs.
  const float sinr_cosp = 2.0f * (q.w * q.x + q.y * q.z);
  const float cosr_cosp = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
  const float roll = std::atan2(sinr_cosp, cosr_cosp);

  const float sinp = 2.0f * (q.w * q.y - q.z * q.x);
  const float pitch = std::abs(sinp) >= 1.0f ? std::copysign(1.57079633f, sinp) :
                                                std::asin(sinp);

  const float siny_cosp = 2.0f * (q.w * q.z + q.x * q.y);
  const float cosy_cosp = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
  const float yaw = std::atan2(siny_cosp, cosy_cosp);

  constexpr float RAD_TO_DEG = 57.2957795f;
  return {yaw * RAD_TO_DEG, pitch * RAD_TO_DEG, roll * RAD_TO_DEG};
}

static void CopyOpenXRName(char* dst, size_t dst_size, std::string_view src)
{
  std::memset(dst, 0, dst_size);
  const size_t copy_size = std::min(dst_size - 1, src.size());
  std::memcpy(dst, src.data(), copy_size);
}
}  // namespace

// Checks an XrResult and returns false (with an error log) on failure.
// Requires m_instance to be valid for error string lookup.
#define XR_CHECK(expr)                                                                             \
  do                                                                                               \
  {                                                                                                \
    const XrResult _r = (expr);                                                                    \
    if (XR_FAILED(_r))                                                                             \
    {                                                                                              \
      char _buf[XR_MAX_RESULT_STRING_SIZE]{};                                                      \
      xrResultToString(m_instance, _r, _buf);                                                      \
      ERROR_LOG_FMT(OPENXR, "OpenXR: {} failed: {}", #expr, _buf);                                  \
      return false;                                                                                 \
    }                                                                                              \
  } while (false)

OpenXRManager::OpenXRManager() = default;

OpenXRManager::~OpenXRManager()
{
  StopFrameThread();
  DestroyInputActions();
  ResetInputActionsState();

  if (m_reference_space != XR_NULL_HANDLE)
    xrDestroySpace(m_reference_space);

  if (m_session != XR_NULL_HANDLE)
  {
    if (m_session_running)
      xrEndSession(m_session);
    xrDestroySession(m_session);
  }

  if (m_instance != XR_NULL_HANDLE)
    xrDestroyInstance(m_instance);
}

bool OpenXRManager::IsRuntimeExtensionSupported(const char* extension_name)
{
  if (!extension_name || extension_name[0] == '\0')
    return false;

  uint32_t count = 0;
  XrResult result = xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
  if (XR_FAILED(result) || count == 0)
    return false;

  std::vector<XrExtensionProperties> properties(count, {XR_TYPE_EXTENSION_PROPERTIES});
  result = xrEnumerateInstanceExtensionProperties(nullptr, count, &count, properties.data());
  if (XR_FAILED(result))
    return false;

  const std::string_view wanted(extension_name);
  for (const XrExtensionProperties& property : properties)
  {
    if (wanted == property.extensionName)
      return true;
  }

  return false;
}

std::vector<const char*> OpenXRManager::GetAvailableControllerExtensions()
{
  std::vector<const char*> extensions;
  if (IsRuntimeExtensionSupported(XR_META_TOUCH_CONTROLLER_PLUS_EXTENSION_NAME))
    extensions.push_back(XR_META_TOUCH_CONTROLLER_PLUS_EXTENSION_NAME);
  return extensions;
}

bool OpenXRManager::CreateInstance(const std::vector<const char*>& extra_extensions)
{
  m_enabled_extensions.clear();

  // Log available API layers.
  uint32_t layer_count = 0;
  xrEnumerateApiLayerProperties(0, &layer_count, nullptr);
  std::vector<XrApiLayerProperties> layers(layer_count, {XR_TYPE_API_LAYER_PROPERTIES});
  xrEnumerateApiLayerProperties(layer_count, &layer_count, layers.data());
  for (const auto& layer : layers)
    INFO_LOG_FMT(OPENXR, "OpenXR: Available API layer: {}", layer.layerName);

  // Enumerate and verify extensions.
  uint32_t ext_count = 0;
  xrEnumerateInstanceExtensionProperties(nullptr, 0, &ext_count, nullptr);
  std::vector<XrExtensionProperties> exts(ext_count, {XR_TYPE_EXTENSION_PROPERTIES});
  xrEnumerateInstanceExtensionProperties(nullptr, ext_count, &ext_count, exts.data());
  for (const auto& ext : exts)
    INFO_LOG_FMT(OPENXR, "OpenXR: Available extension: {}", ext.extensionName);

  for (const char* required : extra_extensions)
  {
    const bool found =
        std::any_of(exts.begin(), exts.end(), [required](const XrExtensionProperties& e) {
          return std::string_view{e.extensionName} == required;
        });
    if (!found)
    {
      ERROR_LOG_FMT(OPENXR, "OpenXR: Required extension '{}' not available.", required);
      return false;
    }
  }

  XrVersion requested_api_version = XR_CURRENT_API_VERSION;
  INFO_LOG_FMT(OPENXR, "OpenXR: Requesting API version {}.{}.{}.",
               XR_VERSION_MAJOR(requested_api_version), XR_VERSION_MINOR(requested_api_version),
               XR_VERSION_PATCH(requested_api_version));

  XrApplicationInfo app_info{};
  std::strncpy(app_info.applicationName, "PrimedGun", XR_MAX_APPLICATION_NAME_SIZE - 1);
  app_info.applicationVersion = 1;
  std::strncpy(app_info.engineName, "PrimedGun", XR_MAX_ENGINE_NAME_SIZE - 1);
  app_info.engineVersion = 1;
  app_info.apiVersion = requested_api_version;

  XrInstanceCreateInfo create_info{XR_TYPE_INSTANCE_CREATE_INFO};
  create_info.applicationInfo = app_info;
  create_info.enabledExtensionCount = static_cast<uint32_t>(extra_extensions.size());
  create_info.enabledExtensionNames = extra_extensions.data();

  XrResult result = xrCreateInstance(&create_info, &m_instance);
  if (result == XR_ERROR_API_VERSION_UNSUPPORTED && requested_api_version != XR_API_VERSION_1_0)
  {
    WARN_LOG_FMT(OPENXR,
                 "OpenXR: Runtime rejected API version {}.{}.{}; retrying with 1.0.",
                 XR_VERSION_MAJOR(requested_api_version), XR_VERSION_MINOR(requested_api_version),
                 XR_VERSION_PATCH(requested_api_version));
    app_info.apiVersion = XR_API_VERSION_1_0;
    create_info.applicationInfo = app_info;
    result = xrCreateInstance(&create_info, &m_instance);
  }

  if (XR_FAILED(result))
  {
    ERROR_LOG_FMT(OPENXR,
                  "OpenXR: xrCreateInstance failed ({}).",
                  static_cast<int>(result));
    return false;
  }

  m_enabled_extensions.reserve(extra_extensions.size());
  for (const char* extension : extra_extensions)
  {
    if (extension)
      m_enabled_extensions.emplace_back(extension);
  }

  XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
  xrGetInstanceProperties(m_instance, &props);
  m_runtime_name = props.runtimeName;
  INFO_LOG_FMT(OPENXR, "OpenXR: Runtime '{}' version {}.{}.{}", props.runtimeName,
               XR_VERSION_MAJOR(props.runtimeVersion), XR_VERSION_MINOR(props.runtimeVersion),
               XR_VERSION_PATCH(props.runtimeVersion));

  return true;
}

bool OpenXRManager::InitializeSystem()
{
  XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
  system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

  const XrResult result = xrGetSystem(m_instance, &system_info, &m_system_id);
  if (XR_FAILED(result))
  {
    ERROR_LOG_FMT(OPENXR,
                  "OpenXR: xrGetSystem failed ({}). Is a headset connected?",
                  static_cast<int>(result));
    return false;
  }

  XrSystemProperties props{XR_TYPE_SYSTEM_PROPERTIES};
  xrGetSystemProperties(m_instance, m_system_id, &props);
  INFO_LOG_FMT(OPENXR, "OpenXR: System '{}' (vendor {:08x})", props.systemName, props.vendorId);

  return true;
}

bool OpenXRManager::EnumerateViewConfigurations()
{
  uint32_t view_count = 0;
  XrResult result = xrEnumerateViewConfigurationViews(
      m_instance, m_system_id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &view_count, nullptr);

  if (XR_FAILED(result) || view_count != 2)
  {
    ERROR_LOG_FMT(OPENXR,
                  "OpenXR: Failed to enumerate view configs or unexpected count ({}). "
                  "Expected 2 views for stereo.",
                  view_count);
    return false;
  }

  m_view_config_views.fill({XR_TYPE_VIEW_CONFIGURATION_VIEW});
  XR_CHECK(xrEnumerateViewConfigurationViews(m_instance, m_system_id,
                                             XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                             view_count, &view_count, m_view_config_views.data()));

  for (uint32_t i = 0; i < view_count; ++i)
  {
    INFO_LOG_FMT(OPENXR, "OpenXR: Eye {} recommended {}x{} (max {}x{})", i,
                 m_view_config_views[i].recommendedImageRectWidth,
                 m_view_config_views[i].recommendedImageRectHeight,
                 m_view_config_views[i].maxImageRectWidth,
                 m_view_config_views[i].maxImageRectHeight);
  }

  return true;
}

void OpenXRManager::SetSession(XrSession session)
{
  m_session = session;

  if (m_session == XR_NULL_HANDLE)
  {
    DestroyInputActions();
    ResetInputActionsState();
    return;
  }

  if (!InitializeInputActions())
  {
    WARN_LOG_FMT(OPENXR, "OpenXR: Controller input actions unavailable.");
    ResetInputActionsState();
  }
}

void OpenXRManager::SetSwapchain(IOpenXRSwapchain* swapchain)
{
  if (swapchain == nullptr)
  {
    StopFrameThread();
    m_swapchain = nullptr;
    m_xfb_pose_stamps = {};
    m_xfb_pose_stamp_next = 0;
    m_xfb_pose_stamp_serial = 0;
    m_present_eye_views_valid = false;
    m_present_pose = {};
    return;
  }

  m_swapchain = swapchain;
  if (!swapchain->SupportsDetachedFrameLoop())
  {
    INFO_LOG_FMT(OPENXR, "OpenXR: backend requires the inline frame flow.");
  }
  else if (Config::Get(Config::GFX_VR_USE_XR_PACING_THREAD))
  {
    StartFrameThread();
  }
  else
  {
    INFO_LOG_FMT(OPENXR, "OpenXR: asynchronous pacing disabled; using inline frame flow.");
  }
}

void OpenXRManager::StartFrameThread()
{
  if (m_frame_thread.joinable())
    return;

  m_frame_thread_should_exit.store(false, std::memory_order_release);
  m_frame_thread_running.store(true, std::memory_order_release);
  m_frame_thread = std::thread(&OpenXRManager::FrameThreadLoop, this);
  INFO_LOG_FMT(OPENXR, "OpenXR: asynchronous pacing thread started.");
}

void OpenXRManager::StopFrameThread()
{
  if (!m_frame_thread.joinable())
    return;

  m_frame_thread_should_exit.store(true, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(m_publish_mutex);
    m_publish_cv.notify_all();
  }
  m_frame_thread.join();
  m_frame_thread_running.store(false, std::memory_order_release);

  std::lock_guard<std::mutex> lock(m_publish_mutex);
  m_published_frame.layers.clear();
  m_publish_serial = 0;
  INFO_LOG_FMT(OPENXR, "OpenXR: asynchronous pacing thread stopped.");
}

void OpenXRManager::PublishFrame(
    const std::array<XrCompositionLayerProjectionView, 2>& views,
    XrCompositionLayerFlags layer_flags)
{
  XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  projection.layerFlags = layer_flags;
  projection.space = m_reference_space;
  projection.viewCount = static_cast<uint32_t>(views.size());
  projection.views = views.data();
  std::vector<XrCompositionLayerBaseHeader*> layers = {
      reinterpret_cast<XrCompositionLayerBaseHeader*>(&projection)};
  PublishLayers(layers);
}

void OpenXRManager::PublishLayers(
    const std::vector<XrCompositionLayerBaseHeader*>& layers)
{
  PublishedXRFrame frame;
  frame.layers.reserve(layers.size());

  for (const XrCompositionLayerBaseHeader* header : layers)
  {
    if (header == nullptr)
      continue;

    PublishedXRLayer layer;
    if (header->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION)
    {
      const auto* projection =
          reinterpret_cast<const XrCompositionLayerProjection*>(header);
      if (projection->views == nullptr || projection->viewCount < 2)
        continue;

      const auto valid_orientation = [](const XrQuaternionf& q) {
        return q.x != 0.0f || q.y != 0.0f || q.z != 0.0f || q.w != 0.0f;
      };
      if (!valid_orientation(projection->views[0].pose.orientation) ||
          !valid_orientation(projection->views[1].pose.orientation))
      {
        continue;
      }

      layer.kind = PublishedXRLayer::Kind::Projection;
      layer.layer_flags = projection->layerFlags;
      layer.space = projection->space;
      std::copy_n(projection->views, layer.projection_views.size(),
                  layer.projection_views.begin());
    }
    else if (header->type == XR_TYPE_COMPOSITION_LAYER_QUAD)
    {
      layer.kind = PublishedXRLayer::Kind::Quad;
      layer.quad = *reinterpret_cast<const XrCompositionLayerQuad*>(header);
    }
    else
    {
      static std::atomic<bool> s_logged_unsupported_layer{false};
      if (!s_logged_unsupported_layer.exchange(true, std::memory_order_relaxed))
      {
        WARN_LOG_FMT(OPENXR,
                     "OpenXR: asynchronous pacing ignored unsupported layer type {}.",
                     static_cast<int>(header->type));
      }
      continue;
    }

    frame.layers.emplace_back(std::move(layer));
  }

  if (frame.layers.empty())
    return;

  {
    std::lock_guard<std::mutex> lock(m_publish_mutex);
    m_published_frame = std::move(frame);
    ++m_publish_serial;
  }
  m_publish_cv.notify_all();
}

void OpenXRManager::FrameThreadLoop()
{
  Common::SetCurrentThreadName("OpenXR Pacing");

  PublishedXRFrame last_frame;
  bool have_frame = false;
  uint64_t consumed_serial = 0;
  double end_frame_cost_us = 500.0;
  uint64_t previous_cycle_us = 0;

  while (!m_frame_thread_should_exit.load(std::memory_order_acquire))
  {
    const bool eager_heartbeat = g_ActiveConfig.vr_eager_heartbeat;
    const uint64_t cycle_start_us = Common::Timer::NowUs();

    if (!PollEvents())
      break;

    if (!m_session_running.load(std::memory_order_acquire))
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }

    if (!eager_heartbeat)
    {
      std::unique_lock<std::mutex> lock(m_publish_mutex);
      m_publish_cv.wait_for(lock, std::chrono::milliseconds(150), [&] {
        return m_publish_serial != consumed_serial ||
               m_frame_thread_should_exit.load(std::memory_order_acquire);
      });
      if (m_frame_thread_should_exit.load(std::memory_order_acquire))
        break;
    }

    if (!WaitFrame())
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      continue;
    }
    const uint64_t wait_frame_return_us = Common::Timer::NowUs();
    if (!BeginFrame())
      continue;

    const bool should_render = m_should_render_snapshot.load(std::memory_order_acquire);
    const int64_t period_ns =
        m_predicted_display_period_snapshot.load(std::memory_order_acquire);
    const uint64_t content_wait_start_us = Common::Timer::NowUs();
    if (should_render)
    {
      const int64_t since_wait_ns =
          static_cast<int64_t>(content_wait_start_us - wait_frame_return_us) * 1000;
      int64_t budget_ns = period_ns - since_wait_ns -
                          static_cast<int64_t>(end_frame_cost_us * 1000.0) - 1'000'000;
      budget_ns = std::clamp<int64_t>(budget_ns, 0, period_ns);
      if (static_cast<int64_t>(previous_cycle_us) * 1000 >
          period_ns + period_ns / 4)
      {
        budget_ns = 0;
      }
      if (!eager_heartbeat)
        budget_ns = 0;

      std::unique_lock<std::mutex> lock(m_publish_mutex);
      if (budget_ns > 0 && m_publish_serial == consumed_serial)
      {
        m_publish_cv.wait_for(lock, std::chrono::nanoseconds(budget_ns), [&] {
          return m_publish_serial != consumed_serial ||
                 m_frame_thread_should_exit.load(std::memory_order_acquire);
        });
      }
      if (m_publish_serial != consumed_serial)
      {
        last_frame = m_published_frame;
        consumed_serial = m_publish_serial;
        have_frame = true;
      }
    }

    for (int spins = 0;
         m_video_handoff_active.load(std::memory_order_acquire) > 0 && spins < 40;
         ++spins)
    {
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }

    {
      std::lock_guard<std::mutex> lock(m_publish_mutex);
      if (m_publish_serial != consumed_serial)
      {
        last_frame = m_published_frame;
        consumed_serial = m_publish_serial;
        have_frame = true;
      }
    }

    std::vector<XrCompositionLayerProjection> projections;
    std::vector<XrCompositionLayerQuad> quads;
    std::vector<XrCompositionLayerBaseHeader*> submitted_layers;
    projections.reserve(last_frame.layers.size());
    quads.reserve(last_frame.layers.size());
    submitted_layers.reserve(last_frame.layers.size());

    const bool submit_content = should_render && have_frame;
    if (submit_content)
    {
      for (PublishedXRLayer& published_layer : last_frame.layers)
      {
        if (published_layer.kind == PublishedXRLayer::Kind::Projection)
        {
          XrCompositionLayerProjection projection{
              XR_TYPE_COMPOSITION_LAYER_PROJECTION};
          projection.layerFlags = published_layer.layer_flags;
          projection.space = published_layer.space;
          projection.viewCount =
              static_cast<uint32_t>(published_layer.projection_views.size());
          projection.views = published_layer.projection_views.data();
          projections.emplace_back(projection);
          submitted_layers.push_back(
              reinterpret_cast<XrCompositionLayerBaseHeader*>(&projections.back()));
        }
        else
        {
          quads.emplace_back(published_layer.quad);
          submitted_layers.push_back(
              reinterpret_cast<XrCompositionLayerBaseHeader*>(&quads.back()));
        }
      }
    }

    const uint64_t end_start_us = Common::Timer::NowUs();
    EndFrameDetached(m_frame_state.predictedDisplayTime, GetActiveBlendMode(),
                     submit_content, submitted_layers);
    const uint64_t end_us = Common::Timer::NowUs() - end_start_us;
    end_frame_cost_us = end_frame_cost_us * 0.8 + static_cast<double>(end_us) * 0.2;
    previous_cycle_us = Common::Timer::NowUs() - cycle_start_us;
  }

  m_frame_thread_running.store(false, std::memory_order_release);
  INFO_LOG_FMT(OPENXR, "OpenXR: asynchronous pacing thread exiting.");
}

bool OpenXRManager::InitializeInputActions()
{
  if (m_input_action_set != XR_NULL_HANDLE)
    return true;

  if (m_instance == XR_NULL_HANDLE || m_session == XR_NULL_HANDLE)
    return false;

  auto to_path = [this](const char* path) -> XrPath {
    XrPath xr_path = XR_NULL_PATH;
    if (XR_FAILED(xrStringToPath(m_instance, path, &xr_path)))
      return XR_NULL_PATH;
    return xr_path;
  };

  m_input_hand_paths[0] = to_path("/user/hand/left");
  m_input_hand_paths[1] = to_path("/user/hand/right");
  if (m_input_hand_paths[0] == XR_NULL_PATH || m_input_hand_paths[1] == XR_NULL_PATH)
  {
    ERROR_LOG_FMT(OPENXR, "OpenXR: Failed to create hand subaction paths.");
    return false;
  }

  XrActionSetCreateInfo action_set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
  CopyOpenXRName(action_set_info.actionSetName, XR_MAX_ACTION_SET_NAME_SIZE, "dolphin_input");
  CopyOpenXRName(action_set_info.localizedActionSetName, XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE,
                 "Dolphin Input");
  action_set_info.priority = 0;

  XrResult result = xrCreateActionSet(m_instance, &action_set_info, &m_input_action_set);
  if (XR_FAILED(result))
  {
    ERROR_LOG_FMT(OPENXR, "OpenXR: xrCreateActionSet failed ({}).", static_cast<int>(result));
    return false;
  }

  const auto create_action = [this](XrAction* action, const char* name, const char* localized_name,
                                    XrActionType type) -> bool {
    XrActionCreateInfo action_info{XR_TYPE_ACTION_CREATE_INFO};
    CopyOpenXRName(action_info.actionName, XR_MAX_ACTION_NAME_SIZE, name);
    CopyOpenXRName(action_info.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE,
                   localized_name);
    action_info.actionType = type;
    action_info.countSubactionPaths = static_cast<uint32_t>(m_input_hand_paths.size());
    action_info.subactionPaths = m_input_hand_paths.data();

    const XrResult create_result = xrCreateAction(m_input_action_set, &action_info, action);
    if (XR_FAILED(create_result))
    {
      ERROR_LOG_FMT(OPENXR, "OpenXR: xrCreateAction('{}') failed ({}).", name,
                    static_cast<int>(create_result));
      return false;
    }
    return true;
  };

  if (!create_action(&m_action_primary_click, "primary_click", "Primary Button",
                     XR_ACTION_TYPE_BOOLEAN_INPUT) ||
      !create_action(&m_action_secondary_click, "secondary_click", "Secondary Button",
                     XR_ACTION_TYPE_BOOLEAN_INPUT) ||
      !create_action(&m_action_menu_click, "menu_click", "Menu Button",
                     XR_ACTION_TYPE_BOOLEAN_INPUT) ||
      !create_action(&m_action_thumbstick_click, "thumbstick_click", "Thumbstick Click",
                     XR_ACTION_TYPE_BOOLEAN_INPUT) ||
      !create_action(&m_action_thumbrest_touch, "thumbrest_touch", "Thumb Rest Touch",
                     XR_ACTION_TYPE_BOOLEAN_INPUT) ||
      !create_action(&m_action_trigger_click, "trigger_click", "Trigger Click",
                     XR_ACTION_TYPE_BOOLEAN_INPUT) ||
      !create_action(&m_action_squeeze_click, "squeeze_click", "Squeeze Click",
                     XR_ACTION_TYPE_BOOLEAN_INPUT) ||
      !create_action(&m_action_trigger_value, "trigger_value", "Trigger Value",
                     XR_ACTION_TYPE_FLOAT_INPUT) ||
      !create_action(&m_action_squeeze_value, "squeeze_value", "Squeeze Value",
                     XR_ACTION_TYPE_FLOAT_INPUT) ||
      !create_action(&m_action_squeeze_force, "squeeze_force", "Squeeze Force",
                     XR_ACTION_TYPE_FLOAT_INPUT) ||
      !create_action(&m_action_thumbstick_x, "thumbstick_x", "Thumbstick X",
                     XR_ACTION_TYPE_FLOAT_INPUT) ||
      !create_action(&m_action_thumbstick_y, "thumbstick_y", "Thumbstick Y",
                     XR_ACTION_TYPE_FLOAT_INPUT) ||
      !create_action(&m_action_trackpad_click, "trackpad_click", "Trackpad Click",
                     XR_ACTION_TYPE_BOOLEAN_INPUT) ||
      !create_action(&m_action_trackpad_touch, "trackpad_touch", "Trackpad Touch",
                     XR_ACTION_TYPE_BOOLEAN_INPUT) ||
      !create_action(&m_action_trackpad_x, "trackpad_x", "Trackpad X",
                     XR_ACTION_TYPE_FLOAT_INPUT) ||
      !create_action(&m_action_trackpad_y, "trackpad_y", "Trackpad Y",
                     XR_ACTION_TYPE_FLOAT_INPUT) ||
      !create_action(&m_action_trackpad_force, "trackpad_force", "Trackpad Force",
                     XR_ACTION_TYPE_FLOAT_INPUT) ||
      !create_action(&m_action_aim_pose, "aim_pose", "Aim Pose", XR_ACTION_TYPE_POSE_INPUT) ||
      !create_action(&m_action_grip_pose, "grip_pose", "Grip Pose", XR_ACTION_TYPE_POSE_INPUT) ||
      !create_action(&m_action_haptic, "haptic", "Haptic Output",
                     XR_ACTION_TYPE_VIBRATION_OUTPUT))
  {
    DestroyInputActions();
    return false;
  }

  struct BindingDef
  {
    XrAction action = XR_NULL_HANDLE;
    const char* path = nullptr;
  };

  const auto suggest_bindings = [this, &to_path](const char* profile,
                                                 std::initializer_list<BindingDef> defs) {
    const XrPath profile_path = to_path(profile);
    if (profile_path == XR_NULL_PATH)
      return;

    std::vector<XrActionSuggestedBinding> bindings;
    bindings.reserve(defs.size());
    for (const auto& def : defs)
    {
      if (def.action == XR_NULL_HANDLE || def.path == nullptr)
        continue;
      const XrPath binding_path = to_path(def.path);
      if (binding_path == XR_NULL_PATH)
        continue;
      bindings.push_back({def.action, binding_path});
    }

    if (bindings.empty())
      return;

    XrInteractionProfileSuggestedBinding suggested{
        XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggested.interactionProfile = profile_path;
    suggested.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
    suggested.suggestedBindings = bindings.data();
    const XrResult suggest_result = xrSuggestInteractionProfileBindings(m_instance, &suggested);
    if (XR_FAILED(suggest_result))
    {
      WARN_LOG_FMT(OPENXR, "OpenXR: xrSuggestInteractionProfileBindings('{}') failed ({}).", profile,
                   static_cast<int>(suggest_result));
    }
  };

  suggest_bindings("/interaction_profiles/khr/simple_controller",
                   {
                       {m_action_primary_click, "/user/hand/left/input/select/click"},
                       {m_action_primary_click, "/user/hand/right/input/select/click"},
                       {m_action_menu_click, "/user/hand/left/input/menu/click"},
                       {m_action_menu_click, "/user/hand/right/input/menu/click"},
                       {m_action_aim_pose, "/user/hand/left/input/aim/pose"},
                       {m_action_aim_pose, "/user/hand/right/input/aim/pose"},
                       {m_action_grip_pose, "/user/hand/left/input/grip/pose"},
                       {m_action_grip_pose, "/user/hand/right/input/grip/pose"},
                       {m_action_haptic, "/user/hand/left/output/haptic"},
                       {m_action_haptic, "/user/hand/right/output/haptic"},
                   });

  suggest_bindings("/interaction_profiles/oculus/touch_controller",
                   {
                       {m_action_primary_click, "/user/hand/left/input/x/click"},
                       {m_action_secondary_click, "/user/hand/left/input/y/click"},
                       {m_action_menu_click, "/user/hand/left/input/menu/click"},
                       {m_action_thumbstick_click, "/user/hand/left/input/thumbstick/click"},
                       {m_action_thumbstick_x, "/user/hand/left/input/thumbstick/x"},
                       {m_action_thumbstick_y, "/user/hand/left/input/thumbstick/y"},
                       {m_action_trigger_value, "/user/hand/left/input/trigger/value"},
                       {m_action_squeeze_value, "/user/hand/left/input/squeeze/value"},
                       {m_action_aim_pose, "/user/hand/left/input/aim/pose"},
                       {m_action_grip_pose, "/user/hand/left/input/grip/pose"},
                       {m_action_primary_click, "/user/hand/right/input/a/click"},
                       {m_action_secondary_click, "/user/hand/right/input/b/click"},
                       {m_action_thumbstick_click, "/user/hand/right/input/thumbstick/click"},
                       {m_action_thumbstick_x, "/user/hand/right/input/thumbstick/x"},
                       {m_action_thumbstick_y, "/user/hand/right/input/thumbstick/y"},
                       {m_action_trigger_value, "/user/hand/right/input/trigger/value"},
                       {m_action_squeeze_value, "/user/hand/right/input/squeeze/value"},
                       {m_action_aim_pose, "/user/hand/right/input/aim/pose"},
                       {m_action_grip_pose, "/user/hand/right/input/grip/pose"},
                       {m_action_haptic, "/user/hand/left/output/haptic"},
                       {m_action_haptic, "/user/hand/right/output/haptic"},
                   });

  const auto suggest_touch_plus_bindings = [&](const char* profile) {
    suggest_bindings(profile,
                     {
                         {m_action_primary_click, "/user/hand/left/input/x/click"},
                         {m_action_secondary_click, "/user/hand/left/input/y/click"},
                         {m_action_menu_click, "/user/hand/left/input/menu/click"},
                         {m_action_thumbstick_click, "/user/hand/left/input/thumbstick/click"},
                         {m_action_thumbrest_touch, "/user/hand/left/input/thumbrest/touch"},
                         {m_action_thumbstick_x, "/user/hand/left/input/thumbstick/x"},
                         {m_action_thumbstick_y, "/user/hand/left/input/thumbstick/y"},
                         {m_action_trigger_value, "/user/hand/left/input/trigger/value"},
                         {m_action_squeeze_value, "/user/hand/left/input/squeeze/value"},
                         {m_action_aim_pose, "/user/hand/left/input/aim/pose"},
                         {m_action_grip_pose, "/user/hand/left/input/grip/pose"},
                         {m_action_primary_click, "/user/hand/right/input/a/click"},
                         {m_action_secondary_click, "/user/hand/right/input/b/click"},
                         {m_action_thumbstick_click, "/user/hand/right/input/thumbstick/click"},
                         {m_action_thumbrest_touch, "/user/hand/right/input/thumbrest/touch"},
                         {m_action_thumbstick_x, "/user/hand/right/input/thumbstick/x"},
                         {m_action_thumbstick_y, "/user/hand/right/input/thumbstick/y"},
                         {m_action_trigger_value, "/user/hand/right/input/trigger/value"},
                         {m_action_squeeze_value, "/user/hand/right/input/squeeze/value"},
                         {m_action_aim_pose, "/user/hand/right/input/aim/pose"},
                         {m_action_grip_pose, "/user/hand/right/input/grip/pose"},
                         {m_action_haptic, "/user/hand/left/output/haptic"},
                         {m_action_haptic, "/user/hand/right/output/haptic"},
                     });
  };
  suggest_touch_plus_bindings("/interaction_profiles/meta/touch_controller_plus");

  suggest_bindings("/interaction_profiles/valve/index_controller",
                   {
                       {m_action_primary_click, "/user/hand/left/input/a/click"},
                       {m_action_secondary_click, "/user/hand/left/input/b/click"},
                       {m_action_thumbstick_click, "/user/hand/left/input/thumbstick/click"},
                       {m_action_thumbstick_x, "/user/hand/left/input/thumbstick/x"},
                       {m_action_thumbstick_y, "/user/hand/left/input/thumbstick/y"},
                       {m_action_trigger_value, "/user/hand/left/input/trigger/value"},
                       {m_action_squeeze_value, "/user/hand/left/input/squeeze/value"},
                       {m_action_trackpad_touch, "/user/hand/left/input/trackpad/touch"},
                       {m_action_squeeze_force, "/user/hand/left/input/squeeze/force"},
                       {m_action_trackpad_x, "/user/hand/left/input/trackpad/x"},
                       {m_action_trackpad_y, "/user/hand/left/input/trackpad/y"},
                       {m_action_trackpad_force, "/user/hand/left/input/trackpad/force"},
                       {m_action_aim_pose, "/user/hand/left/input/aim/pose"},
                       {m_action_grip_pose, "/user/hand/left/input/grip/pose"},
                       {m_action_primary_click, "/user/hand/right/input/a/click"},
                       {m_action_secondary_click, "/user/hand/right/input/b/click"},
                       {m_action_thumbstick_click, "/user/hand/right/input/thumbstick/click"},
                       {m_action_thumbstick_x, "/user/hand/right/input/thumbstick/x"},
                       {m_action_thumbstick_y, "/user/hand/right/input/thumbstick/y"},
                       {m_action_trigger_value, "/user/hand/right/input/trigger/value"},
                       {m_action_squeeze_value, "/user/hand/right/input/squeeze/value"},
                       {m_action_trackpad_touch, "/user/hand/right/input/trackpad/touch"},
                       {m_action_squeeze_force, "/user/hand/right/input/squeeze/force"},
                       {m_action_trackpad_x, "/user/hand/right/input/trackpad/x"},
                       {m_action_trackpad_y, "/user/hand/right/input/trackpad/y"},
                       {m_action_trackpad_force, "/user/hand/right/input/trackpad/force"},
                       {m_action_aim_pose, "/user/hand/right/input/aim/pose"},
                       {m_action_grip_pose, "/user/hand/right/input/grip/pose"},
                       {m_action_haptic, "/user/hand/left/output/haptic"},
                       {m_action_haptic, "/user/hand/right/output/haptic"},
                   });

  suggest_bindings("/interaction_profiles/microsoft/motion_controller",
                   {
                       {m_action_menu_click, "/user/hand/left/input/menu/click"},
                       {m_action_thumbstick_click, "/user/hand/left/input/thumbstick/click"},
                       {m_action_thumbstick_x, "/user/hand/left/input/thumbstick/x"},
                       {m_action_thumbstick_y, "/user/hand/left/input/thumbstick/y"},
                       {m_action_trigger_value, "/user/hand/left/input/trigger/value"},
                       {m_action_squeeze_click, "/user/hand/left/input/squeeze/click"},
                       {m_action_aim_pose, "/user/hand/left/input/aim/pose"},
                       {m_action_grip_pose, "/user/hand/left/input/grip/pose"},
                       {m_action_menu_click, "/user/hand/right/input/menu/click"},
                       {m_action_thumbstick_click, "/user/hand/right/input/thumbstick/click"},
                       {m_action_thumbstick_x, "/user/hand/right/input/thumbstick/x"},
                       {m_action_thumbstick_y, "/user/hand/right/input/thumbstick/y"},
                       {m_action_trigger_value, "/user/hand/right/input/trigger/value"},
                       {m_action_squeeze_click, "/user/hand/right/input/squeeze/click"},
                       {m_action_aim_pose, "/user/hand/right/input/aim/pose"},
                       {m_action_grip_pose, "/user/hand/right/input/grip/pose"},
                       {m_action_haptic, "/user/hand/left/output/haptic"},
                       {m_action_haptic, "/user/hand/right/output/haptic"},
                   });

  suggest_bindings("/interaction_profiles/htc/vive_controller",
                   {
                       {m_action_menu_click, "/user/hand/left/input/menu/click"},
                       {m_action_trackpad_click, "/user/hand/left/input/trackpad/click"},
                       {m_action_trackpad_touch, "/user/hand/left/input/trackpad/touch"},
                       {m_action_trackpad_x, "/user/hand/left/input/trackpad/x"},
                       {m_action_trackpad_y, "/user/hand/left/input/trackpad/y"},
                       {m_action_trigger_value, "/user/hand/left/input/trigger/value"},
                       {m_action_squeeze_click, "/user/hand/left/input/squeeze/click"},
                       {m_action_aim_pose, "/user/hand/left/input/aim/pose"},
                       {m_action_grip_pose, "/user/hand/left/input/grip/pose"},
                       {m_action_menu_click, "/user/hand/right/input/menu/click"},
                       {m_action_trackpad_click, "/user/hand/right/input/trackpad/click"},
                       {m_action_trackpad_touch, "/user/hand/right/input/trackpad/touch"},
                       {m_action_trackpad_x, "/user/hand/right/input/trackpad/x"},
                       {m_action_trackpad_y, "/user/hand/right/input/trackpad/y"},
                       {m_action_trigger_value, "/user/hand/right/input/trigger/value"},
                       {m_action_squeeze_click, "/user/hand/right/input/squeeze/click"},
                       {m_action_aim_pose, "/user/hand/right/input/aim/pose"},
                       {m_action_grip_pose, "/user/hand/right/input/grip/pose"},
                       {m_action_haptic, "/user/hand/left/output/haptic"},
                       {m_action_haptic, "/user/hand/right/output/haptic"},
                   });

  XrSessionActionSetsAttachInfo attach_info{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
  const XrActionSet action_set = m_input_action_set;
  attach_info.countActionSets = 1;
  attach_info.actionSets = &action_set;
  result = xrAttachSessionActionSets(m_session, &attach_info);
  if (XR_FAILED(result))
  {
    ERROR_LOG_FMT(OPENXR, "OpenXR: xrAttachSessionActionSets failed ({}).", static_cast<int>(result));
    DestroyInputActions();
    return false;
  }

  auto create_action_space = [this](XrAction action, XrPath subaction_path, XrSpace* out_space,
                                    const char* label) {
    if (action == XR_NULL_HANDLE || subaction_path == XR_NULL_PATH)
      return;

    XrActionSpaceCreateInfo space_info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    space_info.action = action;
    space_info.subactionPath = subaction_path;
    space_info.poseInActionSpace = {{0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 0.f}};
    const XrResult space_result = xrCreateActionSpace(m_session, &space_info, out_space);
    if (XR_FAILED(space_result))
    {
      WARN_LOG_FMT(OPENXR, "OpenXR: xrCreateActionSpace('{}') failed ({}).", label,
                   static_cast<int>(space_result));
    }
  };

  for (size_t hand = 0; hand < m_input_hand_paths.size(); ++hand)
  {
    create_action_space(m_action_aim_pose, m_input_hand_paths[hand], &m_aim_spaces[hand], "aim");
    create_action_space(m_action_grip_pose, m_input_hand_paths[hand], &m_grip_spaces[hand],
                        "grip");
  }

  return true;
}

void OpenXRManager::DestroyInputActions()
{
  for (auto& space : m_aim_spaces)
  {
    if (space != XR_NULL_HANDLE)
      xrDestroySpace(space);
    space = XR_NULL_HANDLE;
  }
  for (auto& space : m_grip_spaces)
  {
    if (space != XR_NULL_HANDLE)
      xrDestroySpace(space);
    space = XR_NULL_HANDLE;
  }

  if (m_input_action_set != XR_NULL_HANDLE)
  {
    xrDestroyActionSet(m_input_action_set);
    m_input_action_set = XR_NULL_HANDLE;
  }

  m_input_hand_paths = {XR_NULL_PATH, XR_NULL_PATH};
  m_action_primary_click = XR_NULL_HANDLE;
  m_action_secondary_click = XR_NULL_HANDLE;
  m_action_menu_click = XR_NULL_HANDLE;
  m_action_thumbstick_click = XR_NULL_HANDLE;
  m_action_thumbrest_touch = XR_NULL_HANDLE;
  m_action_trigger_click = XR_NULL_HANDLE;
  m_action_squeeze_click = XR_NULL_HANDLE;
  m_action_trigger_value = XR_NULL_HANDLE;
  m_action_squeeze_value = XR_NULL_HANDLE;
  m_action_squeeze_force = XR_NULL_HANDLE;
  m_index_grip_pressed = {};
  m_index_trackpad_pressed = {};
  m_action_thumbstick_x = XR_NULL_HANDLE;
  m_action_thumbstick_y = XR_NULL_HANDLE;
  m_action_trackpad_click = XR_NULL_HANDLE;
  m_action_trackpad_touch = XR_NULL_HANDLE;
  m_action_trackpad_x = XR_NULL_HANDLE;
  m_action_trackpad_y = XR_NULL_HANDLE;
  m_action_trackpad_force = XR_NULL_HANDLE;
  m_action_aim_pose = XR_NULL_HANDLE;
  m_action_grip_pose = XR_NULL_HANDLE;
  m_action_haptic = XR_NULL_HANDLE;
  m_haptics_active = {false, false};
}

void OpenXRManager::ResetInputActionsState()
{
  m_index_grip_pressed = {};
  m_index_trackpad_pressed = {};
  m_haptics_active = {false, false};
  Common::VR::OpenXRInputState::Reset();
}

void OpenXRManager::UpdateHaptics()
{
  if (!m_session_running || m_session == XR_NULL_HANDLE || m_action_haptic == XR_NULL_HANDLE)
    return;

  const auto haptics = Common::VR::OpenXRInputState::GetHaptics();

  // Re-send short pulses while active to approximate continuous rumble.
  constexpr XrDuration vibration_duration_ns = 15'000'000;
  constexpr int pulse_cycle_ms = 100;
  const auto now = std::chrono::steady_clock::now();
  const auto cycle_time_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() %
      pulse_cycle_ms;
  const float duty = std::clamp(haptics.intensity, 0.0f, 1.0f);

  for (size_t hand = 0; hand < m_input_hand_paths.size(); ++hand)
  {
    const XrPath hand_path = m_input_hand_paths[hand];
    if (hand_path == XR_NULL_PATH)
      continue;

    const float amplitude = std::clamp(haptics.amplitude[hand], 0.0f, 1.0f);
    const bool pulse_on =
        duty >= 0.99f || cycle_time_ms < static_cast<int>(pulse_cycle_ms * duty);

    XrHapticActionInfo action_info{XR_TYPE_HAPTIC_ACTION_INFO};
    action_info.action = m_action_haptic;
    action_info.subactionPath = hand_path;

    if (amplitude > 0.001f && pulse_on)
    {
      XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
      vibration.amplitude = amplitude;
      vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
      vibration.duration = vibration_duration_ns;

      xrApplyHapticFeedback(
          m_session, &action_info, reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
      m_haptics_active[hand] = true;
    }
    else if (m_haptics_active[hand])
    {
      xrStopHapticFeedback(m_session, &action_info);
      m_haptics_active[hand] = false;
    }
  }
}

void OpenXRManager::UpdateInputActions()
{
  if (!m_session_running || m_session == XR_NULL_HANDLE || m_input_action_set == XR_NULL_HANDLE)
  {
    ResetInputActionsState();
    return;
  }

  const XrActiveActionSet active_action_set{m_input_action_set, XR_NULL_PATH};
  XrActionsSyncInfo sync_info{XR_TYPE_ACTIONS_SYNC_INFO};
  sync_info.countActiveActionSets = 1;
  sync_info.activeActionSets = &active_action_set;
  const XrResult sync_result = xrSyncActions(m_session, &sync_info);
  if (sync_result != XR_SUCCESS)
  {
    ResetInputActionsState();
    return;
  }

  std::array<Common::VR::OpenXRControllerState, 2> controllers{};
  std::array<std::string, 2> interaction_profiles{};
  const auto overlay = Common::VR::OpenXRInputState::GetPrimedGunOverlay();

  const auto path_to_string = [this](XrPath path) {
    if (path == XR_NULL_PATH)
      return std::string{};

    uint32_t count = 0;
    const XrResult count_result = xrPathToString(m_instance, path, 0, &count, nullptr);
    if (XR_FAILED(count_result) || count == 0)
      return std::string{};

    std::string result(count, '\0');
    const XrResult string_result =
        xrPathToString(m_instance, path, count, &count, result.data());
    if (XR_FAILED(string_result) || count == 0)
      return std::string{};

    if (!result.empty() && result.back() == '\0')
      result.pop_back();
    return result;
  };

  const auto locate_space_state = [this](XrSpace space, Common::VR::OpenXRPoseState* pose_state,
                                         Common::VR::OpenXRVelocityState* velocity_state) {
    if (space == XR_NULL_HANDLE || m_reference_space == XR_NULL_HANDLE)
      return;

    XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    location.next = &velocity;

    if (XR_FAILED(
            xrLocateSpace(space, m_reference_space,
                          m_frame_state.predictedDisplayTime + m_frame_extra_ns, &location)))
    {
      return;
    }

    constexpr XrSpaceLocationFlags required_flags = XR_SPACE_LOCATION_POSITION_VALID_BIT |
                                                    XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    pose_state->valid = (location.locationFlags & required_flags) == required_flags;
    if (pose_state->valid)
    {
      pose_state->position = {location.pose.position.x, location.pose.position.y,
                              location.pose.position.z};
      pose_state->orientation = {location.pose.orientation.x, location.pose.orientation.y,
                                 location.pose.orientation.z, location.pose.orientation.w};
    }

    if (velocity_state)
    {
      velocity_state->linear_valid =
          (velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0;
      if (velocity_state->linear_valid)
      {
        velocity_state->linear = {velocity.linearVelocity.x, velocity.linearVelocity.y,
                                  velocity.linearVelocity.z};
      }

      velocity_state->angular_valid =
          (velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) != 0;
      if (velocity_state->angular_valid)
      {
        velocity_state->angular = {velocity.angularVelocity.x, velocity.angularVelocity.y,
                                   velocity.angularVelocity.z};
      }
    }
  };

  for (size_t hand = 0; hand < controllers.size(); ++hand)
  {
    const XrPath hand_path = m_input_hand_paths[hand];
    auto& controller = controllers[hand];
    XrInteractionProfileState profile_state{XR_TYPE_INTERACTION_PROFILE_STATE};
    if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(m_session, hand_path, &profile_state)))
      interaction_profiles[hand] = path_to_string(profile_state.interactionProfile);

    bool action_seen = false;
    const auto get_boolean = [this, hand_path, &action_seen](XrAction action) -> bool {
      XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
      get_info.action = action;
      get_info.subactionPath = hand_path;
      XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
      if (XR_FAILED(xrGetActionStateBoolean(m_session, &get_info, &state)))
        return false;
      action_seen |= (state.isActive == XR_TRUE);
      return state.isActive == XR_TRUE && state.currentState == XR_TRUE;
    };

    const auto get_float = [this, hand_path, &action_seen](XrAction action) -> float {
      XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
      get_info.action = action;
      get_info.subactionPath = hand_path;
      XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
      if (XR_FAILED(xrGetActionStateFloat(m_session, &get_info, &state)))
        return 0.0f;
      action_seen |= (state.isActive == XR_TRUE);
      return state.isActive == XR_TRUE && std::isfinite(state.currentState) ?
                 state.currentState : 0.0f;
    };

    controller.primary_button = get_boolean(m_action_primary_click);
    controller.secondary_button = get_boolean(m_action_secondary_click);
    controller.menu_button = get_boolean(m_action_menu_click);
    controller.thumbstick_button = get_boolean(m_action_thumbstick_click);
    controller.thumbrest_touch = get_boolean(m_action_thumbrest_touch);
    const bool trackpad_click = get_boolean(m_action_trackpad_click);
    controller.trackpad_touch = get_boolean(m_action_trackpad_touch);

    const bool trigger_click = get_boolean(m_action_trigger_click);
    const bool squeeze_click = get_boolean(m_action_squeeze_click);

    controller.trigger_value = std::clamp(get_float(m_action_trigger_value), 0.0f, 1.0f);
    controller.squeeze_value = std::clamp(get_float(m_action_squeeze_value), 0.0f, 1.0f);
    controller.squeeze_force = std::clamp(get_float(m_action_squeeze_force), 0.0f, 1.0f);
    controller.thumbstick_x = std::clamp(get_float(m_action_thumbstick_x), -1.0f, 1.0f);
    controller.thumbstick_y = std::clamp(get_float(m_action_thumbstick_y), -1.0f, 1.0f);
    controller.trackpad_x = std::clamp(get_float(m_action_trackpad_x), -1.0f, 1.0f);
    controller.trackpad_y = std::clamp(get_float(m_action_trackpad_y), -1.0f, 1.0f);
    controller.trackpad_force = std::clamp(get_float(m_action_trackpad_force), 0.0f, 1.0f);
    controller.trackpad_button = trackpad_click || controller.trackpad_force > 0.5f;

    locate_space_state(m_aim_spaces[hand], &controller.aim_pose, nullptr);
    locate_space_state(m_grip_spaces[hand], &controller.grip_pose, &controller.grip_velocity);

    controller.trigger_button = trigger_click || controller.trigger_value > 0.5f;
    controller.squeeze_button = squeeze_click || controller.squeeze_value > 0.5f;
    if (interaction_profiles[hand] == "/interaction_profiles/valve/index_controller")
    {
      m_index_grip_pressed[hand] = Common::VR::UpdatePressureButton(
          controller.squeeze_force, overlay.primedgun_index_grip_press_threshold,
          m_index_grip_pressed[hand]);
      m_index_trackpad_pressed[hand] = Common::VR::UpdatePressureButton(
          controller.trackpad_force, overlay.primedgun_trackpad_press_threshold,
          m_index_trackpad_pressed[hand]);
      controller.squeeze_button = m_index_grip_pressed[hand];
      controller.trackpad_button = m_index_trackpad_pressed[hand];
    }
    else
    {
      m_index_grip_pressed[hand] = false;
      m_index_trackpad_pressed[hand] = false;
    }
    controller.connected = action_seen || controller.aim_pose.valid || controller.grip_pose.valid;
  }

  // Provide HMD head orientation for IR pointer reference direction.
  // Use left eye orientation as a proxy for head center (negligible difference from averaged).
  Common::VR::OpenXRPoseState head_pose;
  if (m_eye_views[0].pose.orientation.w != 0.0f || m_eye_views[0].pose.orientation.x != 0.0f ||
      m_eye_views[0].pose.orientation.y != 0.0f || m_eye_views[0].pose.orientation.z != 0.0f)
  {
    if (!m_home_set)
    {
      if (m_reference_space_is_stage)
      {
        m_home_position = {0.0f, PRIMEDGUN_DEFAULT_STANDING_HEIGHT_M, 0.0f};
      }
      else
      {
        m_home_position.x =
            0.5f * (m_eye_views[0].pose.position.x + m_eye_views[1].pose.position.x);
        m_home_position.y =
            0.5f * (m_eye_views[0].pose.position.y + m_eye_views[1].pose.position.y);
        m_home_position.z =
            0.5f * (m_eye_views[0].pose.position.z + m_eye_views[1].pose.position.z);
      }
      m_home_set = true;
    }

    const auto make_home_relative = [this](Common::VR::OpenXRPoseState* pose) {
      if (!pose || !pose->valid)
        return;

      pose->position[0] -= m_home_position.x;
      pose->position[1] -= m_home_position.y;
      pose->position[2] -= m_home_position.z;
    };

    for (auto& controller : controllers)
    {
      make_home_relative(&controller.aim_pose);
      make_home_relative(&controller.grip_pose);
    }

    head_pose.valid = true;
    head_pose.orientation = {m_eye_views[0].pose.orientation.x,
                             m_eye_views[0].pose.orientation.y,
                             m_eye_views[0].pose.orientation.z,
                             m_eye_views[0].pose.orientation.w};
    head_pose.position = {m_eye_views[0].pose.position.x,
                          m_eye_views[0].pose.position.y,
                          m_eye_views[0].pose.position.z};
    make_home_relative(&head_pose);
  }

  const std::array<float, 3> tracking_origin_position{m_home_position.x, m_home_position.y,
                                                      m_home_position.z};
  Common::VR::OpenXRInputState::SetInteractionProfiles(interaction_profiles);
  Common::VR::OpenXRInputState::SetControllers(controllers, true, head_pose,
                                               tracking_origin_position);
  UpdateHaptics();
}

bool OpenXRManager::CreateReferenceSpace()
{
  XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  space_info.poseInReferenceSpace = {{0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 0.f}};

  // Prefer stage space so PrimedGun starts from the runtime's play-space origin instead of the
  // first HMD pose seen during boot.
  space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
  XrResult result = xrCreateReferenceSpace(m_session, &space_info, &m_reference_space);
  if (XR_SUCCEEDED(result))
  {
    m_reference_space_is_stage = true;
    m_home_position = {0.0f, PRIMEDGUN_DEFAULT_STANDING_HEIGHT_M, 0.0f};
    m_home_set = true;
    INFO_LOG_FMT(OPENXR,
                 "OpenXR: Using stage reference space for PrimedGun play-space origin with default "
                 "startup height {:.2f}m.",
                 PRIMEDGUN_DEFAULT_STANDING_HEIGHT_M);
    return true;
  }

  char result_string[XR_MAX_RESULT_STRING_SIZE]{};
  xrResultToString(m_instance, result, result_string);
  WARN_LOG_FMT(OPENXR, "OpenXR: Stage reference space unavailable ({}); falling back to local space.",
               result_string);

  space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  XR_CHECK(xrCreateReferenceSpace(m_session, &space_info, &m_reference_space));
  m_reference_space_is_stage = false;
  m_home_set = false;
  m_home_position = {0.0f, 0.0f, 0.0f};
  return true;
}

bool OpenXRManager::PollEvents()
{
  XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};

  while (xrPollEvent(m_instance, &event) == XR_SUCCESS)
  {
    switch (event.type)
    {
    case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
    {
      const auto& ev = *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
      HandleSessionStateChange(ev.state);
      break;
    }
    case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
      WARN_LOG_FMT(OPENXR, "OpenXR: Instance loss pending — stopping VR.");
      m_exit_render_loop = true;
      ResetInputActionsState();
      return false;

    case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
      INFO_LOG_FMT(OPENXR, "OpenXR: Reference space change pending.");
      break;

    default:
      break;
    }

    // Reset for next poll.
    event = {XR_TYPE_EVENT_DATA_BUFFER};
  }

  return !m_exit_render_loop;
}

void OpenXRManager::HandleSessionStateChange(XrSessionState new_state)
{
  INFO_LOG_FMT(OPENXR, "OpenXR: Session state → {}", static_cast<int>(new_state));
  m_session_state = new_state;

  switch (new_state)
  {
  case XR_SESSION_STATE_READY:
  {
    XrSessionBeginInfo begin_info{XR_TYPE_SESSION_BEGIN_INFO};
    begin_info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    if (XR_SUCCEEDED(xrBeginSession(m_session, &begin_info)))
    {
      m_session_running = true;
      INFO_LOG_FMT(OPENXR, "OpenXR: Session running.");
    }
    else
    {
      ERROR_LOG_FMT(OPENXR, "OpenXR: xrBeginSession failed.");
    }
    break;
  }
  case XR_SESSION_STATE_STOPPING:
    xrEndSession(m_session);
    m_session_running = false;
    ResetInputActionsState();
    INFO_LOG_FMT(OPENXR, "OpenXR: Session stopped.");
    break;

  case XR_SESSION_STATE_LOSS_PENDING:
  case XR_SESSION_STATE_EXITING:
    m_exit_render_loop = true;
    ResetInputActionsState();
    break;

  default:
    break;
  }
}

// PRIMEFRAME_PREDICT_EXTRA_MS (frame/PACING.md "Prediction for the real display time"): unset or 0
// off; <ms> a fixed extra; auto: one display period for every period the frame is expected to
// end past PRIMEFRAME_PREDICT_ONTIME_MS (19.5) after the runtime's wake-up for it (SteamVR on the
// Frame at framesToThrottle 1 showed frames ended later than that a whole period after their
// predicted display time). Expected end: how late the xrWaitFrame call returned after the wake-up
// plus the average xrWaitFrame return to xrEndFrame time.
namespace
{
struct PredictConfig
{
  int mode = 0;  // 0 off, 1 fixed, 2 auto
  int64_t fixed_ns = 0;
  double ontime_ms = 19.5;
};
const PredictConfig& GetPredictConfig()
{
  static const PredictConfig s_config = [] {
    PredictConfig c;
    const char* extra = std::getenv("PRIMEFRAME_PREDICT_EXTRA_MS");
    if (extra && std::string_view(extra) == "auto")
      c.mode = 2;
    else if (extra && std::atof(extra) > 0.0)
    {
      c.mode = 1;
      c.fixed_ns = static_cast<int64_t>(std::min(std::atof(extra), 100.0) * 1e6);
    }
    if (const char* ontime = std::getenv("PRIMEFRAME_PREDICT_ONTIME_MS"))
      c.ontime_ms = std::atof(ontime);
    INFO_LOG_FMT(OPENXR, "PrimeFrame predict extra: {} (on time within {:.1f} ms of the wake-up)",
                 c.mode == 2 ? std::string("auto") :
                 c.mode == 1 ? fmt::format("{:.2f} ms", c.fixed_ns / 1e6) :
                               std::string("off"),
                 c.ontime_ms);
    return c;
  }();
  return s_config;
}
}  // namespace

// The runtime's wake-up grid (frame/PACING.md "The wake-up"), under m_clock_mutex, right after
// xrWaitFrame returned (m_clock.display_time, m_clock.wake_ns set). SteamVR's XrTime is
// CLOCK_MONOTONIC_RAW, which runs about 33 us a second slower than the steady clock: the lead
// is kept in XrTime and converted with the offset of the moment, so it does not drift.
void OpenXRManager::UpdateWakeGrid(uint64_t waited_us)
{
  timespec raw_ts{};
  clock_gettime(CLOCK_MONOTONIC_RAW, &raw_ts);
  const int64_t raw_now = static_cast<int64_t>(raw_ts.tv_sec) * 1'000'000'000LL + raw_ts.tv_nsec;
  const int64_t steady_now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
  const int64_t raw_offset = raw_now - steady_now;
  if (m_xr_clock == 0 && m_clock.display_time > 0)
  {
    // The clock whose lead is plausible (0 to 500 ms); raw when both are (shortly after a boot
    // they differ by little). PRIMEFRAME_XR_CLOCK=raw|steady forces one.
    const auto plausible = [](int64_t lead) { return lead > 0 && lead < 500'000'000LL; };
    const int64_t lead_raw = m_clock.display_time - (m_clock.wake_ns + raw_offset);
    const int64_t lead_steady = m_clock.display_time - m_clock.wake_ns;
    const char* force = std::getenv("PRIMEFRAME_XR_CLOCK");
    if (force && std::string_view(force) == "raw")
      m_xr_clock = 1;
    else if (force && std::string_view(force) == "steady")
      m_xr_clock = 2;
    else if (plausible(lead_raw))
      m_xr_clock = 1;
    else if (plausible(lead_steady))
      m_xr_clock = 2;
    if (m_xr_clock != 0)
    {
      INFO_LOG_FMT(OPENXR,
                   "PrimeFrame XrTime clock: {} (first lead {:.2f} ms; with the other clock it "
                   "would read {:.2f} ms)",
                   m_xr_clock == 1 ? "CLOCK_MONOTONIC_RAW" : "steady (CLOCK_MONOTONIC)",
                   (m_xr_clock == 1 ? lead_raw : lead_steady) / 1e6,
                   (m_xr_clock == 1 ? lead_steady : lead_raw) / 1e6);
    }
  }
  m_xr_offset_ns = m_xr_clock == 1 ? raw_offset : 0;
  const int64_t lead = m_clock.display_time - (m_clock.wake_ns + m_xr_offset_ns);

  // A call that blocked (over 2 ms; returning at once still takes a fraction of a ms) returned at
  // the runtime's wake-up: its lead is exact. A different exact lead is taken at once while the
  // known one is only the largest seen, else after three blocking calls that agree.
  m_last_wait_blocked = waited_us > 2000;
  if (m_last_wait_blocked)
  {
    if (!m_wake_lead_exact || std::abs(lead - m_wake_lead_ns) < 3'000'000)
    {
      if (!m_wake_lead_exact)
      {
        INFO_LOG_FMT(OPENXR, "PrimeFrame wake-up lead: {:.2f} ms (a call that blocked {:.2f} ms)",
                     lead / 1e6, waited_us / 1e3);
      }
      m_wake_lead_ns = lead;
      m_wake_lead_exact = true;
      m_lead_disagree = 0;
    }
    else
    {
      if (m_lead_disagree > 0 && std::abs(lead - m_lead_candidate_ns) < 1'000'000)
        m_lead_disagree++;
      else
        m_lead_disagree = 1;
      m_lead_candidate_ns = lead;
      if (m_lead_disagree >= 3)
      {
        INFO_LOG_FMT(OPENXR, "PrimeFrame wake-up lead: {:.2f} ms (was {:.2f} ms)", lead / 1e6,
                     m_wake_lead_ns / 1e6);
        m_wake_lead_ns = lead;
        m_lead_disagree = 0;
      }
    }
  }
  else if (!m_wake_lead_exact && lead > m_wake_lead_ns && lead < 500'000'000LL)
  {
    m_wake_lead_ns = lead;
  }
  m_clock.grid_offset_ns = m_wake_lead_ns + m_xr_offset_ns;
  m_clock.early = m_wake_lead_exact && lead > m_wake_lead_ns + 500'000;

  // The refresh spacing: the smallest step between consecutive display times seen at least four
  // times in the last 64 (a step of 0, the same refresh handed out twice, or two refreshes do
  // not count).
  if (m_prev_wait_display_time != 0 && m_clock.display_time > m_prev_wait_display_time + 2'000'000)
  {
    m_display_steps[m_display_step_next] = m_clock.display_time - m_prev_wait_display_time;
    m_display_step_next = (m_display_step_next + 1) % m_display_steps.size();
    m_display_step_count = std::min(m_display_step_count + 1, m_display_steps.size());
    int64_t best = 0;
    for (size_t i = 0; i < m_display_step_count; ++i)
    {
      const int64_t step = m_display_steps[i];
      if (best != 0 && step >= best)
        continue;
      int near = 0;
      for (size_t j = 0; j < m_display_step_count; ++j)
        near += std::abs(m_display_steps[j] - step) < 500'000;
      if (near >= 4)
        best = step;
    }
    if (best != 0)
      m_slot_ns = best;
  }
  m_prev_wait_display_time = m_clock.display_time;
}

void OpenXRManager::GetWakeGrid(double* lead_ms, bool* lead_exact, double* slot_ms) const
{
  std::lock_guard<std::mutex> lock(m_clock_mutex);
  *lead_ms = m_wake_lead_ns / 1e6;
  *lead_exact = m_wake_lead_exact;
  *slot_ms = (m_slot_ns > 0 ? m_slot_ns : m_clock.period_ns) / 1e6;
}

bool OpenXRManager::WantsLeadCalibration() const
{
  std::lock_guard<std::mutex> lock(m_clock_mutex);
  if (m_wake_lead_exact || m_calib_attempts >= 5 || m_clock.display_time == 0)
    return false;
  const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
  return m_calib_last_ns == 0 || now - m_calib_last_ns > 2'000'000'000LL;
}

// The frame's xrWaitFrame return to its GPU work finished (the releaser's fence), for the
// expected finish of later frames of the same kind (handed out early or not).
void OpenXRManager::NoteCycleFinish(XrTime display_time, int64_t finish_ns)
{
  std::lock_guard<std::mutex> lock(m_clock_mutex);
  for (const WaitNote& note : m_wait_notes)
  {
    if (note.display_time != display_time || note.return_ns == 0)
      continue;
    const double sample = std::min((finish_ns - note.return_ns) / 1e6, 60.0);
    double& avg = m_finish_avg_ms[note.early ? 1 : 0];
    avg = avg <= 0 ? sample : avg * 0.9 + sample * 0.1;
    m_cycle_finish_seen = true;
  }
}

bool OpenXRManager::WaitFrame()
{
  XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
  m_frame_state = {XR_TYPE_FRAME_STATE};
  const auto wait_start = std::chrono::steady_clock::now();
  XR_CHECK(xrWaitFrame(m_session, &wait_info, &m_frame_state));
  const auto wait_end = std::chrono::steady_clock::now();
  const uint64_t waited_us = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(wait_end - wait_start).count());
  m_wait_count.fetch_add(1, std::memory_order_relaxed);
  m_wait_total_us.fetch_add(waited_us, std::memory_order_relaxed);
  if (waited_us > m_wait_max_us.load(std::memory_order_relaxed))
    m_wait_max_us.store(waited_us, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(m_clock_mutex);
    m_wait_call_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         wait_start.time_since_epoch())
                         .count();
    m_wait_return_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           wait_end.time_since_epoch())
                           .count();
    m_wait_display_time = m_frame_state.predictedDisplayTime;
    m_clock.display_time = m_frame_state.predictedDisplayTime;
    m_clock.wake_ns = m_wait_return_ns;
    m_clock.period_ns = m_frame_state.predictedDisplayPeriod;
    UpdateWakeGrid(waited_us);

    const PredictConfig& predict = GetPredictConfig();
    const double slot_ms = (m_slot_ns > 0 ? m_slot_ns : m_clock.period_ns) / 1e6;
    // How late after the runtime's wake-up for its display time this call returned (negative:
    // handed out early) plus the usual return-to-finish time of such frames: when the frame is
    // expected to be ready, against SteamVR's on-time limit.
    const double after_wake_ms = (m_wait_return_ns - (m_clock.display_time - m_clock.grid_offset_ns)) / 1e6;
    const double finish_avg =
        m_finish_avg_ms[m_clock.early ? 1 : 0] > 0 ? m_finish_avg_ms[m_clock.early ? 1 : 0] :
                                                      m_finish_avg_ms[m_clock.early ? 0 : 1];
    const double ready_ms = after_wake_ms + finish_avg;

    // Cycle end late (frame/PACING.md "The wake-up"): a frame that will not make SteamVR's
    // on-time limit (with a 2 ms margin, so that no frame straddles it) is planned to land whole
    // refreshes after its display time: the releaser holds it past the limit and its poses are
    // located for the refresh it lands on. No two frames may land on one refresh (the second
    // replaces the first, which is then never shown): a frame handed a display time at or before
    // the last frame's landing is planned late too, except that a frame handed out at least
    // 3 ms early that makes the limit may end the late run (once every 5 s; the last late frame
    // is then not shown).
    int land = 0;
    const int64_t slot_ns = static_cast<int64_t>(slot_ms * 1e6);
    if (m_cycle_end_late.load(std::memory_order_relaxed) && slot_ns > 0 && finish_avg > 0)
    {
      if (ready_ms > predict.ontime_ms - 2.0)
        land = std::max(1, static_cast<int>(std::ceil((ready_ms - predict.ontime_ms) / slot_ms)));
      land = std::min(land, 2);
      if (m_prev_land_time != 0 &&
          m_clock.display_time + land * slot_ns <= m_prev_land_time + slot_ns / 2)
      {
        const bool can_end_run = land == 0 && m_clock.early && after_wake_ms <= -3.0 &&
                                 m_wait_return_ns - m_late_run_end_ns > 5'000'000'000LL;
        if (can_end_run)
        {
          m_late_run_end_ns = m_wait_return_ns;
          m_late_run_ends++;
          INFO_LOG_FMT(OPENXR,
                       "PrimeFrame cycle: a frame handed out {:.2f} ms early ends a late run ({}); "
                       "the late frame before it is replaced",
                       -after_wake_ms, m_late_run_ends);
        }
        else
        {
          const int needed = static_cast<int>(
              std::llround(static_cast<double>(m_prev_land_time - m_clock.display_time) / slot_ns)) + 1;
          land = std::clamp(needed, land, 2);
        }
      }
    }
    m_clock.plan_slots = land;
    m_prev_land_time = m_clock.display_time + land * slot_ns;

    m_frame_extra_ns = 0;
    if (m_predict_allowed && predict.mode == 1)
    {
      m_frame_extra_ns = predict.fixed_ns;
    }
    else if (m_predict_allowed && predict.mode == 2 && land > 0)
    {
      m_frame_extra_ns = land * slot_ns;
    }
    else if (m_predict_allowed && predict.mode == 2 && finish_avg > 0 && slot_ms > 0 &&
             ready_ms > predict.ontime_ms)
    {
      const int periods =
          std::min(3, static_cast<int>(std::ceil((ready_ms - predict.ontime_ms) / slot_ms)));
      m_frame_extra_ns = static_cast<int64_t>(periods * slot_ms * 1e6);
    }
    m_wait_notes[m_wait_note_next] = {m_wait_display_time, m_wait_call_ns, m_wait_return_ns,
                                      m_clock.early, m_clock.plan_slots};
    m_wait_note_next = (m_wait_note_next + 1) % m_wait_notes.size();
  }
  m_predicted_display_time_snapshot.store(m_frame_state.predictedDisplayTime,
                                          std::memory_order_release);
  m_predicted_display_period_snapshot.store(m_frame_state.predictedDisplayPeriod,
                                            std::memory_order_release);
  m_should_render_snapshot.store(m_frame_state.shouldRender == XR_TRUE,
                                 std::memory_order_release);
  if (m_frame_state.predictedDisplayPeriod > 0)
  {
    m_native_display_period_ms.store(
        static_cast<double>(m_frame_state.predictedDisplayPeriod) / 1'000'000.0,
        std::memory_order_release);
  }
  UpdateInputActions();
  return true;
}

bool OpenXRManager::BeginFrame()
{
  if (m_swapchain && !m_swapchain->WaitForPendingFrameFinalization("before xrBeginFrame"))
    return false;

  XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
  std::unique_lock<std::mutex> queue_lock;
  if (m_swapchain)
    queue_lock = m_swapchain->AcquireGraphicsQueueLock();

  const XrResult result = xrBeginFrame(m_session, &begin_info);
  if (XR_FAILED(result))
  {
    char buffer[XR_MAX_RESULT_STRING_SIZE]{};
    xrResultToString(m_instance, result, buffer);
    ERROR_LOG_FMT(OPENXR, "OpenXR: xrBeginFrame failed: {}", buffer);
    return false;
  }
  return true;
}

bool OpenXRManager::EndFrame(const std::vector<XrCompositionLayerBaseHeader*>& layers)
{
  XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
  end_info.displayTime = m_frame_state.predictedDisplayTime;
  end_info.environmentBlendMode = GetActiveBlendMode();

  // Only submit layers when the runtime requests rendering; otherwise submit 0 layers
  // (this handles the VISIBLE/SYNCHRONIZED states correctly).
  if (m_frame_state.shouldRender == XR_TRUE)
  {
    end_info.layerCount = static_cast<uint32_t>(layers.size());
    end_info.layers = layers.data();
  }

  // The runtime's client may submit on our queue inside xrEndFrame (SteamVR does).
  {
    auto queue_lock = m_swapchain ? m_swapchain->AcquireGraphicsQueueLock() :
                                    std::unique_lock<std::mutex>{};
    XR_CHECK(xrEndFrame(m_session, &end_info));
  }
  NoteFrameEnd(end_info.displayTime, m_submitted_eye_views_valid ? m_submitted_locate_time : 0,
               end_info.layerCount > 0);
  return true;
}

void OpenXRManager::TakeWaitFrameStats(uint32_t* count, uint64_t* total_us, uint64_t* max_us)
{
  *count = m_wait_count.exchange(0, std::memory_order_relaxed);
  *total_us = m_wait_total_us.exchange(0, std::memory_order_relaxed);
  *max_us = m_wait_max_us.exchange(0, std::memory_order_relaxed);
}

XrCompositionLayerFlags OpenXRManager::GetProjectionLayerExtraFlags() const
{
  if (GetActiveBlendMode() == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND)
  {
    return XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
           XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
  }
  return 0;
}

bool OpenXRManager::EndFrameDetached(XrTime display_time,
                                     XrEnvironmentBlendMode environment_blend_mode,
                                     bool should_render,
                                     const std::vector<XrCompositionLayerBaseHeader*>& layers,
                                     XrTime pose_time)
{
  XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
  end_info.displayTime = display_time;
  end_info.environmentBlendMode = environment_blend_mode;

  if (should_render)
  {
    end_info.layerCount = static_cast<uint32_t>(layers.size());
    end_info.layers = layers.data();
  }

  {
    auto queue_lock = m_swapchain ? m_swapchain->AcquireGraphicsQueueLock() :
                                    std::unique_lock<std::mutex>{};
    XR_CHECK(xrEndFrame(m_session, &end_info));
  }
  NoteFrameEnd(display_time, pose_time, end_info.layerCount > 0);
  return true;
}

static bool FrameLogEnabled()
{
  static const bool s_enabled = [] {
    const char* env = std::getenv("PRIMEFRAME_FRAME_LOG");
    return !env || std::string_view(env) != "0";
  }();
  return s_enabled;
}

static double OnTimeMs()
{
  static const double s_ms = [] {
    const char* env = std::getenv("PRIMEFRAME_ONTIME_MS");
    return env ? std::atof(env) : 2.5;
  }();
  return s_ms;
}

// One line per ended frame (XRF), the per-second sums for XRSTATS. Times in ms: blk how long its
// xrWaitFrame blocked; lead its predicted display time minus the xrWaitFrame return; r the
// xrWaitFrame return to the xrEndFrame; rg the runtime's wake-up for the display time (display
// time minus the runtime's lead, UpdateWakeGrid) to the xrEndFrame; age the poses' xrLocateViews call
// to the xrEndFrame (-1: unknown); pdt the poses' display time minus the end's (0: the frame
// ends with the display time its poses were located for); dd refreshes since the last ended
// display time (in refresh steps); per the predictedDisplayPeriod; ex how far past the display time
// the poses were located; w the xrWaitFrame return minus the runtime's wake-up for the display
// time (negative: handed out early); pl the refreshes the frame was planned to land after its
// display time (cycle end late).
void OpenXRManager::NoteFrameEnd(XrTime display_time, XrTime pose_time, bool has_layers)
{
  const int64_t end_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
  double blk = -1, lead = -1, r = -1, rg = -1, age = -1, pdt = 0, period_ms = 0, ex = 0, w = 0;
  int64_t dd = 0;
  uint64_t n = 0;
  bool early = false;
  int plan_slots = 0;
  {
    std::lock_guard<std::mutex> lock(m_clock_mutex);
    const int64_t period = m_slot_ns > 0 ? m_slot_ns : m_clock.period_ns;
    period_ms = m_clock.period_ns / 1e6;
    for (const WaitNote& note : m_wait_notes)
    {
      if (note.display_time == display_time && note.return_ns != 0)
      {
        blk = (note.return_ns - note.call_ns) / 1e6;
        lead = (display_time - note.return_ns) / 1e6;
        r = (end_ns - note.return_ns) / 1e6;
        early = note.early;
        plan_slots = note.plan_slots;
        if (m_clock.grid_offset_ns != 0)
          w = (note.return_ns - (display_time - m_clock.grid_offset_ns)) / 1e6;
      }
    }
    if (m_clock.grid_offset_ns != 0)
      rg = (end_ns - (display_time - m_clock.grid_offset_ns)) / 1e6;
    int64_t extra_ns = 0;
    if (pose_time != 0)
    {
      for (const LocateNote& note : m_locate_notes)
      {
        if (note.display_time == pose_time && note.call_ns != 0)
        {
          age = (end_ns - note.call_ns) / 1e6;
          extra_ns = note.extra_ns;
        }
      }
      // Poses located past their display time on purpose count as located for it.
      pose_time -= extra_ns;
      pdt = (pose_time - display_time) / 1e6;
      ex = extra_ns / 1e6;
    }
    if (period > 0 && m_prev_end_display_time != 0)
      dd = (display_time - m_prev_end_display_time + period / 2) / period;
    m_prev_end_display_time = display_time;
    if (has_layers)
    {
      FrameTimingStats& t = m_frame_timing;
      t.frames++;
      if (pose_time != 0 && pose_time != display_time)
        t.pose_mismatch++;
      const double rr = rg >= 0 ? rg : r;
      if (rr > OnTimeMs())
        t.ended_late++;
      if (rr >= 0)
      {
        t.end_sum_ms += rr;
        t.end_max_ms = std::max(t.end_max_ms, rr);
      }
      if (age >= 0)
      {
        t.age_sum_ms += age;
        t.age_max_ms = std::max(t.age_max_ms, age);
        t.age_count++;
      }
      if (lead >= 0)
        t.lead_sum_ms += lead;
      if (extra_ns != 0)
      {
        t.extra_sum_ms += ex;
        t.extra_frames++;
      }
      if (early)
        t.early_frames++;
      if (r >= 0 && !m_cycle_finish_seen)
      {
        const double sample = std::min(r, 50.0);
        double& avg = m_finish_avg_ms[early ? 1 : 0];
        avg = avg <= 0 ? sample : avg * 0.9 + sample * 0.1;
      }
    }
    n = ++m_frame_log_count;
  }
  if (FrameLogEnabled())
  {
    INFO_LOG_FMT(OPENXR,
                 "XRF n={} blk={:.2f} lead={:.2f} r={:.2f} rg={:.2f} age={:.2f} pdt={:.2f} dd={} "
                 "per={:.3f} ex={:.2f} w={:.2f} pl={}{}",
                 n, blk, lead, r, rg, age, pdt, dd, period_ms, ex, w, plan_slots,
                 has_layers ? "" : " empty");
  }
}

void OpenXRManager::TakeFrameTimingStats(FrameTimingStats* out)
{
  std::lock_guard<std::mutex> lock(m_clock_mutex);
  *out = m_frame_timing;
  m_frame_timing = {};
}

void OpenXRManager::ArmLazyFrameStart()
{
  m_lazy_thread = std::this_thread::get_id();
  m_lazy_started = false;
  m_lazy_armed.store(true, std::memory_order_release);
}

bool OpenXRManager::TakeLazyFrameStarted()
{
  m_lazy_armed.store(false, std::memory_order_relaxed);
  const bool started = m_lazy_started;
  m_lazy_started = false;
  return started;
}

// The pose readers are const (the geometry shader manager reads the views through them); the
// frame start they trigger is not.
void OpenXRManager::StartLazyFrameIfArmed() const
{
  if (!m_lazy_armed.load(std::memory_order_acquire) ||
      std::this_thread::get_id() != m_lazy_thread)
  {
    return;
  }
  OpenXRManager* self = const_cast<OpenXRManager*>(this);
  self->m_lazy_armed.store(false, std::memory_order_relaxed);
  if (!self->PollEvents() || !self->IsSessionRunning())
    return;
  if (!self->StartCycleFrame(true))
    return;
  self->m_lazy_started = true;
}

bool OpenXRManager::StartCycleFrame(bool locate_views)
{
  if (WantsLeadCalibration())
  {
    // No exact lead yet (no xrWaitFrame has blocked): end whatever is pending, so that SteamVR
    // blocks the next call until its wake-up; if this one returns at once, spend it on an empty
    // frame and wait for the next refresh.
    if (m_swapchain && !m_swapchain->WaitForPendingFrameFinalization("lead calibration"))
      return false;
    {
      std::lock_guard<std::mutex> lock(m_clock_mutex);
      m_calib_attempts++;
      m_calib_last_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    }
    if (!WaitFrame())
      return false;
    if (!m_last_wait_blocked)
    {
      if (!BeginFrame() || !EndFrame({}) || !WaitFrame())
        return false;
    }
    INFO_LOG_FMT(OPENXR, "PrimeFrame lead calibration {}: the frame's xrWaitFrame {}",
                 m_calib_attempts, m_last_wait_blocked ? "blocked" : "did not block");
    if (!BeginFrame())
      return false;
  }
  else if (m_swapchain && m_swapchain->UsesCycleOverlap())
  {
    // Cycle end asap (frame/PACING.md "The wake-up"): wait for the last frame's end first, so
    // that SteamVR blocks this call until its wake-up. Called while a frame is pending, SteamVR
    // hands out the next refresh at once, up to a period before its wake-up: the frame is
    // located that much earlier for the same refresh (a longer prediction). A room whose frames
    // finish in time does not need the overlap; the late mode (heavy rooms) keeps it.
    static const bool s_asap_wait = [] {
      const char* env = std::getenv("PRIMEFRAME_CYCLE_ASAP_WAIT");
      return !(env && std::string_view(env) == "0");
    }();
    if (s_asap_wait && !m_cycle_end_late.load(std::memory_order_relaxed) &&
        !m_swapchain->WaitForPendingFrameFinalization("cycle asap"))
    {
      return false;
    }
    // The last frame may still be ending on the releaser thread: wait only until it was begun.
    if (!m_swapchain->CycleBeforeWait() || !WaitFrame())
      return false;
    if (m_swapchain->CycleBeginNowAfterWait() && !BeginFrame())
      return false;
  }
  else
  {
    if (m_swapchain && !m_swapchain->WaitForPendingFrameFinalization("cycle frame start"))
      return false;
    if (!WaitFrame() || !BeginFrame())
      return false;
  }
  if (locate_views && ShouldRender())
    LocateViews();
  return true;
}

bool OpenXRManager::LocateViews()
{
  const XrTime snapshot_time =
      m_predicted_display_time_snapshot.load(std::memory_order_acquire);
  if (snapshot_time == 0)
    return false;

  if (IsFrameThreadActive())
  {
    return LocateViewsAt(snapshot_time +
                         m_predicted_display_period_snapshot.load(std::memory_order_acquire));
  }
  return LocateViewsAt(snapshot_time + m_frame_extra_ns, m_frame_extra_ns);
}

bool OpenXRManager::LocateViewsAt(XrTime display_time, int64_t extra_ns)
{
  if (display_time == 0)
    return false;
  XrViewLocateInfo locate_info{XR_TYPE_VIEW_LOCATE_INFO};
  locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  locate_info.displayTime = display_time;
  locate_info.space = m_reference_space;

  XrViewState view_state{XR_TYPE_VIEW_STATE};
  uint32_t view_count = static_cast<uint32_t>(m_views.size());
  m_views.fill({XR_TYPE_VIEW});
  m_eye_views_valid = false;
  {
    std::lock_guard<std::mutex> lock(m_clock_mutex);
    m_locate_notes[m_locate_note_next] = {
        display_time,
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count(),
        extra_ns};
    m_locate_note_next = (m_locate_note_next + 1) % m_locate_notes.size();
  }

  XR_CHECK(xrLocateViews(m_session, &locate_info, &view_state,
                          view_count, &view_count, m_views.data()));

  constexpr XrViewStateFlags required_flags =
      XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
  m_eye_views_valid = view_count >= 2 && (view_state.viewStateFlags & required_flags) == required_flags;
  if (!m_eye_views_valid)
  {
    WARN_LOG_FMT(OPENXR, "OpenXR: Skipping projection layer because view pose is invalid (flags={:#x}, views={}).",
                 static_cast<unsigned int>(view_state.viewStateFlags), view_count);
    return true;
  }

  for (uint32_t i = 0; i < view_count; ++i)
  {
    m_eye_views[i].pose = m_views[i].pose;
    m_eye_views[i].fov = m_views[i].fov;
  }
  m_eye_views_locate_time = display_time;

  if (m_recenter_requested.exchange(false, std::memory_order_acq_rel) && view_count >= 2)
  {
    const float center_x = 0.5f * (m_eye_views[0].pose.position.x + m_eye_views[1].pose.position.x);
    const float center_z = 0.5f * (m_eye_views[0].pose.position.z + m_eye_views[1].pose.position.z);
    const float old_x = m_home_set ? m_home_position.x : (m_reference_space_is_stage ? 0.0f : center_x);
    const float old_z = m_home_set ? m_home_position.z : (m_reference_space_is_stage ? 0.0f : center_z);
    m_home_position.y = 0.5f * (m_eye_views[0].pose.position.y + m_eye_views[1].pose.position.y);
    m_home_position.x = old_x;
    m_home_position.z = old_z;
    m_home_set = true;
    INFO_LOG_FMT(OPENXR, "OpenXR: Recentered home height only to ({:.4f},{:.4f},{:.4f})",
                 m_home_position.x, m_home_position.y, m_home_position.z);
  }

  static uint64_t s_locate_log_counter = 0;
  if ((++s_locate_log_counter % 120) == 1 && view_count >= 2)
  {
    const auto& e0 = m_eye_views[0];
    const auto& e1 = m_eye_views[1];
    const float hx = 0.5f * (e0.pose.position.x + e1.pose.position.x);
    const float hy = 0.5f * (e0.pose.position.y + e1.pose.position.y);
    const float hz = 0.5f * (e0.pose.position.z + e1.pose.position.z);
    const float dx = e1.pose.position.x - e0.pose.position.x;
    const float dy = e1.pose.position.y - e0.pose.position.y;
    const float dz = e1.pose.position.z - e0.pose.position.z;
    const float ipd_m = std::sqrt(dx * dx + dy * dy + dz * dz);
    const EulerDeg euler = QuaternionToEulerDeg(e0.pose.orientation);

    INFO_LOG_FMT(
        VIDEO,
        "OpenXR DBG locate #{}: eye0 pos=({:.4f},{:.4f},{:.4f}) quat=({:.5f},{:.5f},{:.5f},{:.5f}) "
        "eye1 pos=({:.4f},{:.4f},{:.4f}) head_center=({:.4f},{:.4f},{:.4f}) ipd={:.4f}m "
        "ypr=({:.2f},{:.2f},{:.2f})",
        s_locate_log_counter, e0.pose.position.x, e0.pose.position.y, e0.pose.position.z,
        e0.pose.orientation.x, e0.pose.orientation.y, e0.pose.orientation.z, e0.pose.orientation.w,
        e1.pose.position.x, e1.pose.position.y, e1.pose.position.z, hx, hy, hz, ipd_m, euler.yaw,
        euler.pitch, euler.roll);
  }

  return true;
}

void OpenXRManager::RecordRenderedEyeViews()
{
  m_submitted_eye_views = m_eye_views;
  m_submitted_eye_views_valid = m_eye_views_valid;
  m_submitted_locate_time = m_eye_views_locate_time;
}

void OpenXRManager::StampXFBPose(uint32_t xfb_addr)
{
  const XrTime draw_oldest = m_draw_pose_min;
  const XrTime draw_newest = m_draw_pose_max;
  m_draw_pose_min = 0;
  m_draw_pose_max = 0;
  if (!m_submitted_eye_views_valid)
    return;

  XFBPoseStamp* slot = nullptr;
  for (auto& stamp : m_xfb_pose_stamps)
  {
    if (stamp.serial != 0 && stamp.xfb_addr == xfb_addr)
    {
      slot = &stamp;
      break;
    }
  }

  if (!slot)
  {
    slot = &m_xfb_pose_stamps[m_xfb_pose_stamp_next];
    m_xfb_pose_stamp_next = (m_xfb_pose_stamp_next + 1) % m_xfb_pose_stamps.size();
  }

  slot->xfb_addr = xfb_addr;
  slot->serial = ++m_xfb_pose_stamp_serial;
  slot->views = m_submitted_eye_views;
  slot->locate_time = m_submitted_locate_time;
  slot->draw_oldest = draw_oldest;
  slot->draw_newest = draw_newest;
}

void OpenXRManager::SelectPresentPoseForXFB(uint32_t xfb_addr)
{
  const XFBPoseStamp* match = nullptr;
  const XFBPoseStamp* newest = nullptr;
  for (const auto& stamp : m_xfb_pose_stamps)
  {
    if (stamp.serial == 0)
      continue;
    if (stamp.xfb_addr == xfb_addr)
      match = &stamp;
    if (!newest || stamp.serial > newest->serial)
      newest = &stamp;
  }

  m_present_pose = {};
  if (match)
  {
    m_present_pose.valid = true;
    m_present_pose.locate_time = match->locate_time;
    m_present_pose.draw_oldest = match->draw_oldest;
    m_present_pose.draw_newest = match->draw_newest;
  }
  if (!match)
    match = newest;
  if (match)
  {
    m_present_eye_views = match->views;
    m_present_eye_views_valid = true;
  }
}

bool OpenXRManager::IsExtensionEnabled(const char* extension_name) const
{
  if (!extension_name || extension_name[0] == '\0')
    return false;

  const std::string_view wanted(extension_name);
  return std::any_of(m_enabled_extensions.begin(), m_enabled_extensions.end(),
                     [wanted](const std::string& extension) { return extension == wanted; });
}

bool OpenXRManager::ShouldUseVulkanLegacyProjectionFallback() const
{
  return false;
}

bool OpenXRManager::IsQuestOrVirtualDesktopRuntime() const
{
  std::string runtime = m_runtime_name;
  std::transform(runtime.begin(), runtime.end(), runtime.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });

  return runtime.find("quest") != std::string::npos ||
         runtime.find("oculus") != std::string::npos ||
         runtime.find("meta") != std::string::npos ||
         runtime.find("virtual desktop") != std::string::npos;
}

void OpenXRManager::RequestRecenter()
{
  m_recenter_requested.store(true, std::memory_order_release);
}

void OpenXRManager::GetEyeProjectionRows(
    float units_per_meter,
    std::array<std::array<float, 4>, 4>& out_proj_rows,
    std::array<std::array<float, 4>, 2>& out_z_rows) const
{
  StartLazyFrameIfArmed();
  static uint64_t s_proj_log_counter = 0;
  ++s_proj_log_counter;
  const bool do_log = false;
  const float s = std::max(units_per_meter, 0.0001f);
  constexpr float DEG_TO_RAD = 0.01745329252f;
  const float lean_back_rad = g_ActiveConfig.vr_lean_back_angle * DEG_TO_RAD;
  // Positive UI values should move the camera forward.
  // In this projection path, decreasing eye-space Z corresponds to moving forward.
  const float camera_forward_units = -g_ActiveConfig.vr_camera_forward * s;

  if (!m_home_set)
  {
    if (m_reference_space_is_stage)
    {
      m_home_position = {0.0f, PRIMEDGUN_DEFAULT_STANDING_HEIGHT_M, 0.0f};
    }
    else
    {
      m_home_position.x =
          0.5f * (m_eye_views[0].pose.position.x + m_eye_views[1].pose.position.x);
      m_home_position.y =
          0.5f * (m_eye_views[0].pose.position.y + m_eye_views[1].pose.position.y);
      m_home_position.z =
          0.5f * (m_eye_views[0].pose.position.z + m_eye_views[1].pose.position.z);
    }
    m_home_set = true;
    INFO_LOG_FMT(OPENXR, "OpenXR: Home position set to ({:.4f},{:.4f},{:.4f})",
                 m_home_position.x, m_home_position.y, m_home_position.z);
  }

  for (uint32_t eye = 0; eye < 2; ++eye)
  {
    const XrFovf& fov = m_eye_views[eye].fov;
    const XrQuaternionf& q_xr = m_eye_views[eye].pose.orientation;
    const XrVector3f& eye_pos_xr = m_eye_views[eye].pose.position;
    XrQuaternionf q = {-q_xr.x, -q_xr.y, -q_xr.z, q_xr.w};
    if (lean_back_rad != 0.0f)
    {
      const float half_angle = 0.5f * lean_back_rad;
      const XrQuaternionf lean_back_quat = {std::sin(half_angle), 0.0f, 0.0f,
                                            std::cos(half_angle)};
      q = MultiplyQuaternions(q, lean_back_quat);
    }

    // --- Quaternion to 3x3 rotation matrix R ---
    // R transforms from eye-local frame to reference space (standard quaternion convention).
    // Columns of R are the eye's local axes expressed in reference-space coordinates.
    // To go reference→local we use R^T, but it's never needed explicitly: see below.
    const float x2 = 2.0f * q.x * q.x, y2 = 2.0f * q.y * q.y, z2 = 2.0f * q.z * q.z;
    const float xy = 2.0f * q.x * q.y, xz = 2.0f * q.x * q.z, yz = 2.0f * q.y * q.z;
    const float wx = 2.0f * q.w * q.x, wy = 2.0f * q.w * q.y, wz = 2.0f * q.w * q.z;

    // R rows (R[row][col]):
    // R[0] = { 1-y2-z2,  xy+wz,   xz-wy  }
    // R[1] = { xy-wz,    1-x2-z2, yz+wx   }
    // R[2] = { xz+wy,    yz-wx,   1-x2-y2 }
    //
    // R^T maps reference-space positions into eye-local coords:
    //   eye_local = R^T * (viewPos - eye_pos_game)
    //
    // clip_x = P_row0 · eye_local = P_row0 · R^T · d
    //        = Σ_j d_j · (Σ_i P_i · R[j][i])
    //        = (R * P_col0) · d
    //
    // So combined_row = R * proj_col  (NOT R^T * proj_col — that would be wrong).

    // R matrix elements (row, col)
    const float r00 = 1.0f - y2 - z2, r01 = xy + wz, r02 = xz - wy;
    const float r10 = xy - wz, r11 = 1.0f - x2 - z2, r12 = yz + wx;
    const float r20 = xz + wy, r21 = yz - wx, r22 = 1.0f - x2 - y2;

    // --- Asymmetric projection from FOV tangent angles ---
    const float tanL = tanf(fov.angleLeft);   // negative
    const float tanR_val = tanf(fov.angleRight);  // positive
    const float tanU = tanf(fov.angleUp);     // positive
    const float tanD = tanf(fov.angleDown);   // negative

    const float inv_w = 1.0f / (tanR_val - tanL);
    const float inv_h = 1.0f / (tanU - tanD);

    // Raw projection rows (before head rotation):
    //   proj_row0 = { 2*inv_w,  0,        (tanR+tanL)*inv_w }
    //   proj_row1 = { 0,        2*inv_h,  (tanU+tanD)*inv_h }
    const float p0x = 2.0f * inv_w;
    const float p0z = (tanR_val + tanL) * inv_w;
    const float p1y = 2.0f * inv_h;
    const float p1z = (tanU + tanD) * inv_h;

    // --- Bake rotation: combined = R * proj_row ---
    // combined_row0 = R * {p0x, 0, p0z}
    const float c0x = r00 * p0x + r02 * p0z;
    const float c0y = r10 * p0x + r12 * p0z;
    const float c0z = r20 * p0x + r22 * p0z;

    // combined_row1 = R * {0, p1y, p1z}
    const float c1x = r01 * p1y + r02 * p1z;
    const float c1y = r11 * p1y + r12 * p1z;
    const float c1z = r21 * p1y + r22 * p1z;

    // Eye position relative to home, in game units.
    // Includes both IPD offset (per-eye) and head positional tracking (shared).
    float ex = (eye_pos_xr.x - m_home_position.x) * s;
    float ey = (eye_pos_xr.y - m_home_position.y) * s;
    float ez = (eye_pos_xr.z - m_home_position.z) * s;
    if (camera_forward_units != 0.0f)
    {
      // Apply a fixed camera-space forward offset without coupling it to current
      // head orientation. This keeps head tracking anchored like freelook offsets.
      ez += camera_forward_units;
    }

    // W component: -dot(combined_xyz, eye_pos) using the ROTATED projection rows.
    // This gives the correct full view transform: P · R^T · (viewPos - eye_pos).
    const float c0w = -(c0x * ex + c0y * ey + c0z * ez);
    const float c1w = -(c1x * ex + c1y * ey + c1z * ez);

    out_proj_rows[eye * 2 + 0] = {c0x, c0y, c0z, c0w};
    out_proj_rows[eye * 2 + 1] = {c1x, c1y, c1z, c1w};

    // --- Z-axis row for depth/w computation ---
    // z_eye = (R^T * (viewPos - eye_pos)).z = dot(R_col2, viewPos - eye_pos)
    // R_col2 = {r02, r12, r22}
    const float zw = -(r02 * ex + r12 * ey + r22 * ez);
    out_z_rows[eye] = {r02, r12, r22, zw};

    if (do_log)
    {
      INFO_LOG_FMT(
          VIDEO,
          "OpenXR DBG proj #{} eye{}: upm={:.3f} eye_pos_m=({:.4f},{:.4f},{:.4f}) eye_rel_u=({:.3f},"
          "{:.3f},{:.3f}) row0=({:.5f},{:.5f},{:.5f},{:.5f}) row1=({:.5f},{:.5f},{:.5f},{:.5f}) "
          "zrow=({:.5f},{:.5f},{:.5f},{:.5f})",
          s_proj_log_counter, eye, s, eye_pos_xr.x, eye_pos_xr.y, eye_pos_xr.z, ex, ey, ez,
          c0x, c0y, c0z, c0w, c1x, c1y, c1z, c1w, r02, r12, r22, zw);
    }
  }
}

void OpenXRManager::GetRawEyeProjectionRows(
    float units_per_meter,
    std::array<std::array<float, 4>, 4>& out_proj_rows) const
{
  StartLazyFrameIfArmed();
  const float s = std::max(units_per_meter, 0.0001f);

  // Compute head center (average of both eyes) for extracting per-eye local offset.
  const float hx = 0.5f * (m_eye_views[0].pose.position.x + m_eye_views[1].pose.position.x);
  const float hy = 0.5f * (m_eye_views[0].pose.position.y + m_eye_views[1].pose.position.y);
  const float hz = 0.5f * (m_eye_views[0].pose.position.z + m_eye_views[1].pose.position.z);

  // Get head rotation to compute R^T * (eye_world - head_center) = local eye offset.
  const XrQuaternionf& q_xr = m_eye_views[0].pose.orientation;
  const XrQuaternionf q = {-q_xr.x, -q_xr.y, -q_xr.z, q_xr.w};
  const float x2 = 2.0f * q.x * q.x, y2 = 2.0f * q.y * q.y, z2 = 2.0f * q.z * q.z;
  const float xy = 2.0f * q.x * q.y, xz = 2.0f * q.x * q.z, yz = 2.0f * q.y * q.z;
  const float wx = 2.0f * q.w * q.x, wy = 2.0f * q.w * q.y, wz = 2.0f * q.w * q.z;

  // R^T rows (= R columns) for transforming world offset to head-local space
  const float rt00 = 1.0f - y2 - z2, rt01 = xy - wz, rt02 = xz + wy;
  const float rt10 = xy + wz, rt11 = 1.0f - x2 - z2, rt12 = yz - wx;

  for (uint32_t eye = 0; eye < 2; ++eye)
  {
    const XrFovf& fov = m_eye_views[eye].fov;
    const XrVector3f& eye_pos_xr = m_eye_views[eye].pose.position;

    // --- Asymmetric projection from FOV tangent angles (same as rotated version) ---
    const float tanL = tanf(fov.angleLeft);
    const float tanR_val = tanf(fov.angleRight);
    const float tanU = tanf(fov.angleUp);
    const float tanD = tanf(fov.angleDown);

    const float inv_w = 1.0f / (tanR_val - tanL);
    const float inv_h = 1.0f / (tanU - tanD);

    // Raw (unrotated) projection rows
    const float p0x = 2.0f * inv_w;
    const float p0z = (tanR_val + tanL) * inv_w;
    const float p1y = 2.0f * inv_h;
    const float p1z = (tanU + tanD) * inv_h;

    // Per-eye offset in head-local space: R^T * (eye_world - head_center)
    const float dx = eye_pos_xr.x - hx;
    const float dy = eye_pos_xr.y - hy;
    const float dz = eye_pos_xr.z - hz;

    const float local_ex = (rt00 * dx + rt01 * dy + rt02 * dz) * s;
    const float local_ey = (rt10 * dx + rt11 * dy + rt12 * dz) * s;

    // W component using raw (unrotated) P rows and head-local eye offset
    const float pw0 = -(p0x * local_ex + p0z * 0.0f);  // p0z * local_ez ≈ 0
    const float pw1 = -(p1y * local_ey + p1z * 0.0f);  // p1z * local_ez ≈ 0

    out_proj_rows[eye * 2 + 0] = {p0x, 0.0f, p0z, pw0};
    out_proj_rows[eye * 2 + 1] = {0.0f, p1y, p1z, pw1};
  }
}

bool OpenXRManager::GetHeadCullProjection(float cone_degrees, float units_per_meter,
                                          std::array<std::array<float, 4>, 4>* out_matrix,
                                          float* out_effective_degrees) const
{
  StartLazyFrameIfArmed();
  if (!m_eye_views_valid || !m_home_set)
    return false;
  std::array<XrView, 2> views{};
  for (size_t eye = 0; eye < views.size(); ++eye)
  {
    views[eye].pose = m_eye_views[eye].pose;
    views[eye].fov = m_eye_views[eye].fov;
  }
  return BuildHeadCullProjection(views, m_home_position, cone_degrees, units_per_meter,
                                 g_ActiveConfig.vr_lean_back_angle, g_ActiveConfig.vr_camera_forward,
                                 out_matrix, out_effective_degrees);
}

bool OpenXRManager::GetLegacyViewMatrix(float units_per_meter,
                                        Common::Matrix44* out_view_matrix) const
{
  if (!out_view_matrix)
    return false;

  *out_view_matrix = Common::Matrix44::Identity();
  return false;
}

void OpenXRManager::GetLegacyProjectionAdjustments(
    float units_per_meter, float game_projection_x_scale, float game_projection_x_offset,
    float game_projection_y_scale, float game_projection_y_offset,
    std::array<std::array<float, 4>, 2>& out_x_rows,
    std::array<std::array<float, 4>, 2>& out_y_rows) const
{
  out_x_rows = {};
  out_y_rows = {};
}

}  // namespace VR

#endif  // ENABLE_VR
