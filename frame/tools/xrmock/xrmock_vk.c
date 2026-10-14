// Mock OpenXR runtime, Vulkan side (xrmock_gfx.h): XR_KHR_vulkan_enable2 and XR_KHR_vulkan_enable.
// With enable2 the instance and device are made with the app's own vkGetInstanceProcAddr
// (no libvulkan link); with enable (v1) the app makes them itself and hands them over in the
// session's graphics binding (the loader's vkGetInstanceProcAddr is found with dlsym).
// Swapchain images are VkImages on the app's device.
// At each release the mock checks that the GPU had finished the app's work
// (SteamVR on the Frame shows a released image at once, finished or not):
// it waits for the app's queue to go idle and reports a release that had to
// wait (the app released before its GPU work was done).
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "xrmock_gfx.h"

static PFN_vkGetInstanceProcAddr gGipa;
static VkInstance gInstance;
static VkPhysicalDevice gPhys;
static VkDevice gDevice;
static VkQueue gQueue;
static uint32_t gQueueFamily;
static PFN_vkGetDeviceProcAddr gGdpa;

// Memory of every swapchain image, mapped when it is host visible (the exact release check).
#define MAX_IMAGES 256
static struct {
    VkImage image;
    VkDeviceMemory memory;
    unsigned char* map;
    VkDeviceSize size;
} gImages[MAX_IMAGES];
static int gImageCount = 0;

#define DEV(name) ((PFN_##name)gGdpa(gDevice, #name))
#define INST(name) ((PFN_##name)gGipa(gInstance, #name))

// XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR and ..._VULKAN2_KHR are one type value and one layout.
static int ownsBinding(XrStructureType t) { return t == XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR; }

static int gV1 = 0;  // the app made its own instance and device (XR_KHR_vulkan_enable)

// With enable (v1) nothing passed the app's loader entry point to the mock: take it from the
// loader already in the process (the app's own libvulkan).
static void ensureGipa(void) {
    if (gGipa) return;
    void* h = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_NOLOAD);
    if (!h) h = dlopen("libvulkan.so.1", RTLD_NOW);
    if (h) gGipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
}

static int describeBinding(const XrBaseInStructure* n, char* out, size_t cap) {
    const XrGraphicsBindingVulkan2KHR* b = (const XrGraphicsBindingVulkan2KHR*)n;
    if (!gDevice && b->device) {  // enable (v1): the app's own handles
        gV1 = 1;
        ensureGipa();
        gInstance = b->instance;
        gPhys = b->physicalDevice;
        gDevice = b->device;
    }
    int ok = b->instance == gInstance && b->physicalDevice == gPhys && b->device == gDevice;
    if (gDevice && b->device == gDevice) {
        gGdpa = (PFN_vkGetDeviceProcAddr)gGipa(gInstance, "vkGetDeviceProcAddr");
        DEV(vkGetDeviceQueue)(gDevice, b->queueFamilyIndex, b->queueIndex, &gQueue);
        gQueueFamily = b->queueFamilyIndex;
    }
    return snprintf(out, cap, "binding=VULKAN%s handles=%s queueFamily=%u queueIndex=%u", gV1 ? "(v1)" : "2", ok ? (gV1 ? "app-created" : "made-by-runtime") : "FOREIGN",
                    b->queueFamilyIndex, b->queueIndex);
}

static XrResult XRAPI_CALL vkRequirements(XrInstance i, XrSystemId s, XrGraphicsRequirementsVulkanKHR* r) {
    (void)i;
    (void)s;
    // As SteamVR: instances of Vulkan 1.0 to 1.2.
    r->minApiVersionSupported = XR_MAKE_VERSION(1, 0, 0);
    r->maxApiVersionSupported = XR_MAKE_VERSION(1, 2, 0);
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL vkCreateInstanceXr(XrInstance i, const XrVulkanInstanceCreateInfoKHR* ci, VkInstance* out, VkResult* vr) {
    (void)i;
    gGipa = ci->pfnGetInstanceProcAddr;
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)gGipa(VK_NULL_HANDLE, "vkCreateInstance");
    const VkApplicationInfo* app = ci->vulkanCreateInfo->pApplicationInfo;
    if (app && app->apiVersion > VK_API_VERSION_1_2) {
        fprintf(stderr, "xrmock: ERROR instance apiVersion above 1.2\n");
        *vr = VK_ERROR_INCOMPATIBLE_DRIVER;
        return XR_SUCCESS;
    }
    *vr = create(ci->vulkanCreateInfo, ci->vulkanAllocator, out);
    gInstance = *out;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL vkGetDevice(XrInstance i, const XrVulkanGraphicsDeviceGetInfoKHR* gi, VkPhysicalDevice* out) {
    (void)i;
    PFN_vkEnumeratePhysicalDevices en = (PFN_vkEnumeratePhysicalDevices)gGipa(gi->vulkanInstance, "vkEnumeratePhysicalDevices");
    uint32_t n = 1;
    VkPhysicalDevice d = VK_NULL_HANDLE;
    VkResult r = en(gi->vulkanInstance, &n, &d);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !d) return XR_ERROR_RUNTIME_FAILURE;
    gPhys = *out = d;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL vkCreateDeviceXr(XrInstance i, const XrVulkanDeviceCreateInfoKHR* ci, VkDevice* out, VkResult* vr) {
    (void)i;
    PFN_vkCreateDevice create = (PFN_vkCreateDevice)ci->pfnGetInstanceProcAddr(gInstance, "vkCreateDevice");
    *vr = create(ci->vulkanPhysicalDevice, ci->vulkanCreateInfo, ci->vulkanAllocator, out);
    gDevice = *out;
    return XR_SUCCESS;
}

// XR_KHR_vulkan_enable (v1). The app asks before it has a Vulkan instance, so the extension
// lists are fixed strings (MOCK_XR_VK_INSTANCE_EXTS / MOCK_XR_VK_DEVICE_EXTS replace them; the
// defaults are the usual SteamVR set, not read from a Frame).
static XrResult stringOut(const char* s, uint32_t cap, uint32_t* count, char* buf) {
    uint32_t n = (uint32_t)strlen(s) + 1;
    *count = n;
    if (cap == 0) return XR_SUCCESS;
    if (cap < n) return XR_ERROR_SIZE_INSUFFICIENT;
    memcpy(buf, s, n);
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL vk1Requirements(XrInstance i, XrSystemId s, XrGraphicsRequirementsVulkanKHR* r) {
    return vkRequirements(i, s, r);
}

static XrResult XRAPI_CALL vk1InstanceExts(XrInstance i, XrSystemId s, uint32_t cap, uint32_t* count, char* buf) {
    (void)i;
    (void)s;
    const char* e = getenv("MOCK_XR_VK_INSTANCE_EXTS");
    return stringOut(e ? e : "VK_KHR_external_memory_capabilities VK_KHR_get_physical_device_properties2", cap, count, buf);
}

static XrResult XRAPI_CALL vk1DeviceExts(XrInstance i, XrSystemId s, uint32_t cap, uint32_t* count, char* buf) {
    (void)i;
    (void)s;
    const char* e = getenv("MOCK_XR_VK_DEVICE_EXTS");
    return stringOut(e ? e : "VK_KHR_external_memory VK_KHR_external_memory_fd VK_KHR_dedicated_allocation VK_KHR_get_memory_requirements2", cap, count, buf);
}

// The physical device of the app's instance (the first one; the mock's system has one GPU).
static XrResult XRAPI_CALL vk1GraphicsDevice(XrInstance i, XrSystemId s, VkInstance vki, VkPhysicalDevice* out) {
    (void)i;
    (void)s;
    ensureGipa();
    if (!gGipa || !vki) return XR_ERROR_VALIDATION_FAILURE;
    PFN_vkEnumeratePhysicalDevices en = (PFN_vkEnumeratePhysicalDevices)gGipa(vki, "vkEnumeratePhysicalDevices");
    uint32_t n = 1;
    VkPhysicalDevice d = VK_NULL_HANDLE;
    VkResult r = en(vki, &n, &d);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !d) return XR_ERROR_RUNTIME_FAILURE;
    gInstance = vki;
    gPhys = *out = d;
    return XR_SUCCESS;
}

static PFN_xrVoidFunction getProc(const char* name) {
    if (!strcmp(name, "xrGetVulkanGraphicsRequirements2KHR")) return (PFN_xrVoidFunction)vkRequirements;
    if (!strcmp(name, "xrCreateVulkanInstanceKHR")) return (PFN_xrVoidFunction)vkCreateInstanceXr;
    if (!strcmp(name, "xrGetVulkanGraphicsDevice2KHR")) return (PFN_xrVoidFunction)vkGetDevice;
    if (!strcmp(name, "xrCreateVulkanDeviceKHR")) return (PFN_xrVoidFunction)vkCreateDeviceXr;
    if (!strcmp(name, "xrGetVulkanGraphicsRequirementsKHR")) return (PFN_xrVoidFunction)vk1Requirements;
    if (!strcmp(name, "xrGetVulkanInstanceExtensionsKHR")) return (PFN_xrVoidFunction)vk1InstanceExts;
    if (!strcmp(name, "xrGetVulkanDeviceExtensionsKHR")) return (PFN_xrVoidFunction)vk1DeviceExts;
    if (!strcmp(name, "xrGetVulkanGraphicsDeviceKHR")) return (PFN_xrVoidFunction)vk1GraphicsDevice;
    return NULL;
}

// As SteamVR offers them on the Frame.
static const int64_t kFormats[] = {VK_FORMAT_R8G8B8A8_SRGB,   VK_FORMAT_B8G8R8A8_SRGB,    VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                                   VK_FORMAT_D32_SFLOAT,      VK_FORMAT_D16_UNORM,        VK_FORMAT_D24_UNORM_S8_UINT,   VK_FORMAT_D32_SFLOAT_S8_UINT};

static int formats(const int64_t** list) {
    *list = kFormats;
    return (int)(sizeof(kFormats) / sizeof(kFormats[0]));
}

static int64_t formatOf(MockFormatKind kind) {
    switch (kind) {
    case MOCK_FMT_EYE:
    case MOCK_FMT_UI: return VK_FORMAT_R8G8B8A8_SRGB;
    case MOCK_FMT_MOTION: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case MOCK_FMT_DEPTH: return VK_FORMAT_D24_UNORM_S8_UINT;
    }
    return VK_FORMAT_R8G8B8A8_SRGB;
}

static void* createImages(int64_t format, uint64_t usage, int width, int height, int mips, int count) {
    if (!gDevice) return NULL;
    if (!gGdpa) gGdpa = (PFN_vkGetDeviceProcAddr)gGipa(gInstance, "vkGetDeviceProcAddr");
    VkImage* images = (VkImage*)calloc((size_t)count, sizeof(VkImage));
    int depth = format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D16_UNORM || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
    VkImageUsageFlags u = 0;
    if (usage & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) u |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (usage & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) u |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (usage & XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT) u |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    // MOCK_XR_DUMP copies released images out.
    if (!depth && getenv("MOCK_XR_DUMP")) u |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (usage & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) u |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (usage & XR_SWAPCHAIN_USAGE_SAMPLED_BIT) u |= VK_IMAGE_USAGE_SAMPLED_BIT;
    // The compositor samples every image.
    u |= VK_IMAGE_USAGE_SAMPLED_BIT;
    VkPhysicalDeviceMemoryProperties mp;
    INST(vkGetPhysicalDeviceMemoryProperties)(gPhys, &mp);
    for (int i = 0; i < count; i++) {
        VkImageCreateInfo ci;
        memset(&ci, 0, sizeof(ci));
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = (VkFormat)format;
        ci.extent.width = (uint32_t)width;
        ci.extent.height = (uint32_t)height;
        ci.extent.depth = 1;
        ci.mipLevels = (uint32_t)mips;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = u;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        // The app makes views of the linear sibling of an sRGB format.
        if (usage & XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT) ci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        if (DEV(vkCreateImage)(gDevice, &ci, NULL, &images[i]) != VK_SUCCESS) {
            fprintf(stderr, "xrmock: vkCreateImage failed\n");
            continue;
        }
        VkMemoryRequirements req;
        DEV(vkGetImageMemoryRequirements)(gDevice, images[i], &req);
        // Host visible where the device allows it (unified memory on the Frame): the exact
        // release check reads the image's bytes.
        uint32_t type = UINT32_MAX;
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (uint32_t t = 0; t < mp.memoryTypeCount && getenv("MOCK_XR_BUSY_MS"); t++) {
            if ((req.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & want) == want) {
                type = t;
                break;
            }
        }
        for (uint32_t t = 0; t < mp.memoryTypeCount && type == UINT32_MAX; t++) {
            if (req.memoryTypeBits & (1u << t)) type = t;
        }
        VkMemoryAllocateInfo ai;
        memset(&ai, 0, sizeof(ai));
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        VkDeviceMemory mem;
        if (DEV(vkAllocateMemory)(gDevice, &ai, NULL, &mem) == VK_SUCCESS) {
            DEV(vkBindImageMemory)(gDevice, images[i], mem, 0);
            if (gImageCount < MAX_IMAGES) {
                gImages[gImageCount].image = images[i];
                gImages[gImageCount].memory = mem;
                gImages[gImageCount].size = req.size;
                void* map = NULL;
                if ((mp.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
                    DEV(vkMapMemory)(gDevice, mem, 0, VK_WHOLE_SIZE, 0, &map) == VK_SUCCESS)
                    gImages[gImageCount].map = (unsigned char*)map;
                gImageCount++;
            }
        }
    }
    // As a runtime hands them out: color images in COLOR_ATTACHMENT_OPTIMAL,
    // depth in DEPTH_STENCIL_ATTACHMENT_OPTIMAL (XR_KHR_vulkan_enable2).
    if (gQueue) {
        VkCommandPoolCreateInfo pci;
        memset(&pci, 0, sizeof(pci));
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.queueFamilyIndex = gQueueFamily;
        VkCommandPool pool;
        DEV(vkCreateCommandPool)(gDevice, &pci, NULL, &pool);
        VkCommandBufferAllocateInfo ai;
        memset(&ai, 0, sizeof(ai));
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cb;
        DEV(vkAllocateCommandBuffers)(gDevice, &ai, &cb);
        VkCommandBufferBeginInfo bi;
        memset(&bi, 0, sizeof(bi));
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        DEV(vkBeginCommandBuffer)(cb, &bi);
        for (int i = 0; i < count; i++) {
            VkImageMemoryBarrier b;
            memset(&b, 0, sizeof(b));
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            // Images without attachment usage (the sampled quad layers) cannot be put in an attachment layout.
            int attach = depth || (u & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
            b.dstAccessMask = depth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : attach ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : attach ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = images[i];
            b.subresourceRange.aspectMask = depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
            b.subresourceRange.levelCount = (uint32_t)mips;
            b.subresourceRange.layerCount = 1;
            DEV(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        }
        DEV(vkEndCommandBuffer)(cb);
        VkSubmitInfo si;
        memset(&si, 0, sizeof(si));
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        DEV(vkQueueSubmit)(gQueue, 1, &si, VK_NULL_HANDLE);
        DEV(vkQueueWaitIdle)(gQueue);
        DEV(vkDestroyCommandPool)(gDevice, pool, NULL);
    }
    return images;
}

static XrResult fillImages(void* images, int count, XrSwapchainImageBaseHeader* out) {
    const VkImage* im = (const VkImage*)images;
    XrSwapchainImageVulkan2KHR* v = (XrSwapchainImageVulkan2KHR*)out;
    for (int i = 0; i < count; i++) {
        if (v[i].type != XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR) return XR_ERROR_VALIDATION_FAILURE;
        v[i].image = im[i];
    }
    return XR_SUCCESS;
}

// How long the app's queue took to go idle at a release (ms).
static double releaseWaitMs(void) {
    if (!gQueue) return 0.0;
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    DEV(vkQueueWaitIdle)(gQueue);
    clock_gettime(CLOCK_MONOTONIC, &b);
    return (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
}

// The exact check: SAMPLES chunks of the image's memory at its release, compared again once
// an empty submission made at the release (so after all work queued before it) completed.
#define SAMPLES 512
#define CHUNK 64
#define PENDING 32
static struct {
    int image;
    VkFence fence;
    unsigned char bytes[SAMPLES * CHUNK];
} gPending[PENDING];
static int gPendingCount = 0, gChecked = 0;
static VkFence gFreeFences[PENDING];
static int gFreeFenceCount = 0;

// The app may release on one thread (overlays, video thread) while another polls in
// xrWaitFrame (pacing thread): the snapshot list is shared.
static pthread_mutex_t gSnapMutex = PTHREAD_MUTEX_INITIALIZER;

static void sample(int image, unsigned char* out) {
    VkDeviceSize step = gImages[image].size / SAMPLES;
    for (int k = 0; k < SAMPLES; k++) memcpy(out + k * CHUNK, gImages[image].map + (VkDeviceSize)k * step, CHUNK);
}

static int pollSnapshotsLocked(int wait);
static int pollSnapshots(int wait) {
    pthread_mutex_lock(&gSnapMutex);
    int changed = pollSnapshotsLocked(wait);
    pthread_mutex_unlock(&gSnapMutex);
    return changed;
}
static int pollSnapshotsLocked(int wait) {
    int changed = 0;
    unsigned char now[SAMPLES * CHUNK];
    for (int i = 0; i < gPendingCount;) {
        VkResult r = wait ? DEV(vkWaitForFences)(gDevice, 1, &gPending[i].fence, VK_TRUE, 2000000000ull)
                          : DEV(vkGetFenceStatus)(gDevice, gPending[i].fence);
        if (r != VK_SUCCESS) {
            i++;
            continue;
        }
        sample(gPending[i].image, now);
        if (memcmp(now, gPending[i].bytes, sizeof(now)) != 0) changed++;
        gChecked++;
        DEV(vkResetFences)(gDevice, 1, &gPending[i].fence);
        gFreeFences[gFreeFenceCount++] = gPending[i].fence;
        gPending[i] = gPending[--gPendingCount];
    }
    return changed;
}

// At a release, inside xrReleaseSwapchainImage: the app holds its queue lock (SteamVR's client
// submits there too).
static void releaseSnapshotLocked(void* images, int index);
static void releaseSnapshot(void* images, int index) {
    pthread_mutex_lock(&gSnapMutex);
    releaseSnapshotLocked(images, index);
    pthread_mutex_unlock(&gSnapMutex);
}
static void releaseSnapshotLocked(void* images, int index) {
    if (!gQueue) return;
    VkImage image = ((VkImage*)images)[index];
    int found = -1;
    for (int k = 0; k < gImageCount; k++)
        if (gImages[k].image == image && gImages[k].map && gImages[k].size >= SAMPLES * CHUNK) found = k;
    if (found < 0 || gPendingCount == PENDING) return;
    VkFence fence;
    if (gFreeFenceCount) fence = gFreeFences[--gFreeFenceCount];
    else {
        VkFenceCreateInfo fi;
        memset(&fi, 0, sizeof(fi));
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (DEV(vkCreateFence)(gDevice, &fi, NULL, &fence) != VK_SUCCESS) return;
    }
    sample(found, gPending[gPendingCount].bytes);
    VkSubmitInfo si;
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    if (DEV(vkQueueSubmit)(gQueue, 1, &si, fence) != VK_SUCCESS) {
        gFreeFences[gFreeFenceCount++] = fence;
        return;
    }
    gPending[gPendingCount].image = found;
    gPending[gPendingCount].fence = fence;
    gPendingCount++;
}

static int snapshotsChecked(void) { return gChecked; }

// MOCK_XR_DUMP: inside xrReleaseSwapchainImage (the app holds its queue): wait for the queue,
// copy the image (COLOR_ATTACHMENT_OPTIMAL, as the app must leave it) into a host buffer and
// write every `step`-th pixel as a binary PPM. 8-bit RGBA / BGRA formats only.
static int dumpImage(void* images, int index, int width, int height, int64_t format, int step, const char* path) {
    if (!gQueue || !gDevice) return 0;
    int bgra = format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_B8G8R8A8_UNORM;
    if (!bgra && format != VK_FORMAT_R8G8B8A8_SRGB && format != VK_FORMAT_R8G8B8A8_UNORM) return 0;
    VkImage image = ((VkImage*)images)[index];
    VkDeviceSize size = (VkDeviceSize)width * height * 4;
    VkBufferCreateInfo bci;
    memset(&bci, 0, sizeof(bci));
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buf;
    if (DEV(vkCreateBuffer)(gDevice, &bci, NULL, &buf) != VK_SUCCESS) return 0;
    VkMemoryRequirements req;
    DEV(vkGetBufferMemoryRequirements)(gDevice, buf, &req);
    VkPhysicalDeviceMemoryProperties mp;
    INST(vkGetPhysicalDeviceMemoryProperties)(gPhys, &mp);
    uint32_t type = UINT32_MAX;
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t t = 0; t < mp.memoryTypeCount; t++)
        if ((req.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & want) == want) {
            type = t;
            break;
        }
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkMemoryAllocateInfo mai;
    memset(&mai, 0, sizeof(mai));
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (type == UINT32_MAX || DEV(vkAllocateMemory)(gDevice, &mai, NULL, &mem) != VK_SUCCESS) {
        DEV(vkDestroyBuffer)(gDevice, buf, NULL);
        return 0;
    }
    DEV(vkBindBufferMemory)(gDevice, buf, mem, 0);
    VkCommandPoolCreateInfo pci;
    memset(&pci, 0, sizeof(pci));
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = gQueueFamily;
    VkCommandPool pool;
    DEV(vkCreateCommandPool)(gDevice, &pci, NULL, &pool);
    VkCommandBufferAllocateInfo ai;
    memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    DEV(vkAllocateCommandBuffers)(gDevice, &ai, &cb);
    VkCommandBufferBeginInfo bi;
    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    DEV(vkBeginCommandBuffer)(cb, &bi);
    VkImageMemoryBarrier b;
    memset(&b, 0, sizeof(b));
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    DEV(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkBufferImageCopy region;
    memset(&region, 0, sizeof(region));
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = (uint32_t)width;
    region.imageExtent.height = (uint32_t)height;
    region.imageExtent.depth = 1;
    DEV(vkCmdCopyImageToBuffer)(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    DEV(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    DEV(vkEndCommandBuffer)(cb);
    VkSubmitInfo si;
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    int ok = DEV(vkQueueSubmit)(gQueue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS && DEV(vkQueueWaitIdle)(gQueue) == VK_SUCCESS;
    void* map = NULL;
    if (ok && DEV(vkMapMemory)(gDevice, mem, 0, VK_WHOLE_SIZE, 0, &map) == VK_SUCCESS) {
        FILE* f = fopen(path, "wb");
        if (f) {
            int ow = (width + step - 1) / step, oh = (height + step - 1) / step;
            fprintf(f, "P6\n%d %d\n255\n", ow, oh);
            const unsigned char* px = (const unsigned char*)map;
            for (int y = 0; y < height; y += step)
                for (int x = 0; x < width; x += step) {
                    const unsigned char* q = px + ((size_t)y * width + x) * 4;
                    unsigned char rgb[3] = {bgra ? q[2] : q[0], q[1], bgra ? q[0] : q[2]};
                    fwrite(rgb, 1, 3, f);
                }
            fclose(f);
        } else ok = 0;
        DEV(vkUnmapMemory)(gDevice, mem);
    }
    DEV(vkDestroyCommandPool)(gDevice, pool, NULL);
    DEV(vkDestroyBuffer)(gDevice, buf, NULL);
    DEV(vkFreeMemory)(gDevice, mem, NULL);
    return ok;
}

// MOCK_XR_STAMP: the app's stamp block (PRIMEFRAME_FRAME_STAMP in VulkanOpenXR.cpp), 256x20 pixels
// in the top left corner, copied at the release into one of STAMP_SLOTS host-visible slots. Releases
// come from two app threads (eyes on the pacing thread, overlay quads on the video thread).
#define STAMP_W 256
#define STAMP_H 20
#define STAMP_SLOTS 64
static struct {
    VkFence fence;
    VkCommandBuffer cb;
    unsigned gen;
    int submitted, decoded, result, eye;
    uint64_t oldest, newest;
} gStamp[STAMP_SLOTS];
static VkBuffer gStampBuf;
static VkDeviceMemory gStampMem;
static unsigned char* gStampMap;
static VkCommandPool gStampPool;
static int gStampNext = 0, gStampReady = -1;
static pthread_mutex_t gStampMutex = PTHREAD_MUTEX_INITIALIZER;

static int stampSetup(void) {
    if (gStampReady >= 0) return gStampReady;
    gStampReady = 0;
    VkBufferCreateInfo bci;
    memset(&bci, 0, sizeof(bci));
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = (VkDeviceSize)STAMP_W * STAMP_H * 4 * STAMP_SLOTS;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (DEV(vkCreateBuffer)(gDevice, &bci, NULL, &gStampBuf) != VK_SUCCESS) return 0;
    VkMemoryRequirements req;
    DEV(vkGetBufferMemoryRequirements)(gDevice, gStampBuf, &req);
    VkPhysicalDeviceMemoryProperties mp;
    INST(vkGetPhysicalDeviceMemoryProperties)(gPhys, &mp);
    uint32_t type = UINT32_MAX;
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t t = 0; t < mp.memoryTypeCount; t++)
        if ((req.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & want) == want) {
            type = t;
            break;
        }
    VkMemoryAllocateInfo mai;
    memset(&mai, 0, sizeof(mai));
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (type == UINT32_MAX || DEV(vkAllocateMemory)(gDevice, &mai, NULL, &gStampMem) != VK_SUCCESS) return 0;
    DEV(vkBindBufferMemory)(gDevice, gStampBuf, gStampMem, 0);
    void* map = NULL;
    if (DEV(vkMapMemory)(gDevice, gStampMem, 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS) return 0;
    gStampMap = (unsigned char*)map;
    VkCommandPoolCreateInfo pci;
    memset(&pci, 0, sizeof(pci));
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = gQueueFamily;
    if (DEV(vkCreateCommandPool)(gDevice, &pci, NULL, &gStampPool) != VK_SUCCESS) return 0;
    for (int i = 0; i < STAMP_SLOTS; i++) {
        VkCommandBufferAllocateInfo ai;
        memset(&ai, 0, sizeof(ai));
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = gStampPool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkFenceCreateInfo fi;
        memset(&fi, 0, sizeof(fi));
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (DEV(vkAllocateCommandBuffers)(gDevice, &ai, &gStamp[i].cb) != VK_SUCCESS ||
            DEV(vkCreateFence)(gDevice, &fi, NULL, &gStamp[i].fence) != VK_SUCCESS)
            return 0;
    }
    gStampReady = 1;
    return 1;
}

// A cell is 1 when its centre pixel is white (any colour channel above half).
static int stampBit(int i, int column, int row) {
    const unsigned char* q = gStampMap + (size_t)i * STAMP_W * STAMP_H * 4 + ((size_t)(row * 4 + 2) * STAMP_W + (size_t)(column * 4 + 2)) * 4;
    return q[0] > 127 || q[1] > 127 || q[2] > 127;
}
static int stampValue(int i, int row, uint64_t* out) {
    uint64_t value = 0;
    for (int bit = 0; bit < 64; bit++) {
        int one = stampBit(i, bit, row);
        if (one == stampBit(i, bit, row + 1)) return 0;
        if (one) value |= 1ull << bit;
    }
    *out = value;
    return 1;
}
static int stampDecode(int i) {
    static const int marker[4] = {1, 0, 1, 0};
    for (int k = 0; k < 4; k++)
        if (stampBit(i, 2 + k, 2) != marker[k]) return -1;
    if (stampBit(i, 0, 2) == stampBit(i, 1, 2)) return -1;
    if (!stampValue(i, 0, &gStamp[i].oldest) || !stampValue(i, 3, &gStamp[i].newest)) return -1;
    gStamp[i].eye = stampBit(i, 0, 2);
    return 1;
}

static int stampCapture(void* images, int index, int width, int height, unsigned* gen) {
    if (!gQueue || !gDevice || width < STAMP_W || height < STAMP_H) return -1;
    pthread_mutex_lock(&gStampMutex);
    if (!stampSetup()) {
        pthread_mutex_unlock(&gStampMutex);
        return -1;
    }
    int i = gStampNext;
    gStampNext = (gStampNext + 1) % STAMP_SLOTS;
    if (gStamp[i].submitted) {
        // Reused: its copy was queued a whole ring ago and long finished.
        DEV(vkWaitForFences)(gDevice, 1, &gStamp[i].fence, VK_TRUE, 2000000000ull);
        DEV(vkResetFences)(gDevice, 1, &gStamp[i].fence);
        gStamp[i].submitted = 0;
    }
    gStamp[i].gen++;
    gStamp[i].decoded = 0;
    VkCommandBuffer cb = gStamp[i].cb;
    DEV(vkResetCommandBuffer)(cb, 0);
    VkCommandBufferBeginInfo bi;
    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    DEV(vkBeginCommandBuffer)(cb, &bi);
    VkImageMemoryBarrier b;
    memset(&b, 0, sizeof(b));
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = ((VkImage*)images)[index];
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    DEV(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkBufferImageCopy region;
    memset(&region, 0, sizeof(region));
    region.bufferOffset = (VkDeviceSize)i * STAMP_W * STAMP_H * 4;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = STAMP_W;
    region.imageExtent.height = STAMP_H;
    region.imageExtent.depth = 1;
    DEV(vkCmdCopyImageToBuffer)(cb, b.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, gStampBuf, 1, &region);
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    DEV(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    DEV(vkEndCommandBuffer)(cb);
    VkSubmitInfo si;
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    int r = -1;
    if (DEV(vkQueueSubmit)(gQueue, 1, &si, gStamp[i].fence) == VK_SUCCESS) {
        gStamp[i].submitted = 1;
        *gen = gStamp[i].gen;
        r = i;
    }
    pthread_mutex_unlock(&gStampMutex);
    return r;
}

static int stampRead(int i, unsigned gen, int wait, uint64_t* oldest, uint64_t* newest, int* eye) {
    pthread_mutex_lock(&gStampMutex);
    int r;
    if (i < 0 || i >= STAMP_SLOTS || gStamp[i].gen != gen) r = -2;
    else if (!gStamp[i].decoded && !gStamp[i].submitted) r = -2;
    else {
        r = 1;
        if (!gStamp[i].decoded) {
            VkResult v = wait ? DEV(vkWaitForFences)(gDevice, 1, &gStamp[i].fence, VK_TRUE, 2000000000ull)
                              : DEV(vkGetFenceStatus)(gDevice, gStamp[i].fence);
            if (v != VK_SUCCESS) r = 0;
            else {
                DEV(vkResetFences)(gDevice, 1, &gStamp[i].fence);
                gStamp[i].submitted = 0;
                gStamp[i].result = stampDecode(i);
                gStamp[i].decoded = 1;
            }
        }
        if (r) {
            *oldest = gStamp[i].oldest;
            *newest = gStamp[i].newest;
            *eye = gStamp[i].eye;
            r = gStamp[i].result;
        }
    }
    pthread_mutex_unlock(&gStampMutex);
    return r;
}

static void destroyImages(void* images, int count) {
    if (!gDevice || !images) return;
    VkImage* im = (VkImage*)images;
    for (int i = 0; i < count; i++) {
        if (!im[i]) continue;
        for (int k = 0; k < gImageCount; k++) {
            if (gImages[k].image != im[i]) continue;
            if (gImages[k].map) DEV(vkUnmapMemory)(gDevice, gImages[k].memory);
            DEV(vkDestroyImage)(gDevice, im[i], NULL);
            DEV(vkFreeMemory)(gDevice, gImages[k].memory, NULL);
            gImages[k].image = VK_NULL_HANDLE;
            gImages[k].map = NULL;
            break;
        }
    }
    // A pending snapshot of a freed image would sample freed memory: the session is ending.
    gPendingCount = 0;
    free(images);
}

static void shutdownVk(void) {
    if (!gDevice) return;
    DEV(vkDeviceWaitIdle)(gDevice);
    for (int i = 0; i < gPendingCount; i++) DEV(vkDestroyFence)(gDevice, gPending[i].fence, NULL);
    gPendingCount = 0;
    for (int i = 0; i < gFreeFenceCount; i++) DEV(vkDestroyFence)(gDevice, gFreeFences[i], NULL);
    gFreeFenceCount = 0;
    if (gStampReady == 1) {
        for (int i = 0; i < STAMP_SLOTS; i++) DEV(vkDestroyFence)(gDevice, gStamp[i].fence, NULL);
        DEV(vkDestroyCommandPool)(gDevice, gStampPool, NULL);
        DEV(vkUnmapMemory)(gDevice, gStampMem);
        DEV(vkDestroyBuffer)(gDevice, gStampBuf, NULL);
        DEV(vkFreeMemory)(gDevice, gStampMem, NULL);
        gStampReady = 0;
    }
}

// MOCK_XR_EARLY=3: 64 chunks of 16 bytes spread over the image's host mapping, hashed (FNV-1a).
static uint64_t imageSig(void* images, int index) {
    VkImage image = ((VkImage*)images)[index];
    for (int k = 0; k < gImageCount; k++) {
        if (gImages[k].image != image || !gImages[k].map || gImages[k].size < 64 * 16) continue;
        uint64_t h = 1469598103934665603ull;
        VkDeviceSize step = gImages[k].size / 64;
        for (int i = 0; i < 64; i++) {
            const unsigned char* q = gImages[k].map + (VkDeviceSize)i * step;
            for (int b = 0; b < 16; b++) h = (h ^ q[b]) * 1099511628211ull;
        }
        return h ? h : 1;
    }
    return 0;
}

const MockGfx kMockGfxVk = {"vk", ownsBinding, describeBinding, getProc, formats, formatOf, createImages, fillImages, releaseWaitMs,
                            releaseSnapshot, pollSnapshots, snapshotsChecked, dumpImage, destroyImages, shutdownVk, stampCapture, stampRead, imageSig};
