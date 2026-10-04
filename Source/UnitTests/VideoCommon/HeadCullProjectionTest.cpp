// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cmath>
#include <limits>

#include <gtest/gtest.h>

#include "VideoCommon/VR/HeadCullProjection.h"

namespace
{
using Matrix = std::array<std::array<float, 4>, 4>;

std::array<XrView, 2> MakeViews()
{
  std::array<XrView, 2> views{};
  for (size_t eye = 0; eye < views.size(); ++eye)
  {
    views[eye].pose.orientation.w = 1.0f;
    views[eye].pose.position.x = eye == 0 ? -0.032f : 0.032f;
    views[eye].fov = {-0.85f, 0.9f, 0.8f, -0.75f};
  }
  return views;
}

bool Contains(const Matrix& matrix, XrVector3f point)
{
  const auto dot = [&](size_t row) {
    return matrix[row][0] * point.x + matrix[row][1] * point.y +
           matrix[row][2] * point.z + matrix[row][3];
  };
  const float w = dot(3);
  return w >= 0.0f && std::abs(dot(0)) <= w + 0.0001f &&
         std::abs(dot(1)) <= w + 0.0001f;
}

XrVector3f Rotate(const XrQuaternionf& q, XrVector3f v)
{
  const XrVector3f t{2 * (q.y * v.z - q.z * v.y), 2 * (q.z * v.x - q.x * v.z),
                    2 * (q.x * v.y - q.y * v.x)};
  return {v.x + q.w * t.x + q.y * t.z - q.z * t.y,
          v.y + q.w * t.y + q.z * t.x - q.x * t.z,
          v.z + q.w * t.z + q.x * t.y - q.y * t.x};
}
}  // namespace

TEST(HeadCullProjection, RejectsBehindAndOutsideCone)
{
  Matrix matrix{};
  ASSERT_TRUE(VR::BuildHeadCullProjection(MakeViews(), {}, 115, 1, 0, 0, &matrix));
  EXPECT_TRUE(Contains(matrix, {0, 0, -10}));
  EXPECT_FALSE(Contains(matrix, {0, 0, 10}));
  EXPECT_FALSE(Contains(matrix, {100, 0, -1}));
}

TEST(HeadCullProjection, FloorsConeAtHeadsetFieldOfView)
{
  Matrix matrix{};
  float degrees = 0;
  ASSERT_TRUE(VR::BuildHeadCullProjection(MakeViews(), {}, 70, 1, 0, 0, &matrix, &degrees));
  EXPECT_GT(degrees, 103.0f);
  ASSERT_TRUE(VR::BuildHeadCullProjection(MakeViews(), {}, 175, 1, 0, 0, &matrix, &degrees));
  EXPECT_FLOAT_EQ(degrees, 175.0f);
}

TEST(HeadCullProjection, ContainsBothEyeFrustaAtNearAndFarDistancesAfterRotation)
{
  for (float yaw : {0.0f, 0.7f, 1.5707963f, 3.1415926f})
  {
    auto views = MakeViews();
    const XrQuaternionf q{0, std::sin(yaw * 0.5f), 0, std::cos(yaw * 0.5f)};
    for (auto& view : views)
    {
      view.pose.orientation = q;
      view.pose.position = Rotate(q, view.pose.position);
      view.pose.position.y += 1.6f;
    }
    Matrix matrix{};
    constexpr float scale = 3.0f;
    ASSERT_TRUE(VR::BuildHeadCullProjection(views, {0, 1.2f, 0}, 70, scale, 0, 0.2f,
                                            &matrix));
    for (const auto& view : views)
    {
      for (float depth : {0.001f, 0.02f, 1.0f, 100.0f})
      {
        for (float horizontal : {view.fov.angleLeft, view.fov.angleRight})
        {
          for (float vertical : {view.fov.angleDown, view.fov.angleUp})
          {
            const auto p = Rotate(q, {std::tan(horizontal) * depth,
                                      std::tan(vertical) * depth, -depth});
            EXPECT_TRUE(Contains(matrix,
                                 {(p.x + view.pose.position.x) * scale,
                                  (p.y + view.pose.position.y - 1.2f) * scale,
                                  (p.z + view.pose.position.z - 0.2f) * scale}));
          }
        }
      }
    }
  }
}

TEST(HeadCullProjection, AppliesLeanBackRotation)
{
  Matrix matrix{};
  ASSERT_TRUE(VR::BuildHeadCullProjection(MakeViews(), {}, 115, 1, 90, 0, &matrix));
  EXPECT_TRUE(Contains(matrix, {0, -10, 0}));
  EXPECT_FALSE(Contains(matrix, {0, 10, 0}));
}

TEST(HeadCullProjection, SkipsInvalidAndCantedViews)
{
  auto views = MakeViews();
  Matrix matrix{};
  EXPECT_FALSE(VR::BuildHeadCullProjection(views, {},
                                          std::numeric_limits<float>::quiet_NaN(), 1, 0, 0,
                                          &matrix));
  views[1].pose.orientation = {0, 0.1f, 0, std::sqrt(0.99f)};
  EXPECT_FALSE(VR::BuildHeadCullProjection(views, {}, 115, 1, 0, 0, &matrix));
  views = MakeViews();
  views[0].fov.angleLeft = -1.57f;
  EXPECT_FALSE(VR::BuildHeadCullProjection(views, {}, 115, 1, 0, 0, &matrix));
}
