// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <thread>

#include "Common/Flag.h"

// Polls HotkeyManagerEmu on a dedicated low-priority background thread and dispatches
// fired hotkeys to the corresponding Core/Config/State/OpenXR APIs. Mirrors PC's
// Source/Core/DolphinQt/HotkeyScheduler but with no Qt dependency and a smaller, Android-
// relevant action set (see HotkeyDispatcher.cpp for the dispatch table).
//
// Lifetime: Start() must be called once after the boot has settled (see MainAndroid::Run).
// Stop() must be called before Core::Shutdown so the thread isn't reading a torn-down
// g_controller_interface.
class HotkeyDispatcher
{
public:
  // Called from the dispatcher thread when HK_STOP fires. The implementation is
  // expected to wake the main Run() loop (e.g. by setting s_update_main_frame_event)
  // so it observes the Core::Stop and exits cleanly. Kept as a callback so this header
  // doesn't reach into MainAndroid's internal symbols.
  using StopRequestCallback = std::function<void()>;

  HotkeyDispatcher() = default;
  ~HotkeyDispatcher();

  HotkeyDispatcher(const HotkeyDispatcher&) = delete;
  HotkeyDispatcher& operator=(const HotkeyDispatcher&) = delete;

  // No-op if already running.
  void Start(StopRequestCallback on_stop_requested);

  // Signals the thread to exit and joins. Safe to call when not running.
  void Stop();

  bool IsRunning() const { return m_thread.joinable(); }

private:
  void Run();

  std::thread m_thread;
  Common::Flag m_stop_requested;
  StopRequestCallback m_on_stop_requested;

  // Counts consecutive poll ticks that HK_ANDROID_RETURN_TO_MAIN_MENU has been held.
  // Fires once the threshold (~3 s) is reached, then resets to prevent re-firing.
  int m_return_menu_hold_ticks = 0;
};
