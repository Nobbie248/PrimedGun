// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/CullingCodeFinder.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <optional>
#include <utility>

#include <fmt/format.h>

#include "AudioCommon/AudioCommon.h"
#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCAnalyst.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/SignatureDB/SignatureDB.h"
#include "Core/State.h"
#include "Core/System.h"
#include "VideoCommon/FrameDumper.h"
#include "VideoCommon/Statistics.h"

namespace
{
using PatchTemplateKind = CullingCodeFinder::PatchTemplateKind;

constexpr size_t TEMPLATE_COUNT = static_cast<size_t>(PatchTemplateKind::Count);
constexpr std::array<CullingCodeFinder::PatchTemplateKind, TEMPLATE_COUNT> TEMPLATE_ORDER = {
    PatchTemplateKind::LiR3True, PatchTemplateKind::LiR3False, PatchTemplateKind::OriR0True,
    PatchTemplateKind::OriR0False};

constexpr u32 BLR_INSTRUCTION = 0x4E800020u;
constexpr char MANIFEST_SECTION_SESSION[] = "Session";
constexpr char MANIFEST_SECTION_BASELINE[] = "Baseline";
constexpr char MANIFEST_PREFIX_CANDIDATE[] = "Candidate_";
constexpr char MANIFEST_PREFIX_RESULT[] = "Result_";

std::array<CullingCodeFinder::PatchTemplate, TEMPLATE_COUNT> GetPatchTemplates()
{
  return {{
      {PatchTemplateKind::LiR3True, "38600001 + BLR", 0x38600001u},
      {PatchTemplateKind::LiR3False, "38600000 + BLR", 0x38600000u},
      {PatchTemplateKind::OriR0True, "60000001 + BLR", 0x60000001u},
      {PatchTemplateKind::OriR0False, "60000000 + BLR", 0x60000000u},
  }};
}

const CullingCodeFinder::PatchTemplate& LookupTemplate(PatchTemplateKind kind)
{
  static const auto templates = GetPatchTemplates();
  return templates[static_cast<size_t>(kind)];
}

std::string MakeSectionName(std::string_view prefix, size_t index)
{
  return fmt::format("{}{}", prefix, index);
}

std::string MakeTemplateMask(
    const std::array<bool, static_cast<size_t>(PatchTemplateKind::Count)>& enabled_templates)
{
  std::string mask;
  mask.reserve(TEMPLATE_COUNT);
  for (bool enabled : enabled_templates)
    mask.push_back(enabled ? '1' : '0');
  return mask;
}

std::array<bool, static_cast<size_t>(PatchTemplateKind::Count)> ParseTemplateMask(
    std::string_view mask)
{
  std::array<bool, static_cast<size_t>(PatchTemplateKind::Count)> enabled_templates = {true, true,
                                                                                         true,
                                                                                         true};
  if (mask.size() != TEMPLATE_COUNT)
    return enabled_templates;

  for (size_t i = 0; i < TEMPLATE_COUNT; ++i)
    enabled_templates[i] = (mask[i] != '0');
  return enabled_templates;
}

std::string StateToString(CullingCodeFinder::ScanState state)
{
  switch (state)
  {
  case CullingCodeFinder::ScanState::Idle:
    return "Idle";
  case CullingCodeFinder::ScanState::Scanning:
    return "Scanning";
  case CullingCodeFinder::ScanState::Paused:
    return "Paused";
  case CullingCodeFinder::ScanState::Stopped:
    return "Stopped";
  case CullingCodeFinder::ScanState::Finished:
    return "Finished";
  case CullingCodeFinder::ScanState::Error:
    return "Error";
  }

  return "Idle";
}

CullingCodeFinder::ScanState StringToState(std::string_view state)
{
  if (state == "Scanning")
    return CullingCodeFinder::ScanState::Scanning;
  if (state == "Paused")
    return CullingCodeFinder::ScanState::Paused;
  if (state == "Stopped")
    return CullingCodeFinder::ScanState::Stopped;
  if (state == "Finished")
    return CullingCodeFinder::ScanState::Finished;
  if (state == "Error")
    return CullingCodeFinder::ScanState::Error;
  return CullingCodeFinder::ScanState::Idle;
}

std::string ScreenshotBaseName(const CullingCodeFinder::Candidate& candidate,
                               PatchTemplateKind template_kind)
{
  std::string template_name = CullingCodeFinder::GetPatchTemplateName(template_kind);
  Common::ToLower(&template_name);
  template_name = ReplaceAll(template_name, " ", "_");
  template_name = ReplaceAll(template_name, "+", "");
  return fmt::format("{:08X}_{}", candidate.address, template_name);
}

std::string BuildSavestateFilename(std::string_view game_id, int slot)
{
  return fmt::format("{}{}.s{:02d}", File::GetUserPath(D_STATESAVES_IDX), game_id, slot);
}

std::optional<std::filesystem::file_time_type> GetLastWriteTime(const std::string& path)
{
  std::error_code error;
  const auto timestamp = std::filesystem::last_write_time(StringToPath(path), error);
  if (error)
    return std::nullopt;
  return timestamp;
}

size_t FindCandidateIndexByAddress(const std::vector<CullingCodeFinder::Candidate>& candidates, u32 address)
{
  const auto it = std::ranges::find(candidates, address, &CullingCodeFinder::Candidate::address);
  if (it == candidates.end())
    return candidates.size();
  return static_cast<size_t>(std::distance(candidates.begin(), it));
}
}  // namespace

CullingCodeFinder& CullingCodeFinder::GetInstance()
{
  static CullingCodeFinder instance;
  return instance;
}

void CullingCodeFinder::OnFrameEnd()
{
  std::lock_guard lock(m_stats_mutex);
  m_display_stats.draw_calls = g_stats.this_frame.num_draw_calls;
  m_display_stats.triangles_drawn = g_stats.this_frame.num_triangles_drawn;
  m_display_stats.prims = g_stats.this_frame.num_prims;
  m_display_stats.drawn_objects = g_stats.this_frame.num_drawn_objects;
  m_frame_counter.fetch_add(1, std::memory_order_release);
}

CullingCodeFinder::FrameStats CullingCodeFinder::GetLatestStats() const
{
  std::lock_guard lock(m_stats_mutex);
  return m_display_stats;
}

CullingCodeFinder::FrameStats CullingCodeFinder::GetBaseline() const
{
  return m_baseline;
}

void CullingCodeFinder::SetBaseline()
{
  std::lock_guard lock(m_stats_mutex);
  m_baseline = m_display_stats;
  m_baseline_set = true;
}

bool CullingCodeFinder::StartScan(Core::System& system, ScanConfig config, std::string* error)
{
  if (!m_baseline_set)
  {
    if (error)
      *error = "Set a baseline first.";
    return false;
  }

  if (config.output_folder.empty())
    config.output_folder = GetDefaultOutputFolder(SConfig::GetInstance().GetGameID());

  File::CreateFullPath(config.output_folder + DIR_SEP);
  m_scan_config = std::move(config);
  m_game_id = SConfig::GetInstance().GetGameID();
  m_manifest_path = PathToString(StringToPath(m_scan_config.output_folder) / "manifest.ini");

  m_state = ScanState::Idle;
  m_phase = ScanPhase::None;
  m_status_text = "Preparing scan...";
  m_settle_samples.clear();
  m_load_completed = false;
  m_instruction_patched = false;
  m_saved_entry_instruction = 0;
  m_saved_next_instruction = 0;
  m_patched_address = 0;
  m_runtime_overrides_active = false;
  m_turbo_override_active = false;
  m_audio_mute_override_active = false;
  m_has_last_tested_position = false;
  m_last_tested_address = 0;
  m_last_tested_template_index = 0;

  if (TryResumeFromManifest(system, m_scan_config, error))
  {
    if (m_state != ScanState::Finished)
    {
      m_state = ScanState::Scanning;
      m_phase = ScanPhase::RequestLoad;
      ApplyRuntimeOverrides(system);
    }
    UpdateStatusText();
    return true;
  }

  return StartFreshScan(system, m_scan_config, error);
}

bool CullingCodeFinder::RestoreSession(Core::System& system, std::string_view output_folder,
                                       std::string* error, std::string* restored_message)
{
  const std::string manifest_path =
      PathToString(StringToPath(std::string(output_folder)) / "manifest.ini");
  if (!File::Exists(manifest_path))
  {
    if (error)
      *error = "No manifest.ini was found in the selected output folder.";
    return false;
  }

  ManifestData manifest;
  if (!LoadManifest(manifest_path, &manifest, error))
    return false;

  const std::string current_game_id = SConfig::GetInstance().GetGameID();
  if (current_game_id.empty())
  {
    if (error)
      *error = "Boot the game before restoring a culling scan session.";
    return false;
  }

  if (manifest.game_id != current_game_id)
  {
    if (error)
    {
      *error = fmt::format("The saved session is for {} but the running game is {}.",
                           manifest.game_id, current_game_id);
    }
    return false;
  }

  if (manifest.candidates.empty())
  {
    if (error)
      *error = "The saved session does not contain any candidates.";
    return false;
  }

  if (manifest.state == ScanState::Finished || manifest.state == ScanState::Idle)
  {
    if (error)
      *error = "The saved session is not restorable because it is already complete or idle.";
    return false;
  }

  if (!manifest.baseline_set)
  {
    if (error)
      *error = "The saved session has no baseline statistics.";
    return false;
  }

  if (manifest.config.output_folder.empty())
    manifest.config.output_folder = std::string(output_folder);

  size_t candidate_index = manifest.next_candidate_index;
  size_t template_index = TEMPLATE_COUNT;
  for (size_t i = manifest.next_template_index; i < TEMPLATE_COUNT; ++i)
  {
    if (manifest.config.enabled_templates[i])
    {
      template_index = i;
      break;
    }
  }

  if (candidate_index >= manifest.candidates.size() || template_index >= TEMPLATE_COUNT)
  {
    if (manifest.has_last_tested_position)
    {
      candidate_index =
          FindCandidateIndexByAddress(manifest.candidates, manifest.last_tested_address);
      template_index = manifest.last_tested_template_index;
    }

    if (candidate_index >= manifest.candidates.size() || template_index >= TEMPLATE_COUNT ||
        !manifest.config.enabled_templates[template_index])
    {
      if (error)
        *error = "The saved session does not have a valid next test to restore.";
      return false;
    }
  }

  const Candidate skipped_candidate = manifest.candidates[candidate_index];
  const PatchTemplateKind skipped_template = TEMPLATE_ORDER[template_index];
  if (!AdvancePositionAfterCrash(manifest.config, manifest.candidates.size(), &candidate_index,
                                 &template_index))
  {
    if (error)
    {
      *error = fmt::format(
          "The saved session only had {:08X} ({}) remaining, so there is nothing to resume after "
          "skipping it.",
          skipped_candidate.address, GetPatchTemplateName(skipped_template));
    }
    return false;
  }

  m_scan_config = manifest.config;
  m_game_id = manifest.game_id;
  m_map_path = manifest.map_path;
  m_manifest_path = manifest_path;
  m_baseline = manifest.baseline;
  m_baseline_set = manifest.baseline_set;
  m_candidates = std::move(manifest.candidates);
  m_results = std::move(manifest.results);
  m_candidate_index = candidate_index;
  m_template_index = template_index;
  m_has_last_tested_position = manifest.has_last_tested_position;
  m_last_tested_address = manifest.last_tested_address;
  m_last_tested_template_index = manifest.last_tested_template_index;
  m_state = ScanState::Scanning;
  m_phase = ScanPhase::RequestLoad;
  m_status_text = fmt::format("Restored session after skipping {:08X} ({})",
                              skipped_candidate.address, GetPatchTemplateName(skipped_template));
  m_settle_samples.clear();
  m_load_completed = false;
  m_instruction_patched = false;
  m_saved_entry_instruction = 0;
  m_saved_next_instruction = 0;
  m_patched_address = 0;
  m_runtime_overrides_active = false;
  m_turbo_override_active = false;
  m_audio_mute_override_active = false;

  ApplyRuntimeOverrides(system);
  if (!WriteManifest(error))
  {
    RestorePatchedInstructions(system);
    RestoreRuntimeOverrides(system);
    m_state = ScanState::Error;
    if (error && !error->empty())
      m_status_text = *error;
    UpdateStatusText();
    return false;
  }

  UpdateStatusText();
  if (restored_message)
  {
    *restored_message =
        fmt::format("Skipped {:08X} ({}) and resumed from the next pending test.",
                    skipped_candidate.address, GetPatchTemplateName(skipped_template));
  }
  return true;
}

void CullingCodeFinder::PauseScan()
{
  if (m_state == ScanState::Scanning)
    m_state = ScanState::Paused;
}

void CullingCodeFinder::ResumeScan()
{
  if (m_state == ScanState::Paused)
    m_state = ScanState::Scanning;
}

void CullingCodeFinder::StopScan()
{
  m_state = ScanState::Stopped;
  m_phase = ScanPhase::None;
  UpdateStatusText();
}

void CullingCodeFinder::ClearResults(std::string* error)
{
  m_results.clear();
  if (!m_manifest_path.empty() && File::Exists(m_manifest_path))
    WriteManifest(error);
}

void CullingCodeFinder::NotifySavestateLoaded()
{
  m_load_completed = true;
}

bool CullingCodeFinder::GenerateSymbolMap(Core::System& system, std::string* error)
{
  if (SConfig::GetInstance().GetGameID().empty())
  {
    if (error)
      *error = "Boot a game before generating a symbol map.";
    return false;
  }

  std::string existing_map_path;
  std::string writable_map_path;
  PPCSymbolDB::FindMapFile(&existing_map_path, &writable_map_path);
  if (writable_map_path.empty())
  {
    if (error)
      *error = "Unable to determine the writable map path for the current title.";
    return false;
  }

  auto& memory = system.GetMemory();
  PPCSymbolDB& symbol_db = system.GetPPCSymbolDB();
  const Core::CPUThreadGuard guard(system);

  symbol_db.Clear();
  PPCAnalyst::FindFunctions(guard, Memory::MEM1_BASE_ADDR,
                            Memory::MEM1_BASE_ADDR + memory.GetRamSizeReal(), &symbol_db);

  SignatureDB db(SignatureDB::HandlerType::DSY);
  db.Load(File::GetSysDirectory() + TOTALDB);
  db.Apply(guard, &symbol_db);

  File::CreateFullPath(writable_map_path);
  if (!symbol_db.SaveSymbolMap(writable_map_path))
  {
    if (error)
      *error = fmt::format("Failed to save symbol map to {}", writable_map_path);
    return false;
  }

  m_map_path = writable_map_path;
  return true;
}

CullingCodeFinder::ScanState CullingCodeFinder::GetState() const
{
  return m_state;
}

float CullingCodeFinder::GetProgress() const
{
  const int enabled_templates = GetEnabledTemplateCount();
  if (enabled_templates == 0 || m_candidates.empty())
    return 0.0f;

  const size_t total_tests = m_candidates.size() * static_cast<size_t>(enabled_templates);
  if (total_tests == 0)
    return 0.0f;

  return static_cast<float>(ComputeCompletedTests()) / static_cast<float>(total_tests);
}

int CullingCodeFinder::GetResultCount() const
{
  return static_cast<int>(m_results.size());
}

int CullingCodeFinder::GetCandidateCount() const
{
  return static_cast<int>(m_candidates.size());
}

std::string CullingCodeFinder::GetStatusText() const
{
  return m_status_text;
}

std::string CullingCodeFinder::GetMapPath() const
{
  return m_map_path;
}

CullingCodeFinder::ScanConfig CullingCodeFinder::GetConfig() const
{
  return m_scan_config;
}

std::vector<CullingCodeFinder::ScanResult> CullingCodeFinder::GetResults() const
{
  return m_results;
}

bool CullingCodeFinder::IsTurboRuntimeOverrideActive() const
{
  return m_turbo_override_active;
}

bool CullingCodeFinder::IsAudioMuteRuntimeOverrideActive() const
{
  return m_audio_mute_override_active;
}

std::string CullingCodeFinder::GetPatchTemplateName(PatchTemplateKind kind)
{
  return LookupTemplate(kind).display_name;
}

std::array<std::string, 2> CullingCodeFinder::BuildARCodeLines(u32 address, PatchTemplateKind kind)
{
  const PatchTemplate& patch_template = LookupTemplate(kind);
  const u32 ar_address = 0x04000000u | (address & 0x01FFFFFFu);
  return {fmt::format("{:08X} {:08X}", ar_address, patch_template.first_instruction),
          fmt::format("{:08X} {:08X}", ar_address + 4, BLR_INSTRUCTION)};
}

std::string CullingCodeFinder::BuildARCodeText(u32 address, PatchTemplateKind kind)
{
  const auto lines = BuildARCodeLines(address, kind);
  return fmt::format("{}\n{}", lines[0], lines[1]);
}

std::string CullingCodeFinder::GetDefaultOutputFolder(std::string_view game_id)
{
  const std::filesystem::path base = StringToPath(File::GetUserPath(D_SCREENSHOTS_IDX));
  return PathToString(base / std::string(game_id) / "CullingFinder");
}

std::vector<CullingCodeFinder::Candidate> CullingCodeFinder::BuildCandidatesFromSymbolInfo(
    std::vector<CandidateSource> sources, std::string_view symbol_filter, int candidate_limit,
    u32 start_address, u32 end_address)
{
  std::vector<Candidate> candidates;
  candidates.reserve(sources.size());

  for (const CandidateSource& source : sources)
  {
    if (!source.is_function || !source.valid_entry || !source.valid_stub)
      continue;
    if (!symbol_filter.empty() &&
        !Common::CaseInsensitiveContains(source.symbol_name, symbol_filter))
    {
      continue;
    }
    if (start_address != 0 && source.address < start_address)
      continue;
    if (end_address != 0 && source.address > end_address)
      continue;

    candidates.push_back({source.symbol_name, source.address});
  }

  std::ranges::stable_sort(candidates, [](const Candidate& a, const Candidate& b) {
    if (a.address != b.address)
      return a.address < b.address;
    return a.symbol_name < b.symbol_name;
  });

  if (candidate_limit > 0 && static_cast<int>(candidates.size()) > candidate_limit)
    candidates.resize(static_cast<size_t>(candidate_limit));

  return candidates;
}

bool CullingCodeFinder::SaveManifest(const std::string& manifest_path, const ManifestData& manifest,
                                     std::string* error)
{
  Common::IniFile ini;
  auto* session = ini.GetOrCreateSection(MANIFEST_SECTION_SESSION);
  session->Set("GameId", manifest.game_id);
  session->Set("MapPath", manifest.map_path);
  session->Set("State", StateToString(manifest.state));
  session->Set("SavestateSlot", manifest.config.savestate_slot);
  session->Set("SettleFrames", manifest.config.settle_frames);
  session->Set("MinDrawCallIncrease", manifest.config.min_draw_call_increase);
  session->Set("MinTriangleIncrease", manifest.config.min_triangle_increase);
  session->Set("MinObjectIncrease", manifest.config.min_object_increase);
  session->Set("TimeoutSeconds", manifest.config.timeout_seconds);
  session->Set("CandidateLimit", manifest.config.candidate_limit);
  session->Set("StartAddress", manifest.config.start_address);
  session->Set("EndAddress", manifest.config.end_address);
  session->Set("TurboDuringScan", manifest.config.turbo_during_scan);
  session->Set("MuteAudioDuringScan", manifest.config.mute_audio_during_scan);
  session->Set("AutoCaptureReferenceState", manifest.config.auto_capture_reference_state);
  session->Set("SymbolFilter", manifest.config.symbol_filter);
  session->Set("OutputFolder", manifest.config.output_folder);
  session->Set("TemplateMask", MakeTemplateMask(manifest.config.enabled_templates));
  session->Set("BaselineSet", manifest.baseline_set);
  session->Set("NextCandidateIndex", static_cast<u64>(manifest.next_candidate_index));
  session->Set("NextTemplateIndex", static_cast<u64>(manifest.next_template_index));
  session->Set("HasLastTestedPosition", manifest.has_last_tested_position);
  session->Set("LastTestedAddress", manifest.last_tested_address);
  session->Set("LastTestedTemplateIndex", static_cast<u64>(manifest.last_tested_template_index));
  session->Set("CandidateCount", static_cast<u64>(manifest.candidates.size()));
  session->Set("ResultCount", static_cast<u64>(manifest.results.size()));

  auto* baseline = ini.GetOrCreateSection(MANIFEST_SECTION_BASELINE);
  baseline->Set("DrawCalls", manifest.baseline.draw_calls);
  baseline->Set("Triangles", manifest.baseline.triangles_drawn);
  baseline->Set("Prims", manifest.baseline.prims);
  baseline->Set("Objects", manifest.baseline.drawn_objects);

  for (size_t i = 0; i < manifest.candidates.size(); ++i)
  {
    auto* section = ini.GetOrCreateSection(MakeSectionName(MANIFEST_PREFIX_CANDIDATE, i));
    section->Set("Name", manifest.candidates[i].symbol_name);
    section->Set("Address", manifest.candidates[i].address);
  }

  for (size_t i = 0; i < manifest.results.size(); ++i)
  {
    const ScanResult& result = manifest.results[i];
    auto* section = ini.GetOrCreateSection(MakeSectionName(MANIFEST_PREFIX_RESULT, i));
    section->Set("Name", result.symbol_name);
    section->Set("Address", result.address);
    section->Set("Template", static_cast<int>(result.template_kind));
    section->Set("BaselineDrawCalls", result.baseline.draw_calls);
    section->Set("BaselineTriangles", result.baseline.triangles_drawn);
    section->Set("BaselinePrims", result.baseline.prims);
    section->Set("BaselineObjects", result.baseline.drawn_objects);
    section->Set("MeasuredDrawCalls", result.measured.draw_calls);
    section->Set("MeasuredTriangles", result.measured.triangles_drawn);
    section->Set("MeasuredPrims", result.measured.prims);
    section->Set("MeasuredObjects", result.measured.drawn_objects);
    section->Set("DrawCallDelta", result.draw_call_delta);
    section->Set("TriangleDelta", result.triangle_delta);
    section->Set("PrimDelta", result.prim_delta);
    section->Set("ObjectDelta", result.object_delta);
    section->Set("Timeout", result.caused_timeout);
    section->Set("ScreenshotPath", result.screenshot_path);
    const auto lines = BuildARCodeLines(result.address, result.template_kind);
    section->Set("ARLine1", lines[0]);
    section->Set("ARLine2", lines[1]);
  }

  File::CreateFullPath(manifest_path);
  if (!ini.Save(manifest_path))
  {
    if (error)
      *error = fmt::format("Failed to save manifest to {}", manifest_path);
    return false;
  }
  return true;
}

bool CullingCodeFinder::LoadManifest(const std::string& manifest_path, ManifestData* manifest,
                                     std::string* error)
{
  Common::IniFile ini;
  if (!ini.Load(manifest_path))
  {
    if (error)
      *error = fmt::format("Failed to load manifest from {}", manifest_path);
    return false;
  }

  const Common::IniFile::Section* session = ini.GetSection(MANIFEST_SECTION_SESSION);
  const Common::IniFile::Section* baseline = ini.GetSection(MANIFEST_SECTION_BASELINE);
  if (!session || !baseline)
  {
    if (error)
      *error = "Manifest is missing required sections.";
    return false;
  }

  ManifestData loaded;
  session->Get("GameId", &loaded.game_id, std::string{});
  session->Get("MapPath", &loaded.map_path, std::string{});
  std::string state_string;
  session->Get("State", &state_string, std::string{"Idle"});
  loaded.state = StringToState(state_string);
  session->Get("SavestateSlot", &loaded.config.savestate_slot, 1);
  session->Get("SettleFrames", &loaded.config.settle_frames, 5);
  session->Get("MinDrawCallIncrease", &loaded.config.min_draw_call_increase, 1);
  session->Get("MinTriangleIncrease", &loaded.config.min_triangle_increase, 0);
  session->Get("MinObjectIncrease", &loaded.config.min_object_increase, 0);
  session->Get("TimeoutSeconds", &loaded.config.timeout_seconds, 3);
  session->Get("CandidateLimit", &loaded.config.candidate_limit, 0);
  session->Get("StartAddress", &loaded.config.start_address, 0u);
  session->Get("EndAddress", &loaded.config.end_address, 0u);
  session->Get("TurboDuringScan", &loaded.config.turbo_during_scan, true);
  session->Get("MuteAudioDuringScan", &loaded.config.mute_audio_during_scan, true);
  session->Get("AutoCaptureReferenceState", &loaded.config.auto_capture_reference_state, true);
  session->Get("SymbolFilter", &loaded.config.symbol_filter, std::string{});
  session->Get("OutputFolder", &loaded.config.output_folder, std::string{});
  std::string template_mask;
  session->Get("TemplateMask", &template_mask, std::string{});
  loaded.config.enabled_templates = ParseTemplateMask(template_mask);
  session->Get("BaselineSet", &loaded.baseline_set, false);
  u64 next_candidate_index = 0;
  u64 next_template_index = 0;
  u64 last_tested_template_index = 0;
  u64 candidate_count = 0;
  u64 result_count = 0;
  session->Get("NextCandidateIndex", &next_candidate_index, 0ull);
  session->Get("NextTemplateIndex", &next_template_index, 0ull);
  session->Get("HasLastTestedPosition", &loaded.has_last_tested_position, false);
  session->Get("LastTestedAddress", &loaded.last_tested_address, 0u);
  session->Get("LastTestedTemplateIndex", &last_tested_template_index, 0ull);
  session->Get("CandidateCount", &candidate_count, 0ull);
  session->Get("ResultCount", &result_count, 0ull);
  loaded.next_candidate_index = static_cast<size_t>(next_candidate_index);
  loaded.next_template_index = static_cast<size_t>(next_template_index);
  loaded.last_tested_template_index = static_cast<size_t>(last_tested_template_index);

  baseline->Get("DrawCalls", &loaded.baseline.draw_calls, 0);
  baseline->Get("Triangles", &loaded.baseline.triangles_drawn, 0);
  baseline->Get("Prims", &loaded.baseline.prims, 0);
  baseline->Get("Objects", &loaded.baseline.drawn_objects, 0);

  loaded.candidates.reserve(static_cast<size_t>(candidate_count));
  for (size_t i = 0; i < static_cast<size_t>(candidate_count); ++i)
  {
    const Common::IniFile::Section* section =
        ini.GetSection(MakeSectionName(MANIFEST_PREFIX_CANDIDATE, i));
    if (!section)
      continue;

    Candidate candidate;
    section->Get("Name", &candidate.symbol_name, std::string{});
    section->Get("Address", &candidate.address, 0u);
    if (!candidate.symbol_name.empty() && candidate.address != 0)
      loaded.candidates.push_back(std::move(candidate));
  }

  loaded.results.reserve(static_cast<size_t>(result_count));
  for (size_t i = 0; i < static_cast<size_t>(result_count); ++i)
  {
    const Common::IniFile::Section* section =
        ini.GetSection(MakeSectionName(MANIFEST_PREFIX_RESULT, i));
    if (!section)
      continue;

    ScanResult result;
    int template_kind = 0;
    std::string ar_line_1;
    std::string ar_line_2;
    section->Get("Name", &result.symbol_name, std::string{});
    section->Get("Address", &result.address, 0u);
    section->Get("Template", &template_kind, 0);
    result.template_kind = static_cast<PatchTemplateKind>(template_kind);
    section->Get("BaselineDrawCalls", &result.baseline.draw_calls, 0);
    section->Get("BaselineTriangles", &result.baseline.triangles_drawn, 0);
    section->Get("BaselinePrims", &result.baseline.prims, 0);
    section->Get("BaselineObjects", &result.baseline.drawn_objects, 0);
    section->Get("MeasuredDrawCalls", &result.measured.draw_calls, 0);
    section->Get("MeasuredTriangles", &result.measured.triangles_drawn, 0);
    section->Get("MeasuredPrims", &result.measured.prims, 0);
    section->Get("MeasuredObjects", &result.measured.drawn_objects, 0);
    section->Get("DrawCallDelta", &result.draw_call_delta, 0);
    section->Get("TriangleDelta", &result.triangle_delta, 0);
    section->Get("PrimDelta", &result.prim_delta, 0);
    section->Get("ObjectDelta", &result.object_delta, 0);
    section->Get("Timeout", &result.caused_timeout, false);
    section->Get("ScreenshotPath", &result.screenshot_path, std::string{});
    section->Get("ARLine1", &ar_line_1, std::string{});
    section->Get("ARLine2", &ar_line_2, std::string{});
    result.ar_code_text = ar_line_1.empty() || ar_line_2.empty() ?
                              BuildARCodeText(result.address, result.template_kind) :
                              fmt::format("{}\n{}", ar_line_1, ar_line_2);
    loaded.results.push_back(std::move(result));
  }

  *manifest = std::move(loaded);
  return true;
}

bool CullingCodeFinder::AdvancePositionAfterCrash(const ScanConfig& config, size_t candidate_count,
                                                  size_t* candidate_index, size_t* template_index)
{
  if (!candidate_index || !template_index)
    return false;

  const auto find_next_enabled_template = [&config](size_t start_index) {
    for (size_t i = start_index; i < TEMPLATE_COUNT; ++i)
    {
      if (config.enabled_templates[i])
        return i;
    }
    return TEMPLATE_COUNT;
  };

  const size_t next_template = find_next_enabled_template(*template_index + 1);
  if (next_template < TEMPLATE_COUNT)
  {
    *template_index = next_template;
    return *candidate_index < candidate_count;
  }

  ++(*candidate_index);
  if (*candidate_index >= candidate_count)
    return false;

  *template_index = find_next_enabled_template(0);
  return *template_index < TEMPLATE_COUNT;
}

bool CullingCodeFinder::BuildCandidatesFromMap(Core::System& system, std::string* error)
{
  Core::CPUThreadGuard guard(system);
  PPCSymbolDB& symbol_db = system.GetPPCSymbolDB();

  std::string writable_map_path;
  if (!PPCSymbolDB::FindMapFile(&m_map_path, &writable_map_path))
  {
    if (error)
      *error = "No symbol map file found for the current title.";
    return false;
  }

  if (!symbol_db.LoadMap(guard, m_map_path))
  {
    if (error)
      *error = fmt::format("Failed to load symbol map {}", m_map_path);
    return false;
  }

  std::vector<CandidateSource> sources;
  symbol_db.ForEachSymbol([&guard, &sources](const Common::Symbol& symbol) {
    CandidateSource source;
    source.symbol_name = symbol.name;
    source.address = symbol.address;
    source.is_function = symbol.type == Common::Symbol::Type::Function;
    source.valid_entry = PowerPC::MMU::HostIsInstructionRAMAddress(guard, symbol.address);
    source.valid_stub = PowerPC::MMU::HostIsInstructionRAMAddress(guard, symbol.address + 4);
    sources.push_back(std::move(source));
  });

  m_candidates =
      BuildCandidatesFromSymbolInfo(std::move(sources), m_scan_config.symbol_filter,
                                    m_scan_config.candidate_limit, m_scan_config.start_address,
                                    m_scan_config.end_address);
  if (m_candidates.empty())
  {
    if (error)
      *error = "No valid function entrypoints were found in the map.";
    return false;
  }

  return true;
}

bool CullingCodeFinder::TryResumeFromManifest(Core::System& system, const ScanConfig& config,
                                              std::string* error)
{
  if (!File::Exists(m_manifest_path))
    return false;

  ManifestData manifest;
  if (!LoadManifest(m_manifest_path, &manifest, error))
    return false;

  if (manifest.game_id != SConfig::GetInstance().GetGameID() ||
      manifest.config.savestate_slot != config.savestate_slot ||
      manifest.config.settle_frames != config.settle_frames ||
      manifest.config.min_draw_call_increase != config.min_draw_call_increase ||
      manifest.config.min_triangle_increase != config.min_triangle_increase ||
      manifest.config.min_object_increase != config.min_object_increase ||
      manifest.config.timeout_seconds != config.timeout_seconds ||
      manifest.config.candidate_limit != config.candidate_limit ||
      manifest.config.start_address != config.start_address ||
      manifest.config.end_address != config.end_address ||
      manifest.config.turbo_during_scan != config.turbo_during_scan ||
      manifest.config.mute_audio_during_scan != config.mute_audio_during_scan ||
      manifest.config.auto_capture_reference_state != config.auto_capture_reference_state ||
      manifest.config.symbol_filter != config.symbol_filter ||
      manifest.config.enabled_templates != config.enabled_templates)
  {
    return false;
  }

  if (manifest.state == ScanState::Finished || manifest.state == ScanState::Idle ||
      manifest.state == ScanState::Stopped)
    return false;

  if (manifest.candidates.empty())
  {
    if (error)
      *error = "Saved manifest has no candidates to resume.";
    return false;
  }

  m_scan_config = manifest.config;
  m_game_id = manifest.game_id;
  m_map_path = manifest.map_path;
  m_baseline = manifest.baseline;
  m_baseline_set = manifest.baseline_set;
  m_candidates = std::move(manifest.candidates);
  m_results = std::move(manifest.results);
  m_candidate_index = manifest.next_candidate_index;
  m_template_index = FindNextEnabledTemplateIndex(manifest.next_template_index);
  m_has_last_tested_position = manifest.has_last_tested_position;
  m_last_tested_address = manifest.last_tested_address;
  m_last_tested_template_index = manifest.last_tested_template_index;

  if (m_candidate_index >= m_candidates.size())
  {
    m_candidate_index = m_candidates.size();
    m_state = ScanState::Finished;
    m_phase = ScanPhase::None;
  }
  else
  {
    m_state = ScanState::Idle;
    m_phase = ScanPhase::RequestLoad;
  }

  return true;
}

bool CullingCodeFinder::StartFreshScan(Core::System& system, const ScanConfig& config,
                                       std::string* error)
{
  m_scan_config = config;
  if (GetEnabledTemplateCount() == 0)
  {
    if (error)
      *error = "Enable at least one patch template.";
    return false;
  }

  if (!BuildCandidatesFromMap(system, error))
  {
    m_state = ScanState::Error;
    UpdateStatusText();
    return false;
  }

  m_results.clear();
  m_candidate_index = 0;
  m_template_index = FindNextEnabledTemplateIndex(0);
  m_has_last_tested_position = false;
  m_last_tested_address = 0;
  m_last_tested_template_index = 0;
  m_phase = ScanPhase::RequestLoad;
  m_state = ScanState::Scanning;
  ApplyRuntimeOverrides(system);

  if (m_scan_config.auto_capture_reference_state && !CaptureReferenceSavestate(system, error))
  {
    RestorePatchedInstructions(system);
    RestoreRuntimeOverrides(system);
    m_state = ScanState::Error;
    m_status_text = (error && !error->empty()) ? *error : "Failed to capture reference savestate.";
    UpdateStatusText();
    return false;
  }

  if (!WriteManifest(error))
  {
    RestorePatchedInstructions(system);
    RestoreRuntimeOverrides(system);
    m_state = ScanState::Error;
    if (error && !error->empty())
      m_status_text = *error;
    UpdateStatusText();
    return false;
  }

  UpdateStatusText();
  return true;
}

bool CullingCodeFinder::WriteManifest(std::string* error) const
{
  if (m_manifest_path.empty())
    return true;

  ManifestData manifest;
  manifest.game_id = m_game_id;
  manifest.map_path = m_map_path;
  manifest.config = m_scan_config;
  manifest.baseline = m_baseline;
  manifest.baseline_set = m_baseline_set;
  manifest.candidates = m_candidates;
  manifest.results = m_results;
  manifest.next_candidate_index = m_candidate_index;
  manifest.next_template_index = m_template_index;
  manifest.has_last_tested_position = m_has_last_tested_position;
  manifest.last_tested_address = m_last_tested_address;
  manifest.last_tested_template_index = m_last_tested_template_index;
  manifest.state = m_state;
  return SaveManifest(m_manifest_path, manifest, error);
}

bool CullingCodeFinder::CaptureReferenceSavestate(Core::System& system, std::string* error)
{
  const std::string slot_path = BuildSavestateFilename(m_game_id, m_scan_config.savestate_slot);
  const bool existed_before = File::Exists(slot_path);
  const auto previous_write_time = GetLastWriteTime(slot_path);

  m_status_text = "Capturing reference savestate...";
  State::Save(system, m_scan_config.savestate_slot,
              {.compress = false, .wait_for_completion = true});

  const bool exists_after = File::Exists(slot_path);
  const auto current_write_time = GetLastWriteTime(slot_path);
  const bool write_time_advanced =
      !previous_write_time.has_value() || !current_write_time.has_value() ||
      current_write_time.value() != previous_write_time.value();

  if (!exists_after || (existed_before && !write_time_advanced))
  {
    if (error)
    {
      *error = fmt::format("Failed to capture the reference savestate into slot {}.",
                           m_scan_config.savestate_slot);
    }
    return false;
  }

  return true;
}

void CullingCodeFinder::UpdateStatusText()
{
  switch (m_state)
  {
  case ScanState::Idle:
    m_status_text = "Idle";
    return;
  case ScanState::Paused:
    m_status_text = fmt::format("Paused at {}/{} candidates", m_candidate_index,
                                m_candidates.size());
    return;
  case ScanState::Stopped:
    m_status_text = fmt::format("Stopped at {}/{} candidates", m_candidate_index,
                                m_candidates.size());
    return;
  case ScanState::Finished:
    m_status_text =
        fmt::format("Scan complete. {} results from {} candidates.", m_results.size(),
                    m_candidates.size());
    return;
  case ScanState::Error:
    if (m_status_text.empty())
      m_status_text = "Scan error";
    return;
  case ScanState::Scanning:
    break;
  }

  const std::string template_name =
      m_candidate_index < m_candidates.size() ? GetPatchTemplateName(TEMPLATE_ORDER[m_template_index]) :
                                                std::string{};
  const std::string symbol_name =
      m_candidate_index < m_candidates.size() ? m_candidates[m_candidate_index].symbol_name :
                                                std::string{};

  switch (m_phase)
  {
  case ScanPhase::RequestLoad:
    m_status_text = fmt::format("Loading savestate for {} ({})", symbol_name, template_name);
    break;
  case ScanPhase::WaitingForLoad:
    m_status_text = fmt::format("Waiting for savestate load for {} ({})", symbol_name,
                                template_name);
    break;
  case ScanPhase::ApplyPatch:
    m_status_text = fmt::format("Applying {} to {}", template_name, symbol_name);
    break;
  case ScanPhase::WaitingForSettle:
    m_status_text = fmt::format("Measuring {} ({})", symbol_name, template_name);
    break;
  case ScanPhase::Finalize:
    m_status_text = fmt::format("Finalizing {} ({})", symbol_name, template_name);
    break;
  case ScanPhase::None:
    m_status_text = "Scanning";
    break;
  }
}

void CullingCodeFinder::RestorePatchedInstructions(Core::System& system)
{
  if (!m_instruction_patched)
    return;

  Core::CPUThreadGuard guard(system);
  PowerPC::MMU::HostWrite<u32>(guard, m_saved_entry_instruction, m_patched_address);
  PowerPC::MMU::HostWrite<u32>(guard, m_saved_next_instruction, m_patched_address + 4);
  system.GetJitInterface().InvalidateICache(m_patched_address, 8, true);
  m_instruction_patched = false;
  m_saved_entry_instruction = 0;
  m_saved_next_instruction = 0;
  m_patched_address = 0;
}

void CullingCodeFinder::ApplyRuntimeOverrides(Core::System& system)
{
  if (m_runtime_overrides_active)
    return;

  if (m_scan_config.turbo_during_scan && !Core::GetIsThrottlerTempDisabled())
  {
    Core::SetIsThrottlerTempDisabled(true);
    m_turbo_override_active = true;
  }

  if (m_scan_config.mute_audio_during_scan && !Config::Get(Config::MAIN_AUDIO_MUTED))
  {
    Config::SetCurrent(Config::MAIN_AUDIO_MUTED, true);
    AudioCommon::UpdateSoundStream(system);
    m_audio_mute_override_active = true;
  }

  m_runtime_overrides_active = true;
}

void CullingCodeFinder::RestoreRuntimeOverrides(Core::System& system)
{
  if (m_turbo_override_active)
  {
    Core::SetIsThrottlerTempDisabled(false);
    m_turbo_override_active = false;
  }

  if (m_audio_mute_override_active &&
      Config::GetActiveLayerForConfig(Config::MAIN_AUDIO_MUTED) == Config::LayerType::CurrentRun)
  {
    Config::DeleteKey(Config::LayerType::CurrentRun, Config::MAIN_AUDIO_MUTED);
    AudioCommon::UpdateSoundStream(system);
  }

  m_audio_mute_override_active = false;
  m_runtime_overrides_active = false;
}

void CullingCodeFinder::AdvanceToNextTemplate()
{
  const size_t next_template = FindNextEnabledTemplateIndex(m_template_index + 1);
  if (next_template < TEMPLATE_COUNT)
  {
    m_template_index = next_template;
    return;
  }

  ++m_candidate_index;
  m_template_index = FindNextEnabledTemplateIndex(0);
}

void CullingCodeFinder::SortResults()
{
  std::ranges::stable_sort(m_results, [](const ScanResult& a, const ScanResult& b) {
    if (a.caused_timeout != b.caused_timeout)
      return !a.caused_timeout;
    if (a.draw_call_delta != b.draw_call_delta)
      return a.draw_call_delta > b.draw_call_delta;
    if (a.object_delta != b.object_delta)
      return a.object_delta > b.object_delta;
    if (a.triangle_delta != b.triangle_delta)
      return a.triangle_delta > b.triangle_delta;
    return a.address < b.address;
  });
}

CullingCodeFinder::FrameStats CullingCodeFinder::AverageSamples() const
{
  FrameStats average{};
  if (m_settle_samples.empty())
    return average;

  for (const FrameStats& sample : m_settle_samples)
  {
    average.draw_calls += sample.draw_calls;
    average.triangles_drawn += sample.triangles_drawn;
    average.prims += sample.prims;
    average.drawn_objects += sample.drawn_objects;
  }

  const int count = static_cast<int>(m_settle_samples.size());
  average.draw_calls /= count;
  average.triangles_drawn /= count;
  average.prims /= count;
  average.drawn_objects /= count;
  return average;
}

bool CullingCodeFinder::ShouldKeepResult(const FrameStats& measured) const
{
  return measured.draw_calls - m_baseline.draw_calls >= m_scan_config.min_draw_call_increase &&
         measured.triangles_drawn - m_baseline.triangles_drawn >=
             m_scan_config.min_triangle_increase &&
         measured.drawn_objects - m_baseline.drawn_objects >= m_scan_config.min_object_increase;
}

std::string CullingCodeFinder::BuildResultScreenshotPath(const Candidate& candidate,
                                                         PatchTemplateKind kind) const
{
  const std::filesystem::path output_dir = StringToPath(m_scan_config.output_folder);
  return PathToString(output_dir / fmt::format("{}.png", ScreenshotBaseName(candidate, kind)));
}

void CullingCodeFinder::AppendResult(ScanResult result)
{
  if (result.ar_code_text.empty())
    result.ar_code_text = BuildARCodeText(result.address, result.template_kind);
  m_results.push_back(std::move(result));
  SortResults();
}

int CullingCodeFinder::GetEnabledTemplateCount() const
{
  return static_cast<int>(std::count(m_scan_config.enabled_templates.begin(),
                                     m_scan_config.enabled_templates.end(), true));
}

size_t CullingCodeFinder::FindNextEnabledTemplateIndex(size_t start_index) const
{
  for (size_t i = start_index; i < TEMPLATE_COUNT; ++i)
  {
    if (m_scan_config.enabled_templates[i])
      return i;
  }

  return TEMPLATE_COUNT;
}

size_t CullingCodeFinder::ComputeCompletedTests() const
{
  size_t completed = 0;
  for (size_t candidate_index = 0; candidate_index < std::min(m_candidate_index, m_candidates.size());
       ++candidate_index)
  {
    completed += static_cast<size_t>(GetEnabledTemplateCount());
  }

  if (m_candidate_index < m_candidates.size())
  {
    for (size_t template_index = 0; template_index < std::min(m_template_index, TEMPLATE_COUNT);
         ++template_index)
    {
      if (m_scan_config.enabled_templates[template_index])
        ++completed;
    }
  }

  return completed;
}

bool CullingCodeFinder::AdvanceScan(Core::System& system)
{
  if (m_state != ScanState::Scanning)
  {
    if (m_instruction_patched)
    {
      RestorePatchedInstructions(system);
      m_phase = ScanPhase::RequestLoad;
    }
    RestoreRuntimeOverrides(system);
    if (m_state == ScanState::Paused || m_state == ScanState::Stopped ||
        m_state == ScanState::Finished || m_state == ScanState::Error)
      WriteManifest();
    UpdateStatusText();
    return false;
  }

  ApplyRuntimeOverrides(system);

  if (m_candidates.empty() || GetEnabledTemplateCount() == 0)
  {
    RestorePatchedInstructions(system);
    RestoreRuntimeOverrides(system);
    m_state = ScanState::Finished;
    WriteManifest();
    UpdateStatusText();
    return false;
  }

  while (m_state == ScanState::Scanning)
  {
    switch (m_phase)
    {
    case ScanPhase::None:
    case ScanPhase::RequestLoad:
      if (m_candidate_index >= m_candidates.size())
      {
        RestorePatchedInstructions(system);
        RestoreRuntimeOverrides(system);
        m_state = ScanState::Finished;
        WriteManifest();
        UpdateStatusText();
        return false;
      }

      m_has_last_tested_position = true;
      m_last_tested_address = m_candidates[m_candidate_index].address;
      m_last_tested_template_index = m_template_index;
      WriteManifest();
      m_load_completed = false;
      m_settle_samples.clear();
      m_last_sampled_frame = m_frame_counter.load(std::memory_order_acquire);
      m_phase_started_at = std::chrono::steady_clock::now();
      State::Load(system, m_scan_config.savestate_slot);
      m_phase = ScanPhase::WaitingForLoad;
      UpdateStatusText();
      return true;

    case ScanPhase::WaitingForLoad:
    {
      if (m_load_completed)
      {
        m_load_completed = false;
        m_phase = ScanPhase::ApplyPatch;
        UpdateStatusText();
        continue;
      }

      const auto elapsed =
          std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() -
                                                           m_phase_started_at)
              .count();
      if (elapsed >= m_scan_config.timeout_seconds)
      {
        ScanResult result;
        const Candidate& candidate = m_candidates[m_candidate_index];
        result.symbol_name = candidate.symbol_name;
        result.address = candidate.address;
        result.template_kind = TEMPLATE_ORDER[m_template_index];
        result.baseline = m_baseline;
        result.caused_timeout = true;
        result.ar_code_text = BuildARCodeText(result.address, result.template_kind);
        AppendResult(std::move(result));
        AdvanceToNextTemplate();
        WriteManifest();
        m_phase = ScanPhase::RequestLoad;
        UpdateStatusText();
        continue;
      }

      UpdateStatusText();
      return true;
    }

    case ScanPhase::ApplyPatch:
    {
      const Candidate& candidate = m_candidates[m_candidate_index];
      const PatchTemplateKind template_kind = TEMPLATE_ORDER[m_template_index];
      const PatchTemplate& patch_template = LookupTemplate(template_kind);

      Core::CPUThreadGuard guard(system);
      const auto entry = PowerPC::MMU::HostTryRead<u32>(guard, candidate.address);
      const auto next = PowerPC::MMU::HostTryRead<u32>(guard, candidate.address + 4);
      if (!entry.has_value() || !next.has_value())
      {
        ScanResult result;
        result.symbol_name = candidate.symbol_name;
        result.address = candidate.address;
        result.template_kind = template_kind;
        result.baseline = m_baseline;
        result.caused_timeout = true;
        result.ar_code_text = BuildARCodeText(result.address, result.template_kind);
        AppendResult(std::move(result));
        AdvanceToNextTemplate();
        WriteManifest();
        m_phase = ScanPhase::RequestLoad;
        UpdateStatusText();
        continue;
      }

      m_saved_entry_instruction = entry->value;
      m_saved_next_instruction = next->value;
      m_patched_address = candidate.address;
      m_active_template = template_kind;
      PowerPC::MMU::HostWrite<u32>(guard, patch_template.first_instruction, candidate.address);
      PowerPC::MMU::HostWrite<u32>(guard, BLR_INSTRUCTION, candidate.address + 4);
      system.GetJitInterface().InvalidateICache(candidate.address, 8, true);
      m_instruction_patched = true;

      m_settle_samples.clear();
      m_settle_frame_start = m_frame_counter.load(std::memory_order_acquire);
      m_last_sampled_frame = m_settle_frame_start;
      m_phase_started_at = std::chrono::steady_clock::now();
      m_phase = ScanPhase::WaitingForSettle;
      UpdateStatusText();
      return true;
    }

    case ScanPhase::WaitingForSettle:
    {
      const int current_frame = m_frame_counter.load(std::memory_order_acquire);
      if (current_frame > m_last_sampled_frame)
      {
        for (int frame = m_last_sampled_frame + 1; frame <= current_frame; ++frame)
        {
          if (frame > m_settle_frame_start)
            m_settle_samples.push_back(GetLatestStats());
        }
        m_last_sampled_frame = current_frame;
      }

      if (static_cast<int>(m_settle_samples.size()) >= m_scan_config.settle_frames)
      {
        m_phase = ScanPhase::Finalize;
        UpdateStatusText();
        continue;
      }

      const auto elapsed =
          std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() -
                                                           m_phase_started_at)
              .count();
      if (elapsed >= m_scan_config.timeout_seconds)
      {
        m_phase = ScanPhase::Finalize;
        UpdateStatusText();
        continue;
      }

      UpdateStatusText();
      return true;
    }

    case ScanPhase::Finalize:
    {
      const Candidate& candidate = m_candidates[m_candidate_index];
      const PatchTemplateKind template_kind = TEMPLATE_ORDER[m_template_index];
      ScanResult result;
      result.symbol_name = candidate.symbol_name;
      result.address = candidate.address;
      result.template_kind = template_kind;
      result.baseline = m_baseline;
      result.ar_code_text = BuildARCodeText(result.address, result.template_kind);

      const bool timed_out =
          static_cast<int>(m_settle_samples.size()) < m_scan_config.settle_frames;
      if (timed_out)
      {
        result.caused_timeout = true;
        AppendResult(std::move(result));
      }
      else
      {
        result.measured = AverageSamples();
        result.draw_call_delta = result.measured.draw_calls - m_baseline.draw_calls;
        result.triangle_delta = result.measured.triangles_drawn - m_baseline.triangles_drawn;
        result.prim_delta = result.measured.prims - m_baseline.prims;
        result.object_delta = result.measured.drawn_objects - m_baseline.drawn_objects;

        if (ShouldKeepResult(result.measured))
        {
          const std::string screenshot_path = BuildResultScreenshotPath(candidate, template_kind);
          if (g_frame_dumper)
          {
            File::CreateFullPath(screenshot_path);
            g_frame_dumper->SaveScreenshot(screenshot_path);
            result.screenshot_path = screenshot_path;
          }
          AppendResult(std::move(result));
        }
      }

      m_settle_samples.clear();
      AdvanceToNextTemplate();
      if (m_candidate_index >= m_candidates.size())
      {
        RestorePatchedInstructions(system);
        RestoreRuntimeOverrides(system);
        m_state = ScanState::Finished;
        m_phase = ScanPhase::None;
        m_has_last_tested_position = false;
        m_last_tested_address = 0;
        m_last_tested_template_index = 0;
        WriteManifest();
        UpdateStatusText();
        return false;
      }

      m_phase = ScanPhase::RequestLoad;
      WriteManifest();
      UpdateStatusText();
      continue;
    }
    }
  }

  return false;
}
