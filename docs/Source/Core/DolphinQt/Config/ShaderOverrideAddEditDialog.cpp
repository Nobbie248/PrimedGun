// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Config/ShaderOverrideAddEditDialog.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include <QCheckBox>
#include <QComboBox>
#include <QBrush>
#include <QColor>
#include <QDir>
#include <QDirIterator>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSize>
#include <QSpinBox>
#include <QTimer>
#include <QTreeWidget>
#include <QWidget>
#include <QVBoxLayout>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/ConfigManager.h"

namespace
{
constexpr double UPM_OVERRIDE_MIN = 0.01;
constexpr double UPM_OVERRIDE_MAX = 1000.0;
constexpr double UPM_OVERRIDE_STEP = 0.01;

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

static QString FormatRuntimeElementSummary(const ShaderHunter::RuntimeElementSignature& sig)
{
  if (!sig.valid)
    return QObject::tr("No runtime element captured.");

  QStringList parts;
  parts << QObject::tr("Projection: %1")
               .arg(sig.perspective ? QObject::tr("Perspective") : QObject::tr("Orthographic"));

  if (sig.perspective)
  {
    parts << QObject::tr("FOV: %1 / %2")
                 .arg(sig.perspective_hfov_x100 / 100.0, 0, 'f', 2)
                 .arg(sig.perspective_vfov_x100 / 100.0, 0, 'f', 2);
    parts << QObject::tr("Near/Far: %1 / %2")
                 .arg(sig.perspective_near_x1000 / 1000.0, 0, 'f', 3)
                 .arg(sig.perspective_far_x100 / 100.0, 0, 'f', 2);
  }
  else
  {
    parts << QObject::tr("Ortho: L%1 R%2 T%3 B%4")
                 .arg(sig.ortho_left_x100 / 100.0, 0, 'f', 2)
                 .arg(sig.ortho_right_x100 / 100.0, 0, 'f', 2)
                 .arg(sig.ortho_top_x100 / 100.0, 0, 'f', 2)
                 .arg(sig.ortho_bottom_x100 / 100.0, 0, 'f', 2);
    if (sig.use_layer)
      parts << QObject::tr("Layer: %1").arg(sig.ortho_layer);
  }

  if (sig.use_viewport)
  {
    parts << QObject::tr("Viewport: %1,%2 %3x%4")
                 .arg(sig.viewport_x)
                 .arg(sig.viewport_y)
                 .arg(sig.viewport_width)
                 .arg(sig.viewport_height);
  }
  if (sig.use_scissor)
  {
    parts << QObject::tr("Scissor: %1,%2 %3,%4")
                 .arg(sig.scissor_left)
                 .arg(sig.scissor_top)
                 .arg(sig.scissor_right)
                 .arg(sig.scissor_bottom);
  }
  if (sig.use_render_state)
  {
    parts << QObject::tr("State: alpha=%1 ztest=%2 zupdate=%3 zfunc=%4 blend=%5/%6")
                 .arg(QStringLiteral("0x%1").arg(sig.alpha_test_hex, 8, 16, QLatin1Char('0')))
                 .arg(sig.ztest ? QObject::tr("on") : QObject::tr("off"))
                 .arg(sig.zupdate ? QObject::tr("on") : QObject::tr("off"))
                 .arg(sig.zfunc)
                 .arg(sig.blend_color_update ? QObject::tr("col") : QObject::tr("-"))
                 .arg(sig.blend_alpha_update ? QObject::tr("alpha") : QObject::tr("-"));
  }

  return parts.join(QStringLiteral("\n"));
}
}  // namespace

ShaderOverrideAddEditDialog::ShaderOverrideAddEditDialog(
    QWidget* parent, const ShaderHunter::ShaderOverride* edit_override,
    const std::vector<std::string>& available_flags)
    : QDialog(parent)
{
  setWindowTitle(edit_override ? tr("Edit Shader Override") : tr("Add Shader Override"));
  setMinimumWidth(350);

  m_name_edit = new QLineEdit;
  m_name_edit->setPlaceholderText(tr("Override name..."));

  m_comments_edit = new QPlainTextEdit;
  m_comments_edit->setPlaceholderText(tr("Optional notes..."));
  m_comments_edit->setTabChangesFocus(true);
  m_comments_edit->setMinimumHeight(70);

  m_credits_edit = new QLineEdit;
  m_credits_edit->setPlaceholderText(tr("Optional author/credits..."));

  m_hash_edit = new QLineEdit;
  m_hash_edit->setPlaceholderText(tr("Hex hash (e.g. 0012abcd)"));

  m_type_combo = new QComboBox;
  m_type_combo->addItem(tr("Pixel Shader"), static_cast<int>(ShaderHunter::ShaderType::Pixel));
  m_type_combo->addItem(tr("Vertex Shader"), static_cast<int>(ShaderHunter::ShaderType::Vertex));
  m_type_combo->addItem(tr("Geometry Shader"),
                         static_cast<int>(ShaderHunter::ShaderType::Geometry));

  m_match_mode_label = new QLabel(tr("Match Mode:"));
  m_match_mode_combo = new QComboBox;
  m_match_mode_combo->addItem(tr("Exact Hash"),
                              static_cast<int>(ShaderHunter::MatchMode::ExactHash));
  m_match_mode_combo->addItem(tr("Shader Family"),
                              static_cast<int>(ShaderHunter::MatchMode::ShaderFamily));
  m_match_mode_combo->addItem(tr("Runtime Element"),
                              static_cast<int>(ShaderHunter::MatchMode::RuntimeElement));
  m_match_mode_combo->setToolTip(
      tr("Exact Hash: match only this exact shader hash.\n"
         "Shader Family: use the relaxed shader family signature.\n"
         "Runtime Element: match the shader plus captured draw-state signature."));

  m_handling_combo = new QComboBox;
  m_handling_combo->addItem(tr("Skip"), static_cast<int>(ShaderHunter::HandlingType::Skip));
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

  m_layer_label = new QLabel(tr("Layer:"));
  m_layer_spin = new QSpinBox;
  m_layer_spin->setRange(-1, 999);
  m_layer_spin->setSpecialValueText(tr("Auto"));
  m_layer_spin->setValue(-1);
  m_layer_spin->setToolTip(tr("Manual depth layer for Screen/Head Locked handling.\n"
                               "-1 (Auto) = automatic per-draw counter.\n"
                               "Higher values are closer to camera."));

  m_element_depth_label = new QLabel(tr("Element Depth:"));
  m_element_depth_spin = new QDoubleSpinBox;
  m_element_depth_spin->setRange(-1.0, 0.01);
  m_element_depth_spin->setDecimals(4);
  m_element_depth_spin->setSingleStep(0.0001);
  m_element_depth_spin->setSpecialValueText(tr("Global"));
  m_element_depth_spin->setValue(-1.0);
  m_element_depth_spin->setToolTip(tr("Within-element depth range for this shader.\n"
                                       "-1 (Global) = use the global Element Depth setting.\n"
                                       "Higher values fix Z-fighting inside the element."));

  m_units_per_meter_label = new QLabel(tr("Units per Meter:"));
  m_units_per_meter_spin = new QDoubleSpinBox;
  m_units_per_meter_spin->setRange(UPM_OVERRIDE_MIN, UPM_OVERRIDE_MAX);
  m_units_per_meter_spin->setDecimals(2);
  m_units_per_meter_spin->setSingleStep(UPM_OVERRIDE_STEP);
  m_units_per_meter_spin->setValue(1.0);
  m_units_per_meter_spin->setToolTip(tr("Temporary per-shader scale override for VR.\n"
                                         "Higher values make this shader appear larger."));

  m_flag_label = new QLabel(tr("Flag Group:"));
  m_flag_edit = new QLineEdit;
  m_flag_edit->setPlaceholderText(tr("e.g. gameplay"));
  m_flag_edit->setToolTip(tr("When this shader is drawn, it sets this named flag.\n"
                              "Other overrides can be conditional on this flag.\n"
                              "Optional for non-Flag handling (shader acts as both override and flag)."));

  m_condition_label = new QLabel(tr("Condition:"));
  m_condition_combo = new QComboBox;
  m_condition_combo->addItem(tr("(None)"), QString());
  for (const auto& flag : available_flags)
    m_condition_combo->addItem(QString::fromStdString(flag), QString::fromStdString(flag));
  m_condition_combo->setToolTip(
      tr("Select a flag condition for this override.\n"
         "Use Condition Mode to choose active or inactive behavior.\n"
         "Flags are detected from the current and previous frame."));

  m_condition_mode_label = new QLabel(tr("Condition Mode:"));
  m_condition_mode_combo = new QComboBox;
  m_condition_mode_combo->addItem(tr("Activate"), false);
  m_condition_mode_combo->addItem(tr("Deactivate"), true);
  m_condition_mode_combo->setToolTip(
      tr("Activate: apply when the selected flag is active.\n"
         "Deactivate: apply when the selected flag is NOT active."));

  m_clear_efb_check = new QCheckBox(tr("Clear EFB Copy"));
  m_clear_efb_check->setToolTip(
      tr("When this shader is drawn, the next EFB copy will be cleared to\n"
         "transparent instead of copying the framebuffer content.\n"
         "Use this to remove post-processing effects (bloom, blur) in VR.\n"
         "Can be combined with any handling type (Skip, Screen, etc.)."));

  m_clear_efb_min_label = new QLabel(tr("EFB Min Width:"));
  m_clear_efb_min_spin = new QSpinBox;
  m_clear_efb_min_spin->setRange(0, 640);
  m_clear_efb_min_spin->setSingleStep(10);
  m_clear_efb_min_spin->setSpecialValueText(tr("Any"));
  m_clear_efb_min_spin->setValue(0);
  m_clear_efb_min_spin->setToolTip(tr("Only clear EFB copies whose native width >= this value.\n"
                                       "0 (Any) = no lower bound.\n"
                                       "Full EFB = 640 wide; set ~400 to only clear full-screen effects."));

  m_clear_efb_max_label = new QLabel(tr("EFB Max Width:"));
  m_clear_efb_max_spin = new QSpinBox;
  m_clear_efb_max_spin->setRange(0, 640);
  m_clear_efb_max_spin->setSingleStep(10);
  m_clear_efb_max_spin->setSpecialValueText(tr("Any"));
  m_clear_efb_max_spin->setValue(0);
  m_clear_efb_max_spin->setToolTip(tr("Only clear EFB copies whose native width <= this value.\n"
                                       "0 (Any) = no upper bound.\n"
                                       "Use to restrict clearing to small or partial EFB copies."));

  m_element_start_label = new QLabel(tr("Draw Call Start:"));
  m_element_start_spin = new QSpinBox;
  m_element_start_spin->setRange(0, 9999);
  m_element_start_spin->setValue(0);
  m_element_start_spin->setToolTip(
      tr("First draw call index to apply this override.\n"
         "Only used when Draw Call Filter is enabled.\n"
         "Use Draw Call mode in Shader Hunter to identify draw call indices."));

  m_element_end_label = new QLabel(tr("Draw Call End:"));
  m_element_end_spin = new QSpinBox;
  m_element_end_spin->setRange(0, 9999);
  m_element_end_spin->setValue(0);
  m_element_end_spin->setToolTip(
      tr("Last draw call index to apply this override.\n"
         "Only used when Draw Call Filter is enabled.\n"
         "Range is inclusive: start=2, end=5 skips draws 2,3,4,5."));
  m_element_filter_check = new QCheckBox(tr("Draw Call Filter"));
  m_element_filter_check->setToolTip(
      tr("Enable to apply this override only to a specific draw-call range.\n"
         "Ranges adapt automatically when draw-call counts change between frames."));
  m_hash_family_check = new QCheckBox(tr("Match Shader Family"));
  m_hash_family_check->setToolTip(
      tr("Match this override against a relaxed shader family signature instead of the exact hash.\n"
         "Useful when the same effect changes hash across scenes or game revisions."));
  m_hash_family_check->hide();

  m_view_textures_button = new QPushButton(tr("View Textures"));
  m_view_textures_button->setToolTip(
      tr("Show captured textures for this shader hash and type.\n"
         "Check textures in the popup and apply them to Texture Filters."));

  m_capture_element_button = new QPushButton(tr("Capture Current Element"));
  m_capture_element_button->setToolTip(
      tr("Capture the currently selected draw from Shader Hunter as a runtime element signature.\n"
         "The Shader Hunter selection must match this shader hash and type."));

  m_runtime_element_summary_label = new QLabel;
  m_runtime_element_summary_label->setWordWrap(true);
  m_runtime_element_summary_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  m_runtime_element_summary_label->setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);
  m_runtime_element_summary_label->setMinimumHeight(96);

  m_runtime_element_groups_widget = new QWidget;
  auto* runtime_groups_layout = new QHBoxLayout(m_runtime_element_groups_widget);
  runtime_groups_layout->setContentsMargins(0, 0, 0, 0);
  runtime_groups_layout->setSpacing(8);
  m_runtime_use_projection_check = new QCheckBox(tr("Projection"));
  m_runtime_use_layer_check = new QCheckBox(tr("Layer"));
  m_runtime_use_viewport_check = new QCheckBox(tr("Viewport"));
  m_runtime_use_scissor_check = new QCheckBox(tr("Scissor"));
  m_runtime_use_render_state_check = new QCheckBox(tr("Render State"));
  runtime_groups_layout->addWidget(m_runtime_use_projection_check);
  runtime_groups_layout->addWidget(m_runtime_use_layer_check);
  runtime_groups_layout->addWidget(m_runtime_use_viewport_check);
  runtime_groups_layout->addWidget(m_runtime_use_scissor_check);
  runtime_groups_layout->addWidget(m_runtime_use_render_state_check);
  runtime_groups_layout->addStretch(1);

  m_texture_mode_label = new QLabel(tr("Texture Filter Mode:"));
  m_texture_mode_combo = new QComboBox;
  m_texture_mode_combo->addItem(tr("Include"), false);
  m_texture_mode_combo->addItem(tr("Exclude"), true);
  m_texture_mode_combo->setToolTip(
      tr("Include: apply override only when any listed texture hash is bound.\n"
         "Exclude: apply override only when none of the listed texture hashes are bound."));

  m_texture_hash_container = new QWidget;
  m_texture_hash_layout = new QVBoxLayout(m_texture_hash_container);
  m_texture_hash_layout->setContentsMargins(0, 0, 0, 0);
  m_texture_hash_layout->setSpacing(4);
  AddTextureHashField(QString());

  // Pre-fill fields in edit mode
  if (edit_override)
  {
    m_edit_element_reference_total = edit_override->element_reference_total;
    m_edit_runtime_anchor_family = edit_override->hash_family_match;
    m_edit_family_signature = edit_override->family_signature;
    m_edit_match_mode = edit_override->match_mode;
    m_edit_runtime_element = edit_override->runtime_element;
    m_name_edit->setText(QString::fromStdString(edit_override->name));
    m_comments_edit->setPlainText(QString::fromStdString(edit_override->comments));
    m_credits_edit->setText(QString::fromStdString(edit_override->credits));
    m_hash_edit->setText(
        QString::fromStdString(fmt::format("{:016x}", edit_override->hash)));

    const int type_idx = m_type_combo->findData(static_cast<int>(edit_override->type));
    if (type_idx >= 0)
      m_type_combo->setCurrentIndex(type_idx);

    const int handling_idx = m_handling_combo->findData(static_cast<int>(edit_override->handling));
    if (handling_idx >= 0)
      m_handling_combo->setCurrentIndex(handling_idx);

    m_layer_spin->setValue(edit_override->layer);
    m_element_depth_spin->setValue(edit_override->element_depth);
    if (edit_override->units_per_meter > 0.0f)
      m_units_per_meter_spin->setValue(edit_override->units_per_meter);
    m_flag_edit->setText(QString::fromStdString(edit_override->flag_group));

    m_clear_efb_check->setChecked(edit_override->clear_efb);
    m_clear_efb_min_spin->setValue(edit_override->clear_efb_min_width);
    m_clear_efb_max_spin->setValue(edit_override->clear_efb_max_width);

    const bool has_element_filter =
        edit_override->element_start >= 0 || edit_override->element_end >= 0;
    m_element_filter_check->setChecked(has_element_filter);
    const int match_mode_idx =
        m_match_mode_combo->findData(static_cast<int>(m_edit_match_mode));
    if (match_mode_idx >= 0)
      m_match_mode_combo->setCurrentIndex(match_mode_idx);
    m_hash_family_check->setChecked(edit_override->hash_family_match);
    m_element_start_spin->setValue(std::max(0, edit_override->element_start));
    m_element_end_spin->setValue(std::max(0, edit_override->element_end));

    m_updating_texture_hash_fields = true;
    while (m_texture_hash_edits.size() < edit_override->texture_hashes.size())
      AddTextureHashField(QString());
    for (size_t i = 0; i < edit_override->texture_hashes.size(); i++)
    {
      m_texture_hash_edits[i]->setText(
          QString::fromStdString(fmt::format("{:016x}", edit_override->texture_hashes[i])));
    }
    for (size_t i = edit_override->texture_hashes.size(); i < m_texture_hash_edits.size(); i++)
      m_texture_hash_edits[i]->clear();
    m_updating_texture_hash_fields = false;
    EnsureTextureHashFieldRows();
    const int texture_mode_idx =
        m_texture_mode_combo->findData(edit_override->texture_hashes_excluded);
    if (texture_mode_idx >= 0)
      m_texture_mode_combo->setCurrentIndex(texture_mode_idx);

    if (!edit_override->condition_flag.empty())
    {
      const int cond_idx =
          m_condition_combo->findData(QString::fromStdString(edit_override->condition_flag));
      if (cond_idx >= 0)
        m_condition_combo->setCurrentIndex(cond_idx);
      else
      {
        // Flag not in available list — add it dynamically
        m_condition_combo->addItem(QString::fromStdString(edit_override->condition_flag),
                                   QString::fromStdString(edit_override->condition_flag));
        m_condition_combo->setCurrentIndex(m_condition_combo->count() - 1);
      }
    }
    const int mode_idx = m_condition_mode_combo->findData(edit_override->condition_inverted);
    if (mode_idx >= 0)
      m_condition_mode_combo->setCurrentIndex(mode_idx);
  }

  auto* form = new QFormLayout;
  form->addRow(tr("Name:"), m_name_edit);
  form->addRow(tr("Hash:"), m_hash_edit);
  form->addRow(tr("Shader Type:"), m_type_combo);
  form->addRow(m_match_mode_label, m_match_mode_combo);
  form->addRow(tr("Handling:"), m_handling_combo);
  form->addRow(m_layer_label, m_layer_spin);
  form->addRow(m_element_depth_label, m_element_depth_spin);
  form->addRow(m_units_per_meter_label, m_units_per_meter_spin);
  form->addRow(m_flag_label, m_flag_edit);
  form->addRow(m_condition_label, m_condition_combo);
  form->addRow(m_condition_mode_label, m_condition_mode_combo);
  form->addRow(QString(), m_clear_efb_check);
  form->addRow(m_clear_efb_min_label, m_clear_efb_min_spin);
  form->addRow(m_clear_efb_max_label, m_clear_efb_max_spin);
  form->addRow(QString(), m_element_filter_check);
  form->addRow(m_element_start_label, m_element_start_spin);
  form->addRow(m_element_end_label, m_element_end_spin);
  form->addRow(QString(), m_capture_element_button);
  form->addRow(tr("Element Signature:"), m_runtime_element_summary_label);
  form->addRow(tr("Signature Groups:"), m_runtime_element_groups_widget);
  form->addRow(QString(), m_view_textures_button);
  form->addRow(m_texture_mode_label, m_texture_mode_combo);
  form->addRow(tr("Texture Filters:"), m_texture_hash_container);
  form->addRow(tr("Comments:"), m_comments_edit);
  form->addRow(tr("Credits:"), m_credits_edit);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  connect(buttons, &QDialogButtonBox::accepted, this, &ShaderOverrideAddEditDialog::OnAccept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(m_handling_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &ShaderOverrideAddEditDialog::OnHandlingChanged);
  connect(m_clear_efb_check, &QCheckBox::toggled, this,
          &ShaderOverrideAddEditDialog::OnClearEFBChanged);
  connect(m_element_filter_check, &QCheckBox::toggled, this,
          &ShaderOverrideAddEditDialog::OnElementFilterChanged);
  connect(m_match_mode_combo, qOverload<int>(&QComboBox::currentIndexChanged), this,
          &ShaderOverrideAddEditDialog::RefreshMatchModeUi);
  connect(m_condition_combo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
    const bool has_condition = !m_condition_combo->currentData().toString().isEmpty();
    m_condition_mode_label->setEnabled(has_condition);
    m_condition_mode_combo->setEnabled(has_condition);
  });
  connect(m_capture_element_button, &QPushButton::clicked, this,
          &ShaderOverrideAddEditDialog::CaptureCurrentElement);
  connect(m_view_textures_button, &QPushButton::clicked, this,
          &ShaderOverrideAddEditDialog::ShowTextureBrowser);
  for (QCheckBox* checkbox : {m_runtime_use_projection_check, m_runtime_use_layer_check,
                              m_runtime_use_viewport_check, m_runtime_use_scissor_check,
                              m_runtime_use_render_state_check})
  {
    connect(checkbox, &QCheckBox::toggled, this, [this]() {
      m_edit_runtime_element.use_projection = m_runtime_use_projection_check->isChecked();
      m_edit_runtime_element.use_layer = m_runtime_use_layer_check->isChecked();
      m_edit_runtime_element.use_viewport = m_runtime_use_viewport_check->isChecked();
      m_edit_runtime_element.use_scissor = m_runtime_use_scissor_check->isChecked();
      m_edit_runtime_element.use_render_state = m_runtime_use_render_state_check->isChecked();
      RefreshRuntimeElementSummary();
    });
  }

  auto* layout = new QVBoxLayout;
  layout->addLayout(form);
  layout->addWidget(buttons);
  setLayout(layout);

  // Show/hide fields based on initial handling selection
  OnHandlingChanged();
  const bool has_condition = !m_condition_combo->currentData().toString().isEmpty();
  m_condition_mode_label->setEnabled(has_condition);
  m_condition_mode_combo->setEnabled(has_condition);
  OnClearEFBChanged();
  OnElementFilterChanged();
  RefreshRuntimeElementGroupControls();
  RefreshMatchModeUi();
  RefreshRuntimeElementSummary();
}

ShaderHunter::ShaderOverride ShaderOverrideAddEditDialog::GetResult() const
{
  ShaderHunter::ShaderOverride result;
  result.name = m_name_edit->text().toStdString();
  result.comments = m_comments_edit->toPlainText().trimmed().toStdString();
  result.credits = m_credits_edit->text().trimmed().toStdString();
  result.hash = std::strtoull(m_hash_edit->text().toStdString().c_str(), nullptr, 16);
  result.type =
      static_cast<ShaderHunter::ShaderType>(m_type_combo->currentData().toInt());
  result.match_mode =
      static_cast<ShaderHunter::MatchMode>(m_match_mode_combo->currentData().toInt());
  result.hash_family_match = false;
  result.family_signature = 0;

  if (result.match_mode == ShaderHunter::MatchMode::ShaderFamily)
  {
    result.hash_family_match = true;
    result.family_signature = m_edit_family_signature;
    if (const auto signature =
            ShaderHunter::GetInstance().GetShaderFamilySignature(result.type, result.hash);
        signature.has_value())
    {
      result.family_signature = *signature;
    }
  }
  else if (result.match_mode == ShaderHunter::MatchMode::RuntimeElement)
  {
    result.runtime_element = m_edit_runtime_element;
    result.hash_family_match = m_edit_runtime_anchor_family;
    result.family_signature = m_edit_runtime_anchor_family ? m_edit_family_signature : 0;
  }
  result.handling =
      static_cast<ShaderHunter::HandlingType>(m_handling_combo->currentData().toInt());
  result.layer = (result.handling == ShaderHunter::HandlingType::Screen ||
                  result.handling == ShaderHunter::HandlingType::HeadLocked) ?
                     m_layer_spin->value() : -1;
  result.element_depth = (result.handling == ShaderHunter::HandlingType::Screen ||
                          result.handling == ShaderHunter::HandlingType::HeadLocked) ?
                             static_cast<float>(m_element_depth_spin->value()) : -1.0f;
  result.units_per_meter = result.handling == ShaderHunter::HandlingType::UnitsPerMeter ?
                               static_cast<float>(m_units_per_meter_spin->value()) :
                               -1.0f;
  result.clear_efb = m_clear_efb_check->isChecked();
  result.clear_efb_min_width = result.clear_efb ? m_clear_efb_min_spin->value() : 0;
  result.clear_efb_max_width = result.clear_efb ? m_clear_efb_max_spin->value() : 0;
  if (m_element_filter_check->isChecked())
  {
    result.element_start = m_element_start_spin->value();
    result.element_end = m_element_end_spin->value();
    result.element_reference_total = m_edit_element_reference_total;
  }
  else
  {
    result.element_start = -1;
    result.element_end = -1;
    result.element_reference_total = 0;
  }
  const auto texture_tokens = CollectTextureHashTokens();
  for (const std::string& token : texture_tokens)
  {
    const u64 parsed = std::strtoull(token.c_str(), nullptr, 16);
    if (parsed != 0)
      result.texture_hashes.push_back(parsed);
  }
  std::sort(result.texture_hashes.begin(), result.texture_hashes.end());
  result.texture_hashes.erase(
      std::unique(result.texture_hashes.begin(), result.texture_hashes.end()),
      result.texture_hashes.end());
  result.texture_hashes_excluded =
      !result.texture_hashes.empty() && m_texture_mode_combo->currentData().toBool();
  result.enabled = true;
  result.user_defined = true;

  // flag_group is available on all handling types (optional except for Flag)
  result.flag_group = m_flag_edit->text().trimmed().toStdString();

  if (result.handling != ShaderHunter::HandlingType::Flag)
  {
    result.condition_flag = m_condition_combo->currentData().toString().toStdString();
    result.condition_inverted =
        !result.condition_flag.empty() && m_condition_mode_combo->currentData().toBool();
  }

  return result;
}

void ShaderOverrideAddEditDialog::OnAccept()
{
  const std::string name = m_name_edit->text().toStdString();
  if (name.empty())
  {
    QMessageBox::warning(this, tr("Validation Error"), tr("Name cannot be empty."));
    return;
  }

  const std::string hash_str = m_hash_edit->text().toStdString();
  if (hash_str.empty())
  {
    QMessageBox::warning(this, tr("Validation Error"), tr("Hash cannot be empty."));
    return;
  }

  // Validate hex string
  for (char c : hash_str)
  {
    if (!std::isxdigit(static_cast<unsigned char>(c)))
    {
      QMessageBox::warning(this, tr("Validation Error"),
                           tr("Hash must contain only hexadecimal characters (0-9, a-f)."));
      return;
    }
  }

  if (hash_str.size() > 16)
  {
    QMessageBox::warning(this, tr("Validation Error"),
                         tr("Hash must be at most 16 hex digits."));
    return;
  }

  // Validate texture hash hex string (if provided)
  const auto texture_tokens = CollectTextureHashTokens();
  for (const std::string& token : texture_tokens)
  {
    for (char c : token)
    {
      if (!std::isxdigit(static_cast<unsigned char>(c)))
      {
        QMessageBox::warning(this, tr("Validation Error"),
                             tr("Texture filter values must contain only hexadecimal characters "
                                "(0-9, a-f)."));
        return;
      }
    }
    if (token.size() > 16)
    {
      QMessageBox::warning(this, tr("Validation Error"),
                           tr("Each texture filter value must be at most 16 hex digits."));
      return;
    }
  }

  // Validate flag group name for Flag handling
  if (m_element_filter_check->isChecked() &&
      m_element_start_spin->value() > m_element_end_spin->value())
  {
    QMessageBox::warning(this, tr("Validation Error"),
                         tr("Draw Call Start must be less than or equal to Draw Call End."));
    return;
  }

  // Validate flag group name for Flag handling
  const auto handling = static_cast<ShaderHunter::HandlingType>(
      m_handling_combo->currentData().toInt());
  if (handling == ShaderHunter::HandlingType::Flag && m_flag_edit->text().trimmed().isEmpty())
  {
    QMessageBox::warning(this, tr("Validation Error"),
                         tr("Flag Group name cannot be empty for Flag handling."));
    return;
  }
  if (handling == ShaderHunter::HandlingType::UnitsPerMeter &&
      m_units_per_meter_spin->value() <= 0.0)
  {
    QMessageBox::warning(this, tr("Validation Error"),
                         tr("Units per Meter must be greater than 0."));
    return;
  }

  const auto match_mode = static_cast<ShaderHunter::MatchMode>(m_match_mode_combo->currentData().toInt());
  if (match_mode == ShaderHunter::MatchMode::RuntimeElement && !m_edit_runtime_element.valid)
  {
    QMessageBox::warning(this, tr("Validation Error"),
                         tr("Runtime Element match mode requires a captured element signature."));
    return;
  }

  accept();
}

void ShaderOverrideAddEditDialog::OnHandlingChanged()
{
  const auto handling = static_cast<ShaderHunter::HandlingType>(
      m_handling_combo->currentData().toInt());
  const bool show_layer = (handling == ShaderHunter::HandlingType::Screen ||
                           handling == ShaderHunter::HandlingType::HeadLocked);
  const bool show_units_per_meter = (handling == ShaderHunter::HandlingType::UnitsPerMeter);
  const bool is_flag = (handling == ShaderHunter::HandlingType::Flag);

  m_layer_label->setVisible(show_layer);
  m_layer_spin->setVisible(show_layer);
  m_element_depth_label->setVisible(show_layer);
  m_element_depth_spin->setVisible(show_layer);
  m_units_per_meter_label->setVisible(show_units_per_meter);
  m_units_per_meter_spin->setVisible(show_units_per_meter);
  // Flag group is always visible (optional for non-Flag handling, required for Flag)
  m_flag_label->setVisible(true);
  m_flag_edit->setVisible(true);
  m_condition_label->setVisible(!is_flag);
  m_condition_combo->setVisible(!is_flag);
  m_condition_mode_label->setVisible(!is_flag);
  m_condition_mode_combo->setVisible(!is_flag);
}

void ShaderOverrideAddEditDialog::OnClearEFBChanged()
{
  const bool show = m_clear_efb_check->isChecked();
  m_clear_efb_min_label->setVisible(show);
  m_clear_efb_min_spin->setVisible(show);
  m_clear_efb_max_label->setVisible(show);
  m_clear_efb_max_spin->setVisible(show);
}

void ShaderOverrideAddEditDialog::OnElementFilterChanged()
{
  const bool show = m_element_filter_check->isChecked();
  m_element_start_label->setVisible(show);
  m_element_start_spin->setVisible(show);
  m_element_end_label->setVisible(show);
  m_element_end_spin->setVisible(show);
}

void ShaderOverrideAddEditDialog::RefreshMatchModeUi()
{
  const auto match_mode =
      static_cast<ShaderHunter::MatchMode>(m_match_mode_combo->currentData().toInt());
  const bool show_runtime = match_mode == ShaderHunter::MatchMode::RuntimeElement;
  m_capture_element_button->setVisible(show_runtime);
  m_runtime_element_summary_label->setVisible(show_runtime);
  m_runtime_element_groups_widget->setVisible(show_runtime);
  RefreshRuntimeElementGroupControls();
  RefreshRuntimeElementSummary();
}

void ShaderOverrideAddEditDialog::RefreshRuntimeElementSummary()
{
  const auto match_mode =
      static_cast<ShaderHunter::MatchMode>(m_match_mode_combo->currentData().toInt());
  if (match_mode != ShaderHunter::MatchMode::RuntimeElement)
  {
    m_runtime_element_summary_label->setText(
        tr("Runtime element matching is disabled for this override."));
    return;
  }

  QString summary = FormatRuntimeElementSummary(m_edit_runtime_element);
  if (m_edit_runtime_element.valid)
  {
    summary += tr("\nAnchor: %1")
                   .arg(m_edit_runtime_anchor_family ? tr("Shader Family") : tr("Exact Hash"));
  }
  else
  {
    summary += tr("\nUse Shader Hunter to select the target draw, then capture it here.");
  }
  m_runtime_element_summary_label->setText(summary);
}

void ShaderOverrideAddEditDialog::RefreshRuntimeElementGroupControls()
{
  const QSignalBlocker projection_blocker(m_runtime_use_projection_check);
  const QSignalBlocker layer_blocker(m_runtime_use_layer_check);
  const QSignalBlocker viewport_blocker(m_runtime_use_viewport_check);
  const QSignalBlocker scissor_blocker(m_runtime_use_scissor_check);
  const QSignalBlocker render_state_blocker(m_runtime_use_render_state_check);

  m_runtime_use_projection_check->setChecked(m_edit_runtime_element.use_projection);
  m_runtime_use_layer_check->setChecked(m_edit_runtime_element.use_layer);
  m_runtime_use_viewport_check->setChecked(m_edit_runtime_element.use_viewport);
  m_runtime_use_scissor_check->setChecked(m_edit_runtime_element.use_scissor);
  m_runtime_use_render_state_check->setChecked(m_edit_runtime_element.use_render_state);

  const bool has_signature = m_edit_runtime_element.valid;
  m_runtime_use_projection_check->setEnabled(has_signature);
  m_runtime_use_layer_check->setEnabled(has_signature);
  m_runtime_use_viewport_check->setEnabled(has_signature);
  m_runtime_use_scissor_check->setEnabled(has_signature);
  m_runtime_use_render_state_check->setEnabled(has_signature);
}

void ShaderOverrideAddEditDialog::CaptureCurrentElement()
{
  const std::string hash_str = m_hash_edit->text().trimmed().toStdString();
  if (hash_str.empty())
  {
    QMessageBox::warning(this, tr("Capture Element"),
                         tr("Enter the shader hash before capturing a runtime element."));
    return;
  }

  for (char c : hash_str)
  {
    if (!std::isxdigit(static_cast<unsigned char>(c)))
    {
      QMessageBox::warning(this, tr("Capture Element"),
                           tr("Hash must contain only hexadecimal characters (0-9, a-f)."));
      return;
    }
  }

  const auto shader_type =
      static_cast<ShaderHunter::ShaderType>(m_type_combo->currentData().toInt());
  const u64 shader_hash = std::strtoull(hash_str.c_str(), nullptr, 16);
  auto& hunter = ShaderHunter::GetInstance();
  const auto signature = hunter.GetSelectedRuntimeElementSignature();
  if (!signature.has_value())
  {
    QMessageBox::warning(
        this, tr("Capture Element"),
        tr("No current element is available.\nUse Shader Hunter to hunt and highlight the target draw first."));
    return;
  }

  if (hunter.GetActiveType() != shader_type || hunter.GetSelectedHash() != shader_hash)
  {
    QMessageBox::warning(
        this, tr("Capture Element"),
        tr("Shader Hunter selection does not match this override.\nSelect the same shader hash and type in Shader Hunter, then capture again."));
    return;
  }

  m_edit_runtime_element = *signature;
  m_edit_match_mode = ShaderHunter::MatchMode::RuntimeElement;
  const int runtime_index =
      m_match_mode_combo->findData(static_cast<int>(ShaderHunter::MatchMode::RuntimeElement));
  if (runtime_index >= 0)
    m_match_mode_combo->setCurrentIndex(runtime_index);

  m_edit_runtime_anchor_family = false;
  m_edit_family_signature = 0;
  if (shader_type == ShaderHunter::ShaderType::Pixel)
  {
    if (const auto family_signature = hunter.GetShaderFamilySignature(shader_type, shader_hash);
        family_signature.has_value())
    {
      m_edit_runtime_anchor_family = true;
      m_edit_family_signature = *family_signature;
    }
  }

  RefreshRuntimeElementSummary();
  RefreshRuntimeElementGroupControls();
}

void ShaderOverrideAddEditDialog::ShowTextureBrowser()
{
  const std::string hash_str = m_hash_edit->text().trimmed().toStdString();
  if (hash_str.empty())
  {
    QMessageBox::warning(this, tr("View Textures"),
                         tr("Hash cannot be empty. Enter the shader hash first."));
    return;
  }
  if (hash_str.size() > 16)
  {
    QMessageBox::warning(this, tr("View Textures"),
                         tr("Hash must be at most 16 hex digits."));
    return;
  }
  for (char c : hash_str)
  {
    if (!std::isxdigit(static_cast<unsigned char>(c)))
    {
      QMessageBox::warning(this, tr("View Textures"),
                           tr("Hash must contain only hexadecimal characters (0-9, a-f)."));
      return;
    }
  }

  const u64 shader_hash = std::strtoull(hash_str.c_str(), nullptr, 16);
  const auto shader_type =
      static_cast<ShaderHunter::ShaderType>(m_type_combo->currentData().toInt());
  auto& hunter = ShaderHunter::GetInstance();
  const bool was_hunting_enabled = hunter.IsEnabled();
  const auto previous_active_type = hunter.GetActiveType();
  const int previous_selected_pos = hunter.GetSelectedPosition();
  const u64 previous_selected_hash = hunter.GetSelectedHash();

  auto* dlg = new QDialog(this);
  const char* type_str = shader_type == ShaderHunter::ShaderType::Pixel    ? "PS" :
                         shader_type == ShaderHunter::ShaderType::Vertex   ? "VS" :
                                                                              "GS";
  dlg->setWindowTitle(tr("Textures for %1 %2")
                          .arg(QString::fromLatin1(type_str))
                          .arg(static_cast<qulonglong>(shader_hash), 16, 16, QLatin1Char('0')));
  dlg->setAttribute(Qt::WA_DeleteOnClose);
  dlg->setMinimumSize(620, 320);

  auto* info = new QLabel;
  info->setWordWrap(true);
  auto* continuous_scan_check = new QCheckBox(tr("Continuous Scan"));
  continuous_scan_check->setToolTip(
      tr("Continuously refresh while this window is open.\n"
         "Textures seen during scan stay visible in gray."));
  auto* scan_timer = new QTimer(dlg);
  scan_timer->setInterval(250);

  auto* tree = new QTreeWidget;
  tree->setHeaderLabels({tr("Use"), tr("Texture Hash"), tr("Name"), tr("Preview")});
  tree->setRootIsDecorated(false);
  tree->setSelectionMode(QAbstractItemView::NoSelection);
  tree->header()->setStretchLastSection(true);
  tree->setIconSize(QSize(64, 64));

  if (!was_hunting_enabled)
    hunter.SetEnabled(true);
  hunter.SetTextureToolActive(true);
  connect(dlg, &QObject::destroyed, this,
          [was_hunting_enabled, previous_active_type, previous_selected_pos,
           previous_selected_hash]() {
    auto& restore_hunter = ShaderHunter::GetInstance();
    restore_hunter.SetTextureToolActive(false);
    restore_hunter.ClearTextureSkipFilters();
    if (previous_selected_pos >= 0)
    {
      if (!restore_hunter.SelectShader(previous_active_type, previous_selected_hash))
        restore_hunter.SetActiveType(previous_active_type);
    }
    else
    {
      restore_hunter.SetActiveType(previous_active_type);
    }
    if (!was_hunting_enabled)
      restore_hunter.SetEnabled(false);
  });

  const auto initial_hashes = CollectTextureHashValues();
  auto selected_hashes =
      std::make_shared<std::unordered_set<u64>>(initial_hashes.begin(), initial_hashes.end());
  auto scanned_hashes = std::make_shared<std::unordered_map<u64, std::string>>();

  const auto populate = [tree, info, selected_hashes, scanned_hashes, continuous_scan_check,
                         shader_hash, shader_type]() {
    auto& populate_hunter = ShaderHunter::GetInstance();
    if (populate_hunter.SelectShader(shader_type, shader_hash))
    {
      populate_hunter.ClearTextureSkipFilters();
      for (u64 selected_hash : *selected_hashes)
        populate_hunter.SetTextureSkipEnabled(selected_hash, true);
    }

    const auto textures = populate_hunter.GetTexturesForHash(shader_type, shader_hash);
    std::unordered_map<u64, ShaderHunter::TextureUsage> captured;
    captured.reserve(textures.size());
    for (const auto& tex : textures)
    {
      captured.emplace(tex.hash, tex);
      if (continuous_scan_check->isChecked())
      {
        auto& saved_name = (*scanned_hashes)[tex.hash];
        if (saved_name.empty() && !tex.name.empty())
          saved_name = tex.name;
      }
    }

    std::vector<u64> texture_hashes;
    texture_hashes.reserve(textures.size() + selected_hashes->size() + scanned_hashes->size());
    for (const auto& [captured_hash, _] : captured)
      texture_hashes.push_back(captured_hash);
    for (u64 saved_hash : *selected_hashes)
      texture_hashes.push_back(saved_hash);
    for (const auto& [seen_hash, _] : *scanned_hashes)
      texture_hashes.push_back(seen_hash);
    std::sort(texture_hashes.begin(), texture_hashes.end());
    texture_hashes.erase(std::unique(texture_hashes.begin(), texture_hashes.end()), texture_hashes.end());

    const auto preview_paths = FindTexturePreviewPaths(texture_hashes);

    const QSignalBlocker blocker(tree);
    tree->clear();

    int saved_only_count = 0;
    int scanned_only_count = 0;
    for (u64 hash : texture_hashes)
    {
      const auto captured_it = captured.find(hash);
      const bool in_current_scene = (captured_it != captured.end());
      const bool is_saved_only = !in_current_scene && selected_hashes->count(hash) > 0;
      if (is_saved_only)
        ++saved_only_count;
      const bool is_scanned_only = !in_current_scene && !is_saved_only &&
                                   scanned_hashes->find(hash) != scanned_hashes->end();
      if (is_scanned_only)
        ++scanned_only_count;

      auto* item = new QTreeWidgetItem;
      item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
      item->setCheckState(0, selected_hashes->count(hash) > 0 ? Qt::Checked : Qt::Unchecked);
      item->setText(
          1, QStringLiteral("%1").arg(static_cast<qulonglong>(hash), 16, 16, QLatin1Char('0')));
      if (in_current_scene)
      {
        const auto& tex = captured_it->second;
        item->setText(2, tex.name.empty() ? QObject::tr("(unknown)") : QString::fromStdString(tex.name));
      }
      else
      {
        if (is_saved_only)
        {
          item->setText(2, QObject::tr("(saved filter, not in current scene)"));
        }
        else
        {
          const auto scanned_it = scanned_hashes->find(hash);
          const bool has_scanned_name =
              scanned_it != scanned_hashes->end() && !scanned_it->second.empty();
          item->setText(2, has_scanned_name ? QString::fromStdString(scanned_it->second) :
                                              QObject::tr("(seen during scan, not in current scene)"));
        }
        const QBrush gray_brush(QColor(140, 140, 140));
        item->setForeground(1, gray_brush);
        item->setForeground(2, gray_brush);
      }
      item->setData(0, Qt::UserRole, static_cast<qulonglong>(hash));

      auto preview_it = preview_paths.find(hash);
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

    if (textures.empty())
    {
      if (saved_only_count > 0 || scanned_only_count > 0)
      {
        info->setText(QObject::tr(
            "No textures from the current scene were captured yet.\n"
            "Saved filters and previously scanned textures are shown in gray.\n"
            "Shader Hunter was enabled temporarily for this window."));
      }
      else
      {
        info->setText(QObject::tr(
            "No textures captured yet for this shader.\n"
            "Shader Hunter was enabled temporarily for this window."));
      }
    }
    else
    {
      info->setText(QObject::tr(
          "%1 captured texture(s). %2 saved-only and %3 scanned-only shown in gray.\n"
          "Check rows and press Apply to update Texture Filters.")
                        .arg(textures.size())
                        .arg(saved_only_count)
                        .arg(scanned_only_count));
    }
  };

  connect(tree, &QTreeWidget::itemChanged, dlg, [selected_hashes, shader_hash, shader_type](
                                                     QTreeWidgetItem* item, int column) {
    if (!item || column != 0)
      return;
    const u64 texture_hash = item->data(0, Qt::UserRole).toULongLong();
    const bool enabled = item->checkState(0) == Qt::Checked;
    if (enabled)
      selected_hashes->insert(texture_hash);
    else
      selected_hashes->erase(texture_hash);

    auto& item_changed_hunter = ShaderHunter::GetInstance();
    if (item_changed_hunter.SelectShader(shader_type, shader_hash))
      item_changed_hunter.SetTextureSkipEnabled(texture_hash, enabled);
  });

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
  auto* refresh_button = buttons->addButton(tr("Refresh"), QDialogButtonBox::ActionRole);
  auto* apply_button = buttons->addButton(tr("Apply"), QDialogButtonBox::AcceptRole);

  connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::close);
  connect(refresh_button, &QPushButton::clicked, dlg, populate);
  connect(scan_timer, &QTimer::timeout, dlg, populate);
  connect(continuous_scan_check, &QCheckBox::toggled, dlg, [scan_timer, populate](bool checked) {
    if (checked)
    {
      populate();
      scan_timer->start();
    }
    else
    {
      scan_timer->stop();
    }
  });
  connect(apply_button, &QPushButton::clicked, dlg, [this, selected_hashes]() {
    std::vector<u64> hashes(selected_hashes->begin(), selected_hashes->end());
    std::sort(hashes.begin(), hashes.end());
    SetTextureHashFields(hashes);
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
  QTimer::singleShot(200, dlg, populate);
  dlg->show();
}

std::vector<std::string> ShaderOverrideAddEditDialog::CollectTextureHashTokens() const
{
  std::vector<std::string> tokens;
  const QRegularExpression separators(QStringLiteral("[,;\\s]+"));
  const QRegularExpression hex_exact(QStringLiteral("^[0-9A-Fa-f]{1,16}$"));
  const QRegularExpression hex_16_anywhere(QStringLiteral("([0-9A-Fa-f]{16})"));
  for (QLineEdit* edit : m_texture_hash_edits)
  {
    if (edit == nullptr)
      continue;
    const auto parts = edit->text().trimmed().split(separators, Qt::SkipEmptyParts);
    for (const QString& part : parts)
    {
      if (hex_exact.match(part).hasMatch())
      {
        tokens.push_back(part.toLower().toStdString());
        continue;
      }

      const auto match = hex_16_anywhere.match(part);
      if (match.hasMatch())
      {
        tokens.push_back(match.captured(1).toLower().toStdString());
        continue;
      }

      tokens.push_back(part.toStdString());
    }
  }
  return tokens;
}

std::vector<u64> ShaderOverrideAddEditDialog::CollectTextureHashValues() const
{
  std::vector<u64> hashes;
  for (const std::string& token : CollectTextureHashTokens())
  {
    if (token.empty() || token.size() > 16)
      continue;
    bool valid = true;
    for (char c : token)
    {
      if (!std::isxdigit(static_cast<unsigned char>(c)))
      {
        valid = false;
        break;
      }
    }
    if (!valid)
      continue;
    const u64 parsed = std::strtoull(token.c_str(), nullptr, 16);
    if (parsed != 0)
      hashes.push_back(parsed);
  }
  std::sort(hashes.begin(), hashes.end());
  hashes.erase(std::unique(hashes.begin(), hashes.end()), hashes.end());
  return hashes;
}

void ShaderOverrideAddEditDialog::SetTextureHashFields(const std::vector<u64>& hashes)
{
  m_updating_texture_hash_fields = true;
  while (m_texture_hash_edits.size() < hashes.size())
    AddTextureHashField(QString());

  for (size_t i = 0; i < hashes.size(); i++)
  {
    m_texture_hash_edits[i]->setText(
        QString::fromStdString(fmt::format("{:016x}", hashes[i])));
  }
  for (size_t i = hashes.size(); i < m_texture_hash_edits.size(); i++)
    m_texture_hash_edits[i]->clear();

  m_updating_texture_hash_fields = false;
  EnsureTextureHashFieldRows();
}

void ShaderOverrideAddEditDialog::AddTextureHashField(const QString& text)
{
  auto* edit = new QLineEdit;
  edit->setPlaceholderText(tr("Hex hash"));
  edit->setToolTip(
      tr("Optional texture filter hash.\n"
         "Enter one hash per line; a new line is added automatically.\n"
         "You can also paste multiple hashes separated by comma, semicolon, or spaces."));
  edit->setText(text);
  m_texture_hash_layout->addWidget(edit);
  m_texture_hash_edits.push_back(edit);

  connect(edit, &QLineEdit::textChanged, this, [this, edit](const QString&) {
    SanitizeTextureHashField(edit);
    EnsureTextureHashFieldRows();
  });
}

void ShaderOverrideAddEditDialog::EnsureTextureHashFieldRows()
{
  if (m_updating_texture_hash_fields)
    return;

  m_updating_texture_hash_fields = true;

  int last_non_empty = -1;
  for (int i = 0; i < static_cast<int>(m_texture_hash_edits.size()); i++)
  {
    if (!m_texture_hash_edits[i]->text().trimmed().isEmpty())
      last_non_empty = i;
  }

  const int desired_rows = std::max(1, last_non_empty + 2);

  while (static_cast<int>(m_texture_hash_edits.size()) < desired_rows)
    AddTextureHashField(QString());

  while (static_cast<int>(m_texture_hash_edits.size()) > desired_rows)
  {
    QLineEdit* edit = m_texture_hash_edits.back();
    m_texture_hash_edits.pop_back();
    m_texture_hash_layout->removeWidget(edit);
    delete edit;
  }

  m_updating_texture_hash_fields = false;
}

void ShaderOverrideAddEditDialog::SanitizeTextureHashField(QLineEdit* edit)
{
  if (edit == nullptr || m_updating_texture_hash_fields)
    return;

  const QRegularExpression separators(QStringLiteral("[,;\\s]+"));
  const QRegularExpression hex_exact(QStringLiteral("^[0-9A-Fa-f]{1,16}$"));
  const QRegularExpression hex_16_anywhere(QStringLiteral("([0-9A-Fa-f]{16})"));

  const auto parts = edit->text().trimmed().split(separators, Qt::SkipEmptyParts);
  if (parts.isEmpty())
    return;

  QStringList sanitized_parts;
  bool changed = false;
  for (const QString& part : parts)
  {
    if (hex_exact.match(part).hasMatch())
    {
      const QString lowered = part.toLower();
      sanitized_parts.push_back(lowered);
      if (lowered != part)
        changed = true;
      continue;
    }

    const auto match = hex_16_anywhere.match(part);
    if (match.hasMatch())
    {
      sanitized_parts.push_back(match.captured(1).toLower());
      changed = true;
      continue;
    }

    sanitized_parts.push_back(part);
  }

  if (!changed)
    return;

  const QString new_text = sanitized_parts.join(QStringLiteral(" "));
  if (new_text == edit->text())
    return;

  const QSignalBlocker blocker(edit);
  edit->setText(new_text);
}
