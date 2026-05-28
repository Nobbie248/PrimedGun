// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <string>

#include <QWidget>

#include "Common/StringUtil.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QTabWidget;

class VRConfigWidget final : public QWidget
{
  Q_OBJECT

public:
  explicit VRConfigWidget(std::string game_id, QWidget* parent = nullptr);

private:
  enum class BoolMode
  {
    Inherit = 0,
    Enabled = 1,
    Disabled = 2,
  };

  using ValueMap = std::map<std::string, std::string, Common::CaseInsensitiveLess>;

  void CreateWidgets();
  void LoadFromFile();
  void SaveToFile();
  void RefreshEditorTabs();

  std::string GetINIPath() const;
  static bool IsVRSectionHeader(std::string_view line);
  static std::string ReadFileWithoutVRSection(const std::string& path);
  static ValueMap ReadVRSectionValues(const std::string& path);
  static BoolMode ParseBoolMode(const ValueMap& values, const char* key);
  static void SetBoolMode(QComboBox* combo, BoolMode mode);
  static BoolMode GetBoolMode(const QComboBox* combo);
  static void PopulateBoolModeCombo(QComboBox* combo);

  const std::string m_game_id;
  bool m_updating = false;

  QCheckBox* m_override_units_per_meter = nullptr;
  QDoubleSpinBox* m_units_per_meter = nullptr;
  QCheckBox* m_override_lean_back_angle = nullptr;
  QDoubleSpinBox* m_lean_back_angle = nullptr;
  QCheckBox* m_override_camera_forward = nullptr;
  QDoubleSpinBox* m_camera_forward = nullptr;
  QCheckBox* m_override_head_locked_curvature = nullptr;
  QDoubleSpinBox* m_head_locked_curvature = nullptr;
  QCheckBox* m_override_element_depth = nullptr;
  QDoubleSpinBox* m_element_depth = nullptr;
  QComboBox* m_virtual_screen_mode = nullptr;
  QComboBox* m_dont_clear_screen_mode = nullptr;
  QComboBox* m_force_vbi_90hz_mode = nullptr;

  QTabWidget* m_local_tab = nullptr;
  int m_prev_tab_index = 0;
};
