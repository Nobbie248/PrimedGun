// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "VideoCommon/MetroidElementClassifier.h"
#include "VideoCommon/ShaderHunter.h"

class ElementsGroupManager
{
public:
  using RuntimeElementSignature = ShaderHunter::RuntimeElementSignature;
  using ShaderType = ShaderHunter::ShaderType;
  using HandlingType = ShaderHunter::HandlingType;
  using HuntingOption = ShaderHunter::HuntingOption;

  enum class PreviewAction
  {
    None = 0,
    Skip = 1,
    Pink = 2
  };

  enum class MatchKind
  {
    RuntimeSignature = 0,
    ProfileLayer = 1,
  };

  struct DrawRecord
  {
    int draw_index = -1;
    u32 draw_sequence = 0;
    u64 vs_hash = 0;
    u64 ps_hash = 0;
    u64 gs_hash = 0;
    u64 vs_family = 0;
    u64 ps_family = 0;
    u64 gs_family = 0;
    RuntimeElementSignature signature;
    MetroidElementProfile profile_id = MetroidElementProfile::None;
    MetroidElementLayer profile_layer = MetroidElementLayer::Unknown;
    std::string profile_layer_name;
    std::array<u64, 8> textures{};
    std::array<std::string, 8> texture_names{};

    u64 GetHash(ShaderType type) const;
    u64 GetFamily(ShaderType type) const;
  };

  struct StableSubMatchSignature
  {
    RuntimeElementSignature runtime_element;
    u64 vs_family = 0;
    u64 ps_family = 0;
    u64 gs_family = 0;
    std::vector<u64> texture_hashes;
    int occurrence_slot = -1;
  };

  struct SelectedSubgroupSignature
  {
    u64 vs_family = 0;
    u64 ps_family = 0;
    u64 gs_family = 0;
    std::vector<u64> texture_hashes;
  };

  struct CurrentMatchCandidate
  {
    SelectedSubgroupSignature subgroup;
    DrawRecord representative_draw;
    int raw_draw_count = 0;
    bool active_this_frame = false;
  };

  struct ElementGroupOverride
  {
    std::string name;
    std::string comments;
    std::string credits;
    MatchKind match_kind = MatchKind::RuntimeSignature;
    HandlingType handling = HandlingType::Skip;
    RuntimeElementSignature runtime_element;
    MetroidElementProfile profile_id = MetroidElementProfile::None;
    std::vector<MetroidElementLayer> profile_layers;
    int layer = -1;
    float element_depth = -1.0f;
    float units_per_meter = -1.0f;
    int element_start = -1;
    int element_end = -1;
    int element_reference_total = 0;
    std::vector<u64> texture_hashes;
    bool texture_hashes_excluded = false;
    std::vector<SelectedSubgroupSignature> selected_match_filter;
    bool selected_match_filter_excluded = false;
    bool enabled = true;
    bool user_defined = true;
    std::string flag_group;
    std::string condition_flag;
    bool condition_inverted = false;
  };

  struct SeedCandidate
  {
    RuntimeElementSignature signature;
    RuntimeElementSignature group_signature;
    DrawRecord representative_draw;
    int occurrence_count = 0;
  };

  struct Status
  {
    bool popup_open = false;
    bool hunt_enabled = false;
    HuntingOption option = HuntingOption::Skip;
    bool seed_valid = false;
    RuntimeElementSignature seed_signature;
    std::optional<DrawRecord> seed_draw;
    int selected_seed_index = -1;
    int total_seed_candidates = 0;
    int selected_match = 0;
    int total_matches = 0;
    int active_matches = 0;
    int selected_match_filter_count = 0;
    bool selected_match_filter_excluded = false;
    int highlighted_match_raw_draw_count = 0;
    std::optional<DrawRecord> highlighted_draw;
  };

  static ElementsGroupManager& GetInstance();

  static std::vector<ElementGroupOverride> LoadOverridesFromINI(
      const std::string& game_id, std::optional<u16> revision = std::nullopt);
  static void SaveOverridesToINI(const std::string& game_id,
                                 const std::vector<ElementGroupOverride>& overrides);
  static RuntimeElementSignature MakeSelectedMatchFilterSignature(
      const RuntimeElementSignature& signature);
  static StableSubMatchSignature MakeStableSubMatchSignature(const DrawRecord& draw,
                                                             int occurrence_slot);
  static SelectedSubgroupSignature MakeSelectedSubgroupSignature(const DrawRecord& draw);

  void LoadOverrides(const std::string& game_id);
  void LoadOverridesIfNeeded(const std::string& game_id);
  bool HasOverrides() const;
  bool NeedsProfileClassification() const;
  bool IsMetroidPrime1GCProfileActive() const;

  void SetPopupOpen(bool open);
  bool IsPopupOpen() const;
  void SetHuntEnabled(bool enabled);
  bool IsHuntEnabled() const;
  void SetHuntingOption(HuntingOption option);
  HuntingOption GetHuntingOption() const;

  void SelectSeedCandidate(int index);
  void SetSeedGroupMask(bool projection, bool layer, bool viewport, bool scissor,
                        bool render_state);
  void SelectMatch(int index);
  void NextMatch();
  void PrevMatch();

  std::vector<SeedCandidate> GetSeedCandidates() const;
  std::vector<CurrentMatchCandidate> GetCurrentMatches() const;
  std::vector<SelectedSubgroupSignature> GetSelectedMatchFilters() const;
  bool GetSelectedMatchFilterExcluded() const;
  std::vector<CurrentMatchCandidate> GetSelectedMatchDisplayDraws() const;
  std::vector<DrawRecord> GetCurrentTextureSourceDraws() const;
  std::optional<DrawRecord> GetHighlightedDraw() const;
  std::optional<DrawRecord> GetSelectedSeedDraw() const;
  std::optional<DrawRecord> GetSelectedCurrentMatchDraw() const;
  std::optional<DrawRecord> GetCurrentTextureSourceDraw() const;
  RuntimeElementSignature GetSeedSignature() const;
  Status GetStatus() const;
  bool IsCurrentMatchFilterEnabled(int match_index) const;
  void SetSelectedMatchFilterExcluded(bool excluded);
  void SetCurrentMatchFilterEnabled(int match_index, bool enabled);
  void AddCurrentMatchFilter();
  void RemoveSelectedMatchFilter(int index);

  PreviewAction RegisterDraw(const DrawRecord& draw);
  void ClassifyProfileDraw(DrawRecord* draw, const MetroidProjectionMetrics& metrics);
  void OnFrameEnd();

  void AdvanceOverrideDrawCounters(const DrawRecord& draw);
  void RegisterFlagsForDraw(const DrawRecord& draw);
  bool ShouldSkipByOverride(const DrawRecord& draw) const;
  HandlingType GetOverrideHandling(const DrawRecord& draw) const;
  int GetOverrideLayer(const DrawRecord& draw) const;
  float GetOverrideElementDepth(const DrawRecord& draw) const;
  float GetOverrideUnitsPerMeter(const DrawRecord& draw) const;

private:
  ElementsGroupManager() = default;

  struct CounterKey
  {
    u64 value = 0;

    bool operator==(const CounterKey& other) const
    {
      return value == other.value;
    }
  };

  struct CounterKeyHasher
  {
    size_t operator()(const CounterKey& key) const noexcept;
  };

  struct StableSubMatchContext
  {
    bool valid = false;
    u32 draw_sequence = 0;
    StableSubMatchSignature signature;
  };

  struct GroupMask
  {
    bool projection = false;
    bool layer = false;
    bool viewport = false;
    bool scissor = false;
    bool render_state = false;
  };

  void ClearSeedSelectionLocked();
  void SelectSeedCandidateLocked(int index);
  bool HasActiveHuntLocked() const;
  RuntimeElementSignature GetMaskedSeedSignatureLocked() const;
  RuntimeElementSignature GetSeedGroupSignatureLocked(const RuntimeElementSignature& signature) const;
  bool MatchesSelectedMatchFilterLocked(const DrawRecord& draw) const;
  bool MatchesSelectedMatchFilterSignatureLocked(const SelectedSubgroupSignature& signature) const;
  std::vector<CurrentMatchCandidate> ResolveSelectedMatchDisplayDrawsLocked() const;
  StableSubMatchSignature GetStableSubMatchSignatureLocked(const DrawRecord& draw) const;
  std::pair<int, int> ResolveElementRange(const ElementGroupOverride& entry,
                                          const DrawRecord& draw) const;
  bool DoesEntryMatchForRange(const ElementGroupOverride& entry, const DrawRecord& draw) const;
  bool DoesSelectedMatchFilterPass(const ElementGroupOverride& entry, const DrawRecord& draw) const;
  bool DoesEntryMatch(const ElementGroupOverride& entry, const DrawRecord& draw,
                      bool include_condition) const;
  bool DoesTextureFilterPass(const DrawRecord& draw, const ElementGroupOverride& entry) const;
  CounterKey GetCounterKey(const ElementGroupOverride& entry) const;

  mutable std::mutex m_mutex;
  int m_popup_open_count = 0;
  std::atomic_bool m_popup_open = false;
  std::atomic_bool m_has_overrides = false;
  bool m_hunt_enabled = false;
  std::atomic_bool m_has_profile_overrides = false;
  HuntingOption m_hunting_option = HuntingOption::Skip;
  RuntimeElementSignature m_seed_signature{};
  RuntimeElementSignature m_seed_group_signature{};
  std::optional<DrawRecord> m_seed_draw;
  GroupMask m_group_mask;
  int m_selected_seed_index = -1;
  int m_selected_match = 0;
  std::vector<SelectedSubgroupSignature> m_selected_match_filters;
  bool m_selected_match_filters_excluded = false;

  std::vector<DrawRecord> m_collecting_draws;
  std::vector<DrawRecord> m_display_draws;
  std::vector<SeedCandidate> m_display_seed_candidates;
  std::vector<DrawRecord> m_collecting_matches;
  std::vector<DrawRecord> m_display_raw_matches;
  std::vector<SelectedSubgroupSignature> m_display_raw_match_signatures;
  std::vector<CurrentMatchCandidate> m_display_matches;
  int m_collecting_match_total = 0;
  int m_display_match_total = 0;
  std::optional<DrawRecord> m_collecting_highlighted_draw;
  std::optional<DrawRecord> m_display_highlighted_draw;
  int m_display_highlighted_match_raw_draw_count = 0;

  std::vector<ElementGroupOverride> m_overrides;
  std::vector<MetroidElementProfile> m_active_profiles;
  MetroidElementClassifier m_metroid_classifier;
  std::string m_loaded_game_id;

  mutable std::unordered_map<CounterKey, int, CounterKeyHasher> m_draw_counters;
  mutable std::unordered_map<CounterKey, int, CounterKeyHasher> m_draw_totals_prev;
  mutable std::unordered_map<CounterKey, int, CounterKeyHasher> m_current_draw_indices;
  mutable std::unordered_map<u64, int> m_stable_submatch_occurrence_counters;
  mutable StableSubMatchContext m_current_stable_submatch;
};
