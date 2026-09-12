// Copyright 2026 PrimedGun Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

namespace PrimedGun
{
struct RuntimeSettings;

// Path of the INI that persists RuntimeSettings for the current user.
std::string GetSettingsFilePath();

// Applies the persisted settings on top of the current runtime settings, then marks them loaded.
// When no settings file exists yet, DolphinQt's legacy Qt.ini [primedgun] block is imported
// instead, so desktop users keep the calibration they saved before persistence moved into Core.
void LoadRuntimeSettings();

// Loads the persisted settings unless LoadRuntimeSettings() already ran. Frontends that own a
// settings UI load explicitly at startup so the widgets show saved values before boot; this is
// the fallback for the ones that don't, and it must not clobber unsaved UI edits.
void EnsureRuntimeSettingsLoaded();

// Writes the given settings to GetSettingsFilePath(). Returns false if the file could not be
// written.
bool SaveRuntimeSettings(const RuntimeSettings& settings);

// Applies the settings stored in another install's INI to the runtime and persists them here.
// Accepts this file's own layout as well as the Qt.ini groups older builds wrote. Whether the mod
// is enabled stays owned by the current install, matching what the transfer UI imports.
bool ImportRuntimeSettings(const std::string& path);

// Writes the current settings if the in-headset menu asked for a save since the last call, and
// acknowledges the request. Called once per frame from OnFrameEnd so the VR menu persists on
// every platform rather than only the ones with a Qt host polling for it.
void ProcessPendingVrSettingsSave();
}  // namespace PrimedGun
