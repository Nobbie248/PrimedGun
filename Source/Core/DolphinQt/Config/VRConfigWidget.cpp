// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Config/VRConfigWidget.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QStyle>
#include <QTabWidget>
#include <QVBoxLayout>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Common/StringUtil.h"

#include "Core/Config/GraphicsSettings.h"
#include "Core/ConfigManager.h"
#include "DolphinQt/Config/GameConfigEdit.h"
#include "DolphinQt/QtUtils/QtUtils.h"

namespace
{
constexpr std::string_view VR_SECTION_NAME = "Graphics.VR";
constexpr std::string_view LEGACY_VR_SECTION_NAME = "GFX.VR";
constexpr double VR_GAME_UPM_MIN = 0.01;
constexpr double VR_GAME_UPM_MAX = 1000.0;
constexpr double VR_GAME_UPM_STEP = 0.01;

void ResetTabPages(QTabWidget* tab)
{
  while (tab->count() > 0)
  {
    QWidget* const page = tab->widget(0);
    tab->removeTab(0);
    delete page;
  }
}
}  // namespace

VRConfigWidget::VRConfigWidget(std::string game_id, QWidget* parent)
    : QWidget(parent), m_game_id(std::move(game_id))
{
  CreateWidgets();
  LoadFromFile();
}

void VRConfigWidget::CreateWidgets()
{
  auto* general_layout = new QVBoxLayout;

  auto* description = new QLabel(
      tr("Configure per-game VR overrides saved to GameSettingsVR/<GameID>.ini."));
  description->setWordWrap(true);
  general_layout->addWidget(description);

  auto* override_group = new QGroupBox(tr("Overrides"));
  auto* form = new QFormLayout;
  override_group->setLayout(form);

  auto* units_row = new QWidget;
  auto* units_layout = new QHBoxLayout;
  units_layout->setContentsMargins(0, 0, 0, 0);
  units_row->setLayout(units_layout);

  m_override_units_per_meter = new QCheckBox(tr("Override"));
  m_units_per_meter = new QDoubleSpinBox;
  m_units_per_meter->setRange(VR_GAME_UPM_MIN, VR_GAME_UPM_MAX);
  m_units_per_meter->setSingleStep(VR_GAME_UPM_STEP);
  m_units_per_meter->setDecimals(2);
  m_units_per_meter->setSuffix(tr(" units/m"));
  units_layout->addWidget(m_override_units_per_meter);
  units_layout->addWidget(m_units_per_meter, 1);

  auto* lean_back_row = new QWidget;
  auto* lean_back_layout = new QHBoxLayout;
  lean_back_layout->setContentsMargins(0, 0, 0, 0);
  lean_back_row->setLayout(lean_back_layout);

  m_override_lean_back_angle = new QCheckBox(tr("Override"));
  m_lean_back_angle = new QDoubleSpinBox;
  m_lean_back_angle->setRange(Config::GFX_VR_LEAN_BACK_ANGLE_MIN, Config::GFX_VR_LEAN_BACK_ANGLE_MAX);
  m_lean_back_angle->setSingleStep(Config::GFX_VR_LEAN_BACK_ANGLE_STEP);
  m_lean_back_angle->setDecimals(1);
  m_lean_back_angle->setSuffix(tr(" deg"));
  lean_back_layout->addWidget(m_override_lean_back_angle);
  lean_back_layout->addWidget(m_lean_back_angle, 1);

  auto* camera_forward_row = new QWidget;
  auto* camera_forward_layout = new QHBoxLayout;
  camera_forward_layout->setContentsMargins(0, 0, 0, 0);
  camera_forward_row->setLayout(camera_forward_layout);

  m_override_camera_forward = new QCheckBox(tr("Override"));
  m_camera_forward = new QDoubleSpinBox;
  m_camera_forward->setRange(Config::GFX_VR_CAMERA_FORWARD_MIN, Config::GFX_VR_CAMERA_FORWARD_MAX);
  m_camera_forward->setSingleStep(Config::GFX_VR_CAMERA_FORWARD_STEP);
  m_camera_forward->setDecimals(1);
  m_camera_forward->setSuffix(tr(" m"));
  camera_forward_layout->addWidget(m_override_camera_forward);
  camera_forward_layout->addWidget(m_camera_forward, 1);

  auto* head_locked_curvature_row = new QWidget;
  auto* head_locked_curvature_layout = new QHBoxLayout;
  head_locked_curvature_layout->setContentsMargins(0, 0, 0, 0);
  head_locked_curvature_row->setLayout(head_locked_curvature_layout);

  m_override_head_locked_curvature = new QCheckBox(tr("Override"));
  m_head_locked_curvature = new QDoubleSpinBox;
  m_head_locked_curvature->setRange(Config::GFX_VR_HEAD_LOCKED_CURVATURE_MIN,
                                    Config::GFX_VR_HEAD_LOCKED_CURVATURE_MAX);
  m_head_locked_curvature->setSingleStep(Config::GFX_VR_HEAD_LOCKED_CURVATURE_STEP);
  m_head_locked_curvature->setDecimals(2);
  head_locked_curvature_layout->addWidget(m_override_head_locked_curvature);
  head_locked_curvature_layout->addWidget(m_head_locked_curvature, 1);

  auto* element_depth_row = new QWidget;
  auto* element_depth_layout = new QHBoxLayout;
  element_depth_layout->setContentsMargins(0, 0, 0, 0);
  element_depth_row->setLayout(element_depth_layout);

  m_override_element_depth = new QCheckBox(tr("Override"));
  m_element_depth = new QDoubleSpinBox;
  m_element_depth->setRange(Config::GFX_VR_ELEMENT_DEPTH_MIN, Config::GFX_VR_ELEMENT_DEPTH_MAX);
  m_element_depth->setSingleStep(Config::GFX_VR_ELEMENT_DEPTH_STEP);
  m_element_depth->setDecimals(4);
  element_depth_layout->addWidget(m_override_element_depth);
  element_depth_layout->addWidget(m_element_depth, 1);

  m_virtual_screen_mode = new QComboBox;
  m_dont_clear_screen_mode = new QComboBox;
  m_force_vbi_90hz_mode = new QComboBox;
  PopulateBoolModeCombo(m_virtual_screen_mode);
  PopulateBoolModeCombo(m_dont_clear_screen_mode);
  PopulateBoolModeCombo(m_force_vbi_90hz_mode);

  form->addRow(tr("Units per Meter"), units_row);
  form->addRow(tr("Lean Back Angle"), lean_back_row);
  form->addRow(tr("Camera Forward"), camera_forward_row);
  form->addRow(tr("Head Locked Curvature"), head_locked_curvature_row);
  form->addRow(tr("Element Depth"), element_depth_row);
  form->addRow(tr("Virtual Screen"), m_virtual_screen_mode);
  form->addRow(tr("Don't Clear Screen"), m_dont_clear_screen_mode);
  form->addRow(tr("Force VBI Frequency to 90 Hz in VR"), m_force_vbi_90hz_mode);

  general_layout->addWidget(override_group);
  general_layout->addStretch();

  auto* const general_widget = new QWidget;
  general_widget->setLayout(general_layout);

  auto* editor_layout = new QVBoxLayout;

  auto* local_group = new QGroupBox(tr("VR User Config"));
  auto* local_layout = new QVBoxLayout;
  m_local_tab = new QTabWidget;
  local_layout->addWidget(m_local_tab);
  local_group->setLayout(local_layout);

  editor_layout->addWidget(local_group);

  auto* const editor_widget = new QWidget;
  editor_widget->setLayout(editor_layout);

  auto* const tabs = new QTabWidget;
  tabs->addTab(general_widget, tr("General"));
  const int editor_index = tabs->addTab(editor_widget, tr("Editor"));

  auto* const root_layout = new QVBoxLayout;
  auto* const help_label = new QLabel(tr("These VR settings override core Dolphin settings."));
  auto* const help_widget =
      QtUtils::CreateIconWarning(this, QStyle::SP_MessageBoxQuestion, help_label);
  root_layout->addWidget(help_widget);
  root_layout->addWidget(tabs);
  setLayout(root_layout);

  connect(m_override_units_per_meter, &QCheckBox::toggled, this, [this](bool checked) {
    m_units_per_meter->setEnabled(checked);
    SaveToFile();
  });
  connect(m_units_per_meter, &QDoubleSpinBox::valueChanged, this,
          [this](double) { SaveToFile(); });
  connect(m_override_lean_back_angle, &QCheckBox::toggled, this, [this](bool checked) {
    m_lean_back_angle->setEnabled(checked);
    SaveToFile();
  });
  connect(m_lean_back_angle, &QDoubleSpinBox::valueChanged, this,
          [this](double) { SaveToFile(); });
  connect(m_override_camera_forward, &QCheckBox::toggled, this, [this](bool checked) {
    m_camera_forward->setEnabled(checked);
    SaveToFile();
  });
  connect(m_camera_forward, &QDoubleSpinBox::valueChanged, this,
          [this](double) { SaveToFile(); });
  connect(m_override_head_locked_curvature, &QCheckBox::toggled, this, [this](bool checked) {
    m_head_locked_curvature->setEnabled(checked);
    SaveToFile();
  });
  connect(m_head_locked_curvature, &QDoubleSpinBox::valueChanged, this,
          [this](double) { SaveToFile(); });
  connect(m_override_element_depth, &QCheckBox::toggled, this, [this](bool checked) {
    m_element_depth->setEnabled(checked);
    SaveToFile();
  });
  connect(m_element_depth, &QDoubleSpinBox::valueChanged, this,
          [this](double) { SaveToFile(); });
  connect(m_virtual_screen_mode, &QComboBox::currentIndexChanged, this,
          [this](int) { SaveToFile(); });
  connect(m_dont_clear_screen_mode, &QComboBox::currentIndexChanged, this,
          [this](int) { SaveToFile(); });
  connect(m_force_vbi_90hz_mode, &QComboBox::currentIndexChanged, this,
          [this](int) { SaveToFile(); });

  connect(tabs, &QTabWidget::currentChanged, this, [this, editor_index](int index) {
    // Sync editor content with current file after changes from General.
    if (index == editor_index)
      RefreshEditorTabs();

    // Reload controls after raw INI edits.
    if (m_prev_tab_index == editor_index)
    {
      LoadFromFile();
      if (SConfig::GetInstance().GetGameID() == m_game_id)
        SConfig::GetInstance().ReloadGameVRConfigOverrides();
    }

    m_prev_tab_index = index;
  });

  RefreshEditorTabs();
}

void VRConfigWidget::LoadFromFile()
{
  const ValueMap values = ReadVRSectionValues(GetINIPath());

  m_updating = true;

  const auto units_it = values.find("UnitsPerMeter");
  const bool units_overridden = units_it != values.end();
  m_override_units_per_meter->setChecked(units_overridden);
  m_units_per_meter->setEnabled(units_overridden);

  double units_per_meter = Config::GetBase(Config::GFX_VR_UNITS_PER_METER);
  if (units_overridden)
  {
    double parsed = units_per_meter;
    if (TryParse(units_it->second, &parsed))
      units_per_meter = parsed;
  }
  units_per_meter =
      std::clamp(units_per_meter, VR_GAME_UPM_MIN, VR_GAME_UPM_MAX);
  m_units_per_meter->setValue(units_per_meter);

  const auto lean_back_it = values.find("LeanBackAngle");
  const bool lean_back_overridden = lean_back_it != values.end();
  m_override_lean_back_angle->setChecked(lean_back_overridden);
  m_lean_back_angle->setEnabled(lean_back_overridden);

  double lean_back_angle = Config::GetBase(Config::GFX_VR_LEAN_BACK_ANGLE);
  if (lean_back_overridden)
  {
    double parsed = lean_back_angle;
    if (TryParse(lean_back_it->second, &parsed))
      lean_back_angle = parsed;
  }
  lean_back_angle =
      std::clamp(lean_back_angle, static_cast<double>(Config::GFX_VR_LEAN_BACK_ANGLE_MIN),
                 static_cast<double>(Config::GFX_VR_LEAN_BACK_ANGLE_MAX));
  m_lean_back_angle->setValue(lean_back_angle);

  const auto camera_forward_it = values.find("CameraForward");
  const bool camera_forward_overridden = camera_forward_it != values.end();
  m_override_camera_forward->setChecked(camera_forward_overridden);
  m_camera_forward->setEnabled(camera_forward_overridden);

  double camera_forward = Config::GetBase(Config::GFX_VR_CAMERA_FORWARD);
  if (camera_forward_overridden)
  {
    double parsed = camera_forward;
    if (TryParse(camera_forward_it->second, &parsed))
      camera_forward = parsed;
  }
  camera_forward =
      std::clamp(camera_forward, static_cast<double>(Config::GFX_VR_CAMERA_FORWARD_MIN),
                 static_cast<double>(Config::GFX_VR_CAMERA_FORWARD_MAX));
  m_camera_forward->setValue(camera_forward);

  const auto head_locked_curvature_it = values.find("HeadLockedCurvature");
  const bool head_locked_curvature_overridden = head_locked_curvature_it != values.end();
  m_override_head_locked_curvature->setChecked(head_locked_curvature_overridden);
  m_head_locked_curvature->setEnabled(head_locked_curvature_overridden);

  double head_locked_curvature = Config::GetBase(Config::GFX_VR_HEAD_LOCKED_CURVATURE);
  if (head_locked_curvature_overridden)
  {
    double parsed = head_locked_curvature;
    if (TryParse(head_locked_curvature_it->second, &parsed))
      head_locked_curvature = parsed;
  }
  head_locked_curvature =
      std::clamp(head_locked_curvature, static_cast<double>(Config::GFX_VR_HEAD_LOCKED_CURVATURE_MIN),
                 static_cast<double>(Config::GFX_VR_HEAD_LOCKED_CURVATURE_MAX));
  m_head_locked_curvature->setValue(head_locked_curvature);

  const auto element_depth_it = values.find("ElementDepth");
  const bool element_depth_overridden = element_depth_it != values.end();
  m_override_element_depth->setChecked(element_depth_overridden);
  m_element_depth->setEnabled(element_depth_overridden);

  double element_depth = Config::GetBase(Config::GFX_VR_ELEMENT_DEPTH);
  if (element_depth_overridden)
  {
    double parsed = element_depth;
    if (TryParse(element_depth_it->second, &parsed))
      element_depth = parsed;
  }
  element_depth =
      std::clamp(element_depth, static_cast<double>(Config::GFX_VR_ELEMENT_DEPTH_MIN),
                 static_cast<double>(Config::GFX_VR_ELEMENT_DEPTH_MAX));
  m_element_depth->setValue(element_depth);

  SetBoolMode(m_virtual_screen_mode, ParseBoolMode(values, "VirtualScreen"));
  SetBoolMode(m_dont_clear_screen_mode, ParseBoolMode(values, "DontClearScreen"));
  SetBoolMode(m_force_vbi_90hz_mode, ParseBoolMode(values, "AutoVBIFromHMD"));

  m_updating = false;
}

void VRConfigWidget::SaveToFile()
{
  if (m_updating || m_game_id.empty())
    return;

  std::vector<std::pair<std::string, std::string>> entries;
  if (m_override_units_per_meter->isChecked())
  {
    entries.emplace_back("UnitsPerMeter",
                         QString::number(m_units_per_meter->value(), 'f', 2).toStdString());
  }
  if (m_override_lean_back_angle->isChecked())
  {
    entries.emplace_back("LeanBackAngle",
                         QString::number(m_lean_back_angle->value(), 'f', 1).toStdString());
  }
  if (m_override_camera_forward->isChecked())
  {
    entries.emplace_back("CameraForward",
                         QString::number(m_camera_forward->value(), 'f', 1).toStdString());
  }
  if (m_override_head_locked_curvature->isChecked())
  {
    entries.emplace_back("HeadLockedCurvature",
                         QString::number(m_head_locked_curvature->value(), 'f', 2).toStdString());
  }
  if (m_override_element_depth->isChecked())
  {
    entries.emplace_back("ElementDepth",
                         QString::number(m_element_depth->value(), 'f', 4).toStdString());
  }

  const auto append_bool = [&entries](const char* key, BoolMode mode) {
    if (mode == BoolMode::Inherit)
      return;
    entries.emplace_back(key, mode == BoolMode::Enabled ? "True" : "False");
  };
  append_bool("VirtualScreen", GetBoolMode(m_virtual_screen_mode));
  append_bool("DontClearScreen", GetBoolMode(m_dont_clear_screen_mode));
  append_bool("AutoVBIFromHMD", GetBoolMode(m_force_vbi_90hz_mode));

  const std::string path = GetINIPath();
  std::string base = ReadFileWithoutVRSection(path);
  while (!base.empty() &&
         (base.back() == '\n' || base.back() == '\r' ||
          std::isspace(static_cast<unsigned char>(base.back()))))
    base.pop_back();

  std::ostringstream out;
  if (!base.empty())
    out << base << "\n";

  if (!entries.empty())
  {
    out << '[' << VR_SECTION_NAME << "]\n";
    for (const auto& [key, value] : entries)
      out << key << " = " << value << "\n";
  }

  const std::string output = out.str();
  if (output.empty())
  {
    File::Delete(path, File::IfAbsentBehavior::NoConsoleWarning);
  }
  else
  {
    File::CreateFullPath(path);
    std::ofstream file(path, std::ios::trunc);
    file << output;
  }

  if (SConfig::GetInstance().GetGameID() == m_game_id)
    SConfig::GetInstance().ReloadGameVRConfigOverrides();
}

void VRConfigWidget::RefreshEditorTabs()
{
  if (!m_local_tab || m_game_id.empty())
    return;

  ResetTabPages(m_local_tab);

  const std::string local_path = GetINIPath();
  m_local_tab->addTab(new GameConfigEdit(nullptr, QString::fromStdString(local_path), false),
                      QString::fromStdString(m_game_id + ".ini"));
}

std::string VRConfigWidget::GetINIPath() const
{
  return File::GetUserPath(D_GAMESETTINGSVR_IDX) + m_game_id + ".ini";
}

bool VRConfigWidget::IsVRSectionHeader(std::string_view line)
{
  const std::string_view trimmed = StripWhitespace(line);
  if (trimmed.size() < 3 || trimmed.front() != '[' || trimmed.back() != ']')
    return false;

  const std::string_view section_name = trimmed.substr(1, trimmed.size() - 2);
  return Common::CaseInsensitiveEquals(section_name, VR_SECTION_NAME) ||
         Common::CaseInsensitiveEquals(section_name, LEGACY_VR_SECTION_NAME);
}

std::string VRConfigWidget::ReadFileWithoutVRSection(const std::string& path)
{
  std::ifstream file(path);
  if (!file.is_open())
    return {};

  std::ostringstream out;
  bool skipping = false;
  std::string line;
  while (std::getline(file, line))
  {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();

    const std::string_view trimmed = StripWhitespace(line);
    if (IsVRSectionHeader(trimmed))
    {
      skipping = true;
      continue;
    }

    if (skipping && !trimmed.empty() && trimmed.front() == '[')
      skipping = false;

    if (!skipping)
      out << line << "\n";
  }

  return out.str();
}

VRConfigWidget::ValueMap VRConfigWidget::ReadVRSectionValues(const std::string& path)
{
  ValueMap values;

  std::ifstream file(path);
  if (!file.is_open())
    return values;

  bool in_section = false;
  std::string line;
  while (std::getline(file, line))
  {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();

    const std::string_view trimmed = StripWhitespace(line);
    if (trimmed.empty())
      continue;

    if (IsVRSectionHeader(trimmed))
    {
      in_section = true;
      continue;
    }

    if (in_section && trimmed.front() == '[')
      break;

    if (!in_section)
      continue;

    std::string key;
    std::string value;
    Common::IniFile::ParseLine(trimmed, &key, &value);
    if (!key.empty())
      values.insert_or_assign(std::move(key), std::move(value));
  }

  return values;
}

VRConfigWidget::BoolMode VRConfigWidget::ParseBoolMode(const ValueMap& values, const char* key)
{
  const auto it = values.find(key);
  if (it == values.end())
    return BoolMode::Inherit;

  bool parsed = false;
  if (TryParse(it->second, &parsed))
    return parsed ? BoolMode::Enabled : BoolMode::Disabled;
  return BoolMode::Inherit;
}

void VRConfigWidget::SetBoolMode(QComboBox* combo, BoolMode mode)
{
  combo->setCurrentIndex(static_cast<int>(mode));
}

VRConfigWidget::BoolMode VRConfigWidget::GetBoolMode(const QComboBox* combo)
{
  const int index = combo->currentIndex();
  if (index == static_cast<int>(BoolMode::Enabled))
    return BoolMode::Enabled;
  if (index == static_cast<int>(BoolMode::Disabled))
    return BoolMode::Disabled;
  return BoolMode::Inherit;
}

void VRConfigWidget::PopulateBoolModeCombo(QComboBox* combo)
{
  combo->addItem(QObject::tr("Inherit"));
  combo->addItem(QObject::tr("Enabled"));
  combo->addItem(QObject::tr("Disabled"));
}
