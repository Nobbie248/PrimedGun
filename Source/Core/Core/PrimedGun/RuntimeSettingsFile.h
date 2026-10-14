// Copyright 2026 PrimedGun Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <string>

#include "Core/PrimedGun/NativeRuntime.h"

namespace PrimedGun
{
// The in-VR menu settings as a [primedgun] section of an ini file, with the key names and the
// loading rules of the Qt frontend's "primedgun/<key>" QSettings entries. Used by the NoGUI build.

// Starts from `settings`, overrides every key found in the file. Returns false if the file does
// not exist (or has no readable [primedgun] section); `settings` is then only normalised.
bool LoadRuntimeSettingsFile(const std::string& path, RuntimeSettings* settings);

// Writes every key into the [primedgun] section, keeping other sections and keys of the file.
bool SaveRuntimeSettingsFile(const std::string& path, const RuntimeSettings& settings);

// Host persistence (NoGUI): while enabled, the VR runtime saves the settings to `path` when the
// in-VR menu closes or Save is pressed, and calls `exit_game` (from the CPU thread) when the menu's
// EXIT GAME is confirmed, instead of leaving both requests for a Qt timer to consume.
void EnableHostPersistence(const std::string& path, std::function<void()> exit_game);
bool HostPersistenceEnabled();
// Saves the current runtime settings to the enabled path. Returns true on success.
bool SaveHostPersistence(const char* reason);
void RequestHostExit();
}  // namespace PrimedGun
