// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include <QObject>

namespace VR
{
#if defined(ENABLE_VR) && defined(_WIN32)
class OpenXRUtilitySession;
#endif
}

class MappingWindow;
class QTimer;

class OpenXRWiimoteConfigSessionController final : public QObject
{
  Q_OBJECT

public:
  OpenXRWiimoteConfigSessionController(MappingWindow* window, int port);
  ~OpenXRWiimoteConfigSessionController() override;

private:
  void TryStart();
  void Stop();
  void UpdateOverlay();

  MappingWindow* const m_window;
  const int m_port;
  QTimer* const m_overlay_timer;
#if defined(ENABLE_VR) && defined(_WIN32)
  std::unique_ptr<VR::OpenXRUtilitySession> m_session;
#endif
};
