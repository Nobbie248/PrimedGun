// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QDialog>

#include "VideoCommon/ShaderHunter.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QString;
class QSpinBox;
class QVBoxLayout;
class QWidget;

class ShaderOverrideAddEditDialog : public QDialog
{
  Q_OBJECT
public:
  // edit_override != nullptr → edit mode (pre-fills fields); nullptr → add mode.
  explicit ShaderOverrideAddEditDialog(
      QWidget* parent, const ShaderHunter::ShaderOverride* edit_override = nullptr,
      const std::vector<std::string>& available_flags = {});

  ShaderHunter::ShaderOverride GetResult() const;

private:
  std::vector<std::string> CollectTextureHashTokens() const;
  std::vector<u64> CollectTextureHashValues() const;
  void SetTextureHashFields(const std::vector<u64>& hashes);
  void AddTextureHashField(const QString& text);
  void EnsureTextureHashFieldRows();
  void SanitizeTextureHashField(QLineEdit* edit);
  void ShowTextureBrowser();

  void OnAccept();
  void OnHandlingChanged();
  void OnClearEFBChanged();
  void OnElementFilterChanged();

  QLineEdit* m_name_edit;
  QPlainTextEdit* m_comments_edit;
  QLineEdit* m_credits_edit;
  QLineEdit* m_hash_edit;
  QComboBox* m_type_combo;
  QComboBox* m_match_mode_combo;
  QLabel* m_match_mode_label;
  QComboBox* m_handling_combo;
  QSpinBox* m_layer_spin;
  QLabel* m_layer_label;
  QDoubleSpinBox* m_element_depth_spin;
  QLabel* m_element_depth_label;
  QDoubleSpinBox* m_units_per_meter_spin;
  QLabel* m_units_per_meter_label;
  QLineEdit* m_flag_edit;
  QLabel* m_flag_label;
  QComboBox* m_condition_combo;
  QLabel* m_condition_label;
  QComboBox* m_condition_mode_combo;
  QLabel* m_condition_mode_label;
  QCheckBox* m_clear_efb_check;
  QSpinBox* m_clear_efb_min_spin;
  QSpinBox* m_clear_efb_max_spin;
  QLabel* m_clear_efb_min_label;
  QLabel* m_clear_efb_max_label;
  QSpinBox* m_element_start_spin;
  QSpinBox* m_element_end_spin;
  QLabel* m_element_start_label;
  QLabel* m_element_end_label;
  QCheckBox* m_element_filter_check;
  QCheckBox* m_hash_family_check;
  QPushButton* m_view_textures_button;
  QComboBox* m_texture_mode_combo;
  QLabel* m_texture_mode_label;
  QWidget* m_texture_hash_container;
  QScrollArea* m_texture_hash_scroll;
  QVBoxLayout* m_texture_hash_layout;
  std::vector<QLineEdit*> m_texture_hash_edits;
  bool m_updating_texture_hash_fields = false;
  int m_edit_element_reference_total = 0;
  u64 m_edit_family_signature = 0;
};
