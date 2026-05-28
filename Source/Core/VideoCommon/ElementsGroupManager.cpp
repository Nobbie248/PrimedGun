// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/ElementsGroupManager.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include <fmt/format.h>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Core/ConfigLoaders/GameConfigLoader.h"
#include "VideoCommon/RuntimeElementMatcher.h"

namespace
{
std::string GetVRGameSettingsPath(const std::string& game_id)
{
  return File::GetUserPath(D_GAMESETTINGSVR_IDX) + game_id + ".ini";
}

std::string GetSysVRGameSettingsPath(const std::string& filename)
{
  return File::GetSysDirectory() + GAMESETTINGSVR_DIR DIR_SEP + filename;
}

struct ParsedElementGroupOverrideFile
{
  std::vector<ElementsGroupManager::ElementGroupOverride> entries;
  bool has_enable_section = false;
  std::set<std::string> enabled_names;
};

std::string ReadFileWithoutElementSections(const std::string& path)
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

    if (trimmed == "[ElementsGroupOverride_Enable]" || trimmed == "[ElementsGroupOverride]")
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

bool ParseRuntimeElementSignatureField(ShaderHunter::RuntimeElementSignature* signature,
                                       const std::string& key, const std::string& value)
{
  if (key == "sig_perspective")
    signature->perspective = (value == "1" || value == "true");
  else if (key == "sig_persp_hfov")
    signature->perspective_hfov_x100 = std::stoi(value);
  else if (key == "sig_persp_vfov")
    signature->perspective_vfov_x100 = std::stoi(value);
  else if (key == "sig_persp_near")
    signature->perspective_near_x1000 = std::stoi(value);
  else if (key == "sig_persp_far")
    signature->perspective_far_x100 = std::stoi(value);
  else if (key == "sig_ortho_left")
    signature->ortho_left_x100 = std::stoi(value);
  else if (key == "sig_ortho_right")
    signature->ortho_right_x100 = std::stoi(value);
  else if (key == "sig_ortho_top")
    signature->ortho_top_x100 = std::stoi(value);
  else if (key == "sig_ortho_bottom")
    signature->ortho_bottom_x100 = std::stoi(value);
  else if (key == "sig_use_projection")
    signature->use_projection = (value == "1" || value == "true");
  else if (key == "sig_use_layer")
    signature->use_layer = (value == "1" || value == "true");
  else if (key == "sig_use_viewport")
    signature->use_viewport = (value == "1" || value == "true");
  else if (key == "sig_use_scissor")
    signature->use_scissor = (value == "1" || value == "true");
  else if (key == "sig_use_render_state")
    signature->use_render_state = (value == "1" || value == "true");
  else if (key == "sig_layer")
    signature->ortho_layer = std::stoi(value);
  else if (key == "sig_vp_x")
    signature->viewport_x = std::stoi(value);
  else if (key == "sig_vp_y")
    signature->viewport_y = std::stoi(value);
  else if (key == "sig_vp_w")
    signature->viewport_width = std::stoi(value);
  else if (key == "sig_vp_h")
    signature->viewport_height = std::stoi(value);
  else if (key == "sig_sc_l")
    signature->scissor_left = std::stoi(value);
  else if (key == "sig_sc_t")
    signature->scissor_top = std::stoi(value);
  else if (key == "sig_sc_r")
    signature->scissor_right = std::stoi(value);
  else if (key == "sig_sc_b")
    signature->scissor_bottom = std::stoi(value);
  else if (key == "sig_alpha")
    signature->alpha_test_hex = std::strtoul(value.c_str(), nullptr, 16);
  else if (key == "sig_ztest")
    signature->ztest = (value == "1" || value == "true");
  else if (key == "sig_zupdate")
    signature->zupdate = (value == "1" || value == "true");
  else if (key == "sig_zfunc")
    signature->zfunc = std::stoi(value);
  else if (key == "sig_blend_color")
    signature->blend_color_update = (value == "1" || value == "true");
  else if (key == "sig_blend_alpha")
    signature->blend_alpha_update = (value == "1" || value == "true");
  else
    return false;

  signature->valid = true;
  return true;
}

void SaveRuntimeElementSignature(std::ostringstream& out,
                                 const ShaderHunter::RuntimeElementSignature& sig,
                                 const std::string& prefix)
{
  out << prefix << "sig_perspective=" << (sig.perspective ? 1 : 0) << "\n";
  out << prefix << "sig_persp_hfov=" << sig.perspective_hfov_x100 << "\n";
  out << prefix << "sig_persp_vfov=" << sig.perspective_vfov_x100 << "\n";
  out << prefix << "sig_persp_near=" << sig.perspective_near_x1000 << "\n";
  out << prefix << "sig_persp_far=" << sig.perspective_far_x100 << "\n";
  out << prefix << "sig_ortho_left=" << sig.ortho_left_x100 << "\n";
  out << prefix << "sig_ortho_right=" << sig.ortho_right_x100 << "\n";
  out << prefix << "sig_ortho_top=" << sig.ortho_top_x100 << "\n";
  out << prefix << "sig_ortho_bottom=" << sig.ortho_bottom_x100 << "\n";
  out << prefix << "sig_use_projection=" << (sig.use_projection ? 1 : 0) << "\n";
  out << prefix << "sig_use_layer=" << (sig.use_layer ? 1 : 0) << "\n";
  out << prefix << "sig_use_viewport=" << (sig.use_viewport ? 1 : 0) << "\n";
  out << prefix << "sig_use_scissor=" << (sig.use_scissor ? 1 : 0) << "\n";
  out << prefix << "sig_use_render_state=" << (sig.use_render_state ? 1 : 0) << "\n";
  out << prefix << "sig_layer=" << sig.ortho_layer << "\n";
  out << prefix << "sig_vp_x=" << sig.viewport_x << "\n";
  out << prefix << "sig_vp_y=" << sig.viewport_y << "\n";
  out << prefix << "sig_vp_w=" << sig.viewport_width << "\n";
  out << prefix << "sig_vp_h=" << sig.viewport_height << "\n";
  out << prefix << "sig_sc_l=" << sig.scissor_left << "\n";
  out << prefix << "sig_sc_t=" << sig.scissor_top << "\n";
  out << prefix << "sig_sc_r=" << sig.scissor_right << "\n";
  out << prefix << "sig_sc_b=" << sig.scissor_bottom << "\n";
  out << prefix << "sig_alpha=" << fmt::format("{:08x}", sig.alpha_test_hex) << "\n";
  out << prefix << "sig_ztest=" << (sig.ztest ? 1 : 0) << "\n";
  out << prefix << "sig_zupdate=" << (sig.zupdate ? 1 : 0) << "\n";
  out << prefix << "sig_zfunc=" << sig.zfunc << "\n";
  out << prefix << "sig_blend_color=" << (sig.blend_color_update ? 1 : 0) << "\n";
  out << prefix << "sig_blend_alpha=" << (sig.blend_alpha_update ? 1 : 0) << "\n";
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

std::vector<MetroidElementLayer> ParseProfileLayerList(const std::string& value)
{
  std::vector<MetroidElementLayer> layers;
  std::string token;

  auto flush_token = [&]() {
    if (token.empty())
      return;
    if (const auto layer = MetroidElementLayerFromString(token))
      layers.push_back(*layer);
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
  return layers;
}

void SortDeduplicateProfileLayers(std::vector<MetroidElementLayer>* layers)
{
  std::sort(layers->begin(), layers->end(), [](MetroidElementLayer lhs, MetroidElementLayer rhs) {
    return static_cast<int>(lhs) < static_cast<int>(rhs);
  });
  layers->erase(std::unique(layers->begin(), layers->end()), layers->end());
}

std::vector<u64> CollectNonZeroTextureHashes(const std::array<u64, 8>& textures)
{
  std::vector<u64> hashes;
  hashes.reserve(textures.size());
  for (u64 texture_hash : textures)
  {
    if (texture_hash != 0)
      hashes.push_back(texture_hash);
  }
  std::sort(hashes.begin(), hashes.end());
  hashes.erase(std::unique(hashes.begin(), hashes.end()), hashes.end());
  return hashes;
}

void HashCombine(size_t& seed, size_t value)
{
  seed ^= value + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

template <typename T>
void HashCombineValue(size_t& seed, const T& value)
{
  HashCombine(seed, std::hash<T>{}(value));
}

size_t ComputeRuntimeElementKey(const ShaderHunter::RuntimeElementSignature& sig,
                                const std::vector<u64>& texture_hashes, bool texture_excluded)
{
  size_t seed = 0;
  HashCombineValue(seed, sig.valid);
  HashCombineValue(seed, sig.perspective);
  HashCombineValue(seed, sig.use_projection);
  HashCombineValue(seed, sig.use_layer);
  HashCombineValue(seed, sig.use_viewport);
  HashCombineValue(seed, sig.use_scissor);
  HashCombineValue(seed, sig.use_render_state);
  HashCombineValue(seed, sig.perspective_hfov_x100);
  HashCombineValue(seed, sig.perspective_vfov_x100);
  HashCombineValue(seed, sig.perspective_near_x1000);
  HashCombineValue(seed, sig.perspective_far_x100);
  HashCombineValue(seed, sig.ortho_left_x100);
  HashCombineValue(seed, sig.ortho_right_x100);
  HashCombineValue(seed, sig.ortho_top_x100);
  HashCombineValue(seed, sig.ortho_bottom_x100);
  HashCombineValue(seed, sig.ortho_layer);
  HashCombineValue(seed, sig.viewport_x);
  HashCombineValue(seed, sig.viewport_y);
  HashCombineValue(seed, sig.viewport_width);
  HashCombineValue(seed, sig.viewport_height);
  HashCombineValue(seed, sig.scissor_left);
  HashCombineValue(seed, sig.scissor_top);
  HashCombineValue(seed, sig.scissor_right);
  HashCombineValue(seed, sig.scissor_bottom);
  HashCombineValue(seed, sig.alpha_test_hex);
  HashCombineValue(seed, sig.ztest);
  HashCombineValue(seed, sig.zupdate);
  HashCombineValue(seed, sig.zfunc);
  HashCombineValue(seed, sig.blend_color_update);
  HashCombineValue(seed, sig.blend_alpha_update);
  HashCombineValue(seed, texture_excluded);
  for (const u64 texture_hash : texture_hashes)
    HashCombineValue(seed, texture_hash);
  return seed;
}

size_t ComputeProfileElementKey(MetroidElementProfile profile,
                                const std::vector<MetroidElementLayer>& layers,
                                const std::vector<u64>& texture_hashes, bool texture_excluded)
{
  size_t seed = 0;
  HashCombineValue(seed, static_cast<int>(profile));
  for (const MetroidElementLayer layer : layers)
    HashCombineValue(seed, static_cast<int>(layer));
  HashCombineValue(seed, texture_excluded);
  for (const u64 texture_hash : texture_hashes)
    HashCombineValue(seed, texture_hash);
  return seed;
}

const char* GetHandlingName(ElementsGroupManager::HandlingType handling)
{
  return handling == ElementsGroupManager::HandlingType::Screen         ? "screen" :
         handling == ElementsGroupManager::HandlingType::Fullscreen     ? "fullscreen" :
         handling == ElementsGroupManager::HandlingType::FullscreenMono ? "fullscreen_mono" :
         handling == ElementsGroupManager::HandlingType::HeadLocked     ? "headlocked" :
         handling == ElementsGroupManager::HandlingType::Flag           ? "flag" :
         handling == ElementsGroupManager::HandlingType::UnitsPerMeter  ? "units_per_meter" :
                                                                          "skip";
}

bool SignaturesEqual(const ShaderHunter::RuntimeElementSignature& lhs,
                     const ShaderHunter::RuntimeElementSignature& rhs)
{
  return lhs.valid == rhs.valid && lhs.perspective == rhs.perspective &&
         lhs.perspective_hfov_x100 == rhs.perspective_hfov_x100 &&
         lhs.perspective_vfov_x100 == rhs.perspective_vfov_x100 &&
         lhs.perspective_near_x1000 == rhs.perspective_near_x1000 &&
         lhs.perspective_far_x100 == rhs.perspective_far_x100 &&
         lhs.ortho_left_x100 == rhs.ortho_left_x100 &&
         lhs.ortho_right_x100 == rhs.ortho_right_x100 &&
         lhs.ortho_top_x100 == rhs.ortho_top_x100 &&
         lhs.ortho_bottom_x100 == rhs.ortho_bottom_x100 &&
         lhs.ortho_layer == rhs.ortho_layer && lhs.viewport_x == rhs.viewport_x &&
         lhs.viewport_y == rhs.viewport_y && lhs.viewport_width == rhs.viewport_width &&
         lhs.viewport_height == rhs.viewport_height &&
         lhs.scissor_left == rhs.scissor_left && lhs.scissor_top == rhs.scissor_top &&
         lhs.scissor_right == rhs.scissor_right && lhs.scissor_bottom == rhs.scissor_bottom &&
         lhs.alpha_test_hex == rhs.alpha_test_hex && lhs.ztest == rhs.ztest &&
         lhs.zupdate == rhs.zupdate && lhs.zfunc == rhs.zfunc &&
         lhs.blend_color_update == rhs.blend_color_update &&
         lhs.blend_alpha_update == rhs.blend_alpha_update;
}

bool StableSubMatchSignaturesEqual(const ElementsGroupManager::StableSubMatchSignature& lhs,
                                   const ElementsGroupManager::StableSubMatchSignature& rhs)
{
  return SignaturesEqual(lhs.runtime_element, rhs.runtime_element) && lhs.vs_family == rhs.vs_family &&
         lhs.ps_family == rhs.ps_family && lhs.gs_family == rhs.gs_family &&
         lhs.texture_hashes == rhs.texture_hashes && lhs.occurrence_slot == rhs.occurrence_slot;
}

bool SelectedSubgroupSignaturesEqual(const ElementsGroupManager::SelectedSubgroupSignature& lhs,
                                     const ElementsGroupManager::SelectedSubgroupSignature& rhs)
{
  return lhs.vs_family == rhs.vs_family && lhs.ps_family == rhs.ps_family &&
         lhs.gs_family == rhs.gs_family && lhs.texture_hashes == rhs.texture_hashes;
}

bool StableSubMatchBaseEqual(const ElementsGroupManager::StableSubMatchSignature& lhs,
                             const ElementsGroupManager::StableSubMatchSignature& rhs)
{
  return SignaturesEqual(lhs.runtime_element, rhs.runtime_element) && lhs.vs_family == rhs.vs_family &&
         lhs.ps_family == rhs.ps_family && lhs.gs_family == rhs.gs_family &&
         lhs.texture_hashes == rhs.texture_hashes;
}

size_t ComputeSelectedSubgroupKey(const ElementsGroupManager::SelectedSubgroupSignature& signature)
{
  size_t seed = 0;
  HashCombineValue(seed, signature.vs_family);
  HashCombineValue(seed, signature.ps_family);
  HashCombineValue(seed, signature.gs_family);
  for (const u64 texture_hash : signature.texture_hashes)
    HashCombineValue(seed, texture_hash);
  return seed;
}

size_t ComputeStableSubMatchBaseKey(const ElementsGroupManager::StableSubMatchSignature& signature)
{
  size_t seed = ComputeRuntimeElementKey(signature.runtime_element, signature.texture_hashes, false);
  HashCombineValue(seed, signature.vs_family);
  HashCombineValue(seed, signature.ps_family);
  HashCombineValue(seed, signature.gs_family);
  return seed;
}

bool GroupMaskHasActiveGroups(bool projection, bool layer, bool viewport, bool scissor,
                              bool render_state)
{
  return projection || layer || viewport || scissor || render_state;
}

bool IsClassifiedMetroidLayer(MetroidElementLayer layer)
{
  return layer != MetroidElementLayer::Unknown && layer != MetroidElementLayer::Unknown2D;
}

bool ShouldUseFullscreenMonoForMetroidLayer(MetroidElementLayer layer)
{
  switch (layer)
  {
  case MetroidElementLayer::ScreenFade:
  case MetroidElementLayer::ScreenOverlay:
  case MetroidElementLayer::EchoEffect:
  case MetroidElementLayer::ThermalEffect:
  case MetroidElementLayer::ThermalEffectGun:
  case MetroidElementLayer::ChargeBeamEffect:
  case MetroidElementLayer::XRayEffect:
  case MetroidElementLayer::DarkEffect:
  case MetroidElementLayer::VisorDirt:
  case MetroidElementLayer::ScanDarken:
  case MetroidElementLayer::ScanHighlighter:
    return true;
  default:
    return false;
  }
}

void NormalizeProfileHandlingForCompatibility(ElementsGroupManager::ElementGroupOverride* entry)
{
  if (entry == nullptr || entry->handling != ElementsGroupManager::HandlingType::Fullscreen)
    return;

  if (std::any_of(entry->profile_layers.begin(), entry->profile_layers.end(),
                  ShouldUseFullscreenMonoForMetroidLayer))
  {
    entry->handling = ElementsGroupManager::HandlingType::FullscreenMono;
  }
}

void SaveStableSubMatchSignature(std::ostringstream& out,
                                 const ElementsGroupManager::StableSubMatchSignature& signature,
                                 const std::string& prefix)
{
  SaveRuntimeElementSignature(out, signature.runtime_element, prefix);
  if (signature.vs_family != 0)
    out << prefix << "vs_family=" << fmt::format("{:016x}", signature.vs_family) << "\n";
  if (signature.ps_family != 0)
    out << prefix << "ps_family=" << fmt::format("{:016x}", signature.ps_family) << "\n";
  if (signature.gs_family != 0)
    out << prefix << "gs_family=" << fmt::format("{:016x}", signature.gs_family) << "\n";
  for (u64 texture_hash : signature.texture_hashes)
    out << prefix << "texture=" << fmt::format("{:016x}", texture_hash) << "\n";
  out << prefix << "slot=" << signature.occurrence_slot << "\n";
}

void SaveSelectedSubgroupSignature(std::ostringstream& out,
                                   const ElementsGroupManager::SelectedSubgroupSignature& signature,
                                   const std::string& prefix)
{
  if (signature.vs_family != 0)
    out << prefix << "vs_family=" << fmt::format("{:016x}", signature.vs_family) << "\n";
  if (signature.ps_family != 0)
    out << prefix << "ps_family=" << fmt::format("{:016x}", signature.ps_family) << "\n";
  if (signature.gs_family != 0)
    out << prefix << "gs_family=" << fmt::format("{:016x}", signature.gs_family) << "\n";
  for (u64 texture_hash : signature.texture_hashes)
    out << prefix << "texture=" << fmt::format("{:016x}", texture_hash) << "\n";
}

bool ParseStableSubMatchField(ElementsGroupManager::StableSubMatchSignature* signature,
                              const std::string& key, const std::string& value)
{
  if (ParseRuntimeElementSignatureField(&signature->runtime_element, key, value))
    return true;
  if (key == "vs_family")
    signature->vs_family = std::strtoull(value.c_str(), nullptr, 16);
  else if (key == "ps_family")
    signature->ps_family = std::strtoull(value.c_str(), nullptr, 16);
  else if (key == "gs_family")
    signature->gs_family = std::strtoull(value.c_str(), nullptr, 16);
  else if (key == "texture")
  {
    const u64 parsed = std::strtoull(value.c_str(), nullptr, 16);
    if (parsed != 0)
      signature->texture_hashes.push_back(parsed);
  }
  else if (key == "slot")
    signature->occurrence_slot = std::stoi(value);
  else
    return false;

  signature->runtime_element.valid = true;
  return true;
}

bool ParseSelectedSubgroupField(ElementsGroupManager::SelectedSubgroupSignature* signature,
                                const std::string& key, const std::string& value)
{
  if (key == "vs_family")
    signature->vs_family = std::strtoull(value.c_str(), nullptr, 16);
  else if (key == "ps_family")
    signature->ps_family = std::strtoull(value.c_str(), nullptr, 16);
  else if (key == "gs_family")
    signature->gs_family = std::strtoull(value.c_str(), nullptr, 16);
  else if (key == "texture")
  {
    const u64 parsed = std::strtoull(value.c_str(), nullptr, 16);
    if (parsed != 0)
      signature->texture_hashes.push_back(parsed);
  }
  else
  {
    return false;
  }

  return true;
}
}  // namespace

u64 ElementsGroupManager::DrawRecord::GetHash(ShaderType type) const
{
  switch (type)
  {
  case ShaderType::Vertex:
    return vs_hash;
  case ShaderType::Geometry:
    return gs_hash;
  case ShaderType::Pixel:
  default:
    return ps_hash;
  }
}

u64 ElementsGroupManager::DrawRecord::GetFamily(ShaderType type) const
{
  switch (type)
  {
  case ShaderType::Vertex:
    return vs_family;
  case ShaderType::Geometry:
    return gs_family;
  case ShaderType::Pixel:
  default:
    return ps_family;
  }
}

ElementsGroupManager& ElementsGroupManager::GetInstance()
{
  static ElementsGroupManager instance;
  return instance;
}

ElementsGroupManager::RuntimeElementSignature
ElementsGroupManager::MakeSelectedMatchFilterSignature(
    const RuntimeElementSignature& signature)
{
  RuntimeElementSignature filter = signature;
  filter.valid = true;
  filter.use_projection = true;
  filter.use_layer = true;
  filter.use_viewport = true;
  filter.use_scissor = true;
  filter.use_render_state = true;
  return filter;
}

ElementsGroupManager::StableSubMatchSignature ElementsGroupManager::MakeStableSubMatchSignature(
    const DrawRecord& draw, int occurrence_slot)
{
  StableSubMatchSignature signature;
  signature.runtime_element = MakeSelectedMatchFilterSignature(draw.signature);
  signature.vs_family = draw.vs_family;
  signature.ps_family = draw.ps_family;
  signature.gs_family = draw.gs_family;
  signature.texture_hashes = CollectNonZeroTextureHashes(draw.textures);
  signature.occurrence_slot = occurrence_slot;
  return signature;
}

ElementsGroupManager::SelectedSubgroupSignature
ElementsGroupManager::MakeSelectedSubgroupSignature(const DrawRecord& draw)
{
  SelectedSubgroupSignature signature;
  signature.vs_family = draw.vs_family;
  signature.ps_family = draw.ps_family;
  signature.gs_family = draw.gs_family;
  signature.texture_hashes = CollectNonZeroTextureHashes(draw.textures);
  return signature;
}

size_t ElementsGroupManager::CounterKeyHasher::operator()(const CounterKey& key) const noexcept
{
  return std::hash<u64>{}(key.value);
}

ParsedElementGroupOverrideFile
LoadElementGroupOverridesFromINIFile(const std::string& path)
{
  using ElementGroupOverride = ElementsGroupManager::ElementGroupOverride;
  using StableSubMatchSignature = ElementsGroupManager::StableSubMatchSignature;
  using SelectedSubgroupSignature = ElementsGroupManager::SelectedSubgroupSignature;
  using ShaderType = ElementsGroupManager::ShaderType;
  using HandlingType = ElementsGroupManager::HandlingType;

  ParsedElementGroupOverrideFile parsed;
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
      if (line == "[ElementsGroupOverride_Enable]")
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
  ElementGroupOverride current{};
  bool has_entry = false;
  std::string current_format;
  std::map<int, StableSubMatchSignature> current_selected_match_filters_v3;
  std::map<int, SelectedSubgroupSignature> current_selected_match_filters_v4;

  auto commit_entry = [&]() {
    const bool runtime_format =
        current_format == "element_only_v2" || current_format == "element_only_v3" ||
        current_format == "element_only_v4" || current_format == "element_only_v5";
    const bool profile_format = current_format == "element_profile_v1";
    if (!has_entry || current.name.empty() || (!runtime_format && !profile_format))
      return;
    current.enabled = parsed.has_enable_section ? (parsed.enabled_names.count(current.name) > 0) :
                                                  true;
    current.selected_match_filter.clear();
    if (current_format == "element_only_v3")
    {
      for (auto& [index, signature] : current_selected_match_filters_v3)
      {
        std::sort(signature.texture_hashes.begin(), signature.texture_hashes.end());
        signature.texture_hashes.erase(
            std::unique(signature.texture_hashes.begin(), signature.texture_hashes.end()),
            signature.texture_hashes.end());
        SelectedSubgroupSignature subgroup;
        subgroup.vs_family = signature.vs_family;
        subgroup.ps_family = signature.ps_family;
        subgroup.gs_family = signature.gs_family;
        subgroup.texture_hashes = signature.texture_hashes;
        if ((subgroup.vs_family != 0 || subgroup.ps_family != 0 || subgroup.gs_family != 0 ||
             !subgroup.texture_hashes.empty()) &&
            std::none_of(current.selected_match_filter.begin(), current.selected_match_filter.end(),
                         [&subgroup](const SelectedSubgroupSignature& existing) {
                           return SelectedSubgroupSignaturesEqual(existing, subgroup);
                         }))
        {
          current.selected_match_filter.push_back(subgroup);
        }
      }
    }
    else if (current_format == "element_only_v4" || current_format == "element_only_v5")
    {
      for (auto& [index, signature] : current_selected_match_filters_v4)
      {
        std::sort(signature.texture_hashes.begin(), signature.texture_hashes.end());
        signature.texture_hashes.erase(
            std::unique(signature.texture_hashes.begin(), signature.texture_hashes.end()),
            signature.texture_hashes.end());
        if ((signature.vs_family != 0 || signature.ps_family != 0 || signature.gs_family != 0 ||
             !signature.texture_hashes.empty()) &&
            std::none_of(current.selected_match_filter.begin(), current.selected_match_filter.end(),
                         [&signature](const SelectedSubgroupSignature& existing) {
                           return SelectedSubgroupSignaturesEqual(existing, signature);
                         }))
        {
          current.selected_match_filter.push_back(signature);
        }
      }
    }
    if (profile_format)
    {
      current.match_kind = ElementsGroupManager::MatchKind::ProfileLayer;
      SortDeduplicateProfileLayers(&current.profile_layers);
      if (current.profile_id != MetroidElementProfile::None && !current.profile_layers.empty())
      {
        NormalizeProfileHandlingForCompatibility(&current);
        parsed.entries.push_back(current);
      }
    }
    else if (current.runtime_element.valid)
    {
      current.match_kind = ElementsGroupManager::MatchKind::RuntimeSignature;
      parsed.entries.push_back(current);
    }
  };

  std::string line;
  while (std::getline(file, line))
  {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();

    if (line == "[ElementsGroupOverride]")
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
      current.user_defined = true;
      has_entry = true;
      current_format.clear();
      current_selected_match_filters_v3.clear();
      current_selected_match_filters_v4.clear();
      continue;
    }

    if (!has_entry)
      continue;

    std::string key;
    std::string value;
    if (!ParseKeyValue(line, key, value))
      continue;

    if (key == "format")
    {
      current_format = value;
      current.match_kind = current_format == "element_profile_v1" ?
                               ElementsGroupManager::MatchKind::ProfileLayer :
                               ElementsGroupManager::MatchKind::RuntimeSignature;
    }
    else if (key == "handling")
      current.handling = value == "screen"          ? HandlingType::Screen :
                         value == "fullscreen"      ? HandlingType::Fullscreen :
                         value == "fullscreen_mono" ? HandlingType::FullscreenMono :
                         value == "headlocked"      ? HandlingType::HeadLocked :
                         value == "flag"            ? HandlingType::Flag :
                         value == "units_per_meter" || value == "upm" ?
                             HandlingType::UnitsPerMeter :
                             HandlingType::Skip;
    else if (key == "layer")
      current.layer = std::stoi(value);
    else if (key == "element_depth")
      current.element_depth = std::stof(value);
    else if (key == "units_per_meter" || key == "upm")
      current.units_per_meter = std::stof(value);
    else if (key == "flag")
      current.flag_group = value;
    else if (key == "condition")
      current.condition_flag = value;
    else if (key == "condition_mode")
      current.condition_inverted =
          (value == "deactivate" || value == "inactive" || value == "1" || value == "true");
    else if (key == "element_start")
      current.element_start = std::stoi(value);
    else if (key == "element_end")
      current.element_end = std::stoi(value);
    else if (key == "element_total")
      current.element_reference_total = std::stoi(value);
    else if (key == "texture")
    {
      const auto parsed_hashes = ParseTextureHashList(value);
      current.texture_hashes.insert(current.texture_hashes.end(), parsed_hashes.begin(),
                                    parsed_hashes.end());
    }
    else if (key == "texture_mode")
      current.texture_hashes_excluded =
          (value == "exclude" || value == "excluded" || value == "not");
    else if (key == "selected_match_mode")
      current.selected_match_filter_excluded =
          (value == "exclude" || value == "excluded" || value == "not");
    else if (key == "comments")
      current.comments = value;
    else if (key == "credits")
      current.credits = value;
    else if (key == "profile")
    {
      if (const auto profile = MetroidElementProfileFromString(value))
        current.profile_id = *profile;
    }
    else if (key == "profile_layer" || key == "profile_layers")
    {
      const auto parsed_layers = ParseProfileLayerList(value);
      current.profile_layers.insert(current.profile_layers.end(), parsed_layers.begin(),
                                    parsed_layers.end());
    }
    else if (current_format == "element_only_v3" && key.rfind("selected_match_", 0) == 0)
    {
      const size_t prefix_len = std::string("selected_match_").size();
      const size_t field_sep = key.find('_', prefix_len);
      if (field_sep != std::string::npos)
      {
        const std::string index_str = key.substr(prefix_len, field_sep - prefix_len);
        const std::string subkey = key.substr(field_sep + 1);
        const int match_index = std::stoi(index_str);
        ParseStableSubMatchField(&current_selected_match_filters_v3[match_index], subkey, value);
      }
    }
    else if ((current_format == "element_only_v4" || current_format == "element_only_v5") &&
             key.rfind("selected_match_", 0) == 0)
    {
      const size_t prefix_len = std::string("selected_match_").size();
      const size_t field_sep = key.find('_', prefix_len);
      if (field_sep != std::string::npos)
      {
        const std::string index_str = key.substr(prefix_len, field_sep - prefix_len);
        const std::string subkey = key.substr(field_sep + 1);
        const int match_index = std::stoi(index_str);
        ParseSelectedSubgroupField(&current_selected_match_filters_v4[match_index], subkey, value);
      }
    }
    else if (ParseRuntimeElementSignatureField(&current.runtime_element, key, value))
    {
      continue;
    }
  }

  commit_entry();
  return parsed;
}

static void MergeParsedElementGroupOverrideFile(
    std::vector<ElementsGroupManager::ElementGroupOverride>* result,
    std::map<std::string, size_t>* index_by_name, ParsedElementGroupOverrideFile parsed)
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

std::vector<ElementsGroupManager::ElementGroupOverride>
ElementsGroupManager::LoadOverridesFromINI(const std::string& game_id, std::optional<u16> revision)
{
  if (game_id.empty())
    return {};

  std::vector<ElementGroupOverride> result;
  std::map<std::string, size_t> index_by_name;

  for (const std::string& filename : ConfigLoaders::GetGameIniFilenames(game_id, revision))
    MergeParsedElementGroupOverrideFile(
        &result, &index_by_name, LoadElementGroupOverridesFromINIFile(GetSysVRGameSettingsPath(filename)));

  for (const std::string& filename : ConfigLoaders::GetGameIniFilenames(game_id, revision))
    MergeParsedElementGroupOverrideFile(
        &result, &index_by_name,
        LoadElementGroupOverridesFromINIFile(File::GetUserPath(D_GAMESETTINGSVR_IDX) + filename));

  return result;
}

void ElementsGroupManager::SaveOverridesToINI(const std::string& game_id,
                                              const std::vector<ElementGroupOverride>& overrides)
{
  if (game_id.empty())
    return;

  const std::string path = GetVRGameSettingsPath(game_id);
  File::CreateFullPath(path);
  std::string base = ReadFileWithoutElementSections(path);
  while (!base.empty() && (base.back() == '\n' || base.back() == '\r' || base.back() == ' '))
    base.pop_back();
  if (!base.empty())
    base += "\n";

  std::ostringstream out;
  out << base;
  out << "[ElementsGroupOverride_Enable]\n";
  for (const auto& entry : overrides)
  {
    if (entry.enabled)
      out << "$" << entry.name << "\n";
  }

  out << "[ElementsGroupOverride]\n";
  for (const auto& entry : overrides)
  {
    out << "$" << entry.name << "\n";
    out << "format="
        << (entry.match_kind == MatchKind::ProfileLayer ? "element_profile_v1" : "element_only_v5")
        << "\n";
    out << "handling=" << GetHandlingName(entry.handling) << "\n";
    if (entry.layer >= 0)
      out << "layer=" << entry.layer << "\n";
    if (entry.element_depth >= 0.0f)
      out << "element_depth=" << entry.element_depth << "\n";
    if (entry.units_per_meter > 0.0f)
      out << "units_per_meter=" << entry.units_per_meter << "\n";
    if (!entry.flag_group.empty())
      out << "flag=" << entry.flag_group << "\n";
    if (!entry.condition_flag.empty())
    {
      out << "condition=" << entry.condition_flag << "\n";
      out << "condition_mode=" << (entry.condition_inverted ? "deactivate" : "activate") << "\n";
    }
    if (entry.element_start >= 0)
      out << "element_start=" << entry.element_start << "\n";
    if (entry.element_end >= 0)
      out << "element_end=" << entry.element_end << "\n";
    if (entry.element_reference_total > 0)
      out << "element_total=" << entry.element_reference_total << "\n";
    if (!entry.comments.empty())
      out << "comments=" << entry.comments << "\n";
    if (!entry.credits.empty())
      out << "credits=" << entry.credits << "\n";
    if (!entry.texture_hashes.empty())
    {
      out << "texture_mode=" << (entry.texture_hashes_excluded ? "exclude" : "include") << "\n";
      for (u64 texture_hash : entry.texture_hashes)
        out << "texture=" << fmt::format("{:016x}", texture_hash) << "\n";
    }
    if (!entry.selected_match_filter.empty())
    {
      out << "selected_match_mode="
          << (entry.selected_match_filter_excluded ? "exclude" : "include") << "\n";
    }

    if (entry.match_kind == MatchKind::ProfileLayer)
    {
      out << "profile=" << MetroidElementProfileToININame(entry.profile_id) << "\n";
      for (const MetroidElementLayer layer : entry.profile_layers)
        out << "profile_layer=" << MetroidElementLayerToININame(layer) << "\n";
    }
    else
    {
      const auto& sig = entry.runtime_element;
      SaveRuntimeElementSignature(out, sig, "");
      for (size_t i = 0; i < entry.selected_match_filter.size(); ++i)
      {
        SaveSelectedSubgroupSignature(out, entry.selected_match_filter[i],
                                      fmt::format("selected_match_{}_", i));
      }
    }
    out << "\n";
  }

  std::ofstream outfile(path, std::ios::trunc);
  outfile << out.str();
}

void ElementsGroupManager::LoadOverrides(const std::string& game_id)
{
  std::lock_guard lock(m_mutex);
  m_loaded_game_id = game_id;
  m_overrides.clear();
  m_draw_counters.clear();
  m_draw_totals_prev.clear();
  m_current_draw_indices.clear();
  m_stable_submatch_occurrence_counters.clear();
  m_current_stable_submatch = {};
  m_active_profiles.clear();
  m_metroid_classifier = {};
  if (game_id.empty())
  {
    m_has_overrides.store(false);
    m_has_profile_overrides.store(false);
    return;
  }

  const auto all_overrides = LoadOverridesFromINI(game_id);
  for (const auto& entry : all_overrides)
  {
    if (entry.enabled)
    {
      m_overrides.push_back(entry);
      if (entry.match_kind == MatchKind::ProfileLayer &&
          entry.profile_id != MetroidElementProfile::None &&
          std::find(m_active_profiles.begin(), m_active_profiles.end(), entry.profile_id) ==
              m_active_profiles.end())
      {
        m_active_profiles.push_back(entry.profile_id);
      }
    }
  }

  m_has_overrides.store(!m_overrides.empty());
  m_has_profile_overrides.store(!m_active_profiles.empty());

  if (!m_overrides.empty())
  {
    INFO_LOG_FMT(VIDEO, "ElementsGroup: loaded {} enabled overrides for {}", m_overrides.size(),
                 game_id);
  }
}

void ElementsGroupManager::LoadOverridesIfNeeded(const std::string& game_id)
{
  {
    std::lock_guard lock(m_mutex);
    if (game_id == m_loaded_game_id)
      return;
  }
  LoadOverrides(game_id);
}

bool ElementsGroupManager::HasOverrides() const
{
  return m_has_overrides.load();
}

bool ElementsGroupManager::NeedsProfileClassification() const
{
  return m_has_profile_overrides.load();
}

bool ElementsGroupManager::IsMetroidPrime1GCProfileActive() const
{
  std::lock_guard lock(m_mutex);
  return std::find(m_active_profiles.begin(), m_active_profiles.end(),
                   MetroidElementProfile::Prime1GC) != m_active_profiles.end();
}

void ElementsGroupManager::SetPopupOpen(bool open)
{
  std::lock_guard lock(m_mutex);
  if (open)
  {
    ++m_popup_open_count;
  }
  else if (m_popup_open_count > 0)
  {
    --m_popup_open_count;
  }

  if (m_popup_open_count <= 0)
  {
    m_popup_open_count = 0;
    m_hunt_enabled = false;
    m_collecting_draws.clear();
    m_display_draws.clear();
    m_display_seed_candidates.clear();
    m_collecting_matches.clear();
    m_display_raw_matches.clear();
    m_display_matches.clear();
    m_collecting_match_total = 0;
    m_display_match_total = 0;
    m_collecting_highlighted_draw.reset();
    m_display_highlighted_draw.reset();
    m_display_highlighted_match_raw_draw_count = 0;
    m_stable_submatch_occurrence_counters.clear();
    m_current_stable_submatch = {};
    ClearSeedSelectionLocked();
  }

  m_popup_open.store(m_popup_open_count > 0);
}

bool ElementsGroupManager::IsPopupOpen() const
{
  return m_popup_open.load();
}

void ElementsGroupManager::SetHuntEnabled(bool enabled)
{
  std::lock_guard lock(m_mutex);
  if (m_hunt_enabled == enabled)
    return;

  m_hunt_enabled = enabled;
  if (!m_hunt_enabled)
  {
    m_collecting_draws.clear();
    m_display_draws.clear();
    m_display_seed_candidates.clear();
    m_collecting_matches.clear();
    m_collecting_match_total = 0;
    m_collecting_highlighted_draw.reset();
    ClearSeedSelectionLocked();
  }
}

bool ElementsGroupManager::IsHuntEnabled() const
{
  std::lock_guard lock(m_mutex);
  return m_hunt_enabled;
}

void ElementsGroupManager::SetHuntingOption(HuntingOption option)
{
  std::lock_guard lock(m_mutex);
  m_hunting_option = option;
}

ElementsGroupManager::HuntingOption ElementsGroupManager::GetHuntingOption() const
{
  std::lock_guard lock(m_mutex);
  return m_hunting_option;
}

void ElementsGroupManager::ClearSeedSelectionLocked()
{
  m_seed_signature = {};
  m_seed_group_signature = {};
  m_seed_draw.reset();
  m_selected_seed_index = -1;
  m_selected_match = 0;
  m_selected_match_filters.clear();
  m_selected_match_filters_excluded = false;
  m_display_raw_matches.clear();
  m_display_matches.clear();
  m_display_match_total = 0;
  m_display_highlighted_draw.reset();
  m_display_highlighted_match_raw_draw_count = 0;
}

ElementsGroupManager::RuntimeElementSignature ElementsGroupManager::GetMaskedSeedSignatureLocked() const
{
  RuntimeElementSignature masked = m_seed_signature;
  masked.use_projection = m_group_mask.projection;
  masked.use_layer = m_group_mask.layer;
  masked.use_viewport = m_group_mask.viewport;
  masked.use_scissor = m_group_mask.scissor;
  masked.use_render_state = m_group_mask.render_state;
  return masked;
}

ElementsGroupManager::RuntimeElementSignature
ElementsGroupManager::GetSeedGroupSignatureLocked(const RuntimeElementSignature& signature) const
{
  if (!signature.valid)
    return {};

  if (!GroupMaskHasActiveGroups(m_group_mask.projection, m_group_mask.layer, m_group_mask.viewport,
                                m_group_mask.scissor, m_group_mask.render_state))
    return signature;

  RuntimeElementSignature grouped{};
  grouped.valid = true;

  if (m_group_mask.projection)
  {
    grouped.perspective = signature.perspective;
    if (signature.perspective)
    {
      grouped.perspective_hfov_x100 = signature.perspective_hfov_x100;
      grouped.perspective_vfov_x100 = signature.perspective_vfov_x100;
      grouped.perspective_near_x1000 = signature.perspective_near_x1000;
      grouped.perspective_far_x100 = signature.perspective_far_x100;
    }
    else
    {
      grouped.ortho_left_x100 = signature.ortho_left_x100;
      grouped.ortho_right_x100 = signature.ortho_right_x100;
      grouped.ortho_top_x100 = signature.ortho_top_x100;
      grouped.ortho_bottom_x100 = signature.ortho_bottom_x100;
    }
  }

  if (m_group_mask.layer)
  {
    grouped.perspective = signature.perspective;
    grouped.ortho_layer = signature.ortho_layer;
  }

  if (m_group_mask.viewport)
  {
    grouped.viewport_x = signature.viewport_x;
    grouped.viewport_y = signature.viewport_y;
    grouped.viewport_width = signature.viewport_width;
    grouped.viewport_height = signature.viewport_height;
  }

  if (m_group_mask.scissor)
  {
    grouped.scissor_left = signature.scissor_left;
    grouped.scissor_top = signature.scissor_top;
    grouped.scissor_right = signature.scissor_right;
    grouped.scissor_bottom = signature.scissor_bottom;
  }

  if (m_group_mask.render_state)
  {
    grouped.alpha_test_hex = signature.alpha_test_hex;
    grouped.ztest = signature.ztest;
    grouped.zupdate = signature.zupdate;
    grouped.zfunc = signature.zfunc;
    grouped.blend_color_update = signature.blend_color_update;
    grouped.blend_alpha_update = signature.blend_alpha_update;
  }

  return grouped;
}

void ElementsGroupManager::SelectSeedCandidateLocked(int index)
{
  if (index < 0 || index >= static_cast<int>(m_display_seed_candidates.size()))
  {
    ClearSeedSelectionLocked();
    return;
  }

  m_selected_seed_index = index;
  m_seed_signature = m_display_seed_candidates[index].representative_draw.signature;
  m_seed_group_signature = m_display_seed_candidates[index].group_signature;
  m_seed_draw = m_display_seed_candidates[index].representative_draw;
  m_selected_match = 0;
  m_selected_match_filters.clear();
  m_selected_match_filters_excluded = false;
  m_collecting_match_total = 0;
  m_display_match_total = 0;
  m_collecting_highlighted_draw.reset();
  m_display_highlighted_draw.reset();
  m_display_highlighted_match_raw_draw_count = 0;
  m_collecting_matches.clear();
  m_display_raw_matches.clear();
  m_display_matches.clear();
}

void ElementsGroupManager::SelectSeedCandidate(int index)
{
  std::lock_guard lock(m_mutex);
  SelectSeedCandidateLocked(index);
}

void ElementsGroupManager::SetSeedGroupMask(bool projection, bool layer, bool viewport,
                                            bool scissor, bool render_state)
{
  std::lock_guard lock(m_mutex);
  m_group_mask.projection = projection;
  m_group_mask.layer = layer;
  m_group_mask.viewport = viewport;
  m_group_mask.scissor = scissor;
  m_group_mask.render_state = render_state;
  if (m_seed_signature.valid)
    m_seed_group_signature = GetSeedGroupSignatureLocked(m_seed_signature);
  m_selected_match = 0;
  m_selected_match_filters.clear();
  m_selected_match_filters_excluded = false;
  m_collecting_matches.clear();
  m_display_raw_matches.clear();
  m_display_matches.clear();
  m_collecting_match_total = 0;
  m_display_match_total = 0;
  m_collecting_highlighted_draw.reset();
  m_display_highlighted_draw.reset();
  m_display_highlighted_match_raw_draw_count = 0;
}

void ElementsGroupManager::SelectMatch(int index)
{
  std::lock_guard lock(m_mutex);
  if (index < 0 || index >= m_display_match_total)
    return;

  m_selected_match = index;
  if (index < static_cast<int>(m_display_matches.size()))
  {
    m_display_highlighted_draw = m_display_matches[index].representative_draw;
    m_display_highlighted_match_raw_draw_count = m_display_matches[index].raw_draw_count;
  }
}

void ElementsGroupManager::NextMatch()
{
  std::lock_guard lock(m_mutex);
  if (m_display_matches.empty())
    return;
  if (std::none_of(m_display_matches.begin(), m_display_matches.end(),
                   [](const CurrentMatchCandidate& candidate) { return candidate.active_this_frame; }))
    return;

  for (int step = 1; step <= static_cast<int>(m_display_matches.size()); ++step)
  {
    const int next_index = (m_selected_match + step) % static_cast<int>(m_display_matches.size());
    if (!m_display_matches[next_index].active_this_frame)
      continue;
    m_selected_match = next_index;
    m_display_highlighted_draw = m_display_matches[m_selected_match].representative_draw;
    m_display_highlighted_match_raw_draw_count = m_display_matches[m_selected_match].raw_draw_count;
    return;
  }
}

void ElementsGroupManager::PrevMatch()
{
  std::lock_guard lock(m_mutex);
  if (m_display_matches.empty())
    return;
  if (std::none_of(m_display_matches.begin(), m_display_matches.end(),
                   [](const CurrentMatchCandidate& candidate) { return candidate.active_this_frame; }))
    return;

  for (int step = 1; step <= static_cast<int>(m_display_matches.size()); ++step)
  {
    int prev_index = m_selected_match - step;
    while (prev_index < 0)
      prev_index += static_cast<int>(m_display_matches.size());
    if (!m_display_matches[prev_index].active_this_frame)
      continue;
    m_selected_match = prev_index;
    m_display_highlighted_draw = m_display_matches[m_selected_match].representative_draw;
    m_display_highlighted_match_raw_draw_count = m_display_matches[m_selected_match].raw_draw_count;
    return;
  }
}

std::optional<ElementsGroupManager::DrawRecord> ElementsGroupManager::GetHighlightedDraw() const
{
  std::lock_guard lock(m_mutex);
  return m_display_highlighted_draw;
}

std::optional<ElementsGroupManager::DrawRecord> ElementsGroupManager::GetSelectedSeedDraw() const
{
  std::lock_guard lock(m_mutex);
  return m_seed_draw;
}

std::optional<ElementsGroupManager::DrawRecord>
ElementsGroupManager::GetSelectedCurrentMatchDraw() const
{
  std::lock_guard lock(m_mutex);
  if (m_selected_match < 0 || m_selected_match >= static_cast<int>(m_display_matches.size()))
    return std::nullopt;

  return m_display_matches[static_cast<size_t>(m_selected_match)].representative_draw;
}

std::optional<ElementsGroupManager::DrawRecord>
ElementsGroupManager::GetCurrentTextureSourceDraw() const
{
  std::lock_guard lock(m_mutex);
  if (m_selected_match >= 0 && m_selected_match < static_cast<int>(m_display_matches.size()))
    return m_display_matches[static_cast<size_t>(m_selected_match)].representative_draw;

  return m_seed_draw;
}

std::vector<ElementsGroupManager::DrawRecord> ElementsGroupManager::GetCurrentTextureSourceDraws() const
{
  std::lock_guard lock(m_mutex);
  if (m_selected_match_filters.empty())
    return m_display_raw_matches;

  std::vector<DrawRecord> draws;
  draws.reserve(m_display_raw_matches.size());
  for (size_t i = 0; i < m_display_raw_matches.size() && i < m_display_raw_match_signatures.size();
       ++i)
  {
    const bool included =
        MatchesSelectedMatchFilterSignatureLocked(m_display_raw_match_signatures[i]);
    if (m_selected_match_filters_excluded ? !included : included)
      draws.push_back(m_display_raw_matches[i]);
  }
  return draws;
}

std::vector<ElementsGroupManager::SeedCandidate> ElementsGroupManager::GetSeedCandidates() const
{
  std::lock_guard lock(m_mutex);
  if (!m_hunt_enabled)
    return {};
  return m_display_seed_candidates;
}

std::vector<ElementsGroupManager::CurrentMatchCandidate> ElementsGroupManager::GetCurrentMatches() const
{
  std::lock_guard lock(m_mutex);
  return m_display_matches;
}

std::vector<ElementsGroupManager::SelectedSubgroupSignature>
ElementsGroupManager::GetSelectedMatchFilters() const
{
  std::lock_guard lock(m_mutex);
  return m_selected_match_filters;
}

bool ElementsGroupManager::GetSelectedMatchFilterExcluded() const
{
  std::lock_guard lock(m_mutex);
  return m_selected_match_filters_excluded;
}

std::vector<ElementsGroupManager::CurrentMatchCandidate>
ElementsGroupManager::GetSelectedMatchDisplayDraws() const
{
  std::lock_guard lock(m_mutex);
  return ResolveSelectedMatchDisplayDrawsLocked();
}

ElementsGroupManager::RuntimeElementSignature ElementsGroupManager::GetSeedSignature() const
{
  std::lock_guard lock(m_mutex);
  return GetMaskedSeedSignatureLocked();
}

ElementsGroupManager::Status ElementsGroupManager::GetStatus() const
{
  std::lock_guard lock(m_mutex);
  Status status;
  status.popup_open = m_popup_open_count > 0;
  status.hunt_enabled = m_hunt_enabled;
  status.option = m_hunting_option;
  status.seed_valid = m_seed_signature.valid;
  status.seed_signature = GetMaskedSeedSignatureLocked();
  status.seed_draw = m_seed_draw;
  status.selected_seed_index = m_selected_seed_index;
  status.total_seed_candidates =
      m_hunt_enabled ? static_cast<int>(m_display_seed_candidates.size()) : 0;
  status.selected_match = m_selected_match;
  status.total_matches = m_display_match_total;
  status.active_matches = static_cast<int>(std::count_if(
      m_display_matches.begin(), m_display_matches.end(),
      [](const CurrentMatchCandidate& candidate) { return candidate.active_this_frame; }));
  status.selected_match_filter_count = static_cast<int>(m_selected_match_filters.size());
  status.selected_match_filter_excluded = m_selected_match_filters_excluded;
  status.highlighted_match_raw_draw_count = m_display_highlighted_match_raw_draw_count;
  status.highlighted_draw = m_display_highlighted_draw;
  return status;
}

void ElementsGroupManager::SetSelectedMatchFilterExcluded(bool excluded)
{
  std::lock_guard lock(m_mutex);
  m_selected_match_filters_excluded = excluded;
}

bool ElementsGroupManager::IsCurrentMatchFilterEnabled(int match_index) const
{
  std::lock_guard lock(m_mutex);
  if (match_index < 0 || match_index >= static_cast<int>(m_display_matches.size()))
    return false;

  const SelectedSubgroupSignature& filter = m_display_matches[match_index].subgroup;
  return std::any_of(m_selected_match_filters.begin(), m_selected_match_filters.end(),
                     [&filter](const SelectedSubgroupSignature& existing) {
                       return SelectedSubgroupSignaturesEqual(existing, filter);
                     });
}

void ElementsGroupManager::SetCurrentMatchFilterEnabled(int match_index, bool enabled)
{
  std::lock_guard lock(m_mutex);
  if (match_index < 0 || match_index >= static_cast<int>(m_display_matches.size()))
    return;

  const SelectedSubgroupSignature& filter = m_display_matches[match_index].subgroup;
  const auto it = std::find_if(m_selected_match_filters.begin(), m_selected_match_filters.end(),
                               [&filter](const SelectedSubgroupSignature& existing) {
                                 return SelectedSubgroupSignaturesEqual(existing, filter);
                               });

  if (enabled)
  {
    if (it == m_selected_match_filters.end())
      m_selected_match_filters.push_back(filter);
  }
  else if (it != m_selected_match_filters.end())
  {
    m_selected_match_filters.erase(it);
  }
}

void ElementsGroupManager::AddCurrentMatchFilter()
{
  std::lock_guard lock(m_mutex);
  if (m_selected_match < 0 || m_selected_match >= static_cast<int>(m_display_matches.size()))
    return;

  const SelectedSubgroupSignature& filter = m_display_matches[m_selected_match].subgroup;
  const auto it = std::find_if(m_selected_match_filters.begin(), m_selected_match_filters.end(),
                               [&filter](const SelectedSubgroupSignature& existing) {
                                 return SelectedSubgroupSignaturesEqual(existing, filter);
                               });
  if (it == m_selected_match_filters.end())
    m_selected_match_filters.push_back(filter);
}

void ElementsGroupManager::RemoveSelectedMatchFilter(int index)
{
  std::lock_guard lock(m_mutex);
  if (index < 0 || index >= static_cast<int>(m_selected_match_filters.size()))
    return;
  m_selected_match_filters.erase(m_selected_match_filters.begin() + index);
}

void ElementsGroupManager::ClassifyProfileDraw(DrawRecord* draw,
                                               const MetroidProjectionMetrics& metrics)
{
  if (draw == nullptr)
    return;

  std::lock_guard lock(m_mutex);
  if (m_active_profiles.empty())
    return;

  MetroidElementProfile selected_profile = MetroidElementProfile::None;
  MetroidElementLayer selected_layer = MetroidElementLayer::Unknown;
  for (MetroidElementProfile profile : m_active_profiles)
  {
    const MetroidElementLayer layer = m_metroid_classifier.Classify(profile, metrics);
    if (selected_profile == MetroidElementProfile::None || IsClassifiedMetroidLayer(layer))
    {
      selected_profile = profile;
      selected_layer = layer;
    }
    if (IsClassifiedMetroidLayer(layer))
      break;
  }

  draw->profile_id = selected_profile;
  draw->profile_layer = selected_layer;
  draw->profile_layer_name = std::string(MetroidElementLayerToDisplayName(selected_layer));
}

ElementsGroupManager::PreviewAction ElementsGroupManager::RegisterDraw(const DrawRecord& draw)
{
  std::lock_guard lock(m_mutex);

  if (m_popup_open_count > 0 && m_hunt_enabled)
  {
    DrawRecord recorded = draw;
    recorded.draw_index = static_cast<int>(m_collecting_draws.size());
    m_collecting_draws.push_back(recorded);

    if (HasActiveHuntLocked() &&
        RuntimeElementMatcher::Matches(GetMaskedSeedSignatureLocked(), recorded.signature))
    {
      m_collecting_matches.push_back(recorded);
      m_collecting_match_total++;

      if (MatchesSelectedMatchFilterLocked(recorded))
        return m_hunting_option == HuntingOption::Pink ? PreviewAction::Pink : PreviewAction::Skip;
    }
  }

  return PreviewAction::None;
}

void ElementsGroupManager::OnFrameEnd()
{
  std::lock_guard lock(m_mutex);

  m_draw_totals_prev = m_draw_counters;
  m_draw_counters.clear();
  m_current_draw_indices.clear();
  m_metroid_classifier.ResetFrame();

  if (m_popup_open_count <= 0)
    return;

  m_display_draws = std::move(m_collecting_draws);
  m_collecting_draws.clear();
  m_display_raw_matches = std::move(m_collecting_matches);
  m_collecting_matches.clear();
  m_display_raw_match_signatures.clear();
  for (auto& candidate : m_display_matches)
  {
    candidate.active_this_frame = false;
    candidate.raw_draw_count = 0;
  }
  std::vector<SelectedSubgroupSignature> raw_signatures;
  raw_signatures.reserve(m_display_raw_matches.size());
  for (const DrawRecord& draw : m_display_raw_matches)
  {
    SelectedSubgroupSignature signature = MakeSelectedSubgroupSignature(draw);
    raw_signatures.push_back(signature);
  }
  m_display_raw_match_signatures = raw_signatures;

  for (size_t i = 0; i < m_display_raw_matches.size(); ++i)
  {
    const DrawRecord& draw = m_display_raw_matches[i];
    const SelectedSubgroupSignature& signature = raw_signatures[i];

    auto candidate_it = std::find_if(m_display_matches.begin(), m_display_matches.end(),
                                     [&signature](const CurrentMatchCandidate& candidate) {
                                       return SelectedSubgroupSignaturesEqual(candidate.subgroup,
                                                                              signature);
                                     });
    if (candidate_it == m_display_matches.end())
      m_display_matches.push_back(CurrentMatchCandidate{.subgroup = signature,
                                                        .representative_draw = draw,
                                                        .raw_draw_count = 1,
                                                        .active_this_frame = true});
    else
    {
      candidate_it->representative_draw = draw;
      candidate_it->raw_draw_count++;
      candidate_it->active_this_frame = true;
    }
  }

  m_display_match_total = static_cast<int>(m_display_matches.size());
  m_collecting_match_total = 0;
  m_display_seed_candidates.clear();
  m_display_seed_candidates.reserve(m_display_draws.size());
  for (const DrawRecord& draw : m_display_draws)
  {
    const RuntimeElementSignature group_signature = GetSeedGroupSignatureLocked(draw.signature);
    auto it = std::find_if(m_display_seed_candidates.begin(), m_display_seed_candidates.end(),
                           [&group_signature](const SeedCandidate& candidate) {
                             return SignaturesEqual(candidate.group_signature, group_signature);
                           });
    if (it == m_display_seed_candidates.end())
    {
      m_display_seed_candidates.push_back(SeedCandidate{.signature = draw.signature,
                                                        .group_signature = group_signature,
                                                        .representative_draw = draw,
                                                        .occurrence_count = 1});
    }
    else
    {
      it->signature = draw.signature;
      it->group_signature = group_signature;
      it->representative_draw = draw;
      it->occurrence_count++;
    }
  }

  if (m_seed_signature.valid)
  {
    const auto it = std::find_if(m_display_seed_candidates.begin(), m_display_seed_candidates.end(),
                                 [this](const SeedCandidate& candidate) {
                                   return SignaturesEqual(candidate.group_signature,
                                                          m_seed_group_signature);
                                 });
    if (it == m_display_seed_candidates.end())
    {
      ClearSeedSelectionLocked();
    }
    else
    {
      m_selected_seed_index = static_cast<int>(std::distance(m_display_seed_candidates.begin(), it));
      m_seed_signature = it->representative_draw.signature;
      m_seed_group_signature = it->group_signature;
      m_seed_draw = it->representative_draw;
    }
  }

  if (m_display_matches.empty())
  {
    m_selected_match = 0;
    m_display_highlighted_draw.reset();
    m_display_highlighted_match_raw_draw_count = 0;
  }
  else if (m_selected_match >= static_cast<int>(m_display_matches.size()))
  {
    m_selected_match = 0;
  }

  if (!m_display_matches.empty() && !m_display_matches[m_selected_match].active_this_frame)
  {
    const auto it =
        std::find_if(m_display_matches.begin(), m_display_matches.end(),
                     [](const CurrentMatchCandidate& candidate) { return candidate.active_this_frame; });
    if (it != m_display_matches.end())
      m_selected_match = static_cast<int>(std::distance(m_display_matches.begin(), it));
  }

  if (!m_display_matches.empty() && m_selected_match < static_cast<int>(m_display_matches.size()))
  {
    m_display_highlighted_draw = m_display_matches[m_selected_match].representative_draw;
    m_display_highlighted_match_raw_draw_count = m_display_matches[m_selected_match].raw_draw_count;
  }
  else
  {
    m_display_highlighted_match_raw_draw_count = 0;
  }

  m_stable_submatch_occurrence_counters.clear();
  m_current_stable_submatch = {};
}

bool ElementsGroupManager::HasActiveHuntLocked() const
{
  return m_popup_open_count > 0 && m_hunt_enabled && m_seed_signature.valid &&
         GroupMaskHasActiveGroups(m_group_mask.projection, m_group_mask.layer, m_group_mask.viewport,
                                  m_group_mask.scissor, m_group_mask.render_state);
}

bool ElementsGroupManager::MatchesSelectedMatchFilterLocked(const DrawRecord& draw) const
{
  if (m_selected_match_filters.empty())
    return true;

  const bool included =
      MatchesSelectedMatchFilterSignatureLocked(MakeSelectedSubgroupSignature(draw));
  return m_selected_match_filters_excluded ? !included : included;
}

bool ElementsGroupManager::MatchesSelectedMatchFilterSignatureLocked(
    const SelectedSubgroupSignature& stable_signature) const
{
  if (m_selected_match_filters.empty())
    return true;

  return std::any_of(m_selected_match_filters.begin(), m_selected_match_filters.end(),
                     [&stable_signature](const SelectedSubgroupSignature& filter) {
                       return SelectedSubgroupSignaturesEqual(filter, stable_signature);
                     });
}

std::vector<ElementsGroupManager::CurrentMatchCandidate>
ElementsGroupManager::ResolveSelectedMatchDisplayDrawsLocked() const
{
  std::vector<CurrentMatchCandidate> result;
  result.reserve(m_selected_match_filters.size());
  for (const SelectedSubgroupSignature& filter : m_selected_match_filters)
  {
    const auto it = std::find_if(m_display_matches.begin(), m_display_matches.end(),
                                 [&filter](const CurrentMatchCandidate& candidate) {
                                   return SelectedSubgroupSignaturesEqual(candidate.subgroup, filter);
                                 });
    if (it != m_display_matches.end())
      result.push_back(*it);
    else
      result.push_back(CurrentMatchCandidate{.subgroup = filter,
                                             .representative_draw = DrawRecord{.draw_index = -1},
                                             .raw_draw_count = 0});
  }
  return result;
}

ElementsGroupManager::StableSubMatchSignature
ElementsGroupManager::GetStableSubMatchSignatureLocked(const DrawRecord& draw) const
{
  if (m_current_stable_submatch.valid && m_current_stable_submatch.draw_sequence == draw.draw_sequence)
    return m_current_stable_submatch.signature;

  const StableSubMatchSignature base_signature = MakeStableSubMatchSignature(draw, -1);
  const u64 base_key = static_cast<u64>(ComputeStableSubMatchBaseKey(base_signature));
  const int occurrence_slot = m_stable_submatch_occurrence_counters[base_key]++;
  m_current_stable_submatch.valid = true;
  m_current_stable_submatch.draw_sequence = draw.draw_sequence;
  m_current_stable_submatch.signature = MakeStableSubMatchSignature(draw, occurrence_slot);
  return m_current_stable_submatch.signature;
}

void ElementsGroupManager::AdvanceOverrideDrawCounters(const DrawRecord& draw)
{
  std::lock_guard lock(m_mutex);
  GetStableSubMatchSignatureLocked(draw);
  for (const auto& entry : m_overrides)
  {
    if (entry.element_start < 0 || entry.element_end < 0)
      continue;
    if (!DoesEntryMatchForRange(entry, draw))
      continue;

    const CounterKey key = GetCounterKey(entry);
    m_current_draw_indices[key] = m_draw_counters[key];
    m_draw_counters[key]++;
  }
}

bool ElementsGroupManager::DoesTextureFilterPass(const DrawRecord& draw,
                                                 const ElementGroupOverride& entry) const
{
  if (entry.texture_hashes.empty())
    return true;

  bool any_match = false;
  for (u64 current_hash : draw.textures)
  {
    if (current_hash == 0)
      continue;
    if (std::find(entry.texture_hashes.begin(), entry.texture_hashes.end(), current_hash) !=
        entry.texture_hashes.end())
    {
      any_match = true;
      break;
    }
  }

  return entry.texture_hashes_excluded ? !any_match : any_match;
}

ElementsGroupManager::CounterKey ElementsGroupManager::GetCounterKey(
    const ElementGroupOverride& entry) const
{
  size_t seed = 0;
  HashCombineValue(seed, static_cast<int>(entry.match_kind));
  if (entry.match_kind == MatchKind::ProfileLayer)
  {
    HashCombineValue(seed, ComputeProfileElementKey(entry.profile_id, entry.profile_layers,
                                                    entry.texture_hashes,
                                                    entry.texture_hashes_excluded));
  }
  else
  {
    HashCombineValue(seed, ComputeRuntimeElementKey(entry.runtime_element, entry.texture_hashes,
                                                    entry.texture_hashes_excluded));
  }
  for (const SelectedSubgroupSignature& filter : entry.selected_match_filter)
  {
    HashCombineValue(seed, ComputeSelectedSubgroupKey(filter));
  }
  HashCombineValue(seed, entry.selected_match_filter_excluded);
  return CounterKey{static_cast<u64>(seed)};
}

std::pair<int, int> ElementsGroupManager::ResolveElementRange(const ElementGroupOverride& entry,
                                                              const DrawRecord& draw) const
{
  if (entry.element_start < 0 || entry.element_end < 0)
    return {-1, -1};

  const CounterKey key = GetCounterKey(entry);
  const auto total_it = m_draw_totals_prev.find(key);
  const int current_total = total_it != m_draw_totals_prev.end() ? total_it->second :
                                                               entry.element_reference_total;
  const int reference_total = entry.element_reference_total;
  if (current_total <= 0 || reference_total <= 0 || current_total == reference_total)
    return {entry.element_start, entry.element_end};

  const double scale = static_cast<double>(current_total) / static_cast<double>(reference_total);
  const int range_start = std::clamp(static_cast<int>(std::lround(entry.element_start * scale)),
                                     0, current_total - 1);
  const int range_end = std::clamp(static_cast<int>(std::lround(entry.element_end * scale)),
                                   0, current_total - 1);
  return {std::min(range_start, range_end), std::max(range_start, range_end)};
}

bool ElementsGroupManager::DoesEntryMatchForRange(const ElementGroupOverride& entry,
                                                  const DrawRecord& draw) const
{
  if (!entry.enabled)
    return false;

  if (entry.match_kind == MatchKind::ProfileLayer)
  {
    if (entry.profile_id == MetroidElementProfile::None || entry.profile_layers.empty() ||
        draw.profile_id != entry.profile_id)
    {
      return false;
    }
    if (std::find(entry.profile_layers.begin(), entry.profile_layers.end(), draw.profile_layer) ==
        entry.profile_layers.end())
    {
      return false;
    }
  }
  else
  {
    if (!entry.runtime_element.valid)
      return false;

    if (!RuntimeElementMatcher::Matches(entry.runtime_element, draw.signature))
      return false;
  }

  if (!DoesSelectedMatchFilterPass(entry, draw))
    return false;

  if (!DoesTextureFilterPass(draw, entry))
    return false;

  return true;
}

bool ElementsGroupManager::DoesSelectedMatchFilterPass(const ElementGroupOverride& entry,
                                                       const DrawRecord& draw) const
{
  if (entry.selected_match_filter.empty())
    return true;

  const SelectedSubgroupSignature stable_signature = MakeSelectedSubgroupSignature(draw);
  const bool included =
      std::any_of(entry.selected_match_filter.begin(), entry.selected_match_filter.end(),
                  [&stable_signature](const SelectedSubgroupSignature& filter) {
                    return SelectedSubgroupSignaturesEqual(filter, stable_signature);
                  });
  return entry.selected_match_filter_excluded ? !included : included;
}

bool ElementsGroupManager::DoesEntryMatch(const ElementGroupOverride& entry, const DrawRecord& draw,
                                          bool include_condition) const
{
  if (!DoesEntryMatchForRange(entry, draw))
    return false;

  if (entry.element_start >= 0 && entry.element_end >= 0)
  {
    const auto [range_start, range_end] = ResolveElementRange(entry, draw);
    const CounterKey key = GetCounterKey(entry);
    const auto current_it = m_current_draw_indices.find(key);
    const int current_draw_index = current_it != m_current_draw_indices.end() ? current_it->second : -1;
    if (current_draw_index < range_start || current_draw_index > range_end)
      return false;
  }

  if (include_condition && !entry.condition_flag.empty())
  {
    const bool active = ShaderHunter::GetInstance().IsFlagActive(entry.condition_flag);
    if (entry.condition_inverted ? active : !active)
      return false;
  }

  return true;
}

void ElementsGroupManager::RegisterFlagsForDraw(const DrawRecord& draw)
{
  std::lock_guard lock(m_mutex);
  GetStableSubMatchSignatureLocked(draw);
  for (const auto& entry : m_overrides)
  {
    if (entry.flag_group.empty())
      continue;
    if (!DoesEntryMatch(entry, draw, true))
      continue;
    if (ShaderHunter::GetInstance().IsDebugLogging())
    {
      INFO_LOG_FMT(VIDEO, "ElementsGroup match(flag): '{}' draw#{} flag={}", entry.name,
                   draw.draw_index + 1, entry.flag_group);
    }
    ShaderHunter::GetInstance().RegisterExternalFlag(entry.flag_group);
  }
}

bool ElementsGroupManager::ShouldSkipByOverride(const DrawRecord& draw) const
{
  std::lock_guard lock(m_mutex);
  GetStableSubMatchSignatureLocked(draw);
  for (const auto& entry : m_overrides)
  {
    if (entry.handling != HandlingType::Skip)
      continue;
    if (DoesEntryMatch(entry, draw, true))
    {
      if (ShaderHunter::GetInstance().IsDebugLogging())
      {
        INFO_LOG_FMT(VIDEO, "ElementsGroup match(skip): '{}' draw#{}", entry.name,
                     draw.draw_index + 1);
      }
      return true;
    }
  }
  return false;
}

ElementsGroupManager::HandlingType ElementsGroupManager::GetOverrideHandling(
    const DrawRecord& draw) const
{
  std::lock_guard lock(m_mutex);
  GetStableSubMatchSignatureLocked(draw);
  for (const auto& entry : m_overrides)
  {
    if (entry.handling == HandlingType::Skip || entry.handling == HandlingType::Flag)
      continue;
    if (DoesEntryMatch(entry, draw, true))
    {
      if (ShaderHunter::GetInstance().IsDebugLogging())
      {
        INFO_LOG_FMT(VIDEO, "ElementsGroup match(handling): '{}' draw#{} handling={}", entry.name,
                     draw.draw_index + 1, static_cast<int>(entry.handling));
      }
      return entry.handling;
    }
  }
  return HandlingType::Skip;
}

int ElementsGroupManager::GetOverrideLayer(const DrawRecord& draw) const
{
  std::lock_guard lock(m_mutex);
  GetStableSubMatchSignatureLocked(draw);
  for (const auto& entry : m_overrides)
  {
    if (entry.layer < 0)
      continue;
    if (DoesEntryMatch(entry, draw, true))
      return entry.layer;
  }
  return -1;
}

float ElementsGroupManager::GetOverrideElementDepth(const DrawRecord& draw) const
{
  std::lock_guard lock(m_mutex);
  GetStableSubMatchSignatureLocked(draw);
  for (const auto& entry : m_overrides)
  {
    if (entry.element_depth < 0.0f)
      continue;
    if (DoesEntryMatch(entry, draw, true))
      return entry.element_depth;
  }
  return -1.0f;
}

float ElementsGroupManager::GetOverrideUnitsPerMeter(const DrawRecord& draw) const
{
  std::lock_guard lock(m_mutex);
  GetStableSubMatchSignatureLocked(draw);
  for (const auto& entry : m_overrides)
  {
    if (entry.units_per_meter <= 0.0f)
      continue;
    if (DoesEntryMatch(entry, draw, true))
      return entry.units_per_meter;
  }
  return -1.0f;
}
