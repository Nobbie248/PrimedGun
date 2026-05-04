#pragma once

#include <cstdint>

namespace PrimedGun {

inline constexpr wchar_t SharedMemoryName[] = L"Local\\PrimedGunSharedState";
inline constexpr wchar_t SharedMutexName[] = L"Local\\PrimedGunSharedStateMutex";
inline constexpr uint32_t SharedStateMagic = 0x50475652; // PGVR
inline constexpr uint32_t SharedStateVersion = 4;
inline constexpr uint32_t MaxGamePatches = 128;
inline constexpr uint32_t HookStatusDllAlive = 1u << 0;
inline constexpr uint32_t HookStatusOpenXrInstalled = 1u << 1;
inline constexpr uint32_t HookStatusOpenXrRuntimeInstalled = 1u << 2;
inline constexpr uint32_t HookStatusOpenXrGetProcReady = 1u << 3;

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Quat {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

struct PoseState {
    Vec3 positionMeters{};
    Quat orientation{};
    Vec3 linearVelocityMetersPerSecond{};
    Vec3 angularVelocityRadiansPerSecond{};
};

struct CenterEyeViewport {
    uint32_t enabled = 1;
    float x = 0.0f;
    float y = 0.0f;
    float width = 1.0f;
    float height = 1.0f;
};

struct SettingsState {
    uint32_t enableCenterEyeViewport = 1;
    uint32_t enableGraphicsHooks = 1;
    uint32_t enableVrApiHooks = 1;
    float worldScale = 1.0f;
    CenterEyeViewport centerEyeViewport{};
};

struct GameState {
    uint32_t gameIdHash = 0;
    uint32_t inGame = 0;
    uint32_t inMenu = 0;
    uint32_t frameIndex = 0;
};

struct GamePatch {
    uint32_t enabled = 0;
    uint32_t address = 0;
    uint32_t value = 0;
    uint32_t requireOriginal = 0;
    uint32_t original = 0;
    uint32_t applied = 0;
    uint32_t lastSeen = 0;
};

struct PatchState {
    uint32_t generation = 0;
    uint32_t count = 0;
    GamePatch patches[MaxGamePatches]{};
};

struct SharedState {
    uint32_t magic = SharedStateMagic;
    uint32_t version = SharedStateVersion;
    uint64_t appHeartbeat = 0;
    uint64_t hookHeartbeat = 0;
    uint64_t trackingGeneration = 0;
    uint32_t trackingSource = 0; // 0 = none/app legacy, 1 = DolphinXR OpenXR, 2 = DolphinXR HMD only
    uint32_t trackingRuntimeActive = 0;
    uint32_t hookStatusFlags = 0;
    uint32_t openxrModuleFlags = 0;
    uint64_t openxrInstallAttempts = 0;
    PoseState hmdPose{};
    PoseState leftHandPose{};
    PoseState rightHandPose{};
    SettingsState settings{};
    GameState game{};
    PatchState patch{};
};

} // namespace PrimedGun
