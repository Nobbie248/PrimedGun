// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/MainWindow.h"

#include "Common/CommonPaths.h"
#include "Common/Version.h"

#include <QApplication>
#include <QActionGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QMenu>
#include <QMimeData>
#include <QLabel>
#include <QKeyEvent>
#include <QMessageBox>
#include <QPixmap>
#include <QSizePolicy>
#include <QButtonGroup>
#include <QRadioButton>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QStackedWidget>
#include <QStyleHints>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <QWindow>

#include <fmt/format.h>

#include <array>
#include <cstring>
#include <future>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#endif

#if defined(__unix__) || defined(__unix) || defined(__APPLE__)
#include <signal.h>

#include "QtUtils/SignalDaemon.h"
#endif

#ifndef _WIN32
#include <qpa/qplatformnativeinterface.h>
#endif

#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/ScopeGuard.h"
#include "Common/Version.h"
#include "Common/WindowSystemInfo.h"

#include "Core/AchievementManager.h"
#include "Core/Boot/Boot.h"
#include "Core/BootManager.h"
#include "Core/CommonTitles.h"
#include "Core/Config/AchievementSettings.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/UISettings.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/FreeLookManager.h"
#include "Core/HW/DVD/DVDInterface.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/EXI/EXI_Device.h"
#include "Core/HW/GBAPad.h"
#include "Core/HW/AddressSpace.h"
#include "Core/HW/GCKeyboard.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/Wiimote.h"
#include "Core/HotkeyManager.h"
#include "Core/IOS/USB/Bluetooth/BTEmu.h"
#include "Core/Movie.h"
#include "Core/PrimedGun/NativeRuntime.h"
#include "Core/PrimedGun/PPCTrace.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/State.h"
#include "Core/System.h"
#include "Core/WiiUtils.h"

#include "DiscIO/DirectoryBlob.h"
#include "DiscIO/Enums.h"
#include "DiscIO/NANDImporter.h"
#include "DiscIO/RiivolutionPatcher.h"

#include "DolphinQt/AboutDialog.h"
#include "DolphinQt/Achievements/AchievementsWindow.h"
#include "DolphinQt/CheatsManager.h"
#include "DolphinQt/Config/FreeLookWindow.h"
#include "DolphinQt/Config/LogConfigWidget.h"
#include "DolphinQt/Config/LogWidget.h"
#include "DolphinQt/Config/Mapping/MappingWindow.h"
#include "DolphinQt/Config/PropertiesDialog.h"
#include "DolphinQt/Config/SettingsWindow.h"
#include "DolphinQt/ConvertDialog.h"
#include "DolphinQt/Debugger/AssemblerWidget.h"
#include "DolphinQt/Debugger/BreakpointWidget.h"
#include "DolphinQt/Debugger/CodeViewWidget.h"
#include "DolphinQt/Debugger/CodeWidget.h"
#include "DolphinQt/Debugger/JITWidget.h"
#include "DolphinQt/Debugger/MemoryWidget.h"
#include "DolphinQt/Debugger/NetworkWidget.h"
#include "DolphinQt/Debugger/RegisterWidget.h"
#include "DolphinQt/Debugger/ThreadWidget.h"
#include "DolphinQt/Debugger/WatchWidget.h"
#include "DolphinQt/DiscordHandler.h"
#include "DolphinQt/EmulatedUSB/LogitechMicWindow.h"
#include "DolphinQt/EmulatedUSB/WiiSpeakWindow.h"
#include "DolphinQt/FIFO/FIFOPlayerWindow.h"
#include "DolphinQt/GCMemcardManager.h"
#include "DolphinQt/GameList/GameList.h"
#include "DolphinQt/Host.h"
#include "DolphinQt/HotkeyScheduler.h"
#include "DolphinQt/InfinityBase/InfinityBaseWindow.h"
#include "DolphinQt/MenuBar.h"
#include "DolphinQt/NKitWarningDialog.h"
#include "DolphinQt/QtUtils/DolphinFileDialog.h"
#include "DolphinQt/QtUtils/FileOpenEventFilter.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/QtUtils/ParallelProgressDialog.h"
#include "DolphinQt/QtUtils/QueueOnObject.h"
#include "DolphinQt/QtUtils/RunOnObject.h"
#include "DolphinQt/QtUtils/WindowActivationEventFilter.h"
#include "DolphinQt/RenderWidget.h"
#include "DolphinQt/ResourcePackManager.h"
#include "DolphinQt/Resources.h"
#include "DolphinQt/RiivolutionBootWidget.h"
#include "DolphinQt/SearchBar.h"
#include "DolphinQt/Settings.h"
#include "DolphinQt/SkylanderPortal/SkylanderPortalWindow.h"
#include "DolphinQt/TAS/GBATASInputWindow.h"
#include "DolphinQt/TAS/GCTASInputWindow.h"
#include "DolphinQt/TAS/WiiTASInputWindow.h"
#include "DolphinQt/ToolBar.h"
#include "DolphinQt/WiiUpdate.h"

#include "UICommon/DiscordPresence.h"
#include "UICommon/GameFile.h"
#include "UICommon/ResourcePack/Manager.h"
#include "UICommon/ResourcePack/ResourcePack.h"

#include "UICommon/UICommon.h"

#include "VideoCommon/HiresTextures.h"
#include "VideoCommon/AsyncRequests.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/VideoConfig.h"

namespace
{
constexpr const char* PRIMEGUN_CANNON_GAME_ID = "GM8E01";
constexpr const char* PRIMEGUN_CANNON_PACK_FOLDER = "000_PrimedGunCannon";
constexpr const char* PRIMEGUN_CANNON_LIBRARY_FOLDER = "PrimedGun/CannonTextures";
constexpr std::array<const char*, 3> PRIMEGUN_CANNON_TEXTURE_NAMES = {
    "tex1_128x128_m_3c6ded49d64d30f2_14",
    "tex1_128x128_m_bec6d78ea7dd739e_14",
    "tex1_64x64_m_c7625e7ecd9cd5c2_14",
};
constexpr int PRIMEGUN_CANNON_SHEEN_TEXTURE_INDEX = 2;
constexpr std::array<const char*, 3> PRIMEGUN_CANNON_TEXTURE_LABELS = {
    "Cannon texture A",
    "Cannon texture B",
    "Shine mask",
};

QString PrimedGunCannonPackDir()
{
  return QDir::cleanPath(QString::fromStdString(File::GetUserPath(D_HIRESTEXTURES_IDX)) +
                         QLatin1Char('/') + QString::fromLatin1(PRIMEGUN_CANNON_PACK_FOLDER));
}

QString PrimedGunCannonLibraryDir()
{
  return QDir::cleanPath(QString::fromStdString(File::GetUserPath(D_LOAD_IDX)) +
                         QLatin1Char('/') + QString::fromLatin1(PRIMEGUN_CANNON_LIBRARY_FOLDER));
}

QString PrimedGunCannonAppLibraryDir()
{
  return QDir::cleanPath(QString::fromStdString(File::GetSysDirectory()) +
                         QStringLiteral("../") + QString::fromLatin1(PORTABLE_USER_DIR) +
                         QStringLiteral("/Load/") +
                         QString::fromLatin1(PRIMEGUN_CANNON_LIBRARY_FOLDER));
}

QString PrimedGunCannonLibraryFilePath(const QString& relative_path)
{
  const QString user_path = QDir::cleanPath(PrimedGunCannonLibraryDir() + QLatin1Char('/') +
                                           relative_path);
  if (QFileInfo(user_path).isFile())
    return user_path;

  const QString app_path = QDir::cleanPath(PrimedGunCannonAppLibraryDir() + QLatin1Char('/') +
                                          relative_path);
  if (QFileInfo(app_path).isFile())
    return app_path;

  return user_path;
}

void PrimedGunAddUniquePath(QStringList* paths, const QString& path)
{
  const QString clean_path = QDir::cleanPath(path);
  if (!paths->contains(clean_path, Qt::CaseInsensitive))
    paths->append(clean_path);
}

bool PrimedGunLooksLikeMemoryCard(const QFileInfo& file)
{
  if (!file.isFile())
    return false;

  const QString suffix = file.suffix().toLower();
  if (file.fileName().contains(QStringLiteral(".backup-"), Qt::CaseInsensitive))
    return false;

  return suffix == QStringLiteral("raw") || suffix == QStringLiteral("gcp");
}

int PrimedGunMemoryCardScore(const QFileInfo& file)
{
  const QString name = file.fileName().toLower();
  int score = 0;
  if (name == QStringLiteral("online.usa.raw"))
    score += 100;
  if (name == QStringLiteral("memorycarda.usa.raw"))
    score += 90;
  if (name.contains(QStringLiteral(".usa.")) || name.endsWith(QStringLiteral(".usa.raw")) ||
      name.endsWith(QStringLiteral(".usa.gcp")))
  {
    score += 50;
  }
  if (name.contains(QStringLiteral("memorycarda")) || name.contains(QStringLiteral("online")))
    score += 25;
  if (file.suffix().compare(QStringLiteral("raw"), Qt::CaseInsensitive) == 0)
    score += 10;
  return score;
}

QString PrimedGunBestMemoryCardInDir(const QString& dir_path)
{
  const QDir dir(dir_path);
  if (!dir.exists())
    return {};

  const QFileInfoList files =
      dir.entryInfoList({QStringLiteral("*.raw"), QStringLiteral("*.gcp")}, QDir::Files,
                        QDir::Time);
  QFileInfo best_file;
  int best_score = -1;
  for (const QFileInfo& file : files)
  {
    if (!PrimedGunLooksLikeMemoryCard(file))
      continue;

    const int score = PrimedGunMemoryCardScore(file);
    if (score > best_score)
    {
      best_score = score;
      best_file = file;
    }
  }

  return best_file.exists() ? best_file.absoluteFilePath() : QString{};
}

void PrimedGunAddMemoryCardSearchBase(QStringList* search_dirs, const QString& candidate_base)
{
  PrimedGunAddUniquePath(search_dirs, candidate_base);
  PrimedGunAddUniquePath(search_dirs, candidate_base + QStringLiteral("/GC"));
  PrimedGunAddUniquePath(search_dirs, candidate_base + QStringLiteral("/User/GC"));
  PrimedGunAddUniquePath(search_dirs, candidate_base + QStringLiteral("/x64/User/GC"));
  PrimedGunAddUniquePath(search_dirs, candidate_base + QStringLiteral("/Binary/User/GC"));
  PrimedGunAddUniquePath(search_dirs, candidate_base + QStringLiteral("/Binary/x64/User/GC"));
  PrimedGunAddUniquePath(search_dirs, candidate_base + QStringLiteral("/build/bin/User/GC"));
  PrimedGunAddUniquePath(search_dirs, candidate_base + QStringLiteral("/build/bin/x64/User/GC"));
  PrimedGunAddUniquePath(search_dirs, candidate_base + QStringLiteral("/core/User/GC"));
}

QStringList PrimedGunOneFolderAboveX64MemoryCardSearchDirs()
{
  QStringList search_dirs;
  const QFileInfo app_dir(QApplication::applicationDirPath());
  const QString x64_dir = app_dir.absoluteFilePath();
  const QString install_root = app_dir.absoluteDir().absolutePath();

  // First pass: folders beside x64 in the same release/runtime folder.
  const QDir install_root_dir(install_root);
  const QFileInfoList install_root_children =
      install_root_dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);
  for (const QFileInfo& child : install_root_children)
  {
    if (QDir::cleanPath(child.absoluteFilePath()) == QDir::cleanPath(x64_dir))
      continue;
    PrimedGunAddMemoryCardSearchBase(&search_dirs, child.absoluteFilePath());
  }

  return search_dirs;
}

QStringList PrimedGunTwoFoldersAboveX64MemoryCardSearchDirs()
{
  QStringList search_dirs;
  const QFileInfo app_dir(QApplication::applicationDirPath());
  const QString install_root = app_dir.absoluteDir().absolutePath();
  const QString install_parent = QFileInfo(install_root).absoluteDir().absolutePath();

  // Fallback pass: folders beside the current release/runtime folder.
  PrimedGunAddUniquePath(&search_dirs, install_parent + QStringLiteral("/GC"));
  PrimedGunAddUniquePath(&search_dirs, install_parent + QStringLiteral("/User/GC"));
  PrimedGunAddUniquePath(&search_dirs, install_parent + QStringLiteral("/core/User/GC"));

  const QDir parent_dir(install_parent);
  const QFileInfoList siblings =
      parent_dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);
  for (const QFileInfo& sibling : siblings)
  {
    if (QDir::cleanPath(sibling.absoluteFilePath()) == QDir::cleanPath(install_root))
      continue;
    PrimedGunAddMemoryCardSearchBase(&search_dirs, sibling.absoluteFilePath());
  }

  return search_dirs;
}

bool PrimedGunSamePath(const QString& a, const QString& b)
{
#ifdef _WIN32
  constexpr Qt::CaseSensitivity path_case_sensitivity = Qt::CaseInsensitive;
#else
  constexpr Qt::CaseSensitivity path_case_sensitivity = Qt::CaseSensitive;
#endif
  return QString::compare(QDir::cleanPath(QFileInfo(a).absoluteFilePath()),
                          QDir::cleanPath(QFileInfo(b).absoluteFilePath()),
                          path_case_sensitivity) == 0;
}

bool PrimedGunUserDirHasNearbyExe(const QString& user_dir_path)
{
  const QDir user_dir(user_dir_path);
  if (!user_dir.exists())
    return false;

  const QFileInfo user_dir_info(user_dir.absolutePath());
  const QDir install_dir = user_dir_info.absoluteDir();
  QStringList candidates;
  PrimedGunAddUniquePath(&candidates, install_dir.filePath(QStringLiteral("PrimedGun.exe")));
  PrimedGunAddUniquePath(&candidates, install_dir.filePath(QStringLiteral("x64/PrimedGun.exe")));
  PrimedGunAddUniquePath(&candidates, install_dir.filePath(QStringLiteral("Binary/PrimedGun.exe")));
  PrimedGunAddUniquePath(&candidates,
                         install_dir.filePath(QStringLiteral("Binary/x64/PrimedGun.exe")));
  PrimedGunAddUniquePath(&candidates,
                         install_dir.filePath(QStringLiteral("build/bin/PrimedGun.exe")));
  PrimedGunAddUniquePath(&candidates,
                         install_dir.filePath(QStringLiteral("build/bin/x64/PrimedGun.exe")));

  for (const QString& candidate : candidates)
  {
    const QFileInfo exe_info(candidate);
    if (exe_info.isFile() &&
        exe_info.fileName().compare(QStringLiteral("PrimedGun.exe"), Qt::CaseInsensitive) == 0)
    {
      return true;
    }
  }

  return false;
}

QString PrimedGunFindOldMemoryCardInDirs(const QStringList& search_dirs,
                                         const QString& current_memory_card)
{
  QString best_path;
  int best_score = -1;
  for (const QString& dir : search_dirs)
  {
    const QString candidate = PrimedGunBestMemoryCardInDir(dir);
    if (candidate.isEmpty())
      continue;
    if (PrimedGunSamePath(candidate, current_memory_card))
      continue;

    const QFileInfo candidate_info(candidate);
    const int score = PrimedGunMemoryCardScore(candidate_info);
    if (score > best_score)
    {
      best_score = score;
      best_path = candidate_info.absoluteFilePath();
    }
  }

  return best_path;
}

QString PrimedGunFindNearbyOldMemoryCard(const QString& current_memory_card)
{
  QString source = PrimedGunFindOldMemoryCardInDirs(
      PrimedGunOneFolderAboveX64MemoryCardSearchDirs(), current_memory_card);
  if (!source.isEmpty())
    return source;

  return PrimedGunFindOldMemoryCardInDirs(PrimedGunTwoFoldersAboveX64MemoryCardSearchDirs(),
                                          current_memory_card);
}

QString PrimedGunUserDirNearMemoryCard(const QString& memory_card_path)
{
  const QFileInfo memory_card_info(memory_card_path);
  const QDir gc_dir = memory_card_info.absoluteDir();
  const QFileInfo user_dir_info(gc_dir.absolutePath());
  return user_dir_info.absoluteDir().absolutePath();
}

QString PrimedGunConfigDirNearMemoryCard(const QString& memory_card_path)
{
  const QString user_dir_path = PrimedGunUserDirNearMemoryCard(memory_card_path);
  if (user_dir_path.isEmpty())
    return {};

  const QString config_dir_path = QDir(user_dir_path).filePath(QStringLiteral("Config"));
  return QFileInfo(config_dir_path).isDir() ? config_dir_path : QString{};
}

QString PrimedGunFindSettingsNearMemoryCard(const QString& memory_card_path)
{
  const QString user_dir_path = PrimedGunUserDirNearMemoryCard(memory_card_path);
  if (!PrimedGunUserDirHasNearbyExe(user_dir_path))
    return {};

  const QString config_dir_path = PrimedGunConfigDirNearMemoryCard(memory_card_path);
  if (config_dir_path.isEmpty())
    return {};

  const QString qt_settings_path = QDir(config_dir_path).filePath(QStringLiteral("Qt.ini"));
  return QFileInfo::exists(qt_settings_path) ? qt_settings_path : QString{};
}

bool PrimedGunShouldImportSettingKey(const QString& key)
{
  // Keep transfer focused on user-facing PrimeGun preferences. Runtime activation and old
  // path/history fields should stay owned by the current install.
  return key != QStringLiteral("enabled") &&
         key != QStringLiteral("last_memcard_transfer_dir");
}

int PrimedGunImportSettingsFromFile(const QString& settings_path, QSettings* destination_settings)
{
  if (settings_path.isEmpty() || destination_settings == nullptr)
    return 0;

  QSettings source_settings(settings_path, QSettings::IniFormat);
  source_settings.beginGroup(QStringLiteral("primegun"));
  const QStringList keys = source_settings.allKeys();

  int imported_count = 0;
  for (const QString& key : keys)
  {
    if (!PrimedGunShouldImportSettingKey(key))
      continue;

    destination_settings->setValue(QStringLiteral("primegun/%1").arg(key),
                                   source_settings.value(key));
    ++imported_count;
  }

  source_settings.endGroup();
  const QString selected_game_key = QStringLiteral("mainwindow/selected_metroid_prime_path");
  if (source_settings.contains(selected_game_key))
  {
    destination_settings->setValue(selected_game_key, source_settings.value(selected_game_key));
    ++imported_count;
  }

  if (imported_count > 0)
    destination_settings->sync();
  return imported_count;
}

bool PrimedGunShouldTransferDolphinSettingsFile(const QString& relative_path)
{
  const QFileInfo file(relative_path);
  const QString file_name = file.fileName();
  if (file_name.compare(QStringLiteral("Qt.ini"), Qt::CaseInsensitive) == 0 ||
      file_name.compare(QStringLiteral("Logger.ini"), Qt::CaseInsensitive) == 0 ||
      file_name.compare(QStringLiteral("TimePlayed.ini"), Qt::CaseInsensitive) == 0)
  {
    return false;
  }

  if (file_name.contains(QStringLiteral(".backup-"), Qt::CaseInsensitive))
    return false;

  return file.suffix().compare(QStringLiteral("ini"), Qt::CaseInsensitive) == 0;
}

bool PrimedGunCopySettingsTree(const QString& source_dir_path, const QString& destination_dir_path,
                               const QString& timestamp, int* copied_count,
                               QString* error_message)
{
  const QDir source_dir(source_dir_path);
  if (!source_dir.exists())
    return true;

  QDir destination_dir(destination_dir_path);
  if (!destination_dir.exists() && !destination_dir.mkpath(QStringLiteral(".")))
  {
    if (error_message != nullptr)
      *error_message = QObject::tr("Could not create settings folder:\n%1").arg(destination_dir_path);
    return false;
  }

  QDirIterator iterator(source_dir_path, QStringList{QStringLiteral("*.ini")}, QDir::Files,
                        QDirIterator::Subdirectories);
  while (iterator.hasNext())
  {
    const QString source_path = iterator.next();
    const QString relative_path = source_dir.relativeFilePath(source_path);
    if (!PrimedGunShouldTransferDolphinSettingsFile(relative_path))
      continue;

    const QString destination_path = destination_dir.filePath(relative_path);
    if (PrimedGunSamePath(source_path, destination_path))
      continue;

    const QFileInfo destination_info(destination_path);
    QDir destination_file_dir = destination_info.absoluteDir();
    if (!destination_file_dir.exists() && !destination_file_dir.mkpath(QStringLiteral(".")))
    {
      if (error_message != nullptr)
      {
        *error_message = QObject::tr("Could not create settings folder:\n%1")
                             .arg(destination_file_dir.absolutePath());
      }
      return false;
    }

    QString backup_path;
    if (destination_info.exists())
    {
      const QString suffix = destination_info.suffix();
      const QString backup_name =
          suffix.isEmpty() ?
              QStringLiteral("%1.backup-%2").arg(destination_info.fileName(), timestamp) :
              QStringLiteral("%1.backup-%2.%3")
                  .arg(destination_info.completeBaseName(), timestamp, suffix);
      backup_path = destination_file_dir.filePath(backup_name);
      if (!QFile::rename(destination_path, backup_path))
      {
        if (error_message != nullptr)
          *error_message =
              QObject::tr("Could not back up current Dolphin setting:\n%1").arg(destination_path);
        return false;
      }
    }

    if (!QFile::copy(source_path, destination_path))
    {
      if (!backup_path.isEmpty())
        QFile::rename(backup_path, destination_path);
      if (error_message != nullptr)
        *error_message =
            QObject::tr("Could not copy Dolphin setting to:\n%1").arg(destination_path);
      return false;
    }

    if (copied_count != nullptr)
      ++*copied_count;
  }

  return true;
}

int PrimedGunTransferDolphinSettingsNearMemoryCard(const QString& memory_card_path,
                                                   QString* source_user_dir,
                                                   QString* error_message)
{
  const QString old_user_dir_path = PrimedGunUserDirNearMemoryCard(memory_card_path);
  if (old_user_dir_path.isEmpty())
    return 0;
  if (!PrimedGunUserDirHasNearbyExe(old_user_dir_path))
    return 0;

  const QString current_user_dir_path =
      QDir::cleanPath(QString::fromStdString(File::GetUserPath(D_USER_IDX)));
  if (PrimedGunSamePath(old_user_dir_path, current_user_dir_path))
    return 0;

  if (source_user_dir != nullptr)
    *source_user_dir = old_user_dir_path;

  const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss"));
  int copied_count = 0;
  const QDir old_user_dir(old_user_dir_path);
  const QDir current_user_dir(current_user_dir_path);

  if (!PrimedGunCopySettingsTree(old_user_dir.filePath(QStringLiteral("Config")),
                                 current_user_dir.filePath(QStringLiteral("Config")), timestamp,
                                 &copied_count, error_message))
  {
    return -1;
  }

  if (!PrimedGunCopySettingsTree(old_user_dir.filePath(QStringLiteral("GameSettings")),
                                 current_user_dir.filePath(QStringLiteral("GameSettings")),
                                 timestamp, &copied_count, error_message))
  {
    return -1;
  }

  if (!PrimedGunCopySettingsTree(old_user_dir.filePath(QStringLiteral("GameSettingsVR")),
                                 current_user_dir.filePath(QStringLiteral("GameSettingsVR")),
                                 timestamp, &copied_count, error_message))
  {
    return -1;
  }

  return copied_count;
}

QString PrimedGunCannonSlotDir(int slot)
{
  return QDir::cleanPath(PrimedGunCannonLibraryDir() +
                         QStringLiteral("/slot_%1").arg(slot));
}

QString PrimedGunCannonCustomDir()
{
  return QDir::cleanPath(PrimedGunCannonLibraryDir() + QStringLiteral("/custom"));
}

QString PrimedGunCannonRemoveShinePresetPath()
{
  return PrimedGunCannonLibraryFilePath(
      QStringLiteral("presets/remove_shine/") +
      QString::fromLatin1(PRIMEGUN_CANNON_TEXTURE_NAMES[PRIMEGUN_CANNON_SHEEN_TEXTURE_INDEX]) +
      QStringLiteral(".dds"));
}

QString PrimedGunCannonRestoreShinePresetPath(int slot)
{
  return PrimedGunCannonLibraryFilePath(
      QStringLiteral("presets/restore_shine/slot_%1/").arg(slot) +
      QString::fromLatin1(PRIMEGUN_CANNON_TEXTURE_NAMES[PRIMEGUN_CANNON_SHEEN_TEXTURE_INDEX]) +
      QStringLiteral(".dds"));
}

QString PrimedGunCannonUserTexturePackSourcePath()
{
  return QDir::cleanPath(QString::fromStdString(File::GetUserPath(D_HIRESTEXTURES_IDX)) +
                         QLatin1Char('/') + QString::fromLatin1(PRIMEGUN_CANNON_GAME_ID) +
                         QLatin1Char('/') +
                         QString::fromLatin1(
                             PRIMEGUN_CANNON_TEXTURE_NAMES[PRIMEGUN_CANNON_SHEEN_TEXTURE_INDEX]) +
                         QStringLiteral(".dds"));
}

QString PrimedGunCannonSourcePath(int slot, int texture_index, const QString& extension)
{
  return PrimedGunCannonSlotDir(slot) + QLatin1Char('/') +
         QString::fromLatin1(PRIMEGUN_CANNON_TEXTURE_NAMES[texture_index]) + extension;
}

QString PrimedGunCannonExistingSourcePath(int slot, int texture_index, const QString& extension)
{
  return PrimedGunCannonLibraryFilePath(
      QStringLiteral("slot_%1/").arg(slot) +
      QString::fromLatin1(PRIMEGUN_CANNON_TEXTURE_NAMES[texture_index]) + extension);
}

QString PrimedGunCannonDefaultPreviewPath(int texture_index)
{
  const QString relative_base = QStringLiteral("default/") +
                               QString::fromLatin1(PRIMEGUN_CANNON_TEXTURE_NAMES[texture_index]);
  const QString dds_path = PrimedGunCannonLibraryFilePath(relative_base + QStringLiteral(".dds"));
  if (QFileInfo(dds_path).isFile())
    return dds_path;

  const QString png_path = PrimedGunCannonLibraryFilePath(relative_base + QStringLiteral(".png"));
  if (QFileInfo(png_path).isFile())
    return png_path;

  return {};
}

QString PrimedGunCannonActivePath(int texture_index, const QString& extension)
{
  return PrimedGunCannonPackDir() + QLatin1Char('/') +
         QString::fromLatin1(PRIMEGUN_CANNON_TEXTURE_NAMES[texture_index]) + extension;
}

void PrimedGunRemoveCannonSlotTextureFiles(int slot, int texture_index)
{
  QFile::remove(PrimedGunCannonSourcePath(slot, texture_index, QStringLiteral(".png")));
  QFile::remove(PrimedGunCannonSourcePath(slot, texture_index, QStringLiteral(".dds")));
}

void PrimedGunRemoveActiveCannonTextureFiles(int texture_index)
{
  QFile::remove(PrimedGunCannonActivePath(texture_index, QStringLiteral(".png")));
  QFile::remove(PrimedGunCannonActivePath(texture_index, QStringLiteral(".dds")));
}

QString PrimedGunCannonSlotSetting(int slot, int texture_index)
{
  return QStringLiteral("primegun/cannon_texture_slot_%1_target_%2").arg(slot).arg(texture_index);
}

QString PrimedGunResolveCannonTextureSource(int slot, int texture_index, const QString& stored_source)
{
  const QString normalized_stored_source = QDir::fromNativeSeparators(stored_source);
  const bool uses_legacy_primegun_folder =
      normalized_stored_source.contains(QStringLiteral("/Load/PrimedGun/"));

  if (!stored_source.isEmpty() && !uses_legacy_primegun_folder &&
      QFileInfo(stored_source).isFile())
    return stored_source;

  const QString dds_source =
      PrimedGunCannonExistingSourcePath(slot, texture_index, QStringLiteral(".dds"));
  if (QFileInfo(dds_source).isFile())
    return dds_source;

  const QString png_source =
      PrimedGunCannonExistingSourcePath(slot, texture_index, QStringLiteral(".png"));
  if (QFileInfo(png_source).isFile())
    return png_source;

  return stored_source;
}

QString PrimedGunCannonSlotName(int slot)
{
  if (slot == 0)
    return QObject::tr("Default");
  if (slot == 5)
    return QObject::tr("Custom");
  return QObject::tr("Slot %1").arg(slot);
}

bool PrimedGunEnsureCannonPackRegistration()
{
  QDir pack_dir(PrimedGunCannonPackDir());
  if (!pack_dir.exists() && !pack_dir.mkpath(QStringLiteral(".")))
    return false;

  if (!pack_dir.mkpath(QStringLiteral("gameids")))
    return false;

  QFile game_id_file(pack_dir.filePath(QStringLiteral("gameids/%1.txt").arg(
      QString::fromLatin1(PRIMEGUN_CANNON_GAME_ID))));
  if (!game_id_file.exists())
  {
    if (!game_id_file.open(QIODevice::WriteOnly | QIODevice::Text))
      return false;
    game_id_file.write("PrimedGun managed cannon texture override\n");
  }

  return true;
}

bool PrimedGunEnsureRemoveShinePreset(QString* error)
{
  const QString preset_path = PrimedGunCannonRemoveShinePresetPath();
  QFileInfo preset_info(preset_path);
  if (preset_info.exists() && preset_info.isFile())
    return true;

  QDir preset_dir(preset_info.absolutePath());
  if (!preset_dir.exists() && !preset_dir.mkpath(QStringLiteral(".")))
  {
    if (error)
      *error = QObject::tr("Could not create the remove-shine preset folder.");
    return false;
  }

  const QString source_path = PrimedGunCannonUserTexturePackSourcePath();
  if (!QFileInfo::exists(source_path))
  {
    if (error)
      *error = QObject::tr("Could not find the remove-shine DDS at:\n%1").arg(source_path);
    return false;
  }

  if (!QFile::copy(source_path, preset_path))
  {
    if (error)
      *error = QObject::tr("Could not save the remove-shine DDS into:\n%1").arg(preset_path);
    return false;
  }

  return true;
}

bool PrimedGunBackupSlotShinePreset(int slot, QString* error)
{
  const QString backup_path = PrimedGunCannonRestoreShinePresetPath(slot);
  if (QFileInfo(backup_path).isFile())
    return true;

  const QString source = PrimedGunResolveCannonTextureSource(
      slot, PRIMEGUN_CANNON_SHEEN_TEXTURE_INDEX, QString());
  if (source.isEmpty())
    return true;

  const QString remove_shine_path = QDir::cleanPath(PrimedGunCannonRemoveShinePresetPath());
  if (QDir::cleanPath(source) == remove_shine_path)
    return true;

  QFileInfo backup_info(backup_path);
  QDir backup_dir(backup_info.absolutePath());
  if (!backup_dir.exists() && !backup_dir.mkpath(QStringLiteral(".")))
  {
    if (error)
      *error = QObject::tr("Could not create the restore-shine preset folder.");
    return false;
  }

  QFile::remove(backup_path);
  if (!QFile::copy(source, backup_path))
  {
    if (error)
      *error = QObject::tr("Could not save the current slot shine texture into:\n%1").arg(backup_path);
    return false;
  }

  return true;
}

void PrimedGunSetCannonPathLabel(QLabel* label, const QString& text)
{
  label->setText(text);
  label->setToolTip(text);
}

void PrimedGunTouchFile(const QString& path)
{
  QFile file(path);
  if (file.open(QIODevice::ReadWrite))
    file.setFileTime(QDateTime::currentDateTimeUtc(), QFileDevice::FileModificationTime);
}

QImage PrimedGunDecodeDxt1Preview(const QString& path)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return {};

  const QByteArray bytes = file.readAll();
  const auto data = reinterpret_cast<const unsigned char*>(bytes.constData());
  const int size = bytes.size();
  if (size < 128 || bytes.left(4) != QByteArrayLiteral("DDS "))
    return {};

  const auto read_u16 = [data](int offset) {
    return static_cast<quint16>(data[offset] | (data[offset + 1] << 8));
  };
  const auto read_u32 = [data](int offset) {
    return static_cast<quint32>(data[offset] | (data[offset + 1] << 8) | (data[offset + 2] << 16) |
                              (data[offset + 3] << 24));
  };

  const quint32 height = read_u32(12);
  const quint32 width = read_u32(16);
  const quint32 fourcc = read_u32(84);
  if (width == 0 || height == 0 || fourcc != 0x31545844)  // DXT1
    return {};

  QImage image(static_cast<int>(width), static_cast<int>(height), QImage::Format_RGBA8888);
  image.fill(Qt::transparent);

  const auto expand_565 = [](quint16 color) {
    const int r5 = (color >> 11) & 0x1f;
    const int g6 = (color >> 5) & 0x3f;
    const int b5 = color & 0x1f;
    return qRgba((r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2), 255);
  };

  const int blocks_x = static_cast<int>((width + 3) / 4);
  const int blocks_y = static_cast<int>((height + 3) / 4);
  int offset = 128;
  for (int block_y = 0; block_y < blocks_y; ++block_y)
  {
    for (int block_x = 0; block_x < blocks_x; ++block_x)
    {
      if (offset + 8 > size)
        return image;

      const quint16 c0 = read_u16(offset);
      const quint16 c1 = read_u16(offset + 2);
      const quint32 indices = read_u32(offset + 4);
      offset += 8;

      std::array<QRgb, 4> colors{};
      colors[0] = expand_565(c0);
      colors[1] = expand_565(c1);

      const auto mix = [](QRgb a, QRgb b, int aw, int bw, int div, int alpha = 255) {
        return qRgba((qRed(a) * aw + qRed(b) * bw) / div,
                     (qGreen(a) * aw + qGreen(b) * bw) / div,
                     (qBlue(a) * aw + qBlue(b) * bw) / div, alpha);
      };

      if (c0 > c1)
      {
        colors[2] = mix(colors[0], colors[1], 2, 1, 3);
        colors[3] = mix(colors[0], colors[1], 1, 2, 3);
      }
      else
      {
        colors[2] = mix(colors[0], colors[1], 1, 1, 2);
        colors[3] = qRgba(0, 0, 0, 0);
      }

      for (int y = 0; y < 4; ++y)
      {
        for (int x = 0; x < 4; ++x)
        {
          const int pixel_x = block_x * 4 + x;
          const int pixel_y = block_y * 4 + y;
          if (pixel_x >= static_cast<int>(width) || pixel_y >= static_cast<int>(height))
            continue;

          const int index = (indices >> (2 * (y * 4 + x))) & 0x3;
          image.setPixel(pixel_x, pixel_y, colors[index]);
        }
      }
    }
  }

  return image;
}

QImage PrimedGunLoadCannonTexturePreview(const QString& path)
{
  if (path.isEmpty())
    return {};

  QImage image(path);
  if (!image.isNull())
    return image;

  if (QFileInfo(path).suffix().compare(QStringLiteral("dds"), Qt::CaseInsensitive) == 0)
    return PrimedGunDecodeDxt1Preview(path);

  return {};
}

void PrimedGunSetCannonPreviewLabel(QLabel* label, const QString& path)
{
  label->setToolTip(path);
  const QImage image = PrimedGunLoadCannonTexturePreview(path);
  if (image.isNull())
  {
    label->clear();
    label->setText(QObject::tr("No preview"));
    return;
  }

  label->setText(QString());
  label->setPixmap(QPixmap::fromImage(image).scaled(64, 64, Qt::KeepAspectRatio,
                                                    Qt::SmoothTransformation));
}

void PrimedGunClearActiveCannonTextures()
{
  for (const char* texture_name : PRIMEGUN_CANNON_TEXTURE_NAMES)
  {
    const QString base = PrimedGunCannonPackDir() + QLatin1Char('/') + QString::fromLatin1(texture_name);
    QFile::remove(base + QStringLiteral(".png"));
    QFile::remove(base + QStringLiteral(".dds"));
  }
}

bool PrimedGunApplyCannonTextureSlot(int slot, QString* error)
{
  if (!PrimedGunEnsureCannonPackRegistration())
  {
    if (error)
      *error = QObject::tr("Could not create the PrimedGun cannon texture pack folder.");
    return false;
  }

  PrimedGunClearActiveCannonTextures();
  if (slot == 0)
    return true;

  QSettings& settings = Settings::GetQSettings();
  for (int texture_index = 0; texture_index < static_cast<int>(PRIMEGUN_CANNON_TEXTURE_NAMES.size());
       ++texture_index)
  {
    const QString setting_key = PrimedGunCannonSlotSetting(slot, texture_index);
    const QString stored_source = settings.value(setting_key).toString();
    const QString source = PrimedGunResolveCannonTextureSource(slot, texture_index, stored_source);
    if (source.isEmpty())
      continue;

    if (source != stored_source)
      settings.setValue(setting_key, source);

    const QFileInfo source_info(source);
    if (!source_info.exists() || !source_info.isFile())
    {
      if (error)
        *error = QObject::tr("The selected cannon texture file no longer exists:\n%1").arg(source);
      return false;
    }

    const QString suffix = source_info.suffix().toLower();
    if (suffix != QStringLiteral("png") && suffix != QStringLiteral("dds"))
    {
      if (error)
        *error = QObject::tr("Cannon textures must be PNG or DDS files.");
      return false;
    }

    const QString destination = PrimedGunCannonActivePath(texture_index, QLatin1Char('.') + suffix);
    QFile::remove(destination);
    if (!QFile::copy(source, destination))
    {
      if (error)
        *error = QObject::tr("Could not copy cannon texture to:\n%1").arg(destination);
      return false;
    }
    PrimedGunTouchFile(destination);
  }

  return true;
}

void PrimedGunRefreshCustomTextureConfig(bool use_active_overrides)
{
  g_Config.Refresh();
  UpdateActiveConfig();
  for (const char* texture_name : PRIMEGUN_CANNON_TEXTURE_NAMES)
    HiresTexture::RemoveAssetPath(texture_name);
  HiresTexture::Update();
  for (int texture_index = 0; texture_index < static_cast<int>(PRIMEGUN_CANNON_TEXTURE_NAMES.size());
       ++texture_index)
  {
    const char* texture_name = PRIMEGUN_CANNON_TEXTURE_NAMES[texture_index];
    const QString active_dds = PrimedGunCannonActivePath(texture_index, QStringLiteral(".dds"));
    const QString active_png = PrimedGunCannonActivePath(texture_index, QStringLiteral(".png"));
    const QString active_path =
        QFileInfo(active_dds).isFile() ? active_dds :
        QFileInfo(active_png).isFile() ? active_png :
        QString();
    if (use_active_overrides && !active_path.isEmpty())
      HiresTexture::SetAssetPath(texture_name, QDir::toNativeSeparators(active_path).toStdString());
    HiresTexture::MarkDirty(texture_name);
  }
  AsyncRequests::GetInstance()->PushEvent([] {
    if (g_texture_cache)
      g_texture_cache->Invalidate();
  });
}

void PrimedGunDumpMem1(QWidget* parent)
{
  AddressSpace::Accessors* accessors = AddressSpace::GetAccessors(AddressSpace::Type::Mem1);
  if (!accessors || !accessors->begin())
  {
    ModalMessageBox::critical(parent, QObject::tr("RAM Dump"),
                              QObject::tr("Failed to dump RAM: memory is not available."));
    return;
  }

  const std::string path = File::GetUserPath(F_MEM1DUMP_IDX);
  File::CreateFullPath(path);
  File::IOFile file(path, "wb");
  if (!file)
  {
    ModalMessageBox::critical(
        parent, QObject::tr("RAM Dump"),
        QObject::tr("Failed to dump RAM: could not open %1").arg(QString::fromStdString(path)));
    return;
  }

  const size_t size = static_cast<size_t>(std::distance(accessors->begin(), accessors->end()));
  if (!file.WriteBytes(accessors->begin(), size))
  {
    ModalMessageBox::critical(
        parent, QObject::tr("RAM Dump"),
        QObject::tr("Failed to dump RAM: could not write %1").arg(QString::fromStdString(path)));
    return;
  }

  const std::string context_path = path + ".txt";
  File::IOFile context_file(context_path, "wb");
  if (context_file)
  {
    const auto& system = Core::System::GetInstance();
    const auto& ppc_state = system.GetPPCState();
    std::string context = fmt::format(
        "PrimedGun RAM dump context\n"
        "PC={:08X}\n"
        "NPC={:08X}\n"
        "LR={:08X}\n"
        "CTR={:08X}\n"
        "SRR0={:08X}\n"
        "SRR1={:08X}\n"
        "DSISR={:08X}\n"
        "DAR={:08X}\n"
        "Exceptions={:08X}\n",
        ppc_state.pc, ppc_state.npc, ppc_state.spr[SPR_LR], ppc_state.spr[SPR_CTR],
        ppc_state.spr[SPR_SRR0], ppc_state.spr[SPR_SRR1], ppc_state.spr[SPR_DSISR],
        ppc_state.spr[SPR_DAR], ppc_state.Exceptions);

    context += "\nGPRs\n";
    for (int reg = 0; reg < 32; reg += 4)
    {
      context += fmt::format("R{:02}={:08X} R{:02}={:08X} R{:02}={:08X} R{:02}={:08X}\n",
                             reg, ppc_state.gpr[reg], reg + 1, ppc_state.gpr[reg + 1],
                             reg + 2, ppc_state.gpr[reg + 2], reg + 3,
                             ppc_state.gpr[reg + 3]);
    }

    const auto read_mem1_u32 = [accessors](u32 address) -> std::optional<u32> {
      constexpr u32 mem1_base = 0x80000000u;
      if (address < mem1_base)
        return std::nullopt;

      const size_t offset = static_cast<size_t>(address - mem1_base);
      const u8* const begin = accessors->begin();
      const u8* const end = accessors->end();
      if (begin + offset + sizeof(u32) > end)
        return std::nullopt;

      const u8* const ptr = begin + offset;
      return (static_cast<u32>(ptr[0]) << 24) | (static_cast<u32>(ptr[1]) << 16) |
             (static_cast<u32>(ptr[2]) << 8) | static_cast<u32>(ptr[3]);
    };
    const auto read_mem1_u8 = [accessors](u32 address) -> std::optional<u8> {
      constexpr u32 mem1_base = 0x80000000u;
      if (address < mem1_base)
        return std::nullopt;

      const size_t offset = static_cast<size_t>(address - mem1_base);
      const u8* const begin = accessors->begin();
      const u8* const end = accessors->end();
      if (begin + offset + sizeof(u8) > end)
        return std::nullopt;

      return *(begin + offset);
    };
    const auto read_mem1_f32 = [&read_mem1_u32](u32 address) -> std::optional<float> {
      const std::optional<u32> word = read_mem1_u32(address);
      if (!word)
        return std::nullopt;

      float value = 0.0f;
      const u32 bits = *word;
      std::memcpy(&value, &bits, sizeof(value));
      return value;
    };
    const auto format_u32 = [](std::optional<u32> value) {
      return value ? fmt::format("{:08X}", *value) : std::string{"unreadable"};
    };
    const auto format_u8 = [](std::optional<u8> value) {
      return value ? fmt::format("{:02X}", *value) : std::string{"unreadable"};
    };
    const auto format_f32 = [](std::optional<float> value) {
      return value ? fmt::format("{:.6f}", *value) : std::string{"unreadable"};
    };
    const auto ppc_branch = [](u32 from, u32 to) {
      const s32 offset = static_cast<s32>(to - from);
      return 0x48000000u | (static_cast<u32>(offset) & 0x03FFFFFCu);
    };
    const auto append_words = [&](const char* label, u32 base, int count) {
      context += fmt::format("{} base={:08X}", label, base);
      for (int i = 0; i < count; ++i)
      {
        const u32 address = base + static_cast<u32>(i * 4);
        context += fmt::format(" +{:02X}={}", i * 4, format_u32(read_mem1_u32(address)));
      }
      context += "\n";
    };
    const auto append_transform = [&](const char* label, std::optional<u32> transform) {
      context += fmt::format("{}={}\n", label, format_u32(transform));
      if (!transform || *transform < 0x80000000u)
        return;

      for (u32 row = 0; row < 3; ++row)
      {
        const u32 base = *transform + row * 0x10u;
        context += fmt::format(
            "{} row{} raw={} {} {} {} f32={} {} {} {}\n", label, row,
            format_u32(read_mem1_u32(base + 0x00u)), format_u32(read_mem1_u32(base + 0x04u)),
            format_u32(read_mem1_u32(base + 0x08u)), format_u32(read_mem1_u32(base + 0x0Cu)),
            format_f32(read_mem1_f32(base + 0x00u)), format_f32(read_mem1_f32(base + 0x04u)),
            format_f32(read_mem1_f32(base + 0x08u)), format_f32(read_mem1_f32(base + 0x0Cu)));
      }
    };
    const auto append_branch_watch = [&](const char* label, u32 site, u32 original, u32 cave) {
      const u32 branch = ppc_branch(site, cave);
      const std::optional<u32> actual = read_mem1_u32(site);
      std::string status = "unreadable";
      if (actual)
      {
        if (*actual == original)
          status = "original";
        else if (*actual == branch)
          status = "branch-installed";
        else
          status = "changed";
      }
      context += fmt::format("{} site={:08X} actual={} original={:08X} branch={:08X} "
                             "cave={:08X} status={}\n",
                             label, site, format_u32(actual), original, branch, cave, status);
    };
    const auto append_code_window = [&](const char* label, u32 center, int before_words,
                                        int after_words) {
      if (center < 0x80000000u || center > 0x817FFFFFu)
      {
        context += fmt::format("{} center={:08X} outside_mem1\n", label, center);
        return;
      }

      const u32 aligned = center & ~3u;
      const u32 start = aligned - static_cast<u32>(std::max(before_words, 0) * 4);
      const u32 end = aligned + static_cast<u32>(std::max(after_words, 0) * 4);
      context += fmt::format("{} center={:08X} range={:08X}-{:08X}\n", label, center, start, end);
      for (u32 address = start; address <= end; address += 4u)
      {
        const std::optional<u32> value = read_mem1_u32(address);
        context += fmt::format("{}{:08X}={}", address == aligned ? ">" : " ", address,
                               format_u32(value));
        if (((address - start) / 4u % 4u) == 3u || address == end)
          context += "\n";
        else
          context += " ";
      }
    };
    const auto append_vtable = [&](const char* label, std::optional<u32> vtable, int entries) {
      context += fmt::format("{}={}\n", label, format_u32(vtable));
      if (!vtable || *vtable < 0x80000000u)
        return;

      for (int i = 0; i < entries; ++i)
      {
        const u32 entry_address = *vtable + static_cast<u32>(i * 4);
        const std::optional<u32> function = read_mem1_u32(entry_address);
        context += fmt::format("{}[{:02}]@{:08X}={}\n", label, i, entry_address,
                               format_u32(function));
        if (i < 6 && function && *function >= 0x80000000u)
        {
          const std::string code_label = fmt::format("{}[{:02}]code", label, i);
          append_code_window(code_label.c_str(), *function, 0, 7);
        }
      }
    };
    const auto append_object_deep = [&](const char* label, std::optional<u32> object) {
      context += fmt::format("{}={}\n", label, format_u32(object));
      if (!object || *object < 0x80000000u)
        return;

      const std::optional<u32> vtable = read_mem1_u32(*object);
      append_words(fmt::format("{}+000", label).c_str(), *object, 32);
      append_words(fmt::format("{}+080", label).c_str(), *object + 0x80u, 24);
      append_transform(fmt::format("{}Transform+34", label).c_str(), *object + 0x34u);
      append_vtable(fmt::format("{}VTable", label).c_str(), vtable, 16);
    };

    context += "\nCPU path code windows\n";
    append_code_window("PCWindow", ppc_state.pc, 8, 12);
    append_code_window("NPCWindow", ppc_state.npc, 4, 8);
    append_code_window("LRWindow", ppc_state.spr[SPR_LR], 8, 12);
    append_code_window("CTRWindow", ppc_state.spr[SPR_CTR], 4, 8);
    append_code_window("SRR0Window", ppc_state.spr[SPR_SRR0], 8, 12);

    context += "\nPatch sites\n";
    for (const u32 address :
         {0x8000A968u, 0x8000A9B4u, 0x8000E548u, 0x80041A8Cu, 0x8000E7B4u,
          0x8000E808u, 0x8000E83Cu, 0x8000E71Cu,
          0x8000FA50u, 0x800BD808u, 0x800BE25Cu, 0x80112508u, 0x801122CCu,
          0x800E0434u, 0x801B9070u, 0x800830A0u, 0x8026529Cu, 0x80345200u,
          0x8017EACCu, 0x8018C950u, 0x8018C988u, 0x800243CCu, 0x80024414u,
          0x80024450u, 0x8002448Cu, 0x800244C8u, 0x80024504u})
    {
      const std::optional<u32> value = read_mem1_u32(address);
      context += fmt::format("{:08X}={}\n", address, format_u32(value));
    }

    context += "\nHook cave heads\n";
    for (const u32 address :
         {0x80001C00u, 0x80001C80u, 0x80001D00u, 0x80001D40u, 0x80001D80u,
          0x80001DC0u, 0x80001E00u, 0x80001E80u, 0x80001F00u, 0x80001FA0u,
          0x80002000u, 0x80002180u, 0x80002200u, 0x80002280u, 0x800022C0u,
          0x80002300u,
          0x800023A0u, 0x80002400u, 0x80002620u, 0x80002660u, 0x800026D0u,
          0x80002740u, 0x80002780u})
    {
      const std::optional<u32> value = read_mem1_u32(address);
      context += fmt::format("{:08X}={}\n", address, format_u32(value));
    }

    context += "\nPPC/ASM watchdog\n";
    append_branch_watch("CameraReturnLegacyUnsafe", 0x8000A9B4u, 0x80010054u, 0x80002280u);
    append_branch_watch("FirstPersonPitchLoad", 0x8000E548u, 0xC3FE03ECu, 0x80001C00u);
    append_branch_watch("CombatPitch0", 0x8000E7B4u, 0xEC21E828u, 0x80001D40u);
    append_branch_watch("CombatPitch1", 0x8000E808u, 0xEC21E828u, 0x80001D80u);
    append_branch_watch("CombatPitch2", 0x8000E83Cu, 0xEC21E828u, 0x80001DC0u);
    append_branch_watch("FirstPersonOrbitAimVector", 0x8000E71Cu, 0x801E0304u, 0x800022C0u);
    append_branch_watch("CombatElevationPitch", 0x8000FA50u, 0xD01D01C0u, 0x80001E00u);
    append_branch_watch("ScanIndicatorViewBasis", 0x801122CCu, 0xC0410074u, 0x80002200u);
    append_branch_watch("BallCameraLevel", 0x800830A0u, 0x387F0034u, 0x80002740u);
    append_branch_watch("InterpolationCameraLevel", 0x8026529Cu, 0x887E00E4u, 0x80002780u);
    append_words("FirstPersonPitchCave", 0x80001C00u, 8);
    append_words("CombatPitchCave0", 0x80001D40u, 8);
    append_words("CombatPitchCave1", 0x80001D80u, 8);
    append_words("CombatPitchCave2", 0x80001DC0u, 8);
    append_words("CombatElevationPitchCave", 0x80001E00u, 12);
    append_words("ScanIndicatorViewBasisCave", 0x80002200u, 8);
    append_words("DisableFrustumCullingCave", 0x80002280u, 10);
    append_words("FirstPersonOrbitAimVectorCave", 0x800022C0u, 4);
    append_words("BallCameraLevelCave", 0x80002740u, 16);
    append_words("InterpolationCameraLevelCave", 0x80002780u, 20);

    constexpr u32 state_manager = 0x8045A1A8u;
    constexpr u32 player_offset = 0x84Cu;
    constexpr u32 camera_manager_offset = 0x86Cu;
    constexpr u32 object_list_offset = 0x810u;
    constexpr u32 transform_offset = 0x34u;
    const std::optional<u32> player = read_mem1_u32(state_manager + player_offset);

    context += "\nState snapshot\n";
    context += fmt::format("StateManagerPlayer={}\n", format_u32(player));
    const std::optional<u32> camera_manager =
        read_mem1_u32(state_manager + camera_manager_offset);
    const std::optional<u32> object_list = read_mem1_u32(state_manager + object_list_offset);
    context += fmt::format("StateManagerCameraManager={}\n", format_u32(camera_manager));
    context += fmt::format("StateManagerObjectList={}\n", format_u32(object_list));
    context += fmt::format("PlayerState={}\n", format_u32(read_mem1_u32(state_manager + 0x8B8u)));
    context += fmt::format("GpGameState={}\n", format_u32(read_mem1_u32(0x805A8C40u)));
    context += fmt::format("AramActiveDmasHead={}\n", format_u32(read_mem1_u32(0x805A679Cu + 4u)));
    context += fmt::format("AramActiveDmasTail={}\n", format_u32(read_mem1_u32(0x805A679Cu + 8u)));
    context += fmt::format("FinalInputDpadHeld0={}\n", format_u8(read_mem1_u8(state_manager + 0xB80u)));
    context += fmt::format("FinalInputDpadHeld1={}\n", format_u8(read_mem1_u8(state_manager + 0xB81u)));
    context += fmt::format("FinalInputDpadPressed0={}\n", format_u8(read_mem1_u8(state_manager + 0xB82u)));
    if (player && *player >= 0x80000000u)
    {
      context += fmt::format("PlayerCameraState={}\n", format_u32(read_mem1_u32(*player + 0x2F4u)));
      context += fmt::format("PlayerMorphState={}\n", format_u32(read_mem1_u32(*player + 0x2F8u)));
      context += fmt::format("PlayerMovementState={}\n", format_u32(read_mem1_u32(*player + 0x258u)));
      context += fmt::format("PlayerOrbitState={}\n", format_u32(read_mem1_u32(*player + 0x304u)));
      context += fmt::format("PlayerOrbitTarget={}\n", format_u32(read_mem1_u32(*player + 0x310u)));
      context += fmt::format("PlayerVisorState={}\n", format_u32(read_mem1_u32(*player + 0x330u)));
      context += fmt::format("PlayerFreeLookState={}\n", format_u32(read_mem1_u32(*player + 0x3DCu)));
      context += fmt::format("PlayerCannon={}\n", format_u32(read_mem1_u32(*player + 0x490u)));
      context += fmt::format("PlayerGunAlpha={}\n", format_u32(read_mem1_u32(*player + 0x494u)));
      context += fmt::format("PlayerHolsterState={}\n", format_u32(read_mem1_u32(*player + 0x498u)));
      context += fmt::format("PlayerInputFlags={}\n", format_u8(read_mem1_u8(*player + 0x9C6u)));
    }

    context += "\nCamera snapshot\n";
    std::optional<u32> camera_uid_word;
    std::optional<u32> active_camera;
    std::optional<u32> active_camera_transform;
    if (camera_manager && *camera_manager >= 0x80000000u)
      camera_uid_word = read_mem1_u32(*camera_manager);
    if (camera_uid_word && object_list && *object_list >= 0x80000000u)
    {
      const u32 camera_uid = (*camera_uid_word >> 16) & 0xffffu;
      if (camera_uid != 0xffffu)
        active_camera = read_mem1_u32(*object_list + ((camera_uid & 0x03ffu) << 3) + 4u);
    }
    if (active_camera && *active_camera >= 0x80000000u)
      active_camera_transform = *active_camera + transform_offset;

    context += fmt::format("CameraUidWord={}\n", format_u32(camera_uid_word));
    context += fmt::format("ActiveCameraObject={}\n", format_u32(active_camera));
    if (camera_uid_word && object_list && *object_list >= 0x80000000u)
    {
      const u32 camera_uid = (*camera_uid_word >> 16) & 0xffffu;
      if (camera_uid != 0xffffu)
      {
        append_words("ActiveCameraObjectListEntry",
                     *object_list + ((camera_uid & 0x03ffu) << 3), 2);
      }
    }
    append_transform("ActiveCameraTransform", active_camera_transform);
    if (camera_manager && *camera_manager >= 0x80000000u)
    {
      append_words("CameraManager+000", *camera_manager, 12);
      append_words("CameraManager+080", *camera_manager + 0x80u, 12);
      for (const u32 slot_offset : {0x10u, 0x80u, 0x84u, 0x88u, 0x8Cu, 0x90u})
      {
        const std::optional<u32> candidate = read_mem1_u32(*camera_manager + slot_offset);
        context += fmt::format("CameraManagerSlot+{:02X}={}\n", slot_offset,
                               format_u32(candidate));
        if (candidate && *candidate >= 0x80000000u)
        {
          const std::string label = fmt::format("CameraSlot+{:02X}Transform", slot_offset);
          append_transform(label.c_str(), *candidate + transform_offset);
        }
      }
    }

    context += "\nDeep pointer paths\n";
    append_words("StateManager+800", state_manager + 0x800u, 32);
    append_object_deep("PlayerDeep", player);
    if (player && *player >= 0x80000000u)
    {
      const std::optional<u32> cannon = read_mem1_u32(*player + 0x490u);
      append_object_deep("PlayerCannonDeep", cannon);
      if (cannon && *cannon >= 0x80000000u)
      {
        append_transform("PlayerCannonGunXF+3E8", *cannon + 0x3E8u);
        append_transform("PlayerCannonBeamXF+418", *cannon + 0x418u);
        append_transform("PlayerCannonWorldXF+4A8", *cannon + 0x4A8u);
        append_transform("PlayerCannonLocalXF+4D8", *cannon + 0x4D8u);
      }
    }
    append_object_deep("ActiveCameraDeep", active_camera);
    if (camera_manager && *camera_manager >= 0x80000000u)
    {
      for (const u32 slot_offset : {0x10u, 0x80u, 0x84u, 0x88u, 0x8Cu, 0x90u})
      {
        const std::optional<u32> candidate = read_mem1_u32(*camera_manager + slot_offset);
        if (candidate && *candidate >= 0x80000000u && *candidate <= 0x817FFFFFu)
        {
          const std::string label = fmt::format("CameraSlot+{:02X}Deep", slot_offset);
          append_object_deep(label.c_str(), candidate);
        }
      }
    }

    context += "\nPrimedGun scratch\n";
    for (const u32 address :
         {0x80002800u, 0x80002838u, 0x80002840u, 0x80002850u, 0x80002C00u,
          0x80002D00u, 0x80002D40u, 0x80002DA0u, 0x80002E80u, 0x80002E84u,
          0x80002E88u, 0x80002E8Cu, 0x80002E90u, 0x80002E94u})
    {
      context += fmt::format("{:08X}={}\n", address, format_u32(read_mem1_u32(address)));
    }

    context_file.WriteBytes(context.data(), context.size());
  }

  ModalMessageBox::information(
      parent, QObject::tr("RAM Dump"),
      QObject::tr("RAM dump written to:\n%1\n\nCrash context written to:\n%2\n\nPlease send both files together.")
          .arg(QString::fromStdString(path), QString::fromStdString(context_path)));
}
}  // namespace


#ifdef HAVE_XRANDR
#include "UICommon/X11Utils.h"
// This #define within X11/X.h conflicts with our WiimoteSource enum.
#undef None
// This #define within X11/X.h conflicts with QEvent::KeyPress.
#undef KeyPress
#endif

#if defined(__unix__) || defined(__unix) || defined(__APPLE__)
void MainWindow::OnSignal()
{
  close();
}

static void InstallSignalHandler()
{
  struct sigaction sa;
  sa.sa_handler = &SignalDaemon::HandleInterrupt;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART | SA_RESETHAND;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
}
#endif

static WindowSystemType GetWindowSystemType()
{
  // Determine WSI type based on Qt platform.
  QString platform_name = QGuiApplication::platformName();
  if (platform_name == QStringLiteral("windows"))
    return WindowSystemType::Windows;
  else if (platform_name == QStringLiteral("cocoa"))
    return WindowSystemType::MacOS;
  else if (platform_name == QStringLiteral("xcb"))
    return WindowSystemType::X11;
  else if (platform_name == QStringLiteral("wayland"))
    return WindowSystemType::Wayland;
  else if (platform_name == QStringLiteral("haiku"))
    return WindowSystemType::Haiku;

  ModalMessageBox::critical(
      nullptr, QStringLiteral("Error"),
      QString::asprintf("Unknown Qt platform: %s", platform_name.toStdString().c_str()));
  return WindowSystemType::Headless;
}

static WindowSystemInfo GetWindowSystemInfo(QWindow* window)
{
  WindowSystemInfo wsi;
  wsi.type = GetWindowSystemType();

  // Our Win32 Qt external doesn't have the private API.
#if defined(WIN32) || defined(__APPLE__) || defined(__HAIKU__)
  wsi.render_window = window ? reinterpret_cast<void*>(window->winId()) : nullptr;
  wsi.render_surface = wsi.render_window;
#else
  QPlatformNativeInterface* pni = QGuiApplication::platformNativeInterface();
  wsi.display_connection = pni->nativeResourceForWindow("display", window);
  if (wsi.type == WindowSystemType::Wayland)
    wsi.render_window = window ? pni->nativeResourceForWindow("surface", window) : nullptr;
  else
    wsi.render_window = window ? reinterpret_cast<void*>(window->winId()) : nullptr;
  wsi.render_surface = wsi.render_window;
#endif
  wsi.render_surface_scale = window ? static_cast<float>(window->devicePixelRatio()) : 1.0f;

  return wsi;
}

static std::vector<std::string> StringListToStdVector(QStringList list)
{
  std::vector<std::string> result;
  result.reserve(list.size());

  for (const QString& s : list)
    result.push_back(s.toStdString());

  return result;
}

static constexpr const char* SELECTED_METROID_GAME_SETTING = "mainwindow/selected_metroid_prime_path";

class PrimedGunScaledImageLabel final : public QLabel
{
public:
  explicit PrimedGunScaledImageLabel(QWidget* parent = nullptr) : QLabel(parent)
  {
    setAlignment(Qt::AlignCenter);
    setMinimumSize(240, 160);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  }

  void SetSourcePixmap(const QPixmap& pixmap)
  {
    m_source = pixmap;
    UpdateScaledPixmap();
  }

protected:
  void resizeEvent(QResizeEvent* event) override
  {
    QLabel::resizeEvent(event);
    UpdateScaledPixmap();
  }

private:
  void UpdateScaledPixmap()
  {
    if (m_source.isNull() || width() <= 0 || height() <= 0)
      return;

    setPixmap(m_source.scaled(size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
  }

  QPixmap m_source;
};

MainWindow::MainWindow(Core::System& system, std::unique_ptr<BootParameters> boot_parameters,
                       const std::string& movie_path)
    : QMainWindow(nullptr), m_system(system)
{
  setWindowTitle(
      QStringLiteral("PrimedGun v%1").arg(QString::fromStdString(Common::GetScmDescStr())));
  const QIcon primegun_icon(QApplication::applicationDirPath() +
                            QStringLiteral("/assets/PrimedGun.png"));
  setWindowIcon(primegun_icon.isNull() ? Resources::GetAppIcon() : primegun_icon);
  setUnifiedTitleAndToolBarOnMac(true);
  setAcceptDrops(true);
  setAttribute(Qt::WA_NativeWindow);

  CreateComponents();

  ConnectGameList();
  ConnectHost();
  ConnectToolBar();
  ConnectRenderWidget();
  ConnectStack();
  ConnectMenuBar();

  QSettings& settings = Settings::GetQSettings();
  settings.remove(QStringLiteral("mainwindow/state"));
  settings.remove(QStringLiteral("mainwindow/geometry"));
  setWindowState(windowState() & ~Qt::WindowMaximized);
  setMinimumSize(700, 720);
  resize(700, 720);
  if (m_menu_bar)
    m_menu_bar->hide();
  if (m_tool_bar)
    m_tool_bar->hide();
  if (!Settings::Instance().IsBatchModeEnabled())
  {
    show();
  }

  InitControllers();
  ConnectHotkeys();

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
  connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this,
          [this](Qt::ColorScheme colorScheme) {
            Settings::Instance().ApplyStyle();
            if (m_skylander_window)
              m_skylander_window->RefreshList();
          });
#endif

#ifdef USE_RETRO_ACHIEVEMENTS
  connect(m_game_list, &GameList::OpenAchievementSettings, this,
          &MainWindow::ShowAchievementSettings);
#endif  // USE_RETRO_ACHIEVEMENTS

  InitCoreCallbacks();

#ifdef USE_RETRO_ACHIEVEMENTS
  AchievementManager::GetInstance().Init(reinterpret_cast<void*>(winId()));
  if (AchievementManager::GetInstance().IsHardcoreModeActive())
    Settings::Instance().SetDebugModeEnabled(false);
  // This needs to trigger on both RA_HARDCORE_ENABLED and RA_ENABLED
  m_config_changed_callback_id = Config::AddConfigChangedCallback(
      [this] { QueueOnObject(this, [this] { this->OnHardcoreChanged(); }); });
  // If hardcore is enabled when the emulator starts, make sure it turns off what it needs to
  if (Config::Get(Config::RA_HARDCORE_ENABLED))
    OnHardcoreChanged();
#endif  // USE_RETRO_ACHIEVEMENTS

#if defined(__unix__) || defined(__unix) || defined(__APPLE__)
  auto* daemon = new SignalDaemon(this);

  connect(daemon, &SignalDaemon::InterruptReceived, this, &MainWindow::OnSignal);

  InstallSignalHandler();
#endif

  if (boot_parameters)
  {
    m_pending_boot = std::move(boot_parameters);

    if (!movie_path.empty())
    {
      std::optional<std::string> savestate_path;
      if (m_system.GetMovie().PlayInput(movie_path, &savestate_path))
      {
        m_pending_boot->boot_session_data.SetSavestateData(std::move(savestate_path),
                                                           DeleteSavestateAfterBoot::No);
        emit RecordingStatusChanged(true);
      }
    }
  }

  m_state_slot =
      std::clamp(Settings::Instance().GetStateSlot(), 1, static_cast<int>(State::NUM_STATES));

  m_render_widget_geometry = settings.value(QStringLiteral("renderwidget/geometry")).toByteArray();

  // Restoring of window states can sometimes go wrong, resulting in widgets being visible when they
  // shouldn't be so we have to reapply all our rules afterwards.
  Settings::Instance().RefreshWidgetVisibility();

  if (!ResourcePack::Init())
  {
    ModalMessageBox::critical(this, tr("Error"),
                              tr("Error occurred while loading some texture packs"));
  }

  for (auto& pack : ResourcePack::GetPacks())
  {
    if (!pack.IsValid())
    {
      ModalMessageBox::critical(this, tr("Error"),
                                tr("Invalid Pack %1 provided: %2")
                                    .arg(QString::fromStdString(pack.GetPath()))
                                    .arg(QString::fromStdString(pack.GetError())));
      return;
    }
  }

  Host::GetInstance()->SetMainWindowHandle(reinterpret_cast<void*>(winId()));

  if (m_pending_boot != nullptr)
  {
    StartGame(std::move(m_pending_boot));
    m_pending_boot.reset();
  }
}

MainWindow::~MainWindow()
{
#ifdef USE_RETRO_ACHIEVEMENTS
  Config::RemoveConfigChangedCallback(m_config_changed_callback_id);
  AchievementManager::GetInstance().Shutdown();
#endif  // USE_RETRO_ACHIEVEMENTS

  delete m_render_widget;
  for (int i = 0; i < 4; i++)
  {
    delete m_gc_tas_input_windows[i];
    delete m_gba_tas_input_windows[i];
    delete m_wii_tas_input_windows[i];
  }

  ShutdownControllers();

  QSettings& settings = Settings::GetQSettings();

  settings.setValue(QStringLiteral("renderwidget/geometry"), m_render_widget_geometry);

  Config::Save();
}

WindowSystemInfo MainWindow::GetWindowSystemInfo() const
{
  return ::GetWindowSystemInfo(m_render_widget->windowHandle());
}

void MainWindow::InitControllers()
{
  if (g_controller_interface.IsInit())
    return;

  UICommon::InitControllers(::GetWindowSystemInfo(windowHandle()));

  m_hotkey_scheduler = new HotkeyScheduler();
  m_hotkey_scheduler->Start();

  // Defaults won't work reliably without loading and saving the config first

  Wiimote::LoadConfig();
  Wiimote::GetConfig()->SaveConfig();

  Pad::LoadConfig();
  Pad::GetConfig()->SaveConfig();

  Pad::LoadGBAConfig();
  Pad::GetGBAConfig()->SaveConfig();

  Keyboard::LoadConfig();
  Keyboard::GetConfig()->SaveConfig();

  FreeLook::LoadInputConfig();
  FreeLook::GetInputConfig()->SaveConfig();
}

void MainWindow::ShutdownControllers()
{
  m_hotkey_scheduler->Stop();

  Settings::Instance().UnregisterDevicesChangedCallback();

  UICommon::ShutdownControllers();

  m_hotkey_scheduler->deleteLater();
}

void MainWindow::InitCoreCallbacks()
{
  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this, [this](Core::State state) {
    if (state == Core::State::Uninitialized)
      OnStopComplete();

    if (state == Core::State::Running && m_fullscreen_requested)
    {
      FullScreen();
      m_fullscreen_requested = false;
    }
  });
  installEventFilter(this);
  m_render_widget->installEventFilter(this);

  // Handle file open events
  auto* filter = new FileOpenEventFilter(QGuiApplication::instance());
  connect(filter, &FileOpenEventFilter::fileOpened, this, [this](const QString& file_name) {
    StartGame(BootParameters::GenerateFromFile(file_name.toStdString()));
  });
}

static void InstallHotkeyFilter(QWidget* dialog)
{
  auto* filter = new WindowActivationEventFilter(dialog);
  dialog->installEventFilter(filter);

  filter->connect(filter, &WindowActivationEventFilter::windowDeactivated,
                  [] { HotkeyManagerEmu::Enable(true); });
  filter->connect(filter, &WindowActivationEventFilter::windowActivated,
                  [] { HotkeyManagerEmu::Enable(false); });
}

void MainWindow::CreateComponents()
{
  m_menu_bar = new MenuBar(this);
  m_tool_bar = new ToolBar(this);
  m_search_bar = new SearchBar(this);
  m_game_list = new GameList(this);
  m_render_widget = new RenderWidget;
  m_stack = new QStackedWidget(this);

  for (int i = 0; i < 4; i++)
  {
    m_gc_tas_input_windows[i] = new GCTASInputWindow(nullptr, i);
    m_gba_tas_input_windows[i] = new GBATASInputWindow(nullptr, i);
    m_wii_tas_input_windows[i] = new WiiTASInputWindow(nullptr, i);
  }

  m_jit_widget = new JITWidget(m_system, this);
  m_log_widget = new LogWidget(this);
  m_log_config_widget = new LogConfigWidget(this);
  m_memory_widget = new MemoryWidget(m_system, this);
  m_network_widget = new NetworkWidget(this);
  m_register_widget = new RegisterWidget(this);
  m_thread_widget = new ThreadWidget(this);
  m_watch_widget = new WatchWidget(this);
  m_breakpoint_widget = new BreakpointWidget(this);
  m_code_widget = new CodeWidget(this);
  m_assembler_widget = new AssemblerWidget(this);

  const auto request_watch = [this](QString name, u32 addr) {
    m_watch_widget->AddWatch(name, addr);
  };
  const auto request_breakpoint = [this](u32 addr) { m_breakpoint_widget->AddBP(addr); };
  const auto request_memory_breakpoint = [this](u32 addr) {
    m_breakpoint_widget->AddAddressMBP(addr);
  };
  const auto request_view_in_memory = [this](u32 addr) { m_memory_widget->SetAddress(addr); };
  const auto request_view_in_code = [this](u32 addr) {
    m_code_widget->SetAddress(addr, CodeViewWidget::SetAddressUpdate::WithDetailedUpdate);
  };

  connect(m_jit_widget, &JITWidget::SetCodeAddress, m_code_widget, &CodeWidget::OnSetCodeAddress);
  connect(m_watch_widget, &WatchWidget::RequestMemoryBreakpoint, request_memory_breakpoint);
  connect(m_watch_widget, &WatchWidget::ShowMemory, m_memory_widget, &MemoryWidget::SetAddress);
  connect(m_register_widget, &RegisterWidget::RequestMemoryBreakpoint, request_memory_breakpoint);
  connect(m_register_widget, &RegisterWidget::RequestWatch, request_watch);
  connect(m_register_widget, &RegisterWidget::RequestViewInMemory, request_view_in_memory);
  connect(m_register_widget, &RegisterWidget::RequestViewInCode, request_view_in_code);
  connect(m_thread_widget, &ThreadWidget::RequestBreakpoint, request_breakpoint);
  connect(m_thread_widget, &ThreadWidget::RequestMemoryBreakpoint, request_memory_breakpoint);
  connect(m_thread_widget, &ThreadWidget::RequestWatch, request_watch);
  connect(m_thread_widget, &ThreadWidget::RequestViewInMemory, request_view_in_memory);
  connect(m_thread_widget, &ThreadWidget::RequestViewInCode, request_view_in_code);

  connect(m_code_widget, &CodeWidget::RequestPPCComparison, m_jit_widget,
          &JITWidget::OnRequestPPCComparison);
  connect(m_code_widget, &CodeWidget::ShowMemory, m_memory_widget, &MemoryWidget::SetAddress);
  connect(m_memory_widget, &MemoryWidget::ShowCode, m_code_widget, [this](u32 address) {
    m_code_widget->SetAddress(address, CodeViewWidget::SetAddressUpdate::WithDetailedUpdate);
  });
  connect(m_memory_widget, &MemoryWidget::RequestWatch, request_watch);

  connect(m_breakpoint_widget, &BreakpointWidget::ShowCode, [this](u32 address) {
    if (Core::GetState(m_system) == Core::State::Paused)
      m_code_widget->SetAddress(address, CodeViewWidget::SetAddressUpdate::WithDetailedUpdate);
  });
  connect(m_breakpoint_widget, &BreakpointWidget::ShowMemory, m_memory_widget,
          &MemoryWidget::SetAddress);
}

void MainWindow::ConnectMenuBar()
{
  setMenuBar(m_menu_bar);
  // File
  connect(m_menu_bar, &MenuBar::Open, this, &MainWindow::Open);
  connect(m_menu_bar, &MenuBar::Exit, this, &MainWindow::close);
  connect(m_menu_bar, &MenuBar::EjectDisc, this, &MainWindow::EjectDisc);
  connect(m_menu_bar, &MenuBar::ChangeDisc, this, &MainWindow::ChangeDisc);
  connect(m_menu_bar, &MenuBar::OpenUserFolder, this, &MainWindow::OpenUserFolder);
  connect(m_menu_bar, &MenuBar::OpenConfigFolder, this, &MainWindow::OpenConfigFolder);
  connect(m_menu_bar, &MenuBar::OpenCacheFolder, this, &MainWindow::OpenCacheFolder);

  // Emulation
  connect(m_menu_bar, &MenuBar::Pause, this, &MainWindow::Pause);
  connect(m_menu_bar, &MenuBar::Play, this, [this] { Play(); });
  connect(m_menu_bar, &MenuBar::Stop, this, &MainWindow::RequestStop);
  connect(m_menu_bar, &MenuBar::Reset, this, &MainWindow::Reset);
  connect(m_menu_bar, &MenuBar::Fullscreen, this, &MainWindow::FullScreen);
  connect(m_menu_bar, &MenuBar::FrameAdvance, this, &MainWindow::FrameAdvance);
  connect(m_menu_bar, &MenuBar::Screenshot, this, &MainWindow::ScreenShot);
  connect(m_menu_bar, &MenuBar::StateLoad, this, &MainWindow::StateLoad);
  connect(m_menu_bar, &MenuBar::StateSave, this, &MainWindow::StateSave);
  connect(m_menu_bar, &MenuBar::StateLoadSlot, this, &MainWindow::StateLoadSlot);
  connect(m_menu_bar, &MenuBar::StateSaveSlot, this, &MainWindow::StateSaveSlot);
  connect(m_menu_bar, &MenuBar::StateLoadSlotAt, this, &MainWindow::StateLoadSlotAt);
  connect(m_menu_bar, &MenuBar::StateSaveSlotAt, this, &MainWindow::StateSaveSlotAt);
  connect(m_menu_bar, &MenuBar::StateLoadUndo, this, &MainWindow::StateLoadUndo);
  connect(m_menu_bar, &MenuBar::StateSaveUndo, this, &MainWindow::StateSaveUndo);
  connect(m_menu_bar, &MenuBar::StateSaveOldest, this, &MainWindow::StateSaveOldest);
  connect(m_menu_bar, &MenuBar::SetStateSlot, this, &MainWindow::SetStateSlot);

  // Options
  connect(m_menu_bar, &MenuBar::Configure, this, &MainWindow::ShowSettingsWindow);
  connect(m_menu_bar, &MenuBar::ConfigureGraphics, this, &MainWindow::ShowGraphicsWindow);
  connect(m_menu_bar, &MenuBar::ConfigureAudio, this, &MainWindow::ShowAudioWindow);
  connect(m_menu_bar, &MenuBar::ConfigureControllers, this, &MainWindow::ShowControllersWindow);
  connect(m_menu_bar, &MenuBar::ConfigureHotkeys, this, &MainWindow::ShowHotkeyDialog);
  connect(m_menu_bar, &MenuBar::ConfigureFreelook, this, &MainWindow::ShowFreeLookWindow);

  // Tools
  connect(m_menu_bar, &MenuBar::ShowMemcardManager, this, &MainWindow::ShowMemcardManager);
  connect(m_menu_bar, &MenuBar::ShowResourcePackManager, this,
          &MainWindow::ShowResourcePackManager);
  connect(m_menu_bar, &MenuBar::ShowCheatsManager, this, &MainWindow::ShowCheatsManager);
  connect(m_menu_bar, &MenuBar::BootGameCubeIPL, this, &MainWindow::OnBootGameCubeIPL);
  connect(m_menu_bar, &MenuBar::ImportNANDBackup, this, &MainWindow::OnImportNANDBackup);
  connect(m_menu_bar, &MenuBar::PerformOnlineUpdate, this, &MainWindow::PerformOnlineUpdate);
  connect(m_menu_bar, &MenuBar::BootWiiSystemMenu, this, &MainWindow::BootWiiSystemMenu);
  connect(m_menu_bar, &MenuBar::ShowFIFOPlayer, this, &MainWindow::ShowFIFOPlayer);
  connect(m_menu_bar, &MenuBar::ShowSkylanderPortal, this, &MainWindow::ShowSkylanderPortal);
  connect(m_menu_bar, &MenuBar::ShowInfinityBase, this, &MainWindow::ShowInfinityBase);
  connect(m_menu_bar, &MenuBar::ShowWiiSpeakWindow, this, &MainWindow::ShowWiiSpeakWindow);
  connect(m_menu_bar, &MenuBar::ShowLogitechMicWindow, this, &MainWindow::ShowLogitechMicWindow);
  connect(m_menu_bar, &MenuBar::ConnectWiiRemote, this, &MainWindow::OnConnectWiiRemote);

#ifdef USE_RETRO_ACHIEVEMENTS
  connect(m_menu_bar, &MenuBar::ShowAchievementsWindow, this, &MainWindow::ShowAchievementsWindow);
#endif  // USE_RETRO_ACHIEVEMENTS

  // Movie
  connect(m_menu_bar, &MenuBar::PlayRecording, this, &MainWindow::OnPlayRecording);
  connect(m_menu_bar, &MenuBar::StartRecording, this, &MainWindow::OnStartRecording);
  connect(m_menu_bar, &MenuBar::StopRecording, this, &MainWindow::OnStopRecording);
  connect(m_menu_bar, &MenuBar::ExportRecording, this, &MainWindow::OnExportRecording);
  connect(m_menu_bar, &MenuBar::ShowTASInput, this, &MainWindow::ShowTASInput);
  connect(m_menu_bar, &MenuBar::ConfigureOSD, this, &MainWindow::ShowOSDWindow);

  // View
  connect(m_menu_bar, &MenuBar::ShowList, m_game_list, &GameList::SetListView);
  connect(m_menu_bar, &MenuBar::ShowGrid, m_game_list, &GameList::SetGridView);
  connect(m_menu_bar, &MenuBar::PurgeGameListCache, m_game_list, &GameList::PurgeCache);
  connect(m_menu_bar, &MenuBar::ShowSearch, m_search_bar, &SearchBar::Show);

  connect(m_menu_bar, &MenuBar::ColumnVisibilityToggled, m_game_list,
          &GameList::OnColumnVisibilityToggled);

  connect(m_menu_bar, &MenuBar::GameListPlatformVisibilityToggled, m_game_list,
          &GameList::OnGameListVisibilityChanged);
  connect(m_menu_bar, &MenuBar::GameListRegionVisibilityToggled, m_game_list,
          &GameList::OnGameListVisibilityChanged);

  connect(m_menu_bar, &MenuBar::ShowAboutDialog, this, &MainWindow::ShowAboutDialog);

  connect(m_game_list, &GameList::SelectionChanged, m_menu_bar, &MenuBar::SelectionChanged);
  connect(this, &MainWindow::ReadOnlyModeChanged, m_menu_bar, &MenuBar::ReadOnlyModeChanged);
  connect(this, &MainWindow::RecordingStatusChanged, m_menu_bar, &MenuBar::RecordingStatusChanged);
}

void MainWindow::ConnectHotkeys()
{
  connect(m_hotkey_scheduler, &HotkeyScheduler::Open, this, &MainWindow::Open);
  connect(m_hotkey_scheduler, &HotkeyScheduler::ChangeDisc, this, &MainWindow::ChangeDisc);
  connect(m_hotkey_scheduler, &HotkeyScheduler::EjectDisc, this, &MainWindow::EjectDisc);
  connect(m_hotkey_scheduler, &HotkeyScheduler::ExitHotkey, this, &MainWindow::close);
  connect(m_hotkey_scheduler, &HotkeyScheduler::UnlockCursor, this, &MainWindow::UnlockCursor);
  connect(m_hotkey_scheduler, &HotkeyScheduler::TogglePauseHotkey, this, &MainWindow::TogglePause);
  connect(m_hotkey_scheduler, &HotkeyScheduler::ActivateChat, this, &MainWindow::OnActivateChat);
  connect(m_hotkey_scheduler, &HotkeyScheduler::RequestGolfControl, this,
          &MainWindow::OnRequestGolfControl);
  connect(m_hotkey_scheduler, &HotkeyScheduler::RefreshGameListHotkey, this,
          &MainWindow::RefreshGameList);
  connect(m_hotkey_scheduler, &HotkeyScheduler::StopHotkey, this, &MainWindow::RequestStop);
  connect(m_hotkey_scheduler, &HotkeyScheduler::ResetHotkey, this, &MainWindow::Reset);
  connect(m_hotkey_scheduler, &HotkeyScheduler::ScreenShotHotkey, this, &MainWindow::ScreenShot);
  connect(m_hotkey_scheduler, &HotkeyScheduler::FullScreenHotkey, this, &MainWindow::FullScreen);

  connect(m_hotkey_scheduler, &HotkeyScheduler::StateLoadSlot, this, &MainWindow::StateLoadSlotAt);
  connect(m_hotkey_scheduler, &HotkeyScheduler::StateSaveSlot, this, &MainWindow::StateSaveSlotAt);
  connect(m_hotkey_scheduler, &HotkeyScheduler::StateLoadLastSaved, this,
          &MainWindow::StateLoadLastSavedAt);
  connect(m_hotkey_scheduler, &HotkeyScheduler::StateLoadUndo, this, &MainWindow::StateLoadUndo);
  connect(m_hotkey_scheduler, &HotkeyScheduler::StateSaveUndo, this, &MainWindow::StateSaveUndo);
  connect(m_hotkey_scheduler, &HotkeyScheduler::StateSaveOldest, this,
          &MainWindow::StateSaveOldest);
  connect(m_hotkey_scheduler, &HotkeyScheduler::StateSaveFile, this, &MainWindow::StateSave);
  connect(m_hotkey_scheduler, &HotkeyScheduler::StateLoadFile, this, &MainWindow::StateLoad);

  connect(m_hotkey_scheduler, &HotkeyScheduler::StateLoadSlotHotkey, this,
          &MainWindow::StateLoadSlot);
  connect(m_hotkey_scheduler, &HotkeyScheduler::StateSaveSlotHotkey, this,
          &MainWindow::StateSaveSlot);
  connect(m_hotkey_scheduler, &HotkeyScheduler::SetStateSlotHotkey, this,
          &MainWindow::SetStateSlot);
  connect(m_hotkey_scheduler, &HotkeyScheduler::IncrementSelectedStateSlotHotkey, this,
          &MainWindow::IncrementSelectedStateSlot);
  connect(m_hotkey_scheduler, &HotkeyScheduler::DecrementSelectedStateSlotHotkey, this,
          &MainWindow::DecrementSelectedStateSlot);
  connect(m_hotkey_scheduler, &HotkeyScheduler::StartRecording, this,
          &MainWindow::OnStartRecording);
  connect(m_hotkey_scheduler, &HotkeyScheduler::PlayRecording, this, &MainWindow::OnPlayRecording);
  connect(m_hotkey_scheduler, &HotkeyScheduler::ExportRecording, this,
          &MainWindow::OnExportRecording);
  connect(m_hotkey_scheduler, &HotkeyScheduler::ConnectWiiRemote, this,
          &MainWindow::OnConnectWiiRemote);
  connect(m_hotkey_scheduler, &HotkeyScheduler::ToggleReadOnlyMode, [this] {
    auto& movie = m_system.GetMovie();
    bool read_only = !movie.IsReadOnly();
    movie.SetReadOnly(read_only);
    emit ReadOnlyModeChanged(read_only);
  });
#ifdef USE_RETRO_ACHIEVEMENTS
  connect(m_hotkey_scheduler, &HotkeyScheduler::OpenAchievements, this,
          &MainWindow::ShowAchievementsWindow, Qt::QueuedConnection);
#endif  // USE_RETRO_ACHIEVEMENTS

  connect(m_hotkey_scheduler, &HotkeyScheduler::Step, m_code_widget, &CodeWidget::Step);
  connect(m_hotkey_scheduler, &HotkeyScheduler::StepOver, m_code_widget, &CodeWidget::StepOver);
  connect(m_hotkey_scheduler, &HotkeyScheduler::StepOut, m_code_widget, &CodeWidget::StepOut);
  connect(m_hotkey_scheduler, &HotkeyScheduler::Skip, m_code_widget, &CodeWidget::Skip);

  connect(m_hotkey_scheduler, &HotkeyScheduler::ShowPC, m_code_widget, &CodeWidget::ShowPC);
  connect(m_hotkey_scheduler, &HotkeyScheduler::SetPC, m_code_widget, &CodeWidget::SetPC);

  connect(m_hotkey_scheduler, &HotkeyScheduler::ToggleBreakpoint, m_code_widget,
          &CodeWidget::ToggleBreakpoint);
  connect(m_hotkey_scheduler, &HotkeyScheduler::AddBreakpoint, m_code_widget,
          &CodeWidget::AddBreakpoint);

  connect(m_hotkey_scheduler, &HotkeyScheduler::SkylandersPortalHotkey, this,
          &MainWindow::ShowSkylanderPortal);
  connect(m_hotkey_scheduler, &HotkeyScheduler::InfinityBaseHotkey, this,
          &MainWindow::ShowInfinityBase);
}

void MainWindow::ConnectToolBar()
{
  addToolBar(m_tool_bar);

  connect(m_tool_bar, &ToolBar::OpenPressed, this, &MainWindow::Open);
  connect(m_tool_bar, &ToolBar::RefreshPressed, this, &MainWindow::RefreshGameList);

  connect(m_tool_bar, &ToolBar::PlayPressed, this, [this] { Play(); });
  connect(m_tool_bar, &ToolBar::PausePressed, this, &MainWindow::Pause);
  connect(m_tool_bar, &ToolBar::StopPressed, this, &MainWindow::RequestStop);
  connect(m_tool_bar, &ToolBar::FullScreenPressed, this, &MainWindow::FullScreen);
  connect(m_tool_bar, &ToolBar::ScreenShotPressed, this, &MainWindow::ScreenShot);
  connect(m_tool_bar, &ToolBar::SettingsPressed, this, &MainWindow::ShowSettingsWindow);
  connect(m_tool_bar, &ToolBar::ControllersPressed, this, &MainWindow::ShowControllersWindow);
  connect(m_tool_bar, &ToolBar::GraphicsPressed, this, &MainWindow::ShowGraphicsWindow);
  connect(m_tool_bar, &ToolBar::VRPressed, this, &MainWindow::ShowVRWindow);

  connect(m_tool_bar, &ToolBar::StepPressed, m_code_widget, &CodeWidget::Step);
  connect(m_tool_bar, &ToolBar::StepOverPressed, m_code_widget, &CodeWidget::StepOver);
  connect(m_tool_bar, &ToolBar::StepOutPressed, m_code_widget, &CodeWidget::StepOut);
  connect(m_tool_bar, &ToolBar::SkipPressed, m_code_widget, &CodeWidget::Skip);
  connect(m_tool_bar, &ToolBar::ShowPCPressed, m_code_widget, &CodeWidget::ShowPC);
  connect(m_tool_bar, &ToolBar::SetPCPressed, m_code_widget, &CodeWidget::SetPC);
}

void MainWindow::ConnectGameList()
{
  connect(m_game_list, &GameList::GameSelected, this, [this] { Play(); });
  connect(m_game_list, &GameList::OnStartWithRiivolution, this,
          &MainWindow::ShowRiivolutionBootWidget);

  connect(m_game_list, &GameList::OpenGeneralSettings, this, &MainWindow::ShowGeneralWindow);
  connect(m_game_list, &GameList::OpenGraphicsSettings, this, &MainWindow::ShowGraphicsWindow);
}

void MainWindow::ConnectRenderWidget()
{
  m_rendering_to_main = false;
  m_render_widget->hide();
  connect(m_render_widget, &RenderWidget::Closed, this, &MainWindow::ForceStop);
  connect(m_render_widget, &RenderWidget::FocusChanged, this, [this](bool focus) {
    if (m_render_widget->isFullScreen())
      SetFullScreenResolution(focus);
  });
}

void MainWindow::ConnectHost()
{
  connect(Host::GetInstance(), &Host::RequestStop, this, &MainWindow::RequestStop);
}

void MainWindow::ConnectStack()
{
  const QString selected_metroid_game_setting =
      QString::fromLatin1(SELECTED_METROID_GAME_SETTING);

  auto* widget = new QWidget;
  auto* layout = new QVBoxLayout;
  widget->setLayout(layout);

  m_game_list->hide();
  m_search_bar->hide();

  auto* game_tab = new QWidget(widget);
  auto* game_layout = new QVBoxLayout(game_tab);
  auto* selected_game = new QLabel(game_tab);
  auto* select_button = new QPushButton(tr("Select Game..."), game_tab);
  auto* play_button = new QPushButton(tr("Play"), game_tab);
  auto* pause_button = new QPushButton(tr("Pause"), game_tab);
  auto* stop_button = new QPushButton(tr("Stop"), game_tab);
  auto* options_button = new QPushButton(tr("Game Options..."), game_tab);
  auto* select_state_slot_button = new QPushButton(tr("Select State Slot"), game_tab);
  auto* transfer_old_save_button =
      new QPushButton(tr("Transfer Old Memory Card / Settings"), game_tab);
  const QString game_button_style = QStringLiteral(R"(
    QPushButton {
      background-color: #242a33;
      border: 1px solid #343c49;
      border-radius: 4px;
      color: #edf0f4;
      min-height: 24px;
      padding: 4px 12px;
    }
    QPushButton:hover {
      background-color: #2d3440;
      border-color: #c2802e;
      color: #ffffff;
    }
    QPushButton:pressed {
      background-color: #303844;
      border-color: #c2802e;
      color: #c2802e;
    }
    QPushButton:disabled {
      background-color: #1f2126;
      border-color: #1f2126;
      color: #666d76;
    }
  )");
  const QString primary_game_button_style = game_button_style + QStringLiteral(R"(
    QPushButton {
      background-color: #263342;
      border-color: #3e4c5f;
    }
  )");
  const QString selected_game_button_style = game_button_style + QStringLiteral(R"(
    QPushButton {
      color: #f0a12a;
    }
  )");
  select_button->setFlat(true);
  play_button->setFlat(true);
  pause_button->setFlat(true);
  stop_button->setFlat(true);
  options_button->setFlat(true);
  select_state_slot_button->setFlat(true);
  transfer_old_save_button->setFlat(true);
  select_button->setStyleSheet(game_button_style);
  play_button->setStyleSheet(primary_game_button_style);
  pause_button->setStyleSheet(game_button_style);
  stop_button->setStyleSheet(game_button_style);
  options_button->setStyleSheet(game_button_style);
  select_state_slot_button->setStyleSheet(game_button_style);
  transfer_old_save_button->setStyleSheet(game_button_style);
  QSettings& settings = Settings::GetQSettings();
  const auto load_primegun_runtime_settings = [&settings] {
    PrimedGun::RuntimeSettings runtime = PrimedGun::GetRuntimeSettings();
    runtime.enabled = settings.value(QStringLiteral("primegun/enabled"), runtime.enabled).toBool();
    runtime.builtin_patches_enabled =
        settings.value(QStringLiteral("primegun/builtin_patches_enabled"),
                       runtime.builtin_patches_enabled)
            .toBool();
    runtime.patch_disable_frustum_culling =
        settings.value(QStringLiteral("primegun/patch_disable_frustum_culling"),
                       runtime.patch_disable_frustum_culling)
            .toBool();
    runtime.patch_no_idle_sway =
        settings.value(QStringLiteral("primegun/patch_no_idle_sway"), runtime.patch_no_idle_sway)
            .toBool();
    runtime.patch_disable_arm_cannon_idle_fidget =
        settings.value(QStringLiteral("primegun/patch_disable_arm_cannon_idle_fidget"),
                       runtime.patch_disable_arm_cannon_idle_fidget)
            .toBool();
    runtime.patch_beam_projectile_timing =
        settings.value(QStringLiteral("primegun/patch_beam_projectile_timing"),
                       runtime.patch_beam_projectile_timing)
            .toBool();
    runtime.patch_xr_visor_dpad_timing =
        settings.value(QStringLiteral("primegun/patch_xr_visor_dpad_timing"),
                       runtime.patch_xr_visor_dpad_timing)
            .toBool();
    runtime.patch_cannon_rotation =
        settings.value(QStringLiteral("primegun/patch_cannon_rotation"),
                       runtime.patch_cannon_rotation)
            .toBool();
    runtime.patch_gun_ray_target =
        settings.value(QStringLiteral("primegun/patch_gun_ray_target"), runtime.patch_gun_ray_target)
            .toBool();
    runtime.patch_reticle =
        settings.value(QStringLiteral("primegun/patch_reticle"), runtime.patch_reticle).toBool();
    runtime.builtin_patches_enabled = true;
    runtime.patch_disable_frustum_culling = true;
    runtime.patch_no_idle_sway = true;
    runtime.patch_disable_arm_cannon_idle_fidget = true;
    runtime.patch_beam_projectile_timing = true;
    runtime.patch_xr_visor_dpad_timing = true;
    runtime.patch_cannon_rotation = true;
    runtime.patch_gun_ray_target = true;
    runtime.patch_reticle = true;
    runtime.use_right_hand =
        settings.value(QStringLiteral("primegun/use_right_hand"), runtime.use_right_hand).toBool();
    runtime.offset_x = 0.0f;
    runtime.offset_y = 0.0f;
    runtime.offset_z = 0.0f;
    runtime.model_offset_x =
        settings.value(QStringLiteral("primegun/model_offset_x"), runtime.model_offset_x).toFloat();
    runtime.model_offset_y =
        settings.value(QStringLiteral("primegun/model_offset_y"), runtime.model_offset_y).toFloat();
    runtime.model_offset_z =
        settings.value(QStringLiteral("primegun/model_offset_z"), runtime.model_offset_z).toFloat();
    runtime.rot_offset_x =
        settings.value(QStringLiteral("primegun/rot_offset_x"), runtime.rot_offset_x).toFloat();
    runtime.rot_offset_y =
        settings.value(QStringLiteral("primegun/rot_offset_y"), runtime.rot_offset_y).toFloat();
    runtime.rot_offset_z =
        settings.value(QStringLiteral("primegun/rot_offset_z"), runtime.rot_offset_z).toFloat();
    runtime.world_scale =
        settings.value(QStringLiteral("primegun/world_scale"), runtime.world_scale).toFloat();
    runtime.require_trigger = false;
    runtime.trigger_threshold = 0.5f;
    runtime.rumble_enabled =
        settings.value(QStringLiteral("primegun/rumble_enabled"), runtime.rumble_enabled).toBool();
    runtime.rumble_intensity =
        settings.value(QStringLiteral("primegun/rumble_intensity"), runtime.rumble_intensity).toFloat();
    runtime.rumble_hand_mode =
        settings.value(QStringLiteral("primegun/rumble_hand_mode"), runtime.rumble_hand_mode).toInt();
    runtime.primegun_grip_inputs_enabled =
        settings.value(QStringLiteral("primegun/primegun_grip_inputs_enabled"),
                       runtime.primegun_grip_inputs_enabled)
            .toBool();
    runtime.primegun_grip_inputs_use_trackpad =
        settings.value(QStringLiteral("primegun/primegun_grip_inputs_use_trackpad"),
                       runtime.primegun_grip_inputs_use_trackpad)
            .toBool();
    runtime.primegun_trackpad_press_threshold =
        settings.value(QStringLiteral("primegun/primegun_trackpad_press_threshold"),
                       runtime.primegun_trackpad_press_threshold)
            .toFloat();
    runtime.combat_jump_use_primary_button =
        settings.value(QStringLiteral("primegun/combat_jump_use_primary_button"),
                       runtime.combat_jump_use_primary_button)
            .toBool();
    runtime.vr_menu_hold_left_stick =
        settings.value(QStringLiteral("primegun/vr_menu_hold_left_stick"),
                       runtime.vr_menu_hold_left_stick)
            .toBool();
    runtime.vr_menu_requires_head_zone =
        settings.value(QStringLiteral("primegun/vr_menu_requires_head_zone"),
                       runtime.vr_menu_requires_head_zone)
            .toBool();
    runtime.cinematic_screen_enabled =
        settings.value(QStringLiteral("primegun/cinematic_screen_enabled"),
                       runtime.cinematic_screen_enabled)
            .toBool();
    runtime.metroid_hud_distance =
        settings.value(QStringLiteral("primegun/metroid_hud_distance"),
                       runtime.metroid_hud_distance)
            .toFloat();
    runtime.metroid_hud_size =
        settings.value(QStringLiteral("primegun/metroid_hud_size"), runtime.metroid_hud_size)
            .toFloat();
    runtime.gun_targeting_enabled =
        settings.value(QStringLiteral("primegun/gun_targeting_enabled"),
                       runtime.gun_targeting_enabled)
            .toBool();
    runtime.gun_targeting_distance =
        settings.value(QStringLiteral("primegun/gun_targeting_distance"),
                       runtime.gun_targeting_distance)
            .toFloat();
    runtime.gun_targeting_radius =
        settings.value(QStringLiteral("primegun/gun_targeting_radius"),
                       runtime.gun_targeting_radius)
            .toFloat();
    runtime.visor_helmet_enabled =
        settings.value(QStringLiteral("primegun/visor_helmet_enabled"),
                       runtime.visor_helmet_enabled)
            .toBool();
    runtime.vr_overlays_enabled =
        settings.value(QStringLiteral("primegun/vr_overlays_enabled"),
                       runtime.vr_overlays_enabled)
            .toBool();
    runtime.height_prompt_enabled =
        settings.value(QStringLiteral("primegun/height_prompt_enabled"),
                       runtime.height_prompt_enabled)
            .toBool();
    runtime.position_marker_enabled =
        settings.value(QStringLiteral("primegun/position_marker_enabled"),
                       runtime.position_marker_enabled)
            .toBool();
    runtime.xr_dpad_enabled =
        settings.value(QStringLiteral("primegun/xr_dpad_enabled"), runtime.xr_dpad_enabled).toBool();
    runtime.xr_dpad_head_radius =
        settings.value(QStringLiteral("primegun/xr_dpad_head_radius"),
                       runtime.xr_dpad_head_radius)
            .toFloat();
    runtime.xr_dpad_head_y_below =
        settings.value(QStringLiteral("primegun/xr_dpad_head_y_below"),
                       runtime.xr_dpad_head_y_below)
            .toFloat();
    runtime.xr_dpad_deadzone =
        settings.value(QStringLiteral("primegun/xr_dpad_deadzone"), runtime.xr_dpad_deadzone)
            .toFloat();
    runtime.directional_movement_enabled =
        settings.value(QStringLiteral("primegun/directional_movement_enabled"),
                       runtime.directional_movement_enabled)
            .toBool();
    runtime.directional_movement_use_right_stick =
        settings.value(QStringLiteral("primegun/directional_movement_use_right_stick"),
                       runtime.directional_movement_use_right_stick)
            .toBool();
    runtime.directional_movement_use_hmd_direction =
        settings.value(QStringLiteral("primegun/directional_movement_use_hmd_direction"),
                       runtime.directional_movement_use_hmd_direction)
            .toBool();
    runtime.directional_movement_deadzone =
        settings.value(QStringLiteral("primegun/directional_movement_deadzone"),
                       runtime.directional_movement_deadzone)
            .toFloat();
    runtime.directional_movement_speed =
        settings.value(QStringLiteral("primegun/directional_movement_speed"),
                       runtime.directional_movement_speed)
            .toFloat();
    runtime.directional_movement_accel =
        settings.value(QStringLiteral("primegun/directional_movement_accel"),
                       runtime.directional_movement_accel)
            .toFloat();
    runtime.directional_movement_air_accel =
        settings.value(QStringLiteral("primegun/directional_movement_air_accel"),
                       runtime.directional_movement_air_accel)
            .toFloat();
    runtime.look_yaw_sensitivity =
        settings.value(QStringLiteral("primegun/look_yaw_sensitivity"),
                       runtime.look_yaw_sensitivity)
            .toFloat();
    runtime.snap_turn_enabled =
        settings.value(QStringLiteral("primegun/snap_turn_enabled"), runtime.snap_turn_enabled)
            .toBool();
    runtime.snap_turn_degrees =
        settings.value(QStringLiteral("primegun/snap_turn_degrees"), runtime.snap_turn_degrees)
            .toInt();
    PrimedGun::SetRuntimeSettings(runtime);
  };
  const auto save_primegun_runtime_settings = [&settings](const PrimedGun::RuntimeSettings& runtime) {
    settings.setValue(QStringLiteral("primegun/enabled"), runtime.enabled);
    settings.setValue(QStringLiteral("primegun/builtin_patches_enabled"),
                      runtime.builtin_patches_enabled);
    settings.setValue(QStringLiteral("primegun/patch_disable_frustum_culling"),
                      runtime.patch_disable_frustum_culling);
    settings.setValue(QStringLiteral("primegun/patch_no_idle_sway"), runtime.patch_no_idle_sway);
    settings.setValue(QStringLiteral("primegun/patch_disable_arm_cannon_idle_fidget"),
                      runtime.patch_disable_arm_cannon_idle_fidget);
    settings.setValue(QStringLiteral("primegun/patch_beam_projectile_timing"),
                      runtime.patch_beam_projectile_timing);
    settings.setValue(QStringLiteral("primegun/patch_xr_visor_dpad_timing"),
                      runtime.patch_xr_visor_dpad_timing);
    settings.setValue(QStringLiteral("primegun/patch_cannon_rotation"), runtime.patch_cannon_rotation);
    settings.setValue(QStringLiteral("primegun/patch_gun_ray_target"), runtime.patch_gun_ray_target);
    settings.setValue(QStringLiteral("primegun/patch_reticle"), runtime.patch_reticle);
    settings.setValue(QStringLiteral("primegun/use_right_hand"), runtime.use_right_hand);
    settings.remove(QStringLiteral("primegun/offset_x"));
    settings.remove(QStringLiteral("primegun/offset_y"));
    settings.remove(QStringLiteral("primegun/offset_z"));
    settings.setValue(QStringLiteral("primegun/model_offset_x"), runtime.model_offset_x);
    settings.setValue(QStringLiteral("primegun/model_offset_y"), runtime.model_offset_y);
    settings.setValue(QStringLiteral("primegun/model_offset_z"), runtime.model_offset_z);
    settings.setValue(QStringLiteral("primegun/rot_offset_x"), runtime.rot_offset_x);
    settings.setValue(QStringLiteral("primegun/rot_offset_y"), runtime.rot_offset_y);
    settings.setValue(QStringLiteral("primegun/rot_offset_z"), runtime.rot_offset_z);
    settings.setValue(QStringLiteral("primegun/world_scale"), runtime.world_scale);
    settings.setValue(QStringLiteral("primegun/require_trigger"), runtime.require_trigger);
    settings.setValue(QStringLiteral("primegun/trigger_threshold"), runtime.trigger_threshold);
    settings.setValue(QStringLiteral("primegun/rumble_enabled"), runtime.rumble_enabled);
    settings.setValue(QStringLiteral("primegun/rumble_intensity"), runtime.rumble_intensity);
    settings.setValue(QStringLiteral("primegun/rumble_hand_mode"), runtime.rumble_hand_mode);
    settings.setValue(QStringLiteral("primegun/primegun_grip_inputs_enabled"),
                      runtime.primegun_grip_inputs_enabled);
    settings.setValue(QStringLiteral("primegun/primegun_grip_inputs_use_trackpad"),
                      runtime.primegun_grip_inputs_use_trackpad);
    settings.setValue(QStringLiteral("primegun/primegun_trackpad_press_threshold"),
                      runtime.primegun_trackpad_press_threshold);
    settings.setValue(QStringLiteral("primegun/combat_jump_use_primary_button"),
                      runtime.combat_jump_use_primary_button);
    settings.setValue(QStringLiteral("primegun/vr_menu_hold_left_stick"),
                      runtime.vr_menu_hold_left_stick);
    settings.setValue(QStringLiteral("primegun/vr_menu_requires_head_zone"),
                      runtime.vr_menu_requires_head_zone);
    settings.setValue(QStringLiteral("primegun/cinematic_screen_enabled"),
                      runtime.cinematic_screen_enabled);
    settings.setValue(QStringLiteral("primegun/metroid_hud_distance"),
                      runtime.metroid_hud_distance);
    settings.setValue(QStringLiteral("primegun/metroid_hud_size"), runtime.metroid_hud_size);
    settings.setValue(QStringLiteral("primegun/gun_targeting_enabled"),
                      runtime.gun_targeting_enabled);
    settings.setValue(QStringLiteral("primegun/gun_targeting_distance"),
                      runtime.gun_targeting_distance);
    settings.setValue(QStringLiteral("primegun/gun_targeting_radius"), runtime.gun_targeting_radius);
    settings.setValue(QStringLiteral("primegun/visor_helmet_enabled"),
                      runtime.visor_helmet_enabled);
    settings.setValue(QStringLiteral("primegun/vr_overlays_enabled"), runtime.vr_overlays_enabled);
    settings.setValue(QStringLiteral("primegun/height_prompt_enabled"),
                      runtime.height_prompt_enabled);
    settings.setValue(QStringLiteral("primegun/position_marker_enabled"),
                      runtime.position_marker_enabled);
    settings.setValue(QStringLiteral("primegun/xr_dpad_enabled"), runtime.xr_dpad_enabled);
    settings.setValue(QStringLiteral("primegun/xr_dpad_head_radius"), runtime.xr_dpad_head_radius);
    settings.setValue(QStringLiteral("primegun/xr_dpad_head_y_below"),
                      runtime.xr_dpad_head_y_below);
    settings.setValue(QStringLiteral("primegun/xr_dpad_deadzone"), runtime.xr_dpad_deadzone);
    settings.setValue(QStringLiteral("primegun/directional_movement_enabled"),
                      runtime.directional_movement_enabled);
    settings.setValue(QStringLiteral("primegun/directional_movement_use_right_stick"),
                      runtime.directional_movement_use_right_stick);
    settings.setValue(QStringLiteral("primegun/directional_movement_use_hmd_direction"),
                      runtime.directional_movement_use_hmd_direction);
    settings.setValue(QStringLiteral("primegun/directional_movement_deadzone"),
                      runtime.directional_movement_deadzone);
    settings.setValue(QStringLiteral("primegun/directional_movement_speed"),
                      runtime.directional_movement_speed);
    settings.setValue(QStringLiteral("primegun/directional_movement_accel"),
                      runtime.directional_movement_accel);
    settings.setValue(QStringLiteral("primegun/directional_movement_air_accel"),
                      runtime.directional_movement_air_accel);
    settings.setValue(QStringLiteral("primegun/look_yaw_sensitivity"),
                      runtime.look_yaw_sensitivity);
    settings.setValue(QStringLiteral("primegun/snap_turn_enabled"), runtime.snap_turn_enabled);
    settings.setValue(QStringLiteral("primegun/snap_turn_degrees"), runtime.snap_turn_degrees);
  };
  load_primegun_runtime_settings();
  auto* primegun_vr_save_timer = new QTimer(this);
  connect(primegun_vr_save_timer, &QTimer::timeout, this, [this, save_primegun_runtime_settings] {
    if (PrimedGun::ConsumeVrSettingsSaveRequest())
    {
      save_primegun_runtime_settings(PrimedGun::GetRuntimeSettings());
      PrimedGun::MarkVrSettingsSaved();
    }

    if (PrimedGun::ConsumeVrStateLoadRequest())
      StateLoadSlot();

    if (PrimedGun::ConsumeVrStateSaveRequest())
      StateSaveSlot();
  });
  primegun_vr_save_timer->start(250);
  auto* revision_warning = new QLabel(tr("Wrong revision"), game_tab);
  revision_warning->setObjectName(QStringLiteral("PrimedGunBad"));
  revision_warning->setVisible(false);

  const auto update_selected_game = [this, selected_game, play_button, revision_warning,
                                     options_button](const QString& path) {
    selected_game->setVisible(!path.isEmpty());
    selected_game->setText(path.isEmpty() ? QString{} :
                                            tr("Selected: %1").arg(QFileInfo(path).fileName()));
    play_button->setEnabled(!path.isEmpty());
    options_button->setEnabled(!path.isEmpty());
    QString game_warning;
    if (!path.isEmpty())
    {
      const UICommon::GameFile game(path.toStdString());
      if (game.IsValid())
      {
        if (game.GetGameID() != "GM8E01")
          game_warning = tr("Wrong game");
        else if (game.GetRevision() != 0)
          game_warning = tr("Wrong revision");
      }
    }
    revision_warning->setText(game_warning);
    revision_warning->setVisible(!game_warning.isEmpty());
  };
  const auto make_selected_game = [selected_metroid_game_setting, this] {
    const QString path = Settings::GetQSettings().value(selected_metroid_game_setting).toString();
    if (path.isEmpty())
    {
      ModalMessageBox::information(this, tr("Information"), tr("No game selected."));
      return std::shared_ptr<UICommon::GameFile>{};
    }

    auto game = std::make_shared<UICommon::GameFile>(path.toStdString());
    if (!game->IsValid())
    {
      ModalMessageBox::critical(this, tr("Error"), tr("The selected game file could not be read."));
      return std::shared_ptr<UICommon::GameFile>{};
    }

    return game;
  };

  setMinimumSize(700, 720);
  setWindowTitle(
      QStringLiteral("PrimedGun v%1").arg(QString::fromStdString(Common::GetScmDescStr())));

  game_layout->setContentsMargins(14, 12, 14, 10);
  game_layout->setSpacing(8);
  game_tab->setStyleSheet(QStringLiteral(R"(
    QWidget { background: #101215; color: #edf0f4; font-family: Consolas, monospace; font-size: 12px; }
    QFrame#PrimedGunPanel, QTabWidget::pane { background: #121519; border: 1px solid #353a43; border-radius: 5px; }
    QLabel#PrimedGunTitle, QLabel#PrimedGunSection { color: #f0a12a; }
    QLabel#PrimedGunNotice { color: #f0a12a; font-size: 14px; font-weight: bold; }
    QLabel#PrimedGunMuted { color: #858b94; }
    QLabel#PrimedGunBad { color: #ff5b45; }
    QLabel#PrimedGunGood { color: #38d86f; }
    QPushButton { background: #242a33; border: 1px solid #242a33; border-radius: 4px; color: #edf0f4; padding: 5px 10px; }
    QPushButton:hover { background: #2d3440; border-color: #3b4553; }
    QPushButton:pressed { background: #303844; border-color: #c2802e; color: #f0a12a; }
    QPushButton#PrimedGunStart { background: #84241e; border-color: #84241e; min-height: 24px; }
    QPushButton#PrimedGunStatusPill { background: #611c18; border: 1px solid #b13a2e; border-radius: 5px; text-align: left; min-width: 96px; }
    QPushButton#PrimedGunStatusPill[ok="true"] { background: #154823; border-color: #2f9c54; }
    QTabBar::tab { background: #20262d; color: #edf0f4; padding: 5px 12px; border-top-left-radius: 4px; border-top-right-radius: 4px; margin-right: 2px; }
    QTabBar::tab:selected { background: #34404b; }
    QTabBar::tab:hover { background: #2b333d; }
    QCheckBox::indicator { width: 18px; height: 18px; border-radius: 4px; background: #242a33; }
    QCheckBox::indicator:checked { background: #d38a2d; }
    QRadioButton::indicator { width: 18px; height: 18px; border-radius: 9px; background: #20242b; }
    QRadioButton::indicator:checked { background: #f0a12a; }
    QSlider::groove:horizontal { height: 22px; border-radius: 4px; background: #202329; }
    QSlider::handle:horizontal { width: 12px; margin: 2px 0; border-radius: 5px; background: #d38a2d; }
    QDoubleSpinBox { background: #202329; border: 1px solid #202329; color: #edf0f4; border-radius: 4px; padding: 4px; }
    QScrollArea { border: 1px solid #353a43; border-radius: 5px; background: #121519; }
  )"));

  auto* header = new QFrame(game_tab);
  header->setObjectName(QStringLiteral("PrimedGunPanel"));
  auto* header_layout = new QGridLayout(header);
  header_layout->setContentsMargins(14, 12, 14, 12);
  auto* brand = new QLabel(tr("PrimedGun"), header);
  brand->setObjectName(QStringLiteral("PrimedGunTitle"));
  const QString primedgun_version =
      QStringLiteral("v%1").arg(QString::fromStdString(Common::GetScmDescStr()));
  auto* version = new QLabel(primedgun_version, header);
  version->setObjectName(QStringLiteral("PrimedGunMuted"));
  auto* status_tracking = new QLabel(tr("Waiting for DolphinXR OpenXR"), header);
  status_tracking->setObjectName(QStringLiteral("PrimedGunBad"));
  auto* status_hook = new QLabel(tr("Hook: OpenXR seen, waiting for bridge"), header);
  status_hook->setObjectName(QStringLiteral("PrimedGunBad"));
  auto* status_game = new QLabel(tr("Load game: not ready, try reconnect"), header);
  status_game->setObjectName(QStringLiteral("PrimedGunBad"));
  auto* start_button = new QPushButton(header);
  start_button->setObjectName(QStringLiteral("PrimedGunStart"));
  auto* reconnect_dolphin = new QPushButton(tr("Reconnect Dolphin"), header);
  auto* reconnect_hook = new QPushButton(tr("Reconnect Hook"), header);
  const auto make_status_pill = [header](const QString& text) {
    auto* pill = new QPushButton(text, header);
    pill->setObjectName(QStringLiteral("PrimedGunStatusPill"));
    pill->setEnabled(false);
    return pill;
  };
  header_layout->addWidget(brand, 0, 0);
  header_layout->addWidget(version, 0, 1);
  header_layout->addWidget(start_button, 0, 4);
  header_layout->addWidget(make_status_pill(tr("Tracking")), 2, 0);
  header_layout->addWidget(status_tracking, 2, 1, 1, 3);
  header_layout->addWidget(reconnect_dolphin, 2, 4);
  header_layout->addWidget(make_status_pill(tr("Hook")), 3, 0);
  header_layout->addWidget(status_hook, 3, 1, 1, 3);
  header_layout->addWidget(reconnect_hook, 3, 4);
  header_layout->addWidget(make_status_pill(tr("Game")), 4, 0);
  header_layout->addWidget(status_game, 4, 1, 1, 3);
  header_layout->setColumnStretch(3, 1);
  header->hide();

  auto runtime = std::make_shared<PrimedGun::RuntimeSettings>(PrimedGun::GetRuntimeSettings());
  const auto refresh_start_button = [start_button, runtime] {
    start_button->setText(runtime->enabled ? QObject::tr("Active - Stop") :
                                            QObject::tr("Inactive - Start"));
    start_button->setStyleSheet(runtime->enabled ?
        QStringLiteral("QPushButton#PrimedGunStart { background: #20783b; border-color: #20783b; }") :
        QStringLiteral(""));
  };
  refresh_start_button();
  connect(start_button, &QPushButton::clicked, this, [runtime, refresh_start_button] {
    runtime->enabled = !runtime->enabled;
    PrimedGun::SetRuntimeSettings(*runtime);
    refresh_start_button();
  });
  connect(reconnect_dolphin, &QPushButton::clicked, this, [this] { RefreshGameList(); });
  connect(reconnect_hook, &QPushButton::clicked, this, [] { PrimedGun::ResetNativeRuntime(); });

  auto* tabs = new QTabWidget(game_tab);
  tabs->setDocumentMode(true);

  const auto make_scroll_tab = [tabs](const QString& name) {
    auto* page = new QWidget(tabs);
    auto* page_layout = new QVBoxLayout(page);
    page_layout->setContentsMargins(10, 8, 10, 8);
    page_layout->setSpacing(8);
    auto* scroll = new QScrollArea(page);
    scroll->setWidgetResizable(true);
    auto* content = new QWidget(scroll);
    auto* content_layout = new QVBoxLayout(content);
    content_layout->setContentsMargins(12, 10, 12, 10);
    content_layout->setSpacing(8);
    scroll->setWidget(content);
    page_layout->addWidget(scroll);
    tabs->addTab(page, name);
    return content_layout;
  };
  const auto section_label = [](const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("PrimedGunSection"));
    return label;
  };
  const auto separator = [](QVBoxLayout* parent_layout) {
    auto* line = new QFrame;
    line->setFrameShape(QFrame::HLine);
    line->setStyleSheet(QStringLiteral("color: #444955;"));
    parent_layout->addWidget(line);
  };
  const auto apply_runtime = [runtime] { PrimedGun::SetRuntimeSettings(*runtime); };
  const QString assets_dir = QApplication::applicationDirPath() + QStringLiteral("/assets/");

  auto* select_state_slot_menu = new QMenu(select_state_slot_button);
  struct StateSlotMenuRow
  {
    QPushButton* select = nullptr;
    QPushButton* load = nullptr;
    QPushButton* save = nullptr;
  };
  std::vector<StateSlotMenuRow> state_slot_menu_rows;
  state_slot_menu_rows.reserve(State::NUM_STATES);
  const int state_slot_count = static_cast<int>(State::NUM_STATES);
  for (int slot = 1; slot <= state_slot_count; ++slot)
  {
    auto* action = new QWidgetAction(select_state_slot_menu);
    auto* row = new QWidget(select_state_slot_menu);
    auto* row_layout = new QHBoxLayout(row);
    row_layout->setContentsMargins(8, 2, 8, 2);
    row_layout->setSpacing(6);

    auto* select = new QPushButton(row);
    auto* load = new QPushButton(tr("Load"), row);
    auto* save = new QPushButton(tr("Save"), row);
    select->setFlat(true);
    load->setFlat(true);
    save->setFlat(true);
    select->setStyleSheet(game_button_style);
    load->setStyleSheet(game_button_style);
    save->setStyleSheet(game_button_style);
    select->setMinimumWidth(176);
    select->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    load->setMinimumWidth(56);
    save->setMinimumWidth(56);

    row_layout->addWidget(select, 1);
    row_layout->addWidget(load);
    row_layout->addWidget(save);
    action->setDefaultWidget(row);
    select_state_slot_menu->addAction(action);

    state_slot_menu_rows.push_back({select, load, save});

    connect(select, &QPushButton::clicked, this, [this, select_state_slot_menu, slot] {
      m_menu_bar->SetStateSlot(slot);
      select_state_slot_menu->close();
    });
    connect(load, &QPushButton::clicked, this, [this, select_state_slot_menu, slot] {
      m_menu_bar->SetStateSlot(slot);
      StateLoadSlotAt(slot);
      select_state_slot_menu->close();
    });
    connect(save, &QPushButton::clicked, this, [this, select_state_slot_menu, slot] {
      m_menu_bar->SetStateSlot(slot);
      StateSaveSlotAt(slot);
      select_state_slot_menu->close();
    });
  }

  const auto refresh_state_slot_menu = [state_slot_menu_rows, state_slot_count, game_button_style,
                                        selected_game_button_style] {
    const int selected_slot = Settings::Instance().GetStateSlot();
    for (int index = 0; index < state_slot_count; ++index)
    {
      const int slot = index + 1;
      const QString info = QString::fromStdString(State::GetInfoStringOfSlot(slot));
      const QString label =
          (selected_slot == slot ? QObject::tr("Current Slot %1 - %2") :
                                   QObject::tr("Select Slot %1 - %2"))
              .arg(slot)
              .arg(info);
      state_slot_menu_rows.at(index).select->setText(label);
      state_slot_menu_rows.at(index).select->setStyleSheet(
          selected_slot == slot ? selected_game_button_style : game_button_style);
    }
  };
  connect(select_state_slot_menu, &QMenu::aboutToShow, this, refresh_state_slot_menu);
  refresh_state_slot_menu();
  select_state_slot_button->setMenu(select_state_slot_menu);

  auto* setup_layout = make_scroll_tab(tr("Setup"));
  setup_layout->setContentsMargins(12, 10, 12, 0);
  setup_layout->addWidget(section_label(tr("Setup"), game_tab));
  auto* notes = new QLabel(tr("Use the README for setup instructions."), game_tab);
  notes->setObjectName(QStringLiteral("PrimedGunMuted"));
  setup_layout->addWidget(notes);
  separator(setup_layout);
  setup_layout->addWidget(selected_game);
  auto* select_game_row = new QHBoxLayout;
  auto* select_game_note = new QLabel(tr("Select Metroid Prime NTSC Revision 0 (1.0)."), game_tab);
  select_game_note->setObjectName(QStringLiteral("PrimedGunMuted"));
  select_game_row->addWidget(select_game_note);
  select_game_row->addStretch();
  select_game_row->addWidget(revision_warning);
  setup_layout->addLayout(select_game_row);
  setup_layout->addWidget(select_button);
  setup_layout->addWidget(play_button);
  auto* play_control_row = new QHBoxLayout;
  play_control_row->setSpacing(8);
  play_control_row->addWidget(pause_button);
  play_control_row->addWidget(stop_button);
  setup_layout->addLayout(play_control_row);
  setup_layout->addWidget(options_button);
  setup_layout->addSpacing(10);
  setup_layout->addWidget(section_label(tr("Save States / Memory Card"), game_tab));
  setup_layout->addWidget(select_state_slot_button);
  setup_layout->addWidget(transfer_old_save_button);
  setup_layout->addSpacing(12);
  setup_layout->addStretch();
  auto* setup_art = new QLabel(game_tab);
  setup_art->setAlignment(Qt::AlignLeft | Qt::AlignBottom);
  setup_art->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  const QPixmap samus_art(assets_dir + QStringLiteral("samus.png"));
  if (!samus_art.isNull())
  {
    const QPixmap scaled_samus =
        samus_art.scaled(208, 126, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    setup_art->setPixmap(scaled_samus);
    setup_art->setFixedHeight(scaled_samus.height());
  }
  setup_layout->addWidget(setup_art, 0, Qt::AlignLeft | Qt::AlignBottom);

  auto* controller_layout = make_scroll_tab(tr("Controller"));
  controller_layout->addWidget(section_label(tr("Controller Mapping"), game_tab));
  auto* reset_controller = new QPushButton(tr("Reset Controller"), game_tab);
  controller_layout->addWidget(reset_controller);
  auto* right_hand = new QRadioButton(tr("Right hand"), game_tab);
  auto* left_hand = new QRadioButton(tr("Left hand"), game_tab);
  auto* hand_group = new QButtonGroup(game_tab);
  hand_group->addButton(right_hand);
  hand_group->addButton(left_hand);
  hand_group->setExclusive(true);
  right_hand->setChecked(runtime->use_right_hand);
  left_hand->setChecked(!runtime->use_right_hand);
  auto* hand_row = new QHBoxLayout;
  hand_row->addWidget(right_hand);
  hand_row->addWidget(left_hand);
  hand_row->addStretch();
  controller_layout->addLayout(hand_row);
  auto* auto_bindings = new QCheckBox(tr("Auto set controller bindings"), game_tab);
  auto_bindings->setChecked(true);
  controller_layout->addWidget(auto_bindings);
  auto* vr_overlays_enabled = new QCheckBox(tr("In-headset overlays"), game_tab);
  vr_overlays_enabled->setChecked(runtime->vr_overlays_enabled);
  auto* vr_menu_hold_left_stick =
      new QCheckBox(tr("Longer held press for VR menu"), game_tab);
  vr_menu_hold_left_stick->setChecked(runtime->vr_menu_hold_left_stick);
  controller_layout->addWidget(vr_menu_hold_left_stick);
  auto* vr_menu_requires_head_zone =
      new QCheckBox(tr("VR menu requires controller near head to activate"), game_tab);
  vr_menu_requires_head_zone->setChecked(runtime->vr_menu_requires_head_zone);
  controller_layout->addWidget(vr_menu_requires_head_zone);
  auto* cinematic_screen_enabled = new QCheckBox(tr("Show cutscenes on cinema screen"), game_tab);
  cinematic_screen_enabled->setChecked(runtime->cinematic_screen_enabled);
  auto* combat_jump_use_primary_button = new QCheckBox(tr("Use A button for jump"), game_tab);
  combat_jump_use_primary_button->setChecked(runtime->combat_jump_use_primary_button);
  controller_layout->addWidget(combat_jump_use_primary_button);
  auto* rumble_enabled = new QCheckBox(tr("Rumble"), game_tab);
  rumble_enabled->setChecked(runtime->rumble_enabled);
  auto* rumble_hand_mode = new QComboBox(game_tab);
  rumble_hand_mode->addItem(tr("Both"), 0);
  rumble_hand_mode->addItem(tr("Left only"), 1);
  rumble_hand_mode->addItem(tr("Right only"), 2);
  rumble_hand_mode->setCurrentIndex(std::clamp(runtime->rumble_hand_mode, 0, 2));
  rumble_hand_mode->installEventFilter(this);
  auto* rumble_hand_row = new QHBoxLayout;
  auto* rumble_hand_label = new QLabel(tr("Rumble target"), game_tab);
  rumble_hand_label->setMinimumWidth(170);
  rumble_hand_label->setMaximumWidth(170);
  rumble_hand_row->addWidget(rumble_hand_label);
  rumble_hand_row->addWidget(rumble_hand_mode, 0);
  rumble_hand_row->addStretch();
  auto* dpad_enabled = new QCheckBox(tr("Enable visor gesture input"), game_tab);
  dpad_enabled->setChecked(runtime->xr_dpad_enabled);
  auto* primegun_grip_inputs_enabled =
      new QCheckBox(tr("Use grip input"), game_tab);
  primegun_grip_inputs_enabled->setChecked(runtime->primegun_grip_inputs_enabled);
  auto* primegun_grip_inputs_use_trackpad =
      new QCheckBox(tr("Use touchpad for PrimedGun grip inputs (Index users)"), game_tab);
  primegun_grip_inputs_use_trackpad->setChecked(runtime->primegun_grip_inputs_use_trackpad);

  auto float_rows = std::make_shared<std::vector<std::pair<QDoubleSpinBox*, QSlider*>>>();
  const auto add_float_row = [this, game_tab, apply_runtime, float_rows](
                                 QVBoxLayout* parent_layout, const QString& label_text,
                                 double min, double max, double step, double value,
                                 const std::function<void(float)>& setter) {
    auto* row = new QHBoxLayout;
    row->setSpacing(8);
    auto* label = new QLabel(label_text, game_tab);
    label->setMinimumWidth(170);
    label->setMaximumWidth(170);
    auto* slider = new QSlider(Qt::Horizontal, game_tab);
    auto* spin = new QDoubleSpinBox(game_tab);
    slider->installEventFilter(this);
    spin->installEventFilter(this);
    spin->setRange(min, max);
    spin->setSingleStep(step);
    spin->setDecimals(step < 0.1 ? 3 : 2);
    spin->setMinimumWidth(76);
    spin->setMaximumWidth(76);
    slider->setRange(static_cast<int>(min / step), static_cast<int>(max / step));
    slider->setValue(static_cast<int>(value / step));
    spin->setValue(value);
    row->addWidget(label, 0);
    row->addWidget(slider, 1);
    row->addWidget(spin, 0);
    auto* minus = new QPushButton(tr("-"), game_tab);
    auto* plus = new QPushButton(tr("+"), game_tab);
    minus->setFixedWidth(28);
    plus->setFixedWidth(28);
    row->addWidget(minus);
    row->addWidget(plus);
    parent_layout->addLayout(row);
    QObject::connect(slider, &QSlider::valueChanged, spin, [spin, step, setter, apply_runtime](int v) {
      const double value = v * step;
      if (spin->value() != value)
        spin->setValue(value);
      setter(static_cast<float>(value));
      apply_runtime();
    });
    QObject::connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), slider,
                     [slider, step, setter, apply_runtime](double v) {
      const int slider_value = static_cast<int>(v / step);
      if (slider->value() != slider_value)
        slider->setValue(slider_value);
      setter(static_cast<float>(v));
      apply_runtime();
    });
    QObject::connect(minus, &QPushButton::clicked, spin, [spin, step] { spin->setValue(spin->value() - step); });
    QObject::connect(plus, &QPushButton::clicked, spin, [spin, step] { spin->setValue(spin->value() + step); });
    float_rows->emplace_back(spin, slider);
    return spin;
  };

  separator(controller_layout);
  controller_layout->addWidget(section_label(tr("Rumble / Grip Inputs"), game_tab));
  controller_layout->addWidget(rumble_enabled);
  controller_layout->addLayout(rumble_hand_row);
  controller_layout->addWidget(primegun_grip_inputs_enabled);
  controller_layout->addWidget(primegun_grip_inputs_use_trackpad);
  auto* trackpad_press_threshold_spin =
      add_float_row(controller_layout, tr("Touchpad sensitivity"), 0.05, 1.00, 0.05,
                    runtime->primegun_trackpad_press_threshold,
                    [runtime](float v) { runtime->primegun_trackpad_press_threshold = v; });
  auto* rumble_intensity_spin =
      add_float_row(controller_layout, tr("Rumble intensity"), 0.00, 1.00, 0.05,
                    runtime->rumble_intensity,
                    [runtime](float v) { runtime->rumble_intensity = v; });
  separator(controller_layout);
  controller_layout->addWidget(section_label(tr("D-pad"), game_tab));
  controller_layout->addWidget(dpad_enabled);
  auto* dpad_radius_spin =
      add_float_row(controller_layout, tr("Head radius"), 0.08, 0.28, 0.01,
                    runtime->xr_dpad_head_radius,
                    [runtime](float v) { runtime->xr_dpad_head_radius = v; });
  auto* dpad_below_spin =
      add_float_row(controller_layout, tr("Below head"), 0.02, 0.25, 0.01,
                    runtime->xr_dpad_head_y_below,
                    [runtime](float v) { runtime->xr_dpad_head_y_below = v; });
  auto* dpad_deadzone_spin =
      add_float_row(controller_layout, tr("Stick deadzone"), 0.20, 0.80, 0.01,
                    runtime->xr_dpad_deadzone,
                    [runtime](float v) { runtime->xr_dpad_deadzone = v; });
  separator(controller_layout);
  controller_layout->addWidget(section_label(tr("Directional Movement"), game_tab));
  auto* movement_enabled = new QCheckBox(tr("Left stick strafe movement"), game_tab);
  movement_enabled->setChecked(runtime->directional_movement_enabled);
  controller_layout->addWidget(movement_enabled);
  auto* left_stick = new QRadioButton(tr("Left stick"), game_tab);
  auto* right_stick = new QRadioButton(tr("Right stick"), game_tab);
  auto* move_stick_group = new QButtonGroup(game_tab);
  move_stick_group->addButton(left_stick);
  move_stick_group->addButton(right_stick);
  move_stick_group->setExclusive(true);
  left_stick->setChecked(!runtime->directional_movement_use_right_stick);
  right_stick->setChecked(runtime->directional_movement_use_right_stick);
  auto* move_stick_row = new QHBoxLayout;
  move_stick_row->addWidget(left_stick);
  move_stick_row->addWidget(right_stick);
  move_stick_row->addStretch();
  controller_layout->addLayout(move_stick_row);
  auto* controller_direction = new QRadioButton(tr("Controller direction"), game_tab);
  auto* hmd_direction = new QRadioButton(tr("HMD direction"), game_tab);
  auto* movement_direction_group = new QButtonGroup(game_tab);
  movement_direction_group->addButton(controller_direction);
  movement_direction_group->addButton(hmd_direction);
  movement_direction_group->setExclusive(true);
  controller_direction->setChecked(!runtime->directional_movement_use_hmd_direction);
  hmd_direction->setChecked(runtime->directional_movement_use_hmd_direction);
  auto* movement_direction_row = new QHBoxLayout;
  movement_direction_row->addWidget(controller_direction);
  movement_direction_row->addWidget(hmd_direction);
  movement_direction_row->addStretch();
  controller_layout->addLayout(movement_direction_row);
  auto* movement_deadzone_spin =
      add_float_row(controller_layout, tr("Movement deadzone"), 0.05, 0.80, 0.01,
                    runtime->directional_movement_deadzone,
                    [runtime](float v) { runtime->directional_movement_deadzone = v; });
  auto* movement_speed_spin =
      add_float_row(controller_layout, tr("Movement speed"), 4.0, 30.0, 0.25,
                    runtime->directional_movement_speed,
                    [runtime](float v) { runtime->directional_movement_speed = v; });
  auto* movement_accel_spin =
      add_float_row(controller_layout, tr("Movement acceleration"), 5.0, 120.0, 1.0,
                    runtime->directional_movement_accel,
                    [runtime](float v) { runtime->directional_movement_accel = v; });
  auto* movement_air_accel_spin =
      add_float_row(controller_layout, tr("Air acceleration"), 0.0, 60.0, 0.5,
                    runtime->directional_movement_air_accel,
                    [runtime](float v) { runtime->directional_movement_air_accel = v; });
  auto* look_yaw_sensitivity_spin =
      add_float_row(controller_layout, tr("Look yaw sensitivity"), 0.20, 3.00, 0.05,
                    runtime->look_yaw_sensitivity,
                    [runtime](float v) { runtime->look_yaw_sensitivity = v; });
  auto* snap_turn_enabled = new QCheckBox(tr("Snap turn"), game_tab);
  snap_turn_enabled->setChecked(runtime->snap_turn_enabled);
  controller_layout->addWidget(snap_turn_enabled);
  auto* snap_turn_degrees = new QComboBox(game_tab);
  snap_turn_degrees->addItem(tr("30 degrees"), 30);
  snap_turn_degrees->addItem(tr("45 degrees"), 45);
  snap_turn_degrees->addItem(tr("60 degrees"), 60);
  snap_turn_degrees->addItem(tr("90 degrees"), 90);
  const auto set_snap_turn_degrees_combo = [snap_turn_degrees](int degrees) {
    const int index = snap_turn_degrees->findData(degrees);
    snap_turn_degrees->setCurrentIndex(index >= 0 ? index : 1);
  };
  set_snap_turn_degrees_combo(runtime->snap_turn_degrees);
  snap_turn_degrees->installEventFilter(this);
  auto* snap_turn_degrees_row = new QHBoxLayout;
  auto* snap_turn_degrees_label = new QLabel(tr("Snap turn angle"), game_tab);
  snap_turn_degrees_label->setMinimumWidth(170);
  snap_turn_degrees_label->setMaximumWidth(170);
  snap_turn_degrees_row->addWidget(snap_turn_degrees_label);
  snap_turn_degrees_row->addWidget(snap_turn_degrees, 0);
  snap_turn_degrees_row->addStretch();
  controller_layout->addLayout(snap_turn_degrees_row);
  controller_layout->addStretch();

  auto* calibration_layout = make_scroll_tab(tr("Calibration"));
  calibration_layout->addWidget(section_label(tr("In-headset Display"), game_tab));
  calibration_layout->addWidget(vr_overlays_enabled);
  auto* height_prompt_enabled = new QCheckBox(tr("Show height prompt"), game_tab);
  height_prompt_enabled->setChecked(runtime->height_prompt_enabled);
  calibration_layout->addWidget(height_prompt_enabled);
  calibration_layout->addWidget(cinematic_screen_enabled);
  auto* visor_helmet_enabled = new QCheckBox(tr("Enable visor helmet"), game_tab);
  visor_helmet_enabled->setChecked(runtime->visor_helmet_enabled);
  auto* visor_helmet_row = new QHBoxLayout;
  visor_helmet_row->addWidget(visor_helmet_enabled);
  auto* visor_helmet_note =
      new QLabel(tr("Allows user to turn helmet back on in the pause menu"), game_tab);
  visor_helmet_note->setObjectName(QStringLiteral("PrimedGunMuted"));
  visor_helmet_row->addWidget(visor_helmet_note);
  visor_helmet_row->addStretch();
  calibration_layout->addLayout(visor_helmet_row);
  auto* position_marker_enabled = new QCheckBox(tr("Show floor position marker"), game_tab);
  position_marker_enabled->setChecked(runtime->position_marker_enabled);
  calibration_layout->addWidget(position_marker_enabled);
  separator(calibration_layout);
  calibration_layout->addWidget(section_label(tr("HUD"), game_tab));
  auto* reset_hud = new QPushButton(tr("Reset HUD"), game_tab);
  calibration_layout->addWidget(reset_hud);
  auto* metroid_hud_distance_spin =
      add_float_row(calibration_layout, tr("HUD distance"), 0.10, 3.00, 0.05,
                    runtime->metroid_hud_distance,
                    [runtime](float v) { runtime->metroid_hud_distance = v; });
  auto* metroid_hud_size_spin =
      add_float_row(calibration_layout, tr("HUD size"), 0.10, 3.00, 0.05,
                    runtime->metroid_hud_size,
                    [runtime](float v) { runtime->metroid_hud_size = v; });
  separator(calibration_layout);
  calibration_layout->addWidget(section_label(tr("Targeting"), game_tab));
  auto* reset_aiming = new QPushButton(tr("Reset Targeting"), game_tab);
  calibration_layout->addWidget(reset_aiming);
  auto* target_distance_spin =
      add_float_row(calibration_layout, tr("Target distance"), 10.0, 120.0, 1.0,
                    runtime->gun_targeting_distance,
                    [runtime](float v) { runtime->gun_targeting_distance = v; });
  auto* target_radius_spin =
      add_float_row(calibration_layout, tr("Target radius"), 0.5, 8.0, 0.1,
                    runtime->gun_targeting_radius,
                    [runtime](float v) { runtime->gun_targeting_radius = v; });
  separator(calibration_layout);
  calibration_layout->addWidget(section_label(tr("Offset Tuning"), game_tab));
  auto* reset_calibration = new QPushButton(tr("Reset Calibration"), game_tab);
  calibration_layout->addWidget(reset_calibration);
  separator(calibration_layout);
  calibration_layout->addWidget(section_label(tr("Position"), game_tab));
  auto* model_x_spin =
      add_float_row(calibration_layout, tr("Left / right"), -2.0, 2.0, 0.01,
                    runtime->model_offset_x, [runtime](float v) { runtime->model_offset_x = v; });
  auto* model_y_spin =
      add_float_row(calibration_layout, tr("Forward / back"), -2.0, 2.0, 0.01,
                    runtime->model_offset_y, [runtime](float v) { runtime->model_offset_y = v; });
  auto* model_z_spin =
      add_float_row(calibration_layout, tr("Up / down"), -2.0, 2.0, 0.01,
                    runtime->model_offset_z, [runtime](float v) { runtime->model_offset_z = v; });
  separator(calibration_layout);
  calibration_layout->addWidget(section_label(tr("Rotation"), game_tab));
  auto* rot_x_spin =
      add_float_row(calibration_layout, tr("Pitch offset"), -180.0, 180.0, 0.5,
                    runtime->rot_offset_x, [runtime](float v) { runtime->rot_offset_x = v; });
  auto* rot_y_spin =
      add_float_row(calibration_layout, tr("Yaw offset"), -180.0, 180.0, 0.5,
                    runtime->rot_offset_y, [runtime](float v) { runtime->rot_offset_y = v; });
  auto* rot_z_spin =
      add_float_row(calibration_layout, tr("Roll offset"), -180.0, 180.0, 0.5,
                    runtime->rot_offset_z, [runtime](float v) { runtime->rot_offset_z = v; });
  separator(calibration_layout);
  calibration_layout->addWidget(section_label(tr("Presets"), game_tab));
  auto* preset_row = new QHBoxLayout;
  auto* default_preset = new QPushButton(game_tab);
  auto* samus_preset = new QPushButton(game_tab);
  const QString default_arm_path = assets_dir + QStringLiteral("default arm.png");
  const QString samus_arm_path = assets_dir + QStringLiteral("samus arm.png");
  default_preset->setIcon(QIcon(default_arm_path));
  default_preset->setIconSize(QSize(120, 90));
  default_preset->setToolTip(tr("Default arm preset: zero offsets"));
  samus_preset->setIcon(QIcon(samus_arm_path));
  samus_preset->setIconSize(QSize(120, 90));
  samus_preset->setToolTip(tr("Samus arm preset"));
  preset_row->addWidget(default_preset);
  preset_row->addWidget(samus_preset);
  preset_row->addStretch();
  calibration_layout->addLayout(preset_row);
  calibration_layout->addStretch();

  auto* cannon_layout = make_scroll_tab(tr("Cannon Textures"));
  cannon_layout->addWidget(section_label(tr("Custom Cannon Textures"), game_tab));
  auto* cannon_note = new QLabel(
      tr("PrimedGun stores cannon texture slots outside Metroid Prime's GM8E01 texture-pack folder. "
         "Default disables the PrimedGun override so any installed HD texture pack can supply the "
         "cannon textures."),
      game_tab);
  cannon_note->setWordWrap(true);
  cannon_note->setObjectName(QStringLiteral("PrimedGunMuted"));
  cannon_layout->addWidget(cannon_note);
  separator(cannon_layout);

  QSettings& cannon_settings = Settings::GetQSettings();
  int active_cannon_slot =
      cannon_settings.value(QStringLiteral("primegun/cannon_texture_slot"), 0).toInt();
  if (active_cannon_slot < 0 || active_cannon_slot > 5)
    active_cannon_slot = 0;

  auto* cannon_slot_group = new QButtonGroup(game_tab);
  cannon_slot_group->setExclusive(true);
  auto* cannon_slot_row = new QHBoxLayout;
  cannon_slot_row->setSpacing(8);
  for (int slot = 0; slot <= 5; ++slot)
  {
    auto* radio = new QRadioButton(PrimedGunCannonSlotName(slot), game_tab);
    radio->setChecked(slot == active_cannon_slot);
    cannon_slot_group->addButton(radio, slot);
    cannon_slot_row->addWidget(radio);
  }
  cannon_slot_row->addStretch();
  cannon_layout->addLayout(cannon_slot_row);

  auto* cannon_status = new QLabel(game_tab);
  cannon_status->setWordWrap(true);
  cannon_status->setObjectName(QStringLiteral("PrimedGunMuted"));
  cannon_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  cannon_status->setMinimumWidth(220);
  cannon_layout->addWidget(cannon_status);

  auto* cannon_texture_box = new QGroupBox(tr("Selected Slot Files"), game_tab);
  auto* cannon_texture_grid = new QGridLayout(cannon_texture_box);
  cannon_texture_grid->setColumnStretch(2, 1);
  std::array<QLabel*, PRIMEGUN_CANNON_TEXTURE_NAMES.size()> cannon_texture_path_labels{};
  std::array<QLabel*, PRIMEGUN_CANNON_TEXTURE_NAMES.size()> cannon_texture_preview_labels{};
  std::array<QPushButton*, PRIMEGUN_CANNON_TEXTURE_NAMES.size()> cannon_texture_import_buttons{};
  for (int texture_index = 0;
       texture_index < static_cast<int>(PRIMEGUN_CANNON_TEXTURE_NAMES.size()); ++texture_index)
  {
    auto* target_label = new QLabel(
        tr("%1").arg(QString::fromLatin1(PRIMEGUN_CANNON_TEXTURE_LABELS[texture_index])),
        cannon_texture_box);
    auto* path_label = new QLabel(cannon_texture_box);
    path_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    path_label->setObjectName(QStringLiteral("PrimedGunMuted"));
    path_label->setWordWrap(true);
    path_label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    path_label->setMinimumWidth(220);
    auto* preview_label = new QLabel(cannon_texture_box);
    preview_label->setFixedSize(72, 72);
    preview_label->setAlignment(Qt::AlignCenter);
    preview_label->setFrameShape(QFrame::StyledPanel);
    preview_label->setObjectName(QStringLiteral("PrimedGunMuted"));
    auto* import_button = new QPushButton(tr("Import..."), cannon_texture_box);
    cannon_texture_grid->addWidget(target_label, texture_index, 0);
    cannon_texture_grid->addWidget(preview_label, texture_index, 1);
    cannon_texture_grid->addWidget(path_label, texture_index, 2);
    cannon_texture_grid->addWidget(import_button, texture_index, 3);
    cannon_texture_path_labels[texture_index] = path_label;
    cannon_texture_preview_labels[texture_index] = preview_label;
    cannon_texture_import_buttons[texture_index] = import_button;

    connect(import_button, &QPushButton::clicked, this,
            [this, cannon_slot_group, texture_index, cannon_texture_path_labels,
             cannon_texture_preview_labels, cannon_status] {
      const int slot = cannon_slot_group->checkedId();
      if (slot != 5)
      {
        ModalMessageBox::information(this, tr("Cannon Textures"),
                                     tr("Choose Custom before importing a texture."));
        return;
      }

      QDir custom_dir(PrimedGunCannonCustomDir());
      if (!custom_dir.exists())
        custom_dir.mkpath(QStringLiteral("."));

      const QString source = DolphinFileDialog::getOpenFileName(
          this, tr("Select Cannon Texture"), PrimedGunCannonCustomDir(),
          tr("Texture Images (*.png *.dds);;All Files (*)"));
      if (source.isEmpty())
        return;

      const QFileInfo source_info(source);
      const QString suffix = source_info.suffix().toLower();
      if (suffix != QStringLiteral("png") && suffix != QStringLiteral("dds"))
      {
        ModalMessageBox::warning(this, tr("Cannon Textures"),
                                 tr("Cannon textures must be PNG or DDS files."));
        return;
      }

      QDir slot_dir(PrimedGunCannonSlotDir(slot));
      if (!slot_dir.exists() && !slot_dir.mkpath(QStringLiteral(".")))
      {
        ModalMessageBox::critical(this, tr("Cannon Textures"),
                                  tr("Could not create the selected cannon texture slot folder."));
        return;
      }

      const QString destination =
          PrimedGunCannonSourcePath(slot, texture_index, QLatin1Char('.') + suffix);
      const bool already_in_slot = QDir::cleanPath(QFileInfo(source).absoluteFilePath()) ==
                                   QDir::cleanPath(QFileInfo(destination).absoluteFilePath());
      if (!already_in_slot)
      {
        PrimedGunRemoveCannonSlotTextureFiles(slot, texture_index);
      }
      if (!already_in_slot && !QFile::copy(source, destination))
      {
        ModalMessageBox::critical(this, tr("Cannon Textures"),
                                  tr("Could not copy the selected texture into the slot."));
        return;
      }

      QSettings& settings = Settings::GetQSettings();
      settings.setValue(PrimedGunCannonSlotSetting(slot, texture_index), destination);
      PrimedGunSetCannonPathLabel(cannon_texture_path_labels[texture_index],
                                 QDir::toNativeSeparators(destination));
      PrimedGunSetCannonPreviewLabel(cannon_texture_preview_labels[texture_index], destination);
      cannon_status->setText(tr("Imported texture for %1. Click Apply to use it.")
                                 .arg(PrimedGunCannonSlotName(slot)));
    });
  }
  cannon_layout->addWidget(cannon_texture_box);

  auto refresh_cannon_texture_ui = [cannon_slot_group, cannon_texture_path_labels,
                                    cannon_texture_preview_labels, cannon_texture_import_buttons,
                                    cannon_status] {
    const int slot = cannon_slot_group->checkedId();
    QSettings& settings = Settings::GetQSettings();
    for (int texture_index = 0;
         texture_index < static_cast<int>(PRIMEGUN_CANNON_TEXTURE_NAMES.size()); ++texture_index)
    {
      cannon_texture_import_buttons[texture_index]->setVisible(slot == 5);
      QLabel* label = cannon_texture_path_labels[texture_index];
      if (slot <= 0)
      {
        PrimedGunSetCannonPathLabel(label, QObject::tr("Default: no PrimedGun override"));
        PrimedGunSetCannonPreviewLabel(cannon_texture_preview_labels[texture_index],
                                      PrimedGunCannonDefaultPreviewPath(texture_index));
        continue;
      }

      const QString setting_key = PrimedGunCannonSlotSetting(slot, texture_index);
      const QString stored_path = settings.value(setting_key).toString();
      const QString path = PrimedGunResolveCannonTextureSource(slot, texture_index, stored_path);
      if (path != stored_path)
        settings.setValue(setting_key, path);
      PrimedGunSetCannonPathLabel(label, path.isEmpty() ? QObject::tr("No texture imported") :
                                                   QDir::toNativeSeparators(path));
      PrimedGunSetCannonPreviewLabel(cannon_texture_preview_labels[texture_index], path);
    }

    cannon_status->setText(slot <= 0 ?
                               QObject::tr("Default is active. PrimedGun cannon overrides are clear.") :
                               QObject::tr("%1 is active.").arg(PrimedGunCannonSlotName(slot)));
  };

  auto apply_cannon_slot = [this, cannon_slot_group, refresh_cannon_texture_ui, cannon_status] {
    const int slot = cannon_slot_group->checkedId();
    Settings::GetQSettings().setValue(QStringLiteral("primegun/cannon_texture_slot"), slot);

    QString error;
    if (!PrimedGunApplyCannonTextureSlot(slot, &error))
    {
      ModalMessageBox::critical(this, tr("Cannon Textures"), error);
      return;
    }

    Config::SetBaseOrCurrent(Config::GFX_HIRES_TEXTURES, true);
    PrimedGunRefreshCustomTextureConfig(slot > 0);
    refresh_cannon_texture_ui();
    cannon_status->setText(slot <= 0 ?
                               tr("Default applied. Installed HD texture packs can supply the cannon.") :
                               tr("Applied %1. Custom textures are enabled.")
                                   .arg(PrimedGunCannonSlotName(slot)));
  };

  connect(cannon_slot_group, &QButtonGroup::idClicked, this,
          [refresh_cannon_texture_ui, cannon_status, cannon_slot_group] {
    refresh_cannon_texture_ui();
    const int slot = cannon_slot_group->checkedId();
    cannon_status->setText(slot <= 0 ?
                               QObject::tr("Default selected. Click Apply to use it.") :
                               QObject::tr("%1 selected. Click Apply to use it.")
                                   .arg(PrimedGunCannonSlotName(slot)));
  });

  auto* cannon_actions = new QHBoxLayout;
  auto* apply_cannon_button = new QPushButton(tr("Apply"), game_tab);
  auto* remove_cannon_shine_button = new QPushButton(tr("Remove Shine"), game_tab);
  auto* restore_cannon_shine_button = new QPushButton(tr("Restore Shine"), game_tab);
  auto* open_cannon_library_button = new QPushButton(tr("Open Slot Folder"), game_tab);
  auto* open_cannon_pack_button = new QPushButton(tr("Open Active Pack"), game_tab);
  cannon_actions->addWidget(apply_cannon_button);
  cannon_actions->addWidget(remove_cannon_shine_button);
  cannon_actions->addWidget(restore_cannon_shine_button);
  cannon_actions->addWidget(open_cannon_library_button);
  cannon_actions->addWidget(open_cannon_pack_button);
  cannon_actions->addStretch();
  cannon_layout->addLayout(cannon_actions);
  connect(apply_cannon_button, &QPushButton::clicked, this, apply_cannon_slot);
  connect(remove_cannon_shine_button, &QPushButton::clicked, this,
          [this, cannon_slot_group, cannon_texture_path_labels, cannon_status,
           cannon_texture_preview_labels, refresh_cannon_texture_ui] {
    const int slot = cannon_slot_group->checkedId();
    if (slot <= 0)
    {
      ModalMessageBox::information(this, tr("Cannon Textures"),
                                   tr("Choose Slot 1-4 or Custom before applying Remove Shine."));
      return;
    }

    QString error;
    if (!PrimedGunEnsureRemoveShinePreset(&error))
    {
      ModalMessageBox::critical(this, tr("Cannon Textures"), error);
      return;
    }
    if (!PrimedGunBackupSlotShinePreset(slot, &error))
    {
      ModalMessageBox::critical(this, tr("Cannon Textures"), error);
      return;
    }

    QDir slot_dir(PrimedGunCannonSlotDir(slot));
    if (!slot_dir.exists() && !slot_dir.mkpath(QStringLiteral(".")))
    {
      ModalMessageBox::critical(this, tr("Cannon Textures"),
                                tr("Could not create the selected cannon texture slot folder."));
      return;
    }

    const QString source = PrimedGunCannonRemoveShinePresetPath();
    const QString destination = PrimedGunCannonSourcePath(
        slot, PRIMEGUN_CANNON_SHEEN_TEXTURE_INDEX, QStringLiteral(".dds"));
    PrimedGunRemoveCannonSlotTextureFiles(slot, PRIMEGUN_CANNON_SHEEN_TEXTURE_INDEX);
    if (!QFile::copy(source, destination))
    {
      ModalMessageBox::critical(this, tr("Cannon Textures"),
                                tr("Could not copy the remove-shine texture into the slot."));
      return;
    }

    QSettings& settings = Settings::GetQSettings();
    settings.setValue(PrimedGunCannonSlotSetting(slot, PRIMEGUN_CANNON_SHEEN_TEXTURE_INDEX),
                      destination);
    settings.setValue(QStringLiteral("primegun/cannon_texture_slot"), slot);

    if (!PrimedGunApplyCannonTextureSlot(slot, &error))
    {
      ModalMessageBox::critical(this, tr("Cannon Textures"), error);
      return;
    }

    Config::SetBaseOrCurrent(Config::GFX_HIRES_TEXTURES, true);
    PrimedGunRefreshCustomTextureConfig(true);
    refresh_cannon_texture_ui();
    PrimedGunSetCannonPathLabel(cannon_texture_path_labels[PRIMEGUN_CANNON_SHEEN_TEXTURE_INDEX],
                               QDir::toNativeSeparators(destination));
    PrimedGunSetCannonPreviewLabel(
        cannon_texture_preview_labels[PRIMEGUN_CANNON_SHEEN_TEXTURE_INDEX], destination);
    cannon_status->setText(tr("Applied %1 with Remove Shine. Custom textures are enabled.")
                               .arg(PrimedGunCannonSlotName(slot)));
  });
  connect(restore_cannon_shine_button, &QPushButton::clicked, this,
          [this, cannon_slot_group, cannon_texture_path_labels, cannon_status,
           cannon_texture_preview_labels, refresh_cannon_texture_ui] {
    const int slot = cannon_slot_group->checkedId();
    if (slot <= 0)
    {
      ModalMessageBox::information(this, tr("Cannon Textures"),
                                   tr("Choose Slot 1-4 or Custom before restoring shine."));
      return;
    }

    const QString source = PrimedGunCannonRestoreShinePresetPath(slot);
    if (!QFileInfo(source).isFile())
    {
      ModalMessageBox::critical(this, tr("Cannon Textures"),
                                tr("Could not find the saved shine texture for this slot."));
      return;
    }

    QDir slot_dir(PrimedGunCannonSlotDir(slot));
    if (!slot_dir.exists() && !slot_dir.mkpath(QStringLiteral(".")))
    {
      ModalMessageBox::critical(this, tr("Cannon Textures"),
                                tr("Could not create the selected cannon texture slot folder."));
      return;
    }

    const QFileInfo source_info(source);
    const QString suffix = source_info.suffix().toLower();
    const QString destination = PrimedGunCannonSourcePath(
        slot, PRIMEGUN_CANNON_SHEEN_TEXTURE_INDEX, QLatin1Char('.') + suffix);
    PrimedGunRemoveCannonSlotTextureFiles(slot, PRIMEGUN_CANNON_SHEEN_TEXTURE_INDEX);
    if (!QFile::copy(source, destination))
    {
      ModalMessageBox::critical(this, tr("Cannon Textures"),
                                tr("Could not copy the default shine texture into the slot."));
      return;
    }

    QSettings& settings = Settings::GetQSettings();
    settings.setValue(PrimedGunCannonSlotSetting(slot, PRIMEGUN_CANNON_SHEEN_TEXTURE_INDEX),
                      destination);
    settings.setValue(QStringLiteral("primegun/cannon_texture_slot"), slot);

    QString error;
    if (!PrimedGunApplyCannonTextureSlot(slot, &error))
    {
      ModalMessageBox::critical(this, tr("Cannon Textures"), error);
      return;
    }

    Config::SetBaseOrCurrent(Config::GFX_HIRES_TEXTURES, true);
    PrimedGunRefreshCustomTextureConfig(true);
    refresh_cannon_texture_ui();
    PrimedGunSetCannonPathLabel(cannon_texture_path_labels[PRIMEGUN_CANNON_SHEEN_TEXTURE_INDEX],
                               QDir::toNativeSeparators(destination));
    PrimedGunSetCannonPreviewLabel(
        cannon_texture_preview_labels[PRIMEGUN_CANNON_SHEEN_TEXTURE_INDEX], destination);
    cannon_status->setText(tr("Restored shine for %1. Cannon base textures are unchanged.")
                               .arg(PrimedGunCannonSlotName(slot)));
  });
  connect(open_cannon_library_button, &QPushButton::clicked, this, [] {
    QDir dir(PrimedGunCannonLibraryDir());
    if (!dir.exists())
      dir.mkpath(QStringLiteral("."));
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir.absolutePath()));
  });
  connect(open_cannon_pack_button, &QPushButton::clicked, this, [] {
    PrimedGunEnsureCannonPackRegistration();
    QDesktopServices::openUrl(QUrl::fromLocalFile(PrimedGunCannonPackDir()));
  });
  refresh_cannon_texture_ui();
  cannon_layout->addStretch();

  auto* layout_tab = new QWidget(tabs);
  auto* layout_tab_layout = new QVBoxLayout(layout_tab);
  layout_tab_layout->setContentsMargins(14, 10, 14, 10);
  auto* layout_title = section_label(tr("Controller Layout"), layout_tab);
  layout_tab_layout->addWidget(layout_title);
  auto* controller_map = new PrimedGunScaledImageLabel(layout_tab);
  controller_map->SetSourcePixmap(QPixmap(assets_dir + QStringLiteral("controller layout.png")));
  layout_tab_layout->addWidget(controller_map, 1);
  tabs->addTab(layout_tab, tr("Layout"));

  auto* dolphin_layout = make_scroll_tab(tr("Dolphin Config"));
  dolphin_layout->addWidget(section_label(tr("Dolphin Config"), game_tab));
  auto* dolphin_note = new QLabel(
      tr("Open Dolphin's native configuration windows when you need emulator-specific settings."),
      game_tab);
  dolphin_note->setObjectName(QStringLiteral("PrimedGunMuted"));
  dolphin_layout->addWidget(dolphin_note);
  auto* open_general = new QPushButton(tr("Dolphin Settings"), game_tab);
  auto* open_hotkeys = new QPushButton(tr("Hotkey Settings"), game_tab);
  auto* open_memcards = new QPushButton(tr("Memory Card Manager"), game_tab);
  auto* open_cheats = new QPushButton(tr("Cheat Manager"), game_tab);
  auto* open_texture_packs = new QPushButton(tr("Resource Pack Manager"), game_tab);
  auto* debug_label = section_label(tr("Debug (send dump to dev upon crash)"), game_tab);
  auto* dump_ram = new QPushButton(tr("Dump RAM"), game_tab);
  dolphin_layout->addWidget(open_general);
  dolphin_layout->addWidget(open_hotkeys);
  dolphin_layout->addWidget(open_memcards);
  dolphin_layout->addWidget(open_cheats);
  dolphin_layout->addWidget(open_texture_packs);
  dolphin_layout->addSpacing(16);
  dolphin_layout->addWidget(debug_label);
  dolphin_layout->addWidget(dump_ram);
  dolphin_layout->addStretch();
  connect(open_general, &QPushButton::clicked, this, &MainWindow::ShowGeneralWindow);
  connect(open_hotkeys, &QPushButton::clicked, this, &MainWindow::ShowHotkeyDialog);
  connect(open_memcards, &QPushButton::clicked, this, &MainWindow::ShowMemcardManager);
  connect(open_cheats, &QPushButton::clicked, this, &MainWindow::ShowCheatsManager);
  connect(open_texture_packs, &QPushButton::clicked, this, &MainWindow::ShowResourcePackManager);
  connect(dump_ram, &QPushButton::clicked, this, [this] { PrimedGunDumpMem1(this); });

  game_layout->addWidget(tabs, 1);
  auto* footer_line = new QFrame(game_tab);
  footer_line->setFrameShape(QFrame::HLine);
  game_layout->addWidget(footer_line);
  auto* footer = new QHBoxLayout;
  auto* reset_all = new QPushButton(tr("Reset All"), game_tab);
  auto* save_settings_button = new QPushButton(tr("Save Settings"), game_tab);
  auto* credit = new QLabel(tr("By Nobbie   %1").arg(primedgun_version), game_tab);
  credit->setObjectName(QStringLiteral("PrimedGunMuted"));
  footer->addWidget(reset_all);
  footer->addWidget(save_settings_button);
  footer->addStretch();
  footer->addWidget(credit);
  game_layout->addLayout(footer);

  const auto refresh_visible_settings = [=] {
    const QSignalBlocker right_hand_blocker{right_hand};
    const QSignalBlocker left_hand_blocker{left_hand};
    const QSignalBlocker vr_overlays_enabled_blocker{vr_overlays_enabled};
    const QSignalBlocker height_prompt_enabled_blocker{height_prompt_enabled};
    const QSignalBlocker vr_menu_hold_left_stick_blocker{vr_menu_hold_left_stick};
    const QSignalBlocker vr_menu_requires_head_zone_blocker{vr_menu_requires_head_zone};
    const QSignalBlocker cinematic_screen_enabled_blocker{cinematic_screen_enabled};
    const QSignalBlocker rumble_enabled_blocker{rumble_enabled};
    const QSignalBlocker rumble_hand_mode_blocker{rumble_hand_mode};
    const QSignalBlocker dpad_enabled_blocker{dpad_enabled};
    const QSignalBlocker combat_jump_use_primary_button_blocker{
        combat_jump_use_primary_button};
    const QSignalBlocker primegun_grip_inputs_enabled_blocker{primegun_grip_inputs_enabled};
    const QSignalBlocker primegun_grip_inputs_use_trackpad_blocker{
        primegun_grip_inputs_use_trackpad};
    const QSignalBlocker movement_enabled_blocker{movement_enabled};
    const QSignalBlocker left_stick_blocker{left_stick};
    const QSignalBlocker right_stick_blocker{right_stick};
    const QSignalBlocker controller_direction_blocker{controller_direction};
    const QSignalBlocker hmd_direction_blocker{hmd_direction};
    const QSignalBlocker snap_turn_enabled_blocker{snap_turn_enabled};
    const QSignalBlocker snap_turn_degrees_blocker{snap_turn_degrees};
    const QSignalBlocker visor_helmet_enabled_blocker{visor_helmet_enabled};
    const QSignalBlocker position_marker_enabled_blocker{position_marker_enabled};
    const auto set_float = [float_rows](QDoubleSpinBox* spin, double value) {
      QSlider* linked_slider = nullptr;
      for (const auto& [row_spin, row_slider] : *float_rows)
      {
        if (row_spin == spin)
        {
          linked_slider = row_slider;
          break;
        }
      }

      const QSignalBlocker spin_blocker{spin};
      spin->setValue(value);
      if (linked_slider)
      {
        const QSignalBlocker slider_blocker{linked_slider};
        linked_slider->setValue(static_cast<int>(value / spin->singleStep()));
      }
    };

    right_hand->setChecked(runtime->use_right_hand);
    left_hand->setChecked(!runtime->use_right_hand);
    vr_overlays_enabled->setChecked(runtime->vr_overlays_enabled);
    height_prompt_enabled->setChecked(runtime->height_prompt_enabled);
    vr_menu_hold_left_stick->setChecked(runtime->vr_menu_hold_left_stick);
    vr_menu_requires_head_zone->setChecked(runtime->vr_menu_requires_head_zone);
    cinematic_screen_enabled->setChecked(runtime->cinematic_screen_enabled);
    rumble_enabled->setChecked(runtime->rumble_enabled);
    rumble_hand_mode->setCurrentIndex(std::clamp(runtime->rumble_hand_mode, 0, 2));
    dpad_enabled->setChecked(runtime->xr_dpad_enabled);
    combat_jump_use_primary_button->setChecked(runtime->combat_jump_use_primary_button);
    primegun_grip_inputs_enabled->setChecked(runtime->primegun_grip_inputs_enabled);
    primegun_grip_inputs_use_trackpad->setChecked(runtime->primegun_grip_inputs_use_trackpad);
    movement_enabled->setChecked(runtime->directional_movement_enabled);
    left_stick->setChecked(!runtime->directional_movement_use_right_stick);
    right_stick->setChecked(runtime->directional_movement_use_right_stick);
    controller_direction->setChecked(!runtime->directional_movement_use_hmd_direction);
    hmd_direction->setChecked(runtime->directional_movement_use_hmd_direction);
    snap_turn_enabled->setChecked(runtime->snap_turn_enabled);
    set_snap_turn_degrees_combo(runtime->snap_turn_degrees);
    visor_helmet_enabled->setChecked(runtime->visor_helmet_enabled);
    position_marker_enabled->setChecked(runtime->position_marker_enabled);
    set_float(dpad_radius_spin, runtime->xr_dpad_head_radius);
    set_float(trackpad_press_threshold_spin, runtime->primegun_trackpad_press_threshold);
    set_float(rumble_intensity_spin, runtime->rumble_intensity);
    set_float(dpad_below_spin, runtime->xr_dpad_head_y_below);
    set_float(dpad_deadzone_spin, runtime->xr_dpad_deadzone);
    set_float(movement_deadzone_spin, runtime->directional_movement_deadzone);
    set_float(movement_speed_spin, runtime->directional_movement_speed);
    set_float(movement_accel_spin, runtime->directional_movement_accel);
    set_float(movement_air_accel_spin, runtime->directional_movement_air_accel);
    set_float(look_yaw_sensitivity_spin, runtime->look_yaw_sensitivity);
    set_float(metroid_hud_distance_spin, runtime->metroid_hud_distance);
    set_float(metroid_hud_size_spin, runtime->metroid_hud_size);
    set_float(target_distance_spin, runtime->gun_targeting_distance);
    set_float(target_radius_spin, runtime->gun_targeting_radius);
    set_float(model_x_spin, runtime->model_offset_x);
    set_float(model_y_spin, runtime->model_offset_y);
    set_float(model_z_spin, runtime->model_offset_z);
    set_float(rot_x_spin, runtime->rot_offset_x);
    set_float(rot_y_spin, runtime->rot_offset_y);
    set_float(rot_z_spin, runtime->rot_offset_z);
  };

  const auto reset_calibration_values = [runtime, refresh_visible_settings, apply_runtime] {
    runtime->model_offset_x = 0.0f;
    runtime->model_offset_y = 0.0f;
    runtime->model_offset_z = 0.0f;
    runtime->rot_offset_x = 0.0f;
    runtime->rot_offset_y = 0.0f;
    runtime->rot_offset_z = 0.0f;
    refresh_visible_settings();
    apply_runtime();
  };
  connect(reset_controller, &QPushButton::clicked, this,
          [runtime, refresh_visible_settings, apply_runtime] {
    runtime->use_right_hand = true;
    runtime->vr_overlays_enabled = true;
    runtime->vr_menu_hold_left_stick = false;
    runtime->vr_menu_requires_head_zone = false;
    runtime->cinematic_screen_enabled = false;
    runtime->rumble_enabled = true;
    runtime->rumble_intensity = 0.35f;
    runtime->rumble_hand_mode = 2;
    runtime->xr_dpad_enabled = true;
    runtime->combat_jump_use_primary_button = false;
    runtime->primegun_grip_inputs_enabled = true;
    runtime->primegun_grip_inputs_use_trackpad = false;
    runtime->primegun_trackpad_press_threshold = 0.5f;
    runtime->directional_movement_enabled = true;
    runtime->directional_movement_use_right_stick = false;
    runtime->directional_movement_use_hmd_direction = false;
    runtime->xr_dpad_head_radius = 0.28f;
    runtime->xr_dpad_head_y_below = 0.02f;
    runtime->xr_dpad_deadzone = 0.45f;
    runtime->directional_movement_deadzone = 0.25f;
    runtime->directional_movement_speed = 14.0f;
    runtime->directional_movement_accel = 45.0f;
    runtime->directional_movement_air_accel = 8.0f;
    runtime->look_yaw_sensitivity = 1.0f;
    runtime->snap_turn_enabled = false;
    runtime->snap_turn_degrees = 45;
    refresh_visible_settings();
    apply_runtime();
  });
  connect(reset_aiming, &QPushButton::clicked, this,
          [runtime, refresh_visible_settings, apply_runtime] {
    runtime->gun_targeting_enabled = true;
    runtime->gun_targeting_distance = 60.0f;
    runtime->gun_targeting_radius = 4.0f;
    runtime->visor_helmet_enabled = false;
    refresh_visible_settings();
    apply_runtime();
  });
  connect(right_hand, &QRadioButton::toggled, this, [runtime, apply_runtime](bool checked) {
    if (checked)
    {
      runtime->use_right_hand = true;
      apply_runtime();
    }
  });
  connect(left_hand, &QRadioButton::toggled, this, [runtime, apply_runtime](bool checked) {
    if (checked)
    {
      runtime->use_right_hand = false;
      apply_runtime();
    }
  });
  connect(vr_overlays_enabled, &QCheckBox::toggled, this,
          [runtime, apply_runtime](bool checked) {
    runtime->vr_overlays_enabled = checked;
    apply_runtime();
  });
  connect(vr_menu_hold_left_stick, &QCheckBox::toggled, this,
          [runtime, apply_runtime](bool checked) {
    runtime->vr_menu_hold_left_stick = checked;
    apply_runtime();
  });
  connect(vr_menu_requires_head_zone, &QCheckBox::toggled, this,
          [runtime, apply_runtime](bool checked) {
    runtime->vr_menu_requires_head_zone = checked;
    apply_runtime();
  });
  connect(cinematic_screen_enabled, &QCheckBox::toggled, this,
          [runtime, apply_runtime](bool checked) {
    runtime->cinematic_screen_enabled = checked;
    apply_runtime();
  });
  connect(rumble_enabled, &QCheckBox::toggled, this, [runtime, apply_runtime](bool checked) {
    runtime->rumble_enabled = checked;
    apply_runtime();
  });
  connect(rumble_hand_mode, qOverload<int>(&QComboBox::currentIndexChanged), this,
          [runtime, apply_runtime](int index) {
    runtime->rumble_hand_mode = std::clamp(index, 0, 2);
    apply_runtime();
  });
  connect(dpad_enabled, &QCheckBox::toggled, this, [runtime, apply_runtime](bool checked) {
    runtime->xr_dpad_enabled = checked;
    apply_runtime();
  });
  connect(combat_jump_use_primary_button, &QCheckBox::toggled, this,
          [runtime, apply_runtime](bool checked) {
    runtime->combat_jump_use_primary_button = checked;
    apply_runtime();
  });
  connect(primegun_grip_inputs_enabled, &QCheckBox::toggled, this,
          [runtime, apply_runtime](bool checked) {
    runtime->primegun_grip_inputs_enabled = checked;
    apply_runtime();
  });
  connect(primegun_grip_inputs_use_trackpad, &QCheckBox::toggled, this,
          [runtime, apply_runtime](bool checked) {
    runtime->primegun_grip_inputs_use_trackpad = checked;
    apply_runtime();
  });
  connect(movement_enabled, &QCheckBox::toggled, this, [runtime, apply_runtime](bool checked) {
    runtime->directional_movement_enabled = checked;
    apply_runtime();
  });
  connect(left_stick, &QRadioButton::toggled, this, [runtime, apply_runtime](bool checked) {
    if (checked)
    {
      runtime->directional_movement_use_right_stick = false;
      apply_runtime();
    }
  });
  connect(right_stick, &QRadioButton::toggled, this, [runtime, apply_runtime](bool checked) {
    if (checked)
    {
      runtime->directional_movement_use_right_stick = true;
      apply_runtime();
    }
  });
  connect(controller_direction, &QRadioButton::toggled, this,
          [runtime, apply_runtime](bool checked) {
    if (checked)
    {
      runtime->directional_movement_use_hmd_direction = false;
      apply_runtime();
    }
  });
  connect(hmd_direction, &QRadioButton::toggled, this, [runtime, apply_runtime](bool checked) {
    if (checked)
    {
      runtime->directional_movement_use_hmd_direction = true;
      apply_runtime();
    }
  });
  connect(snap_turn_enabled, &QCheckBox::toggled, this, [runtime, apply_runtime](bool checked) {
    runtime->snap_turn_enabled = checked;
    apply_runtime();
  });
  connect(snap_turn_degrees, qOverload<int>(&QComboBox::currentIndexChanged), this,
          [runtime, snap_turn_degrees, apply_runtime](int index) {
    if (index < 0)
      return;
    runtime->snap_turn_degrees = snap_turn_degrees->itemData(index).toInt();
    apply_runtime();
  });
  connect(visor_helmet_enabled, &QCheckBox::toggled, this,
          [runtime, apply_runtime](bool checked) {
    runtime->visor_helmet_enabled = checked;
    apply_runtime();
  });
  connect(height_prompt_enabled, &QCheckBox::toggled, this,
          [runtime, apply_runtime](bool checked) {
    runtime->height_prompt_enabled = checked;
    apply_runtime();
  });
  connect(position_marker_enabled, &QCheckBox::toggled, this,
          [runtime, apply_runtime](bool checked) {
    runtime->position_marker_enabled = checked;
    apply_runtime();
  });
  connect(reset_hud, &QPushButton::clicked, this,
          [runtime, refresh_visible_settings, apply_runtime] {
    const PrimedGun::RuntimeSettings defaults{};
    runtime->metroid_hud_distance = defaults.metroid_hud_distance;
    runtime->metroid_hud_size = defaults.metroid_hud_size;
    refresh_visible_settings();
    apply_runtime();
  });
  connect(reset_calibration, &QPushButton::clicked, this, reset_calibration_values);
  connect(default_preset, &QPushButton::clicked, this, reset_calibration_values);
  connect(samus_preset, &QPushButton::clicked, this,
          [runtime, refresh_visible_settings, apply_runtime] {
    runtime->model_offset_x = 0.0f;
    runtime->model_offset_y = -0.300f;
    runtime->model_offset_z = 0.0f;
    runtime->rot_offset_x = 0.0f;
    runtime->rot_offset_y = 20.0f;
    runtime->rot_offset_z = -90.0f;
    refresh_visible_settings();
    apply_runtime();
  });
  connect(reset_all, &QPushButton::clicked, this,
          [runtime, refresh_visible_settings, refresh_start_button, apply_runtime] {
    *runtime = {};
    runtime->offset_x = 0.0f;
    runtime->offset_y = 0.0f;
    runtime->offset_z = 0.0f;
    refresh_visible_settings();
    refresh_start_button();
    apply_runtime();
  });
  connect(save_settings_button, &QPushButton::clicked, this,
          [save_primegun_runtime_settings, runtime] { save_primegun_runtime_settings(*runtime); });

  auto* runtime_ui_sync_timer = new QTimer(this);
  connect(runtime_ui_sync_timer, &QTimer::timeout, this, [runtime, refresh_visible_settings] {
    *runtime = PrimedGun::GetRuntimeSettings();
    refresh_visible_settings();
  });
  runtime_ui_sync_timer->start(100);

  update_selected_game(settings.value(selected_metroid_game_setting).toString());

  connect(select_button, &QPushButton::clicked, this,
          [this, selected_metroid_game_setting, update_selected_game] {
    const QStringList files = PromptFileNames();
    if (files.isEmpty())
      return;

    Settings::GetQSettings().setValue(selected_metroid_game_setting, files.front());
    update_selected_game(files.front());
  });

  connect(play_button, &QPushButton::clicked, this, [this, selected_metroid_game_setting] {
    const QString path = Settings::GetQSettings().value(selected_metroid_game_setting).toString();
    if (!path.isEmpty())
      StartGame(path, ScanForSecondDisc::Yes);
  });
  connect(pause_button, &QPushButton::clicked, this, &MainWindow::TogglePause);
  connect(stop_button, &QPushButton::clicked, this, &MainWindow::RequestStop);
  connect(transfer_old_save_button, &QPushButton::clicked, this,
          [this, load_primegun_runtime_settings, runtime, refresh_visible_settings,
           selected_metroid_game_setting, update_selected_game] {
    if (Core::GetState(m_system) != Core::State::Uninitialized)
    {
      ModalMessageBox::warning(this, tr("Transfer Old Memory Card"),
                               tr("Stop the game before transferring a memory card save."));
      return;
    }

    QMessageBox prompt(this);
    prompt.setWindowTitle(tr("Transfer Old Memory Card"));
    prompt.setText(tr("Transfer old save game."));
    prompt.setInformativeText(
        tr("PrimedGun will search nearby old PrimedGun folders and automatically transfer and "
           "apply your save."));
    auto* transfer_button = prompt.addButton(tr("Transfer"), QMessageBox::AcceptRole);
    prompt.addButton(QMessageBox::Cancel);
    prompt.exec();

    if (prompt.clickedButton() != transfer_button)
      return;

    const QString destination = QDir::toNativeSeparators(QString::fromStdString(
        Config::GetMemcardPath(ExpansionInterface::Slot::A, DiscIO::Region::NTSC_U)));
    const QString source = PrimedGunFindNearbyOldMemoryCard(destination);
    if (source.isEmpty())
    {
      ModalMessageBox::critical(
          this, tr("Transfer Old Memory Card"),
          tr("Could not find an old memory card near this PrimedGun install.\n\n"
             "Place the new version near your old PrimedGun folder, then try again."));
      return;
    }

    const QFileInfo destination_info(destination);
    QDir destination_dir = destination_info.absoluteDir();
    if (!destination_dir.exists() && !destination_dir.mkpath(QStringLiteral(".")))
    {
      ModalMessageBox::critical(
          this, tr("Transfer Old Memory Card"),
          tr("Could not create the current memory card folder:\n%1")
              .arg(destination_dir.absolutePath()));
      return;
    }

#ifdef _WIN32
    constexpr Qt::CaseSensitivity path_case_sensitivity = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity path_case_sensitivity = Qt::CaseSensitive;
#endif
    if (QString::compare(QDir::cleanPath(QFileInfo(source).absoluteFilePath()),
                         QDir::cleanPath(destination_info.absoluteFilePath()),
                         path_case_sensitivity) == 0)
    {
      ModalMessageBox::information(this, tr("Transfer Old Memory Card"),
                                   tr("That save is already the current memory card."));
      return;
    }

    QString backup_path;
    if (destination_info.exists())
    {
      const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss"));
      const QString suffix = destination_info.suffix();
      const QString backup_name =
          suffix.isEmpty() ?
              QStringLiteral("%1.backup-%2").arg(destination_info.fileName(), timestamp) :
              QStringLiteral("%1.backup-%2.%3")
                  .arg(destination_info.completeBaseName(), timestamp, suffix);
      backup_path = destination_dir.filePath(backup_name);
      if (!QFile::rename(destination, backup_path))
      {
        ModalMessageBox::critical(
          this, tr("Transfer Old Memory Card"),
            tr("Could not back up the current memory card:\n%1").arg(destination));
        return;
      }
    }

    if (!QFile::copy(source, destination))
    {
      if (!backup_path.isEmpty())
        QFile::rename(backup_path, destination);
      ModalMessageBox::critical(this, tr("Transfer Old Memory Card"),
                                tr("Could not copy the selected save to:\n%1").arg(destination));
      return;
    }

    const QString old_settings_path = PrimedGunFindSettingsNearMemoryCard(source);
    const int imported_settings_count =
        PrimedGunImportSettingsFromFile(old_settings_path, &Settings::GetQSettings());
    QString old_dolphin_settings_source;
    QString dolphin_settings_error;
    const int imported_dolphin_settings_count = PrimedGunTransferDolphinSettingsNearMemoryCard(
        source, &old_dolphin_settings_source, &dolphin_settings_error);
    if (imported_dolphin_settings_count < 0)
    {
      ModalMessageBox::critical(this, tr("Transfer Old Memory Card"), dolphin_settings_error);
      return;
    }

    if (imported_settings_count > 0)
    {
      load_primegun_runtime_settings();
      *runtime = PrimedGun::GetRuntimeSettings();
      refresh_visible_settings();
      update_selected_game(Settings::GetQSettings().value(selected_metroid_game_setting).toString());
    }

    QString message = tr("Old memory card transferred to:\n%1").arg(destination);
    if (!backup_path.isEmpty())
      message += tr("\n\nPrevious memory card backed up to:\n%1").arg(backup_path);
    if (imported_settings_count > 0)
    {
      message += tr("\n\nPrimeGun settings transferred from:\n%1").arg(old_settings_path);
    }
    else
    {
      message += tr("\n\nNo old PrimeGun settings were found in a folder with PrimedGun.exe.");
    }
    if (imported_dolphin_settings_count > 0)
    {
      message += tr("\n\nDolphin settings transferred from:\n%1\n%2 file(s) copied.")
                     .arg(old_dolphin_settings_source)
                     .arg(imported_dolphin_settings_count);
    }
    else
    {
      message += tr("\n\nNo old user-created Dolphin settings were found in a folder with "
                    "PrimedGun.exe.");
    }
    ModalMessageBox::information(this, tr("Transfer Old Memory Card"), message);
  });

  connect(options_button, &QPushButton::clicked, this,
          [this, make_selected_game, selected_metroid_game_setting, options_button] {
    const auto game = make_selected_game();
    if (!game)
      return;

    QMenu menu(options_button);
    menu.addAction(tr("Properties"), this, [this, game] {
      auto property_windows = this->findChildren<PropertiesDialog*>();
      auto it =
          std::ranges::find(property_windows, game->GetFilePath(), &PropertiesDialog::GetFilePath);
      if (it != property_windows.end())
      {
        (*it)->raise();
        return;
      }

      auto* properties = new PropertiesDialog(this, *game);
      connect(properties, &PropertiesDialog::OpenGeneralSettings, this,
              &MainWindow::ShowGeneralWindow);
      connect(properties, &PropertiesDialog::OpenGraphicsSettings, this,
              &MainWindow::ShowGraphicsWindow);
      connect(properties, &PropertiesDialog::finished, properties, &QObject::deleteLater);
      properties->show();
    });
    menu.addAction(tr("Wiki"), this, [game] {
      const QString game_id = QString::fromStdString(game->GetGameID());
      QDesktopServices::openUrl(QUrl(QStringLiteral(
          "https://wiki.dolphin-emu.org/dolphin-redirect.php?gameid=%1").arg(game_id)));
    });

    menu.addSeparator();
    menu.addAction(tr("Start with Riivolution Patches..."), this,
                   [this, game] { ShowRiivolutionBootWidget(*game); });

    menu.addSeparator();
    menu.addAction(tr("Set as Default ISO"), this, [game] {
      Settings::Instance().SetDefaultGame(
          QDir::toNativeSeparators(QString::fromStdString(game->GetFilePath())));
    });
    menu.addAction(tr("Convert File..."), this, [this, game] {
      ConvertDialog dialog({std::const_pointer_cast<const UICommon::GameFile>(game)}, this);
      dialog.exec();
    });
    auto* change_disc = menu.addAction(tr("Change Disc"));
    change_disc->setEnabled(false);

    menu.addSeparator();
    menu.addAction(tr("Open GameCube Save Folder"), this, [this] {
      const QUrl url = QUrl::fromLocalFile(QString::fromStdString(File::GetUserPath(D_GCUSER_IDX)));
      QDesktopServices::openUrl(url);
    });
    menu.addAction(tr("Open Containing Folder"), this, [game] {
      std::string parent_directory_path;
      SplitPath(game->GetFilePath(), &parent_directory_path, nullptr, nullptr);
      if (!parent_directory_path.empty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(parent_directory_path)));
    });
    menu.addAction(tr("Delete File..."), this, [this, game, selected_metroid_game_setting] {
      if (ModalMessageBox::question(this, tr("Confirm"),
                                    tr("Are you sure you want to delete this file?")) !=
          QMessageBox::Yes)
        return;

      if (File::Delete(game->GetFilePath()))
        Settings::GetQSettings().remove(selected_metroid_game_setting);
      else
        ModalMessageBox::critical(this, tr("Failure"), tr("Failed to delete the selected file."));
    });

    auto* shortcut = menu.addAction(tr("Add Shortcut to Desktop"));
    shortcut->setEnabled(false);

    menu.addSeparator();
    auto* tags = menu.addMenu(tr("Tags"));
    tags->setEnabled(false);
    auto* new_tag = menu.addAction(tr("New Tag..."));
    new_tag->setEnabled(false);
    auto* remove_tag = menu.addAction(tr("Remove Tag..."));
    remove_tag->setEnabled(false);

    menu.addSeparator();
    menu.exec(options_button->mapToGlobal(QPoint(0, options_button->height())));
  });

  layout->addWidget(game_tab);
  layout->setContentsMargins(0, 0, 0, 0);

  connect(m_search_bar, &SearchBar::Search, m_game_list, &GameList::SetSearchTerm);

  m_stack->addWidget(widget);

  setCentralWidget(m_stack);

  setDockOptions(DockOption::AllowNestedDocks | DockOption::AllowTabbedDocks);
  setTabPosition(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea, QTabWidget::North);
  addDockWidget(Qt::LeftDockWidgetArea, m_log_widget);
  addDockWidget(Qt::LeftDockWidgetArea, m_log_config_widget);
  addDockWidget(Qt::LeftDockWidgetArea, m_code_widget);
  addDockWidget(Qt::LeftDockWidgetArea, m_register_widget);
  addDockWidget(Qt::LeftDockWidgetArea, m_thread_widget);
  addDockWidget(Qt::LeftDockWidgetArea, m_watch_widget);
  addDockWidget(Qt::LeftDockWidgetArea, m_breakpoint_widget);
  addDockWidget(Qt::LeftDockWidgetArea, m_memory_widget);
  addDockWidget(Qt::LeftDockWidgetArea, m_jit_widget);
  addDockWidget(Qt::LeftDockWidgetArea, m_assembler_widget);

  tabifyDockWidget(m_log_widget, m_log_config_widget);
  tabifyDockWidget(m_log_widget, m_code_widget);
  tabifyDockWidget(m_log_widget, m_register_widget);
  tabifyDockWidget(m_log_widget, m_thread_widget);
  tabifyDockWidget(m_log_widget, m_watch_widget);
  tabifyDockWidget(m_log_widget, m_breakpoint_widget);
  tabifyDockWidget(m_log_widget, m_memory_widget);
  tabifyDockWidget(m_log_widget, m_jit_widget);
  tabifyDockWidget(m_log_widget, m_assembler_widget);
}

void MainWindow::RefreshGameList()
{
  Settings::Instance().ReloadTitleDB();
  Settings::Instance().RefreshGameList();
}

QStringList MainWindow::PromptFileNames()
{
  auto& settings = Settings::Instance().GetQSettings();
  QStringList paths = DolphinFileDialog::getOpenFileNames(
      this, tr("Select a File"),
      settings.value(QStringLiteral("mainwindow/lastdir"), QString{}).toString(),
      QStringLiteral("%1 (*.elf *.dol *.gcm *.bin *.iso *.tgc *.wbfs *.ciso *.gcz *.wia *.rvz "
                     "hif_000000.nfs *.wad *.dff *.m3u *.json);;%2 (*)")
          .arg(tr("All GC/Wii files"))
          .arg(tr("All Files")));

  if (!paths.isEmpty())
  {
    settings.setValue(QStringLiteral("mainwindow/lastdir"),
                      QFileInfo(paths.front()).absoluteDir().absolutePath());
  }

  return paths;
}

void MainWindow::ChangeDisc()
{
  std::vector<std::string> paths = StringListToStdVector(PromptFileNames());

  if (paths.empty())
    return;

  m_system.GetDVDInterface().ChangeDisc(Core::CPUThreadGuard{m_system}, paths);
}

void MainWindow::EjectDisc()
{
  m_system.GetDVDInterface().EjectDisc(Core::CPUThreadGuard{m_system}, DVD::EjectCause::User);
}

void MainWindow::OpenUserFolder()
{
  std::string path = File::GetUserPath(D_USER_IDX);

  QUrl url = QUrl::fromLocalFile(QString::fromStdString(path));
  QDesktopServices::openUrl(url);
}

void MainWindow::OpenConfigFolder()
{
  std::string path = File::GetUserPath(D_CONFIG_IDX);

  QUrl url = QUrl::fromLocalFile(QString::fromStdString(path));
  QDesktopServices::openUrl(url);
}

void MainWindow::OpenCacheFolder()
{
  std::string path = File::GetUserPath(D_CACHE_IDX);

  QUrl url = QUrl::fromLocalFile(QString::fromStdString(path));
  QDesktopServices::openUrl(url);
}

void MainWindow::Open()
{
  QStringList files = PromptFileNames();
  if (!files.isEmpty())
    StartGame(StringListToStdVector(files));
}

void MainWindow::Play(const std::optional<std::string>& savestate_path)
{
  // If we're in a paused game, start it up again.
  // Otherwise, play the selected game, if there is one.
  // Otherwise, play the default game.
  // Otherwise, play the last played game, if there is one.
  // Otherwise, prompt for a new game.
  if (Core::GetState(m_system) == Core::State::Paused)
  {
    Core::SetState(m_system, Core::State::Running);
  }
  else
  {
    std::shared_ptr<const UICommon::GameFile> selection = m_game_list->GetSelectedGame();
    if (selection)
    {
      StartGame(selection->GetFilePath(), ScanForSecondDisc::Yes,
                std::make_unique<BootSessionData>(savestate_path, DeleteSavestateAfterBoot::No));
    }
    else
    {
      const QString default_path = QString::fromStdString(Config::Get(Config::MAIN_DEFAULT_ISO));
      if (!default_path.isEmpty() && QFile::exists(default_path))
      {
        StartGame(default_path, ScanForSecondDisc::Yes,
                  std::make_unique<BootSessionData>(savestate_path, DeleteSavestateAfterBoot::No));
      }
      else
      {
        Open();
      }
    }
  }
}

void MainWindow::Pause()
{
  Core::SetState(m_system, Core::State::Paused);
}

void MainWindow::TogglePause()
{
  if (Core::GetState(m_system) == Core::State::Paused)
  {
    Play();
  }
  else
  {
    Pause();
  }
}

void MainWindow::OnStopComplete()
{
  m_stop_requested = false;
  HideRenderWidget(!m_exit_requested, m_exit_requested);
  SetFullScreenResolution(false);

  if (m_exit_requested || Settings::Instance().IsBatchModeEnabled())
  {
    if (m_assembler_widget->ApplicationCloseRequest())
    {
      QGuiApplication::exit(0);
    }
    else
    {
      m_exit_requested = false;
    }
  }

  // If the current emulation prevented the booting of another, do that now
  if (m_pending_boot != nullptr)
  {
    StartGame(std::move(m_pending_boot));
    m_pending_boot.reset();
  }
}

bool MainWindow::RequestStop()
{
  if (Core::IsUninitialized(m_system))
  {
    Core::QueueHostJob([this](Core::System&) { OnStopComplete(); }, true);
    return true;
  }

  const bool rendered_widget_was_active =
      Settings::Instance().IsKeepWindowOnTopEnabled() ||
      (m_render_widget->isActiveWindow() && !m_render_widget->isFullScreen());
  QWidget* confirm_parent = (!m_rendering_to_main && rendered_widget_was_active) ?
                                m_render_widget :
                                static_cast<QWidget*>(this);
  const bool was_cursor_locked = m_render_widget->IsCursorLocked();

  if (!m_render_widget->isFullScreen())
    m_render_widget_geometry = m_render_widget->saveGeometry();
  else
    FullScreen();

  bool confirm_on_stop = Config::Get(Config::MAIN_CONFIRM_ON_STOP);
#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION
  confirm_on_stop = confirm_on_stop || AchievementManager::GetInstance().CheckForModifications();
#endif  // RC_CLIENT_SUPPORTS_RAINTEGRATION
  if (confirm_on_stop)
  {
    if (std::exchange(m_stop_confirm_showing, true))
      return true;

    Common::ScopeGuard confirm_lock([this] { m_stop_confirm_showing = false; });

    const Core::State state = Core::GetState(m_system);

    bool pause = true;

    if (pause)
      Core::SetState(m_system, Core::State::Paused);

    if (rendered_widget_was_active)
    {
      // We have to do this before creating the message box, otherwise we might receive the window
      // activation event before we know we need to lock the cursor again.
      m_render_widget->SetCursorLockedOnNextActivation(was_cursor_locked);
    }

    // This is to avoid any "race conditions" between the "Window Activate" message and the
    // message box returning, which could break cursor locking depending on the order
    m_render_widget->SetWaitingForMessageBox(true);
    QString message;
    if (m_stop_requested)
    {
      message = tr("A shutdown is already in progress. Unsaved data "
                   "may be lost if you stop the current emulation "
                   "before it completes. Force stop?");
    }
#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION
    else if (AchievementManager::GetInstance().CheckForModifications())
    {
      message = tr(
          "Do you want to stop the current emulation? Unsaved achievement modifications detected.");
    }
#endif  // RC_CLIENT_SUPPORTS_RAINTEGRATION
    else
    {
      message = tr("Do you want to stop the current emulation?");
    }
    auto confirm = ModalMessageBox::question(confirm_parent, tr("Confirm"), message,
                                             QMessageBox::Yes | QMessageBox::No,
                                             QMessageBox::NoButton, Qt::ApplicationModal);

    // If a user confirmed stopping the emulation, we do not capture the cursor again,
    // even if the render widget will stay alive for a while.
    // If a used rejected stopping the emulation, we instead capture the cursor again,
    // and let them continue playing as if nothing had happened
    // (assuming cursor locking is on).
    if (confirm != QMessageBox::Yes)
    {
      m_render_widget->SetWaitingForMessageBox(false);

      if (pause)
        Core::SetState(m_system, state);

      return false;
    }
    else
    {
      m_render_widget->SetCursorLockedOnNextActivation(false);
      // This needs to be after SetCursorLockedOnNextActivation(false) as it depends on it
      m_render_widget->SetWaitingForMessageBox(false);
    }
  }

  OnStopRecording();
  // TODO: Add Debugger shutdown

  if (!m_stop_requested && UICommon::TriggerSTMPowerEvent())
  {
    m_stop_requested = true;

    // Unpause because gracefully shutting down needs the game to actually request a shutdown.
    // TODO: Do not unpause in debug mode to allow debugging until the complete shutdown.
    if (Core::GetState(m_system) == Core::State::Paused)
      Core::SetState(m_system, Core::State::Running);

    return true;
  }

  ForceStop();
#ifdef Q_OS_WIN
  // Allow windows to idle or turn off display again
  SetThreadExecutionState(ES_CONTINUOUS);
#endif
  return true;
}

void MainWindow::ForceStop()
{
  Core::Stop(m_system);
}

void MainWindow::Reset()
{
  auto& movie = m_system.GetMovie();
  if (movie.IsRecordingInput())
    movie.SetReset(true);
  m_system.GetProcessorInterface().ResetButton_Tap();
}

void MainWindow::FrameAdvance()
{
  Core::DoFrameStep(m_system);
}

void MainWindow::FullScreen()
{
  // If the render widget is fullscreen we want to reset it to whatever is in
  // settings. If it's set to be fullscreen then it just remakes the window,
  // which probably isn't ideal.
  bool was_fullscreen = m_render_widget->isFullScreen();

  if (!was_fullscreen)
    m_render_widget_geometry = m_render_widget->saveGeometry();

  HideRenderWidget(false);
  SetFullScreenResolution(!was_fullscreen);

  if (was_fullscreen)
  {
    ShowRenderWidget();
  }
  else
  {
    m_render_widget->showFullScreen();
  }
}

void MainWindow::UnlockCursor()
{
  if (!m_render_widget->isFullScreen())
    m_render_widget->SetCursorLocked(false);
}

void MainWindow::ScreenShot()
{
  Core::SaveScreenShot();
}

void MainWindow::ScanForSecondDiscAndStartGame(const UICommon::GameFile& game,
                                               std::unique_ptr<BootSessionData> boot_session_data)
{
  auto second_game = m_game_list->FindSecondDisc(game);

  std::vector<std::string> paths = {game.GetFilePath()};
  if (second_game != nullptr)
    paths.push_back(second_game->GetFilePath());

  StartGame(paths, std::move(boot_session_data));
}

void MainWindow::StartGame(const QString& path, ScanForSecondDisc scan,
                           std::unique_ptr<BootSessionData> boot_session_data)
{
  StartGame(path.toStdString(), scan, std::move(boot_session_data));
}

void MainWindow::StartGame(const std::string& path, ScanForSecondDisc scan,
                           std::unique_ptr<BootSessionData> boot_session_data)
{
  if (scan == ScanForSecondDisc::Yes)
  {
    std::shared_ptr<const UICommon::GameFile> game = m_game_list->FindGame(path);
    if (game != nullptr)
    {
      ScanForSecondDiscAndStartGame(*game, std::move(boot_session_data));
      return;
    }
  }

  StartGame(BootParameters::GenerateFromFile(
      path, boot_session_data ? std::move(*boot_session_data) : BootSessionData()));
}

void MainWindow::StartGame(const std::vector<std::string>& paths,
                           std::unique_ptr<BootSessionData> boot_session_data)
{
  StartGame(BootParameters::GenerateFromFile(
      paths, boot_session_data ? std::move(*boot_session_data) : BootSessionData()));
}

void MainWindow::StartGame(std::unique_ptr<BootParameters>&& parameters)
{
  if (parameters && std::holds_alternative<BootParameters::Disc>(parameters->parameters))
  {
    if (std::get<BootParameters::Disc>(parameters->parameters).volume->IsNKit())
    {
      if (!NKitWarningDialog::ShowUnlessDisabled())
        return;
    }

    const auto volume_type =
        std::get<BootParameters::Disc>(parameters->parameters).volume->GetVolumeType();
    if (volume_type != DiscIO::Platform::Triforce)
    {
      const bool triforce_hardware_sp1 =
          Config::Get(Config::MAIN_SERIAL_PORT_1) == ExpansionInterface::EXIDeviceType::Baseboard;
      const bool triforce_hardware_port_1 = Config::Get(Config::GetInfoForSIDevice(0)) ==
                                            SerialInterface::SIDevices::SIDEVICE_AM_BASEBOARD;

      // Some Triforce tools don't include a boot.id file, but they can still be launched.
      if (triforce_hardware_sp1)
      {
        ModalMessageBox::warning(this, tr("Warning"),
                                 tr("Non-Triforce games cannot be booted with Triforce hardware "
                                    "attached.\nPlease remove the Triforce Baseboard from SP1."),
                                 QMessageBox::Ok);
      }
      if (triforce_hardware_port_1)
      {
        ModalMessageBox::warning(this, tr("Warning"),
                                 tr("Non-Triforce games cannot be booted with Triforce hardware "
                                    "attached.\nPlease remove the Triforce Baseboard from Port 1."),
                                 QMessageBox::Ok);
      }
    }
  }

  // If we're running, only start a new game once we've stopped the last.
  if (!Core::IsUninitialized(m_system))
  {
    if (!RequestStop())
      return;

    // As long as the shutdown isn't complete, we can't boot, so let's boot later
    m_pending_boot = std::move(parameters);
    return;
  }

  // We need the render widget before booting.
  ShowRenderWidget();

  // Boot up, show an error if it fails to load the game.
  if (!BootManager::BootCore(m_system, std::move(parameters),
                             ::GetWindowSystemInfo(m_render_widget->windowHandle())))
  {
    ModalMessageBox::critical(this, tr("Error"), tr("Failed to init core"), QMessageBox::Ok);
    HideRenderWidget();
    return;
  }

  if (Config::Get(Config::MAIN_FULLSCREEN))
    m_fullscreen_requested = true;
}

void MainWindow::SetFullScreenResolution(bool fullscreen)
{
  if (Config::Get(Config::MAIN_FULLSCREEN_DISPLAY_RES) == "Auto")
    return;
#ifdef _WIN32

  if (!fullscreen)
  {
    ChangeDisplaySettings(nullptr, CDS_FULLSCREEN);
    return;
  }

  DEVMODE screen_settings;
  memset(&screen_settings, 0, sizeof(screen_settings));
  screen_settings.dmSize = sizeof(screen_settings);
  sscanf(Config::Get(Config::MAIN_FULLSCREEN_DISPLAY_RES).c_str(), "%lux%lu",
         &screen_settings.dmPelsWidth, &screen_settings.dmPelsHeight);
  screen_settings.dmBitsPerPel = 32;
  screen_settings.dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT;

  // Try To Set Selected Mode And Get Results.  NOTE: CDS_FULLSCREEN Gets Rid Of Start Bar.
  ChangeDisplaySettings(&screen_settings, CDS_FULLSCREEN);
#elif defined(HAVE_XRANDR) && HAVE_XRANDR
  if (m_xrr_config)
    m_xrr_config->ToggleDisplayMode(fullscreen);
#endif
}

void MainWindow::ShowRenderWidget()
{
  SetFullScreenResolution(false);
  Host::GetInstance()->SetRenderFullscreen(false);

  if (Config::Get(Config::MAIN_RENDER_TO_MAIN))
  {
    // If we're rendering to main, add it to the stack and keep the branded window title.
    m_rendering_to_main = true;

    m_stack->setCurrentIndex(m_stack->addWidget(m_render_widget));
    setWindowTitle(
        QStringLiteral("PrimedGun v%1").arg(QString::fromStdString(Common::GetScmDescStr())));
    m_stack->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_stack->repaint();

    Host::GetInstance()->SetRenderFocus(isActiveWindow());
  }
  else
  {
    // Otherwise, just show it.
    m_rendering_to_main = false;

    m_render_widget->showNormal();
    m_render_widget->restoreGeometry(m_render_widget_geometry);
  }
}

void MainWindow::HideRenderWidget(bool reinit, bool is_exit)
{
  if (m_rendering_to_main)
  {
    // Remove the widget from the stack and reparent it to nullptr, so that it can draw
    // itself in a new window if it wants. Disconnect the title updates.
    m_stack->removeWidget(m_render_widget);
    m_render_widget->setParent(nullptr);
    m_rendering_to_main = false;
    m_stack->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    setWindowTitle(
        QStringLiteral("PrimedGun v%1").arg(QString::fromStdString(Common::GetScmDescStr())));
  }

  // The following code works around a driver bug that would lead to Dolphin crashing when changing
  // graphics backends (e.g. OpenGL to Vulkan). To avoid this the render widget is (safely)
  // recreated
  if (reinit)
  {
    m_render_widget->hide();
    disconnect(m_render_widget, &RenderWidget::Closed, this, &MainWindow::ForceStop);

    m_render_widget->removeEventFilter(this);
    m_render_widget->deleteLater();

    m_render_widget = new RenderWidget;

    m_render_widget->installEventFilter(this);
    connect(m_render_widget, &RenderWidget::Closed, this, &MainWindow::ForceStop);
    connect(m_render_widget, &RenderWidget::FocusChanged, this, [this](bool focus) {
      if (m_render_widget->isFullScreen())
        SetFullScreenResolution(focus);
    });

    // The controller interface will still be registered to the old render widget, if the core
    // has booted. Therefore, we should re-bind it to the main window for now. When the core
    // is next started, it will be swapped back to the new render widget.
    g_controller_interface.ChangeWindow(::GetWindowSystemInfo(windowHandle()).render_window,
                                        is_exit ? ControllerInterface::WindowChangeReason::Exit :
                                                  ControllerInterface::WindowChangeReason::Other);
  }
}

void MainWindow::ShowControllersWindow()
{
  ShowSettingsWindow();
  m_settings_window->SelectPane(SettingsWindowPaneIndex::Controllers);
}

void MainWindow::ShowTriforceWindow()
{
  ShowSettingsWindow();
  m_settings_window->SelectPane(SettingsWindowPaneIndex::Triforce);
}

void MainWindow::ShowFreeLookWindow()
{
  if (!m_freelook_window)
  {
    m_freelook_window = new FreeLookWindow(this);
    InstallHotkeyFilter(m_freelook_window);

#ifdef USE_RETRO_ACHIEVEMENTS
    connect(m_freelook_window, &FreeLookWindow::OpenAchievementSettings, this,
            &MainWindow::ShowAchievementSettings);
#endif  // USE_RETRO_ACHIEVEMENTS
  }

  m_freelook_window->show();
  m_freelook_window->raise();
  m_freelook_window->activateWindow();
}

void MainWindow::ShowSettingsWindow()
{
  if (!m_settings_window)
  {
#ifdef HAVE_XRANDR
    if (GetWindowSystemType() == WindowSystemType::X11)
    {
      m_xrr_config = std::make_unique<X11Utils::XRRConfiguration>(
          static_cast<Display*>(QGuiApplication::platformNativeInterface()->nativeResourceForWindow(
              "display", windowHandle())),
          winId());
    }
#endif
    m_settings_window = new SettingsWindow(this);
    InstallHotkeyFilter(m_settings_window);
  }

  m_settings_window->show();
  m_settings_window->raise();
  m_settings_window->activateWindow();
}

void MainWindow::ShowAudioWindow()
{
  ShowSettingsWindow();
  m_settings_window->SelectPane(SettingsWindowPaneIndex::Audio);
}

void MainWindow::ShowGeneralWindow()
{
  ShowSettingsWindow();
  m_settings_window->SelectPane(SettingsWindowPaneIndex::General);
}

void MainWindow::ShowOSDWindow()
{
  ShowSettingsWindow();
  m_settings_window->SelectPane(SettingsWindowPaneIndex::OnScreenDisplay);
}

void MainWindow::ShowAboutDialog()
{
  AboutDialog about{this};
  about.exec();
}

void MainWindow::ShowHotkeyDialog()
{
  if (!m_hotkey_window)
  {
    m_hotkey_window = new MappingWindow(this, MappingWindow::Type::MAPPING_HOTKEYS, 0);
    InstallHotkeyFilter(m_hotkey_window);
  }

  m_hotkey_window->show();
  m_hotkey_window->raise();
  m_hotkey_window->activateWindow();
}

void MainWindow::ShowGraphicsWindow()
{
  ShowSettingsWindow();
  m_settings_window->SelectPane(SettingsWindowPaneIndex::Graphics);
}

void MainWindow::ShowVRWindow()
{
  ShowSettingsWindow();
  m_settings_window->SelectPane(SettingsWindowPaneIndex::VR);
}

void MainWindow::ShowFIFOPlayer()
{
  if (!m_fifo_window)
  {
    m_fifo_window.reset(new FIFOPlayerWindow(m_system.GetFifoPlayer(), m_system.GetFifoRecorder()));
    connect(m_fifo_window.get(), &FIFOPlayerWindow::LoadFIFORequested, this,
            [this](const QString& path) { StartGame(path, ScanForSecondDisc::No); });
  }

  m_fifo_window->show();
  m_fifo_window->raise();
  m_fifo_window->activateWindow();
}

void MainWindow::ShowSkylanderPortal()
{
  if (!m_skylander_window)
  {
    m_skylander_window = new SkylanderPortalWindow();
  }

  m_skylander_window->show();
  m_skylander_window->raise();
  m_skylander_window->activateWindow();
}

void MainWindow::ShowInfinityBase()
{
  if (!m_infinity_window)
  {
    m_infinity_window = new InfinityBaseWindow();
  }

  m_infinity_window->show();
  m_infinity_window->raise();
  m_infinity_window->activateWindow();
}

void MainWindow::ShowWiiSpeakWindow()
{
  if (!m_wii_speak_window)
  {
    m_wii_speak_window = new WiiSpeakWindow();
  }

  m_wii_speak_window->show();
  m_wii_speak_window->raise();
  m_wii_speak_window->activateWindow();
}

void MainWindow::ShowLogitechMicWindow()
{
  if (!m_logitech_mic_window)
  {
    m_logitech_mic_window = new LogitechMicWindow();
  }

  m_logitech_mic_window->show();
  m_logitech_mic_window->raise();
  m_logitech_mic_window->activateWindow();
}

void MainWindow::StateLoad()
{
  QString dialog_path = (Config::Get(Config::MAIN_CURRENT_STATE_PATH).empty()) ?
                            QDir::currentPath() :
                            QString::fromStdString(Config::Get(Config::MAIN_CURRENT_STATE_PATH));
  QString path = DolphinFileDialog::getOpenFileName(
      this, tr("Select a File"), dialog_path, tr("All Save States (*.sav *.s??);; All Files (*)"));
  Config::SetBase(Config::MAIN_CURRENT_STATE_PATH, QFileInfo(path).dir().path().toStdString());
  if (!path.isEmpty())
    State::LoadAs(m_system, path.toStdString());
}

void MainWindow::StateSave()
{
  QString dialog_path = (Config::Get(Config::MAIN_CURRENT_STATE_PATH).empty()) ?
                            QDir::currentPath() :
                            QString::fromStdString(Config::Get(Config::MAIN_CURRENT_STATE_PATH));
  QString path = DolphinFileDialog::getSaveFileName(
      this, tr("Select a File"), dialog_path, tr("All Save States (*.sav *.s??);; All Files (*)"));
  Config::SetBase(Config::MAIN_CURRENT_STATE_PATH, QFileInfo(path).dir().path().toStdString());
  if (!path.isEmpty())
    State::SaveAs(m_system, path.toStdString());
}

void MainWindow::StateLoadSlot()
{
  State::Load(m_system, m_state_slot);
}

void MainWindow::StateSaveSlot()
{
  State::Save(m_system, m_state_slot);
}

void MainWindow::StateLoadSlotAt(int slot)
{
  State::Load(m_system, slot);
}

void MainWindow::StateLoadLastSavedAt(int slot)
{
  State::LoadLastSaved(m_system, slot);
}

void MainWindow::StateSaveSlotAt(int slot)
{
  State::Save(m_system, slot);
}

void MainWindow::StateLoadUndo()
{
  State::UndoLoadState(m_system);
}

void MainWindow::StateSaveUndo()
{
  State::UndoSaveState(m_system);
}

void MainWindow::StateSaveOldest()
{
  State::SaveFirstSaved(m_system);
}

void MainWindow::SetStateSlot(int slot)
{
  Settings::Instance().SetStateSlot(slot);
  m_state_slot = slot;

  Core::DisplayMessage(fmt::format("Selected slot {} - {}", m_state_slot,
                                   State::GetInfoStringOfSlot(m_state_slot, false)),
                       2500);
}

void MainWindow::IncrementSelectedStateSlot()
{
  u32 state_slot = m_state_slot + 1;
  if (state_slot > State::NUM_STATES)
    state_slot = 1;
  m_menu_bar->SetStateSlot(state_slot);
}

void MainWindow::DecrementSelectedStateSlot()
{
  u32 state_slot = m_state_slot - 1;
  if (state_slot < 1)
    state_slot = State::NUM_STATES;
  m_menu_bar->SetStateSlot(state_slot);
}

void MainWindow::PerformOnlineUpdate(const std::string& region)
{
  WiiUpdate::PerformOnlineUpdate(region, this);
  // Since the update may have installed a newer system menu, trigger a refresh.
  Settings::Instance().NANDRefresh();
}

void MainWindow::BootWiiSystemMenu()
{
  StartGame(std::make_unique<BootParameters>(BootParameters::NANDTitle{Titles::SYSTEM_MENU}));
}

void MainWindow::UpdateScreenSaverInhibition()
{
  const bool inhibit = Config::Get(Config::MAIN_DISABLE_SCREENSAVER) &&
                       (Core::GetState(m_system) == Core::State::Running);

  if (inhibit == m_is_screensaver_inhibited)
    return;

  m_is_screensaver_inhibited = inhibit;

  UICommon::InhibitScreenSaver(inhibit);
}

bool MainWindow::eventFilter(QObject* object, QEvent* event)
{
  if (event->type() == QEvent::Wheel &&
      (qobject_cast<QSlider*>(object) || qobject_cast<QDoubleSpinBox*>(object) ||
       qobject_cast<QComboBox*>(object)))
  {
    return true;
  }

  if (event->type() == QEvent::KeyPress)
  {
    const QKeyEvent* key_event = static_cast<const QKeyEvent*>(event);
    if (key_event->key() == Qt::Key_F7 && !key_event->isAutoRepeat())
    {
      PrimedGun::PPCTrace::Toggle();
      return true;
    }
  }

  if (event->type() == QEvent::Close)
  {
    if (object == this)
    {
      if (Core::IsUninitialized(m_system))
      {
        m_exit_requested = true;
        RequestStop();
      }
      else
      {
        m_exit_requested = false;
        RequestStop();
      }
    }
    else
    {
      RequestStop();
    }

    static_cast<QCloseEvent*>(event)->ignore();
    return true;
  }

  return false;
}

QMenu* MainWindow::createPopupMenu()
{
  // Disable the default popup menu as it exposes the debugger UI even when the debugger UI is
  // disabled, which can lead to user confusion (see e.g. https://bugs.dolphin-emu.org/issues/13306)
  return nullptr;
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
  if (event->mimeData()->hasUrls() && event->mimeData()->urls().size() == 1)
    event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event)
{
  const QList<QUrl>& urls = event->mimeData()->urls();
  if (urls.empty())
    return;

  QStringList files;
  QStringList folders;

  for (const QUrl& url : urls)
  {
    QFileInfo file_info(url.toLocalFile());
    QString path = file_info.filePath();

    if (!file_info.exists() || !file_info.isReadable())
    {
      ModalMessageBox::critical(this, tr("Error"), tr("Failed to open '%1'").arg(path));
      return;
    }

    (file_info.isFile() ? files : folders).append(path);
  }

  if (!files.isEmpty())
  {
    StartGame(StringListToStdVector(files));
  }
  else
  {
    Settings& settings = Settings::Instance();
    const bool show_confirm = !settings.GetPaths().empty();

    for (const QString& folder : folders)
    {
      if (show_confirm)
      {
        if (ModalMessageBox::question(
                this, tr("Confirm"),
                tr("Do you want to add \"%1\" to the list of Game Paths?").arg(folder)) !=
            QMessageBox::Yes)
          return;
      }
      settings.AddPath(folder);
    }
  }
}

QSize MainWindow::sizeHint() const
{
  return QSize(290, 292);
}

void MainWindow::OnBootGameCubeIPL(DiscIO::Region region)
{
  StartGame(std::make_unique<BootParameters>(BootParameters::IPL{region}));
}

void MainWindow::OnImportNANDBackup()
{
  auto response = ModalMessageBox::question(
      this, tr("Question"),
      tr("Merging a new NAND over your currently selected NAND will overwrite any channels "
         "and savegames that already exist. This process is not reversible, so it is "
         "recommended that you keep backups of both NANDs. Are you sure you want to "
         "continue?"));

  if (response == QMessageBox::No)
    return;

  QString file =
      DolphinFileDialog::getOpenFileName(this, tr("Select NAND Backup"), QDir::currentPath(),
                                         tr("BootMii NAND backup file (*.bin);;"
                                            "All Files (*)"));

  if (file.isEmpty())
    return;

  ParallelProgressDialog dialog(this);
  dialog.GetRaw()->setMinimum(0);
  dialog.GetRaw()->setMaximum(0);
  dialog.GetRaw()->setLabelText(tr("Importing NAND backup"));
  dialog.GetRaw()->setCancelButton(nullptr);

  auto beginning = QDateTime::currentDateTime().toMSecsSinceEpoch();

  std::future<void> result = std::async(std::launch::async, [&] {
    DiscIO::NANDImporter().ImportNANDBin(
        file.toStdString(),
        [&dialog, beginning] {
          dialog.SetLabelText(
              tr("Importing NAND backup\n Time elapsed: %1s")
                  .arg((QDateTime::currentDateTime().toMSecsSinceEpoch() - beginning) / 1000));
        },
        [this] {
          std::optional<std::string> keys_file = RunOnObject(this, [this] {
            return DolphinFileDialog::getOpenFileName(
                       this, tr("Select Keys File (OTP/SEEPROM Dump)"), QDir::currentPath(),
                       tr("BootMii keys file (*.bin);;"
                          "All Files (*)"))
                .toStdString();
          });
          if (keys_file)
            return *keys_file;
          return std::string("");
        });
    dialog.Reset();
  });

  dialog.GetRaw()->exec();

  result.wait();

  m_menu_bar->UpdateToolsMenu(Core::State::Uninitialized);
}

void MainWindow::OnPlayRecording()
{
  if (AchievementManager::GetInstance().IsHardcoreModeActive())
  {
    ModalMessageBox::critical(
        this, tr("Error"),
        tr("Playback of input recordings is disabled in RetroAchievements hardcore mode."));
    return;
  }

  QString dtm_file = DolphinFileDialog::getOpenFileName(
      this, tr("Select the Recording File to Play"), QString(), tr("Dolphin TAS Movies (*.dtm)"));

  if (dtm_file.isEmpty())
    return;

  auto& movie = m_system.GetMovie();
  if (!movie.IsReadOnly())
  {
    // let's make the read-only flag consistent at the start of a movie.
    movie.SetReadOnly(true);
    emit ReadOnlyModeChanged(true);
  }

  std::optional<std::string> savestate_path;
  if (movie.PlayInput(dtm_file.toStdString(), &savestate_path))
  {
    emit RecordingStatusChanged(true);

    Play(savestate_path);
  }
}

void MainWindow::OnStartRecording()
{
  auto& movie = m_system.GetMovie();
  if (Core::GetState(m_system) == Core::State::Starting ||
      Core::GetState(m_system) == Core::State::Stopping || movie.IsRecordingInput() ||
      movie.IsPlayingInput())
  {
    return;
  }

  if (movie.IsReadOnly())
  {
    // The user just chose to record a movie, so that should take precedence
    movie.SetReadOnly(false);
    emit ReadOnlyModeChanged(true);
  }

  Movie::ControllerTypeArray controllers{};
  Movie::WiimoteEnabledArray wiimotes{};

  for (int i = 0; i < 4; i++)
  {
    const SerialInterface::SIDevices si_device = Config::Get(Config::GetInfoForSIDevice(i));
    if (si_device == SerialInterface::SIDEVICE_GC_GBA_EMULATED)
      controllers[i] = Movie::ControllerType::GBA;
    else if (SerialInterface::SIDevice_IsGCController(si_device))
      controllers[i] = Movie::ControllerType::GC;
    else
      controllers[i] = Movie::ControllerType::None;
    wiimotes[i] = Config::Get(Config::GetInfoForWiimoteSource(i)) != WiimoteSource::None;
  }

  if (movie.BeginRecordingInput(controllers, wiimotes))
  {
    emit RecordingStatusChanged(true);

    if (Core::IsUninitialized(m_system))
      Play();
  }
}

void MainWindow::OnStopRecording()
{
  auto& movie = m_system.GetMovie();
  if (movie.IsRecordingInput())
    OnExportRecording();
  if (movie.IsMovieActive())
    movie.EndPlayInput(false);
  emit RecordingStatusChanged(false);
}

void MainWindow::OnExportRecording()
{
  const Core::CPUThreadGuard guard(m_system);

  QString dtm_file = DolphinFileDialog::getSaveFileName(
      this, tr("Save Recording File As"), QString(), tr("Dolphin TAS Movies (*.dtm)"));
  if (!dtm_file.isEmpty())
    m_system.GetMovie().SaveRecording(dtm_file.toStdString());
}

void MainWindow::OnActivateChat()
{
}

void MainWindow::OnRequestGolfControl()
{
}

void MainWindow::ShowTASInput()
{
  for (int i = 0; i < num_gc_controllers; i++)
  {
    const auto si_device = Config::Get(Config::GetInfoForSIDevice(i));
    if (si_device == SerialInterface::SIDEVICE_GC_GBA_EMULATED)
    {
      m_gba_tas_input_windows[i]->show();
      m_gba_tas_input_windows[i]->raise();
      m_gba_tas_input_windows[i]->activateWindow();
    }
    else if (si_device != SerialInterface::SIDEVICE_NONE &&
             si_device != SerialInterface::SIDEVICE_GC_GBA)
    {
      m_gc_tas_input_windows[i]->show();
      m_gc_tas_input_windows[i]->raise();
      m_gc_tas_input_windows[i]->activateWindow();
    }
  }

  for (int i = 0; i < num_wii_controllers; i++)
  {
    const WiimoteSource source = Config::Get(Config::GetInfoForWiimoteSource(i));
    if ((source == WiimoteSource::Emulated || source == WiimoteSource::OpenXR) &&
        (!Core::IsRunning(m_system) || m_system.IsWii()))
    {
      m_wii_tas_input_windows[i]->show();
      m_wii_tas_input_windows[i]->raise();
      m_wii_tas_input_windows[i]->activateWindow();
    }
  }
}

void MainWindow::OnConnectWiiRemote(int id)
{
  const Core::CPUThreadGuard guard(m_system);
  if (const auto bt = WiiUtils::GetBluetoothEmuDevice())
  {
    const auto wm = bt->AccessWiimoteByIndex(id);
    wm->Activate(!wm->IsConnected());
  }
}

#ifdef USE_RETRO_ACHIEVEMENTS
void MainWindow::ShowAchievementsWindow()
{
  if (!m_achievements_window)
  {
    m_achievements_window = new AchievementsWindow(this);
  }

  m_achievements_window->show();
  m_achievements_window->raise();
  m_achievements_window->activateWindow();
  m_achievements_window->UpdateData(AchievementManager::UpdatedItems{.all = true});
}

void MainWindow::ShowAchievementSettings()
{
  ShowAchievementsWindow();
  m_achievements_window->ForceSettingsTab();
}

void MainWindow::OnHardcoreChanged()
{
  bool hardcore_active = AchievementManager::GetInstance().IsHardcoreModeActive();
  if (hardcore_active)
    Settings::Instance().SetDebugModeEnabled(false);
  // EmulationStateChanged causes several dialogs to redraw, including anything affected by hardcore
  // mode. Every dialog that depends on hardcore mode is redrawn by EmulationStateChanged.
  if (hardcore_active != m_former_hardcore_setting)
    emit Settings::Instance().EmulationStateChanged(Core::GetState(Core::System::GetInstance()));
  m_former_hardcore_setting = hardcore_active;
}
#endif  // USE_RETRO_ACHIEVEMENTS

void MainWindow::ShowMemcardManager()
{
  GCMemcardManager manager(this);

  manager.exec();
}

void MainWindow::ShowResourcePackManager()
{
  ResourcePackManager manager(this);

  manager.exec();
}

void MainWindow::ShowCheatsManager()
{
  if (!m_cheats_manager)
  {
    m_cheats_manager = new CheatsManager(m_system, this);

    connect(m_cheats_manager, &CheatsManager::ShowMemory, m_memory_widget,
            &MemoryWidget::SetAddress);
    connect(m_cheats_manager, &CheatsManager::RequestWatch, m_watch_widget, &WatchWidget::AddWatch);
    connect(m_cheats_manager, &CheatsManager::OpenGeneralSettings, this,
            &MainWindow::ShowGeneralWindow);

#ifdef USE_RETRO_ACHIEVEMENTS
    connect(m_cheats_manager, &CheatsManager::OpenAchievementSettings, this,
            &MainWindow::ShowAchievementSettings);
#endif  // USE_RETRO_ACHIEVEMENTS
  }

  m_cheats_manager->show();
}

void MainWindow::ShowRiivolutionBootWidget(const UICommon::GameFile& game)
{
  auto second_game = m_game_list->FindSecondDisc(game);
  std::vector<std::string> paths = {game.GetFilePath()};
  if (second_game != nullptr)
    paths.push_back(second_game->GetFilePath());
  std::unique_ptr<BootParameters> boot_params = BootParameters::GenerateFromFile(paths);
  if (!boot_params)
    return;
  if (!std::holds_alternative<BootParameters::Disc>(boot_params->parameters))
    return;

  auto& disc = std::get<BootParameters::Disc>(boot_params->parameters);
  RiivolutionBootWidget w(disc.volume->GetGameID(), disc.volume->GetRevision(),
                          disc.volume->GetDiscNumber(), game.GetFilePath(), this);

#ifdef USE_RETRO_ACHIEVEMENTS
  connect(&w, &RiivolutionBootWidget::OpenAchievementSettings, this,
          &MainWindow::ShowAchievementSettings);
#endif  // USE_RETRO_ACHIEVEMENTS

  w.exec();
  if (!w.ShouldBoot())
    return;

  AddRiivolutionPatches(boot_params.get(), std::move(w.GetPatches()));
  StartGame(std::move(boot_params));
}
