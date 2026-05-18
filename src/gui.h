#pragma once

#include "imgui.h"
#include "settings.h"
#include "dolphin_memory.h"
#include "tracking_math.h"

#include <algorithm>
#include <atomic>
#include <string>

struct AppState {
    bool  active        = false;
    bool  dolphin_ok    = false;
    bool  tracking_ok   = false;
    float fps           = 0.0f;
    float tracker_fps   = 0.0f;
    float tracker_dt_ms = 0.0f;
    float tracker_poll_ms = 0.0f;
    float writer_dt_ms  = 0.0f;
    float writer_work_ms = 0.0f;
    float writer_write_ms = 0.0f;
    int   tracker_drop_count = 0;
    int   writer_drop_count = 0;
    int   tracker_hist_head = 0;
    int   writer_hist_head = 0;
    float tracker_hist_ms[16] = {};
    float writer_hist_ms[16] = {};
    Matrix3x4 last_matrix = {};
    Pose  last_pose     = {};
    std::string dolphin_status;
    std::string hook_status = "Hook: waiting for Dolphin";
    std::string tracking_status;
    bool  game_rev0_ok = false;
    std::string game_status = "Game: not checked";
    uint32_t dbg_state_mgr  = 0;
    uint32_t dbg_player     = 0;
    uint32_t dbg_pitch_addr = 0;
    uint64_t dbg_mem_base   = 0;
    uint32_t dbg_cam_mgr    = 0;
    uint32_t dbg_fp_cam     = 0;
    uint32_t dbg_gun_ptr    = 0;
    uint32_t dbg_gun_xf     = 0;
    uint32_t dbg_beam_xf    = 0;
    uint32_t dbg_world_xf   = 0;
    uint32_t dbg_local_xf   = 0;
    bool     dbg_cannon_rot_valid = false;
    uint32_t dbg_cannon_rot_addr[9] = {};
    float    dbg_cannon_rot_pre[9] = {};
    float    dbg_cannon_rot_post[9] = {};
    float    dbg_cannon_rot_target[9] = {};
    float    dbg_cannon_rot_drift[9] = {};
    float    dbg_cannon_rot_max_drift = 0.0f;
    bool     dbg_cannon_rot_game_changed = false;
    int      dbg_cannon_rot_log_count = 0;
    float    dbg_player_yaw_deg = 0.0f;
    float    dbg_player_yaw_delta_deg = 0.0f;
    uint16_t dbg_gun_target_uid = 0xffff;
    uint32_t dbg_gun_target_obj = 0;
    float    dbg_gun_target_score = 0.0f;
    float    dbg_gun_target_along = 0.0f;
    float    dbg_gun_target_perp = 0.0f;
    int      dbg_gun_target_candidates = 0;
    bool     dbg_gun_target_write = false;
    bool     dbg_xr_dpad_active = false;
    int      dbg_xr_dpad_dir = 0;
    int      dbg_left_stick_axis = 0;
    float    dbg_left_stick_x = 0.0f;
    float    dbg_left_stick_y = 0.0f;
    float    dbg_left_to_head_dist = 0.0f;
    float    dbg_left_to_head_y = 0.0f;
    bool     dbg_directional_move_active = false;
    float    dbg_directional_move_yaw_deg = 0.0f;
    float    dbg_directional_move_stick_mag = 0.0f;
    std::atomic<bool> manually_paused = false;
    std::atomic<bool> recenter_requested = true;
    std::atomic<bool> reconnect_dolphin_requested = false;
    std::atomic<bool> reconnect_tracking_requested = false;
    std::atomic<bool> remap_dolphin_controls_requested = false;
    std::atomic<bool> dolphin_performance_apply_requested = false;
    ImTextureID controller_layout_texture = ImTextureID_Invalid;
    int controller_layout_width = 0;
    int controller_layout_height = 0;
};

inline void draw_gui(Settings& s, AppState& app, DolphinMemory& dolphin)
{
    (void)dolphin;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    constexpr float base_width = 700.0f;
    constexpr float base_height = 720.0f;
    const float deck_width = std::max(base_width, viewport->WorkSize.x);
    const float deck_height = std::max(base_height, viewport->WorkSize.y);
    const float ui_scale = std::clamp(
        std::min(deck_width / base_width, deck_height / base_height),
        1.0f, 2.0f);
    const auto scaled = [ui_scale](float value) { return value * ui_scale; };
    const float logical_width = deck_width / ui_scale;
    const bool compact = logical_width < 650.0f;
    const float label_column = scaled(compact ? 145.0f : 190.0f);
    const float input_width = scaled(compact ? 96.0f : 112.0f);
    const float header_height = scaled(compact ? 208.0f : 182.0f);
    const float footer_height = scaled(48.0f);

    const ImVec4 accent = ImVec4(0.95f, 0.64f, 0.22f, 1.0f);
    const ImVec4 ok = ImVec4(0.20f, 0.78f, 0.44f, 1.0f);
    const ImVec4 warn = ImVec4(0.95f, 0.34f, 0.28f, 1.0f);
    const ImVec4 muted = ImVec4(0.58f, 0.62f, 0.67f, 1.0f);

    ImGui::SetNextWindowPos(viewport->WorkPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(deck_width, deck_height), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(scaled(14.0f), scaled(12.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(scaled(9.0f), scaled(5.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(scaled(9.0f), scaled(7.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, scaled(13.0f));
    ImGui::Begin("PrimedGun Control Deck", nullptr,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);
    ImGui::SetWindowFontScale(ui_scale);

    auto status_pill = [&](const char* label, const char* value, bool healthy) {
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float badge_width = scaled(116.0f);
        const float badge_height = scaled(24.0f);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImU32 fill = ImGui::ColorConvertFloat4ToU32(
            healthy ? ImVec4(0.10f, 0.30f, 0.18f, 1.0f) : ImVec4(0.34f, 0.12f, 0.11f, 1.0f));
        const ImU32 border = ImGui::ColorConvertFloat4ToU32(
            healthy ? ImVec4(0.18f, 0.58f, 0.32f, 1.0f) : ImVec4(0.70f, 0.22f, 0.18f, 1.0f));
        draw->AddRectFilled(start, ImVec2(start.x + badge_width, start.y + badge_height), fill, scaled(5.0f));
        draw->AddRect(start, ImVec2(start.x + badge_width, start.y + badge_height), border, scaled(5.0f));
        draw->AddText(ImVec2(start.x + scaled(10.0f), start.y + scaled(4.0f)),
                      ImGui::ColorConvertFloat4ToU32(ImVec4(0.92f, 0.94f, 0.96f, 1.0f)), label);
        ImGui::Dummy(ImVec2(badge_width, badge_height));
        ImGui::SameLine();
        ImGui::TextColored(healthy ? ok : warn, "%s", value);
    };

    auto begin_panel = [&](const char* id, const char* title, float height = 0.0f) {
        ImGui::BeginChild(id, ImVec2(0, height), ImGuiChildFlags_Borders);
        ImGui::TextColored(accent, "%s", title);
        ImGui::Separator();
    };

    auto end_panel = [&]() {
        ImGui::EndChild();
    };

    auto slider_input = [&](const char* label, float* value,
                            float min, float max,
                            float step, float fast,
                            const char* fmt) {
        ImGui::PushID(label);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(label_column);
        ImGui::SetNextItemWidth(-input_width - scaled(34.0f));
        ImGui::SliderFloat("##slider", value, min, max, fmt);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(input_width);
        ImGui::InputFloat("##input", value, step, fast, fmt);
        ImGui::PopID();
    };

    auto metric_float = [&](const char* label, float value, const char* suffix = "") {
        ImGui::TextColored(muted, "%s", label);
        ImGui::SameLine(scaled(compact ? 116.0f : 145.0f));
        ImGui::Text("%.2f%s", value, suffix);
    };

    auto metric_int = [&](const char* label, int value) {
        ImGui::TextColored(muted, "%s", label);
        ImGui::SameLine(scaled(compact ? 116.0f : 145.0f));
        ImGui::Text("%d", value);
    };

    ImGui::BeginChild("##header", ImVec2(0, header_height), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::TextColored(accent, "PrimedGun");
    ImGui::SameLine();
    ImGui::TextDisabled("v%s", PRIMEDGUN_VERSION_STRING);
    ImGui::TextDisabled("Metroid Prime GCN NTSC Rev 0 (GM8E01)");

    if (!compact)
        ImGui::SameLine(std::max(0.0f, ImGui::GetContentRegionAvail().x - scaled(214.0f)));
    ImGui::PushStyleColor(ImGuiCol_Button, app.active ? ImVec4(0.12f, 0.45f, 0.22f, 1.0f)
                                                      : ImVec4(0.48f, 0.14f, 0.12f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, app.active ? ImVec4(0.16f, 0.56f, 0.28f, 1.0f)
                                                             : ImVec4(0.60f, 0.19f, 0.16f, 1.0f));
    if (ImGui::Button(app.active ? "Active - Stop" : "Inactive - Start", ImVec2(scaled(170.0f), scaled(34.0f)))) {
        if (app.active) {
            app.active = false;
            app.manually_paused.store(true, std::memory_order_relaxed);
        } else {
            app.active = true;
            app.manually_paused.store(false, std::memory_order_relaxed);
            app.recenter_requested.store(true, std::memory_order_relaxed);
        }
    }
    ImGui::PopStyleColor(2);

    ImGui::Spacing();
    status_pill("Tracking", app.tracking_status.c_str(), app.tracking_ok);
    status_pill("Hook", app.hook_status.c_str(),
                app.hook_status.find("attached") != std::string::npos);
    status_pill("Game", app.game_status.c_str(), app.game_rev0_ok);
    ImGui::EndChild();

    ImGui::Spacing();

    ImGui::BeginChild("##content", ImVec2(0, -footer_height), 0, ImGuiWindowFlags_HorizontalScrollbar);

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(scaled(10.0f), scaled(5.0f)));
    if (ImGui::BeginTabBar("##primedgun_tabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
        if (ImGui::BeginTabItem("Game")) {
            begin_panel("##game_panel", "Game Connection", 0.0f);
            ImGui::Text("Target: Metroid Prime GCN NTSC Rev 0 (GM8E01)");
            ImGui::TextColored(app.game_rev0_ok ? ok : warn, "%s", app.game_status.c_str());
            ImGui::Spacing();
            if (ImGui::Button("Reconnect Dolphin")) {
                app.reconnect_dolphin_requested.store(true, std::memory_order_relaxed);
            }
            ImGui::SameLine();
            if (ImGui::Button("Reconnect Hook")) {
                app.reconnect_tracking_requested.store(true, std::memory_order_relaxed);
            }
            ImGui::Spacing();
            ImGui::TextDisabled("Runtime status");
            ImGui::BulletText("%s", app.dolphin_status.c_str());
            ImGui::BulletText("%s", app.hook_status.c_str());
            ImGui::BulletText("Tracking: %s", app.tracking_status.c_str());
            end_panel();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Dolphin Settings")) {
            begin_panel("##dolphin_panel", "Dolphin Profile");
            const bool old_dolphin_60fps_cap = s.dolphin_60fps_cap;
            ImGui::Checkbox("Limit Dolphin to 60 FPS", &s.dolphin_60fps_cap);
            if (s.dolphin_60fps_cap != old_dolphin_60fps_cap) {
                app.dolphin_performance_apply_requested.store(true, std::memory_order_relaxed);
            }

            ImGui::Spacing();
            ImGui::TextDisabled("Applied to Dolphin's active GM8E01 VR profile.");
            end_panel();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Controller")) {
            begin_panel("##controller_panel", "Controller Mapping");
            if (ImGui::Button("Reset Controller")) {
                s.use_right_hand = kDefaultUseRightHand;
                s.auto_dolphin_xr_controls = kDefaultAutoDolphinXrControls;
                s.dolphin_60fps_cap = kDefaultDolphin60FpsCap;
                s.xr_dpad_enabled = kDefaultXrDpadEnabled;
                s.xr_dpad_head_radius = kDefaultXrDpadHeadRadius;
                s.xr_dpad_head_y_below = kDefaultXrDpadHeadYBelow;
                s.xr_dpad_deadzone = kDefaultXrDpadDeadzone;
                s.xr_dpad_stick_axis = kDefaultXrDpadStickAxis;
                s.directional_movement_enabled = kDefaultDirectionalMovementEnabled;
                s.directional_movement_use_right_stick = kDefaultDirectionalMovementUseRightStick;
                s.directional_movement_deadzone = kDefaultDirectionalMovementDeadzone;
                s.directional_movement_speed = kDefaultDirectionalMovementSpeed;
                s.directional_movement_accel = kDefaultDirectionalMovementAccel;
                s.directional_movement_air_accel = kDefaultDirectionalMovementAirAccel;
                app.remap_dolphin_controls_requested.store(true, std::memory_order_relaxed);
                app.dolphin_performance_apply_requested.store(true, std::memory_order_relaxed);
            }

            int hand = s.use_right_hand ? 0 : 1;
            ImGui::RadioButton("Right hand", &hand, 0);
            ImGui::SameLine();
            ImGui::RadioButton("Left hand", &hand, 1);
            s.use_right_hand = (hand == 0);

            const bool old_auto_dolphin_xr_controls = s.auto_dolphin_xr_controls;
            ImGui::Checkbox("Temporarily map Dolphin Port 1 to OpenXR", &s.auto_dolphin_xr_controls);
            if (s.auto_dolphin_xr_controls != old_auto_dolphin_xr_controls) {
                app.remap_dolphin_controls_requested.store(true, std::memory_order_relaxed);
            }

            ImGui::SeparatorText("Left hand D-pad");
            ImGui::Checkbox("Enable visor gesture input", &s.xr_dpad_enabled);
            slider_input("Head radius", &s.xr_dpad_head_radius, 0.08f, 0.28f, 0.01f, 0.05f, "%.2f");
            slider_input("Below head", &s.xr_dpad_head_y_below, 0.02f, 0.25f, 0.01f, 0.05f, "%.2f");
            slider_input("Stick deadzone", &s.xr_dpad_deadzone, 0.2f, 0.8f, 0.01f, 0.1f, "%.2f");
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Stick axis");
            ImGui::SameLine(label_column);
            ImGui::SetNextItemWidth(scaled(160.0f));
            ImGui::SliderInt("##stick_axis", &s.xr_dpad_stick_axis, -1, 4);
            s.xr_dpad_head_radius = std::clamp(s.xr_dpad_head_radius, 0.08f, 0.28f);
            s.xr_dpad_head_y_below = std::clamp(s.xr_dpad_head_y_below, 0.02f, 0.25f);
            s.xr_dpad_deadzone = std::clamp(s.xr_dpad_deadzone, 0.2f, 0.8f);
            s.xr_dpad_stick_axis = std::clamp(s.xr_dpad_stick_axis, -1, 4);

            ImGui::SeparatorText("Directional Movement");
            ImGui::Checkbox("Offhand yaw strafing", &s.directional_movement_enabled);
            int move_stick = s.directional_movement_use_right_stick ? 1 : 0;
            ImGui::RadioButton("Left stick", &move_stick, 0);
            ImGui::SameLine();
            ImGui::RadioButton("Right stick", &move_stick, 1);
            s.directional_movement_use_right_stick = (move_stick == 1);
            slider_input("Movement deadzone", &s.directional_movement_deadzone, 0.05f, 0.8f, 0.01f, 0.1f, "%.2f");
            slider_input("Movement speed", &s.directional_movement_speed, 4.0f, 30.0f, 0.25f, 1.0f, "%.2f");
            slider_input("Movement accel", &s.directional_movement_accel, 5.0f, 120.0f, 1.0f, 5.0f, "%.1f");
            slider_input("Air accel", &s.directional_movement_air_accel, 0.0f, 60.0f, 0.5f, 2.0f, "%.1f");
            s.directional_movement_deadzone = std::clamp(s.directional_movement_deadzone, 0.05f, 0.8f);
            s.directional_movement_speed = std::clamp(s.directional_movement_speed, 4.0f, 30.0f);
            s.directional_movement_accel = std::clamp(s.directional_movement_accel, 5.0f, 120.0f);
            s.directional_movement_air_accel = std::clamp(s.directional_movement_air_accel, 0.0f, 60.0f);
            end_panel();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Aiming")) {
            begin_panel("##aiming_panel", "Aiming");
            if (ImGui::Button("Reset Aiming")) {
                s.gun_targeting_enabled = kDefaultGunTargetingEnabled;
                s.gun_targeting_distance = kDefaultGunTargetingDistance;
                s.gun_targeting_radius = kDefaultGunTargetingRadius;
            }
            ImGui::Checkbox("Gun selects lock/scan target", &s.gun_targeting_enabled);
            slider_input("Target distance", &s.gun_targeting_distance, 10.0f, 120.0f, 1.0f, 5.0f, "%.1f");
            slider_input("Target radius", &s.gun_targeting_radius, 0.5f, 8.0f, 0.1f, 0.5f, "%.1f");
            s.gun_targeting_distance = std::clamp(s.gun_targeting_distance, 10.0f, 120.0f);
            s.gun_targeting_radius = std::clamp(s.gun_targeting_radius, 0.5f, 8.0f);
            ImGui::SeparatorText("Live Target");
            ImGui::Text("UID %04X  Object %08X  Write %s",
                        app.dbg_gun_target_uid, app.dbg_gun_target_obj,
                        app.dbg_gun_target_write ? "yes" : "no");
            ImGui::Text("Ray %.2f forward  %.2f off  candidates %d",
                        app.dbg_gun_target_along, app.dbg_gun_target_perp,
                        app.dbg_gun_target_candidates);
            end_panel();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Calibration")) {
            begin_panel("##calibration_panel", "Offset Tuning");
            ImGui::TextDisabled("Position");
            slider_input("Left / right", &s.offset_x, -2, 2, 0.01f, 0.1f, "%.3f");
            slider_input("Up / down", &s.offset_y, -2, 2, 0.01f, 0.1f, "%.3f");
            slider_input("Forward / back", &s.offset_z, -2, 2, 0.01f, 0.1f, "%.3f");

            ImGui::SeparatorText("Rotation");
            slider_input("Pitch offset", &s.rot_offset_x, -180, 180, 0.5f, 5.0f, "%.2f");
            slider_input("Yaw offset", &s.rot_offset_y, -180, 180, 0.5f, 5.0f, "%.2f");
            slider_input("Roll offset", &s.rot_offset_z, -180, 180, 0.5f, 5.0f, "%.2f");

            ImGui::SeparatorText("Scale");
            slider_input("World scale", &s.world_scale, 1, 50, 0.5f, 5.0f, "%.2f");
            s.world_scale = std::clamp(s.world_scale, 1.0f, 50.0f);

            if (ImGui::Button("Reset Position")) {
                s.offset_x = kDefaultOffsetX;
                s.offset_y = kDefaultOffsetY;
                s.offset_z = kDefaultOffsetZ;
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset Rotation")) {
                s.rot_offset_x = kDefaultRotOffsetX;
                s.rot_offset_y = kDefaultRotOffsetY;
                s.rot_offset_z = kDefaultRotOffsetZ;
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset Scale")) {
                s.world_scale = kDefaultWorldScale;
            }
            end_panel();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Layout")) {
            begin_panel("##controller_map_panel", "Controller Layout");
            if (app.controller_layout_texture != ImTextureID_Invalid &&
                app.controller_layout_width > 0 &&
                app.controller_layout_height > 0) {
                const ImVec2 avail = ImGui::GetContentRegionAvail();
                const float image_aspect =
                    static_cast<float>(app.controller_layout_width) /
                    static_cast<float>(app.controller_layout_height);
                ImVec2 image_size(avail.x, avail.x / image_aspect);
                if (image_size.y > avail.y) {
                    image_size.y = avail.y;
                    image_size.x = avail.y * image_aspect;
                }
                image_size.x = std::max(1.0f, image_size.x);
                image_size.y = std::max(1.0f, image_size.y);
                const float indent = std::max(0.0f, (avail.x - image_size.x) * 0.5f);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
                ImGui::Image(app.controller_layout_texture, image_size);
            } else {
                ImGui::TextDisabled("Controller layout image not loaded.");
            }
            end_panel();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Debug")) {
            begin_panel("##debug_panel", "Debug");
            ImGui::Checkbox("Show matrix values", &s.show_matrix_debug);
            ImGui::Checkbox("Show controller pose", &s.show_controller_debug);

            ImGui::SeparatorText("Runtime");
            metric_float("Tracker poll", app.tracker_poll_ms, " ms");
            metric_int("Tracker drops", app.tracker_drop_count);
            metric_float("Writer work", app.writer_work_ms, " ms");
            metric_float("Writer write", app.writer_write_ms, " ms");
            metric_int("Writer drops", app.writer_drop_count);

            ImGui::SeparatorText("Input");
            ImGui::Text("D-pad: %s  dir %d", app.dbg_xr_dpad_active ? "active" : "off", app.dbg_xr_dpad_dir);
            ImGui::Text("Left stick axis %d: %.2f %.2f", app.dbg_left_stick_axis, app.dbg_left_stick_x, app.dbg_left_stick_y);
            ImGui::Text("Left-to-head: %.2f m  y %.2f m", app.dbg_left_to_head_dist, app.dbg_left_to_head_y);
            ImGui::Text("Body yaw: %s  %.1f deg  stick %.2f",
                        app.dbg_directional_move_active ? "active" : "off",
                        app.dbg_directional_move_yaw_deg,
                        app.dbg_directional_move_stick_mag);

            ImGui::SeparatorText("Memory");
            ImGui::Text("mem_base:  %llX", app.dbg_mem_base);
            ImGui::Text("state_mgr: %08X", app.dbg_state_mgr);
            ImGui::Text("player:    %08X", app.dbg_player);
            ImGui::Text("pitch@:    %08X", app.dbg_pitch_addr);
            ImGui::Text("cam_mgr:   %08X", app.dbg_cam_mgr);
            ImGui::Text("gun_ptr:   %08X", app.dbg_gun_ptr);
            ImGui::Text("gun_xf@:   %08X", app.dbg_gun_xf);
            ImGui::Text("beam_xf@:  %08X", app.dbg_beam_xf);
            ImGui::Text("world_xf@: %08X", app.dbg_world_xf);
            ImGui::Text("local_xf@: %08X", app.dbg_local_xf);
            ImGui::Text("player yaw: %.2f deg", app.dbg_player_yaw_deg);
            ImGui::Text("yaw delta:  %.2f deg", app.dbg_player_yaw_delta_deg);

            if (s.show_controller_debug && app.last_pose.valid) {
                ImGui::SeparatorText("Controller Pose");
                ImGui::Text("Position: %.3f %.3f %.3f", app.last_pose.px, app.last_pose.py, app.last_pose.pz);
                ImGui::Text("Rotation: %.3f %.3f %.3f %.3f",
                            app.last_pose.qx, app.last_pose.qy, app.last_pose.qz, app.last_pose.qw);
                ImGui::Text("Trigger: %.2f", app.last_pose.trigger);
            }

            if (s.show_matrix_debug) {
                auto& m = app.last_matrix;
                ImGui::SeparatorText("Arm Cannon Matrix");
                ImGui::Text("[%6.3f %6.3f %6.3f | %8.3f]",
                    m.at(0,0), m.at(0,1), m.at(0,2), m.at(0,3));
                ImGui::Text("[%6.3f %6.3f %6.3f | %8.3f]",
                    m.at(1,0), m.at(1,1), m.at(1,2), m.at(1,3));
                ImGui::Text("[%6.3f %6.3f %6.3f | %8.3f]",
                    m.at(2,0), m.at(2,1), m.at(2,2), m.at(2,3));
            }
            end_panel();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();

    ImGui::Separator();
    if (ImGui::Button("Reset All")) {
        s.reset_all();
        app.remap_dolphin_controls_requested.store(true, std::memory_order_relaxed);
        app.dolphin_performance_apply_requested.store(true, std::memory_order_relaxed);
    }
    ImGui::SameLine();
    if (ImGui::Button("Save Settings")) {
        s.save();
        ImGui::OpenPopup("Saved!");
    }
    if (!compact)
        ImGui::SameLine(std::max(scaled(320.0f), ImGui::GetContentRegionAvail().x - scaled(145.0f)));
    ImGui::TextDisabled("By Nobbie  v%s", PRIMEDGUN_VERSION_STRING);

    if (ImGui::BeginPopup("Saved!")) {
        ImGui::Text("Settings saved to primedgun_settings.ini");
        ImGui::EndPopup();
    }

    ImGui::End();
    ImGui::PopStyleVar(4);
}
