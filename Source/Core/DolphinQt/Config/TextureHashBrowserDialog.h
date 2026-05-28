// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <string>
#include <vector>

#include <QDialog>
#include <QString>

#include "Common/CommonTypes.h"

class QWidget;

struct TextureHashBrowserEntry
{
  u64 hash = 0;
  std::string name;
};

struct TextureHashBrowserConfig
{
  QString title;
  QString empty_info_text;
  QString current_label;
  std::function<QString()> fetch_current_label;
  std::vector<u64> initial_selected_hashes;
  std::function<std::vector<TextureHashBrowserEntry>()> fetch_current_entries;
  std::function<void(const std::vector<u64>&)> apply_selected_hashes;
  std::function<void(const std::vector<u64>&)> live_selection_changed;
};

QDialog* ShowTextureHashBrowserDialog(QWidget* parent, const TextureHashBrowserConfig& config);
