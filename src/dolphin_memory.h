#pragma once
#include <Windows.h>
#include <TlHelp32.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <stdexcept>

// Dolphin emulates the GameCube/Wii memory starting at a base address in its process.
// We only attach to the bundled runtime under PrimedGun.exe's sibling "core" folder.

class DolphinMemory {
public:
    DolphinMemory() = default;
    ~DolphinMemory() { disconnect(); }

    // ── Connect ───────────────────────────────────────────
    bool connect() {
        disconnect();

        dolphin_pid_ = find_process("Dolphin.exe");
        if (!dolphin_pid_) { status_ = "Bundled core not found"; return false; }

        handle_ = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION,
                              FALSE, dolphin_pid_);
        if (!handle_) {
            status_ = "Failed to open Dolphin process (run as admin?)";
            return false;
        }

        mem_base_ = find_emu_base();
        if (!mem_base_) {
            status_ = "Could not find emulated RAM base (game not running?)";
            CloseHandle(handle_);
            handle_ = nullptr;
            return false;
        }

        status_ = "Connected";
        return true;
    }

    bool refresh_memory_base() {
        if (!handle_) {
            return connect();
        }

        DWORD exit_code = 0;
        if (!GetExitCodeProcess(handle_, &exit_code) || exit_code != STILL_ACTIVE) {
            disconnect();
            return connect();
        }

        mem_base_ = find_emu_base();
        status_ = mem_base_ ? "Connected" : "Could not find emulated RAM base (game not running?)";
        return mem_base_ != 0;
    }

    void disconnect() {
        if (handle_) { CloseHandle(handle_); handle_ = nullptr; }
        dolphin_pid_ = 0;
        mem_base_ = 0;
        status_ = "Disconnected";
    }

    bool is_connected() const { return handle_ != nullptr && mem_base_ != 0; }
    uint64_t get_mem_base() const { return mem_base_; }
    const std::string& status() const { return status_; }

    // ── Read/Write ────────────────────────────────────────
    // addr = GameCube effective address (e.g. 0x8045A1A8)
    float read_float(uint32_t addr) const {
        uint32_t raw = read_u32(addr);
        // GameCube is big-endian, PC is little-endian — swap bytes
        float val;
        memcpy(&val, &raw, 4);
        return val;
    }

    uint32_t read_u32(uint32_t addr) const {
        uint32_t val = 0;
        read_bytes(addr, &val, 4);
        return _byteswap_ulong(val);
    }

    uint16_t read_u16(uint32_t addr) const {
        uint16_t val = 0;
        read_bytes(addr, &val, 2);
        return _byteswap_ushort(val);
    }

    bool read_block(uint32_t addr, void* buf, size_t size) const {
        if (!handle_ || !mem_base_ || !is_mem1_range(addr, size)) return false;
        SIZE_T bytes_read = 0;
        return ReadProcessMemory(handle_, (LPCVOID)gc_to_host(addr), buf, size, &bytes_read) &&
               bytes_read == size;
    }

    uint8_t read_u8(uint32_t addr) const {
        uint8_t val = 0;
        read_bytes(addr, &val, 1);
        return val;
    }

    void write_float(uint32_t addr, float val) const {
        uint32_t raw;
        memcpy(&raw, &val, 4);
        raw = _byteswap_ulong(raw);
        write_bytes(addr, &raw, 4);
    }

    void write_u32(uint32_t addr, uint32_t val) const {
        val = _byteswap_ulong(val);
        write_bytes(addr, &val, 4);
    }

    void write_u16(uint32_t addr, uint16_t val) const {
        val = _byteswap_ushort(val);
        write_bytes(addr, &val, 2);
    }

    void write_u8(uint32_t addr, uint8_t val) const {
        write_bytes(addr, &val, 1);
    }

    // Dereference a pointer in game memory
    uint32_t deref(uint32_t ptr_addr) const {
        return read_u32(ptr_addr);
    }

private:
    HANDLE   handle_     = nullptr;
    DWORD    dolphin_pid_ = 0;
    uintptr_t mem_base_  = 0;
    std::string status_  = "Disconnected";

    static constexpr uint32_t kMem1Start = 0x80000000u;
    static constexpr uint32_t kMem1Size = 0x01800000u;
    static constexpr uint32_t kMem1End = kMem1Start + kMem1Size;

    static bool is_mem1_range(uint32_t gc_addr, size_t size) {
        if (gc_addr < kMem1Start || gc_addr >= kMem1End)
            return false;
        const uint64_t end = static_cast<uint64_t>(gc_addr) + static_cast<uint64_t>(size);
        return end <= kMem1End;
    }

    // Convert GC address to host address
    uintptr_t gc_to_host(uint32_t gc_addr) const {
        // GC RAM starts at 0x80000000
        uint32_t offset = gc_addr - kMem1Start;
        return mem_base_ + offset;
    }

    void read_bytes(uint32_t gc_addr, void* buf, size_t size) const {
        if (!handle_ || !mem_base_ || !is_mem1_range(gc_addr, size)) return;
        SIZE_T bytes_read;
        ReadProcessMemory(handle_, (LPCVOID)gc_to_host(gc_addr), buf, size, &bytes_read);
    }

    void write_bytes(uint32_t gc_addr, const void* buf, size_t size) const {
        if (!handle_ || !mem_base_ || !is_mem1_range(gc_addr, size)) return;
        SIZE_T bytes_written;
        WriteProcessMemory(handle_, (LPVOID)gc_to_host(gc_addr), buf, size, &bytes_written);
    }

    static std::string lower_ascii(std::string text) {
        for (char& ch : text) {
            if (ch >= 'A' && ch <= 'Z')
                ch = static_cast<char>(ch - 'A' + 'a');
            else if (ch == '/')
                ch = '\\';
        }
        return text;
    }

    static std::string bundled_core_prefix() {
        char module_path[MAX_PATH] = {};
        const DWORD len = GetModuleFileNameA(nullptr, module_path, MAX_PATH);
        if (len == 0 || len >= MAX_PATH)
            return {};

        std::string dir(module_path, len);
        const size_t slash = dir.find_last_of("\\/");
        if (slash == std::string::npos)
            return {};
        dir.resize(slash + 1);
        dir += "core\\";
        return lower_ascii(dir);
    }

    static bool process_is_bundled_core(DWORD pid) {
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process)
            return false;

        char path[MAX_PATH] = {};
        DWORD size = MAX_PATH;
        const BOOL ok = QueryFullProcessImageNameA(process, 0, path, &size);
        CloseHandle(process);
        if (!ok || size == 0)
            return false;

        const std::string core = bundled_core_prefix();
        const std::string candidate = lower_ascii(std::string(path, size));
        return !core.empty() &&
               candidate.size() >= core.size() &&
               candidate.compare(0, core.size(), core) == 0;
    }

    // Find the bundled Dolphin/Core process, return PID.
    static DWORD find_process(const char* name) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return 0;
        PROCESSENTRY32 pe = { sizeof(pe) };
        DWORD pid = 0;
        if (Process32First(snap, &pe)) {
            do {
                const bool name_matches =
                    _stricmp(pe.szExeFile, name) == 0 ||
                    (_stricmp(name, "Dolphin.exe") == 0 &&
                     (_stricmp(pe.szExeFile, "Core.exe") == 0 ||
                      _stricmp(pe.szExeFile, "Dolphin.exe") == 0));
                if (name_matches && process_is_bundled_core(pe.th32ProcessID)) {
                    pid = pe.th32ProcessID;
                    break;
                }
            } while (Process32Next(snap, &pe));
        }
        CloseHandle(snap);
        return pid;
    }

    static bool is_readable_protect(DWORD protect) {
        if (protect & (PAGE_GUARD | PAGE_NOACCESS))
            return false;
        protect &= 0xff;
        return protect == PAGE_READONLY ||
               protect == PAGE_READWRITE ||
               protect == PAGE_WRITECOPY ||
               protect == PAGE_EXECUTE_READ ||
               protect == PAGE_EXECUTE_READWRITE ||
               protect == PAGE_EXECUTE_WRITECOPY;
    }

    bool read_process_u32_raw(uintptr_t address, uint32_t& value) const {
        SIZE_T bytes_read = 0;
        uint32_t raw = 0;
        if (!ReadProcessMemory(handle_, reinterpret_cast<LPCVOID>(address), &raw, sizeof(raw), &bytes_read) ||
            bytes_read != sizeof(raw)) {
            return false;
        }
        value = _byteswap_ulong(raw);
        return true;
    }

    bool looks_like_mem1_candidate(uintptr_t base, size_t region_size) const {
        if (region_size < 0x01800000)
            return false;

        uint32_t game_id = 0;
        uint32_t game_rev = 0;
        if (!read_process_u32_raw(base, game_id) ||
            !read_process_u32_raw(base + 4, game_rev)) {
            return false;
        }

        // GM8E + 01/revision lives at the start of MEM1 once Prime is loaded.
        return game_id == 0x474D3845u && (game_rev >> 16) == 0x3031u;
    }

    // Find Dolphin's emulated RAM base. Prefer the old stable mapped 32/64 MB
    // regions, then validate wider/private candidates by the disc header at
    // MEM1+0 when this Dolphin build exposes RAM differently.
    uintptr_t find_emu_base() const {
        MEMORY_BASIC_INFORMATION mbi;
        uintptr_t addr = 0;
        uintptr_t fallback = 0;

        while (VirtualQueryEx(handle_, (LPCVOID)addr, &mbi, sizeof(mbi))) {
            if (mbi.State == MEM_COMMIT && mbi.Type == MEM_MAPPED &&
                (mbi.RegionSize == 0x4000000 || mbi.RegionSize == 0x2000000)) {
                uintptr_t base = (uintptr_t)mbi.BaseAddress;
                if (looks_like_mem1_candidate(base, mbi.RegionSize))
                    return base;
                if (!fallback)
                    fallback = base;
            }
            if (addr + mbi.RegionSize <= addr) break;
            addr += mbi.RegionSize;
        }

        addr = 0;
        while (VirtualQueryEx(handle_, (LPCVOID)addr, &mbi, sizeof(mbi))) {
            if (mbi.State == MEM_COMMIT && is_readable_protect(mbi.Protect) &&
                mbi.RegionSize >= 0x01800000 &&
                (mbi.Type == MEM_MAPPED || mbi.Type == MEM_PRIVATE)) {
                uintptr_t base = (uintptr_t)mbi.BaseAddress;
                if (looks_like_mem1_candidate(base, mbi.RegionSize))
                    return base;
            }
            if (addr + mbi.RegionSize <= addr) break;
            addr += mbi.RegionSize;
        }

        return fallback;
    }
};
