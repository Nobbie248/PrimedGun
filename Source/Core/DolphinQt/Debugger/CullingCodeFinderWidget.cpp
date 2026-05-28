// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Debugger/CullingCodeFinderWidget.h"

#include <algorithm>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <filesystem>

#include <fmt/format.h>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Core/ActionReplay.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/System.h"
#include "DolphinQt/Host.h"
#include "VideoCommon/CullingCodeFinder.h"

namespace
{
u32 ParseOptionalHexField(const QString& text, bool* ok)
{
  QString parsed_text = text.trimmed();
  if (parsed_text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
    parsed_text.remove(0, 2);

  *ok = parsed_text.isEmpty();
  return parsed_text.isEmpty() ? 0u : parsed_text.toUInt(ok, 16);
}

QString PathLeaf(QString path)
{
  return QFileInfo(path).fileName();
}

QString PathLeaf(const std::string& path)
{
  return QFileInfo(QString::fromStdString(path)).fileName();
}
}  // namespace

CullingCodeFinderWidget::CullingCodeFinderWidget(QWidget* parent) : QDialog(parent)
{
  setWindowTitle(tr("Culling Code Finder"));
  setMinimumWidth(840);

  CreateWidgets();
  ConnectSignals();
  RefreshStaticInfo();
  UpdateButtonStates();

  m_update_timer = new QTimer(this);
  connect(m_update_timer, &QTimer::timeout, this, &CullingCodeFinderWidget::UpdateDisplay);
  m_update_timer->start(16);
}

void CullingCodeFinderWidget::closeEvent(QCloseEvent* event)
{
  auto& finder = CullingCodeFinder::GetInstance();
  if (finder.GetState() == CullingCodeFinder::ScanState::Scanning ||
      finder.GetState() == CullingCodeFinder::ScanState::Paused)
  {
    finder.StopScan();
    finder.AdvanceScan(Core::System::GetInstance());
  }

  m_update_timer->stop();
  event->accept();
}

void CullingCodeFinderWidget::CreateWidgets()
{
  auto* main_layout = new QVBoxLayout(this);

  auto* stats_group = new QGroupBox(tr("Live Statistics"));
  auto* stats_layout = new QHBoxLayout;
  stats_group->setLayout(stats_layout);

  stats_layout->addWidget(new QLabel(tr("Draw Calls:")));
  m_live_draw_calls = new QLabel(QStringLiteral("0"));
  stats_layout->addWidget(m_live_draw_calls);
  stats_layout->addWidget(new QLabel(tr("Triangles:")));
  m_live_triangles = new QLabel(QStringLiteral("0"));
  stats_layout->addWidget(m_live_triangles);
  stats_layout->addWidget(new QLabel(tr("Prims:")));
  m_live_prims = new QLabel(QStringLiteral("0"));
  stats_layout->addWidget(m_live_prims);
  stats_layout->addWidget(new QLabel(tr("Objects:")));
  m_live_objects = new QLabel(QStringLiteral("0"));
  stats_layout->addWidget(m_live_objects);
  stats_layout->addStretch();
  main_layout->addWidget(stats_group);

  auto* baseline_group = new QGroupBox(tr("Baseline"));
  auto* baseline_layout = new QHBoxLayout;
  baseline_group->setLayout(baseline_layout);
  m_baseline_label = new QLabel(tr("Not set"));
  baseline_layout->addWidget(m_baseline_label);
  baseline_layout->addStretch();
  m_set_baseline_btn = new QPushButton(tr("Set Baseline"));
  baseline_layout->addWidget(m_set_baseline_btn);
  main_layout->addWidget(baseline_group);

  auto* prerequisite_group = new QGroupBox(tr("Prerequisites"));
  auto* prerequisite_layout = new QVBoxLayout;
  prerequisite_group->setLayout(prerequisite_layout);

  auto* map_layout = new QHBoxLayout;
  map_layout->addWidget(new QLabel(tr("Map File:")));
  m_map_path_label = new QLabel(tr("Not found"));
  m_map_path_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  map_layout->addWidget(m_map_path_label, 1);
  m_generate_map_btn = new QPushButton(tr("Generate .map"));
  map_layout->addWidget(m_generate_map_btn);
  prerequisite_layout->addLayout(map_layout);

  auto* output_layout = new QHBoxLayout;
  output_layout->addWidget(new QLabel(tr("Output Folder:")));
  m_output_folder_edit = new QLineEdit;
  output_layout->addWidget(m_output_folder_edit, 1);
  m_browse_output_btn = new QPushButton(tr("Browse"));
  m_open_output_btn = new QPushButton(tr("Open"));
  output_layout->addWidget(m_browse_output_btn);
  output_layout->addWidget(m_open_output_btn);
  prerequisite_layout->addLayout(output_layout);
  main_layout->addWidget(prerequisite_group);

  auto* config_group = new QGroupBox(tr("Scan Configuration"));
  auto* config_layout = new QVBoxLayout;
  config_group->setLayout(config_layout);

  auto* top_row = new QHBoxLayout;
  top_row->addWidget(new QLabel(tr("Savestate Slot:")));
  m_savestate_slot_spin = new QSpinBox;
  m_savestate_slot_spin->setRange(1, 10);
  m_savestate_slot_spin->setValue(1);
  top_row->addWidget(m_savestate_slot_spin);
  top_row->addWidget(new QLabel(tr("Settle Frames:")));
  m_settle_frames_spin = new QSpinBox;
  m_settle_frames_spin->setRange(1, 60);
  m_settle_frames_spin->setValue(5);
  top_row->addWidget(m_settle_frames_spin);
  top_row->addWidget(new QLabel(tr("Candidate Limit:")));
  m_candidate_limit_spin = new QSpinBox;
  m_candidate_limit_spin->setRange(0, 1000000);
  m_candidate_limit_spin->setValue(0);
  m_candidate_limit_spin->setSpecialValueText(tr("Unlimited"));
  top_row->addWidget(m_candidate_limit_spin);
  top_row->addStretch();
  config_layout->addLayout(top_row);

  m_settle_frames_help = new QLabel(
      tr("Settle Frames are emulated frames after each savestate reload. Turbo only reduces wall-"
         "clock scan time."));
  m_settle_frames_help->setWordWrap(true);
  config_layout->addWidget(m_settle_frames_help);

  auto* threshold_row = new QHBoxLayout;
  threshold_row->addWidget(new QLabel(tr("Min Draw Calls +:")));
  m_min_draw_call_spin = new QSpinBox;
  m_min_draw_call_spin->setRange(1, 1000000);
  m_min_draw_call_spin->setValue(1);
  m_min_draw_call_spin->setToolTip(
      tr("Minimum increase in draw calls versus the baseline required to keep a result."));
  threshold_row->addWidget(m_min_draw_call_spin);
  threshold_row->addWidget(new QLabel(tr("Min Triangles +:")));
  m_min_triangle_spin = new QSpinBox;
  m_min_triangle_spin->setRange(0, 100000000);
  m_min_triangle_spin->setValue(0);
  m_min_triangle_spin->setToolTip(
      tr("Minimum increase in drawn triangles versus the baseline required to keep a result."));
  threshold_row->addWidget(m_min_triangle_spin);
  threshold_row->addWidget(new QLabel(tr("Min Objects +:")));
  m_min_object_spin = new QSpinBox;
  m_min_object_spin->setRange(0, 1000000);
  m_min_object_spin->setValue(0);
  m_min_object_spin->setToolTip(
      tr("Minimum increase in drawn objects versus the baseline required to keep a result."));
  threshold_row->addWidget(m_min_object_spin);
  threshold_row->addStretch();
  config_layout->addLayout(threshold_row);

  auto* filter_row = new QHBoxLayout;
  filter_row->addWidget(new QLabel(tr("Start Address:")));
  m_start_address_edit = new QLineEdit;
  m_start_address_edit->setPlaceholderText(QStringLiteral("0 or 80052FE8"));
  filter_row->addWidget(m_start_address_edit);
  filter_row->addWidget(new QLabel(tr("End Address:")));
  m_end_address_edit = new QLineEdit;
  m_end_address_edit->setPlaceholderText(QStringLiteral("0 or 80053000"));
  filter_row->addWidget(m_end_address_edit);
  filter_row->addWidget(new QLabel(tr("Symbol Filter:")));
  m_symbol_filter_edit = new QLineEdit;
  filter_row->addWidget(m_symbol_filter_edit, 1);
  config_layout->addLayout(filter_row);

  auto* runtime_row = new QHBoxLayout;
  m_turbo_during_scan_check = new QCheckBox(tr("Turbo During Scan"));
  m_turbo_during_scan_check->setChecked(true);
  runtime_row->addWidget(m_turbo_during_scan_check);
  m_mute_audio_during_scan_check = new QCheckBox(tr("Mute Audio During Scan"));
  m_mute_audio_during_scan_check->setChecked(true);
  runtime_row->addWidget(m_mute_audio_during_scan_check);
  m_auto_capture_reference_state_check = new QCheckBox(tr("Auto-Capture Reference State"));
  m_auto_capture_reference_state_check->setChecked(true);
  runtime_row->addWidget(m_auto_capture_reference_state_check);
  runtime_row->addStretch();
  config_layout->addLayout(runtime_row);

  auto* template_row = new QHBoxLayout;
  constexpr std::array<const char*, 4> labels = {"38600001 + BLR", "38600000 + BLR",
                                                  "60000001 + BLR", "60000000 + BLR"};
  for (size_t i = 0; i < labels.size(); ++i)
  {
    m_template_checks[i] = new QCheckBox(QString::fromLatin1(labels[i]));
    m_template_checks[i]->setChecked(true);
    template_row->addWidget(m_template_checks[i]);
  }
  template_row->addStretch();
  config_layout->addLayout(template_row);
  main_layout->addWidget(config_group);

  auto* controls_layout = new QHBoxLayout;
  m_start_btn = new QPushButton(tr("Start Scan"));
  m_restore_btn = new QPushButton(tr("Restore Session"));
  m_pause_btn = new QPushButton(tr("Pause"));
  m_stop_btn = new QPushButton(tr("Stop"));
  controls_layout->addWidget(m_start_btn);
  controls_layout->addWidget(m_restore_btn);
  controls_layout->addWidget(m_pause_btn);
  controls_layout->addWidget(m_stop_btn);
  controls_layout->addStretch();
  main_layout->addLayout(controls_layout);

  m_progress_bar = new QProgressBar;
  m_progress_bar->setRange(0, 100);
  m_progress_bar->setValue(0);
  main_layout->addWidget(m_progress_bar);

  m_status_label = new QLabel(tr("Idle"));
  main_layout->addWidget(m_status_label);

  auto* results_group = new QGroupBox(tr("Results"));
  auto* results_layout = new QVBoxLayout;
  results_group->setLayout(results_layout);

  m_results_table = new QTableWidget(0, 8);
  m_results_table->setHorizontalHeaderLabels(
      {tr("Symbol"), tr("Address"), tr("Template"), tr("DC Delta"), tr("Tri Delta"),
       tr("Obj Delta"), tr("Timeout"), tr("Screenshot")});
  m_results_table->setSelectionBehavior(QTableWidget::SelectRows);
  m_results_table->setSelectionMode(QTableWidget::ExtendedSelection);
  m_results_table->setEditTriggers(QTableWidget::NoEditTriggers);
  m_results_table->horizontalHeader()->setStretchLastSection(true);
  m_results_table->verticalHeader()->setVisible(false);
  results_layout->addWidget(m_results_table);

  m_show_timeout_results_check = new QCheckBox(tr("Show Timeout Results"));
  m_show_timeout_results_check->setChecked(false);
  results_layout->addWidget(m_show_timeout_results_check);

  auto* result_buttons = new QHBoxLayout;
  m_generate_ar_btn = new QPushButton(tr("Generate AR Code"));
  m_reset_results_btn = new QPushButton(tr("Reset Results"));
  result_buttons->addWidget(m_generate_ar_btn);
  result_buttons->addWidget(m_reset_results_btn);
  result_buttons->addStretch();
  results_layout->addLayout(result_buttons);
  main_layout->addWidget(results_group);
}

void CullingCodeFinderWidget::ConnectSignals()
{
  connect(m_set_baseline_btn, &QPushButton::clicked, this,
          &CullingCodeFinderWidget::OnSetBaseline);
  connect(m_generate_map_btn, &QPushButton::clicked, this, &CullingCodeFinderWidget::OnGenerateMap);
  connect(m_start_btn, &QPushButton::clicked, this, &CullingCodeFinderWidget::OnStartScan);
  connect(m_restore_btn, &QPushButton::clicked, this, &CullingCodeFinderWidget::OnRestoreSession);
  connect(m_pause_btn, &QPushButton::clicked, this, &CullingCodeFinderWidget::OnPauseScan);
  connect(m_stop_btn, &QPushButton::clicked, this, &CullingCodeFinderWidget::OnStopScan);
  connect(m_generate_ar_btn, &QPushButton::clicked, this,
          &CullingCodeFinderWidget::OnGenerateARCode);
  connect(m_reset_results_btn, &QPushButton::clicked, this,
          &CullingCodeFinderWidget::OnResetResults);
  connect(m_show_timeout_results_check, &QCheckBox::toggled, this, [this] {
    UpdateResultsTable();
    UpdateButtonStates();
  });
  connect(m_browse_output_btn, &QPushButton::clicked, this,
          &CullingCodeFinderWidget::OnBrowseOutputFolder);
  connect(m_open_output_btn, &QPushButton::clicked, this,
          &CullingCodeFinderWidget::OnOpenOutputFolder);
}

void CullingCodeFinderWidget::RefreshStaticInfo()
{
  const std::string game_id = SConfig::GetInstance().GetGameID();
  std::string map_path;
  PPCSymbolDB::FindMapFile(&map_path, nullptr);
  if (map_path.empty())
  {
    const std::string finder_map_path = CullingCodeFinder::GetInstance().GetMapPath();
    const std::string expected_suffix = game_id + ".map";
    if (!finder_map_path.empty() && !game_id.empty() && finder_map_path.ends_with(expected_suffix))
      map_path = finder_map_path;
  }

  m_map_path_label->setText(map_path.empty() ? tr("Not found") : QString::fromStdString(map_path));

  if (m_output_folder_edit->text().isEmpty() && !game_id.empty())
  {
    m_output_folder_edit->setText(
        QString::fromStdString(CullingCodeFinder::GetDefaultOutputFolder(game_id)));
  }
}

void CullingCodeFinderWidget::UpdateDisplay()
{
  auto& finder = CullingCodeFinder::GetInstance();
  const auto stats = finder.GetLatestStats();
  m_live_draw_calls->setText(QString::number(stats.draw_calls));
  m_live_triangles->setText(QString::number(stats.triangles_drawn));
  m_live_prims->setText(QString::number(stats.prims));
  m_live_objects->setText(QString::number(stats.drawn_objects));

  if (finder.GetState() == CullingCodeFinder::ScanState::Scanning)
    finder.AdvanceScan(Core::System::GetInstance());

  const auto baseline = finder.GetBaseline();
  m_baseline_label->setText(finder.HasBaseline() ?
                                tr("DC: %1  |  Tri: %2  |  Prims: %3  |  Obj: %4")
                                    .arg(baseline.draw_calls)
                                    .arg(baseline.triangles_drawn)
                                    .arg(baseline.prims)
                                    .arg(baseline.drawn_objects) :
                                tr("Not set"));

  m_progress_bar->setValue(static_cast<int>(finder.GetProgress() * 100.0f));
  m_status_label->setText(QString::fromStdString(finder.GetStatusText()));

  const int result_count = finder.GetResultCount();
  if (result_count != m_last_result_count)
  {
    UpdateResultsTable();
    m_last_result_count = result_count;
  }

  RefreshStaticInfo();
  UpdateButtonStates();
}

void CullingCodeFinderWidget::OnSetBaseline()
{
  auto& finder = CullingCodeFinder::GetInstance();
  finder.SetBaseline();
  UpdateDisplay();
}

void CullingCodeFinderWidget::OnStartScan()
{
  CullingCodeFinder::ScanConfig config;
  bool start_address_ok = false;
  bool end_address_ok = false;
  const u32 parsed_start_address =
      ParseOptionalHexField(m_start_address_edit->text(), &start_address_ok);
  const u32 parsed_end_address = ParseOptionalHexField(m_end_address_edit->text(), &end_address_ok);
  if (!start_address_ok)
  {
    QMessageBox::warning(this, tr("Invalid Start Address"),
                         tr("Enter the start address as hexadecimal, for example 80052FE8."));
    return;
  }
  if (!end_address_ok)
  {
    QMessageBox::warning(this, tr("Invalid End Address"),
                         tr("Enter the end address as hexadecimal, for example 80053000."));
    return;
  }
  if (parsed_start_address != 0 && parsed_end_address != 0 &&
      parsed_start_address > parsed_end_address)
  {
    QMessageBox::warning(this, tr("Invalid Address Range"),
                         tr("The start address must be lower than or equal to the end address."));
    return;
  }

  config.savestate_slot = m_savestate_slot_spin->value();
  config.settle_frames = m_settle_frames_spin->value();
  config.min_draw_call_increase = m_min_draw_call_spin->value();
  config.min_triangle_increase = m_min_triangle_spin->value();
  config.min_object_increase = m_min_object_spin->value();
  config.candidate_limit = m_candidate_limit_spin->value();
  config.start_address = parsed_start_address;
  config.end_address = parsed_end_address;
  config.turbo_during_scan = m_turbo_during_scan_check->isChecked();
  config.mute_audio_during_scan = m_mute_audio_during_scan_check->isChecked();
  config.auto_capture_reference_state = m_auto_capture_reference_state_check->isChecked();
  config.symbol_filter = m_symbol_filter_edit->text().trimmed().toStdString();
  config.output_folder = m_output_folder_edit->text().trimmed().toStdString();

  for (size_t i = 0; i < m_template_checks.size(); ++i)
    config.enabled_templates[i] = m_template_checks[i]->isChecked();

  std::string error;
  if (!CullingCodeFinder::GetInstance().StartScan(Core::System::GetInstance(), std::move(config),
                                                  &error))
  {
    QMessageBox::warning(this, tr("Unable to Start Scan"), QString::fromStdString(error));
    return;
  }

  m_last_result_count = -1;
  UpdateDisplay();
}

void CullingCodeFinderWidget::OnRestoreSession()
{
  const QString output_folder = m_output_folder_edit->text().trimmed();
  if (output_folder.isEmpty())
  {
    QMessageBox::warning(this, tr("No Output Folder"),
                         tr("Select the scan output folder that contains manifest.ini first."));
    return;
  }

  std::string error;
  std::string restored_message;
  auto& finder = CullingCodeFinder::GetInstance();
  if (!finder.RestoreSession(Core::System::GetInstance(), output_folder.toStdString(), &error,
                             &restored_message))
  {
    QMessageBox::warning(this, tr("Unable to Restore Session"), QString::fromStdString(error));
    return;
  }

  ApplyConfigToWidgets();
  m_last_result_count = -1;
  finder.AdvanceScan(Core::System::GetInstance());
  UpdateDisplay();

  QMessageBox::information(this, tr("Session Restored"),
                           QString::fromStdString(restored_message));
}

void CullingCodeFinderWidget::OnGenerateMap()
{
  std::string error;
  if (!CullingCodeFinder::GetInstance().GenerateSymbolMap(Core::System::GetInstance(), &error))
  {
    QMessageBox::warning(this, tr("Unable to Generate Map"), QString::fromStdString(error));
    return;
  }

  emit Host::GetInstance()->PPCSymbolsChanged();
  RefreshStaticInfo();
  QMessageBox::information(this, tr("Map Generated"),
                           tr("Rebuilt and saved the writable symbol map for the running game."));
}

void CullingCodeFinderWidget::OnPauseScan()
{
  auto& finder = CullingCodeFinder::GetInstance();
  if (finder.GetState() == CullingCodeFinder::ScanState::Scanning)
  {
    finder.PauseScan();
    finder.AdvanceScan(Core::System::GetInstance());
  }
  else if (finder.GetState() == CullingCodeFinder::ScanState::Paused)
  {
    finder.ResumeScan();
  }

  UpdateDisplay();
}

void CullingCodeFinderWidget::OnStopScan()
{
  auto& finder = CullingCodeFinder::GetInstance();
  finder.StopScan();
  finder.AdvanceScan(Core::System::GetInstance());
  UpdateDisplay();
}

void CullingCodeFinderWidget::OnGenerateARCode()
{
  const std::string game_id = SConfig::GetInstance().GetGameID();
  if (game_id.empty())
  {
    QMessageBox::warning(this, tr("No Game Running"),
                         tr("Boot a game before generating Action Replay codes."));
    return;
  }

  const QModelIndexList selected_rows = m_results_table->selectionModel()->selectedRows();
  if (selected_rows.empty())
  {
    QMessageBox::information(this, tr("No Results Selected"),
                             tr("Select one or more results first."));
    return;
  }

  const auto results = CullingCodeFinder::GetInstance().GetResults();
  std::vector<int> row_indices;
  row_indices.reserve(selected_rows.size());
  for (const QModelIndex& index : selected_rows)
    row_indices.push_back(index.row());
  std::ranges::sort(row_indices);

  std::vector<ActionReplay::ARCode> generated_codes;
  generated_codes.reserve(row_indices.size());
  QString clipboard_text;

  for (int row : row_indices)
  {
    if (row < 0 || row >= static_cast<int>(m_visible_result_indices.size()))
      continue;

    const int result_index = m_visible_result_indices[static_cast<size_t>(row)];
    if (result_index < 0 || result_index >= static_cast<int>(results.size()))
      continue;

    const auto& result = results[static_cast<size_t>(result_index)];
    const auto lines = CullingCodeFinder::BuildARCodeLines(result.address, result.template_kind);

    ActionReplay::ARCode code;
    code.name =
        fmt::format("VR Culling - {} [{:08X}] {}", result.symbol_name, result.address,
                    CullingCodeFinder::GetPatchTemplateName(result.template_kind));
    code.enabled = true;
    code.default_enabled = false;
    code.user_defined = true;

    for (const std::string& line : lines)
    {
      const auto parsed = ActionReplay::DeserializeLine(line);
      if (const auto* entry = std::get_if<ActionReplay::AREntry>(&parsed))
        code.ops.push_back(*entry);
    }

    if (code.ops.size() != lines.size())
      continue;

    generated_codes.push_back(std::move(code));

    if (!clipboard_text.isEmpty())
      clipboard_text.append(QStringLiteral("\n\n"));
    clipboard_text.append(QString::fromStdString(result.ar_code_text));
  }

  if (generated_codes.empty())
  {
    QMessageBox::warning(this, tr("Unable to Generate Codes"),
                         tr("The selected results did not produce valid AR code lines."));
    return;
  }

  Common::IniFile game_ini_local;
  const Common::IniFile game_ini_default =
      SConfig::LoadDefaultGameIni(game_id, SConfig::GetInstance().GetRevision());
  const std::string ini_path =
      std::string(File::GetUserPath(D_GAMESETTINGS_IDX)).append(game_id).append(".ini");
  game_ini_local.Load(ini_path);

  std::vector<ActionReplay::ARCode> codes = ActionReplay::LoadCodes(game_ini_default, game_ini_local);

  for (ActionReplay::ARCode& code : generated_codes)
  {
    std::erase_if(codes, [&](const ActionReplay::ARCode& existing_code) {
      return existing_code.user_defined && existing_code.name == code.name;
    });
    codes.push_back(std::move(code));
  }

  ActionReplay::SaveCodes(&game_ini_local, codes);
  if (!game_ini_local.Save(ini_path))
  {
    QMessageBox::warning(this, tr("Unable to Save Codes"),
                         tr("Failed to save generated codes to the local game INI."));
    return;
  }

  ActionReplay::ApplyCodes(codes, game_id, SConfig::GetInstance().GetRevision());
  QGuiApplication::clipboard()->setText(clipboard_text);

  QMessageBox::information(this, tr("AR Codes Generated"),
                           tr("Saved %1 code(s) and copied the generated text to the clipboard.")
                               .arg(generated_codes.size()));
}

void CullingCodeFinderWidget::OnBrowseOutputFolder()
{
  const QString current_path = m_output_folder_edit->text().trimmed();
  const QString selected_path =
      QFileDialog::getExistingDirectory(this, tr("Select Output Folder"), current_path);
  if (!selected_path.isEmpty())
    m_output_folder_edit->setText(selected_path);
}

void CullingCodeFinderWidget::OnOpenOutputFolder()
{
  const QString output_path = m_output_folder_edit->text().trimmed();
  if (output_path.isEmpty())
    return;

  File::CreateFullPath(output_path.toStdString() + DIR_SEP);
  QDesktopServices::openUrl(QUrl::fromLocalFile(output_path));
}

void CullingCodeFinderWidget::OnResetResults()
{
  auto& finder = CullingCodeFinder::GetInstance();
  std::string error;
  finder.ClearResults(&error);
  if (!error.empty())
  {
    QMessageBox::warning(this, tr("Unable to Reset Results"), QString::fromStdString(error));
    return;
  }

  m_last_result_count = -1;
  UpdateDisplay();
}

void CullingCodeFinderWidget::UpdateResultsTable()
{
  const auto results = CullingCodeFinder::GetInstance().GetResults();
  const bool show_timeouts =
      m_show_timeout_results_check && m_show_timeout_results_check->isChecked();

  m_visible_result_indices.clear();
  m_visible_result_indices.reserve(results.size());
  for (int i = 0; i < static_cast<int>(results.size()); ++i)
  {
    if (!show_timeouts && results[static_cast<size_t>(i)].caused_timeout)
      continue;
    m_visible_result_indices.push_back(i);
  }

  QSignalBlocker blocker(m_results_table);
  m_results_table->clearSelection();
  m_results_table->setRowCount(static_cast<int>(m_visible_result_indices.size()));

  for (int row = 0; row < static_cast<int>(m_visible_result_indices.size()); ++row)
  {
    const auto& result = results[static_cast<size_t>(m_visible_result_indices[row])];
    auto* symbol_item = new QTableWidgetItem(QString::fromStdString(result.symbol_name));
    auto* address_item = new QTableWidgetItem(QString::asprintf("%08X", result.address));
    auto* template_item = new QTableWidgetItem(
        QString::fromStdString(CullingCodeFinder::GetPatchTemplateName(result.template_kind)));
    auto* draw_call_item = new QTableWidgetItem(QString::number(result.draw_call_delta));
    auto* triangle_item = new QTableWidgetItem(QString::number(result.triangle_delta));
    auto* object_item = new QTableWidgetItem(QString::number(result.object_delta));
    auto* timeout_item = new QTableWidgetItem(result.caused_timeout ? tr("Yes") : tr("No"));
    auto* screenshot_item = new QTableWidgetItem(PathLeaf(result.screenshot_path));

    address_item->setTextAlignment(Qt::AlignCenter);
    draw_call_item->setTextAlignment(Qt::AlignCenter);
    triangle_item->setTextAlignment(Qt::AlignCenter);
    object_item->setTextAlignment(Qt::AlignCenter);
    timeout_item->setTextAlignment(Qt::AlignCenter);

    m_results_table->setItem(row, 0, symbol_item);
    m_results_table->setItem(row, 1, address_item);
    m_results_table->setItem(row, 2, template_item);
    m_results_table->setItem(row, 3, draw_call_item);
    m_results_table->setItem(row, 4, triangle_item);
    m_results_table->setItem(row, 5, object_item);
    m_results_table->setItem(row, 6, timeout_item);
    m_results_table->setItem(row, 7, screenshot_item);
  }

  m_results_table->resizeColumnsToContents();
}

void CullingCodeFinderWidget::UpdateButtonStates()
{
  const auto state = CullingCodeFinder::GetInstance().GetState();
  const bool actively_scanning = state == CullingCodeFinder::ScanState::Scanning;
  const bool paused = state == CullingCodeFinder::ScanState::Paused;
  const bool busy = actively_scanning || paused;

  m_set_baseline_btn->setEnabled(!busy);
  m_generate_map_btn->setEnabled(!busy);
  m_savestate_slot_spin->setEnabled(!busy);
  m_settle_frames_spin->setEnabled(!busy);
  m_min_draw_call_spin->setEnabled(!busy);
  m_min_triangle_spin->setEnabled(!busy);
  m_min_object_spin->setEnabled(!busy);
  m_candidate_limit_spin->setEnabled(!busy);
  m_start_address_edit->setEnabled(!busy);
  m_end_address_edit->setEnabled(!busy);
  m_symbol_filter_edit->setEnabled(!busy);
  m_turbo_during_scan_check->setEnabled(!busy);
  m_mute_audio_during_scan_check->setEnabled(!busy);
  m_auto_capture_reference_state_check->setEnabled(!busy);
  m_output_folder_edit->setEnabled(!busy);
  m_browse_output_btn->setEnabled(!busy);
  for (QCheckBox* checkbox : m_template_checks)
  {
    if (checkbox)
      checkbox->setEnabled(!busy);
  }

  m_start_btn->setEnabled(!busy);
  m_restore_btn->setEnabled(!busy && !m_output_folder_edit->text().trimmed().isEmpty());
  m_pause_btn->setEnabled(actively_scanning || paused);
  m_pause_btn->setText(paused ? tr("Resume") : tr("Pause"));
  m_stop_btn->setEnabled(busy);
  m_show_timeout_results_check->setEnabled(true);
  m_generate_ar_btn->setEnabled(m_results_table->rowCount() > 0);
  m_reset_results_btn->setEnabled(!busy && !CullingCodeFinder::GetInstance().GetResults().empty());
  m_open_output_btn->setEnabled(!m_output_folder_edit->text().trimmed().isEmpty());
}

void CullingCodeFinderWidget::ApplyConfigToWidgets()
{
  const auto config = CullingCodeFinder::GetInstance().GetConfig();

  m_savestate_slot_spin->setValue(config.savestate_slot);
  m_settle_frames_spin->setValue(config.settle_frames);
  m_min_draw_call_spin->setValue(config.min_draw_call_increase);
  m_min_triangle_spin->setValue(config.min_triangle_increase);
  m_min_object_spin->setValue(config.min_object_increase);
  m_candidate_limit_spin->setValue(config.candidate_limit);
  m_start_address_edit->setText(config.start_address == 0 ?
                                    QString{} :
                                    QString::asprintf("%08X", config.start_address));
  m_end_address_edit->setText(config.end_address == 0 ?
                                  QString{} :
                                  QString::asprintf("%08X", config.end_address));
  m_symbol_filter_edit->setText(QString::fromStdString(config.symbol_filter));
  m_turbo_during_scan_check->setChecked(config.turbo_during_scan);
  m_mute_audio_during_scan_check->setChecked(config.mute_audio_during_scan);
  m_auto_capture_reference_state_check->setChecked(config.auto_capture_reference_state);
  m_output_folder_edit->setText(QString::fromStdString(config.output_folder));

  for (size_t i = 0; i < m_template_checks.size(); ++i)
  {
    if (m_template_checks[i])
      m_template_checks[i]->setChecked(config.enabled_templates[i]);
  }
}
