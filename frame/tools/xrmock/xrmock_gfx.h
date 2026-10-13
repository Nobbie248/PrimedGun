// The graphics side of the mock OpenXR runtime (xrmock.c): everything that
// depends on the graphics API the session is bound to.  The core never
// includes a platform or graphics header; it asks the table that claims the
// session's graphics binding.
//
// Implemented: xrmock_gles.c (XR_MNDX_egl_enable + XR_KHR_opengl_es_enable,
// swapchain images are textures in the app's current EGL context) and
// xrmock_vk.c (XR_KHR_vulkan_enable2: instance and device made through the
// mock with the app's loader, swapchain images are VkImages on the app's
// device, a release checks that the app's GPU work had finished).
#pragma once

#include <openxr/openxr.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { MOCK_FMT_EYE, MOCK_FMT_UI, MOCK_FMT_MOTION, MOCK_FMT_DEPTH } MockFormatKind;

typedef struct MockGfx {
    const char* name;
    // The session's graphics binding (first struct of XrSessionCreateInfo::next
    // that this table claims).  Writes a description for the trace.
    int (*owns_binding)(XrStructureType type);
    int (*describe_binding)(const XrBaseInStructure* binding, char* out, size_t cap);
    // Entry points of the graphics extension (xrGetOpenGLESGraphicsRequirementsKHR ...).
    PFN_xrVoidFunction (*get_proc)(const char* name);
    // The swapchain formats of the API, and the one for each kind.
    int (*formats)(const int64_t** list);
    int64_t (*format_of)(MockFormatKind kind);
    // Images of a swapchain: `count` of them, `mips` levels.
    void* (*create_images)(int64_t format, uint64_t usage, int width, int height, int mips, int count);
    // Fills `count` XrSwapchainImage* structures (type-checked) with the images.
    XrResult (*fill_images)(void* images, int count, XrSwapchainImageBaseHeader* out);
    // At a release: how long the app's GPU work took to finish (ms; 0 when it
    // already had).  Null: not checked.
    double (*release_wait_ms)(void);
    // Exact check (MOCK_XR_BUSY_MS, Vulkan): at a release, sample the released image's memory
    // and mark the queue; once the GPU passed the mark, sample again: a difference means the
    // GPU was still writing the image when it was released.  Null: not available.
    void (*release_snapshot)(void* images, int index);
    // Compares the snapshots whose marks the GPU passed (wait != 0: all of them, waiting);
    // returns how many changed after their release.
    int (*poll_snapshots)(int wait);
    // How many releases the exact check covered.
    int (*snapshots_checked)(void);
    // MOCK_XR_DUMP: writes image `index` (waits for the app's queue first) as a PPM, every
    // `step`-th pixel. Null: not available.
    int (*dump_image)(void* images, int index, int width, int height, int64_t format, int step, const char* path);
} MockGfx;

extern const MockGfx kMockGfxGles;
extern const MockGfx kMockGfxVk;
