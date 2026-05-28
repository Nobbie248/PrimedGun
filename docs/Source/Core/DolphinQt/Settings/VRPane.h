// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QPointer>
#include <QWidget>

class ConfigBool;
class ConfigFloatSlider;
class ConfigSlider;
class QLabel;
class ShaderHunterWidget;

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

  ConfigBool* m_enable_openxr = nullptr;
  ConfigFloatSlider* m_units_per_meter = nullptr;
  QLabel* m_units_per_meter_value = nullptr;
  ConfigFloatSlider* m_lean_back_angle = nullptr;
  QLabel* m_lean_back_angle_value = nullptr;
  ConfigFloatSlider* m_camera_forward = nullptr;
  QLabel* m_camera_forward_value = nullptr;
  ConfigBool* m_virtual_screen = nullptr;
  ConfigFloatSlider* m_screen_distance = nullptr;
  QLabel* m_screen_distance_value = nullptr;
  ConfigFloatSlider* m_screen_size = nullptr;
  QLabel* m_screen_size_value = nullptr;
  ConfigFloatSlider* m_head_locked_curvature = nullptr;
  QLabel* m_head_locked_curvature_value = nullptr;
  ConfigBool* m_dont_clear_screen = nullptr;
  ConfigBool* m_disable_cpu_cull = nullptr;
  ConfigBool* m_auto_vbi_from_hmd = nullptr;
  ConfigSlider* m_clear_efb_slider = nullptr;
  QLabel* m_clear_efb_value = nullptr;
  ConfigBool* m_remove_bars = nullptr;
  ConfigBool* m_metroid_visor_fix = nullptr;
  ConfigBool* m_auto_layer_spread = nullptr;
  ConfigFloatSlider* m_layer_offset = nullptr;
  QLabel* m_layer_offset_value = nullptr;
  ConfigFloatSlider* m_element_depth = nullptr;
  QLabel* m_element_depth_value = nullptr;
  ConfigBool* m_load_custom_shaders = nullptr;
  QPointer<ShaderHunterWidget> m_shader_hunter_widget;
};
