#include "GameTimingHooks.h"

#include "PrimedGunShared.h"

#include <windows.h>
#include <psapi.h>
#include <dbghelp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace PrimedGun::Hook {
void Log(std::wstring_view message);
bool LoggingEnabled();
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
constexpr uint32_t kObjectListOffset = 0x810u;
constexpr uint32_t kCameraManagerOffset = 0x86Cu;
constexpr uint32_t kTransformOffset = 0x34u;
constexpr uint32_t kGunTargetHookScratch = 0x817FE400u;
constexpr uint32_t kFinalInputOffset = 0xB54u;
constexpr uint32_t kFinalInputRightStickY = kFinalInputOffset + 0x14u;
constexpr uint32_t kFinalInputRightStickYPress = kFinalInputOffset + 0x23u;
constexpr uint32_t kOrbitStateOffset = 0x304u;
constexpr uint32_t kFirstPersonPitchOffset = 0x3ECu;
constexpr uint32_t kFirstPersonPitchVelOffset = 0x3F0u;
constexpr uint32_t kOrbitStateGrapple = 5u;

std::atomic<bool> g_installed = false;
uintptr_t g_memBase = 0;
uint64_t g_suppressionCalls = 0;
uint64_t g_renderSuppressChecks = 0;
uint64_t g_notLockedLogs = 0;
uint64_t g_lastResolveTick = 0;
uint64_t g_lockLatchUntilTick = 0;
bool g_traceLastLockHeld = false;
bool g_traceHavePitch = false;
uint32_t g_traceFramesRemaining = 0;
uint32_t g_traceSequence = 0;
uint64_t g_traceStartTick = 0;
uint64_t g_traceLastLogTick = 0;
float g_traceLastPitch = 0.0f;
float g_traceLastCameraFwdZ = 0.0f;
uint32_t g_traceLastOrbitState = 0xffffffffu;
uint32_t g_traceLastScanState = 0xffffffffu;
bool g_traceLastButtonLock = false;
bool g_traceHaveCamera = false;
bool g_dumpedPlayerOrbitCode = false;
bool g_dumpedFirstPersonCameraCode = false;
uint32_t g_lastOrbitState = 0xffffffffu;
uint32_t g_lastPatchGeneration = 0;
uint32_t g_lastPatchCount = 0;
std::unordered_map<uint32_t, uint32_t> g_appPatchOriginals;
uint32_t g_lastInputBindingRequestGeneration = 0;
uint64_t g_lastInputBindingAttemptTick = 0;
uint32_t g_inputBindingAttemptCount = 0;
bool g_dumpedDolphinConfigProbe = false;
uint64_t g_lastDolphinConfigFileProbeTick = 0;
uint64_t g_lastDolphinConfigMemoryProbeTick = 0;
bool g_configFileTraceInstalled = false;
bool g_symbolsInitialized = false;
uint64_t g_lastConfigFileTracePatchTick = 0;
SharedState* g_activeSharedState = nullptr;
std::wstring g_lastPublishedConfigRoot;
std::wstring g_lastPublishedGameSettingsRoot;
std::wstring g_lastPublishedGameSettingsVrRoot;
uint32_t g_lastSaveProbeRequestGeneration = 0;
bool g_saveProbeAttempted = false;
bool g_startupSetupAttempted = false;
uint64_t g_lastStartupSetupAttemptTick = 0;
uint32_t g_startupSetupAttemptCount = 0;

using PFN_CreateFileW = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using PFN_WriteFile = BOOL(WINAPI*)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
using PFN_CloseHandle = BOOL(WINAPI*)(HANDLE);
using PFN_MoveFileExW = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, DWORD);
using PFN_MoveFileW = BOOL(WINAPI*)(LPCWSTR, LPCWSTR);
using PFN_DeleteFileW = BOOL(WINAPI*)(LPCWSTR);
using PFN_wfopen = FILE*(__cdecl*)(const wchar_t*, const wchar_t*);
using PFN_wfopen_s = errno_t(__cdecl*)(FILE**, const wchar_t*, const wchar_t*);
using PFN_fopen = FILE*(__cdecl*)(const char*, const char*);
using PFN_fwrite = size_t(__cdecl*)(const void*, size_t, size_t, FILE*);
using PFN_fclose = int(__cdecl*)(FILE*);

PFN_CreateFileW g_realCreateFileW = nullptr;
PFN_WriteFile g_realWriteFile = nullptr;
PFN_CloseHandle g_realCloseHandle = nullptr;
PFN_MoveFileExW g_realMoveFileExW = nullptr;
PFN_MoveFileW g_realMoveFileW = nullptr;
PFN_DeleteFileW g_realDeleteFileW = nullptr;
PFN_wfopen g_realWfopen = nullptr;
PFN_wfopen_s g_realWfopenS = nullptr;
PFN_fopen g_realFopen = nullptr;
PFN_fwrite g_realFwrite = nullptr;
PFN_fclose g_realFclose = nullptr;

std::mutex g_configTraceMutex;
std::unordered_map<HANDLE, std::wstring> g_tracedConfigHandles;
std::unordered_map<FILE*, std::wstring> g_tracedConfigFiles;
thread_local bool g_inConfigTraceHook = false;

struct ProbeFileState {
    fs::path path;
    uintmax_t size = 0;
    uint64_t writeTime = 0;
    uint64_t hash = 0;
    bool exists = false;
    bool seen = false;
};

std::vector<ProbeFileState> g_probeFileStates;

struct PpcPatch {
    uint32_t address;
    uint32_t original;
    uint32_t replacement;
    const wchar_t* description;
    bool applied;
    bool loggedWaiting;
};

struct ConditionalOrbitPatch {
    uint32_t address;
    uint32_t original;
    uint32_t flattened;
    uint16_t flattenedStackOffset;
    uint32_t cave;
    const wchar_t* description;
    bool applied;
    bool loggedWaiting;
};

ConditionalOrbitPatch g_orbitPatches[] = {
    {0x8000EBECu, 0x3881044Cu, 0x388103FCu, 0x03FCu, 0x80001AD0u,
     L"CFirstPersonCamera pre-orbit scan-aware flattened target vector", false, false},
    {0x8000EFF4u, 0x3881044Cu, 0x38810394u, 0x0394u, 0x80001B00u,
     L"CFirstPersonCamera normal lock-on scan-aware flattened target vector", false, false},
    {0x8000F5C0u, 0x3881044Cu, 0x38810320u, 0x0320u, 0x80001B30u,
     L"CFirstPersonCamera early follow scan-aware flattened target vector", false, false},
};

struct ConditionalPitchLoadPatch {
    uint32_t address;
    uint32_t original;
    uint32_t cave;
    const wchar_t* description;
    bool applied;
    bool loggedWaiting;
};

ConditionalPitchLoadPatch g_firstPersonPitchLoadPatch = {
    0x8000E548u, 0xC3FE03ECu, 0x80001B70u,
    L"CFirstPersonCamera scan-aware first-person pitch load", false, false};

constexpr uint32_t kLoadZeroToF2 = 0xC04280B0u; // lfs f2, -0x7f50(r2)
constexpr uint32_t kLoadZeroToF1 = 0xC02280B0u; // lfs f1, -0x7f50(r2)
constexpr uint32_t kLoadZeroToF31 = 0xC3E280B0u; // lfs f31, -0x7f50(r2)

struct DynamicPpcPatch {
    uint32_t address;
    uint32_t original;
    uint32_t replacement;
    const wchar_t* description;
};

DynamicPpcPatch g_combatPitchPatches[] = {
    {0x8000E7B4u, 0xEC21E828u, kLoadZeroToF1,
     L"CFirstPersonCamera lock target Z delta A"},
    {0x8000E808u, 0xEC21E828u, kLoadZeroToF1,
     L"CFirstPersonCamera lock target Z delta B"},
    {0x8000E83Cu, 0xEC21E828u, kLoadZeroToF1,
     L"CFirstPersonCamera lock target Z delta C"},
    {0x8000F050u, 0xC05F01B4u, kLoadZeroToF2,
     L"CFirstPersonCamera combat orbit transition current camera Z"},
    {0x8000EB44u, 0xC041086Cu, kLoadZeroToF2,
     L"CFirstPersonCamera pre-orbit current camera Z"},
    {0x8000EF44u, 0xC041086Cu, kLoadZeroToF2,
     L"CFirstPersonCamera normal orbit current camera Z"},
    {0x8000F50Cu, 0xC041086Cu, kLoadZeroToF2,
     L"CFirstPersonCamera early follow current camera Z"},
};

bool g_combatPitchPatchActive = false;
uint64_t g_lastCombatPitchPatchLogTick = 0;

bool IsMem1Range(uint32_t gcAddr, size_t size) {
    if (gcAddr < kMem1Start || gcAddr >= kMem1Start + kMem1Size) {
        return false;
    }
    return static_cast<uint64_t>(gcAddr) + static_cast<uint64_t>(size) <=
        static_cast<uint64_t>(kMem1Start) + kMem1Size;
}

uint32_t PpcBranch(uint32_t from, uint32_t to) {
    const int32_t offset = static_cast<int32_t>(to - from);
    return 0x48000000u | (static_cast<uint32_t>(offset) & 0x03FFFFFCu);
}

uint32_t PpcBeq(uint32_t from, uint32_t to) {
    const int32_t offset = static_cast<int32_t>(to - from);
    return 0x41820000u | (static_cast<uint32_t>(offset) & 0x0000FFFCu);
}

uint32_t PpcAddiR4R1(uint16_t offset) {
    return 0x38810000u | offset;
}

std::string TrimAscii(const std::string& text) {
    const size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
        return {};
    const size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

bool SameAsciiNoCase(const std::string& a, const std::string& b) {
    return _stricmp(a.c_str(), b.c_str()) == 0;
}

std::vector<std::string> ReadLines(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

void WriteLinesIfChanged(const fs::path& path, const std::vector<std::string>& lines) {
    const std::vector<std::string> existing = ReadLines(path);
    if (existing == lines)
        return;

    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    for (size_t i = 0; i < lines.size(); ++i) {
        file << lines[i];
        if (i + 1 < lines.size())
            file << "\n";
    }
}

bool FindIniSection(const std::vector<std::string>& lines, const std::string& section,
                    size_t& begin, size_t& end) {
    begin = lines.size();
    end = lines.size();
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string trimmed = TrimAscii(lines[i]);
        if (trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']') {
            const std::string name = trimmed.substr(1, trimmed.size() - 2);
            if (SameAsciiNoCase(name, section)) {
                begin = i;
                for (size_t j = i + 1; j < lines.size(); ++j) {
                    const std::string next = TrimAscii(lines[j]);
                    if (next.size() >= 2 && next.front() == '[' && next.back() == ']') {
                        end = j;
                        return true;
                    }
                }
                end = lines.size();
                return true;
            }
        }
    }
    return false;
}

std::vector<std::string> MergeDuplicateIniSection(const std::vector<std::string>& lines,
                                                  const std::string& section) {
    std::vector<std::string> output;
    output.reserve(lines.size());

    bool found_target = false;
    bool in_duplicate_target = false;
    for (const std::string& line : lines) {
        const std::string trimmed = TrimAscii(line);
        const bool is_section_header =
            trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']';

        if (is_section_header) {
            const std::string name = trimmed.substr(1, trimmed.size() - 2);
            if (SameAsciiNoCase(name, section)) {
                if (found_target) {
                    in_duplicate_target = true;
                    continue;
                }
                found_target = true;
                in_duplicate_target = false;
            } else {
                in_duplicate_target = false;
            }
        }

        if (in_duplicate_target)
            continue;

        output.push_back(line);
    }

    return output;
}

void ApplyIniSectionValues(const fs::path& path, const std::string& section,
                           const std::vector<std::pair<std::string, std::string>>& values,
                           const std::vector<std::string>& remove_keys) {
    std::vector<std::string> lines = MergeDuplicateIniSection(ReadLines(path), section);
    const std::string section_header = "[" + section + "]";

    size_t section_begin = lines.size();
    size_t section_end = lines.size();
    if (!FindIniSection(lines, section, section_begin, section_end)) {
        if (values.empty())
            return;
        if (!lines.empty() && !lines.back().empty())
            lines.push_back({});
        lines.push_back(section_header);
        section_begin = lines.size() - 1;
        section_end = lines.size();
    }

    auto key_from_line = [](const std::string& line) -> std::string {
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            return {};
        return TrimAscii(line.substr(0, eq));
    };

    std::vector<bool> used(values.size(), false);
    std::vector<std::string> output;
    output.reserve(lines.size() + values.size());

    for (size_t i = 0; i < lines.size(); ++i) {
        if (i <= section_begin || i >= section_end) {
            output.push_back(lines[i]);
            continue;
        }

        const std::string key = key_from_line(lines[i]);
        if (key.empty()) {
            output.push_back(lines[i]);
            continue;
        }

        if (std::find_if(remove_keys.begin(), remove_keys.end(), [&](const std::string& item) {
                return SameAsciiNoCase(item, key);
            }) != remove_keys.end()) {
            continue;
        }

        bool replaced = false;
        for (size_t value_i = 0; value_i < values.size(); ++value_i) {
            if (SameAsciiNoCase(values[value_i].first, key)) {
                if (used[value_i]) {
                    replaced = true;
                    break;
                }
                output.push_back(values[value_i].first + " = " + values[value_i].second);
                used[value_i] = true;
                replaced = true;
                break;
            }
        }
        if (!replaced)
            output.push_back(lines[i]);
    }

    size_t insert_at = output.size();
    for (size_t i = 0; i < output.size(); ++i) {
        if (SameAsciiNoCase(TrimAscii(output[i]), section_header)) {
            insert_at = i + 1;
            for (size_t j = i + 1; j < output.size(); ++j) {
                const std::string next = TrimAscii(output[j]);
                if (next.size() >= 2 && next.front() == '[' && next.back() == ']') {
                    insert_at = j;
                    break;
                }
                insert_at = j + 1;
            }
            break;
        }
    }

    std::vector<std::string> additions;
    for (size_t i = 0; i < values.size(); ++i) {
        if (!used[i])
            additions.push_back(values[i].first + " = " + values[i].second);
    }
    output.insert(output.begin() + static_cast<std::ptrdiff_t>(insert_at), additions.begin(),
                  additions.end());
    WriteLinesIfChanged(path, output);
}

void ReplaceIniSection(const fs::path& path, const std::string& section,
                       const std::vector<std::pair<std::string, std::string>>& values) {
    const std::string section_header = "[" + section + "]";
    const std::vector<std::string> lines = ReadLines(path);
    std::vector<std::string> output;
    output.reserve(lines.size() + values.size() + 2);

    output.push_back(section_header);
    for (const auto& value : values)
        output.push_back(value.first + " = " + value.second);

    bool skipping_target = false;
    for (const std::string& line : lines) {
        const std::string trimmed = TrimAscii(line);
        const bool is_section_header =
            trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']';
        if (is_section_header) {
            const std::string name = trimmed.substr(1, trimmed.size() - 2);
            skipping_target = SameAsciiNoCase(name, section);
        }
        if (skipping_target)
            continue;

        if (output.size() == values.size() + 1 && !line.empty())
            output.push_back({});
        output.push_back(line);
    }

    while (!output.empty() && output.back().empty())
        output.pop_back();
    WriteLinesIfChanged(path, output);
}

std::optional<fs::path> GetDolphinExeDirectory() {
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    while (size == buffer.size()) {
        buffer.resize(buffer.size() * 2);
        size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    }
    if (size == 0)
        return std::nullopt;
    buffer.resize(size);
    return fs::path(buffer).parent_path();
}

std::wstring Hex64(uintptr_t value) {
    wchar_t buffer[32] = {};
    swprintf_s(buffer, L"0x%016llX", static_cast<unsigned long long>(value));
    return buffer;
}

size_t MainModuleImageSize(HMODULE module) {
    if (!module)
        return 0;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const uint8_t*>(module) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return 0;
    return nt->OptionalHeader.SizeOfImage;
}

std::vector<uintptr_t> FindAsciiInModule(HMODULE module, std::string_view needle, size_t limit = 8) {
    std::vector<uintptr_t> hits;
    if (!module || needle.empty())
        return hits;

    const auto* base = reinterpret_cast<const uint8_t*>(module);
    const size_t size = MainModuleImageSize(module);
    if (size < needle.size())
        return hits;

    for (size_t i = 0; i + needle.size() <= size && hits.size() < limit; ++i) {
        if (std::memcmp(base + i, needle.data(), needle.size()) == 0)
            hits.push_back(reinterpret_cast<uintptr_t>(base + i));
    }
    return hits;
}

bool IsReadablePointer(const void* ptr, size_t size);

bool BytesMatch(const void* address, std::initializer_list<uint8_t> expected) {
    if (!IsReadablePointer(address, expected.size()))
        return false;
    const auto* bytes = reinterpret_cast<const uint8_t*>(address);
    size_t i = 0;
    for (uint8_t value : expected) {
        if (bytes[i++] != value)
            return false;
    }
    return true;
}

void LogPathState(const wchar_t* label, const fs::path& path) {
    std::error_code ec;
    const bool exists = fs::exists(path, ec);
    const uintmax_t size = exists ? fs::file_size(path, ec) : 0;
    Log(std::wstring(label) + L": " + path.wstring() + (exists ? L" exists size=" : L" missing size=") +
        std::to_wstring(size));
}

uint64_t FileTimeToU64(const fs::file_time_type& time) {
    return static_cast<uint64_t>(time.time_since_epoch().count());
}

uint64_t HashFileFNV1a(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    uint64_t hash = 1469598103934665603ull;
    char buffer[4096] = {};
    while (file) {
        file.read(buffer, sizeof(buffer));
        const std::streamsize read = file.gcount();
        for (std::streamsize i = 0; i < read; ++i) {
            hash ^= static_cast<uint8_t>(buffer[i]);
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

void LogPathStateWithHash(const wchar_t* label, const fs::path& path) {
    std::error_code ec;
    ProbeFileState current;
    current.path = path;
    current.exists = fs::exists(path, ec);
    if (current.exists) {
        current.size = fs::file_size(path, ec);
        current.writeTime = FileTimeToU64(fs::last_write_time(path, ec));
        current.hash = HashFileFNV1a(path);
    }

    auto it = std::find_if(g_probeFileStates.begin(), g_probeFileStates.end(),
                           [&](const ProbeFileState& state) {
                               return _wcsicmp(state.path.lexically_normal().wstring().c_str(),
                                               path.lexically_normal().wstring().c_str()) == 0;
                           });
    const bool changed = it == g_probeFileStates.end() || it->exists != current.exists ||
                         it->size != current.size || it->writeTime != current.writeTime ||
                         it->hash != current.hash;
    if (!changed)
        return;

    Log(std::wstring(label) + L": " + path.wstring() +
        (current.exists ? L" exists" : L" missing") + L" size=" +
        std::to_wstring(current.size) + L" write=" + std::to_wstring(current.writeTime) +
        L" hash=" + Hex64(static_cast<uintptr_t>(current.hash)));

    if (it == g_probeFileStates.end())
        g_probeFileStates.push_back(current);
    else
        *it = current;
}

bool IsReadableMemoryProtect(DWORD protect) {
    if ((protect & PAGE_GUARD) != 0 || (protect & PAGE_NOACCESS) != 0)
        return false;
    protect &= 0xff;
    return protect == PAGE_READONLY || protect == PAGE_READWRITE || protect == PAGE_WRITECOPY ||
           protect == PAGE_EXECUTE_READ || protect == PAGE_EXECUTE_READWRITE ||
           protect == PAGE_EXECUTE_WRITECOPY;
}

bool IsWritableMemoryProtect(DWORD protect) {
    if ((protect & PAGE_GUARD) != 0 || (protect & PAGE_NOACCESS) != 0)
        return false;
    protect &= 0xff;
    return protect == PAGE_READWRITE || protect == PAGE_WRITECOPY ||
           protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
}

std::vector<uintptr_t> FindAsciiInWritableMemory(std::string_view needle, size_t limit = 12) {
    std::vector<uintptr_t> hits;
    if (needle.empty())
        return hits;

    SYSTEM_INFO info = {};
    GetSystemInfo(&info);
    uintptr_t cursor = reinterpret_cast<uintptr_t>(info.lpMinimumApplicationAddress);
    const uintptr_t max_addr = reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress);

    while (cursor < max_addr && hits.size() < limit) {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &mbi, sizeof(mbi)) != sizeof(mbi))
            break;

        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const size_t size = mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && IsReadableMemoryProtect(mbi.Protect) &&
            IsWritableMemoryProtect(mbi.Protect) && size >= needle.size()) {
            const auto* bytes = reinterpret_cast<const uint8_t*>(base);
            for (size_t i = 0; i + needle.size() <= size && hits.size() < limit; ++i) {
                if (std::memcmp(bytes + i, needle.data(), needle.size()) == 0)
                    hits.push_back(base + i);
            }
        }

        const uintptr_t next = base + size;
        if (next <= cursor)
            break;
        cursor = next;
    }

    return hits;
}

bool IsReadablePointer(const void* ptr, size_t size) {
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(ptr, &mbi, sizeof(mbi)))
        return false;
    if (mbi.State != MEM_COMMIT || !IsReadableMemoryProtect(mbi.Protect))
        return false;
    const uintptr_t start = reinterpret_cast<uintptr_t>(ptr);
    const uintptr_t end = start + size;
    const uintptr_t region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return end >= start && end <= region_end;
}

bool PatchImportThunk(void** thunk, void* replacement, void** original) {
    if (!thunk || !IsReadablePointer(thunk, sizeof(void*)))
        return false;
    void* current = *thunk;
    if (current == replacement)
        return false;
    if (original && !*original)
        *original = current;

    DWORD old_protect = 0;
    if (!VirtualProtect(thunk, sizeof(void*), PAGE_READWRITE, &old_protect))
        return false;
    *thunk = replacement;
    DWORD ignored = 0;
    VirtualProtect(thunk, sizeof(void*), old_protect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), thunk, sizeof(void*));
    return true;
}

std::wstring LowerPathText(std::wstring value) {
    std::replace(value.begin(), value.end(), L'/', L'\\');
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(towlower(c));
    });
    return value;
}

bool IsDolphinConfigTracePath(const std::wstring& path) {
    const std::wstring lower = LowerPathText(path);
    return lower.find(L"gcpadnew.ini") != std::wstring::npos ||
           lower.find(L"hotkeys.ini") != std::wstring::npos ||
           lower.find(L"gamesettings\\gm8e01.ini") != std::wstring::npos ||
           lower.find(L"gamesettingsvr\\gm8e01.ini") != std::wstring::npos ||
           lower.find(L"config\\dolphin.ini") != std::wstring::npos ||
           lower.find(L"config\\gfx.ini") != std::wstring::npos;
}

bool IsDolphinConfigTraceDirectoryPath(const std::wstring& path) {
    const std::wstring lower = LowerPathText(path);
    return lower.find(L"dolphin emulator\\config\\") != std::wstring::npos ||
           lower.find(L"dolphin emulator\\gamesettings\\") != std::wstring::npos ||
           lower.find(L"dolphin emulator\\gamesettingsvr\\") != std::wstring::npos ||
           lower.find(L"\\dolphin-openxr\\config\\") != std::wstring::npos ||
           lower.find(L"\\dolphin-openxr\\user\\config\\") != std::wstring::npos ||
           lower.find(L"\\dolphin-openxr\\gamesettings\\") != std::wstring::npos ||
           lower.find(L"\\dolphin-openxr\\user\\gamesettings\\") != std::wstring::npos ||
           lower.find(L"\\dolphin-openxr\\gamesettingsvr\\") != std::wstring::npos ||
           lower.find(L"\\dolphin-openxr\\user\\gamesettingsvr\\") != std::wstring::npos;
}

bool ShouldTraceConfigPath(const std::wstring& path) {
    return IsDolphinConfigTracePath(path) || IsDolphinConfigTraceDirectoryPath(path);
}

bool CopySharedPath(wchar_t* destination, const std::wstring& value) {
    if (!destination || value.empty())
        return false;

    const size_t max_chars = MaxSharedPathChars - 1;
    const size_t count = std::min(value.size(), max_chars);
    std::wmemcpy(destination, value.c_str(), count);
    destination[count] = L'\0';
    return true;
}

std::optional<fs::path> RootBeforeFolder(const fs::path& path, const wchar_t* folder_name) {
    fs::path root;
    for (const fs::path& part : path.lexically_normal()) {
        if (_wcsicmp(part.c_str(), folder_name) == 0)
            return root;
        root /= part;
    }
    return std::nullopt;
}

void PublishDolphinConfigRoot(const fs::path& path) {
    if (!g_activeSharedState || path.empty())
        return;

    const fs::path normalized = path.lexically_normal();
    const std::wstring lower = LowerPathText(normalized.wstring());
    bool published = false;

    if (lower.find(L"\\config\\") != std::wstring::npos) {
        if (const std::optional<fs::path> root = RootBeforeFolder(normalized, L"Config")) {
            const std::wstring text = root->wstring();
            if (!text.empty() && _wcsicmp(text.c_str(), g_lastPublishedConfigRoot.c_str()) != 0 &&
                CopySharedPath(g_activeSharedState->dolphinConfigRoot, text)) {
                g_lastPublishedConfigRoot = text;
                published = true;
                Log(L"Dolphin active config root observed: " + text);
            }
        }
    }

    if (lower.find(L"\\gamesettingsvr\\") != std::wstring::npos) {
        if (const std::optional<fs::path> root = RootBeforeFolder(normalized, L"GameSettingsVR")) {
            const std::wstring text = root->wstring();
            if (!text.empty() && _wcsicmp(text.c_str(), g_lastPublishedGameSettingsVrRoot.c_str()) != 0 &&
                CopySharedPath(g_activeSharedState->dolphinGameSettingsVrRoot, text)) {
                g_lastPublishedGameSettingsVrRoot = text;
                published = true;
                Log(L"Dolphin active GameSettingsVR root observed: " + text);
            }
        }
    } else if (lower.find(L"\\gamesettings\\") != std::wstring::npos) {
        if (const std::optional<fs::path> root = RootBeforeFolder(normalized, L"GameSettings")) {
            const std::wstring text = root->wstring();
            if (!text.empty() && _wcsicmp(text.c_str(), g_lastPublishedGameSettingsRoot.c_str()) != 0 &&
                CopySharedPath(g_activeSharedState->dolphinGameSettingsRoot, text)) {
                g_lastPublishedGameSettingsRoot = text;
                published = true;
                Log(L"Dolphin active GameSettings root observed: " + text);
            }
        }
    }

    if (published)
        ++g_activeSharedState->dolphinConfigPathGeneration;
}

void InitializeSymbols() {
    if (g_symbolsInitialized)
        return;

    std::wstring symbol_path = L".;G:\\Dolphin-OpenXR\\Release2;"
                               L"G:\\dolphinXR-WIP3-patched\\dolphinXR-WIP3\\Binary\\x64\\Release";
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
    if (SymInitializeW(GetCurrentProcess(), symbol_path.c_str(), TRUE)) {
        g_symbolsInitialized = true;
        Log(L"Dolphin config trace symbol resolver initialized.");
    } else {
        Log(L"Dolphin config trace symbol resolver failed: " +
            std::to_wstring(GetLastError()));
    }
}

std::wstring ModuleRelativeAddress(void* address) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module) ||
        !module) {
        return Hex64(reinterpret_cast<uintptr_t>(address));
    }

    MODULEINFO info = {};
    GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info));
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
    const uintptr_t base = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
    const uintptr_t addr = reinterpret_cast<uintptr_t>(address);
    const fs::path module_path(path);

    std::wstring result = module_path.filename().wstring() + L"+" + Hex64(addr - base);
    if (g_symbolsInitialized) {
        alignas(SYMBOL_INFOW) uint8_t buffer[sizeof(SYMBOL_INFOW) + MAX_SYM_NAME * sizeof(wchar_t)] = {};
        auto* symbol = reinterpret_cast<SYMBOL_INFOW*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFOW);
        symbol->MaxNameLen = MAX_SYM_NAME;
        DWORD64 displacement = 0;
        if (SymFromAddrW(GetCurrentProcess(), static_cast<DWORD64>(addr), &displacement, symbol)) {
            result += L" ";
            result += symbol->Name;
            if (displacement != 0)
                result += L"+0x" + Hex64(static_cast<uintptr_t>(displacement));
        }
    }
    return result;
}

std::wstring CaptureConfigWriteStack() {
    void* frames[24] = {};
    const USHORT count = CaptureStackBackTrace(2, static_cast<DWORD>(std::size(frames)), frames, nullptr);
    std::wstring out;
    for (USHORT i = 0; i < count; ++i) {
        if (!out.empty())
            out += L" <- ";
        out += ModuleRelativeAddress(frames[i]);
    }
    return out;
}

HANDLE WINAPI Hook_CreateFileW(LPCWSTR file_name, DWORD desired_access, DWORD share_mode,
                               LPSECURITY_ATTRIBUTES security_attributes, DWORD creation_disposition,
                               DWORD flags_and_attributes, HANDLE template_file) {
    HANDLE handle = g_realCreateFileW
                        ? g_realCreateFileW(file_name, desired_access, share_mode, security_attributes,
                                            creation_disposition, flags_and_attributes, template_file)
                        : INVALID_HANDLE_VALUE;
    if (g_inConfigTraceHook || handle == INVALID_HANDLE_VALUE || !file_name)
        return handle;

    const bool writes = (desired_access & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0;
    const std::wstring path(file_name);
    if (writes && ShouldTraceConfigPath(path)) {
        PublishDolphinConfigRoot(path);
        std::lock_guard<std::mutex> lock(g_configTraceMutex);
        g_tracedConfigHandles[handle] = path;
        g_inConfigTraceHook = true;
        Log(L"Dolphin config trace CreateFileW write path=" + path +
            L" disposition=" + std::to_wstring(creation_disposition) +
            L" access=" + Hex64(desired_access) +
            L" stack=" + CaptureConfigWriteStack());
        g_inConfigTraceHook = false;
    }
    return handle;
}

BOOL WINAPI Hook_WriteFile(HANDLE file, LPCVOID buffer, DWORD bytes_to_write,
                           LPDWORD bytes_written, LPOVERLAPPED overlapped) {
    std::wstring path;
    if (!g_inConfigTraceHook) {
        std::lock_guard<std::mutex> lock(g_configTraceMutex);
        const auto it = g_tracedConfigHandles.find(file);
        if (it != g_tracedConfigHandles.end())
            path = it->second;
    }

    if (!path.empty()) {
        g_inConfigTraceHook = true;
        Log(L"Dolphin config trace WriteFile path=" + path +
            L" bytes=" + std::to_wstring(bytes_to_write) +
            L" stack=" + CaptureConfigWriteStack());
        g_inConfigTraceHook = false;
    }

    return g_realWriteFile ? g_realWriteFile(file, buffer, bytes_to_write, bytes_written, overlapped)
                           : FALSE;
}

BOOL WINAPI Hook_CloseHandle(HANDLE object) {
    if (!g_inConfigTraceHook) {
        std::lock_guard<std::mutex> lock(g_configTraceMutex);
        g_tracedConfigHandles.erase(object);
    }
    return g_realCloseHandle ? g_realCloseHandle(object) : FALSE;
}

bool CFileModeCanWrite(const wchar_t* mode) {
    if (!mode)
        return false;
    for (const wchar_t* p = mode; *p; ++p) {
        if (*p == L'w' || *p == L'a' || *p == L'+')
            return true;
    }
    return false;
}

bool CFileModeCanWriteA(const char* mode) {
    if (!mode)
        return false;
    for (const char* p = mode; *p; ++p) {
        if (*p == 'w' || *p == 'a' || *p == '+')
            return true;
    }
    return false;
}

std::wstring WidenAnsiPath(const char* path) {
    if (!path)
        return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    if (needed > 1) {
        std::wstring wide(static_cast<size_t>(needed - 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path, -1, wide.data(), needed);
        return wide;
    }
    const size_t len = std::strlen(path);
    std::wstring wide;
    wide.reserve(len);
    for (size_t i = 0; i < len; ++i)
        wide.push_back(static_cast<unsigned char>(path[i]));
    return wide;
}

void TrackConfigFile(FILE* file, const std::wstring& path, const std::wstring& mode,
                     const wchar_t* api_name) {
    if (!file || path.empty() || !ShouldTraceConfigPath(path))
        return;
    PublishDolphinConfigRoot(path);
    std::lock_guard<std::mutex> lock(g_configTraceMutex);
    g_tracedConfigFiles[file] = path;
    g_inConfigTraceHook = true;
    Log(std::wstring(L"Dolphin config trace ") + api_name + L" path=" + path +
        L" mode=" + mode + L" stack=" + CaptureConfigWriteStack());
    g_inConfigTraceHook = false;
}

FILE* __cdecl Hook_wfopen(const wchar_t* file_name, const wchar_t* mode) {
    FILE* file = g_realWfopen ? g_realWfopen(file_name, mode) : nullptr;
    if (!g_inConfigTraceHook && file && CFileModeCanWrite(mode))
        TrackConfigFile(file, file_name ? std::wstring(file_name) : std::wstring{},
                        mode ? std::wstring(mode) : std::wstring{}, L"_wfopen");
    return file;
}

errno_t __cdecl Hook_wfopen_s(FILE** stream, const wchar_t* file_name, const wchar_t* mode) {
    const errno_t result = g_realWfopenS ? g_realWfopenS(stream, file_name, mode) : EINVAL;
    if (!g_inConfigTraceHook && result == 0 && stream && *stream && CFileModeCanWrite(mode))
        TrackConfigFile(*stream, file_name ? std::wstring(file_name) : std::wstring{},
                        mode ? std::wstring(mode) : std::wstring{}, L"_wfopen_s");
    return result;
}

FILE* __cdecl Hook_fopen(const char* file_name, const char* mode) {
    FILE* file = g_realFopen ? g_realFopen(file_name, mode) : nullptr;
    if (!g_inConfigTraceHook && file && CFileModeCanWriteA(mode)) {
        const std::wstring wide_path = WidenAnsiPath(file_name);
        TrackConfigFile(file, wide_path, WidenAnsiPath(mode), L"fopen");
    }
    return file;
}

size_t __cdecl Hook_fwrite(const void* buffer, size_t size, size_t count, FILE* stream) {
    std::wstring path;
    if (!g_inConfigTraceHook) {
        std::lock_guard<std::mutex> lock(g_configTraceMutex);
        const auto it = g_tracedConfigFiles.find(stream);
        if (it != g_tracedConfigFiles.end())
            path = it->second;
    }
    if (!path.empty()) {
        g_inConfigTraceHook = true;
        Log(L"Dolphin config trace fwrite path=" + path +
            L" bytes=" + std::to_wstring(size * count) +
            L" stack=" + CaptureConfigWriteStack());
        g_inConfigTraceHook = false;
    }
    return g_realFwrite ? g_realFwrite(buffer, size, count, stream) : 0;
}

int __cdecl Hook_fclose(FILE* stream) {
    if (!g_inConfigTraceHook) {
        std::lock_guard<std::mutex> lock(g_configTraceMutex);
        g_tracedConfigFiles.erase(stream);
    }
    return g_realFclose ? g_realFclose(stream) : EOF;
}

BOOL WINAPI Hook_MoveFileExW(LPCWSTR existing_file_name, LPCWSTR new_file_name, DWORD flags) {
    const std::wstring old_path = existing_file_name ? std::wstring(existing_file_name) : std::wstring{};
    const std::wstring new_path = new_file_name ? std::wstring(new_file_name) : std::wstring{};
    if (!g_inConfigTraceHook &&
        (ShouldTraceConfigPath(old_path) || ShouldTraceConfigPath(new_path))) {
        PublishDolphinConfigRoot(ShouldTraceConfigPath(new_path) ? fs::path(new_path) : fs::path(old_path));
        g_inConfigTraceHook = true;
        Log(L"Dolphin config trace MoveFileExW old=" + old_path + L" new=" + new_path +
            L" flags=" + Hex64(flags) + L" stack=" + CaptureConfigWriteStack());
        g_inConfigTraceHook = false;
    }
    return g_realMoveFileExW ? g_realMoveFileExW(existing_file_name, new_file_name, flags) : FALSE;
}

BOOL WINAPI Hook_MoveFileW(LPCWSTR existing_file_name, LPCWSTR new_file_name) {
    const std::wstring old_path = existing_file_name ? std::wstring(existing_file_name) : std::wstring{};
    const std::wstring new_path = new_file_name ? std::wstring(new_file_name) : std::wstring{};
    if (!g_inConfigTraceHook &&
        (ShouldTraceConfigPath(old_path) || ShouldTraceConfigPath(new_path))) {
        PublishDolphinConfigRoot(ShouldTraceConfigPath(new_path) ? fs::path(new_path) : fs::path(old_path));
        g_inConfigTraceHook = true;
        Log(L"Dolphin config trace MoveFileW old=" + old_path + L" new=" + new_path +
            L" stack=" + CaptureConfigWriteStack());
        g_inConfigTraceHook = false;
    }
    return g_realMoveFileW ? g_realMoveFileW(existing_file_name, new_file_name) : FALSE;
}

BOOL WINAPI Hook_DeleteFileW(LPCWSTR file_name) {
    const std::wstring path = file_name ? std::wstring(file_name) : std::wstring{};
    if (!g_inConfigTraceHook && ShouldTraceConfigPath(path)) {
        PublishDolphinConfigRoot(path);
        g_inConfigTraceHook = true;
        Log(L"Dolphin config trace DeleteFileW path=" + path +
            L" stack=" + CaptureConfigWriteStack());
        g_inConfigTraceHook = false;
    }
    return g_realDeleteFileW ? g_realDeleteFileW(file_name) : FALSE;
}

uint32_t PatchConfigTraceImportsForModule(HMODULE module, const char* imported_dll_name) {
    auto* base = reinterpret_cast<uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (!IsReadablePointer(dos, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (!IsReadablePointer(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE)
        return 0;

    const IMAGE_DATA_DIRECTORY& import_dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (import_dir.VirtualAddress == 0 || import_dir.Size == 0)
        return 0;

    uint32_t patched_count = 0;
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + import_dir.VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        const char* dll_name = reinterpret_cast<const char*>(base + descriptor->Name);
        if (_stricmp(dll_name, imported_dll_name) != 0)
            continue;

        auto* original_thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk);
        auto* first_thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
        if (!descriptor->OriginalFirstThunk)
            original_thunk = first_thunk;

        for (; original_thunk->u1.AddressOfData; ++original_thunk, ++first_thunk) {
            if (IMAGE_SNAP_BY_ORDINAL(original_thunk->u1.Ordinal))
                continue;
            auto* import_by_name =
                reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + original_thunk->u1.AddressOfData);
            const char* function_name = reinterpret_cast<const char*>(import_by_name->Name);
            if (std::strcmp(function_name, "CreateFileW") == 0) {
                patched_count += PatchImportThunk(reinterpret_cast<void**>(&first_thunk->u1.Function),
                                                  reinterpret_cast<void*>(&Hook_CreateFileW),
                                                  reinterpret_cast<void**>(&g_realCreateFileW)) ? 1u : 0u;
            } else if (std::strcmp(function_name, "WriteFile") == 0) {
                patched_count += PatchImportThunk(reinterpret_cast<void**>(&first_thunk->u1.Function),
                                                  reinterpret_cast<void*>(&Hook_WriteFile),
                                                  reinterpret_cast<void**>(&g_realWriteFile)) ? 1u : 0u;
            } else if (std::strcmp(function_name, "CloseHandle") == 0) {
                patched_count += PatchImportThunk(reinterpret_cast<void**>(&first_thunk->u1.Function),
                                                  reinterpret_cast<void*>(&Hook_CloseHandle),
                                                  reinterpret_cast<void**>(&g_realCloseHandle)) ? 1u : 0u;
            } else if (std::strcmp(function_name, "MoveFileExW") == 0) {
                patched_count += PatchImportThunk(reinterpret_cast<void**>(&first_thunk->u1.Function),
                                                  reinterpret_cast<void*>(&Hook_MoveFileExW),
                                                  reinterpret_cast<void**>(&g_realMoveFileExW)) ? 1u : 0u;
            } else if (std::strcmp(function_name, "MoveFileW") == 0) {
                patched_count += PatchImportThunk(reinterpret_cast<void**>(&first_thunk->u1.Function),
                                                  reinterpret_cast<void*>(&Hook_MoveFileW),
                                                  reinterpret_cast<void**>(&g_realMoveFileW)) ? 1u : 0u;
            } else if (std::strcmp(function_name, "DeleteFileW") == 0) {
                patched_count += PatchImportThunk(reinterpret_cast<void**>(&first_thunk->u1.Function),
                                                  reinterpret_cast<void*>(&Hook_DeleteFileW),
                                                  reinterpret_cast<void**>(&g_realDeleteFileW)) ? 1u : 0u;
            } else if (std::strcmp(function_name, "_wfopen") == 0) {
                patched_count += PatchImportThunk(reinterpret_cast<void**>(&first_thunk->u1.Function),
                                                  reinterpret_cast<void*>(&Hook_wfopen),
                                                  reinterpret_cast<void**>(&g_realWfopen)) ? 1u : 0u;
            } else if (std::strcmp(function_name, "_wfopen_s") == 0) {
                patched_count += PatchImportThunk(reinterpret_cast<void**>(&first_thunk->u1.Function),
                                                  reinterpret_cast<void*>(&Hook_wfopen_s),
                                                  reinterpret_cast<void**>(&g_realWfopenS)) ? 1u : 0u;
            } else if (std::strcmp(function_name, "fopen") == 0) {
                patched_count += PatchImportThunk(reinterpret_cast<void**>(&first_thunk->u1.Function),
                                                  reinterpret_cast<void*>(&Hook_fopen),
                                                  reinterpret_cast<void**>(&g_realFopen)) ? 1u : 0u;
            } else if (std::strcmp(function_name, "fwrite") == 0) {
                patched_count += PatchImportThunk(reinterpret_cast<void**>(&first_thunk->u1.Function),
                                                  reinterpret_cast<void*>(&Hook_fwrite),
                                                  reinterpret_cast<void**>(&g_realFwrite)) ? 1u : 0u;
            } else if (std::strcmp(function_name, "fclose") == 0) {
                patched_count += PatchImportThunk(reinterpret_cast<void**>(&first_thunk->u1.Function),
                                                  reinterpret_cast<void*>(&Hook_fclose),
                                                  reinterpret_cast<void**>(&g_realFclose)) ? 1u : 0u;
            }
        }
    }
    if (patched_count > 0) {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
        const fs::path module_path(path);
        Log(L"Dolphin config trace patched " + std::to_wstring(patched_count) +
            L" imports in " + module_path.filename().wstring() +
            L" from " + WidenAnsiPath(imported_dll_name));
    }
    return patched_count;
}

bool InstallConfigFileTraceHooks() {
    HMODULE modules[1024] = {};
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed))
        return false;

    uint32_t patched_count = 0;
    const size_t count = std::min<size_t>(needed / sizeof(HMODULE), std::size(modules));
    for (size_t i = 0; i < count; ++i) {
        patched_count += PatchConfigTraceImportsForModule(modules[i], "kernel32.dll");
        patched_count += PatchConfigTraceImportsForModule(modules[i], "KernelBase.dll");
        patched_count += PatchConfigTraceImportsForModule(modules[i], "api-ms-win-crt-stdio-l1-1-0.dll");
        patched_count += PatchConfigTraceImportsForModule(modules[i], "ucrtbase.dll");
    }

    if (patched_count > 0 && !g_configFileTraceInstalled) {
        g_configFileTraceInstalled = true;
        Log(L"Dolphin config trace hooks installed.");
    }
    return patched_count > 0;
}

fs::path EnvPath(const wchar_t* name) {
    const wchar_t* value = _wgetenv(name);
    if (!value || !*value)
        return {};
    return fs::path(value);
}

void AddUniquePath(std::vector<fs::path>& paths, const fs::path& path, bool require_exists) {
    if (path.empty())
        return;
    std::error_code ec;
    if (require_exists && !fs::exists(path, ec))
        return;

    const std::wstring normalized = path.lexically_normal().wstring();
    const auto found = std::find_if(paths.begin(), paths.end(), [&](const fs::path& item) {
        return _wcsicmp(item.lexically_normal().wstring().c_str(), normalized.c_str()) == 0;
    });
    if (found == paths.end())
        paths.push_back(path);
}

std::vector<fs::path> DolphinProfileFiles(const fs::path& relative_path, bool require_exists) {
    std::vector<fs::path> paths;
    const std::optional<fs::path> exe_dir = GetDolphinExeDirectory();
    if (exe_dir) {
        AddUniquePath(paths, *exe_dir / relative_path, require_exists);
        AddUniquePath(paths, *exe_dir / L"User" / relative_path, require_exists);
        AddUniquePath(paths, exe_dir->parent_path() / relative_path, require_exists);
        AddUniquePath(paths, exe_dir->parent_path() / L"User" / relative_path, require_exists);
    }

    const fs::path appdata = EnvPath(L"APPDATA");
    if (!appdata.empty())
        AddUniquePath(paths, appdata / L"Dolphin Emulator" / relative_path, require_exists);

    const fs::path userprofile = EnvPath(L"USERPROFILE");
    if (!userprofile.empty())
        AddUniquePath(paths, userprofile / L"Documents" / L"Dolphin Emulator" / relative_path,
                      require_exists);

    return paths;
}

std::vector<fs::path> DolphinActiveProfileFiles(const fs::path& relative_path, bool require_exists) {
    std::vector<fs::path> paths;
    if (!g_lastPublishedConfigRoot.empty()) {
        const fs::path root = fs::path(g_lastPublishedConfigRoot);
        fs::path path;
        const std::wstring lower = LowerPathText(relative_path.wstring());
        if (lower.rfind(L"config\\", 0) == 0 || lower.rfind(L"gamesettings\\", 0) == 0 ||
            lower.rfind(L"gamesettingsvr\\", 0) == 0) {
            path = root / relative_path;
        } else {
            path = root / L"Config" / relative_path;
        }
        AddUniquePath(paths, path, require_exists);
    }

    if (paths.empty())
        return DolphinProfileFiles(relative_path, require_exists);
    return paths;
}

bool IniSectionContainsAll(const fs::path& path, const std::string& section,
                           const std::vector<std::string>& required_lines) {
    const std::vector<std::string> lines = ReadLines(path);
    size_t begin = 0;
    size_t end = 0;
    if (!FindIniSection(lines, section, begin, end))
        return false;

    for (const std::string& required : required_lines) {
        bool found = false;
        for (size_t i = begin + 1; i < end; ++i) {
            if (SameAsciiNoCase(TrimAscii(lines[i]), required)) {
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    return true;
}

bool VerifyPrimedGunGCPadConfig(const fs::path& path) {
    return IniSectionContainsAll(path, "GCPad1",
                                 {"Device = OpenXR/0/OpenXR Controller",
                                  "Buttons/A = `Right Button A`",
                                  "Buttons/B = `Right Button B`",
                                  "Buttons/X = `Left Button X`",
                                  "Buttons/Y = `Right Button Squeeze`&`Right Squeeze`",
                                  "Buttons/Z = `Left Button Squeeze`&`Left Squeeze`",
                                  "Buttons/Start = `Left Button Y`",
                                  "Main Stick/Up = `Left Thumbstick Y+`",
                                  "Main Stick/Down = `Left Thumbstick Y-`",
                                  "Main Stick/Left = `Left Thumbstick X-`",
                                  "Main Stick/Right = `Left Thumbstick X+`",
                                  "Main Stick/Dead Zone = 12.",
                                  "C-Stick/Up = `Right Thumbstick Y+`",
                                  "C-Stick/Down = `Right Thumbstick Y-`",
                                  "C-Stick/Left = `Right Thumbstick X-`",
                                  "C-Stick/Right = `Right Thumbstick X+`",
                                  "C-Stick/Dead Zone = 12.",
                                  "Triggers/L = `Left Trigger`",
                                  "Triggers/R = `Right Trigger`",
                                  "Triggers/L-Analog = `Left Trigger`",
                                  "Triggers/R-Analog = `Right Trigger`",
                                  "Rumble/Motor = `Motor Right`"});
}

bool VerifyPrimedGunInputBindings() {
    bool saw_candidate = false;
    for (const fs::path& path : DolphinActiveProfileFiles(L"Config\\GCPadNew.ini", true)) {
        saw_candidate = true;
        if (VerifyPrimedGunGCPadConfig(path)) {
            Log(L"Verified PrimedGun GCPad config in Dolphin active path: " + path.wstring());
            return true;
        }
        Log(L"PrimedGun GCPad config verification failed for: " + path.wstring());
    }

    if (!saw_candidate)
        Log(L"PrimedGun GCPad config verification found no existing active GCPadNew.ini target.");
    return false;
}

void ProbeDolphinConfigSystems() {
    if (!LoggingEnabled())
        return;

    HMODULE main_module = GetModuleHandleW(nullptr);
    wchar_t exe_path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe_path, static_cast<DWORD>(std::size(exe_path)));
    Log(L"Dolphin config probe: exe=" + std::wstring(exe_path));
    Log(L"Dolphin config probe: main module base=" +
        Hex64(reinterpret_cast<uintptr_t>(main_module)) + L" size=" +
        std::to_wstring(MainModuleImageSize(main_module)));

    const std::vector<std::string_view> anchors = {
        "GCPadNew",
        "Hotkeys",
        "GCPad1",
        "OpenXR/0/OpenXR Controller",
        "VR/Reset VR Position",
        "GameSettingsVR",
        "Graphics.VR",
        "UnitsPerMeter",
        "StereoDepthPercentage",
        "GPUDeterminismMode",
    };

    for (std::string_view anchor : anchors) {
        const std::vector<uintptr_t> hits = FindAsciiInModule(main_module, anchor);
        std::wstring line = L"Dolphin config probe anchor \"" +
                            std::wstring(anchor.begin(), anchor.end()) + L"\" hits=" +
                            std::to_wstring(hits.size());
        for (uintptr_t hit : hits)
            line += L" " + Hex64(hit);
        Log(line);
    }

    for (const fs::path& path : DolphinProfileFiles(L"Config\\GCPadNew.ini", false))
        LogPathState(L"Dolphin config probe GCPad candidate", path);
    for (const fs::path& path : DolphinProfileFiles(L"Config\\Hotkeys.ini", false))
        LogPathState(L"Dolphin config probe Hotkeys candidate", path);
    for (const fs::path& path : DolphinProfileFiles(L"GameSettings\\GM8E01.ini", false))
        LogPathState(L"Dolphin config probe GameSettings candidate", path);
    for (const fs::path& path : DolphinProfileFiles(L"GameSettingsVR\\GM8E01.ini", false))
        LogPathState(L"Dolphin config probe GameSettingsVR candidate", path);
}

void ProbeDolphinConfigFilesOccasionally() {
    if (!LoggingEnabled())
        return;
    const uint64_t now = GetTickCount64();
    if (now - g_lastDolphinConfigFileProbeTick < 2000)
        return;
    g_lastDolphinConfigFileProbeTick = now;

    for (const fs::path& path : DolphinActiveProfileFiles(L"Config\\GCPadNew.ini", true)) {
        PublishDolphinConfigRoot(path);
        LogPathStateWithHash(L"Dolphin config watch GCPad", path);
    }
    for (const fs::path& path : DolphinProfileFiles(L"Config\\Hotkeys.ini", true)) {
        PublishDolphinConfigRoot(path);
        LogPathStateWithHash(L"Dolphin config watch Hotkeys", path);
    }
    for (const fs::path& path : DolphinProfileFiles(L"GameSettings\\GM8E01.ini", true)) {
        PublishDolphinConfigRoot(path);
        LogPathStateWithHash(L"Dolphin config watch GameSettings", path);
    }
    for (const fs::path& path : DolphinProfileFiles(L"GameSettingsVR\\GM8E01.ini", true)) {
        PublishDolphinConfigRoot(path);
        LogPathStateWithHash(L"Dolphin config watch GameSettingsVR", path);
    }
}

void ProbeDolphinConfigMemoryOccasionally() {
    if (!LoggingEnabled())
        return;
    const uint64_t now = GetTickCount64();
    if (now - g_lastDolphinConfigMemoryProbeTick < 5000)
        return;
    g_lastDolphinConfigMemoryProbeTick = now;

    const std::vector<std::string_view> anchors = {
        "GCPad1",
        "OpenXR/0/OpenXR Controller",
        "VR/Reset VR Position",
        "Buttons/A",
        "Main Stick/Up",
        "Right Button Thumbstick",
    };
    for (std::string_view anchor : anchors) {
        const std::vector<uintptr_t> hits = FindAsciiInWritableMemory(anchor);
        std::wstring line = L"Dolphin config writable anchor \"" +
                            std::wstring(anchor.begin(), anchor.end()) + L"\" hits=" +
                            std::to_wstring(hits.size());
        for (uintptr_t hit : hits)
            line += L" " + Hex64(hit);
        Log(line);
    }
}

bool TriggerDolphinPadConfigSaveProbe() {
    if (g_saveProbeAttempted)
        return false;

    HMODULE main_module = GetModuleHandleW(nullptr);
    const uintptr_t base = reinterpret_cast<uintptr_t>(main_module);
    if (!base)
        return false;

    // Dolphin XR Release2: InputConfig::SaveConfig. Guard the prologue so this stays
    // a no-op on unknown builds instead of calling a random address.
    auto* save_config = reinterpret_cast<void(__fastcall*)(void*)>(base + 0x7d4a30u);
    if (!BytesMatch(reinterpret_cast<void*>(save_config),
                    {0x48, 0x89, 0x5c, 0x24, 0x18, 0x48, 0x89, 0x4c, 0x24, 0x08,
                     0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41,
                     0x57})) {
        Log(L"Dolphin config save probe skipped: SaveConfig signature did not match this build.");
        g_saveProbeAttempted = true;
        return false;
    }

    void* gcpad_config = nullptr;
    for (uintptr_t hit : FindAsciiInModule(main_module, "GCPadNew", 16)) {
        auto* candidate = reinterpret_cast<void*>(hit - 0x20u);
        const auto* ini_name = reinterpret_cast<const char*>(reinterpret_cast<uintptr_t>(candidate) + 0x20u);
        if (IsReadablePointer(candidate, 0x80) && std::strcmp(ini_name, "GCPadNew") == 0) {
            gcpad_config = candidate;
            break;
        }
    }

    if (!gcpad_config) {
        Log(L"Dolphin config save probe skipped: could not locate GCPadNew InputConfig object.");
        return false;
    }

    g_saveProbeAttempted = true;
    Log(L"Dolphin config save probe calling Pad GCPadNew SaveConfig object=" +
        Hex64(reinterpret_cast<uintptr_t>(gcpad_config)) +
        L" function=" + Hex64(reinterpret_cast<uintptr_t>(save_config)));
    save_config(gcpad_config);
    Log(L"Dolphin config save probe completed.");
    return true;
}

bool ApplyPrimedGunDolphinSetupFromHook() {
    bool wrote_any = false;

    for (const fs::path& path : DolphinProfileFiles(L"Config\\GCPadNew.ini", true)) {
        ApplyIniSectionValues(path, "GCPad1",
                              {{"Device", "OpenXR/0/OpenXR Controller"},
                               {"Buttons/A", "`Right Button A`"},
                               {"Buttons/B", "`Right Button B`"},
                               {"Buttons/X", "`Left Button X`"},
                               {"Buttons/Y", "`Right Button Squeeze`&`Right Squeeze`"},
                               {"Buttons/Z", "`Left Button Squeeze`&`Left Squeeze`"},
                               {"Buttons/Start", "`Left Button Y`"},
                               {"Main Stick/Up", "`Left Thumbstick Y+`"},
                               {"Main Stick/Down", "`Left Thumbstick Y-`"},
                               {"Main Stick/Left", "`Left Thumbstick X-`"},
                               {"Main Stick/Right", "`Left Thumbstick X+`"},
                               {"Main Stick/Dead Zone", "12."},
                               {"C-Stick/Up", "`Right Thumbstick Y+`"},
                               {"C-Stick/Down", "`Right Thumbstick Y-`"},
                               {"C-Stick/Left", "`Right Thumbstick X-`"},
                               {"C-Stick/Right", "`Right Thumbstick X+`"},
                               {"C-Stick/Dead Zone", "12."},
                               {"Triggers/L", "`Left Trigger`"},
                               {"Triggers/R", "`Right Trigger`"},
                               {"Triggers/L-Analog", "`Left Trigger`"},
                               {"Triggers/R-Analog", "`Right Trigger`"},
                               {"Rumble/Motor", "`Motor Right`"}},
                              {});
        wrote_any = true;
        Log(L"Applied PrimedGun GCPad config to Dolphin active path: " + path.wstring());
    }

    for (const fs::path& path : DolphinActiveProfileFiles(L"Config\\Hotkeys.ini", true)) {
        ApplyIniSectionValues(path, "Hotkeys",
                              {{"Device", "OpenXR/0/OpenXR Controller"},
                               {"VR/Reset VR Position", "`Right Button Thumbstick`"}},
                              {});
        wrote_any = true;
        Log(L"Applied PrimedGun hotkey config to Dolphin active path: " + path.wstring());
    }

    for (const fs::path& path : DolphinActiveProfileFiles(L"Config\\Dolphin.ini", false)) {
        ApplyIniSectionValues(path, "VR", {{"EnableVR", "True"}}, {});
        wrote_any = true;
        Log(L"Applied PrimedGun Dolphin global config to Dolphin active path: " +
            path.wstring());
    }

    for (const fs::path& path : DolphinActiveProfileFiles(L"Config\\GFX.ini", false)) {
        ApplyIniSectionValues(path, "VR", {{"EnableOpenXR", "True"}}, {});
        wrote_any = true;
        Log(L"Applied PrimedGun GFX OpenXR config to Dolphin active path: " + path.wstring());
    }

    for (const fs::path& path : DolphinActiveProfileFiles(L"GameSettings\\GM8E01.ini", false)) {
        ApplyIniSectionValues(path, "Video_Stereoscopy",
                              {{"StereoDepthPercentage", "100"},
                               {"StereoConvergence", "20.00"},
                               {"StereoEFBMonoDepth", "False"}},
                              {});
        wrote_any = true;
        Log(L"Applied PrimedGun GM8E01 game config to Dolphin active path: " + path.wstring());
    }

    for (const fs::path& path : DolphinActiveProfileFiles(L"GameSettingsVR\\GM8E01.ini", false)) {
        ReplaceIniSection(path, "Graphics.VR",
                          {{"UnitsPerMeter", "1.50"},
                           {"CameraForward", "0.0"}});
        ApplyIniSectionValues(path, "GFX.VR", {},
                              {"UnitsPerMeter", "UnitsPerMetre", "LeanBackAngle",
                               "CameraForward", "HeadLockedCurvature", "ElementDepth",
                               "VirtualScreen", "DontClearScreen", "OpcodeReplay",
                               "AutoVBIFromHMD"});
        ApplyIniSectionValues(path, "VR", {},
                              {"UnitsPerMeter", "UnitsPerMetre", "LeanBackAngle",
                               "CameraForward", "HeadLockedCurvature", "ElementDepth",
                               "VirtualScreen", "DontClearScreen", "OpcodeReplay",
                               "AutoVBIFromHMD"});
        wrote_any = true;
        Log(L"Applied PrimedGun GM8E01 VR config to Dolphin active path: " + path.wstring());
    }

    return wrote_any;
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

void WriteU8(uint32_t gcAddr, uint8_t value) {
    uint8_t* p = HostPtr(gcAddr, 1);
    if (!p) {
        return;
    }
    *p = value;
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
    if (!LoggingEnabled()) {
        return;
    }
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
    if (!LoggingEnabled()) {
        return;
    }
    if (g_dumpedFirstPersonCameraCode || !g_memBase) {
        return;
    }

    uint8_t* bytes = HostPtr(kFirstPersonCameraStart, kFirstPersonCameraEnd - kFirstPersonCameraStart);
    if (!bytes) {
        Log(L"GameTimingHooks failed to dump CFirstPersonCamera PPC range: no host pointer.");
        return;
    }
    if (ReadU32(0x8000E7B4u) == 0 || ReadU32(0x8000F128u) == 0) {
        return;
    }
    g_dumpedFirstPersonCameraCode = true;

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
    // The dynamic combat pitch patch handles orbit lock without touching scan mode.
    return;

    if (!g_memBase) {
        return;
    }

    for (ConditionalOrbitPatch& patch : g_orbitPatches) {
        if (patch.applied) {
            continue;
        }

        const uint32_t current = ReadU32(patch.address);
        const uint32_t branch = PpcBranch(patch.address, patch.cave);
        if (current == branch) {
            patch.applied = true;
            Log(L"GameTimingHooks orbit PPC patch already present at 0x" +
                std::to_wstring(patch.address) + L": " + patch.description);
            continue;
        }

        if (current != patch.original && current != patch.flattened) {
            if (current != 0 && !patch.loggedWaiting) {
                patch.loggedWaiting = true;
                Log(L"GameTimingHooks waiting to patch orbit PPC at 0x" +
                    std::to_wstring(patch.address) + L": current instruction does not match expected yet.");
            }
            continue;
        }

        const uint32_t originalLabel = patch.cave + 0x20u;
        const uint32_t flattenedLabel = patch.cave + 0x28u;
        const uint32_t returnAddr = patch.address + 4u;

        WriteU32(patch.cave + 0x00u, 0x3D808045u);                      // lis r12, 0x8045
        WriteU32(patch.cave + 0x04u, 0x618CA1A8u);                      // ori r12, r12, 0xA1A8
        WriteU32(patch.cave + 0x08u, 0x818C084Cu);                      // lwz r12, 0x84C(r12)
        WriteU32(patch.cave + 0x0Cu, 0x2C0C0000u);                      // cmpwi r12, 0
        WriteU32(patch.cave + 0x10u, PpcBeq(patch.cave + 0x10u, originalLabel));
        WriteU32(patch.cave + 0x14u, 0x818C0330u);                      // lwz r12, 0x330(r12)
        WriteU32(patch.cave + 0x18u, 0x2C0C0000u);                      // cmpwi r12, 0
        WriteU32(patch.cave + 0x1Cu, PpcBeq(patch.cave + 0x1Cu, flattenedLabel));
        WriteU32(patch.cave + 0x20u, patch.original);
        WriteU32(patch.cave + 0x24u, PpcBranch(patch.cave + 0x24u, returnAddr));
        WriteU32(patch.cave + 0x28u, PpcAddiR4R1(patch.flattenedStackOffset));
        WriteU32(patch.cave + 0x2Cu, PpcBranch(patch.cave + 0x2Cu, returnAddr));

        if (WriteU32(patch.address, branch)) {
            patch.applied = true;
            Log(L"GameTimingHooks patched orbit PPC at 0x" +
                std::to_wstring(patch.address) + L": " + patch.description + L".");
        }
    }
}

bool ShouldFlattenCombatPitch(uint32_t stateMgr, uint32_t player) {
    if (player < kMem1Start) {
        return false;
    }

    const uint32_t scanState = ReadU32(player + 0x330);
    if (scanState != 0) {
        return false;
    }

    const uint32_t orbitState = ReadU32(player + kOrbitStateOffset);
    const uint8_t held0 = ReadU8(stateMgr + kFinalInputOffset + 0x2c);
    const uint32_t scratchPlayer = ReadU32(kGunTargetHookScratch);
    const uint16_t scratchUid = ReadU16(kGunTargetHookScratch + 4);

    const bool buttonLock = (held0 & 0x04u) != 0;
    const bool scratchLock = scratchPlayer == player && scratchUid != 0xffffu;
    const bool orbitActive = orbitState != 0 && orbitState != kOrbitStateGrapple;
    return buttonLock || scratchLock || orbitActive;
}

void PatchFirstPersonPitchLoad() {
    if (!g_memBase || g_firstPersonPitchLoadPatch.applied) {
        return;
    }

    auto& patch = g_firstPersonPitchLoadPatch;
    const uint32_t current = ReadU32(patch.address);
    const uint32_t branch = PpcBranch(patch.address, patch.cave);
    if (current == branch) {
        patch.applied = true;
        Log(L"GameTimingHooks first-person pitch load patch already present at 0x" +
            std::to_wstring(patch.address) + L": " + patch.description);
        return;
    }

    if (current != patch.original) {
        if (current != 0 && !patch.loggedWaiting) {
            patch.loggedWaiting = true;
            Log(L"GameTimingHooks waiting to patch first-person pitch load at 0x" +
                std::to_wstring(patch.address) + L"; current instruction did not match.");
        }
        return;
    }

    const uint32_t zeroLabel = patch.cave + 0x18u;
    const uint32_t returnAddr = patch.address + 4u;

    WriteU32(patch.cave + 0x00u, 0x819E0330u);                  // lwz r12, 0x330(r30)
    WriteU32(patch.cave + 0x04u, 0x2C0C0000u);                  // cmpwi r12, 0
    WriteU32(patch.cave + 0x08u, PpcBeq(patch.cave + 0x08u, zeroLabel));
    WriteU32(patch.cave + 0x0Cu, patch.original);               // lfs f31, 0x3ec(r30)
    WriteU32(patch.cave + 0x10u, PpcBranch(patch.cave + 0x10u, returnAddr));
    WriteU32(patch.cave + 0x14u, 0x60000000u);                  // nop padding
    WriteU32(patch.cave + 0x18u, kLoadZeroToF31);
    WriteU32(patch.cave + 0x1Cu, PpcBranch(patch.cave + 0x1Cu, returnAddr));

    if (WriteU32(patch.address, branch)) {
        patch.applied = true;
        Log(L"GameTimingHooks patched first-person pitch load at 0x" +
            std::to_wstring(patch.address) + L": " + patch.description + L".");
    }
}

bool SetDynamicPatchState(const DynamicPpcPatch& patch, bool active) {
    const uint32_t current = ReadU32(patch.address);
    const uint32_t desired = active ? patch.replacement : patch.original;
    if (current == desired) {
        return true;
    }

    const uint32_t expected = active ? patch.original : patch.replacement;
    if (current != expected) {
        return false;
    }

    return WriteU32(patch.address, desired);
}

void UpdateCombatPitchPatchForLogicTick() {
    if (!g_installed.load() || !ResolveMemBase()) {
        return;
    }

    const uint32_t stateMgr = kStateManager;
    const uint32_t player = ReadU32(stateMgr + kPlayerOffset);
    const bool shouldBeActive = ShouldFlattenCombatPitch(stateMgr, player);

    bool allOk = true;
    for (const DynamicPpcPatch& patch : g_combatPitchPatches) {
        allOk = SetDynamicPatchState(patch, shouldBeActive) && allOk;
    }

    if (allOk && shouldBeActive != g_combatPitchPatchActive) {
        g_combatPitchPatchActive = shouldBeActive;
        const uint64_t now = GetTickCount64();
        if (LoggingEnabled() && now - g_lastCombatPitchPatchLogTick >= 100) {
            g_lastCombatPitchPatchLogTick = now;
            Log(std::wstring(L"GameTimingHooks combat pitch patch ") +
                (shouldBeActive ? L"enabled" : L"disabled"));
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
        if (!IsMem1Range(patch.address, 4)) {
            patch.lastSeen = 0xffffffffu;
            continue;
        }

        const uint32_t current = ReadU32(patch.address);
        patch.lastSeen = current;
        if (!patch.enabled) {
            patch.applied = 0;
            const auto original = g_appPatchOriginals.find(patch.address);
            if (original != g_appPatchOriginals.end() && current == patch.value &&
                WriteU32(patch.address, original->second)) {
                patch.lastSeen = original->second;
                Log(L"GameTimingHooks restored disabled app patch[" + std::to_wstring(i) +
                    L"] address=0x" + std::to_wstring(patch.address));
            }
            continue;
        }

        if (patch.applied) {
            continue;
        }

        if (current == patch.value) {
            patch.applied = 1;
            continue;
        }

        if (patch.requireOriginal && current != patch.original) {
            continue;
        }

        if (g_appPatchOriginals.find(patch.address) == g_appPatchOriginals.end())
            g_appPatchOriginals.emplace(patch.address, current);

        if (WriteU32(patch.address, patch.value)) {
            patch.applied = 1;
            Log(L"GameTimingHooks applied app patch[" + std::to_wstring(i) +
                L"] address=0x" + std::to_wstring(patch.address) +
                L" value=0x" + std::to_wstring(patch.value));
        }
    }
}

void ApplyInputBindingRequest(SharedState* state) {
    if (!state || state->inputBindingRequestGeneration == 0) {
        return;
    }

    const uint64_t now = GetTickCount64();
    if (state->inputBindingAppliedGeneration == state->inputBindingRequestGeneration &&
        state->inputBindingStatus == 2u) {
        return;
    }

    if (state->inputBindingRequestGeneration == g_lastInputBindingRequestGeneration &&
        now - g_lastInputBindingAttemptTick < 1000) {
        return;
    }

    if (state->inputBindingRequestGeneration != g_lastInputBindingRequestGeneration) {
        g_inputBindingAttemptCount = 0;
    } else if (g_inputBindingAttemptCount >= 3) {
        state->inputBindingStatus = 4u;
        return;
    }

    g_lastInputBindingRequestGeneration = state->inputBindingRequestGeneration;
    g_lastInputBindingAttemptTick = now;
    ++g_inputBindingAttemptCount;
    state->inputBindingStatus = 1u;

    if (g_lastSaveProbeRequestGeneration != state->inputBindingRequestGeneration) {
        g_lastSaveProbeRequestGeneration = state->inputBindingRequestGeneration;
        TriggerDolphinPadConfigSaveProbe();
    }

    const bool wrote_setup = ApplyPrimedGunDolphinSetupFromHook();
    Log(std::wstring(L"PrimedGun Dolphin setup deployment after save probe ") +
        (wrote_setup ? L"wrote config." : L"found no writable config targets."));

    if (wrote_setup && VerifyPrimedGunInputBindings()) {
        state->inputBindingAppliedGeneration = state->inputBindingRequestGeneration;
        state->inputBindingStatus = 2u;
        Log(L"GameTimingHooks verified PrimedGun input binding request generation=" +
            std::to_wstring(g_lastInputBindingRequestGeneration) + L".");
        return;
    }

    state->inputBindingStatus = 3u;
    Log(L"GameTimingHooks will retry PrimedGun input binding request generation=" +
        std::to_wstring(g_lastInputBindingRequestGeneration) +
        L"; Dolphin active config files were not verified yet. attempt=" +
        std::to_wstring(g_inputBindingAttemptCount) + L"/3.");
}

void ApplyStartupDolphinSetupOnce() {
    if (g_startupSetupAttempted)
        return;

    const uint64_t now = GetTickCount64();
    if (g_startupSetupAttemptCount >= 3)
        return;
    if (g_startupSetupAttemptCount > 0 && now - g_lastStartupSetupAttemptTick < 1000)
        return;

    if (!TriggerDolphinPadConfigSaveProbe())
        return;

    ++g_startupSetupAttemptCount;
    g_lastStartupSetupAttemptTick = now;
    const bool wrote_setup = ApplyPrimedGunDolphinSetupFromHook();
    Log(std::wstring(L"PrimedGun Dolphin startup setup deployment ") +
        (wrote_setup ? L"wrote config." : L"found no writable config targets."));
    if (wrote_setup && VerifyPrimedGunInputBindings()) {
        g_startupSetupAttempted = true;
        Log(L"PrimedGun Dolphin startup setup verified.");
        return;
    }

    Log(L"PrimedGun Dolphin startup setup will retry; binding verification failed. attempt=" +
        std::to_wstring(g_startupSetupAttemptCount) + L"/3.");
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

std::wstring Hex32(uint32_t value);
std::wstring FloatText(float value);

bool ResolveActiveCameraTransform(uint32_t stateMgr, uint32_t& camera, uint32_t& cameraXf) {
    camera = 0;
    cameraXf = 0;
    const uint32_t cameraManager = ReadU32(stateMgr + kCameraManagerOffset);
    if (cameraManager < kMem1Start) {
        return false;
    }

    const uint16_t cameraUid = static_cast<uint16_t>(ReadU32(cameraManager) >> 16);
    if (cameraUid == 0xffffu) {
        return false;
    }

    const uint32_t objectList = ReadU32(stateMgr + kObjectListOffset);
    if (objectList < kMem1Start) {
        return false;
    }

    camera = ReadU32(objectList + ((cameraUid & 0x3ffu) << 3) + 4);
    if (camera < kMem1Start) {
        return false;
    }

    cameraXf = camera + kTransformOffset;
    return true;
}

std::wstring Hex8(uint8_t value) {
    wchar_t buffer[8] = {};
    swprintf_s(buffer, L"0x%02X", static_cast<unsigned int>(value));
    return buffer;
}

std::wstring Hex16(uint16_t value) {
    wchar_t buffer[10] = {};
    swprintf_s(buffer, L"0x%04X", static_cast<unsigned int>(value));
    return buffer;
}

std::wstring Hex32(uint32_t value) {
    wchar_t buffer[16] = {};
    swprintf_s(buffer, L"0x%08X", value);
    return buffer;
}

std::wstring FloatText(float value) {
    wchar_t buffer[32] = {};
    if (std::isfinite(value)) {
        swprintf_s(buffer, L"%.5f", static_cast<double>(value));
    } else {
        swprintf_s(buffer, L"nan");
    }
    return buffer;
}

void TraceOrbitLockForLogicTick() {
    if (!LoggingEnabled() || !g_installed.load() || !ResolveMemBase()) {
        return;
    }

    constexpr uint32_t kTraceFrames = 180u;
    const uint32_t stateMgr = kStateManager;
    const uint32_t player = ReadU32(stateMgr + kPlayerOffset);
    if (player < kMem1Start) {
        g_traceLastLockHeld = false;
        g_traceFramesRemaining = 0;
        g_traceHavePitch = false;
        return;
    }

    const uint8_t held0 = ReadU8(stateMgr + kFinalInputOffset + 0x2c);
    const uint8_t pressed0 = ReadU8(stateMgr + kFinalInputOffset + 0x2e);
    const uint32_t scratchPlayer = ReadU32(kGunTargetHookScratch);
    const uint16_t scratchUid = ReadU16(kGunTargetHookScratch + 4);
    const bool buttonLock = (held0 & 0x04u) != 0;
    const bool scratchLock = scratchPlayer == player && scratchUid != 0xffffu;
    const bool lockHeld = buttonLock || scratchLock;
    const uint32_t orbitState = ReadU32(player + kOrbitStateOffset);
    const uint32_t scanState = ReadU32(player + 0x330);
    uint32_t camera = 0;
    uint32_t cameraXf = 0;
    const bool haveCamera = ResolveActiveCameraTransform(stateMgr, camera, cameraXf);
    const float cameraFwdZ = haveCamera ? ReadFloat(cameraXf + 0x18) : 0.0f;
    const bool cameraPitchJump = haveCamera && g_traceHaveCamera &&
        std::fabs(cameraFwdZ - g_traceLastCameraFwdZ) > 0.02f;
    const bool orbitChanged = orbitState != g_traceLastOrbitState;
    const bool scanChanged = scanState != g_traceLastScanState;
    const bool buttonChanged = buttonLock != g_traceLastButtonLock;
    const float pitch = ReadFloat(player + kFirstPersonPitchOffset);
    const bool pitchJump = g_traceHavePitch && std::isfinite(pitch) &&
        std::fabs(pitch - g_traceLastPitch) > 0.01f;

    if (lockHeld != g_traceLastLockHeld || orbitChanged || scanChanged || buttonChanged ||
        pitchJump || cameraPitchJump) {
        if (lockHeld != g_traceLastLockHeld) {
            ++g_traceSequence;
            g_traceStartTick = GetTickCount64();
            g_traceLastLogTick = 0;
            Log(std::wstring(L"OrbitTrace transition seq=") +
                std::to_wstring(g_traceSequence) +
                L" lock=" + std::to_wstring(lockHeld ? 1 : 0) +
                L" buttonLock=" + std::to_wstring(buttonLock ? 1 : 0) +
                L" scratchLock=" + std::to_wstring(scratchLock ? 1 : 0) +
                L" orbit=" + std::to_wstring(orbitState) +
                L" scan=" + std::to_wstring(scanState) +
                L" pitch=" + FloatText(pitch) +
                L" camFwdZ=" + FloatText(cameraFwdZ));
        } else if (orbitChanged || scanChanged || buttonChanged || cameraPitchJump) {
            if (g_traceFramesRemaining == 0) {
                ++g_traceSequence;
                g_traceStartTick = GetTickCount64();
                g_traceLastLogTick = 0;
            }
            Log(std::wstring(L"OrbitTrace state seq=") +
                std::to_wstring(g_traceSequence) +
                L" buttonLock=" + std::to_wstring(buttonLock ? 1 : 0) +
                L" scratchLock=" + std::to_wstring(scratchLock ? 1 : 0) +
                L" orbit=" + std::to_wstring(orbitState) +
                L" scan=" + std::to_wstring(scanState) +
                L" camFwdZ=" + FloatText(cameraFwdZ));
        }
        g_traceFramesRemaining = std::max(g_traceFramesRemaining, kTraceFrames);
    }
    g_traceLastLockHeld = lockHeld;
    g_traceLastOrbitState = orbitState;
    g_traceLastScanState = scanState;
    g_traceLastButtonLock = buttonLock;
    g_traceHavePitch = std::isfinite(pitch);
    g_traceLastPitch = pitch;
    g_traceHaveCamera = haveCamera && std::isfinite(cameraFwdZ);
    g_traceLastCameraFwdZ = cameraFwdZ;

    if (g_traceFramesRemaining == 0) {
        return;
    }
    --g_traceFramesRemaining;

    const uint64_t now = GetTickCount64();
    if (g_traceLastLogTick != 0 && now - g_traceLastLogTick < 8) {
        return;
    }
    g_traceLastLogTick = now;

    const uint64_t elapsed = now - g_traceStartTick;
    const uint32_t objectList = ReadU32(stateMgr + kObjectListOffset);
    uint32_t scratchObj = 0;
    if (objectList >= kMem1Start && scratchUid != 0xffffu) {
        scratchObj = ReadU32(objectList + ((scratchUid & 0x3ffu) << 3) + 4);
    }

    Log(std::wstring(L"OrbitTrace seq=") + std::to_wstring(g_traceSequence) +
        L" tMs=" + std::to_wstring(elapsed) +
        L" lock=" + std::to_wstring(lockHeld ? 1 : 0) +
        L" buttonLock=" + std::to_wstring(buttonLock ? 1 : 0) +
        L" scratchLock=" + std::to_wstring(scratchLock ? 1 : 0) +
        L" held0=" + Hex8(held0) +
        L" press0=" + Hex8(pressed0) +
        L" orbit=" + std::to_wstring(orbitState) +
        L" scan=" + std::to_wstring(scanState) +
        L" cam=" + std::to_wstring(ReadU32(player + 0x2f4)) +
        L" morph=" + std::to_wstring(ReadU32(player + 0x2f8)) +
        L" move=" + std::to_wstring(ReadU32(player + 0x258)) +
        L" free=" + Hex32(ReadU32(player + 0x3dc)) +
        L" pitch=" + FloatText(pitch) +
        L" pitchVel=" + FloatText(ReadFloat(player + kFirstPersonPitchVelOffset)) +
        L" yaw=" + FloatText(ReadFloat(player + 0x3e4)) +
        L" yawVel=" + FloatText(ReadFloat(player + 0x3e8)) +
        L" camera=" + Hex32(camera) +
        L" camFwdZ=" + FloatText(cameraFwdZ) +
        L" camUpZ=" + FloatText(haveCamera ? ReadFloat(cameraXf + 0x28) : 0.0f) +
        L" camPos=" + FloatText(haveCamera ? ReadFloat(cameraXf + 0x0c) : 0.0f) +
        L"," + FloatText(haveCamera ? ReadFloat(cameraXf + 0x1c) : 0.0f) +
        L"," + FloatText(haveCamera ? ReadFloat(cameraXf + 0x2c) : 0.0f) +
        L" f29c=" + FloatText(ReadFloat(player + 0x29c)) +
        L" b374=" + std::to_wstring(static_cast<unsigned int>(ReadU8(player + 0x374))) +
        L" b3dc=" + std::to_wstring(static_cast<unsigned int>(ReadU8(player + 0x3dc))) +
        L" b3dd=" + std::to_wstring(static_cast<unsigned int>(ReadU8(player + 0x3dd))) +
        L" scratchPlayer=" + Hex32(scratchPlayer) +
        L" scratchUid=" + Hex16(scratchUid) +
        L" scratchObj=" + Hex32(scratchObj) +
        L" ppcE7B4=" + Hex32(ReadU32(0x8000E7B4u)) +
        L" ppcE808=" + Hex32(ReadU32(0x8000E808u)) +
        L" ppcE83C=" + Hex32(ReadU32(0x8000E83Cu)) +
        L" ppcF050=" + Hex32(ReadU32(0x8000F050u)) +
        L" ppcEB44=" + Hex32(ReadU32(0x8000EB44u)) +
        L" ppcEBEC=" + Hex32(ReadU32(0x8000EBECu)) +
        L" ppcEF44=" + Hex32(ReadU32(0x8000EF44u)) +
        L" ppcEFF4=" + Hex32(ReadU32(0x8000EFF4u)) +
        L" ppcF128=" + Hex32(ReadU32(0x8000F128u)) +
        L" ppcF184=" + Hex32(ReadU32(0x8000F184u)) +
        L" ppcF50C=" + Hex32(ReadU32(0x8000F50Cu)) +
        L" ppcF5C0=" + Hex32(ReadU32(0x8000F5C0u)));
}

} // namespace

bool Install() {
    if (g_installed.exchange(true)) {
        return true;
    }
    Log(L"GameTimingHooks installed.");
    if (!g_dumpedDolphinConfigProbe) {
        g_dumpedDolphinConfigProbe = true;
        ProbeDolphinConfigSystems();
    }
    InitializeSymbols();
    InstallConfigFileTraceHooks();
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

void PollFast(SharedState*) {
    UpdateCombatPitchPatchForLogicTick();
    TraceOrbitLockForLogicTick();
}

void Poll(SharedState* state) {
    if (g_installed.load()) {
        g_activeSharedState = state;
        const uint64_t now = GetTickCount64();
        if (now - g_lastConfigFileTracePatchTick >= 2000) {
            g_lastConfigFileTracePatchTick = now;
            InstallConfigFileTraceHooks();
        }
        ProbeDolphinConfigFilesOccasionally();
        ApplyStartupDolphinSetupOnce();
        ApplyInputBindingRequest(state);
        if (ResolveMemBase()) {
            DumpPlayerOrbitCodeOnce();
            DumpFirstPersonCameraCodeOnce();
            ApplySharedPatches(state);
            PatchOrbitCode();
        }
    }
}

void Shutdown() {
    if (ResolveMemBase()) {
        for (const DynamicPpcPatch& patch : g_combatPitchPatches) {
            SetDynamicPatchState(patch, false);
        }
    }
    if (g_installed.exchange(false)) {
        if (g_symbolsInitialized) {
            SymCleanup(GetCurrentProcess());
            g_symbolsInitialized = false;
        }
        Log(L"GameTimingHooks shutdown.");
    }
}

} // namespace PrimedGun::Hook::GameTimingHooks
