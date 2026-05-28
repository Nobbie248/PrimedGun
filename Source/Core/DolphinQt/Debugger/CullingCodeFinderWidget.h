// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QDialog>

#include <array>
#include <vector>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QProgressBar;
class QSpinBox;
class QTableWidget;
class QTimer;

class CullingCodeFinderWidget : public QDialog
{
  Q_OBJECT
public:
  explicit CullingCodeFinderWidget(QWidget* parent = nullptr);

private:
  void closeEvent(QCloseEvent* event) override;
  void CreateWidgets();
  void ConnectSignals();
  void RefreshStaticInfo();
  void UpdateDisplay();
  void OnSetBaseline();
  void OnStartScan();
  void OnRestoreSession();
  void OnPauseScan();
  void OnStopScan();
  void OnGenerateMap();
  void OnGenerateARCode();
  void OnBrowseOutputFolder();
  void OnOpenOutputFolder();
  void OnResetResults();
  void UpdateResultsTable();
  void UpdateButtonStates();
  void ApplyConfigToWidgets();

  QLabel* m_live_draw_calls = nullptr;
  QLabel* m_live_triangles = nullptr;
  QLabel* m_live_prims = nullptr;
  QLabel* m_live_objects = nullptr;

  QLabel* m_baseline_label = nullptr;
  QPushButton* m_set_baseline_btn = nullptr;

  QLabel* m_map_path_label = nullptr;
  QPushButton* m_generate_map_btn = nullptr;
  QLineEdit* m_output_folder_edit = nullptr;
  QPushButton* m_browse_output_btn = nullptr;
  QPushButton* m_open_output_btn = nullptr;

  QSpinBox* m_savestate_slot_spin = nullptr;
  QSpinBox* m_settle_frames_spin = nullptr;
  QLabel* m_settle_frames_help = nullptr;
  QSpinBox* m_min_draw_call_spin = nullptr;
  QSpinBox* m_min_triangle_spin = nullptr;
  QSpinBox* m_min_object_spin = nullptr;
  QSpinBox* m_candidate_limit_spin = nullptr;
  QLineEdit* m_start_address_edit = nullptr;
  QLineEdit* m_end_address_edit = nullptr;
  QLineEdit* m_symbol_filter_edit = nullptr;
  QCheckBox* m_turbo_during_scan_check = nullptr;
  QCheckBox* m_mute_audio_during_scan_check = nullptr;
  QCheckBox* m_auto_capture_reference_state_check = nullptr;
  std::array<QCheckBox*, 4> m_template_checks{};

  QPushButton* m_start_btn = nullptr;
  QPushButton* m_restore_btn = nullptr;
  QPushButton* m_pause_btn = nullptr;
  QPushButton* m_stop_btn = nullptr;
  QProgressBar* m_progress_bar = nullptr;
  QLabel* m_status_label = nullptr;

  QTableWidget* m_results_table = nullptr;
  QCheckBox* m_show_timeout_results_check = nullptr;
  QPushButton* m_generate_ar_btn = nullptr;
  QPushButton* m_reset_results_btn = nullptr;
  std::vector<int> m_visible_result_indices;
  int m_last_result_count = -1;

  QTimer* m_update_timer = nullptr;
};
