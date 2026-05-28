// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Config/HideObjectAddEditDialog.h"

#include <algorithm>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

#include "VideoCommon/HideObjectEngine.h"

HideObjectAddEditDialog::HideObjectAddEditDialog(
    QWidget* parent, const HideObjectEngine::HideObject* existing_code,
    const std::vector<HideObjectEngine::HideObject>& all_codes)
    : QDialog(parent), m_all_codes(all_codes)
{
  if (existing_code)
  {
    m_is_edit = true;
    m_original_name = existing_code->name;
    m_result = *existing_code;
    if (!m_result.entries.empty())
      m_current_entry = m_result.entries[0];
  }
  else
  {
    m_current_entry.type = HideObjectEngine::HideObjectType::Bits16;
    m_current_entry.value_upper = 0;
    m_current_entry.value_lower = 0;
  }

  setWindowTitle(m_is_edit ? tr("Edit Hide Object Code") : tr("Add Hide Object Code"));
  setMinimumWidth(400);

  CreateWidgets();
  ConnectWidgets();
  UpdateValueDisplay();
}

void HideObjectAddEditDialog::CreateWidgets()
{
  auto* name_label = new QLabel(tr("Name:"));
  m_name_edit = new QLineEdit;
  if (m_is_edit)
    m_name_edit->setText(QString::fromStdString(m_result.name));
  else
    m_name_edit->setPlaceholderText(tr("Enter code name..."));

  auto* type_label = new QLabel(tr("Size:"));
  m_type_combo = new QComboBox;
  for (int i = 0; i < static_cast<int>(HideObjectEngine::HideObjectType::Count); i++)
    m_type_combo->addItem(QString::fromLatin1(
        HideObjectEngine::GetTypeName(static_cast<HideObjectEngine::HideObjectType>(i))));
  m_type_combo->setCurrentIndex(static_cast<int>(m_current_entry.type));

  auto* value_label = new QLabel(tr("Value (hex):"));
  m_value_edit = new QLineEdit;
  m_value_edit->setFont(QFont(QStringLiteral("Courier New"), 10));

  m_up_button = new QPushButton(tr("Up"));
  m_down_button = new QPushButton(tr("Down"));
  m_range_finder_toggle = new QCheckBox(tr("Range Finder"));
  m_range_label = new QLabel;
  m_range_lower_slider = new QSlider(Qt::Horizontal);
  m_range_upper_slider = new QSlider(Qt::Horizontal);

  m_range_lower_slider->setRange(0, 255);
  m_range_upper_slider->setRange(0, 255);
  m_range_lower_slider->setValue(0);
  m_range_upper_slider->setValue(255);
  UpdateRangeLabel();

  const QString tooltip =
      tr("The Up/Down buttons can be used to find new codes.\n"
         "While the game is playing, find an object you want to hide and select '8bits'.\n"
         "Keep clicking Up until the object disappears.\n"
         "Now choose '16bits' and continue clicking Up until the object is hidden again.\n"
         "Repeat until the code is long enough to be unique.\n"
         "Warning: Too short of a code may hide other objects too.");
  m_up_button->setToolTip(tooltip);
  m_down_button->setToolTip(tooltip);

  auto* button_box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);

  auto* grid = new QGridLayout;
  grid->addWidget(name_label, 0, 0);
  grid->addWidget(m_name_edit, 0, 1, 1, 3);
  grid->addWidget(type_label, 1, 0);
  grid->addWidget(m_type_combo, 1, 1, 1, 3);
  grid->addWidget(value_label, 2, 0);
  grid->addWidget(m_value_edit, 2, 1);
  grid->addWidget(m_up_button, 2, 2);
  grid->addWidget(m_down_button, 2, 3);
  grid->addWidget(m_range_finder_toggle, 3, 0, 1, 4);
  grid->addWidget(m_range_label, 4, 0, 1, 4);
  grid->addWidget(new QLabel(tr("Lower Bound:")), 5, 0);
  grid->addWidget(m_range_lower_slider, 5, 1, 1, 3);
  grid->addWidget(new QLabel(tr("Upper Bound:")), 6, 0);
  grid->addWidget(m_range_upper_slider, 6, 1, 1, 3);

  m_range_label->setEnabled(false);
  m_range_lower_slider->setEnabled(false);
  m_range_upper_slider->setEnabled(false);

  auto* layout = new QVBoxLayout{this};
  layout->addLayout(grid);
  layout->addWidget(button_box);

  connect(button_box, &QDialogButtonBox::accepted, this, &HideObjectAddEditDialog::OnAccept);
  connect(button_box, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void HideObjectAddEditDialog::ConnectWidgets()
{
  connect(m_type_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &HideObjectAddEditDialog::OnTypeChanged);
  connect(m_up_button, &QPushButton::clicked, this, &HideObjectAddEditDialog::OnUpClicked);
  connect(m_down_button, &QPushButton::clicked, this, &HideObjectAddEditDialog::OnDownClicked);
  connect(m_range_finder_toggle, &QCheckBox::toggled, this,
          &HideObjectAddEditDialog::OnRangeFinderToggled);
  connect(m_range_lower_slider, &QSlider::valueChanged, this,
          &HideObjectAddEditDialog::OnRangeBoundsChanged);
  connect(m_range_upper_slider, &QSlider::valueChanged, this,
          &HideObjectAddEditDialog::OnRangeBoundsChanged);
}

void HideObjectAddEditDialog::UpdateValueDisplay()
{
  const int char_len = HideObjectEngine::GetByteCount(m_current_entry.type) * 2;

  if (char_len <= 16)
  {
    // Fits in value_lower
    m_value_edit->setText(
        QStringLiteral("%1").arg(m_current_entry.value_lower, char_len, 16, QLatin1Char('0')).toUpper());
  }
  else
  {
    // Need both upper and lower
    const int upper_chars = char_len - 16;
    m_value_edit->setText(
        QStringLiteral("%1%2")
            .arg(m_current_entry.value_upper, upper_chars, 16, QLatin1Char('0'))
            .arg(m_current_entry.value_lower, 16, 16, QLatin1Char('0'))
            .toUpper());
  }
}

bool HideObjectAddEditDialog::ParseValueFromUI()
{
  const QString text = m_value_edit->text().trimmed();
  if (text.isEmpty())
  {
    QMessageBox::warning(this, tr("Error"), tr("Value cannot be empty."));
    return false;
  }

  const int expected_chars = HideObjectEngine::GetByteCount(m_current_entry.type) * 2;

  bool ok = false;
  if (expected_chars <= 16)
  {
    m_current_entry.value_lower = text.toULongLong(&ok, 16);
    m_current_entry.value_upper = 0;
  }
  else
  {
    // Split: upper portion + lower 16 hex chars
    if (text.length() > 16)
    {
      const QString upper_str = text.left(text.length() - 16);
      const QString lower_str = text.right(16);
      m_current_entry.value_upper = upper_str.toULongLong(&ok, 16);
      if (ok)
        m_current_entry.value_lower = lower_str.toULongLong(&ok, 16);
    }
    else
    {
      m_current_entry.value_upper = 0;
      m_current_entry.value_lower = text.toULongLong(&ok, 16);
    }
  }

  if (!ok)
  {
    QMessageBox::warning(this, tr("Error"),
                         tr("Invalid hex value. Use characters 0-9 and A-F only."));
    return false;
  }

  return true;
}

void HideObjectAddEditDialog::OnTypeChanged()
{
  if (!ParseValueFromUI())
    return;

  const int old_byte_count = HideObjectEngine::GetByteCount(m_current_entry.type);
  m_current_entry.type = static_cast<HideObjectEngine::HideObjectType>(m_type_combo->currentIndex());
  const int new_byte_count = HideObjectEngine::GetByteCount(m_current_entry.type);

  auto shift_left_128 = [](u64& upper, u64& lower, int bits) {
    if (bits <= 0)
      return;
    if (bits >= 128)
    {
      upper = 0;
      lower = 0;
      return;
    }
    if (bits >= 64)
    {
      upper = lower << (bits - 64);
      lower = 0;
      return;
    }

    upper = (upper << bits) | (lower >> (64 - bits));
    lower <<= bits;
  };

  auto shift_right_128 = [](u64& upper, u64& lower, int bits) {
    if (bits <= 0)
      return;
    if (bits >= 128)
    {
      upper = 0;
      lower = 0;
      return;
    }
    if (bits >= 64)
    {
      lower = upper >> (bits - 64);
      upper = 0;
      return;
    }

    lower = (lower >> bits) | (upper << (64 - bits));
    upper >>= bits;
  };

  // Keep current bytes in place relative to the most-significant side when size changes.
  // Example: 8-bit 01 -> 16-bit 0100; and 16-bit 0100 -> 8-bit 01.
  const int shift_bytes = new_byte_count - old_byte_count;
  if (shift_bytes > 0)
    shift_left_128(m_current_entry.value_upper, m_current_entry.value_lower, shift_bytes * 8);
  else if (shift_bytes < 0)
    shift_right_128(m_current_entry.value_upper, m_current_entry.value_lower, -shift_bytes * 8);

  // Truncate to the newly selected size.
  if (new_byte_count <= 8)
  {
    m_current_entry.value_upper = 0;
    if (new_byte_count < 8)
    {
      const u64 mask = (1ULL << (new_byte_count * 8)) - 1;
      m_current_entry.value_lower &= mask;
    }
  }
  else
  {
    const int upper_bytes = new_byte_count - 8;
    if (upper_bytes < 8)
    {
      const u64 mask = (1ULL << (upper_bytes * 8)) - 1;
      m_current_entry.value_upper &= mask;
    }
  }

  UpdateValueDisplay();
  ApplyTemporarily();
}

void HideObjectAddEditDialog::OnUpClicked()
{
  if (!ParseValueFromUI())
    return;

  // Brute-force only the last byte (two hex chars), without carrying into higher bytes.
  const u8 low_byte = static_cast<u8>(m_current_entry.value_lower & 0xFFULL);
  const u8 next_low_byte = static_cast<u8>(low_byte + 1);
  m_current_entry.value_lower = (m_current_entry.value_lower & ~0xFFULL) | next_low_byte;

  UpdateValueDisplay();
  ApplyTemporarily();
}

void HideObjectAddEditDialog::OnDownClicked()
{
  if (!ParseValueFromUI())
    return;

  // Brute-force only the last byte (two hex chars), without borrowing from higher bytes.
  const u8 low_byte = static_cast<u8>(m_current_entry.value_lower & 0xFFULL);
  const u8 prev_low_byte = static_cast<u8>(low_byte - 1);
  m_current_entry.value_lower = (m_current_entry.value_lower & ~0xFFULL) | prev_low_byte;

  UpdateValueDisplay();
  ApplyTemporarily();
}

void HideObjectAddEditDialog::OnRangeFinderToggled(bool enabled)
{
  m_range_label->setEnabled(enabled);
  m_range_lower_slider->setEnabled(enabled);
  m_range_upper_slider->setEnabled(enabled);
  UpdateRangeLabel();
  ApplyTemporarily();
}

void HideObjectAddEditDialog::OnRangeBoundsChanged()
{
  if (sender() == m_range_lower_slider && m_range_lower_slider->value() > m_range_upper_slider->value())
    m_range_upper_slider->setValue(m_range_lower_slider->value());
  else if (sender() == m_range_upper_slider &&
           m_range_upper_slider->value() < m_range_lower_slider->value())
    m_range_lower_slider->setValue(m_range_upper_slider->value());

  UpdateRangeLabel();
  if (!m_range_finder_toggle->isChecked())
    return;

  if (!ParseValueFromUI())
    return;

  ApplyTemporarily();
}

void HideObjectAddEditDialog::UpdateRangeLabel()
{
  const int lower = std::min(m_range_lower_slider->value(), m_range_upper_slider->value());
  const int upper = std::max(m_range_lower_slider->value(), m_range_upper_slider->value());
  m_range_label->setText(
      tr("Current range: %1 - %2")
          .arg(QStringLiteral("%1").arg(lower, 2, 16, QLatin1Char('0')).toUpper())
          .arg(QStringLiteral("%1").arg(upper, 2, 16, QLatin1Char('0')).toUpper()));
}

void HideObjectAddEditDialog::ApplyTemporarily()
{
  // Temporarily apply this single code to the running game for immediate visual feedback.
  std::vector<HideObjectEngine::HideObject> temp_list;

  if (m_range_finder_toggle->isChecked())
  {
    // Range finder: apply all values for the last byte in [lower..upper], fixed upper bytes.
    const int lower = std::min(m_range_lower_slider->value(), m_range_upper_slider->value());
    const int upper = std::max(m_range_lower_slider->value(), m_range_upper_slider->value());
    temp_list.reserve(static_cast<size_t>(upper - lower + 1));

    for (int v = lower; v <= upper; ++v)
    {
      HideObjectEngine::HideObject temp_code;
      temp_code.name = "temp_range";
      temp_code.active = true;
      temp_code.user_defined = false;

      HideObjectEngine::HideObjectEntry entry = m_current_entry;
      entry.value_lower = (entry.value_lower & ~0xFFULL) | static_cast<u64>(v & 0xFF);
      temp_code.entries.push_back(entry);

      temp_list.push_back(std::move(temp_code));
    }
  }
  else
  {
    // Single-value brute force: only the current entry.
    HideObjectEngine::HideObject temp_code;
    temp_code.name = "temp_brute_force";
    temp_code.entries.push_back(m_current_entry);
    temp_code.active = true;
    temp_code.user_defined = false;
    temp_list.push_back(std::move(temp_code));
  }

  HideObjectEngine::Engine::GetInstance().ApplyCodes(temp_list);
}

void HideObjectAddEditDialog::OnAccept()
{
  const QString name = m_name_edit->text().trimmed();
  if (name.isEmpty())
  {
    QMessageBox::warning(this, tr("Error"), tr("Please enter a name for this code."));
    return;
  }

  // Check name uniqueness
  for (const auto& code : m_all_codes)
  {
    if (code.name == name.toStdString() && code.name != m_original_name)
    {
      QMessageBox::warning(this, tr("Error"),
                           tr("Name is already in use. Please choose a unique name."));
      return;
    }
  }

  if (!ParseValueFromUI())
    return;

  m_result.name = name.toStdString();
  m_result.entries.clear();
  m_result.entries.push_back(m_current_entry);
  m_result.active = true;
  m_result.user_defined = true;

  accept();
}
