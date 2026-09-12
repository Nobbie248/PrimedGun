// Copyright 2026 PrimedGun Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PrimedGun/Settings.h"

#include <mutex>
#include <string>

#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Common/Logging/Log.h"

#include "Core/PrimedGun/NativeRuntime.h"
#include "Core/PrimedGun/SettingsVisitor.h"

namespace PrimedGun
{
namespace
{
constexpr char SETTINGS_FILE_NAME[] = "PrimedGun.ini";
constexpr char SETTINGS_SECTION[] = "Runtime";

// DolphinQt stored these in Qt.ini's [primedgun] group before persistence moved into Core, and
// the key names are unchanged so that file can still be imported. Nothing else reads it.
constexpr char LEGACY_SETTINGS_FILE_NAME[] = "Qt.ini";
constexpr char LEGACY_SETTINGS_SECTION[] = "primedgun";
// Even older builds used this group name, before the project was renamed.
constexpr char OLDEST_SETTINGS_SECTION[] = "primegun";

std::mutex s_load_mutex;
bool s_loaded = false;

// Reads one INI section into a RuntimeSettings, leaving members without a stored key alone so
// they keep the struct default. Returns false when the section is missing entirely.
bool ApplySectionToSettings(const Common::IniFile& ini, std::string_view section_name,
                            RuntimeSettings* settings)
{
  const Common::IniFile::Section* section = ini.GetSection(section_name);
  if (section == nullptr)
    return false;

  VisitPersistedSettings(*settings, [section](const char* key, auto& value) {
    std::remove_reference_t<decltype(value)> parsed{};
    if (section->Get(key, &parsed, value))
      value = parsed;
  });
  return true;
}

// Reads whichever section an INI happens to use, so a file written by any past build still loads.
bool ApplyAnyKnownSectionToSettings(const Common::IniFile& ini, RuntimeSettings* settings)
{
  for (const char* section : {SETTINGS_SECTION, LEGACY_SETTINGS_SECTION, OLDEST_SETTINGS_SECTION})
  {
    if (ApplySectionToSettings(ini, section, settings))
      return true;
  }
  return false;
}
}  // namespace

std::string GetSettingsFilePath()
{
  return File::GetUserPath(D_CONFIG_IDX) + SETTINGS_FILE_NAME;
}

void LoadRuntimeSettings()
{
  {
    std::lock_guard lock{s_load_mutex};
    s_loaded = true;
  }

  RuntimeSettings settings = GetRuntimeSettings();

  const std::string path = GetSettingsFilePath();
  Common::IniFile ini;
  if (ini.Load(path) && ApplySectionToSettings(ini, SETTINGS_SECTION, &settings))
  {
    INFO_LOG_FMT(CORE, "PrimedGun: loaded VR settings from {}", path);
  }
  else
  {
    // First run on this install, or an upgrade from a build that kept these in Qt.ini.
    const std::string legacy_path = File::GetUserPath(D_CONFIG_IDX) + LEGACY_SETTINGS_FILE_NAME;
    Common::IniFile legacy_ini;
    if (legacy_ini.Load(legacy_path) && ApplyAnyKnownSectionToSettings(legacy_ini, &settings))
    {
      INFO_LOG_FMT(CORE, "PrimedGun: imported VR settings from {}", legacy_path);
      SetRuntimeSettings(settings);
      SaveRuntimeSettings(GetRuntimeSettings());
      return;
    }

    INFO_LOG_FMT(CORE, "PrimedGun: no saved VR settings, using defaults");
  }

  SetRuntimeSettings(settings);
}

void EnsureRuntimeSettingsLoaded()
{
  {
    std::lock_guard lock{s_load_mutex};
    if (s_loaded)
      return;
  }

  LoadRuntimeSettings();
}

bool SaveRuntimeSettings(const RuntimeSettings& settings)
{
  const std::string path = GetSettingsFilePath();

  // Keep any unrelated sections a future version may add to this file.
  Common::IniFile ini;
  ini.Load(path);

  RuntimeSettings values = settings;
  Common::IniFile::Section* section = ini.GetOrCreateSection(SETTINGS_SECTION);
  VisitPersistedSettings(values,
                         [section](const char* key, auto& value) { section->Set(key, value); });

  if (!ini.Save(path))
  {
    ERROR_LOG_FMT(CORE, "PrimedGun: failed to save VR settings to {}", path);
    return false;
  }

  INFO_LOG_FMT(CORE, "PrimedGun: saved VR settings to {}", path);
  return true;
}

bool ImportRuntimeSettings(const std::string& path)
{
  Common::IniFile ini;
  if (!ini.Load(path))
  {
    WARN_LOG_FMT(CORE, "PrimedGun: could not read VR settings to import from {}", path);
    return false;
  }

  RuntimeSettings settings = GetRuntimeSettings();
  const bool was_enabled = settings.enabled;
  if (!ApplyAnyKnownSectionToSettings(ini, &settings))
  {
    WARN_LOG_FMT(CORE, "PrimedGun: no VR settings found in {}", path);
    return false;
  }

  settings.enabled = was_enabled;
  SetRuntimeSettings(settings);
  SaveRuntimeSettings(GetRuntimeSettings());
  INFO_LOG_FMT(CORE, "PrimedGun: imported VR settings from {}", path);
  return true;
}

void ProcessPendingVrSettingsSave()
{
  if (!ConsumeVrSettingsSaveRequest())
    return;

  SaveRuntimeSettings(GetRuntimeSettings());
  MarkVrSettingsSaved();
}
}  // namespace PrimedGun
