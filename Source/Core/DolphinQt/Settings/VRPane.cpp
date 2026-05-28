// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Settings/VRPane.h"

#include <QGroupBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QAbstractSlider>
#include <QTabWidget>
#include <QVBoxLayout>

#include "Common/Config/Config.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/System.h"

#include "DolphinQt/Config/HideObjectAddEditDialog.h"
#include "DolphinQt/Config/ConfigControls/ConfigBool.h"
#include "DolphinQt/Config/ConfigControls/ConfigChoice.h"
#include "DolphinQt/Config/ConfigControls/ConfigFloatSlider.h"
#include "DolphinQt/Config/ConfigControls/ConfigSlider.h"
#include "DolphinQt/Debugger/CullingCodeFinderWidget.h"
#include "DolphinQt/Debugger/ShaderHunterWidget.h"
#include "DolphinQt/Settings.h"
#include "VideoCommon/HideObjectEngine.h"
#include "VideoCommon/VideoConfig.h"

VRPane::VRPane(QWidget* parent) : QWidget(parent)
{
  auto* main_layout = new QVBoxLayout;
  auto* tabs = new QTabWidget(this);
  auto* general_tab = new QWidget(this);
  auto* general_layout = new QVBoxLayout;
  auto* hack_tab = new QWidget(this);
  auto* hack_layout = new QVBoxLayout;
  auto* tools_tab = new QWidget(this);
  auto* tools_tab_layout = new QVBoxLayout;

  general_tab->setLayout(general_layout);
  hack_tab->setLayout(hack_layout);
  tools_tab->setLayout(tools_tab_layout);
  tabs->addTab(general_tab, tr("General"));
  tabs->addTab(hack_tab, tr("Hacks"));
  tabs->addTab(tools_tab, tr("Tools"));

  auto* openxr_group = new QGroupBox(tr("OpenXR"));
  auto* openxr_layout = new QGridLayout;
  openxr_group->setLayout(openxr_layout);
  auto* camera_group = new QGroupBox(tr("Camera"));
  auto* camera_layout = new QGridLayout;
  camera_group->setLayout(camera_layout);
  auto* virtual_screen_group = new QGroupBox(tr("Virtual Screen"));
  auto* virtual_screen_layout = new QGridLayout;
  virtual_screen_group->setLayout(virtual_screen_layout);
  auto* framerate_group = new QGroupBox(tr("Framerate"));
  auto* framerate_layout = new QGridLayout;
  framerate_group->setLayout(framerate_layout);
  auto* rendering_group = new QGroupBox(tr("Rendering"));
  auto* rendering_layout = new QGridLayout;
  rendering_group->setLayout(rendering_layout);
  auto* ar_mode_group = new QGroupBox(tr("AR Mode"));
  auto* ar_mode_layout = new QGridLayout;
  ar_mode_group->setLayout(ar_mode_layout);

  m_enable_openxr = new ConfigBool(tr("Enable VR"), Config::GFX_VR_ENABLE_OPENXR);
  m_reference_space_mode = new ConfigChoiceMap<OpenXRReferenceSpaceMode>(
      {{tr("LOCAL"), OpenXRReferenceSpaceMode::Local},
       {tr("STAGE + Height"), OpenXRReferenceSpaceMode::StageHeight},
       {tr("STAGE"), OpenXRReferenceSpaceMode::Stage}},
      Config::GFX_VR_REFERENCE_SPACE_MODE);
  m_tracking_mode = new ConfigChoiceMap<OpenXRTrackingMode>(
      {{tr("6DoF"), OpenXRTrackingMode::Full6DoF},
       {tr("3DoF"), OpenXRTrackingMode::Rotation3DoF},
       {tr("None"), OpenXRTrackingMode::None}},
      Config::GFX_VR_TRACKING_MODE);
  m_use_vulkan_multiview = new ConfigBool(tr("Use Vulkan Multiview"),
                                          Config::GFX_VR_USE_VULKAN_MULTIVIEW);
  m_auto_immediate_xfb =
      new ConfigBool(tr("Force Immediately Present XFB"),
                     Config::GFX_VR_AUTO_IMMEDIATE_XFB);
  m_units_per_meter = new ConfigFloatSlider(Config::GFX_VR_UNITS_PER_METER_MIN,
                                            Config::GFX_VR_UNITS_PER_METER_MAX,
                                            Config::GFX_VR_UNITS_PER_METER,
                                            Config::GFX_VR_UNITS_PER_METER_STEP);
  m_units_per_meter_value = new QLabel();
  m_lean_back_angle = new ConfigFloatSlider(Config::GFX_VR_LEAN_BACK_ANGLE_MIN,
                                            Config::GFX_VR_LEAN_BACK_ANGLE_MAX,
                                            Config::GFX_VR_LEAN_BACK_ANGLE,
                                            Config::GFX_VR_LEAN_BACK_ANGLE_STEP);
  m_lean_back_angle_value = new QLabel();
  m_camera_forward = new ConfigFloatSlider(Config::GFX_VR_CAMERA_FORWARD_MIN,
                                           Config::GFX_VR_CAMERA_FORWARD_MAX,
                                           Config::GFX_VR_CAMERA_FORWARD,
                                           Config::GFX_VR_CAMERA_FORWARD_STEP);
  m_camera_forward_value = new QLabel();
  m_camera_height = new ConfigFloatSlider(Config::GFX_VR_CAMERA_HEIGHT_MIN,
                                          Config::GFX_VR_CAMERA_HEIGHT_MAX,
                                          Config::GFX_VR_CAMERA_HEIGHT,
                                          Config::GFX_VR_CAMERA_HEIGHT_STEP);
  m_camera_height_value = new QLabel();

  openxr_layout->addWidget(m_enable_openxr, 0, 0, 1, 3);

  m_ar_mode = new ConfigBool(tr("AR Mode (Passthrough)"), Config::GFX_VR_AR_MODE);

  m_ar_background_alpha =
      new ConfigFloatSlider(Config::GFX_VR_AR_BACKGROUND_ALPHA_MIN,
                            Config::GFX_VR_AR_BACKGROUND_ALPHA_MAX,
                            Config::GFX_VR_AR_BACKGROUND_ALPHA,
                            Config::GFX_VR_AR_BACKGROUND_ALPHA_STEP);
  m_ar_background_alpha_value = new QLabel();
  m_ar_background_alpha_value->setText(
      QString::asprintf("%.2f", m_ar_background_alpha->GetValue()));
  connect(m_ar_background_alpha, &ConfigFloatSlider::valueChanged, this, [this] {
    m_ar_background_alpha_value->setText(
        QString::asprintf("%.2f", m_ar_background_alpha->GetValue()));
  });

  openxr_layout->addWidget(new ConfigFloatLabel(tr("Units per Meter:"), m_units_per_meter), 1, 0);
  openxr_layout->addWidget(m_units_per_meter, 1, 1);
  openxr_layout->addWidget(m_units_per_meter_value, 1, 2);

  m_mirror_view = new ConfigChoiceMap<OpenXRMirrorView>(
      {{tr("Both Eyes"), OpenXRMirrorView::BothEyes},
       {tr("Left Eye"), OpenXRMirrorView::LeftEye},
       {tr("Right Eye"), OpenXRMirrorView::RightEye},
       {tr("None"), OpenXRMirrorView::None}},
      Config::GFX_VR_MIRROR_VIEW);
  openxr_layout->addWidget(new QLabel(tr("Desktop Mirror View:")), 2, 0);
  openxr_layout->addWidget(m_mirror_view, 2, 1, 1, 2);

  camera_layout->addWidget(new ConfigFloatLabel(tr("Lean Back Angle (deg):"), m_lean_back_angle), 0,
                           0);
  camera_layout->addWidget(m_lean_back_angle, 0, 1);
  camera_layout->addWidget(m_lean_back_angle_value, 0, 2);
  camera_layout->addWidget(new ConfigFloatLabel(tr("Camera Forward (m):"), m_camera_forward), 1, 0);
  camera_layout->addWidget(m_camera_forward, 1, 1);
  camera_layout->addWidget(m_camera_forward_value, 1, 2);
  camera_layout->addWidget(new ConfigFloatLabel(tr("Camera Height (m):"), m_camera_height), 2, 0);
  camera_layout->addWidget(m_camera_height, 2, 1);
  camera_layout->addWidget(m_camera_height_value, 2, 2);

  m_units_per_meter_value->setText(QString::asprintf("%.1f", m_units_per_meter->GetValue()));
  connect(m_units_per_meter, &ConfigFloatSlider::valueChanged, this, [this] {
    m_units_per_meter_value->setText(QString::asprintf("%.1f", m_units_per_meter->GetValue()));
  });
  m_lean_back_angle_value->setText(QString::asprintf("%.1f", m_lean_back_angle->GetValue()));
  connect(m_lean_back_angle, &ConfigFloatSlider::valueChanged, this, [this] {
    m_lean_back_angle_value->setText(QString::asprintf("%.1f", m_lean_back_angle->GetValue()));
  });
  m_camera_forward_value->setText(QString::asprintf("%.1f", m_camera_forward->GetValue()));
  connect(m_camera_forward, &ConfigFloatSlider::valueChanged, this, [this] {
    m_camera_forward_value->setText(QString::asprintf("%.1f", m_camera_forward->GetValue()));
  });
  connect(m_camera_forward, &QAbstractSlider::actionTriggered, this, [](int) {
    Config::SetBaseOrCurrent(Config::GFX_VR_ENABLE_CAMERA_FORWARD, true);
  });
  m_camera_height_value->setText(QString::asprintf("%.1f", m_camera_height->GetValue()));
  connect(m_camera_height, &ConfigFloatSlider::valueChanged, this, [this] {
    m_camera_height_value->setText(QString::asprintf("%.1f", m_camera_height->GetValue()));
  });
  connect(m_camera_height, &QAbstractSlider::actionTriggered, this, [](int) {
    Config::SetBaseOrCurrent(Config::GFX_VR_ENABLE_CAMERA_HEIGHT, true);
  });

  // Virtual screen settings
  m_virtual_screen =
      new ConfigBool(tr("Virtual Screen for 2D Content"), Config::GFX_VR_VIRTUAL_SCREEN);
  virtual_screen_layout->addWidget(m_virtual_screen, 0, 0, 1, 3);

  m_screen_distance = new ConfigFloatSlider(Config::GFX_VR_SCREEN_DISTANCE_MIN,
                                            Config::GFX_VR_SCREEN_DISTANCE_MAX,
                                            Config::GFX_VR_SCREEN_DISTANCE,
                                            Config::GFX_VR_SCREEN_DISTANCE_STEP);
  m_screen_distance_value = new QLabel();
  m_screen_size = new ConfigFloatSlider(Config::GFX_VR_SCREEN_SIZE_MIN,
                                        Config::GFX_VR_SCREEN_SIZE_MAX,
                                        Config::GFX_VR_SCREEN_SIZE,
                                        Config::GFX_VR_SCREEN_SIZE_STEP);
  m_screen_size_value = new QLabel();
  m_head_locked_curvature =
      new ConfigFloatSlider(Config::GFX_VR_HEAD_LOCKED_CURVATURE_MIN,
                            Config::GFX_VR_HEAD_LOCKED_CURVATURE_MAX,
                            Config::GFX_VR_HEAD_LOCKED_CURVATURE,
                            Config::GFX_VR_HEAD_LOCKED_CURVATURE_STEP);
  m_head_locked_curvature_value = new QLabel();

  virtual_screen_layout->addWidget(
      new ConfigFloatLabel(tr("Screen Distance (m):"), m_screen_distance), 1, 0);
  virtual_screen_layout->addWidget(m_screen_distance, 1, 1);
  virtual_screen_layout->addWidget(m_screen_distance_value, 1, 2);
  virtual_screen_layout->addWidget(new ConfigFloatLabel(tr("Screen Size (m):"), m_screen_size), 2,
                                   0);
  virtual_screen_layout->addWidget(m_screen_size, 2, 1);
  virtual_screen_layout->addWidget(m_screen_size_value, 2, 2);
  virtual_screen_layout->addWidget(
      new ConfigFloatLabel(tr("Head Locked Curvature:"), m_head_locked_curvature), 3, 0);
  virtual_screen_layout->addWidget(m_head_locked_curvature, 3, 1);
  virtual_screen_layout->addWidget(m_head_locked_curvature_value, 3, 2);

  m_screen_distance_value->setText(QString::asprintf("%.1f", m_screen_distance->GetValue()));
  connect(m_screen_distance, &ConfigFloatSlider::valueChanged, this, [this] {
    m_screen_distance_value->setText(QString::asprintf("%.1f", m_screen_distance->GetValue()));
  });
  m_screen_size_value->setText(QString::asprintf("%.1f", m_screen_size->GetValue()));
  connect(m_screen_size, &ConfigFloatSlider::valueChanged, this, [this] {
    m_screen_size_value->setText(QString::asprintf("%.1f", m_screen_size->GetValue()));
  });
  m_head_locked_curvature_value->setText(
      QString::asprintf("%.2f", m_head_locked_curvature->GetValue()));
  connect(m_head_locked_curvature, &ConfigFloatSlider::valueChanged, this, [this] {
    m_head_locked_curvature_value->setText(
        QString::asprintf("%.2f", m_head_locked_curvature->GetValue()));
  });

  m_opcode_replay_mode = new ConfigChoiceMap<OpenXROpcodeReplayMode>(
      {{tr("Off"), OpenXROpcodeReplayMode::Off},
       {tr("25 Hz Input"), OpenXROpcodeReplayMode::Input25Hz},
       {tr("30 Hz Input"), OpenXROpcodeReplayMode::Input30Hz},
       {tr("50 Hz Input"), OpenXROpcodeReplayMode::Input50Hz},
       {tr("60 Hz Input"), OpenXROpcodeReplayMode::Input60Hz}},
      Config::GFX_VR_OPCODE_REPLAY);
  framerate_layout->addWidget(new QLabel(tr("Opcode Replay Input:")), 0, 0);
  framerate_layout->addWidget(m_opcode_replay_mode, 0, 1, 1, 2);

  m_replay_refresh_rate = new ConfigChoiceMap<int>(
      {{tr("Auto"), Config::GFX_VR_OPCODE_REPLAY_TARGET_REFRESH_RATE_AUTO},
       {tr("72 Hz"), Config::GFX_VR_OPCODE_REPLAY_TARGET_REFRESH_RATE_72},
       {tr("90 Hz"), Config::GFX_VR_OPCODE_REPLAY_TARGET_REFRESH_RATE_90},
       {tr("120 Hz"), Config::GFX_VR_OPCODE_REPLAY_TARGET_REFRESH_RATE_120}},
      Config::GFX_VR_OPCODE_REPLAY_TARGET_REFRESH_RATE);
  framerate_layout->addWidget(new QLabel(tr("Replay Refresh Rate:")), 1, 0);
  framerate_layout->addWidget(m_replay_refresh_rate, 1, 1, 1, 2);
  auto update_replay_refresh_rate_enabled = [this] {
    m_replay_refresh_rate->setEnabled(m_opcode_replay_mode->currentIndex() > 0);
  };
  update_replay_refresh_rate_enabled();
  connect(m_opcode_replay_mode, &QComboBox::currentIndexChanged, this,
          update_replay_refresh_rate_enabled);

  m_forced_vbi_frequency = new ConfigChoiceMap<int>(
      {{tr("Auto"), Config::GFX_VR_FORCED_VBI_FREQUENCY_AUTO},
       {tr("Off"), Config::GFX_VR_FORCED_VBI_FREQUENCY_OFF},
       {tr("72 Hz"), Config::GFX_VR_FORCED_VBI_FREQUENCY_72},
       {tr("90 Hz"), Config::GFX_VR_FORCED_VBI_FREQUENCY_90},
       {tr("120 Hz"), Config::GFX_VR_FORCED_VBI_FREQUENCY_120}},
      Config::GFX_VR_FORCED_VBI_FREQUENCY);
  framerate_layout->addWidget(new QLabel(tr("Forced VBI Frequency:")), 2, 0);
  framerate_layout->addWidget(m_forced_vbi_frequency, 2, 1, 1, 2);
  connect(m_forced_vbi_frequency, &QComboBox::currentIndexChanged, this,
          [](int) { Config::SetBaseOrCurrent(Config::GFX_VR_AUTO_VBI_FROM_HMD, false); });

  m_clear_efb_slider = new ConfigSlider(Config::GFX_VR_CLEAR_EFB_MIN,
                                        Config::GFX_VR_CLEAR_EFB_MAX,
                                        Config::GFX_VR_CLEAR_EFB_COPIES,
                                        Config::GFX_VR_CLEAR_EFB_STEP);
  m_clear_efb_value = new QLabel();
  rendering_layout->addWidget(
      new ConfigSliderLabel(tr("Clear EFB (min width):"), m_clear_efb_slider), 0, 0);
  rendering_layout->addWidget(m_clear_efb_slider, 0, 1);
  rendering_layout->addWidget(m_clear_efb_value, 0, 2);

  auto update_clear_efb_label = [this] {
    const int val = m_clear_efb_slider->value();
    m_clear_efb_value->setText(val == 0 ? tr("Off") : QString::number(val));
  };
  update_clear_efb_label();
  connect(m_clear_efb_slider, &ConfigSlider::valueChanged, this, update_clear_efb_label);

  // VR Gamma
  m_vr_gamma = new ConfigFloatSlider(Config::GFX_VR_GAMMA_MIN, Config::GFX_VR_GAMMA_MAX,
                                     Config::GFX_VR_GAMMA, Config::GFX_VR_GAMMA_STEP);
  m_vr_gamma_value = new QLabel();
  rendering_layout->addWidget(new ConfigFloatLabel(tr("VR Gamma:"), m_vr_gamma), 1, 0);
  rendering_layout->addWidget(m_vr_gamma, 1, 1);
  rendering_layout->addWidget(m_vr_gamma_value, 1, 2);

  auto update_gamma_label = [this] {
    const float val = m_vr_gamma->GetValue();
    m_vr_gamma_value->setText(val <= 1.01f ? tr("Off") : QString::asprintf("%.1f", val));
  };
  update_gamma_label();
  connect(m_vr_gamma, &ConfigFloatSlider::valueChanged, this, update_gamma_label);

  // Auto Layer Spread
  m_auto_layer_spread =
      new ConfigBool(tr("Auto Layer Spread"), Config::GFX_VR_AUTO_LAYER_SPREAD);
  virtual_screen_layout->addWidget(m_auto_layer_spread, 4, 0, 1, 3);

  m_layer_offset = new ConfigFloatSlider(Config::GFX_VR_LAYER_OFFSET_MIN,
                                          Config::GFX_VR_LAYER_OFFSET_MAX,
                                          Config::GFX_VR_LAYER_OFFSET,
                                          Config::GFX_VR_LAYER_OFFSET_STEP);
  m_layer_offset_value = new QLabel();
  virtual_screen_layout->addWidget(new ConfigFloatLabel(tr("Layer Offset:"), m_layer_offset), 5,
                                   0);
  virtual_screen_layout->addWidget(m_layer_offset, 5, 1);
  virtual_screen_layout->addWidget(m_layer_offset_value, 5, 2);

  m_layer_offset_value->setText(QString::asprintf("%.4f", m_layer_offset->GetValue()));
  connect(m_layer_offset, &ConfigFloatSlider::valueChanged, this, [this] {
    m_layer_offset_value->setText(QString::asprintf("%.4f", m_layer_offset->GetValue()));
  });

  m_element_depth = new ConfigFloatSlider(Config::GFX_VR_ELEMENT_DEPTH_MIN,
                                           Config::GFX_VR_ELEMENT_DEPTH_MAX,
                                           Config::GFX_VR_ELEMENT_DEPTH,
                                           Config::GFX_VR_ELEMENT_DEPTH_STEP);
  m_element_depth_value = new QLabel();
  virtual_screen_layout->addWidget(new ConfigFloatLabel(tr("Element Depth:"), m_element_depth), 6,
                                   0);
  virtual_screen_layout->addWidget(m_element_depth, 6, 1);
  virtual_screen_layout->addWidget(m_element_depth_value, 6, 2);

  m_element_depth_value->setText(QString::asprintf("%.4f", m_element_depth->GetValue()));
  connect(m_element_depth, &ConfigFloatSlider::valueChanged, this, [this] {
    m_element_depth_value->setText(QString::asprintf("%.4f", m_element_depth->GetValue()));
  });

  // 3D HUD: toggle + depth slider for Screen/HeadLocked perspective overrides
  m_hud_3d_enable = new ConfigBool(tr("3D HUD (Preserve Depth)"), Config::GFX_VR_HUD_3D_ENABLE);
  virtual_screen_layout->addWidget(m_hud_3d_enable, 7, 0, 1, 3);

  m_hud_3d_closer = new ConfigFloatSlider(Config::GFX_VR_HUD_3D_CLOSER_MIN,
                                           Config::GFX_VR_HUD_3D_CLOSER_MAX,
                                           Config::GFX_VR_HUD_3D_CLOSER,
                                           Config::GFX_VR_HUD_3D_CLOSER_STEP);
  m_hud_3d_closer_value = new QLabel();
  virtual_screen_layout->addWidget(new ConfigFloatLabel(tr("3D HUD Depth:"), m_hud_3d_closer), 8,
                                   0);
  virtual_screen_layout->addWidget(m_hud_3d_closer, 8, 1);
  virtual_screen_layout->addWidget(m_hud_3d_closer_value, 8, 2);

  m_hud_3d_closer_value->setText(QString::asprintf("%.2f", m_hud_3d_closer->GetValue()));
  connect(m_hud_3d_closer, &ConfigFloatSlider::valueChanged, this, [this] {
    m_hud_3d_closer_value->setText(QString::asprintf("%.2f", m_hud_3d_closer->GetValue()));
  });

  general_layout->addWidget(openxr_group);
  general_layout->addWidget(camera_group);
  general_layout->addWidget(virtual_screen_group);
  general_layout->addWidget(framerate_group);
  general_layout->addWidget(rendering_group);
  auto* general_actions_layout = new QHBoxLayout;
  general_actions_layout->addStretch();
  m_reset_general_settings = new QPushButton(tr("Reset Settings"));
  connect(m_reset_general_settings, &QPushButton::clicked, this, &VRPane::ResetGeneralSettings);
  general_actions_layout->addWidget(m_reset_general_settings);
  general_layout->addLayout(general_actions_layout);

  auto* hacks_group = new QGroupBox(tr("VR Hacks"));
  auto* hacks_group_layout = new QVBoxLayout;
  hacks_group->setLayout(hacks_group_layout);

  m_dont_clear_screen =
      new ConfigBool(tr("Don't Clear Screen"), Config::GFX_VR_DONT_CLEAR_SCREEN);
  m_disable_cpu_cull =
      new ConfigBool(tr("Disable CPU Culling in VR"), Config::GFX_VR_DISABLE_CPU_CULL);
  m_remove_bars = new ConfigBool(tr("Remove Cinematic Bars"), Config::GFX_VR_REMOVE_BARS);
  m_ortho_scissor_fix =
      new ConfigBool(tr("Ortho Scissor Fix"), Config::GFX_VR_ORTHO_SCISSOR_FIX);
  m_metroid_visor_fix =
      new ConfigBool(tr("Metroid Prime Visor Fix"), Config::GFX_VR_METROID_VISOR_FIX);
  m_metroid_visor_fix->setToolTip(
      tr("Enables per-eye thermal/X-ray visor rendering in Metroid Prime via layered "
         "palette conversion and a D3D-specific fullscreen-mono fallback. Disable if "
         "non-Metroid titles regress (e.g. Mario Kart Wii effects appearing twice or "
         "Vulkan device-lost crashes)."));
  m_lock_head_pose =
      new ConfigBool(tr("Lock Head Pose Per Frame"), Config::GFX_VR_LOCK_HEAD_POSE);
  m_ar_mode_debug =
      new ConfigBool(tr("Fake AR Mode (Debug)"), Config::GFX_VR_AR_MODE_DEBUG);

  hacks_group_layout->addWidget(m_use_vulkan_multiview);
  hacks_group_layout->addWidget(m_auto_immediate_xfb);
  hacks_group_layout->addWidget(m_lock_head_pose);
  hacks_group_layout->addWidget(m_dont_clear_screen);
  hacks_group_layout->addWidget(m_disable_cpu_cull);
  hacks_group_layout->addWidget(m_remove_bars);
  hacks_group_layout->addWidget(m_ortho_scissor_fix);
  hacks_group_layout->addWidget(m_metroid_visor_fix);
  hack_layout->addWidget(hacks_group);

  ar_mode_layout->addWidget(m_ar_mode, 0, 0, 1, 3);
  ar_mode_layout->addWidget(
      new ConfigFloatLabel(tr("AR Background Alpha:"), m_ar_background_alpha), 1, 0);
  ar_mode_layout->addWidget(m_ar_background_alpha, 1, 1);
  ar_mode_layout->addWidget(m_ar_background_alpha_value, 1, 2);
  ar_mode_layout->addWidget(m_ar_mode_debug, 2, 0, 1, 3);
  hack_layout->addWidget(ar_mode_group);

  // Lock Head Pose is incompatible with Immediately Present XFB — when enabled,
  // force-disable both Auto-enable Immediate XFB (local) and the Graphics Hacks
  // Immediately Present XFB config key, and grey them out.
  const auto update_lock_head_pose = [this] {
    const bool locked = m_lock_head_pose->isChecked();
    m_auto_immediate_xfb->setDisabled(locked);
    if (locked)
    {
      Config::SetBaseOrCurrent(Config::GFX_VR_AUTO_IMMEDIATE_XFB, false);
      Config::SetBaseOrCurrent(Config::GFX_HACK_IMMEDIATE_XFB, false);
    }
  };
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
  connect(m_lock_head_pose, &QCheckBox::checkStateChanged, this,
          [update_lock_head_pose](Qt::CheckState) { update_lock_head_pose(); });
#else
  connect(m_lock_head_pose, &QCheckBox::stateChanged, this,
          [update_lock_head_pose](int) { update_lock_head_pose(); });
#endif
  update_lock_head_pose();  // Apply initial state

  auto* tools_group = new QGroupBox(tr("Tools"));
  auto* tools_layout = new QGridLayout;
  tools_group->setLayout(tools_layout);

  auto* add_hide_object_code_btn = new QPushButton(tr("Add Hide Object Code"));
  connect(add_hide_object_code_btn, &QPushButton::clicked, this, [this] {
    const std::string game_id = SConfig::GetInstance().GetGameID();
    if (game_id.empty() || game_id == "00000000")
    {
      QMessageBox::information(this, tr("No Game Running"),
                               tr("Start a game before adding Hide Object codes."));
      return;
    }

    auto codes = HideObjectEngine::LoadFromINI(game_id);
    HideObjectAddEditDialog dialog(this, nullptr, codes);
    if (dialog.exec() == QDialog::Accepted)
    {
      codes.push_back(dialog.GetResult());
      HideObjectEngine::SaveToINI(game_id, codes);
      HideObjectEngine::Engine::GetInstance().ApplyCodes(codes);
    }
    else
    {
      // Restore the active code set if temporary brute-force values were applied.
      HideObjectEngine::Engine::GetInstance().ApplyCodes(codes);
    }
  });
  tools_layout->addWidget(add_hide_object_code_btn, 0, 0);

  auto* shader_hunter_btn = new QPushButton(tr("Shader Hunter"));
  connect(shader_hunter_btn, &QPushButton::clicked, this, [this] {
    const std::string game_id = SConfig::GetInstance().GetGameID();
    if (game_id.empty() || game_id == "00000000")
    {
      QMessageBox::information(this, tr("No Game Running"),
                               tr("Start a game before searching for shaders."));
      return;
    }

    if (m_shader_hunter_widget)
    {
      m_shader_hunter_widget->show();
      m_shader_hunter_widget->raise();
      m_shader_hunter_widget->activateWindow();
      return;
    }

    m_shader_hunter_widget = new ShaderHunterWidget(this);
    m_shader_hunter_widget->setAttribute(Qt::WA_DeleteOnClose, true);
    m_shader_hunter_widget->show();
    connect(m_shader_hunter_widget, &QObject::destroyed, this,
            [this] { m_shader_hunter_widget = nullptr; });
  });
  tools_layout->addWidget(shader_hunter_btn, 1, 0);

  auto* culling_finder_btn = new QPushButton(tr("Culling Code Finder"));
  connect(culling_finder_btn, &QPushButton::clicked, this, [this] {
    const std::string game_id = SConfig::GetInstance().GetGameID();
    if (game_id.empty() || game_id == "00000000")
    {
      QMessageBox::information(this, tr("No Game Running"),
                               tr("Start a game before searching for culling codes."));
      return;
    }

    if (m_culling_finder_widget)
    {
      m_culling_finder_widget->show();
      m_culling_finder_widget->raise();
      m_culling_finder_widget->activateWindow();
      return;
    }

    m_culling_finder_widget = new CullingCodeFinderWidget(this);
    m_culling_finder_widget->setAttribute(Qt::WA_DeleteOnClose, true);
    m_culling_finder_widget->show();
    connect(m_culling_finder_widget, &QObject::destroyed, this,
            [this] { m_culling_finder_widget = nullptr; });
  });
  tools_layout->addWidget(culling_finder_btn, 2, 0);

  m_load_custom_shaders =
      new ConfigBool(tr("Load Custom Shaders"), Config::GFX_VR_LOAD_CUSTOM_SHADERS);
  tools_layout->addWidget(m_load_custom_shaders, 3, 0);

  m_enable_openxr_config_scene = new ConfigBool(
      tr("Enable OpenXR Config Scene For Controller Binding"),
      Config::GFX_VR_ENABLE_OPENXR_CONFIG_SCENE);
  tools_layout->addWidget(m_enable_openxr_config_scene, 4, 0);

  tools_layout->addWidget(new QLabel(tr("Default VR Position:")), 5, 0);
  tools_layout->addWidget(m_reference_space_mode, 5, 1);
  tools_layout->addWidget(new QLabel(tr("Tracking Mode:")), 6, 0);
  tools_layout->addWidget(m_tracking_mode, 6, 1);

  general_layout->addStretch();
  hack_layout->addStretch();
  tools_tab_layout->addWidget(tools_group);
  tools_tab_layout->addStretch();

  main_layout->addWidget(tabs);

  setLayout(main_layout);

  AddDescriptions();

  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this,
          &VRPane::OnEmulationStateChanged);
  OnEmulationStateChanged(Core::GetState(Core::System::GetInstance()));
}

void VRPane::AddDescriptions()
{
  static constexpr char TR_ENABLE_OPENXR_DESCRIPTION[] = QT_TR_NOOP(
      "Enables VR rendering through OpenXR."
      "<br><br>When enabled, Dolphin uses OpenXR instead of Stereoscopic 3D output modes."
      "<br><br>This setting cannot be changed while emulation is active."
      "<br><br><dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
  static constexpr char TR_USE_VULKAN_MULTIVIEW_DESCRIPTION[] = QT_TR_NOOP(
      "Uses Vulkan multiview for OpenXR stereo rendering when the GPU and runtime support it."
      "<br><br>This renders both eyes through a multiview render pass instead of using "
      "geometry-shader stereo expansion, which can be much faster on Quest/Adreno."
      "<br><br>This setting only affects the Vulkan backend and requires restarting emulation."
      "<br><br><dolphin_emphasis>If unsure on Quest, leave this checked.</dolphin_emphasis>");
  static constexpr char TR_AUTO_IMMEDIATE_XFB_DESCRIPTION[] = QT_TR_NOOP(
      "Forces Immediately Present XFB on while OpenXR is enabled."
      "<br><br>This improves head-tracking responsiveness and reduces latency in VR by presenting "
      "each XFB copy as soon as it is created."
      "<br><br>Turn this off if a game flickers with Immediately Present XFB, then use "
      "<b>Lock Head Pose Per Frame</b> instead to keep head-tracking coherent."
      "<br><br><dolphin_emphasis>If unsure, leave this checked.</dolphin_emphasis>");
  static constexpr char TR_UNITS_PER_METER_DESCRIPTION[] = QT_TR_NOOP(
      "Sets how many game world units correspond to one real-world meter."
      "<br><br>Higher values increase stereo scale and make the world appear smaller."
      "<br><br>Lower values decrease stereo scale and make the world appear larger.");
  static constexpr char TR_REFERENCE_SPACE_MODE_DESCRIPTION[] = QT_TR_NOOP(
      "Selects how Dolphin sets the default VR position when OpenXR starts."
      "<br><br>LOCAL uses OpenXR <code>LOCAL</code> space and keeps the current behavior: Dolphin "
      "sets home from the initial headset position."
      "<br><br>STAGE + Height uses OpenXR <code>STAGE</code> space, keeps the play-space center "
      "for horizontal position, and offsets height by the initial headset height."
      "<br><br>STAGE uses OpenXR <code>STAGE</code> space and uses the play-space origin directly."
      "<br><br>If <code>STAGE</code> is unavailable, Dolphin falls back to <code>LOCAL</code>."
      "<br><br>This setting requires restarting emulation."
      "<br><br><dolphin_emphasis>If unsure, use LOCAL.</dolphin_emphasis>");
  static constexpr char TR_TRACKING_MODE_DESCRIPTION[] = QT_TR_NOOP(
      "Selects how OpenXR headset tracking affects the VR camera."
      "<br><br>6DoF uses full rotation and position tracking."
      "<br><br>3DoF uses head rotation tracking only and ignores headset position movement."
      "<br><br>None disables both head rotation and position tracking."
      "<br><br>This only affects the emulated VR camera; OpenXR still receives frames normally."
      "<br><br><dolphin_emphasis>If unsure, use 6DoF.</dolphin_emphasis>");
  static constexpr char TR_LEAN_BACK_ANGLE_DESCRIPTION[] = QT_TR_NOOP(
      "Applies a constant pitch offset (in degrees) to the VR camera."
      "<br><br>Use this to compensate games where the default viewpoint feels tilted."
      "<br><br>Positive values lean the camera back, negative values lean it forward.");
  static constexpr char TR_CAMERA_FORWARD_DESCRIPTION[] = QT_TR_NOOP(
      "Applies a constant forward/backward offset (in meters) to the VR camera."
      "<br><br>Positive values move the camera forward, negative values move it backward.");
  static constexpr char TR_CAMERA_HEIGHT_DESCRIPTION[] = QT_TR_NOOP(
      "Applies a constant vertical offset (in meters) to the VR camera."
      "<br><br>Positive values move the camera up, negative values move it down.");
  static constexpr char TR_VIRTUAL_SCREEN_DESCRIPTION[] = QT_TR_NOOP(
      "Places 2D content (menus, FMV, in-game HUD) on a virtual screen floating in 3D space."
      "<br><br>Disable for games that rely on full-screen EFB effects that don't work with a virtual screen.");
  static constexpr char TR_SCREEN_DISTANCE_DESCRIPTION[] = QT_TR_NOOP(
      "Distance in meters to the virtual screen used for 2D content (menus, FMV, HUD)."
      "<br><br>The screen is fixed in space like a TV — it stays in place when you turn your head.");
  static constexpr char TR_SCREEN_SIZE_DESCRIPTION[] = QT_TR_NOOP(
      "Height in meters of the virtual screen used for 2D content."
      "<br><br>Larger values make the screen bigger, smaller values make it more compact.");
  static constexpr char TR_HEAD_LOCKED_CURVATURE_DESCRIPTION[] = QT_TR_NOOP(
      "Curves the virtual screen used by Head Locked shader overrides."
      "<br><br>0 keeps the screen flat. Higher values increase curvature."
      "<br><br>This only affects shaders using Head Locked handling.");
  static constexpr char TR_DONT_CLEAR_SCREEN_DESCRIPTION[] = QT_TR_NOOP(
      "Prevents clearing the VR eye framebuffers before rendering each frame."
      "<br><br>By default the screen is cleared to black, which prevents image trails when "
      "looking outside the rendered area. Enable this if clearing causes visual glitches "
      "in specific games."
      "<br><br><dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
  static constexpr char TR_DISABLE_CPU_CULL_DESCRIPTION[] = QT_TR_NOOP(
      "Disables CPU-side primitive culling when OpenXR VR is active."
      "<br><br>This may fix missing geometry in some games at the cost of a small performance hit."
      "<br><br>This only affects Dolphin's CPU culling optimization. It does not override "
      "the game's own backface culling state.");
  static constexpr char TR_OPCODE_REPLAY_DESCRIPTION[] = QT_TR_NOOP(
      "Selects the expected input cadence for Opcode Replay."
      "<br><br>25 Hz and 50 Hz Input are intended for PAL games. 30 Hz and 60 Hz Input are "
      "intended for NTSC games."
      "<br><br>Opcode Replay submits extra synthetic OpenXR frames between real emulation frames "
      "to better match the selected Replay Refresh Rate."
      "<br><br>This only affects OpenXR HMD output. The desktop mirror stays at the real game "
      "frame cadence."
      "<br><br><dolphin_emphasis>If unsure, leave this set to Off.</dolphin_emphasis>");
  static constexpr char TR_REPLAY_REFRESH_RATE_DESCRIPTION[] = QT_TR_NOOP(
      "Selects the headset refresh rate target for Opcode Replay."
      "<br><br>Auto uses the current HMD refresh rate, choosing the closest supported value: "
      "72, 90, or 120 Hz."
      "<br><br>For explicit 72 Hz, 90 Hz, and 120 Hz selections, Dolphin requests that OpenXR "
      "display refresh rate from the runtime when Opcode Replay is enabled. Replay only arms "
      "when the active OpenXR display period matches the selected target."
      "<br><br>This covers 25/30/50/60 Hz input to 72/90/120 Hz replay targets."
      "<br><br>If the runtime does not support display refresh requests, Dolphin logs a warning "
      "and continues using the current headset refresh rate."
      "<br><br><dolphin_emphasis>If unsure, use Auto.</dolphin_emphasis>");
  static constexpr char TR_MIRROR_VIEW_DESCRIPTION[] = QT_TR_NOOP(
      "Selects what the desktop render window shows while OpenXR is active."
      "<br><br>Both Eyes shows the current side-by-side mirror. Left Eye and Right Eye fill the "
      "window with a single eye. None leaves the desktop window blank while continuing to render "
      "normally to the headset."
      "<br><br>This only affects the desktop mirror view; it does not affect the OpenXR headset "
      "output.");
  static constexpr char TR_FORCED_VBI_FREQUENCY_DESCRIPTION[] = QT_TR_NOOP(
      "Forces Dolphin's VBI frequency to the selected rate while OpenXR VR is enabled."
      "<br><br>Auto samples the headset refresh rate once at OpenXR session startup and uses "
      "the closest supported value: 72, 90, or 120 Hz."
      "<br><br>This overrides the Advanced tab's VBI percentage during VR sessions."
      "<br><br><dolphin_emphasis>If unsure, leave this set to Off.</dolphin_emphasis>");
  static constexpr char TR_AUTO_LAYER_SPREAD_DESCRIPTION[] = QT_TR_NOOP(
      "Automatically spreads 2D elements (menus, HUD, text) on the virtual screen into separate "
      "depth layers to prevent Z-fighting."
      "<br><br>Each ortho draw call gets a unique layer index, with later draws closer to the "
      "camera. Disable this if you prefer manual layer control via Shader Overrides."
      "<br><br><dolphin_emphasis>If unsure, leave this checked.</dolphin_emphasis>");
  static constexpr char TR_LAYER_OFFSET_DESCRIPTION[] = QT_TR_NOOP(
      "Controls the depth separation between consecutive 2D layers on the virtual screen."
      "<br><br>Higher values spread layers further apart (stronger Z-fighting fix but may look "
      "wrong at extreme values). Lower values keep layers closer together."
      "<br><br>Default: 0.0020");
  static constexpr char TR_ELEMENT_DEPTH_DESCRIPTION[] = QT_TR_NOOP(
      "Controls the depth range within individual 2D elements on the virtual screen."
      "<br><br>Higher values give more depth separation within each element, fixing Z-fighting "
      "that appears as flickering inside UI elements. Set to 0 to flatten elements completely."
      "<br><br>Default: 0.0010");
  static constexpr char TR_HUD_3D_ENABLE_DESCRIPTION[] = QT_TR_NOOP(
      "Preserves the internal 3D depth of Screen / HeadLocked shader overrides when the source "
      "draw uses a perspective projection (e.g. Metroid Prime's 3D helmet/visor/reticle)."
      "<br><br>When off, perspective HUDs marked Screen/HeadLocked get flattened onto the "
      "virtual screen plane (correct for flat 2D HUDs, causes Z-fighting for 3D meshes). When "
      "on, the depth spread is widened so the original Z-ordering of the 3D mesh shows through."
      "<br><br>2D (orthographic) HUDs are unaffected — they have no internal Z variation, "
      "so the toggle only changes how perspective draws look."
      "<br><br><dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
  static constexpr char TR_HUD_3D_CLOSER_DESCRIPTION[] = QT_TR_NOOP(
      "Controls the depth thickness (in meters) of 3D HUD geometry on the virtual screen."
      "<br><br>0.0 = flat (same as toggle off); 1.0 = ±1.0 m of HMD-space depth around "
      "the virtual screen plane. Reference: Hydra used 0.5 m for Metroid Prime. Combine with "
      "a closer Screen Distance (1.0–1.5 m) for stronger stereoscopic effect on small HUD "
      "elements. Only applies when 3D HUD (Preserve Depth) is enabled."
      "<br><br>Default: 0.50");
  static constexpr char TR_CLEAR_EFB_COPIES_DESCRIPTION[] = QT_TR_NOOP(
      "Clears EFB (Embedded Framebuffer) copy textures to transparent when the copy's native "
      "width is at or above this threshold. Set to 0 to disable."
      "<br><br>Full-screen post-processing effects (bloom, blur) typically use full EFB width "
      "(640). HUD and small UI elements use smaller partial copies. Increasing this value "
      "preserves smaller EFB copies (like HUD textures) while removing large full-screen effects."
      "<br><br>Recommended: 400-640 to clear only full-screen effects, or 0 to disable."
      "<br><br><dolphin_emphasis>If unsure, leave this at 0 (off).</dolphin_emphasis>");
  static constexpr char TR_REMOVE_BARS_DESCRIPTION[] = QT_TR_NOOP(
      "Expands the scissor and viewport to fill the full active rendering area for perspective "
      "draws, removing cinematic letterbox bars."
      "<br><br>Some games (e.g. Metroid Prime) use GX scissor clipping to create cinematic "
      "bars that mask the top and bottom of the screen. In VR these bars are distracting. "
      "This option removes them by expanding the rendered area."
      "<br><br>Disable this if it causes visual artifacts in a specific game."
      "<br><br><dolphin_emphasis>If unsure, leave this checked.</dolphin_emphasis>");
  static constexpr char TR_ORTHO_SCISSOR_FIX_DESCRIPTION[] = QT_TR_NOOP(
      "Expands the scissor to the full active rendering area for orthographic VR draws."
      "<br><br>This fixes 2D content that is reprojected onto the VR virtual screen but still "
      "uses game-side EFB scissor coordinates, such as the item roulette in Mario Kart: "
      "Double Dash!!."
      "<br><br>Disable this if a game needs its original orthographic scissor clipping."
      "<br><br><dolphin_emphasis>If unsure, leave this checked.</dolphin_emphasis>");
  static constexpr char TR_LOCK_HEAD_POSE_DESCRIPTION[] = QT_TR_NOOP(
      "Snaps OpenXR head-tracking updates to game-frame boundaries (XFB copies) so every draw "
      "call within a single game frame uses one consistent head pose."
      "<br><br>Without this, when <b>Immediately Present XFB</b> is disabled, "
      "<code>LocateViews()</code> can mutate the head pose mid-frame on the video thread, "
      "causing different parts of the same frame (e.g. level geometry vs. dynamic objects) "
      "to be rendered with different poses. The visible symptom is objects appearing to "
      "&quot;stay in place&quot; and jump to their correct positions a few frames later."
      "<br><br>This option captures a single head pose at the XFB-copy point in FIFO order, "
      "guaranteeing all draws of a frame are coherent. It has no effect when "
      "<b>Immediately Present XFB</b> is on (that path is already coherent)."
      "<br><br><dolphin_emphasis>If unsure, leave this checked.</dolphin_emphasis>");
  static constexpr char TR_VR_GAMMA_DESCRIPTION[] = QT_TR_NOOP(
      "Adjusts gamma correction for VR eye output."
      "<br><br>1.0 = no correction (default). 2.2 = standard sRGB gamma. "
      "Higher values darken the image further."
      "<br><br>Some headsets expect sRGB-encoded content and appear "
      "too bright without correction. Adjust this slider until the brightness matches "
      "the desktop mirror."
      "<br><br><dolphin_emphasis>If unsure, leave this at 1.0 (Off).</dolphin_emphasis>");
  static constexpr char TR_LOAD_CUSTOM_SHADERS_DESCRIPTION[] = QT_TR_NOOP(
      "Loads custom shader replacements from the Load/Shaders/&lt;GameID&gt;/ directory."
      "<br><br>Place edited shader files (dumped via Shader Hunter) in this folder to override "
      "the game's original shaders. Files must be named &lt;hash&gt;-&lt;vs|ps|gs&gt;.txt."
      "<br><br><dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
  static constexpr char TR_ENABLE_OPENXR_CONFIG_SCENE_DESCRIPTION[] = QT_TR_NOOP(
      "Starts a temporary OpenXR scene while configuring an <b>OpenXR Wii Remote</b> with no "
      "game running."
      "<br><br>This keeps OpenXR controller input active so the mapping window can detect button "
      "presses and motion input outside emulation."
      "<br><br>If the temporary session cannot start, Dolphin falls back to the desktop-only "
      "mapping window."
      "<br><br>Disable this if your runtime or headset has trouble with the temporary binding "
      "scene."
      "<br><br><dolphin_emphasis>If unsure, leave this checked.</dolphin_emphasis>");
  static constexpr char TR_AR_MODE_DEBUG_DESCRIPTION[] = QT_TR_NOOP(
      "Debug aid for AR Mode development on headsets that do <b>not</b> support passthrough."
      "<br><br>Clears the per-eye OpenXR swapchain to <b>solid magenta</b> instead of "
      "transparent black, so the regions that <i>would</i> be see-through in real AR Mode "
      "become clearly visible. The OpenXR composition state is left as <code>OPAQUE</code>, "
      "so this works on any headset and never triggers "
      "<code>XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED</code>."
      "<br><br>Use this to verify that the eye framebuffer clear path, layer flag wiring, "
      "and game-geometry-on-empty-background composition all behave correctly without "
      "needing a passthrough-capable runtime."
      "<br><br><dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
  static constexpr char TR_AR_MODE_DESCRIPTION[] = QT_TR_NOOP(
      "Submits the OpenXR projection layer with alpha-blended composition so that empty "
      "(transparent) pixels reveal the headset passthrough feed, making game geometry float "
      "over the real world."
      "<br><br>Requires a headset and runtime that support "
      "<code>XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND</code> (e.g. Quest 3, Vive XR Elite, "
      "Varjo, HoloLens). On opaque-only PCVR headsets (Index, Vive Pro) enabling this will "
      "cause <code>xrEndFrame</code> to fail; disable it again if the VR session stops "
      "submitting frames."
      "<br><br><dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");

  m_enable_openxr->SetDescription(tr(TR_ENABLE_OPENXR_DESCRIPTION));
  m_use_vulkan_multiview->SetDescription(tr(TR_USE_VULKAN_MULTIVIEW_DESCRIPTION));
  m_auto_immediate_xfb->SetDescription(tr(TR_AUTO_IMMEDIATE_XFB_DESCRIPTION));
  m_reference_space_mode->SetDescription(tr(TR_REFERENCE_SPACE_MODE_DESCRIPTION));
  m_tracking_mode->SetDescription(tr(TR_TRACKING_MODE_DESCRIPTION));
  m_units_per_meter->SetDescription(tr(TR_UNITS_PER_METER_DESCRIPTION));
  m_lean_back_angle->SetDescription(tr(TR_LEAN_BACK_ANGLE_DESCRIPTION));
  m_camera_forward->SetDescription(tr(TR_CAMERA_FORWARD_DESCRIPTION));
  m_camera_height->SetDescription(tr(TR_CAMERA_HEIGHT_DESCRIPTION));
  m_virtual_screen->SetDescription(tr(TR_VIRTUAL_SCREEN_DESCRIPTION));
  m_screen_distance->SetDescription(tr(TR_SCREEN_DISTANCE_DESCRIPTION));
  m_screen_size->SetDescription(tr(TR_SCREEN_SIZE_DESCRIPTION));
  m_head_locked_curvature->SetDescription(tr(TR_HEAD_LOCKED_CURVATURE_DESCRIPTION));
  m_dont_clear_screen->SetDescription(tr(TR_DONT_CLEAR_SCREEN_DESCRIPTION));
  m_disable_cpu_cull->SetDescription(tr(TR_DISABLE_CPU_CULL_DESCRIPTION));
  m_opcode_replay_mode->SetDescription(tr(TR_OPCODE_REPLAY_DESCRIPTION));
  m_mirror_view->SetDescription(tr(TR_MIRROR_VIEW_DESCRIPTION));
  m_replay_refresh_rate->SetDescription(tr(TR_REPLAY_REFRESH_RATE_DESCRIPTION));
  m_forced_vbi_frequency->SetDescription(tr(TR_FORCED_VBI_FREQUENCY_DESCRIPTION));
  m_clear_efb_slider->SetDescription(tr(TR_CLEAR_EFB_COPIES_DESCRIPTION));
  m_remove_bars->SetDescription(tr(TR_REMOVE_BARS_DESCRIPTION));
  m_ortho_scissor_fix->SetDescription(tr(TR_ORTHO_SCISSOR_FIX_DESCRIPTION));
  m_lock_head_pose->SetDescription(tr(TR_LOCK_HEAD_POSE_DESCRIPTION));
  m_vr_gamma->SetDescription(tr(TR_VR_GAMMA_DESCRIPTION));
  m_auto_layer_spread->SetDescription(tr(TR_AUTO_LAYER_SPREAD_DESCRIPTION));
  m_layer_offset->SetDescription(tr(TR_LAYER_OFFSET_DESCRIPTION));
  m_element_depth->SetDescription(tr(TR_ELEMENT_DEPTH_DESCRIPTION));
  m_hud_3d_enable->SetDescription(tr(TR_HUD_3D_ENABLE_DESCRIPTION));
  m_hud_3d_closer->SetDescription(tr(TR_HUD_3D_CLOSER_DESCRIPTION));
  m_load_custom_shaders->SetDescription(tr(TR_LOAD_CUSTOM_SHADERS_DESCRIPTION));
  m_enable_openxr_config_scene->SetDescription(tr(TR_ENABLE_OPENXR_CONFIG_SCENE_DESCRIPTION));
  m_ar_mode->SetDescription(tr(TR_AR_MODE_DESCRIPTION));
  m_ar_mode_debug->SetDescription(tr(TR_AR_MODE_DEBUG_DESCRIPTION));
}

void VRPane::OnEmulationStateChanged(Core::State state)
{
  const bool running = state != Core::State::Uninitialized;
  m_enable_openxr->setEnabled(!running);
  m_reference_space_mode->setEnabled(!running);
  m_use_vulkan_multiview->setEnabled(!running);
  m_reset_general_settings->setEnabled(!running);
}

void VRPane::ResetGeneralSettings()
{
  Config::ConfigChangeCallbackGuard guard;

  Config::SetBaseOrCurrent(Config::GFX_VR_ENABLE_OPENXR,
                           Config::GFX_VR_ENABLE_OPENXR.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_REFERENCE_SPACE_MODE,
                           Config::GFX_VR_REFERENCE_SPACE_MODE.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_TRACKING_MODE,
                           Config::GFX_VR_TRACKING_MODE.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_UNITS_PER_METER,
                           Config::GFX_VR_UNITS_PER_METER.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_LEAN_BACK_ANGLE,
                           Config::GFX_VR_LEAN_BACK_ANGLE.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_ENABLE_CAMERA_FORWARD,
                           Config::GFX_VR_ENABLE_CAMERA_FORWARD.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_CAMERA_FORWARD,
                           Config::GFX_VR_CAMERA_FORWARD.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_ENABLE_CAMERA_HEIGHT,
                           Config::GFX_VR_ENABLE_CAMERA_HEIGHT.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_CAMERA_HEIGHT,
                           Config::GFX_VR_CAMERA_HEIGHT.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_VIRTUAL_SCREEN,
                           Config::GFX_VR_VIRTUAL_SCREEN.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_SCREEN_DISTANCE,
                           Config::GFX_VR_SCREEN_DISTANCE.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_SCREEN_SIZE,
                           Config::GFX_VR_SCREEN_SIZE.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_HEAD_LOCKED_CURVATURE,
                           Config::GFX_VR_HEAD_LOCKED_CURVATURE.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_OPCODE_REPLAY,
                           Config::GFX_VR_OPCODE_REPLAY.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_MIRROR_VIEW,
                           Config::GFX_VR_MIRROR_VIEW.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_OPCODE_REPLAY_TARGET_REFRESH_RATE,
                           Config::GFX_VR_OPCODE_REPLAY_TARGET_REFRESH_RATE.GetDefaultValue());
  m_replay_refresh_rate->setEnabled(false);
  Config::SetBaseOrCurrent(Config::GFX_VR_FORCED_VBI_FREQUENCY,
                           Config::GFX_VR_FORCED_VBI_FREQUENCY.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_AUTO_VBI_FROM_HMD,
                           Config::GFX_VR_AUTO_VBI_FROM_HMD.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_CLEAR_EFB_COPIES,
                           Config::GFX_VR_CLEAR_EFB_COPIES.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_GAMMA, Config::GFX_VR_GAMMA.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_AUTO_LAYER_SPREAD,
                           Config::GFX_VR_AUTO_LAYER_SPREAD.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_LAYER_OFFSET,
                           Config::GFX_VR_LAYER_OFFSET.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_ELEMENT_DEPTH,
                           Config::GFX_VR_ELEMENT_DEPTH.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_HUD_3D_ENABLE,
                           Config::GFX_VR_HUD_3D_ENABLE.GetDefaultValue());
  Config::SetBaseOrCurrent(Config::GFX_VR_HUD_3D_CLOSER,
                           Config::GFX_VR_HUD_3D_CLOSER.GetDefaultValue());
}
