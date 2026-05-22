#include "OpenXrHooks.h"

#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_VULKAN

#include <windows.h>
#include <psapi.h>
#include <d3d11.h>
#include <vulkan/vulkan.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace PrimedGun::Hook {
void Log(std::wstring_view message);
}

namespace PrimedGun::Hook::OpenXrHooks {
namespace {

constexpr uint32_t kButtonTriggerClick = 1u << 0;
constexpr uint32_t kButtonThumbstickClick = 1u << 1;
constexpr uint32_t kButtonA = 1u << 2;
constexpr uint32_t kButtonB = 1u << 3;

using PFN_xrGetInstanceProcAddr = XrResult(XRAPI_PTR*)(XrInstance, const char*, PFN_xrVoidFunction*);
using PFN_GetProcAddress = FARPROC(WINAPI*)(HMODULE, LPCSTR);

PFN_xrGetInstanceProcAddr g_realGetInstanceProcAddr = nullptr;
PFN_xrNegotiateLoaderRuntimeInterface g_realNegotiateLoaderRuntimeInterface = nullptr;
PFN_GetProcAddress g_realGetProcAddress = nullptr;
PFN_xrCreateInstance g_realCreateInstance = nullptr;
PFN_xrCreateSession g_realCreateSession = nullptr;
PFN_xrCreateSwapchain g_realCreateSwapchain = nullptr;
PFN_xrEnumerateSwapchainFormats g_realEnumerateSwapchainFormats = nullptr;
PFN_xrDestroySwapchain g_realDestroySwapchain = nullptr;
PFN_xrCreateReferenceSpace g_realCreateReferenceSpace = nullptr;
PFN_xrEnumerateSwapchainImages g_realEnumerateSwapchainImages = nullptr;
PFN_xrAcquireSwapchainImage g_realAcquireSwapchainImage = nullptr;
PFN_xrWaitSwapchainImage g_realWaitSwapchainImage = nullptr;
PFN_xrReleaseSwapchainImage g_realReleaseSwapchainImage = nullptr;
PFN_xrEndFrame g_realEndFrame = nullptr;
PFN_xrStringToPath g_realStringToPath = nullptr;
PFN_xrCreateAction g_realCreateAction = nullptr;
PFN_xrCreateActionSpace g_realCreateActionSpace = nullptr;
PFN_xrGetActionStateBoolean g_realGetActionStateBoolean = nullptr;
PFN_xrGetActionStateFloat g_realGetActionStateFloat = nullptr;
PFN_xrGetActionStateVector2f g_realGetActionStateVector2f = nullptr;
PFN_xrLocateViews g_realLocateViews = nullptr;
PFN_xrLocateSpace g_realLocateSpace = nullptr;
PFN_xrEnumerateInstanceExtensionProperties g_realEnumerateInstanceExtensionProperties = nullptr;
PFN_xrEnumerateDisplayRefreshRatesFB g_xrEnumerateDisplayRefreshRatesFB = nullptr;
PFN_xrGetDisplayRefreshRateFB g_xrGetDisplayRefreshRateFB = nullptr;
PFN_xrRequestDisplayRefreshRateFB g_xrRequestDisplayRefreshRateFB = nullptr;

std::atomic<bool> g_installed = false;
std::atomic<bool> g_runtimeInstalled = false;
std::atomic<bool> g_getProcAddressInstalled = false;
std::atomic<bool> g_inlineDetourInstalled = false;
std::atomic<SharedState*> g_sharedState = nullptr;
std::mutex g_mutex;
std::unordered_map<XrPath, std::string> g_paths;
std::unordered_map<XrAction, std::string> g_actions;
std::unordered_map<XrSpace, std::string> g_spaces;
uint64_t g_generation = 0;
uint64_t g_lastLogMs = 0;
uint64_t g_lastInstallCheckMs = 0;
XrSession g_dolphinSession = XR_NULL_HANDLE;
XrInstance g_dolphinInstance = XR_NULL_HANDLE;
XrSpace g_dolphinReferenceSpace = XR_NULL_HANDLE;
uint8_t* g_dolphinOpenXrManager = nullptr;
uint64_t g_lastManagerScanMs = 0;
bool g_dolphinHomeZeroed = false;
bool g_dolphinReferenceSpaceIsStage = false;
uint32_t g_startupCenterSpoofFrames = 0;
uint32_t g_appliedViewHeightGeneration = 0;
uint32_t g_validLocateViewFrames = 0;
uint8_t* g_lastManagerCandidate = nullptr;
uint32_t g_managerStableScans = 0;
uint64_t g_lastHeightRecenterLogMs = 0;
bool g_heightOnlyRecenterPatchAttempted = false;
bool g_heightOnlyRecenterPatchInstalled = false;
uint64_t g_leftGripPoseMs = 0;
uint64_t g_rightGripPoseMs = 0;
bool g_displayRefreshRateExtensionAvailable = false;
bool g_displayRefreshRateExtensionEnabled = false;
int g_lastDisplayRefreshRatePreference = -1;

enum class GraphicsApi {
    Unknown,
    D3D11,
    Vulkan,
};

struct QuadLayerState {
    XrSession session = XR_NULL_HANDLE;
    GraphicsApi graphicsApi = GraphicsApi::Unknown;
    ID3D11Device* d3dDevice = nullptr;
    ID3D11DeviceContext* d3dContext = nullptr;
    VkInstance vkInstance = VK_NULL_HANDLE;
    VkPhysicalDevice vkPhysicalDevice = VK_NULL_HANDLE;
    VkDevice vkDevice = VK_NULL_HANDLE;
    VkQueue vkQueue = VK_NULL_HANDLE;
    uint32_t vkQueueFamilyIndex = 0;
    uint32_t vkQueueIndex = 0;
    VkCommandPool vkCommandPool = VK_NULL_HANDLE;
    VkBuffer vkStagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vkStagingMemory = VK_NULL_HANDLE;
    VkDeviceSize vkStagingSize = 0;
    XrSwapchain swapchain = XR_NULL_HANDLE;
    XrSwapchain promptSwapchain = XR_NULL_HANDLE;
    int64_t format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uint32_t width = 1024;
    uint32_t height = 384;
    uint32_t promptWidth = 1024;
    uint32_t promptHeight = 384;
    XrSpace space = XR_NULL_HANDLE;
    uint64_t promptFirstFrameMs = 0;
    uint32_t contentKind = 0;
    uint32_t uploadedMenuGeneration = 0;
    bool promptTextureReady = false;
    bool textureReady = false;
    bool warnedNoTexture = false;
    bool warnedCreateFailed = false;
    bool loggedSelectedFormat = false;
    bool warnedVulkanResolveFailed = false;
    uint64_t appendedFrames = 0;
};

QuadLayerState g_quadLayer;

struct HookEyeView {
    XrPosef pose;
    XrFovf fov;
};

struct DolphinOpenXrManagerLayout {
    XrInstance instance;
    XrSystemId systemId;
    XrSession session;
    XrSpace referenceSpace;
    void* swapchain;
    XrSessionState sessionState;
    bool sessionRunning;
    bool exitRenderLoop;
    XrFrameState frameState;
    XrActionSet inputActionSet;
    std::array<XrPath, 2> inputHandPaths;
    XrAction actionPrimaryClick;
    XrAction actionSecondaryClick;
    XrAction actionMenuClick;
    XrAction actionThumbstickClick;
    XrAction actionTriggerClick;
    XrAction actionSqueezeClick;
    XrAction actionTriggerValue;
    XrAction actionSqueezeValue;
    XrAction actionThumbstickX;
    XrAction actionThumbstickY;
    XrAction actionAimPose;
    XrAction actionGripPose;
    XrAction actionHaptic;
    std::array<XrSpace, 2> aimSpaces;
    std::array<XrSpace, 2> gripSpaces;
    std::array<bool, 2> hapticsActive;
    std::array<XrViewConfigurationView, 2> viewConfigViews;
    std::array<XrView, 2> views;
    std::array<HookEyeView, 2> eyeViews;
    bool homeSet;
    XrVector3f homePosition;
    std::atomic<bool> recenterRequested;
    XrEnvironmentBlendMode activeBlendMode;
};

static_assert(offsetof(DolphinOpenXrManagerLayout, referenceSpace) == 24);

PFN_vkGetInstanceProcAddr g_vkGetInstanceProcAddr = nullptr;
PFN_vkGetDeviceProcAddr g_vkGetDeviceProcAddr = nullptr;
PFN_vkGetDeviceQueue g_vkGetDeviceQueue = nullptr;
PFN_vkCreateCommandPool g_vkCreateCommandPool = nullptr;
PFN_vkDestroyCommandPool g_vkDestroyCommandPool = nullptr;
PFN_vkAllocateCommandBuffers g_vkAllocateCommandBuffers = nullptr;
PFN_vkFreeCommandBuffers g_vkFreeCommandBuffers = nullptr;
PFN_vkBeginCommandBuffer g_vkBeginCommandBuffer = nullptr;
PFN_vkEndCommandBuffer g_vkEndCommandBuffer = nullptr;
PFN_vkCmdPipelineBarrier g_vkCmdPipelineBarrier = nullptr;
PFN_vkCmdCopyBufferToImage g_vkCmdCopyBufferToImage = nullptr;
PFN_vkQueueSubmit g_vkQueueSubmit = nullptr;
PFN_vkQueueWaitIdle g_vkQueueWaitIdle = nullptr;
PFN_vkCreateBuffer g_vkCreateBuffer = nullptr;
PFN_vkDestroyBuffer g_vkDestroyBuffer = nullptr;
PFN_vkGetBufferMemoryRequirements g_vkGetBufferMemoryRequirements = nullptr;
PFN_vkAllocateMemory g_vkAllocateMemory = nullptr;
PFN_vkFreeMemory g_vkFreeMemory = nullptr;
PFN_vkBindBufferMemory g_vkBindBufferMemory = nullptr;
PFN_vkMapMemory g_vkMapMemory = nullptr;
PFN_vkUnmapMemory g_vkUnmapMemory = nullptr;
PFN_vkGetPhysicalDeviceMemoryProperties g_vkGetPhysicalDeviceMemoryProperties = nullptr;

std::wstring Widen(std::string_view value) {
    return std::wstring(value.begin(), value.end());
}

std::wstring FormatPose(const XrPosef& pose) {
    wchar_t buffer[256] = {};
    swprintf_s(buffer, L"pos=(%.3f %.3f %.3f) quat=(%.3f %.3f %.3f %.3f)",
               pose.position.x, pose.position.y, pose.position.z,
               pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w);
    return buffer;
}

const wchar_t* GraphicsApiName(GraphicsApi api) {
    switch (api) {
    case GraphicsApi::D3D11:
        return L"D3D11";
    case GraphicsApi::Vulkan:
        return L"Vulkan";
    default:
        return L"Unknown";
    }
}

GraphicsApi DetectGraphicsApiFromSessionCreateInfo(const XrSessionCreateInfo* createInfo) {
    for (const XrBaseInStructure* next = createInfo ? static_cast<const XrBaseInStructure*>(createInfo->next) : nullptr;
         next; next = next->next) {
        if (next->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
            return GraphicsApi::D3D11;
        }
        if (next->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) {
            return GraphicsApi::Vulkan;
        }
    }
    return GraphicsApi::Unknown;
}

const XrGraphicsBindingD3D11KHR* FindD3D11Binding(const XrSessionCreateInfo* createInfo) {
    for (const XrBaseInStructure* next = createInfo ? static_cast<const XrBaseInStructure*>(createInfo->next) : nullptr;
         next; next = next->next) {
        if (next->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
            return reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(next);
        }
    }
    return nullptr;
}

const XrGraphicsBindingVulkanKHR* FindVulkanBinding(const XrSessionCreateInfo* createInfo) {
    for (const XrBaseInStructure* next = createInfo ? static_cast<const XrBaseInStructure*>(createInfo->next) : nullptr;
         next; next = next->next) {
        if (next->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) {
            return reinterpret_cast<const XrGraphicsBindingVulkanKHR*>(next);
        }
    }
    return nullptr;
}

void CleanupVulkanQuadResources() {
    if (!g_quadLayer.vkDevice) {
        g_quadLayer.vkCommandPool = VK_NULL_HANDLE;
        g_quadLayer.vkStagingBuffer = VK_NULL_HANDLE;
        g_quadLayer.vkStagingMemory = VK_NULL_HANDLE;
        g_quadLayer.vkStagingSize = 0;
        return;
    }
    if (g_quadLayer.vkStagingBuffer && g_vkDestroyBuffer) {
        g_vkDestroyBuffer(g_quadLayer.vkDevice, g_quadLayer.vkStagingBuffer, nullptr);
    }
    if (g_quadLayer.vkStagingMemory && g_vkFreeMemory) {
        g_vkFreeMemory(g_quadLayer.vkDevice, g_quadLayer.vkStagingMemory, nullptr);
    }
    if (g_quadLayer.vkCommandPool && g_vkDestroyCommandPool) {
        g_vkDestroyCommandPool(g_quadLayer.vkDevice, g_quadLayer.vkCommandPool, nullptr);
    }
    g_quadLayer.vkCommandPool = VK_NULL_HANDLE;
    g_quadLayer.vkStagingBuffer = VK_NULL_HANDLE;
    g_quadLayer.vkStagingMemory = VK_NULL_HANDLE;
    g_quadLayer.vkStagingSize = 0;
}

void DestroyPromptSwapchain() {
    if (g_quadLayer.swapchain != XR_NULL_HANDLE && g_realDestroySwapchain) {
        g_realDestroySwapchain(g_quadLayer.swapchain);
    }
    g_quadLayer.swapchain = XR_NULL_HANDLE;
    g_quadLayer.textureReady = false;
    g_quadLayer.contentKind = 0;
    g_quadLayer.uploadedMenuGeneration = 0;
}

void DestroyHeightPromptSwapchain() {
    if (g_quadLayer.promptSwapchain != XR_NULL_HANDLE && g_realDestroySwapchain) {
        g_realDestroySwapchain(g_quadLayer.promptSwapchain);
    }
    g_quadLayer.promptSwapchain = XR_NULL_HANDLE;
    g_quadLayer.promptTextureReady = false;
}

bool FindFrameProjectionSpace(const XrFrameEndInfo* frameEndInfo, XrSpace& space) {
    if (!frameEndInfo || !frameEndInfo->layers || frameEndInfo->layerCount == 0)
        return false;

    for (uint32_t i = 0; i < frameEndInfo->layerCount; ++i) {
        const XrCompositionLayerBaseHeader* layer = frameEndInfo->layers[i];
        if (layer && layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            space = reinterpret_cast<const XrCompositionLayerProjection*>(layer)->space;
            return space != XR_NULL_HANDLE;
        }
    }
    return false;
}

bool ResolveVulkanFunctions() {
    HMODULE vulkan = GetModuleHandleW(L"vulkan-1.dll");
    if (!vulkan) {
        vulkan = LoadLibraryW(L"vulkan-1.dll");
    }
    if (!vulkan) {
        return false;
    }

    if (!g_vkGetInstanceProcAddr) {
        g_vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            GetProcAddress(vulkan, "vkGetInstanceProcAddr"));
    }
    if (!g_vkGetDeviceProcAddr) {
        g_vkGetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
            GetProcAddress(vulkan, "vkGetDeviceProcAddr"));
    }
    if (!g_vkGetInstanceProcAddr) {
        return false;
    }
    if (!g_vkGetDeviceProcAddr) {
        g_vkGetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
            g_vkGetInstanceProcAddr(g_quadLayer.vkInstance, "vkGetDeviceProcAddr"));
    }
    g_vkGetPhysicalDeviceMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        g_vkGetInstanceProcAddr(g_quadLayer.vkInstance, "vkGetPhysicalDeviceMemoryProperties"));
    if (!g_vkGetPhysicalDeviceMemoryProperties) {
        g_vkGetPhysicalDeviceMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
            GetProcAddress(vulkan, "vkGetPhysicalDeviceMemoryProperties"));
    }
    if (!g_vkGetDeviceProcAddr || !g_vkGetPhysicalDeviceMemoryProperties || !g_quadLayer.vkDevice) {
        if (!g_quadLayer.warnedVulkanResolveFailed) {
            g_quadLayer.warnedVulkanResolveFailed = true;
            Log(std::wstring(L"OpenXR quad prompt Vulkan resolver base failed: getInstance=") +
                (g_vkGetInstanceProcAddr ? L"yes" : L"no") +
                L" getDevice=" + (g_vkGetDeviceProcAddr ? L"yes" : L"no") +
                L" memoryProps=" + (g_vkGetPhysicalDeviceMemoryProperties ? L"yes" : L"no") +
                L" device=" + (g_quadLayer.vkDevice ? L"yes" : L"no"));
        }
        return false;
    }

    auto deviceProc = [&](const char* name) {
        return g_vkGetDeviceProcAddr(g_quadLayer.vkDevice, name);
    };
    g_vkGetDeviceQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(deviceProc("vkGetDeviceQueue"));
    g_vkCreateCommandPool = reinterpret_cast<PFN_vkCreateCommandPool>(deviceProc("vkCreateCommandPool"));
    g_vkDestroyCommandPool = reinterpret_cast<PFN_vkDestroyCommandPool>(deviceProc("vkDestroyCommandPool"));
    g_vkAllocateCommandBuffers = reinterpret_cast<PFN_vkAllocateCommandBuffers>(deviceProc("vkAllocateCommandBuffers"));
    g_vkFreeCommandBuffers = reinterpret_cast<PFN_vkFreeCommandBuffers>(deviceProc("vkFreeCommandBuffers"));
    g_vkBeginCommandBuffer = reinterpret_cast<PFN_vkBeginCommandBuffer>(deviceProc("vkBeginCommandBuffer"));
    g_vkEndCommandBuffer = reinterpret_cast<PFN_vkEndCommandBuffer>(deviceProc("vkEndCommandBuffer"));
    g_vkCmdPipelineBarrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(deviceProc("vkCmdPipelineBarrier"));
    g_vkCmdCopyBufferToImage = reinterpret_cast<PFN_vkCmdCopyBufferToImage>(deviceProc("vkCmdCopyBufferToImage"));
    g_vkQueueSubmit = reinterpret_cast<PFN_vkQueueSubmit>(deviceProc("vkQueueSubmit"));
    g_vkQueueWaitIdle = reinterpret_cast<PFN_vkQueueWaitIdle>(deviceProc("vkQueueWaitIdle"));
    g_vkCreateBuffer = reinterpret_cast<PFN_vkCreateBuffer>(deviceProc("vkCreateBuffer"));
    g_vkDestroyBuffer = reinterpret_cast<PFN_vkDestroyBuffer>(deviceProc("vkDestroyBuffer"));
    g_vkGetBufferMemoryRequirements = reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(deviceProc("vkGetBufferMemoryRequirements"));
    g_vkAllocateMemory = reinterpret_cast<PFN_vkAllocateMemory>(deviceProc("vkAllocateMemory"));
    g_vkFreeMemory = reinterpret_cast<PFN_vkFreeMemory>(deviceProc("vkFreeMemory"));
    g_vkBindBufferMemory = reinterpret_cast<PFN_vkBindBufferMemory>(deviceProc("vkBindBufferMemory"));
    g_vkMapMemory = reinterpret_cast<PFN_vkMapMemory>(deviceProc("vkMapMemory"));
    g_vkUnmapMemory = reinterpret_cast<PFN_vkUnmapMemory>(deviceProc("vkUnmapMemory"));

    const bool ok = g_vkGetDeviceQueue && g_vkCreateCommandPool && g_vkDestroyCommandPool &&
        g_vkAllocateCommandBuffers && g_vkFreeCommandBuffers && g_vkBeginCommandBuffer &&
        g_vkEndCommandBuffer && g_vkCmdPipelineBarrier && g_vkCmdCopyBufferToImage &&
        g_vkQueueSubmit && g_vkQueueWaitIdle && g_vkCreateBuffer && g_vkDestroyBuffer &&
        g_vkGetBufferMemoryRequirements && g_vkAllocateMemory && g_vkFreeMemory &&
        g_vkBindBufferMemory && g_vkMapMemory && g_vkUnmapMemory;
    if (!ok && !g_quadLayer.warnedVulkanResolveFailed) {
        g_quadLayer.warnedVulkanResolveFailed = true;
        Log(std::wstring(L"OpenXR quad prompt Vulkan resolver missing: queue=") +
            (g_vkGetDeviceQueue ? L"yes" : L"no") +
            L" cmdPool=" + (g_vkCreateCommandPool ? L"yes" : L"no") +
            L" allocCmd=" + (g_vkAllocateCommandBuffers ? L"yes" : L"no") +
            L" barrier=" + (g_vkCmdPipelineBarrier ? L"yes" : L"no") +
            L" copy=" + (g_vkCmdCopyBufferToImage ? L"yes" : L"no") +
            L" submit=" + (g_vkQueueSubmit ? L"yes" : L"no") +
            L" buffer=" + (g_vkCreateBuffer ? L"yes" : L"no") +
            L" allocMem=" + (g_vkAllocateMemory ? L"yes" : L"no") +
            L" map=" + (g_vkMapMemory ? L"yes" : L"no"));
    }
    return ok;
}

bool FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties, uint32_t& memoryTypeIndex) {
    if (!g_vkGetPhysicalDeviceMemoryProperties || !g_quadLayer.vkPhysicalDevice) {
        return false;
    }
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    g_vkGetPhysicalDeviceMemoryProperties(g_quadLayer.vkPhysicalDevice, &memoryProperties);
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            memoryTypeIndex = i;
            return true;
        }
    }
    return false;
}

bool EnsureVulkanUploadResources(const std::vector<uint32_t>& pixels) {
    if (!ResolveVulkanFunctions()) {
        Log(L"OpenXR quad prompt failed to resolve Vulkan upload functions.");
        return false;
    }
    if (!g_quadLayer.vkQueue) {
        g_vkGetDeviceQueue(g_quadLayer.vkDevice, g_quadLayer.vkQueueFamilyIndex,
                           g_quadLayer.vkQueueIndex, &g_quadLayer.vkQueue);
    }
    if (!g_quadLayer.vkQueue) {
        Log(L"OpenXR quad prompt failed to get Vulkan queue.");
        return false;
    }

    if (!g_quadLayer.vkCommandPool) {
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = g_quadLayer.vkQueueFamilyIndex;
        const VkResult result = g_vkCreateCommandPool(g_quadLayer.vkDevice, &poolInfo, nullptr,
                                                      &g_quadLayer.vkCommandPool);
        if (result != VK_SUCCESS) {
            Log(L"OpenXR quad prompt failed to create Vulkan command pool result=" + std::to_wstring(result));
            return false;
        }
    }

    const VkDeviceSize uploadSize = static_cast<VkDeviceSize>(pixels.size() * sizeof(uint32_t));
    if (g_quadLayer.vkStagingBuffer && g_quadLayer.vkStagingSize < uploadSize) {
        if (g_vkDestroyBuffer)
            g_vkDestroyBuffer(g_quadLayer.vkDevice, g_quadLayer.vkStagingBuffer, nullptr);
        if (g_vkFreeMemory)
            g_vkFreeMemory(g_quadLayer.vkDevice, g_quadLayer.vkStagingMemory, nullptr);
        g_quadLayer.vkStagingBuffer = VK_NULL_HANDLE;
        g_quadLayer.vkStagingMemory = VK_NULL_HANDLE;
        g_quadLayer.vkStagingSize = 0;
    }

    if (!g_quadLayer.vkStagingBuffer) {
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = uploadSize;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkResult result = g_vkCreateBuffer(g_quadLayer.vkDevice, &bufferInfo, nullptr,
                                           &g_quadLayer.vkStagingBuffer);
        if (result != VK_SUCCESS) {
            Log(L"OpenXR quad prompt failed to create Vulkan staging buffer result=" + std::to_wstring(result));
            return false;
        }

        VkMemoryRequirements requirements{};
        g_vkGetBufferMemoryRequirements(g_quadLayer.vkDevice, g_quadLayer.vkStagingBuffer, &requirements);
        uint32_t memoryType = 0;
        if (!FindMemoryType(requirements.memoryTypeBits,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            memoryType)) {
            Log(L"OpenXR quad prompt failed to find Vulkan host-visible memory.");
            return false;
        }

        VkMemoryAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocateInfo.allocationSize = requirements.size;
        allocateInfo.memoryTypeIndex = memoryType;
        result = g_vkAllocateMemory(g_quadLayer.vkDevice, &allocateInfo, nullptr,
                                    &g_quadLayer.vkStagingMemory);
        if (result != VK_SUCCESS) {
            Log(L"OpenXR quad prompt failed to allocate Vulkan staging memory result=" + std::to_wstring(result));
            return false;
        }
        result = g_vkBindBufferMemory(g_quadLayer.vkDevice, g_quadLayer.vkStagingBuffer,
                                      g_quadLayer.vkStagingMemory, 0);
        if (result != VK_SUCCESS) {
            Log(L"OpenXR quad prompt failed to bind Vulkan staging memory result=" + std::to_wstring(result));
            return false;
        }
        g_quadLayer.vkStagingSize = uploadSize;
    }

    void* mapped = nullptr;
    const VkResult result = g_vkMapMemory(g_quadLayer.vkDevice, g_quadLayer.vkStagingMemory, 0,
                                          uploadSize, 0, &mapped);
    if (result != VK_SUCCESS || !mapped) {
        Log(L"OpenXR quad prompt failed to map Vulkan staging memory result=" + std::to_wstring(result));
        return false;
    }
    std::memcpy(mapped, pixels.data(), static_cast<size_t>(uploadSize));
    g_vkUnmapMemory(g_quadLayer.vkDevice, g_quadLayer.vkStagingMemory);
    return true;
}

bool UploadPromptToVulkanImage(VkImage image, uint32_t width, uint32_t height) {
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocateInfo.commandPool = g_quadLayer.vkCommandPool;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = 1;
    VkResult result = g_vkAllocateCommandBuffers(g_quadLayer.vkDevice, &allocateInfo, &commandBuffer);
    if (result != VK_SUCCESS) {
        Log(L"OpenXR quad prompt failed to allocate Vulkan command buffer result=" + std::to_wstring(result));
        return false;
    }

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = g_vkBeginCommandBuffer(commandBuffer, &beginInfo);
    if (result != VK_SUCCESS) {
        Log(L"OpenXR quad prompt failed to begin Vulkan command buffer result=" + std::to_wstring(result));
        g_vkFreeCommandBuffers(g_quadLayer.vkDevice, g_quadLayer.vkCommandPool, 1, &commandBuffer);
        return false;
    }

    VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toTransfer.srcAccessMask = 0;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = image;
    toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toTransfer.subresourceRange.baseMipLevel = 0;
    toTransfer.subresourceRange.levelCount = 1;
    toTransfer.subresourceRange.baseArrayLayer = 0;
    toTransfer.subresourceRange.layerCount = 1;
    g_vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toTransfer);

    VkBufferImageCopy copyRegion{};
    copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.imageSubresource.mipLevel = 0;
    copyRegion.imageSubresource.baseArrayLayer = 0;
    copyRegion.imageSubresource.layerCount = 1;
    copyRegion.imageExtent = {width, height, 1};
    g_vkCmdCopyBufferToImage(commandBuffer, g_quadLayer.vkStagingBuffer, image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

    VkImageMemoryBarrier toShader{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toShader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShader.image = image;
    toShader.subresourceRange = toTransfer.subresourceRange;
    g_vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toShader);

    result = g_vkEndCommandBuffer(commandBuffer);
    if (result != VK_SUCCESS) {
        Log(L"OpenXR quad prompt failed to end Vulkan command buffer result=" + std::to_wstring(result));
        g_vkFreeCommandBuffers(g_quadLayer.vkDevice, g_quadLayer.vkCommandPool, 1, &commandBuffer);
        return false;
    }

    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    result = g_vkQueueSubmit(g_quadLayer.vkQueue, 1, &submitInfo, VK_NULL_HANDLE);
    if (result == VK_SUCCESS) {
        result = g_vkQueueWaitIdle(g_quadLayer.vkQueue);
    }
    g_vkFreeCommandBuffers(g_quadLayer.vkDevice, g_quadLayer.vkCommandPool, 1, &commandBuffer);
    if (result != VK_SUCCESS) {
        Log(L"OpenXR quad prompt failed to submit Vulkan upload result=" + std::to_wstring(result));
        return false;
    }
    return true;
}

std::array<uint8_t, 7> OverlayGlyphRows(char ch) {
    switch (ch) {
    case 'A': return {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    case 'B': return {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E};
    case 'C': return {0x0F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0F};
    case 'D': return {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E};
    case 'E': return {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
    case 'F': return {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10};
    case 'G': return {0x0F, 0x10, 0x10, 0x13, 0x11, 0x11, 0x0F};
    case 'H': return {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    case 'I': return {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F};
    case 'K': return {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11};
    case 'L': return {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F};
    case 'M': return {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
    case 'N': return {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
    case 'O': return {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
    case 'P': return {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
    case 'R': return {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11};
    case 'S': return {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
    case 'T': return {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
    case 'U': return {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
    case 'V': return {0x11, 0x11, 0x11, 0x11, 0x0A, 0x0A, 0x04};
    case 'W': return {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11};
    case 'X': return {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11};
    case 'Y': return {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04};
    case 'Z': return {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F};
    case '0': return {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E};
    case '1': return {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E};
    case '2': return {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F};
    case '3': return {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E};
    case '4': return {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02};
    case '5': return {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E};
    case '6': return {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E};
    case '7': return {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
    case '8': return {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E};
    case '9': return {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E};
    case '.': return {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C};
    case '-': return {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};
    case '+': return {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00};
    default: return {0, 0, 0, 0, 0, 0, 0};
    }
}

void FillRect(std::vector<uint32_t>& pixels, uint32_t width, uint32_t height,
              int x, int y, int w, int h, uint32_t color) {
    const int x0 = std::clamp(x, 0, static_cast<int>(width));
    const int y0 = std::clamp(y, 0, static_cast<int>(height));
    const int x1 = std::clamp(x + w, 0, static_cast<int>(width));
    const int y1 = std::clamp(y + h, 0, static_cast<int>(height));
    for (int py = y0; py < y1; ++py) {
        for (int px = x0; px < x1; ++px) {
            pixels[static_cast<size_t>(py) * width + px] = color;
        }
    }
}

int OverlayTextWidth(const char* text, int scale) {
    const int chars = static_cast<int>(std::strlen(text));
    return chars > 0 ? ((chars * 6) - 1) * scale : 0;
}

void DrawOverlayText(std::vector<uint32_t>& pixels, uint32_t width, uint32_t height,
                     const char* text, int x, int y, int scale, uint32_t color) {
    int cursor = x;
    for (const char* p = text; *p; ++p) {
        const auto rows = OverlayGlyphRows(*p);
        for (int row = 0; row < 7; ++row) {
            const uint8_t bits = rows[static_cast<size_t>(row)];
            for (int col = 0; col < 5; ++col) {
                if ((bits & (1u << (4 - col))) != 0) {
                    FillRect(pixels, width, height, cursor + col * scale, y + row * scale,
                             scale, scale, color);
                }
            }
        }
        cursor += 6 * scale;
    }
}

std::vector<uint32_t> BuildPromptPixels(uint32_t width, uint32_t height) {
    std::vector<uint32_t> pixels(static_cast<size_t>(width) * height, 0x00000000u);
    FillRect(pixels, width, height, 0, 0, static_cast<int>(width), static_cast<int>(height), 0xD0100804u);
    FillRect(pixels, width, height, 0, 0, static_cast<int>(width), 8, 0xE0FFB030u);
    FillRect(pixels, width, height, 0, static_cast<int>(height) - 8, static_cast<int>(width), 8, 0xE0FFB030u);

    constexpr const char* line1 = "CLICK RIGHT";
    constexpr const char* line2 = "THUMBSTICK";
    constexpr const char* line3 = "TO SET";
    constexpr const char* line4 = "HEIGHT";
    const int scale = 5;
    const int lineStep = 78;
    const int y = 44;
    const uint32_t text = 0xFFFFD8A0u;
    DrawOverlayText(pixels, width, height, line1,
                    (static_cast<int>(width) - OverlayTextWidth(line1, scale)) / 2, y, scale, text);
    DrawOverlayText(pixels, width, height, line2,
                    (static_cast<int>(width) - OverlayTextWidth(line2, scale)) / 2, y + lineStep, scale, text);
    DrawOverlayText(pixels, width, height, line3,
                    (static_cast<int>(width) - OverlayTextWidth(line3, scale)) / 2, y + lineStep * 2, scale, text);
    DrawOverlayText(pixels, width, height, line4,
                    (static_cast<int>(width) - OverlayTextWidth(line4, scale)) / 2, y + lineStep * 3, scale, text);
    return pixels;
}

void DrawValueText(std::vector<uint32_t>& pixels, uint32_t width, uint32_t height,
                   const char* label, const char* value, int x, int y, int rowWidth, bool selected) {
    const uint32_t rowBg = selected ? 0xE04A2C12u : 0x70201810u;
    const uint32_t accent = selected ? 0xFFFFB030u : 0x80FFB030u;
    const uint32_t text = selected ? 0xFFFFE6B8u : 0xFFD8C0A0u;
    FillRect(pixels, width, height, x, y - 9, rowWidth, 31, rowBg);
    FillRect(pixels, width, height, x, y - 9, 6, 31, accent);
    DrawOverlayText(pixels, width, height, label, x + 16, y, 2, text);

    const int valueWidth = 150;
    const int valueBoxX = x + rowWidth - valueWidth - 10;
    FillRect(pixels, width, height, valueBoxX, y - 5, valueWidth, 23,
             selected ? 0xD030180Cu : 0x9030180Cu);
    DrawOverlayText(pixels, width, height, "-", valueBoxX + 8, y, 2, 0xFFFFB030u);
    DrawOverlayText(pixels, width, height, "+", valueBoxX + valueWidth - 20, y, 2, 0xFFFFB030u);
    const int valueX = valueBoxX + (valueWidth - OverlayTextWidth(value, 2)) / 2;
    DrawOverlayText(pixels, width, height, value, valueX, y, 2, 0xFFFFF0C8u);
}

std::string FloatText(float value, int precision = 2) {
    char buffer[32] = {};
    if (precision == 3)
        snprintf(buffer, sizeof(buffer), "%.3f", value);
    else if (precision == 1)
        snprintf(buffer, sizeof(buffer), "%.1f", value);
    else
        snprintf(buffer, sizeof(buffer), "%.2f", value);
    return buffer;
}

struct VrMenuRow {
    const char* label;
    std::string value;
};

std::vector<VrMenuRow> BuildSettingsRows(const SettingsState& s) {
    switch (s.vrMenuTab) {
    case 1:
        return {
            {"WORLD SCALE", FloatText(s.worldScale, 2)},
            {"POSITION LEFT RIGHT", FloatText(s.offsetX, 3)},
            {"POSITION UP DOWN", FloatText(s.offsetY, 3)},
            {"POSITION FORWARD BACK", FloatText(s.offsetZ, 3)},
            {"RESET POSITION", "RESET"},
            {"MODEL OFFSET LEFT RIGHT", FloatText(s.modelOffsetX, 3)},
            {"MODEL OFFSET UP DOWN", FloatText(s.modelOffsetY, 3)},
            {"MODEL OFFSET FORWARD BACK", FloatText(s.modelOffsetZ, 3)},
            {"RESET MODEL OFFSET", "RESET"},
            {"ROTATION PITCH", FloatText(s.rotOffsetX, 1)},
            {"ROTATION YAW", FloatText(s.rotOffsetY, 1)},
            {"ROTATION ROLL", FloatText(s.rotOffsetZ, 1)},
            {"RESET ROTATION", "RESET"},
        };
    case 2:
        return {
            {"RIGHT HAND", s.useRightHand ? "ON" : "OFF"},
            {"REQUIRE TRIGGER", s.requireTrigger ? "ON" : "OFF"},
            {"TRIGGER THRESHOLD", FloatText(s.triggerThreshold, 2)},
            {"AUTO SET CONTROLLER BINDINGS", s.autoDolphinXrControls ? "ON" : "OFF"},
            {"VISOR GESTURE", s.xrDpadEnabled ? "ON" : "OFF"},
            {"HEAD RADIUS", FloatText(s.xrDpadHeadRadius, 2)},
            {"HEAD BELOW AMOUNT", FloatText(s.xrDpadHeadYBelow, 2)},
            {"STICK DEADZONE", FloatText(s.xrDpadDeadzone, 2)},
            {"RESET VISOR", "RESET"},
        };
    case 3:
        return {
            {"DIRECTIONAL MOVE", s.directionalMovementEnabled ? "ON" : "OFF"},
            {"MOVEMENT STICK", s.directionalMovementUseRightStick ? "RIGHT" : "LEFT"},
            {"MOVE DEADZONE", FloatText(s.directionalMovementDeadzone, 2)},
            {"MOVE SPEED", FloatText(s.directionalMovementSpeed, 1)},
            {"MOVE ACCELERATION", FloatText(s.directionalMovementAccel, 1)},
            {"AIR ACCELERATION", FloatText(s.directionalMovementAirAccel, 1)},
            {"RESET MOVEMENT", "RESET"},
        };
    case 4:
        return {
            {"DEFAULT ARM PRESET", "APPLY"},
            {"SAMUS ARM PRESET", "APPLY"},
        };
    default:
        return {
            {"TARGETING", s.gunTargetingEnabled ? "ON" : "OFF"},
            {"TARGET DISTANCE", FloatText(s.gunTargetingDistance, 1)},
            {"TARGET RADIUS", FloatText(s.gunTargetingRadius, 1)},
            {"RESET TARGETING", "RESET"},
        };
    }
}

std::vector<uint32_t> BuildSettingsMenuPixels(uint32_t width, uint32_t height, const SettingsState& s) {
    std::vector<uint32_t> pixels(static_cast<size_t>(width) * height, 0x00000000u);
    FillRect(pixels, width, height, 0, 0, static_cast<int>(width), static_cast<int>(height), 0xD0100804u);
    FillRect(pixels, width, height, 0, 0, static_cast<int>(width), 10, 0xE0FFB030u);
    FillRect(pixels, width, height, 0, static_cast<int>(height) - 10, static_cast<int>(width), 10, 0xE0FFB030u);
    DrawOverlayText(pixels, width, height, "PRIMEDGUN SETTINGS", 48, 28, 4, 0xFFFFD8A0u);
    if (s.vrMenuSavedNotice != 0) {
        constexpr const char* saved = "SETTINGS SAVED";
        DrawOverlayText(pixels, width, height, saved,
                        static_cast<int>(width) - 42 - OverlayTextWidth(saved, 2),
                        34, 2, 0xFFFFE6B8u);
    }

    constexpr const char* tabs[] = {"AIMING", "CALIBRATION", "CONTROLLER", "MOVEMENT", "PRESETS"};
    const int tabY = 64;
    const int tabH = 38;
    const int tabX = 36;
    const int tabW = 180;
    const int tabGap = 10;
    for (int i = 0; i < static_cast<int>(std::size(tabs)); ++i) {
        const bool active = i == static_cast<int>(s.vrMenuTab);
        const int x = tabX + i * (tabW + tabGap);
        FillRect(pixels, width, height, x, tabY, tabW, tabH, active ? 0xD04A2C12u : 0x7030180Cu);
        FillRect(pixels, width, height, x, tabY + tabH - 4, tabW, 4, active ? 0xFFFFB030u : 0x604A2C12u);
        const int textX = x + (tabW - OverlayTextWidth(tabs[i], 2)) / 2;
        DrawOverlayText(pixels, width, height, tabs[i], textX, tabY + 11, 2,
                        active ? 0xFFFFE6B8u : 0xFFD8C0A0u);
    }

    auto drawButton = [&](const char* label, int x, int y, int w) {
        FillRect(pixels, width, height, x, y, w, 28, 0x9030180Cu);
        FillRect(pixels, width, height, x, y, 5, 28, 0x80FFB030u);
        const int textX = x + (w - OverlayTextWidth(label, 2)) / 2;
        DrawOverlayText(pixels, width, height, label, textX, y + 7, 2, 0xFFFFE6B8u);
    };
    drawButton("SAVE SETTINGS", 52, 108, 220);
    drawButton("RESET ALL", 300, 108, 220);

    const std::vector<VrMenuRow> rows = BuildSettingsRows(s);
    const int count = static_cast<int>(rows.size());
    const int firstY = 154;
    const int step = 25;
    const int rowX = 52;
    const int rowWidth = static_cast<int>(width) - 104;
    for (int i = 0; i < count; ++i) {
        DrawValueText(pixels, width, height, rows[i].label, rows[i].value.c_str(),
                      rowX, firstY + i * step, rowWidth,
                      i == static_cast<int>(s.vrMenuSelectedIndex));
    }
    if (s.vrMenuPointerActive) {
        (void)s;
    }
    return pixels;
}

std::vector<uint32_t> BuildQuadPixels(uint32_t width, uint32_t height, uint32_t contentKind, const SettingsState& settings) {
    if (contentKind == 2)
        return BuildSettingsMenuPixels(width, height, settings);
    return BuildPromptPixels(width, height);
}

bool SelectSwapchainFormat(GraphicsApi api, int64_t& format) {
    if (!g_realEnumerateSwapchainFormats || g_quadLayer.session == XR_NULL_HANDLE) {
        return false;
    }

    uint32_t formatCount = 0;
    XrResult result = g_realEnumerateSwapchainFormats(g_quadLayer.session, 0, &formatCount, nullptr);
    if (XR_FAILED(result) || formatCount == 0) {
        Log(L"OpenXR quad prompt failed to enumerate swapchain format count result=" + std::to_wstring(result));
        return false;
    }

    std::vector<int64_t> formats(formatCount);
    result = g_realEnumerateSwapchainFormats(g_quadLayer.session, formatCount, &formatCount, formats.data());
    if (XR_FAILED(result)) {
        Log(L"OpenXR quad prompt failed to enumerate swapchain formats result=" + std::to_wstring(result));
        return false;
    }

    const int64_t preferredD3D[] = {
        DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
    };
    const int64_t preferredVulkan[] = {
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_R8G8B8A8_SRGB,
        VK_FORMAT_B8G8R8A8_SRGB,
    };

    const int64_t* preferred = api == GraphicsApi::Vulkan ? preferredVulkan : preferredD3D;
    const size_t preferredCount = api == GraphicsApi::Vulkan ? std::size(preferredVulkan) : std::size(preferredD3D);
    for (size_t i = 0; i < preferredCount; ++i) {
        if (std::find(formats.begin(), formats.end(), preferred[i]) != formats.end()) {
            format = preferred[i];
            if (!g_quadLayer.loggedSelectedFormat) {
                g_quadLayer.loggedSelectedFormat = true;
                Log(L"OpenXR quad prompt selected swapchain format=" + std::to_wstring(format));
            }
            return true;
        }
    }

    format = formats[0];
    if (!g_quadLayer.loggedSelectedFormat) {
        g_quadLayer.loggedSelectedFormat = true;
        Log(L"OpenXR quad prompt falling back to first runtime swapchain format=" + std::to_wstring(format));
    }
    return true;
}

bool IsReadableMemory(const void* ptr, size_t size) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(ptr, &mbi, sizeof(mbi)))
        return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return false;
    const uintptr_t start = reinterpret_cast<uintptr_t>(ptr);
    const uintptr_t end = start + size;
    const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return end >= start && end <= regionEnd;
}

bool IsWritableProtect(DWORD protect) {
    if (protect & (PAGE_GUARD | PAGE_NOACCESS))
        return false;
    protect &= 0xff;
    return protect == PAGE_READWRITE ||
           protect == PAGE_WRITECOPY ||
           protect == PAGE_EXECUTE_READWRITE ||
           protect == PAGE_EXECUTE_WRITECOPY;
}

bool PatchImportThunk(void** thunk, void* replacement, void** original) {
    if (!thunk || !IsReadableMemory(thunk, sizeof(void*)))
        return false;
    void* current = *thunk;
    if (current == replacement)
        return true;
    if (original && !*original)
        *original = current;

    DWORD oldProtect = 0;
    if (!VirtualProtect(thunk, sizeof(void*), PAGE_READWRITE, &oldProtect))
        return false;
    *thunk = replacement;
    DWORD ignored = 0;
    VirtualProtect(thunk, sizeof(void*), oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), thunk, sizeof(void*));
    return true;
}

bool IsOrdinalProcName(LPCSTR name) {
    return (reinterpret_cast<uintptr_t>(name) >> 16) == 0;
}

void CopyPose(PoseState& dst, const XrPosef& pose) {
    dst.positionMeters.x = pose.position.x;
    dst.positionMeters.y = pose.position.y;
    dst.positionMeters.z = pose.position.z;
    dst.orientation.x = pose.orientation.x;
    dst.orientation.y = pose.orientation.y;
    dst.orientation.z = pose.orientation.z;
    dst.orientation.w = pose.orientation.w;
}

void CopyPosePosition(PoseState& dst, const XrPosef& pose) {
    dst.positionMeters.x = pose.position.x;
    dst.positionMeters.y = pose.position.y;
    dst.positionMeters.z = pose.position.z;
}

void CopyPoseOrientation(PoseState& dst, const XrPosef& pose) {
    dst.orientation.x = pose.orientation.x;
    dst.orientation.y = pose.orientation.y;
    dst.orientation.z = pose.orientation.z;
    dst.orientation.w = pose.orientation.w;
}

bool IsPoseValid(XrSpaceLocationFlags flags) {
    return (flags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0 &&
           (flags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
}

void MarkOpenXrActive(SharedState* state) {
    if (!state)
        return;
    state->trackingSource = 1;
    state->trackingRuntimeActive = 1;
    state->trackingGeneration = ++g_generation;
}

bool LooksLikeDolphinOpenXrManager(uint8_t* candidate) {
    if (!candidate || !IsReadableMemory(candidate, sizeof(DolphinOpenXrManagerLayout)))
        return false;

    auto* manager = reinterpret_cast<DolphinOpenXrManagerLayout*>(candidate);
    if (manager->session != g_dolphinSession || manager->referenceSpace != g_dolphinReferenceSpace)
        return false;
    if (manager->frameState.type != XR_TYPE_FRAME_STATE)
        return false;
    if (manager->views[0].type != XR_TYPE_VIEW || manager->views[1].type != XR_TYPE_VIEW)
        return false;
    return true;
}

uint8_t* FindDolphinOpenXrManager() {
    if (g_dolphinSession == XR_NULL_HANDLE || g_dolphinReferenceSpace == XR_NULL_HANDLE)
        return nullptr;

    const uintptr_t referenceOffset = offsetof(DolphinOpenXrManagerLayout, referenceSpace);
    uint8_t* address = nullptr;
    MEMORY_BASIC_INFORMATION mbi{};
    while (VirtualQuery(address, &mbi, sizeof(mbi))) {
        auto* region = static_cast<uint8_t*>(mbi.BaseAddress);
        const size_t size = mbi.RegionSize;
        address = region + size;

        if (mbi.State != MEM_COMMIT || !IsWritableProtect(mbi.Protect) ||
            size < sizeof(DolphinOpenXrManagerLayout)) {
            continue;
        }

        const size_t limit = size - sizeof(XrSpace);
        for (size_t offset = 0; offset <= limit; offset += sizeof(void*)) {
            XrSpace value = XR_NULL_HANDLE;
            std::memcpy(&value, region + offset, sizeof(value));
            if (value != g_dolphinReferenceSpace || offset < referenceOffset)
                continue;

            uint8_t* candidate = region + offset - referenceOffset;
            if (LooksLikeDolphinOpenXrManager(candidate))
                return candidate;
        }
    }
    return nullptr;
}

bool EnsureDolphinOpenXrManager() {
    if (g_dolphinSession == XR_NULL_HANDLE || g_dolphinReferenceSpace == XR_NULL_HANDLE)
        return false;

    if (g_validLocateViewFrames < 300)
        return false;

    if (!LooksLikeDolphinOpenXrManager(g_dolphinOpenXrManager)) {
        const uint64_t now = GetTickCount64();
        if (g_lastManagerScanMs != 0 && now - g_lastManagerScanMs < 1000)
            return false;
        g_lastManagerScanMs = now;
        uint8_t* candidate = FindDolphinOpenXrManager();
        if (!candidate) {
            g_lastManagerCandidate = nullptr;
            g_managerStableScans = 0;
            g_dolphinOpenXrManager = nullptr;
            return false;
        }
        if (candidate == g_lastManagerCandidate) {
            ++g_managerStableScans;
        } else {
            g_lastManagerCandidate = candidate;
            g_managerStableScans = 1;
        }
        if (g_managerStableScans >= 3) {
            g_dolphinOpenXrManager = candidate;
            Log(L"OpenXrHooks found stable Dolphin OpenXRManager for height-only recenter.");
        }
    }

    return g_dolphinOpenXrManager != nullptr;
}

struct PeSectionRange {
    uint8_t* begin = nullptr;
    size_t size = 0;
};

bool GetModuleSection(HMODULE module, const char* sectionName, PeSectionRange& range) {
    if (!module || !sectionName)
        return false;

    auto* base = reinterpret_cast<uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return false;

    IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        char name[9] = {};
        std::memcpy(name, section[i].Name, sizeof(section[i].Name));
        if (std::strcmp(name, sectionName) == 0) {
            range.begin = base + section[i].VirtualAddress;
            range.size = std::max<size_t>(section[i].Misc.VirtualSize, section[i].SizeOfRawData);
            return range.begin && range.size != 0;
        }
    }
    return false;
}

uint8_t* FindBytes(uint8_t* begin, size_t size, const uint8_t* needle, size_t needleSize) {
    if (!begin || !needle || needleSize == 0 || size < needleSize)
        return nullptr;
    const size_t limit = size - needleSize;
    for (size_t i = 0; i <= limit; ++i) {
        if (std::memcmp(begin + i, needle, needleSize) == 0)
            return begin + i;
    }
    return nullptr;
}

uint8_t* FindAscii(uint8_t* begin, size_t size, const char* text) {
    return FindBytes(begin, size, reinterpret_cast<const uint8_t*>(text), std::strlen(text));
}

uint8_t* FindRipRelativeLeaTo(uint8_t* begin, size_t size, uint8_t* target) {
    if (!begin || !target || size < 7)
        return nullptr;
    for (size_t i = 0; i + 7 <= size; ++i) {
        uint8_t* at = begin + i;
        if (at[0] != 0x48 || at[1] != 0x8D || at[2] != 0x05)
            continue;
        int32_t displacement = 0;
        std::memcpy(&displacement, at + 3, sizeof(displacement));
        if (at + 7 + displacement == target)
            return at;
    }
    return nullptr;
}

uint8_t* FindMovssStore(uint8_t* begin, uint8_t* end, uint8_t modrm) {
    if (!begin || !end || end <= begin)
        return nullptr;
    for (uint8_t* at = begin; at + 8 <= end; ++at) {
        if (at[0] == 0xF3 && at[1] == 0x0F && at[2] == 0x11 && at[3] == modrm)
            return at;
    }
    return nullptr;
}

int32_t ReadDisp32(const uint8_t* instruction) {
    int32_t value = 0;
    std::memcpy(&value, instruction + 4, sizeof(value));
    return value;
}

bool NopInstruction(uint8_t* address, size_t length) {
    if (!address || length == 0)
        return false;

    bool alreadyNopped = true;
    for (size_t i = 0; i < length; ++i) {
        if (address[i] != 0x90) {
            alreadyNopped = false;
            break;
        }
    }
    if (alreadyNopped)
        return true;

    DWORD oldProtect = 0;
    if (!VirtualProtect(address, length, PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;
    std::memset(address, 0x90, length);
    FlushInstructionCache(GetCurrentProcess(), address, length);
    DWORD ignored = 0;
    VirtualProtect(address, length, oldProtect, &ignored);
    return true;
}

void InstallHeightOnlyRecenterPatch() {
    if (g_heightOnlyRecenterPatchInstalled || g_heightOnlyRecenterPatchAttempted)
        return;
    g_heightOnlyRecenterPatchAttempted = true;

    HMODULE exe = GetModuleHandleW(nullptr);
    PeSectionRange text{};
    PeSectionRange rdata{};
    if (!GetModuleSection(exe, ".text", text) || !GetModuleSection(exe, ".rdata", rdata)) {
        Log(L"OpenXrHooks height-only recenter patch failed: PE sections unavailable.");
        return;
    }

    constexpr const char* recenterText = "OpenXR: Recentered home position to";
    uint8_t* recenterString = FindAscii(rdata.begin, rdata.size, recenterText);
    if (!recenterString) {
        Log(L"OpenXrHooks height-only recenter patch failed: recenter string not found.");
        return;
    }

    uint8_t* recenterLea = FindRipRelativeLeaTo(text.begin, text.size, recenterString);
    if (!recenterLea) {
        Log(L"OpenXrHooks height-only recenter patch failed: recenter string xref not found.");
        return;
    }

    uint8_t* windowBegin = recenterLea > text.begin + 0x100 ? recenterLea - 0x100 : text.begin;
    uint8_t* textEnd = text.begin + text.size;
    uint8_t* windowEnd = std::min(textEnd, recenterLea + 0x240);

    uint8_t* storeX = FindMovssStore(windowBegin, windowEnd, 0x8B);
    uint8_t* storeY = FindMovssStore(storeX ? storeX + 8 : windowBegin, windowEnd, 0x9B);
    uint8_t* storeZ = FindMovssStore(storeY ? storeY + 8 : windowBegin, windowEnd, 0x93);
    if (!storeX || !storeY || !storeZ) {
        Log(L"OpenXrHooks height-only recenter patch failed: home position stores not found.");
        return;
    }

    const int32_t dispX = ReadDisp32(storeX);
    const int32_t dispY = ReadDisp32(storeY);
    const int32_t dispZ = ReadDisp32(storeZ);
    if (!(dispY == dispX + 4 && dispZ == dispY + 4)) {
        Log(L"OpenXrHooks height-only recenter patch failed: unexpected home store offsets x=" +
            std::to_wstring(dispX) + L" y=" + std::to_wstring(dispY) + L" z=" + std::to_wstring(dispZ));
        return;
    }

    if (!NopInstruction(storeX, 8) || !NopInstruction(storeZ, 8)) {
        Log(L"OpenXrHooks height-only recenter patch failed: VirtualProtect/NOP failed.");
        return;
    }

    g_heightOnlyRecenterPatchInstalled = true;
    Log(L"OpenXrHooks installed Dolphin height-only recenter patch. Dolphin reset now updates Y only.");
}

void ApplyDolphinHomeHeightOnly(float headCenterY) {
    // Disabled after testing: writing Dolphin's OpenXRManager homePosition from
    // the injected bridge can crash on right-stick recenter because the runtime
    // object layout is not stable enough to treat as a writable ABI.
    return;
    SharedState* state = g_sharedState.load();
    if (!state || state->settings.viewHeightGeneration == 0)
        return;
    if (state->settings.viewHeightGeneration == g_appliedViewHeightGeneration)
        return;
    if (!std::isfinite(headCenterY))
        return;
    if (!EnsureDolphinOpenXrManager())
        return;

    auto* manager = reinterpret_cast<DolphinOpenXrManagerLayout*>(g_dolphinOpenXrManager);
    if (!LooksLikeDolphinOpenXrManager(g_dolphinOpenXrManager))
        return;
    manager->homeSet = true;
    manager->homePosition.y = headCenterY;
    g_appliedViewHeightGeneration = state->settings.viewHeightGeneration;
    Log(L"OpenXrHooks applied Dolphin height-only home recenter.");
}

void CenterViewsForDolphinStartup(uint32_t viewCount, XrView* views) {
    if (!views || viewCount == 0 || g_startupCenterSpoofFrames == 0)
        return;

    SharedState* state = g_sharedState.load();
    float startupHeight = state ? state->settings.viewHeightHomeMeters : 1.6374f;
    if (!std::isfinite(startupHeight))
        startupHeight = 1.6374f;
    startupHeight = std::clamp(startupHeight, 0.4f, 2.4f);

    XrVector3f center = views[0].pose.position;
    if (viewCount >= 2) {
        center.x = (views[0].pose.position.x + views[1].pose.position.x) * 0.5f;
        center.y = (views[0].pose.position.y + views[1].pose.position.y) * 0.5f;
        center.z = (views[0].pose.position.z + views[1].pose.position.z) * 0.5f;
    }

    for (uint32_t i = 0; i < viewCount; ++i) {
        views[i].pose.position.x -= center.x;
        views[i].pose.position.y += startupHeight - center.y;
        views[i].pose.position.z -= center.z;
    }
    --g_startupCenterSpoofFrames;
}

PoseState* PoseForSubaction(SharedState* state, XrPath subactionPath) {
    if (!state || subactionPath == XR_NULL_PATH)
        return nullptr;

    std::lock_guard<std::mutex> guard(g_mutex);
    const auto pathIt = g_paths.find(subactionPath);
    if (pathIt == g_paths.end())
        return nullptr;
    if (pathIt->second == "/user/hand/left")
        return &state->leftHandPose;
    if (pathIt->second == "/user/hand/right")
        return &state->rightHandPose;
    return nullptr;
}

std::string ActionName(XrAction action) {
    std::lock_guard<std::mutex> guard(g_mutex);
    const auto it = g_actions.find(action);
    return it != g_actions.end() ? it->second : std::string{};
}

bool RuntimeSupportsDisplayRefreshRateExtension() {
    if (g_displayRefreshRateExtensionAvailable)
        return true;
    if (!g_realEnumerateInstanceExtensionProperties && g_realGetInstanceProcAddr) {
        PFN_xrVoidFunction fn = nullptr;
        if (XR_SUCCEEDED(g_realGetInstanceProcAddr(
                XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", &fn))) {
            g_realEnumerateInstanceExtensionProperties =
                reinterpret_cast<PFN_xrEnumerateInstanceExtensionProperties>(fn);
        }
    }
    if (!g_realEnumerateInstanceExtensionProperties)
        return false;

    uint32_t count = 0;
    XrResult result = g_realEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
    if (XR_FAILED(result) || count == 0)
        return false;

    std::vector<XrExtensionProperties> extensions(
        count, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
    result = g_realEnumerateInstanceExtensionProperties(nullptr, count, &count, extensions.data());
    if (XR_FAILED(result))
        return false;

    for (const XrExtensionProperties& extension : extensions) {
        if (std::strcmp(extension.extensionName, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME) == 0) {
            g_displayRefreshRateExtensionAvailable = true;
            Log(L"OpenXrHooks detected XR_FB_display_refresh_rate support.");
            return true;
        }
    }
    Log(L"OpenXrHooks: XR_FB_display_refresh_rate is not supported by this runtime.");
    return false;
}

bool ExtensionAlreadyEnabled(const XrInstanceCreateInfo* createInfo, const char* extensionName) {
    if (!createInfo || !extensionName)
        return false;
    for (uint32_t i = 0; i < createInfo->enabledExtensionCount; ++i) {
        const char* enabled = createInfo->enabledExtensionNames ? createInfo->enabledExtensionNames[i] : nullptr;
        if (enabled && std::strcmp(enabled, extensionName) == 0)
            return true;
    }
    return false;
}

void ResolveDisplayRefreshRateFunctions(XrInstance instance) {
    if (!g_realGetInstanceProcAddr || instance == XR_NULL_HANDLE)
        return;

    auto resolve = [&](const char* name, auto& out) {
        if (out)
            return;
        PFN_xrVoidFunction fn = nullptr;
        if (XR_SUCCEEDED(g_realGetInstanceProcAddr(instance, name, &fn)) && fn)
            out = reinterpret_cast<std::remove_reference_t<decltype(out)>>(fn);
    };

    resolve("xrEnumerateDisplayRefreshRatesFB", g_xrEnumerateDisplayRefreshRatesFB);
    resolve("xrGetDisplayRefreshRateFB", g_xrGetDisplayRefreshRateFB);
    resolve("xrRequestDisplayRefreshRateFB", g_xrRequestDisplayRefreshRateFB);
}

void ApplyDolphinDisplayRefreshRatePreference(XrInstance instance, XrSession session) {
    (void)instance;
    (void)session;
}

PFN_xrVoidFunction WrapProc(const char* name, PFN_xrVoidFunction proc);
bool InstallInlineDetour(HMODULE openxr);
bool InstallRuntimeNegotiationDetour(HMODULE runtime);

XrResult XRAPI_PTR Hook_xrGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
    if (!g_realGetInstanceProcAddr)
        return XR_ERROR_HANDLE_INVALID;
    const XrResult result = g_realGetInstanceProcAddr(instance, name, function);
    if (XR_SUCCEEDED(result) && function && *function)
        *function = WrapProc(name, *function);
    return result;
}

XrResult XRAPI_PTR Hook_xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo* loaderInfo,
                                                          XrNegotiateRuntimeRequest* runtimeRequest) {
    if (!g_realNegotiateLoaderRuntimeInterface)
        return XR_ERROR_RUNTIME_FAILURE;

    const XrResult result = g_realNegotiateLoaderRuntimeInterface(loaderInfo, runtimeRequest);
    if (XR_SUCCEEDED(result) && runtimeRequest && runtimeRequest->getInstanceProcAddr) {
        if (!g_realGetInstanceProcAddr)
            g_realGetInstanceProcAddr = runtimeRequest->getInstanceProcAddr;
        runtimeRequest->getInstanceProcAddr = &Hook_xrGetInstanceProcAddr;
        if (!g_runtimeInstalled.exchange(true))
            Log(L"OpenXrHooks wrapped SteamVR OpenXR runtime getInstanceProcAddr.");
    }
    return result;
}

FARPROC WINAPI Hook_GetProcAddress(HMODULE module, LPCSTR procName) {
    FARPROC proc = g_realGetProcAddress ? g_realGetProcAddress(module, procName) : nullptr;
    if (!proc || !procName || IsOrdinalProcName(procName))
        return proc;

    if (std::strcmp(procName, "xrNegotiateLoaderRuntimeInterface") == 0) {
        if (!g_realNegotiateLoaderRuntimeInterface)
            g_realNegotiateLoaderRuntimeInterface = reinterpret_cast<PFN_xrNegotiateLoaderRuntimeInterface>(proc);
        if (!g_runtimeInstalled.exchange(true))
            Log(L"OpenXrHooks intercepted GetProcAddress(xrNegotiateLoaderRuntimeInterface).");
        return reinterpret_cast<FARPROC>(&Hook_xrNegotiateLoaderRuntimeInterface);
    }
    return proc;
}

XrResult XRAPI_PTR Hook_xrStringToPath(XrInstance instance, const char* pathString, XrPath* path) {
    const XrResult result = g_realStringToPath(instance, pathString, path);
    if (XR_SUCCEEDED(result) && path && pathString) {
        std::lock_guard<std::mutex> guard(g_mutex);
        g_paths[*path] = pathString;
    }
    return result;
}

XrResult XRAPI_PTR Hook_xrCreateAction(XrActionSet actionSet, const XrActionCreateInfo* createInfo, XrAction* action) {
    const XrResult result = g_realCreateAction(actionSet, createInfo, action);
    if (XR_SUCCEEDED(result) && createInfo && action) {
        std::lock_guard<std::mutex> guard(g_mutex);
        g_actions[*action] = createInfo->actionName;
        Log(L"OpenXrHooks action: " + Widen(createInfo->actionName));
    }
    return result;
}

XrResult XRAPI_PTR Hook_xrCreateActionSpace(XrSession session, const XrActionSpaceCreateInfo* createInfo, XrSpace* space) {
    const XrResult result = g_realCreateActionSpace(session, createInfo, space);
    if (XR_SUCCEEDED(result) && createInfo && space) {
        std::lock_guard<std::mutex> guard(g_mutex);
        const auto actionIt = g_actions.find(createInfo->action);
        const auto pathIt = g_paths.find(createInfo->subactionPath);
        if (actionIt != g_actions.end() && pathIt != g_paths.end() &&
            (actionIt->second == "aim_pose" || actionIt->second == "grip_pose")) {
            g_spaces[*space] = actionIt->second + "|" + pathIt->second;
        }
    }
    return result;
}

XrResult XRAPI_PTR Hook_xrCreateInstance(const XrInstanceCreateInfo* createInfo, XrInstance* instance) {
    if (!g_realCreateInstance)
        return XR_ERROR_RUNTIME_FAILURE;

    XrInstanceCreateInfo patched{};
    std::vector<const char*> extensions;
    const XrInstanceCreateInfo* createInfoToUse = createInfo;
    g_displayRefreshRateExtensionEnabled = false;

    if (createInfo && RuntimeSupportsDisplayRefreshRateExtension()) {
        extensions.reserve(static_cast<size_t>(createInfo->enabledExtensionCount) + 1);
        for (uint32_t i = 0; i < createInfo->enabledExtensionCount; ++i) {
            if (createInfo->enabledExtensionNames && createInfo->enabledExtensionNames[i])
                extensions.push_back(createInfo->enabledExtensionNames[i]);
        }
        if (!ExtensionAlreadyEnabled(createInfo, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME)) {
            extensions.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
            patched = *createInfo;
            patched.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
            patched.enabledExtensionNames = extensions.data();
            createInfoToUse = &patched;
            Log(L"OpenXrHooks enabling XR_FB_display_refresh_rate for Dolphin.");
        } else {
            Log(L"OpenXrHooks found XR_FB_display_refresh_rate already enabled by Dolphin.");
        }
    }

    const XrResult result = g_realCreateInstance(createInfoToUse, instance);
    if (XR_SUCCEEDED(result)) {
        g_displayRefreshRateExtensionEnabled =
            createInfoToUse && ExtensionAlreadyEnabled(createInfoToUse, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    }
    return result;
}

XrResult XRAPI_PTR Hook_xrCreateSession(XrInstance instance, const XrSessionCreateInfo* createInfo,
                                        XrSession* session) {
    const GraphicsApi graphicsApi = DetectGraphicsApiFromSessionCreateInfo(createInfo);
    ID3D11Device* d3dDevice = nullptr;
    if (const XrGraphicsBindingD3D11KHR* d3dBinding = FindD3D11Binding(createInfo)) {
        d3dDevice = d3dBinding->device;
    }
    const XrGraphicsBindingVulkanKHR* vulkanBinding = FindVulkanBinding(createInfo);
    const XrResult result = g_realCreateSession(instance, createInfo, session);
    if (XR_SUCCEEDED(result) && session && *session != XR_NULL_HANDLE) {
        DestroyPromptSwapchain();
        DestroyHeightPromptSwapchain();
        CleanupVulkanQuadResources();
        if (g_quadLayer.d3dContext) {
            g_quadLayer.d3dContext->Release();
            g_quadLayer.d3dContext = nullptr;
        }
        if (g_quadLayer.d3dDevice) {
            g_quadLayer.d3dDevice->Release();
            g_quadLayer.d3dDevice = nullptr;
        }
        g_quadLayer.session = *session;
        g_quadLayer.graphicsApi = graphicsApi;
        g_quadLayer.format = graphicsApi == GraphicsApi::Vulkan ?
            static_cast<int64_t>(VK_FORMAT_R8G8B8A8_UNORM) :
            static_cast<int64_t>(DXGI_FORMAT_R8G8B8A8_UNORM);
        g_quadLayer.vkInstance = VK_NULL_HANDLE;
        g_quadLayer.vkPhysicalDevice = VK_NULL_HANDLE;
        g_quadLayer.vkDevice = VK_NULL_HANDLE;
        g_quadLayer.vkQueue = VK_NULL_HANDLE;
        g_quadLayer.vkQueueFamilyIndex = 0;
        g_quadLayer.vkQueueIndex = 0;
        if (vulkanBinding) {
            g_quadLayer.vkInstance = vulkanBinding->instance;
            g_quadLayer.vkPhysicalDevice = vulkanBinding->physicalDevice;
            g_quadLayer.vkDevice = vulkanBinding->device;
            g_quadLayer.vkQueueFamilyIndex = vulkanBinding->queueFamilyIndex;
            g_quadLayer.vkQueueIndex = vulkanBinding->queueIndex;
        }
        if (d3dDevice) {
            g_quadLayer.d3dDevice = d3dDevice;
            g_quadLayer.d3dDevice->AddRef();
            g_quadLayer.d3dDevice->GetImmediateContext(&g_quadLayer.d3dContext);
        }
        g_dolphinInstance = instance;
        g_quadLayer.swapchain = XR_NULL_HANDLE;
        g_quadLayer.promptSwapchain = XR_NULL_HANDLE;
        g_quadLayer.textureReady = false;
        g_quadLayer.promptTextureReady = false;
        g_quadLayer.warnedNoTexture = false;
        g_quadLayer.warnedCreateFailed = false;
        g_quadLayer.loggedSelectedFormat = false;
        g_quadLayer.warnedVulkanResolveFailed = false;
        g_quadLayer.promptFirstFrameMs = 0;
        g_quadLayer.appendedFrames = 0;
        Log(L"OpenXR quad prompt observed session graphics API: " +
            std::wstring(GraphicsApiName(graphicsApi)));
        g_lastDisplayRefreshRatePreference = -1;
        ApplyDolphinDisplayRefreshRatePreference(instance, *session);
    }
    return result;
}

bool EnsureD3D11PromptSwapchain() {
    SharedState* shared = g_sharedState.load();
    const SettingsState settings = shared ? shared->settings : SettingsState{};
    const uint32_t desiredKind = settings.vrMenuVisible ? 2u : 1u;
    const uint32_t desiredGeneration = desiredKind == 2 ? settings.vrMenuGeneration : 0u;
    const uint32_t desiredHeight = desiredKind == 2 ? 512u : 384u;
    if (g_quadLayer.textureReady && g_quadLayer.contentKind == desiredKind &&
        g_quadLayer.uploadedMenuGeneration == desiredGeneration &&
        g_quadLayer.height == desiredHeight) {
        return true;
    }
    if (g_quadLayer.swapchain != XR_NULL_HANDLE)
        DestroyPromptSwapchain();
    g_quadLayer.width = 1024;
    g_quadLayer.height = desiredHeight;
    if (g_quadLayer.session == XR_NULL_HANDLE || g_quadLayer.graphicsApi != GraphicsApi::D3D11 ||
        !g_quadLayer.d3dContext || !g_realCreateSwapchain || !g_realEnumerateSwapchainFormats ||
        !g_realEnumerateSwapchainImages ||
        !g_realAcquireSwapchainImage || !g_realWaitSwapchainImage || !g_realReleaseSwapchainImage) {
        return false;
    }

    SelectSwapchainFormat(GraphicsApi::D3D11, g_quadLayer.format);

    XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    createInfo.format = g_quadLayer.format;
    createInfo.sampleCount = 1;
    createInfo.width = g_quadLayer.width;
    createInfo.height = g_quadLayer.height;
    createInfo.faceCount = 1;
    createInfo.arraySize = 1;
    createInfo.mipCount = 1;

    XrResult result = g_realCreateSwapchain(g_quadLayer.session, &createInfo, &g_quadLayer.swapchain);
    if (XR_FAILED(result) || g_quadLayer.swapchain == XR_NULL_HANDLE) {
        if (!g_quadLayer.warnedCreateFailed) {
            g_quadLayer.warnedCreateFailed = true;
            Log(L"OpenXR quad prompt failed to create D3D11 swapchain result=" + std::to_wstring(result) +
                L" format=" + std::to_wstring(createInfo.format));
        }
        return false;
    }

    uint32_t imageCount = 0;
    result = g_realEnumerateSwapchainImages(g_quadLayer.swapchain, 0, &imageCount, nullptr);
    if (XR_FAILED(result) || imageCount == 0) {
        Log(L"OpenXR quad prompt failed to enumerate D3D11 image count result=" + std::to_wstring(result));
        DestroyPromptSwapchain();
        return false;
    }

    std::vector<XrSwapchainImageD3D11KHR> images(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    result = g_realEnumerateSwapchainImages(
        g_quadLayer.swapchain, imageCount, &imageCount,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
    if (XR_FAILED(result)) {
        Log(L"OpenXR quad prompt failed to enumerate D3D11 images result=" + std::to_wstring(result));
        DestroyPromptSwapchain();
        return false;
    }

    const std::vector<uint32_t> pixels = BuildQuadPixels(g_quadLayer.width, g_quadLayer.height, desiredKind, settings);
    for (uint32_t i = 0; i < imageCount; ++i) {
        uint32_t acquired = 0;
        XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        result = g_realAcquireSwapchainImage(g_quadLayer.swapchain, &acquireInfo, &acquired);
        if (XR_FAILED(result)) {
            Log(L"OpenXR quad prompt failed to acquire D3D11 image result=" + std::to_wstring(result));
            DestroyPromptSwapchain();
            return false;
        }
        XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        waitInfo.timeout = XR_INFINITE_DURATION;
        result = g_realWaitSwapchainImage(g_quadLayer.swapchain, &waitInfo);
        if (XR_FAILED(result)) {
            Log(L"OpenXR quad prompt failed to wait D3D11 image result=" + std::to_wstring(result));
            DestroyPromptSwapchain();
            return false;
        }

        if (acquired < images.size() && images[acquired].texture) {
            g_quadLayer.d3dContext->UpdateSubresource(
                images[acquired].texture, 0, nullptr, pixels.data(),
                g_quadLayer.width * sizeof(uint32_t), 0);
        }

        XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        result = g_realReleaseSwapchainImage(g_quadLayer.swapchain, &releaseInfo);
        if (XR_FAILED(result)) {
            Log(L"OpenXR quad prompt failed to release D3D11 image result=" + std::to_wstring(result));
            DestroyPromptSwapchain();
            return false;
        }
    }

    g_quadLayer.textureReady = true;
    g_quadLayer.contentKind = desiredKind;
    g_quadLayer.uploadedMenuGeneration = desiredGeneration;
    Log(L"OpenXR quad prompt D3D11 swapchain ready.");
    return true;
}

bool EnsureD3D11HeightPromptSwapchain() {
    if (g_quadLayer.promptTextureReady && g_quadLayer.promptSwapchain != XR_NULL_HANDLE &&
        g_quadLayer.promptWidth == 1024 && g_quadLayer.promptHeight == 384) {
        return true;
    }
    if (g_quadLayer.promptSwapchain != XR_NULL_HANDLE)
        DestroyHeightPromptSwapchain();

    g_quadLayer.promptWidth = 1024;
    g_quadLayer.promptHeight = 384;
    if (g_quadLayer.session == XR_NULL_HANDLE || g_quadLayer.graphicsApi != GraphicsApi::D3D11 ||
        !g_quadLayer.d3dContext || !g_realCreateSwapchain || !g_realEnumerateSwapchainFormats ||
        !g_realEnumerateSwapchainImages ||
        !g_realAcquireSwapchainImage || !g_realWaitSwapchainImage || !g_realReleaseSwapchainImage) {
        return false;
    }

    SelectSwapchainFormat(GraphicsApi::D3D11, g_quadLayer.format);

    XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    createInfo.format = g_quadLayer.format;
    createInfo.sampleCount = 1;
    createInfo.width = g_quadLayer.promptWidth;
    createInfo.height = g_quadLayer.promptHeight;
    createInfo.faceCount = 1;
    createInfo.arraySize = 1;
    createInfo.mipCount = 1;

    XrResult result = g_realCreateSwapchain(g_quadLayer.session, &createInfo, &g_quadLayer.promptSwapchain);
    if (XR_FAILED(result) || g_quadLayer.promptSwapchain == XR_NULL_HANDLE) {
        Log(L"OpenXR height prompt failed to create D3D11 swapchain result=" + std::to_wstring(result));
        return false;
    }

    uint32_t imageCount = 0;
    result = g_realEnumerateSwapchainImages(g_quadLayer.promptSwapchain, 0, &imageCount, nullptr);
    if (XR_FAILED(result) || imageCount == 0) {
        DestroyHeightPromptSwapchain();
        return false;
    }

    std::vector<XrSwapchainImageD3D11KHR> images(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    result = g_realEnumerateSwapchainImages(
        g_quadLayer.promptSwapchain, imageCount, &imageCount,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
    if (XR_FAILED(result)) {
        DestroyHeightPromptSwapchain();
        return false;
    }

    const std::vector<uint32_t> pixels = BuildPromptPixels(g_quadLayer.promptWidth, g_quadLayer.promptHeight);
    for (uint32_t i = 0; i < imageCount; ++i) {
        uint32_t acquired = 0;
        XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        result = g_realAcquireSwapchainImage(g_quadLayer.promptSwapchain, &acquireInfo, &acquired);
        if (XR_FAILED(result)) {
            DestroyHeightPromptSwapchain();
            return false;
        }
        XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        waitInfo.timeout = XR_INFINITE_DURATION;
        result = g_realWaitSwapchainImage(g_quadLayer.promptSwapchain, &waitInfo);
        if (XR_FAILED(result)) {
            DestroyHeightPromptSwapchain();
            return false;
        }
        if (acquired < images.size() && images[acquired].texture) {
            g_quadLayer.d3dContext->UpdateSubresource(
                images[acquired].texture, 0, nullptr, pixels.data(),
                g_quadLayer.promptWidth * sizeof(uint32_t), 0);
        }
        XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        result = g_realReleaseSwapchainImage(g_quadLayer.promptSwapchain, &releaseInfo);
        if (XR_FAILED(result)) {
            DestroyHeightPromptSwapchain();
            return false;
        }
    }

    g_quadLayer.promptTextureReady = true;
    Log(L"OpenXR height prompt D3D11 swapchain ready.");
    return true;
}

bool EnsureVulkanPromptSwapchain() {
    SharedState* shared = g_sharedState.load();
    const SettingsState settings = shared ? shared->settings : SettingsState{};
    const uint32_t desiredKind = settings.vrMenuVisible ? 2u : 1u;
    const uint32_t desiredGeneration = desiredKind == 2 ? settings.vrMenuGeneration : 0u;
    const uint32_t desiredHeight = desiredKind == 2 ? 512u : 384u;
    if (g_quadLayer.textureReady && g_quadLayer.contentKind == desiredKind &&
        g_quadLayer.uploadedMenuGeneration == desiredGeneration &&
        g_quadLayer.height == desiredHeight) {
        return true;
    }
    if (g_quadLayer.swapchain != XR_NULL_HANDLE)
        DestroyPromptSwapchain();
    g_quadLayer.width = 1024;
    g_quadLayer.height = desiredHeight;
    if (g_quadLayer.session == XR_NULL_HANDLE || g_quadLayer.graphicsApi != GraphicsApi::Vulkan ||
        !g_quadLayer.vkDevice || !g_realCreateSwapchain || !g_realEnumerateSwapchainFormats ||
        !g_realEnumerateSwapchainImages ||
        !g_realAcquireSwapchainImage || !g_realWaitSwapchainImage || !g_realReleaseSwapchainImage) {
        return false;
    }

    SelectSwapchainFormat(GraphicsApi::Vulkan, g_quadLayer.format);

    XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    createInfo.format = g_quadLayer.format;
    createInfo.sampleCount = 1;
    createInfo.width = g_quadLayer.width;
    createInfo.height = g_quadLayer.height;
    createInfo.faceCount = 1;
    createInfo.arraySize = 1;
    createInfo.mipCount = 1;

    XrResult result = g_realCreateSwapchain(g_quadLayer.session, &createInfo, &g_quadLayer.swapchain);
    if (XR_FAILED(result) || g_quadLayer.swapchain == XR_NULL_HANDLE) {
        if (!g_quadLayer.warnedCreateFailed) {
            g_quadLayer.warnedCreateFailed = true;
            Log(L"OpenXR quad prompt failed to create Vulkan swapchain result=" + std::to_wstring(result) +
                L" format=" + std::to_wstring(createInfo.format) +
                L" usage=" + std::to_wstring(createInfo.usageFlags));
        }
        return false;
    }

    uint32_t imageCount = 0;
    result = g_realEnumerateSwapchainImages(g_quadLayer.swapchain, 0, &imageCount, nullptr);
    if (XR_FAILED(result) || imageCount == 0) {
        Log(L"OpenXR quad prompt failed to enumerate Vulkan image count result=" + std::to_wstring(result));
        DestroyPromptSwapchain();
        return false;
    }

    std::vector<XrSwapchainImageVulkanKHR> images(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    result = g_realEnumerateSwapchainImages(
        g_quadLayer.swapchain, imageCount, &imageCount,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
    if (XR_FAILED(result)) {
        Log(L"OpenXR quad prompt failed to enumerate Vulkan images result=" + std::to_wstring(result));
        DestroyPromptSwapchain();
        return false;
    }

    const std::vector<uint32_t> pixels = BuildQuadPixels(g_quadLayer.width, g_quadLayer.height, desiredKind, settings);
    if (!EnsureVulkanUploadResources(pixels)) {
        DestroyPromptSwapchain();
        return false;
    }

    for (uint32_t i = 0; i < imageCount; ++i) {
        uint32_t acquired = 0;
        XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        result = g_realAcquireSwapchainImage(g_quadLayer.swapchain, &acquireInfo, &acquired);
        if (XR_FAILED(result)) {
            Log(L"OpenXR quad prompt failed to acquire Vulkan image result=" + std::to_wstring(result));
            DestroyPromptSwapchain();
            return false;
        }

        XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        waitInfo.timeout = XR_INFINITE_DURATION;
        result = g_realWaitSwapchainImage(g_quadLayer.swapchain, &waitInfo);
        if (XR_FAILED(result)) {
            Log(L"OpenXR quad prompt failed to wait Vulkan image result=" + std::to_wstring(result));
            DestroyPromptSwapchain();
            return false;
        }

        if (acquired < images.size() && images[acquired].image != VK_NULL_HANDLE &&
            !UploadPromptToVulkanImage(images[acquired].image, g_quadLayer.width, g_quadLayer.height)) {
            DestroyPromptSwapchain();
            return false;
        }

        XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        result = g_realReleaseSwapchainImage(g_quadLayer.swapchain, &releaseInfo);
        if (XR_FAILED(result)) {
            Log(L"OpenXR quad prompt failed to release Vulkan image result=" + std::to_wstring(result));
            DestroyPromptSwapchain();
            return false;
        }
    }

    g_quadLayer.textureReady = true;
    g_quadLayer.contentKind = desiredKind;
    g_quadLayer.uploadedMenuGeneration = desiredGeneration;
    Log(L"OpenXR quad prompt Vulkan swapchain ready.");
    return true;
}

bool EnsureVulkanHeightPromptSwapchain() {
    if (g_quadLayer.promptTextureReady && g_quadLayer.promptSwapchain != XR_NULL_HANDLE &&
        g_quadLayer.promptWidth == 1024 && g_quadLayer.promptHeight == 384) {
        return true;
    }
    if (g_quadLayer.promptSwapchain != XR_NULL_HANDLE)
        DestroyHeightPromptSwapchain();

    g_quadLayer.promptWidth = 1024;
    g_quadLayer.promptHeight = 384;
    if (g_quadLayer.session == XR_NULL_HANDLE || g_quadLayer.graphicsApi != GraphicsApi::Vulkan ||
        !g_quadLayer.vkDevice || !g_realCreateSwapchain || !g_realEnumerateSwapchainFormats ||
        !g_realEnumerateSwapchainImages ||
        !g_realAcquireSwapchainImage || !g_realWaitSwapchainImage || !g_realReleaseSwapchainImage) {
        return false;
    }

    SelectSwapchainFormat(GraphicsApi::Vulkan, g_quadLayer.format);

    XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    createInfo.format = g_quadLayer.format;
    createInfo.sampleCount = 1;
    createInfo.width = g_quadLayer.promptWidth;
    createInfo.height = g_quadLayer.promptHeight;
    createInfo.faceCount = 1;
    createInfo.arraySize = 1;
    createInfo.mipCount = 1;

    XrResult result = g_realCreateSwapchain(g_quadLayer.session, &createInfo, &g_quadLayer.promptSwapchain);
    if (XR_FAILED(result) || g_quadLayer.promptSwapchain == XR_NULL_HANDLE) {
        Log(L"OpenXR height prompt failed to create Vulkan swapchain result=" + std::to_wstring(result));
        return false;
    }

    uint32_t imageCount = 0;
    result = g_realEnumerateSwapchainImages(g_quadLayer.promptSwapchain, 0, &imageCount, nullptr);
    if (XR_FAILED(result) || imageCount == 0) {
        DestroyHeightPromptSwapchain();
        return false;
    }

    std::vector<XrSwapchainImageVulkanKHR> images(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    result = g_realEnumerateSwapchainImages(
        g_quadLayer.promptSwapchain, imageCount, &imageCount,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
    if (XR_FAILED(result)) {
        DestroyHeightPromptSwapchain();
        return false;
    }

    const std::vector<uint32_t> pixels = BuildPromptPixels(g_quadLayer.promptWidth, g_quadLayer.promptHeight);
    if (!EnsureVulkanUploadResources(pixels)) {
        DestroyHeightPromptSwapchain();
        return false;
    }

    for (uint32_t i = 0; i < imageCount; ++i) {
        uint32_t acquired = 0;
        XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        result = g_realAcquireSwapchainImage(g_quadLayer.promptSwapchain, &acquireInfo, &acquired);
        if (XR_FAILED(result)) {
            DestroyHeightPromptSwapchain();
            return false;
        }

        XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        waitInfo.timeout = XR_INFINITE_DURATION;
        result = g_realWaitSwapchainImage(g_quadLayer.promptSwapchain, &waitInfo);
        if (XR_FAILED(result)) {
            DestroyHeightPromptSwapchain();
            return false;
        }

        if (acquired < images.size() && images[acquired].image != VK_NULL_HANDLE &&
            !UploadPromptToVulkanImage(images[acquired].image,
                                       g_quadLayer.promptWidth,
                                       g_quadLayer.promptHeight)) {
            DestroyHeightPromptSwapchain();
            return false;
        }

        XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        result = g_realReleaseSwapchainImage(g_quadLayer.promptSwapchain, &releaseInfo);
        if (XR_FAILED(result)) {
            DestroyHeightPromptSwapchain();
            return false;
        }
    }

    g_quadLayer.promptTextureReady = true;
    Log(L"OpenXR height prompt Vulkan swapchain ready.");
    return true;
}

bool ShouldShowHeightPrompt() {
    SharedState* state = g_sharedState.load();
    if (!state || state->settings.showAlignmentPrompt == 0 || state->settings.vrMenuVisible != 0) {
        g_quadLayer.promptFirstFrameMs = 0;
        return false;
    }

    const uint64_t now = GetTickCount64();
    if (g_quadLayer.promptFirstFrameMs == 0) {
        g_quadLayer.promptFirstFrameMs = now;
    }
    return now - g_quadLayer.promptFirstFrameMs <= 10000;
}

XrVector3f RotateVector(const XrQuaternionf& q, XrVector3f v) {
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    const XrVector3f t{
        2.0f * (y * v.z - z * v.y),
        2.0f * (z * v.x - x * v.z),
        2.0f * (x * v.y - y * v.x)};
    return {
        v.x + w * t.x + (y * t.z - z * t.y),
        v.y + w * t.y + (z * t.x - x * t.z),
        v.z + w * t.z + (x * t.y - y * t.x)};
}

XrQuaternionf MulQuat(const XrQuaternionf& a, const XrQuaternionf& b) {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

bool BuildQuadLayer(const XrFrameEndInfo* frameEndInfo, XrCompositionLayerQuad& quad) {
    if (!frameEndInfo || !frameEndInfo->layers || frameEndInfo->layerCount == 0) {
        return false;
    }

    SharedState* shared = g_sharedState.load();
    const bool menuVisible = shared && shared->settings.vrMenuVisible != 0;
    const bool heightPromptVisible = !menuVisible && ShouldShowHeightPrompt();
    if (!menuVisible && !heightPromptVisible) {
        return false;
    }

    XrSwapchain layerSwapchain = XR_NULL_HANDLE;
    uint32_t layerWidth = 0;
    uint32_t layerHeight = 0;
    bool textureReady = false;

    if (menuVisible) {
        if (g_quadLayer.graphicsApi == GraphicsApi::D3D11 && !EnsureD3D11PromptSwapchain()) {
            return false;
        }
        if (g_quadLayer.graphicsApi == GraphicsApi::Vulkan && !EnsureVulkanPromptSwapchain()) {
            return false;
        }
        layerSwapchain = g_quadLayer.swapchain;
        layerWidth = g_quadLayer.width;
        layerHeight = g_quadLayer.height;
        textureReady = g_quadLayer.textureReady;
    } else {
        if (g_quadLayer.graphicsApi == GraphicsApi::D3D11 && !EnsureD3D11HeightPromptSwapchain()) {
            return false;
        }
        if (g_quadLayer.graphicsApi == GraphicsApi::Vulkan && !EnsureVulkanHeightPromptSwapchain()) {
            return false;
        }
        layerSwapchain = g_quadLayer.promptSwapchain;
        layerWidth = g_quadLayer.promptWidth;
        layerHeight = g_quadLayer.promptHeight;
        textureReady = g_quadLayer.promptTextureReady;
    }

    if (!textureReady || layerSwapchain == XR_NULL_HANDLE) {
        if (!g_quadLayer.warnedNoTexture) {
            g_quadLayer.warnedNoTexture = true;
            Log(L"OpenXR quad overlay has no upload path for graphics=" +
                std::wstring(GraphicsApiName(g_quadLayer.graphicsApi)));
        }
        return false;
    }

    XrSpace space = XR_NULL_HANDLE;
    if (!FindFrameProjectionSpace(frameEndInfo, space)) {
        return false;
    }

    quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                      XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    quad.space = space;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = layerSwapchain;
    quad.subImage.imageRect.offset = {0, 0};
    quad.subImage.imageRect.extent = {
        static_cast<int32_t>(layerWidth),
        static_cast<int32_t>(layerHeight)};
    if (menuVisible) {
        const PoseState left = shared->leftHandPose;
        quad.pose.orientation = MulQuat(
            {left.orientation.x, left.orientation.y, left.orientation.z, left.orientation.w},
            {-0.70710678f, 0.0f, 0.0f, 0.70710678f});
        XrVector3f offset = RotateVector(quad.pose.orientation, {0.0f, 0.10f, -0.18f});
        quad.pose.position = {
            left.positionMeters.x + offset.x,
            left.positionMeters.y + offset.y,
            left.positionMeters.z + offset.z};
        quad.size = {1.05f, 0.72f};
    } else {
        const PoseState hmd = shared ? shared->hmdPose : PoseState{};
        quad.pose.orientation = {
            hmd.orientation.x,
            hmd.orientation.y,
            hmd.orientation.z,
            hmd.orientation.w};
        XrVector3f offset = RotateVector(quad.pose.orientation, {0.0f, 0.0f, -1.35f});
        quad.pose.position = {
            hmd.positionMeters.x + offset.x,
            hmd.positionMeters.y + offset.y,
            hmd.positionMeters.z + offset.z};
        quad.size = {0.675f, 0.25f};
    }

    if (!g_quadLayer.warnedNoTexture) {
        g_quadLayer.warnedNoTexture = true;
        Log(L"OpenXR quad prompt appending composition layer.");
    }
    return true;
}

bool BuildLaserLayer(const XrFrameEndInfo* frameEndInfo, XrCompositionLayerQuad& laser) {
    SharedState* shared = g_sharedState.load();
    if (!shared || shared->settings.vrMenuVisible == 0 ||
        !frameEndInfo || g_quadLayer.swapchain == XR_NULL_HANDLE || !g_quadLayer.textureReady) {
        return false;
    }

    XrSpace space = XR_NULL_HANDLE;
    if (!FindFrameProjectionSpace(frameEndInfo, space))
        return false;

    const PoseState right = shared->rightHandPose;
    XrQuaternionf rightQ{right.orientation.x, right.orientation.y, right.orientation.z, right.orientation.w};
    XrVector3f forward = RotateVector(rightQ, {0.0f, 0.0f, -1.0f});
    const float half = 0.70710678f;

    laser = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    laser.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                       XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    laser.space = space;
    laser.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    laser.subImage.swapchain = g_quadLayer.swapchain;
    laser.subImage.imageRect.offset = {0, 0};
    laser.subImage.imageRect.extent = {16, 10};
    laser.pose.orientation = MulQuat(rightQ, {-half, 0.0f, 0.0f, half});
    laser.pose.position = {
        right.positionMeters.x + forward.x * 0.40f,
        right.positionMeters.y + forward.y * 0.40f,
        right.positionMeters.z + forward.z * 0.40f};
    laser.size = {0.008f, 0.80f};
    return true;
}

XrResult XRAPI_PTR Hook_xrEndFrame(XrSession session, const XrFrameEndInfo* frameEndInfo) {
    if (session == g_dolphinSession)
        ApplyDolphinDisplayRefreshRatePreference(g_dolphinInstance, session);

    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    if (session == g_quadLayer.session && BuildQuadLayer(frameEndInfo, quad)) {
        XrCompositionLayerQuad laser{XR_TYPE_COMPOSITION_LAYER_QUAD};
        const bool hasLaser = BuildLaserLayer(frameEndInfo, laser);
        std::vector<const XrCompositionLayerBaseHeader*> layers;
        layers.reserve(static_cast<size_t>(frameEndInfo->layerCount) + (hasLaser ? 2 : 1));
        for (uint32_t i = 0; i < frameEndInfo->layerCount; ++i) {
            layers.push_back(frameEndInfo->layers[i]);
        }
        layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad));
        if (hasLaser)
            layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&laser));

        XrFrameEndInfo patched = *frameEndInfo;
        patched.layerCount = static_cast<uint32_t>(layers.size());
        patched.layers = layers.data();
        g_quadLayer.appendedFrames++;
        return g_realEndFrame(session, &patched);
    }

    return g_realEndFrame(session, frameEndInfo);
}

XrResult XRAPI_PTR Hook_xrGetActionStateFloat(XrSession session, const XrActionStateGetInfo* getInfo,
                                              XrActionStateFloat* stateOut) {
    const XrResult result = g_realGetActionStateFloat(session, getInfo, stateOut);
    if (XR_SUCCEEDED(result) && getInfo && stateOut) {
        SharedState* state = g_sharedState.load();
        PoseState* pose = PoseForSubaction(state, getInfo->subactionPath);
        const std::string action = ActionName(getInfo->action);
        const float value = stateOut->isActive ? stateOut->currentState : 0.0f;
        if (pose) {
            if (action == "trigger_value") {
                pose->linearVelocityMetersPerSecond.x = std::clamp(value, 0.0f, 1.0f);
                MarkOpenXrActive(state);
            } else if (action == "thumbstick_x") {
                pose->linearVelocityMetersPerSecond.y = std::clamp(value, -1.0f, 1.0f);
                MarkOpenXrActive(state);
            } else if (action == "thumbstick_y") {
                pose->linearVelocityMetersPerSecond.z = std::clamp(value, -1.0f, 1.0f);
                MarkOpenXrActive(state);
            }
        }
    }
    return result;
}

XrResult XRAPI_PTR Hook_xrGetActionStateBoolean(XrSession session, const XrActionStateGetInfo* getInfo,
                                                XrActionStateBoolean* stateOut) {
    const XrResult result = g_realGetActionStateBoolean(session, getInfo, stateOut);
    if (XR_SUCCEEDED(result) && getInfo && stateOut) {
        SharedState* state = g_sharedState.load();
        PoseState* pose = PoseForSubaction(state, getInfo->subactionPath);
        const std::string action = ActionName(getInfo->action);
        if (!pose && state && action == "menu_click") {
            pose = &state->leftHandPose;
        }
        const bool pressed = stateOut->isActive && stateOut->currentState;
        if (state && state->settings.vrMenuVisible &&
            (action == "primary_click" || action == "secondary_click")) {
            stateOut->currentState = XR_FALSE;
            stateOut->changedSinceLastSync = XR_FALSE;
        }
        if (pose) {
            if (action == "trigger_click") {
                if (pressed)
                    pose->linearVelocityMetersPerSecond.x = 1.0f;
                pose->angularVelocityRadiansPerSecond.x = pressed ? 1.0f : 0.0f;
                if (pressed) pose->buttons |= kButtonTriggerClick;
                else pose->buttons &= ~kButtonTriggerClick;
                MarkOpenXrActive(state);
            } else if ((action.find("thumbstick") != std::string::npos &&
                        action.find("click") != std::string::npos) ||
                       action == "menu_click") {
                pose->angularVelocityRadiansPerSecond.y = pressed ? 1.0f : 0.0f;
                if (pressed) pose->buttons |= kButtonThumbstickClick;
                else pose->buttons &= ~kButtonThumbstickClick;
                MarkOpenXrActive(state);
            } else if (action.find("button_a") != std::string::npos ||
                       action.find("a_click") != std::string::npos ||
                       action == "primary_click" ||
                       action == "a") {
                if (pressed) pose->buttons |= kButtonA;
                else pose->buttons &= ~kButtonA;
                if (state && state->settings.vrMenuVisible) {
                    stateOut->currentState = XR_FALSE;
                    stateOut->changedSinceLastSync = XR_FALSE;
                }
                MarkOpenXrActive(state);
            } else if (action.find("button_b") != std::string::npos ||
                       action.find("b_click") != std::string::npos ||
                       action == "secondary_click" ||
                       action == "b") {
                if (pressed) pose->buttons |= kButtonB;
                else pose->buttons &= ~kButtonB;
                if (state && state->settings.vrMenuVisible) {
                    stateOut->currentState = XR_FALSE;
                    stateOut->changedSinceLastSync = XR_FALSE;
                }
                MarkOpenXrActive(state);
            }
        }
    }
    return result;
}

XrResult XRAPI_PTR Hook_xrGetActionStateVector2f(XrSession session, const XrActionStateGetInfo* getInfo,
                                                 XrActionStateVector2f* stateOut) {
    const XrResult result = g_realGetActionStateVector2f(session, getInfo, stateOut);
    if (XR_SUCCEEDED(result) && getInfo && stateOut) {
        SharedState* state = g_sharedState.load();
        PoseState* pose = PoseForSubaction(state, getInfo->subactionPath);
        const std::string action = ActionName(getInfo->action);
        if (pose && (action == "thumbstick" || action == "primary_2d_axis")) {
            const float x = stateOut->isActive ? stateOut->currentState.x : 0.0f;
            const float y = stateOut->isActive ? stateOut->currentState.y : 0.0f;
            pose->linearVelocityMetersPerSecond.y = std::clamp(x, -1.0f, 1.0f);
            pose->linearVelocityMetersPerSecond.z = std::clamp(y, -1.0f, 1.0f);
            MarkOpenXrActive(state);
        }
    }
    return result;
}

XrResult XRAPI_PTR Hook_xrCreateReferenceSpace(XrSession session,
                                               const XrReferenceSpaceCreateInfo* createInfo,
                                               XrSpace* space) {
    if (!createInfo || !g_realCreateReferenceSpace) {
        return g_realCreateReferenceSpace(session, createInfo, space);
    }

    XrReferenceSpaceCreateInfo stageInfo = *createInfo;
    XrReferenceSpaceCreateInfo originalInfo = *createInfo;
    const bool wantsHeadLocalSpace =
        createInfo->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL ||
        createInfo->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_STAGE;
    if (wantsHeadLocalSpace) {
        stageInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
        stageInfo.poseInReferenceSpace = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
    }

    XrResult result = g_realCreateReferenceSpace(session, wantsHeadLocalSpace ? &stageInfo : createInfo, space);
    bool usingStage = wantsHeadLocalSpace && XR_SUCCEEDED(result) && space && *space != XR_NULL_HANDLE;
    if (wantsHeadLocalSpace && !usingStage) {
        result = g_realCreateReferenceSpace(session, &originalInfo, space);
    }
    if (XR_SUCCEEDED(result) && space && *space != XR_NULL_HANDLE &&
        (createInfo->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL ||
         createInfo->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_STAGE)) {
        g_dolphinSession = session;
        g_dolphinReferenceSpace = *space;
        g_dolphinOpenXrManager = nullptr;
        g_lastManagerScanMs = 0;
        g_dolphinHomeZeroed = false;
        g_appliedViewHeightGeneration = 0;
        g_validLocateViewFrames = 0;
        g_lastManagerCandidate = nullptr;
        g_managerStableScans = 0;
        g_dolphinReferenceSpaceIsStage = usingStage;
        g_startupCenterSpoofFrames = usingStage ? 180 : 0;
        Log(usingStage ? L"OpenXrHooks forced Dolphin reference space to STAGE for play-space origin."
                       : L"OpenXrHooks kept Dolphin original reference space; STAGE was unavailable.");
    }
    return result;
}

XrResult XRAPI_PTR Hook_xrLocateViews(XrSession session, const XrViewLocateInfo* viewLocateInfo,
                                      XrViewState* viewState, uint32_t viewCapacityInput,
                                      uint32_t* viewCountOutput, XrView* views) {
    const XrResult result = g_realLocateViews(session, viewLocateInfo, viewState, viewCapacityInput, viewCountOutput, views);
    if (XR_SUCCEEDED(result) && views && viewCountOutput && *viewCountOutput > 0 && viewState &&
        IsPoseValid(viewState->viewStateFlags)) {
        const uint32_t count = std::min<uint32_t>(*viewCountOutput, viewCapacityInput);
        if (g_validLocateViewFrames < 1000000)
            ++g_validLocateViewFrames;
        SharedState* state = g_sharedState.load();
        float headCenterY = views[0].pose.position.y;
        if (count >= 2) {
            headCenterY = (views[0].pose.position.y + views[1].pose.position.y) * 0.5f;
        }
        if (state) {
            XrPosef pose = views[0].pose;
            if (count >= 2) {
                pose.position.x = (views[0].pose.position.x + views[1].pose.position.x) * 0.5f;
                pose.position.y = (views[0].pose.position.y + views[1].pose.position.y) * 0.5f;
                pose.position.z = (views[0].pose.position.z + views[1].pose.position.z) * 0.5f;
            }
            CopyPose(state->hmdPose, pose);
            MarkOpenXrActive(state);
        }
        InstallHeightOnlyRecenterPatch();
        ApplyDolphinHomeHeightOnly(headCenterY);
        CenterViewsForDolphinStartup(count, views);
    }
    return result;
}

XrResult XRAPI_PTR Hook_xrLocateSpace(XrSpace space, XrSpace baseSpace, XrTime time, XrSpaceLocation* location) {
    const XrResult result = g_realLocateSpace(space, baseSpace, time, location);
    if (XR_SUCCEEDED(result) && location && IsPoseValid(location->locationFlags)) {
        std::string tag;
        {
            std::lock_guard<std::mutex> guard(g_mutex);
            const auto it = g_spaces.find(space);
            if (it != g_spaces.end())
                tag = it->second;
        }

        SharedState* state = g_sharedState.load();
        if (state && (tag.find("aim_pose") != std::string::npos ||
                      tag.find("grip_pose") != std::string::npos)) {
            const bool isLeft = tag.find("/user/hand/left") != std::string::npos;
            const bool isRight = tag.find("/user/hand/right") != std::string::npos;
            PoseState* hand = isLeft ? &state->leftHandPose : (isRight ? &state->rightHandPose : nullptr);
            if (!hand)
                return result;

            const uint64_t now = GetTickCount64();
            uint64_t& gripMs = isLeft ? g_leftGripPoseMs : g_rightGripPoseMs;
            if (tag.find("grip_pose") != std::string::npos) {
                CopyPosePosition(*hand, location->pose);
                gripMs = now;
            } else {
                CopyPoseOrientation(*hand, location->pose);
                if (gripMs == 0 || now - gripMs > 250)
                    CopyPosePosition(*hand, location->pose);
            }
            MarkOpenXrActive(state);
        }
    }
    return result;
}

PFN_xrVoidFunction WrapProc(const char* name, PFN_xrVoidFunction proc) {
    if (!name || !proc)
        return proc;
    if (std::strcmp(name, "xrCreateInstance") == 0) {
        if (!g_realCreateInstance)
            g_realCreateInstance = reinterpret_cast<PFN_xrCreateInstance>(proc);
        return reinterpret_cast<PFN_xrVoidFunction>(&Hook_xrCreateInstance);
    }
    if (std::strcmp(name, "xrStringToPath") == 0) {
        if (!g_realStringToPath)
            g_realStringToPath = reinterpret_cast<PFN_xrStringToPath>(proc);
        return reinterpret_cast<PFN_xrVoidFunction>(&Hook_xrStringToPath);
    }
    if (std::strcmp(name, "xrCreateAction") == 0) {
        if (!g_realCreateAction)
            g_realCreateAction = reinterpret_cast<PFN_xrCreateAction>(proc);
        return reinterpret_cast<PFN_xrVoidFunction>(&Hook_xrCreateAction);
    }
    if (std::strcmp(name, "xrCreateActionSpace") == 0) {
        if (!g_realCreateActionSpace)
            g_realCreateActionSpace = reinterpret_cast<PFN_xrCreateActionSpace>(proc);
        return reinterpret_cast<PFN_xrVoidFunction>(&Hook_xrCreateActionSpace);
    }
    if (std::strcmp(name, "xrCreateSession") == 0) {
        if (!g_realCreateSession)
            g_realCreateSession = reinterpret_cast<PFN_xrCreateSession>(proc);
        return reinterpret_cast<PFN_xrVoidFunction>(&Hook_xrCreateSession);
    }
    if (std::strcmp(name, "xrCreateReferenceSpace") == 0) {
        if (!g_realCreateReferenceSpace)
            g_realCreateReferenceSpace = reinterpret_cast<PFN_xrCreateReferenceSpace>(proc);
        return reinterpret_cast<PFN_xrVoidFunction>(&Hook_xrCreateReferenceSpace);
    }
    if (std::strcmp(name, "xrCreateSwapchain") == 0) {
        if (!g_realCreateSwapchain)
            g_realCreateSwapchain = reinterpret_cast<PFN_xrCreateSwapchain>(proc);
        return proc;
    }
    if (std::strcmp(name, "xrEnumerateSwapchainFormats") == 0) {
        if (!g_realEnumerateSwapchainFormats)
            g_realEnumerateSwapchainFormats = reinterpret_cast<PFN_xrEnumerateSwapchainFormats>(proc);
        return proc;
    }
    if (std::strcmp(name, "xrDestroySwapchain") == 0) {
        if (!g_realDestroySwapchain)
            g_realDestroySwapchain = reinterpret_cast<PFN_xrDestroySwapchain>(proc);
        return proc;
    }
    if (std::strcmp(name, "xrEnumerateSwapchainImages") == 0) {
        if (!g_realEnumerateSwapchainImages)
            g_realEnumerateSwapchainImages = reinterpret_cast<PFN_xrEnumerateSwapchainImages>(proc);
        return proc;
    }
    if (std::strcmp(name, "xrAcquireSwapchainImage") == 0) {
        if (!g_realAcquireSwapchainImage)
            g_realAcquireSwapchainImage = reinterpret_cast<PFN_xrAcquireSwapchainImage>(proc);
        return proc;
    }
    if (std::strcmp(name, "xrWaitSwapchainImage") == 0) {
        if (!g_realWaitSwapchainImage)
            g_realWaitSwapchainImage = reinterpret_cast<PFN_xrWaitSwapchainImage>(proc);
        return proc;
    }
    if (std::strcmp(name, "xrReleaseSwapchainImage") == 0) {
        if (!g_realReleaseSwapchainImage)
            g_realReleaseSwapchainImage = reinterpret_cast<PFN_xrReleaseSwapchainImage>(proc);
        return proc;
    }
    if (std::strcmp(name, "xrEndFrame") == 0) {
        if (!g_realEndFrame)
            g_realEndFrame = reinterpret_cast<PFN_xrEndFrame>(proc);
        return reinterpret_cast<PFN_xrVoidFunction>(&Hook_xrEndFrame);
    }
    if (std::strcmp(name, "xrGetActionStateFloat") == 0) {
        if (!g_realGetActionStateFloat)
            g_realGetActionStateFloat = reinterpret_cast<PFN_xrGetActionStateFloat>(proc);
        return reinterpret_cast<PFN_xrVoidFunction>(&Hook_xrGetActionStateFloat);
    }
    if (std::strcmp(name, "xrGetActionStateBoolean") == 0) {
        if (!g_realGetActionStateBoolean)
            g_realGetActionStateBoolean = reinterpret_cast<PFN_xrGetActionStateBoolean>(proc);
        return reinterpret_cast<PFN_xrVoidFunction>(&Hook_xrGetActionStateBoolean);
    }
    if (std::strcmp(name, "xrGetActionStateVector2f") == 0) {
        if (!g_realGetActionStateVector2f)
            g_realGetActionStateVector2f = reinterpret_cast<PFN_xrGetActionStateVector2f>(proc);
        return reinterpret_cast<PFN_xrVoidFunction>(&Hook_xrGetActionStateVector2f);
    }
    if (std::strcmp(name, "xrLocateViews") == 0) {
        if (!g_realLocateViews)
            g_realLocateViews = reinterpret_cast<PFN_xrLocateViews>(proc);
        return reinterpret_cast<PFN_xrVoidFunction>(&Hook_xrLocateViews);
    }
    if (std::strcmp(name, "xrLocateSpace") == 0) {
        if (!g_realLocateSpace)
            g_realLocateSpace = reinterpret_cast<PFN_xrLocateSpace>(proc);
        return reinterpret_cast<PFN_xrVoidFunction>(&Hook_xrLocateSpace);
    }
    return proc;
}

bool PatchModuleImports(HMODULE module, const char* importedDllName) {
    auto* base = reinterpret_cast<uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (!IsReadableMemory(dos, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (!IsReadableMemory(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE)
        return false;

    const IMAGE_DATA_DIRECTORY& importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (importDir.VirtualAddress == 0 || importDir.Size == 0)
        return false;

    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + importDir.VirtualAddress);
    bool patchedAny = false;
    for (; descriptor->Name; ++descriptor) {
        const char* dllName = reinterpret_cast<const char*>(base + descriptor->Name);
        if (_stricmp(dllName, importedDllName) != 0)
            continue;

        auto* originalThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk);
        auto* firstThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
        if (!descriptor->OriginalFirstThunk)
            originalThunk = firstThunk;

        for (; originalThunk->u1.AddressOfData; ++originalThunk, ++firstThunk) {
            if (IMAGE_SNAP_BY_ORDINAL(originalThunk->u1.Ordinal))
                continue;

            auto* importByName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + originalThunk->u1.AddressOfData);
            const char* functionName = reinterpret_cast<const char*>(importByName->Name);
            if (std::strcmp(functionName, "xrGetInstanceProcAddr") == 0) {
                patchedAny |= PatchImportThunk(reinterpret_cast<void**>(&firstThunk->u1.Function),
                                               reinterpret_cast<void*>(&Hook_xrGetInstanceProcAddr),
                                               reinterpret_cast<void**>(&g_realGetInstanceProcAddr));
            } else if (_stricmp(importedDllName, "kernel32.dll") == 0 &&
                       std::strcmp(functionName, "GetProcAddress") == 0) {
                patchedAny |= PatchImportThunk(reinterpret_cast<void**>(&firstThunk->u1.Function),
                                               reinterpret_cast<void*>(&Hook_GetProcAddress),
                                               reinterpret_cast<void**>(&g_realGetProcAddress));
            }
        }
    }
    return patchedAny;
}

bool PatchLoadedModules() {
    HMODULE modules[1024]{};
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed))
        return false;

    bool patchedAny = false;
    const size_t count = std::min<size_t>(needed / sizeof(HMODULE), std::size(modules));
    for (size_t i = 0; i < count; ++i) {
        patchedAny |= PatchModuleImports(modules[i], "openxr_loader.dll");
        patchedAny |= PatchModuleImports(modules[i], "OpenXR_loader.dll");
        patchedAny |= PatchModuleImports(modules[i], "kernel32.dll");
    }
    return patchedAny;
}

void* ResolveExportJumpTarget(void* function) {
    auto* bytes = static_cast<uint8_t*>(function);
    if (!IsReadableMemory(bytes, 8))
        return function;
    if (bytes[0] == 0xE9) {
        int32_t displacement = 0;
        std::memcpy(&displacement, bytes + 1, sizeof(displacement));
        return bytes + 5 + displacement;
    }
    return function;
}

bool InstallAbsoluteJump(void* target, void* replacement) {
    auto* bytes = static_cast<uint8_t*>(target);
    if (!IsReadableMemory(bytes, 12))
        return false;
    if (bytes[0] == 0x48 && bytes[1] == 0xB8 && bytes[10] == 0xFF && bytes[11] == 0xE0)
        return true;

    uint8_t patch[12] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};
    std::memcpy(patch + 2, &replacement, sizeof(replacement));

    DWORD oldProtect = 0;
    if (!VirtualProtect(bytes, sizeof(patch), PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;
    std::memcpy(bytes, patch, sizeof(patch));
    DWORD ignored = 0;
    VirtualProtect(bytes, sizeof(patch), oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), bytes, sizeof(patch));
    return true;
}

void* CreateTrampoline(void* target, size_t patchSize) {
    constexpr size_t kJumpSize = 12;
    auto* targetBytes = static_cast<uint8_t*>(target);
    auto* stub = static_cast<uint8_t*>(VirtualAlloc(nullptr, patchSize + kJumpSize, MEM_COMMIT | MEM_RESERVE,
                                                    PAGE_EXECUTE_READWRITE));
    if (!stub)
        return nullptr;
    std::memcpy(stub, targetBytes, patchSize);
    uint8_t jump[kJumpSize] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};
    void* continuation = targetBytes + patchSize;
    std::memcpy(jump + 2, &continuation, sizeof(continuation));
    std::memcpy(stub + patchSize, jump, sizeof(jump));
    FlushInstructionCache(GetCurrentProcess(), stub, patchSize + kJumpSize);
    return stub;
}

bool InstallAbsoluteJumpWithTrampoline(void* target, void* replacement, void** original) {
    constexpr size_t kPatchSize = 16;
    auto* bytes = static_cast<uint8_t*>(target);
    if (!IsReadableMemory(bytes, kPatchSize))
        return false;
    if (bytes[0] == 0x48 && bytes[1] == 0xB8 && bytes[10] == 0xFF && bytes[11] == 0xE0)
        return true;

    if (original && !*original) {
        *original = CreateTrampoline(target, kPatchSize);
        if (!*original)
            return false;
    }

    uint8_t patch[kPatchSize] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0, 0x90, 0x90, 0x90, 0x90};
    std::memcpy(patch + 2, &replacement, sizeof(replacement));

    DWORD oldProtect = 0;
    if (!VirtualProtect(bytes, sizeof(patch), PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;
    std::memcpy(bytes, patch, sizeof(patch));
    DWORD ignored = 0;
    VirtualProtect(bytes, sizeof(patch), oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), bytes, sizeof(patch));
    return true;
}

bool InstallInlineDetour(HMODULE openxr) {
    void* raw = reinterpret_cast<void*>(GetProcAddress(openxr, "xrGetInstanceProcAddr"));
    if (!raw)
        return false;
    if (!g_realGetInstanceProcAddr)
        g_realGetInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(ResolveExportJumpTarget(raw));

    const bool ok = InstallAbsoluteJump(raw, reinterpret_cast<void*>(&Hook_xrGetInstanceProcAddr));
    if (ok && !g_inlineDetourInstalled.exchange(true))
        Log(L"OpenXrHooks inline detour installed on xrGetInstanceProcAddr.");
    return ok;
}

bool InstallRuntimeNegotiationDetour(HMODULE runtime) {
    void* raw = reinterpret_cast<void*>(GetProcAddress(runtime, "xrNegotiateLoaderRuntimeInterface"));
    if (!raw)
        return false;
    const bool ok = InstallAbsoluteJumpWithTrampoline(
        raw,
        reinterpret_cast<void*>(&Hook_xrNegotiateLoaderRuntimeInterface),
        reinterpret_cast<void**>(&g_realNegotiateLoaderRuntimeInterface));
    if (ok && !g_runtimeInstalled.exchange(true))
        Log(L"OpenXrHooks installed on xrNegotiateLoaderRuntimeInterface.");
    return ok;
}

bool InstallGetProcAddressDetour() {
    return false;
}

} // namespace

bool InstallIfAvailable(SharedState* state) {
    g_sharedState = state;
    HMODULE openxr = GetModuleHandleW(L"openxr_loader.dll");
    if (!openxr)
        openxr = GetModuleHandleW(L"OpenXR_loader.dll");
    HMODULE runtime = GetModuleHandleW(L"vrclient_x64.dll");
    if (!runtime)
        runtime = GetModuleHandleW(L"LibOVRRT64_1.dll");
    if (!runtime)
        runtime = GetModuleHandleW(L"MixedRealityRuntime.dll");
    const bool getProcDetour = InstallGetProcAddressDetour();
    const bool inlineDetour = openxr ? InstallInlineDetour(openxr) : false;
    const bool runtimeDetour = runtime ? InstallRuntimeNegotiationDetour(runtime) : false;
    const bool patched = PatchLoadedModules();
    const bool installedAny = inlineDetour || patched || runtimeDetour || getProcDetour;
    if (state) {
        state->openxrInstallAttempts++;
        uint32_t moduleFlags = 0;
        if (openxr)
            moduleFlags |= 1u << 0;
        if (runtime)
            moduleFlags |= 1u << 1;
        state->openxrModuleFlags = moduleFlags;
        state->hookStatusFlags |= HookStatusDllAlive;
        if (installedAny || g_installed.load())
            state->hookStatusFlags |= HookStatusOpenXrInstalled;
        if (runtimeDetour || g_runtimeInstalled.load())
            state->hookStatusFlags |= HookStatusOpenXrRuntimeInstalled;
        if (g_realGetInstanceProcAddr)
            state->hookStatusFlags |= HookStatusOpenXrGetProcReady;
    }
    if (installedAny && !g_installed.exchange(true)) {
        Log(std::wstring(L"OpenXrHooks installed. inline=") + (inlineDetour ? L"yes" : L"no") +
            L" IAT patched=" + (patched ? L"yes" : L"no") +
            L" runtime=" + (runtimeDetour ? L"yes" : L"no") +
            L" getproc=" + (getProcDetour ? L"yes" : L"no"));
    }
    return installedAny;
}

void Poll(SharedState* state) {
    g_sharedState = state;

    const uint64_t now = GetTickCount64();
    if (!g_installed.load() || !g_runtimeInstalled.load() || !g_realGetInstanceProcAddr) {
        if (now - g_lastInstallCheckMs >= 1000) {
            g_lastInstallCheckMs = now;
            InstallIfAvailable(state);
        }
    }
    if (state) {
        state->hookStatusFlags |= HookStatusDllAlive;
        if (g_installed.load())
            state->hookStatusFlags |= HookStatusOpenXrInstalled;
        if (g_runtimeInstalled.load())
            state->hookStatusFlags |= HookStatusOpenXrRuntimeInstalled;
        if (g_realGetInstanceProcAddr)
            state->hookStatusFlags |= HookStatusOpenXrGetProcReady;
    }

    if (g_installed.load() && now - g_lastLogMs > 5000) {
        g_lastLogMs = now;
        std::lock_guard<std::mutex> guard(g_mutex);
        Log(L"OpenXrHooks status: paths=" + std::to_wstring(g_paths.size()) +
            L" actions=" + std::to_wstring(g_actions.size()) +
            L" spaces=" + std::to_wstring(g_spaces.size()) +
            L" generation=" + std::to_wstring(g_generation));
    }
}

void Shutdown() {
    g_sharedState = nullptr;
    DestroyPromptSwapchain();
    DestroyHeightPromptSwapchain();
    CleanupVulkanQuadResources();
    Log(L"OpenXrHooks shutdown.");
}

} // namespace PrimedGun::Hook::OpenXrHooks
