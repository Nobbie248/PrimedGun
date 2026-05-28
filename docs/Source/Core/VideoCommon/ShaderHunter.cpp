// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/ShaderHunter.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string_view>

#include "Common/FileUtil.h"
#include "Common/Hash.h"
#include "Common/Logging/Log.h"
#include "VideoCommon/GeometryShaderGen.h"
#include "VideoCommon/PixelShaderGen.h"
#include "VideoCommon/ShaderGenCommon.h"
#include "VideoCommon/VertexShaderGen.h"
#include "VideoCommon/VideoConfig.h"

namespace
{
constexpr u64 FAMILY_HASH_OFFSET = 1469598103934665603ULL;
constexpr u64 FAMILY_HASH_PRIME = 1099511628211ULL;

const char* MatchModeToConfigString(ShaderHunter::MatchMode mode)
{
  switch (mode)
  {
  case ShaderHunter::MatchMode::ShaderFamily:
    return "family";
  case ShaderHunter::MatchMode::RuntimeElement:
    return "runtime_element";
  case ShaderHunter::MatchMode::ExactHash:
  default:
    return "exact";
  }
}

void MixFamilyHash(u64& hash, u64 value)
{
  hash ^= value;
  hash *= FAMILY_HASH_PRIME;
}

u64 ComputePixelFamilySignature(const pixel_shader_uid_data& uid)
{
  u64 signature = FAMILY_HASH_OFFSET;

  // Keep TEV/shader-logic identity, ignore fog and other scene-variant state.
  MixFamilyHash(signature, uid.nIndirectStagesUsed);
  MixFamilyHash(signature, uid.genMode_numtexgens);
  MixFamilyHash(signature, uid.genMode_numtevstages);
  MixFamilyHash(signature, uid.genMode_numindstages);
  MixFamilyHash(signature, uid.numColorChans);
  MixFamilyHash(signature, static_cast<u64>(uid.Pretest));
  MixFamilyHash(signature, static_cast<u64>(uid.alpha_test_comp0));
  MixFamilyHash(signature, static_cast<u64>(uid.alpha_test_comp1));
  MixFamilyHash(signature, static_cast<u64>(uid.alpha_test_logic));
  MixFamilyHash(signature, static_cast<u64>(uid.ztex_op));
  MixFamilyHash(signature, uid.per_pixel_depth);
  MixFamilyHash(signature, static_cast<u64>(uid.ztest));

  MixFamilyHash(signature, uid.tevindref_bi0);
  MixFamilyHash(signature, uid.tevindref_bc0);
  MixFamilyHash(signature, uid.tevindref_bi1);
  MixFamilyHash(signature, uid.tevindref_bc1);
  MixFamilyHash(signature, uid.tevindref_bi2);
  MixFamilyHash(signature, uid.tevindref_bc2);
  MixFamilyHash(signature, uid.tevindref_bi3);
  MixFamilyHash(signature, uid.tevindref_bc3);

  const int stage_count =
      std::clamp(static_cast<int>(uid.genMode_numtevstages) + 1, 1, 16);
  for (int i = 0; i < stage_count; ++i)
  {
    const auto& stage = uid.stagehash[i];
    MixFamilyHash(signature, stage.cc);
    MixFamilyHash(signature, stage.ac);
    MixFamilyHash(signature, stage.tevorders_texmap);
    MixFamilyHash(signature, stage.tevorders_texcoord);
    MixFamilyHash(signature, stage.tevorders_enable);
    MixFamilyHash(signature, static_cast<u64>(stage.tevorders_colorchan));
    MixFamilyHash(signature, stage.tevind);
    MixFamilyHash(signature, static_cast<u64>(stage.ras_swap_r));
    MixFamilyHash(signature, static_cast<u64>(stage.ras_swap_g));
    MixFamilyHash(signature, static_cast<u64>(stage.ras_swap_b));
    MixFamilyHash(signature, static_cast<u64>(stage.ras_swap_a));
    MixFamilyHash(signature, static_cast<u64>(stage.tex_swap_r));
    MixFamilyHash(signature, static_cast<u64>(stage.tex_swap_g));
    MixFamilyHash(signature, static_cast<u64>(stage.tex_swap_b));
    MixFamilyHash(signature, static_cast<u64>(stage.tex_swap_a));
    MixFamilyHash(signature, static_cast<u64>(stage.tevksel_kc));
    MixFamilyHash(signature, static_cast<u64>(stage.tevksel_ka));
  }

  return signature;
}

u64 ComputeShaderFamilySignature(ShaderHunter::ShaderType type, const u8* uid_data, size_t uid_size)
{
  if (!uid_data || uid_size == 0)
    return 0;

  if (type == ShaderHunter::ShaderType::Pixel && uid_size >= sizeof(pixel_shader_uid_data))
  {
    const auto* ps_uid = reinterpret_cast<const pixel_shader_uid_data*>(uid_data);
    return ComputePixelFamilySignature(*ps_uid);
  }

  return static_cast<u64>(Common::ComputeCRC32(uid_data, static_cast<u32>(uid_size)));
}

std::vector<u64> CollectSortedTextureHashes(
    const std::unordered_map<u64, std::string>& texture_usage)
{
  std::vector<u64> hashes;
  hashes.reserve(texture_usage.size());
  for (const auto& [hash, _] : texture_usage)
    hashes.push_back(hash);
  std::sort(hashes.begin(), hashes.end());
  return hashes;
}

bool HasAnyRuntimeElementGroups(const ShaderHunter::RuntimeElementSignature& signature)
{
  return signature.use_projection || signature.use_layer || signature.use_viewport ||
         signature.use_scissor || signature.use_render_state;
}
}  // namespace

ShaderHunter& ShaderHunter::GetInstance()
{
  static ShaderHunter instance;
  return instance;
}

u64 ShaderHunter::RegisterShader(ShaderType type, u64 hash, const u8* uid_data, size_t uid_size)
{
  const u64 family_signature = ComputeShaderFamilySignature(type, uid_data, uid_size);

  std::lock_guard lock(m_mutex);
  const int t = static_cast<int>(type);
  if (hash != 0 && family_signature != 0)
    m_shader_family_signatures[t][hash] = family_signature;

  if (m_enabled)
    m_collecting[t].insert(hash);

  // Cache UID bytes for later shader source regeneration (dump)
  if (m_enabled && m_uid_cache[t].find(hash) == m_uid_cache[t].end() && uid_data && uid_size > 0)
    m_uid_cache[t][hash] = std::vector<u8>(uid_data, uid_data + uid_size);

  return family_signature;
}

std::optional<u64> ShaderHunter::GetShaderFamilySignature(ShaderType type, u64 hash) const
{
  if (hash == 0)
    return std::nullopt;

  std::lock_guard lock(m_mutex);
  const auto& per_type = m_shader_family_signatures[static_cast<int>(type)];
  const auto it = per_type.find(hash);
  if (it == per_type.end() || it->second == 0)
    return std::nullopt;
  return it->second;
}

void ShaderHunter::SetCurrentDrawShaderFamilies(u64 vs_family, u64 ps_family, u64 gs_family)
{
  m_current_vs_family = vs_family;
  m_current_ps_family = ps_family;
  m_current_gs_family = gs_family;
}

void ShaderHunter::SetCurrentDrawSignature(const RuntimeElementSignature& signature)
{
  m_current_draw_signature = signature;
}

void ShaderHunter::SetGroupHuntEnabled(bool enabled)
{
  std::lock_guard lock(m_mutex);
  m_group_hunt_enabled = enabled;
  if (!enabled)
  {
    m_group_hunt_match_total = 0;
    m_group_hunt_match_counter = 0;
    m_group_hunt_draw_counter = 0;
    m_group_hunt_selected_match = 0;
    m_group_hunt_selected_draw = {};
  }
}

bool ShaderHunter::IsGroupHuntEnabled() const
{
  std::lock_guard lock(m_mutex);
  return m_group_hunt_enabled;
}

bool ShaderHunter::SeedGroupHuntFromCurrentDraw()
{
  std::lock_guard lock(m_mutex);

  if (!m_selected_draw_signature.valid)
    return false;

  const int type_index = static_cast<int>(m_active_type);
  if (type_index < 0 || type_index >= TYPE_COUNT)
    return false;
  if (m_selected_pos[type_index] < 0 || m_selected_hash[type_index] == ~0ULL)
    return false;

  m_group_hunt_seed_signature = m_selected_draw_signature;
  m_group_hunt_seed_type = m_active_type;
  m_group_hunt_seed_hash = m_selected_hash[type_index];
  m_group_hunt_selected_match = 0;
  m_group_hunt_match_total = 0;
  m_group_hunt_match_counter = 0;
  m_group_hunt_draw_counter = 0;
  m_group_hunt_selected_draw = {};
  return true;
}

void ShaderHunter::SetGroupHuntGroupMask(bool use_projection, bool use_layer, bool use_viewport,
                                         bool use_scissor, bool use_render_state)
{
  std::lock_guard lock(m_mutex);
  m_group_hunt_seed_signature.use_projection = use_projection;
  m_group_hunt_seed_signature.use_layer = use_layer;
  m_group_hunt_seed_signature.use_viewport = use_viewport;
  m_group_hunt_seed_signature.use_scissor = use_scissor;
  m_group_hunt_seed_signature.use_render_state = use_render_state;
}

ShaderHunter::RuntimeElementSignature ShaderHunter::GetGroupHuntSeedSignature() const
{
  std::lock_guard lock(m_mutex);
  return m_group_hunt_seed_signature;
}

std::optional<ShaderHunter::HuntedDraw> ShaderHunter::GetGroupHuntHighlightedDraw() const
{
  std::lock_guard lock(m_mutex);
  if (!HasActiveGroupHuntLocked() || !m_group_hunt_selected_draw.valid)
    return std::nullopt;
  return m_group_hunt_selected_draw;
}

void ShaderHunter::RegisterDrawCombination(u64 vs_hash, u64 ps_hash, u64 gs_hash)
{
  if (!m_enabled)
    return;
  std::lock_guard lock(m_mutex);

  const auto record_textures_for_shader = [this](ShaderType type, u64 shader_hash) {
    auto& shader_map = m_shader_texture_usage_collecting[static_cast<int>(type)];
    auto& textures_for_hash = shader_map[shader_hash];
    for (int i = 0; i < 8; i++)
    {
      const u64 texture_hash = m_current_draw_textures[i];
      if (texture_hash == 0)
        continue;
      auto& name = textures_for_hash[texture_hash];
      if (name.empty())
        name = m_current_draw_texture_names[i];
    }
  };

  record_textures_for_shader(ShaderType::Vertex, vs_hash);
  record_textures_for_shader(ShaderType::Pixel, ps_hash);
  record_textures_for_shader(ShaderType::Geometry, gs_hash);
}

void ShaderHunter::OnFrameEnd()
{
  // Always update flags, even when hunting is disabled (flags serve persistent overrides).
  // Log activations/deactivations.
  for (const auto& flag : m_flags_seen_this_frame)
  {
    if (m_flag_age.find(flag) == m_flag_age.end())
      INFO_LOG_FMT(VIDEO, "ShaderHunter: Flag '{}' activated", flag);
  }
  for (const auto& [flag, _] : m_flag_age)
  {
    if (m_flags_seen_this_frame.count(flag) == 0)
      INFO_LOG_FMT(VIDEO, "ShaderHunter: Flag '{}' deactivated", flag);
  }

  // Swap: only flags seen this frame are active next frame.
  m_flag_age.clear();
  for (const auto& flag : m_flags_seen_this_frame)
    m_flag_age[flag] = 0;
  m_flags_seen_this_frame.clear();

  // Clear debug logging per-frame dedup set
  m_debug_logged_combos.clear();

  // Reset per-hash override draw counters for next frame (always, even when hunting disabled)
  if (m_has_element_overrides)
  {
    m_override_draw_totals_prev = m_override_draw_counters;
    m_override_draw_counters.clear();
  }
  else
  {
    m_override_draw_totals_prev.clear();
  }

  m_group_hunt_match_total = m_group_hunt_match_counter;
  m_group_hunt_match_counter = 0;
  m_group_hunt_draw_counter = 0;
  m_group_hunt_selected_draw = {};
  if (m_group_hunt_match_total <= 0)
    m_group_hunt_selected_match = 0;
  else if (m_group_hunt_selected_match >= m_group_hunt_match_total)
    m_group_hunt_selected_match = 0;

  if (!m_enabled)
    return;
  std::lock_guard lock(m_mutex);
  for (int i = 0; i < TYPE_COUNT; i++)
  {
    m_display[i] = std::move(m_collecting[i]);
    m_collecting[i].clear();
  }

  // Element mode: store total draw count for UI, reset per-frame counter
  m_element_total = m_element_counter;
  m_element_counter = 0;

  // Swap element texture capture buffers
  m_element_textures_display = std::move(m_element_textures_collecting);
  m_element_textures_collecting.clear();
  m_element_tex_names_display = std::move(m_element_tex_names_collecting);
  m_element_tex_names_collecting.clear();
  m_shader_texture_usage_display = std::move(m_shader_texture_usage_collecting);
  for (auto& per_type : m_shader_texture_usage_collecting)
    per_type.clear();
  m_texture_usage_display = std::move(m_texture_usage_collecting);
  m_texture_usage_collecting.clear();
}

bool ShaderHunter::ShouldSkipDraw(u64 vs_hash, u64 ps_hash, u64 gs_hash)
{
  if (!m_enabled)
  {
    m_should_highlight_selected_draw = false;
    return false;
  }
  std::lock_guard lock(m_mutex);

  const int current_draw_index = m_group_hunt_draw_counter++;
  if (HasActiveGroupHuntLocked())
  {
    const bool matches_group = IsRuntimeElementMatch(m_group_hunt_seed_signature);
    if (matches_group)
    {
      const int match_index = m_group_hunt_match_counter++;
      const bool selected_match = match_index == m_group_hunt_selected_match;
      m_should_highlight_selected_draw = selected_match && m_hunting_option == HuntingOption::Pink;
      if (selected_match)
      {
        m_group_hunt_selected_draw.valid = true;
        m_group_hunt_selected_draw.type = m_group_hunt_seed_type;
        m_group_hunt_selected_draw.hash =
            static_cast<u64>(GetHashForType(m_group_hunt_seed_type, vs_hash, ps_hash, gs_hash));
        m_group_hunt_selected_draw.signature = m_current_draw_signature;
        m_group_hunt_selected_draw.textures = m_current_draw_textures;
        m_group_hunt_selected_draw.texture_names = m_current_draw_texture_names;
        m_group_hunt_selected_draw.draw_index = current_draw_index;
        m_group_hunt_selected_draw.match_index = match_index;
      }

      return selected_match && m_hunting_option == HuntingOption::Skip;
    }

    m_should_highlight_selected_draw = false;
    return false;
  }

  const int t = static_cast<int>(m_active_type);
  if (m_selected_pos[t] < 0)
  {
    m_should_highlight_selected_draw = false;
    return false;
  }

  const u64 sel = m_selected_hash[t];
  bool matches = false;
  switch (m_active_type)
  {
  case ShaderType::Pixel:   matches = (ps_hash == sel); break;
  case ShaderType::Vertex:  matches = (vs_hash == sel); break;
  case ShaderType::Geometry: matches = (gs_hash == sel); break;
  default:
    m_should_highlight_selected_draw = false;
    return false;
  }

  if (!matches)
  {
    m_should_highlight_selected_draw = false;
    return false;
  }

  // Record unique textures used by this selected shader in the current frame.
  for (int i = 0; i < 8; i++)
  {
    const u64 texture_hash = m_current_draw_textures[i];
    if (texture_hash == 0)
      continue;
    auto& name = m_texture_usage_collecting[texture_hash];
    if (name.empty())
      name = m_current_draw_texture_names[i];
  }

  // Count this draw call for element mode tracking
  const int draw_index = m_element_counter++;

  // Capture per-element textures during hunting (for UI display)
  if (m_element_mode)
  {
    // Grow the collecting vectors to fit this draw index
    if (draw_index >= static_cast<int>(m_element_textures_collecting.size()))
    {
      m_element_textures_collecting.resize(draw_index + 1);
      m_element_tex_names_collecting.resize(draw_index + 1);
    }
    m_element_textures_collecting[draw_index] = m_current_draw_textures;
    m_element_tex_names_collecting[draw_index] = m_current_draw_texture_names;
  }

  bool selected_draw_matches_hunting = false;

  // Texture-filter mode: target only checked texture hashes; unchecked means no action.
  if (m_selected_texture_hash != 0 || m_texture_skip_mode_active)
  {
    // Texture hash hunting uses the union of:
    // 1) the currently selected hash from Next/Previous Texture Hash
    // 2) all manually added hashes from Add/Remove Texture Hash
    bool has_any_texture_target = false;

    if (m_selected_texture_hash != 0)
    {
      has_any_texture_target = true;
      for (u64 texture_hash : m_current_draw_textures)
      {
        if (texture_hash == m_selected_texture_hash)
        {
          selected_draw_matches_hunting = true;
          break;
        }
      }
    }

    if (!selected_draw_matches_hunting && !m_texture_skip_filters.empty())
    {
      has_any_texture_target = true;
      for (u64 texture_hash : m_current_draw_textures)
      {
        if (texture_hash != 0 && m_texture_skip_filters.count(texture_hash) > 0)
        {
          selected_draw_matches_hunting = true;
          break;
        }
      }
    }

    if (!has_any_texture_target)
    {
      m_should_highlight_selected_draw = false;
      return false;
    }
  }
  else if (m_element_mode)
  {
    // Range mode: target all draws in [start, end]
    if (m_element_start >= 0 && m_element_end >= 0)
      selected_draw_matches_hunting = draw_index >= m_element_start && draw_index <= m_element_end;
    // Single element mode: target only the cursor
    else if (m_selected_element >= 0)
      selected_draw_matches_hunting = draw_index == m_selected_element;
  }
  else
  {
    // Normal mode: target all draws with this selected hash.
    selected_draw_matches_hunting = true;
  }

  m_should_highlight_selected_draw =
      selected_draw_matches_hunting && m_hunting_option == HuntingOption::Pink;
  if (!selected_draw_matches_hunting)
    return false;

  if (m_current_draw_signature.valid)
  {
    m_selected_draw_signature = m_current_draw_signature;
    m_selected_draw_signature_type = m_active_type;
    m_selected_draw_signature_hash = sel;
  }

  return m_hunting_option == HuntingOption::Skip;
}

void ShaderHunter::SetEnabled(bool enabled)
{
  std::lock_guard lock(m_mutex);
  m_enabled = enabled;
  if (!enabled)
  {
    m_should_highlight_selected_draw = false;
    m_selected_draw_signature = {};
    m_selected_draw_signature_hash = 0;
    m_group_hunt_enabled = false;
    m_group_hunt_seed_signature = {};
    m_group_hunt_seed_hash = 0;
    m_group_hunt_match_total = 0;
    m_group_hunt_match_counter = 0;
    m_group_hunt_draw_counter = 0;
    m_group_hunt_selected_match = 0;
    m_group_hunt_selected_draw = {};
    for (int i = 0; i < TYPE_COUNT; i++)
    {
      m_collecting[i].clear();
      m_display[i].clear();
      m_selected_hash[i] = ~0ULL;
      m_selected_pos[i] = -1;
    }
    m_texture_skip_filters.clear();
    m_selected_texture_hash = 0;
    m_selected_texture_pos = -1;
    m_texture_skip_mode_active = false;
    m_texture_tool_active.store(false, std::memory_order_relaxed);
    m_current_vs_family = 0;
    m_current_ps_family = 0;
    m_current_gs_family = 0;
    m_texture_usage_collecting.clear();
    m_texture_usage_display.clear();
    for (auto& per_type : m_shader_texture_usage_collecting)
      per_type.clear();
    for (auto& per_type : m_shader_texture_usage_display)
      per_type.clear();
  }
}

bool ShaderHunter::IsEnabled() const
{
  return m_enabled;
}

void ShaderHunter::SetHuntingOption(HuntingOption option)
{
  std::lock_guard lock(m_mutex);
  m_hunting_option = option;
}

ShaderHunter::HuntingOption ShaderHunter::GetHuntingOption() const
{
  std::lock_guard lock(m_mutex);
  return m_hunting_option;
}

bool ShaderHunter::ShouldHighlightSelectedDraw() const
{
  std::lock_guard lock(m_mutex);
  return m_should_highlight_selected_draw;
}

void ShaderHunter::SetActiveType(ShaderType type)
{
  std::lock_guard lock(m_mutex);
  if (m_active_type != type)
  {
    m_group_hunt_enabled = false;
    m_group_hunt_selected_draw = {};
    m_group_hunt_match_total = 0;
    m_group_hunt_match_counter = 0;
    m_group_hunt_draw_counter = 0;
    m_texture_skip_filters.clear();
    m_selected_texture_hash = 0;
    m_selected_texture_pos = -1;
    m_texture_skip_mode_active = false;
    m_texture_usage_collecting.clear();
    m_texture_usage_display.clear();
  }
  m_active_type = type;
}

ShaderHunter::ShaderType ShaderHunter::GetActiveType() const
{
  return m_active_type;
}

void ShaderHunter::NextShader()
{
  std::lock_guard lock(m_mutex);
  if (HasActiveGroupHuntLocked())
  {
    if (m_group_hunt_match_total <= 0)
      return;
    m_group_hunt_selected_match++;
    if (m_group_hunt_selected_match >= m_group_hunt_match_total)
      m_group_hunt_selected_match = 0;
    return;
  }
  const int t = static_cast<int>(m_active_type);
  auto& visited = m_display[t];
  if (visited.empty())
    return;

  auto it = visited.find(m_selected_hash[t]);
  if (it != visited.end())
  {
    ++it;
    if (it != visited.end())
    {
      m_selected_pos[t]++;
    }
    else
    {
      it = visited.begin();
      m_selected_pos[t] = 0;
    }
  }
  else
  {
    it = visited.begin();
    m_selected_pos[t] = 0;
  }
  const u64 previous = m_selected_hash[t];
  m_selected_hash[t] = *it;
  if (previous != m_selected_hash[t])
  {
    m_texture_skip_filters.clear();
    m_selected_texture_hash = 0;
    m_selected_texture_pos = -1;
    m_texture_skip_mode_active = false;
    m_texture_usage_collecting.clear();
    m_texture_usage_display.clear();
  }
}

void ShaderHunter::PrevShader()
{
  std::lock_guard lock(m_mutex);
  if (HasActiveGroupHuntLocked())
  {
    if (m_group_hunt_match_total <= 0)
      return;
    m_group_hunt_selected_match--;
    if (m_group_hunt_selected_match < 0)
      m_group_hunt_selected_match = m_group_hunt_match_total - 1;
    return;
  }
  const int t = static_cast<int>(m_active_type);
  auto& visited = m_display[t];
  if (visited.empty())
    return;

  auto it = visited.find(m_selected_hash[t]);
  if (it != visited.end())
  {
    if (it != visited.begin())
    {
      --it;
      m_selected_pos[t]--;
    }
    else
    {
      it = std::prev(visited.end());
      m_selected_pos[t] = static_cast<int>(visited.size()) - 1;
    }
  }
  else
  {
    it = std::prev(visited.end());
    m_selected_pos[t] = static_cast<int>(visited.size()) - 1;
  }
  const u64 previous = m_selected_hash[t];
  m_selected_hash[t] = *it;
  if (previous != m_selected_hash[t])
  {
    m_texture_skip_filters.clear();
    m_selected_texture_hash = 0;
    m_selected_texture_pos = -1;
    m_texture_skip_mode_active = false;
    m_texture_usage_collecting.clear();
    m_texture_usage_display.clear();
  }
}

bool ShaderHunter::SelectShader(ShaderType type, u64 hash)
{
  std::lock_guard lock(m_mutex);
  const int t = static_cast<int>(type);
  auto& visited = m_display[t];
  if (visited.empty())
    return false;

  const auto it = visited.find(hash);
  if (it == visited.end())
    return false;

  const bool type_changed = (m_active_type != type);
  const bool hash_changed = (m_selected_hash[t] != hash);
  if (type_changed || hash_changed)
  {
    m_texture_skip_filters.clear();
    m_selected_texture_hash = 0;
    m_selected_texture_pos = -1;
    m_texture_skip_mode_active = false;
    m_texture_usage_collecting.clear();
    m_texture_usage_display.clear();
  }

  m_active_type = type;
  m_selected_hash[t] = hash;
  m_selected_pos[t] = static_cast<int>(std::distance(visited.begin(), it));
  return true;
}

u64 ShaderHunter::GetSelectedHash() const
{
  std::lock_guard lock(m_mutex);
  return m_selected_hash[static_cast<int>(m_active_type)];
}

int ShaderHunter::GetSelectedPosition() const
{
  std::lock_guard lock(m_mutex);
  return m_selected_pos[static_cast<int>(m_active_type)];
}

int ShaderHunter::GetTotalCount() const
{
  std::lock_guard lock(m_mutex);
  return static_cast<int>(m_display[static_cast<int>(m_active_type)].size());
}

void ShaderHunter::SetElementMode(bool enabled)
{
  std::lock_guard lock(m_mutex);
  m_element_mode = enabled;
  if (enabled)
  {
    if (m_selected_element < 0)
      m_selected_element = 0;
  }
  else
  {
    // Reset range when element mode is disabled
    m_element_start = -1;
    m_element_end = -1;
  }
}

bool ShaderHunter::IsElementMode() const
{
  return m_element_mode;
}

void ShaderHunter::NextElement()
{
  std::lock_guard lock(m_mutex);
  if (m_element_total <= 0)
    return;
  m_selected_element++;
  if (m_selected_element >= m_element_total)
    m_selected_element = 0;
}

void ShaderHunter::PrevElement()
{
  std::lock_guard lock(m_mutex);
  if (m_element_total <= 0)
    return;
  m_selected_element--;
  if (m_selected_element < 0)
    m_selected_element = m_element_total - 1;
}

int ShaderHunter::GetSelectedElement() const
{
  return m_selected_element;
}

int ShaderHunter::GetElementTotal() const
{
  return m_element_total;
}

void ShaderHunter::MarkRangeStart()
{
  std::lock_guard lock(m_mutex);
  if (m_selected_element >= 0)
  {
    m_element_start = m_selected_element;
    // If end is not set or is before start, snap it to start
    if (m_element_end < m_element_start)
      m_element_end = m_element_start;
  }
}

void ShaderHunter::MarkRangeEnd()
{
  std::lock_guard lock(m_mutex);
  if (m_selected_element >= 0)
  {
    m_element_end = m_selected_element;
    // If start is not set or is after end, snap it to end
    if (m_element_start < 0 || m_element_start > m_element_end)
      m_element_start = m_element_end;
  }
}

void ShaderHunter::ClearRange()
{
  std::lock_guard lock(m_mutex);
  m_element_start = -1;
  m_element_end = -1;
}

int ShaderHunter::GetElementStart() const
{
  return m_element_start;
}

int ShaderHunter::GetElementEnd() const
{
  return m_element_end;
}

void ShaderHunter::SetCurrentDrawTextures(const std::array<u64, 8>& hashes,
                                           const std::array<std::string, 8>& names)
{
  m_current_draw_textures = hashes;
  m_current_draw_texture_names = names;
}

std::optional<ShaderHunter::RuntimeElementSignature>
ShaderHunter::GetSelectedRuntimeElementSignature() const
{
  std::lock_guard lock(m_mutex);

  if (HasActiveGroupHuntLocked() && m_group_hunt_selected_draw.valid)
    return m_group_hunt_selected_draw.signature;

  if (!m_selected_draw_signature.valid)
    return std::nullopt;

  const int type_index = static_cast<int>(m_active_type);
  if (type_index < 0 || type_index >= TYPE_COUNT)
    return std::nullopt;
  if (m_selected_pos[type_index] < 0 || m_selected_hash[type_index] == ~0ULL)
    return std::nullopt;
  if (m_selected_draw_signature_type != m_active_type ||
      m_selected_draw_signature_hash != m_selected_hash[type_index])
  {
    return std::nullopt;
  }

  return m_selected_draw_signature;
}

bool ShaderHunter::HasActiveGroupHuntLocked() const
{
  return m_group_hunt_enabled && m_group_hunt_seed_signature.valid &&
         HasAnyRuntimeElementGroups(m_group_hunt_seed_signature);
}

u64 ShaderHunter::GetHashForType(ShaderType type, u64 vs_hash, u64 ps_hash, u64 gs_hash) const
{
  switch (type)
  {
  case ShaderType::Vertex:
    return vs_hash;
  case ShaderType::Pixel:
    return ps_hash;
  case ShaderType::Geometry:
    return gs_hash;
  default:
    return 0;
  }
}

std::array<u64, 8> ShaderHunter::GetElementTextures(int element) const
{
  std::lock_guard lock(m_mutex);
  if (element >= 0 && element < static_cast<int>(m_element_textures_display.size()))
    return m_element_textures_display[element];
  return {};
}

std::array<std::string, 8> ShaderHunter::GetElementTextureNames(int element) const
{
  std::lock_guard lock(m_mutex);
  if (element >= 0 && element < static_cast<int>(m_element_tex_names_display.size()))
    return m_element_tex_names_display[element];
  return {};
}

// ============================================================================
// Static INI helpers — direct file I/O to avoid IniFile's key-value parsing
// corrupting our raw-line sections.
// ============================================================================

// Helper: read file contents, stripping [ShaderOverride_Enable] and [ShaderOverride] sections.
static std::string ReadFileWithoutShaderSections(const std::string& path)
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

    if (trimmed == "[ShaderOverride_Enable]" || trimmed == "[ShaderOverride]")
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

// Helper: parse a key=value line with optional whitespace around '='.
static bool ParseKeyValue(const std::string& line, std::string& key, std::string& value)
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

static std::string EscapeIniText(std::string_view text)
{
  std::string escaped;
  escaped.reserve(text.size());
  for (const char c : text)
  {
    switch (c)
    {
    case '\\':
      escaped += "\\\\";
      break;
    case '\n':
      escaped += "\\n";
      break;
    case '\r':
      break;
    default:
      escaped.push_back(c);
      break;
    }
  }
  return escaped;
}

static std::string UnescapeIniText(std::string_view text)
{
  std::string unescaped;
  unescaped.reserve(text.size());
  for (size_t i = 0; i < text.size(); i++)
  {
    const char c = text[i];
    if (c == '\\' && i + 1 < text.size())
    {
      const char next = text[i + 1];
      if (next == 'n')
      {
        unescaped.push_back('\n');
        i++;
        continue;
      }
      if (next == '\\')
      {
        unescaped.push_back('\\');
        i++;
        continue;
      }
    }
    unescaped.push_back(c);
  }
  return unescaped;
}

// Helper: parse one or more hex texture hashes from a comma/space/semicolon separated list.
// Invalid tokens are ignored.
static std::vector<u64> ParseTextureHashList(const std::string& value)
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

// Helper: evaluate texture hash filter against currently bound textures.
// Include mode: pass when any listed hash matches.
// Exclude mode: pass when none of the listed hashes match.
static bool DoesTextureFilterPass(const std::array<u64, 8>& current_textures,
                                  const std::vector<u64>& filter_hashes, bool exclude_mode)
{
  if (filter_hashes.empty())
    return true;

  bool any_match = false;
  for (u64 current_hash : current_textures)
  {
    if (current_hash == 0)
      continue;
    if (std::find(filter_hashes.begin(), filter_hashes.end(), current_hash) != filter_hashes.end())
    {
      any_match = true;
      break;
    }
  }

  return exclude_mode ? !any_match : any_match;
}

static std::string GetVRGameSettingsPath(const std::string& game_id)
{
  return File::GetUserPath(D_GAMESETTINGSVR_IDX) + game_id + ".ini";
}

std::vector<ShaderHunter::ShaderOverride>
ShaderHunter::LoadOverridesFromINI(const std::string& game_id)
{
  std::vector<ShaderOverride> result;
  if (game_id.empty())
    return result;

  const std::string path = GetVRGameSettingsPath(game_id);
  std::ifstream file(path);
  if (!file.is_open())
    return result;

  // First pass: read [ShaderOverride_Enable] to get enabled names
  std::set<std::string> enabled_names;
  bool has_enable_section = false;
  {
    bool in_section = false;
    std::string line;
    while (std::getline(file, line))
    {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();

      if (line == "[ShaderOverride_Enable]")
      {
        in_section = true;
        has_enable_section = true;
        continue;
      }
      if (in_section && !line.empty() && line[0] == '[')
        break;
      if (!in_section || line.empty())
        continue;
      if (line[0] == '$')
        enabled_names.insert(line.substr(1));
    }
  }

  // Second pass: read [ShaderOverride] for override data
  file.clear();
  file.seekg(0);

  bool in_section = false;
  ShaderOverride current{};
  bool has_entry = false;

  auto commit_entry = [&]() {
    if (!has_entry || current.hash == 0)
      return;
    std::sort(current.texture_hashes.begin(), current.texture_hashes.end());
    current.texture_hashes.erase(
        std::unique(current.texture_hashes.begin(), current.texture_hashes.end()),
        current.texture_hashes.end());
    // Backward compatibility: old INIs may not have [ShaderOverride_Enable].
    // In that case, treat all entries as enabled.
    current.enabled = has_enable_section ? (enabled_names.count(current.name) > 0) : true;
    result.push_back(current);
  };

  std::string line;
  while (std::getline(file, line))
  {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();

    if (line == "[ShaderOverride]")
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
      current.hash = 0;
      current.type = ShaderType::Pixel;
      current.enabled = false;
      current.user_defined = true;
      current.condition_flag.clear();
      current.condition_inverted = false;
      has_entry = true;
    }
    else if (has_entry)
    {
      // Backward compatibility: accept legacy '+' prefixes from older files.
      std::string data_line = line;
      if (!data_line.empty() && data_line[0] == '+')
        data_line = data_line.substr(1);

      std::string key, value;
      if (!ParseKeyValue(data_line, key, value))
        continue;

      if (key == "Hash")
        current.hash = std::strtoull(value.c_str(), nullptr, 16);
      else if (key == "comments")
      {
        current.comments = UnescapeIniText(value);
      }
      else if (key == "credits")
      {
        current.credits = UnescapeIniText(value);
      }
      else if (key == "Type")
      {
        if (value == "PS")
          current.type = ShaderType::Pixel;
        else if (value == "VS")
          current.type = ShaderType::Vertex;
        else if (value == "GS")
          current.type = ShaderType::Geometry;
      }
      else if (key == "handling")
      {
        if (value == "screen")
          current.handling = HandlingType::Screen;
        else if (value == "fullscreen")
          current.handling = HandlingType::Fullscreen;
        else if (value == "fullscreen_mono" || value == "fullscreenmono" || value == "mono")
          current.handling = HandlingType::FullscreenMono;
        else if (value == "headlocked")
          current.handling = HandlingType::HeadLocked;
        else if (value == "flag")
          current.handling = HandlingType::Flag;
        else if (value == "units_per_meter" || value == "unitspermeter" || value == "upm")
          current.handling = HandlingType::UnitsPerMeter;
        else
          current.handling = HandlingType::Skip;
      }
      else if (key == "layer")
      {
        current.layer = std::stoi(value);
      }
      else if (key == "element_depth")
      {
        current.element_depth = std::stof(value);
      }
      else if (key == "units_per_meter" || key == "upm")
      {
        current.units_per_meter = std::stof(value);
      }
      else if (key == "flag")
      {
        current.flag_group = value;
      }
      else if (key == "condition")
      {
        std::string cond = value;
        // Legacy compatibility: support inline negation ("!flag", "not:flag").
        if (!cond.empty() && cond[0] == '!')
        {
          current.condition_inverted = true;
          cond.erase(cond.begin());
        }
        else if (cond.rfind("not:", 0) == 0)
        {
          current.condition_inverted = true;
          cond = cond.substr(4);
        }
        current.condition_flag = cond;
      }
      else if (key == "condition_mode")
      {
        std::string mode = value;
        std::transform(mode.begin(), mode.end(), mode.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (mode == "deactivate" || mode == "inactive" || mode == "not" || mode == "1" ||
            mode == "true")
        {
          current.condition_inverted = true;
        }
        else if (mode == "activate" || mode == "active" || mode == "0" || mode == "false")
        {
          current.condition_inverted = false;
        }
      }
      else if (key == "clear_efb")
      {
        current.clear_efb = (value == "1" || value == "true");
      }
      else if (key == "clear_efb_min")
      {
        current.clear_efb_min_width = std::stoi(value);
      }
      else if (key == "clear_efb_max")
      {
        current.clear_efb_max_width = std::stoi(value);
      }
      else if (key == "element_start")
      {
        current.element_start = std::stoi(value);
      }
      else if (key == "element_end")
      {
        current.element_end = std::stoi(value);
      }
      else if (key == "element_total")
      {
        current.element_reference_total = std::stoi(value);
      }
      else if (key == "texture")
      {
        const auto parsed_hashes = ParseTextureHashList(value);
        current.texture_hashes.insert(current.texture_hashes.end(), parsed_hashes.begin(),
                                      parsed_hashes.end());
      }
      else if (key == "texture_mode")
      {
        std::string mode = value;
        std::transform(mode.begin(), mode.end(), mode.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (mode == "exclude" || mode == "excluded" || mode == "not")
          current.texture_hashes_excluded = true;
        else if (mode == "include" || mode == "included")
          current.texture_hashes_excluded = false;
      }
      else if (key == "hash_family")
      {
        std::string mode = value;
        std::transform(mode.begin(), mode.end(), mode.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        current.hash_family_match = (mode == "1" || mode == "true" || mode == "yes" ||
                                     mode == "on" || mode == "family");
      }
      else if (key == "family_signature")
      {
        current.family_signature = std::strtoull(value.c_str(), nullptr, 16);
      }
      else if (key == "match_mode")
      {
        std::string mode = value;
        std::transform(mode.begin(), mode.end(), mode.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (mode == "runtime" || mode == "runtime_element" || mode == "element")
          current.match_mode = MatchMode::RuntimeElement;
        else if (mode == "family")
          current.match_mode = MatchMode::ShaderFamily;
        else
          current.match_mode = MatchMode::ExactHash;
      }
      else if (key == "element_projection")
      {
        current.runtime_element.valid = true;
        std::string projection = value;
        std::transform(projection.begin(), projection.end(), projection.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        current.runtime_element.perspective = projection == "perspective";
      }
      else if (key == "element_use_projection")
      {
        current.runtime_element.use_projection = (value == "1" || value == "true");
      }
      else if (key == "element_use_layer")
      {
        current.runtime_element.use_layer = (value == "1" || value == "true");
      }
      else if (key == "element_use_viewport")
      {
        current.runtime_element.use_viewport = (value == "1" || value == "true");
      }
      else if (key == "element_use_scissor")
      {
        current.runtime_element.use_scissor = (value == "1" || value == "true");
      }
      else if (key == "element_use_render_state")
      {
        current.runtime_element.use_render_state = (value == "1" || value == "true");
      }
      else if (key == "element_perspective_hfov_x100")
      {
        current.runtime_element.perspective_hfov_x100 = std::stoi(value);
      }
      else if (key == "element_perspective_vfov_x100")
      {
        current.runtime_element.perspective_vfov_x100 = std::stoi(value);
      }
      else if (key == "element_perspective_near_x1000")
      {
        current.runtime_element.perspective_near_x1000 = std::stoi(value);
      }
      else if (key == "element_perspective_far_x100")
      {
        current.runtime_element.perspective_far_x100 = std::stoi(value);
      }
      else if (key == "element_ortho_left_x100")
      {
        current.runtime_element.ortho_left_x100 = std::stoi(value);
      }
      else if (key == "element_ortho_right_x100")
      {
        current.runtime_element.ortho_right_x100 = std::stoi(value);
      }
      else if (key == "element_ortho_top_x100")
      {
        current.runtime_element.ortho_top_x100 = std::stoi(value);
      }
      else if (key == "element_ortho_bottom_x100")
      {
        current.runtime_element.ortho_bottom_x100 = std::stoi(value);
      }
      else if (key == "element_ortho_near_x100")
      {
        current.runtime_element.ortho_near_x100 = std::stoi(value);
      }
      else if (key == "element_ortho_far_x100")
      {
        current.runtime_element.ortho_far_x100 = std::stoi(value);
      }
      else if (key == "element_layer")
      {
        current.runtime_element.ortho_layer = std::stoi(value);
      }
      else if (key == "element_viewport_x")
      {
        current.runtime_element.viewport_x = std::stoi(value);
      }
      else if (key == "element_viewport_y")
      {
        current.runtime_element.viewport_y = std::stoi(value);
      }
      else if (key == "element_viewport_width")
      {
        current.runtime_element.viewport_width = std::stoi(value);
      }
      else if (key == "element_viewport_height")
      {
        current.runtime_element.viewport_height = std::stoi(value);
      }
      else if (key == "element_scissor_left")
      {
        current.runtime_element.scissor_left = std::stoi(value);
      }
      else if (key == "element_scissor_top")
      {
        current.runtime_element.scissor_top = std::stoi(value);
      }
      else if (key == "element_scissor_right")
      {
        current.runtime_element.scissor_right = std::stoi(value);
      }
      else if (key == "element_scissor_bottom")
      {
        current.runtime_element.scissor_bottom = std::stoi(value);
      }
      else if (key == "element_alpha_test_hex")
      {
        current.runtime_element.alpha_test_hex = std::strtoul(value.c_str(), nullptr, 16);
      }
      else if (key == "element_blend_color_update")
      {
        current.runtime_element.blend_color_update = (value == "1" || value == "true");
      }
      else if (key == "element_blend_alpha_update")
      {
        current.runtime_element.blend_alpha_update = (value == "1" || value == "true");
      }
      else if (key == "element_ztest")
      {
        current.runtime_element.ztest = (value == "1" || value == "true");
      }
      else if (key == "element_zupdate")
      {
        current.runtime_element.zupdate = (value == "1" || value == "true");
      }
      else if (key == "element_zfunc")
      {
        current.runtime_element.zfunc = std::stoi(value);
      }
    }
  }

  commit_entry();
  for (auto& entry : result)
  {
    if (entry.match_mode == MatchMode::ExactHash && entry.hash_family_match)
      entry.match_mode = MatchMode::ShaderFamily;
  }
  return result;
}

void ShaderHunter::SaveOverridesToINI(const std::string& game_id,
                                      const std::vector<ShaderOverride>& overrides)
{
  if (game_id.empty())
    return;

  const std::string path = GetVRGameSettingsPath(game_id);
  File::CreateFullPath(path);
  std::string base = ReadFileWithoutShaderSections(path);

  // Strip trailing whitespace/newlines, then add one newline separator
  while (!base.empty() && (base.back() == '\n' || base.back() == '\r' || base.back() == ' '))
    base.pop_back();
  if (!base.empty())
    base += "\n";

  std::ostringstream out;
  out << base;

  // [ShaderOverride_Enable] — lists enabled override names
  out << "[ShaderOverride_Enable]\n";
  for (const auto& ovr : overrides)
  {
    if (ovr.enabled)
      out << "$" << ovr.name << "\n";
  }

  // [ShaderOverride] — full override data (all overrides, enabled or not)
  // Data lines are written as plain key=value in GameSettingsVR.
  out << "[ShaderOverride]\n";
  for (const auto& ovr : overrides)
  {
    const char* type_str = ovr.type == ShaderType::Vertex   ? "VS" :
                           ovr.type == ShaderType::Geometry  ? "GS" :
                                                               "PS";
    const char* handling_str = ovr.handling == HandlingType::Screen      ? "screen" :
                               ovr.handling == HandlingType::Fullscreen  ? "fullscreen" :
                               ovr.handling == HandlingType::FullscreenMono ?
                                   "fullscreen_mono" :
                               ovr.handling == HandlingType::HeadLocked  ? "headlocked" :
                               ovr.handling == HandlingType::Flag        ? "flag" :
                               ovr.handling == HandlingType::UnitsPerMeter ? "units_per_meter" :
                                                                           "skip";
    out << "$" << ovr.name << "\n";
    if (!ovr.comments.empty())
      out << "comments=" << EscapeIniText(ovr.comments) << "\n";
    if (!ovr.credits.empty())
      out << "credits=" << EscapeIniText(ovr.credits) << "\n";
    out << "Hash=" << fmt::format("{:016x}", ovr.hash) << "\n";
    out << "Type=" << type_str << "\n";
    out << "match_mode=" << MatchModeToConfigString(ovr.match_mode) << "\n";
    out << "handling=" << handling_str << "\n";
    if (ovr.layer >= 0)
      out << "layer=" << ovr.layer << "\n";
    if (ovr.element_depth >= 0.0f)
      out << "element_depth=" << ovr.element_depth << "\n";
    if (ovr.handling == HandlingType::UnitsPerMeter && ovr.units_per_meter > 0.0f)
      out << "units_per_meter=" << ovr.units_per_meter << "\n";
    if (!ovr.flag_group.empty())
      out << "flag=" << ovr.flag_group << "\n";
    if (!ovr.condition_flag.empty())
    {
      out << "condition=" << ovr.condition_flag << "\n";
      out << "condition_mode=" << (ovr.condition_inverted ? "deactivate" : "activate") << "\n";
    }
    if (ovr.clear_efb)
    {
      out << "clear_efb=1\n";
      if (ovr.clear_efb_min_width > 0)
        out << "clear_efb_min=" << ovr.clear_efb_min_width << "\n";
      if (ovr.clear_efb_max_width > 0)
        out << "clear_efb_max=" << ovr.clear_efb_max_width << "\n";
    }
    if (ovr.element_start >= 0)
      out << "element_start=" << ovr.element_start << "\n";
    if (ovr.element_end >= 0)
      out << "element_end=" << ovr.element_end << "\n";
    if (ovr.element_reference_total > 0)
      out << "element_total=" << ovr.element_reference_total << "\n";
    if (ovr.hash_family_match)
    {
      out << "hash_family=1\n";
      if (ovr.family_signature != 0)
        out << "family_signature=" << fmt::format("{:016x}", ovr.family_signature) << "\n";
    }
    if (ovr.match_mode == MatchMode::RuntimeElement && ovr.runtime_element.valid)
    {
      const auto& sig = ovr.runtime_element;
      out << "element_projection=" << (sig.perspective ? "perspective" : "orthographic") << "\n";
      out << "element_use_projection=" << (sig.use_projection ? 1 : 0) << "\n";
      out << "element_use_layer=" << (sig.use_layer ? 1 : 0) << "\n";
      out << "element_use_viewport=" << (sig.use_viewport ? 1 : 0) << "\n";
      out << "element_use_scissor=" << (sig.use_scissor ? 1 : 0) << "\n";
      out << "element_use_render_state=" << (sig.use_render_state ? 1 : 0) << "\n";
      out << "element_perspective_hfov_x100=" << sig.perspective_hfov_x100 << "\n";
      out << "element_perspective_vfov_x100=" << sig.perspective_vfov_x100 << "\n";
      out << "element_perspective_near_x1000=" << sig.perspective_near_x1000 << "\n";
      out << "element_perspective_far_x100=" << sig.perspective_far_x100 << "\n";
      out << "element_ortho_left_x100=" << sig.ortho_left_x100 << "\n";
      out << "element_ortho_right_x100=" << sig.ortho_right_x100 << "\n";
      out << "element_ortho_top_x100=" << sig.ortho_top_x100 << "\n";
      out << "element_ortho_bottom_x100=" << sig.ortho_bottom_x100 << "\n";
      out << "element_ortho_near_x100=" << sig.ortho_near_x100 << "\n";
      out << "element_ortho_far_x100=" << sig.ortho_far_x100 << "\n";
      out << "element_layer=" << sig.ortho_layer << "\n";
      out << "element_viewport_x=" << sig.viewport_x << "\n";
      out << "element_viewport_y=" << sig.viewport_y << "\n";
      out << "element_viewport_width=" << sig.viewport_width << "\n";
      out << "element_viewport_height=" << sig.viewport_height << "\n";
      out << "element_scissor_left=" << sig.scissor_left << "\n";
      out << "element_scissor_top=" << sig.scissor_top << "\n";
      out << "element_scissor_right=" << sig.scissor_right << "\n";
      out << "element_scissor_bottom=" << sig.scissor_bottom << "\n";
      out << "element_alpha_test_hex=" << fmt::format("{:08x}", sig.alpha_test_hex) << "\n";
      out << "element_blend_color_update=" << (sig.blend_color_update ? 1 : 0) << "\n";
      out << "element_blend_alpha_update=" << (sig.blend_alpha_update ? 1 : 0) << "\n";
      out << "element_ztest=" << (sig.ztest ? 1 : 0) << "\n";
      out << "element_zupdate=" << (sig.zupdate ? 1 : 0) << "\n";
      out << "element_zfunc=" << sig.zfunc << "\n";
    }
    if (!ovr.texture_hashes.empty())
    {
      out << "texture_mode=" << (ovr.texture_hashes_excluded ? "exclude" : "include") << "\n";
      for (u64 texture_hash : ovr.texture_hashes)
        out << "texture=" << fmt::format("{:016x}", texture_hash) << "\n";
    }
    out << "\n";
  }

  std::ofstream outfile(path, std::ios::trunc);
  outfile << out.str();
}

// ============================================================================
// Runtime override management
// ============================================================================

void ShaderHunter::LoadOverrides(const std::string& game_id)
{
  std::lock_guard lock(m_mutex);

  m_overrides.clear();
  m_override_ps_hashes.clear();
  m_override_vs_hashes.clear();
  m_override_gs_hashes.clear();
  m_screen_hashes.clear();
  m_fullscreen_hashes.clear();
  m_fullscreen_mono_hashes.clear();
  m_headlocked_hashes.clear();
  m_units_per_meter_overrides.clear();
  m_clear_efb_hashes.clear();
  m_clear_efb_bounds.clear();
  m_clear_next_efb = false;
  m_screen_layers.clear();
  m_element_depths.clear();
  m_flag_rules.clear();
  m_conditional_overrides.clear();
  m_flags_seen_this_frame.clear();
  m_flag_age.clear();
  m_all_override_hashes.clear();
  m_has_element_overrides = false;
  m_has_texture_overrides = false;
  m_override_draw_counters.clear();
  m_override_draw_totals_prev.clear();
  m_runtime_element_reference_totals.clear();
  m_loaded_game_id = game_id;

  if (game_id.empty())
    return;

  auto all = LoadOverridesFromINI(game_id);

  for (auto& ovr : all)
  {
    if (!ovr.enabled)
      continue;

    m_all_override_hashes.insert(ovr.hash);
    // Any override with a flag_group sets a flag when drawn (regardless of handling type)
    if (!ovr.flag_group.empty())
      m_flag_rules.push_back({ovr.type, ovr.hash, ovr.hash_family_match, ovr.family_signature,
                              ovr.flag_group, ovr.texture_hashes, ovr.texture_hashes_excluded});

    // Flag-only overrides: just set the flag, render normally (no handling change)
    if (ovr.handling == HandlingType::Flag)
    {
      m_overrides.push_back(std::move(ovr));
      continue;
    }

    // Conditional, draw-call-range, or texture-conditioned overrides: store separately
    if (!ovr.condition_flag.empty() ||
        (ovr.element_start >= 0 && ovr.element_end >= 0) || !ovr.texture_hashes.empty() ||
        ovr.hash_family_match || ovr.match_mode == MatchMode::RuntimeElement)
    {
      if (ovr.element_start >= 0 && ovr.element_end >= 0)
        m_has_element_overrides = true;
      if (!ovr.texture_hashes.empty())
        m_has_texture_overrides = true;
      m_conditional_overrides.push_back(
          {ovr.hash, ovr.handling, ovr.type, ovr.match_mode, ovr.layer, ovr.element_depth,
           ovr.units_per_meter,
           ovr.element_start, ovr.element_end, ovr.element_reference_total, ovr.texture_hashes,
           ovr.texture_hashes_excluded, ovr.hash_family_match, ovr.family_signature,
           ovr.runtime_element,
           ovr.condition_flag, ovr.condition_inverted});
      m_overrides.push_back(std::move(ovr));
      continue;
    }

    // Unconditional overrides: insert into fast hash sets (existing behavior)
    switch (ovr.handling)
    {
    case HandlingType::Skip:
      switch (ovr.type)
      {
      case ShaderType::Pixel:
        m_override_ps_hashes.insert(ovr.hash);
        break;
      case ShaderType::Vertex:
        m_override_vs_hashes.insert(ovr.hash);
        break;
      case ShaderType::Geometry:
        m_override_gs_hashes.insert(ovr.hash);
        break;
      default:
        break;
      }
      break;
    case HandlingType::Screen:
      m_screen_hashes.insert(ovr.hash);
      if (ovr.layer >= 0)
        m_screen_layers[ovr.hash] = ovr.layer;
      if (ovr.element_depth >= 0.0f)
        m_element_depths[ovr.hash] = ovr.element_depth;
      break;
    case HandlingType::Fullscreen:
      m_fullscreen_hashes.insert(ovr.hash);
      break;
    case HandlingType::FullscreenMono:
      m_fullscreen_mono_hashes.insert(ovr.hash);
      break;
    case HandlingType::HeadLocked:
      m_headlocked_hashes.insert(ovr.hash);
      if (ovr.element_depth >= 0.0f)
        m_element_depths[ovr.hash] = ovr.element_depth;
      if (ovr.layer >= 0)
        m_screen_layers[ovr.hash] = ovr.layer;
      break;
    case HandlingType::UnitsPerMeter:
      if (ovr.units_per_meter > 0.0f)
        m_units_per_meter_overrides[ovr.hash] = ovr.units_per_meter;
      break;
    default:
      break;
    }

    // ClearEFB is an independent flag that can be set on any handling type.
    if (ovr.clear_efb)
    {
      m_clear_efb_hashes.insert(ovr.hash);
      if (ovr.clear_efb_min_width > 0 || ovr.clear_efb_max_width > 0)
        m_clear_efb_bounds[ovr.hash] = {ovr.clear_efb_min_width, ovr.clear_efb_max_width};
    }

    m_overrides.push_back(std::move(ovr));
  }

  if (!m_overrides.empty())
  {
    INFO_LOG_FMT(VIDEO, "ShaderHunter: Loaded {} enabled shader overrides for game {}",
                 m_overrides.size(), game_id);
  }
}

void ShaderHunter::LoadOverridesIfNeeded(const std::string& game_id)
{
  if (game_id == m_loaded_game_id)
    return;
  LoadOverrides(game_id);
}

void ShaderHunter::AddAndSaveOverride(const std::string& game_id, const std::string& name,
                                      ShaderType type, u64 hash, HandlingType handling)
{
  std::lock_guard lock(m_mutex);

  // Load all existing overrides from INI (including disabled ones)
  auto all = LoadOverridesFromINI(game_id);

  // Add the new override (enabled by default)
  ShaderOverride entry;
  entry.name = name;
  entry.hash = hash;
  entry.type = type;
  entry.handling = handling;
  entry.enabled = true;
  entry.user_defined = true;
  all.push_back(entry);

  // Save all overrides back to INI
  SaveOverridesToINI(game_id, all);

  // Also add to runtime handling sets so it takes effect immediately.
  m_overrides.push_back(entry);
  switch (handling)
  {
  case HandlingType::Skip:
    switch (type)
    {
    case ShaderType::Pixel:
      m_override_ps_hashes.insert(hash);
      break;
    case ShaderType::Vertex:
      m_override_vs_hashes.insert(hash);
      break;
    case ShaderType::Geometry:
      m_override_gs_hashes.insert(hash);
      break;
    default:
      break;
    }
    break;
  case HandlingType::Screen:
    m_screen_hashes.insert(hash);
    break;
  case HandlingType::Fullscreen:
    m_fullscreen_hashes.insert(hash);
    break;
  case HandlingType::FullscreenMono:
    m_fullscreen_mono_hashes.insert(hash);
    break;
  case HandlingType::HeadLocked:
    m_headlocked_hashes.insert(hash);
    break;
  case HandlingType::Flag:
    // Flag overrides are added via the UI with flag_group set on the ShaderOverride.
    // AddAndSaveOverride is used from ShaderHunterWidget quick-add which defaults to Skip.
    break;
  case HandlingType::UnitsPerMeter:
    // AddAndSaveOverride currently has no UPM parameter; this handling is authored through edit UIs.
    break;
  }

  INFO_LOG_FMT(VIDEO, "ShaderHunter: Saved override '{}' (hash={:016x}, type={}, handling={}) for game {}",
               name, hash,
               type == ShaderType::Pixel   ? "PS" :
               type == ShaderType::Vertex  ? "VS" :
                                             "GS",
               handling == HandlingType::Screen      ? "screen" :
               handling == HandlingType::Fullscreen   ? "fullscreen" :
               handling == HandlingType::FullscreenMono ? "fullscreen_mono" :
               handling == HandlingType::HeadLocked   ? "headlocked" :
               handling == HandlingType::Flag         ? "flag" :
               handling == HandlingType::UnitsPerMeter ? "units_per_meter" :
                                                         "skip",
               game_id);
}

bool ShaderHunter::HasOverrides() const
{
  return !m_overrides.empty();
}

void ShaderHunter::CheckClearEFBForDraw(u64 vs_hash, u64 ps_hash, u64 gs_hash)
{
  if (ShouldBypassSelectedOverrideForTextureTool(vs_hash, ps_hash, gs_hash))
    return;

  if (m_clear_efb_hashes.empty())
    return;

  for (u64 h : {vs_hash, ps_hash, gs_hash})
  {
    if (m_clear_efb_hashes.count(h) == 0)
      continue;

    m_clear_next_efb = true;

    auto it = m_clear_efb_bounds.find(h);
    if (it != m_clear_efb_bounds.end())
    {
      // Store this override's size bounds; if multiple match, widen to the union.
      if (m_pending_clear_min == 0 && m_pending_clear_max == 0)
      {
        m_pending_clear_min = it->second.first;
        m_pending_clear_max = it->second.second;
      }
      else
      {
        m_pending_clear_min = std::min(m_pending_clear_min, it->second.first);
        m_pending_clear_max = (m_pending_clear_max == 0 || it->second.second == 0) ?
                                  0 : std::max(m_pending_clear_max, it->second.second);
      }
    }
    else
    {
      // No bounds specified = any size — widen to unrestricted.
      m_pending_clear_min = 0;
      m_pending_clear_max = 0;
    }
    return;
  }
}

bool ShaderHunter::ShouldClearEFBCopy(int width)
{
  if (!m_clear_next_efb)
    return false;

  m_clear_next_efb = false;
  const int min_w = m_pending_clear_min;
  const int max_w = m_pending_clear_max;
  m_pending_clear_min = 0;
  m_pending_clear_max = 0;

  // Both 0 = no restriction, clear any size.
  if (min_w == 0 && max_w == 0)
    return true;

  if (min_w > 0 && width < min_w)
    return false;
  if (max_w > 0 && width > max_w)
    return false;

  return true;
}

void ShaderHunter::RegisterFlags(u64 vs_hash, u64 ps_hash, u64 gs_hash)
{
  if (m_flag_rules.empty())
    return;

  const auto matches_rule_shader = [&](const FlagRule& rule) {
    u64 current_hash = 0;
    u64 current_family = 0;
    switch (rule.type)
    {
    case ShaderType::Vertex:
      current_hash = vs_hash;
      current_family = m_current_vs_family;
      break;
    case ShaderType::Pixel:
      current_hash = ps_hash;
      current_family = m_current_ps_family;
      break;
    case ShaderType::Geometry:
      current_hash = gs_hash;
      current_family = m_current_gs_family;
      break;
    default:
      return false;
    }

    if (rule.hash_family_match)
    {
      u64 rule_family = rule.family_signature;
      if (rule_family == 0)
      {
        const auto& per_type = m_shader_family_signatures[static_cast<int>(rule.type)];
        const auto it = per_type.find(rule.hash);
        if (it != per_type.end())
          rule_family = it->second;
      }
      if (rule_family != 0 && current_family != 0)
        return rule_family == current_family;
    }
    return current_hash == rule.hash;
  };

  for (const auto& rule : m_flag_rules)
  {
    if (rule.flag_group.empty())
      continue;
    if (!matches_rule_shader(rule))
      continue;
    if (!DoesTextureFilterPass(m_current_draw_textures, rule.texture_hashes,
                               rule.texture_hashes_excluded))
    {
      continue;
    }
    m_flags_seen_this_frame.insert(rule.flag_group);
  }
}

void ShaderHunter::RegisterExternalFlag(const std::string& flag_name)
{
  if (flag_name.empty())
    return;

  std::lock_guard lock(m_mutex);
  m_flags_seen_this_frame.insert(flag_name);
}

bool ShaderHunter::IsFlagActive(const std::string& flag_name) const
{
  if (flag_name.empty())
    return false;

  std::lock_guard lock(m_mutex);
  return m_flag_age.find(flag_name) != m_flag_age.end() ||
         m_flags_seen_this_frame.find(flag_name) != m_flags_seen_this_frame.end();
}

std::vector<ShaderHunter::FlagStatus> ShaderHunter::GetFlagStatusesForOSD() const
{
  std::set<std::string> known_flags;
  for (const auto& rule : m_flag_rules)
  {
    if (!rule.flag_group.empty())
      known_flags.insert(rule.flag_group);
  }
  for (const auto& [flag_name, _] : m_flag_age)
    known_flags.insert(flag_name);
  for (const auto& ovr : m_overrides)
  {
    if (!ovr.condition_flag.empty())
      known_flags.insert(ovr.condition_flag);
  }

  std::vector<ShaderHunter::FlagStatus> result;
  result.reserve(known_flags.size());
  for (const auto& flag_name : known_flags)
  {
    FlagStatus status;
    status.flag_name = flag_name;
    status.is_active = m_flag_age.find(flag_name) != m_flag_age.end() ||
                       m_flags_seen_this_frame.find(flag_name) != m_flags_seen_this_frame.end();

    std::unordered_set<std::string> seen_names;
    for (const auto& ovr : m_overrides)
    {
      if (ovr.condition_flag != flag_name)
        continue;

      std::string shader_name = ovr.name;
      if (shader_name.empty())
      {
        shader_name = fmt::format("Unnamed {:08x}",
                                  static_cast<u32>(ovr.hash & 0xffffffffULL));
      }

      if (seen_names.insert(shader_name).second)
        status.impacted_shader_names.push_back(std::move(shader_name));
    }

    std::sort(status.impacted_shader_names.begin(), status.impacted_shader_names.end());
    result.push_back(std::move(status));
  }
  return result;
}

ShaderHunter::HuntingStatus ShaderHunter::GetHuntingStatusForOSD() const
{
  std::lock_guard lock(m_mutex);

  HuntingStatus status;
  status.enabled = m_enabled;
  status.option = m_hunting_option;
  status.active_type = m_active_type;

  const int t = static_cast<int>(m_active_type);
  status.selected_hash = m_selected_hash[t];
  status.selected_position = m_selected_pos[t];
  status.selected_total = static_cast<int>(m_display[t].size());

  status.element_mode = m_element_mode;
  status.selected_element = m_selected_element;
  status.element_total = m_element_total;
  status.element_start = m_element_start;
  status.element_end = m_element_end;

  status.texture_filters.reserve(m_texture_skip_filters.size());
  for (const u64 hash : m_texture_skip_filters)
    status.texture_filters.push_back(hash);
  std::sort(status.texture_filters.begin(), status.texture_filters.end());

  status.group_hunt_enabled = m_group_hunt_enabled;
  status.group_hunt_seed_valid = m_group_hunt_seed_signature.valid;
  status.group_hunt_seed_type = m_group_hunt_seed_type;
  status.group_hunt_seed_hash = m_group_hunt_seed_hash;
  status.group_hunt_selected_match = m_group_hunt_selected_match;
  status.group_hunt_total_matches = m_group_hunt_match_total;
  status.group_hunt_seed_signature = m_group_hunt_seed_signature;
  if (m_group_hunt_selected_draw.valid)
  {
    status.group_hunt_draw_index = m_group_hunt_selected_draw.draw_index;
    status.group_hunt_current_hash = m_group_hunt_selected_draw.hash;
  }

  return status;
}

void ShaderHunter::SetDebugLogging(bool enabled)
{
  m_debug_logging = enabled;
  if (enabled)
    INFO_LOG_FMT(VIDEO, "ShaderHunter: Debug override logging ENABLED");
  else
    INFO_LOG_FMT(VIDEO, "ShaderHunter: Debug override logging DISABLED");
}

bool ShaderHunter::IsDebugLogging() const
{
  return m_debug_logging;
}

void ShaderHunter::DebugLogUnmatched(u64 vs_hash, u64 ps_hash, u64 gs_hash) const
{
  if (!m_debug_logging || m_all_override_hashes.empty())
    return;

  // Only log if at least one of the draw call's hashes appears in any override
  if (m_all_override_hashes.count(vs_hash) == 0 &&
      m_all_override_hashes.count(ps_hash) == 0 &&
      m_all_override_hashes.count(gs_hash) == 0)
    return;

  const u64 combo = vs_hash ^ (ps_hash * 0x9e3779b97f4a7c15ULL) ^ 0x2ULL;
  if (m_debug_logged_combos.count(combo) != 0)
    return;
  m_debug_logged_combos.insert(combo);

  INFO_LOG_FMT(VIDEO,
      "ShaderHunter: NO MATCH (default render) VS={:08x} PS={:08x} GS={:08x}",
      static_cast<u32>(vs_hash), static_cast<u32>(ps_hash), static_cast<u32>(gs_hash));
}

void ShaderHunter::AdvanceOverrideDrawCounters(u64 vs_hash, u64 ps_hash, u64 gs_hash)
{
  if (!m_has_element_overrides)
    return;
  m_current_vs_draw_idx = m_override_draw_counters[vs_hash]++;
  m_current_ps_draw_idx = m_override_draw_counters[ps_hash]++;
  m_current_gs_draw_idx = m_override_draw_counters[gs_hash]++;
}

std::pair<int, int> ShaderHunter::ResolveElementRange(const ConditionalOverride& cond) const
{
  int start = cond.element_start;
  int end = cond.element_end;
  if (start < 0 || end < 0)
    return {start, end};

  const auto total_it = m_override_draw_totals_prev.find(cond.hash);
  if (total_it == m_override_draw_totals_prev.end() || total_it->second <= 0)
    return {start, end};

  const int previous_total = total_it->second;
  int reference_total = cond.element_reference_total;
  if (reference_total <= 0)
  {
    const auto inserted =
        m_runtime_element_reference_totals.emplace(cond.hash, previous_total);
    reference_total = inserted.first->second;
  }

  const int delta = previous_total - reference_total;
  start += delta;
  end += delta;

  if (start < 0)
    start = 0;
  if (end < start)
    end = start;

  if (previous_total > 0)
  {
    if (start >= previous_total)
      start = previous_total - 1;
    if (end >= previous_total)
      end = previous_total - 1;
    if (end < start)
      end = start;
  }

  return {start, end};
}

bool ShaderHunter::IsConditionFlagMatch(const ConditionalOverride& cond) const
{
  if (cond.condition_flag.empty())
    return true;

  // Consider both:
  // - active flags from the previous frame (m_flag_age)
  // - flags already seen in the current frame so far (m_flags_seen_this_frame)
  // This avoids one-frame latency for condition evaluation.
  const bool is_active = m_flag_age.count(cond.condition_flag) > 0 ||
                         m_flags_seen_this_frame.count(cond.condition_flag) > 0;
  return cond.condition_inverted ? !is_active : is_active;
}

u64 ShaderHunter::ResolveConditionalFamilySignature(const ConditionalOverride& cond) const
{
  if (!cond.hash_family_match)
    return 0;
  if (cond.family_signature != 0)
    return cond.family_signature;

  const auto& per_type = m_shader_family_signatures[static_cast<int>(cond.type)];
  const auto it = per_type.find(cond.hash);
  if (it == per_type.end())
    return 0;
  return it->second;
}

bool ShaderHunter::IsConditionalHashMatch(const ConditionalOverride& cond, u64 vs_hash, u64 ps_hash,
                                          u64 gs_hash) const
{
  u64 current_hash = 0;
  u64 current_family = 0;
  switch (cond.type)
  {
  case ShaderType::Vertex:
    current_hash = vs_hash;
    current_family = m_current_vs_family;
    break;
  case ShaderType::Pixel:
    current_hash = ps_hash;
    current_family = m_current_ps_family;
    break;
  case ShaderType::Geometry:
    current_hash = gs_hash;
    current_family = m_current_gs_family;
    break;
  default:
    return false;
  }

  const auto matches_anchor = [&](bool prefer_family) {
    if (prefer_family)
    {
      const u64 cond_family = ResolveConditionalFamilySignature(cond);
      if (cond_family != 0 && current_family != 0)
        return cond_family == current_family;
    }
    return current_hash == cond.hash;
  };

  switch (cond.match_mode)
  {
  case MatchMode::ShaderFamily:
    return matches_anchor(true);
  case MatchMode::RuntimeElement:
    return matches_anchor(cond.hash_family_match) && IsRuntimeElementMatch(cond.runtime_element);
  case MatchMode::ExactHash:
  default:
    return current_hash == cond.hash;
  }
}

bool ShaderHunter::IsRuntimeElementMatch(const RuntimeElementSignature& signature) const
{
  if (!signature.valid || !m_current_draw_signature.valid)
    return false;

  if (signature.use_projection)
  {
    if (signature.perspective != m_current_draw_signature.perspective)
      return false;

    if (signature.perspective)
    {
      if (signature.perspective_hfov_x100 != m_current_draw_signature.perspective_hfov_x100 ||
          signature.perspective_vfov_x100 != m_current_draw_signature.perspective_vfov_x100 ||
          signature.perspective_near_x1000 != m_current_draw_signature.perspective_near_x1000 ||
          signature.perspective_far_x100 != m_current_draw_signature.perspective_far_x100)
      {
        return false;
      }
    }
    else
    {
      if (signature.ortho_left_x100 != m_current_draw_signature.ortho_left_x100 ||
          signature.ortho_right_x100 != m_current_draw_signature.ortho_right_x100 ||
          signature.ortho_top_x100 != m_current_draw_signature.ortho_top_x100 ||
          signature.ortho_bottom_x100 != m_current_draw_signature.ortho_bottom_x100 ||
          signature.ortho_near_x100 != m_current_draw_signature.ortho_near_x100 ||
          signature.ortho_far_x100 != m_current_draw_signature.ortho_far_x100)
      {
        return false;
      }
    }
  }

  if (signature.use_layer && signature.ortho_layer != m_current_draw_signature.ortho_layer)
    return false;

  if (signature.use_viewport)
  {
    if (signature.viewport_x != m_current_draw_signature.viewport_x ||
        signature.viewport_y != m_current_draw_signature.viewport_y ||
        signature.viewport_width != m_current_draw_signature.viewport_width ||
        signature.viewport_height != m_current_draw_signature.viewport_height)
    {
      return false;
    }
  }

  if (signature.use_scissor)
  {
    if (signature.scissor_left != m_current_draw_signature.scissor_left ||
        signature.scissor_top != m_current_draw_signature.scissor_top ||
        signature.scissor_right != m_current_draw_signature.scissor_right ||
        signature.scissor_bottom != m_current_draw_signature.scissor_bottom)
    {
      return false;
    }
  }

  if (signature.use_render_state)
  {
    if (signature.alpha_test_hex != m_current_draw_signature.alpha_test_hex ||
        signature.blend_color_update != m_current_draw_signature.blend_color_update ||
        signature.blend_alpha_update != m_current_draw_signature.blend_alpha_update ||
        signature.ztest != m_current_draw_signature.ztest ||
        signature.zupdate != m_current_draw_signature.zupdate ||
        signature.zfunc != m_current_draw_signature.zfunc)
    {
      return false;
    }
  }

  return true;
}

bool ShaderHunter::ShouldBypassSelectedOverrideForTextureTool(u64 vs_hash, u64 ps_hash,
                                                              u64 gs_hash) const
{
  if (!m_enabled || !m_texture_tool_active.load(std::memory_order_relaxed))
    return false;

  const int active_type_index = static_cast<int>(m_active_type);
  if (active_type_index < 0 || active_type_index >= TYPE_COUNT)
    return false;
  if (m_selected_pos[active_type_index] < 0)
    return false;

  const u64 selected_hash = m_selected_hash[active_type_index];
  switch (m_active_type)
  {
  case ShaderType::Pixel:
    return ps_hash == selected_hash;
  case ShaderType::Vertex:
    return vs_hash == selected_hash;
  case ShaderType::Geometry:
    return gs_hash == selected_hash;
  default:
    return false;
  }
}

bool ShaderHunter::ShouldSkipByOverride(u64 vs_hash, u64 ps_hash, u64 gs_hash) const
{
  if (m_overrides.empty())
    return false;

  if (ShouldBypassSelectedOverrideForTextureTool(vs_hash, ps_hash, gs_hash))
    return false;

  // Unconditional skip check (existing O(1) hash sets)
  if (m_override_ps_hashes.count(ps_hash) > 0 || m_override_vs_hashes.count(vs_hash) > 0 ||
      m_override_gs_hashes.count(gs_hash) > 0)
  {
    if (m_debug_logging)
    {
      const u64 combo = vs_hash ^ (ps_hash * 0x9e3779b97f4a7c15ULL);
      if (m_debug_logged_combos.count(combo) == 0)
      {
        m_debug_logged_combos.insert(combo);
        INFO_LOG_FMT(VIDEO,
            "ShaderHunter: SKIP (unconditional) VS={:08x} PS={:08x} GS={:08x}",
            static_cast<u32>(vs_hash), static_cast<u32>(ps_hash), static_cast<u32>(gs_hash));
      }
    }
    return true;
  }

  // Conditional/draw-call-range/texture skip overrides
  for (const auto& cond : m_conditional_overrides)
  {
    if (cond.handling != HandlingType::Skip)
      continue;
    if (!IsConditionalHashMatch(cond, vs_hash, ps_hash, gs_hash))
      continue;
    if (!IsConditionFlagMatch(cond))
      continue;
    // Element range: check per-hash draw index
    if (cond.element_start >= 0 && cond.element_end >= 0)
    {
      const auto [range_start, range_end] = ResolveElementRange(cond);
      const int draw_idx = cond.type == ShaderType::Vertex   ? m_current_vs_draw_idx :
                           cond.type == ShaderType::Pixel    ? m_current_ps_draw_idx :
                                                                m_current_gs_draw_idx;
      if (draw_idx < range_start || draw_idx > range_end)
        continue;
    }
    // Texture condition: require at least one matching texture bound to any stage.
    if (!DoesTextureFilterPass(m_current_draw_textures, cond.texture_hashes,
                               cond.texture_hashes_excluded))
      continue;
    if (m_debug_logging)
    {
      const u64 combo = vs_hash ^ (ps_hash * 0x9e3779b97f4a7c15ULL);
      if (m_debug_logged_combos.count(combo) == 0)
      {
        m_debug_logged_combos.insert(combo);
        INFO_LOG_FMT(VIDEO,
            "ShaderHunter: SKIP (conditional) VS={:08x} PS={:08x} GS={:08x} "
            "matched={:08x} cond='{}{}'",
            static_cast<u32>(vs_hash), static_cast<u32>(ps_hash), static_cast<u32>(gs_hash),
            static_cast<u32>(cond.hash), cond.condition_inverted ? "!" : "",
            cond.condition_flag);
      }
    }
    return true;
  }
  return false;
}

ShaderHunter::HandlingType ShaderHunter::GetOverrideHandling(u64 vs_hash, u64 ps_hash,
                                                              u64 gs_hash) const
{
  if (ShouldBypassSelectedOverrideForTextureTool(vs_hash, ps_hash, gs_hash))
  {
    const bool has_fullscreen_mono_overrides =
        !m_fullscreen_mono_hashes.empty() ||
        std::any_of(m_conditional_overrides.begin(), m_conditional_overrides.end(),
                    [](const ConditionalOverride& cond) {
                      return cond.handling == HandlingType::FullscreenMono;
                    });
    if (m_debug_logging || has_fullscreen_mono_overrides)
    {
      const u64 combo = vs_hash ^ (ps_hash * 0x9e3779b97f4a7c15ULL) ^ 0xB0A55ULL;
      if (m_debug_logged_combos.count(combo) == 0)
      {
        m_debug_logged_combos.insert(combo);
        INFO_LOG_FMT(VIDEO,
                     "ShaderHunter: Override bypassed for texture tool VS={:08x} PS={:08x} GS={:08x}",
                     static_cast<u32>(vs_hash), static_cast<u32>(ps_hash),
                     static_cast<u32>(gs_hash));
      }
    }
    return HandlingType::Skip;
  }

  const char* handling_name = nullptr;
  HandlingType result = HandlingType::Skip;
  const auto log_conditional_reject = [&](const ConditionalOverride& cond, u64 reason_tag,
                                          const std::string& reason) {
    if (!m_debug_logging && cond.handling != HandlingType::FullscreenMono)
      return;
    const u64 combo = vs_hash ^ (ps_hash * 0x9e3779b97f4a7c15ULL) ^ (cond.hash << 1) ^ reason_tag;
    if (m_debug_logged_combos.count(combo) != 0)
      return;
    m_debug_logged_combos.insert(combo);
    INFO_LOG_FMT(
        VIDEO,
        "ShaderHunter: Conditional reject (handling={}) VS={:08x} PS={:08x} GS={:08x} "
        "target={:08x} reason={}",
        cond.handling == HandlingType::Screen      ? "Screen" :
        cond.handling == HandlingType::Fullscreen  ? "Fullscreen" :
        cond.handling == HandlingType::FullscreenMono ? "FullscreenMono" :
        cond.handling == HandlingType::HeadLocked  ? "HeadLocked" :
        cond.handling == HandlingType::UnitsPerMeter ? "UnitsPerMeter" :
                                                       "Skip",
        static_cast<u32>(vs_hash), static_cast<u32>(ps_hash), static_cast<u32>(gs_hash),
        static_cast<u32>(cond.hash), reason);
  };

  // Unconditional checks (existing O(1) hash sets)
  if (m_screen_hashes.count(vs_hash) > 0 || m_screen_hashes.count(ps_hash) > 0 ||
      m_screen_hashes.count(gs_hash) > 0)
  {
    result = HandlingType::Screen;
    handling_name = "Screen";
  }
  else if (m_fullscreen_hashes.count(vs_hash) > 0 || m_fullscreen_hashes.count(ps_hash) > 0 ||
           m_fullscreen_hashes.count(gs_hash) > 0)
  {
    result = HandlingType::Fullscreen;
    handling_name = "Fullscreen";
  }
  else if (m_fullscreen_mono_hashes.count(vs_hash) > 0 ||
           m_fullscreen_mono_hashes.count(ps_hash) > 0 ||
           m_fullscreen_mono_hashes.count(gs_hash) > 0)
  {
    result = HandlingType::FullscreenMono;
    handling_name = "FullscreenMono";
  }
  else if (m_headlocked_hashes.count(vs_hash) > 0 || m_headlocked_hashes.count(ps_hash) > 0 ||
           m_headlocked_hashes.count(gs_hash) > 0)
  {
    result = HandlingType::HeadLocked;
    handling_name = "HeadLocked";
  }
  else if (m_units_per_meter_overrides.count(vs_hash) > 0 ||
           m_units_per_meter_overrides.count(ps_hash) > 0 ||
           m_units_per_meter_overrides.count(gs_hash) > 0)
  {
    result = HandlingType::UnitsPerMeter;
    handling_name = "UnitsPerMeter";
  }
  else
  {
    // Conditional/draw-call-range/texture overrides (small vector scan)
    for (const auto& cond : m_conditional_overrides)
    {
      if (cond.handling == HandlingType::Skip)
        continue;
      if (!IsConditionalHashMatch(cond, vs_hash, ps_hash, gs_hash))
        continue;
      if (!IsConditionFlagMatch(cond))
      {
        std::string reason = "flag condition failed";
        if (!cond.condition_flag.empty())
        {
          reason = "flag '" + cond.condition_flag + "'";
          if (cond.condition_inverted)
            reason += " (inverted)";
          reason += " not satisfied";
        }
        log_conditional_reject(cond, 0xF1ULL, reason);
        continue;
      }
      // Element range: check per-hash draw index
      if (cond.element_start >= 0 && cond.element_end >= 0)
      {
        const auto [range_start, range_end] = ResolveElementRange(cond);
        const int draw_idx = cond.type == ShaderType::Vertex   ? m_current_vs_draw_idx :
                            cond.type == ShaderType::Pixel    ? m_current_ps_draw_idx :
                                                                 m_current_gs_draw_idx;
        if (draw_idx < range_start || draw_idx > range_end)
        {
          log_conditional_reject(cond, 0xE1ULL,
                                 "draw_idx=" + std::to_string(draw_idx) + " outside [" +
                                     std::to_string(range_start) + ", " +
                                     std::to_string(range_end) + "]");
          continue;
        }
      }
      // Texture condition: require at least one matching texture bound to any stage.
      if (!DoesTextureFilterPass(m_current_draw_textures, cond.texture_hashes,
                                 cond.texture_hashes_excluded))
      {
        log_conditional_reject(
            cond, 0xA1ULL,
            std::string(cond.texture_hashes_excluded ? "exclude" : "include") +
                " texture filter mismatch (" + std::to_string(cond.texture_hashes.size()) +
                " entries)");
        continue;
      }
      result = cond.handling;
      handling_name = cond.handling == HandlingType::Screen      ? "Screen(conditional)" :
                      cond.handling == HandlingType::Fullscreen  ? "Fullscreen(conditional)" :
                      cond.handling == HandlingType::FullscreenMono ?
                          "FullscreenMono(conditional)" :
                      cond.handling == HandlingType::HeadLocked  ? "HeadLocked(conditional)" :
                      cond.handling == HandlingType::UnitsPerMeter ?
                          "UnitsPerMeter(conditional)" :
                                                                   "???";
      break;
    }
  }

  if (m_debug_logging && handling_name != nullptr)
  {
    const u64 combo = vs_hash ^ (ps_hash * 0x9e3779b97f4a7c15ULL) ^ 0x1ULL;
    if (m_debug_logged_combos.count(combo) == 0)
    {
      m_debug_logged_combos.insert(combo);
      INFO_LOG_FMT(VIDEO,
          "ShaderHunter: {} VS={:08x} PS={:08x} GS={:08x}",
          handling_name,
          static_cast<u32>(vs_hash), static_cast<u32>(ps_hash), static_cast<u32>(gs_hash));
    }
  }

  return result;
}

int ShaderHunter::GetOverrideLayer(u64 vs_hash, u64 ps_hash, u64 gs_hash) const
{
  if (ShouldBypassSelectedOverrideForTextureTool(vs_hash, ps_hash, gs_hash))
    return -1;

  // Unconditional layers
  for (u64 h : {vs_hash, ps_hash, gs_hash})
  {
    auto it = m_screen_layers.find(h);
    if (it != m_screen_layers.end())
      return it->second;
  }

  // Conditional layers
  for (const auto& cond : m_conditional_overrides)
  {
    if (cond.layer < 0)
      continue;
    if (!IsConditionalHashMatch(cond, vs_hash, ps_hash, gs_hash))
      continue;
    if (!IsConditionFlagMatch(cond))
      continue;
    if (cond.element_start >= 0 && cond.element_end >= 0)
    {
      const auto [range_start, range_end] = ResolveElementRange(cond);
      const int draw_idx = cond.type == ShaderType::Vertex   ? m_current_vs_draw_idx :
                           cond.type == ShaderType::Pixel    ? m_current_ps_draw_idx :
                                                                m_current_gs_draw_idx;
      if (draw_idx < range_start || draw_idx > range_end)
        continue;
    }
    if (!DoesTextureFilterPass(m_current_draw_textures, cond.texture_hashes,
                               cond.texture_hashes_excluded))
      continue;
    return cond.layer;
  }
  return -1;  // auto
}

float ShaderHunter::GetOverrideElementDepth(u64 vs_hash, u64 ps_hash, u64 gs_hash) const
{
  if (ShouldBypassSelectedOverrideForTextureTool(vs_hash, ps_hash, gs_hash))
    return -1.0f;

  // Unconditional element depths
  for (u64 h : {vs_hash, ps_hash, gs_hash})
  {
    auto it = m_element_depths.find(h);
    if (it != m_element_depths.end())
      return it->second;
  }

  // Conditional element depths
  for (const auto& cond : m_conditional_overrides)
  {
    if (cond.element_depth < 0.0f)
      continue;
    if (!IsConditionalHashMatch(cond, vs_hash, ps_hash, gs_hash))
      continue;
    if (!IsConditionFlagMatch(cond))
      continue;
    if (cond.element_start >= 0 && cond.element_end >= 0)
    {
      const auto [range_start, range_end] = ResolveElementRange(cond);
      const int draw_idx = cond.type == ShaderType::Vertex   ? m_current_vs_draw_idx :
                           cond.type == ShaderType::Pixel    ? m_current_ps_draw_idx :
                                                                m_current_gs_draw_idx;
      if (draw_idx < range_start || draw_idx > range_end)
        continue;
    }
    if (!DoesTextureFilterPass(m_current_draw_textures, cond.texture_hashes,
                               cond.texture_hashes_excluded))
      continue;
    return cond.element_depth;
  }
  return -1.0f;  // use global
}

float ShaderHunter::GetOverrideUnitsPerMeter(u64 vs_hash, u64 ps_hash, u64 gs_hash) const
{
  if (ShouldBypassSelectedOverrideForTextureTool(vs_hash, ps_hash, gs_hash))
    return -1.0f;

  // Unconditional per-hash UPM overrides
  for (u64 h : {vs_hash, ps_hash, gs_hash})
  {
    auto it = m_units_per_meter_overrides.find(h);
    if (it != m_units_per_meter_overrides.end() && it->second > 0.0f)
      return it->second;
  }

  // Conditional per-hash UPM overrides
  for (const auto& cond : m_conditional_overrides)
  {
    if (cond.handling != HandlingType::UnitsPerMeter || cond.units_per_meter <= 0.0f)
      continue;
    if (!IsConditionalHashMatch(cond, vs_hash, ps_hash, gs_hash))
      continue;
    if (!IsConditionFlagMatch(cond))
      continue;
    if (cond.element_start >= 0 && cond.element_end >= 0)
    {
      const auto [range_start, range_end] = ResolveElementRange(cond);
      const int draw_idx = cond.type == ShaderType::Vertex   ? m_current_vs_draw_idx :
                           cond.type == ShaderType::Pixel    ? m_current_ps_draw_idx :
                                                                m_current_gs_draw_idx;
      if (draw_idx < range_start || draw_idx > range_end)
        continue;
    }
    if (!DoesTextureFilterPass(m_current_draw_textures, cond.texture_hashes,
                               cond.texture_hashes_excluded))
      continue;
    return cond.units_per_meter;
  }

  return -1.0f;  // use global
}

// ============================================================================
// Shader dumping — regenerate source from cached UID and write to file
// ============================================================================

bool ShaderHunter::DumpShader(const std::string& game_id, ShaderType type, u64 hash) const
{
  std::lock_guard lock(m_mutex);

  const int t = static_cast<int>(type);
  auto it = m_uid_cache[t].find(hash);
  if (it == m_uid_cache[t].end())
  {
    WARN_LOG_FMT(VIDEO, "ShaderHunter: No cached UID for hash {:08x}, cannot dump", hash);
    return false;
  }

  const auto& uid_bytes = it->second;
  const APIType api_type = g_backend_info.api_type;
  const ShaderHostConfig host_config = ShaderHostConfig::GetCurrent();

  std::string source;
  const char* type_suffix = "";

  switch (type)
  {
  case ShaderType::Vertex:
  {
    if (uid_bytes.size() < sizeof(vertex_shader_uid_data))
    {
      WARN_LOG_FMT(VIDEO, "ShaderHunter: UID data too small for VS (got {}, need {})",
                   uid_bytes.size(), sizeof(vertex_shader_uid_data));
      return false;
    }
    const auto* uid_data = reinterpret_cast<const vertex_shader_uid_data*>(uid_bytes.data());
    ShaderCode code = GenerateVertexShaderCode(api_type, host_config, uid_data, {});
    source = code.GetBuffer();
    type_suffix = "vs";
    break;
  }
  case ShaderType::Pixel:
  {
    if (uid_bytes.size() < sizeof(pixel_shader_uid_data))
    {
      WARN_LOG_FMT(VIDEO, "ShaderHunter: UID data too small for PS (got {}, need {})",
                   uid_bytes.size(), sizeof(pixel_shader_uid_data));
      return false;
    }
    const auto* uid_data = reinterpret_cast<const pixel_shader_uid_data*>(uid_bytes.data());
    ShaderCode code = GeneratePixelShaderCode(api_type, host_config, uid_data, {});
    source = code.GetBuffer();
    type_suffix = "ps";
    break;
  }
  case ShaderType::Geometry:
  {
    if (uid_bytes.size() < sizeof(geometry_shader_uid_data))
    {
      WARN_LOG_FMT(VIDEO, "ShaderHunter: UID data too small for GS (got {}, need {})",
                   uid_bytes.size(), sizeof(geometry_shader_uid_data));
      return false;
    }
    const auto* uid_data = reinterpret_cast<const geometry_shader_uid_data*>(uid_bytes.data());
    ShaderCode code = GenerateGeometryShaderCode(api_type, host_config, uid_data);
    source = code.GetBuffer();
    type_suffix = "gs";
    break;
  }
  default:
    return false;
  }

  // Build dump path: Dump/Shaders/<game_id>/
  const std::string dump_dir =
      File::GetUserPath(D_DUMP_IDX) + "Shaders/" + game_id + "/";
  if (!File::IsDirectory(dump_dir))
    File::CreateFullPath(dump_dir);

  const std::string filename =
      dump_dir + fmt::format("{:08x}-{}.txt", static_cast<u32>(hash), type_suffix);

  std::ofstream outfile(filename, std::ios::trunc);
  if (!outfile.is_open())
  {
    WARN_LOG_FMT(VIDEO, "ShaderHunter: Failed to open '{}' for writing", filename);
    return false;
  }

  outfile << source;
  outfile.close();

  INFO_LOG_FMT(VIDEO, "ShaderHunter: Dumped {} shader {:08x} to '{}'",
               type_suffix, static_cast<u32>(hash), filename);
  return true;
}

std::vector<ShaderHunter::TextureUsage> ShaderHunter::GetTexturesForSelectedShader() const
{
  std::lock_guard lock(m_mutex);
  std::vector<TextureUsage> result;
  result.reserve(m_texture_usage_display.size());
  for (const auto& [texture_hash, texture_name] : m_texture_usage_display)
    result.push_back({texture_hash, texture_name});
  std::sort(result.begin(), result.end(),
            [](const TextureUsage& a, const TextureUsage& b) { return a.hash < b.hash; });
  return result;
}

std::vector<ShaderHunter::TextureUsage> ShaderHunter::GetTexturesForHash(ShaderType type,
                                                                          u64 hash) const
{
  std::lock_guard lock(m_mutex);
  std::vector<TextureUsage> result;

  const auto& per_type = m_shader_texture_usage_display[static_cast<int>(type)];
  auto it = per_type.find(hash);
  if (it == per_type.end())
    return result;

  result.reserve(it->second.size());
  for (const auto& [texture_hash, texture_name] : it->second)
    result.push_back({texture_hash, texture_name});

  std::sort(result.begin(), result.end(),
            [](const TextureUsage& a, const TextureUsage& b) { return a.hash < b.hash; });
  return result;
}

void ShaderHunter::SetTextureSkipEnabled(u64 texture_hash, bool enabled)
{
  if (texture_hash == 0)
    return;
  std::lock_guard lock(m_mutex);
  m_texture_skip_mode_active = true;
  if (enabled)
    m_texture_skip_filters.insert(texture_hash);
  else
    m_texture_skip_filters.erase(texture_hash);
}

bool ShaderHunter::IsTextureSkipEnabled(u64 texture_hash) const
{
  std::lock_guard lock(m_mutex);
  return m_texture_skip_filters.count(texture_hash) > 0;
}

void ShaderHunter::ClearTextureSkipFilters()
{
  std::lock_guard lock(m_mutex);
  m_texture_skip_filters.clear();
  m_texture_skip_mode_active = true;
}

void ShaderHunter::NextTextureHash()
{
  std::lock_guard lock(m_mutex);
  const auto hashes = CollectSortedTextureHashes(m_texture_usage_display);
  if (hashes.empty())
  {
    m_selected_texture_hash = 0;
    m_selected_texture_pos = -1;
    return;
  }

  const auto it = std::find(hashes.begin(), hashes.end(), m_selected_texture_hash);
  if (it == hashes.end())
  {
    m_selected_texture_pos = 0;
    m_selected_texture_hash = hashes[0];
    return;
  }

  const std::size_t next_index =
      (static_cast<std::size_t>(std::distance(hashes.begin(), it)) + 1) % hashes.size();
  m_selected_texture_pos = static_cast<int>(next_index);
  m_selected_texture_hash = hashes[next_index];
}

void ShaderHunter::PrevTextureHash()
{
  std::lock_guard lock(m_mutex);
  const auto hashes = CollectSortedTextureHashes(m_texture_usage_display);
  if (hashes.empty())
  {
    m_selected_texture_hash = 0;
    m_selected_texture_pos = -1;
    return;
  }

  const auto it = std::find(hashes.begin(), hashes.end(), m_selected_texture_hash);
  if (it == hashes.end())
  {
    m_selected_texture_pos = static_cast<int>(hashes.size()) - 1;
    m_selected_texture_hash = hashes.back();
    return;
  }

  const std::size_t current_index = static_cast<std::size_t>(std::distance(hashes.begin(), it));
  const std::size_t prev_index = current_index == 0 ? hashes.size() - 1 : current_index - 1;
  m_selected_texture_pos = static_cast<int>(prev_index);
  m_selected_texture_hash = hashes[prev_index];
}

u64 ShaderHunter::GetSelectedTextureHash() const
{
  std::lock_guard lock(m_mutex);
  return m_selected_texture_hash;
}

bool ShaderHunter::ToggleSelectedTextureHashFilter()
{
  std::lock_guard lock(m_mutex);

  if (m_selected_texture_hash == 0 || m_texture_usage_display.count(m_selected_texture_hash) == 0)
  {
    const auto hashes = CollectSortedTextureHashes(m_texture_usage_display);
    if (hashes.empty())
    {
      m_selected_texture_hash = 0;
      m_selected_texture_pos = -1;
      return false;
    }
    m_selected_texture_pos = 0;
    m_selected_texture_hash = hashes[0];
  }

  m_texture_skip_mode_active = true;
  const auto [it, inserted] = m_texture_skip_filters.insert(m_selected_texture_hash);
  if (!inserted)
  {
    m_texture_skip_filters.erase(it);
    return false;
  }
  return true;
}

bool ShaderHunter::SaveSelectedShaderOverride(const std::string& game_id, HandlingType handling)
{
  if (game_id.empty())
    return false;

  ShaderOverride entry;
  u64 captured_family_signature = 0;
  {
    std::lock_guard lock(m_mutex);
    if (HasActiveGroupHuntLocked())
    {
      if (!m_group_hunt_selected_draw.valid)
        return false;
      entry.hash = m_group_hunt_selected_draw.hash;
      entry.type = m_group_hunt_selected_draw.type;
      entry.match_mode = MatchMode::RuntimeElement;
      entry.runtime_element = m_group_hunt_selected_draw.signature;
      if (entry.type == ShaderType::Pixel)
        captured_family_signature = m_current_ps_family;
    }
    else
    {
      const int type_index = static_cast<int>(m_active_type);
      if (m_selected_pos[type_index] < 0 || m_selected_hash[type_index] == ~0ULL)
        return false;

      entry.hash = m_selected_hash[type_index];
      entry.type = m_active_type;
    }
    entry.handling = handling;
    entry.enabled = true;
    entry.user_defined = true;
    entry.name = fmt::format("Hotkey {} {:08x}",
                             entry.type == ShaderType::Pixel   ? "PS" :
                             entry.type == ShaderType::Vertex  ? "VS" :
                                                                 "GS",
                             static_cast<u32>(entry.hash));

    if (m_element_mode && m_element_start >= 0 && m_element_end >= 0)
    {
      entry.element_start = m_element_start;
      entry.element_end = m_element_end;
      entry.element_reference_total = m_element_total;
    }

    entry.texture_hashes.assign(m_texture_skip_filters.begin(), m_texture_skip_filters.end());
    std::sort(entry.texture_hashes.begin(), entry.texture_hashes.end());
    entry.texture_hashes.erase(std::unique(entry.texture_hashes.begin(), entry.texture_hashes.end()),
                               entry.texture_hashes.end());
    entry.texture_hashes_excluded = false;

    if (entry.handling == HandlingType::Flag)
      entry.flag_group = entry.name;

    if (HasActiveGroupHuntLocked())
    {
      // Already seeded from the selected group-hunt draw above.
    }
    else if (m_selected_draw_signature.valid && m_selected_draw_signature_type == entry.type &&
             m_selected_draw_signature_hash == entry.hash)
    {
      entry.match_mode = MatchMode::RuntimeElement;
      entry.runtime_element = m_selected_draw_signature;
      if (entry.type == ShaderType::Pixel)
        captured_family_signature = m_current_ps_family;
    }
  }

  if (captured_family_signature != 0)
  {
    entry.hash_family_match = true;
    entry.family_signature = captured_family_signature;
  }

  auto all = LoadOverridesFromINI(game_id);
  auto name_is_used = [&](const std::string& name) {
    return std::any_of(all.begin(), all.end(), [&](const ShaderOverride& ovr) { return ovr.name == name; });
  };
  if (name_is_used(entry.name))
  {
    const std::string base_name = entry.name;
    int suffix = 2;
    do
    {
      entry.name = fmt::format("{} ({})", base_name, suffix++);
    } while (name_is_used(entry.name));
  }

  all.push_back(entry);
  SaveOverridesToINI(game_id, all);
  LoadOverrides(game_id);

  INFO_LOG_FMT(VIDEO,
               "ShaderHunter: Hotkey-saved override '{}' (hash={:016x}, type={}, handling={}, textures={}) for game {}",
               entry.name, entry.hash,
               entry.type == ShaderType::Pixel   ? "PS" :
               entry.type == ShaderType::Vertex  ? "VS" :
                                                   "GS",
               entry.handling == HandlingType::Screen      ? "screen" :
               entry.handling == HandlingType::Fullscreen  ? "fullscreen" :
               entry.handling == HandlingType::FullscreenMono ? "fullscreen_mono" :
               entry.handling == HandlingType::HeadLocked  ? "headlocked" :
               entry.handling == HandlingType::Flag        ? "flag" :
               entry.handling == HandlingType::UnitsPerMeter ? "units_per_meter" :
                                                                "skip",
               entry.texture_hashes.size(), game_id);
  return true;
}

void ShaderHunter::SetTextureToolActive(bool active)
{
  m_texture_tool_active.store(active, std::memory_order_relaxed);
}
