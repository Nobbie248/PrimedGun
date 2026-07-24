// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QPointer>
#include <QWidget>

class ConfigBool;
class ConfigFloatSlider;
class ConfigSlider;
class QLabel;
class QPushButton;
class CullingCodeFinderWidget;
class ShaderHunterWidget;
template <typename T>
class ConfigChoiceMap;
enum class OpenXROpcodeReplayMode : int;
enum class OpenXRMirrorView : int;
enum class OpenXRReferenceSpaceMode : int;
enum class OpenXRTrackingMode : int;

namespace Core
{
enum class State;
}

class VRPane final : public QWidget
{
  Q_OBJECT
public:
  explicit VRPane(QWidget* parent = nullptr);

private:
  void AddDescriptions();
  void OnEmulationStateChanged(Core::State state);
  void ResetGeneralSettings();

  ConfigBool* m_enable_openxr = nullptr;
  ConfigChoiceMap<OpenXRReferenceSpaceMode>* m_reference_space_mode = nullptr;
  ConfigChoiceMap<OpenXRTrackingMode>* m_tracking_mode = nullptr;
  ConfigBool* m_auto_immediate_xfb = nullptr;
  ConfigFloatSlider* m_units_per_meter = nullptr;
  QLabel* m_units_per_meter_value = nullptr;
  ConfigFloatSlider* m_lean_back_angle = nullptr;
  QLabel* m_lean_back_angle_value = nullptr;
  ConfigFloatSlider* m_camera_forward = nullptr;
  QLabel* m_camera_forward_value = nullptr;
  ConfigFloatSlider* m_camera_height = nullptr;
  QLabel* m_camera_height_value = nullptr;
  ConfigBool* m_virtual_screen = nullptr;
  ConfigFloatSlider* m_screen_distance = nullptr;
  QLabel* m_screen_distance_value = nullptr;
  ConfigFloatSlider* m_screen_size = nullptr;
  QLabel* m_screen_size_value = nullptr;
  ConfigFloatSlider* m_head_locked_curvature = nullptr;
  QLabel* m_head_locked_curvature_value = nullptr;
  ConfigBool* m_dont_clear_screen = nullptr;
  ConfigBool* m_disable_cpu_cull = nullptr;
  ConfigBool* m_ortho_scissor_fix = nullptr;
  ConfigBool* m_metroid_visor_fix = nullptr;
  ConfigBool* m_xr_pacing_thread = nullptr;
  ConfigBool* m_eager_heartbeat = nullptr;
  ConfigChoiceMap<OpenXROpcodeReplayMode>* m_opcode_replay_mode = nullptr;
  ConfigChoiceMap<OpenXRMirrorView>* m_mirror_view = nullptr;
  ConfigChoiceMap<int>* m_replay_refresh_rate = nullptr;
  ConfigChoiceMap<int>* m_forced_vbi_frequency = nullptr;
  ConfigSlider* m_clear_efb_slider = nullptr;
  QLabel* m_clear_efb_value = nullptr;
  ConfigBool* m_remove_bars = nullptr;
  ConfigBool* m_lock_head_pose = nullptr;
  ConfigBool* m_use_vulkan_multiview = nullptr;
  ConfigBool* m_ar_mode = nullptr;
  ConfigFloatSlider* m_ar_background_alpha = nullptr;
  QLabel* m_ar_background_alpha_value = nullptr;
  ConfigFloatSlider* m_vr_gamma = nullptr;
  QLabel* m_vr_gamma_value = nullptr;
  ConfigBool* m_auto_layer_spread = nullptr;
  ConfigFloatSlider* m_layer_offset = nullptr;
  QLabel* m_layer_offset_value = nullptr;
  ConfigFloatSlider* m_element_depth = nullptr;
  QLabel* m_element_depth_value = nullptr;
  ConfigBool* m_hud_3d_enable = nullptr;
  ConfigFloatSlider* m_hud_3d_closer = nullptr;
  QLabel* m_hud_3d_closer_value = nullptr;
  QPushButton* m_reset_general_settings = nullptr;
  ConfigBool* m_load_custom_shaders = nullptr;
  ConfigBool* m_enable_openxr_config_scene = nullptr;
  ConfigBool* m_ar_mode_debug = nullptr;
  QPointer<CullingCodeFinderWidget> m_culling_finder_widget;
  QPointer<ShaderHunterWidget> m_shader_hunter_widget;
};
