// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef ENABLE_VR

#include <vector>

#include "Common/CommonTypes.h"

namespace VR
{
class OpenXRUtilitySession
{
public:
  OpenXRUtilitySession() = default;
  ~OpenXRUtilitySession();

  bool Start();
  void Stop();
  bool IsRunning() const;
  void SetOverlayImage(const std::vector<u8>& rgba, int width, int height);

private:
  bool m_running = false;
};
}  // namespace VR

#endif
