// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/TextureElementManager.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include <fmt/format.h>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Core/ConfigLoaders/GameConfigLoader.h"

namespace
{
constexpr int MAX_TEXTURE_ELEMENT_LOGS = 240;
constexpr bool ENABLE_PRIMEDGUN_TEXTURE_ELEMENT_LOGGING = false;
std::atomic<int> s_texture_element_log_count = 0;

void AppendTextureElementLog(std::string_view text)
{
  if (!ENABLE_PRIMEDGUN_TEXTURE_ELEMENT_LOGGING)
    return;

  File::CreateFullPath(File::GetUserPath(D_LOGS_IDX));
  File::IOFile file(File::GetUserPath(D_LOGS_IDX) + "PrimedGunTextureElement.log", "ab");
  if (!file)
    return;
  file.WriteString(std::string(text));
}

std::string GetVRGameSettingsPath(const std::string& game_id)
{
  return File::GetUserPath(D_GAMESETTINGSVR_IDX) + game_id + ".ini";
}

std::string GetSysVRGameSettingsPath(const std::string& filename)
{
  return File::GetSysDirectory() + GAMESETTINGSVR_DIR DIR_SEP + filename;
}

std::string ReadFileWithoutTextureSections(const std::string& path)
{
  std::ifstream file(path);
  if (!file.is_open())
    return {};

  std::ostringstream out;
  bool skipping = false;
  std::string line;
  while (std::getline(file, line))
  {
    std::string trimmed = line;
    if (!trimmed.empty() && trimmed.back() == '\r')
      trimmed.pop_back();

    if (trimmed == "[TextureElementOverride_Enable]" || trimmed == "[TextureElementOverride]")
    {
      skipping = true;
      continue;
    }
    if (skipping && !trimmed.empty() && trimmed[0] == '[')
      skipping = false;

    if (!skipping)
      out << line << "\n";
  }
  return out.str();
}

bool ParseKeyValue(const std::string& line, std::string& key, std::string& value)
{
  const auto eq = line.find('=');
  if (eq == std::string::npos)
    return false;

  key = line.substr(0, eq);
  value = line.substr(eq + 1);
  while (!key.empty() && key.back() == ' ')
    key.pop_back();
  while (!value.empty() && value.front() == ' ')
    value.erase(value.begin());
  return true;
}

std::vector<u64> ParseTextureHashList(const std::string& value)
{
  std::vector<u64> hashes;
  std::string token;

  auto flush_token = [&]() {
    if (token.empty())
      return;

    bool valid = token.size() <= 16;
    for (char c : token)
    {
      if (!std::isxdigit(static_cast<unsigned char>(c)))
      {
        valid = false;
        break;
      }
    }

    if (valid)
    {
      const u64 hash = std::strtoull(token.c_str(), nullptr, 16);
      if (hash != 0)
        hashes.push_back(hash);
    }
    token.clear();
  };

  for (char c : value)
  {
    if (c == ',' || c == ';' || std::isspace(static_cast<unsigned char>(c)))
      flush_token();
    else
      token.push_back(c);
  }
  flush_token();

  std::sort(hashes.begin(), hashes.end());
  hashes.erase(std::unique(hashes.begin(), hashes.end()), hashes.end());
  return hashes;
}

using HandlingType = TextureElementManager::HandlingType;
using TextureElementOverride = TextureElementManager::TextureElementOverride;

const char* HandlingToString(HandlingType handling)
{
  switch (handling)
  {
  case HandlingType::Screen:
    return "screen";
  case HandlingType::Fullscreen:
    return "fullscreen";
  case HandlingType::FullscreenMono:
    return "fullscreen_mono";
  case HandlingType::HeadLocked:
    return "headlocked";
  case HandlingType::UnitsPerMeter:
    return "units_per_meter";
  default:
    return "skip";
  }
}

HandlingType HandlingFromString(const std::string& value)
{
  if (value == "screen")
    return HandlingType::Screen;
  if (value == "fullscreen")
    return HandlingType::Fullscreen;
  if (value == "fullscreen_mono" || value == "fullscreenmono")
    return HandlingType::FullscreenMono;
  if (value == "headlocked")
    return HandlingType::HeadLocked;
  if (value == "units_per_meter" || value == "unitspermeter" || value == "upm")
    return HandlingType::UnitsPerMeter;
  return HandlingType::Skip;
}

struct ParsedTextureOverrideFile
{
  std::vector<TextureElementOverride> entries;
  bool has_enable_section = false;
  std::set<std::string> enabled_names;
};

ParsedTextureOverrideFile LoadTextureOverridesFromINIFile(const std::string& path)
{
  ParsedTextureOverrideFile parsed;
  std::ifstream file(path);
  if (!file.is_open())
    return parsed;

  {
    bool in_section = false;
    std::string line;
    while (std::getline(file, line))
    {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();

      if (line == "[TextureElementOverride_Enable]")
      {
        in_section = true;
        parsed.has_enable_section = true;
        continue;
      }
      if (in_section && !line.empty() && line[0] == '[')
        break;
      if (!in_section || line.empty())
        continue;
      if (line[0] == '$')
        parsed.enabled_names.insert(line.substr(1));
    }
  }

  file.clear();
  file.seekg(0);

  bool in_section = false;
  TextureElementOverride current{};
  bool has_entry = false;

  auto commit_entry = [&]() {
    if (!has_entry || current.texture_hashes.empty())
      return;

    std::sort(current.texture_hashes.begin(), current.texture_hashes.end());
    current.texture_hashes.erase(
        std::unique(current.texture_hashes.begin(), current.texture_hashes.end()),
        current.texture_hashes.end());
    current.enabled = parsed.has_enable_section ?
                          (parsed.enabled_names.count(current.name) > 0) :
                          true;
    parsed.entries.push_back(current);
  };

  std::string line;
  while (std::getline(file, line))
  {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();

    if (line == "[TextureElementOverride]")
    {
      in_section = true;
      continue;
    }
    if (in_section && !line.empty() && line[0] == '[')
      break;
    if (!in_section || line.empty())
      continue;

    if (line[0] == '$')
    {
      commit_entry();
      current = {};
      current.name = line.substr(1);
      current.handling = HandlingType::Skip;
      current.enabled = false;
      has_entry = true;
    }
    else if (has_entry)
    {
      std::string key, value;
      if (!ParseKeyValue(line, key, value))
        continue;

      if (key == "handling")
        current.handling = HandlingFromString(value);
      else if (key == "layer")
        current.layer = std::stoi(value);
      else if (key == "element_depth")
        current.element_depth = std::stof(value);
      else if (key == "units_per_meter" || key == "upm")
        current.units_per_meter = std::stof(value);
      else if (key == "comments")
        current.comments = value;
      else if (key == "texture")
      {
        const auto parsed_hashes = ParseTextureHashList(value);
        current.texture_hashes.insert(current.texture_hashes.end(), parsed_hashes.begin(),
                                      parsed_hashes.end());
      }
    }
  }

  commit_entry();
  return parsed;
}

void MergeParsedTextureOverrideFile(std::vector<TextureElementOverride>* result,
                                    std::map<std::string, size_t>* index_by_name,
                                    ParsedTextureOverrideFile parsed)
{
  for (auto& entry : parsed.entries)
  {
    const auto it = index_by_name->find(entry.name);
    if (it != index_by_name->end())
      (*result)[it->second] = std::move(entry);
    else
    {
      const size_t index = result->size();
      index_by_name->emplace(entry.name, index);
      result->push_back(std::move(entry));
    }
  }

  if (parsed.has_enable_section)
  {
    for (auto& entry : *result)
      entry.enabled = parsed.enabled_names.count(entry.name) > 0;
  }
}
}  // namespace

TextureElementManager& TextureElementManager::GetInstance()
{
  static TextureElementManager instance;
  return instance;
}

std::vector<TextureElementManager::TextureElementOverride>
TextureElementManager::LoadOverridesFromINI(const std::string& game_id, std::optional<u16> revision)
{
  if (game_id.empty())
    return {};

  std::vector<TextureElementOverride> result;
  std::map<std::string, size_t> index_by_name;

  for (const std::string& filename : ConfigLoaders::GetGameIniFilenames(game_id, revision))
    MergeParsedTextureOverrideFile(
        &result, &index_by_name,
        LoadTextureOverridesFromINIFile(GetSysVRGameSettingsPath(filename)));

  for (const std::string& filename : ConfigLoaders::GetGameIniFilenames(game_id, revision))
    MergeParsedTextureOverrideFile(
        &result, &index_by_name,
        LoadTextureOverridesFromINIFile(File::GetUserPath(D_GAMESETTINGSVR_IDX) + filename));

  return result;
}

void TextureElementManager::SaveOverridesToINI(
    const std::string& game_id, const std::vector<TextureElementOverride>& overrides)
{
  if (game_id.empty())
    return;

  const std::string path = GetVRGameSettingsPath(game_id);
  File::CreateFullPath(path);
  std::string base = ReadFileWithoutTextureSections(path);

  while (!base.empty() && (base.back() == '\n' || base.back() == '\r' || base.back() == ' '))
    base.pop_back();
  if (!base.empty())
    base += "\n";

  std::ostringstream out;
  out << base;
  out << "[TextureElementOverride_Enable]\n";
  for (const auto& ovr : overrides)
  {
    if (ovr.enabled)
      out << "$" << ovr.name << "\n";
  }

  out << "[TextureElementOverride]\n";
  for (const auto& ovr : overrides)
  {
    out << "$" << ovr.name << "\n";
    out << "handling=" << HandlingToString(ovr.handling) << "\n";
    if (ovr.layer >= 0)
      out << "layer=" << ovr.layer << "\n";
    if (ovr.element_depth >= 0.0f)
      out << "element_depth=" << ovr.element_depth << "\n";
    if (ovr.handling == HandlingType::UnitsPerMeter && ovr.units_per_meter > 0.0f)
      out << "units_per_meter=" << ovr.units_per_meter << "\n";
    if (!ovr.comments.empty())
    {
      std::string comments = ovr.comments;
      std::replace(comments.begin(), comments.end(), '\n', ' ');
      std::replace(comments.begin(), comments.end(), '\r', ' ');
      out << "comments=" << comments << "\n";
    }
    for (u64 texture_hash : ovr.texture_hashes)
      out << "texture=" << fmt::format("{:016x}", texture_hash) << "\n";
    out << "\n";
  }

  std::ofstream outfile(path, std::ios::trunc);
  outfile << out.str();
}

void TextureElementManager::LoadOverrides(const std::string& game_id)
{
  std::lock_guard lock(m_mutex);

  m_overrides.clear();
  m_texture_handling.clear();
  m_loaded_game_id = game_id;
  m_has_overrides.store(false, std::memory_order_relaxed);

  if (game_id.empty())
    return;

  auto all = LoadOverridesFromINI(game_id);
  bool has_overrides = false;

  for (auto& ovr : all)
  {
    if (!ovr.enabled || ovr.texture_hashes.empty())
      continue;

    has_overrides = true;

    const ResolvedHandling resolved{ovr.handling, ovr.layer, ovr.element_depth,
                                    ovr.units_per_meter};
    for (u64 texture_hash : ovr.texture_hashes)
      m_texture_handling.emplace(texture_hash, resolved);

    m_overrides.push_back(std::move(ovr));
  }

  m_has_overrides.store(has_overrides, std::memory_order_relaxed);

  if (ENABLE_PRIMEDGUN_TEXTURE_ELEMENT_LOGGING && !m_overrides.empty())
  {
    INFO_LOG_FMT(VIDEO, "TextureElementManager: Loaded {} enabled texture overrides for game {}",
                 m_overrides.size(), game_id);
    AppendTextureElementLog(fmt::format("loaded game={} enabled={}\n", game_id, m_overrides.size()));
    for (const auto& override_entry : m_overrides)
    {
      AppendTextureElementLog(fmt::format("  ${} handling={} textures={}\n", override_entry.name,
                                          HandlingToString(override_entry.handling),
                                          override_entry.texture_hashes.size()));
    }
  }
}

void TextureElementManager::LoadOverridesIfNeeded(const std::string& game_id)
{
  if (game_id == m_loaded_game_id)
    return;
  LoadOverrides(game_id);
}

bool TextureElementManager::HasOverrides() const
{
  return m_has_overrides.load(std::memory_order_relaxed);
}

bool TextureElementManager::NeedsTextureHashes() const
{
  return m_has_overrides.load(std::memory_order_relaxed);
}

bool TextureElementManager::ShouldSkipByTexture(const std::array<u64, 8>& bound) const
{
  if (m_texture_handling.empty())
    return false;

  for (u64 hash : bound)
  {
    if (hash == 0)
      continue;
    const auto it = m_texture_handling.find(hash);
    if (it != m_texture_handling.end() && it->second.handling == HandlingType::Skip)
    {
      if (ENABLE_PRIMEDGUN_TEXTURE_ELEMENT_LOGGING)
      {
        const int log_index = s_texture_element_log_count.fetch_add(1, std::memory_order_relaxed);
        if (log_index < MAX_TEXTURE_ELEMENT_LOGS)
        {
          AppendTextureElementLog(
              fmt::format("skip-match hash={:016x} slot_log={}\n", hash, log_index + 1));
        }
      }
      return true;
    }
  }
  return false;
}

TextureElementManager::HandlingType TextureElementManager::GetHandlingForTextures(
    const std::array<u64, 8>& bound, int* layer, float* element_depth,
    float* units_per_meter) const
{
  if (m_texture_handling.empty())
    return HandlingType::Skip;

  for (u64 hash : bound)
  {
    if (hash == 0)
      continue;
    const auto it = m_texture_handling.find(hash);
    if (it == m_texture_handling.end() || it->second.handling == HandlingType::Skip)
      continue;

    if (ENABLE_PRIMEDGUN_TEXTURE_ELEMENT_LOGGING)
    {
      const int log_index = s_texture_element_log_count.fetch_add(1, std::memory_order_relaxed);
      if (log_index < MAX_TEXTURE_ELEMENT_LOGS)
      {
        AppendTextureElementLog(fmt::format("handling-match hash={:016x} handling={} layer={} "
                                            "depth={:.3f} upm={:.3f} slot_log={}\n",
                                            hash, HandlingToString(it->second.handling),
                                            it->second.layer, it->second.element_depth,
                                            it->second.units_per_meter, log_index + 1));
      }
    }

    if (layer != nullptr)
      *layer = it->second.layer;
    if (element_depth != nullptr)
      *element_depth = it->second.element_depth;
    if (units_per_meter != nullptr)
      *units_per_meter = it->second.units_per_meter;
    return it->second.handling;
  }
  return HandlingType::Skip;
}
