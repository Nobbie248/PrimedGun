// Copyright 2026 PrimedGun Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"

namespace PrimedGun
{
// Driven by NTSC VI fields, not rendered frames (which stop during the hang).
class FrameCompletionWatchdog
{
public:
  enum class Action
  {
    None,
    Retry,
    Pause
  };

  Action Update(bool waiting, u32 game_frame)
  {
    if (!waiting || !m_tracking || game_frame != m_game_frame)
    {
      m_tracking = waiting;
      m_game_frame = game_frame;
      m_stalled_fields = 0;
      return Action::None;
    }

    // Stay paused on resume until completion actually progresses. Repeated resumes
    // must not run down the remaining time on Prime's ten-second fatal watchdog.
    if (m_stalled_fields < 360)
      ++m_stalled_fields;
    if (m_stalled_fields == 360)
      return Action::Pause;
    if (m_stalled_fields >= 120 && m_stalled_fields % 60 == 0)
      return Action::Retry;
    return Action::None;
  }

  bool IsStalled() const { return m_stalled_fields != 0; }

private:
  bool m_tracking = false;
  u32 m_game_frame = 0;
  u32 m_stalled_fields = 0;
};
}  // namespace PrimedGun
