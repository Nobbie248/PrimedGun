// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef ENABLE_VR

#include "VideoCommon/VR/OpenXRUtilitySession.h"

#include "Common/Logging/Log.h"

namespace VR
{
OpenXRUtilitySession::~OpenXRUtilitySession()
{
  Stop();
}

bool OpenXRUtilitySession::Start()
{
  WARN_LOG_FMT(VIDEO, "OpenXR utility config scene is not available in this build.");
  m_running = false;
  return false;
}

void OpenXRUtilitySession::Stop()
{
  m_running = false;
}

bool OpenXRUtilitySession::IsRunning() const
{
  return m_running;
}

void OpenXRUtilitySession::SetOverlayImage(const std::vector<u8>& rgba, int width, int height)
{
}
}  // namespace VR

#endif
