#include "OpenXrHooks.h"

#include <windows.h>
#include <psapi.h>

#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace PrimedGun::Hook {
void Log(std::wstring_view message);
}

namespace PrimedGun::Hook::OpenXrHooks {
namespace {

using PFN_xrGetInstanceProcAddr = XrResult(XRAPI_PTR*)(XrInstance, const char*, PFN_xrVoidFunction*);
using PFN_GetProcAddress = FARPROC(WINAPI*)(HMODULE, LPCSTR);

PFN_xrGetInstanceProcAddr g_realGetInstanceProcAddr = nullptr;
PFN_xrNegotiateLoaderRuntimeInterface g_realNegotiateLoaderRuntimeInterface = nullptr;
PFN_GetProcAddress g_realGetProcAddress = nullptr;
PFN_xrStringToPath g_realStringToPath = nullptr;
PFN_xrCreateAction g_realCreateAction = nullptr;
PFN_xrCreateActionSpace g_realCreateActionSpace = nullptr;
PFN_xrGetActionStateBoolean g_realGetActionStateBoolean = nullptr;
PFN_xrGetActionStateFloat g_realGetActionStateFloat = nullptr;
PFN_xrGetActionStateVector2f g_realGetActionStateVector2f = nullptr;
PFN_xrLocateViews g_realLocateViews = nullptr;
PFN_xrLocateSpace g_realLocateSpace = nullptr;

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
    }
    return result;
}

XrResult XRAPI_PTR Hook_xrCreateActionSpace(XrSession session, const XrActionSpaceCreateInfo* createInfo, XrSpace* space) {
    const XrResult result = g_realCreateActionSpace(session, createInfo, space);
    if (XR_SUCCEEDED(result) && createInfo && space) {
        std::lock_guard<std::mutex> guard(g_mutex);
        const auto actionIt = g_actions.find(createInfo->action);
        const auto pathIt = g_paths.find(createInfo->subactionPath);
        if (actionIt != g_actions.end() && pathIt != g_paths.end() && actionIt->second == "aim_pose") {
            g_spaces[*space] = actionIt->second + "|" + pathIt->second;
        }
    }
    return result;
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
        if (pose && action == "trigger_click" && stateOut->isActive && stateOut->currentState) {
            pose->linearVelocityMetersPerSecond.x = 1.0f;
            MarkOpenXrActive(state);
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

XrResult XRAPI_PTR Hook_xrLocateViews(XrSession session, const XrViewLocateInfo* viewLocateInfo,
                                      XrViewState* viewState, uint32_t viewCapacityInput,
                                      uint32_t* viewCountOutput, XrView* views) {
    const XrResult result = g_realLocateViews(session, viewLocateInfo, viewState, viewCapacityInput, viewCountOutput, views);
    if (XR_SUCCEEDED(result) && views && viewCountOutput && *viewCountOutput > 0 && viewState &&
        IsPoseValid(viewState->viewStateFlags)) {
        SharedState* state = g_sharedState.load();
        if (state) {
            const uint32_t count = std::min<uint32_t>(*viewCountOutput, viewCapacityInput);
            XrPosef pose = views[0].pose;
            if (count >= 2) {
                pose.position.x = (views[0].pose.position.x + views[1].pose.position.x) * 0.5f;
                pose.position.y = (views[0].pose.position.y + views[1].pose.position.y) * 0.5f;
                pose.position.z = (views[0].pose.position.z + views[1].pose.position.z) * 0.5f;
            }
            CopyPose(state->hmdPose, pose);
            MarkOpenXrActive(state);
        }
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
        if (state && tag.find("aim_pose") != std::string::npos) {
            if (tag.find("/user/hand/left") != std::string::npos)
                CopyPose(state->leftHandPose, location->pose);
            else if (tag.find("/user/hand/right") != std::string::npos)
                CopyPose(state->rightHandPose, location->pose);
            MarkOpenXrActive(state);
        }
    }
    return result;
}

PFN_xrVoidFunction WrapProc(const char* name, PFN_xrVoidFunction proc) {
    if (!name || !proc)
        return proc;
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
    Log(L"OpenXrHooks shutdown.");
}

} // namespace PrimedGun::Hook::OpenXrHooks
