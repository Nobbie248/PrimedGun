#include "VulkanHooks.h"

#include "GameTimingHooks.h"
#include "../hook_common/PrimedGunShared.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace PrimedGun::Hook {
void Log(std::wstring_view message);
}

namespace PrimedGun::Hook::VulkanHooks {
namespace {

#ifndef VKAPI_PTR
#define VKAPI_PTR __stdcall
#endif

using VkInstance = struct VkInstance_T*;
using VkDevice = struct VkDevice_T*;
using VkCommandBuffer = struct VkCommandBuffer_T*;
using VkQueue = struct VkQueue_T*;
using VkImage = struct VkImage_T*;
using VkImageView = struct VkImageView_T*;
using VkBuffer = struct VkBuffer_T*;
using VkRenderPass = struct VkRenderPass_T*;
using VkFramebuffer = struct VkFramebuffer_T*;
using VkDeviceMemory = struct VkDeviceMemory_T*;
using VkDescriptorSet = struct VkDescriptorSet_T*;
using VkPipeline = struct VkPipeline_T*;
using VkPipelineLayout = struct VkPipelineLayout_T*;
using VkSampler = struct VkSampler_T*;
using VkResult = int32_t;
using VkStructureType = int32_t;
using VkFlags = uint32_t;
using VkBool32 = uint32_t;
using PFN_vkVoidFunction = void (*)();

struct VkExtent2D {
    uint32_t width;
    uint32_t height;
};

struct VkOffset2D {
    int32_t x;
    int32_t y;
};

struct VkRect2D {
    VkOffset2D offset;
    VkExtent2D extent;
};

struct VkViewport {
    float x;
    float y;
    float width;
    float height;
    float minDepth;
    float maxDepth;
};

struct VkClearColorValue {
    union {
        float float32[4];
        int32_t int32[4];
        uint32_t uint32[4];
    };
};

struct VkClearDepthStencilValue {
    float depth;
    uint32_t stencil;
};

union VkClearValue {
    VkClearColorValue color;
    VkClearDepthStencilValue depthStencil;
};

struct VkClearAttachment {
    VkFlags aspectMask;
    uint32_t colorAttachment;
    VkClearValue clearValue;
};

struct VkClearRect {
    VkRect2D rect;
    uint32_t baseArrayLayer;
    uint32_t layerCount;
};

struct VkImageSubresourceRange {
    VkFlags aspectMask;
    uint32_t baseMipLevel;
    uint32_t levelCount;
    uint32_t baseArrayLayer;
    uint32_t layerCount;
};

struct VkImageSubresourceLayers {
    VkFlags aspectMask;
    uint32_t mipLevel;
    uint32_t baseArrayLayer;
    uint32_t layerCount;
};

struct VkOffset3D {
    int32_t x;
    int32_t y;
    int32_t z;
};

struct VkExtent3D {
    uint32_t width;
    uint32_t height;
    uint32_t depth;
};

struct VkImageCreateInfo {
    VkStructureType sType;
    const void* pNext;
    VkFlags flags;
    int32_t imageType;
    int32_t format;
    VkExtent3D extent;
    uint32_t mipLevels;
    uint32_t arrayLayers;
    int32_t samples;
    int32_t tiling;
    VkFlags usage;
    int32_t sharingMode;
    uint32_t queueFamilyIndexCount;
    const uint32_t* pQueueFamilyIndices;
    int32_t initialLayout;
};

struct VkImageCopy {
    VkImageSubresourceLayers srcSubresource;
    VkOffset3D srcOffset;
    VkImageSubresourceLayers dstSubresource;
    VkOffset3D dstOffset;
    VkExtent3D extent;
};

struct VkComponentMapping {
    int32_t r;
    int32_t g;
    int32_t b;
    int32_t a;
};

struct VkImageViewCreateInfo {
    VkStructureType sType;
    const void* pNext;
    VkFlags flags;
    VkImage image;
    int32_t viewType;
    int32_t format;
    VkComponentMapping components;
    VkImageSubresourceRange subresourceRange;
};

struct VkFramebufferCreateInfo {
    VkStructureType sType;
    const void* pNext;
    VkFlags flags;
    VkRenderPass renderPass;
    uint32_t attachmentCount;
    const VkImageView* pAttachments;
    uint32_t width;
    uint32_t height;
    uint32_t layers;
};

struct VkRenderPassBeginInfo {
    VkStructureType sType;
    const void* pNext;
    VkRenderPass renderPass;
    VkFramebuffer framebuffer;
    VkRect2D renderArea;
    uint32_t clearValueCount;
    const void* pClearValues;
};

struct VkRenderingInfo {
    VkStructureType sType;
    const void* pNext;
    VkFlags flags;
    VkRect2D renderArea;
    uint32_t layerCount;
    uint32_t viewMask;
    uint32_t colorAttachmentCount;
    const void* pColorAttachments;
    const void* pDepthAttachment;
    const void* pStencilAttachment;
};

struct VkPresentInfoKHR {
    VkStructureType sType;
    const void* pNext;
    uint32_t waitSemaphoreCount;
    const void* pWaitSemaphores;
    uint32_t swapchainCount;
    const void* pSwapchains;
    const uint32_t* pImageIndices;
    VkResult* pResults;
};

struct VkDescriptorImageInfo {
    VkSampler sampler;
    VkImageView imageView;
    int32_t imageLayout;
};

struct VkWriteDescriptorSet {
    VkStructureType sType;
    const void* pNext;
    VkDescriptorSet dstSet;
    uint32_t dstBinding;
    uint32_t dstArrayElement;
    uint32_t descriptorCount;
    int32_t descriptorType;
    const VkDescriptorImageInfo* pImageInfo;
    const void* pBufferInfo;
    const void* pTexelBufferView;
};

enum VkSubpassContents : int32_t {
    VK_SUBPASS_CONTENTS_INLINE = 0,
    VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS = 1,
};

enum VkImageLayout : int32_t {
    VK_IMAGE_LAYOUT_UNDEFINED = 0,
};

enum VkFilter : int32_t {
    VK_FILTER_NEAREST = 0,
    VK_FILTER_LINEAR = 1,
};

using PFN_vkGetInstanceProcAddr = PFN_vkVoidFunction (*)(VkInstance, const char*);
using PFN_vkGetDeviceProcAddr = PFN_vkVoidFunction (*)(VkDevice, const char*);
using PFN_vkCmdSetViewport = void (*)(VkCommandBuffer, uint32_t, uint32_t, const VkViewport*);
using PFN_vkCmdSetScissor = void (*)(VkCommandBuffer, uint32_t, uint32_t, const VkRect2D*);
using PFN_vkCmdBeginRenderPass = void (*)(VkCommandBuffer, const VkRenderPassBeginInfo*, VkSubpassContents);
using PFN_vkCmdEndRenderPass = void (*)(VkCommandBuffer);
using PFN_vkCmdBeginRendering = void (*)(VkCommandBuffer, const VkRenderingInfo*);
using PFN_vkCmdEndRendering = void (*)(VkCommandBuffer);
using PFN_vkCmdClearAttachments = void (*)(VkCommandBuffer, uint32_t, const VkClearAttachment*, uint32_t, const VkClearRect*);
using PFN_vkCmdClearColorImage = void (*)(VkCommandBuffer, VkImage, VkImageLayout, const VkClearColorValue*, uint32_t, const VkImageSubresourceRange*);
using PFN_vkCmdCopyImage = void (*)(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, uint32_t, const void*);
using PFN_vkCmdBlitImage = void (*)(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, uint32_t, const void*, VkFilter);
using PFN_vkQueuePresentKHR = VkResult (*)(VkQueue, const VkPresentInfoKHR*);
using PFN_vkCreateImage = VkResult (*)(VkDevice, const VkImageCreateInfo*, const void*, VkImage*);
using PFN_vkDestroyImage = void (*)(VkDevice, VkImage, const void*);
using PFN_vkCreateImageView = VkResult (*)(VkDevice, const VkImageViewCreateInfo*, const void*, VkImageView*);
using PFN_vkDestroyImageView = void (*)(VkDevice, VkImageView, const void*);
using PFN_vkCreateFramebuffer = VkResult (*)(VkDevice, const VkFramebufferCreateInfo*, const void*, VkFramebuffer*);
using PFN_vkDestroyFramebuffer = void (*)(VkDevice, VkFramebuffer, const void*);
using PFN_vkUpdateDescriptorSets = void (*)(VkDevice, uint32_t, const VkWriteDescriptorSet*, uint32_t, const void*);
using PFN_vkCmdBindDescriptorSets = void (*)(VkCommandBuffer, int32_t, VkPipelineLayout, uint32_t, uint32_t, const VkDescriptorSet*, uint32_t, const uint32_t*);
using PFN_vkCmdBindPipeline = void (*)(VkCommandBuffer, int32_t, VkPipeline);
using PFN_vkCmdDraw = void (*)(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t);
using PFN_vkCmdDrawIndexed = void (*)(VkCommandBuffer, uint32_t, uint32_t, uint32_t, int32_t, uint32_t);

PFN_vkGetInstanceProcAddr g_realGetInstanceProcAddr = nullptr;
PFN_vkGetDeviceProcAddr g_realGetDeviceProcAddr = nullptr;
PFN_vkCmdSetViewport g_realCmdSetViewport = nullptr;
PFN_vkCmdSetScissor g_realCmdSetScissor = nullptr;
PFN_vkCmdBeginRenderPass g_realCmdBeginRenderPass = nullptr;
PFN_vkCmdEndRenderPass g_realCmdEndRenderPass = nullptr;
PFN_vkCmdBeginRendering g_realCmdBeginRendering = nullptr;
PFN_vkCmdEndRendering g_realCmdEndRendering = nullptr;
PFN_vkCmdClearAttachments g_realCmdClearAttachments = nullptr;
PFN_vkCmdClearColorImage g_realCmdClearColorImage = nullptr;
PFN_vkCmdCopyImage g_realCmdCopyImage = nullptr;
PFN_vkCmdBlitImage g_realCmdBlitImage = nullptr;
PFN_vkQueuePresentKHR g_realQueuePresentKHR = nullptr;
PFN_vkCreateImage g_realCreateImage = nullptr;
PFN_vkDestroyImage g_realDestroyImage = nullptr;
PFN_vkCreateImageView g_realCreateImageView = nullptr;
PFN_vkDestroyImageView g_realDestroyImageView = nullptr;
PFN_vkCreateFramebuffer g_realCreateFramebuffer = nullptr;
PFN_vkDestroyFramebuffer g_realDestroyFramebuffer = nullptr;
PFN_vkUpdateDescriptorSets g_realUpdateDescriptorSets = nullptr;
PFN_vkCmdBindDescriptorSets g_realCmdBindDescriptorSets = nullptr;
PFN_vkCmdBindPipeline g_realCmdBindPipeline = nullptr;
PFN_vkCmdDraw g_realCmdDraw = nullptr;
PFN_vkCmdDrawIndexed g_realCmdDrawIndexed = nullptr;

std::atomic<uint64_t> g_viewportCalls = 0;
std::atomic<uint64_t> g_scissorCalls = 0;
std::atomic<uint64_t> g_renderPassCalls = 0;
std::atomic<uint64_t> g_clearCalls = 0;
std::atomic<uint64_t> g_copyCalls = 0;
std::atomic<uint64_t> g_blitCalls = 0;
std::atomic<uint64_t> g_presentCalls = 0;
std::atomic<bool> g_installed = false;
std::atomic<bool> g_inlineDetoursInstalled = false;
std::atomic<bool> g_forceNextFrameSummary = false;
std::atomic<bool> g_captureDrawCalls = false;
std::atomic<bool> g_armCaptureNextFrame = false;
std::atomic<uint64_t> g_drawTraceLines = 0;
std::atomic<bool> g_showAlignmentPrompt = false;
std::atomic<uint64_t> g_overlayDraws = 0;
std::mutex g_logMutex;
std::mutex g_frameMutex;
std::mutex g_resourceMutex;

struct ViewportCapture {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

struct ScissorCapture {
    int32_t x = 0;
    int32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct PassCapture {
    VkRenderPass renderPass = nullptr;
    VkFramebuffer framebuffer = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t clearCount = 0;
    int vrEyeIndex = -1;
    uint32_t viewportCount = 0;
    uint32_t scissorCount = 0;
    std::array<ViewportCapture, 4> firstViewports{};
    std::array<ScissorCapture, 4> firstScissors{};
};

struct CopyCapture {
    VkImage srcImage = nullptr;
    VkImage dstImage = nullptr;
    uint32_t regionCount = 0;
    VkExtent3D firstExtent{};
    VkOffset3D firstSrcOffset{};
    VkOffset3D firstDstOffset{};
};

struct FrameCapture {
    uint64_t frameIndex = 0;
    uint32_t copyImageCount = 0;
    uint32_t blitImageCount = 0;
    uint32_t vrTargetPassCount = 0;
    std::array<CopyCapture, 8> firstCopies{};
    std::vector<PassCapture> passes;
    int activePass = -1;
};

FrameCapture g_frame;
std::atomic<uint64_t> g_activitySnapshots = 0;
std::atomic<bool> g_previousFrameHadBrokenEffectPattern = false;

struct ImageViewInfo {
    VkImage image = nullptr;
    int32_t format = 0;
};

struct ImageInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 0;
    uint32_t mipLevels = 0;
    uint32_t arrayLayers = 0;
    int32_t format = 0;
    VkFlags usage = 0;
};

struct DescriptorImageBinding {
    uint32_t binding = 0;
    uint32_t arrayElement = 0;
    int32_t descriptorType = 0;
    int32_t imageLayout = 0;
    VkSampler sampler = nullptr;
    VkImageView imageView = nullptr;
};

struct DescriptorSetInfo {
    std::array<DescriptorImageBinding, 32> imageBindings{};
    uint32_t imageBindingCount = 0;
};

struct BoundCommandState {
    std::array<VkDescriptorSet, 8> descriptorSets{};
    uint32_t descriptorSetCount = 0;
    VkPipelineLayout pipelineLayout = nullptr;
    VkPipeline pipeline = nullptr;
    int32_t pipelineBindPoint = 0;
};

struct FramebufferInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t layers = 0;
    uint32_t attachmentCount = 0;
    std::array<VkImageView, 8> views{};
    std::array<VkImage, 8> images{};
};

std::unordered_map<VkImageView, ImageViewInfo> g_imageViews;
std::unordered_map<VkImage, ImageInfo> g_images;
std::unordered_map<VkFramebuffer, FramebufferInfo> g_framebuffers;
std::unordered_map<VkDescriptorSet, DescriptorSetInfo> g_descriptorSets;
std::unordered_map<VkCommandBuffer, BoundCommandState> g_boundState;

std::wstring Widen(const char* text) {
    if (!text) {
        return L"<null>";
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (size <= 1) {
        return L"";
    }
    std::wstring out(static_cast<size_t>(size - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), size);
    return out;
}

void LogLimited(std::atomic<uint64_t>& counter, uint64_t limit, std::wstring_view message) {
    const uint64_t value = counter.fetch_add(1);
    if (value < limit || value == 60 || value == 300 || value == 1200) {
        std::lock_guard<std::mutex> guard(g_logMutex);
        Log(message);
    }
}

bool Near(float a, float b, float tolerance) {
    return std::fabs(a - b) <= tolerance;
}

bool HasStereoEyeViewports(const PassCapture& pass) {
    if (pass.viewportCount < 2) {
        return false;
    }

    bool left = false;
    bool right = false;
    const uint32_t count = std::min<uint32_t>(pass.viewportCount, static_cast<uint32_t>(pass.firstViewports.size()));
    for (uint32_t i = 0; i < count; ++i) {
        const ViewportCapture& vp = pass.firstViewports[i];
        left |= Near(vp.x, 25.0f, 8.0f) && Near(vp.width, 911.0f, 16.0f);
        right |= Near(vp.x, 985.0f, 8.0f) && Near(vp.width, 911.0f, 16.0f);
    }
    return left && right;
}

bool IsVrEyeTargetPass(const PassCapture& pass) {
    return pass.width >= 2600 && pass.height >= 2600 && pass.viewportCount <= 1;
}

bool HasBrokenEffectViewportPattern(const PassCapture& pass) {
    if (pass.width != 2560 || pass.height != 2112 || pass.viewportCount < 2) {
        return false;
    }

    bool centeredEffect = false;
    bool repeatedFullEfb = false;
    uint32_t fullEfbCount = 0;
    uint32_t centeredEffectCount = 0;
    const uint32_t count = std::min<uint32_t>(pass.viewportCount, static_cast<uint32_t>(pass.firstViewports.size()));
    for (uint32_t i = 0; i < count; ++i) {
        const ViewportCapture& vp = pass.firstViewports[i];
        const bool fullEfb = Near(vp.x, 0.0f, 2.0f) &&
            Near(vp.y, 0.0f, 2.0f) &&
            Near(vp.width, 2560.0f, 8.0f) &&
            Near(vp.height, 1792.0f, 8.0f);
        const bool centered = Near(vp.x, 128.0f, 4.0f) &&
            Near(vp.y, 0.0f, 2.0f) &&
            Near(vp.width, 2304.0f, 8.0f) &&
            Near(vp.height, 1792.0f, 8.0f);

        fullEfbCount += fullEfb ? 1 : 0;
        centeredEffectCount += centered ? 1 : 0;
    }

    centeredEffect = centeredEffectCount >= 2;
    repeatedFullEfb = fullEfbCount >= 3;
    return centeredEffect || repeatedFullEfb;
}

bool FrameHasBrokenEffectPatternLocked() {
    for (const PassCapture& pass : g_frame.passes) {
        if (HasBrokenEffectViewportPattern(pass)) {
            return true;
        }
    }
    return false;
}

std::wstring ClassifyPass(const PassCapture& pass) {
    if (pass.width == 1920 && pass.height == 1017 && HasStereoEyeViewports(pass)) {
        return L"StereoComposite";
    }
    if (pass.width == 1920 && pass.height == 1017) {
        return L"MirrorOrFinal";
    }
    if (pass.width == 2560 && pass.height == 2112) {
        return L"EFBWithEffectsCandidate";
    }
    if (pass.width == 2560 && pass.height == 1792) {
        return L"EFBColorCandidate";
    }
    if (pass.width == 2804 && pass.height == 2860) {
        return L"VRTargetOrEffect";
    }
    return L"Unknown";
}

std::wstring DescribeViewport(const ViewportCapture& vp) {
    return L"(" + std::to_wstring(static_cast<int>(std::lround(vp.x))) +
        L"," + std::to_wstring(static_cast<int>(std::lround(vp.y))) +
        L" " + std::to_wstring(static_cast<int>(std::lround(vp.width))) +
        L"x" + std::to_wstring(static_cast<int>(std::lround(vp.height))) +
        L")";
}

std::wstring DescribeHandle(const void* handle) {
    wchar_t buffer[32] = {};
    swprintf_s(buffer, L"0x%p", handle);
    return buffer;
}

std::wstring DescribeCopy(const CopyCapture& copy) {
    return L"src=" + DescribeHandle(copy.srcImage) +
        L" dst=" + DescribeHandle(copy.dstImage) +
        L" regions=" + std::to_wstring(copy.regionCount) +
        L" extent=" + std::to_wstring(copy.firstExtent.width) +
        L"x" + std::to_wstring(copy.firstExtent.height) +
        L"x" + std::to_wstring(copy.firstExtent.depth) +
        L" srcOfs=(" + std::to_wstring(copy.firstSrcOffset.x) +
        L"," + std::to_wstring(copy.firstSrcOffset.y) +
        L"," + std::to_wstring(copy.firstSrcOffset.z) +
        L") dstOfs=(" + std::to_wstring(copy.firstDstOffset.x) +
        L"," + std::to_wstring(copy.firstDstOffset.y) +
        L"," + std::to_wstring(copy.firstDstOffset.z) +
        L")";
}

std::wstring DescribeScissor(const ScissorCapture& sc) {
    return L"sc=(" + std::to_wstring(sc.x) +
        L"," + std::to_wstring(sc.y) +
        L" " + std::to_wstring(sc.width) +
        L"x" + std::to_wstring(sc.height) +
        L")";
}

bool OverlayCandidatePass(const PassCapture& pass) {
    return IsVrEyeTargetPass(pass);
}

std::array<uint8_t, 7> GlyphRows(char ch) {
    switch (ch) {
    case 'A': return {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    case 'B': return {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E};
    case 'C': return {0x0F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0F};
    case 'D': return {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E};
    case 'E': return {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
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
    case 'Y': return {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04};
    default: return {0, 0, 0, 0, 0, 0, 0};
    }
}

int TextWidthPixels(const char* text, int scale) {
    const int chars = static_cast<int>(std::strlen(text));
    return chars > 0 ? ((chars * 6) - 1) * scale : 0;
}

void AddClearRect(std::vector<VkClearRect>& rects, int x, int y, int width, int height, uint32_t passWidth, uint32_t passHeight) {
    const int x0 = std::clamp(x, 0, static_cast<int>(passWidth));
    const int y0 = std::clamp(y, 0, static_cast<int>(passHeight));
    const int x1 = std::clamp(x + width, 0, static_cast<int>(passWidth));
    const int y1 = std::clamp(y + height, 0, static_cast<int>(passHeight));
    if (x1 <= x0 || y1 <= y0) {
        return;
    }

    VkClearRect rect{};
    rect.rect.offset = {x0, y0};
    rect.rect.extent = {static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0)};
    rect.baseArrayLayer = 0;
    rect.layerCount = 1;
    rects.push_back(rect);
}

void AddTextRects(std::vector<VkClearRect>& rects, const char* text, int x, int y, int scale, uint32_t passWidth, uint32_t passHeight) {
    int cursor = x;
    for (const char* p = text; *p; ++p) {
        const char ch = *p;
        if (ch != ' ') {
            const std::array<uint8_t, 7> rows = GlyphRows(ch);
            for (int row = 0; row < 7; ++row) {
                uint8_t bits = rows[static_cast<size_t>(row)];
                int col = 0;
                while (col < 5) {
                    while (col < 5 && ((bits & (1u << (4 - col))) == 0)) {
                        ++col;
                    }
                    const int start = col;
                    while (col < 5 && ((bits & (1u << (4 - col))) != 0)) {
                        ++col;
                    }
                    if (col > start) {
                        AddClearRect(rects, cursor + start * scale, y + row * scale,
                                     (col - start) * scale, scale, passWidth, passHeight);
                    }
                }
            }
        }
        cursor += 6 * scale;
    }
}

void ClearRects(VkCommandBuffer commandBuffer, const VkClearColorValue& color, const std::vector<VkClearRect>& rects) {
    if (!g_realCmdClearAttachments || rects.empty()) {
        return;
    }

    VkClearAttachment attachment{};
    attachment.aspectMask = 0x1; // VK_IMAGE_ASPECT_COLOR_BIT
    attachment.colorAttachment = 0;
    attachment.clearValue.color = color;

    constexpr size_t kBatchSize = 128;
    for (size_t i = 0; i < rects.size(); i += kBatchSize) {
        const uint32_t count = static_cast<uint32_t>(std::min(kBatchSize, rects.size() - i));
        g_realCmdClearAttachments(commandBuffer, 1, &attachment, count, rects.data() + i);
    }
}

void DrawAlignmentPromptInViewport(VkCommandBuffer commandBuffer, const PassCapture& pass, float rawX, float rawY, float rawW, float rawH) {
    if (!g_realCmdClearAttachments || rawW < 180.0f || std::fabs(rawH) < 120.0f) {
        return;
    }

    const float vx = rawX;
    const float vy = rawH < 0.0f ? rawY + rawH : rawY;
    const float vw = rawW;
    const float vh = std::fabs(rawH);
    const int scale = std::clamp(static_cast<int>(vw / 170.0f), 3, 6);

    constexpr const char* kLine1 = "ALIGN PLAYER VIEW";
    constexpr const char* kLine2 = "MOVE BODY TO LINE UP GUN";
    constexpr const char* kLine3 = "CLICK RIGHT STICK";
    const int text1 = TextWidthPixels(kLine1, scale);
    const int text2 = TextWidthPixels(kLine2, scale);
    const int text3 = TextWidthPixels(kLine3, scale);
    const int textW = std::max(text1, std::max(text2, text3));
    const int lineStep = 10 * scale;
    const int panelW = textW + 16 * scale;
    const int panelH = (7 * scale) * 3 + (lineStep - 7 * scale) * 2 + 12 * scale;
    const float stereoShift = 0.0f;
    const int panelX = static_cast<int>(std::lround(vx + (vw - panelW) * 0.5f + stereoShift));
    const int panelY = static_cast<int>(std::lround(vy + (vh - panelH) * 0.48f));

    std::vector<VkClearRect> panelRects;
    AddClearRect(panelRects, panelX, panelY, panelW, panelH, pass.width, pass.height);
    VkClearColorValue panelColor{};
    panelColor.float32[0] = 0.01f;
    panelColor.float32[1] = 0.02f;
    panelColor.float32[2] = 0.025f;
    panelColor.float32[3] = 0.88f;
    ClearRects(commandBuffer, panelColor, panelRects);

    std::vector<VkClearRect> textRects;
    const int textX = panelX + (panelW - textW) / 2;
    const int textY = panelY + 6 * scale;
    AddTextRects(textRects, kLine1, textX + (textW - text1) / 2, textY, scale, pass.width, pass.height);
    AddTextRects(textRects, kLine2, textX + (textW - text2) / 2, textY + lineStep, scale, pass.width, pass.height);
    AddTextRects(textRects, kLine3, textX + (textW - text3) / 2, textY + lineStep * 2, scale, pass.width, pass.height);

    VkClearColorValue textColor{};
    textColor.float32[0] = 0.18f;
    textColor.float32[1] = 0.82f;
    textColor.float32[2] = 1.0f;
    textColor.float32[3] = 1.0f;
    ClearRects(commandBuffer, textColor, textRects);
}

void DrawAlignmentPrompt(VkCommandBuffer commandBuffer, const PassCapture& pass) {
    if (!g_showAlignmentPrompt.load(std::memory_order_relaxed) || !OverlayCandidatePass(pass)) {
        return;
    }

    const uint32_t viewportCount = std::min<uint32_t>(pass.viewportCount, static_cast<uint32_t>(pass.firstViewports.size()));
    if (viewportCount == 0) {
        DrawAlignmentPromptInViewport(commandBuffer, pass, 0.0f, 0.0f,
                                      static_cast<float>(pass.width), static_cast<float>(pass.height));
    } else {
        for (uint32_t i = 0; i < viewportCount; ++i) {
            const ViewportCapture& vp = pass.firstViewports[i];
            DrawAlignmentPromptInViewport(commandBuffer, pass, vp.x, vp.y, vp.width, vp.height);
        }
    }

    const uint64_t draws = g_overlayDraws.fetch_add(1, std::memory_order_relaxed);
    if (draws < 4 || draws == 60 || draws == 300) {
        Log(L"Vulkan alignment prompt drawn on " + ClassifyPass(pass) + L" " +
            std::to_wstring(pass.width) + L"x" + std::to_wstring(pass.height) +
            L" eye=" + std::to_wstring(pass.vrEyeIndex));
    }
}

std::wstring DescribeFramebufferAttachments(VkFramebuffer framebuffer) {
    std::lock_guard<std::mutex> guard(g_resourceMutex);
    const auto it = g_framebuffers.find(framebuffer);
    if (it == g_framebuffers.end()) {
        return L" attachments=<unknown>";
    }

    const FramebufferInfo& info = it->second;
    std::wstring out = L" fbSize=" + std::to_wstring(info.width) +
        L"x" + std::to_wstring(info.height) +
        L" layers=" + std::to_wstring(info.layers) +
        L" attachments=" + std::to_wstring(info.attachmentCount);

    const uint32_t count = std::min<uint32_t>(info.attachmentCount, static_cast<uint32_t>(info.images.size()));
    for (uint32_t i = 0; i < count; ++i) {
        out += L" a" + std::to_wstring(i) + L"=" + DescribeHandle(info.images[i]);
    }
    return out;
}

std::wstring DescribeImageView(VkImageView view) {
    std::lock_guard<std::mutex> guard(g_resourceMutex);
    const auto viewIt = g_imageViews.find(view);
    if (viewIt == g_imageViews.end()) {
        return DescribeHandle(view) + L":?";
    }

    std::wstring out = DescribeHandle(view) + L":fmt" + std::to_wstring(viewIt->second.format);
    const auto imageIt = g_images.find(viewIt->second.image);
    if (imageIt != g_images.end()) {
        const ImageInfo& image = imageIt->second;
        out += L":" + std::to_wstring(image.width) +
            L"x" + std::to_wstring(image.height) +
            L" mips=" + std::to_wstring(image.mipLevels) +
            L" usage=0x";
        wchar_t usage[16] = {};
        swprintf_s(usage, L"%x", image.usage);
        out += usage;
    }
    return out;
}

std::wstring DescribeBoundDescriptors(VkCommandBuffer commandBuffer) {
    BoundCommandState bound{};
    std::array<DescriptorSetInfo, 8> sets{};
    {
        std::lock_guard<std::mutex> guard(g_resourceMutex);
        const auto boundIt = g_boundState.find(commandBuffer);
        if (boundIt == g_boundState.end()) {
            return L" bound=<none>";
        }

        bound = boundIt->second;
        const uint32_t setCount = std::min<uint32_t>(bound.descriptorSetCount, static_cast<uint32_t>(bound.descriptorSets.size()));
        for (uint32_t setIndex = 0; setIndex < setCount; ++setIndex) {
            const auto setIt = g_descriptorSets.find(bound.descriptorSets[setIndex]);
            if (setIt == g_descriptorSets.end()) {
                continue;
            }
            sets[setIndex] = setIt->second;
        }
    }

    std::wstring out = L" pipe=" + DescribeHandle(bound.pipeline) +
        L" layout=" + DescribeHandle(bound.pipelineLayout) +
        L" bindPoint=" + std::to_wstring(bound.pipelineBindPoint) +
        L" sets=" + std::to_wstring(bound.descriptorSetCount);

    uint32_t loggedImages = 0;
    const uint32_t setCount = std::min<uint32_t>(bound.descriptorSetCount, static_cast<uint32_t>(bound.descriptorSets.size()));
    for (uint32_t setIndex = 0; setIndex < setCount && loggedImages < 14; ++setIndex) {
        const DescriptorSetInfo& setInfo = sets[setIndex];
        if (setInfo.imageBindingCount == 0) {
            continue;
        }
        out += L" set" + std::to_wstring(setIndex) + L"=" + DescribeHandle(bound.descriptorSets[setIndex]);
        const uint32_t imageCount = std::min<uint32_t>(setInfo.imageBindingCount, static_cast<uint32_t>(setInfo.imageBindings.size()));
        for (uint32_t imageIndex = 0; imageIndex < imageCount && loggedImages < 14; ++imageIndex) {
            const DescriptorImageBinding& binding = setInfo.imageBindings[imageIndex];
            out += L" b" + std::to_wstring(binding.binding) +
                L"[" + std::to_wstring(binding.arrayElement) + L"]" +
                L" type=" + std::to_wstring(binding.descriptorType) +
                L" layout=" + std::to_wstring(binding.imageLayout) +
                L" sampler=" + DescribeHandle(binding.sampler) +
                L" view=" + DescribeImageView(binding.imageView);
            ++loggedImages;
        }
    }
    return out;
}

void LogFrameSummaryLocked(std::wstring_view reason) {
    const bool forced = g_forceNextFrameSummary.exchange(false);
    if (!forced && g_frame.frameIndex > 180 && (g_frame.frameIndex % 300) != 0 && reason == L"present") {
        return;
    }

    std::wstring summary = L"Frame " + std::to_wstring(g_frame.frameIndex) +
        L" [" + std::wstring(reason) + (forced ? L", forced" : L"") + L"]" +
        L": passes=" + std::to_wstring(g_frame.passes.size()) +
        L" copies=" + std::to_wstring(g_frame.copyImageCount) +
        L" blits=" + std::to_wstring(g_frame.blitImageCount);

    const size_t count = std::min<size_t>(g_frame.passes.size(), 12);
    for (size_t i = 0; i < count; ++i) {
        const PassCapture& pass = g_frame.passes[i];
        summary += L"\n  [" + std::to_wstring(i) + L"] " + ClassifyPass(pass) +
            L" " + std::to_wstring(pass.width) + L"x" + std::to_wstring(pass.height) +
            L" clears=" + std::to_wstring(pass.clearCount) +
            L" viewports=" + std::to_wstring(pass.viewportCount) +
            L" rp=" + DescribeHandle(pass.renderPass) +
            L" fb=" + DescribeHandle(pass.framebuffer) +
            DescribeFramebufferAttachments(pass.framebuffer);

        const uint32_t viewportCount = std::min<uint32_t>(pass.viewportCount, static_cast<uint32_t>(pass.firstViewports.size()));
        for (uint32_t vp = 0; vp < viewportCount; ++vp) {
            summary += L" " + DescribeViewport(pass.firstViewports[vp]);
        }
        const uint32_t scissorCount = std::min<uint32_t>(pass.scissorCount, static_cast<uint32_t>(pass.firstScissors.size()));
        for (uint32_t sc = 0; sc < scissorCount; ++sc) {
            summary += L" " + DescribeScissor(pass.firstScissors[sc]);
        }
    }

    const uint32_t copyCount = std::min<uint32_t>(g_frame.copyImageCount, static_cast<uint32_t>(g_frame.firstCopies.size()));
    for (uint32_t i = 0; i < copyCount; ++i) {
        summary += L"\n  copy[" + std::to_wstring(i) + L"] " + DescribeCopy(g_frame.firstCopies[i]);
    }

    std::lock_guard<std::mutex> logGuard(g_logMutex);
    Log(summary);
}

void BeginTrackedPass(const VkRenderPassBeginInfo* beginInfo) {
    std::lock_guard<std::mutex> guard(g_frameMutex);
    PassCapture pass{};
    pass.renderPass = beginInfo->renderPass;
    pass.framebuffer = beginInfo->framebuffer;
    pass.width = beginInfo->renderArea.extent.width;
    pass.height = beginInfo->renderArea.extent.height;
    pass.clearCount = beginInfo->clearValueCount;
    if (pass.width >= 2600 && pass.height >= 2600) {
        pass.vrEyeIndex = static_cast<int>(g_frame.vrTargetPassCount++ % 2);
    }
    g_frame.passes.push_back(pass);
    g_frame.activePass = static_cast<int>(g_frame.passes.size() - 1);
}

void BeginTrackedDynamicRendering(const VkRenderingInfo* renderingInfo) {
    std::lock_guard<std::mutex> guard(g_frameMutex);
    PassCapture pass{};
    pass.width = renderingInfo->renderArea.extent.width;
    pass.height = renderingInfo->renderArea.extent.height;
    pass.clearCount = renderingInfo->colorAttachmentCount;
    if (pass.width >= 2600 && pass.height >= 2600) {
        pass.vrEyeIndex = static_cast<int>(g_frame.vrTargetPassCount++ % 2);
    }
    g_frame.passes.push_back(pass);
    g_frame.activePass = static_cast<int>(g_frame.passes.size() - 1);
}

void TrackViewport(const VkViewport& viewport) {
    std::lock_guard<std::mutex> guard(g_frameMutex);
    if (g_frame.activePass < 0 || static_cast<size_t>(g_frame.activePass) >= g_frame.passes.size()) {
        return;
    }

    PassCapture& pass = g_frame.passes[static_cast<size_t>(g_frame.activePass)];
    if (pass.viewportCount < pass.firstViewports.size()) {
        pass.firstViewports[pass.viewportCount] = ViewportCapture{ viewport.x, viewport.y, viewport.width, viewport.height };
    }
    pass.viewportCount++;
}

void TrackScissor(const VkRect2D& scissor) {
    std::lock_guard<std::mutex> guard(g_frameMutex);
    if (g_frame.activePass < 0 || static_cast<size_t>(g_frame.activePass) >= g_frame.passes.size()) {
        return;
    }

    PassCapture& pass = g_frame.passes[static_cast<size_t>(g_frame.activePass)];
    if (pass.scissorCount < pass.firstScissors.size()) {
        pass.firstScissors[pass.scissorCount] = ScissorCapture{
            scissor.offset.x,
            scissor.offset.y,
            scissor.extent.width,
            scissor.extent.height,
        };
    }
    pass.scissorCount++;
}

void EndTrackedPass() {
    std::lock_guard<std::mutex> guard(g_frameMutex);
    g_frame.activePass = -1;
}

bool SnapshotActivePass(PassCapture& out) {
    std::lock_guard<std::mutex> guard(g_frameMutex);
    if (g_frame.activePass < 0 || static_cast<size_t>(g_frame.activePass) >= g_frame.passes.size()) {
        return false;
    }
    out = g_frame.passes[static_cast<size_t>(g_frame.activePass)];
    return true;
}

void TrackCopyImage(VkImage srcImage, VkImage dstImage, uint32_t regionCount, const void* regions) {
    std::lock_guard<std::mutex> guard(g_frameMutex);
    if (g_frame.copyImageCount < g_frame.firstCopies.size()) {
        CopyCapture& copy = g_frame.firstCopies[g_frame.copyImageCount];
        copy.srcImage = srcImage;
        copy.dstImage = dstImage;
        copy.regionCount = regionCount;
        if (regions && regionCount > 0) {
            const auto* imageCopies = static_cast<const VkImageCopy*>(regions);
            copy.firstExtent = imageCopies[0].extent;
            copy.firstSrcOffset = imageCopies[0].srcOffset;
            copy.firstDstOffset = imageCopies[0].dstOffset;
        }
    }
    g_frame.copyImageCount++;

    const uint64_t snapshot = g_activitySnapshots.fetch_add(1);
    if ((snapshot < 12 || snapshot == 60 || snapshot == 300) && (g_frame.copyImageCount % 2) == 0) {
        LogFrameSummaryLocked(L"copy-snapshot");
    }
}

void TrackBlitImage() {
    std::lock_guard<std::mutex> guard(g_frameMutex);
    g_frame.blitImageCount++;
}

void TrackPresent() {
    std::lock_guard<std::mutex> guard(g_frameMutex);
    const bool hadBrokenEffectPattern = FrameHasBrokenEffectPatternLocked();
    g_frame.frameIndex++;
    LogFrameSummaryLocked(L"present");
    g_previousFrameHadBrokenEffectPattern = hadBrokenEffectPattern;
    const bool finishedDrawCapture = g_captureDrawCalls.exchange(false, std::memory_order_relaxed);
    const uint64_t nextFrame = g_frame.frameIndex;
    g_frame = FrameCapture{};
    g_frame.frameIndex = nextFrame;
    if (finishedDrawCapture) {
        g_drawTraceLines.store(0, std::memory_order_relaxed);
        Log(L"Vulkan draw-call capture ended at present.");
    }
    if (g_armCaptureNextFrame.exchange(false, std::memory_order_relaxed)) {
        g_drawTraceLines.store(0, std::memory_order_relaxed);
        g_forceNextFrameSummary.store(true, std::memory_order_relaxed);
        g_captureDrawCalls.store(true, std::memory_order_relaxed);
        Log(L"Vulkan draw-call capture started on next full frame.");
    }
}

template <typename T>
void StoreReal(T& slot, PFN_vkVoidFunction fn) {
    if (!slot && fn) {
        slot = reinterpret_cast<T>(fn);
    }
}

void VKAPI_PTR Hook_vkCmdSetViewport(VkCommandBuffer commandBuffer, uint32_t firstViewport, uint32_t viewportCount, const VkViewport* viewports) {
    if (viewports && viewportCount > 0) {
        const VkViewport& vp = viewports[0];
        TrackViewport(vp);
        LogLimited(g_viewportCalls, 8,
            L"vkCmdSetViewport first=" + std::to_wstring(firstViewport) +
            L" count=" + std::to_wstring(viewportCount) +
            L" x=" + std::to_wstring(vp.x) +
            L" y=" + std::to_wstring(vp.y) +
            L" w=" + std::to_wstring(vp.width) +
            L" h=" + std::to_wstring(vp.height));

    }
    g_realCmdSetViewport(commandBuffer, firstViewport, viewportCount, viewports);
}

void VKAPI_PTR Hook_vkCmdSetScissor(VkCommandBuffer commandBuffer, uint32_t firstScissor, uint32_t scissorCount, const VkRect2D* scissors) {
    if (scissors && scissorCount > 0) {
        const VkRect2D& sc = scissors[0];
        TrackScissor(sc);
        LogLimited(g_scissorCalls, 4,
            L"vkCmdSetScissor first=" + std::to_wstring(firstScissor) +
            L" count=" + std::to_wstring(scissorCount) +
            L" x=" + std::to_wstring(sc.offset.x) +
            L" y=" + std::to_wstring(sc.offset.y) +
            L" w=" + std::to_wstring(sc.extent.width) +
            L" h=" + std::to_wstring(sc.extent.height));

    }
    g_realCmdSetScissor(commandBuffer, firstScissor, scissorCount, scissors);
}

void VKAPI_PTR Hook_vkCmdBeginRenderPass(VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo* beginInfo, VkSubpassContents contents) {
    if (beginInfo) {
        BeginTrackedPass(beginInfo);
        LogLimited(g_renderPassCalls, 8,
            L"vkCmdBeginRenderPass area=" + std::to_wstring(beginInfo->renderArea.offset.x) +
            L"," + std::to_wstring(beginInfo->renderArea.offset.y) +
            L" " + std::to_wstring(beginInfo->renderArea.extent.width) +
            L"x" + std::to_wstring(beginInfo->renderArea.extent.height) +
            L" clears=" + std::to_wstring(beginInfo->clearValueCount));
    }
    g_realCmdBeginRenderPass(commandBuffer, beginInfo, contents);
}

void VKAPI_PTR Hook_vkCmdEndRenderPass(VkCommandBuffer commandBuffer) {
    PassCapture pass{};
    if (SnapshotActivePass(pass)) {
        DrawAlignmentPrompt(commandBuffer, pass);
    }
    EndTrackedPass();
    g_realCmdEndRenderPass(commandBuffer);
}

void VKAPI_PTR Hook_vkCmdBeginRendering(VkCommandBuffer commandBuffer, const VkRenderingInfo* renderingInfo) {
    if (renderingInfo) {
        BeginTrackedDynamicRendering(renderingInfo);
        LogLimited(g_renderPassCalls, 8,
            L"vkCmdBeginRendering area=" + std::to_wstring(renderingInfo->renderArea.offset.x) +
            L"," + std::to_wstring(renderingInfo->renderArea.offset.y) +
            L" " + std::to_wstring(renderingInfo->renderArea.extent.width) +
            L"x" + std::to_wstring(renderingInfo->renderArea.extent.height) +
            L" colors=" + std::to_wstring(renderingInfo->colorAttachmentCount));
    }
    g_realCmdBeginRendering(commandBuffer, renderingInfo);
}

void VKAPI_PTR Hook_vkCmdEndRendering(VkCommandBuffer commandBuffer) {
    PassCapture pass{};
    if (SnapshotActivePass(pass)) {
        DrawAlignmentPrompt(commandBuffer, pass);
    }
    EndTrackedPass();
    g_realCmdEndRendering(commandBuffer);
}

void VKAPI_PTR Hook_vkCmdClearColorImage(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout layout, const VkClearColorValue* color, uint32_t rangeCount, const VkImageSubresourceRange* ranges) {
    if (color) {
        LogLimited(g_clearCalls, 80,
            L"vkCmdClearColorImage rgba=" + std::to_wstring(color->float32[0]) +
            L"," + std::to_wstring(color->float32[1]) +
            L"," + std::to_wstring(color->float32[2]) +
            L"," + std::to_wstring(color->float32[3]) +
            L" ranges=" + std::to_wstring(rangeCount));
    }
    g_realCmdClearColorImage(commandBuffer, image, layout, color, rangeCount, ranges);
}

void VKAPI_PTR Hook_vkCmdCopyImage(VkCommandBuffer commandBuffer, VkImage srcImage, VkImageLayout srcLayout, VkImage dstImage, VkImageLayout dstLayout, uint32_t regionCount, const void* regions) {
    TrackCopyImage(srcImage, dstImage, regionCount, regions);
    LogLimited(g_copyCalls, 4, L"vkCmdCopyImage regions=" + std::to_wstring(regionCount));
    g_realCmdCopyImage(commandBuffer, srcImage, srcLayout, dstImage, dstLayout, regionCount, regions);
}

void VKAPI_PTR Hook_vkCmdBlitImage(VkCommandBuffer commandBuffer, VkImage srcImage, VkImageLayout srcLayout, VkImage dstImage, VkImageLayout dstLayout, uint32_t regionCount, const void* regions, VkFilter filter) {
    TrackBlitImage();
    LogLimited(g_blitCalls, 4, L"vkCmdBlitImage regions=" + std::to_wstring(regionCount) + L" filter=" + std::to_wstring(filter));
    g_realCmdBlitImage(commandBuffer, srcImage, srcLayout, dstImage, dstLayout, regionCount, regions, filter);
}

VkResult VKAPI_PTR Hook_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* presentInfo) {
    TrackPresent();
    LogLimited(g_presentCalls, 8, L"vkQueuePresentKHR swapchains=" + std::to_wstring(presentInfo ? presentInfo->swapchainCount : 0));
    return g_realQueuePresentKHR(queue, presentInfo);
}

VkResult VKAPI_PTR Hook_vkCreateImage(VkDevice device, const VkImageCreateInfo* createInfo, const void* allocator, VkImage* image) {
    VkResult result = g_realCreateImage(device, createInfo, allocator, image);
    if (result == 0 && createInfo && image && *image) {
        std::lock_guard<std::mutex> guard(g_resourceMutex);
        g_images[*image] = ImageInfo{
            createInfo->extent.width,
            createInfo->extent.height,
            createInfo->extent.depth,
            createInfo->mipLevels,
            createInfo->arrayLayers,
            createInfo->format,
            createInfo->usage,
        };
    }
    return result;
}

void VKAPI_PTR Hook_vkDestroyImage(VkDevice device, VkImage image, const void* allocator) {
    {
        std::lock_guard<std::mutex> guard(g_resourceMutex);
        g_images.erase(image);
    }
    g_realDestroyImage(device, image, allocator);
}

VkResult VKAPI_PTR Hook_vkCreateImageView(VkDevice device, const VkImageViewCreateInfo* createInfo, const void* allocator, VkImageView* imageView) {
    VkResult result = g_realCreateImageView(device, createInfo, allocator, imageView);
    if (result == 0 && createInfo && imageView && *imageView) {
        std::lock_guard<std::mutex> guard(g_resourceMutex);
        g_imageViews[*imageView] = ImageViewInfo{ createInfo->image, createInfo->format };
    }
    return result;
}

void VKAPI_PTR Hook_vkUpdateDescriptorSets(VkDevice device, uint32_t writeCount, const VkWriteDescriptorSet* writes, uint32_t copyCount, const void* copies) {
    if (writes && writeCount > 0) {
        std::lock_guard<std::mutex> guard(g_resourceMutex);
        for (uint32_t writeIndex = 0; writeIndex < writeCount; ++writeIndex) {
            const VkWriteDescriptorSet& write = writes[writeIndex];
            if (!write.dstSet || !write.pImageInfo || write.descriptorCount == 0) {
                continue;
            }
            if (write.descriptorType != 1 && write.descriptorType != 2 && write.descriptorType != 3 && write.descriptorType != 10) {
                continue;
            }

            DescriptorSetInfo& info = g_descriptorSets[write.dstSet];
            for (uint32_t imageIndex = 0; imageIndex < write.descriptorCount; ++imageIndex) {
                const VkDescriptorImageInfo& imageInfo = write.pImageInfo[imageIndex];
                if (!imageInfo.imageView) {
                    continue;
                }

                const uint32_t arrayElement = write.dstArrayElement + imageIndex;
                DescriptorImageBinding* target = nullptr;
                for (uint32_t existing = 0; existing < info.imageBindingCount; ++existing) {
                    DescriptorImageBinding& binding = info.imageBindings[existing];
                    if (binding.binding == write.dstBinding && binding.arrayElement == arrayElement) {
                        target = &binding;
                        break;
                    }
                }
                if (!target && info.imageBindingCount < info.imageBindings.size()) {
                    target = &info.imageBindings[info.imageBindingCount++];
                }
                if (target) {
                    target->binding = write.dstBinding;
                    target->arrayElement = arrayElement;
                    target->descriptorType = write.descriptorType;
                    target->imageLayout = imageInfo.imageLayout;
                    target->sampler = imageInfo.sampler;
                    target->imageView = imageInfo.imageView;
                }
            }
        }
    }
    g_realUpdateDescriptorSets(device, writeCount, writes, copyCount, copies);
}

void VKAPI_PTR Hook_vkCmdBindDescriptorSets(
    VkCommandBuffer commandBuffer,
    int32_t pipelineBindPoint,
    VkPipelineLayout layout,
    uint32_t firstSet,
    uint32_t descriptorSetCount,
    const VkDescriptorSet* descriptorSets,
    uint32_t dynamicOffsetCount,
    const uint32_t* dynamicOffsets) {
    if (descriptorSets && descriptorSetCount > 0) {
        std::lock_guard<std::mutex> guard(g_resourceMutex);
        BoundCommandState& state = g_boundState[commandBuffer];
        state.pipelineBindPoint = pipelineBindPoint;
        state.pipelineLayout = layout;
        const uint32_t count = std::min<uint32_t>(descriptorSetCount, static_cast<uint32_t>(state.descriptorSets.size()));
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t target = firstSet + i;
            if (target < state.descriptorSets.size()) {
                state.descriptorSets[target] = descriptorSets[i];
            }
        }
        state.descriptorSetCount = std::max<uint32_t>(state.descriptorSetCount, std::min<uint32_t>(firstSet + count, static_cast<uint32_t>(state.descriptorSets.size())));
    }
    g_realCmdBindDescriptorSets(commandBuffer, pipelineBindPoint, layout, firstSet, descriptorSetCount, descriptorSets, dynamicOffsetCount, dynamicOffsets);
}

void VKAPI_PTR Hook_vkCmdBindPipeline(VkCommandBuffer commandBuffer, int32_t pipelineBindPoint, VkPipeline pipeline) {
    {
        std::lock_guard<std::mutex> guard(g_resourceMutex);
        BoundCommandState& state = g_boundState[commandBuffer];
        state.pipeline = pipeline;
        state.pipelineBindPoint = pipelineBindPoint;
    }
    g_realCmdBindPipeline(commandBuffer, pipelineBindPoint, pipeline);
}

void LogDrawTrace(VkCommandBuffer commandBuffer, const wchar_t* kind, uint32_t indexOrVertexCount, uint32_t instanceCount) {
    if (!g_captureDrawCalls.load(std::memory_order_relaxed) &&
        g_armCaptureNextFrame.exchange(false, std::memory_order_relaxed)) {
        g_drawTraceLines.store(0, std::memory_order_relaxed);
        g_forceNextFrameSummary.store(true, std::memory_order_relaxed);
        g_captureDrawCalls.store(true, std::memory_order_relaxed);
        std::lock_guard<std::mutex> guard(g_logMutex);
        Log(L"Vulkan draw-call capture started on next draw fallback.");
    }
    if (!g_captureDrawCalls.load(std::memory_order_relaxed)) {
        return;
    }
    const uint64_t line = g_drawTraceLines.fetch_add(1, std::memory_order_relaxed);
    if (line >= 900) {
        return;
    }

    PassCapture pass{};
    if (!SnapshotActivePass(pass)) {
        return;
    }

    std::wstring message = L"HUD draw trace " + std::to_wstring(line) +
        L" " + kind +
        L" count=" + std::to_wstring(indexOrVertexCount) +
        L" inst=" + std::to_wstring(instanceCount) +
        L" pass=" + ClassifyPass(pass) +
        L" " + std::to_wstring(pass.width) + L"x" + std::to_wstring(pass.height) +
        L" vpCount=" + std::to_wstring(pass.viewportCount) +
        L" scCount=" + std::to_wstring(pass.scissorCount);

    const uint32_t viewportCount = std::min<uint32_t>(pass.viewportCount, static_cast<uint32_t>(pass.firstViewports.size()));
    for (uint32_t i = 0; i < viewportCount; ++i) {
        message += L" " + DescribeViewport(pass.firstViewports[i]);
    }
    const uint32_t scissorCount = std::min<uint32_t>(pass.scissorCount, static_cast<uint32_t>(pass.firstScissors.size()));
    for (uint32_t i = 0; i < scissorCount; ++i) {
        message += L" " + DescribeScissor(pass.firstScissors[i]);
    }
    message += DescribeBoundDescriptors(commandBuffer);

    std::lock_guard<std::mutex> guard(g_logMutex);
    Log(message);
}

void VKAPI_PTR Hook_vkCmdDraw(VkCommandBuffer commandBuffer, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) {
    LogDrawTrace(commandBuffer, L"draw", vertexCount, instanceCount);
    g_realCmdDraw(commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
}

void VKAPI_PTR Hook_vkCmdDrawIndexed(VkCommandBuffer commandBuffer, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) {
    LogDrawTrace(commandBuffer, L"drawIndexed", indexCount, instanceCount);
    g_realCmdDrawIndexed(commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void VKAPI_PTR Hook_vkDestroyImageView(VkDevice device, VkImageView imageView, const void* allocator) {
    {
        std::lock_guard<std::mutex> guard(g_resourceMutex);
        g_imageViews.erase(imageView);
    }
    g_realDestroyImageView(device, imageView, allocator);
}

VkResult VKAPI_PTR Hook_vkCreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo* createInfo, const void* allocator, VkFramebuffer* framebuffer) {
    VkResult result = g_realCreateFramebuffer(device, createInfo, allocator, framebuffer);
    if (result == 0 && createInfo && framebuffer && *framebuffer) {
        FramebufferInfo info{};
        info.width = createInfo->width;
        info.height = createInfo->height;
        info.layers = createInfo->layers;
        info.attachmentCount = createInfo->attachmentCount;

        std::lock_guard<std::mutex> guard(g_resourceMutex);
        const uint32_t count = std::min<uint32_t>(createInfo->attachmentCount, static_cast<uint32_t>(info.views.size()));
        for (uint32_t i = 0; i < count; ++i) {
            info.views[i] = createInfo->pAttachments ? createInfo->pAttachments[i] : nullptr;
            const auto viewIt = g_imageViews.find(info.views[i]);
            if (viewIt != g_imageViews.end()) {
                info.images[i] = viewIt->second.image;
            }
        }
        g_framebuffers[*framebuffer] = info;
    }
    return result;
}

void VKAPI_PTR Hook_vkDestroyFramebuffer(VkDevice device, VkFramebuffer framebuffer, const void* allocator) {
    {
        std::lock_guard<std::mutex> guard(g_resourceMutex);
        g_framebuffers.erase(framebuffer);
    }
    g_realDestroyFramebuffer(device, framebuffer, allocator);
}

PFN_vkVoidFunction WrapProc(const char* name, PFN_vkVoidFunction real) {
    if (!name || !real) {
        return real;
    }

    if (std::strcmp(name, "vkCmdSetViewport") == 0) {
        StoreReal(g_realCmdSetViewport, real);
        return reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkCmdSetViewport);
    }
    if (std::strcmp(name, "vkCmdSetScissor") == 0) {
        StoreReal(g_realCmdSetScissor, real);
        return reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkCmdSetScissor);
    }
    if (std::strcmp(name, "vkCmdBeginRenderPass") == 0) {
        StoreReal(g_realCmdBeginRenderPass, real);
        return reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkCmdBeginRenderPass);
    }
    if (std::strcmp(name, "vkCmdEndRenderPass") == 0) {
        StoreReal(g_realCmdEndRenderPass, real);
        return reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkCmdEndRenderPass);
    }
    if (std::strcmp(name, "vkCmdBeginRendering") == 0 || std::strcmp(name, "vkCmdBeginRenderingKHR") == 0) {
        StoreReal(g_realCmdBeginRendering, real);
        return reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkCmdBeginRendering);
    }
    if (std::strcmp(name, "vkCmdEndRendering") == 0 || std::strcmp(name, "vkCmdEndRenderingKHR") == 0) {
        StoreReal(g_realCmdEndRendering, real);
        return reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkCmdEndRendering);
    }
    if (std::strcmp(name, "vkCmdClearAttachments") == 0) {
        StoreReal(g_realCmdClearAttachments, real);
        return real;
    }
    if (std::strcmp(name, "vkCmdClearColorImage") == 0) {
        StoreReal(g_realCmdClearColorImage, real);
        return reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkCmdClearColorImage);
    }
    if (std::strcmp(name, "vkCmdCopyImage") == 0) {
        StoreReal(g_realCmdCopyImage, real);
        return reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkCmdCopyImage);
    }
    if (std::strcmp(name, "vkCmdBlitImage") == 0) {
        StoreReal(g_realCmdBlitImage, real);
        return reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkCmdBlitImage);
    }
    if (std::strcmp(name, "vkQueuePresentKHR") == 0) {
        StoreReal(g_realQueuePresentKHR, real);
        return reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkQueuePresentKHR);
    }
    if (std::strcmp(name, "vkCreateImage") == 0) {
        StoreReal(g_realCreateImage, real);
        return reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkCreateImage);
    }
    if (std::strcmp(name, "vkDestroyImage") == 0) {
        StoreReal(g_realDestroyImage, real);
        return reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkDestroyImage);
    }
    if (std::strcmp(name, "vkCreateImageView") == 0) {
        StoreReal(g_realCreateImageView, real);
        return reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkCreateImageView);
    }
    if (std::strcmp(name, "vkDestroyImageView") == 0) {
        StoreReal(g_realDestroyImageView, real);
        return reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkDestroyImageView);
    }
    if (std::strcmp(name, "vkCreateFramebuffer") == 0) {
        StoreReal(g_realCreateFramebuffer, real);
        return reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkCreateFramebuffer);
    }
    if (std::strcmp(name, "vkDestroyFramebuffer") == 0) {
        StoreReal(g_realDestroyFramebuffer, real);
        return reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkDestroyFramebuffer);
    }
    if (std::strcmp(name, "vkUpdateDescriptorSets") == 0) {
        StoreReal(g_realUpdateDescriptorSets, real);
        return reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkUpdateDescriptorSets);
    }
    if (std::strcmp(name, "vkCmdBindDescriptorSets") == 0) {
        StoreReal(g_realCmdBindDescriptorSets, real);
        return reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkCmdBindDescriptorSets);
    }
    if (std::strcmp(name, "vkCmdBindPipeline") == 0) {
        StoreReal(g_realCmdBindPipeline, real);
        return reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkCmdBindPipeline);
    }
    if (std::strcmp(name, "vkCmdDraw") == 0) {
        StoreReal(g_realCmdDraw, real);
        return reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkCmdDraw);
    }
    if (std::strcmp(name, "vkCmdDrawIndexed") == 0) {
        StoreReal(g_realCmdDrawIndexed, real);
        return reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkCmdDrawIndexed);
    }

    return real;
}

PFN_vkVoidFunction VKAPI_PTR Hook_vkGetInstanceProcAddr(VkInstance instance, const char* name) {
    PFN_vkVoidFunction real = g_realGetInstanceProcAddr(instance, name);
    if (!g_realCmdClearAttachments && instance) {
        StoreReal(g_realCmdClearAttachments, g_realGetInstanceProcAddr(instance, "vkCmdClearAttachments"));
    }
    PFN_vkVoidFunction wrapped = WrapProc(name, real);
    if (wrapped != real) {
        Log(L"Wrapped instance Vulkan proc: " + Widen(name));
    }
    return wrapped;
}

PFN_vkVoidFunction VKAPI_PTR Hook_vkGetDeviceProcAddr(VkDevice device, const char* name) {
    PFN_vkVoidFunction real = g_realGetDeviceProcAddr(device, name);
    if (!g_realCmdClearAttachments && device) {
        StoreReal(g_realCmdClearAttachments, g_realGetDeviceProcAddr(device, "vkCmdClearAttachments"));
    }
    PFN_vkVoidFunction wrapped = WrapProc(name, real);
    if (wrapped != real) {
        Log(L"Wrapped device Vulkan proc: " + Widen(name));
    }
    return wrapped;
}

bool IsReadableMemory(const void* ptr, size_t size) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(ptr, &mbi, sizeof(mbi))) {
        return false;
    }
    const auto address = reinterpret_cast<uintptr_t>(ptr);
    const auto base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    const bool committed = mbi.State == MEM_COMMIT;
    const bool guarded = (mbi.Protect & PAGE_GUARD) != 0;
    const bool noAccess = (mbi.Protect & PAGE_NOACCESS) != 0;
    return committed && !guarded && !noAccess && address + size <= base + mbi.RegionSize;
}

bool PatchImportThunk(void** thunk, void* replacement, void** original) {
    if (!IsReadableMemory(thunk, sizeof(void*))) {
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(thunk, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        return false;
    }

    if (original && !*original) {
        *original = *thunk;
    }
    *thunk = replacement;

    DWORD ignored = 0;
    VirtualProtect(thunk, sizeof(void*), oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), thunk, sizeof(void*));
    return true;
}

bool PatchModuleImports(HMODULE module, const char* importedDllName) {
    auto* base = reinterpret_cast<uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (!IsReadableMemory(dos, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (!IsReadableMemory(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }

    const IMAGE_DATA_DIRECTORY& importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (importDir.VirtualAddress == 0 || importDir.Size == 0) {
        return false;
    }

    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + importDir.VirtualAddress);
    bool patchedAny = false;

    for (; descriptor->Name; ++descriptor) {
        const char* dllName = reinterpret_cast<const char*>(base + descriptor->Name);
        if (_stricmp(dllName, importedDllName) != 0) {
            continue;
        }

        auto* originalThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk);
        auto* firstThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
        if (!descriptor->OriginalFirstThunk) {
            originalThunk = firstThunk;
        }

        for (; originalThunk->u1.AddressOfData; ++originalThunk, ++firstThunk) {
            if (IMAGE_SNAP_BY_ORDINAL(originalThunk->u1.Ordinal)) {
                continue;
            }

            auto* importByName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + originalThunk->u1.AddressOfData);
            const char* functionName = reinterpret_cast<const char*>(importByName->Name);

            if (std::strcmp(functionName, "vkGetInstanceProcAddr") == 0) {
                patchedAny |= PatchImportThunk(reinterpret_cast<void**>(&firstThunk->u1.Function), reinterpret_cast<void*>(&Hook_vkGetInstanceProcAddr), reinterpret_cast<void**>(&g_realGetInstanceProcAddr));
            } else if (std::strcmp(functionName, "vkGetDeviceProcAddr") == 0) {
                patchedAny |= PatchImportThunk(reinterpret_cast<void**>(&firstThunk->u1.Function), reinterpret_cast<void*>(&Hook_vkGetDeviceProcAddr), reinterpret_cast<void**>(&g_realGetDeviceProcAddr));
            }
        }
    }

    return patchedAny;
}

bool PatchLoadedModules() {
    HMODULE modules[1024]{};
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) {
        return false;
    }

    bool patchedAny = false;
    const size_t count = std::min<size_t>(needed / sizeof(HMODULE), std::size(modules));
    for (size_t i = 0; i < count; ++i) {
        patchedAny |= PatchModuleImports(modules[i], "vulkan-1.dll");
    }
    return patchedAny;
}

void* ResolveExportJumpTarget(void* function) {
    auto* bytes = static_cast<uint8_t*>(function);
    if (!IsReadableMemory(bytes, 8)) {
        return function;
    }

    // The Windows Vulkan loader commonly exports a short x64 relative JMP stub:
    // E9 xx xx xx xx CC CC...
    if (bytes[0] == 0xE9) {
        int32_t displacement = 0;
        std::memcpy(&displacement, bytes + 1, sizeof(displacement));
        return bytes + 5 + displacement;
    }

    return function;
}

bool InstallAbsoluteJump(void* target, void* replacement) {
    auto* bytes = static_cast<uint8_t*>(target);
    if (!IsReadableMemory(bytes, 12)) {
        return false;
    }

    // Do not repatch if our absolute jump is already present.
    if (bytes[0] == 0x48 && bytes[1] == 0xB8 && bytes[10] == 0xFF && bytes[11] == 0xE0) {
        return true;
    }

    uint8_t patch[12] = {
        0x48, 0xB8,                         // mov rax, imm64
        0, 0, 0, 0, 0, 0, 0, 0,
        0xFF, 0xE0                          // jmp rax
    };
    std::memcpy(patch + 2, &replacement, sizeof(replacement));

    DWORD oldProtect = 0;
    if (!VirtualProtect(bytes, sizeof(patch), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }

    std::memcpy(bytes, patch, sizeof(patch));

    DWORD ignored = 0;
    VirtualProtect(bytes, sizeof(patch), oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), bytes, sizeof(patch));
    return true;
}

bool InstallInlineDetours(HMODULE vulkan) {
    void* rawGetInstanceProcAddr = reinterpret_cast<void*>(GetProcAddress(vulkan, "vkGetInstanceProcAddr"));
    void* rawGetDeviceProcAddr = reinterpret_cast<void*>(GetProcAddress(vulkan, "vkGetDeviceProcAddr"));
    if (!rawGetInstanceProcAddr || !rawGetDeviceProcAddr) {
        return false;
    }

    if (!g_realGetInstanceProcAddr) {
        g_realGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(ResolveExportJumpTarget(rawGetInstanceProcAddr));
    }
    if (!g_realGetDeviceProcAddr) {
        g_realGetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(ResolveExportJumpTarget(rawGetDeviceProcAddr));
    }

    const bool instanceOk = InstallAbsoluteJump(rawGetInstanceProcAddr, reinterpret_cast<void*>(&Hook_vkGetInstanceProcAddr));
    const bool deviceOk = InstallAbsoluteJump(rawGetDeviceProcAddr, reinterpret_cast<void*>(&Hook_vkGetDeviceProcAddr));

    if (instanceOk && deviceOk && !g_inlineDetoursInstalled.exchange(true)) {
        Log(L"VulkanHooks inline detours installed on vkGetInstanceProcAddr/vkGetDeviceProcAddr.");
    }

    return instanceOk && deviceOk;
}

} // namespace

bool InstallIfAvailable() {
    HMODULE vulkan = GetModuleHandleW(L"vulkan-1.dll");
    if (!vulkan) {
        return false;
    }

    const bool inlineDetours = InstallInlineDetours(vulkan);

    if (!g_realGetInstanceProcAddr || !g_realGetDeviceProcAddr) {
        Log(L"VulkanHooks: failed to locate vkGet*ProcAddr exports.");
        return false;
    }

    const bool patched = PatchLoadedModules();
    if (!g_installed.exchange(true)) {
        Log(std::wstring(L"VulkanHooks installed. inline=") + (inlineDetours ? L"yes" : L"no") + L" IAT patched=" + (patched ? L"yes" : L"no"));
        Log(L"Framebuffer mutation experiments disabled; Vulkan hook is tracing only.");
    }
    return true;
}

void PollRuntimeControls(const SharedState* shared) {
    const bool shouldShow = false;
    const bool wasShowing = g_showAlignmentPrompt.exchange(shouldShow, std::memory_order_relaxed);
    if (wasShowing != shouldShow) {
        Log(std::wstring(L"Vulkan alignment prompt ") + (shouldShow ? L"enabled" : L"disabled") +
            L" appFlag=" + std::to_wstring(shared ? shared->settings.showAlignmentPrompt : 0) +
            L" tracking=" + std::to_wstring(shared ? shared->trackingRuntimeActive : 0));
    }
    g_armCaptureNextFrame.store(false, std::memory_order_relaxed);
    g_captureDrawCalls.store(false, std::memory_order_relaxed);
}

void Shutdown() {
    if (g_installed.exchange(false)) {
        Log(L"VulkanHooks shutdown.");
    }
}

} // namespace PrimedGun::Hook::VulkanHooks
