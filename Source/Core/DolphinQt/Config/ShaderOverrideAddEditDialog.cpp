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
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QWidget>
#include <QVBoxLayout>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/ConfigManager.h"
#include "DolphinQt/Config/TextureHashBrowserDialog.h"

namespace
{
constexpr double UPM_OVERRIDE_MIN = 0.01;
constexpr double UPM_OVERRIDE_MAX = 1000.0;
constexpr double UPM_OVERRIDE_STEP = 0.01;
constexpr int MAX_VISIBLE_TEXTURE_HASH_ROWS = 10;
constexpr int TEXTURE_HASH_ROW_HEIGHT = 28;

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
  m_match_mode_combo->setToolTip(
      tr("Exact Hash: match only this exact shader hash.\n"
         "Shader Family: use the relaxed shader family signature."));

  m_handling_combo = new QComboBox;
  m_handling_combo->addItem(tr("Skip"), static_cast<int>(ShaderHunter::HandlingType::Skip));
  m_handling_combo->addItem(tr("Screen"), static_cast<int>(ShaderHunter::HandlingType::Screen));
  m_handling_combo->addItem(tr("Fullscreen"),
                             static_cast<int>(ShaderHunter::HandlingType::Fullscreen));
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
  m_texture_hash_scroll = new QScrollArea;
  m_texture_hash_scroll->setWidgetResizable(true);
  m_texture_hash_scroll->setFrameShape(QFrame::NoFrame);
  m_texture_hash_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  m_texture_hash_scroll->setWidget(m_texture_hash_container);
  AddTextureHashField(QString());

  // Pre-fill fields in edit mode
  if (edit_override)
  {
    m_edit_element_reference_total = edit_override->element_reference_total;
    m_edit_family_signature = edit_override->family_signature;
    m_name_edit->setText(QString::fromStdString(edit_override->name));
    m_comments_edit->setPlainText(QString::fromStdString(edit_override->comments));
    m_credits_edit->setText(QString::fromStdString(edit_override->credits));
    m_hash_edit->setText(
        QString::fromStdString(fmt::format("{:016x}", edit_override->hash)));

    const int type_idx = m_type_combo->findData(static_cast<int>(edit_override->type));
    if (type_idx >= 0)
      m_type_combo->setCurrentIndex(type_idx);

    const ShaderHunter::MatchMode initial_match_mode =
        edit_override->match_mode == ShaderHunter::MatchMode::RuntimeElement ?
            ShaderHunter::MatchMode::ExactHash :
            edit_override->match_mode;
    const int match_mode_idx =
        m_match_mode_combo->findData(static_cast<int>(initial_match_mode));
    if (match_mode_idx >= 0)
      m_match_mode_combo->setCurrentIndex(match_mode_idx);

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
  form->addRow(QString(), m_view_textures_button);
  form->addRow(m_texture_mode_label, m_texture_mode_combo);
  form->addRow(tr("Texture Filters:"), m_texture_hash_scroll);
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
  connect(m_condition_combo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
    const bool has_condition = !m_condition_combo->currentData().toString().isEmpty();
    m_condition_mode_label->setEnabled(has_condition);
    m_condition_mode_combo->setEnabled(has_condition);
  });
  connect(m_view_textures_button, &QPushButton::clicked, this,
          &ShaderOverrideAddEditDialog::ShowTextureBrowser);

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

  const char* type_str = shader_type == ShaderHunter::ShaderType::Pixel    ? "PS" :
                         shader_type == ShaderHunter::ShaderType::Vertex   ? "VS" :
                                                                              "GS";

  if (!was_hunting_enabled)
    hunter.SetEnabled(true);
  hunter.SetTextureToolActive(true);

  const auto initial_hashes = CollectTextureHashValues();
  const auto sync_selected_hashes = [shader_hash, shader_type](const std::vector<u64>& hashes) {
    auto& sync_hunter = ShaderHunter::GetInstance();
    if (!sync_hunter.SelectShader(shader_type, shader_hash))
      return;

    sync_hunter.ClearTextureSkipFilters();
    for (u64 selected_hash : hashes)
      sync_hunter.SetTextureSkipEnabled(selected_hash, true);
  };

  TextureHashBrowserConfig browser_config;
  browser_config.title = tr("Textures for %1 %2")
                             .arg(QString::fromLatin1(type_str))
                             .arg(static_cast<qulonglong>(shader_hash), 16, 16,
                                  QLatin1Char('0'));
  browser_config.empty_info_text =
      tr("No textures captured yet for this shader.\n"
         "Shader Hunter was enabled temporarily for this window.");
  browser_config.current_label = tr("%1 %2")
                                     .arg(QString::fromLatin1(type_str))
                                     .arg(static_cast<qulonglong>(shader_hash), 16, 16,
                                          QLatin1Char('0'));
  browser_config.initial_selected_hashes = initial_hashes;
  browser_config.fetch_current_entries = [shader_hash, shader_type]() {
    auto& populate_hunter = ShaderHunter::GetInstance();
    if (!populate_hunter.SelectShader(shader_type, shader_hash))
      return std::vector<TextureHashBrowserEntry>{};

    std::vector<TextureHashBrowserEntry> entries;
    for (const auto& tex : populate_hunter.GetTexturesForHash(shader_type, shader_hash))
      entries.push_back(TextureHashBrowserEntry{.hash = tex.hash, .name = tex.name});
    return entries;
  };
  browser_config.apply_selected_hashes = [this](const std::vector<u64>& hashes) {
    SetTextureHashFields(hashes);
  };
  browser_config.live_selection_changed = sync_selected_hashes;

  auto* dlg = ShowTextureHashBrowserDialog(this, browser_config);

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

  const int visible_rows = std::min(desired_rows, MAX_VISIBLE_TEXTURE_HASH_ROWS);
  const int viewport_height = visible_rows * TEXTURE_HASH_ROW_HEIGHT;
  m_texture_hash_scroll->setMinimumHeight(viewport_height);
  m_texture_hash_scroll->setMaximumHeight(viewport_height);

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
