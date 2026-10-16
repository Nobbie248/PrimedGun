// A mock OpenXR runtime for testing the app's frame loop (xr_app.cpp)
// without a headset.  Loaded by the OpenXR loader through XR_RUNTIME_JSON
// (see runtime.json.in), in the app's process.
//
// It keeps the call discipline of a real runtime where the app can get it
// wrong (swapchain acquire / release order, images in flight) and a frame
// clock, and writes a trace of the calls that decide how the app paces:
// swapchain creation, acquire, release, frame begin / end with the layers
// (swapchains, image rects, eye visibility, SpaceWarp info).  The trace is
// deterministic as far as the app is: poses, fovs and times are not in it.
// tools/xrmock/trace-compare.sh runs two builds and diffs their traces.
//
// Environment:
//   MOCK_XR_TRACE=<file>      trace output (none: no trace)
//   MOCK_XR_SECONDS=<s>       run time, then the session stops (20)
//   MOCK_XR_HZ=<hz>           display refresh (120; the app pairs its eyes
//                             over two refreshes at 120 Hz, and renders both
//                             eyes each refresh at other rates)
//   MOCK_XR_SPACEWARP=1       offer XR_FB_space_warp (the app then renders
//                             motion vectors and depth with each frame)
//   MOCK_XR_EXTENSIONS=a,b    replace the list of extensions offered
//   MOCK_XR_SKIP=1            a frame begun late loses refreshes (the display
//                             times jump), as on a headset
//   MOCK_XR_SKIP=late         a late xrWaitFrame returns at once with the refresh
//                             that is running (SteamVR on the Frame); display
//                             times stay in the future
//   MOCK_XR_LATCH_MS=<ms>     a layer stack lands on the first refresh at least
//                             this long after its xrEndFrame (one period), never
//                             before its display time; the summary gives the
//                             cadence (refreshes each new eye set stays on screen)
//   MOCK_XR_BUSY_MS=<ms>      Vulkan: check that each released image was finished.
//                             Exact where the images can be host visible (the
//                             Frame): the image's bytes at the release against
//                             its bytes once all work queued before the release
//                             completed (other queued work does not count). Else
//                             a release that waits longer than <ms> for the
//                             app's queue to go idle is an error.
//   MOCK_XR_EYE=<w>x<h>       recommended eye size (1728x1728, SteamVR's on
//                             the Frame)
//   MOCK_XR_TIME_OFFSET_S=<s> XrTime runs this far ahead of CLOCK_MONOTONIC
//                             (0); XR_KHR_convert_timespec_time converts
//   MOCK_XR_CLOCK=raw         XrTime is CLOCK_MONOTONIC_RAW (SteamVR on the Frame), which drifts
//                             against CLOCK_MONOTONIC
//   MOCK_XR_EARLY=1           while a frame is begun and not ended, xrWaitFrame does not block: it
//                             returns at once with the next refresh even before its wake-up (SteamVR
//                             on the Frame); summary line "waits". 2: also a call after that
//                             refresh's wake-up gets the one after it. 3: SteamVR's hand-out as the
//                             headset sessions of 2026-10-14 showed it (see handOut3 below): with a
//                             frame pending, the refresh after the one it was handed (that same one
//                             again when it is still ahead, or when it is the running one and the
//                             pending frame's GPU work already wrote its eye image; the running one
//                             when the pending frame was for an earlier refresh and was handed out
//                             on time); with none pending, the refresh after the one the last frame
//                             landed on, early and at once after a frame that landed late, else
//                             blocking until its wake-up. Summary line "hand-out"
//   MOCK_XR_PHASE_MS=<ms>[@<s>,<s>...]  at the first xrWaitFrame call after each of the given
//                             session seconds (default 5), move the refresh grid so that this call
//                             comes <ms> after a wake-up (the game's frame phase against SteamVR's
//                             refreshes; the sessions showed calls about 14.5 ms after one)
//   MOCK_XR_FOCUS_BLIP=<ms>[@<s>]  every <s> seconds (7) the session loses
//                             focus for <ms> ms (VISIBLE, then FOCUSED again),
//                             as SteamVR does now and then
//   MOCK_XR_PROFILE=<path>    the current interaction profile of both hands
//                             (e.g. /interaction_profiles/valve/frame_controller;
//                             default none); an INTERACTION_PROFILE_CHANGED
//                             event follows the first FOCUSED, and suggested
//                             bindings for it are traced
//   MOCK_XR_RATES=<hz>,...    refresh rates XR_FB_display_refresh_rate offers (72,90,120)
//   MOCK_XR_DUMP=<dir>        Vulkan: write released images as PPM files (every 4th pixel)
//                             <dir>/t<seconds>-sc<id>-<n>.ppm: at the seconds in
//                             MOCK_XR_DUMP_AT=<s>,<s>... (default every 10 s), the next
//                             MOCK_XR_DUMP_FRAMES=<n> (1) releases of every swapchain
//   MOCK_XR_STAMP=1           Vulkan, with PRIMEDFRAME_FRAME_STAMP=1 in the app: every eye image
//                             carries (a block in its corner) the oldest and newest locate (display
//                             time) whose poses the draws of that image used; both are compared with
//                             the locate the submitted layer's poses came from (summary "stamps";
//                             a mismatch is an ERROR)
//   MOCK_XR_HEAD_DPS=<deg/s>  the head's peak turning speed (yaw, a sine; default 54, the old
//                             +-17 degrees at 0.5 Hz); MOCK_XR_HEAD_DEG=<deg> its amplitude (17.2).
//                             The head also rises 0.1 mm a second, so no two locates share a pose.
//   MOCK_XR_HEAD_PROFILE=stops  instead of the sine: the head turns at MOCK_XR_HEAD_DPS (yaw) for
//                             0.3 s, stops dead for 0.7 s, turns back, and so on; xrLocateViews then
//                             predicts as a runtime does, from the head's pose and speed at the call
//                             (constant speed), so a pose predicted past a stop overshoots it. The
//                             summary line "prediction" gives the yaw error of each new eye set
//                             against the head at the refresh it lands on (overshoot: past it in the
//                             direction of the last turn; trail: behind it)
//   MOCK_XR_LAND=throttled    with MOCK_XR_THROTTLE, a layer stack that misses its display time lands
//                             a whole throttled period later, not on the next refresh (SteamVR on the
//                             Frame at framesToThrottle 1: 2 vsyncs to photons on time, 4 when late)
//   MOCK_XR_LEAD=<refreshes>  predictedDisplayTime this many refreshes after xrWaitFrame's wake-up
//                             (default 3: SteamVR's lead on the Frame was 25 to 27 ms at 120 Hz)
//   MOCK_XR_THROTTLE=<n>      SteamVR's framesToThrottle: xrWaitFrame hands out every (1 + n)th
//                             refresh (predictedDisplayPeriod (1 + n) refreshes); default 0
//   MOCK_XR_ONTIME_MS=<ms>    the summary's "latency" line counts new eye sets ended later than this
//                             after the wake-up of their display time (2.5; SteamVR on the Frame
//                             showed 60 fps frames ended later than ~2.5 ms after it in a 1-3
//                             cadence when they were not a whole refresh late)
// The "latency" line also gives, per new eye set, the refresh it lands on minus the time its head
// poses were located for (the prediction's shortfall) and the same for the controllers
// (xrLocateSpace calls after the xrWaitFrame the frame began with; the poses themselves stay fixed).
// Every xrEndFrame must carry the predictedDisplayTime of the xrWaitFrame its xrBeginFrame followed
// (ERROR otherwise; summary line "end times").
// The graphics API is chosen by the app's binding (xrmock_gfx.h).
//
// Checks (ERROR lines in the trace, and a summary on stderr when the session
// ends): acquire / wait / release order per swapchain (one image waited for
// at a time, a release only of a waited image, no more acquired than the
// swapchain has); with Vulkan, that the
// app's GPU work had finished at each release (SteamVR shows a released image
// at once); that both eyes of a projection layer come from one xrLocateViews
// (the views turn with time, so views located for different times differ),
// and for which display time that locate was made.
#define _GNU_SOURCE
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>

#include "xrmock_gfx.h"

static FILE* gTrace;
static int gFrame = 0;
static void trace(const char* fmt, ...) {
    if (!gTrace) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(gTrace, "f%d ", gFrame);
    vfprintf(gTrace, fmt, ap);
    fputc('\n', gTrace);
    va_end(ap);
    fflush(gTrace);
}

static int64_t nowNs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}
// XrTime = CLOCK_MONOTONIC + gTimeOffset (MOCK_XR_TIME_OFFSET_S); with MOCK_XR_CLOCK=raw it is
// CLOCK_MONOTONIC_RAW (+ the offset), as SteamVR on the Frame: it drifts against CLOCK_MONOTONIC
// by NTP's rate correction (tens of us a second), and the app must convert, not subtract a constant.
static int64_t gTimeOffset = 0;
static int gClockRaw = 0;
// MOCK_XR_EARLY and the summary's "waits" line.
static int gEarly = 0, gPendingFrame = 0, gEarlyHandouts = 0, gBlockedWaits = 0, gWaitCalls = 0;
// MOCK_XR_EARLY=3: refreshes as wake-up indices (refresh periods from gT0); the refresh handed out by
// the last xrWaitFrame and whether it was handed out late (over 4 ms after its wake-up), the same for
// the frame begun, the refresh the last ended frame landed on and whether that was after its display
// time; hand-outs by kind.
static int64_t gHandWake = -1, gPendWake = -1, gLastLandWake = -1;
static int gHandLate = 0, gPendLate = 0, gLastLandedLate = 0;
static int gHandNext = 0, gHandAgain = 0, gHandRunning = 0, gHandSkip = 0, gHandBlocked = 0, gHandAtOnce = 0;
// MOCK_XR_PHASE_MS
static double gPhaseMs = -1.0, gPhaseAt[16];
static int gPhaseCount = 0, gPhaseNext = 0, gPhaseShifts = 0;
static int64_t gSessionStart = 0;
// Per 10 s window: holds of new eye sets, never shown, landed on time / later, hand-outs.
static int gWinHeld[6], gWinUnseen = 0, gWinOnTime = 0, gWinLater = 0, gWinIdx = -1;
static int gWinHand[6];
static int64_t xrOff(void) {
    if (!gClockRaw) return gTimeOffset;
    struct timespec m, r;
    clock_gettime(CLOCK_MONOTONIC, &m);
    clock_gettime(CLOCK_MONOTONIC_RAW, &r);
    return gTimeOffset + ((int64_t)r.tv_sec - (int64_t)m.tv_sec) * 1000000000ll + (r.tv_nsec - m.tv_nsec);
}

// The graphics tables, tried in order for the session's binding.  Lane D2
// adds the Vulkan one here.
static const MockGfx* const kGfxTables[] = {&kMockGfxGles, &kMockGfxVk};
static const MockGfx* gGfx = NULL;  // the one the session is bound to

#define MAXSC 64
typedef struct {
    int w, h, mips, count;
    int64_t format;
    void* images;
    int acquired, next;
    int waited;  // of the images acquired, how many were waited for
    int released;  // an image was released since the last xrEndFrame
    int dumpMark, dumped;  // MOCK_XR_DUMP: the dump time this chain is on, images written for it
    int attach;            // colour attachment usage (eye images; quads are sampled)
    int stampSlot;         // MOCK_XR_STAMP: the last released image's stamp read-back slot, -1 none
    unsigned stampGen;
    uint64_t waitSig;      // MOCK_XR_EARLY=3: the waited image's signature at its wait
} Chain;
static Chain gChains[MAXSC];
static int gErrors = 0;  // ERROR lines in the trace
static int gChainCount = 0;

static char gExtNames[64][XR_MAX_EXTENSION_NAME_SIZE];
static int gExtCount = 0;

static XrPath gPaths[512];
static char gPathStr[512][128];
static int gPathCount = 0;

static int gEventQueue[16], gEventCount = 0;
static int gEyeW = 1728, gEyeH = 1728;
// Focus blips (MOCK_XR_FOCUS_BLIP) and the interaction profile (MOCK_XR_PROFILE).
#define EVENT_PROFILE_CHANGED (-2)
static double gBlipMs = 0.0, gBlipEvery = 7.0;
static int64_t gNextBlip = 0, gRefocusAt = 0;
static int gFocused = 0, gBlips = 0, gProfileSent = 0;
static const char* gProfile = NULL;
static int64_t gT0 = 0, gPeriod = 8333333, gEndAt = 0;
static int64_t gLead = 3;  // MOCK_XR_LEAD (refreshes)
static double gHeadAmp = 0.3, gHeadHz = 0.5;  // MOCK_XR_HEAD_DEG, MOCK_XR_HEAD_DPS
static int gHeadStops = 0;                     // MOCK_XR_HEAD_PROFILE=stops
static double gHeadRate = 0.0;                 // its turning speed, rad/s
static int gLandThrottled = 0;                 // MOCK_XR_LAND=throttled
// MOCK_XR_HEAD_PROFILE=stops: yaw and its rate at t seconds (display-time clock, from the session's start).
static double stopsYaw(double t, double* rate) {
    const double turn = 0.3;
    long seg = (long)floor(t);
    double ph = t - (double)seg, moved = ph < turn ? ph : turn;
    int up = (seg % 2) == 0;
    if (rate) *rate = ph < turn ? (up ? gHeadRate : -gHeadRate) : 0.0;
    return up ? gHeadRate * moved : gHeadRate * (turn - moved);
}
// Controller locates (xrLocateSpace) of the frame: the last one after its xrWaitFrame.
static XrTime gCtlTime = 0, gBegunCtlTime = 0;
static int64_t gCtlCallNs = 0, gBegunCtlCallNs = 0;
// Prediction shortfall of new eye sets (landing refresh minus the time located for), head and controllers,
// and the yaw error with MOCK_XR_HEAD_PROFILE=stops.
static double gShortSumMs = 0, gShortMinMs = 1e9, gShortMaxMs = -1e9, gCtlShortSumMs = 0, gCtlAgeSumMs = 0;
static int gShortN = 0, gCtlN = 0, gShortHist[5];
static double gYawAbsSum = 0, gYawOverMax = 0, gYawTrailMax = 0, gYawMovingAbsSum = 0;
static int gYawN = 0, gYawMovingN = 0, gYawOverN = 0;
// xrEndFrame against the frame's predicted display time (the one its xrBeginFrame followed).
static XrTime gWaitPredicted = 0, gBegunPredicted = 0;
static int gEndTimeMatch = 0, gEndTimeOff = 0;
static double gEndTimeOffWorstMs = 0.0;
static int gQuitSent = 0;

static void pushState(int s) {
    if (gEventCount < 16) gEventQueue[gEventCount++] = s;
}

static XrResult XRAPI_CALL mEnumExt(const char* layer, uint32_t cap, uint32_t* count, XrExtensionProperties* props) {
    (void)layer;
    *count = (uint32_t)gExtCount;
    if (cap == 0) return XR_SUCCESS;
    if (cap < (uint32_t)gExtCount) return XR_ERROR_SIZE_INSUFFICIENT;
    for (int i = 0; i < gExtCount; i++) {
        memset(props[i].extensionName, 0, sizeof(props[i].extensionName));
        strcpy(props[i].extensionName, gExtNames[i]);
        props[i].extensionVersion = 1;
    }
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL mCreateInstance(const XrInstanceCreateInfo* ci, XrInstance* out) {
    char line[2048] = "";
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
        strcat(line, " ");
        strcat(line, ci->enabledExtensionNames[i]);
        int offered = 0;
        for (int k = 0; k < gExtCount; k++) offered |= !strcmp(gExtNames[k], ci->enabledExtensionNames[i]);
        if (!offered) {
            fprintf(stderr, "xrmock: refused xrCreateInstance: extension %s is not offered\n", ci->enabledExtensionNames[i]);
            return XR_ERROR_EXTENSION_NOT_PRESENT;
        }
    }
    trace("createInstance extensions:%s", line);
    *out = (XrInstance)(uintptr_t)1;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mDestroyInstance(XrInstance i) { (void)i; return XR_SUCCESS; }
static XrResult XRAPI_CALL mResultToString(XrInstance i, XrResult r, char buf[XR_MAX_RESULT_STRING_SIZE]) {
    (void)i;
    snprintf(buf, XR_MAX_RESULT_STRING_SIZE, "XrResult_%d", (int)r);
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mGetInstanceProps(XrInstance i, XrInstanceProperties* p) {
    (void)i;
    p->runtimeVersion = XR_MAKE_VERSION(0, 1, 0);
    strcpy(p->runtimeName, "xrmock");
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mGetSystem(XrInstance i, const XrSystemGetInfo* gi, XrSystemId* id) {
    (void)i;
    (void)gi;
    *id = 1;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mGetSystemProps(XrInstance i, XrSystemId s, XrSystemProperties* p) {
    (void)i;
    (void)s;
    strcpy(p->systemName, "mock frame");
    for (XrBaseOutStructure* n = (XrBaseOutStructure*)p->next; n; n = n->next) {
        if (n->type == XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT) {
            ((XrSystemEyeGazeInteractionPropertiesEXT*)n)->supportsEyeGazeInteraction = XR_TRUE;
        }
        if (n->type == XR_TYPE_SYSTEM_SPACE_WARP_PROPERTIES_FB) {
            XrSystemSpaceWarpPropertiesFB* w = (XrSystemSpaceWarpPropertiesFB*)n;
            w->recommendedMotionVectorImageRectWidth = 420;
            w->recommendedMotionVectorImageRectHeight = 440;
        }
    }
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mCreateSession(XrInstance i, const XrSessionCreateInfo* ci, XrSession* out) {
    (void)i;
    for (const XrBaseInStructure* n = (const XrBaseInStructure*)ci->next; n && !gGfx; n = n->next) {
        for (unsigned t = 0; t < sizeof(kGfxTables) / sizeof(kGfxTables[0]); t++) {
            if (kGfxTables[t]->owns_binding(n->type)) {
                char desc[256];
                gGfx = kGfxTables[t];
                gGfx->describe_binding(n, desc, sizeof(desc));
                trace("createSession %s", desc);
                break;
            }
        }
    }
    if (!gGfx) {
        trace("createSession: no graphics binding the mock knows");
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }
    *out = (XrSession)(uintptr_t)2;
    pushState(XR_SESSION_STATE_IDLE);
    pushState(XR_SESSION_STATE_READY);
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mDestroySession(XrSession s) {
    (void)s;
    if (gGfx && gGfx->shutdown) gGfx->shutdown();
    return XR_SUCCESS;
}
static int gSpaces = 0;
static XrResult XRAPI_CALL mCreateRefSpace(XrSession s, const XrReferenceSpaceCreateInfo* ci, XrSpace* out) {
    (void)s;
    (void)ci;
    *out = (XrSpace)(uintptr_t)(100 + gSpaces++);
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mCreateActionSpace(XrSession s, const XrActionSpaceCreateInfo* ci, XrSpace* out) {
    (void)s;
    (void)ci;
    *out = (XrSpace)(uintptr_t)(100 + gSpaces++);
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mDestroySpace(XrSpace s) { (void)s; return XR_SUCCESS; }
// Added for the game apps, which call these.
static XrResult XRAPI_CALL mEnumBlend(XrInstance i, XrSystemId sys, XrViewConfigurationType vct, uint32_t cap, uint32_t* count, XrEnvironmentBlendMode* modes) {
    (void)i;
    (void)sys;
    (void)vct;
    *count = 1;
    if (cap >= 1 && modes) modes[0] = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mEnumRefSpaces(XrSession s, uint32_t cap, uint32_t* count, XrReferenceSpaceType* spaces) {
    (void)s;
    *count = 3;
    if (cap >= 3 && spaces) {
        spaces[0] = XR_REFERENCE_SPACE_TYPE_VIEW;
        spaces[1] = XR_REFERENCE_SPACE_TYPE_LOCAL;
        spaces[2] = XR_REFERENCE_SPACE_TYPE_STAGE;
    }
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mDestroyActionSet(XrActionSet a) { (void)a; return XR_SUCCESS; }
static XrResult XRAPI_CALL mLocateSpace(XrSpace sp, XrSpace base, XrTime t, XrSpaceLocation* loc) {
    (void)sp;
    (void)base;
    gCtlTime = t;
    gCtlCallNs = nowNs();
    loc->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT |
                         XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    loc->pose.orientation = (XrQuaternionf){0, 0, 0, 1};
    loc->pose.position = (XrVector3f){0, 1.6f, 0};
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mEnumViewConfigViews(XrInstance i, XrSystemId s, XrViewConfigurationType t, uint32_t cap, uint32_t* count,
                                                XrViewConfigurationView* v) {
    (void)i;
    (void)s;
    (void)t;
    *count = 2;
    if (cap == 0) return XR_SUCCESS;
    for (int e = 0; e < 2; e++) {
        v[e].recommendedImageRectWidth = (uint32_t)gEyeW;
        v[e].recommendedImageRectHeight = (uint32_t)gEyeH;
        v[e].maxImageRectWidth = 4096;
        v[e].maxImageRectHeight = 4096;
        v[e].recommendedSwapchainSampleCount = 1;
        v[e].maxSwapchainSampleCount = 1;
    }
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mEnumFormats(XrSession s, uint32_t cap, uint32_t* count, int64_t* f) {
    (void)s;
    const int64_t* list;
    int n = gGfx->formats(&list);
    *count = (uint32_t)n;
    if (cap == 0) return XR_SUCCESS;
    for (int i = 0; i < n && i < (int)cap; i++) f[i] = list[i];
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mCreateSwapchain(XrSession s, const XrSwapchainCreateInfo* ci, XrSwapchain* out) {
    (void)s;
    if (gChainCount >= MAXSC) return XR_ERROR_LIMIT_REACHED;
    Chain* c = &gChains[gChainCount];
    c->w = (int)ci->width;
    c->h = (int)ci->height;
    c->mips = (int)ci->mipCount;
    c->format = ci->format;
    c->count = 3;
    c->attach = (ci->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) != 0;
    c->stampSlot = -1;
    c->images = gGfx->create_images(c->format, ci->usageFlags, c->w, c->h, c->mips, c->count);
    trace("createSwapchain sc%d %dx%d mips=%d format=0x%llx usage=0x%llx", gChainCount, c->w, c->h, c->mips, (long long)c->format,
          (unsigned long long)ci->usageFlags);
    if (gGfx == &kMockGfxVk && ci->mipCount > 1) {
        // SteamVR on the Frame allocates a Vulkan swapchain image's memory for
        // level 0 only: levels below it are written past the image.
        trace("ERROR createSwapchain sc%d with %u mip levels: SteamVR allocates one", gChainCount, ci->mipCount);
        gErrors++;
    }
    *out = (XrSwapchain)(uintptr_t)(1000 + gChainCount);
    gChainCount++;
    return XR_SUCCESS;
}
static int gReleases = 0, gBusyReleases = 0;
static Chain* chainOf(XrSwapchain sc) { return &gChains[(uintptr_t)sc - 1000]; }
static XrResult XRAPI_CALL mDestroySwapchain(XrSwapchain sc) {
    Chain* c = chainOf(sc);
    // The app destroys its device after this; images left behind are validation errors.
    if (gGfx && gGfx->destroy_images && c->images) {
        if (gGfx->poll_snapshots) {
            int changed = gGfx->poll_snapshots(1);
            gErrors += changed;
            gBusyReleases += changed;
        }
        gGfx->destroy_images(c->images, c->count);
        c->images = NULL;
    }
    return XR_SUCCESS;
}
static int chainId(XrSwapchain sc) { return (int)((uintptr_t)sc - 1000); }
static XrResult XRAPI_CALL mEnumSwapImages(XrSwapchain sc, uint32_t cap, uint32_t* count, XrSwapchainImageBaseHeader* im) {
    Chain* c = chainOf(sc);
    *count = (uint32_t)c->count;
    if (cap == 0) return XR_SUCCESS;
    return gGfx->fill_images(c->images, c->count < (int)cap ? c->count : (int)cap, im);
}
static XrResult XRAPI_CALL mAcquire(XrSwapchain sc, const XrSwapchainImageAcquireInfo* ai, uint32_t* idx) {
    (void)ai;
    Chain* c = chainOf(sc);
    if (c->acquired >= c->count) {
        trace("ERROR acquire sc%d with all images acquired", chainId(sc));
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    *idx = (uint32_t)c->next;
    c->next = (c->next + 1) % c->count;
    c->acquired++;
    trace("acquire sc%d idx=%u", chainId(sc), *idx);
    return XR_SUCCESS;
}
static double gBusyMaxMs = 0.0;
static int gWaitHist[5];
static int gPoseAge[6];  // display time minus locate time in refreshes: <0, 0, 1, 2, 3, more
static int gPoseSets = 0, gPoseSameTime = 0, gPoseEarlier = 0, gPoseLater = 0, gPoseSplit = 0, gPoseUnknown = 0;
static int checkPoses(const XrCompositionLayerProjection* p, XrTime displayTime, int fresh);
static XrTime locateTime(int i);
static int64_t locateCallNs(int i);
// Latency of new eye sets: the xrLocateViews call of their poses to the refresh they land on,
// and their xrEndFrame against the wake-up of their display time's xrWaitFrame (SteamVR on the
// Frame showed 60 fps frames ended more than ~2.5 ms after it in a 1-3 cadence; MOCK_XR_ONTIME_MS).
static double gLatSumMs = 0, gLatMaxMs = 0, gLatMinMs = 1e9, gWakeEndSumMs = 0, gWakeEndMaxMs = 0, gOnTimeMs = 2.5;
static int gLatN = 0, gOffWindow = 0, gLatHist[6];
static int gThrottle = 0;  // MOCK_XR_THROTTLE: SteamVR's framesToThrottle (slots every 1 + N refreshes)
static int gStampOn = 0;
// Frames by what they showed: a projection layer whose eye images were both released since
// the last xrEndFrame (fresh), one shown again (repeat), none.
static int gLateLayers = 0, gLayerEnds = 0, gUnseen = 0, gHeld[6], gRun2 = 0, gRun2Max = 0, gFlips = 0;
static int64_t gLastFreshSlot = -1, gLastHeld = -1;
static int gEnds = 0, gFreshSets = 0, gFreshSame = 0, gFreshEarlier = 0, gFreshLater = 0, gRepeatSets = 0, gQuadEnds = 0, gEmptyEnds = 0;
static XrResult XRAPI_CALL mWaitImage(XrSwapchain sc, const XrSwapchainImageWaitInfo* wi) {
    (void)wi;
    Chain* c = chainOf(sc);
    if (c->waited >= c->acquired) {
        trace("ERROR wait sc%d without an image acquired", chainId(sc));
        gErrors++;
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    // OpenXR: one image of a swapchain waited for at a time (SteamVR returns
    // XR_ERROR_CALL_ORDER_INVALID, and the game aborted on it in round 3).
    if (c->waited > 0) {
        trace("ERROR wait sc%d with an image already waited for and not released", chainId(sc));
        gErrors++;
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    c->waited++;
    if (gEarly == 3 && c->attach && gGfx && gGfx->image_sig) {
        int idx = ((c->next - c->acquired) % c->count + c->count) % c->count;
        c->waitSig = gGfx->image_sig(c->images, idx);
    }
    return XR_SUCCESS;
}
// MOCK_XR_DUMP (see the top): the image being released, written at the dump times.
static void maybeDump(Chain* c, XrSwapchain sc) {
    static const char* dir = NULL;
    static double at[64];
    static int atCount = -1, frames = 1;
    if (atCount < 0) {
        dir = getenv("MOCK_XR_DUMP");
        atCount = 0;
        const char* list = getenv("MOCK_XR_DUMP_AT");
        for (const char* q = list; q && *q && atCount < 64;) {
            at[atCount++] = atof(q);
            q = strchr(q, ',');
            if (q) q++;
        }
        if (!atCount)
            for (; atCount < 64; atCount++) at[atCount] = 10.0 * (atCount + 1);
        if (getenv("MOCK_XR_DUMP_FRAMES")) frames = atoi(getenv("MOCK_XR_DUMP_FRAMES"));
    }
    if (!dir || !gGfx || !gGfx->dump_image || !gT0) return;
    double t = (double)(nowNs() - gT0) / 1e9;
    int mark = 0;
    while (mark < atCount && at[mark] <= t) mark++;
    if (mark == 0) return;
    if (c->dumpMark != mark) {
        c->dumpMark = mark;
        c->dumped = 0;
    }
    if (c->dumped >= frames || t - at[mark - 1] > 5.0) return;
    int idx = ((c->next - c->acquired) % c->count + c->count) % c->count;
    char path[512];
    snprintf(path, sizeof(path), "%s/t%03d-sc%d-%02d.ppm", dir, (int)at[mark - 1], chainId(sc), c->dumped);
    if (gGfx->dump_image(c->images, idx, c->w, c->h, c->format, 4, path)) trace("dump sc%d %s", chainId(sc), path);
    c->dumped++;
}

static XrResult XRAPI_CALL mRelease(XrSwapchain sc, const XrSwapchainImageReleaseInfo* ri) {
    (void)ri;
    Chain* c = chainOf(sc);
    if (c->acquired <= 0) {
        trace("ERROR release sc%d without acquire", chainId(sc));
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    // The image released is the oldest acquired, and it must have been waited
    // for: with one waited image at most, acquired must not exceed the
    // waited one plus those acquired after it.
    if (c->waited <= 0) {
        trace("ERROR release sc%d of an image not waited for", chainId(sc));
        gErrors++;
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    // Opt-in (MOCK_XR_BUSY_MS): the app's frame loop now submits the next
    // set before it releases the last, so the queue is rarely idle here.
    static int busyCheck = -1;
    if (busyCheck < 0) busyCheck = getenv("MOCK_XR_BUSY_MS") != NULL;
    if (busyCheck && gGfx && gGfx->release_snapshot) {
        // Exact: the image's bytes at the release against its bytes once all work queued
        // before the release completed (xrmock_vk.c).  Other work in the queue does not count.
        int idx = ((c->next - c->acquired) % c->count + c->count) % c->count;
        gGfx->release_snapshot(c->images, idx);
        gReleases++;
    } else if (busyCheck && gGfx && gGfx->release_wait_ms) {
        // The wait includes other processes' GPU work (the desktop's
        // compositor) ahead of the mock's own check: up to 4 ms seen
        // with the app's work all done.  The eyes' work takes 10 ms and more.
        static double limit = -1.0;
        if (limit < 0.0) limit = getenv("MOCK_XR_BUSY_MS") ? atof(getenv("MOCK_XR_BUSY_MS")) : 4.0;
        double ms = gGfx->release_wait_ms();
        gReleases++;
        gWaitHist[ms < 0.5 ? 0 : ms < 1.0 ? 1 : ms < 2.0 ? 2 : ms < 4.0 ? 3 : 4]++;
        if (ms > limit) {
            gBusyReleases++;
            if (ms > gBusyMaxMs) gBusyMaxMs = ms;
            trace("ERROR release sc%d before the GPU finished", chainId(sc));
            gErrors++;
        }
    }
    maybeDump(c, sc);
    if (gStampOn && c->attach && gGfx && gGfx->stamp_capture) {
        int idx = ((c->next - c->acquired) % c->count + c->count) % c->count;
        c->stampSlot = gGfx->stamp_capture(c->images, idx, c->w, c->h, &c->stampGen);
    }
    c->acquired--;
    c->waited--;
    c->released = 1;
    trace("release sc%d", chainId(sc));
    return XR_SUCCESS;
}
// MOCK_XR_STAMP: eye images of submitted projection layers waiting for their stamp read-back.
#define STAMP_PENDING 512
static struct {
    int slot, eye, fresh;
    unsigned gen;
    XrTime expect;  // the time the layer's poses were located for
} gStampPending[STAMP_PENDING];
static int gStampPendingCount = 0;
static int gStampChecked = 0, gStampMatch = 0, gStampMixed = 0, gStampOther = 0, gStampOtherEye = 0, gStampZero = 0, gStampNone = 0,
           gStampLost = 0, gStampUnknownPose = 0, gStampMixedFresh = 0, gStampOtherFresh = 0;
static int gStampOff[5];  // newest stamp minus the layer's locate in refreshes: <=-2, -1, between, +1, >=+2
static double gStampSpreadMaxMs = 0.0;  // newest minus oldest locate of one image
static void stampPoll(int wait) {
    for (int i = 0; i < gStampPendingCount;) {
        uint64_t oldest = 0, newest = 0;
        int eye = 0;
        int r = gGfx->stamp_read(gStampPending[i].slot, gStampPending[i].gen, wait, &oldest, &newest, &eye);
        if (r == 0) {
            i++;
            continue;
        }
        gStampChecked++;
        if (r == -2) gStampLost++;
        else if (r < 0) gStampNone++;
        else if (oldest == 0 || newest == 0) gStampZero++;
        else {
            const int64_t expect = (int64_t)gStampPending[i].expect;
            const int64_t dOld = (int64_t)oldest - expect, dNew = (int64_t)newest - expect;
            const int sameOld = dOld > -200000 && dOld < 200000, sameNew = dNew > -200000 && dNew < 200000;
            const double spread = (double)((int64_t)newest - (int64_t)oldest) / 1e6;
            if (spread > gStampSpreadMaxMs) gStampSpreadMaxMs = spread;
            if (sameOld && sameNew && eye == gStampPending[i].eye) gStampMatch++;
            else if (sameOld && sameNew) {
                gStampOtherEye++;
                trace("ERROR stamp: eye %d image carries the other eye's stamp", gStampPending[i].eye);
                gErrors++;
            } else {
                if (oldest != newest) {
                    gStampMixed++;
                    if (gStampPending[i].fresh) gStampMixedFresh++;
                } else {
                    gStampOther++;
                    if (gStampPending[i].fresh) gStampOtherFresh++;
                }
                double k = (double)dNew / (double)gPeriod;
                long n = lround(k);
                gStampOff[fabs(k - (double)n) > 0.1 ? 2 : n <= -2 ? 0 : n == -1 ? 1 : n == 1 ? 3 : 4]++;
                if (gStampMixed + gStampOther <= 20)
                    trace("ERROR stamp: eye %d image drawn with poses located %.2f to %.2f ms from the one submitted with it%s", gStampPending[i].eye,
                          (double)dOld / 1e6, (double)dNew / 1e6, gStampPending[i].fresh ? "" : " (repeat)");
                gErrors++;
            }
        }
        gStampPending[i] = gStampPending[--gStampPendingCount];
    }
}
static XrResult XRAPI_CALL mBeginSession(XrSession s, const XrSessionBeginInfo* bi) {
    (void)s;
    (void)bi;
    gT0 = nowNs();
    gSessionStart = gT0;
    const char* secs = getenv("MOCK_XR_SECONDS");
    gEndAt = gT0 + (int64_t)((secs ? atof(secs) : 20.0) * 1e9);
    pushState(XR_SESSION_STATE_SYNCHRONIZED);
    pushState(XR_SESSION_STATE_VISIBLE);
    pushState(XR_SESSION_STATE_FOCUSED);
    trace("beginSession");
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mEndSession(XrSession s) {
    (void)s;
    if (gGfx && gGfx->poll_snapshots) {
        int changed = gGfx->poll_snapshots(1);
        gErrors += changed;
        gBusyReleases += changed;
    }
    if (gGfx && gGfx->snapshots_checked)
        fprintf(stderr, "xrmock: exact release check: %d releases compared after the GPU passed them, %d changed after release\n",
                gGfx->snapshots_checked(), gBusyReleases);
    fprintf(stderr,
            "xrmock: summary: %d errors; %d releases checked for finished GPU work, %d before it finished (worst %.1f ms); "
            "%d projection layers: eyes from one locate for the display time submitted %d, for an earlier one %d, a later one %d; eyes from "
            "different locates %d, poses not from a locate %d\n",
            gErrors, gReleases, gBusyReleases, gBusyMaxMs, gPoseSets, gPoseSameTime, gPoseEarlier, gPoseLater, gPoseSplit, gPoseUnknown);
    fprintf(stderr, "xrmock: projection layers by refreshes from the locate to the display time: later %d, 0: %d, 1: %d, 2: %d, 3: %d, more %d\n", gPoseAge[0],
            gPoseAge[1], gPoseAge[2], gPoseAge[3], gPoseAge[4], gPoseAge[5]);
    fprintf(stderr,
            "xrmock: frames: %d ended; %d showed newly released eyes (from one locate for the display time submitted %d, for an earlier one %d, "
            "a later one %d), %d repeated a projection layer, %d a quad, %d no layer; focus blips %d\n",
            gEnds, gFreshSets, gFreshSame, gFreshEarlier, gFreshLater, gRepeatSets, gQuadEnds, gEmptyEnds, gBlips);
    if (gStampOn) {
        if (gGfx && gGfx->stamp_read) stampPoll(1);
        fprintf(stderr,
                "xrmock: stamps: %d eye images of projection layers checked: %d drawn entirely with the locate of their submitted poses; "
                "%d drawn with several locates (%d fresh), %d with one other locate (%d fresh) (newest minus submitted, refreshes <=-2: %d, -1: %d, "
                "between: %d, +1: %d, >=+2: %d; widest spread in one image %.2f ms); %d with the other eye's stamp, %d stamped 0 (XFB not "
                "stamped), %d without a stamp, %d not read back, %d eyes of layers with poses from no locate\n",
                gStampChecked, gStampMatch, gStampMixed, gStampMixedFresh, gStampOther, gStampOtherFresh, gStampOff[0], gStampOff[1], gStampOff[2],
                gStampOff[3], gStampOff[4], gStampSpreadMaxMs, gStampOtherEye, gStampZero, gStampNone, gStampLost, gStampUnknownPose);
    }
    fprintf(stderr, "xrmock: waits: %d xrWaitFrame calls, %d blocked more than 2 ms, %d handed out the next refresh early (a frame was pending)%s\n",
            gWaitCalls, gBlockedWaits, gEarlyHandouts, gClockRaw ? "; XrTime is CLOCK_MONOTONIC_RAW" : "");
    if (gEarly == 3)
        fprintf(stderr, "xrmock: hand-out: next %d, again %d, running %d, skip %d; blocked %d, at once %d; grid moved %d times\n", gHandNext,
                gHandAgain, gHandRunning, gHandSkip, gHandBlocked, gHandAtOnce, gPhaseShifts);
    fprintf(stderr, "xrmock: end times: %d xrEndFrame with the predicted display time of their frame, %d with another (worst %.2f ms)\n", gEndTimeMatch,
            gEndTimeOff, gEndTimeOffWorstMs);
    fprintf(stderr,
            "xrmock: cadence: %d layer stacks ended, %d late (landed after their display time); new eye sets held on screen for 1 refresh %d, "
            "2: %d, 3: %d, 4: %d, 5+: %d; never shown %d; 1-3 or 3-1 flips %d; longest run of 2s %d\n",
            gLayerEnds, gLateLayers, gHeld[1], gHeld[2], gHeld[3], gHeld[4], gHeld[5], gUnseen, gFlips, gRun2Max);
    if (gLatN) {
        fprintf(stderr,
                "xrmock: latency: %d new eye sets: their poses' xrLocateViews call to the refresh they land on avg %.1f ms (min %.1f, max %.1f; "
                "<20 ms %d, 20-30 %d, 30-40 %d, 40-50 %d, 50-60 %d, more %d); xrEndFrame after the wake-up of their display time avg %.2f ms "
                "(max %.2f), %d later than %.1f ms after it\n",
                gLatN, gLatSumMs / gLatN, gLatMinMs, gLatMaxMs, gLatHist[0], gLatHist[1], gLatHist[2], gLatHist[3], gLatHist[4], gLatHist[5],
                gWakeEndSumMs / gLatN, gWakeEndMaxMs, gOffWindow, gOnTimeMs);
    }
    if (gShortN) {
        fprintf(stderr,
                "xrmock: prediction: %d new eye sets: the refresh they land on minus the time their head poses were located for avg %.2f ms "
                "(min %.2f, max %.2f; <-1 ms %d, -1..1 %d, 1..9.3 %d, 9.3..17.7 %d, more %d); controllers (%d sets): located for %.2f ms before "
                "the landing, the xrLocateSpace call %.2f ms before it\n",
                gShortN, gShortSumMs / gShortN, gShortMinMs, gShortMaxMs, gShortHist[0], gShortHist[1], gShortHist[2], gShortHist[3], gShortHist[4],
                gCtlN, gCtlN ? gCtlShortSumMs / gCtlN : 0.0, gCtlN ? gCtlAgeSumMs / gCtlN : 0.0);
    }
    if (gYawN) {
        fprintf(stderr,
                "xrmock: prediction yaw error at the landing refresh (stops profile, %d sets): avg |err| %.2f deg (while turning %.2f over %d sets), "
                "overshoot max %.2f deg (%d sets past the head by more than 0.5 deg), trail max %.2f deg\n",
                gYawN, gYawAbsSum / gYawN, gYawMovingN ? gYawMovingAbsSum / gYawMovingN : 0.0, gYawMovingN, gYawOverMax, gYawOverN, gYawTrailMax);
    }
    if (gReleases) {
        fprintf(stderr, "xrmock: queue drain time at the releases: <0.5 ms %d, 0.5-1 %d, 1-2 %d, 2-4 %d, >4 %d\n", gWaitHist[0], gWaitHist[1], gWaitHist[2],
                gWaitHist[3], gWaitHist[4]);
    }
    pushState(XR_SESSION_STATE_EXITING);
    trace("endSession");
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mRequestExit(XrSession s) {
    (void)s;
    pushState(XR_SESSION_STATE_STOPPING);
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mPollEvent(XrInstance i, XrEventDataBuffer* ev) {
    (void)i;
    if (gBlipMs > 0.0 && gFocused && !gQuitSent) {
        int64_t now = nowNs();
        if (!gNextBlip) gNextBlip = now + (int64_t)(gBlipEvery * 1e9);
        if (!gRefocusAt && now >= gNextBlip) {
            pushState(XR_SESSION_STATE_VISIBLE);
            gRefocusAt = now + (int64_t)(gBlipMs * 1e6);
            gBlips++;
            trace("focus blip %.0f ms", gBlipMs);
        } else if (gRefocusAt && now >= gRefocusAt) {
            pushState(XR_SESSION_STATE_FOCUSED);
            gRefocusAt = 0;
            gNextBlip = now + (int64_t)(gBlipEvery * 1e9);
        }
    }
    if (gEventCount == 0) return XR_EVENT_UNAVAILABLE;
    int st = gEventQueue[0];
    memmove(gEventQueue, gEventQueue + 1, (size_t)(--gEventCount) * sizeof(int));
    if (st == EVENT_PROFILE_CHANGED) {
        XrEventDataInteractionProfileChanged* p = (XrEventDataInteractionProfileChanged*)ev;
        p->type = XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED;
        p->next = NULL;
        p->session = (XrSession)(uintptr_t)2;
        trace("event interactionProfileChanged");
        return XR_SUCCESS;
    }
    if (st == XR_SESSION_STATE_FOCUSED && !gFocused) {
        gFocused = 1;
        if (gProfile && !gProfileSent) {
            gProfileSent = 1;
            if (gEventCount < 16) gEventQueue[gEventCount++] = EVENT_PROFILE_CHANGED;
        }
    }
    XrEventDataSessionStateChanged* e = (XrEventDataSessionStateChanged*)ev;
    e->type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
    e->next = NULL;
    e->session = (XrSession)(uintptr_t)2;
    e->state = (XrSessionState)st;
    e->time = 0;
    trace("event sessionState=%d", st);
    return XR_SUCCESS;
}

static int gWaits = 0;
// MOCK_XR_EARLY=3: whether the pending frame's GPU work has written its first eye image (the
// image's signature changed since the app waited for it; the app blits the eye images last).
static int pendingGpuDone(void) {
    if (!gGfx || !gGfx->image_sig) return 0;
    for (int k = 0; k < gChainCount; k++) {
        Chain* c = &gChains[k];
        if (!c->attach || c->waited <= 0 || !c->waitSig) continue;
        int idx = ((c->next - c->acquired) % c->count + c->count) % c->count;
        return gGfx->image_sig(c->images, idx) != c->waitSig;
    }
    return 0;
}
// MOCK_XR_EARLY=3, SteamVR's hand-out (sessions of 2026-10-14, framesToThrottle 1): the wake-up index of
// the refresh a call made now gets; *block: sleep until its wake-up. Kinds (summary, windows): 0 the
// refresh after the last one, 1 the pending frame's refresh again, 2 the running one, 3 one past the
// refresh after the last frame's display time (that frame landed late).
static int64_t handOut3(int64_t now, int* block, int* kind) {
    const int64_t step = 1 + gThrottle;
    const int64_t run = (now - gT0) / gPeriod / step * step;
    int64_t target;
    *block = 0;
    if (gPendingFrame && gPendWake >= 0) {
        if (gPendWake > run) {
            target = gPendWake;
            *kind = 1;
        } else if (gPendWake == run) {
            const int done = pendingGpuDone();
            target = done ? run : run + step;
            *kind = done ? 1 : 0;
        } else {
            target = gPendLate ? run + step : run;
            *kind = gPendLate ? 0 : 2;
        }
    } else if (gLastLandWake >= 0) {
        target = gLastLandWake + step;
        *kind = gLastLandedLate ? 3 : 0;
        if (target <= run) {
            target = run;
            *kind = 2;
        } else if (!gLastLandedLate) {
            *block = 1;
        }
    } else {
        target = run + step;
        *kind = 0;
        *block = 1;
    }
    return target;
}
static void windowLine(int64_t now) {
    if (!gSessionStart) return;
    int idx = (int)((now - gSessionStart) / 10000000000ll);
    if (idx == gWinIdx) return;
    if (gWinIdx >= 0) {
        fprintf(stderr,
                "xrmock: window %4d-%4d s: new eye sets held 1: %d, 2: %d, 3: %d, 4: %d, 5+: %d, never shown %d; landed on their display "
                "time %d, later %d; hand-out next %d, again %d, running %d, skip %d, blocked %d\n",
                gWinIdx * 10, gWinIdx * 10 + 10, gWinHeld[1], gWinHeld[2], gWinHeld[3], gWinHeld[4], gWinHeld[5], gWinUnseen, gWinOnTime,
                gWinLater, gWinHand[0], gWinHand[1], gWinHand[2], gWinHand[3], gWinHand[4]);
    }
    memset(gWinHeld, 0, sizeof(gWinHeld));
    memset(gWinHand, 0, sizeof(gWinHand));
    gWinUnseen = gWinOnTime = gWinLater = 0;
    gWinIdx = idx;
}
static XrResult XRAPI_CALL mWaitFrame(XrSession s, const XrFrameWaitInfo* wi, XrFrameState* fs) {
    (void)s;
    (void)wi;
    int64_t now = nowNs();
    windowLine(now);
    // MOCK_XR_PHASE_MS: move the grid (gT0 forward by less than a slot) so that this call comes the
    // given time after a wake-up.
    if (gPhaseMs >= 0.0 && gT0 && gPhaseNext < gPhaseCount && now - gSessionStart >= (int64_t)(gPhaseAt[gPhaseNext] * 1e9)) {
        const int64_t step = (1 + gThrottle) * gPeriod;
        const int64_t cur = (now - gT0) % step;
        const int64_t want = (int64_t)(gPhaseMs * 1e6) % step;
        const int64_t delta = ((cur - want) % step + step) % step;
        gT0 += delta;
        gPhaseNext++;
        gPhaseShifts++;
        fprintf(stderr, "xrmock: phase: grid moved %.2f ms at %.1f s, calls now %.2f ms after a wake-up\n", (double)delta / 1e6,
                (double)(now - gSessionStart) / 1e9, gPhaseMs);
    }
    // MOCK_XR_SKIP=1: a frame loop that falls behind loses refreshes, as on a
    // headset (the next frame waits for the next refresh to come).
    // MOCK_XR_SKIP=late: a late call returns at once with the refresh that is running (SteamVR on
    // the Frame does not block a late xrWaitFrame); the display times stay in the future.
    static int skip = -1;
    if (skip < 0) {
        const char* e = getenv("MOCK_XR_SKIP");
        skip = !e ? 0 : !strcmp(e, "late") ? 2 : 1;
    }
    if (skip == 1 && gT0 && now > gT0 + (int64_t)gWaits * gPeriod + gPeriod / 2) {
        gWaits = (int)((now - gT0) / gPeriod) + 1;
    } else if (skip == 2 && gT0 && now >= gT0 + (int64_t)(gWaits + 1 + gThrottle) * gPeriod) {
        gWaits = (int)((now - gT0) / gPeriod / (1 + gThrottle) * (1 + gThrottle));
    }
    int64_t wake = gT0 + (int64_t)gWaits * gPeriod;
    if (gEarly == 3 && gT0) {
        int block = 0, kind = 0;
        const int64_t idx = handOut3(now, &block, &kind);
        wake = gT0 + idx * gPeriod;
        gWinHand[kind]++;
        if (kind == 0) gHandNext++;
        else if (kind == 1) gHandAgain++;
        else if (kind == 2) gHandRunning++;
        else gHandSkip++;
        if (block && wake > now) {
            gHandBlocked++;
            gWinHand[4]++;
            if (wake - now > 2000000) gBlockedWaits++;
            struct timespec ts = {(time_t)((wake - now) / 1000000000ll), (long)((wake - now) % 1000000000ll)};
            nanosleep(&ts, NULL);
        } else {
            gHandAtOnce++;
            if (wake > now) gEarlyHandouts++;
        }
        gWaits = (int)idx;
        gHandWake = idx;
        gHandLate = nowNs() - wake > 4000000;
    } else {
    // MOCK_XR_EARLY=2: as 1, and a call made while a frame is pending after the next refresh's
    // wake-up gets the refresh after it (SteamVR's "refresh after the running one"; whether it
    // does that always is not known).
    if (gEarly == 2 && gPendingFrame && gT0 && wake <= now) {
        while (wake <= now) { gWaits += 1 + gThrottle; wake = gT0 + (int64_t)gWaits * gPeriod; }
    }
    // MOCK_XR_EARLY=1 (SteamVR on the Frame): while a frame is begun and not yet ended, a call
    // does not block: it returns at once with the next refresh, even when that refresh's wake-up
    // is still ahead (the app then has its poses earlier than the wake-up).
    if (wake > now && gEarly && gPendingFrame) {
        gEarlyHandouts++;
    } else if (wake > now) {
        if (wake - now > 2000000) gBlockedWaits++;
        struct timespec ts = {(time_t)((wake - now) / 1000000000ll), (long)((wake - now) % 1000000000ll)};
        nanosleep(&ts, NULL);
    }
    }
    gWaitCalls++;
    fs->predictedDisplayTime = wake + gLead * gPeriod + xrOff();
    gWaitPredicted = fs->predictedDisplayTime;
    gCtlTime = 0;
    gCtlCallNs = 0;
    fs->predictedDisplayPeriod = gPeriod * (1 + gThrottle);
    fs->shouldRender = XR_TRUE;
    gWaits += 1 + gThrottle;
    if (gEndAt && nowNs() > gEndAt && !gQuitSent) {
        gQuitSent = 1;
        pushState(XR_SESSION_STATE_STOPPING);
    }
    gFrame++;
    trace("waitFrame");
    if (gStampOn && gGfx && gGfx->stamp_read) stampPoll(0);
    if (gGfx && gGfx->poll_snapshots) {
        int changed = gGfx->poll_snapshots(0);
        for (int k = 0; k < changed; k++) {
            trace("ERROR an image changed after its release: released before the GPU finished");
            gErrors++;
            gBusyReleases++;
        }
    }
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mBeginFrame(XrSession s, const XrFrameBeginInfo* bi) {
    (void)s;
    (void)bi;
    gBegunPredicted = gWaitPredicted;
    gPendingFrame = 1;
    gPendWake = gHandWake;
    gPendLate = gHandLate;
    gBegunCtlTime = gCtlTime;
    gBegunCtlCallNs = gCtlCallNs;
    trace("beginFrame");
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mEndFrame(XrSession s, const XrFrameEndInfo* ei) {
    (void)s;
    char buf[1024] = "";
    int freshLocated = -1;
    if (gBegunPredicted && ei->displayTime == gBegunPredicted) gEndTimeMatch++;
    else if (gBegunPredicted) {
        gEndTimeOff++;
        double off = fabs((double)((int64_t)ei->displayTime - (int64_t)gBegunPredicted)) / 1e6;
        if (off > gEndTimeOffWorstMs) gEndTimeOffWorstMs = off;
        if (gEndTimeOff <= 20) trace("ERROR endFrame display time %.2f ms off its frame's predicted one", off);
        gErrors++;
    }
    for (uint32_t i = 0; i < ei->layerCount; i++) {
        const XrCompositionLayerBaseHeader* l = ei->layers[i];
        char t[256];
        if (l->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            const XrCompositionLayerProjection* p = (const XrCompositionLayerProjection*)l;
            int fresh = p->viewCount == 2 && chainOf(p->views[0].subImage.swapchain)->released &&
                        chainOf(p->views[1].subImage.swapchain)->released;
            if (i == 0) {
                if (fresh) gFreshSets++;
                else gRepeatSets++;
            }
            int located = checkPoses(p, ei->displayTime, fresh);
            if (i == 0 && fresh) freshLocated = located;
            if (gStampOn && gGfx && gGfx->stamp_read && p->viewCount == 2 && i == 0) {
                for (int v = 0; v < 2; v++) {
                    Chain* ch = chainOf(p->views[v].subImage.swapchain);
                    if (located < 0) gStampUnknownPose++;
                    else if (ch->stampSlot < 0) gStampNone++;
                    else if (gStampPendingCount < STAMP_PENDING) {
                        gStampPending[gStampPendingCount].slot = ch->stampSlot;
                        gStampPending[gStampPendingCount].gen = ch->stampGen;
                        gStampPending[gStampPendingCount].eye = v;
                        gStampPending[gStampPendingCount].fresh = fresh;
                        gStampPending[gStampPendingCount].expect = locateTime(located);
                        gStampPendingCount++;
                    } else
                        gStampLost++;
                }
            }
            snprintf(t, sizeof(t), " proj[flags=%llx", (unsigned long long)p->layerFlags);
            strcat(buf, t);
            for (uint32_t v = 0; v < p->viewCount; v++) {
                const XrCompositionLayerProjectionView* pv = &p->views[v];
                int warp = 0;
                for (const XrBaseInStructure* n = (const XrBaseInStructure*)pv->next; n; n = n->next) warp |= n->type == XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB;
                snprintf(t, sizeof(t), " sc%d:%dx%d%s", chainId(pv->subImage.swapchain), pv->subImage.imageRect.extent.width,
                         pv->subImage.imageRect.extent.height, warp ? "+warp" : "");
                strcat(buf, t);
            }
            strcat(buf, "]");
        } else if (l->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
            const XrCompositionLayerQuad* q = (const XrCompositionLayerQuad*)l;
            snprintf(t, sizeof(t), " quad[sc%d:%dx%d eye=%d]", chainId(q->subImage.swapchain), q->subImage.imageRect.extent.width,
                     q->subImage.imageRect.extent.height, (int)q->eyeVisibility);
            strcat(buf, t);
        } else {
            snprintf(t, sizeof(t), " layer%d", (int)l->type);
            strcat(buf, t);
        }
    }
    // Which refresh the layer stack lands on: the compositor takes what was ended by one period
    // (MOCK_XR_LATCH_MS) before a refresh, and nothing before its display time. Fresh eye sets
    // give the cadence: how many refreshes each one stays on screen.
    int64_t landSlot = -1, wantSlot = -1;
    if (gT0 && ei->layerCount > 0) {
        static int64_t latch = -1;
        if (latch < 0) latch = getenv("MOCK_XR_LATCH_MS") ? (int64_t)(atof(getenv("MOCK_XR_LATCH_MS")) * 1e6) : gPeriod;
        int64_t want = (int64_t)ei->displayTime - xrOff() - gT0;
        wantSlot = (want + gPeriod / 2) / gPeriod;
        int64_t earliest = nowNs() + latch - gT0;
        landSlot = (earliest + gPeriod - 1) / gPeriod;
        if (landSlot < wantSlot) landSlot = wantSlot;
        if (gLandThrottled && gThrottle > 0 && landSlot > wantSlot) {
            int64_t step = 1 + gThrottle;
            landSlot = wantSlot + (landSlot - wantSlot + step - 1) / step * step;
        }
        if (landSlot > wantSlot) {
            gLateLayers++;
            trace("late: ended for refresh %lld, lands on %lld", (long long)wantSlot, (long long)landSlot);
        }
        gLayerEnds++;
        int fresh = ei->layers[0]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION && ((const XrCompositionLayerProjection*)ei->layers[0])->viewCount == 2 &&
                    chainOf(((const XrCompositionLayerProjection*)ei->layers[0])->views[0].subImage.swapchain)->released;
        if (fresh) {
            if (landSlot > wantSlot) gWinLater++;
            else gWinOnTime++;
            if (gLastFreshSlot >= 0) {
                int64_t held = landSlot - gLastFreshSlot;
                if (held <= 0) {
                    gUnseen++;
                    gWinUnseen++;
                } else {
                    gHeld[held > 5 ? 5 : held]++;
                    gWinHeld[held > 5 ? 5 : held]++;
                    // A run: consecutive fresh sets each held 2 refreshes.
                    if (held == 2) {
                        if (++gRun2 > gRun2Max) gRun2Max = gRun2;
                    } else gRun2 = 0;
                    if (gLastHeld >= 0 && ((gLastHeld == 1 && held == 3) || (gLastHeld == 3 && held == 1))) gFlips++;
                    gLastHeld = held;
                }
            }
            gLastFreshSlot = landSlot;
            if (freshLocated >= 0 && locateCallNs(freshLocated)) {
                double lat = (double)(gT0 + landSlot * gPeriod - locateCallNs(freshLocated)) / 1e6;
                gLatSumMs += lat;
                if (lat > gLatMaxMs) gLatMaxMs = lat;
                if (lat < gLatMinMs) gLatMinMs = lat;
                gLatN++;
                int b = lat < 20 ? 0 : lat < 30 ? 1 : lat < 40 ? 2 : lat < 50 ? 3 : lat < 60 ? 4 : 5;
                gLatHist[b]++;
                const int64_t landNs = gT0 + landSlot * gPeriod;
                double shortMs = (double)(landNs - ((int64_t)locateTime(freshLocated) - xrOff())) / 1e6;
                gShortSumMs += shortMs;
                if (shortMs < gShortMinMs) gShortMinMs = shortMs;
                if (shortMs > gShortMaxMs) gShortMaxMs = shortMs;
                gShortHist[shortMs < -1 ? 0 : shortMs < 1 ? 1 : shortMs < 9.3 ? 2 : shortMs < 17.7 ? 3 : 4]++;
                gShortN++;
                if (gBegunCtlTime) {
                    gCtlShortSumMs += (double)(landNs - ((int64_t)gBegunCtlTime - xrOff())) / 1e6;
                    gCtlAgeSumMs += (double)(landNs - gBegunCtlCallNs) / 1e6;
                    gCtlN++;
                }
                if (gHeadStops) {
                    const XrQuaternionf* q = &((const XrCompositionLayerProjection*)ei->layers[0])->views[0].pose.orientation;
                    double shown = 2.0 * atan2(q->y, q->w);
                    double callRate = 0.0;
                    stopsYaw((double)(locateCallNs(freshLocated) - gSessionStart) / 1e9, &callRate);
                    double moving = 0.0;
                    double truth = stopsYaw((double)(landNs - gSessionStart) / 1e9, &moving);
                    double err = (shown - truth) * 180.0 / 3.14159265;
                    double dir = callRate > 0 ? 1.0 : callRate < 0 ? -1.0 : 0.0;
                    gYawAbsSum += fabs(err);
                    gYawN++;
                    if (moving != 0.0) {
                        gYawMovingAbsSum += fabs(err);
                        gYawMovingN++;
                    }
                    if (dir != 0.0 && err * dir > gYawOverMax) gYawOverMax = err * dir;
                    if (dir != 0.0 && err * dir > 0.5) gYawOverN++;
                    if (dir != 0.0 && -err * dir > gYawTrailMax) gYawTrailMax = -err * dir;
                }
                double wakeEnd = (double)(nowNs() - ((int64_t)ei->displayTime - xrOff() - gLead * gPeriod)) / 1e6;
                gWakeEndSumMs += wakeEnd;
                if (wakeEnd > gWakeEndMaxMs) gWakeEndMaxMs = wakeEnd;
                if (wakeEnd > gOnTimeMs) gOffWindow++;
            }
        }
    }
    trace("endFrame layers=%u%s d=%lld at=%lld", ei->layerCount, buf, (long long)wantSlot, (long long)landSlot);
    gEnds++;
    if (ei->layerCount == 0) gEmptyEnds++;
    else if (ei->layers[0]->type == XR_TYPE_COMPOSITION_LAYER_QUAD) gQuadEnds++;
    for (int k = 0; k < gChainCount; k++) gChains[k].released = 0;
    gPendingFrame = 0;
    if (gT0) {
        // MOCK_XR_EARLY=3: where this frame lands (an empty one: on its display time).
        const int64_t want = ((int64_t)ei->displayTime - xrOff() - gT0 + gPeriod / 2) / gPeriod;
        const int64_t land = landSlot >= 0 ? landSlot : want;
        gLastLandWake = land - gLead;
        gLastLandedLate = land > want;
    }
    return XR_SUCCESS;
}
// The head turns slowly from side to side, so views located for different
// times differ.  The last locates are kept for the pose check in mEndFrame.
#define LOCATES 32
static struct {
    XrTime time;
    XrPosef pose[2];
    int64_t callNs;
} gLocates[LOCATES];
static int gLocateNext = 0;
static XrResult XRAPI_CALL mLocateViews(XrSession s, const XrViewLocateInfo* li, XrViewState* vs, uint32_t cap, uint32_t* count, XrView* v) {
    (void)s;
    vs->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_TRACKED_BIT |
                         XR_VIEW_STATE_POSITION_TRACKED_BIT;
    *count = 2;
    double t = (double)(li->displayTime - xrOff() - gSessionStart) / 1e9;
    float yaw = (float)(gHeadAmp * sin(t * 2.0 * 3.14159265 * gHeadHz));
    if (gHeadStops) {
        // Predicted as a runtime does: from the pose and speed at the call, at constant speed.
        double now = (double)(nowNs() - gSessionStart) / 1e9, rate = 0.0;
        yaw = (float)(stopsYaw(now, &rate) + rate * (t - now));
    }
    float rise = (float)(t * 1e-4);  // 0.1 mm a second: the sway repeats, the pose never does
    float c = cosf(yaw), sn = sinf(yaw);
    for (uint32_t e = 0; e < 2 && e < cap; e++) {
        float x = e ? 0.032f : -0.032f;
        v[e].pose.orientation = (XrQuaternionf){0, sinf(yaw * 0.5f), 0, cosf(yaw * 0.5f)};
        v[e].pose.position = (XrVector3f){x * c, 1.6f + rise, -x * sn};
        v[e].fov = (XrFovf){-0.84f, 0.84f, 0.84f, -0.84f};
        gLocates[gLocateNext].pose[e] = v[e].pose;
    }
    gLocates[gLocateNext].time = li->displayTime;
    gLocates[gLocateNext].callNs = nowNs();
    gLocateNext = (gLocateNext + 1) % LOCATES;
    return XR_SUCCESS;
}

static int samePose(const XrPosef* a, const XrPosef* b) { return !memcmp(a, b, sizeof(XrPosef)); }

// Both eyes of a projection layer: from one locate, and for which time.
static XrTime locateTime(int i) { return gLocates[i].time; }
static int64_t locateCallNs(int i) { return gLocates[i].callNs; }
static int checkPoses(const XrCompositionLayerProjection* p, XrTime displayTime, int fresh) {
    if (p->viewCount != 2) return -1;
    gPoseSets++;
    int left = -1, both = -1;
    for (int i = 0; i < LOCATES; i++) {
        if (!gLocates[i].time) continue;
        if (samePose(&gLocates[i].pose[0], &p->views[0].pose)) {
            left = i;
            if (samePose(&gLocates[i].pose[1], &p->views[1].pose)) both = i;
        }
    }
    if (both < 0) {
        if (left >= 0) {
            gPoseSplit++;
            trace("ERROR projection layer eyes from different locates");
            gErrors++;
        } else {
            gPoseUnknown++;
        }
        return -1;
    }
    XrTime t = gLocates[both].time;
    if (t == displayTime) gPoseSameTime++;
    else if (t < displayTime) gPoseEarlier++;
    else gPoseLater++;
    if (fresh) {
        if (t == displayTime) gFreshSame++;
        else if (t < displayTime) gFreshEarlier++;
        else gFreshLater++;
    }
    long k = lround((double)(displayTime - t) / (double)gPeriod);
    gPoseAge[k < 0 ? 0 : k > 3 ? 5 : k + 1]++;
    return both;
}
static XrResult XRAPI_CALL mStringToPath(XrInstance i, const char* s, XrPath* p) {
    (void)i;
    for (int k = 0; k < gPathCount; k++)
        if (!strcmp(gPathStr[k], s)) {
            *p = gPaths[k];
            return XR_SUCCESS;
        }
    strncpy(gPathStr[gPathCount], s, 127);
    gPaths[gPathCount] = (XrPath)(5000 + gPathCount);
    *p = gPaths[gPathCount++];
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mPathToString(XrInstance i, XrPath p, uint32_t cap, uint32_t* len, char* buf) {
    (void)i;
    for (int k = 0; k < gPathCount; k++)
        if (gPaths[k] == p) {
            strncpy(buf, gPathStr[k], cap);
            *len = (uint32_t)strlen(gPathStr[k]) + 1;
            return XR_SUCCESS;
        }
    return XR_ERROR_PATH_INVALID;
}
static XrResult XRAPI_CALL mCreateActionSet(XrInstance i, const XrActionSetCreateInfo* ci, XrActionSet* o) {
    (void)i;
    (void)ci;
    *o = (XrActionSet)(uintptr_t)3;
    return XR_SUCCESS;
}
static int gActions = 0;
static XrResult XRAPI_CALL mCreateAction(XrActionSet s, const XrActionCreateInfo* ci, XrAction* o) {
    (void)s;
    (void)ci;
    *o = (XrAction)(uintptr_t)(200 + gActions++);
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mPathToString(XrInstance i, XrPath p, uint32_t cap, uint32_t* len, char* buf);
static XrResult XRAPI_CALL mSuggest(XrInstance i, const XrInteractionProfileSuggestedBinding* b) {
    (void)i;
    char profile[128] = "";
    uint32_t len = 0;
    if (gProfile && mPathToString(i, b->interactionProfile, sizeof(profile), &len, profile) == XR_SUCCESS && !strcmp(profile, gProfile)) {
        trace("suggestBindings %s: %u bindings accepted", profile, b->countSuggestedBindings);
        for (uint32_t k = 0; k < b->countSuggestedBindings; k++) {
            char path[128] = "";
            if (mPathToString(i, b->suggestedBindings[k].binding, sizeof(path), &len, path) == XR_SUCCESS) trace("  binding %s", path);
        }
    }
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mAttach(XrSession s, const XrSessionActionSetsAttachInfo* a) {
    (void)s;
    (void)a;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mSync(XrSession s, const XrActionsSyncInfo* a) {
    (void)s;
    (void)a;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mGetBool(XrSession s, const XrActionStateGetInfo* g, XrActionStateBoolean* o) {
    (void)s;
    (void)g;
    o->isActive = XR_FALSE;
    o->currentState = XR_FALSE;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mGetFloat(XrSession s, const XrActionStateGetInfo* g, XrActionStateFloat* o) {
    (void)s;
    (void)g;
    o->isActive = XR_FALSE;
    o->currentState = 0;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mGetVec2(XrSession s, const XrActionStateGetInfo* g, XrActionStateVector2f* o) {
    (void)s;
    (void)g;
    o->isActive = XR_FALSE;
    o->currentState.x = o->currentState.y = 0;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mHaptic(XrSession s, const XrHapticActionInfo* h, const XrHapticBaseHeader* b) {
    (void)s;
    (void)h;
    (void)b;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mHapticStop(XrSession s, const XrHapticActionInfo* h) {
    (void)s;
    (void)h;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mGetProfile(XrSession s, XrPath p, XrInteractionProfileState* o) {
    (void)s;
    (void)p;
    o->interactionProfile = XR_NULL_PATH;
    if (gProfile && gProfileSent) mStringToPath((XrInstance)(uintptr_t)1, gProfile, &o->interactionProfile);
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mReqRefresh(XrSession s, float hz) {
    (void)s;
    // Honoured before the session begins (the frame clock is fixed after that).
    // Honoured before the session begins (the frame clock is fixed after that), unless
    // MOCK_XR_HZ fixes the rate.
    const char* fixed = getenv("MOCK_XR_HZ");
    if (!gT0 && !fixed && (hz == 72.0f || hz == 90.0f || hz == 120.0f)) gPeriod = (int64_t)(1e9 / hz);
    trace("requestRefreshRate %.0f%s", hz, fixed ? " (MOCK_XR_HZ fixes the rate)" : gT0 ? " (session running: ignored)" : "");
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mEnumRefresh(XrSession s, uint32_t cap, uint32_t* count, float* r) {
    (void)s;
    // MOCK_XR_RATES=<hz>,<hz>... (default 72,90,120); SteamVR on the Frame offered only 90 in H1.
    float rates[8] = {72, 90, 120};
    uint32_t n = 3;
    if (getenv("MOCK_XR_RATES")) {
        n = 0;
        for (const char* q = getenv("MOCK_XR_RATES"); q && *q && n < 8;) {
            rates[n++] = (float)atof(q);
            q = strchr(q, ',');
            if (q) q++;
        }
    }
    *count = n;
    if (cap)
        for (uint32_t i = 0; i < n && i < cap; i++) r[i] = rates[i];
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mGetRefresh(XrSession s, float* hz) {
    (void)s;
    *hz = (float)(1e9 / (double)gPeriod);
    return XR_SUCCESS;
}
struct timespec;
static XrResult XRAPI_CALL mTimespecToTime(XrInstance i, const struct timespec* ts, XrTime* t) {
    (void)i;
    *t = (XrTime)ts->tv_sec * 1000000000ll + ts->tv_nsec + xrOff();
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mTimeToTimespec(XrInstance i, XrTime t, struct timespec* ts) {
    (void)i;
    int64_t ns = (int64_t)t - xrOff();
    ts->tv_sec = (time_t)(ns / 1000000000ll);
    ts->tv_nsec = (long)(ns % 1000000000ll);
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mPerfLevel(XrSession s, XrPerfSettingsDomainEXT d, XrPerfSettingsLevelEXT l) {
    (void)s;
    (void)d;
    (void)l;
    return XR_SUCCESS;
}

typedef struct {
    const char* name;
    PFN_xrVoidFunction fn;
} Entry;
#define E(n, f) {n, (PFN_xrVoidFunction)f}
static const Entry kTable[] = {
    E("xrEnumerateInstanceExtensionProperties", mEnumExt), E("xrCreateInstance", mCreateInstance), E("xrDestroyInstance", mDestroyInstance), E("xrGetInstanceProperties", mGetInstanceProps), E("xrResultToString", mResultToString),
    E("xrGetSystem", mGetSystem), E("xrGetSystemProperties", mGetSystemProps), E("xrCreateSession", mCreateSession),
    E("xrDestroySession", mDestroySession), E("xrCreateReferenceSpace", mCreateRefSpace), E("xrCreateActionSpace", mCreateActionSpace),
    E("xrDestroySpace", mDestroySpace), E("xrEnumerateEnvironmentBlendModes", mEnumBlend), E("xrEnumerateReferenceSpaces", mEnumRefSpaces), E("xrDestroyActionSet", mDestroyActionSet), E("xrLocateSpace", mLocateSpace), E("xrEnumerateViewConfigurationViews", mEnumViewConfigViews),
    E("xrEnumerateSwapchainFormats", mEnumFormats), E("xrCreateSwapchain", mCreateSwapchain), E("xrDestroySwapchain", mDestroySwapchain), E("xrEnumerateSwapchainImages", mEnumSwapImages),
    E("xrAcquireSwapchainImage", mAcquire), E("xrWaitSwapchainImage", mWaitImage), E("xrReleaseSwapchainImage", mRelease),
    E("xrBeginSession", mBeginSession), E("xrEndSession", mEndSession), E("xrRequestExitSession", mRequestExit), E("xrPollEvent", mPollEvent),
    E("xrWaitFrame", mWaitFrame), E("xrBeginFrame", mBeginFrame), E("xrEndFrame", mEndFrame), E("xrLocateViews", mLocateViews),
    E("xrStringToPath", mStringToPath), E("xrPathToString", mPathToString), E("xrCreateActionSet", mCreateActionSet),
    E("xrCreateAction", mCreateAction), E("xrSuggestInteractionProfileBindings", mSuggest), E("xrAttachSessionActionSets", mAttach),
    E("xrSyncActions", mSync), E("xrGetActionStateBoolean", mGetBool), E("xrGetActionStateFloat", mGetFloat), E("xrGetActionStateVector2f", mGetVec2),
    E("xrApplyHapticFeedback", mHaptic), E("xrStopHapticFeedback", mHapticStop), E("xrGetCurrentInteractionProfile", mGetProfile),
    E("xrRequestDisplayRefreshRateFB", mReqRefresh), E("xrEnumerateDisplayRefreshRatesFB", mEnumRefresh), E("xrGetDisplayRefreshRateFB", mGetRefresh),
    E("xrPerfSettingsSetPerformanceLevelEXT", mPerfLevel),
    E("xrConvertTimespecTimeToTimeKHR", mTimespecToTime), E("xrConvertTimeToTimespecTimeKHR", mTimeToTimespec),
};

static void init(void) {
    static int done;
    if (done) return;
    done = 1;
    const char* tp = getenv("MOCK_XR_TRACE");
    if (tp) gTrace = fopen(tp, "w");
    const char* hz = getenv("MOCK_XR_HZ");
    if (hz) gPeriod = (int64_t)(1e9 / atof(hz));
    const char* eye = getenv("MOCK_XR_EYE");
    if (eye) sscanf(eye, "%dx%d", &gEyeW, &gEyeH);
    const char* off = getenv("MOCK_XR_TIME_OFFSET_S");
    if (off) gTimeOffset = (int64_t)(atof(off) * 1e9);
    const char* blip = getenv("MOCK_XR_FOCUS_BLIP");
    if (blip) {
        gBlipMs = atof(blip);
        const char* at = strchr(blip, '@');
        if (at) gBlipEvery = atof(at + 1);
    }
    gStampOn = getenv("MOCK_XR_STAMP") != NULL;
    if (getenv("MOCK_XR_LEAD")) gLead = atoi(getenv("MOCK_XR_LEAD"));
    gEarly = getenv("MOCK_XR_EARLY") ? atoi(getenv("MOCK_XR_EARLY")) : 0;
    const char* phase = getenv("MOCK_XR_PHASE_MS");
    if (phase && *phase) {
        gPhaseMs = atof(phase);
        const char* at = strchr(phase, '@');
        for (const char* q = at ? at + 1 : NULL; q && *q && gPhaseCount < 16;) {
            gPhaseAt[gPhaseCount++] = atof(q);
            q = strchr(q, ',');
            if (q) q++;
        }
        if (!gPhaseCount) gPhaseAt[gPhaseCount++] = 5.0;
    }
    gClockRaw = getenv("MOCK_XR_CLOCK") && !strcmp(getenv("MOCK_XR_CLOCK"), "raw");
    if (getenv("MOCK_XR_THROTTLE")) gThrottle = atoi(getenv("MOCK_XR_THROTTLE"));
    if (getenv("MOCK_XR_ONTIME_MS")) gOnTimeMs = atof(getenv("MOCK_XR_ONTIME_MS"));
    if (getenv("MOCK_XR_HEAD_DEG")) gHeadAmp = atof(getenv("MOCK_XR_HEAD_DEG")) * 3.14159265 / 180.0;
    if (getenv("MOCK_XR_HEAD_DPS") && gHeadAmp > 0.0)
        gHeadHz = atof(getenv("MOCK_XR_HEAD_DPS")) * 3.14159265 / 180.0 / (2.0 * 3.14159265 * gHeadAmp);
    if (getenv("MOCK_XR_HEAD_PROFILE") && !strcmp(getenv("MOCK_XR_HEAD_PROFILE"), "stops")) {
        gHeadStops = 1;
        gHeadRate = (getenv("MOCK_XR_HEAD_DPS") ? atof(getenv("MOCK_XR_HEAD_DPS")) : 54.0) * 3.14159265 / 180.0;
    }
    gLandThrottled = getenv("MOCK_XR_LAND") && !strcmp(getenv("MOCK_XR_LAND"), "throttled");
    gProfile = getenv("MOCK_XR_PROFILE");
    if (gProfile && !*gProfile) gProfile = NULL;
    const char* list = getenv("MOCK_XR_EXTENSIONS");
    if (list) {
        char* copy = strdup(list);
        for (char* tok = strtok(copy, ","); tok && gExtCount < 64; tok = strtok(NULL, ",")) strcpy(gExtNames[gExtCount++], tok);
        free(copy);
    } else {
        const char* exts[] = {"XR_KHR_opengl_es_enable", "XR_MNDX_egl_enable", "XR_KHR_vulkan_enable2", "XR_KHR_vulkan_enable", "XR_FB_display_refresh_rate",
                              "XR_EXT_performance_settings", "XR_VALVE_frame_controller_interaction", "XR_KHR_composition_layer_depth", "XR_EXT_eye_gaze_interaction",
                              "XR_KHR_convert_timespec_time"};
        for (unsigned i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) strcpy(gExtNames[gExtCount++], exts[i]);
        if (getenv("MOCK_XR_SPACEWARP")) strcpy(gExtNames[gExtCount++], "XR_FB_space_warp");
    }
}

static XRAPI_ATTR XrResult XRAPI_CALL mockGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* fn) {
    (void)instance;
    init();
    for (unsigned i = 0; i < sizeof(kTable) / sizeof(kTable[0]); i++) {
        if (!strcmp(kTable[i].name, name)) {
            *fn = kTable[i].fn;
            return XR_SUCCESS;
        }
    }
    // The graphics extension's own functions: the app asks for them before
    // the session exists, so any table may answer.
    for (unsigned t = 0; t < sizeof(kGfxTables) / sizeof(kGfxTables[0]); t++) {
        PFN_xrVoidFunction f = kGfxTables[t]->get_proc(name);
        if (f) {
            *fn = f;
            return XR_SUCCESS;
        }
    }
    *fn = NULL;
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

__attribute__((visibility("default"))) XrResult XRAPI_CALL xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo* li,
                                                                                            XrNegotiateRuntimeRequest* rr) {
    (void)li;
    init();
    rr->runtimeInterfaceVersion = 1;
    rr->runtimeApiVersion = XR_CURRENT_API_VERSION;
    rr->getInstanceProcAddr = mockGetInstanceProcAddr;
    return XR_SUCCESS;
}
