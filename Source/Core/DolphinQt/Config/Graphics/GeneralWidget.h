// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>

#include <QWidget>

class ConfigBool;
class ConfigChoice;
class ConfigFloatSlider;
class ConfigInteger;
class ConfigRadioInt;
class ConfigStringChoice;
class GraphicsPane;
class QLabel;
class ToolTipComboBox;
template <typename T>
class ConfigChoiceMap;
enum class OpenXRMirrorView : int;

namespace Config
{
class Layer;
}  // namespace Config

class GeneralWidget final : public QWidget
{
  Q_OBJECT
public:
  explicit GeneralWidget(GraphicsPane* gfx_pane);

signals:
  void BackendChanged(const QString& backend);

private:
  void BackendWarning();

  void CreateWidgets();
  void ToggleCustomAspectRatio(int index);
  void ConnectWidgets();
  void AddDescriptions();

  void OnBackendChanged(const QString& backend_name);
  void OnEmulationStateChanged(bool running);

  // Video
  ConfigStringChoice* m_backend_combo;
  ToolTipComboBox* m_adapter_combo;
  ConfigChoice* m_aspect_combo;
  QLabel* m_custom_aspect_label;
  ConfigInteger* m_custom_aspect_width;
  ConfigInteger* m_custom_aspect_height;
  ConfigBool* m_enable_vsync;
  ConfigBool* m_enable_fullscreen;
  ConfigBool* m_frame_skip;

  // Options
  ConfigBool* m_autoadjust_window_size;
  ConfigBool* m_render_main_window;
  ConfigChoiceMap<OpenXRMirrorView>* m_desktop_mirror_mode;
  ConfigFloatSlider* m_desktop_mirror_join_separation;
  QLabel* m_desktop_mirror_join_separation_value;
  ConfigFloatSlider* m_desktop_mirror_join_left_eye_offset;
  QLabel* m_desktop_mirror_join_left_eye_offset_value;
  ConfigFloatSlider* m_desktop_mirror_join_right_eye_offset;
  QLabel* m_desktop_mirror_join_right_eye_offset_value;
  ConfigFloatSlider* m_desktop_mirror_join_blend_width;
  QLabel* m_desktop_mirror_join_blend_width_value;

  std::array<ConfigRadioInt*, 4> m_shader_compilation_mode{};
  ConfigBool* m_wait_for_shaders;
  int m_previous_backend = 0;
  Config::Layer* m_game_layer = nullptr;
};
