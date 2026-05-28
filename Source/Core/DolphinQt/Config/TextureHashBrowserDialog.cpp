// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Config/TextureHashBrowserDialog.h"

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include <QAbstractItemView>
#include <QBrush>
#include <QCheckBox>
#include <QColor>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSize>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "Common/FileUtil.h"
#include "Core/ConfigManager.h"

namespace
{
QString ToHashHex(u64 hash)
{
  return QStringLiteral("%1").arg(static_cast<qulonglong>(hash), 16, 16, QLatin1Char('0'));
}

std::unordered_map<u64, QString> FindTexturePreviewPaths(const std::vector<u64>& hashes)
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

QPixmap LoadPreviewPixmapFresh(const QString& path)
{
  QImageReader reader(path);
  reader.setAutoTransform(true);
  const QImage image = reader.read();
  if (image.isNull())
    return {};
  return QPixmap::fromImage(image);
}
}  // namespace

QDialog* ShowTextureHashBrowserDialog(QWidget* parent, const TextureHashBrowserConfig& config)
{
  auto* dlg = new QDialog(parent);
  dlg->setAttribute(Qt::WA_DeleteOnClose);
  dlg->setWindowTitle(config.title);
  dlg->setMinimumSize(620, 320);

  auto* info = new QLabel;
  info->setWordWrap(true);

  auto* continuous_scan_check = new QCheckBox(QObject::tr("Continuous Scan"));
  continuous_scan_check->setToolTip(
      QObject::tr("Continuously refresh while this window is open.\n"
                   "Textures seen during scan stay visible in gray."));

  auto* scan_timer = new QTimer(dlg);
  scan_timer->setInterval(250);

  auto* tree = new QTreeWidget;
  tree->setHeaderLabels({QObject::tr("Use"), QObject::tr("Texture Hash"), QObject::tr("Name"),
                         QObject::tr("Preview")});
  tree->setRootIsDecorated(false);
  tree->setSelectionMode(QAbstractItemView::NoSelection);
  tree->header()->setStretchLastSection(true);
  tree->setIconSize(QSize(64, 64));

  auto selected_hashes = std::make_shared<std::unordered_set<u64>>(
      config.initial_selected_hashes.begin(), config.initial_selected_hashes.end());
  auto scanned_hashes = std::make_shared<std::unordered_map<u64, std::string>>();

  const auto notify_live_selection = [selected_hashes, config]() {
    if (!config.live_selection_changed)
      return;

    std::vector<u64> hashes(selected_hashes->begin(), selected_hashes->end());
    std::sort(hashes.begin(), hashes.end());
    config.live_selection_changed(hashes);
  };

  const auto populate = [tree, info, selected_hashes, scanned_hashes, continuous_scan_check,
                         config, notify_live_selection]() {
    notify_live_selection();
    const QString current_label =
        config.fetch_current_label ? config.fetch_current_label() : config.current_label;

    const auto textures = config.fetch_current_entries ? config.fetch_current_entries() :
                                                         std::vector<TextureHashBrowserEntry>{};

    std::unordered_map<u64, TextureHashBrowserEntry> captured;
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
    texture_hashes.erase(std::unique(texture_hashes.begin(), texture_hashes.end()),
                         texture_hashes.end());

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
      item->setText(1, ToHashHex(hash));

      if (in_current_scene)
      {
        const auto& tex = captured_it->second;
        item->setText(2, tex.name.empty() ? QObject::tr("(unknown)") :
                                           QString::fromStdString(tex.name));
      }
      else
      {
        if (is_saved_only)
        {
          item->setText(2, QObject::tr("(saved filter, not in current selection)"));
        }
        else
        {
          const auto scanned_it = scanned_hashes->find(hash);
          const bool has_scanned_name =
              scanned_it != scanned_hashes->end() && !scanned_it->second.empty();
          item->setText(2, has_scanned_name ? QString::fromStdString(scanned_it->second) :
                                              QObject::tr("(seen during scan, not in current selection)"));
        }
        const QBrush gray_brush(QColor(140, 140, 140));
        item->setForeground(1, gray_brush);
        item->setForeground(2, gray_brush);
      }

      item->setData(0, Qt::UserRole, static_cast<qulonglong>(hash));

      const auto preview_it = preview_paths.find(hash);
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

    for (int i = 0; i < 4; ++i)
      tree->resizeColumnToContents(i);

    if (textures.empty())
    {
      if (saved_only_count > 0 || scanned_only_count > 0)
      {
        info->setText(QObject::tr(
            "%1\nSaved filters and previously scanned textures are shown in gray.")
                          .arg(config.empty_info_text));
      }
      else
      {
        info->setText(config.empty_info_text);
      }
    }
    else
    {
      info->setText(QObject::tr(
          "%1 current texture(s) for %4. %2 saved-only and %3 scanned-only shown in gray.\n"
          "Check rows and press Apply to update Texture Filters.")
                        .arg(textures.size())
                        .arg(saved_only_count)
                        .arg(scanned_only_count)
                        .arg(current_label));
    }
  };

  QObject::connect(tree, &QTreeWidget::itemChanged, dlg,
                   [selected_hashes, notify_live_selection](QTreeWidgetItem* item, int column) {
                     if (!item || column != 0)
                       return;

                     const u64 texture_hash = item->data(0, Qt::UserRole).toULongLong();
                     const bool enabled = item->checkState(0) == Qt::Checked;
                     if (enabled)
                       selected_hashes->insert(texture_hash);
                     else
                       selected_hashes->erase(texture_hash);

                     notify_live_selection();
                   });

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
  auto* refresh_button = buttons->addButton(QObject::tr("Refresh"), QDialogButtonBox::ActionRole);
  auto* apply_button = buttons->addButton(QObject::tr("Apply"), QDialogButtonBox::AcceptRole);

  QObject::connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::close);
  QObject::connect(refresh_button, &QPushButton::clicked, dlg, populate);
  QObject::connect(scan_timer, &QTimer::timeout, dlg, populate);
  QObject::connect(continuous_scan_check, &QCheckBox::toggled, dlg, [scan_timer, populate](bool checked) {
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
  QObject::connect(apply_button, &QPushButton::clicked, dlg, [selected_hashes, config]() {
    if (!config.apply_selected_hashes)
      return;

    std::vector<u64> hashes(selected_hashes->begin(), selected_hashes->end());
    std::sort(hashes.begin(), hashes.end());
    config.apply_selected_hashes(hashes);
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
  return dlg;
}
