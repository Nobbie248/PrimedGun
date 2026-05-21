#include "HookRuntime.h"

#include "GraphicsHooks.h"
#include "GameTimingHooks.h"
#include "DolphinXrBridge.h"
#include "Ipc.h"
#include "OpenXrHooks.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string_view>

namespace fs = std::filesystem;

namespace PrimedGun::Hook {

void Log(std::wstring_view message);

namespace {

std::atomic<bool> g_running = false;
HANDLE g_thread = nullptr;
std::wofstream g_log;
bool g_logging_enabled = false;

fs::path LocalAppDataPath() {
    wchar_t buffer[MAX_PATH] = {};
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, static_cast<DWORD>(MAX_PATH));
    if (len == 0 || len >= MAX_PATH) {
        return fs::temp_directory_path();
    }
    return fs::path(buffer);
}

DWORD WINAPI RuntimeThread(void*) {
    SharedStateView shared;
    if (!shared.Open()) {
        Log(L"Failed to open shared PrimedGun state.");
        return 1;
    }

    GameTimingHooks::Install();
    GraphicsHooks::Install();
    OpenXrHooks::InstallIfAvailable(shared.Get());
    Log(L"PrimedGun hook runtime is active inside Dolphin.");

    uint64_t lastMaintenanceTick = 0;
    while (g_running.load()) {
        const uint64_t now = GetTickCount64();
        GameTimingHooks::PollFast(shared.Get());
        if (now - lastMaintenanceTick >= 250) {
            lastMaintenanceTick = now;
            shared.Heartbeat();
            GraphicsHooks::PollBackendModules(shared.Get());
            GameTimingHooks::Poll(shared.Get());
            OpenXrHooks::Poll(shared.Get());

            if (SharedState* state = shared.Get()) {
                state->hookStatusFlags |= HookStatusDllAlive;
                state->game.frameIndex++;
            }
        }

        Sleep(1);
    }

    OpenXrHooks::Shutdown();
    GraphicsHooks::Shutdown();
    GameTimingHooks::Shutdown();
    Log(L"PrimedGun hook runtime stopped.");
    return 0;
}

} // namespace

bool LoggingEnabled() {
    return g_logging_enabled;
}

void Log(std::wstring_view message) {
    if (g_logging_enabled && g_log.is_open()) {
        g_log << message << L"\n";
        g_log.flush();
    }
}

bool StartRuntime(HMODULE) {
    if (g_running.exchange(true)) {
        return true;
    }

    wchar_t log_value[16] = {};
    const DWORD log_len = GetEnvironmentVariableW(L"PRIMEDGUN_ENABLE_LOGS", log_value,
                                                  static_cast<DWORD>(std::size(log_value)));
    g_logging_enabled = log_len > 0 && log_len < std::size(log_value) && log_value[0] == L'1';
    if (g_logging_enabled) {
        const fs::path logDir = LocalAppDataPath() / L"PrimedGun";
        std::error_code ec;
        fs::create_directories(logDir, ec);
        g_log.open(logDir / L"PrimedGun_DolphinHook.log", std::ios::app);
    }
    Log(L"PrimedGun_DolphinHook loaded.");

    g_thread = CreateThread(nullptr, 0, RuntimeThread, nullptr, 0, nullptr);
    if (!g_thread) {
        g_running = false;
        Log(L"CreateThread failed.");
        return false;
    }
    return true;
}

void StopRuntime() {
    if (!g_running.exchange(false)) {
        return;
    }

    if (g_thread) {
        WaitForSingleObject(g_thread, 3000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    Log(L"PrimedGun_DolphinHook unloading.");
    if (g_log.is_open()) {
        g_log.close();
    }
}

} // namespace PrimedGun::Hook
