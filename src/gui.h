#pragma once
#include "imgui.h"
#include "settings.h"
#include "dolphin_memory.h"
#include "tracking_math.h"
#include <atomic>
#include <string>
#include <algorithm>

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
    std::atomic<bool> recenter_requested = true;
    std::atomic<bool> reconnect_dolphin_requested = false;
    std::atomic<bool> reconnect_tracking_requested = false;
    std::atomic<bool> remap_dolphin_controls_requested = false;
    std::atomic<bool> dolphin_performance_apply_requested = false;
    std::atomic<bool> shader_profile_apply_requested = false;
};

inline void draw_gui(Settings& s, AppState& app,
                     DolphinMemory& dolphin)
{
    constexpr float UI_SCALE = 0.9f; // 10% slimmer

    // ── Status bar ───────────────────────────
    ImGui::SetNextWindowPos({0, 0}, ImGuiCond_Always);
    ImGui::SetNextWindowSize({500 * UI_SCALE, 96}, ImGuiCond_Always);

    ImGui::Begin("##status", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);

    ImGui::TextColored(
        app.tracking_ok ? ImVec4(0,1,0,1) : ImVec4(1,0.3f,0.3f,1),
        "Tracking: %s", app.tracking_status.c_str());

    ImGui::Text("%s", app.hook_status.c_str());

    ImGui::TextColored(
        app.game_rev0_ok ? ImVec4(0,1,0,1) : ImVec4(1,0.3f,0.3f,1),
        "%s", app.game_status.c_str());

    ImGui::End();

    // ── Main window ──────────────────────────
    ImGui::SetNextWindowPos({0, 96}, ImGuiCond_Once);
    ImGui::SetNextWindowSize({520 * UI_SCALE, 600}, ImGuiCond_Once);

    ImGui::Begin("PrimedGun Settings");

    float full_width = ImGui::GetContentRegionAvail().x;

    auto SliderInput = [&](const char* label, float* v,
                           float min, float max,
                           float step, float fast,
                           const char* fmt)
    {
        ImGui::Text("%s", label);
        ImGui::PushID(label);

        ImGui::SetNextItemWidth(-100);
        ImGui::SliderFloat("##s", v, min, max);

        ImGui::SameLine();

        ImGui::SetNextItemWidth(90);
        ImGui::InputFloat("##i", v, step, fast, fmt);

        ImGui::PopID();
    };

    // ── Enable ───────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_Button,
        app.active ? ImVec4(0.2f,0.7f,0.2f,1) : ImVec4(0.7f,0.2f,0.2f,1));

    if (ImGui::Button(app.active ? "ACTIVE - Click to Stop" : "INACTIVE - Click to Start",
                      {full_width, 40})) {
        app.active = !app.active;
        if (app.active)
            app.recenter_requested.store(true, std::memory_order_relaxed);
    }

    ImGui::PopStyleColor();
    ImGui::Separator();

    // ── Game ────────────────────────────────
    if (ImGui::CollapsingHeader("Game")) {
        ImGui::Text("Metroid Prime GCN NTSC Rev 0 (GM8E01)");
        ImGui::TextColored(
            app.game_rev0_ok ? ImVec4(0,1,0,1) : ImVec4(1,0.3f,0.3f,1),
            "%s", app.game_status.c_str());

        if (ImGui::Button("Reconnect Dolphin")) {
            app.reconnect_dolphin_requested.store(true, std::memory_order_relaxed);
        }

        ImGui::SameLine();

        if (ImGui::Button("Reconnect Dolphin Hook")) {
            app.reconnect_tracking_requested.store(true, std::memory_order_relaxed);
        }
    }

    if (ImGui::CollapsingHeader("Dolphin Performance")) {
        const bool old_dolphin_60fps_cap = s.dolphin_60fps_cap;
        ImGui::Checkbox("Limit Dolphin to 60 FPS", &s.dolphin_60fps_cap);
        if (s.dolphin_60fps_cap != old_dolphin_60fps_cap) {
            app.dolphin_performance_apply_requested.store(true, std::memory_order_relaxed);
        }
    }

    // ── Controller ─────────────────────────
    if (ImGui::CollapsingHeader("Controller")) {
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
        }

        int hand = s.use_right_hand ? 0 : 1;
        ImGui::RadioButton("Right hand", &hand, 0); ImGui::SameLine();
        ImGui::RadioButton("Left hand",  &hand, 1);
        s.use_right_hand = (hand == 0);

        const bool old_auto_dolphin_xr_controls = s.auto_dolphin_xr_controls;
        ImGui::Checkbox("Temporarily map Dolphin Port 1 to OpenXR", &s.auto_dolphin_xr_controls);
        if (s.auto_dolphin_xr_controls != old_auto_dolphin_xr_controls) {
            app.remap_dolphin_controls_requested.store(true, std::memory_order_relaxed);
        }

        ImGui::SeparatorText("Left hand D-pad");
        ImGui::Checkbox("Enable visor gesture input", &s.xr_dpad_enabled);
        SliderInput("Head radius", &s.xr_dpad_head_radius, 0.08f, 0.28f, 0.01f, 0.05f, "%.2f");
        SliderInput("Below head", &s.xr_dpad_head_y_below, 0.02f, 0.25f, 0.01f, 0.05f, "%.2f");
        SliderInput("Stick deadzone", &s.xr_dpad_deadzone, 0.2f, 0.8f, 0.01f, 0.1f, "%.2f");
        ImGui::SliderInt("Stick axis (-1 auto)", &s.xr_dpad_stick_axis, -1, 4);
        s.xr_dpad_head_radius = std::clamp(s.xr_dpad_head_radius, 0.08f, 0.28f);
        s.xr_dpad_head_y_below = std::clamp(s.xr_dpad_head_y_below, 0.02f, 0.25f);
        s.xr_dpad_deadzone = std::clamp(s.xr_dpad_deadzone, 0.2f, 0.8f);
        s.xr_dpad_stick_axis = std::clamp(s.xr_dpad_stick_axis, -1, 4);
        ImGui::Text("D-pad mode: %s  dir: %d", app.dbg_xr_dpad_active ? "active" : "off", app.dbg_xr_dpad_dir);
        ImGui::Text("left stick axis %d: %.2f %.2f", app.dbg_left_stick_axis, app.dbg_left_stick_x, app.dbg_left_stick_y);
        ImGui::Text("left-to-head: %.2f m  y %.2f m", app.dbg_left_to_head_dist, app.dbg_left_to_head_y);

        ImGui::SeparatorText("Directional movement");
        ImGui::Checkbox("Offhand yaw strafing", &s.directional_movement_enabled);
        int move_stick = s.directional_movement_use_right_stick ? 1 : 0;
        ImGui::RadioButton("Left stick", &move_stick, 0); ImGui::SameLine();
        ImGui::RadioButton("Right stick", &move_stick, 1);
        s.directional_movement_use_right_stick = (move_stick == 1);
        SliderInput("Movement deadzone", &s.directional_movement_deadzone, 0.05f, 0.8f, 0.01f, 0.1f, "%.2f");
        SliderInput("Movement speed", &s.directional_movement_speed, 4.0f, 30.0f, 0.25f, 1.0f, "%.2f");
        SliderInput("Movement accel", &s.directional_movement_accel, 5.0f, 120.0f, 1.0f, 5.0f, "%.1f");
        SliderInput("Air accel", &s.directional_movement_air_accel, 0.0f, 60.0f, 0.5f, 2.0f, "%.1f");
        s.directional_movement_deadzone = std::clamp(s.directional_movement_deadzone, 0.05f, 0.8f);
        s.directional_movement_speed = std::clamp(s.directional_movement_speed, 4.0f, 30.0f);
        s.directional_movement_accel = std::clamp(s.directional_movement_accel, 5.0f, 120.0f);
        s.directional_movement_air_accel = std::clamp(s.directional_movement_air_accel, 0.0f, 60.0f);
        ImGui::Text("body yaw: %s  %.1f deg  stick %.2f",
                    app.dbg_directional_move_active ? "active" : "off",
                    app.dbg_directional_move_yaw_deg,
                    app.dbg_directional_move_stick_mag);

    }

    if (ImGui::CollapsingHeader("Aiming")) {
        if (ImGui::Button("Reset Aiming")) {
            s.gun_targeting_enabled = kDefaultGunTargetingEnabled;
            s.gun_targeting_distance = kDefaultGunTargetingDistance;
            s.gun_targeting_radius = kDefaultGunTargetingRadius;
        }

        ImGui::Checkbox("Gun selects lock/scan target", &s.gun_targeting_enabled);
        SliderInput("Target distance", &s.gun_targeting_distance, 10.0f, 120.0f, 1.0f, 5.0f, "%.1f");
        SliderInput("Target radius", &s.gun_targeting_radius, 0.5f, 8.0f, 0.1f, 0.5f, "%.1f");
        s.gun_targeting_distance = std::clamp(s.gun_targeting_distance, 10.0f, 120.0f);
        s.gun_targeting_radius = std::clamp(s.gun_targeting_radius, 0.5f, 8.0f);
        ImGui::Text("target uid: %04X  obj@: %08X  write %s",
                    app.dbg_gun_target_uid, app.dbg_gun_target_obj,
                    app.dbg_gun_target_write ? "yes" : "no");
        ImGui::Text("ray: %.2f forward  %.2f off  candidates %d",
                    app.dbg_gun_target_along, app.dbg_gun_target_perp,
                    app.dbg_gun_target_candidates);
    }

    // ── Offset ──────────────────────────────
    if (ImGui::CollapsingHeader("Offset Tuning")) {

        ImGui::Text("Position");
        SliderInput("X", &s.offset_x, -2, 2, 0.01f, 0.1f, "%.3f");
        SliderInput("Y", &s.offset_y, -2, 2, 0.01f, 0.1f, "%.3f");
        SliderInput("Z", &s.offset_z, -2, 2, 0.01f, 0.1f, "%.3f");

        ImGui::Spacing();

        ImGui::SeparatorText("Overall Gun Rotation Offset");
        ImGui::TextWrapped("Use these three angles to rotate the entire gun orientation after controller tracking.");
        SliderInput("Pitch Offset", &s.rot_offset_x, -180, 180, 0.5f, 5.0f, "%.2f");
        SliderInput("Yaw Offset",   &s.rot_offset_y, -180, 180, 0.5f, 5.0f, "%.2f");
        SliderInput("Roll Offset",  &s.rot_offset_z, -180, 180, 0.5f, 5.0f, "%.2f");
        ImGui::Text("Current rot offset: %.2f  %.2f  %.2f",
            s.rot_offset_x, s.rot_offset_y, s.rot_offset_z);

        if (ImGui::Button("Reset offsets")) {
            s.offset_x = kDefaultOffsetX;
            s.offset_y = kDefaultOffsetY;
            s.offset_z = kDefaultOffsetZ;
            s.rot_offset_x = kDefaultRotOffsetX;
            s.rot_offset_y = kDefaultRotOffsetY;
            s.rot_offset_z = kDefaultRotOffsetZ;
        }
    }

    // ── Scaling ───────────────────────────
    if (ImGui::CollapsingHeader("Scaling")) {

        SliderInput("World scale", &s.world_scale, 1, 50, 0.5f, 5.0f, "%.2f");

        if (ImGui::Button("Reset scale")) {
            s.world_scale = kDefaultWorldScale;
        }

        s.world_scale = std::clamp(s.world_scale, 1.0f, 50.0f);
    }

    // ── Debug ───────────────────────────────
    if (ImGui::CollapsingHeader("Debug")) {
        ImGui::Checkbox("Show matrix values", &s.show_matrix_debug);
        ImGui::Checkbox("Show controller pose", &s.show_controller_debug);
        if (s.show_controller_debug && app.last_pose.valid) {
            ImGui::Text("Controller pos: %.3f %.3f %.3f",
                app.last_pose.px, app.last_pose.py, app.last_pose.pz);
            ImGui::Text("Controller rot: %.3f %.3f %.3f %.3f",
                app.last_pose.qx, app.last_pose.qy,
                app.last_pose.qz, app.last_pose.qw);
            ImGui::Text("Trigger: %.2f", app.last_pose.trigger);
        }

        if (s.show_matrix_debug) {
            auto& m = app.last_matrix;
            ImGui::Text("Arm cannon matrix:");
            ImGui::Text("  [%6.3f %6.3f %6.3f | %8.3f]",
                m.at(0,0), m.at(0,1), m.at(0,2), m.at(0,3));
            ImGui::Text("  [%6.3f %6.3f %6.3f | %8.3f]",
                m.at(1,0), m.at(1,1), m.at(1,2), m.at(1,3));
            ImGui::Text("  [%6.3f %6.3f %6.3f | %8.3f]",
                m.at(2,0), m.at(2,1), m.at(2,2), m.at(2,3));
        }

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
        ImGui::Text("gun target: uid %04X obj %08X write %s score %.3f",
            app.dbg_gun_target_uid, app.dbg_gun_target_obj,
            app.dbg_gun_target_write ? "yes" : "no",
            app.dbg_gun_target_score);

        ImGui::SeparatorText("Timing");
        ImGui::Text("tracker: poll %.2f ms  drops %d",
            app.tracker_poll_ms, app.tracker_drop_count);
        ImGui::Text("writer:  work %.2f ms  write %.2f ms  drops %d",
            app.writer_work_ms, app.writer_write_ms, app.writer_drop_count);
    }

    ImGui::Separator();

    if (ImGui::Button("Reset All Settings", {full_width, 30})) {
        s.reset_all();
    }

    if (ImGui::Button("Save Settings", {full_width, 30})) {
        s.save();
        ImGui::OpenPopup("Saved!");
    }

    if (ImGui::BeginPopup("Saved!")) {
        ImGui::Text("Settings saved to primedgun_settings.ini");
        ImGui::EndPopup();
    }

    ImGui::Spacing();
    const char* credit = "By Nobbie  v" PRIMEDGUN_VERSION_STRING;
    const float credit_width = ImGui::CalcTextSize(credit).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + full_width - credit_width);
    ImGui::TextDisabled("%s", credit);

    ImGui::End();
}
