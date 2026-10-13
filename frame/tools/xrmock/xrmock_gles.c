// Mock OpenXR runtime, OpenGL ES side (xrmock_gfx.h): the swapchain images
// are textures of the app's own EGL context (the mock runs in its process, on
// the thread that calls xrCreateSwapchain).
#define XR_USE_PLATFORM_EGL
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "xrmock_gfx.h"

static int ownsBinding(XrStructureType t) { return t == XR_TYPE_GRAPHICS_BINDING_EGL_MNDX; }

static int describeBinding(const XrBaseInStructure* n, char* out, size_t cap) {
    const XrGraphicsBindingEGLMNDX* b = (const XrGraphicsBindingEGLMNDX*)n;
    return snprintf(out, cap, "binding=EGL_MNDX display=%d config=%d context=%d getProcAddress=%d", b->display != 0, b->config != 0, b->context != 0,
                    b->getProcAddress != 0);
}

static XrResult XRAPI_CALL glesRequirements(XrInstance i, XrSystemId s, XrGraphicsRequirementsOpenGLESKHR* r) {
    (void)i;
    (void)s;
    r->minApiVersionSupported = XR_MAKE_VERSION(3, 0, 0);
    r->maxApiVersionSupported = XR_MAKE_VERSION(3, 2, 0);
    return XR_SUCCESS;
}

static PFN_xrVoidFunction getProc(const char* name) {
    if (!strcmp(name, "xrGetOpenGLESGraphicsRequirementsKHR")) return (PFN_xrVoidFunction)glesRequirements;
    return NULL;
}

static const int64_t kFormats[] = {GL_RGBA8, GL_SRGB8_ALPHA8, GL_RGBA16F, GL_DEPTH24_STENCIL8, GL_DEPTH_COMPONENT32F};

static int formats(const int64_t** list) {
    *list = kFormats;
    return (int)(sizeof(kFormats) / sizeof(kFormats[0]));
}

static int64_t formatOf(MockFormatKind kind) {
    switch (kind) {
    case MOCK_FMT_EYE:
    case MOCK_FMT_UI: return GL_SRGB8_ALPHA8;
    case MOCK_FMT_MOTION: return GL_RGBA16F;
    case MOCK_FMT_DEPTH: return GL_DEPTH24_STENCIL8;
    }
    return GL_RGBA8;
}

static void* createImages(int64_t format, uint64_t usage, int width, int height, int mips, int count) {
    (void)usage;
    GLuint* tex = (GLuint*)calloc((size_t)count, sizeof(GLuint));
    glGenTextures(count, tex);
    for (int i = 0; i < count; i++) {
        glBindTexture(GL_TEXTURE_2D, tex[i]);
        glTexStorage2D(GL_TEXTURE_2D, mips, (GLenum)format, width, height);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

static XrResult fillImages(void* images, int count, XrSwapchainImageBaseHeader* out) {
    const GLuint* tex = (const GLuint*)images;
    XrSwapchainImageOpenGLESKHR* g = (XrSwapchainImageOpenGLESKHR*)out;
    for (int i = 0; i < count; i++) {
        if (g[i].type != XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR) return XR_ERROR_VALIDATION_FAILURE;
        g[i].image = tex[i];
    }
    return XR_SUCCESS;
}

const MockGfx kMockGfxGles = {"gles", ownsBinding, describeBinding, getProc, formats, formatOf, createImages, fillImages, NULL, NULL, NULL, NULL, NULL, NULL, NULL};
