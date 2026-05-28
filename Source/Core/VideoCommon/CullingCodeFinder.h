// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

namespace Core
{
class System;
}

class CullingCodeFinder
{
public:
  struct FrameStats
  {
    int draw_calls = 0;
    int triangles_drawn = 0;
    int prims = 0;
    int drawn_objects = 0;
  };

  enum class PatchTemplateKind
  {
    LiR3True = 0,
    LiR3False,
    OriR0True,
    OriR0False,
    Count
  };

  struct ScanConfig
  {
    int savestate_slot = 1;
    int settle_frames = 5;
    int min_draw_call_increase = 1;
    int min_triangle_increase = 0;
    int min_object_increase = 0;
    int timeout_seconds = 3;
    int candidate_limit = 0;
    u32 start_address = 0;
    u32 end_address = 0;
    bool turbo_during_scan = true;
    bool mute_audio_during_scan = true;
    bool auto_capture_reference_state = true;
    std::string symbol_filter;
    std::string output_folder;
    std::array<bool, static_cast<size_t>(PatchTemplateKind::Count)> enabled_templates = {
        true, true, true, true};
  };

  struct Candidate
  {
    std::string symbol_name;
    u32 address = 0;
  };

  struct CandidateSource
  {
    std::string symbol_name;
    u32 address = 0;
    bool is_function = true;
    bool valid_entry = true;
    bool valid_stub = true;
  };

  struct ScanResult
  {
    std::string symbol_name;
    u32 address = 0;
    PatchTemplateKind template_kind = PatchTemplateKind::LiR3True;
    FrameStats baseline{};
    FrameStats measured{};
    int draw_call_delta = 0;
    int triangle_delta = 0;
    int prim_delta = 0;
    int object_delta = 0;
    bool caused_timeout = false;
    std::string screenshot_path;
    std::string ar_code_text;
  };

  enum class ScanState
  {
    Idle,
    Scanning,
    Paused,
    Stopped,
    Finished,
    Error,
  };

  struct ManifestData
  {
    std::string game_id;
    std::string map_path;
    ScanConfig config;
    FrameStats baseline{};
    bool baseline_set = false;
    std::vector<Candidate> candidates;
    std::vector<ScanResult> results;
    size_t next_candidate_index = 0;
    size_t next_template_index = 0;
    bool has_last_tested_position = false;
    u32 last_tested_address = 0;
    size_t last_tested_template_index = 0;
    ScanState state = ScanState::Idle;
  };

  struct PatchTemplate
  {
    PatchTemplateKind kind = PatchTemplateKind::LiR3True;
    const char* display_name = "";
    u32 first_instruction = 0;
  };

  static CullingCodeFinder& GetInstance();

  void OnFrameEnd();

  FrameStats GetLatestStats() const;
  FrameStats GetBaseline() const;
  bool HasBaseline() const { return m_baseline_set; }
  void SetBaseline();

  bool StartScan(Core::System& system, ScanConfig config, std::string* error = nullptr);
  bool RestoreSession(Core::System& system, std::string_view output_folder,
                      std::string* error = nullptr, std::string* restored_message = nullptr);
  void PauseScan();
  void ResumeScan();
  void StopScan();
  void ClearResults(std::string* error = nullptr);
  void NotifySavestateLoaded();
  bool GenerateSymbolMap(Core::System& system, std::string* error = nullptr);

  ScanState GetState() const;
  float GetProgress() const;
  int GetResultCount() const;
  int GetCandidateCount() const;
  std::string GetStatusText() const;
  std::string GetMapPath() const;
  ScanConfig GetConfig() const;
  std::vector<ScanResult> GetResults() const;
  bool IsTurboRuntimeOverrideActive() const;
  bool IsAudioMuteRuntimeOverrideActive() const;

  bool AdvanceScan(Core::System& system);

  static std::string GetPatchTemplateName(PatchTemplateKind kind);
  static std::array<std::string, 2> BuildARCodeLines(u32 address, PatchTemplateKind kind);
  static std::string BuildARCodeText(u32 address, PatchTemplateKind kind);
  static std::string GetDefaultOutputFolder(std::string_view game_id);
  static std::vector<Candidate> BuildCandidatesFromSymbolInfo(
      std::vector<CandidateSource> sources, std::string_view symbol_filter, int candidate_limit,
      u32 start_address, u32 end_address);
  static bool SaveManifest(const std::string& manifest_path, const ManifestData& manifest,
                           std::string* error = nullptr);
  static bool LoadManifest(const std::string& manifest_path, ManifestData* manifest,
                           std::string* error = nullptr);
  static bool AdvancePositionAfterCrash(const ScanConfig& config, size_t candidate_count,
                                        size_t* candidate_index, size_t* template_index);

private:
  CullingCodeFinder() = default;

  enum class ScanPhase
  {
    None,
    RequestLoad,
    WaitingForLoad,
    ApplyPatch,
    WaitingForSettle,
    Finalize,
  };

  bool BuildCandidatesFromMap(Core::System& system, std::string* error);
  bool TryResumeFromManifest(Core::System& system, const ScanConfig& config, std::string* error);
  bool StartFreshScan(Core::System& system, const ScanConfig& config, std::string* error);
  bool CaptureReferenceSavestate(Core::System& system, std::string* error);
  bool WriteManifest(std::string* error = nullptr) const;
  void UpdateStatusText();
  void RestorePatchedInstructions(Core::System& system);
  void ApplyRuntimeOverrides(Core::System& system);
  void RestoreRuntimeOverrides(Core::System& system);
  void AdvanceToNextTemplate();
  void SortResults();
  FrameStats AverageSamples() const;
  bool ShouldKeepResult(const FrameStats& measured) const;
  std::string BuildResultScreenshotPath(const Candidate& candidate, PatchTemplateKind kind) const;
  void AppendResult(ScanResult result);
  int GetEnabledTemplateCount() const;
  size_t FindNextEnabledTemplateIndex(size_t start_index) const;
  size_t ComputeCompletedTests() const;

  mutable std::mutex m_stats_mutex;
  FrameStats m_display_stats{};
  std::atomic<int> m_frame_counter{0};

  FrameStats m_baseline{};
  bool m_baseline_set = false;

  ScanState m_state = ScanState::Idle;
  ScanPhase m_phase = ScanPhase::None;
  ScanConfig m_scan_config{};
  std::string m_game_id;
  std::string m_map_path;
  std::string m_manifest_path;
  std::string m_status_text = "Idle";

  std::vector<Candidate> m_candidates;
  std::vector<ScanResult> m_results;
  size_t m_candidate_index = 0;
  size_t m_template_index = 0;
  bool m_has_last_tested_position = false;
  u32 m_last_tested_address = 0;
  size_t m_last_tested_template_index = 0;

  u32 m_saved_entry_instruction = 0;
  u32 m_saved_next_instruction = 0;
  u32 m_patched_address = 0;
  PatchTemplateKind m_active_template = PatchTemplateKind::LiR3True;
  bool m_instruction_patched = false;
  bool m_load_completed = false;
  bool m_runtime_overrides_active = false;
  bool m_turbo_override_active = false;
  bool m_audio_mute_override_active = false;
  int m_settle_frame_start = 0;
  int m_last_sampled_frame = 0;
  std::vector<FrameStats> m_settle_samples;
  std::chrono::steady_clock::time_point m_phase_started_at{};
};
