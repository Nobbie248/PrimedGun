// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef ENABLE_VR

// Must define the D3D11 platform before any OpenXR includes.
#define XR_USE_GRAPHICS_API_D3D11

#include "VideoBackends/D3D/D3DOpenXR.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Common/Assert.h"
#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/Image.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"

#include "VideoBackends/D3D/D3DBase.h"
#include "VideoBackends/D3D/DXTexture.h"
#include "Common/VR/OpenXRInputState.h"
#include "VideoCommon/TextureConfig.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/VR/OpenXRD3D11Common.h"
#include "VideoCommon/VR/OpenXRManager.h"

namespace DX11
{
std::unique_ptr<D3DOpenXR> g_openxr_d3d;

namespace
{
std::array<uint8_t, 7> Glyph(char ch)
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
  case 'K': return {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11};
  case 'L': return {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F};
  case 'M': return {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
  case 'N': return {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
  case 'O': return {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
  case 'P': return {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
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

void FillRect(std::vector<uint32_t>& pixels, uint32_t width, uint32_t height, int x, int y, int w,
              int h, uint32_t color)
{
  const int x0 = std::clamp(x, 0, static_cast<int>(width));
  const int y0 = std::clamp(y, 0, static_cast<int>(height));
  const int x1 = std::clamp(x + w, 0, static_cast<int>(width));
  const int y1 = std::clamp(y + h, 0, static_cast<int>(height));
  for (int py = y0; py < y1; ++py)
    for (int px = x0; px < x1; ++px)
      pixels[static_cast<size_t>(py) * width + px] = color;
}

int TextWidth(const char* text, int scale)
{
  const int chars = static_cast<int>(std::strlen(text));
  return chars > 0 ? ((chars * 6) - 1) * scale : 0;
}

void DrawText(std::vector<uint32_t>& pixels, uint32_t width, uint32_t height, const char* text,
              int x, int y, int scale, uint32_t color)
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

std::string FloatText(float value, int precision)
{
  char buffer[32]{};
  std::snprintf(buffer, sizeof(buffer), precision == 3 ? "%.3f" : precision == 1 ? "%.1f" : "%.2f",
                value);
  return buffer;
}

struct PrimeGunPng
{
  Common::UniqueBuffer<u8> data;
  u32 width = 0;
  u32 height = 0;
  bool tried = false;
};

bool LoadPrimeGunPngFromPath(const std::string& path, PrimeGunPng* image)
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

const PrimeGunPng& LoadPrimeGunPng(const char* filename)
{
  static PrimeGunPng power;
  static PrimeGunPng wave;
  static PrimeGunPng ice;
  static PrimeGunPng plasma;

  PrimeGunPng* image = &power;
  if (std::strcmp(filename, "wave.png") == 0)
    image = &wave;
  else if (std::strcmp(filename, "ice.png") == 0)
    image = &ice;
  else if (std::strcmp(filename, "plasma.png") == 0)
    image = &plasma;

  if (!image->tried)
  {
    image->tried = true;
    const std::string path = File::GetExeDirectory() + DIR_SEP "assets" DIR_SEP + filename;
    if (!LoadPrimeGunPngFromPath(path, image))
      WARN_LOG_FMT(VIDEO, "PrimeGun: Failed to load weapon panel asset '{}'.", path);
  }

  return *image;
}

void BlendPixel(std::vector<uint32_t>& pixels, uint32_t width, uint32_t height, int x, int y,
                uint8_t r, uint8_t g, uint8_t b, uint8_t a)
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
  const uint8_t out_a = static_cast<uint8_t>(std::min(255u, static_cast<uint32_t>(a) +
                                                            (static_cast<uint32_t>(da) * inv_a) /
                                                                255u));
  const uint8_t out_r =
      static_cast<uint8_t>((static_cast<uint32_t>(r) * a + static_cast<uint32_t>(dr) * inv_a) /
                           255u);
  const uint8_t out_g =
      static_cast<uint8_t>((static_cast<uint32_t>(g) * a + static_cast<uint32_t>(dg) * inv_a) /
                           255u);
  const uint8_t out_b =
      static_cast<uint8_t>((static_cast<uint32_t>(b) * a + static_cast<uint32_t>(db) * inv_a) /
                           255u);
  pixels[dst_index] = (static_cast<uint32_t>(out_a) << 24) |
                      (static_cast<uint32_t>(out_r) << 16) |
                      (static_cast<uint32_t>(out_g) << 8) | static_cast<uint32_t>(out_b);
}

void DrawPngFit(std::vector<uint32_t>& pixels, uint32_t width, uint32_t height,
                const PrimeGunPng& image, int x, int y, int w, int h, bool selected)
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

  const float scale =
      std::min(static_cast<float>(w) / static_cast<float>(image.width),
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

bool MenuRowIsNumeric(uint32_t tab, int index)
{
  switch (tab)
  {
  case 0:
    return index == 1 || index == 2;
  case 1:
    return index >= 0 && index <= 5;
  case 2:
    return index == 2 || index == 5 || index == 6 || index == 7;
  case 3:
    return index >= 2 && index <= 5;
  default:
    return false;
  }
}

std::vector<MenuRow> BuildMenuRows(const Common::VR::PrimeGunVrOverlayState& s)
{
  switch (s.tab)
  {
  case 1:
    return {{"POS LEFT/RIGHT", FloatText(s.model_offset_x, 3)},
            {"POS FORWARD/BACK", FloatText(s.model_offset_y, 3)},
            {"POS UP/DOWN", FloatText(s.model_offset_z, 3)},
            {"ROT PITCH", FloatText(s.rot_offset_x, 1)},
            {"ROT YAW", FloatText(s.rot_offset_y, 1)},
            {"ROT ROLL", FloatText(s.rot_offset_z, 1)},
            {"RESET CALIBRATION", "PRESS"}};
  case 2:
    return {{"RIGHT HAND", s.use_right_hand ? "ON" : "OFF"},
            {"REQUIRE TRIGGER", s.require_trigger ? "ON" : "OFF"},
            {"TRIGGER", FloatText(s.trigger_threshold, 2)},
            {"VISOR GESTURE", s.xr_dpad_enabled ? "ON" : "OFF"},
            {"D-PAD ENABLED", s.xr_dpad_enabled ? "ON" : "OFF"},
            {"HEAD RADIUS", FloatText(s.xr_dpad_head_radius, 2)},
            {"HEAD BELOW", FloatText(s.xr_dpad_head_y_below, 2)},
            {"STICK DEADZONE", FloatText(s.xr_dpad_deadzone, 2)},
            {"RESET CONTROLLER", "PRESS"}};
  case 3:
    return {{"DIRECTIONAL MOVE", s.directional_movement_enabled ? "ON" : "OFF"},
            {"MOVEMENT STICK", s.directional_movement_use_right_stick ? "RIGHT" : "LEFT"},
            {"MOVE DEADZONE", FloatText(s.directional_movement_deadzone, 2)},
            {"MOVE SPEED", FloatText(s.directional_movement_speed, 1)},
            {"MOVE ACCEL", FloatText(s.directional_movement_accel, 1)},
            {"AIR ACCEL", FloatText(s.directional_movement_air_accel, 1)},
            {"RESET MOVEMENT", "PRESS"}};
  case 4:
    return {{"DEFAULT ARM PRESET", "APPLY"}, {"SAMUS ARM PRESET", "APPLY"}};
  default:
    return {{"TARGETING", s.gun_targeting_enabled ? "ON" : "OFF"},
            {"TARGET DISTANCE", FloatText(s.gun_targeting_distance, 1)},
            {"TARGET RADIUS", FloatText(s.gun_targeting_radius, 1)},
            {"RESET TARGETING", "PRESS"}};
  }
}

std::vector<uint32_t> BuildPromptPixels(uint32_t width, uint32_t height)
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

std::vector<uint32_t> BuildMenuPixels(uint32_t width, uint32_t height,
                                      const Common::VR::PrimeGunVrOverlayState& s)
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

  constexpr const char* tabs[] = {"AIMING", "CALIBRATION", "CONTROLLER", "MOVEMENT", "PRESETS"};
  for (int i = 0; i < 5; ++i)
  {
    const int x = 36 + i * 190;
    const bool active = i == static_cast<int>(s.tab);
    FillRect(pixels, width, height, x, 64, 180, 38, active ? 0xD04A2C12u : 0x7030180Cu);
    FillRect(pixels, width, height, x, 98, 180, 4, active ? 0xFFFFB030u : 0x604A2C12u);
    DrawText(pixels, width, height, tabs[i], x + (180 - TextWidth(tabs[i], 2)) / 2, 75, 2,
             active ? 0xFFFFE6B8u : 0xFFD8C0A0u);
  }
  auto draw_button = [&](const char* label, int x, int y, int w) {
    FillRect(pixels, width, height, x, y, w, 28, 0x9030180Cu);
    FillRect(pixels, width, height, x, y, 5, 28, 0x80FFB030u);
    DrawText(pixels, width, height, label, x + (w - TextWidth(label, 2)) / 2, y + 7, 2,
             0xFFFFE6B8u);
  };
  draw_button("SAVE SETTINGS", 52, 108, 220);
  draw_button("RESET ALL", 300, 108, 220);

  const auto rows = BuildMenuRows(s);
  const int row_x = 52;
  const int row_w = static_cast<int>(width) - 104;
  for (int i = 0; i < static_cast<int>(rows.size()); ++i)
  {
    const int y = 156 + i * 25;
    const bool selected = i == static_cast<int>(s.selected_index);
    FillRect(pixels, width, height, row_x, y - 9, row_w, 31,
             selected ? 0xE04A2C12u : 0x70201810u);
    FillRect(pixels, width, height, row_x, y - 9, 6, 31,
             selected ? 0xFFFFB030u : 0x80FFB030u);
    DrawText(pixels, width, height, rows[i].label, row_x + 16, y, 2,
             selected ? 0xFFFFE6B8u : 0xFFD8C0A0u);
    const int value_x = row_x + row_w - 190;
    const bool numeric = MenuRowIsNumeric(s.tab, i);
    FillRect(pixels, width, height, value_x, y - 5, 170, 23,
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

std::vector<uint32_t> BuildWeaponPanelPixels(uint32_t width, uint32_t height,
                                             const Common::VR::PrimeGunVrOverlayState& s)
{
  std::vector<uint32_t> pixels(static_cast<size_t>(width) * height, 0);

  auto draw_slot = [&](uint32_t index, const char* filename, int x, int y, int w, int h) {
    DrawPngFit(pixels, width, height, LoadPrimeGunPng(filename), x, y, w, h,
               s.weapon_selected_index == index);
  };

  draw_slot(1, "power.png", 184, 32, 144, 144);
  draw_slot(4, "plasma.png", 32, 184, 144, 144);
  draw_slot(2, "wave.png", 336, 184, 144, 144);
  draw_slot(3, "ice.png", 184, 336, 144, 144);
  return pixels;
}

XrVector3f RotateVector(const XrQuaternionf& q, const XrVector3f& v)
{
  const XrVector3f t{2.0f * (q.y * v.z - q.z * v.y), 2.0f * (q.z * v.x - q.x * v.z),
                     2.0f * (q.x * v.y - q.y * v.x)};
  return {v.x + q.w * t.x + (q.y * t.z - q.z * t.y),
          v.y + q.w * t.y + (q.z * t.x - q.x * t.z),
          v.z + q.w * t.z + (q.x * t.y - q.y * t.x)};
}

XrQuaternionf MulQuat(const XrQuaternionf& a, const XrQuaternionf& b)
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

HybridControllerPose MakeHybridPose(const Common::VR::OpenXRControllerState& controller)
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

HybridControllerPose MakeGripPose(const Common::VR::OpenXRControllerState& controller)
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

HybridControllerPose MakeAimPose(const Common::VR::OpenXRControllerState& controller)
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

void AddTrackingOrigin(HybridControllerPose* pose,
                       const Common::VR::OpenXRInputSnapshot& snapshot)
{
  if (!pose || !pose->valid)
    return;

  pose->position.x += snapshot.tracking_origin_position[0];
  pose->position.y += snapshot.tracking_origin_position[1];
  pose->position.z += snapshot.tracking_origin_position[2];
}
}  // namespace

D3DOpenXR::D3DOpenXR() = default;

D3DOpenXR::~D3DOpenXR()
{
  Shutdown();
}

bool D3DOpenXR::Initialize()
{
  INFO_LOG_FMT(VIDEO, "OpenXR D3D11: Starting initialization...");
  ASSERT_MSG(VIDEO, !VR::g_openxr, "OpenXRManager already initialized.");

  auto mgr = std::make_unique<VR::OpenXRManager>();

  // The D3D11 graphics binding extension is mandatory.
  // Also enable optional controller profile extensions (Meta, Pico, etc.) when available.
  std::vector<const char*> extensions = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
  if (VR::OpenXRManager::IsRuntimeExtensionSupported(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME))
  {
    extensions.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    INFO_LOG_FMT(VIDEO, "OpenXR: Enabling XR_FB_display_refresh_rate.");
  }
  const auto controller_exts = VR::OpenXRManager::GetAvailableControllerExtensions();
  extensions.insert(extensions.end(), controller_exts.begin(), controller_exts.end());
  if (!mgr->CreateInstance(extensions))
    return false;

  if (!mgr->InitializeSystem())
    return false;

  if (!mgr->EnumerateViewConfigurations())
    return false;

  // Publish the manager globally so CreateSessionD3D11 can access instance/system IDs.
  VR::g_openxr = std::move(mgr);

  if (!CreateSessionD3D11())
  {
    VR::g_openxr.reset();
    return false;
  }

  if (!VR::g_openxr->CreateReferenceSpace())
  {
    VR::g_openxr.reset();
    return false;
  }

  if (!CreateSwapchains())
  {
    VR::g_openxr.reset();
    return false;
  }

  // Register this object as the swapchain provider so Presenter can acquire eye images.
  VR::g_openxr->SetSwapchain(this);

  INFO_LOG_FMT(VIDEO, "OpenXR D3D11: Initialization complete.");
  return true;
}

void D3DOpenXR::Shutdown()
{
  // Clear swapchain pointer before destroying swapchains so no dangling use occurs.
  if (VR::g_openxr)
    VR::g_openxr->SetSwapchain(nullptr);

  DestroySwapchains();
  VR::g_openxr.reset();
  if (D3D::context)
    D3D::context->Flush();
  INFO_LOG_FMT(VIDEO, "OpenXR D3D11: Shut down.");
}

bool D3DOpenXR::CreateSessionD3D11()
{
  ASSERT(D3D::device != nullptr);
  ASSERT(VR::g_openxr != nullptr);

  XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
  if (!VR::D3D11OpenXR::QueryGraphicsRequirements(VR::g_openxr->GetInstance(),
                                                  VR::g_openxr->GetSystemId(), &requirements))
    return false;

  INFO_LOG_FMT(VIDEO,
               "OpenXR: D3D11 requirements — adapter LUID {:#010x}{:08x}, "
               "min feature level {:#x}",
               requirements.adapterLuid.HighPart, requirements.adapterLuid.LowPart,
               static_cast<int>(requirements.minFeatureLevel));

  XrSession session = XR_NULL_HANDLE;
  if (!VR::D3D11OpenXR::CreateSessionFromRequirements(VR::g_openxr->GetInstance(),
                                                      VR::g_openxr->GetSystemId(), requirements,
                                                      D3D::device.Get(), &session))
    return false;

  VR::g_openxr->SetSession(session);
  INFO_LOG_FMT(VIDEO, "OpenXR D3D11: Session created successfully.");
  return true;
}

bool D3DOpenXR::CreateSwapchains()
{
  ASSERT(VR::g_openxr != nullptr);

  const auto& view_cfgs = VR::g_openxr->GetViewConfigViews();
  int64_t swapchain_format = 0;
  if (!VR::D3D11OpenXR::SelectSwapchainFormat(VR::g_openxr->GetSession(), &swapchain_format))
    return false;

  for (uint32_t eye = 0; eye < 2; ++eye)
  {
    auto& sc = m_eye_swapchains[eye];
    sc.width = view_cfgs[eye].recommendedImageRectWidth;
    sc.height = view_cfgs[eye].recommendedImageRectHeight;

    // Format must come from xrEnumerateSwapchainFormats.
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.arraySize = 1;
    info.format = swapchain_format;
    info.width = sc.width;
    info.height = sc.height;
    info.mipCount = 1;
    info.faceCount = 1;
    info.sampleCount = 1;
    // We render into this swapchain image as a color target, but never sample from it.
    // Some runtimes reject the extra SAMPLED usage for certain formats.
    //
    // MUTABLE_FORMAT lets us keep the swapchain declared as sRGB (so the compositor
    // decodes correctly) while creating a UNORM RTV on the underlying texture, so
    // BlitFromTexture writes raw sRGB-encoded bytes without a double gamma encode.
    // With this flag the runtime must back the texture with a DXGI _TYPELESS format.
    info.usageFlags =
        XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;

    XrResult result = xrCreateSwapchain(VR::g_openxr->GetSession(), &info, &sc.swapchain);
    if (XR_FAILED(result))
    {
      ERROR_LOG_FMT(VIDEO, "OpenXR: xrCreateSwapchain failed for eye {} ({}).", eye,
                    static_cast<int>(result));
      return false;
    }

    // Enumerate the D3D11 textures backing this swapchain.
    uint32_t image_count = 0;
    xrEnumerateSwapchainImages(sc.swapchain, 0, &image_count, nullptr);

    std::vector<XrSwapchainImageD3D11KHR> images(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    xrEnumerateSwapchainImages(sc.swapchain, image_count, &image_count,
                               reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));

    sc.textures.resize(image_count);
    sc.framebuffers.resize(image_count);

    for (uint32_t i = 0; i < image_count; ++i)
    {
      // Adopt the runtime-owned texture. DXTexture::CreateAdopted reads the D3D11 texture
      // descriptor to build the TextureConfig and AddRefs the underlying resource.
      sc.textures[i] = DXTexture::CreateAdopted(ComPtr<ID3D11Texture2D>(images[i].texture));
      if (!sc.textures[i])
      {
        ERROR_LOG_FMT(VIDEO, "OpenXR: DXTexture::CreateAdopted failed for eye {}, image {}.",
                      eye, i);
        return false;
      }

      // No depth attachment for now; depth will be added in Phase 3 when the render path
      // is fully integrated with the EFB.
      sc.framebuffers[i] = DXFramebuffer::Create(sc.textures[i].get(), nullptr, {});
      if (!sc.framebuffers[i])
      {
        ERROR_LOG_FMT(VIDEO, "OpenXR: DXFramebuffer::Create failed for eye {}, image {}.",
                      eye, i);
        return false;
      }
    }

    INFO_LOG_FMT(VIDEO, "OpenXR: Eye {} swapchain ready: {}x{}, {} images.", eye, sc.width,
                 sc.height, image_count);
  }

  return true;
}

void D3DOpenXR::DestroySwapchains()
{
  DestroyPrimeGunOverlaySwapchain();
  DestroyPrimeGunLaserSwapchain();

  for (uint32_t eye = 0; eye < 2; ++eye)
  {
    auto& sc = m_eye_swapchains[eye];

    if (m_image_acquired[eye] && sc.swapchain != XR_NULL_HANDLE)
    {
      XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
      const XrResult release_result = xrReleaseSwapchainImage(sc.swapchain, &release_info);
      if (XR_FAILED(release_result))
      {
        WARN_LOG_FMT(VIDEO,
                     "OpenXR: xrReleaseSwapchainImage during shutdown failed for eye {} ({}).",
                     eye, static_cast<int>(release_result));
      }
      m_image_acquired[eye] = false;
    }

    // Release Dolphin wrappers before destroying the swapchain so the
    // runtime's textures are only freed after our references are gone.
    sc.framebuffers.clear();
    sc.textures.clear();

    if (sc.swapchain != XR_NULL_HANDLE)
    {
      const XrResult destroy_result = xrDestroySwapchain(sc.swapchain);
      if (XR_FAILED(destroy_result))
      {
        WARN_LOG_FMT(VIDEO, "OpenXR: xrDestroySwapchain failed for eye {} ({}).", eye,
                     static_cast<int>(destroy_result));
      }
      sc.swapchain = XR_NULL_HANDLE;
    }
  }
}

void D3DOpenXR::DestroyPrimeGunOverlaySwapchain()
{
  auto& overlay = m_primegun_overlay_swapchain;
  overlay.images.clear();
  overlay.texture_ready = false;
  overlay.content_kind = 0;
  overlay.generation = 0;
  if (overlay.swapchain != XR_NULL_HANDLE)
  {
    const XrResult result = xrDestroySwapchain(overlay.swapchain);
    if (XR_FAILED(result))
      WARN_LOG_FMT(VIDEO, "OpenXR: PrimeGun overlay xrDestroySwapchain failed ({}).",
                   static_cast<int>(result));
    overlay.swapchain = XR_NULL_HANDLE;
  }
}

void D3DOpenXR::DestroyPrimeGunLaserSwapchain()
{
  auto& laser = m_primegun_laser_swapchain;
  laser.images.clear();
  laser.texture_ready = false;
  if (laser.swapchain != XR_NULL_HANDLE)
  {
    const XrResult result = xrDestroySwapchain(laser.swapchain);
    if (XR_FAILED(result))
      WARN_LOG_FMT(VIDEO, "OpenXR: PrimeGun laser xrDestroySwapchain failed ({}).",
                   static_cast<int>(result));
    laser.swapchain = XR_NULL_HANDLE;
  }
}

bool D3DOpenXR::EnsurePrimeGunLaserSwapchain()
{
  auto& laser = m_primegun_laser_swapchain;
  if (laser.texture_ready && laser.swapchain != XR_NULL_HANDLE)
    return true;

  DestroyPrimeGunLaserSwapchain();
  int64_t swapchain_format = DXGI_FORMAT_R8G8B8A8_UNORM;
  VR::D3D11OpenXR::SelectSwapchainFormat(VR::g_openxr->GetSession(), &swapchain_format);

  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  info.arraySize = 1;
  info.format = swapchain_format;
  info.width = 16;
  info.height = 10;
  info.mipCount = 1;
  info.faceCount = 1;
  info.sampleCount = 1;
  info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
  XrResult result = xrCreateSwapchain(VR::g_openxr->GetSession(), &info, &laser.swapchain);
  if (XR_FAILED(result) || laser.swapchain == XR_NULL_HANDLE)
    return false;

  uint32_t image_count = 0;
  result = xrEnumerateSwapchainImages(laser.swapchain, 0, &image_count, nullptr);
  if (XR_FAILED(result) || image_count == 0)
  {
    DestroyPrimeGunLaserSwapchain();
    return false;
  }

  laser.images.assign(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
  result = xrEnumerateSwapchainImages(
      laser.swapchain, image_count, &image_count,
      reinterpret_cast<XrSwapchainImageBaseHeader*>(laser.images.data()));
  if (XR_FAILED(result))
  {
    DestroyPrimeGunLaserSwapchain();
    return false;
  }

  std::array<uint32_t, 160> pixels{};
  for (int y = 3; y < 7; ++y)
    for (int x = 0; x < 16; ++x)
      pixels[static_cast<size_t>(y * 16 + x)] = 0xE080D8FFu;

  for (uint32_t i = 0; i < image_count; ++i)
  {
    uint32_t acquired = 0;
    XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    result = xrAcquireSwapchainImage(laser.swapchain, &acquire_info, &acquired);
    if (XR_FAILED(result))
    {
      DestroyPrimeGunLaserSwapchain();
      return false;
    }
    XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait_info.timeout = XR_INFINITE_DURATION;
    result = xrWaitSwapchainImage(laser.swapchain, &wait_info);
    if (XR_SUCCEEDED(result) && acquired < laser.images.size() && laser.images[acquired].texture)
      D3D::context->UpdateSubresource(laser.images[acquired].texture, 0, nullptr, pixels.data(),
                                      16 * sizeof(uint32_t), 0);
    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(laser.swapchain, &release_info);
    if (XR_FAILED(result))
    {
      DestroyPrimeGunLaserSwapchain();
      return false;
    }
  }

  laser.texture_ready = true;
  return true;
}

bool D3DOpenXR::EnsurePrimeGunOverlaySwapchain(uint32_t content_kind, uint32_t generation,
                                               uint32_t width, uint32_t height,
                                               const std::vector<uint32_t>& pixels)
{
  auto& overlay = m_primegun_overlay_swapchain;
  if (overlay.texture_ready && overlay.swapchain != XR_NULL_HANDLE &&
      overlay.content_kind == content_kind && overlay.generation == generation &&
      overlay.width == width && overlay.height == height)
  {
    return true;
  }

  DestroyPrimeGunOverlaySwapchain();
  overlay.width = width;
  overlay.height = height;

  int64_t swapchain_format = DXGI_FORMAT_R8G8B8A8_UNORM;
  VR::D3D11OpenXR::SelectSwapchainFormat(VR::g_openxr->GetSession(), &swapchain_format);

  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  info.arraySize = 1;
  info.format = swapchain_format;
  info.width = width;
  info.height = height;
  info.mipCount = 1;
  info.faceCount = 1;
  info.sampleCount = 1;
  info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
  XrResult result = xrCreateSwapchain(VR::g_openxr->GetSession(), &info, &overlay.swapchain);
  if (XR_FAILED(result) || overlay.swapchain == XR_NULL_HANDLE)
  {
    WARN_LOG_FMT(VIDEO, "OpenXR: PrimeGun overlay xrCreateSwapchain failed ({}).",
                 static_cast<int>(result));
    return false;
  }

  uint32_t image_count = 0;
  result = xrEnumerateSwapchainImages(overlay.swapchain, 0, &image_count, nullptr);
  if (XR_FAILED(result) || image_count == 0)
  {
    DestroyPrimeGunOverlaySwapchain();
    return false;
  }

  overlay.images.assign(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
  result = xrEnumerateSwapchainImages(
      overlay.swapchain, image_count, &image_count,
      reinterpret_cast<XrSwapchainImageBaseHeader*>(overlay.images.data()));
  if (XR_FAILED(result))
  {
    DestroyPrimeGunOverlaySwapchain();
    return false;
  }

  for (uint32_t i = 0; i < image_count; ++i)
  {
    uint32_t acquired = 0;
    XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    result = xrAcquireSwapchainImage(overlay.swapchain, &acquire_info, &acquired);
    if (XR_FAILED(result))
    {
      DestroyPrimeGunOverlaySwapchain();
      return false;
    }
    XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait_info.timeout = XR_INFINITE_DURATION;
    result = xrWaitSwapchainImage(overlay.swapchain, &wait_info);
    if (XR_SUCCEEDED(result) && acquired < overlay.images.size() && overlay.images[acquired].texture)
    {
      D3D::context->UpdateSubresource(overlay.images[acquired].texture, 0, nullptr, pixels.data(),
                                      width * sizeof(uint32_t), 0);
    }
    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(overlay.swapchain, &release_info);
    if (XR_FAILED(result))
    {
      DestroyPrimeGunOverlaySwapchain();
      return false;
    }
  }

  overlay.content_kind = content_kind;
  overlay.generation = generation;
  overlay.texture_ready = true;
  return true;
}

bool D3DOpenXR::AppendPrimeGunOverlayLayers(std::vector<XrCompositionLayerBaseHeader*>* layers)
{
  if (!VR::g_openxr || !layers)
    return false;

  const auto overlay = Common::VR::OpenXRInputState::GetPrimeGunOverlay();
  if (!overlay.menu_visible && !overlay.prompt_visible && !overlay.weapon_panel_visible)
    return false;

  const Common::VR::OpenXRInputSnapshot snapshot = Common::VR::OpenXRInputState::GetSnapshot();
  if (!snapshot.runtime_active)
    return false;

  const bool menu = overlay.menu_visible;
  const bool weapon_panel = !menu && overlay.weapon_panel_visible;
  const uint32_t content_kind = menu ? 2u : weapon_panel ? 3u : 1u;
  const uint32_t width = menu ? 1024 : weapon_panel ? 512 : 1024;
  const uint32_t height = menu ? 512 : weapon_panel ? 512 : 384;
  const uint32_t generation = menu ? overlay.generation :
                              weapon_panel ? (100u + overlay.weapon_selected_index) :
                                             1u;
  const std::vector<uint32_t> pixels = menu        ? BuildMenuPixels(width, height, overlay) :
                                       weapon_panel ? BuildWeaponPanelPixels(width, height, overlay) :
                                                      BuildPromptPixels(width, height);
  if (!EnsurePrimeGunOverlaySwapchain(content_kind, generation, width, height, pixels))
    return false;

  m_primegun_overlay_layer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
  m_primegun_overlay_layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                        XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
  m_primegun_overlay_layer.space = VR::g_openxr->GetReferenceSpace();
  m_primegun_overlay_layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
  m_primegun_overlay_layer.subImage.swapchain = m_primegun_overlay_swapchain.swapchain;
  m_primegun_overlay_layer.subImage.imageRect.offset = {0, 0};
  m_primegun_overlay_layer.subImage.imageRect.extent = {static_cast<int32_t>(width),
                                                        static_cast<int32_t>(height)};

  HybridControllerPose left_pose = MakeGripPose(snapshot.controllers[0]);
  HybridControllerPose right_pose = MakeAimPose(snapshot.controllers[1]);
  AddTrackingOrigin(&left_pose, snapshot);
  AddTrackingOrigin(&right_pose, snapshot);

  if (menu && left_pose.valid)
  {
    const XrQuaternionf q = left_pose.orientation;
    m_primegun_overlay_layer.pose.orientation = MulQuat(q, {-0.70710678f, 0.0f, 0.0f, 0.70710678f});
    const XrVector3f offset = RotateVector(m_primegun_overlay_layer.pose.orientation,
                                           {0.0f, 0.10f, -0.18f});
    m_primegun_overlay_layer.pose.position = {left_pose.position.x + offset.x,
                                              left_pose.position.y + offset.y,
                                              left_pose.position.z + offset.z};
    m_primegun_overlay_layer.size = {1.05f, 0.72f};
  }
  else if (weapon_panel)
  {
    m_primegun_overlay_layer.pose.orientation = {overlay.weapon_panel_orientation[0],
                                                 overlay.weapon_panel_orientation[1],
                                                 overlay.weapon_panel_orientation[2],
                                                 overlay.weapon_panel_orientation[3]};
    const XrVector3f offset =
        RotateVector(m_primegun_overlay_layer.pose.orientation, {0.0f, 0.055f, -0.26f});
    m_primegun_overlay_layer.pose.position = {
        overlay.weapon_panel_position[0] + snapshot.tracking_origin_position[0] + offset.x,
        overlay.weapon_panel_position[1] + snapshot.tracking_origin_position[1] + offset.y,
        overlay.weapon_panel_position[2] + snapshot.tracking_origin_position[2] + offset.z};
    m_primegun_overlay_layer.size = {0.42f, 0.42f};
  }
  else if (snapshot.head_pose.valid)
  {
    const auto& head = snapshot.head_pose;
    m_primegun_overlay_layer.pose.orientation = {head.orientation[0], head.orientation[1],
                                                 head.orientation[2], head.orientation[3]};
    const XrVector3f offset =
        RotateVector(m_primegun_overlay_layer.pose.orientation, {0.0f, 0.0f, -1.35f});
    m_primegun_overlay_layer.pose.position = {
        head.position[0] + snapshot.tracking_origin_position[0] + offset.x,
        head.position[1] + snapshot.tracking_origin_position[1] + offset.y,
        head.position[2] + snapshot.tracking_origin_position[2] + offset.z};
    m_primegun_overlay_layer.size = {0.675f, 0.25f};
  }
  else
  {
    return false;
  }

  layers->push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&m_primegun_overlay_layer));

  if (menu && right_pose.valid && EnsurePrimeGunLaserSwapchain())
  {
    const XrQuaternionf q = right_pose.orientation;
    const XrVector3f forward = RotateVector(q, {0.0f, 0.0f, -1.0f});
    m_primegun_laser_layer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    m_primegun_laser_layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                        XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    m_primegun_laser_layer.space = VR::g_openxr->GetReferenceSpace();
    m_primegun_laser_layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    m_primegun_laser_layer.subImage.swapchain = m_primegun_laser_swapchain.swapchain;
    m_primegun_laser_layer.subImage.imageRect.offset = {0, 0};
    m_primegun_laser_layer.subImage.imageRect.extent = {16, 10};
    m_primegun_laser_layer.pose.orientation = MulQuat(q, {-0.70710678f, 0.0f, 0.0f, 0.70710678f});
    m_primegun_laser_layer.pose.position = {right_pose.position.x + forward.x * 0.40f,
                                            right_pose.position.y + forward.y * 0.40f,
                                            right_pose.position.z + forward.z * 0.40f};
    m_primegun_laser_layer.size = {0.008f, 0.80f};
    layers->push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&m_primegun_laser_layer));
  }

  return true;
}

AbstractFramebuffer* D3DOpenXR::AcquireEyeFramebuffer(uint32_t eye_index)
{
  ASSERT(eye_index < 2);
  auto& sc = m_eye_swapchains[eye_index];

  XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  if (XR_FAILED(xrAcquireSwapchainImage(sc.swapchain, &acquire_info,
                                         &m_acquired_image_index[eye_index])))
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR: xrAcquireSwapchainImage failed for eye {}.", eye_index);
    return nullptr;
  }
  m_image_acquired[eye_index] = true;

  // Block until the acquired image is safe to write.
  XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wait_info.timeout = XR_INFINITE_DURATION;
  if (XR_FAILED(xrWaitSwapchainImage(sc.swapchain, &wait_info)))
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR: xrWaitSwapchainImage failed for eye {}.", eye_index);

    // Ensure we don't leak an acquired image if waiting fails.
    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    const XrResult release_result = xrReleaseSwapchainImage(sc.swapchain, &release_info);
    if (XR_FAILED(release_result))
    {
      WARN_LOG_FMT(VIDEO,
                   "OpenXR: xrReleaseSwapchainImage after wait failure failed for eye {} ({}).",
                   eye_index, static_cast<int>(release_result));
    }
    m_image_acquired[eye_index] = false;
    return nullptr;
  }

  return sc.framebuffers[m_acquired_image_index[eye_index]].get();
}

void D3DOpenXR::ReleaseEyeTexture(uint32_t eye_index)
{
  ASSERT(eye_index < 2);
  if (!m_image_acquired[eye_index])
    return;

  XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  const XrResult result = xrReleaseSwapchainImage(m_eye_swapchains[eye_index].swapchain,
                                                   &release_info);
  if (XR_FAILED(result))
  {
    WARN_LOG_FMT(VIDEO, "OpenXR: xrReleaseSwapchainImage failed for eye {} ({}).", eye_index,
                 static_cast<int>(result));
  }
  m_image_acquired[eye_index] = false;
}

bool D3DOpenXR::SubmitFrame()
{
  ASSERT(VR::g_openxr != nullptr);

  // Use the submit snapshot captured when the GS pose cache was last refreshed. Using
  // live m_eye_views here would pick up LocateViews that ran between the last draw and
  // xrEndFrame, causing ATW to reproject against the wrong pose.
  const auto& eye_views = VR::g_openxr->GetSubmittedEyeViews();
  if (!VR::g_openxr->AreSubmittedEyeViewsValid())
    return VR::g_openxr->EndFrame({});

  for (uint32_t eye = 0; eye < 2; ++eye)
  {
    auto& pv = m_projection_views[eye];
    pv = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    pv.pose = eye_views[eye].pose;
    pv.fov = eye_views[eye].fov;
    pv.subImage.swapchain = m_eye_swapchains[eye].swapchain;
    pv.subImage.imageArrayIndex = 0;
    pv.subImage.imageRect = {
        {0, 0},
        {static_cast<int32_t>(m_eye_swapchains[eye].width),
         static_cast<int32_t>(m_eye_swapchains[eye].height)}};
  }

  m_projection_layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  m_projection_layer.space = VR::g_openxr->GetReferenceSpace();
  m_projection_layer.viewCount = 2;
  m_projection_layer.views = m_projection_views.data();

  if (VR::g_openxr->GetActiveBlendMode() == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND)
  {
    m_projection_layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                    XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
  }

  std::vector<XrCompositionLayerBaseHeader*> layers = {
      reinterpret_cast<XrCompositionLayerBaseHeader*>(&m_projection_layer)};
  AppendPrimeGunOverlayLayers(&layers);

  return VR::g_openxr->EndFrame(layers);
}

}  // namespace DX11

#endif  // ENABLE_VR
