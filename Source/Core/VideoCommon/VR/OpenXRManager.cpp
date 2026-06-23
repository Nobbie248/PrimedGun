// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef ENABLE_VR

#include "VideoCommon/VR/OpenXRManager.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "Common/Config/Config.h"
#include "Common/Logging/Log.h"
#include "Common/Matrix.h"
#include "Common/VR/OpenXRInputState.h"
#include "Core/Config/GraphicsSettings.h"
#include "VideoCommon/VideoConfig.h"

#if defined(ANDROID)
#include <unistd.h>  // gettid()

// XR_KHR_android_thread_settings (xrSetAndroidApplicationThreadKHR + XrAndroidThreadTypeKHR) lives
// in openxr_platform.h behind XR_USE_PLATFORM_ANDROID; jni.h provides the jobject it references.
#include <jni.h>
#define XR_USE_PLATFORM_ANDROID
#include <openxr/openxr_platform.h>
#endif

namespace VR
{
std::unique_ptr<OpenXRManager> g_openxr;

namespace
{
constexpr float PRIMEGUN_DEFAULT_STANDING_HEIGHT_M = 1.2f;

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
  DestroyInputActions();
  ResetInputActionsState();

  if (m_reference_space != XR_NULL_HANDLE)
    xrDestroySpace(m_reference_space);
  if (m_old_reference_space != XR_NULL_HANDLE)
    xrDestroySpace(m_old_reference_space);
  if (m_recenter_base_space != XR_NULL_HANDLE)
    xrDestroySpace(m_recenter_base_space);

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
  return extensions;
}

bool OpenXRManager::CreateInstance(const std::vector<const char*>& extra_extensions,
                                   const void* platform_instance_create_next)
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
  create_info.next = platform_instance_create_next;
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
      !create_action(&m_action_trigger_click, "trigger_click", "Trigger Click",
                     XR_ACTION_TYPE_BOOLEAN_INPUT) ||
      !create_action(&m_action_squeeze_click, "squeeze_click", "Squeeze Click",
                     XR_ACTION_TYPE_BOOLEAN_INPUT) ||
      !create_action(&m_action_trigger_value, "trigger_value", "Trigger Value",
                     XR_ACTION_TYPE_FLOAT_INPUT) ||
      !create_action(&m_action_squeeze_value, "squeeze_value", "Squeeze Value",
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

  // NOTE: the Oculus Touch interaction profile has NO trackpad component
  // (/input/trackpad/*). Suggesting any unsupported path makes the runtime reject the ENTIRE
  // xrSuggestInteractionProfileBindings call for this profile, so the Quest then falls back to
  // khr/simple_controller (only trigger->select + menu) and every thumbstick/trigger/grip/face
  // binding silently dies. Keep trackpad bindings out of this profile — they belong only on
  // profiles that actually have a trackpad (vive, index).
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

  suggest_bindings("/interaction_profiles/valve/index_controller",
                   {
                       {m_action_primary_click, "/user/hand/left/input/a/click"},
                       {m_action_secondary_click, "/user/hand/left/input/b/click"},
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
  m_action_trigger_click = XR_NULL_HANDLE;
  m_action_squeeze_click = XR_NULL_HANDLE;
  m_action_trigger_value = XR_NULL_HANDLE;
  m_action_squeeze_value = XR_NULL_HANDLE;
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
  m_haptics_active = {false, false};
  Common::VR::OpenXRInputState::Reset();
}

void OpenXRManager::UpdateHaptics()
{
  if (!m_session_running || m_session == XR_NULL_HANDLE || m_action_haptic == XR_NULL_HANDLE)
    return;

  const auto haptics = Common::VR::OpenXRInputState::GetHaptics();

  // Re-send short pulses while active to approximate continuous rumble.
  constexpr XrDuration vibration_duration_ns = 50'000'000;

  for (size_t hand = 0; hand < m_input_hand_paths.size(); ++hand)
  {
    const XrPath hand_path = m_input_hand_paths[hand];
    if (hand_path == XR_NULL_PATH)
      continue;

    const float amplitude = std::clamp(haptics.amplitude[hand], 0.0f, 1.0f);

    XrHapticActionInfo action_info{XR_TYPE_HAPTIC_ACTION_INFO};
    action_info.action = m_action_haptic;
    action_info.subactionPath = hand_path;

    if (amplitude > 0.001f)
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
  if (XR_FAILED(sync_result))
  {
    ResetInputActionsState();
    return;
  }

  std::array<Common::VR::OpenXRControllerState, 2> controllers{};

  const auto locate_space_state = [this](XrSpace space, Common::VR::OpenXRPoseState* pose_state,
                                         Common::VR::OpenXRVelocityState* velocity_state) {
    if (space == XR_NULL_HANDLE || m_reference_space == XR_NULL_HANDLE)
      return;

    XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    location.next = &velocity;

    if (XR_FAILED(
            xrLocateSpace(space, m_reference_space, m_frame_state.predictedDisplayTime, &location)))
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

    bool action_seen = false;
    const auto get_boolean = [this, hand_path, &action_seen](XrAction action) -> bool {
      XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
      get_info.action = action;
      get_info.subactionPath = hand_path;
      XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
      if (XR_FAILED(xrGetActionStateBoolean(m_session, &get_info, &state)))
        return false;
      action_seen |= (state.isActive == XR_TRUE);
      return state.currentState == XR_TRUE;
    };

    const auto get_float = [this, hand_path, &action_seen](XrAction action) -> float {
      XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
      get_info.action = action;
      get_info.subactionPath = hand_path;
      XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
      if (XR_FAILED(xrGetActionStateFloat(m_session, &get_info, &state)))
        return 0.0f;
      action_seen |= (state.isActive == XR_TRUE);
      return state.currentState;
    };

    controller.primary_button = get_boolean(m_action_primary_click);
    controller.secondary_button = get_boolean(m_action_secondary_click);
    controller.menu_button = get_boolean(m_action_menu_click);
    controller.thumbstick_button = get_boolean(m_action_thumbstick_click);
    const bool trackpad_click = get_boolean(m_action_trackpad_click);
    controller.trackpad_touch = get_boolean(m_action_trackpad_touch);

    const bool trigger_click = get_boolean(m_action_trigger_click);
    const bool squeeze_click = get_boolean(m_action_squeeze_click);

    controller.trigger_value = std::clamp(get_float(m_action_trigger_value), 0.0f, 1.0f);
    controller.squeeze_value = std::clamp(get_float(m_action_squeeze_value), 0.0f, 1.0f);
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
        m_home_position = {0.0f, PRIMEGUN_DEFAULT_STANDING_HEIGHT_M, 0.0f};
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
    // Immutable copy at the same (identity) origin used to measure the head for full recenters.
    if (XR_FAILED(xrCreateReferenceSpace(m_session, &space_info, &m_recenter_base_space)))
      m_recenter_base_space = XR_NULL_HANDLE;  // recenter becomes a no-op rather than crashing
    m_home_position = {0.0f, PRIMEGUN_DEFAULT_STANDING_HEIGHT_M, 0.0f};
    m_home_set = true;
    INFO_LOG_FMT(OPENXR,
                 "OpenXR: Using stage reference space for PrimedGun play-space origin with default "
                 "startup height {:.2f}m.",
                 PRIMEGUN_DEFAULT_STANDING_HEIGHT_M);
    return true;
  }

  char result_string[XR_MAX_RESULT_STRING_SIZE]{};
  xrResultToString(m_instance, result, result_string);
  WARN_LOG_FMT(OPENXR, "OpenXR: Stage reference space unavailable ({}); falling back to local space.",
               result_string);

  space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  XR_CHECK(xrCreateReferenceSpace(m_session, &space_info, &m_reference_space));
  if (XR_FAILED(xrCreateReferenceSpace(m_session, &space_info, &m_recenter_base_space)))
    m_recenter_base_space = XR_NULL_HANDLE;
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

    case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB:
    {
      // The runtime changed the panel rate (our request, or a system/thermal decision). Keep the
      // recorded display period in sync so anything reading GetNativeDisplayPeriodMs stays accurate.
      const auto& ev = *reinterpret_cast<const XrEventDataDisplayRefreshRateChangedFB*>(&event);
      if (ev.toDisplayRefreshRate > 0.0f)
        m_native_display_period_ms = 1000.0 / ev.toDisplayRefreshRate;
      INFO_LOG_FMT(OPENXR, "OpenXR: display refresh rate changed {:g} Hz → {:g} Hz.",
                   ev.fromDisplayRefreshRate, ev.toDisplayRefreshRate);
      break;
    }

    case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
    {
      // The runtime is moving a reference space's origin — this is what the Quest "Reset View"
      // (Meta-button hold) raises. Respond like most apps do: recentre the player to the
      // play-space centre, face them forward and reset eye height. Applied on the render thread.
      const auto& ev = *reinterpret_cast<const XrEventDataReferenceSpaceChangePending*>(&event);
      INFO_LOG_FMT(OPENXR, "OpenXR: Reference space change pending (type={}) — requesting recenter.",
                   static_cast<int>(ev.referenceSpaceType));
      RequestFullRecenter();
      break;
    }

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
      // Re-applied on every transition to READY (levels can reset across focus loss).
      ApplyPerformanceLevelHints();
      // Request the configured HMD panel refresh rate now that the session is running
      // (xrRequestDisplayRefreshRateFB requires a running session).
      ApplyDisplayRefreshRate();
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

void OpenXRManager::ApplyPerformanceLevelHints()
{
  // Only meaningful when XR_EXT_performance_settings was enabled at instance creation
  // (currently the Android/Quest Vulkan path). No-op everywhere else.
  if (m_session == XR_NULL_HANDLE ||
      !IsExtensionEnabled(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME))
  {
    return;
  }

  PFN_xrPerfSettingsSetPerformanceLevelEXT set_level = nullptr;
  if (XR_FAILED(xrGetInstanceProcAddr(m_instance, "xrPerfSettingsSetPerformanceLevelEXT",
                                      reinterpret_cast<PFN_xrVoidFunction*>(&set_level))) ||
      set_level == nullptr)
  {
    WARN_LOG_FMT(OPENXR, "OpenXR: xrPerfSettingsSetPerformanceLevelEXT not exposed by the loader.");
    return;
  }

  // GPU: this workload is GPU-bound at 2x, so request the highest sustainable clock.
  // CPU: SUSTAINED_HIGH by default; the "level 5" hint opts into BOOST, which pushes the CPU
  // harder for the CPU-bound Gekko JIT at the cost of more aggressive thermal throttling.
  const bool cpu_boost = Config::Get(Config::GFX_VR_QUEST_CPU_LEVEL_5_HINT);
  const XrPerfSettingsLevelEXT cpu_level =
      cpu_boost ? XR_PERF_SETTINGS_LEVEL_BOOST_EXT : XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT;
  const XrPerfSettingsLevelEXT gpu_level = XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT;

  const XrResult cpu_result =
      set_level(m_session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, cpu_level);
  const XrResult gpu_result =
      set_level(m_session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, gpu_level);

  INFO_LOG_FMT(OPENXR,
               "OpenXR: performance levels set (CPU={} [{}], GPU=SUSTAINED_HIGH [{}]).",
               cpu_boost ? "BOOST" : "SUSTAINED_HIGH", static_cast<int>(cpu_result),
               static_cast<int>(gpu_result));
}

void OpenXRManager::ApplyDisplayRefreshRate()
{
  // Only meaningful when XR_FB_display_refresh_rate was enabled at instance creation (the
  // Android/Quest path). No-op everywhere else (desktop runtimes usually lack it).
  if (m_session == XR_NULL_HANDLE ||
      !IsExtensionEnabled(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME))
  {
    return;
  }

  PFN_xrEnumerateDisplayRefreshRatesFB enumerate = nullptr;
  PFN_xrGetDisplayRefreshRateFB get_rate = nullptr;
  PFN_xrRequestDisplayRefreshRateFB request_rate = nullptr;
  if (XR_FAILED(xrGetInstanceProcAddr(m_instance, "xrEnumerateDisplayRefreshRatesFB",
                                      reinterpret_cast<PFN_xrVoidFunction*>(&enumerate))) ||
      XR_FAILED(xrGetInstanceProcAddr(m_instance, "xrGetDisplayRefreshRateFB",
                                      reinterpret_cast<PFN_xrVoidFunction*>(&get_rate))) ||
      XR_FAILED(xrGetInstanceProcAddr(m_instance, "xrRequestDisplayRefreshRateFB",
                                      reinterpret_cast<PFN_xrVoidFunction*>(&request_rate))) ||
      enumerate == nullptr || get_rate == nullptr || request_rate == nullptr)
  {
    WARN_LOG_FMT(OPENXR,
                 "OpenXR: XR_FB_display_refresh_rate functions not exposed by the loader.");
    return;
  }

  // Enumerate the rates the runtime/panel actually supports (two-call idiom).
  uint32_t count = 0;
  if (XR_FAILED(enumerate(m_session, 0, &count, nullptr)) || count == 0)
  {
    WARN_LOG_FMT(OPENXR, "OpenXR: no supported display refresh rates reported.");
    return;
  }
  std::vector<float> rates(count);
  if (XR_FAILED(enumerate(m_session, count, &count, rates.data())))
  {
    WARN_LOG_FMT(OPENXR, "OpenXR: xrEnumerateDisplayRefreshRatesFB failed.");
    return;
  }

  // Current/active rate, for logging and to seed m_native_display_period_ms even on Auto.
  float current_rate = 0.0f;
  get_rate(m_session, &current_rate);
  if (current_rate > 0.0f)
    m_native_display_period_ms = 1000.0 / current_rate;

  std::string rate_list;
  for (size_t i = 0; i < rates.size(); ++i)
  {
    if (i != 0)
      rate_list += ", ";
    rate_list += std::to_string(static_cast<int>(std::lround(rates[i])));
  }

  const int configured =
      Config::NormalizeVRDisplayRefreshRate(Config::Get(Config::GFX_VR_DISPLAY_REFRESH_RATE));
  if (configured == Config::GFX_VR_DISPLAY_REFRESH_RATE_AUTO)
  {
    INFO_LOG_FMT(OPENXR,
                 "OpenXR: display refresh rate Auto — leaving runtime default {:g} Hz "
                 "(supported: {}).",
                 current_rate, rate_list);
    return;
  }

  // Pick the supported rate closest to the configured target (exact match if available), so a
  // request for an unsupported rate degrades to the nearest the panel actually offers.
  const float target = static_cast<float>(configured);
  float chosen = rates[0];
  float best_delta = std::abs(rates[0] - target);
  for (const float rate : rates)
  {
    const float delta = std::abs(rate - target);
    if (delta < best_delta)
    {
      best_delta = delta;
      chosen = rate;
    }
  }

  const XrResult result = request_rate(m_session, chosen);
  if (XR_SUCCEEDED(result))
  {
    m_native_display_period_ms = 1000.0 / chosen;
    INFO_LOG_FMT(OPENXR,
                 "OpenXR: requested display refresh rate {:g} Hz (target {} Hz; was {:g} Hz; "
                 "supported: {}).",
                 chosen, configured, current_rate, rate_list);
  }
  else
  {
    WARN_LOG_FMT(OPENXR,
                 "OpenXR: xrRequestDisplayRefreshRateFB({:g}) failed [{}] (supported: {}).",
                 chosen, static_cast<int>(result), rate_list);
  }
}

bool OpenXRManager::RegisterCurrentAndroidThread(AndroidThreadType thread_type,
                                                 const char* thread_name)
{
#if defined(ANDROID)
  // Only meaningful when XR_KHR_android_thread_settings was enabled at instance creation (the
  // Quest/Android Vulkan path). No-op everywhere else.
  if (m_session == XR_NULL_HANDLE ||
      !IsExtensionEnabled(XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME))
  {
    return false;
  }

  PFN_xrSetAndroidApplicationThreadKHR set_thread = nullptr;
  if (XR_FAILED(xrGetInstanceProcAddr(m_instance, "xrSetAndroidApplicationThreadKHR",
                                      reinterpret_cast<PFN_xrVoidFunction*>(&set_thread))) ||
      set_thread == nullptr)
  {
    WARN_LOG_FMT(OPENXR,
                 "OpenXR: xrSetAndroidApplicationThreadKHR not exposed by the loader; '{}' left "
                 "unmarked.",
                 thread_name);
    return false;
  }

  const bool is_app_main = thread_type == AndroidThreadType::ApplicationMain;
  const XrAndroidThreadTypeKHR xr_type = is_app_main ?
                                             XR_ANDROID_THREAD_TYPE_APPLICATION_MAIN_KHR :
                                             XR_ANDROID_THREAD_TYPE_RENDERER_MAIN_KHR;
  const uint32_t tid = static_cast<uint32_t>(gettid());
  const XrResult result = set_thread(m_session, xr_type, tid);
  INFO_LOG_FMT(OPENXR, "OpenXR: marked thread '{}' (tid {}) as {} [{}].", thread_name, tid,
               is_app_main ? "APPLICATION_MAIN" : "RENDERER_MAIN", static_cast<int>(result));
  return XR_SUCCEEDED(result);
#else
  (void)thread_type;
  (void)thread_name;
  return true;
#endif
}

bool OpenXRManager::ApplyFoveationProfile(XrSwapchain swapchain)
{
  if (m_session == XR_NULL_HANDLE || swapchain == XR_NULL_HANDLE)
    return false;

  // Needs the profile-creation + swapchain-state-update FB extensions (with the level/dynamic
  // config). These are Quest-only in practice; the backend only enables them when the runtime
  // advertises all three, so this is a no-op on desktop/PCVR.
  if (!IsExtensionEnabled(XR_FB_FOVEATION_EXTENSION_NAME) ||
      !IsExtensionEnabled(XR_FB_FOVEATION_CONFIGURATION_EXTENSION_NAME) ||
      !IsExtensionEnabled(XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME))
  {
    return false;
  }

  const int level_setting = Config::Get(Config::GFX_VR_FOVEATION_LEVEL);
  if (level_setting <= Config::GFX_VR_FOVEATION_LEVEL_OFF)
    return false;

  XrFoveationLevelFB level = XR_FOVEATION_LEVEL_HIGH_FB;
  switch (level_setting)
  {
  case Config::GFX_VR_FOVEATION_LEVEL_LOW:
    level = XR_FOVEATION_LEVEL_LOW_FB;
    break;
  case Config::GFX_VR_FOVEATION_LEVEL_MEDIUM:
    level = XR_FOVEATION_LEVEL_MEDIUM_FB;
    break;
  case Config::GFX_VR_FOVEATION_LEVEL_HIGH:
  default:
    level = XR_FOVEATION_LEVEL_HIGH_FB;
    break;
  }

  PFN_xrCreateFoveationProfileFB create_profile = nullptr;
  PFN_xrDestroyFoveationProfileFB destroy_profile = nullptr;
  PFN_xrUpdateSwapchainFB update_swapchain = nullptr;
  if (XR_FAILED(xrGetInstanceProcAddr(m_instance, "xrCreateFoveationProfileFB",
                                      reinterpret_cast<PFN_xrVoidFunction*>(&create_profile))) ||
      XR_FAILED(xrGetInstanceProcAddr(m_instance, "xrDestroyFoveationProfileFB",
                                      reinterpret_cast<PFN_xrVoidFunction*>(&destroy_profile))) ||
      XR_FAILED(xrGetInstanceProcAddr(m_instance, "xrUpdateSwapchainFB",
                                      reinterpret_cast<PFN_xrVoidFunction*>(&update_swapchain))) ||
      create_profile == nullptr || destroy_profile == nullptr || update_swapchain == nullptr)
  {
    WARN_LOG_FMT(OPENXR, "OpenXR: FB foveation functions not exposed by the loader.");
    return false;
  }

  // Fixed (non-eye-tracked) foveation: a single static level profile applied to the swapchain.
  XrFoveationLevelProfileCreateInfoFB level_info{XR_TYPE_FOVEATION_LEVEL_PROFILE_CREATE_INFO_FB};
  level_info.level = level;
  level_info.verticalOffset = 0.0f;
  level_info.dynamic = XR_FOVEATION_DYNAMIC_DISABLED_FB;

  XrFoveationProfileCreateInfoFB profile_info{XR_TYPE_FOVEATION_PROFILE_CREATE_INFO_FB};
  profile_info.next = &level_info;

  XrFoveationProfileFB profile = XR_NULL_HANDLE;
  XrResult result = create_profile(m_session, &profile_info, &profile);
  if (XR_FAILED(result))
  {
    WARN_LOG_FMT(OPENXR, "OpenXR: xrCreateFoveationProfileFB failed ({}).",
                 static_cast<int>(result));
    return false;
  }

  XrSwapchainStateFoveationFB foveation_state{XR_TYPE_SWAPCHAIN_STATE_FOVEATION_FB};
  foveation_state.profile = profile;
  result = update_swapchain(
      swapchain, reinterpret_cast<const XrSwapchainStateBaseHeaderFB*>(&foveation_state));

  // The profile only has to live across the update call; the runtime keeps the resulting state.
  destroy_profile(profile);

  if (XR_FAILED(result))
  {
    WARN_LOG_FMT(OPENXR, "OpenXR: xrUpdateSwapchainFB (foveation) failed ({}).",
                 static_cast<int>(result));
    return false;
  }

  INFO_LOG_FMT(OPENXR, "OpenXR: applied fixed-foveation level {} to swapchain.", level_setting);
  return true;
}

bool OpenXRManager::WaitFrame()
{
  XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
  m_frame_state = {XR_TYPE_FRAME_STATE};
  XR_CHECK(xrWaitFrame(m_session, &wait_info, &m_frame_state));
  UpdateInputActions();
  return true;
}

bool OpenXRManager::BeginFrame()
{
  XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
  XR_CHECK(xrBeginFrame(m_session, &begin_info));
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

  XR_CHECK(xrEndFrame(m_session, &end_info));
  return true;
}

bool OpenXRManager::EndFrameDetached(XrTime display_time,
                                     XrEnvironmentBlendMode environment_blend_mode,
                                     bool should_render,
                                     const std::vector<XrCompositionLayerBaseHeader*>& layers)
{
  XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
  end_info.displayTime = display_time;
  end_info.environmentBlendMode = environment_blend_mode;

  if (should_render)
  {
    end_info.layerCount = static_cast<uint32_t>(layers.size());
    end_info.layers = layers.data();
  }

  XR_CHECK(xrEndFrame(m_session, &end_info));
  return true;
}

bool OpenXRManager::LocateViews()
{
  // Apply a pending full recenter before locating, so this frame already renders recentered.
  if (m_full_recenter_requested.exchange(false, std::memory_order_acq_rel))
    ApplyFullRecenter();

  XrViewLocateInfo locate_info{XR_TYPE_VIEW_LOCATE_INFO};
  locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  locate_info.displayTime = m_frame_state.predictedDisplayTime;
  locate_info.space = m_reference_space;

  XrViewState view_state{XR_TYPE_VIEW_STATE};
  uint32_t view_count = static_cast<uint32_t>(m_views.size());
  m_views.fill({XR_TYPE_VIEW});
  m_eye_views_valid = false;

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

void OpenXRManager::RequestFullRecenter()
{
  m_full_recenter_requested.store(true, std::memory_order_release);
}

void OpenXRManager::ApplyFullRecenter()
{
  if (m_session == XR_NULL_HANDLE || m_recenter_base_space == XR_NULL_HANDLE)
    return;

  // Measure the current head pose against the immutable base space (absolute play-space coords),
  // so chained recenters never accumulate drift.
  XrViewLocateInfo locate_info{XR_TYPE_VIEW_LOCATE_INFO};
  locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  locate_info.displayTime = m_frame_state.predictedDisplayTime;
  locate_info.space = m_recenter_base_space;

  XrViewState view_state{XR_TYPE_VIEW_STATE};
  std::array<XrView, 2> views{};
  for (auto& view : views)
    view.type = XR_TYPE_VIEW;
  uint32_t count = 0;
  if (XR_FAILED(xrLocateViews(m_session, &locate_info, &view_state,
                              static_cast<uint32_t>(views.size()), &count, views.data())) ||
      count < 2)
  {
    return;
  }
  constexpr XrViewStateFlags required_flags =
      XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
  if ((view_state.viewStateFlags & required_flags) != required_flags)
    return;

  const float head_x = 0.5f * (views[0].pose.position.x + views[1].pose.position.x);
  const float head_y = 0.5f * (views[0].pose.position.y + views[1].pose.position.y);
  const float head_z = 0.5f * (views[0].pose.position.z + views[1].pose.position.z);

  // Yaw (heading) about the up axis, derived from the head's horizontal forward (-Z) vector so
  // pitch/roll are dropped — the recentered view is level. Forward = R(q)*(0,0,-1):
  //   fwd.x = -2(xz + wy),  fwd.z = -(1 - 2(x^2 + y^2))
  // and Ry(yaw)*(0,0,-1) = (-sin yaw, 0, -cos yaw), so yaw = atan2(-fwd.x, -fwd.z).
  const XrQuaternionf& q = views[0].pose.orientation;
  const float yaw = std::atan2(2.0f * (q.x * q.z + q.w * q.y),
                               1.0f - 2.0f * (q.x * q.x + q.y * q.y));
  const float half_yaw = 0.5f * yaw;

  XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  space_info.referenceSpaceType =
      m_reference_space_is_stage ? XR_REFERENCE_SPACE_TYPE_STAGE : XR_REFERENCE_SPACE_TYPE_LOCAL;
  // New origin sits at the head's ground projection, yaw-aligned to where they look. Keeping y at 0
  // leaves the space floor-aligned; eye height is handled by m_home_position below.
  space_info.poseInReferenceSpace.orientation = {0.0f, std::sin(half_yaw), 0.0f, std::cos(half_yaw)};
  space_info.poseInReferenceSpace.position = {head_x, 0.0f, head_z};

  XrSpace new_space = XR_NULL_HANDLE;
  if (XR_FAILED(xrCreateReferenceSpace(m_session, &space_info, &new_space)) ||
      new_space == XR_NULL_HANDLE)
  {
    WARN_LOG_FMT(OPENXR, "OpenXR: Full recenter failed to create reference space.");
    return;
  }

  // The previous active space may still be referenced by an in-flight (already-submitted) frame, so
  // it can't be freed immediately. Free the one swapped out at the prior recenter (long since
  // finalized) and stash the current one in its place.
  if (m_old_reference_space != XR_NULL_HANDLE)
    xrDestroySpace(m_old_reference_space);
  m_old_reference_space = m_reference_space;
  m_reference_space = new_space;

  // Reset eye height in software: yaw rotates about Y and the translation is X/Z only, so the head's
  // height in the new space is unchanged (= head_y). Anchor the home there so the eye maps to the
  // game's standing eye level.
  m_home_position = {0.0f, head_y, 0.0f};
  m_home_set = true;

  INFO_LOG_FMT(OPENXR,
               "OpenXR: Full recenter (x={:.3f} z={:.3f} yaw={:.1f}deg height={:.3f}).", head_x,
               head_z, yaw * 57.2957795f, head_y);
}

void OpenXRManager::GetEyeProjectionRows(
    float units_per_meter,
    std::array<std::array<float, 4>, 4>& out_proj_rows,
    std::array<std::array<float, 4>, 2>& out_z_rows) const
{
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
      m_home_position = {0.0f, PRIMEGUN_DEFAULT_STANDING_HEIGHT_M, 0.0f};
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
