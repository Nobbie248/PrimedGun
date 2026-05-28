// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <vector>

#include <QCloseEvent>
#include <QDialog>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;
class QVBoxLayout;
class QWidget;

class ShaderHunterWidget : public QDialog
{
  Q_OBJECT
public:
  explicit ShaderHunterWidget(QWidget* parent = nullptr);

signals:
  void OverridesChanged();

private:
  void closeEvent(QCloseEvent* event) override;
  void CreateWidgets();
  void ConnectSignals();
  void UpdateDisplay();
  void ShowDrawCallDialog();
  void SetSelectedDrawCallRange(int start, int end, int total);
  void SetSelectedTextureHashes(const std::vector<uint64_t>& hashes);
  void SaveCurrentShader();
  void DumpCurrentShader();
  void ShowTexturesDialog();

  QVBoxLayout* m_main_layout = nullptr;
  QCheckBox* m_enable_checkbox;
  QComboBox* m_hunting_option_combo;
  QCheckBox* m_debug_log_checkbox;
  QComboBox* m_type_combo;
  QLabel* m_hash_label;
  QLabel* m_position_label;
  QPushButton* m_prev_button;
  QPushButton* m_next_button;
  QLineEdit* m_shader_name_edit;
  QComboBox* m_handling_combo;
  QLabel* m_units_per_meter_label;
  QDoubleSpinBox* m_units_per_meter_spin;
  QPushButton* m_save_button;
  QPushButton* m_dump_button;
  QPushButton* m_textures_button;
  QPushButton* m_draw_calls_button;
  QLabel* m_selected_draw_call_label;
  QComboBox* m_texture_filter_mode_combo;
  QLabel* m_selected_texture_label;
  QTimer* m_update_timer;
  int m_saved_element_start = -1;
  int m_saved_element_end = -1;
  int m_saved_element_total = 0;
  std::vector<uint64_t> m_saved_texture_filters;
};
