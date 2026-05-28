// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Config/Mapping/OpenXRWiimoteConfigSessionController.h"

#include <cstring>

#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <QPen>
#include <QTimer>

#include <array>
#include <string>
#include <vector>

#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/Core.h"
#include "Core/HW/Wiimote.h"
#include "Core/System.h"
#include "DolphinQt/Config/Mapping/MappingWindow.h"
#include "DolphinQt/Settings.h"
#if defined(ENABLE_VR) && defined(_WIN32)
#include "Common/VR/OpenXRInputState.h"
#include "VideoCommon/VR/OpenXRManager.h"
#include "VideoCommon/VR/OpenXRUtilitySession.h"
#endif

namespace
{
constexpr int OVERLAY_WIDTH = 1024;
constexpr int OVERLAY_HEIGHT = 704;

#if defined(ENABLE_VR) && defined(_WIN32)
QString BoolToDigit(bool value)
{
  return value ? QStringLiteral("1") : QStringLiteral("0");
}

QString BuildDigitalStateLine1(const Common::VR::OpenXRControllerState& controller)
{
  return QObject::tr("Primary: %1   Secondary: %2   Menu: %3")
      .arg(BoolToDigit(controller.primary_button), BoolToDigit(controller.secondary_button),
           BoolToDigit(controller.menu_button));
}

QString BuildDigitalStateLine2(const Common::VR::OpenXRControllerState& controller)
{
  return QObject::tr("Trigger Btn: %1   Grip Btn: %2   Stick Btn: %3")
      .arg(BoolToDigit(controller.trigger_button), BoolToDigit(controller.squeeze_button),
           BoolToDigit(controller.thumbstick_button));
}

QString BuildAnalogStateLine(const Common::VR::OpenXRControllerState& controller)
{
  return QObject::tr("Trigger: %1   Grip: %2   Force: %3   Stick: (%4, %5)")
      .arg(QString::number(controller.trigger_value, 'f', 2),
           QString::number(controller.squeeze_value, 'f', 2),
           QString::number(controller.squeeze_force, 'f', 2),
           QString::number(controller.thumbstick_x, 'f', 2),
           QString::number(controller.thumbstick_y, 'f', 2));
}

std::vector<u8> RenderOverlayImage(int port, const Common::VR::OpenXRInputSnapshot& snapshot)
{
  QImage image(OVERLAY_WIDTH, OVERLAY_HEIGHT, QImage::Format_RGBA8888);
  image.fill(qRgba(0, 0, 0, 0));

  QPainter painter(&image);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setRenderHint(QPainter::TextAntialiasing, true);

  const QRect background_rect(24, 24, OVERLAY_WIDTH - 48, OVERLAY_HEIGHT - 48);
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(18, 22, 28, 220));
  painter.drawRoundedRect(background_rect, 24, 24);

  QFont title_font = painter.font();
  title_font.setPointSize(28);
  title_font.setBold(true);
  painter.setFont(title_font);
  painter.setPen(QColor(245, 247, 250));
  painter.drawText(QRect(56, 54, OVERLAY_WIDTH - 112, 50), Qt::AlignLeft | Qt::AlignVCenter,
                   QObject::tr("OpenXR Wii Remote Binding"));

  QFont subtitle_font = painter.font();
  subtitle_font.setPointSize(18);
  subtitle_font.setBold(false);
  painter.setFont(subtitle_font);
  painter.setPen(QColor(180, 188, 198));
  painter.drawText(QRect(56, 114, OVERLAY_WIDTH - 112, 36), Qt::AlignLeft | Qt::AlignVCenter,
                   QObject::tr("Wii Remote %1").arg(port + 1));

  painter.drawText(QRect(56, 164, OVERLAY_WIDTH - 112, 36), Qt::AlignLeft | Qt::AlignVCenter,
                   QObject::tr("Use the desktop window to bind controls"));

  const std::array<QString, 2> labels = {QObject::tr("Left Controller"),
                                         QObject::tr("Right Controller")};

  QFont status_font = painter.font();
  status_font.setPointSize(17);
  painter.setFont(status_font);

  for (int i = 0; i < 2; ++i)
  {
    const auto& controller = snapshot.controllers[i];
    const int top = 236 + i * 168;
    const QRect row_rect(56, top, OVERLAY_WIDTH - 112, 144);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(32, 38, 46, 210));
    painter.drawRoundedRect(row_rect, 16, 16);

    painter.setBrush(controller.connected ? QColor(44, 182, 125) : QColor(196, 84, 84));
    painter.drawEllipse(QPoint(92, top + 28), 10, 10);

    painter.setPen(QColor(238, 240, 243));
    painter.drawText(QRect(120, top + 2, 280, 52), Qt::AlignLeft | Qt::AlignVCenter, labels[i]);

    painter.setPen(controller.connected ? QColor(129, 228, 180) : QColor(246, 151, 151));
    painter.drawText(QRect(OVERLAY_WIDTH - 300, top + 2, 220, 52),
                     Qt::AlignRight | Qt::AlignVCenter,
                     controller.connected ? QObject::tr("Connected") : QObject::tr("Waiting"));

    QFont details_font = painter.font();
    details_font.setPointSize(13);
    painter.setFont(details_font);
    painter.setPen(QColor(208, 214, 221));
    painter.drawText(QRect(88, top + 52, OVERLAY_WIDTH - 176, 24), Qt::AlignLeft | Qt::AlignVCenter,
                     BuildDigitalStateLine1(controller));
    painter.drawText(QRect(88, top + 82, OVERLAY_WIDTH - 176, 24), Qt::AlignLeft | Qt::AlignVCenter,
                     BuildDigitalStateLine2(controller));
    painter.setPen(QColor(162, 171, 182));
    painter.drawText(QRect(88, top + 112, OVERLAY_WIDTH - 176, 24), Qt::AlignLeft | Qt::AlignVCenter,
                     BuildAnalogStateLine(controller));

    painter.setFont(status_font);
  }

  painter.setPen(QColor(140, 148, 160));
  painter.drawText(QRect(56, OVERLAY_HEIGHT - 60, OVERLAY_WIDTH - 112, 30),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   snapshot.runtime_active ? QObject::tr("OpenXR runtime active")
                                           : QObject::tr("Waiting for OpenXR runtime"));

  painter.end();

  std::vector<u8> pixels(static_cast<size_t>(image.sizeInBytes()));
  std::memcpy(pixels.data(), image.constBits(), pixels.size());
  return pixels;
}
#endif
}  // namespace

OpenXRWiimoteConfigSessionController::OpenXRWiimoteConfigSessionController(MappingWindow* window,
                                                                           int port)
    : QObject(window), m_window(window), m_port(port), m_overlay_timer(new QTimer(this))
{
  m_overlay_timer->setInterval(100);

  connect(m_overlay_timer, &QTimer::timeout, this,
          &OpenXRWiimoteConfigSessionController::UpdateOverlay);
  connect(window, &QDialog::finished, this, &OpenXRWiimoteConfigSessionController::Stop);
  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this,
          [this](Core::State state) {
            if (state != Core::State::Uninitialized)
              Stop();
          });

  QTimer::singleShot(0, this, &OpenXRWiimoteConfigSessionController::TryStart);
}

OpenXRWiimoteConfigSessionController::~OpenXRWiimoteConfigSessionController()
{
  Stop();
}

void OpenXRWiimoteConfigSessionController::TryStart()
{
#if defined(ENABLE_VR) && defined(_WIN32)
  if (m_session)
    return;

  if (Core::GetState(Core::System::GetInstance()) != Core::State::Uninitialized)
    return;

  if (!Config::Get(Config::GFX_VR_ENABLE_OPENXR_CONFIG_SCENE))
    return;

  if (Config::Get(Config::GetInfoForWiimoteSource(m_port)) != WiimoteSource::OpenXR)
    return;

  if (VR::g_openxr)
    return;

  m_session = std::make_unique<VR::OpenXRUtilitySession>();
  if (!m_session->Start())
  {
    m_session.reset();
    return;
  }

  UpdateOverlay();
  m_overlay_timer->start();
#endif
}

void OpenXRWiimoteConfigSessionController::Stop()
{
  m_overlay_timer->stop();

#if defined(ENABLE_VR) && defined(_WIN32)
  if (m_session)
  {
    m_session->Stop();
    m_session.reset();
  }
#endif
}

void OpenXRWiimoteConfigSessionController::UpdateOverlay()
{
#if defined(ENABLE_VR) && defined(_WIN32)
  if (!m_session)
    return;

  if (!m_session->IsRunning())
  {
    Stop();
    return;
  }

  const Common::VR::OpenXRInputSnapshot snapshot = Common::VR::OpenXRInputState::GetSnapshot();
  m_session->SetOverlayImage(RenderOverlayImage(m_port, snapshot), OVERLAY_WIDTH, OVERLAY_HEIGHT);
#endif
}
