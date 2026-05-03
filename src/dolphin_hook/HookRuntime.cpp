#include "HookRuntime.h"

#include "GraphicsHooks.h"
#include "GameTimingHooks.h"
#include "Ipc.h"
#include "JitHooks.h"

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
    JitHooks::Install();
    Log(L"PrimedGun hook runtime is active inside Dolphin.");

    uint64_t lastMaintenanceTick = 0;
    while (g_running.load()) {
        GameTimingHooks::SuppressLockCameraPitchForLogicTick();

        const uint64_t now = GetTickCount64();
        if (now - lastMaintenanceTick >= 250) {
            lastMaintenanceTick = now;
            shared.Heartbeat();
            GameTimingHooks::Poll(shared.Get());
            GraphicsHooks::PollBackendModules();
            JitHooks::Poll();

            if (SharedState* state = shared.Get()) {
                state->game.frameIndex++;
            }
        }

        Sleep(1);
    }

    GraphicsHooks::Shutdown();
    GameTimingHooks::Shutdown();
    JitHooks::Shutdown();
    Log(L"PrimedGun hook runtime stopped.");
    return 0;
}

} // namespace

void Log(std::wstring_view message) {
    if (g_log.is_open()) {
        g_log << message << L"\n";
        g_log.flush();
    }
}

bool StartRuntime(HMODULE) {
    if (g_running.exchange(true)) {
        return true;
    }

    const fs::path logDir = LocalAppDataPath() / L"PrimedGun";
    std::error_code ec;
    fs::create_directories(logDir, ec);
    g_log.open(logDir / L"PrimedGun_DolphinHook.log", std::ios::app);
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
