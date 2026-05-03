#include "GameTimingHooks.h"

#include "PrimedGunShared.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace PrimedGun::Hook {
void Log(std::wstring_view message);
}

namespace PrimedGun::Hook::GameTimingHooks {
namespace {

namespace fs = std::filesystem;

constexpr uint32_t kMem1Start = 0x80000000u;
constexpr uint32_t kMem1Size = 0x02000000u;
constexpr uint32_t kPlayerOrbitStart = 0x8017B2E0u;
constexpr uint32_t kPlayerOrbitEnd = 0x8017FB84u;
constexpr uint32_t kFirstPersonCameraStart = 0x8000E3D0u;
constexpr uint32_t kFirstPersonCameraEnd = 0x8000FA80u;
constexpr uint32_t kPpcNop = 0x60000000u;
constexpr uint32_t kStateManager = 0x8045A1A8u;
constexpr uint32_t kPlayerOffset = 0x84Cu;
constexpr uint32_t kGunTargetHookScratch = 0x817FE400u;
constexpr uint32_t kFinalInputOffset = 0xB54u;
constexpr uint32_t kOrbitStateOffset = 0x304u;
constexpr uint32_t kFirstPersonPitchOffset = 0x3ECu;
constexpr uint32_t kOrbitStateGrapple = 5u;

std::atomic<bool> g_installed = false;
uintptr_t g_memBase = 0;
uint64_t g_suppressionCalls = 0;
uint64_t g_renderSuppressChecks = 0;
uint64_t g_notLockedLogs = 0;
uint64_t g_lastResolveTick = 0;
uint64_t g_lockLatchUntilTick = 0;
bool g_dumpedPlayerOrbitCode = false;
bool g_dumpedFirstPersonCameraCode = false;
uint32_t g_lastOrbitState = 0xffffffffu;
uint32_t g_lastPatchGeneration = 0;
uint32_t g_lastPatchCount = 0;

struct PpcPatch {
    uint32_t address;
    uint32_t original;
    const wchar_t* description;
    bool applied;
    bool loggedWaiting;
};

PpcPatch g_orbitPatches[] = {
    // Replace target.y - camera.y with zero in CFirstPersonCamera::UpdateTransform.
    {0x8000E7B0u, 0xEC42F028u, L"CFirstPersonCamera target vertical delta branch A", false, false},
    {0x8000E804u, 0xEC42F028u, L"CFirstPersonCamera target vertical delta branch B", false, false},
    {0x8000E838u, 0xEC42F028u, L"CFirstPersonCamera target vertical delta branch C", false, false},
};

constexpr uint32_t kLoadZeroToF2 = 0xC04280B0u; // lfs f2, -0x7f50(r2)

bool IsMem1Range(uint32_t gcAddr, size_t size) {
    if (gcAddr < kMem1Start || gcAddr >= kMem1Start + kMem1Size) {
        return false;
    }
    return static_cast<uint64_t>(gcAddr) + static_cast<uint64_t>(size) <=
        static_cast<uint64_t>(kMem1Start) + kMem1Size;
}

uint8_t* HostPtr(uint32_t gcAddr, size_t size) {
    if (!g_memBase || !IsMem1Range(gcAddr, size)) {
        return nullptr;
    }
    return reinterpret_cast<uint8_t*>(g_memBase + (gcAddr - kMem1Start));
}

uint32_t ReadU32(uint32_t gcAddr) {
    uint8_t* p = HostPtr(gcAddr, 4);
    if (!p) {
        return 0;
    }
    uint32_t raw = 0;
    std::memcpy(&raw, p, sizeof(raw));
    return _byteswap_ulong(raw);
}

uint16_t ReadU16(uint32_t gcAddr) {
    uint8_t* p = HostPtr(gcAddr, 2);
    if (!p) {
        return 0;
    }
    uint16_t raw = 0;
    std::memcpy(&raw, p, sizeof(raw));
    return _byteswap_ushort(raw);
}

uint8_t ReadU8(uint32_t gcAddr) {
    uint8_t* p = HostPtr(gcAddr, 1);
    return p ? *p : 0;
}

float ReadFloat(uint32_t gcAddr) {
    const uint32_t raw = ReadU32(gcAddr);
    float value = 0.0f;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

void WriteFloat(uint32_t gcAddr, float value) {
    uint8_t* p = HostPtr(gcAddr, 4);
    if (!p) {
        return;
    }
    uint32_t raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    raw = _byteswap_ulong(raw);
    std::memcpy(p, &raw, sizeof(raw));
}

bool WriteU32(uint32_t gcAddr, uint32_t value) {
    uint8_t* p = HostPtr(gcAddr, 4);
    if (!p) {
        return false;
    }
    uint32_t raw = _byteswap_ulong(value);
    std::memcpy(p, &raw, sizeof(raw));
    FlushInstructionCache(GetCurrentProcess(), p, 4);
    return true;
}

bool ResolveMemBase() {
    const uint64_t now = GetTickCount64();
    if (g_memBase && now - g_lastResolveTick < 1000) {
        return true;
    }
    g_lastResolveTick = now;

    MEMORY_BASIC_INFORMATION mbi{};
    uintptr_t address = 0;
    while (VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)) == sizeof(mbi)) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const size_t size = static_cast<size_t>(mbi.RegionSize);
        if (mbi.State == MEM_COMMIT &&
            mbi.Type == MEM_MAPPED &&
            (size == 0x02000000 || size == 0x04000000)) {
            g_memBase = base;
            if (ReadU32(0x80000000) == 0x474D3845u && ReadU16(0x80000004) == 0x3031u) {
                Log(L"GameTimingHooks resolved GM8E01 MEM1 base in-process.");
                return true;
            }
        }

        const uintptr_t next = base + size;
        if (next <= address) {
            break;
        }
        address = next;
    }

    g_memBase = 0;
    return false;
}

fs::path LocalAppDataPath() {
    wchar_t buffer[MAX_PATH] = {};
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, static_cast<DWORD>(MAX_PATH));
    if (len == 0 || len >= MAX_PATH) {
        return fs::temp_directory_path();
    }
    return fs::path(buffer);
}

void DumpPlayerOrbitCodeOnce() {
    if (g_dumpedPlayerOrbitCode || !g_memBase) {
        return;
    }
    g_dumpedPlayerOrbitCode = true;

    uint8_t* bytes = HostPtr(kPlayerOrbitStart, kPlayerOrbitEnd - kPlayerOrbitStart);
    if (!bytes) {
        Log(L"GameTimingHooks failed to dump CPlayerOrbit PPC range: no host pointer.");
        return;
    }

    const fs::path dumpDir = LocalAppDataPath() / L"PrimedGun" / L"CodeDumps";
    std::error_code ec;
    fs::create_directories(dumpDir, ec);
    const fs::path path = dumpDir / L"CPlayerOrbit_8017B2E0_8017FB84.bin";
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        Log(L"GameTimingHooks failed to open CPlayerOrbit dump: " + path.wstring());
        return;
    }
    out.write(reinterpret_cast<const char*>(bytes), kPlayerOrbitEnd - kPlayerOrbitStart);
    Log(L"GameTimingHooks dumped CPlayerOrbit PPC code: " + path.wstring());
}

void DumpFirstPersonCameraCodeOnce() {
    if (g_dumpedFirstPersonCameraCode || !g_memBase) {
        return;
    }
    g_dumpedFirstPersonCameraCode = true;

    uint8_t* bytes = HostPtr(kFirstPersonCameraStart, kFirstPersonCameraEnd - kFirstPersonCameraStart);
    if (!bytes) {
        Log(L"GameTimingHooks failed to dump CFirstPersonCamera PPC range: no host pointer.");
        return;
    }

    const fs::path dumpDir = LocalAppDataPath() / L"PrimedGun" / L"CodeDumps";
    std::error_code ec;
    fs::create_directories(dumpDir, ec);
    const fs::path path = dumpDir / L"CFirstPersonCamera_8000E3D0_8000FA80.bin";
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        Log(L"GameTimingHooks failed to open CFirstPersonCamera dump: " + path.wstring());
        return;
    }
    out.write(reinterpret_cast<const char*>(bytes), kFirstPersonCameraEnd - kFirstPersonCameraStart);
    Log(L"GameTimingHooks dumped CFirstPersonCamera PPC code: " + path.wstring());
}

void PatchOrbitCode() {
    return;

    if (!g_memBase) {
        return;
    }

    for (PpcPatch& patch : g_orbitPatches) {
        if (patch.applied) {
            continue;
        }

        const uint32_t current = ReadU32(patch.address);
        if (current == kLoadZeroToF2) {
            patch.applied = true;
            Log(L"GameTimingHooks orbit PPC patch already present at 0x" +
                std::to_wstring(patch.address) + L": " + patch.description);
            continue;
        }

        if (current != patch.original) {
            if (current != 0 && !patch.loggedWaiting) {
                patch.loggedWaiting = true;
                Log(L"GameTimingHooks waiting to patch orbit PPC at 0x" +
                    std::to_wstring(patch.address) + L": current instruction does not match expected yet.");
            }
            continue;
        }

        if (WriteU32(patch.address, kLoadZeroToF2)) {
            patch.applied = true;
            Log(L"GameTimingHooks patched orbit PPC at 0x" +
                std::to_wstring(patch.address) + L": " + patch.description + L" -> lfs f2,0.0.");
        }
    }
}

void ApplySharedPatches(SharedState* state) {
    if (!state || !g_memBase || state->patch.count > MaxGamePatches) {
        return;
    }

    if (state->patch.generation != g_lastPatchGeneration || state->patch.count != g_lastPatchCount) {
        g_lastPatchGeneration = state->patch.generation;
        g_lastPatchCount = state->patch.count;
        Log(L"GameTimingHooks received app patch set generation=" +
            std::to_wstring(g_lastPatchGeneration) +
            L" count=" + std::to_wstring(g_lastPatchCount));
    }

    for (uint32_t i = 0; i < state->patch.count; ++i) {
        GamePatch& patch = state->patch.patches[i];
        if (!patch.enabled || patch.applied) {
            continue;
        }
        if (!IsMem1Range(patch.address, 4)) {
            patch.lastSeen = 0xffffffffu;
            continue;
        }

        const uint32_t current = ReadU32(patch.address);
        patch.lastSeen = current;
        if (patch.requireOriginal && current != patch.original) {
            continue;
        }

        if (WriteU32(patch.address, patch.value)) {
            patch.applied = 1;
            Log(L"GameTimingHooks applied app patch[" + std::to_wstring(i) +
                L"] address=0x" + std::to_wstring(patch.address) +
                L" value=0x" + std::to_wstring(patch.value));
        }
    }
}

bool OrbitLockHeldNow(uint32_t stateMgr, uint32_t player) {
    const uint8_t held0 = ReadU8(stateMgr + kFinalInputOffset + 0x2c);
    if ((held0 & 0x04u) != 0) {
        return true;
    }

    const uint32_t scratchPlayer = ReadU32(kGunTargetHookScratch);
    const uint16_t scratchUid = ReadU16(kGunTargetHookScratch + 4);
    return scratchPlayer == player && scratchUid != 0xffffu;
}

bool OrbitLockHeldLatched(uint32_t stateMgr, uint32_t player) {
    const uint64_t now = GetTickCount64();
    if (OrbitLockHeldNow(stateMgr, player)) {
        g_lockLatchUntilTick = now + 250;
        return true;
    }
    return now < g_lockLatchUntilTick;
}

} // namespace

bool Install() {
    if (g_installed.exchange(true)) {
        return true;
    }
    Log(L"GameTimingHooks installed. PPC code dumps enabled; gameplay patches disabled.");
    ResolveMemBase();
    return true;
}

void SuppressLockCameraPitchForLogicTick() {
    if (!g_installed.load() || !ResolveMemBase()) {
        return;
    }

    const uint32_t stateMgr = kStateManager;
    const uint32_t player = ReadU32(stateMgr + kPlayerOffset);
    if (player < kMem1Start) {
        return;
    }

    const uint64_t checkCount = ++g_renderSuppressChecks;
    const uint32_t orbitState = ReadU32(player + kOrbitStateOffset);
    const bool orbiting = orbitState != 0 && orbitState != kOrbitStateGrapple;
    const bool targetLocked = OrbitLockHeldLatched(stateMgr, player);
    if (orbitState != g_lastOrbitState) {
        g_lastOrbitState = orbitState;
        Log(L"GameTimingHooks orbit state changed: " + std::to_wstring(orbitState) +
            L" targetLocked=" + std::to_wstring(targetLocked ? 1 : 0));
    }

    if (!orbiting && !targetLocked) {
        if (checkCount <= 8 || (checkCount % 600) == 0) {
            const uint8_t held0 = ReadU8(stateMgr + kFinalInputOffset + 0x2c);
            const uint32_t scratchPlayer = ReadU32(kGunTargetHookScratch);
            const uint16_t scratchUid = ReadU16(kGunTargetHookScratch + 4);
            const uint64_t logCount = ++g_notLockedLogs;
            Log(L"GameTimingHooks orbit pitch check inactive. check=" +
                std::to_wstring(checkCount) +
                L" orbitState=" + std::to_wstring(orbitState) +
                L" held0=0x" + std::to_wstring(static_cast<unsigned int>(held0)) +
                L" scratchPlayer=0x" + std::to_wstring(static_cast<unsigned int>(scratchPlayer)) +
                L" scratchUid=0x" + std::to_wstring(static_cast<unsigned int>(scratchUid)) +
                L" log=" + std::to_wstring(logCount));
        }
        return;
    }

    const float pitchBefore = ReadFloat(player + kFirstPersonPitchOffset);
    const uint64_t count = ++g_suppressionCalls;
    if (count <= 12 || count == 60 || count == 300 || count == 900) {
        Log(L"GameTimingHooks observed orbit pitch path; live writes disabled. count=" +
            std::to_wstring(count) +
            L" orbitState=" + std::to_wstring(orbitState) +
            L" targetLocked=" + std::to_wstring(targetLocked ? 1 : 0) +
            L" pitchBefore=" + std::to_wstring(pitchBefore));
    }
}

void Poll(SharedState* state) {
    if (g_installed.load()) {
        if (ResolveMemBase()) {
            DumpPlayerOrbitCodeOnce();
            DumpFirstPersonCameraCodeOnce();
            PatchOrbitCode();
            ApplySharedPatches(state);
        }
    }
}

void Shutdown() {
    if (g_installed.exchange(false)) {
        Log(L"GameTimingHooks shutdown.");
    }
}

} // namespace PrimedGun::Hook::GameTimingHooks
