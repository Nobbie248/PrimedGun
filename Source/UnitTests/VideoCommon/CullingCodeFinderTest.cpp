// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Core/State.h"
#include "VideoCommon/CullingCodeFinder.h"

TEST(CullingCodeFinder, BuildCandidatesFromSymbolInfoFiltersSortsAndLimits)
{
  std::vector<CullingCodeFinder::CandidateSource> sources = {
      {"SkipNotFunction", 0x80003000, false, true, true},
      {"SkipInvalidEntry", 0x80003010, true, false, true},
      {"SkipInvalidStub", 0x80003020, true, true, false},
      {"SkipBeforeStart", 0x80003030, true, true, true},
      {"RenderSky", 0x80003120, true, true, true},
      {"renderAlpha", 0x80003080, true, true, true},
      {"RenderBeta", 0x80003040, true, true, true},
  };

  const auto filtered =
      CullingCodeFinder::BuildCandidatesFromSymbolInfo(sources, "render", 2, 0x80003040,
                                                       0x80003100);

  ASSERT_EQ(filtered.size(), 2u);
  EXPECT_EQ(filtered[0].symbol_name, "RenderBeta");
  EXPECT_EQ(filtered[0].address, 0x80003040u);
  EXPECT_EQ(filtered[1].symbol_name, "renderAlpha");
  EXPECT_EQ(filtered[1].address, 0x80003080u);
}

TEST(CullingCodeFinder, BuildARCodeLinesMatchesKnownHydraPatterns)
{
  const auto i_ninja =
      CullingCodeFinder::BuildARCodeLines(0x80052FE8, CullingCodeFinder::PatchTemplateKind::OriR0True);
  EXPECT_EQ(i_ninja[0], "04052FE8 60000001");
  EXPECT_EQ(i_ninja[1], "04052FEC 4E800020");

  const auto metroid =
      CullingCodeFinder::BuildARCodeLines(0x80345C68, CullingCodeFinder::PatchTemplateKind::LiR3True);
  EXPECT_EQ(metroid[0], "04345C68 38600001");
  EXPECT_EQ(metroid[1], "04345C6C 4E800020");
  EXPECT_EQ(CullingCodeFinder::BuildARCodeText(0x80345C68,
                                               CullingCodeFinder::PatchTemplateKind::LiR3True),
            "04345C68 38600001\n04345C6C 4E800020");
}

TEST(CullingCodeFinder, AdvancePositionAfterCrashSkipsToNextPendingTest)
{
  CullingCodeFinder::ScanConfig config;
  config.enabled_templates = {true, false, true, true};

  size_t candidate_index = 0;
  size_t template_index = 2;
  ASSERT_TRUE(
      CullingCodeFinder::AdvancePositionAfterCrash(config, 2, &candidate_index, &template_index));
  EXPECT_EQ(candidate_index, 0u);
  EXPECT_EQ(template_index, 3u);

  candidate_index = 0;
  template_index = 3;
  ASSERT_TRUE(
      CullingCodeFinder::AdvancePositionAfterCrash(config, 2, &candidate_index, &template_index));
  EXPECT_EQ(candidate_index, 1u);
  EXPECT_EQ(template_index, 0u);

  candidate_index = 1;
  template_index = 3;
  EXPECT_FALSE(
      CullingCodeFinder::AdvancePositionAfterCrash(config, 2, &candidate_index, &template_index));
}

TEST(CullingCodeFinder, ManifestRoundTripPreservesResumeState)
{
  const std::string temp_dir = File::CreateTempDir();
  ASSERT_FALSE(temp_dir.empty());

  const std::string manifest_path = temp_dir + DIR_SEP "manifest.ini";

  CullingCodeFinder::ManifestData manifest;
  manifest.game_id = "GM8E01";
  manifest.map_path = temp_dir + DIR_SEP "GM8E01.map";
  manifest.state = CullingCodeFinder::ScanState::Paused;
  manifest.config.savestate_slot = 2;
  manifest.config.settle_frames = 7;
  manifest.config.min_draw_call_increase = 3;
  manifest.config.min_triangle_increase = 11;
  manifest.config.min_object_increase = 2;
  manifest.config.timeout_seconds = 9;
  manifest.config.candidate_limit = 15;
  manifest.config.start_address = 0x80005200;
  manifest.config.end_address = 0x80005300;
  manifest.config.turbo_during_scan = false;
  manifest.config.mute_audio_during_scan = false;
  manifest.config.auto_capture_reference_state = false;
  manifest.config.symbol_filter = "Cull";
  manifest.config.output_folder = temp_dir + DIR_SEP "shots";
  manifest.config.enabled_templates = {true, false, true, false};
  manifest.baseline = {10, 20, 30, 40};
  manifest.baseline_set = true;
  manifest.candidates = {{"CheckCull", 0x80001000}, {"RenderCull", 0x80002000}};
  manifest.next_candidate_index = 1;
  manifest.next_template_index = 2;
  manifest.has_last_tested_position = true;
  manifest.last_tested_address = 0x80002000;
  manifest.last_tested_template_index = 2;

  CullingCodeFinder::ScanResult result;
  result.symbol_name = "RenderCull";
  result.address = 0x80345C68;
  result.template_kind = CullingCodeFinder::PatchTemplateKind::LiR3True;
  result.baseline = {10, 20, 30, 40};
  result.measured = {15, 35, 45, 50};
  result.draw_call_delta = 5;
  result.triangle_delta = 15;
  result.prim_delta = 15;
  result.object_delta = 10;
  result.caused_timeout = false;
  result.screenshot_path = temp_dir + DIR_SEP "shots" DIR_SEP "04345C68_li.png";
  result.ar_code_text = "04345C68 38600001\n04345C6C 4E800020";
  manifest.results.push_back(result);

  std::string error;
  ASSERT_TRUE(CullingCodeFinder::SaveManifest(manifest_path, manifest, &error)) << error;

  CullingCodeFinder::ManifestData loaded;
  ASSERT_TRUE(CullingCodeFinder::LoadManifest(manifest_path, &loaded, &error)) << error;

  EXPECT_EQ(loaded.game_id, manifest.game_id);
  EXPECT_EQ(loaded.map_path, manifest.map_path);
  EXPECT_EQ(loaded.state, manifest.state);
  EXPECT_EQ(loaded.config.savestate_slot, manifest.config.savestate_slot);
  EXPECT_EQ(loaded.config.settle_frames, manifest.config.settle_frames);
  EXPECT_EQ(loaded.config.min_draw_call_increase, manifest.config.min_draw_call_increase);
  EXPECT_EQ(loaded.config.min_triangle_increase, manifest.config.min_triangle_increase);
  EXPECT_EQ(loaded.config.min_object_increase, manifest.config.min_object_increase);
  EXPECT_EQ(loaded.config.timeout_seconds, manifest.config.timeout_seconds);
  EXPECT_EQ(loaded.config.candidate_limit, manifest.config.candidate_limit);
  EXPECT_EQ(loaded.config.start_address, manifest.config.start_address);
  EXPECT_EQ(loaded.config.end_address, manifest.config.end_address);
  EXPECT_EQ(loaded.config.turbo_during_scan, manifest.config.turbo_during_scan);
  EXPECT_EQ(loaded.config.mute_audio_during_scan, manifest.config.mute_audio_during_scan);
  EXPECT_EQ(loaded.config.auto_capture_reference_state,
            manifest.config.auto_capture_reference_state);
  EXPECT_EQ(loaded.config.symbol_filter, manifest.config.symbol_filter);
  EXPECT_EQ(loaded.config.output_folder, manifest.config.output_folder);
  EXPECT_EQ(loaded.config.enabled_templates, manifest.config.enabled_templates);
  EXPECT_EQ(loaded.baseline.draw_calls, manifest.baseline.draw_calls);
  EXPECT_EQ(loaded.baseline.triangles_drawn, manifest.baseline.triangles_drawn);
  EXPECT_EQ(loaded.baseline.prims, manifest.baseline.prims);
  EXPECT_EQ(loaded.baseline.drawn_objects, manifest.baseline.drawn_objects);
  EXPECT_TRUE(loaded.baseline_set);
  EXPECT_EQ(loaded.next_candidate_index, 1u);
  EXPECT_EQ(loaded.next_template_index, 2u);
  EXPECT_TRUE(loaded.has_last_tested_position);
  EXPECT_EQ(loaded.last_tested_address, 0x80002000u);
  EXPECT_EQ(loaded.last_tested_template_index, 2u);
  ASSERT_EQ(loaded.candidates.size(), 2u);
  EXPECT_EQ(loaded.candidates[1].symbol_name, "RenderCull");
  EXPECT_EQ(loaded.candidates[1].address, 0x80002000u);
  ASSERT_EQ(loaded.results.size(), 1u);
  EXPECT_EQ(loaded.results[0].symbol_name, result.symbol_name);
  EXPECT_EQ(loaded.results[0].address, result.address);
  EXPECT_EQ(loaded.results[0].template_kind, result.template_kind);
  EXPECT_EQ(loaded.results[0].draw_call_delta, result.draw_call_delta);
  EXPECT_EQ(loaded.results[0].triangle_delta, result.triangle_delta);
  EXPECT_EQ(loaded.results[0].object_delta, result.object_delta);
  EXPECT_FALSE(loaded.results[0].caused_timeout);
  EXPECT_EQ(loaded.results[0].screenshot_path, result.screenshot_path);
  EXPECT_EQ(loaded.results[0].ar_code_text, result.ar_code_text);

  File::DeleteDirRecursively(temp_dir);
}

TEST(CullingCodeFinder, ManifestRoundTripPreservesLastTestedPositionWithoutResults)
{
  const std::string temp_dir = File::CreateTempDir();
  ASSERT_FALSE(temp_dir.empty());

  const std::string manifest_path = temp_dir + DIR_SEP "manifest.ini";

  CullingCodeFinder::ManifestData manifest;
  manifest.game_id = "GNJEAF";
  manifest.map_path = temp_dir + DIR_SEP "GNJEAF.map";
  manifest.state = CullingCodeFinder::ScanState::Scanning;
  manifest.config.output_folder = temp_dir;
  manifest.config.enabled_templates = {true, true, true, true};
  manifest.baseline = {23, 0, 0, 0};
  manifest.baseline_set = true;
  manifest.candidates = {{"zz_80052fe8_", 0x80052FE8}, {"zz_80053010_", 0x80053010}};
  manifest.next_candidate_index = 0;
  manifest.next_template_index = 2;
  manifest.has_last_tested_position = true;
  manifest.last_tested_address = 0x80052FE8;
  manifest.last_tested_template_index = 2;

  std::string error;
  ASSERT_TRUE(CullingCodeFinder::SaveManifest(manifest_path, manifest, &error)) << error;

  CullingCodeFinder::ManifestData loaded;
  ASSERT_TRUE(CullingCodeFinder::LoadManifest(manifest_path, &loaded, &error)) << error;

  EXPECT_TRUE(File::Exists(manifest_path));
  EXPECT_TRUE(loaded.results.empty());
  EXPECT_EQ(loaded.next_candidate_index, 0u);
  EXPECT_EQ(loaded.next_template_index, 2u);
  EXPECT_TRUE(loaded.has_last_tested_position);
  EXPECT_EQ(loaded.last_tested_address, 0x80052FE8u);
  EXPECT_EQ(loaded.last_tested_template_index, 2u);
  ASSERT_EQ(loaded.candidates.size(), 2u);
  EXPECT_EQ(loaded.candidates[0].address, 0x80052FE8u);

  File::DeleteDirRecursively(temp_dir);
}

TEST(CullingCodeFinder, ManifestRoundTripPreservesStoppedState)
{
  const std::string temp_dir = File::CreateTempDir();
  ASSERT_FALSE(temp_dir.empty());

  const std::string manifest_path = temp_dir + DIR_SEP "manifest.ini";

  CullingCodeFinder::ManifestData manifest;
  manifest.game_id = "GNJEAF";
  manifest.state = CullingCodeFinder::ScanState::Stopped;
  manifest.config.output_folder = temp_dir;
  manifest.baseline_set = true;
  manifest.candidates = {{"zz_80052fe8_", 0x80052FE8}};
  manifest.next_candidate_index = 0;
  manifest.next_template_index = 2;
  manifest.has_last_tested_position = true;
  manifest.last_tested_address = 0x80052FE8;
  manifest.last_tested_template_index = 2;

  std::string error;
  ASSERT_TRUE(CullingCodeFinder::SaveManifest(manifest_path, manifest, &error)) << error;

  CullingCodeFinder::ManifestData loaded;
  ASSERT_TRUE(CullingCodeFinder::LoadManifest(manifest_path, &loaded, &error)) << error;

  EXPECT_EQ(loaded.state, CullingCodeFinder::ScanState::Stopped);
  EXPECT_TRUE(loaded.has_last_tested_position);
  EXPECT_EQ(loaded.last_tested_address, 0x80052FE8u);
  EXPECT_EQ(loaded.last_tested_template_index, 2u);

  File::DeleteDirRecursively(temp_dir);
}

TEST(State, SaveOptionsDefaultsMatchExpectedCompressionBehavior)
{
  State::SaveOptions default_options;
  EXPECT_TRUE(default_options.compress);
  EXPECT_FALSE(default_options.wait_for_completion);

  State::SaveOptions capture_options;
  capture_options.compress = false;
  capture_options.wait_for_completion = true;
  EXPECT_FALSE(capture_options.compress);
  EXPECT_TRUE(capture_options.wait_for_completion);
}
