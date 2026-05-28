// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <array>
#include <cstdint>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"

// 3DMigoto-lite shader hunting: cycle through shaders used per frame and skip selected ones.
// Thread-safe: video thread calls Register/ShouldSkip/OnFrameEnd, UI thread calls Next/Prev/Get*.
class ShaderHunter
{
public:
  enum class ShaderType
  {
    Pixel = 0,
    Vertex = 1,
    Geometry = 2,
    Count = 3
  };

  enum class HandlingType
  {
    Skip = 0,
    Screen = 1,
    Fullscreen = 2,
    HeadLocked = 3,
    Flag = 4,
    UnitsPerMeter = 5,
    FullscreenMono = 6
  };

  enum class HuntingOption
  {
    Skip = 0,
    Pink = 1
  };

  enum class MatchMode
  {
    ExactHash = 0,
    ShaderFamily = 1,
    RuntimeElement = 2
  };

  struct RuntimeElementSignature
  {
    bool valid = false;
    bool perspective = false;

    bool use_projection = true;
    bool use_layer = false;
    bool use_viewport = true;
    bool use_scissor = false;
    bool use_render_state = true;

    int perspective_hfov_x100 = 0;
    int perspective_vfov_x100 = 0;
    int perspective_near_x1000 = 0;
    int perspective_far_x100 = 0;

    int ortho_left_x100 = 0;
    int ortho_right_x100 = 0;
    int ortho_top_x100 = 0;
    int ortho_bottom_x100 = 0;
    int ortho_near_x100 = 0;
    int ortho_far_x100 = 0;

    int ortho_layer = -1;

    int viewport_x = 0;
    int viewport_y = 0;
    int viewport_width = 0;
    int viewport_height = 0;

    int scissor_left = 0;
    int scissor_top = 0;
    int scissor_right = 0;
    int scissor_bottom = 0;

    u32 alpha_test_hex = 0;
    bool blend_color_update = false;
    bool blend_alpha_update = false;
    bool ztest = false;
    bool zupdate = false;
    int zfunc = 0;
  };

  struct HuntedDraw
  {
    bool valid = false;
    ShaderType type = ShaderType::Pixel;
    u64 hash = 0;
    RuntimeElementSignature signature;
    std::array<u64, 8> textures{};
    std::array<std::string, 8> texture_names{};
    int draw_index = -1;
    int match_index = -1;
  };

  static ShaderHunter& GetInstance();

  // --- Video thread ---
  u64 RegisterShader(ShaderType type, u64 hash, const u8* uid_data, size_t uid_size);
  void RegisterDrawCombination(u64 vs_hash, u64 ps_hash, u64 gs_hash);
  void OnFrameEnd();
  bool ShouldSkipDraw(u64 vs_hash, u64 ps_hash, u64 gs_hash);

  // Query and track relaxed per-shader "family" signatures used to match dynamic variants.
  std::optional<u64> GetShaderFamilySignature(ShaderType type, u64 hash) const;
  void SetCurrentDrawShaderFamilies(u64 vs_family, u64 ps_family, u64 gs_family);

  // --- UI thread ---
  void SetEnabled(bool enabled);
  bool IsEnabled() const;
  void SetHuntingOption(HuntingOption option);
  HuntingOption GetHuntingOption() const;
  bool ShouldHighlightSelectedDraw() const;

  void SetActiveType(ShaderType type);
  ShaderType GetActiveType() const;

  void NextShader();
  void PrevShader();
  bool SelectShader(ShaderType type, u64 hash);

  u64 GetSelectedHash() const;
  int GetSelectedPosition() const;
  int GetTotalCount() const;

  // Element mode: cycle through individual draw calls of the selected shader
  void SetElementMode(bool enabled);
  bool IsElementMode() const;
  void NextElement();
  void PrevElement();
  int GetSelectedElement() const;
  int GetElementTotal() const;

  // Element range: skip a contiguous range of draw calls instead of just one
  void MarkRangeStart();   // Set range start to current cursor position
  void MarkRangeEnd();     // Set range end to current cursor position
  void ClearRange();       // Reset to single-element mode
  int GetElementStart() const;
  int GetElementEnd() const;

  // Texture tracking: capture bound textures per draw call for element identification
  void SetCurrentDrawTextures(const std::array<u64, 8>& hashes,
                              const std::array<std::string, 8>& names);
  void SetCurrentDrawSignature(const RuntimeElementSignature& signature);
  std::array<u64, 8> GetElementTextures(int element) const;
  std::array<std::string, 8> GetElementTextureNames(int element) const;
  std::optional<RuntimeElementSignature> GetSelectedRuntimeElementSignature() const;
  void SetGroupHuntEnabled(bool enabled);
  bool IsGroupHuntEnabled() const;
  bool SeedGroupHuntFromCurrentDraw();
  void SetGroupHuntGroupMask(bool use_projection, bool use_layer, bool use_viewport,
                             bool use_scissor, bool use_render_state);
  RuntimeElementSignature GetGroupHuntSeedSignature() const;
  std::optional<HuntedDraw> GetGroupHuntHighlightedDraw() const;

  // --- Persistent overrides (per-game INI) ---
  struct ShaderOverride
  {
    std::string name;
    std::string comments;
    std::string credits;
    u64 hash = 0;
    ShaderType type = ShaderType::Pixel;
    MatchMode match_mode = MatchMode::ExactHash;
    HandlingType handling = HandlingType::Skip;
    int layer = -1;          // Manual layer index for Screen handling (-1 = auto)
    float element_depth = -1.0f;  // Per-override within-element depth (-1 = use global)
    float units_per_meter = -1.0f;  // Per-override UPM for UnitsPerMeter handling (-1 = global)
    bool clear_efb = false;      // Clear the next EFB copy to transparent when this shader draws
    int clear_efb_min_width = 0; // Min native width for EFB clear (0 = no lower bound)
    int clear_efb_max_width = 0; // Max native width for EFB clear (0 = no upper bound)
    int element_start = -1;      // First draw call index to apply override (-1 = all)
    int element_end = -1;        // Last draw call index (-1 = all)
    int element_reference_total = 0;  // Draw-call count used when range was authored (0 = fixed)
    std::vector<u64> texture_hashes;  // Only apply when any listed texture is bound (empty = any)
    bool texture_hashes_excluded = false;  // false=Include list, true=Exclude list
    bool hash_family_match = false;  // false=exact hash, true=family signature
    u64 family_signature = 0;        // 0=resolve from current hash when available
    RuntimeElementSignature runtime_element;
    bool enabled = true;
    bool user_defined = true;
    std::string flag_group;      // For Flag handling: the flag name this shader sets
    std::string condition_flag;  // For other types: condition flag name
    bool condition_inverted = false;  // false=active, true=inactive
  };

  // Static INI helpers (used by both ShaderHunter and ShaderOverrideWidget)
  static std::vector<ShaderOverride> LoadOverridesFromINI(const std::string& game_id);
  static void SaveOverridesToINI(const std::string& game_id,
                                 const std::vector<ShaderOverride>& overrides);

  // Runtime override management
  void LoadOverrides(const std::string& game_id);
  void LoadOverridesIfNeeded(const std::string& game_id);
  void AddAndSaveOverride(const std::string& game_id, const std::string& name, ShaderType type,
                          u64 hash, HandlingType handling);
  bool HasOverrides() const;

  // Advance per-hash draw counters for element-aware overrides.
  // Must be called once per draw from VertexManagerBase, before any override checks.
  void AdvanceOverrideDrawCounters(u64 vs_hash, u64 ps_hash, u64 gs_hash);

  // Always-active skip check (independent of hunting)
  bool ShouldSkipByOverride(u64 vs_hash, u64 ps_hash, u64 gs_hash) const;

  // Returns the handling type for the first matching override
  // (Screen/Fullscreen/FullscreenMono/HeadLocked/UnitsPerMeter),
  // or Skip if no non-skip override matches.
  HandlingType GetOverrideHandling(u64 vs_hash, u64 ps_hash, u64 gs_hash) const;

  // Returns the manual layer for a Screen override (-1 = auto).
  int GetOverrideLayer(u64 vs_hash, u64 ps_hash, u64 gs_hash) const;

  // Returns the per-override element depth (-1 = use global setting).
  float GetOverrideElementDepth(u64 vs_hash, u64 ps_hash, u64 gs_hash) const;

  // Returns the per-override units per meter (>0 = override, -1 = use global setting).
  float GetOverrideUnitsPerMeter(u64 vs_hash, u64 ps_hash, u64 gs_hash) const;

  // ClearEFB: check if any hash has clear_efb set, and mark pending clear with size bounds.
  // Called from VertexManagerBase::Flush when a shader with clear_efb=true is drawn.
  void CheckClearEFBForDraw(u64 vs_hash, u64 ps_hash, u64 gs_hash);
  // Called from TextureCacheBase: returns true (and resets the flag) if EFB copy should be cleared.
  // Checks the copy's native width against stored per-override size bounds.
  bool ShouldClearEFBCopy(int width);

  // Flag system: register flag shaders and check conditions.
  void RegisterFlags(u64 vs_hash, u64 ps_hash, u64 gs_hash);
  void RegisterExternalFlag(const std::string& flag_name);
  bool IsFlagActive(const std::string& flag_name) const;
  // Video-thread snapshot for OSD display (name + active state).
  struct FlagStatus
  {
    std::string flag_name;
    bool is_active = false;
    std::vector<std::string> impacted_shader_names;
  };
  std::vector<FlagStatus> GetFlagStatusesForOSD() const;
  struct HuntingStatus
  {
    bool enabled = false;
    HuntingOption option = HuntingOption::Skip;
    ShaderType active_type = ShaderType::Pixel;
    u64 selected_hash = 0;
    int selected_position = -1;
    int selected_total = 0;
    bool element_mode = false;
    int selected_element = -1;
    int element_total = 0;
    int element_start = -1;
    int element_end = -1;
    std::vector<u64> texture_filters;
    bool group_hunt_enabled = false;
    bool group_hunt_seed_valid = false;
    ShaderType group_hunt_seed_type = ShaderType::Pixel;
    u64 group_hunt_seed_hash = 0;
    int group_hunt_selected_match = -1;
    int group_hunt_total_matches = 0;
    int group_hunt_draw_index = -1;
    u64 group_hunt_current_hash = 0;
    RuntimeElementSignature group_hunt_seed_signature;
  };
  HuntingStatus GetHuntingStatusForOSD() const;

  // Debug logging: log override matches per draw call (toggle via UI)
  void SetDebugLogging(bool enabled);
  bool IsDebugLogging() const;
  // Log draw calls involving any override-relevant hash that had no override match
  void DebugLogUnmatched(u64 vs_hash, u64 ps_hash, u64 gs_hash) const;

  // Dump shader source to Dump/Shaders/<game_id>/<hash>-<type>.txt
  bool DumpShader(const std::string& game_id, ShaderType type, u64 hash) const;

  struct TextureUsage
  {
    u64 hash = 0;
    std::string name;
  };

  // Query texture hashes seen with a specific shader hash in the previous frame.
  std::vector<TextureUsage> GetTexturesForHash(ShaderType type, u64 hash) const;

  // Query texture hashes seen with the currently selected shader in the previous frame.
  std::vector<TextureUsage> GetTexturesForSelectedShader() const;

  // Hunting-only per-texture skip filters for the currently selected shader.
  void SetTextureSkipEnabled(u64 texture_hash, bool enabled);
  bool IsTextureSkipEnabled(u64 texture_hash) const;
  void ClearTextureSkipFilters();
  void NextTextureHash();
  void PrevTextureHash();
  u64 GetSelectedTextureHash() const;
  bool ToggleSelectedTextureHashFilter();
  bool SaveSelectedShaderOverride(const std::string& game_id, HandlingType handling);
  void SetTextureToolActive(bool active);

private:
  ShaderHunter() = default;

  static constexpr int TYPE_COUNT = static_cast<int>(ShaderType::Count);

  mutable std::mutex m_mutex;
  bool m_enabled = false;
  HuntingOption m_hunting_option = HuntingOption::Skip;
  bool m_should_highlight_selected_draw = false;
  ShaderType m_active_type = ShaderType::Pixel;

  // Double-buffered: video thread writes m_collecting, swaps to m_display at frame end.
  std::array<std::set<u64>, TYPE_COUNT> m_collecting{};
  std::array<std::set<u64>, TYPE_COUNT> m_display{};

  std::array<u64, TYPE_COUNT> m_selected_hash{~0ULL, ~0ULL, ~0ULL};
  std::array<int, TYPE_COUNT> m_selected_pos{-1, -1, -1};

  // Element mode: per-draw-call cycling within a selected shader
  bool m_element_mode = false;
  int m_selected_element = -1;   // Cursor position for identification (-1 = skip all)
  int m_element_start = -1;      // Range start (-1 = not set, use cursor)
  int m_element_end = -1;        // Range end (-1 = not set, use cursor)
  int m_element_total = 0;       // Total draw calls using selected hash last frame
  int m_element_counter = 0;     // Per-frame counter, incremented in ShouldSkipDraw

  // UID cache: hash → raw UID bytes (for shader source regeneration/dump)
  std::array<std::unordered_map<u64, std::vector<u8>>, TYPE_COUNT> m_uid_cache{};

  // ClearEFB: when a shader with clear_efb=true is drawn, set this flag.
  // The next EFB copy operation will clear to transparent instead of copying.
  bool m_clear_next_efb = false;
  int m_pending_clear_min = 0;   // Pending clear lower width bound (0 = no lower bound)
  int m_pending_clear_max = 0;   // Pending clear upper width bound (0 = no upper bound)
  std::unordered_set<u64> m_clear_efb_hashes;  // Hashes with clear_efb=true (any handling type)
  std::unordered_map<u64, std::pair<int, int>> m_clear_efb_bounds;  // hash → (min_w, max_w)

  // Persistent overrides loaded from per-game INI
  std::vector<ShaderOverride> m_overrides;
  std::unordered_set<u64> m_override_ps_hashes;  // Skip handling
  std::unordered_set<u64> m_override_vs_hashes;
  std::unordered_set<u64> m_override_gs_hashes;
  std::unordered_set<u64> m_screen_hashes;       // Screen handling (all shader types)
  std::unordered_set<u64> m_fullscreen_hashes;   // Fullscreen handling (all shader types)
  std::unordered_set<u64> m_fullscreen_mono_hashes;  // FullscreenMono handling (all shader types)
  std::unordered_set<u64> m_headlocked_hashes;   // HeadLocked handling (all shader types)
  std::unordered_map<u64, float> m_units_per_meter_overrides;  // hash -> per-override UPM
  std::unordered_map<u64, int> m_screen_layers;      // hash → manual layer (-1 = auto)
  std::unordered_map<u64, float> m_element_depths;  // hash → per-override element depth (-1 = global)
  std::string m_loaded_game_id;

  // Flag system: flags are active only while their shader is drawn each frame.
  struct FlagRule
  {
    ShaderType type = ShaderType::Pixel;
    u64 hash = 0;
    bool hash_family_match = false;
    u64 family_signature = 0;
    std::string flag_group;
    std::vector<u64> texture_hashes;
    bool texture_hashes_excluded = false;
  };
  std::vector<FlagRule> m_flag_rules;  // flag activation rules
  std::unordered_set<std::string> m_flags_seen_this_frame; // flags seen in current frame
  std::unordered_map<std::string, int> m_flag_age;        // flag → 0 if active (from previous frame)

  // Debug logging: log override matches once per unique draw combo per frame
  bool m_debug_logging = false;
  mutable std::unordered_set<u64> m_debug_logged_combos;  // combined hash of vs+ps logged this frame
  std::unordered_set<u64> m_all_override_hashes;   // all hashes from any override (for unmatched log)

  // Conditional overrides: flag condition, draw-call range, and texture filters.
  struct ConditionalOverride
  {
    u64 hash;
    HandlingType handling;
    ShaderType type;
    MatchMode match_mode;
    int layer;
    float element_depth;
    float units_per_meter;
    int element_start;         // First draw index (-1 = all)
    int element_end;           // Last draw index (-1 = all)
    int element_reference_total;  // Draw-call count used when range was authored (0 = fixed)
    std::vector<u64> texture_hashes;  // Only apply when any listed texture is bound (empty = any)
    bool texture_hashes_excluded;     // false=Include list, true=Exclude list
    bool hash_family_match;           // false=exact hash, true=family signature
    u64 family_signature;             // 0=resolve from current hash when available
    RuntimeElementSignature runtime_element;
    std::string condition_flag;
    bool condition_inverted;
  };
  std::vector<ConditionalOverride> m_conditional_overrides;

  // Per-hash per-frame draw counters for element-aware overrides.
  // Written by AdvanceOverrideDrawCounters(), read by ShouldSkipByOverride/GetOverrideHandling.
  // All access is on the video thread only, no lock needed.
  bool m_has_element_overrides = false;
  mutable std::unordered_map<u64, int> m_override_draw_counters;
  mutable std::unordered_map<u64, int> m_override_draw_totals_prev;  // previous frame totals
  mutable std::unordered_map<u64, int> m_runtime_element_reference_totals;
  mutable int m_current_vs_draw_idx = 0;
  mutable int m_current_ps_draw_idx = 0;
  mutable int m_current_gs_draw_idx = 0;

  std::pair<int, int> ResolveElementRange(const ConditionalOverride& cond) const;
  bool IsConditionFlagMatch(const ConditionalOverride& cond) const;
  bool IsConditionalHashMatch(const ConditionalOverride& cond, u64 vs_hash, u64 ps_hash,
                              u64 gs_hash) const;
  bool IsRuntimeElementMatch(const RuntimeElementSignature& signature) const;
  u64 ResolveConditionalFamilySignature(const ConditionalOverride& cond) const;
  bool ShouldBypassSelectedOverrideForTextureTool(u64 vs_hash, u64 ps_hash, u64 gs_hash) const;

  // Texture tracking: current draw's bound textures + per-element capture during hunting.
  std::array<u64, 8> m_current_draw_textures{};
  std::array<std::string, 8> m_current_draw_texture_names{};
  RuntimeElementSignature m_current_draw_signature{};
  RuntimeElementSignature m_selected_draw_signature{};
  ShaderType m_selected_draw_signature_type = ShaderType::Pixel;
  u64 m_selected_draw_signature_hash = 0;
  std::array<std::unordered_map<u64, u64>, TYPE_COUNT> m_shader_family_signatures{};
  u64 m_current_vs_family = 0;
  u64 m_current_ps_family = 0;
  u64 m_current_gs_family = 0;
  std::vector<std::array<u64, 8>> m_element_textures_collecting;
  std::vector<std::array<u64, 8>> m_element_textures_display;
  std::vector<std::array<std::string, 8>> m_element_tex_names_collecting;
  std::vector<std::array<std::string, 8>> m_element_tex_names_display;
  std::array<std::unordered_map<u64, std::unordered_map<u64, std::string>>, TYPE_COUNT>
      m_shader_texture_usage_collecting{};
  std::array<std::unordered_map<u64, std::unordered_map<u64, std::string>>, TYPE_COUNT>
      m_shader_texture_usage_display{};
  std::unordered_map<u64, std::string> m_texture_usage_collecting;
  std::unordered_map<u64, std::string> m_texture_usage_display;
  std::unordered_set<u64> m_texture_skip_filters;
  u64 m_selected_texture_hash = 0;
  int m_selected_texture_pos = -1;
  bool m_texture_skip_mode_active = false;
  std::atomic<bool> m_texture_tool_active{false};
  bool m_has_texture_overrides = false;

  bool HasActiveGroupHuntLocked() const;
  u64 GetHashForType(ShaderType type, u64 vs_hash, u64 ps_hash, u64 gs_hash) const;

  bool m_group_hunt_enabled = false;
  RuntimeElementSignature m_group_hunt_seed_signature{};
  ShaderType m_group_hunt_seed_type = ShaderType::Pixel;
  u64 m_group_hunt_seed_hash = 0;
  int m_group_hunt_selected_match = 0;
  int m_group_hunt_match_total = 0;
  int m_group_hunt_match_counter = 0;
  int m_group_hunt_draw_counter = 0;
  HuntedDraw m_group_hunt_selected_draw{};
};
