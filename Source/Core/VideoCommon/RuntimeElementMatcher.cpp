// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/RuntimeElementMatcher.h"

#include <cstdlib>

namespace RuntimeElementMatcher
{
namespace
{
constexpr int PROJECTION_FOV_TOLERANCE_X100 = 1;
constexpr int PROJECTION_NEAR_TOLERANCE_X1000 = 1;
constexpr int PROJECTION_FAR_TOLERANCE_X100 = 1;
constexpr int ORTHO_PROJECTION_TOLERANCE_X100 = 1;

bool WithinTolerance(int pattern, int candidate, int tolerance)
{
  return std::abs(pattern - candidate) <= tolerance;
}
}  // namespace

bool HasActiveGroups(const ShaderHunter::RuntimeElementSignature& signature)
{
  return signature.valid &&
         (signature.use_projection || signature.use_layer || signature.use_viewport ||
          signature.use_scissor || signature.use_render_state);
}

bool Matches(const ShaderHunter::RuntimeElementSignature& pattern,
             const ShaderHunter::RuntimeElementSignature& candidate)
{
  if (!pattern.valid || !candidate.valid)
    return false;

  if (pattern.use_projection)
  {
    if (pattern.perspective != candidate.perspective)
      return false;

    if (pattern.perspective)
    {
      if (!WithinTolerance(pattern.perspective_hfov_x100, candidate.perspective_hfov_x100,
                           PROJECTION_FOV_TOLERANCE_X100) ||
          !WithinTolerance(pattern.perspective_vfov_x100, candidate.perspective_vfov_x100,
                           PROJECTION_FOV_TOLERANCE_X100) ||
          !WithinTolerance(pattern.perspective_near_x1000, candidate.perspective_near_x1000,
                           PROJECTION_NEAR_TOLERANCE_X1000) ||
          !WithinTolerance(pattern.perspective_far_x100, candidate.perspective_far_x100,
                           PROJECTION_FAR_TOLERANCE_X100))
      {
        return false;
      }
    }
    else
    {
      if (!WithinTolerance(pattern.ortho_left_x100, candidate.ortho_left_x100,
                           ORTHO_PROJECTION_TOLERANCE_X100) ||
          !WithinTolerance(pattern.ortho_right_x100, candidate.ortho_right_x100,
                           ORTHO_PROJECTION_TOLERANCE_X100) ||
          !WithinTolerance(pattern.ortho_top_x100, candidate.ortho_top_x100,
                           ORTHO_PROJECTION_TOLERANCE_X100) ||
          !WithinTolerance(pattern.ortho_bottom_x100, candidate.ortho_bottom_x100,
                           ORTHO_PROJECTION_TOLERANCE_X100))
      {
        return false;
      }
    }
  }

  if (pattern.use_layer)
  {
    if (candidate.perspective || pattern.perspective)
      return false;
    if (pattern.ortho_layer != candidate.ortho_layer)
      return false;
  }

  if (pattern.use_viewport)
  {
    if (pattern.viewport_x != candidate.viewport_x ||
        pattern.viewport_y != candidate.viewport_y ||
        pattern.viewport_width != candidate.viewport_width ||
        pattern.viewport_height != candidate.viewport_height)
    {
      return false;
    }
  }

  if (pattern.use_scissor)
  {
    if (pattern.scissor_left != candidate.scissor_left ||
        pattern.scissor_top != candidate.scissor_top ||
        pattern.scissor_right != candidate.scissor_right ||
        pattern.scissor_bottom != candidate.scissor_bottom)
    {
      return false;
    }
  }

  if (pattern.use_render_state)
  {
    if (pattern.alpha_test_hex != candidate.alpha_test_hex || pattern.ztest != candidate.ztest ||
        pattern.zupdate != candidate.zupdate || pattern.zfunc != candidate.zfunc ||
        pattern.blend_color_update != candidate.blend_color_update ||
        pattern.blend_alpha_update != candidate.blend_alpha_update)
    {
      return false;
    }
  }

  return true;
}
}  // namespace RuntimeElementMatcher
