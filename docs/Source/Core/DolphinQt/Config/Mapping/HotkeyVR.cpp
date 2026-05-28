// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Config/Mapping/HotkeyVR.h"

#include <QGroupBox>
#include <QHBoxLayout>

#include "Core/HotkeyManager.h"

HotkeyVR::HotkeyVR(MappingWindow* window) : MappingWidget(window)
{
  CreateMainLayout();
}

void HotkeyVR::CreateMainLayout()
{
  m_main_layout = new QHBoxLayout();

  m_main_layout->addWidget(CreateGroupBox(tr("VR"), HotkeyManagerEmu::GetHotkeyGroup(HKGP_VR)));
  m_main_layout->addWidget(
      CreateGroupBox(tr("Shader"), HotkeyManagerEmu::GetHotkeyGroup(HKGP_VR_SHADER)));

  setLayout(m_main_layout);
}

InputConfig* HotkeyVR::GetConfig()
{
  return HotkeyManagerEmu::GetConfig();
}

void HotkeyVR::LoadSettings()
{
  HotkeyManagerEmu::LoadConfig();
}

void HotkeyVR::SaveSettings()
{
  HotkeyManagerEmu::GetConfig()->SaveConfig();
}
