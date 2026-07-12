// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/Image.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/VR/OpenXRInputState.h"

#include <openxr/openxr.h>

namespace PrimedGun::Overlay
{
inline std::array<uint8_t, 7> Glyph(char ch)
{
  switch (ch)
  {
  case 'A': return {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
  case 'B': return {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E};
  case 'C': return {0x0F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0F};
  case 'D': return {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E};
  case 'E': return {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
  case 'F': return {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10};
  case 'G': return {0x0F, 0x10, 0x10, 0x13, 0x11, 0x11, 0x0F};
  case 'H': return {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
  case 'I': return {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F};
  case 'J': return {0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0E};
  case 'K': return {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11};
  case 'L': return {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F};
  case 'M': return {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
  case 'N': return {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
  case 'O': return {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
  case 'P': return {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
  case 'Q': return {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D};
  case 'R': return {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11};
  case 'S': return {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
  case 'T': return {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
  case 'U': return {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
  case 'V': return {0x11, 0x11, 0x11, 0x11, 0x0A, 0x0A, 0x04};
  case 'W': return {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11};
  case 'X': return {0x11, 0x0A, 0x04, 0x04, 0x04, 0x0A, 0x11};
  case 'Y': return {0x11, 0x0A, 0x04, 0x04, 0x04, 0x04, 0x04};
  case 'Z': return {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F};
  case '0': return {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E};
  case '1': return {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E};
  case '2': return {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F};
  case '3': return {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E};
  case '4': return {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02};
  case '5': return {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E};
  case '6': return {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E};
  case '7': return {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
  case '8': return {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E};
  case '9': return {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E};
  case '.': return {0, 0, 0, 0, 0, 0x0C, 0x0C};
  case '-': return {0, 0, 0, 0x1F, 0, 0, 0};
  case '+': return {0, 0x04, 0x04, 0x1F, 0x04, 0x04, 0};
  case '/': return {0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10};
  default: return {};
  }
}

inline void FillRect(std::vector<uint32_t>& pixels, uint32_t width, uint32_t height, int x, int y,
                     int w, int h, uint32_t color)
{
  const int x0 = std::clamp(x, 0, static_cast<int>(width));
  const int y0 = std::clamp(y, 0, static_cast<int>(height));
  const int x1 = std::clamp(x + w, 0, static_cast<int>(width));
  const int y1 = std::clamp(y + h, 0, static_cast<int>(height));
  for (int py = y0; py < y1; ++py)
    for (int px = x0; px < x1; ++px)
      pixels[static_cast<size_t>(py) * width + px] = color;
}

inline int TextWidth(const char* text, int scale)
{
  const int chars = static_cast<int>(std::strlen(text));
  return chars > 0 ? ((chars * 6) - 1) * scale : 0;
}

inline void DrawText(std::vector<uint32_t>& pixels, uint32_t width, uint32_t height,
                     const char* text, int x, int y, int scale, uint32_t color)
{
  int cursor = x;
  for (const char* p = text; *p; ++p)
  {
    const auto rows = Glyph(*p);
    for (int row = 0; row < 7; ++row)
      for (int col = 0; col < 5; ++col)
        if ((rows[static_cast<size_t>(row)] & (1u << (4 - col))) != 0)
          FillRect(pixels, width, height, cursor + col * scale, y + row * scale, scale, scale,
                   color);
    cursor += 6 * scale;
  }
}

inline std::string FloatText(float value, int precision)
{
  char buffer[32]{};
  std::snprintf(buffer, sizeof(buffer), precision == 3 ? "%.3f" : precision == 1 ? "%.1f" : "%.2f",
                value);
  return buffer;
}

struct PrimedGunPng
{
  Common::UniqueBuffer<u8> data;
  u32 width = 0;
  u32 height = 0;
  bool tried = false;
};

inline bool LoadPrimedGunPngFromPath(const std::string& path, PrimedGunPng* image)
{
  File::IOFile file(path, "rb", File::SharedAccess::Read);
  if (!file.IsOpen())
    return false;

  const u64 size = file.GetSize();
  if (size == 0)
    return false;

  Common::UniqueBuffer<u8> buffer(size);
  if (!file.ReadBytes(buffer.data(), size))
    return false;

  Common::UniqueBuffer<u8> pixels;
  u32 width = 0;
  u32 height = 0;
  if (!Common::LoadPNG(std::span<const u8>(buffer.data(), static_cast<size_t>(size)), &pixels,
                       &width, &height) ||
      pixels.empty() || width == 0 || height == 0)
  {
    return false;
  }

  image->data = std::move(pixels);
  image->width = width;
  image->height = height;
  return true;
}

inline const PrimedGunPng& LoadPrimedGunPng(const char* filename)
{
  static PrimedGunPng power;
  static PrimedGunPng wave;
  static PrimedGunPng ice;
  static PrimedGunPng plasma;
  static PrimedGunPng position;

  PrimedGunPng* image = &power;
  if (std::strcmp(filename, "wave.png") == 0)
    image = &wave;
  else if (std::strcmp(filename, "ice.png") == 0)
    image = &ice;
  else if (std::strcmp(filename, "plasma.png") == 0)
    image = &plasma;
  else if (std::strcmp(filename, "position.png") == 0)
    image = &position;

  if (!image->tried)
  {
    image->tried = true;
    const std::string path = File::GetExeDirectory() + DIR_SEP "assets" DIR_SEP + filename;
    if (!LoadPrimedGunPngFromPath(path, image))
      WARN_LOG_FMT(VIDEO, "PrimedGun: Failed to load weapon panel asset '{}'.", path);
  }

  return *image;
}

inline void BlendPixel(std::vector<uint32_t>& pixels, uint32_t width, uint32_t height, int x,
                       int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
  if (x < 0 || y < 0 || x >= static_cast<int>(width) || y >= static_cast<int>(height) || a == 0)
    return;

  const size_t dst_index = static_cast<size_t>(y) * width + x;
  const uint32_t dst = pixels[dst_index];
  const uint8_t da = static_cast<uint8_t>(dst >> 24);
  const uint8_t dr = static_cast<uint8_t>(dst >> 16);
  const uint8_t dg = static_cast<uint8_t>(dst >> 8);
  const uint8_t db = static_cast<uint8_t>(dst);
  const uint32_t inv_a = 255u - a;
  const uint8_t out_a = static_cast<uint8_t>(std::min(
      255u, static_cast<uint32_t>(a) + (static_cast<uint32_t>(da) * inv_a) / 255u));
  const uint8_t out_r = static_cast<uint8_t>((static_cast<uint32_t>(r) * a +
                                              static_cast<uint32_t>(dr) * inv_a) /
                                             255u);
  const uint8_t out_g = static_cast<uint8_t>((static_cast<uint32_t>(g) * a +
                                              static_cast<uint32_t>(dg) * inv_a) /
                                             255u);
  const uint8_t out_b = static_cast<uint8_t>((static_cast<uint32_t>(b) * a +
                                              static_cast<uint32_t>(db) * inv_a) /
                                             255u);
  pixels[dst_index] = (static_cast<uint32_t>(out_a) << 24) |
                      (static_cast<uint32_t>(out_r) << 16) |
                      (static_cast<uint32_t>(out_g) << 8) | static_cast<uint32_t>(out_b);
}

inline void DrawPngFit(std::vector<uint32_t>& pixels, uint32_t width, uint32_t height,
                       const PrimedGunPng& image, int x, int y, int w, int h, bool selected)
{
  if (selected)
  {
    FillRect(pixels, width, height, x - 10, y - 10, w + 20, 5, 0xE0FFB030u);
    FillRect(pixels, width, height, x - 10, y + h + 5, w + 20, 5, 0xE0FFB030u);
    FillRect(pixels, width, height, x - 10, y - 10, 5, h + 20, 0xE0FFB030u);
    FillRect(pixels, width, height, x + w + 5, y - 10, 5, h + 20, 0xE0FFB030u);
  }

  if (image.data.empty() || image.width == 0 || image.height == 0)
    return;

  const float scale = std::min(static_cast<float>(w) / static_cast<float>(image.width),
                               static_cast<float>(h) / static_cast<float>(image.height));
  const int draw_w = std::max(1, static_cast<int>(std::lround(image.width * scale)));
  const int draw_h = std::max(1, static_cast<int>(std::lround(image.height * scale)));
  const int dst_x = x + (w - draw_w) / 2;
  const int dst_y = y + (h - draw_h) / 2;

  for (int py = 0; py < draw_h; ++py)
  {
    const u32 sy =
        std::min(image.height - 1, static_cast<u32>((static_cast<int64_t>(py) * image.height) /
                                                    std::max(1, draw_h)));
    for (int px = 0; px < draw_w; ++px)
    {
      const u32 sx =
          std::min(image.width - 1, static_cast<u32>((static_cast<int64_t>(px) * image.width) /
                                                     std::max(1, draw_w)));
      const size_t src = (static_cast<size_t>(sy) * image.width + sx) * 4;
      BlendPixel(pixels, width, height, dst_x + px, dst_y + py, image.data[src + 2],
                 image.data[src + 1], image.data[src + 0], image.data[src + 3]);
    }
  }
}

struct MenuRow
{
  const char* label;
  std::string value;
};

constexpr uint32_t VR_MENU_LAYOUT_TAB = 0;
constexpr uint32_t VR_MENU_CALIBRATION_TAB = 1;
constexpr uint32_t VR_MENU_CONTROL_TAB = 2;
constexpr uint32_t VR_MENU_MOVEMENT_TAB = 3;
constexpr uint32_t VR_MENU_CANNON_TAB = 4;
constexpr uint32_t VR_MENU_STATE_TAB = 5;
constexpr uint32_t RESET_ALL_ACTION = 1;
constexpr uint32_t RESET_TARGETING_ACTION = 2;
constexpr uint32_t RESET_CALIBRATION_ACTION = 3;
constexpr uint32_t RESET_CONTROLLER_ACTION = 4;
constexpr uint32_t RESET_MOVEMENT_ACTION = 5;
constexpr uint32_t STATE_LOAD_ACTION = 1;
constexpr uint32_t STATE_SAVE_ACTION = 2;
constexpr uint32_t STATE_LOAD_NEWEST_ACTION = 3;
constexpr uint32_t STATE_SAVE_OLDEST_ACTION = 4;
constexpr uint32_t STATE_ACTION_ROW_COUNT = 4;
constexpr uint32_t STATE_SLOT_COUNT = 10;

inline std::string ConfirmText(const Common::VR::PrimedGunVrOverlayState& s, uint32_t action)
{
  return s.reset_confirm_action == action ? "ARE YOU SURE?" : "PRESS";
}

inline int ControlMenuActualIndex(uint32_t page, int local_index)
{
  if (local_index <= 0)
    return -1;

  constexpr int first_page_items = 8;
  return page == 0 ? local_index - 1 : first_page_items + local_index - 1;
}

inline int CalibrationMenuActualIndex(uint32_t page, int local_index)
{
  if (local_index <= 0)
    return -1;

  constexpr int first_page_items = 12;
  return page == 0 ? local_index - 1 : first_page_items + local_index - 1;
}

inline bool MenuRowIsNumeric(const Common::VR::PrimedGunVrOverlayState& s, int index)
{
  switch (s.tab)
  {
  case VR_MENU_CALIBRATION_TAB:
  {
    if (index == 0)
      return true;

    const int actual_index = CalibrationMenuActualIndex(s.calibration_page, index);
    return (actual_index >= 1 && actual_index <= 8) ||
           (actual_index >= 12 && actual_index <= 17);
  }
  case VR_MENU_MOVEMENT_TAB:
    return (index >= 3 && index <= 7) || index == 9;
  case VR_MENU_CONTROL_TAB:
  {
    if (index == 0)
      return true;

    const int actual_index = ControlMenuActualIndex(s.control_page, index);
    return actual_index == 3 || actual_index == 9 || actual_index == 13 ||
           actual_index == 14 || actual_index == 15;
  }
  default:
    return false;
  }
}

inline std::string RumbleHandModeText(int mode)
{
  switch (mode)
  {
  case 1:
    return "LEFT";
  case 2:
    return "RIGHT";
  default:
    return "BOTH";
  }
}

inline int MenuRowTextY(const Common::VR::PrimedGunVrOverlayState& s, int index)
{
  int y = 146 + index * 22;
  if (s.tab == VR_MENU_STATE_TAB && index >= static_cast<int>(STATE_ACTION_ROW_COUNT))
    y += 18;
  return y;
}

inline std::vector<MenuRow> BuildMenuRows(const Common::VR::PrimedGunVrOverlayState& s)
{
  switch (s.tab)
  {
  case VR_MENU_CALIBRATION_TAB:
  {
    std::vector<MenuRow> rows;
    rows.push_back({"PAGE", s.calibration_page == 0 ? "1/2" : "2/2"});

    if (s.calibration_page == 0)
    {
      rows.push_back({"CUTSCENE CINEMA SCREEN", s.cinematic_screen_enabled ? "ON" : "OFF"});
      rows.push_back({"HUD DISTANCE", FloatText(s.metroid_hud_distance, 2)});
      rows.push_back({"HUD SIZE", FloatText(s.metroid_hud_size, 2)});
      rows.push_back({"HUD VERTICAL",
                      FloatText(s.metroid_hud_offset_up - s.metroid_hud_offset_down, 2)});
      rows.push_back({"HUD HORIZONTAL",
                      FloatText(s.metroid_hud_offset_right - s.metroid_hud_offset_left, 2)});
      rows.push_back({"TARGET DISTANCE", FloatText(s.gun_targeting_distance, 1)});
      rows.push_back({"TARGET RADIUS", FloatText(s.gun_targeting_radius, 1)});
      rows.push_back({"VISOR HELMET", s.visor_helmet_enabled ? "ON" : "OFF"});
      rows.push_back({"HEIGHT PROMPT", s.height_prompt_enabled ? "ON" : "OFF"});
      rows.push_back({"RESET TARGETING", ConfirmText(s, RESET_TARGETING_ACTION)});
    }
    else
    {
      rows.push_back({"POSITION LEFT/RIGHT", FloatText(s.model_offset_x, 3)});
      rows.push_back({"POSITION FORWARD/BACK", FloatText(s.model_offset_y, 3)});
      rows.push_back({"POSITION UP/DOWN", FloatText(s.model_offset_z, 3)});
      rows.push_back({"ROTATION PITCH", FloatText(s.rot_offset_x, 1)});
      rows.push_back({"ROTATION YAW", FloatText(s.rot_offset_y, 1)});
      rows.push_back({"ROTATION ROLL", FloatText(s.rot_offset_z, 1)});
      rows.push_back({"FLOOR POSITION MARKER", s.position_marker_visible ? "ON" : "OFF"});
      rows.push_back({"RESET CALIBRATION", ConfirmText(s, RESET_CALIBRATION_ACTION)});
      rows.push_back({"DEFAULT ARM PRESET", "APPLY"});
      rows.push_back({"SAMUS ARM PRESET", "APPLY"});
    }
    return rows;
  }
  case VR_MENU_CONTROL_TAB:
  {
    std::vector<MenuRow> rows;
    rows.push_back({"PAGE", s.control_page == 0 ? "1/2" : "2/2"});

    if (s.control_page == 0)
    {
      rows.push_back({"RIGHT HAND", s.use_right_hand ? "ON" : "OFF"});
      rows.push_back({"RUMBLE", s.rumble_enabled ? "ON" : "OFF"});
      rows.push_back({"RUMBLE TARGET", RumbleHandModeText(s.rumble_hand_mode)});
      rows.push_back({"RUMBLE INTENSITY", FloatText(s.rumble_intensity, 2)});
      rows.push_back({"GRIP INPUT", s.primegun_grip_inputs_enabled ? "ON" : "OFF"});
      rows.push_back({"A BUTTON JUMP", s.combat_jump_use_primary_button ? "ON" : "OFF"});
      rows.push_back({"LONGER HELD PRESS FOR VR MENU", s.vr_menu_hold_left_stick ? "ON" : "OFF"});
      rows.push_back({"MENU REQUIRES HAND NEAR HEAD", s.vr_menu_requires_head_zone ? "ON" : "OFF"});
    }
    else
    {
      rows.push_back(
          {"GRIP INPUT SOURCE", s.primegun_grip_inputs_use_trackpad ? "TRACKPAD" : "GRIP"});
      rows.push_back({"TRACKPAD SENSITIVITY", FloatText(s.primegun_trackpad_press_threshold, 2)});
      rows.push_back({"VISOR GESTURE", s.xr_dpad_enabled ? "ON" : "OFF"});
      rows.push_back({"DIRECTION PAD", s.xr_dpad_enabled ? "ON" : "OFF"});
      rows.push_back({"QUEST THUMB REST", s.xr_dpad_use_thumbrest_modifier ? "ON" : "OFF"});
      rows.push_back({"HEAD RADIUS", FloatText(s.xr_dpad_head_radius, 2)});
      rows.push_back({"HEAD BELOW", FloatText(s.xr_dpad_head_y_below, 2)});
      rows.push_back({"STICK DEADZONE", FloatText(s.xr_dpad_deadzone, 2)});
      rows.push_back({"RESET CONTROLLER", ConfirmText(s, RESET_CONTROLLER_ACTION)});
    }

    return rows;
  }
  case VR_MENU_MOVEMENT_TAB:
    return {{"LEFT STICK STRAFE", s.directional_movement_enabled ? "ON" : "OFF"},
            {"MOVEMENT STICK", s.directional_movement_use_right_stick ? "RIGHT" : "LEFT"},
            {"MOVEMENT DIRECTION",
             s.directional_movement_use_hmd_direction ? "HEADSET" : "CONTROLLER"},
            {"MOVEMENT DEADZONE", FloatText(s.directional_movement_deadzone, 2)},
            {"MOVEMENT SPEED", FloatText(s.directional_movement_speed, 1)},
            {"MOVEMENT ACCELERATION", FloatText(s.directional_movement_accel, 1)},
            {"AIR ACCELERATION", FloatText(s.directional_movement_air_accel, 1)},
            {"LOOK YAW SENSITIVITY", FloatText(s.look_yaw_sensitivity, 2)},
            {"SNAP TURN", s.snap_turn_enabled ? "ON" : "OFF"},
            {"SNAP TURN ANGLE", std::to_string(s.snap_turn_degrees)},
            {"RESET MOVEMENT", ConfirmText(s, RESET_MOVEMENT_ACTION)}};
  case VR_MENU_CANNON_TAB:
  {
    auto slot_status = [&](uint32_t slot) {
      return s.cannon_texture_slot == slot ? std::string("SELECTED") : std::string("SELECT");
    };
    return {{"DEFAULT", slot_status(0)}, {"SLOT 1", slot_status(1)},
            {"SLOT 2", slot_status(2)}, {"SLOT 3", slot_status(3)},
            {"SLOT 4", slot_status(4)}, {"CUSTOM", slot_status(5)}};
  }
  case VR_MENU_STATE_TAB:
  {
    std::vector<MenuRow> rows;
    const std::string selected_slot = "SLOT " + std::to_string(s.state_slot);
    rows.push_back({"LOAD STATE",
                    s.state_confirm_action == STATE_LOAD_ACTION ? "ARE YOU SURE?" :
                                                                  selected_slot});
    rows.push_back({"SAVE STATE",
                    s.state_confirm_action == STATE_SAVE_ACTION ? "ARE YOU SURE?" :
                                                                  selected_slot});
    rows.push_back({"LOAD NEWEST",
                    s.state_confirm_action == STATE_LOAD_NEWEST_ACTION ? "ARE YOU SURE?" :
                                                                         "PRESS"});
    rows.push_back({"SAVE OLDEST",
                    s.state_confirm_action == STATE_SAVE_OLDEST_ACTION ? "ARE YOU SURE?" :
                                                                         "PRESS"});
    constexpr std::array<const char*, STATE_SLOT_COUNT> slot_labels = {
        "SLOT 1", "SLOT 2", "SLOT 3", "SLOT 4", "SLOT 5",
        "SLOT 6", "SLOT 7", "SLOT 8", "SLOT 9", "SLOT 10"};
    for (uint32_t slot = 1; slot <= STATE_SLOT_COUNT; ++slot)
    {
      rows.push_back({slot_labels[static_cast<size_t>(slot - 1)],
                      s.state_slot == slot ? "SELECTED" : "SELECT"});
    }
    return rows;
  }
  default:
    return {};
  }
}

inline std::vector<uint32_t> BuildPromptPixels(uint32_t width, uint32_t height)
{
  std::vector<uint32_t> pixels(static_cast<size_t>(width) * height, 0);
  FillRect(pixels, width, height, 0, 0, static_cast<int>(width), static_cast<int>(height),
           0xD0100804u);
  FillRect(pixels, width, height, 0, 0, static_cast<int>(width), 8, 0xE0FFB030u);
  FillRect(pixels, width, height, 0, static_cast<int>(height) - 8, static_cast<int>(width), 8,
           0xE0FFB030u);
  constexpr const char* lines[] = {"CLICK RIGHT", "THUMBSTICK", "TO SET", "HEIGHT"};
  const int scale = 5;
  const int step = 78;
  for (int i = 0; i < 4; ++i)
    DrawText(pixels, width, height, lines[i],
             (static_cast<int>(width) - TextWidth(lines[i], scale)) / 2, 44 + i * step, scale,
             0xFFFFD8A0u);
  return pixels;
}

inline void DrawLayoutTextPage(std::vector<uint32_t>& pixels, uint32_t width, uint32_t height)
{
  constexpr uint32_t title_color = 0xFFFFE6B8u;
  constexpr uint32_t body_color = 0xFFD8C0A0u;
  constexpr uint32_t accent_color = 0xFF40F0E0u;
  constexpr uint32_t row_color = 0x50201810u;
  constexpr uint32_t rule_color = 0x80FFB030u;

  constexpr const char* title = "CONTROLLER LAYOUT";
  DrawText(pixels, width, height, title, (static_cast<int>(width) - TextWidth(title, 3)) / 2, 124,
           3, title_color);

  constexpr const char* visor_help =
      "PLACE OFFHAND NEAR YOUR HEAD AND USE THE CONTROL STICK TO CHANGE VISORS";
  DrawText(pixels, width, height, visor_help,
           (static_cast<int>(width) - TextWidth(visor_help, 2)) / 2, 174, 2, accent_color);

  constexpr int left_x = 72;
  constexpr int right_x = 570;
  constexpr int heading_y = 220;
  DrawText(pixels, width, height, "LEFT HAND", left_x, heading_y, 2, title_color);
  DrawText(pixels, width, height, "RIGHT HAND", right_x, heading_y, 2, title_color);
  FillRect(pixels, width, height, left_x, heading_y + 24, 360, 3, rule_color);
  FillRect(pixels, width, height, right_x, heading_y + 24, 360, 3, rule_color);

  constexpr std::array<const char*, 6> left_rows = {
      "LEFT STICK - MOVE",       "Y BUTTON - START/PAUSE", "X BUTTON - MORPH BALL",
      "LEFT STICK CLICK - VR SETTINGS", "LEFT GRIP - MAP",       "L TRIGGER - LOCK ON"};
  constexpr std::array<const char*, 6> right_rows = {
      "RIGHT STICK - LOOK/JUMP", "B BUTTON - BEAM SELECT", "A BUTTON - SELECT",
      "RIGHT STICK CLICK - SET HEIGHT", "RIGHT GRIP - MISSILES", "RIGHT TRIGGER - SHOOT"};

  for (int i = 0; i < 6; ++i)
  {
    const int y = 262 + i * 34;
    FillRect(pixels, width, height, left_x - 14, y - 8, 398, 26, row_color);
    FillRect(pixels, width, height, right_x - 14, y - 8, 398, 26, row_color);
    FillRect(pixels, width, height, left_x - 14, y - 8, 5, 26, rule_color);
    FillRect(pixels, width, height, right_x - 14, y - 8, 5, 26, rule_color);
    DrawText(pixels, width, height, left_rows[static_cast<size_t>(i)], left_x, y, 2,
             body_color);
    DrawText(pixels, width, height, right_rows[static_cast<size_t>(i)], right_x, y, 2,
             body_color);
  }
}

inline std::vector<uint32_t> BuildMenuPixels(uint32_t width, uint32_t height,
                                             const Common::VR::PrimedGunVrOverlayState& s)
{
  std::vector<uint32_t> pixels(static_cast<size_t>(width) * height, 0);
  FillRect(pixels, width, height, 0, 0, static_cast<int>(width), static_cast<int>(height),
           0xD0100804u);
  FillRect(pixels, width, height, 0, 0, static_cast<int>(width), 10, 0xE0FFB030u);
  FillRect(pixels, width, height, 0, static_cast<int>(height) - 10, static_cast<int>(width), 10,
           0xE0FFB030u);
  DrawText(pixels, width, height, "PRIMEDGUN SETTINGS", 48, 28, 4, 0xFFFFD8A0u);
  if (s.saved_notice)
    DrawText(pixels, width, height, "SETTINGS SAVED", 760, 34, 2, 0xFFFFE6B8u);

  constexpr const char* tabs[] = {"LAYOUT",   "CALIBRATION", "CONTROL",
                                  "MOVEMENT", "TEXTURES",    "STATES"};
  constexpr int tab_width = 150;
  constexpr int tab_step = 166;
  for (int i = 0; i < static_cast<int>(std::size(tabs)); ++i)
  {
    const int x = 22 + i * tab_step;
    const bool active = i == static_cast<int>(s.tab);
    FillRect(pixels, width, height, x, 64, tab_width, 38, active ? 0xD04A2C12u : 0x7030180Cu);
    FillRect(pixels, width, height, x, 98, tab_width, 4, active ? 0xFFFFB030u : 0x604A2C12u);
    DrawText(pixels, width, height, tabs[i], x + (tab_width - TextWidth(tabs[i], 2)) / 2, 75, 2,
             active ? 0xFFFFE6B8u : 0xFFD8C0A0u);
  }

  auto draw_button = [&](const char* label, int x, int y, int w) {
    FillRect(pixels, width, height, x, y, w, 28, 0x9030180Cu);
    FillRect(pixels, width, height, x, y, 5, 28, 0x80FFB030u);
    DrawText(pixels, width, height, label, x + (w - TextWidth(label, 2)) / 2, y + 7, 2,
             0xFFFFE6B8u);
  };

  if (s.tab == VR_MENU_LAYOUT_TAB)
  {
    DrawLayoutTextPage(pixels, width, height);
    return pixels;
  }

  draw_button("SAVE SETTINGS", 52, 108, 220);
  draw_button(s.reset_confirm_action == RESET_ALL_ACTION ? "ARE YOU SURE?" : "RESET ALL", 300,
              108, 220);

  const auto rows = BuildMenuRows(s);
  const int row_x = 52;
  const int row_w = static_cast<int>(width) - 104;
  for (int i = 0; i < static_cast<int>(rows.size()); ++i)
  {
    const int y = MenuRowTextY(s, i);
    const bool selected = i == static_cast<int>(s.selected_index);
    FillRect(pixels, width, height, row_x, y - 8, row_w, 24,
             selected ? 0xE04A2C12u : 0x70201810u);
    FillRect(pixels, width, height, row_x, y - 8, 6, 24,
             selected ? 0xFFFFB030u : 0x80FFB030u);
    DrawText(pixels, width, height, rows[i].label, row_x + 16, y, 2,
             selected ? 0xFFFFE6B8u : 0xFFD8C0A0u);
    const int value_x = row_x + row_w - 190;
    const bool numeric = MenuRowIsNumeric(s, i);
    FillRect(pixels, width, height, value_x, y - 4, 170, 20,
             selected ? 0xD030180Cu : 0x9030180Cu);
    if (numeric)
    {
      DrawText(pixels, width, height, "-", value_x + 8, y, 2, 0xFFFFB030u);
      DrawText(pixels, width, height, "+", value_x + 148, y, 2, 0xFFFFB030u);
    }
    DrawText(pixels, width, height, rows[i].value.c_str(),
             value_x + (170 - TextWidth(rows[i].value.c_str(), 2)) / 2, y, 2, 0xFFFFF0C8u);
  }
  return pixels;
}

inline std::vector<uint32_t> BuildWeaponPanelPixels(
    uint32_t width, uint32_t height, const Common::VR::PrimedGunVrOverlayState& s)
{
  std::vector<uint32_t> pixels(static_cast<size_t>(width) * height, 0);

  auto draw_slot = [&](uint32_t index, const char* filename, int x, int y, int w, int h) {
    DrawPngFit(pixels, width, height, LoadPrimedGunPng(filename), x, y, w, h,
               s.weapon_selected_index == index);
  };

  draw_slot(1, "power.png", 184, 32, 144, 144);
  draw_slot(4, "plasma.png", 32, 184, 144, 144);
  draw_slot(2, "wave.png", 336, 184, 144, 144);
  draw_slot(3, "ice.png", 184, 336, 144, 144);
  return pixels;
}

inline std::vector<uint32_t> BuildPositionMarkerPixels(uint32_t width, uint32_t height)
{
  std::vector<uint32_t> pixels(static_cast<size_t>(width) * height, 0);
  DrawPngFit(pixels, width, height, LoadPrimedGunPng("position.png"), 0, 0,
             static_cast<int>(width), static_cast<int>(height), false);
  return pixels;
}

inline XrVector3f RotateVector(const XrQuaternionf& q, const XrVector3f& v)
{
  const XrVector3f t{2.0f * (q.y * v.z - q.z * v.y), 2.0f * (q.z * v.x - q.x * v.z),
                     2.0f * (q.x * v.y - q.y * v.x)};
  return {v.x + q.w * t.x + (q.y * t.z - q.z * t.y),
          v.y + q.w * t.y + (q.z * t.x - q.x * t.z),
          v.z + q.w * t.z + (q.x * t.y - q.y * t.x)};
}

inline XrQuaternionf YawOnlyQuaternion(const XrQuaternionf& q)
{
  XrVector3f forward = RotateVector(q, {0.0f, 0.0f, -1.0f});
  forward.y = 0.0f;

  const float len = std::sqrt(forward.x * forward.x + forward.z * forward.z);
  if (len < 0.0001f)
    return {0.0f, 0.0f, 0.0f, 1.0f};

  forward.x /= len;
  forward.z /= len;
  const float yaw = std::atan2(-forward.x, -forward.z);
  const float half_yaw = yaw * 0.5f;
  return {0.0f, std::sin(half_yaw), 0.0f, std::cos(half_yaw)};
}

inline XrQuaternionf MulQuat(const XrQuaternionf& a, const XrQuaternionf& b)
{
  return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
          a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
          a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
          a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

struct HybridControllerPose
{
  bool valid = false;
  XrVector3f position{};
  XrQuaternionf orientation{0.0f, 0.0f, 0.0f, 1.0f};
};

inline HybridControllerPose MakeHybridPose(const Common::VR::OpenXRControllerState& controller)
{
  HybridControllerPose pose{};
  if (controller.aim_pose.valid)
  {
    pose.valid = true;
    pose.orientation = {controller.aim_pose.orientation[0], controller.aim_pose.orientation[1],
                        controller.aim_pose.orientation[2], controller.aim_pose.orientation[3]};
    pose.position = {controller.aim_pose.position[0], controller.aim_pose.position[1],
                     controller.aim_pose.position[2]};
  }
  if (controller.grip_pose.valid)
  {
    pose.valid = true;
    pose.position = {controller.grip_pose.position[0], controller.grip_pose.position[1],
                     controller.grip_pose.position[2]};
    if (!controller.aim_pose.valid)
      pose.orientation = {controller.grip_pose.orientation[0], controller.grip_pose.orientation[1],
                          controller.grip_pose.orientation[2], controller.grip_pose.orientation[3]};
  }
  return pose;
}

inline HybridControllerPose MakeGripPose(const Common::VR::OpenXRControllerState& controller)
{
  HybridControllerPose pose{};
  if (controller.grip_pose.valid)
  {
    pose.valid = true;
    pose.position = {controller.grip_pose.position[0], controller.grip_pose.position[1],
                     controller.grip_pose.position[2]};
    pose.orientation = {controller.grip_pose.orientation[0], controller.grip_pose.orientation[1],
                        controller.grip_pose.orientation[2], controller.grip_pose.orientation[3]};
    return pose;
  }
  return MakeHybridPose(controller);
}

inline HybridControllerPose MakeAimPose(const Common::VR::OpenXRControllerState& controller)
{
  HybridControllerPose pose{};
  if (!controller.aim_pose.valid)
    return pose;

  pose.valid = true;
  pose.position = {controller.aim_pose.position[0], controller.aim_pose.position[1],
                   controller.aim_pose.position[2]};
  pose.orientation = {controller.aim_pose.orientation[0], controller.aim_pose.orientation[1],
                      controller.aim_pose.orientation[2], controller.aim_pose.orientation[3]};
  return pose;
}

inline void AddTrackingOrigin(HybridControllerPose* pose,
                              const Common::VR::OpenXRInputSnapshot& snapshot)
{
  if (!pose || !pose->valid)
    return;

  pose->position.x += snapshot.tracking_origin_position[0];
  pose->position.y += snapshot.tracking_origin_position[1];
  pose->position.z += snapshot.tracking_origin_position[2];
}
}  // namespace PrimedGun::Overlay
