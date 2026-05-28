// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Settings/VRPane.h"

#include <QGroupBox>
#include <QGridLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include "Core/Config/GraphicsSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/System.h"

#include "DolphinQt/Config/HideObjectAddEditDialog.h"
#include "DolphinQt/Config/ConfigControls/ConfigBool.h"
#include "DolphinQt/Config/ConfigControls/ConfigFloatSlider.h"
#include "DolphinQt/Config/ConfigControls/ConfigSlider.h"
#include "DolphinQt/Debugger/ShaderHunterWidget.h"
#include "DolphinQt/Settings.h"
#include "VideoCommon/HideObjectEngine.h"

VRPane::VRPane(QWidget* parent) : QWidget(parent)
{
  auto* main_layout = new QVBoxLayout;

  auto* openxr_group = new QGroupBox(tr("OpenXR"));
  auto* openxr_layout = new QGridLayout;
  openxr_group->setLayout(openxr_layout);

  m_enable_openxr = new ConfigBool(tr("Enable OpenXR"), Config::GFX_VR_ENABLE_OPENXR);
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

  openxr_layout->addWidget(m_enable_openxr, 0, 0, 1, 3);
  openxr_layout->addWidget(new ConfigFloatLabel(tr("Units per Meter:"), m_units_per_meter), 1, 0);
  openxr_layout->addWidget(m_units_per_meter, 1, 1);
  openxr_layout->addWidget(m_units_per_meter_value, 1, 2);
  openxr_layout->addWidget(new ConfigFloatLabel(tr("Lean Back Angle (deg):"), m_lean_back_angle), 2,
                           0);
  openxr_layout->addWidget(m_lean_back_angle, 2, 1);
  openxr_layout->addWidget(m_lean_back_angle_value, 2, 2);
  openxr_layout->addWidget(new ConfigFloatLabel(tr("Camera Forward (m):"), m_camera_forward), 3, 0);
  openxr_layout->addWidget(m_camera_forward, 3, 1);
  openxr_layout->addWidget(m_camera_forward_value, 3, 2);

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

  // Virtual screen settings
  m_virtual_screen = new ConfigBool(tr("Virtual Screen for 2D Content"), Config::GFX_VR_VIRTUAL_SCREEN);
  openxr_layout->addWidget(m_virtual_screen, 4, 0, 1, 3);

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

  openxr_layout->addWidget(new ConfigFloatLabel(tr("Screen Distance (m):"), m_screen_distance), 5, 0);
  openxr_layout->addWidget(m_screen_distance, 5, 1);
  openxr_layout->addWidget(m_screen_distance_value, 5, 2);
  openxr_layout->addWidget(new ConfigFloatLabel(tr("Screen Size (m):"), m_screen_size), 6, 0);
  openxr_layout->addWidget(m_screen_size, 6, 1);
  openxr_layout->addWidget(m_screen_size_value, 6, 2);
  openxr_layout->addWidget(
      new ConfigFloatLabel(tr("Head Locked Curvature:"), m_head_locked_curvature), 7, 0);
  openxr_layout->addWidget(m_head_locked_curvature, 7, 1);
  openxr_layout->addWidget(m_head_locked_curvature_value, 7, 2);

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

  m_dont_clear_screen =
      new ConfigBool(tr("Don't Clear Screen"), Config::GFX_VR_DONT_CLEAR_SCREEN);
  openxr_layout->addWidget(m_dont_clear_screen, 8, 0, 1, 3);

  m_disable_cpu_cull =
      new ConfigBool(tr("Disable CPU Culling in VR"), Config::GFX_VR_DISABLE_CPU_CULL);
  openxr_layout->addWidget(m_disable_cpu_cull, 9, 0, 1, 3);

  m_auto_vbi_from_hmd = new ConfigBool(tr("Force VBI Frequency to 90 Hz in VR"),
                                       Config::GFX_VR_AUTO_VBI_FROM_HMD);
  openxr_layout->addWidget(m_auto_vbi_from_hmd, 10, 0, 1, 3);

  m_clear_efb_slider = new ConfigSlider(Config::GFX_VR_CLEAR_EFB_MIN,
                                        Config::GFX_VR_CLEAR_EFB_MAX,
                                        Config::GFX_VR_CLEAR_EFB_COPIES,
                                        Config::GFX_VR_CLEAR_EFB_STEP);
  m_clear_efb_value = new QLabel();
  openxr_layout->addWidget(new ConfigSliderLabel(tr("Clear EFB (min width):"), m_clear_efb_slider),
                           11, 0);
  openxr_layout->addWidget(m_clear_efb_slider, 11, 1);
  openxr_layout->addWidget(m_clear_efb_value, 11, 2);

  auto update_clear_efb_label = [this] {
    const int val = m_clear_efb_slider->value();
    m_clear_efb_value->setText(val == 0 ? tr("Off") : QString::number(val));
  };
  update_clear_efb_label();
  connect(m_clear_efb_slider, &ConfigSlider::valueChanged, this, update_clear_efb_label);

  // Remove Cinematic Bars
  m_remove_bars = new ConfigBool(tr("Remove Cinematic Bars"), Config::GFX_VR_REMOVE_BARS);
  openxr_layout->addWidget(m_remove_bars, 12, 0, 1, 3);

  m_metroid_visor_fix =
      new ConfigBool(tr("Metroid Prime Visor Fix"), Config::GFX_VR_METROID_VISOR_FIX);
  openxr_layout->addWidget(m_metroid_visor_fix, 13, 0, 1, 3);

  // Auto Layer Spread
  m_auto_layer_spread =
      new ConfigBool(tr("Auto Layer Spread"), Config::GFX_VR_AUTO_LAYER_SPREAD);
  openxr_layout->addWidget(m_auto_layer_spread, 14, 0, 1, 3);

  m_layer_offset = new ConfigFloatSlider(Config::GFX_VR_LAYER_OFFSET_MIN,
                                          Config::GFX_VR_LAYER_OFFSET_MAX,
                                          Config::GFX_VR_LAYER_OFFSET,
                                          Config::GFX_VR_LAYER_OFFSET_STEP);
  m_layer_offset_value = new QLabel();
  openxr_layout->addWidget(new ConfigFloatLabel(tr("Layer Offset:"), m_layer_offset), 15, 0);
  openxr_layout->addWidget(m_layer_offset, 15, 1);
  openxr_layout->addWidget(m_layer_offset_value, 15, 2);

  m_layer_offset_value->setText(QString::asprintf("%.4f", m_layer_offset->GetValue()));
  connect(m_layer_offset, &ConfigFloatSlider::valueChanged, this, [this] {
    m_layer_offset_value->setText(QString::asprintf("%.4f", m_layer_offset->GetValue()));
  });

  m_element_depth = new ConfigFloatSlider(Config::GFX_VR_ELEMENT_DEPTH_MIN,
                                           Config::GFX_VR_ELEMENT_DEPTH_MAX,
                                           Config::GFX_VR_ELEMENT_DEPTH,
                                           Config::GFX_VR_ELEMENT_DEPTH_STEP);
  m_element_depth_value = new QLabel();
  openxr_layout->addWidget(new ConfigFloatLabel(tr("Element Depth:"), m_element_depth), 16, 0);
  openxr_layout->addWidget(m_element_depth, 16, 1);
  openxr_layout->addWidget(m_element_depth_value, 16, 2);

  m_element_depth_value->setText(QString::asprintf("%.4f", m_element_depth->GetValue()));
  connect(m_element_depth, &ConfigFloatSlider::valueChanged, this, [this] {
    m_element_depth_value->setText(QString::asprintf("%.4f", m_element_depth->GetValue()));
  });

  main_layout->addWidget(openxr_group);

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

  m_load_custom_shaders =
      new ConfigBool(tr("Load Custom Shaders"), Config::GFX_VR_LOAD_CUSTOM_SHADERS);
  tools_layout->addWidget(m_load_custom_shaders, 2, 0);

  main_layout->addWidget(tools_group);

  main_layout->addStretch();

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
  static constexpr char TR_UNITS_PER_METER_DESCRIPTION[] = QT_TR_NOOP(
      "Sets how many game world units correspond to one real-world meter."
      "<br><br>Higher values increase stereo scale and make the world appear smaller."
      "<br><br>Lower values decrease stereo scale and make the world appear larger.");
  static constexpr char TR_LEAN_BACK_ANGLE_DESCRIPTION[] = QT_TR_NOOP(
      "Applies a constant pitch offset (in degrees) to the VR camera."
      "<br><br>Use this to compensate games where the default viewpoint feels tilted."
      "<br><br>Positive values lean the camera back, negative values lean it forward.");
  static constexpr char TR_CAMERA_FORWARD_DESCRIPTION[] = QT_TR_NOOP(
      "Applies a constant forward/backward offset (in meters) to the VR camera."
      "<br><br>Positive values move the camera forward, negative values move it backward.");
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
  static constexpr char TR_AUTO_VBI_FROM_HMD_DESCRIPTION[] = QT_TR_NOOP(
      "Forces Dolphin's VBI frequency to 90 Hz while OpenXR VR is enabled."
      "<br><br>This overrides the Advanced tab's VBI percentage during VR sessions."
      "<br><br><dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
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
  static constexpr char TR_METROID_VISOR_FIX_DESCRIPTION[] = QT_TR_NOOP(
      "Enables extra fullscreen-mono handling for Metroid Prime visor/framebuffer effects."
      "<br><br>Disable this if non-Metroid games show duplicate or misplaced fullscreen effects."
      "<br><br><dolphin_emphasis>If unsure, leave this checked for Metroid and unchecked when "
      "testing regressions in other games.</dolphin_emphasis>");
  static constexpr char TR_LOAD_CUSTOM_SHADERS_DESCRIPTION[] = QT_TR_NOOP(
      "Loads custom shader replacements from the Load/Shaders/&lt;GameID&gt;/ directory."
      "<br><br>Place edited shader files (dumped via Shader Hunter) in this folder to override "
      "the game's original shaders. Files must be named &lt;hash&gt;-&lt;vs|ps|gs&gt;.txt."
      "<br><br><dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");

  m_enable_openxr->SetDescription(tr(TR_ENABLE_OPENXR_DESCRIPTION));
  m_units_per_meter->SetDescription(tr(TR_UNITS_PER_METER_DESCRIPTION));
  m_lean_back_angle->SetDescription(tr(TR_LEAN_BACK_ANGLE_DESCRIPTION));
  m_camera_forward->SetDescription(tr(TR_CAMERA_FORWARD_DESCRIPTION));
  m_virtual_screen->SetDescription(tr(TR_VIRTUAL_SCREEN_DESCRIPTION));
  m_screen_distance->SetDescription(tr(TR_SCREEN_DISTANCE_DESCRIPTION));
  m_screen_size->SetDescription(tr(TR_SCREEN_SIZE_DESCRIPTION));
  m_head_locked_curvature->SetDescription(tr(TR_HEAD_LOCKED_CURVATURE_DESCRIPTION));
  m_dont_clear_screen->SetDescription(tr(TR_DONT_CLEAR_SCREEN_DESCRIPTION));
  m_disable_cpu_cull->SetDescription(tr(TR_DISABLE_CPU_CULL_DESCRIPTION));
  m_auto_vbi_from_hmd->SetDescription(tr(TR_AUTO_VBI_FROM_HMD_DESCRIPTION));
  m_clear_efb_slider->SetDescription(tr(TR_CLEAR_EFB_COPIES_DESCRIPTION));
  m_remove_bars->SetDescription(tr(TR_REMOVE_BARS_DESCRIPTION));
  m_metroid_visor_fix->SetDescription(tr(TR_METROID_VISOR_FIX_DESCRIPTION));
  m_auto_layer_spread->SetDescription(tr(TR_AUTO_LAYER_SPREAD_DESCRIPTION));
  m_layer_offset->SetDescription(tr(TR_LAYER_OFFSET_DESCRIPTION));
  m_element_depth->SetDescription(tr(TR_ELEMENT_DEPTH_DESCRIPTION));
  m_load_custom_shaders->SetDescription(tr(TR_LOAD_CUSTOM_SHADERS_DESCRIPTION));
}

void VRPane::OnEmulationStateChanged(Core::State state)
{
  const bool running = state != Core::State::Uninitialized;
  m_enable_openxr->setEnabled(!running);
}
