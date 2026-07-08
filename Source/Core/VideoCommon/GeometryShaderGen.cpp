// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GeometryShaderGen.h"

#include "Common/CommonTypes.h"
#include "Common/EnumMap.h"
#include "VideoCommon/DriverDetails.h"
#include "VideoCommon/LightingShaderGen.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"

constexpr Common::EnumMap<const char*, PrimitiveType::TriangleStrip> primitives_ogl{
    "points",
    "lines",
    "triangles",
    "triangles",
};
constexpr Common::EnumMap<const char*, PrimitiveType::TriangleStrip> primitives_d3d{
    "point",
    "line",
    "triangle",
    "triangle",
};

constexpr Common::EnumMap<u32, PrimitiveType::TriangleStrip> vertex_in_map{1u, 2u, 3u, 3u};
constexpr Common::EnumMap<u32, PrimitiveType::TriangleStrip> vertex_out_map{4u, 4u, 4u, 3u};
constexpr u32 GEOMETRY_SHADER_CODE_VERSION = 40;

bool geometry_shader_uid_data::IsPassthrough() const
{
  const bool stereo = g_ActiveConfig.stereo_mode != StereoMode::Off;
  const bool wireframe = g_ActiveConfig.bWireFrame;
  const bool triangle = primitive_type >= static_cast<u32>(PrimitiveType::Triangles);
  if (triangle && !stereo && !wireframe)
    return true;
  // VK_KHR_multiview path: stereo expansion is handled at the render-pass level via
  // gl_ViewIndex in the VS. For triangle primitives without wireframe, the GS would be
  // a pure passthrough — skip the stage entirely on the Vulkan multiview path.
  const bool multiview = ShaderHostConfig::GetCurrent().vk_multiview;
  if (triangle && !wireframe && multiview)
    return true;
  return false;
}

GeometryShaderUid GetGeometryShaderUid(PrimitiveType primitive_type)
{
  GeometryShaderUid out;

  geometry_shader_uid_data* const uid_data = out.GetUidData();
  uid_data->code_version = GEOMETRY_SHADER_CODE_VERSION;
  uid_data->primitive_type = static_cast<u32>(primitive_type);
  uid_data->numTexGens = xfmem.numTexGen.numTexGens;

  return out;
}

static void EmitVertex(ShaderCode& out, const ShaderHostConfig& host_config,
                       const geometry_shader_uid_data* uid_data, const char* vertex,
                       APIType api_type, bool wireframe, bool stereo, bool first_vertex = false);
static void EndPrimitive(ShaderCode& out, const ShaderHostConfig& host_config,
                         const geometry_shader_uid_data* uid_data, APIType api_type, bool wireframe,
                         bool stereo);

ShaderCode GenerateGeometryShaderCode(APIType api_type, const ShaderHostConfig& host_config,
                                      const geometry_shader_uid_data* uid_data)
{
  ShaderCode out;
  // Non-uid template parameters will write to the dummy data (=> gets optimized out)

  const bool wireframe = host_config.wireframe;
  const bool msaa = host_config.msaa;
  const bool ssaa = host_config.ssaa;
  // Under VK_KHR_multiview, the render-pass replicates each draw to both layers via
  // gl_ViewIndex. The VS handles per-eye projection, so the GS must NOT duplicate the
  // primitive — treat stereo as off inside the GS body. This keeps line/point/wireframe
  // expansion working while skipping the eye-loop and the manual gl_Layer write.
  const bool stereo = host_config.stereo && !host_config.vk_multiview;
  const auto primitive_type = static_cast<PrimitiveType>(uid_data->primitive_type);
  const u32 vertex_in = vertex_in_map[primitive_type];
  u32 vertex_out = vertex_out_map[primitive_type];

  if (wireframe)
    vertex_out++;

  if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
  {
    // Insert layout parameters
    if (host_config.backend_gs_instancing)
    {
      out.Write("layout({}, invocations = {}) in;\n", primitives_ogl[primitive_type],
                stereo ? 2 : 1);
      out.Write("layout({}_strip, max_vertices = {}) out;\n", wireframe ? "line" : "triangle",
                vertex_out);
    }
    else
    {
      out.Write("layout({}) in;\n", primitives_ogl[primitive_type]);
      out.Write("layout({}_strip, max_vertices = {}) out;\n", wireframe ? "line" : "triangle",
                stereo ? vertex_out * 2 : vertex_out);
    }
  }

  out.Write("{}", s_lighting_struct);

  // uniforms
  if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
    out.Write("UBO_BINDING(std140, 4) uniform GSBlock {{\n");
  else
    out.Write("cbuffer GSBlock {{\n");

  out.Write("{}", s_geometry_shader_uniforms);
  out.Write("}};\n");

  out.Write("struct VS_OUTPUT {{\n");
  GenerateVSOutputMembers(out, api_type, uid_data->numTexGens, host_config, "",
                          ShaderStage::Geometry);
  out.Write("}};\n");

  if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
  {
    if (host_config.backend_gs_instancing)
      out.Write("#define InstanceID gl_InvocationID\n");

    out.Write("VARYING_LOCATION(0) in VertexData {{\n");
    GenerateVSOutputMembers(out, api_type, uid_data->numTexGens, host_config,
                            GetInterpolationQualifier(msaa, ssaa, true, true),
                            ShaderStage::Geometry);
    out.Write("}} vs[{}];\n", vertex_in);

    out.Write("VARYING_LOCATION(0) out VertexData {{\n");
    GenerateVSOutputMembers(out, api_type, uid_data->numTexGens, host_config,
                            GetInterpolationQualifier(msaa, ssaa, true, false),
                            ShaderStage::Geometry);

    out.Write("}} ps;\n");
    if (stereo && !host_config.backend_gl_layer_in_fs)
      out.Write("flat out int vr_eye_layer;\n");

    out.Write("void main()\n{{\n");
  }
  else  // D3D
  {
    out.Write("struct VertexData {{\n");
    out.Write("\tVS_OUTPUT o;\n");

    if (stereo)
    {
      const u32 eye_layer_location =
          GetVREyeLayerLocation(api_type, uid_data->numTexGens, host_config);
      out.Write("\tuint layer : SV_RenderTargetArrayIndex;\n");
      out.Write("\tuint eye_layer : TEXCOORD{};\n", eye_layer_location);
    }
    out.Write("\tfloat4 posout : SV_Position;\n");

    out.Write("}};\n");

    if (host_config.backend_gs_instancing)
    {
      out.Write("[maxvertexcount({})]\n[instance({})]\n", vertex_out, stereo ? 2 : 1);
      out.Write("void main({} VS_OUTPUT o[{}], inout {}Stream<VertexData> output, in uint "
                "InstanceID : SV_GSInstanceID)\n{{\n",
                primitives_d3d[primitive_type], vertex_in, wireframe ? "Line" : "Triangle");
    }
    else
    {
      out.Write("[maxvertexcount({})]\n", stereo ? vertex_out * 2 : vertex_out);
      out.Write("void main({} VS_OUTPUT o[{}], inout {}Stream<VertexData> output)\n{{\n",
                primitives_d3d[primitive_type], vertex_in, wireframe ? "Line" : "Triangle");
    }

    out.Write("\tVertexData ps;\n");
  }

  if (primitive_type == PrimitiveType::Lines)
  {
    if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
    {
      out.Write("\tVS_OUTPUT start, end;\n");
      AssignVSOutputMembers(out, "start", "vs[0]", uid_data->numTexGens, host_config);
      AssignVSOutputMembers(out, "end", "vs[1]", uid_data->numTexGens, host_config);
    }
    else
    {
      out.Write("\tVS_OUTPUT start = o[0];\n"
                "\tVS_OUTPUT end = o[1];\n");
    }

    GenerateLineOffset(out, "\t", "\t\t", "end.pos", "start.pos", "");
  }
  else if (primitive_type == PrimitiveType::Points)
  {
    if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
    {
      out.Write("\tVS_OUTPUT center;\n");
      AssignVSOutputMembers(out, "center", "vs[0]", uid_data->numTexGens, host_config);
    }
    else
    {
      out.Write("\tVS_OUTPUT center = o[0];\n");
    }

    // Offset from center to upper right vertex
    // Lerp PointSize/2 from [0,0..VpWidth,VpHeight] to [-1,1..1,-1]
    out.Write("\tfloat2 offset = float2(" I_LINEPTPARAMS ".w / " I_LINEPTPARAMS
              ".x, -" I_LINEPTPARAMS ".w / " I_LINEPTPARAMS ".y) * center.pos.w;\n");
  }

  if (stereo)
  {
    // If the GPU supports invocation we don't need a for loop and can simply use the
    // invocation identifier to determine which layer we're rendering.
    if (host_config.backend_gs_instancing)
      out.Write("\tint eye = InstanceID;\n");
    else
      out.Write("\tfor (int eye = 0; eye < 2; ++eye) {{\n");
  }

  if (wireframe)
    out.Write("\tVS_OUTPUT first;\n");

  // Avoid D3D warning about forced unrolling of single-iteration loop
  if (vertex_in > 1)
    out.Write("\tfor (int i = 0; i < {}; ++i) {{\n", vertex_in);
  else
    out.Write("\tint i = 0;\n");

  if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
  {
    out.Write("\tVS_OUTPUT f;\n");
    AssignVSOutputMembers(out, "f", "vs[i]", uid_data->numTexGens, host_config);

    if (host_config.backend_depth_clamp &&
        DriverDetails::HasBug(DriverDetails::BUG_BROKEN_CLIP_DISTANCE))
    {
      // On certain GPUs we have to consume the clip distance from the vertex shader
      // or else the other vertex shader outputs will get corrupted.
      out.Write("\tf.clipDist0 = gl_in[i].gl_ClipDistance[0];\n"
                "\tf.clipDist1 = gl_in[i].gl_ClipDistance[1];\n");
    }
  }
  else
  {
    out.Write("\tVS_OUTPUT f = o[i];\n");
  }

  if (stereo)
  {
    if (host_config.vr_stereo)
    {
      // OpenXR head-tracked per-eye projection.
      // cstereo.w is the perspective flag set by GeometryShaderManager:
      //   1.0 = perspective draw - apply head-tracked per-eye HMD projection
      //   0.25 = fullscreen mono override - leave f.pos from VS untouched
      //   0.0 = orthographic/fullscreen draw - leave f.pos from VS untouched
      //
      // ceye_proj has head rotation and eye position baked in:
      //   x_clip = dot(ceye_proj[eye*2],   viewPos)
      //   y_clip = dot(ceye_proj[eye*2+1], viewPos)
      //
      // cvr_eye_z gives eye-space depth for correct perspective divide and depth:
      //   z_eye = dot(cvr_eye_z[eye], viewPos)
      //   f.pos.w = -z_eye,  f.pos.z = P[2][2]*z_eye + P[2][3]
      out.Write("\tif (" I_STEREOPARAMS ".w > 0.5f)\n");
      out.Write("\t{{\n");
      if (api_type == APIType::Vulkan)
      {
        out.Write("\t\tif (" I_STEREOPARAMS ".y > 0.5f)\n");
        out.Write("\t\t{{\n");
        out.Write("\t\t\tfloat4 eye_proj_x = " I_LEGACY_EYE_PROJ_X "[eye];\n");
        out.Write("\t\t\tfloat4 eye_proj_y = " I_LEGACY_EYE_PROJ_Y "[eye];\n");
        out.Write(
            "\t\t\tf.pos.x = eye_proj_x.z * f.pos.x + eye_proj_x.x - eye_proj_x.y * f.pos.w;\n");
        out.Write(
            "\t\t\tf.pos.y = eye_proj_y.z * f.pos.y + eye_proj_y.x - eye_proj_y.y * f.pos.w;\n");
        if (!host_config.fast_depth_calc)
        {
          out.Write(
              "\t\t\tf.clipPos.x = eye_proj_x.z * f.clipPos.x + eye_proj_x.x - eye_proj_x.y * "
              "f.clipPos.w;\n");
          out.Write(
              "\t\t\tf.clipPos.y = eye_proj_y.z * f.clipPos.y + eye_proj_y.x - eye_proj_y.y * "
              "f.clipPos.w;\n");
        }
        out.Write("\t\t}}\n");
        out.Write("\t\telse\n");
        out.Write("\t\t{{\n");
      }
      out.Write("\t\tfloat4 row0 = " I_EYE_PROJ "[eye * 2 + 0];\n");
      out.Write("\t\tfloat4 row1 = " I_EYE_PROJ "[eye * 2 + 1];\n");
      out.Write("\t\tfloat4 zrow = " I_VR_EYE_Z "[eye];\n");
      // Mirror the world X when the game's projection flips X (stereoparams.x = -1).
      // This must happen BEFORE the VR projection so the per-eye IPD offset (baked
      // into the projection rows' w component) is not affected — only the world
      // geometry is mirrored.  This preserves correct stereo while reversing the
      // triangle winding to match the game's adjusted cull mode.
      out.Write("\t\tfloat4 vp = f.viewPos;\n");
      out.Write("\t\tvp.x *= " I_STEREOPARAMS ".x;\n");
      out.Write("\t\tvp.w *= " I_STEREOPARAMS ".z;\n");
      out.Write("\t\tfloat z_eye = dot(zrow, vp);\n");
      out.Write("\t\tfloat clip_x = dot(row0, vp);\n");
      out.Write("\t\tfloat clip_y = dot(row1, vp);\n");
      out.Write("\t\tfloat clip_w = -z_eye;\n");
      out.Write("\t\tfloat clip_z = " I_VR_DEPTH ".x * z_eye + " I_VR_DEPTH ".y;\n");
      if (!host_config.fast_depth_calc)
        out.Write("\t\tf.clipPos = float4(clip_x, clip_y, clip_z, clip_w);\n");
      out.Write("\t\tf.pos = float4(clip_x, clip_y, clip_z, clip_w);\n");
      out.Write("\t\tf.pos.z = f.pos.w * " I_VR_DEPTH ".w - f.pos.z * " I_VR_DEPTH ".z;\n");
      if (!host_config.backend_clip_control)
        out.Write("\t\tf.pos.z = f.pos.z * 2.0 - f.pos.w;\n");
      out.Write("\t\tf.pos.xy *= sign(" I_VR_PIXELCENTER ".xy * float2(1.0, -1.0));\n");
      out.Write("\t\tf.pos.xy = f.pos.xy - f.pos.w * " I_VR_PIXELCENTER ".xy;\n");
      // VR replaces pos entirely — recalculate clip distances for the new position.
      if (host_config.backend_depth_clamp)
      {
        if (api_type == APIType::Vulkan)
          out.Write("\t\tf.clipDist0 = 1.0;\n\t\tf.clipDist1 = 1.0;\n");
        else
          out.Write("\t\tf.clipDist0 = f.pos.z + f.pos.w;\n\t\tf.clipDist1 = -f.pos.z;\n");
      }
      if (api_type == APIType::Vulkan)
        out.Write("\t\t}}\n");
      out.Write("\t}}\n");

      // Head-locked perspective HUD: preserve the game's original perspective GUI/model
      // geometry, then place the transformed object in head-locked HMD space.
      out.Write("\telse if (" I_STEREOPARAMS ".w < -2.5f)\n");
      out.Write("\t{{\n");
      out.Write("\t\tfloat hud_game_w = (abs(f.pos.w) > 1.0e-6) ? f.pos.w : 1.0e-6;\n");
      out.Write("\t\tfloat hud_game_ndc_z = clamp(f.pos.z / hud_game_w, -1.0, 1.0);\n");
      out.Write("\t\tfloat4 hudPos = float4(\n");
      out.Write("\t\t\tf.viewPos.x * " I_HEAD_PARAMS ".y + " I_VR_SCREEN ".x,\n");
      out.Write("\t\t\tf.viewPos.y * " I_HEAD_PARAMS ".z + " I_VR_SCREEN ".y,\n");
      out.Write("\t\t\tf.viewPos.z * " I_HEAD_PARAMS ".w + " I_VR_SCREEN ".z,\n");
      out.Write("\t\t\t1.0);\n");
      out.Write("\t\thudPos.z = min(hudPos.z, -0.1 * max(" I_VR_DEPTH ".w, 0.001));\n");
      out.Write("\t\tfloat4 row0 = " I_HEAD_PROJ "[eye * 2 + 0];\n");
      out.Write("\t\tfloat4 row1 = " I_HEAD_PROJ "[eye * 2 + 1];\n");
      out.Write("\t\tf.pos.x = dot(row0, hudPos);\n");
      out.Write("\t\tf.pos.y = dot(row1, hudPos);\n");
      out.Write("\t\tf.pos.w = max(-hudPos.z, 0.001);\n");
      out.Write("\t\tf.pos.z = hud_game_ndc_z * f.pos.w;\n");
      if (!host_config.fast_depth_calc)
        out.Write("\t\tf.clipPos = f.pos;\n");
      out.Write("\t\tf.pos.xy *= sign(" I_VR_PIXELCENTER ".xy * float2(1.0, -1.0));\n");
      out.Write("\t\tf.pos.xy = f.pos.xy - f.pos.w * " I_VR_PIXELCENTER ".xy;\n");
      if (host_config.backend_depth_clamp)
      {
        if (api_type == APIType::Vulkan || api_type == APIType::OpenGL)
          out.Write("\t\tf.clipDist0 = 1.0;\n\t\tf.clipDist1 = 1.0;\n");
        else
          out.Write("\t\tf.clipDist0 = f.pos.z + f.pos.w;\n\t\tf.clipDist1 = -f.pos.z;\n");
      }
      out.Write("\t}}\n");

      // Head-locked VR: 2D content on a virtual screen that follows head movements.
      // cstereo.w == -2.0 signals head-locked draw.
      // Uses unrotated (raw) per-eye projection — no head rotation baked in.
      out.Write("\telse if (" I_STEREOPARAMS ".w < -1.5f)\n");
      out.Write("\t{{\n");
      if (api_type == APIType::Vulkan)
      {
        out.Write("\t\tfloat safe_w = (abs(f.pos.w) > 1.0e-5) ? f.pos.w : "
                  "((f.pos.w < 0.0) ? -1.0e-5 : 1.0e-5);\n");
        out.Write("\t\tfloat ndc_x = clamp(f.pos.x / safe_w, -1.0, 1.0);\n");
        out.Write("\t\tfloat ndc_y = clamp(f.pos.y / safe_w, -1.0, 1.0);\n");
        out.Write("\t\tfloat ndc_z = clamp(f.pos.z / safe_w, -1.0, 1.0);\n");
      }
      else
      {
        out.Write("\t\tfloat ndc_x = f.pos.x / f.pos.w;\n");
        out.Write("\t\tfloat ndc_y = f.pos.y / f.pos.w;\n");
        out.Write("\t\tfloat ndc_z = f.pos.z / f.pos.w;\n");
      }
      out.Write("\t\tfloat ndc_z_clamped = clamp(ndc_z, -1.0, 1.0);\n");
      out.Write("\t\tfloat4 screenPos = float4(\n");
      out.Write("\t\t\tndc_x * " I_VR_SCREEN ".x,\n");
      out.Write("\t\t\tndc_y * " I_VR_SCREEN ".y,\n");
      out.Write("\t\t\t-" I_VR_SCREEN ".z + ndc_z_clamped * " I_VR_DEPTH ".z,\n");
      out.Write("\t\t\t1.0);\n");
      out.Write("\t\tfloat curve = max(" I_HEAD_PARAMS ".x, 0.0);\n");
      out.Write("\t\tfloat horizontal = 0.5 * (ndc_x * ndc_x);\n");
      out.Write("\t\tfloat curve_push = curve * horizontal * " I_VR_SCREEN ".z * 0.25;\n");
      out.Write("\t\tcurve_push = min(curve_push, " I_VR_SCREEN ".z * 0.8);\n");
      out.Write("\t\tscreenPos.z += curve_push;\n");
      out.Write("\t\tfloat4 row0 = " I_HEAD_PROJ "[eye * 2 + 0];\n");
      out.Write("\t\tfloat4 row1 = " I_HEAD_PROJ "[eye * 2 + 1];\n");
      out.Write("\t\tf.pos.x = dot(row0, screenPos);\n");
      out.Write("\t\tf.pos.y = dot(row1, screenPos);\n");
      out.Write("\t\tf.pos.w = max(-screenPos.z, 0.001);\n");
      out.Write("\t\tfloat layer_idx = max(" I_VR_SCREEN ".w, 0.0);\n");
      out.Write("\t\tfloat layer_step = max(" I_VR_DEPTH ".x, 0.0);\n");
      out.Write(
          "\t\tfloat max_safe_step = 0.49 / max(layer_idx + 1.0, 1.0);\n");
      out.Write("\t\tlayer_step = min(layer_step, max_safe_step);\n");
      out.Write(
          "\t\tf.pos.z = f.pos.w * (0.5 - layer_idx * layer_step + ndc_z_clamped * 0.0001);\n");
      if (!host_config.fast_depth_calc)
        out.Write("\t\tf.clipPos = f.pos;\n");
      if (!host_config.backend_clip_control)
        out.Write("\t\tf.pos.z = f.pos.z * 2.0 - f.pos.w;\n");
      out.Write("\t\tf.pos.xy *= sign(" I_VR_PIXELCENTER ".xy * float2(1.0, -1.0));\n");
      out.Write("\t\tf.pos.xy = f.pos.xy - f.pos.w * " I_VR_PIXELCENTER ".xy;\n");
      if (host_config.backend_depth_clamp)
      {
        if (api_type == APIType::Vulkan)
          out.Write("\t\tf.clipDist0 = 1.0;\n\t\tf.clipDist1 = 1.0;\n");
        else
          out.Write("\t\tf.clipDist0 = f.pos.z + f.pos.w;\n\t\tf.clipDist1 = -f.pos.z;\n");
      }
      out.Write("\t}}\n");

      // Orthographic VR: place 2D content (menus, FMV, HUD) on a virtual screen plane.
      // cstereo.w == -1.0 signals orthographic draw with VR active.
      // Map NDC from the ortho projection to a 3D position on the virtual screen,
      // then re-project through the per-eye HMD projection (world-fixed).
      out.Write("\telse if (" I_STEREOPARAMS ".w < -0.5f)\n");
      out.Write("\t{{\n");
      if (api_type == APIType::Vulkan)
      {
        out.Write("\t\tfloat safe_w = (abs(f.pos.w) > 1.0e-5) ? f.pos.w : "
                  "((f.pos.w < 0.0) ? -1.0e-5 : 1.0e-5);\n");
        out.Write("\t\tfloat ndc_x = clamp(f.pos.x / safe_w, -1.0, 1.0);\n");
        out.Write("\t\tfloat ndc_y = clamp(f.pos.y / safe_w, -1.0, 1.0);\n");
        out.Write("\t\tfloat ndc_z = clamp(f.pos.z / safe_w, -1.0, 1.0);\n");
      }
      else
      {
        out.Write("\t\tfloat ndc_x = f.pos.x / f.pos.w;\n");
        out.Write("\t\tfloat ndc_y = f.pos.y / f.pos.w;\n");
        out.Write("\t\tfloat ndc_z = f.pos.z / f.pos.w;\n");
      }
      out.Write("\t\tfloat ndc_z_clamped = clamp(ndc_z, -1.0, 1.0);\n");
      // cvr_screen = {half_w, half_h, distance, ortho_layer}
      out.Write("\t\tfloat4 screenPos = float4(\n");
      out.Write("\t\t\tndc_x * " I_VR_SCREEN ".x,\n");
      out.Write("\t\t\tndc_y * " I_VR_SCREEN ".y,\n");
      out.Write("\t\t\t-" I_VR_SCREEN ".z + ndc_z_clamped * " I_VR_DEPTH ".z,\n");
      out.Write("\t\t\t1.0);\n");
      out.Write("\t\tfloat4 row0 = " I_EYE_PROJ "[eye * 2 + 0];\n");
      out.Write("\t\tfloat4 row1 = " I_EYE_PROJ "[eye * 2 + 1];\n");
      out.Write("\t\tfloat4 zrow = " I_VR_EYE_Z "[eye];\n");
      out.Write("\t\tfloat z_eye = dot(zrow, screenPos);\n");
      out.Write("\t\tfloat clip_x = dot(row0, screenPos);\n");
      out.Write("\t\tfloat clip_y = dot(row1, screenPos);\n");
      out.Write("\t\tfloat clip_w = -z_eye;\n");
      if (!host_config.fast_depth_calc)
        out.Write("\t\tf.clipPos = float4(clip_x, clip_y, 0.0, clip_w);\n");
      out.Write("\t\tf.pos = float4(clip_x, clip_y, 0.0, clip_w);\n");
      // Depth: use per-draw layer counter to spread ortho elements apart.
      // Later draws (higher layer index) get smaller z = closer to camera.
      // cvr_depth.x = layer offset (between draws); cvr_depth.y = element depth (within draw).
      out.Write("\t\tfloat layer_idx = max(" I_VR_SCREEN ".w, 0.0);\n");
      out.Write("\t\tfloat layer_step = max(" I_VR_DEPTH ".x, 0.0);\n");
      out.Write(
          "\t\tfloat max_safe_step = 0.49 / max(layer_idx + 1.0, 1.0);\n");
      out.Write("\t\tlayer_step = min(layer_step, max_safe_step);\n");
      out.Write(
          "\t\tf.pos.z = f.pos.w * (0.5 - layer_idx * layer_step + ndc_z_clamped * " I_VR_DEPTH
          ".y);\n");
      if (!host_config.backend_clip_control)
        out.Write("\t\tf.pos.z = f.pos.z * 2.0 - f.pos.w;\n");
      out.Write("\t\tf.pos.xy *= sign(" I_VR_PIXELCENTER ".xy * float2(1.0, -1.0));\n");
      out.Write("\t\tf.pos.xy = f.pos.xy - f.pos.w * " I_VR_PIXELCENTER ".xy;\n");
      if (host_config.backend_depth_clamp)
      {
        if (api_type == APIType::Vulkan)
          out.Write("\t\tf.clipDist0 = 1.0;\n\t\tf.clipDist1 = 1.0;\n");
        else
          out.Write("\t\tf.clipDist0 = f.pos.z + f.pos.w;\n\t\tf.clipDist1 = -f.pos.z;\n");
      }
      out.Write("\t}}\n");

    }
    else
    {
      // For stereoscopy add a small horizontal offset in Normalized Device Coordinates
      // proportional to the depth of the vertex. We retrieve the depth value from the
      // w-component of the projected vertex which contains the negated z-component of
      // the original vertex.
      // For negative parallax (out-of-screen effects) we subtract a convergence value
      // from the depth value. This results in objects at a distance smaller than the
      // convergence distance to seemingly appear in front of the screen.
      // This formula is based on page 13 of the "Nvidia 3D Vision Automatic, Best Practices Guide"
      out.Write("\tfloat hoffset = (eye == 0) ? " I_STEREOPARAMS ".x : " I_STEREOPARAMS ".y;\n");
      out.Write("\tf.pos.x += hoffset * (f.pos.w - " I_STEREOPARAMS ".z);\n");
    }
  }

  if (primitive_type == PrimitiveType::Lines)
  {
    out.Write("\tVS_OUTPUT l = f;\n"
              "\tVS_OUTPUT r = f;\n");

    out.Write("\tl.pos.xy -= offset * l.pos.w;\n"
              "\tr.pos.xy += offset * r.pos.w;\n");

    out.Write("\tif (" I_TEXOFFSET "[2] != 0) {{\n");
    out.Write("\tfloat texOffset = 1.0 / float(" I_TEXOFFSET "[2]);\n");

    for (u32 i = 0; i < uid_data->numTexGens; ++i)
    {
      out.Write("\tif (((" I_TEXOFFSET "[0] >> {}) & 0x1) != 0)\n", i);
      out.Write("\t\tr.tex{}.x += texOffset;\n", i);
    }
    out.Write("\t}}\n");

    EmitVertex(out, host_config, uid_data, "l", api_type, wireframe, stereo, true);
    EmitVertex(out, host_config, uid_data, "r", api_type, wireframe, stereo);
  }
  else if (primitive_type == PrimitiveType::Points)
  {
    out.Write("\tVS_OUTPUT ll = f;\n"
              "\tVS_OUTPUT lr = f;\n"
              "\tVS_OUTPUT ul = f;\n"
              "\tVS_OUTPUT ur = f;\n");

    out.Write("\tll.pos.xy += float2(-1,-1) * offset;\n"
              "\tlr.pos.xy += float2(1,-1) * offset;\n"
              "\tul.pos.xy += float2(-1,1) * offset;\n"
              "\tur.pos.xy += offset;\n");

    out.Write("\tif (" I_TEXOFFSET "[3] != 0) {{\n");
    out.Write("\tfloat2 texOffset = float2(1.0 / float(" I_TEXOFFSET
              "[3]), 1.0 / float(" I_TEXOFFSET "[3]));\n");

    for (u32 i = 0; i < uid_data->numTexGens; ++i)
    {
      out.Write("\tif (((" I_TEXOFFSET "[1] >> {}) & 0x1) != 0) {{\n", i);
      out.Write("\t\tul.tex{}.xy += float2(0,1) * texOffset;\n", i);
      out.Write("\t\tur.tex{}.xy += texOffset;\n", i);
      out.Write("\t\tlr.tex{}.xy += float2(1,0) * texOffset;\n", i);
      out.Write("\t}}\n");
    }
    out.Write("\t}}\n");

    EmitVertex(out, host_config, uid_data, "ll", api_type, wireframe, stereo, true);
    EmitVertex(out, host_config, uid_data, "lr", api_type, wireframe, stereo);
    EmitVertex(out, host_config, uid_data, "ul", api_type, wireframe, stereo);
    EmitVertex(out, host_config, uid_data, "ur", api_type, wireframe, stereo);
  }
  else
  {
    EmitVertex(out, host_config, uid_data, "f", api_type, wireframe, stereo, true);
  }

  // Only close loop if previous code was in one (See D3D warning above)
  if (vertex_in > 1)
    out.Write("\t}}\n");

  EndPrimitive(out, host_config, uid_data, api_type, wireframe, stereo);

  if (stereo && !host_config.backend_gs_instancing)
    out.Write("\t}}\n");

  out.Write("}}\n");

  return out;
}

static void EmitVertex(ShaderCode& out, const ShaderHostConfig& host_config,
                       const geometry_shader_uid_data* uid_data, const char* vertex,
                       APIType api_type, bool wireframe, bool stereo, bool first_vertex)
{
  if (wireframe && first_vertex)
    out.Write("\tif (i == 0) first = {};\n", vertex);

  if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
  {
    // Vulkan NDC space has Y pointing down (right-handed NDC space).
    if (api_type == APIType::Vulkan)
      out.Write("\tgl_Position = float4({0}.pos.x, -{0}.pos.y, {0}.pos.z, {0}.pos.w);\n", vertex);
    else
      out.Write("\tgl_Position = {}.pos;\n", vertex);

    if (host_config.backend_depth_clamp)
    {
      out.Write("\tgl_ClipDistance[0] = {}.clipDist0;\n", vertex);
      out.Write("\tgl_ClipDistance[1] = {}.clipDist1;\n", vertex);
    }
    AssignVSOutputMembers(out, "ps", vertex, uid_data->numTexGens, host_config);
  }
  else
  {
    out.Write("\tps.o = {};\n", vertex);
    out.Write("\tps.posout = {}.pos;\n", vertex);
  }

  if (stereo)
  {
    if (api_type == APIType::D3D && uid_data->numTexGens > 0)
    {
      out.Write("\tif ((" I_STEREOPARAMS ".w > 0.2f && " I_STEREOPARAMS
                ".w < 0.3f) || " I_STEREOPARAMS ".w < -2.1f)\n");
      out.Write("\t\tps.o.tex0.z = float(eye);\n");
    }

    // Select the output layer
    if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
      out.Write("\tgl_Layer = eye;\n");
    else
    {
      out.Write("\tps.layer = eye;\n");
      out.Write("\tps.eye_layer = eye;\n");
    }
    if (!host_config.backend_gl_layer_in_fs)
      out.Write("\tvr_eye_layer = eye;\n");
  }

  if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
    out.Write("\tEmitVertex();\n");
  else
    out.Write("\toutput.Append(ps);\n");
}

static void EndPrimitive(ShaderCode& out, const ShaderHostConfig& host_config,
                         const geometry_shader_uid_data* uid_data, APIType api_type, bool wireframe,
                         bool stereo)
{
  if (wireframe)
    EmitVertex(out, host_config, uid_data, "first", api_type, wireframe, stereo);

  if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
    out.Write("\tEndPrimitive();\n");
  else
    out.Write("\toutput.RestartStrip();\n");
}

void EnumerateGeometryShaderUids(const std::function<void(const GeometryShaderUid&)>& callback)
{
  GeometryShaderUid uid;

  const std::array<PrimitiveType, 3> primitive_lut = {
      {g_backend_info.bSupportsPrimitiveRestart ? PrimitiveType::TriangleStrip :
                                                  PrimitiveType::Triangles,
       PrimitiveType::Lines, PrimitiveType::Points}};
  for (PrimitiveType primitive : primitive_lut)
  {
    geometry_shader_uid_data* const guid = uid.GetUidData();
    guid->code_version = GEOMETRY_SHADER_CODE_VERSION;
    guid->primitive_type = static_cast<u32>(primitive);

    for (u32 texgens = 0; texgens <= 8; texgens++)
    {
      guid->numTexGens = texgens;
      callback(uid);
    }
  }
}
