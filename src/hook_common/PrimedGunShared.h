#pragma once

#include <cstdint>

namespace PrimedGun {

inline constexpr wchar_t SharedMemoryName[] = L"Local\\PrimedGunSharedState";
inline constexpr wchar_t SharedMutexName[] = L"Local\\PrimedGunSharedStateMutex";
inline constexpr uint32_t SharedStateMagic = 0x50475652; // PGVR
inline constexpr uint32_t SharedStateVersion = 11;
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
    uint32_t buttons = 0;
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
    uint32_t showAlignmentPrompt = 0;
    uint32_t vrMenuVisible = 0;
    uint32_t vrMenuGeneration = 0;
    uint32_t vrMenuSelectedIndex = 0;
    uint32_t vrMenuItemCount = 0;
    uint32_t vrMenuPointerActive = 0;
    uint32_t vrMenuSavedNotice = 0;
    float vrMenuPointerX = 0.5f;
    float vrMenuPointerY = 0.5f;
    float worldScale = 1.0f;
    uint32_t useRightHand = 1;
    uint32_t requireTrigger = 0;
    float triggerThreshold = 0.5f;
    float offsetX = 0.0f;
    float offsetY = 0.0f;
    float offsetZ = 0.0f;
    float rotOffsetX = 0.0f;
    float rotOffsetY = 0.0f;
    float rotOffsetZ = 0.0f;
    uint32_t gunTargetingEnabled = 1;
    float gunTargetingDistance = 60.0f;
    float gunTargetingRadius = 2.5f;
    uint32_t autoDolphinXrControls = 1;
    uint32_t dolphin60FpsCap = 1;
    uint32_t xrDpadEnabled = 1;
    float xrDpadHeadRadius = 0.18f;
    float xrDpadHeadYBelow = 0.14f;
    float xrDpadDeadzone = 0.45f;
    uint32_t directionalMovementEnabled = 1;
    uint32_t directionalMovementUseRightStick = 0;
    float directionalMovementDeadzone = 0.25f;
    float directionalMovementSpeed = 14.0f;
    float directionalMovementAccel = 45.0f;
    float directionalMovementAirAccel = 8.0f;
    float viewHeightHomeMeters = 1.6374f;
    uint32_t viewHeightGeneration = 0;
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
