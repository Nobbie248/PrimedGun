// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <openxr/openxr.h>

namespace VR
{
// Pure projection math shared by the renderer and its containment tests.
inline bool BuildHeadCullProjection(const std::array<XrView, 2>& views, const XrVector3f& home,
                                    float cone_degrees, float units_per_meter,
                                    float lean_back_degrees, float camera_forward,
                                    std::array<std::array<float, 4>, 4>* out_matrix,
                                    float* out_effective_degrees = nullptr)
{
  if (!out_matrix || !std::isfinite(cone_degrees) || !std::isfinite(units_per_meter) ||
      !std::isfinite(lean_back_degrees) || !std::isfinite(camera_forward) ||
      !std::isfinite(home.x) || !std::isfinite(home.y) || !std::isfinite(home.z))
    return false;

  // Canted-eye projections need a union of differently oriented frusta; leave them unculled.
  const auto& q0 = views[0].pose.orientation;
  const auto& q1 = views[1].pose.orientation;
  const float dot = q0.x * q1.x + q0.y * q1.y + q0.z * q1.z + q0.w * q1.w;
  if (!std::isfinite(dot) || std::abs(std::abs(dot) - 1.0f) > 0.000001f)
    return false;
  for (const XrView& view : views)
  {
    const auto& orientation = view.pose.orientation;
    const float norm = orientation.x * orientation.x + orientation.y * orientation.y +
                       orientation.z * orientation.z + orientation.w * orientation.w;
    if (!std::isfinite(norm) || std::abs(norm - 1.0f) > 0.00001f)
      return false;
    if (!std::isfinite(view.pose.position.x) || !std::isfinite(view.pose.position.y) ||
        !std::isfinite(view.pose.position.z) || !std::isfinite(view.fov.angleLeft) ||
        !std::isfinite(view.fov.angleRight) || !std::isfinite(view.fov.angleUp) ||
        !std::isfinite(view.fov.angleDown))
      return false;
  }
  constexpr float DEG_TO_RAD = 0.01745329252f;
  const float s = std::max(units_per_meter, 0.0001f);

  // Floor the cone at the rendered FOV: the widest half-angle of either eye, doubled, plus a
  // margin for the IPD offset of near geometry and pose drift between draws of one frame.
  float max_half_angle = 0.0f;
  for (const XrView& view : views)
  {
    max_half_angle = std::max({max_half_angle, std::abs(view.fov.angleLeft),
                               std::abs(view.fov.angleRight), std::abs(view.fov.angleUp),
                               std::abs(view.fov.angleDown)});
  }
  constexpr float RENDER_FOV_MARGIN_DEG = 4.0f;
  const float min_cone_degrees = 2.0f * max_half_angle / DEG_TO_RAD + RENDER_FOV_MARGIN_DEG;
  if (min_cone_degrees >= 179.0f)
    return false;
  const float cone = std::clamp(std::max(cone_degrees, min_cone_degrees), 10.0f, 179.0f);
  if (out_effective_degrees)
    *out_effective_degrees = cone;
  const float half_cone = 0.5f * cone * DEG_TO_RAD;
  const float focal = 1.0f / std::tan(half_cone);

  // Same head rotation as GetEyeProjectionRows, taken from the left eye; runtimes report one
  // orientation for both eyes.
  const XrQuaternionf& q_xr = views[0].pose.orientation;
  XrQuaternionf q = {-q_xr.x, -q_xr.y, -q_xr.z, q_xr.w};
  const float lean_back_rad = lean_back_degrees * DEG_TO_RAD;
  if (lean_back_rad != 0.0f)
  {
    const float half_angle = 0.5f * lean_back_rad;
    const XrQuaternionf lean_back_quat = {std::sin(half_angle), 0.0f, 0.0f, std::cos(half_angle)};
    const auto& a = q;
    const auto& b = lean_back_quat;
    q = {a.w * b.x + a.x * b.w, a.y * b.w + a.z * b.x,
         a.z * b.w - a.y * b.x, a.w * b.w - a.x * b.x};
  }

  const float x2 = 2.0f * q.x * q.x, y2 = 2.0f * q.y * q.y, z2 = 2.0f * q.z * q.z;
  const float xy = 2.0f * q.x * q.y, xz = 2.0f * q.x * q.z, yz = 2.0f * q.y * q.z;
  const float wx = 2.0f * q.w * q.x, wy = 2.0f * q.w * q.y, wz = 2.0f * q.w * q.z;
  const float r00 = 1.0f - y2 - z2, r01 = xy + wz, r02 = xz - wy;
  const float r10 = xy - wz, r11 = 1.0f - x2 - z2, r12 = yz + wx;
  const float r20 = xz + wy, r21 = yz - wx, r22 = 1.0f - x2 - y2;

  // Symmetric frustum: proj_row0 = {focal, 0, 0}, proj_row1 = {0, focal, 0}; combined = R * col.
  const float c0x = r00 * focal, c0y = r10 * focal, c0z = r20 * focal;
  const float c1x = r01 * focal, c1y = r11 * focal, c1z = r21 * focal;

  // Head centre relative to home in game units; averaging the eyes cancels the IPD.
  const XrVector3f& left_pos = views[0].pose.position;
  const XrVector3f& right_pos = views[1].pose.position;
  const float ex = (0.5f * (left_pos.x + right_pos.x) - home.x) * s;
  const float ey = (0.5f * (left_pos.y + right_pos.y) - home.y) * s;
  // Same sign convention as GetEyeProjectionRows: positive camera-forward moves the eye forward.
  const float ez = (0.5f * (left_pos.z + right_pos.z) - home.z) * s -
                   camera_forward * s;

  const float c0w = -(c0x * ex + c0y * ey + c0z * ez);
  const float c1w = -(c1x * ex + c1y * ey + c1z * ez);
  // Move the cone apex backwards enough to enclose both eye origins, even for near geometry.
  const float dx = 0.5f * (left_pos.x - right_pos.x) * s;
  const float dy = 0.5f * (left_pos.y - right_pos.y) * s;
  const float dz = 0.5f * (left_pos.z - right_pos.z) * s;
  const float eye_radius = std::sqrt(dx * dx + dy * dy + dz * dz);
  const float padding = eye_radius * std::sqrt(1.0f + focal * focal);
  const float zw = -(r02 * ex + r12 * ey + r22 * ez) - padding;

  (*out_matrix)[0] = {c0x, c0y, c0z, c0w};
  (*out_matrix)[1] = {c1x, c1y, c1z, c1w};
  (*out_matrix)[2] = {r02, r12, r22, zw};
  (*out_matrix)[3] = {-r02, -r12, -r22, -zw};
  return true;
}
}  // namespace VR
