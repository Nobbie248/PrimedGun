// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Debugger/ShaderHunterWidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QBrush>
#include <QColor>
#include <QDir>
#include <QDirIterator>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSize>
#include <QStringList>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cctype>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/CommonTypes.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/ConfigManager.h"
#include "DolphinQt/Settings.h"
#include "VideoCommon/ShaderHunter.h"

namespace
{
static QString ToHashHex(u64 hash)
{
  return QStringLiteral("%1").arg(static_cast<qulonglong>(hash), 16, 16, QLatin1Char('0'));
}

static std::unordered_map<u64, QString> FindTexturePreviewPaths(const std::vector<u64>& hashes)
{
  std::unordered_map<u64, QString> result;
  if (hashes.empty())
    return result;

  std::unordered_set<u64> remaining(hashes.begin(), hashes.end());
  std::unordered_map<u64, QString> patterns;
  patterns.reserve(remaining.size());
  for (u64 hash : remaining)
    patterns.emplace(hash, QStringLiteral("_%1_").arg(ToHashHex(hash)));

  const QString dump_root = QString::fromStdString(File::GetUserPath(D_DUMPTEXTURES_IDX));
  QStringList roots;
  const std::string game_id = SConfig::GetInstance().GetGameID();
  if (!game_id.empty())
    roots.push_back(QDir::cleanPath(dump_root + QString::fromStdString(game_id)));
  roots.push_back(QDir::cleanPath(dump_root));

  for (const QString& root : roots)
  {
    if (!QDir(root).exists())
      continue;

    QDirIterator it(root, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext() && !remaining.empty())
    {
      const QString path = it.next();
      const QFileInfo info(path);
      const QString suffix = info.suffix().toLower();
      if (suffix != QStringLiteral("png") && suffix != QStringLiteral("jpg") &&
          suffix != QStringLiteral("jpeg") && suffix != QStringLiteral("bmp") &&
          suffix != QStringLiteral("tga"))
      {
        continue;
      }

      const QString filename = info.fileName().toLower();
      for (auto iter = remaining.begin(); iter != remaining.end();)
      {
        const u64 hash = *iter;
        if (filename.contains(patterns[hash]))
        {
          result.emplace(hash, path);
          iter = remaining.erase(iter);
        }
        else
        {
          ++iter;
        }
      }
    }

    if (remaining.empty())
      break;
  }

  return result;
}

static QPixmap LoadPreviewPixmapFresh(const QString& path)
{
  QImageReader reader(path);
  reader.setAutoTransform(true);
  const QImage image = reader.read();
  if (image.isNull())
    return {};
  return QPixmap::fromImage(image);
}
}  // namespace

ShaderHunterWidget::ShaderHunterWidget(QWidget* parent) : QDialog(parent)
{
  setWindowTitle(tr("Shader Hunter"));
  setWindowFlags(windowFlags() | Qt::Window);
  setAttribute(Qt::WA_DeleteOnClose);
  setMinimumWidth(300);

  CreateWidgets();
  ConnectSignals();

  m_update_timer = new QTimer(this);
  connect(m_update_timer, &QTimer::timeout, this, &ShaderHunterWidget::UpdateDisplay);
  m_update_timer->start(100);
}

void ShaderHunterWidget::CreateWidgets()
{
  auto* layout = new QVBoxLayout;
  m_main_layout = layout;

  m_enable_checkbox = new QCheckBox(tr("Enable Shader Hunting"));
  layout->addWidget(m_enable_checkbox);

  auto* hunting_option_layout = new QHBoxLayout;
  hunting_option_layout->addWidget(new QLabel(tr("Hunting Option:")));
  m_hunting_option_combo = new QComboBox;
  m_hunting_option_combo->addItem(tr("Skip"),
                                  static_cast<int>(ShaderHunter::HuntingOption::Skip));
  m_hunting_option_combo->addItem(tr("Pink"),
                                  static_cast<int>(ShaderHunter::HuntingOption::Pink));
  hunting_option_layout->addWidget(m_hunting_option_combo);
  layout->addLayout(hunting_option_layout);

  m_debug_log_checkbox = new QCheckBox(tr("Debug Log Draws"));
  m_debug_log_checkbox->setToolTip(
      tr("Log every draw call with projection, viewport, scissor, and shader hashes.\n"
         "Also logs EFB clear operations. Use to identify how visual elements are drawn.\n"
         "Open Log Configuration and enable INFO for Video to see the output.\n"
         "WARNING: generates a LOT of log output — enable briefly, then disable."));
  layout->addWidget(m_debug_log_checkbox);

  auto* type_layout = new QHBoxLayout;
  type_layout->addWidget(new QLabel(tr("Shader Type:")));
  m_type_combo = new QComboBox;
  m_type_combo->addItem(tr("Pixel Shader"));
  m_type_combo->addItem(tr("Vertex Shader"));
  m_type_combo->addItem(tr("Geometry Shader"));
  type_layout->addWidget(m_type_combo);
  layout->addLayout(type_layout);

  m_group_hunt_checkbox = new QCheckBox(tr("Enable Group Hunt"));
  m_group_hunt_checkbox->setToolTip(
      tr("Cycle frame draws that match the selected runtime signature groups from the seeded draw."));
  layout->addWidget(m_group_hunt_checkbox);

  auto* group_seed_layout = new QHBoxLayout;
  m_group_hunt_seed_button = new QPushButton(tr("Seed From Current Draw"));
  m_group_hunt_seed_button->setToolTip(
      tr("Use the currently highlighted draw as the group-hunt seed.\n"
         "Group hunting then matches other draws that share the checked groups."));
  group_seed_layout->addWidget(m_group_hunt_seed_button);
  group_seed_layout->addStretch(1);
  layout->addLayout(group_seed_layout);

  auto* group_mask_layout = new QHBoxLayout;
  m_group_projection_check = new QCheckBox(tr("Projection"));
  m_group_layer_check = new QCheckBox(tr("Layer"));
  m_group_viewport_check = new QCheckBox(tr("Viewport"));
  m_group_scissor_check = new QCheckBox(tr("Scissor"));
  m_group_render_state_check = new QCheckBox(tr("Render State"));
  for (QCheckBox* checkbox : {m_group_projection_check, m_group_layer_check,
                              m_group_viewport_check, m_group_scissor_check,
                              m_group_render_state_check})
  {
    group_mask_layout->addWidget(checkbox);
  }
  group_mask_layout->addStretch(1);
  layout->addLayout(group_mask_layout);

  m_group_hunt_status_label = new QLabel(tr("Group Hunt: (no seed)"));
  m_group_hunt_status_label->setWordWrap(true);
  layout->addWidget(m_group_hunt_status_label);

  m_hash_label = new QLabel(tr("Hash: (none)"));
  m_hash_label->setFont(QFont(QStringLiteral("Courier")));
  layout->addWidget(m_hash_label);

  m_position_label = new QLabel(tr("- / -"));
  layout->addWidget(m_position_label);

  auto* nav_layout = new QHBoxLayout;
  m_prev_button = new QPushButton(tr("<< Previous"));
  m_next_button = new QPushButton(tr("Next >>"));
  nav_layout->addWidget(m_prev_button);
  nav_layout->addWidget(m_next_button);
  layout->addLayout(nav_layout);

  m_draw_calls_button = new QPushButton(tr("Draw Call Filter"));
  m_draw_calls_button->setToolTip(
      tr("Open the draw-call filter tool for the selected shader.\n"
         "Use it to isolate draw calls and define a range for saved overrides."));
  layout->addWidget(m_draw_calls_button);

  m_selected_draw_call_label = new QLabel;
  m_selected_draw_call_label->setFont(QFont(QStringLiteral("Courier")));
  m_selected_draw_call_label->setVisible(false);
  layout->addWidget(m_selected_draw_call_label);

  // Save shader override section
  m_shader_name_edit = new QLineEdit;
  m_shader_name_edit->setPlaceholderText(tr("Enter shader name..."));
  layout->addWidget(m_shader_name_edit);

  auto* handling_layout = new QHBoxLayout;
  handling_layout->addWidget(new QLabel(tr("Override Handling:")));
  m_handling_combo = new QComboBox;
  m_handling_combo->addItem(tr("Skip Draw"), static_cast<int>(ShaderHunter::HandlingType::Skip));
  m_handling_combo->addItem(tr("Screen"), static_cast<int>(ShaderHunter::HandlingType::Screen));
  m_handling_combo->addItem(tr("Fullscreen"),
                            static_cast<int>(ShaderHunter::HandlingType::Fullscreen));
  m_handling_combo->addItem(tr("Fullscreen Mono"),
                            static_cast<int>(ShaderHunter::HandlingType::FullscreenMono));
  m_handling_combo->addItem(tr("Head Locked"),
                            static_cast<int>(ShaderHunter::HandlingType::HeadLocked));
  m_handling_combo->addItem(tr("Flag"), static_cast<int>(ShaderHunter::HandlingType::Flag));
  m_handling_combo->addItem(tr("Units per Meter"),
                            static_cast<int>(ShaderHunter::HandlingType::UnitsPerMeter));
  handling_layout->addWidget(m_handling_combo);
  layout->addLayout(handling_layout);

  auto* upm_layout = new QHBoxLayout;
  m_units_per_meter_label = new QLabel(tr("Units per Meter:"));
  m_units_per_meter_spin = new QDoubleSpinBox;
  m_units_per_meter_spin->setRange(Config::GFX_VR_UNITS_PER_METER_MIN,
                                   Config::GFX_VR_UNITS_PER_METER_MAX);
  m_units_per_meter_spin->setDecimals(2);
  m_units_per_meter_spin->setSingleStep(Config::GFX_VR_UNITS_PER_METER_STEP);
  m_units_per_meter_spin->setValue(1.0);
  m_units_per_meter_spin->setToolTip(
      tr("Temporary per-shader scale override for VR when handling is Units per Meter."));
  upm_layout->addWidget(m_units_per_meter_label);
  upm_layout->addWidget(m_units_per_meter_spin);
  layout->addLayout(upm_layout);

  m_save_button = new QPushButton(tr("Save Shader"));
  layout->addWidget(m_save_button);

  m_dump_button = new QPushButton(tr("Dump Shader"));
  layout->addWidget(m_dump_button);

  m_textures_button = new QPushButton(tr("View Textures"));
  m_textures_button->setToolTip(
      tr("Show texture hashes used by the currently selected shader.\n"
         "You can toggle texture-based skipping to isolate specific textures."));
  layout->addWidget(m_textures_button);

  auto* texture_mode_layout = new QHBoxLayout;
  texture_mode_layout->addWidget(new QLabel(tr("Texture Filter Mode:")));
  m_texture_filter_mode_combo = new QComboBox;
  m_texture_filter_mode_combo->addItem(tr("Include"), false);
  m_texture_filter_mode_combo->addItem(tr("Exclude"), true);
  m_texture_filter_mode_combo->setToolTip(
      tr("Include: apply override only when selected textures are present.\n"
         "Exclude: apply override only when selected textures are absent."));
  texture_mode_layout->addWidget(m_texture_filter_mode_combo);
  layout->addLayout(texture_mode_layout);

  m_selected_texture_label = new QLabel;
  m_selected_texture_label->setFont(QFont(QStringLiteral("Courier")));
  m_selected_texture_label->setVisible(false);
  layout->addWidget(m_selected_texture_label);

  setLayout(m_main_layout);
}

void ShaderHunterWidget::ConnectSignals()
{
  connect(m_enable_checkbox, &QCheckBox::toggled, this, [this](bool checked) {
    ShaderHunter::GetInstance().SetEnabled(checked);
    m_hunting_option_combo->setEnabled(checked);
  });
  connect(m_hunting_option_combo, qOverload<int>(&QComboBox::currentIndexChanged), this,
          [this](int) {
            auto& hunter = ShaderHunter::GetInstance();
            const auto option = static_cast<ShaderHunter::HuntingOption>(
                m_hunting_option_combo->currentData().toInt());
            hunter.SetHuntingOption(option);
          });
  connect(m_debug_log_checkbox, &QCheckBox::toggled, this, [](bool checked) {
    ShaderHunter::GetInstance().SetDebugLogging(checked);
  });
  connect(m_type_combo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
    SetSelectedDrawCallRange(-1, -1, 0);
    m_saved_texture_filters.clear();
    SetSelectedTextureHashes({});
    ShaderHunter::GetInstance().SetActiveType(static_cast<ShaderHunter::ShaderType>(index));
    UpdateGroupHuntUi();
  });
  connect(m_prev_button, &QPushButton::clicked, this, [this] {
    SetSelectedDrawCallRange(-1, -1, 0);
    m_saved_texture_filters.clear();
    SetSelectedTextureHashes({});
    ShaderHunter::GetInstance().PrevShader();
    UpdateDisplay();
  });
  connect(m_next_button, &QPushButton::clicked, this, [this] {
    SetSelectedDrawCallRange(-1, -1, 0);
    m_saved_texture_filters.clear();
    SetSelectedTextureHashes({});
    ShaderHunter::GetInstance().NextShader();
    UpdateDisplay();
  });
  connect(m_save_button, &QPushButton::clicked, this, &ShaderHunterWidget::SaveCurrentShader);
  connect(m_dump_button, &QPushButton::clicked, this, &ShaderHunterWidget::DumpCurrentShader);
  connect(m_textures_button, &QPushButton::clicked, this, &ShaderHunterWidget::ShowTexturesDialog);
  connect(m_draw_calls_button, &QPushButton::clicked, this, &ShaderHunterWidget::ShowDrawCallDialog);
  connect(m_texture_filter_mode_combo, qOverload<int>(&QComboBox::currentIndexChanged), this,
          [this](int) { SetSelectedTextureHashes(m_saved_texture_filters); });
  connect(m_group_hunt_checkbox, &QCheckBox::toggled, this, [this](bool checked) {
    auto& hunter = ShaderHunter::GetInstance();
    hunter.SetGroupHuntEnabled(checked);
    UpdateGroupHuntUi();
    UpdateDisplay();
  });
  connect(m_group_hunt_seed_button, &QPushButton::clicked, this, [this]() {
    auto& hunter = ShaderHunter::GetInstance();
    if (!hunter.SeedGroupHuntFromCurrentDraw())
    {
      QMessageBox::warning(
          this, tr("Group Hunt"),
          tr("No current draw is available to seed group hunt.\n"
             "Use Shader Hunter or Draw Call Filter to isolate a draw first."));
      return;
    }
    const auto signature = hunter.GetGroupHuntSeedSignature();
    {
      const QSignalBlocker projection_blocker(m_group_projection_check);
      const QSignalBlocker layer_blocker(m_group_layer_check);
      const QSignalBlocker viewport_blocker(m_group_viewport_check);
      const QSignalBlocker scissor_blocker(m_group_scissor_check);
      const QSignalBlocker render_state_blocker(m_group_render_state_check);
      m_group_projection_check->setChecked(signature.use_projection);
      m_group_layer_check->setChecked(signature.use_layer);
      m_group_viewport_check->setChecked(signature.use_viewport);
      m_group_scissor_check->setChecked(signature.use_scissor);
      m_group_render_state_check->setChecked(signature.use_render_state);
    }
    UpdateGroupHuntUi();
    UpdateDisplay();
  });
  for (QCheckBox* checkbox : {m_group_projection_check, m_group_layer_check,
                              m_group_viewport_check, m_group_scissor_check,
                              m_group_render_state_check})
  {
    connect(checkbox, &QCheckBox::toggled, this, [this]() {
      auto& hunter = ShaderHunter::GetInstance();
      hunter.SetGroupHuntGroupMask(m_group_projection_check->isChecked(),
                                   m_group_layer_check->isChecked(),
                                   m_group_viewport_check->isChecked(),
                                   m_group_scissor_check->isChecked(),
                                   m_group_render_state_check->isChecked());
      UpdateGroupHuntUi();
      UpdateDisplay();
    });
  }
  connect(m_handling_combo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
    const auto handling =
        static_cast<ShaderHunter::HandlingType>(m_handling_combo->currentData().toInt());
    const bool show_upm = handling == ShaderHunter::HandlingType::UnitsPerMeter;
    m_units_per_meter_label->setVisible(show_upm);
    m_units_per_meter_spin->setVisible(show_upm);
  });
  const auto handling =
      static_cast<ShaderHunter::HandlingType>(m_handling_combo->currentData().toInt());
  const bool show_upm = handling == ShaderHunter::HandlingType::UnitsPerMeter;
  m_units_per_meter_label->setVisible(show_upm);
  m_units_per_meter_spin->setVisible(show_upm);

  const auto hunting_option = ShaderHunter::GetInstance().GetHuntingOption();
  const int hunting_idx = m_hunting_option_combo->findData(static_cast<int>(hunting_option));
  if (hunting_idx >= 0)
    m_hunting_option_combo->setCurrentIndex(hunting_idx);
  m_hunting_option_combo->setEnabled(ShaderHunter::GetInstance().IsEnabled());
  UpdateGroupHuntUi();
}

void ShaderHunterWidget::UpdateDisplay()
{
  auto& hunter = ShaderHunter::GetInstance();
  const auto status = hunter.GetHuntingStatusForOSD();
  UpdateGroupHuntUi();

  if (status.group_hunt_enabled && status.group_hunt_seed_valid &&
      (status.group_hunt_seed_signature.use_projection || status.group_hunt_seed_signature.use_layer ||
       status.group_hunt_seed_signature.use_viewport || status.group_hunt_seed_signature.use_scissor ||
       status.group_hunt_seed_signature.use_render_state))
  {
    if (status.group_hunt_current_hash != 0)
    {
      m_hash_label->setText(
          tr("Hash: 0x%1")
              .arg(static_cast<uint>(status.group_hunt_current_hash), 8, 16, QLatin1Char('0'))
              .toUpper());
    }
    else
    {
      m_hash_label->setText(tr("Hash: (no group match)"));
    }

    if (status.group_hunt_total_matches > 0)
    {
      m_position_label->setText(
          tr("%1 / %2")
              .arg(status.group_hunt_selected_match + 1)
              .arg(status.group_hunt_total_matches));
      m_selected_draw_call_label->setVisible(true);
      if (status.group_hunt_draw_index >= 0)
      {
        m_selected_draw_call_label->setText(
            tr("Group Match Draw:\n#%1").arg(status.group_hunt_draw_index + 1));
      }
      else
      {
        m_selected_draw_call_label->setText(tr("Group Match Draw:\n(not seen this frame)"));
      }
    }
    else
    {
      m_position_label->setText(tr("- / 0"));
      m_selected_draw_call_label->setVisible(true);
      m_selected_draw_call_label->setText(tr("Group Match Draw:\n(no matches)"));
    }
    return;
  }

  const int pos = hunter.GetSelectedPosition();
  const int total = hunter.GetTotalCount();
  const u64 hash = hunter.GetSelectedHash();

  if (pos >= 0 && total > 0)
  {
    m_hash_label->setText(
        tr("Hash: 0x%1").arg(static_cast<uint>(hash), 8, 16, QLatin1Char('0')).toUpper());
    m_position_label->setText(tr("%1 / %2").arg(pos + 1).arg(total));
  }
  else
  {
    m_hash_label->setText(tr("Hash: (none)"));
    m_position_label->setText(tr("- / %1").arg(total));
  }
}

void ShaderHunterWidget::UpdateGroupHuntUi()
{
  auto& hunter = ShaderHunter::GetInstance();
  const auto status = hunter.GetHuntingStatusForOSD();
  const bool enabled = status.group_hunt_enabled;
  const bool has_seed = status.group_hunt_seed_valid;
  const bool has_groups = has_seed &&
                          (status.group_hunt_seed_signature.use_projection ||
                           status.group_hunt_seed_signature.use_layer ||
                           status.group_hunt_seed_signature.use_viewport ||
                           status.group_hunt_seed_signature.use_scissor ||
                           status.group_hunt_seed_signature.use_render_state);

  {
    const QSignalBlocker blocker(m_group_hunt_checkbox);
    m_group_hunt_checkbox->setChecked(enabled);
  }

  const auto seed_signature = hunter.GetGroupHuntSeedSignature();
  {
    const QSignalBlocker projection_blocker(m_group_projection_check);
    const QSignalBlocker layer_blocker(m_group_layer_check);
    const QSignalBlocker viewport_blocker(m_group_viewport_check);
    const QSignalBlocker scissor_blocker(m_group_scissor_check);
    const QSignalBlocker render_state_blocker(m_group_render_state_check);
    m_group_projection_check->setChecked(seed_signature.use_projection);
    m_group_layer_check->setChecked(seed_signature.use_layer);
    m_group_viewport_check->setChecked(seed_signature.use_viewport);
    m_group_scissor_check->setChecked(seed_signature.use_scissor);
    m_group_render_state_check->setChecked(seed_signature.use_render_state);
  }

  for (QCheckBox* checkbox : {m_group_projection_check, m_group_layer_check,
                              m_group_viewport_check, m_group_scissor_check,
                              m_group_render_state_check})
  {
    checkbox->setEnabled(has_seed);
  }

  m_group_hunt_seed_button->setEnabled(hunter.IsEnabled());
  m_type_combo->setEnabled(!enabled);

  const char* seed_type = status.group_hunt_seed_type == ShaderHunter::ShaderType::Pixel   ? "PS" :
                          status.group_hunt_seed_type == ShaderHunter::ShaderType::Vertex  ? "VS" :
                                                                                             "GS";
  if (!has_seed)
  {
    m_group_hunt_status_label->setText(tr("Group Hunt: (no seed)"));
  }
  else if (!has_groups)
  {
    m_group_hunt_status_label->setText(
        tr("Group Hunt Seed: %1 0x%2\nSelect at least one signature group.")
            .arg(QString::fromLatin1(seed_type))
            .arg(static_cast<uint>(status.group_hunt_seed_hash), 8, 16, QLatin1Char('0')));
  }
  else
  {
    m_group_hunt_status_label->setText(
        tr("Group Hunt Seed: %1 0x%2\nMatches: %3 / %4")
            .arg(QString::fromLatin1(seed_type))
            .arg(static_cast<uint>(status.group_hunt_seed_hash), 8, 16, QLatin1Char('0'))
            .arg(status.group_hunt_total_matches > 0 ? status.group_hunt_selected_match + 1 : 0)
            .arg(status.group_hunt_total_matches));
  }
}
void ShaderHunterWidget::SaveCurrentShader()
{
  auto& hunter = ShaderHunter::GetInstance();
  const int pos = hunter.GetSelectedPosition();
  if (pos < 0)
  {
    QMessageBox::warning(this, tr("Save Shader"), tr("No shader selected. Use hunting to select a shader first."));
    return;
  }

  const std::string game_id = SConfig::GetInstance().GetGameID();
  if (game_id.empty())
  {
    QMessageBox::warning(this, tr("Save Shader"), tr("No game is currently running."));
    return;
  }

  // Load existing overrides first so we can auto-generate a unique name when empty.
  auto all = ShaderHunter::LoadOverridesFromINI(game_id);

  std::string name = m_shader_name_edit->text().toStdString();
  if (name.empty())
  {
    constexpr std::string_view unnamed_prefix = "Unnamed Shader";
    int max_unnamed_index = 0;
    for (const auto& existing : all)
    {
      const std::string& existing_name = existing.name;
      if (existing_name == unnamed_prefix)
      {
        max_unnamed_index = std::max(max_unnamed_index, 1);
        continue;
      }

      if (!existing_name.starts_with(unnamed_prefix) || existing_name.size() <= unnamed_prefix.size())
        continue;

      if (existing_name[unnamed_prefix.size()] != ' ')
        continue;

      const std::string suffix = existing_name.substr(unnamed_prefix.size() + 1);
      if (suffix.empty() ||
          !std::all_of(suffix.begin(), suffix.end(),
                       [](unsigned char c) { return std::isdigit(c) != 0; }))
      {
        continue;
      }

      max_unnamed_index = std::max(max_unnamed_index, std::stoi(suffix));
    }

    name = fmt::format("Unnamed Shader {}", max_unnamed_index + 1);
  }

  u64 hash = hunter.GetSelectedHash();
  auto type = hunter.GetActiveType();
  const auto hunting_status = hunter.GetHuntingStatusForOSD();
  const bool group_hunt_ready =
      hunting_status.group_hunt_enabled && hunting_status.group_hunt_seed_valid &&
      (hunting_status.group_hunt_seed_signature.use_projection ||
       hunting_status.group_hunt_seed_signature.use_layer ||
       hunting_status.group_hunt_seed_signature.use_viewport ||
       hunting_status.group_hunt_seed_signature.use_scissor ||
       hunting_status.group_hunt_seed_signature.use_render_state);
  const auto hunted_draw = hunter.GetGroupHuntHighlightedDraw();
  if (group_hunt_ready && !hunted_draw.has_value())
  {
    QMessageBox::warning(this, tr("Save Shader"),
                         tr("Group Hunt is active, but no matching draw is available in the current frame."));
    return;
  }
  if (hunted_draw.has_value())
  {
    hash = hunted_draw->hash;
    type = hunted_draw->type;
  }
  const auto handling =
      static_cast<ShaderHunter::HandlingType>(m_handling_combo->currentData().toInt());

  // Build override with element range if set
  ShaderHunter::ShaderOverride entry;
  entry.name = name;
  entry.hash = hash;
  entry.type = type;
  entry.handling = handling;
  entry.enabled = true;
  entry.user_defined = true;
  if (hunted_draw.has_value())
  {
    entry.match_mode = ShaderHunter::MatchMode::RuntimeElement;
    entry.runtime_element = hunted_draw->signature;
    if (type == ShaderHunter::ShaderType::Pixel)
    {
      if (const auto family_signature = hunter.GetShaderFamilySignature(type, hash);
          family_signature.has_value())
      {
        entry.hash_family_match = true;
        entry.family_signature = *family_signature;
      }
    }
  }
  else if (const auto runtime_signature = hunter.GetSelectedRuntimeElementSignature();
           runtime_signature)
  {
    entry.match_mode = ShaderHunter::MatchMode::RuntimeElement;
    entry.runtime_element = *runtime_signature;
    if (type == ShaderHunter::ShaderType::Pixel)
    {
      if (const auto family_signature = hunter.GetShaderFamilySignature(type, hash);
          family_signature.has_value())
      {
        entry.hash_family_match = true;
        entry.family_signature = *family_signature;
      }
    }
  }
  if (handling == ShaderHunter::HandlingType::UnitsPerMeter)
    entry.units_per_meter = static_cast<float>(m_units_per_meter_spin->value());
  if (handling == ShaderHunter::HandlingType::Flag)
    entry.flag_group = name;

  // Include draw-call range saved from Draw Call Filter popup (if any),
  // otherwise use the current hunting range.
  if (m_saved_element_start >= 0 && m_saved_element_end >= 0)
  {
    entry.element_start = m_saved_element_start;
    entry.element_end = m_saved_element_end;
    entry.element_reference_total = m_saved_element_total;
  }
  else if (hunter.IsElementMode())
  {
    const int range_start = hunter.GetElementStart();
    const int range_end = hunter.GetElementEnd();
    if (range_start >= 0 && range_end >= 0)
    {
      entry.element_start = range_start;
      entry.element_end = range_end;
      entry.element_reference_total = hunter.GetElementTotal();
    }

    // Auto-capture texture hash from the current element cursor
    const int elem = hunter.GetSelectedElement();
    if (elem >= 0)
    {
      const auto textures = hunter.GetElementTextures(elem);
      // Use the first non-zero texture hash (stage 0 is typically the main texture)
      for (u64 th : textures)
      {
        if (th != 0)
        {
          entry.texture_hashes.push_back(th);
          break;
        }
      }
    }
  }
  else if (m_saved_element_start >= 0 && m_saved_element_end >= 0 && hunter.IsElementMode())
  {
    // Preserve auto texture capture when range was saved from the popup.
    const int elem = hunter.GetSelectedElement();
    if (elem >= 0)
    {
      const auto textures = hunter.GetElementTextures(elem);
      for (u64 th : textures)
      {
        if (th != 0)
        {
          entry.texture_hashes.push_back(th);
          break;
        }
      }
    }
  }

  if (hunted_draw.has_value())
  {
    for (u64 th : hunted_draw->textures)
    {
      if (th != 0)
      {
        entry.texture_hashes.push_back(th);
        break;
      }
    }
  }

  // Include texture filters saved from the Texture Hunter popup (if any).
  entry.texture_hashes.insert(entry.texture_hashes.end(), m_saved_texture_filters.begin(),
                              m_saved_texture_filters.end());
  std::sort(entry.texture_hashes.begin(), entry.texture_hashes.end());
  entry.texture_hashes.erase(
      std::unique(entry.texture_hashes.begin(), entry.texture_hashes.end()),
      entry.texture_hashes.end());
  entry.texture_hashes_excluded =
      !entry.texture_hashes.empty() && m_texture_filter_mode_combo->currentData().toBool();

  // Append and save
  all.push_back(entry);
  ShaderHunter::SaveOverridesToINI(game_id, all);

  // Reload runtime overrides
  hunter.LoadOverrides(game_id);
  emit OverridesChanged();
  Settings::Instance().NotifyShaderOverridesChanged();

  const char* type_str = type == ShaderHunter::ShaderType::Pixel    ? "PS" :
                         type == ShaderHunter::ShaderType::Vertex   ? "VS" :
                                                                      "GS";
  const char* handling_str = handling == ShaderHunter::HandlingType::Screen     ? "screen" :
                             handling == ShaderHunter::HandlingType::Fullscreen ? "fullscreen" :
                             handling == ShaderHunter::HandlingType::FullscreenMono ?
                                 "fullscreen_mono" :
                             handling == ShaderHunter::HandlingType::HeadLocked ? "headlocked" :
                             handling == ShaderHunter::HandlingType::Flag       ? "flag" :
                             handling == ShaderHunter::HandlingType::UnitsPerMeter ?
                                 "units_per_meter" :
                                                                                  "skip";
  QString msg = tr("Saved shader override '%1' (%2, hash 0x%3, handling: %4)")
      .arg(QString::fromStdString(name))
      .arg(QString::fromLatin1(type_str))
      .arg(static_cast<uint>(hash), 8, 16, QLatin1Char('0'))
      .arg(QString::fromLatin1(handling_str));

  if (entry.element_start >= 0 && entry.element_end >= 0)
    msg += tr("\nElement range: %1 - %2").arg(entry.element_start + 1).arg(entry.element_end + 1);

  if (handling == ShaderHunter::HandlingType::UnitsPerMeter && entry.units_per_meter > 0.0f)
    msg += tr("\nUnits per Meter: %1").arg(entry.units_per_meter, 0, 'f', 2);

  if (!entry.texture_hashes.empty())
  {
    QStringList hash_list;
    for (u64 texture_hash : entry.texture_hashes)
      hash_list.append(QString::number(texture_hash, 16).rightJustified(16, QLatin1Char('0')));
    const QString mode_text = entry.texture_hashes_excluded ? tr("Exclude") : tr("Include");
    msg += tr("\nTexture filter mode: %1").arg(mode_text);
    msg += tr("\nTexture filter(s): %1").arg(hash_list.join(QStringLiteral(", ")));
  }

  msg += tr("\nSaved to %1.ini").arg(QString::fromStdString(game_id));

  QMessageBox::information(this, tr("Save Shader"), msg);
  m_shader_name_edit->clear();
  SetSelectedDrawCallRange(-1, -1, 0);
  m_saved_texture_filters.clear();
  m_texture_filter_mode_combo->setCurrentIndex(0);
  SetSelectedTextureHashes({});
}

void ShaderHunterWidget::closeEvent(QCloseEvent* event)
{
  auto& hunter = ShaderHunter::GetInstance();
  hunter.SetElementMode(false);
  hunter.SetGroupHuntEnabled(false);
  hunter.SetEnabled(false);
  SetSelectedDrawCallRange(-1, -1, 0);
  m_saved_texture_filters.clear();
  m_texture_filter_mode_combo->setCurrentIndex(0);
  SetSelectedTextureHashes({});
  QDialog::closeEvent(event);
}

void ShaderHunterWidget::DumpCurrentShader()
{
  auto& hunter = ShaderHunter::GetInstance();
  const int pos = hunter.GetSelectedPosition();
  if (pos < 0)
  {
    QMessageBox::warning(this, tr("Dump Shader"),
                         tr("No shader selected. Use hunting to select a shader first."));
    return;
  }

  const std::string game_id = SConfig::GetInstance().GetGameID();
  if (game_id.empty())
  {
    QMessageBox::warning(this, tr("Dump Shader"), tr("No game is currently running."));
    return;
  }

  const u64 hash = hunter.GetSelectedHash();
  const auto type = hunter.GetActiveType();

  if (hunter.DumpShader(game_id, type, hash))
  {
    const char* type_suffix = type == ShaderHunter::ShaderType::Pixel    ? "ps" :
                              type == ShaderHunter::ShaderType::Vertex   ? "vs" :
                                                                           "gs";
    QMessageBox::information(
        this, tr("Dump Shader"),
        tr("Dumped shader to Dump/Shaders/%1/%2-%3.txt")
            .arg(QString::fromStdString(game_id))
            .arg(static_cast<uint>(hash), 8, 16, QLatin1Char('0'))
            .arg(QString::fromLatin1(type_suffix)));
  }
  else
  {
    QMessageBox::warning(this, tr("Dump Shader"),
                         tr("Failed to dump shader. The shader UID may not be cached yet.\n"
                            "Make sure shader hunting is enabled and the shader has been "
                            "seen in the current session."));
  }
}

void ShaderHunterWidget::ShowDrawCallDialog()
{
  auto& hunter = ShaderHunter::GetInstance();
  const int pos = hunter.GetSelectedPosition();
  if (pos < 0)
  {
    QMessageBox::warning(this, tr("Draw Call Filter"),
                         tr("No shader selected. Enable hunting and select a shader first."));
    return;
  }

  const u64 hash = hunter.GetSelectedHash();
  const auto type = hunter.GetActiveType();
  const char* type_str = type == ShaderHunter::ShaderType::Pixel    ? "PS" :
                         type == ShaderHunter::ShaderType::Vertex   ? "VS" :
                                                                       "GS";

  auto* dlg = new QDialog(this);
  dlg->setWindowTitle(tr("Draw Call Filter for %1 %2")
                          .arg(QString::fromLatin1(type_str))
                          .arg(static_cast<uint>(hash), 8, 16, QLatin1Char('0')));
  dlg->setAttribute(Qt::WA_DeleteOnClose);
  dlg->setMinimumSize(560, 320);

  auto* info_label = new QLabel(
      tr("Use draw-call filtering to isolate one draw at a time or define a start/end range.\n"
         "Saved overrides will include the current draw-call range."));
  info_label->setWordWrap(true);

  auto* enable_check = new QCheckBox(tr("Enable Draw Call Filter"));
  enable_check->setChecked(hunter.IsElementMode());

  auto* draw_call_label = new QLabel(tr("Draw Call: - / -"));
  auto* range_label = new QLabel(tr("Range: (not set)"));
  auto* textures_label = new QLabel(tr("Textures: (none)"));
  textures_label->setFont(QFont(QStringLiteral("Courier"), 8));
  textures_label->setWordWrap(true);

  auto* prev_button = new QPushButton(tr("<< Prev Element"));
  auto* next_button = new QPushButton(tr("Next Element >>"));
  auto* mark_start_button = new QPushButton(tr("Mark Start"));
  auto* mark_end_button = new QPushButton(tr("Mark End"));
  auto* clear_range_button = new QPushButton(tr("Clear Range"));

  mark_start_button->setToolTip(tr("Set the range start to the current element cursor position."));
  mark_end_button->setToolTip(tr("Set the range end to the current element cursor position."));
  clear_range_button->setToolTip(tr("Clear the range and go back to single-element skipping."));

  auto* nav_layout = new QHBoxLayout;
  nav_layout->addWidget(prev_button);
  nav_layout->addWidget(next_button);

  auto* range_buttons_layout = new QHBoxLayout;
  range_buttons_layout->addWidget(mark_start_button);
  range_buttons_layout->addWidget(mark_end_button);
  range_buttons_layout->addWidget(clear_range_button);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
  auto* save_button = buttons->addButton(tr("Save To Shader"), QDialogButtonBox::ActionRole);
  connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::close);

  const auto update_ui = [enable_check, draw_call_label, range_label, textures_label, prev_button,
                          next_button, mark_start_button, mark_end_button, clear_range_button,
                          &hunter]() {
    const bool active = hunter.IsElementMode();
    {
      const QSignalBlocker blocker(enable_check);
      enable_check->setChecked(active);
    }

    prev_button->setEnabled(active);
    next_button->setEnabled(active);
    mark_start_button->setEnabled(active);
    mark_end_button->setEnabled(active);
    clear_range_button->setEnabled(active);

    if (!active)
    {
      draw_call_label->setText(QObject::tr("Draw Call: (disabled)"));
      range_label->setText(QObject::tr("Range: (disabled)"));
      textures_label->setText(QObject::tr("Textures: (disabled)"));
      return;
    }

    const int elem = hunter.GetSelectedElement();
    const int elem_total = hunter.GetElementTotal();
    const int range_start = hunter.GetElementStart();
    const int range_end = hunter.GetElementEnd();

    if (elem_total > 0)
      draw_call_label->setText(QObject::tr("Draw Call: %1 / %2").arg(elem + 1).arg(elem_total));
    else
      draw_call_label->setText(QObject::tr("Draw Call: - / 0"));

    if (range_start >= 0 && range_end >= 0)
    {
      const int count = range_end - range_start + 1;
      range_label->setText(QObject::tr("Range: %1 - %2 (%3 draw(s) skipped)")
                               .arg(range_start + 1)
                               .arg(range_end + 1)
                               .arg(count));
    }
    else
    {
      range_label->setText(QObject::tr("Range: (not set - skipping single element)"));
    }

    if (elem >= 0 && elem_total > 0)
    {
      const auto tex_names = hunter.GetElementTextureNames(elem);
      const auto tex_hashes = hunter.GetElementTextures(elem);
      QStringList tex_list;
      for (int i = 0; i < 8; i++)
      {
        if (tex_hashes[i] == 0)
          continue;

        const QString name = QString::fromStdString(tex_names[i]);
        if (!name.isEmpty())
        {
          tex_list.append(QStringLiteral("[%1] %2").arg(i).arg(name));
        }
        else
        {
          tex_list.append(QObject::tr("[%1] %2")
                              .arg(i)
                              .arg(QString::number(tex_hashes[i], 16)
                                       .rightJustified(16, QLatin1Char('0'))));
        }
      }

      if (tex_list.isEmpty())
        textures_label->setText(QObject::tr("Textures: (none)"));
      else
        textures_label->setText(QObject::tr("Textures:\n%1").arg(tex_list.join(QStringLiteral("\n"))));
    }
    else
    {
      textures_label->setText(QObject::tr("Textures: (none)"));
    }
  };

  connect(enable_check, &QCheckBox::toggled, dlg, [&hunter, update_ui](bool checked) {
    hunter.SetElementMode(checked);
    update_ui();
  });
  connect(prev_button, &QPushButton::clicked, dlg, [&hunter, update_ui]() {
    hunter.PrevElement();
    update_ui();
  });
  connect(next_button, &QPushButton::clicked, dlg, [&hunter, update_ui]() {
    hunter.NextElement();
    update_ui();
  });
  connect(mark_start_button, &QPushButton::clicked, dlg, [&hunter, update_ui]() {
    hunter.MarkRangeStart();
    update_ui();
  });
  connect(mark_end_button, &QPushButton::clicked, dlg, [&hunter, update_ui]() {
    hunter.MarkRangeEnd();
    update_ui();
  });
  connect(clear_range_button, &QPushButton::clicked, dlg, [&hunter, update_ui]() {
    hunter.ClearRange();
    update_ui();
  });
  connect(save_button, &QPushButton::clicked, dlg, [this, &hunter, dlg]() {
    if (!hunter.IsElementMode())
    {
      QMessageBox::warning(
          dlg, tr("Draw Call Filter"),
          tr("Enable Draw Call Filter first, then choose a draw call or range."));
      return;
    }

    int range_start = hunter.GetElementStart();
    int range_end = hunter.GetElementEnd();
    if (range_start < 0 || range_end < 0)
    {
      const int elem = hunter.GetSelectedElement();
      if (elem < 0)
      {
        QMessageBox::warning(dlg, tr("Draw Call Filter"),
                             tr("No draw call is selected yet."));
        return;
      }
      range_start = elem;
      range_end = elem;
    }

    SetSelectedDrawCallRange(range_start, range_end, hunter.GetElementTotal());

    QMessageBox::information(
        dlg, tr("Draw Calls Saved"),
        tr("Saved draw call range %1 - %2 to the current shader.\n"
           "Press 'Save Shader' in Shader Hunter to write it to the config.")
            .arg(range_start + 1)
            .arg(range_end + 1));
  });

  auto* timer = new QTimer(dlg);
  timer->setInterval(100);
  connect(timer, &QTimer::timeout, dlg, update_ui);
  timer->start();

  auto* layout = new QVBoxLayout;
  layout->addWidget(info_label);
  layout->addWidget(enable_check);
  layout->addWidget(draw_call_label);
  layout->addLayout(nav_layout);
  layout->addWidget(range_label);
  layout->addLayout(range_buttons_layout);
  layout->addWidget(textures_label);
  layout->addWidget(buttons);
  dlg->setLayout(layout);

  update_ui();
  dlg->show();
}

void ShaderHunterWidget::ShowTexturesDialog()
{
  auto& hunter = ShaderHunter::GetInstance();
  const int pos = hunter.GetSelectedPosition();
  if (pos < 0)
  {
    QMessageBox::warning(this, tr("View Textures"),
                         tr("No shader selected. Enable hunting and select a shader first."));
    return;
  }

  const u64 hash = hunter.GetSelectedHash();
  const auto type = hunter.GetActiveType();
  const char* type_str = type == ShaderHunter::ShaderType::Pixel    ? "PS" :
                         type == ShaderHunter::ShaderType::Vertex   ? "VS" :
                                                                       "GS";
  hunter.SetTextureToolActive(true);

  auto* dlg = new QDialog(this);
  dlg->setWindowTitle(tr("Textures for %1 %2")
      .arg(QString::fromLatin1(type_str))
      .arg(static_cast<uint>(hash), 8, 16, QLatin1Char('0')));
  dlg->setAttribute(Qt::WA_DeleteOnClose);
  dlg->setMinimumSize(620, 320);

  auto* tree = new QTreeWidget;
  tree->setHeaderLabels({tr("Skip"), tr("Texture Hash"), tr("Name"), tr("Preview")});
  tree->setRootIsDecorated(false);
  tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
  tree->header()->setStretchLastSection(true);
  tree->setIconSize(QSize(64, 64));

  auto* info = new QLabel;
  info->setWordWrap(true);
  auto* continuous_scan_check = new QCheckBox(tr("Continuous Scan"));
  continuous_scan_check->setToolTip(
      tr("Continuously refresh the list while this window is open.\n"
         "Textures seen at least once during scan stay visible in gray."));
  auto* scan_timer = new QTimer(dlg);
  scan_timer->setInterval(250);
  auto seen_textures = std::make_shared<std::unordered_map<u64, std::string>>();

  connect(dlg, &QObject::destroyed, this, []() {
    ShaderHunter::GetInstance().SetTextureToolActive(false);
  });

  const auto update_selected_label = [this, tree]() {
    std::vector<u64> selected_hashes;
    selected_hashes.reserve(tree->topLevelItemCount());
    for (int i = 0; i < tree->topLevelItemCount(); i++)
    {
      QTreeWidgetItem* item = tree->topLevelItem(i);
      if (!item || item->checkState(0) != Qt::Checked)
        continue;
      const u64 texture_hash = item->data(0, Qt::UserRole).toULongLong();
      if (texture_hash != 0)
        selected_hashes.push_back(texture_hash);
    }
    std::sort(selected_hashes.begin(), selected_hashes.end());
    selected_hashes.erase(std::unique(selected_hashes.begin(), selected_hashes.end()),
                          selected_hashes.end());
    SetSelectedTextureHashes(std::vector<uint64_t>(selected_hashes.begin(), selected_hashes.end()));
  };

  const auto populate = [tree, info, continuous_scan_check, seen_textures, &hunter, type_str,
                         hash]() {
    const auto textures = hunter.GetTexturesForSelectedShader();
    std::unordered_map<u64, std::string> current_textures;
    current_textures.reserve(textures.size());
    for (const auto& tex : textures)
      current_textures.emplace(tex.hash, tex.name);

    if (continuous_scan_check->isChecked())
    {
      for (const auto& tex : textures)
      {
        auto& saved_name = (*seen_textures)[tex.hash];
        if (saved_name.empty() && !tex.name.empty())
          saved_name = tex.name;
      }
    }

    std::vector<u64> texture_hashes;
    texture_hashes.reserve(current_textures.size() + seen_textures->size());
    for (const auto& [texture_hash, _] : current_textures)
      texture_hashes.push_back(texture_hash);
    for (const auto& [texture_hash, _] : *seen_textures)
      texture_hashes.push_back(texture_hash);
    std::sort(texture_hashes.begin(), texture_hashes.end());
    texture_hashes.erase(std::unique(texture_hashes.begin(), texture_hashes.end()),
                         texture_hashes.end());
    const auto preview_paths = FindTexturePreviewPaths(texture_hashes);

    const QSignalBlocker blocker(tree);
    tree->clear();

    int seen_only_count = 0;
    for (u64 texture_hash : texture_hashes)
    {
      const auto current_it = current_textures.find(texture_hash);
      const bool in_current_frame = current_it != current_textures.end();
      if (!in_current_frame)
        seen_only_count++;

      auto* item = new QTreeWidgetItem;
      item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
      item->setCheckState(0, hunter.IsTextureSkipEnabled(texture_hash) ? Qt::Checked : Qt::Unchecked);
      item->setText(1,
                    QStringLiteral("%1").arg(static_cast<qulonglong>(texture_hash), 16, 16,
                                             QLatin1Char('0')));
      if (in_current_frame)
      {
        const std::string& name = current_it->second;
        item->setText(2, name.empty() ? QObject::tr("(unknown)") : QString::fromStdString(name));
      }
      else
      {
        const auto seen_it = seen_textures->find(texture_hash);
        const bool has_seen_name = seen_it != seen_textures->end() && !seen_it->second.empty();
        item->setText(2, has_seen_name ? QString::fromStdString(seen_it->second) :
                                         QObject::tr("(seen during scan, not in current frame)"));
        const QBrush gray_brush(QColor(140, 140, 140));
        item->setForeground(1, gray_brush);
        item->setForeground(2, gray_brush);
      }
      item->setData(0, Qt::UserRole, static_cast<qulonglong>(texture_hash));

      auto preview_it = preview_paths.find(texture_hash);
      if (preview_it != preview_paths.end())
      {
        QPixmap pixmap = LoadPreviewPixmapFresh(preview_it->second);
        if (!pixmap.isNull())
        {
          item->setData(3, Qt::DecorationRole,
                        pixmap.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation));
          item->setSizeHint(3, QSize(72, 72));
        }
      }

      tree->addTopLevelItem(item);
    }

    for (int i = 0; i < 4; i++)
      tree->resizeColumnToContents(i);

    if (current_textures.empty() && seen_only_count == 0)
    {
      info->setText(QObject::tr(
          "No textures captured yet for %1 %2.\n"
          "Enable hunting and let at least one frame render with this shader selected.\n"
          "Then use Refresh or Continuous Scan.")
                        .arg(QString::fromLatin1(type_str))
                        .arg(static_cast<uint>(hash), 8, 16, QLatin1Char('0')));
    }
    else if (current_textures.empty())
    {
      info->setText(QObject::tr(
          "No textures in current frame for %1 %2.\n"
          "%3 texture hash(es) were seen during scan and are shown in gray.")
                        .arg(QString::fromLatin1(type_str))
                        .arg(static_cast<uint>(hash), 8, 16, QLatin1Char('0'))
                        .arg(seen_only_count));
    }
    else
    {
      info->setText(QObject::tr(
          "%1 texture hash(es) in current frame for %3 %4. %2 seen earlier (gray).\n"
          "Check a row to skip draws using that texture for the selected shader.")
                        .arg(current_textures.size())
                        .arg(seen_only_count)
                        .arg(QString::fromLatin1(type_str))
                        .arg(static_cast<uint>(hash), 8, 16, QLatin1Char('0')));
    }
  };

  connect(tree, &QTreeWidget::itemChanged, dlg,
          [&hunter, update_selected_label](QTreeWidgetItem* item, int column) {
    if (!item || column != 0)
      return;
    const u64 texture_hash = item->data(0, Qt::UserRole).toULongLong();
    hunter.SetTextureSkipEnabled(texture_hash, item->checkState(0) == Qt::Checked);
    update_selected_label();
          });

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
  auto* refresh_button = buttons->addButton(tr("Refresh"), QDialogButtonBox::ActionRole);
  auto* clear_button = buttons->addButton(tr("Clear Checked"), QDialogButtonBox::ActionRole);
  auto* save_button = buttons->addButton(tr("Save To Shader"), QDialogButtonBox::ActionRole);

  connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::close);
  connect(refresh_button, &QPushButton::clicked, dlg, [populate, update_selected_label]() {
    populate();
    update_selected_label();
  });
  connect(scan_timer, &QTimer::timeout, dlg, [populate, update_selected_label]() {
    populate();
    update_selected_label();
  });
  connect(continuous_scan_check, &QCheckBox::toggled, dlg,
          [scan_timer, populate, update_selected_label](bool checked) {
    if (checked)
    {
      populate();
      update_selected_label();
      scan_timer->start();
    }
    else
    {
      scan_timer->stop();
    }
  });
  connect(clear_button, &QPushButton::clicked, dlg, [&hunter, populate, update_selected_label]() {
    hunter.ClearTextureSkipFilters();
    populate();
    update_selected_label();
  });
  connect(save_button, &QPushButton::clicked, dlg, [this, tree, dlg]() {
    std::vector<u64> selected_hashes;
    selected_hashes.reserve(tree->topLevelItemCount());
    for (int i = 0; i < tree->topLevelItemCount(); i++)
    {
      QTreeWidgetItem* item = tree->topLevelItem(i);
      if (!item || item->checkState(0) != Qt::Checked)
        continue;
      const u64 texture_hash = item->data(0, Qt::UserRole).toULongLong();
      if (texture_hash != 0)
        selected_hashes.push_back(texture_hash);
    }
    std::sort(selected_hashes.begin(), selected_hashes.end());
    selected_hashes.erase(std::unique(selected_hashes.begin(), selected_hashes.end()),
                          selected_hashes.end());
    m_saved_texture_filters = std::move(selected_hashes);
    SetSelectedTextureHashes(
        std::vector<uint64_t>(m_saved_texture_filters.begin(), m_saved_texture_filters.end()));

    QMessageBox::information(
        dlg, tr("Texture Filters Saved"),
        tr("Saved %1 texture hash(es) to the current shader.\n"
           "Press 'Save Shader' in Shader Hunter to write them to the config.")
            .arg(m_saved_texture_filters.size()));
  });

  auto* layout = new QVBoxLayout;
  layout->addWidget(info);
  layout->addWidget(tree);
  auto* bottom_layout = new QHBoxLayout;
  bottom_layout->addWidget(continuous_scan_check);
  bottom_layout->addStretch();
  bottom_layout->addWidget(buttons);
  layout->addLayout(bottom_layout);
  dlg->setLayout(layout);

  populate();
  update_selected_label();
  QTimer::singleShot(120, dlg, [populate, update_selected_label]() {
    populate();
    update_selected_label();
  });
  dlg->show();
}

void ShaderHunterWidget::SetSelectedTextureHashes(const std::vector<uint64_t>& hashes)
{
  if (hashes.empty())
  {
    m_selected_texture_label->setVisible(false);
    return;
  }

  QStringList hash_list;
  for (uint64_t hash : hashes)
  {
    hash_list.append(
        QStringLiteral("%1").arg(static_cast<qulonglong>(hash), 16, 16, QLatin1Char('0')));
  }

  m_selected_texture_label->setVisible(true);
  const bool exclude_mode = m_texture_filter_mode_combo->currentData().toBool();
  m_selected_texture_label->setText(
      tr("Selected Texture %1:\n%2")
          .arg(exclude_mode ? tr("Exclude Filter(s)") : tr("Include Filter(s)"))
          .arg(hash_list.join(QStringLiteral("\n"))));
}

void ShaderHunterWidget::SetSelectedDrawCallRange(int start, int end, int total)
{
  m_saved_element_start = start;
  m_saved_element_end = end;
  m_saved_element_total = total;

  if (start < 0 || end < 0)
  {
    m_selected_draw_call_label->setVisible(false);
    return;
  }

  m_selected_draw_call_label->setVisible(true);
  if (total > 0)
  {
    m_selected_draw_call_label->setText(
        tr("Selected Draw Call Range:\n%1 - %2 @%3")
            .arg(start + 1)
            .arg(end + 1)
            .arg(total));
  }
  else
  {
    m_selected_draw_call_label->setText(
        tr("Selected Draw Call Range:\n%1 - %2").arg(start + 1).arg(end + 1));
  }
}

