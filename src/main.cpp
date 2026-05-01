#include <Windows.h>
#include <mmsystem.h>
#include <d3d11.h>
#include <chrono>
#include <thread>
#include <cmath>
#include <atomic>
#include <mutex>
#include <fstream>
#include <vector>
#include <cstring>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

#include "settings.h"
#include "dolphin_memory.h"
#include "openvr_manager.h"
#include "gui.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static ID3D11Device*           g_device = nullptr;
static ID3D11DeviceContext*    g_context = nullptr;
static IDXGISwapChain*         g_swapchain = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static HWND                    g_hwnd = nullptr;

static Settings       g_settings;
static AppState       g_app;
static DolphinMemory  g_dolphin;
static OpenVRManager  g_openvr;

static float g_smooth_mat[12] = {1,0,0,0, 0,1,0,0, 0,0,1,0};
static float g_smooth_pitch = 0.0f;
static bool g_lock_pitch_have_unlocked = false;
static float g_lock_pitch_unlocked = 0.0f;
static std::atomic<bool> g_running = true;
static std::mutex g_pose_mutex;
static std::mutex g_gun_target_mutex;
static Pose g_latest_pose = {};
static Pose g_latest_left_pose = {};
static Pose g_latest_hmd_pose = {};
static uint32_t g_cached_target_player = 0;
static uint16_t g_cached_target_uid = 0xffff;
static float g_cached_target_x = 0.0f;
static float g_cached_target_y = 0.0f;
static float g_cached_target_z = 0.0f;
static auto g_cached_target_time = std::chrono::steady_clock::time_point{};
static uint32_t g_last_target_player = 0;
static uint16_t g_last_target_uid = 0xffff;
static bool g_latched_target_active = false;
static uint32_t g_latched_target_player = 0;
static uint16_t g_latched_target_uid = 0xffff;
static uint32_t g_latched_target_obj = 0;
static float g_controller_base_prime_x = 0.0f;
static float g_controller_base_prime_y = 0.0f;
static float g_controller_base_prime_z = 0.0f;
static float g_camera_base_prime_x = 0.0f;
static float g_camera_base_prime_y = 0.0f;
static float g_camera_base_prime_z = 0.0f;
static bool g_translation_base_valid = false;
static float g_last_written_basis[9] = {};
static float g_last_desired_basis[9] = {};
static bool g_last_written_basis_valid = false;
static bool g_last_desired_basis_valid = false;
static float g_directional_move_speed = 0.0f;
static constexpr uint32_t k_render_hook_basis_addr = 0x817FE000;
static constexpr uint32_t k_render_hook_expected_gun_addr = k_render_hook_basis_addr + 0x38;
static constexpr uint32_t k_projectile_fire_debug_addr = 0x817FE100;
static constexpr uint32_t k_projectile_probe_debug_addr = 0x817FE300;
static constexpr uint32_t k_gun_target_hook_addr = 0x817FE400;
static constexpr uint32_t k_final_input_offset = 0xB54;
static constexpr uint32_t k_final_input_right_stick_x = k_final_input_offset + 0x10;
static constexpr uint32_t k_final_input_right_stick_y = k_final_input_offset + 0x14;
static constexpr uint32_t k_final_input_right_stick_x_press = k_final_input_offset + 0x22;
static constexpr uint32_t k_final_input_right_stick_y_press = k_final_input_offset + 0x23;
static constexpr uint32_t k_final_input_dpad_held_0 = k_final_input_offset + 0x2C;
static constexpr uint32_t k_final_input_dpad_held_1 = k_final_input_offset + 0x2D;
static constexpr uint32_t k_final_input_dpad_pressed_0 = k_final_input_offset + 0x2E;
static constexpr uint32_t k_player_disable_input_flags_offset = 0x9C6;
static constexpr uint8_t k_player_disable_input_mask = 0x04;
static constexpr uint32_t k_player_free_look_state_offset = 0x3DC;
static constexpr uint32_t k_player_free_look_center_time_offset = 0x3E0;
static constexpr uint32_t k_player_free_look_yaw_angle_offset = 0x3E4;
static constexpr uint32_t k_player_free_look_yaw_vel_offset = 0x3E8;
static constexpr uint32_t k_player_free_look_pitch_angle_offset = 0x3EC;
static constexpr uint32_t k_player_free_look_pitch_vel_offset = 0x3F0;
static constexpr uint32_t k_cannon_rotation_offsets[9] = {
    0x4A8, 0x4AC, 0x4B0,
    0x4B8, 0x4BC, 0x4C0,
    0x4C8, 0x4CC, 0x4D0,
};

static void push_timing_sample(float* hist, int& head, float ms) {
    hist[head & 15] = ms;
    head = (head + 1) & 15;
}

class TimerResolutionScope {
public:
    TimerResolutionScope() {
        active_ = (timeBeginPeriod(1) == TIMERR_NOERROR);
    }

    ~TimerResolutionScope() {
        if (active_) timeEndPeriod(1);
    }

private:
    bool active_ = false;
};

class HighResWaitTimer {
public:
    HighResWaitTimer() {
        timer_ = CreateWaitableTimerExW(nullptr, nullptr,
                                        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
    }

    ~HighResWaitTimer() {
        if (timer_) CloseHandle(timer_);
    }

    void wait_for(std::chrono::microseconds duration) {
        if (!timer_ || duration.count() <= 0) return;

        LARGE_INTEGER due_time = {};
        due_time.QuadPart = -static_cast<LONGLONG>(duration.count() * 10);
        if (SetWaitableTimerEx(timer_, &due_time, 0, nullptr, nullptr, nullptr, 0)) {
            WaitForSingleObject(timer_, INFINITE);
        }
    }

private:
    HANDLE timer_ = nullptr;
};

static void write_basis_preserve_translation(uint32_t addr, const float* basis) {
    if (addr < 0x80000000) return;
    g_dolphin.write_float(addr + 0x00, basis[0]);
    g_dolphin.write_float(addr + 0x04, basis[1]);
    g_dolphin.write_float(addr + 0x08, basis[2]);
    g_dolphin.write_float(addr + 0x10, basis[4]);
    g_dolphin.write_float(addr + 0x14, basis[5]);
    g_dolphin.write_float(addr + 0x18, basis[6]);
    g_dolphin.write_float(addr + 0x20, basis[8]);
    g_dolphin.write_float(addr + 0x24, basis[9]);
    g_dolphin.write_float(addr + 0x28, basis[10]);
}

static void extract_basis9(const float* mat, float* basis) {
    basis[0] = mat[0];
    basis[1] = mat[1];
    basis[2] = mat[2];
    basis[3] = mat[4];
    basis[4] = mat[5];
    basis[5] = mat[6];
    basis[6] = mat[8];
    basis[7] = mat[9];
    basis[8] = mat[10];
}

static bool read_basis9(uint32_t addr, float* basis) {
    if (addr < 0x80000000) return false;

    basis[0] = g_dolphin.read_float(addr + 0x00);
    basis[1] = g_dolphin.read_float(addr + 0x04);
    basis[2] = g_dolphin.read_float(addr + 0x08);
    basis[3] = g_dolphin.read_float(addr + 0x10);
    basis[4] = g_dolphin.read_float(addr + 0x14);
    basis[5] = g_dolphin.read_float(addr + 0x18);
    basis[6] = g_dolphin.read_float(addr + 0x20);
    basis[7] = g_dolphin.read_float(addr + 0x24);
    basis[8] = g_dolphin.read_float(addr + 0x28);

    for (int i = 0; i < 9; ++i) {
        if (!std::isfinite(basis[i])) return false;
    }
    return true;
}

static void write_contiguous_basis9(uint32_t addr, const float* basis) {
    if (addr < 0x80000000) return;
    for (int i = 0; i < 9; ++i) {
        g_dolphin.write_float(addr + static_cast<uint32_t>(i * 4), basis[i]);
    }
}

static void clear_u32_range(uint32_t addr, int words) {
    if (addr < 0x80000000) return;
    for (int i = 0; i < words; ++i) {
        g_dolphin.write_u32(addr + static_cast<uint32_t>(i * 4), 0);
    }
}

static bool resolve_active_camera_transform_addr(uint32_t& camera_addr);
static bool read_player_yaw_from_transform2d(uint32_t addr, float& yaw_deg_out);
static float wrap_angle_radians(float angle);
static void apply_openvr_world_yaw(Pose& pose, float yaw_deg);

enum XrDpadDir {
    XrDpadNone = 0,
    XrDpadUp = 1,
    XrDpadRight = 2,
    XrDpadDown = 3,
    XrDpadLeft = 4,
};

static const char* xr_dpad_dir_name(XrDpadDir dir) {
    switch (dir) {
    case XrDpadUp: return "up";
    case XrDpadRight: return "right";
    case XrDpadDown: return "down";
    case XrDpadLeft: return "left";
    default: return "none";
    }
}

static void append_xr_dpad_debug(const char* phase, bool near_head, bool in_head_zone,
                                 float stick_x, float stick_y, XrDpadDir raw_dir,
                                 XrDpadDir final_dir, int latch_frames,
                                 bool send_press_event, uint8_t held0,
                                 uint8_t held1, uint8_t pressed0) {
    static std::ofstream log;
    static auto start = std::chrono::steady_clock::now();
    static auto last_log = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (last_log.time_since_epoch().count() != 0 &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_log).count() < 50) {
        return;
    }
    last_log = now;

    if (!log.is_open()) {
        log.open("xr_dpad_debug.txt", std::ios::out | std::ios::trunc);
        if (!log.is_open())
            return;
        log << "ms,phase,near_head,in_head_zone,stick_x,stick_y,raw_dir,final_dir,latch,press,held0,held1,pressed0\n";
    }

    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
    log << ms << ',' << phase << ','
        << (near_head ? 1 : 0) << ','
        << (in_head_zone ? 1 : 0) << ','
        << stick_x << ',' << stick_y << ','
        << xr_dpad_dir_name(raw_dir) << ','
        << xr_dpad_dir_name(final_dir) << ','
        << latch_frames << ','
        << (send_press_event ? 1 : 0) << ','
        << static_cast<int>(held0) << ','
        << static_cast<int>(held1) << ','
        << static_cast<int>(pressed0) << '\n';
    log.flush();
}

static XrDpadDir get_stick_dpad_direction(float x, float y, float deadzone) {
    const float ax = std::fabs(x);
    const float ay = std::fabs(y);
    if (ax < deadzone && ay < deadzone) return XrDpadNone;
    if (ay >= ax) return y > 0.0f ? XrDpadUp : XrDpadDown;
    return x > 0.0f ? XrDpadRight : XrDpadLeft;
}

static XrDpadDir get_stick_dpad_direction_with_hysteresis(float x, float y,
                                                          float deadzone,
                                                          XrDpadDir last_dir) {
    const float mag = std::sqrt(x * x + y * y);
    const float exit_deadzone = std::max(0.05f, deadzone * 0.55f);
    if (mag < exit_deadzone)
        return XrDpadNone;

    XrDpadDir dir = get_stick_dpad_direction(x, y, deadzone);
    if (dir != XrDpadNone)
        return dir;
    if (last_dir == XrDpadNone)
        return XrDpadNone;

    const float ax = std::fabs(x);
    const float ay = std::fabs(y);
    constexpr float kAxisSwitchBias = 1.35f;
    switch (last_dir) {
    case XrDpadUp:
    case XrDpadDown:
        if (ay * kAxisSwitchBias >= ax)
            return y >= 0.0f ? XrDpadUp : XrDpadDown;
        break;
    case XrDpadLeft:
    case XrDpadRight:
        if (ax * kAxisSwitchBias >= ay)
            return x >= 0.0f ? XrDpadRight : XrDpadLeft;
        break;
    default:
        break;
    }
    return ax > ay ? (x >= 0.0f ? XrDpadRight : XrDpadLeft)
                   : (y >= 0.0f ? XrDpadUp : XrDpadDown);
}

static void select_xr_dpad_stick_axis(const Pose& pose, float& x, float& y, int& axis_out) {
    if (g_settings.xr_dpad_stick_axis >= 0 && g_settings.xr_dpad_stick_axis < 5) {
        axis_out = g_settings.xr_dpad_stick_axis;
        x = pose.axis_x[axis_out];
        y = pose.axis_y[axis_out];
        return;
    }

    axis_out = pose.stick_axis;
    x = pose.stick_x;
    y = pose.stick_y;
}

static bool left_controller_is_near_head(const Pose& left, const Pose& hmd) {
    if (!left.valid || !hmd.valid) return false;
    const float dx = left.px - hmd.px;
    const float dy = left.py - hmd.py;
    const float dz = left.pz - hmd.pz;
    const float dist_sq = dx * dx + dy * dy + dz * dz;
    const float radius = g_settings.xr_dpad_head_radius;

    g_app.dbg_left_to_head_dist = std::sqrt(dist_sq);
    g_app.dbg_left_to_head_y = dy;
    return dist_sq <= radius * radius &&
           dy >= -g_settings.xr_dpad_head_y_below &&
           dy <= 0.22f;
}

static bool left_controller_is_near_head_for_dpad(const Pose& left, const Pose& hmd) {
    if (!left.valid || !hmd.valid) return false;
    const float dx = left.px - hmd.px;
    const float dy = left.py - hmd.py;
    const float dz = left.pz - hmd.pz;
    const float dist_sq = dx * dx + dy * dy + dz * dz;
    const float radius = g_settings.xr_dpad_head_radius + 0.06f;

    g_app.dbg_left_to_head_dist = std::sqrt(dist_sq);
    g_app.dbg_left_to_head_y = dy;
    return dist_sq <= radius * radius &&
           dy >= -(g_settings.xr_dpad_head_y_below + 0.04f) &&
           dy <= 0.28f;
}

static bool read_camera_yaw_delta_degrees_quiet(float& yaw_delta_deg) {
    uint32_t player_yaw_addr = 0;
    if (!g_dolphin.is_connected() || !resolve_active_camera_transform_addr(player_yaw_addr))
        return false;

    float current_player_yaw_deg = 0.0f;
    if (!read_player_yaw_from_transform2d(player_yaw_addr, current_player_yaw_deg))
        return false;

    yaw_delta_deg = wrap_angle_radians((current_player_yaw_deg + 180.0f) *
                                      (static_cast<float>(M_PI) / 180.0f)) *
                    (-180.0f / static_cast<float>(M_PI));
    return true;
}

static float yaw_from_prime_xy(float x, float y) {
    return std::atan2(x, y);
}

static void prime_xy_from_yaw(float yaw, float& x, float& y) {
    x = std::sin(yaw);
    y = std::cos(yaw);
}

static void set_player_input_disabled_for_dpad(uint32_t state_mgr, bool disabled) {
    static bool s_forced_disabled = false;
    static bool s_was_disabled = false;
    static uint32_t s_flags_addr = 0;
    static auto s_last_refresh = std::chrono::steady_clock::time_point{};

    const auto now = std::chrono::steady_clock::now();
    if (disabled && s_forced_disabled && s_flags_addr >= 0x80000000) {
        const bool refresh =
            s_last_refresh.time_since_epoch().count() == 0 ||
            std::chrono::duration_cast<std::chrono::milliseconds>(now - s_last_refresh).count() >= 250;
        if (!refresh)
            return;
        g_dolphin.write_u8(s_flags_addr, g_dolphin.read_u8(s_flags_addr) | k_player_disable_input_mask);
        s_last_refresh = now;
        return;
    }

    const auto addrs = get_addresses();
    const uint32_t player = g_dolphin.read_u32(state_mgr + addrs.player_offset);
    if (player < 0x80000000) {
        s_forced_disabled = false;
        s_was_disabled = false;
        s_flags_addr = 0;
        s_last_refresh = {};
        return;
    }

    const uint32_t flags_addr = player + k_player_disable_input_flags_offset;
    uint8_t flags = g_dolphin.read_u8(flags_addr);
    if (disabled) {
        if (!s_forced_disabled) {
            s_was_disabled = (flags & k_player_disable_input_mask) != 0;
            s_forced_disabled = true;
        }
        flags |= k_player_disable_input_mask;
        g_dolphin.write_u8(flags_addr, flags);
        s_flags_addr = flags_addr;
        s_last_refresh = now;
        return;
    }

    if (s_forced_disabled && !s_was_disabled) {
        flags &= static_cast<uint8_t>(~k_player_disable_input_mask);
        g_dolphin.write_u8(flags_addr, flags);
    }
    s_forced_disabled = false;
    s_was_disabled = false;
    s_flags_addr = 0;
    s_last_refresh = {};
}

static void suppress_c_stick_for_dpad(uint32_t state_mgr) {
    g_dolphin.write_float(state_mgr + k_final_input_right_stick_x, 0.0f);
    g_dolphin.write_float(state_mgr + k_final_input_right_stick_y, 0.0f);
    g_dolphin.write_u8(state_mgr + k_final_input_right_stick_x_press, 0);
    g_dolphin.write_u8(state_mgr + k_final_input_right_stick_y_press, 0);

    const auto addrs = get_addresses();
    const uint32_t player = g_dolphin.read_u32(state_mgr + addrs.player_offset);
    if (player < 0x80000000)
        return;

    g_dolphin.write_u8(player + k_player_free_look_state_offset, 0);
    g_dolphin.write_u8(player + k_player_free_look_state_offset + 1, 0);
    g_dolphin.write_u8(player + k_player_free_look_state_offset + 2, 0);
    g_dolphin.write_float(player + k_player_free_look_center_time_offset, 0.0f);
    g_dolphin.write_float(player + k_player_free_look_yaw_angle_offset, 0.0f);
    g_dolphin.write_float(player + k_player_free_look_yaw_vel_offset, 0.0f);
    g_dolphin.write_float(player + k_player_free_look_pitch_angle_offset, 0.0f);
    g_dolphin.write_float(player + k_player_free_look_pitch_vel_offset, 0.0f);
}

static bool apply_xr_dpad_input(const Pose& left, const Pose& hmd) {
    static XrDpadDir s_last_dir = XrDpadNone;
    static auto s_dir_start = std::chrono::steady_clock::time_point{};
    static auto s_last_press_pulse = std::chrono::steady_clock::time_point{};
    static auto s_last_near_head = std::chrono::steady_clock::time_point{};
    static XrDpadDir s_latched_dir = XrDpadNone;
    static auto s_latch_until = std::chrono::steady_clock::time_point{};
    static bool s_c_stick_suppressed = false;
    g_app.dbg_xr_dpad_active = false;
    g_app.dbg_xr_dpad_dir = XrDpadNone;
    float stick_x = 0.0f;
    float stick_y = 0.0f;
    int stick_axis = 0;
    select_xr_dpad_stick_axis(left, stick_x, stick_y, stick_axis);
    g_app.dbg_left_stick_axis = stick_axis;
    g_app.dbg_left_stick_x = stick_x;
    g_app.dbg_left_stick_y = stick_y;

    if (!g_settings.xr_dpad_enabled || !g_app.dolphin_ok) {
        if (g_app.dolphin_ok) {
            const auto addrs = get_addresses();
            if (addrs.state_manager >= 0x80000000)
                set_player_input_disabled_for_dpad(addrs.state_manager, false);
        }
        s_last_dir = XrDpadNone;
        s_dir_start = {};
        s_last_press_pulse = {};
        s_last_near_head = {};
        s_latched_dir = XrDpadNone;
        s_latch_until = {};
        s_c_stick_suppressed = false;
        return false;
    }

    const auto addrs = get_addresses();
    const uint32_t state_mgr = addrs.state_manager;
    if (state_mgr < 0x80000000) {
        s_last_dir = XrDpadNone;
        s_dir_start = {};
        s_last_press_pulse = {};
        s_last_near_head = {};
        s_latched_dir = XrDpadNone;
        s_latch_until = {};
        s_c_stick_suppressed = false;
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    const bool near_head = left_controller_is_near_head_for_dpad(left, hmd);
    bool in_head_zone = near_head;
    if (near_head) {
        s_last_near_head = now;
    } else {
        const bool in_head_grace =
            s_last_near_head.time_since_epoch().count() != 0 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - s_last_near_head).count() < 260;
        if (!in_head_grace) {
            set_player_input_disabled_for_dpad(state_mgr, false);
            s_last_dir = XrDpadNone;
            s_dir_start = {};
            s_last_press_pulse = {};
            s_latched_dir = XrDpadNone;
            s_latch_until = {};
            s_c_stick_suppressed = false;
            return false;
        }
        in_head_zone = true;
    }
    set_player_input_disabled_for_dpad(state_mgr, in_head_zone);
    if (!s_c_stick_suppressed) {
        suppress_c_stick_for_dpad(state_mgr);
        s_c_stick_suppressed = true;
    }
    const float dpad_deadzone = std::min(g_settings.xr_dpad_deadzone, 0.25f);
    const XrDpadDir raw_dir = get_stick_dpad_direction_with_hysteresis(
        stick_x, stick_y, dpad_deadzone, s_last_dir);
    XrDpadDir dir = raw_dir;
    if (dir != XrDpadNone) {
        s_latched_dir = dir;
        s_latch_until = now + std::chrono::milliseconds(120);
    } else if (s_latched_dir != XrDpadNone && now < s_latch_until) {
        dir = s_latched_dir;
    }
    const int latch_ms_left =
        (s_latched_dir != XrDpadNone && now < s_latch_until)
            ? static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(s_latch_until - now).count())
            : 0;
    if (dir == XrDpadNone) {
        append_xr_dpad_debug("no_dir", near_head, in_head_zone, stick_x, stick_y,
                             raw_dir, dir, latch_ms_left, false, 0, 0, 0);
        s_last_dir = XrDpadNone;
        s_dir_start = {};
        s_last_press_pulse = {};
        s_latched_dir = XrDpadNone;
        s_latch_until = {};
        return false;
    }

    if (dir != s_last_dir) {
        s_dir_start = now;
        s_last_press_pulse = {};
    }
    const bool in_initial_press_window =
        s_dir_start.time_since_epoch().count() != 0 &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - s_dir_start).count() < 180;
    const bool repeat_press =
        s_last_press_pulse.time_since_epoch().count() == 0 ||
        std::chrono::duration_cast<std::chrono::milliseconds>(now - s_last_press_pulse).count() >= 160;
    const bool send_press_event = in_initial_press_window || repeat_press;
    if (send_press_event)
        s_last_press_pulse = now;

    uint8_t held0 = g_dolphin.read_u8(state_mgr + k_final_input_dpad_held_0);
    uint8_t held1 = g_dolphin.read_u8(state_mgr + k_final_input_dpad_held_1);
    uint8_t pressed0 = g_dolphin.read_u8(state_mgr + k_final_input_dpad_pressed_0);

    // Metroid's decomp names these bits x2c_b31, x2d_b24, etc. On the GCN
    // bitfield layout, that maps to descending bits within each byte.
    held0 &= static_cast<uint8_t>(~0x01u); // D-pad up
    held1 &= static_cast<uint8_t>(~0xE0u); // right/down/left
    pressed0 &= static_cast<uint8_t>(~0x1Cu); // pressed up/right/down/left

    switch (dir) {
    case XrDpadUp:
        held0 |= 0x01u;
        if (send_press_event) pressed0 |= 0x10u;
        break;
    case XrDpadRight:
        held1 |= 0x80u;
        if (send_press_event) pressed0 |= 0x08u;
        break;
    case XrDpadDown:
        held1 |= 0x40u;
        if (send_press_event) pressed0 |= 0x04u;
        break;
    case XrDpadLeft:
        held1 |= 0x20u;
        if (send_press_event) pressed0 |= 0x02u;
        break;
    default:
        break;
    }

    g_dolphin.write_u8(state_mgr + k_final_input_dpad_held_0, held0);
    g_dolphin.write_u8(state_mgr + k_final_input_dpad_held_1, held1);
    g_dolphin.write_u8(state_mgr + k_final_input_dpad_pressed_0, pressed0);
    append_xr_dpad_debug("publish", near_head, in_head_zone, stick_x, stick_y,
                         raw_dir, dir, latch_ms_left, send_press_event,
                         held0, held1, pressed0);

    g_app.dbg_xr_dpad_active = true;
    g_app.dbg_xr_dpad_dir = dir;
    s_last_dir = dir;
    return true;
}

static bool basis_differs(const float* a, const float* b, float epsilon) {
    for (int i = 0; i < 9; ++i) {
        if (std::fabs(a[i] - b[i]) > epsilon) return true;
    }
    return false;
}

static Matrix3x4 read_matrix3x4(uint32_t addr) {
    Matrix3x4 mat = {};
    if (addr < 0x80000000) return mat;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 4; ++col) {
            mat.at(row, col) = g_dolphin.read_float(addr + static_cast<uint32_t>((row * 4 + col) * 4));
        }
    }
    return mat;
}

static bool looks_like_transform_matrix(uint32_t addr);
static bool resolve_active_camera_addr(uint32_t& camera_addr);

static bool resolve_active_camera_transform_addr(uint32_t& camera_xf_addr) {
    uint32_t camera_addr = 0;
    const bool ok = resolve_active_camera_addr(camera_addr);
    if (!ok)
        return false;

    const auto addrs = get_addresses();
    camera_xf_addr = camera_addr + addrs.transform_offset;
    return true;
}

static bool resolve_active_camera_addr(uint32_t& camera_addr) {
    const auto addrs = get_addresses();
    constexpr uint32_t k_object_list_offset = 0x810;

    if (!g_dolphin.is_connected() || addrs.state_manager < 0x80000000)
        return false;

    const uint32_t state_manager = addrs.state_manager;
    const uint32_t player = g_dolphin.read_u32(state_manager + addrs.player_offset);
    if (player < 0x80000000)
        return false;

    const uint32_t gun_ptr = g_dolphin.read_u32(player + addrs.cannon_offset);
    if (gun_ptr < 0x80000000)
        return false;

    const uint32_t camera_manager = g_dolphin.read_u32(state_manager + addrs.camera_manager_offset);
    if (camera_manager < 0x80000000)
        return false;

    const uint16_t camera_uid = static_cast<uint16_t>(g_dolphin.read_u32(camera_manager) >> 16);
    if (camera_uid == 0xFFFF)
        return false;

    const uint32_t object_list = g_dolphin.read_u32(state_manager + k_object_list_offset);
    if (object_list < 0x80000000)
        return false;

    const uint32_t camera = g_dolphin.read_u32(object_list + ((camera_uid & 0x3FFu) << 3) + 4);
    if (camera < 0x80000000)
        return false;

    g_app.dbg_fp_cam = camera;
    camera_addr = camera;
    return true;
}

static bool read_player_yaw_from_transform2d(uint32_t addr, float& yaw_deg_out) {
    const float m00 = g_dolphin.read_float(addr + 0x00);
    const float m01 = g_dolphin.read_float(addr + 0x04);
    const float m10 = g_dolphin.read_float(addr + 0x10);
    const float m11 = g_dolphin.read_float(addr + 0x14);

    if (!std::isfinite(m00) || !std::isfinite(m01) || !std::isfinite(m10) || !std::isfinite(m11))
        return false;

    const float row0_len = std::sqrt(m00 * m00 + m01 * m01);
    const float row1_len = std::sqrt(m10 * m10 + m11 * m11);
    const float det = m00 * m11 - m01 * m10;

    if (row0_len < 0.7f || row0_len > 1.3f || row1_len < 0.7f || row1_len > 1.3f)
        return false;
    if (std::fabs(det) < 0.5f)
        return false;

    yaw_deg_out = std::atan2(m01, m00) * (180.0f / static_cast<float>(M_PI));
    return true;
}

static float wrap_angle_radians(float angle) {
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kTwoPi = 2.0f * kPi;
    while (angle > kPi)
        angle -= kTwoPi;
    while (angle < -kPi)
        angle += kTwoPi;
    return angle;
}

static void append_player_yaw_log(const char* status, float yaw_deg, float yaw_delta_deg) {
    (void)status;
    (void)yaw_deg;
    (void)yaw_delta_deg;
}

static void append_matrix_dump_log(const char* label, uint32_t addr) {
    (void)label;
    (void)addr;
}

static void update_cannon_rotation_debug(uint32_t gun_ptr, const float* target_mat, bool after_write) {
    g_app.dbg_cannon_rot_valid = false;
    if (!g_settings.log_cannon_rotation_debug || gun_ptr < 0x80000000)
        return;

    float values[9] = {};
    float targets[9] = {};
    extract_basis9(target_mat, targets);

    static uint32_t last_gun_ptr = 0;
    static float last_app_written[9] = {};
    static bool last_app_written_valid = false;
    static bool pre_sample_valid = false;
    static bool pre_game_changed = false;
    static float pre_max_drift = 0.0f;
    static float pre_drift[9] = {};

    for (int i = 0; i < 9; ++i) {
        const uint32_t addr = gun_ptr + k_cannon_rotation_offsets[i];
        const float value = g_dolphin.read_float(addr);
        values[i] = value;

        g_app.dbg_cannon_rot_addr[i] = addr;
        g_app.dbg_cannon_rot_target[i] = targets[i];
    }

    if (!after_write) {
        pre_game_changed = false;
        pre_max_drift = 0.0f;
        for (int i = 0; i < 9; ++i) {
            const float drift =
                (last_app_written_valid && gun_ptr == last_gun_ptr)
                    ? std::fabs(values[i] - last_app_written[i])
                    : 0.0f;
            pre_drift[i] = drift;
            if (drift > pre_max_drift)
                pre_max_drift = drift;
            if (drift > 0.0005f)
                pre_game_changed = true;

            g_app.dbg_cannon_rot_pre[i] = values[i];
            g_app.dbg_cannon_rot_post[i] = values[i];
            g_app.dbg_cannon_rot_drift[i] = drift;
        }

        pre_sample_valid = true;
        g_app.dbg_cannon_rot_max_drift = pre_max_drift;
        g_app.dbg_cannon_rot_game_changed = pre_game_changed;
        g_app.dbg_cannon_rot_valid = true;
        return;
    }

    for (int i = 0; i < 9; ++i) {
        g_app.dbg_cannon_rot_post[i] = values[i];
        last_app_written[i] = values[i];
    }
    g_app.dbg_cannon_rot_valid = true;
    last_gun_ptr = gun_ptr;
    last_app_written_valid = true;

    static auto last_log = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    const bool throttle_ok =
        last_log.time_since_epoch().count() == 0 ||
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_log).count() >= 33;

    if (pre_sample_valid && (pre_game_changed || pre_max_drift > 0.00001f) && throttle_ok) {
        last_log = now;
        ++g_app.dbg_cannon_rot_log_count;
    }
}

static Matrix3x4 read_debug_matrix3x4(uint32_t addr) {
    Matrix3x4 mat = {};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 4; ++col) {
            mat.at(row, col) = g_dolphin.read_float(addr + static_cast<uint32_t>((row * 4 + col) * 4));
        }
    }
    return mat;
}

static bool read_vec3(uint32_t addr, float& x, float& y, float& z) {
    x = g_dolphin.read_float(addr + 0);
    y = g_dolphin.read_float(addr + 4);
    z = g_dolphin.read_float(addr + 8);
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

static bool read_transform_translation(uint32_t addr, float& x, float& y, float& z) {
    if (addr < 0x80000000)
        return false;

    x = g_dolphin.read_float(addr + 0x0c);
    y = g_dolphin.read_float(addr + 0x1c);
    z = g_dolphin.read_float(addr + 0x2c);
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

static bool normalize3(float& x, float& y, float& z) {
    const float len = std::sqrt(x * x + y * y + z * z);
    if (!std::isfinite(len) || len < 0.0001f)
        return false;
    x /= len;
    y /= len;
    z /= len;
    return true;
}

static void clear_latched_gun_target();
static void reset_gun_target_switch_state();

static bool looks_like_real_player_projectile(uint32_t obj, uint16_t owner_uid) {
    if (obj < 0x80000000)
        return false;

    const uint32_t attribs = g_dolphin.read_u32(obj + 0xe8);
    const uint32_t weapon_type = g_dolphin.read_u32(obj + 0xf0);
    constexpr uint32_t kKnownProjectileAttribMask = (1u << 18) - 1u;
    constexpr uint32_t kArmCannonAttrib = 1u << 11;

    if ((attribs & ~kKnownProjectileAttribMask) != 0)
        return false;
    if ((attribs & kArmCannonAttrib) == 0)
        return false;
    if (weapon_type > 8)
        return false;

    const uint16_t read_owner_uid = static_cast<uint16_t>(g_dolphin.read_u32(obj + 0xec) >> 16);
    if (read_owner_uid != owner_uid)
        return false;

    return looks_like_transform_matrix(obj + 0x34) &&
           looks_like_transform_matrix(obj + 0x170 + 0x14);
}

static bool player_is_first_person_gun_ready(uint32_t player) {
    if (player < 0x80000000)
        return false;

    const uint32_t camera_state = g_dolphin.read_u32(player + 0x2f4);
    const uint32_t morph_state = g_dolphin.read_u32(player + 0x2f8);
    const float gun_alpha = g_dolphin.read_float(player + 0x494);
    const uint32_t holster_state = g_dolphin.read_u32(player + 0x498);

    if (camera_state != 0) return false; // CPlayer::EPlayerCameraState::FirstPerson
    if (morph_state != 0) return false;  // CPlayer::EPlayerMorphBallState::Unmorphed
    if (!std::isfinite(gun_alpha) || gun_alpha < 0.95f) return false;
    if (holster_state != 2) return false; // CPlayer::EGunHolsterState::Drawn
    return true;
}

static bool player_is_first_person_unmorphed(uint32_t player) {
    if (player < 0x80000000)
        return false;

    const uint32_t camera_state = g_dolphin.read_u32(player + 0x2f4);
    const uint32_t morph_state = g_dolphin.read_u32(player + 0x2f8);
    return camera_state == 0 && morph_state == 0;
}

static bool offhand_prime_yaw(const Pose& offhand, float yaw_delta_deg, float& yaw_out) {
    if (!offhand.valid)
        return false;

    Pose adjusted = offhand;
    apply_openvr_world_yaw(adjusted, yaw_delta_deg);

    const float qx = adjusted.qx;
    const float qy = adjusted.qy;
    const float qz = adjusted.qz;
    const float qw = adjusted.qw;

    const float sin_yaw = 2.0f * (qw * qy + qx * qz);
    const float cos_yaw = 1.0f - 2.0f * (qy * qy + qz * qz);
    if (!std::isfinite(sin_yaw) || !std::isfinite(cos_yaw))
        return false;

    float prime_forward_x = -sin_yaw;
    float prime_forward_y = cos_yaw;
    const float len = std::sqrt(prime_forward_x * prime_forward_x +
                                prime_forward_y * prime_forward_y);
    if (!std::isfinite(len) || len < 0.0001f)
        return false;
    prime_forward_x /= len;
    prime_forward_y /= len;

    const float r12 = 2.0f * (qy * qz - qw * qx);
    const float pitch_up = std::asin(std::clamp(-r12, -1.0f, 1.0f));
    constexpr float kAllowBackwardFlipPitch = 90.0f * static_cast<float>(M_PI / 180.0f);
    if (std::fabs(pitch_up) > kAllowBackwardFlipPitch) {
        prime_forward_x = -prime_forward_x;
        prime_forward_y = -prime_forward_y;
    }

    yaw_out = yaw_from_prime_xy(prime_forward_x, prime_forward_y);
    return true;
}

static bool write_directional_movement_velocity(const Pose& move_input,
                                                const Pose& yaw_controller,
                                                const Pose& hmd,
                                                float yaw_delta_deg,
                                                float dt) {
    g_app.dbg_directional_move_active = false;
    g_app.dbg_directional_move_yaw_deg = 0.0f;
    g_app.dbg_directional_move_stick_mag = 0.0f;

    if (!g_settings.directional_movement_enabled || !move_input.valid ||
        !yaw_controller.valid ||
        !g_dolphin.is_connected() || !g_app.dolphin_ok) {
        g_directional_move_speed = 0.0f;
        return false;
    }

    float stick_x = 0.0f;
    float stick_y = 0.0f;
    int stick_axis = 0;
    select_xr_dpad_stick_axis(move_input, stick_x, stick_y, stick_axis);
    const float stick_mag = std::sqrt(stick_x * stick_x + stick_y * stick_y);
    g_app.dbg_directional_move_stick_mag = stick_mag;
    if (stick_mag < g_settings.directional_movement_deadzone) {
        g_directional_move_speed = 0.0f;
        return false;
    }
    const float move_axis = stick_y;
    const float move_mag = std::fabs(move_axis);
    if (move_mag < g_settings.directional_movement_deadzone ||
        std::fabs(stick_x) > move_mag) {
        g_directional_move_speed = 0.0f;
        return false;
    }

    if (left_controller_is_near_head_for_dpad(move_input, hmd)) {
        g_directional_move_speed = 0.0f;
        return false;
    }

    const auto addrs = get_addresses();
    const uint32_t state_mgr = addrs.state_manager;
    if (state_mgr < 0x80000000) {
        g_directional_move_speed = 0.0f;
        return false;
    }

    const uint32_t player = g_dolphin.read_u32(state_mgr + addrs.player_offset);
    if (!player_is_first_person_unmorphed(player)) {
        g_directional_move_speed = 0.0f;
        return false;
    }

    float yaw = 0.0f;
    if (!offhand_prime_yaw(yaw_controller, yaw_delta_deg, yaw)) {
        g_directional_move_speed = 0.0f;
        return false;
    }
    if (move_axis > 0.0f)
        yaw = wrap_angle_radians(yaw + static_cast<float>(M_PI));

    constexpr uint32_t k_player_velocity_offset = 0x138;
    const float vx = g_dolphin.read_float(player + k_player_velocity_offset + 0x00);
    const float vy = g_dolphin.read_float(player + k_player_velocity_offset + 0x04);
    const float vz = g_dolphin.read_float(player + k_player_velocity_offset + 0x08);
    if (!std::isfinite(vx) || !std::isfinite(vy) || !std::isfinite(vz))
        return false;

    const float flat_speed = std::sqrt(vx * vx + vy * vy);
    if (flat_speed < 0.001f) {
        g_directional_move_speed = 0.0f;
        return false;
    }

    float dir_x = 0.0f;
    float dir_y = 0.0f;
    prime_xy_from_yaw(yaw, dir_x, dir_y);
    const float stick_scale = std::clamp(move_mag, 0.0f, 1.0f);
    const float target_speed = g_settings.directional_movement_speed * stick_scale;
    constexpr uint32_t k_player_movement_state_offset = 0x258;
    const uint32_t movement_state = g_dolphin.read_u32(player + k_player_movement_state_offset);
    const bool on_ground = movement_state == 0;
    if (g_directional_move_speed <= 0.0f || g_directional_move_speed > target_speed)
        g_directional_move_speed = std::min(flat_speed, target_speed);

    const float accel = on_ground ? g_settings.directional_movement_accel
                                  : g_settings.directional_movement_air_accel;
    const float max_delta = accel *
                            std::clamp(dt, 0.0f, 0.05f);
    if (g_directional_move_speed < target_speed)
        g_directional_move_speed = std::min(target_speed, g_directional_move_speed + max_delta);
    else
        g_directional_move_speed = std::max(target_speed, g_directional_move_speed - max_delta);

    g_dolphin.write_float(player + k_player_velocity_offset + 0x00, dir_x * g_directional_move_speed);
    g_dolphin.write_float(player + k_player_velocity_offset + 0x04, dir_y * g_directional_move_speed);
    g_dolphin.write_float(player + k_player_velocity_offset + 0x08, vz);

    g_app.dbg_directional_move_active = true;
    g_app.dbg_directional_move_yaw_deg = yaw * (180.0f / static_cast<float>(M_PI));
    return true;
}

static void disarm_memory_writes() {
    g_last_written_basis_valid = false;
    g_last_desired_basis_valid = false;
    reset_gun_target_switch_state();
    if (g_dolphin.is_connected()) {
        g_dolphin.write_u32(k_render_hook_expected_gun_addr, 0);
        g_dolphin.write_u32(k_gun_target_hook_addr, 0);
        g_dolphin.write_u32(k_gun_target_hook_addr + 4, 0xffffffffu);
    }
}

static void log_player_owned_projectiles(uint32_t state_mgr, uint32_t player, uint32_t gun_ptr, const float* basis) {
    if (!g_dolphin.is_connected())
        return;

    constexpr uint32_t k_object_list_offset = 0x810;
    constexpr uint32_t k_max_objects = 1024;
    static uint32_t seen[k_max_objects] = {};
    static auto last_scan = std::chrono::steady_clock::time_point{};

    const auto now = std::chrono::steady_clock::now();
    if (last_scan.time_since_epoch().count() != 0 &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_scan).count() < 8) {
        return;
    }
    last_scan = now;

    const uint32_t object_list = g_dolphin.read_u32(state_mgr + k_object_list_offset);
    if (object_list < 0x80000000)
        return;

    const uint16_t player_uid = static_cast<uint16_t>(g_dolphin.read_u32(player + 0x8) >> 16);
    if (player_uid == 0xffff)
        return;

    for (uint32_t i = 0; i < k_max_objects; ++i) {
        const uint32_t obj = g_dolphin.read_u32(object_list + (i << 3) + 4);
        if (obj < 0x80000000 || obj == player || obj == gun_ptr)
            continue;

        const uint32_t obj_uid_word = g_dolphin.read_u32(obj + 0x8);
        const uint16_t owner_uid = static_cast<uint16_t>(g_dolphin.read_u32(obj + 0xec) >> 16);
        if (owner_uid != player_uid)
            continue;

        float ox, oy, oz;
        float vx, vy, vz;
        if (!read_vec3(obj + 0x34 + 0x0c, ox, oy, oz))
            continue;
        const bool have_velocity = read_vec3(obj + 0x170 + 0xb0, vx, vy, vz);

        const bool can_correct_projectile =
            have_velocity && looks_like_real_player_projectile(obj, owner_uid);
        const uint32_t marker = obj ^ obj_uid_word ^ (static_cast<uint32_t>(owner_uid) << 16);
        const bool already_corrected = can_correct_projectile && seen[i] == marker;

        if (can_correct_projectile && !already_corrected) {
            write_basis_preserve_translation(obj + 0x34, basis);
            write_basis_preserve_translation(obj + 0x170 + 0x14, basis);
            seen[i] = marker;
        }
        const bool billboarded = false;
        if (!can_correct_projectile || already_corrected)
            continue;
    }
}

static void log_projectile_fire_debug(const char* label, uint32_t base_addr, uint32_t& last_count) {
    (void)label;
    (void)base_addr;
    (void)last_count;
}

static void append_yaw2d_dump_log(const char* label, uint32_t addr) {
    (void)label;
    (void)addr;
}

static void append_yawpair_dump_log(const char* label, uint32_t addr) {
    (void)label;
    (void)addr;
}

static bool is_finite_nonzero(float v) {
    return std::isfinite(v) && std::fabs(v) > 0.0001f;
}

static bool looks_like_transform_matrix(uint32_t addr) {
    if (addr < 0x80000000) return false;

    float m[12] = {};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 4; ++col) {
            m[row * 4 + col] = g_dolphin.read_float(addr + static_cast<uint32_t>((row * 4 + col) * 4));
        }
    }

    const float row0_len = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
    const float row1_len = std::sqrt(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
    const float row2_len = std::sqrt(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
    if (row0_len < 0.5f || row0_len > 1.5f) return false;
    if (row1_len < 0.5f || row1_len > 1.5f) return false;
    if (row2_len < 0.5f || row2_len > 1.5f) return false;

    const float dot01 = std::fabs(m[0] * m[4] + m[1] * m[5] + m[2] * m[6]);
    const float dot02 = std::fabs(m[0] * m[8] + m[1] * m[9] + m[2] * m[10]);
    const float dot12 = std::fabs(m[4] * m[8] + m[5] * m[9] + m[6] * m[10]);
    if (dot01 > 0.25f || dot02 > 0.25f || dot12 > 0.25f) return false;

    return is_finite_nonzero(m[0]) || is_finite_nonzero(m[5]) || is_finite_nonzero(m[10]);
}

static float get_player_yaw_delta_degrees() {
    if (!g_dolphin.is_connected()) {
        g_app.dbg_player_yaw_deg = 0.0f;
        g_app.dbg_player_yaw_delta_deg = 0.0f;
        append_player_yaw_log("no_dolphin_or_state", 0.0f, 0.0f);
        return 0.0f;
    }

    uint32_t player_yaw_addr = 0;
    if (!resolve_active_camera_transform_addr(player_yaw_addr)) {
        g_app.dbg_player_yaw_deg = 0.0f;
        g_app.dbg_player_yaw_delta_deg = 0.0f;
        append_player_yaw_log("no_camera_xf", 0.0f, 0.0f);
        return 0.0f;
    }
    float current_player_yaw_deg = 0.0f;
    if (!read_player_yaw_from_transform2d(player_yaw_addr, current_player_yaw_deg)) {
        g_app.dbg_player_yaw_deg = 0.0f;
        g_app.dbg_player_yaw_delta_deg = 0.0f;
        append_player_yaw_log("invalid_camera_xf_2d", 0.0f, 0.0f);
        append_matrix_dump_log("camera_xf_raw", player_yaw_addr);
        return 0.0f;
    }

    g_app.dbg_player_yaw_deg = current_player_yaw_deg;
    const float yaw_delta_deg =
        wrap_angle_radians((current_player_yaw_deg + 180.0f) *
                           (static_cast<float>(M_PI) / 180.0f)) *
        (-180.0f / static_cast<float>(M_PI));
    const float corrected_yaw_delta_deg = yaw_delta_deg;
    g_app.dbg_player_yaw_delta_deg = corrected_yaw_delta_deg;
    append_player_yaw_log("ok_xf_2d", g_app.dbg_player_yaw_deg, corrected_yaw_delta_deg);
    return corrected_yaw_delta_deg;
}

static void rotate_prime_translation_xy(Matrix3x4& mat, float yaw_deg) {
    const float yaw_rad = yaw_deg * (static_cast<float>(M_PI) / 180.0f);
    const float c = std::cos(yaw_rad);
    const float s = std::sin(yaw_rad);
    const float x = mat.at(0, 3);
    const float y = mat.at(1, 3);
    mat.at(0, 3) = c * x - s * y;
    mat.at(1, 3) = s * x + c * y;
}

static void rotate_prime_matrix_yaw(Matrix3x4& mat, float yaw_deg) {
    const float yaw_rad = yaw_deg * (static_cast<float>(M_PI) / 180.0f);
    const float c = std::cos(yaw_rad);
    const float s = std::sin(yaw_rad);
    for (int col = 0; col < 4; ++col) {
        const float x = mat.at(0, col);
        const float y = mat.at(1, col);
        mat.at(0, col) = c * x - s * y;
        mat.at(1, col) = s * x + c * y;
    }
}

static void apply_openvr_world_yaw(Pose& pose, float yaw_deg) {
    const float yaw_rad = yaw_deg * (static_cast<float>(M_PI) / 180.0f);
    const float half = yaw_rad * 0.5f;
    const float sy = std::sinf(half);
    const float cy = std::cosf(half);

    // Turning the in-game facing direction should rotate the controller's
    // orientation frame around OpenVR's up axis so pitch/roll operate in the
    // turned frame too. Do not rotate the absolute tracked position here,
    // because the local gun offset is handled separately and rotating both
    // causes the cannon to orbit twice as far.
    const float qx = pose.qx;
    const float qy = pose.qy;
    const float qz = pose.qz;
    const float qw = pose.qw;

    // World-yaw quaternion around +Y, pre-multiplied into the controller pose.
    pose.qx = cy * qx + sy * qz;
    pose.qy = cy * qy + sy * qw;
    pose.qz = cy * qz - sy * qx;
    pose.qw = cy * qw - sy * qy;
}

static void apply_position_recenter(Pose& pose) {
    pose.px -= g_controller_base_prime_x;
    pose.py -= g_controller_base_prime_y;
    pose.pz -= g_controller_base_prime_z;
}

static void write_gun_chain(uint32_t gun_xf, uint32_t beam_xf,
                            uint32_t local_xf, uint32_t world_xf,
                            const float* basis) {
    // Upstream/base first, most downstream/visible world xf last.
    write_basis_preserve_translation(gun_xf, basis);
    write_basis_preserve_translation(beam_xf, basis);
    write_basis_preserve_translation(local_xf, basis);
    write_basis_preserve_translation(world_xf, basis);
}

struct GunTargetPick {
    uint16_t uid = 0xffff;
    uint32_t obj = 0;
    float ray_x = 0.0f;
    float ray_y = 0.0f;
    float ray_z = 0.0f;
    float dir_x = 0.0f;
    float dir_y = 0.0f;
    float dir_z = 0.0f;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float along = 0.0f;
    float perp = 0.0f;
    float score = 0.0f;
    bool suppress_orbit_hook = false;
};

static bool actor_has_material(uint32_t obj, int bit) {
    if (obj < 0x80000000 || bit < 0 || bit >= 64)
        return false;

    const uint64_t hi = static_cast<uint64_t>(g_dolphin.read_u32(obj + 0x68));
    const uint64_t lo = static_cast<uint64_t>(g_dolphin.read_u32(obj + 0x6c));
    const uint64_t bits = (hi << 32) | lo;
    return (bits & (uint64_t{1} << static_cast<uint64_t>(bit))) != 0;
}

static bool actor_is_targetable(uint32_t obj) {
    if (obj < 0x80000000)
        return false;
    const uint8_t flags = g_dolphin.read_u8(obj + 0xe7);
    return (flags & 0x01u) != 0; // CActor::xe7_31_targetable
}

static bool actor_is_grapple_point(uint32_t obj) {
    if (obj < 0x80000000)
        return false;

    const uint32_t vtable = g_dolphin.read_u32(obj);
    return vtable >= 0x803E0D00u && vtable < 0x803E0D70u; // __vt__19CScriptGrapplePoint
}

static bool pick_gun_ray_target(uint32_t state_mgr, uint32_t player, uint32_t gun_ptr,
                                uint32_t world_xf, const float* mat, bool strict_lock_aim,
                                GunTargetPick& pick) {
    constexpr uint32_t k_object_list_offset = 0x810;
    constexpr uint32_t k_max_objects = 1024;

    // Targeting cost model:
    // - Old app path: scan every object, then run material/target checks on many candidates.
    // - Current path: scan object pointers, reject by cannon-facing box first, then do expensive checks.
    // - Vanilla path: BuildNearList with material filters, then screen/orbit tests on a small candidate list.
    g_app.dbg_gun_target_candidates = 0;

    if (!g_settings.gun_targeting_enabled || state_mgr < 0x80000000 ||
        player < 0x80000000 || gun_ptr < 0x80000000) {
        return false;
    }

    float ray_x = 0.0f;
    float ray_y = 0.0f;
    float ray_z = 0.0f;
    if (!read_transform_translation(world_xf, ray_x, ray_y, ray_z))
        return false;

    float dir_x = -mat[4];
    float dir_y = mat[5];
    float dir_flat_z = 0.0f;
    if (!normalize3(dir_x, dir_y, dir_flat_z))
        return false;
    float dir_z = mat[6] * -dir_y + -mat[2] * dir_x;
    if (!std::isfinite(dir_z))
        return false;
    dir_z = std::clamp(dir_z, -0.98f, 0.98f);
    const float flat_scale = std::sqrt(std::max(0.0f, 1.0f - dir_z * dir_z));
    dir_x *= flat_scale;
    dir_y *= flat_scale;
    pick.ray_x = ray_x;
    pick.ray_y = ray_y;
    pick.ray_z = ray_z;
    pick.dir_x = dir_x;
    pick.dir_y = dir_y;
    pick.dir_z = dir_z;

    const uint32_t object_list = g_dolphin.read_u32(state_mgr + k_object_list_offset);
    if (object_list < 0x80000000)
        return false;

    const uint16_t player_uid = static_cast<uint16_t>(g_dolphin.read_u32(player + 0x8) >> 16);
    const float max_along = g_settings.gun_targeting_distance;
    const float max_perp = g_settings.gun_targeting_radius;
    const bool scan_mode = g_dolphin.read_u32(player + 0x330) == 1;
    bool found = false;

    for (uint32_t i = 0; i < k_max_objects; ++i) {
        const uint32_t obj = g_dolphin.read_u32(object_list + (i << 3) + 4);
        if (obj < 0x80000000 || obj == player || obj == gun_ptr)
            continue;

        float ox = 0.0f, oy = 0.0f, oz = 0.0f;
        if (!read_transform_translation(obj + 0x34, ox, oy, oz))
            continue;

        float vx = ox - ray_x;
        float vy = oy - ray_y;
        float vz = oz - ray_z;
        const float along = vx * dir_x + vy * dir_y + vz * dir_z;
        if (along < 0.5f || along > max_along)
            continue;

        const float nearest_x = ray_x + dir_x * along;
        const float nearest_y = ray_y + dir_y * along;
        const float nearest_z = ray_z + dir_z * along;
        const float dx = ox - nearest_x;
        const float dy = oy - nearest_y;
        const float dz = oz - nearest_z;
        const float perp_sq = dx * dx + dy * dy + dz * dz;
        if (!std::isfinite(perp_sq) || perp_sq > max_perp * max_perp)
            continue;

        if (!looks_like_transform_matrix(obj + 0x34))
            continue;

        const uint32_t uid_word = g_dolphin.read_u32(obj + 0x8);
        const uint16_t uid = static_cast<uint16_t>(uid_word >> 16);
        if (uid == 0xffff || uid == player_uid)
            continue;

        const uint16_t owner_uid = static_cast<uint16_t>(g_dolphin.read_u32(obj + 0xec) >> 16);
        if (owner_uid == player_uid)
            continue;

        const bool has_target = actor_has_material(obj, 40);
        const bool has_character = actor_has_material(obj, 33) || actor_has_material(obj, 55);
        const bool has_orbit = actor_has_material(obj, 41);
        const bool has_scan = actor_has_material(obj, 39);
        const bool targetable = actor_is_targetable(obj);
        const bool grapple_point = actor_is_grapple_point(obj);
        const bool combat_candidate = has_target && targetable;
        const bool orbit_candidate = has_orbit && targetable;
        const bool scan_candidate = has_scan;
        const bool grapple_candidate = grapple_point && has_orbit;
        if (!combat_candidate && !orbit_candidate && !scan_candidate && !grapple_candidate)
            continue;

        const float perp = std::sqrt(perp_sq);
        if (!std::isfinite(perp) || perp > max_perp)
            continue;

        if (strict_lock_aim) {
            const float aim_cone_perp = std::min(max_perp, 0.18f + along * (scan_mode ? 0.045f : 0.032f));
            if (perp > aim_cone_perp)
                continue;
        }

        ++g_app.dbg_gun_target_candidates;
        float score = perp + along * 0.015f;
        if (grapple_candidate) {
            score -= 0.40f;
        } else if (strict_lock_aim) {
            score = perp * 4.0f + along * 0.002f;
            if (scan_mode && has_scan)
                score -= 0.03f;
            if (!scan_mode && has_target)
                score -= 0.03f;
            if (!has_scan && !has_target && !has_orbit)
                score += 0.10f;
        } else if (scan_mode) {
            if (has_scan)
                score -= 0.20f;
            else if (!has_character)
                score += 0.75f;
            if (!has_scan && !has_target)
                score += 1.0f;
        } else {
            if (!has_character)
                score += has_scan ? 0.35f : 0.75f;
            if (!has_target)
                score += has_scan ? 0.75f : 1.25f;
        }
        if (!found || score < pick.score) {
            found = true;
            pick.uid = uid;
            pick.obj = obj;
            pick.x = ox;
            pick.y = oy;
            pick.z = oz;
            pick.along = along;
            pick.perp = perp;
            pick.score = score;
            pick.suppress_orbit_hook = grapple_candidate;
        }
    }

    return found;
}

static void log_gun_targeting_result(bool found, const GunTargetPick& pick, uint32_t player) {
    (void)found;
    (void)pick;
    (void)player;
}

static void cache_gun_target(uint32_t player, const GunTargetPick& pick) {
    std::lock_guard<std::mutex> lock(g_gun_target_mutex);
    g_cached_target_player = player;
    g_cached_target_uid = pick.uid;
    g_cached_target_x = pick.x;
    g_cached_target_y = pick.y;
    g_cached_target_z = pick.z;
    g_cached_target_time = std::chrono::steady_clock::now();
}

static void write_gun_target_hook_scratch(uint32_t player, uint16_t uid) {
    g_dolphin.write_u32(k_gun_target_hook_addr, player);
    g_dolphin.write_u32(k_gun_target_hook_addr + 4, 0);
    g_dolphin.write_u16(k_gun_target_hook_addr + 4, uid);
}

static bool orbit_lock_button_held(uint32_t state_mgr) {
    const uint8_t held0 = g_dolphin.read_u8(state_mgr + k_final_input_offset + 0x2c);
    return (held0 & 0x04u) != 0; // CFinalInput::x2c_b29_L
}

static bool scan_visor_active(uint32_t player) {
    return player >= 0x80000000 && g_dolphin.read_u32(player + 0x330) == 1;
}

static bool gun_pitch_from_matrix(const float* mat, float& pitch_out) {
    float dir_x = -mat[4];
    float dir_y = mat[5];
    float dir_flat_z = 0.0f;
    if (!normalize3(dir_x, dir_y, dir_flat_z))
        return false;

    float dir_z = mat[6] * -dir_y + -mat[2] * dir_x;
    if (!std::isfinite(dir_z))
        return false;

    pitch_out = std::asin(std::clamp(dir_z, -0.98f, 0.98f));
    return std::isfinite(pitch_out);
}

static void write_scan_pitch_from_matrix(uint32_t player, const float* mat) {
    if (!scan_visor_active(player))
        return;

    float pitch = 0.0f;
    if (!gun_pitch_from_matrix(mat, pitch))
        return;

    constexpr float kMaxScanPitch = 1.35f;
    pitch = std::clamp(pitch, -kMaxScanPitch, kMaxScanPitch);
    constexpr float kPitchSmooth = 0.45f;
    g_smooth_pitch = g_smooth_pitch * kPitchSmooth + pitch * (1.0f - kPitchSmooth);

    const auto addrs = get_addresses();
    g_dolphin.write_float(player + addrs.pitch_offset, g_smooth_pitch);
}

static void reset_lock_camera_pitch_suppression() {
    g_lock_pitch_have_unlocked = false;
    g_lock_pitch_unlocked = 0.0f;
    g_smooth_pitch = 0.0f;
}

static void suppress_lock_camera_pitch(uint32_t state_mgr, uint32_t player) {
    const auto addrs = get_addresses();
    const uint32_t pitch_addr = player + addrs.pitch_offset;
    const float pitch = g_dolphin.read_float(pitch_addr);
    if (!std::isfinite(pitch))
        return;

    constexpr float kMaxFirstPersonPitch = 1.55f;
    const float clamped_pitch = std::clamp(pitch, -kMaxFirstPersonPitch, kMaxFirstPersonPitch);
    if (!orbit_lock_button_held(state_mgr)) {
        g_lock_pitch_unlocked = clamped_pitch;
        g_lock_pitch_have_unlocked = true;
        return;
    }

    if (!g_lock_pitch_have_unlocked) {
        g_lock_pitch_unlocked = clamped_pitch;
        g_lock_pitch_have_unlocked = true;
    }
    g_dolphin.write_float(pitch_addr, g_lock_pitch_unlocked);
}

static bool target_uid_still_exists(uint32_t state_mgr, uint16_t uid, uint32_t expected_obj) {
    if (uid == 0xffff)
        return false;

    constexpr uint32_t k_object_list_offset = 0x810;
    const uint32_t object_list = g_dolphin.read_u32(state_mgr + k_object_list_offset);
    if (object_list < 0x80000000)
        return false;

    const uint32_t obj = g_dolphin.read_u32(object_list + ((uid & 0x3ffu) << 3) + 4);
    if (obj < 0x80000000)
        return false;
    if (expected_obj >= 0x80000000 && obj != expected_obj)
        return false;

    const uint16_t live_uid = static_cast<uint16_t>(g_dolphin.read_u32(obj + 0x8) >> 16);
    return live_uid == uid;
}

static void clear_latched_gun_target() {
    g_latched_target_active = false;
    g_latched_target_player = 0;
    g_latched_target_uid = 0xffff;
    g_latched_target_obj = 0;
}

static void reset_gun_target_switch_state() {
    g_last_target_player = 0;
    g_last_target_uid = 0xffff;
    clear_latched_gun_target();
}

static void write_gun_targeting(uint32_t state_mgr, uint32_t player, uint32_t gun_ptr,
                                uint32_t world_xf, const float* mat) {
    static auto s_last_pick_time = std::chrono::steady_clock::time_point{};
    static GunTargetPick s_last_pick = {};
    static bool s_last_pick_valid = false;
    static uint32_t s_last_pick_player = 0;
    static uint32_t s_last_pick_gun = 0;

    g_app.dbg_gun_target_uid = 0xffff;
    g_app.dbg_gun_target_obj = 0;
    g_app.dbg_gun_target_score = 0.0f;
    g_app.dbg_gun_target_along = 0.0f;
    g_app.dbg_gun_target_perp = 0.0f;
    g_app.dbg_gun_target_write = false;

    if (!g_settings.gun_targeting_enabled || player < 0x80000000) {
        s_last_pick_valid = false;
        reset_gun_target_switch_state();
        return;
    }

    const bool lock_held = orbit_lock_button_held(state_mgr);
    if (!lock_held) {
        clear_latched_gun_target();
    } else if (g_latched_target_active) {
        if (g_latched_target_player != player ||
            !target_uid_still_exists(state_mgr, g_latched_target_uid, g_latched_target_obj)) {
            clear_latched_gun_target();
        }
    }

    const auto now = std::chrono::steady_clock::now();
    const bool same_context = s_last_pick_valid &&
                              s_last_pick_player == player &&
                              s_last_pick_gun == gun_ptr;
    const bool throttle_scan = same_context &&
                               s_last_pick_time.time_since_epoch().count() != 0 &&
                               std::chrono::duration_cast<std::chrono::milliseconds>(now - s_last_pick_time).count() < 33;

    GunTargetPick pick = {};
    bool found = false;
    if (throttle_scan) {
        pick = s_last_pick;
        found = true;
    } else {
        found = pick_gun_ray_target(state_mgr, player, gun_ptr, world_xf, mat, lock_held, pick);
        s_last_pick_time = now;
        s_last_pick_player = player;
        s_last_pick_gun = gun_ptr;
        s_last_pick = pick;
        s_last_pick_valid = found;
        log_gun_targeting_result(found, pick, player);
    }
    if (!found) {
        g_last_target_player = 0;
        g_last_target_uid = 0xffff;
        write_gun_target_hook_scratch(0, 0xffff);
        return;
    }

    if (pick.suppress_orbit_hook) {
        g_last_target_player = 0;
        g_last_target_uid = 0xffff;
        clear_latched_gun_target();
        write_gun_target_hook_scratch(0, 0xffff);

        g_app.dbg_gun_target_uid = pick.uid;
        g_app.dbg_gun_target_obj = pick.obj;
        g_app.dbg_gun_target_score = pick.score;
        g_app.dbg_gun_target_along = pick.along;
        g_app.dbg_gun_target_perp = pick.perp;
        g_app.dbg_gun_target_write = false;
        return;
    }

    cache_gun_target(player, pick);
    clear_latched_gun_target();
    g_last_target_player = player;
    g_last_target_uid = pick.uid;
    write_gun_target_hook_scratch(player, pick.uid);

    g_app.dbg_gun_target_uid = pick.uid;
    g_app.dbg_gun_target_obj = pick.obj;
    g_app.dbg_gun_target_score = pick.score;
    g_app.dbg_gun_target_along = pick.along;
    g_app.dbg_gun_target_perp = pick.perp;
    g_app.dbg_gun_target_write = true;
}

static void write_gun_matrix(const Matrix3x4& mat) {
    const auto addrs = get_addresses();
    constexpr float s = 0.0f;

    for (int i = 0; i < 12; ++i) {
        g_smooth_mat[i] = g_smooth_mat[i] * s + mat.m[i] * (1.0f - s);
    }
    g_app.last_matrix = *reinterpret_cast<Matrix3x4*>(g_smooth_mat);

    if (!g_dolphin.is_connected()) return;

    g_app.dbg_mem_base = g_dolphin.get_mem_base();
    const uint32_t state_mgr = addrs.state_manager;
    g_app.dbg_state_mgr = state_mgr;
    if (state_mgr < 0x80000000) return;

    const uint32_t player = g_dolphin.read_u32(state_mgr + addrs.player_offset);
    g_app.dbg_player = player;
    if (player < 0x80000000) {
        reset_lock_camera_pitch_suppression();
        return;
    }

    g_app.dbg_pitch_addr = player + addrs.pitch_offset;
    g_app.dbg_cam_mgr = g_dolphin.read_u32(state_mgr + addrs.camera_manager_offset);
    g_app.dbg_fp_cam = 0;
    g_app.dbg_gun_ptr = 0;
    g_app.dbg_gun_xf = 0;
    g_app.dbg_beam_xf = 0;
    g_app.dbg_world_xf = 0;
    g_app.dbg_local_xf = 0;
    g_app.dbg_cannon_rot_valid = false;
    g_app.dbg_gun_target_write = false;
    if (!player_is_first_person_gun_ready(player)) {
        reset_lock_camera_pitch_suppression();
        disarm_memory_writes();
        return;
    }
    suppress_lock_camera_pitch(state_mgr, player);

    const uint32_t gun_ptr = g_dolphin.read_u32(player + addrs.cannon_offset);
    g_app.dbg_gun_ptr = gun_ptr;

    if (gun_ptr >= 0x80000000) {
        const uint32_t gun_xf = gun_ptr + addrs.gun_xf_offset;
        const uint32_t beam_xf = gun_ptr + addrs.beam_xf_offset;
        const uint32_t world_xf = gun_ptr + addrs.world_xf_offset;
        const uint32_t local_xf = gun_ptr + addrs.local_xf_offset;

        g_app.dbg_gun_xf = gun_xf;
        g_app.dbg_beam_xf = beam_xf;
        g_app.dbg_world_xf = world_xf;
        g_app.dbg_local_xf = local_xf;

        const bool gun_chain_valid =
            looks_like_transform_matrix(gun_xf) &&
            looks_like_transform_matrix(beam_xf) &&
            looks_like_transform_matrix(world_xf) &&
            looks_like_transform_matrix(local_xf);
        if (!gun_chain_valid) {
            disarm_memory_writes();
            return;
        }
        if (addrs.gun_pos >= 0x80000000) {
            g_dolphin.write_float(addrs.gun_pos + 0, g_smooth_mat[3]);
            g_dolphin.write_float(addrs.gun_pos + 4, g_smooth_mat[7]);
            g_dolphin.write_float(addrs.gun_pos + 8, g_smooth_mat[11]);
        }
        write_gun_targeting(state_mgr, player, gun_ptr, world_xf, g_smooth_mat);

        static bool projectile_debug_cleared = false;
        if (!projectile_debug_cleared) {
            clear_u32_range(k_projectile_fire_debug_addr, 0x80);
            clear_u32_range(k_projectile_fire_debug_addr + 0x100, 0x80);
            clear_u32_range(k_projectile_probe_debug_addr, 0x80);
            projectile_debug_cleared = true;
        }
        log_player_owned_projectiles(state_mgr, player, gun_ptr, g_smooth_mat);
        if (g_settings.log_cannon_rotation_debug)
            update_cannon_rotation_debug(gun_ptr, g_smooth_mat, false);

        float render_hook_basis[9] = {};
        extract_basis9(g_smooth_mat, render_hook_basis);
        write_contiguous_basis9(k_render_hook_basis_addr, render_hook_basis);
        g_dolphin.write_u32(k_render_hook_expected_gun_addr, gun_ptr);

        extract_basis9(g_smooth_mat, g_last_written_basis);
        g_last_written_basis_valid = true;
    } else {
        disarm_memory_writes();
        return;
    }
}

static void tracking_thread() {
    using clock = std::chrono::high_resolution_clock;
    auto last = clock::now();

    while (g_running.load(std::memory_order_relaxed)) {
        const auto loop_start = clock::now();
        const float dt = std::chrono::duration<float>(loop_start - last).count();
        last = loop_start;
        if (dt > 0.0f) g_app.tracker_fps = 1.0f / dt;
        g_app.tracker_dt_ms = dt * 1000.0f;
        if (g_app.tracker_dt_ms > 20.0f) ++g_app.tracker_drop_count;

        if (g_app.openvr_ok) {
            const auto poll_start = clock::now();
            Pose pose = {};
            Pose left_pose = {};
            Pose hmd_pose = {};
            g_openvr.get_latest_poses(g_settings.use_right_hand, pose, left_pose, hmd_pose);
            const auto poll_end = clock::now();
            g_app.tracker_poll_ms = std::chrono::duration<float, std::milli>(poll_end - poll_start).count();
            {
                std::lock_guard<std::mutex> lock(g_pose_mutex);
                g_latest_pose = pose;
                g_latest_left_pose = left_pose;
                g_latest_hmd_pose = hmd_pose;
            }
            g_app.last_pose = pose;
        }

        push_timing_sample(g_app.tracker_hist_ms, g_app.tracker_hist_head, g_app.tracker_dt_ms);

        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
}

static void dpad_thread() {
    constexpr auto k_tick = std::chrono::milliseconds(1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    HighResWaitTimer wait_timer;

    while (g_running.load(std::memory_order_relaxed)) {
        const auto loop_start = std::chrono::steady_clock::now();

        Pose left_pose = {};
        Pose hmd_pose = {};
        {
            std::lock_guard<std::mutex> lock(g_pose_mutex);
            left_pose = g_latest_left_pose;
            hmd_pose = g_latest_hmd_pose;
        }

        if (g_app.active && g_app.dolphin_ok) {
            apply_xr_dpad_input(left_pose, hmd_pose);
        } else {
            g_app.dbg_xr_dpad_active = false;
            g_app.dbg_xr_dpad_dir = XrDpadNone;
        }

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - loop_start);
        if (elapsed < k_tick) {
            wait_timer.wait_for(std::chrono::duration_cast<std::chrono::microseconds>(k_tick - elapsed));
        }
    }
}

static void write_scan_pitch_from_controller_matrix(const Matrix3x4& mat) {
    if (!g_dolphin.is_connected() || !g_app.dolphin_ok)
        return;

    const auto addrs = get_addresses();
    const uint32_t state_mgr = addrs.state_manager;
    if (state_mgr < 0x80000000)
        return;

    const uint32_t player = g_dolphin.read_u32(state_mgr + addrs.player_offset);
    if (player < 0x80000000)
        return;

    write_scan_pitch_from_matrix(player, mat.m);
}

static void writer_thread() {
    using clock = std::chrono::steady_clock;
    auto last = clock::now();
    constexpr auto k_tick = std::chrono::microseconds(8333);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    HighResWaitTimer wait_timer;
    while (g_running.load(std::memory_order_relaxed)) {
        const auto loop_start = clock::now();
        const float dt = std::chrono::duration<float>(loop_start - last).count();
        last = loop_start;
        if (dt > 0.0f) g_app.fps = 1.0f / dt;
        g_app.writer_dt_ms = dt * 1000.0f;
        if (g_app.writer_dt_ms > 20.0f) ++g_app.writer_drop_count;

        Pose pose = {};
        Pose left_pose = {};
        Pose hmd_pose = {};
        {
            std::lock_guard<std::mutex> lock(g_pose_mutex);
            pose = g_latest_pose;
            left_pose = g_latest_left_pose;
            hmd_pose = g_latest_hmd_pose;
        }

        static bool inactive_disarmed = false;
        if (!g_app.active || !g_app.dolphin_ok) {
            g_last_written_basis_valid = false;
            g_last_desired_basis_valid = false;
            if (!inactive_disarmed) {
                disarm_memory_writes();
                inactive_disarmed = true;
            }
        } else {
            inactive_disarmed = false;
        }

        if (pose.valid) {
            const auto work_start = clock::now();
            float player_yaw_delta_deg = 0.0f;
            if (g_app.active && g_app.dolphin_ok) {
                player_yaw_delta_deg = get_player_yaw_delta_degrees();
                const bool right_stick = g_settings.directional_movement_use_right_stick;
                const Pose& movement_stick_pose =
                    right_stick
                        ? (g_settings.use_right_hand ? pose : left_pose)
                        : (g_settings.use_right_hand ? left_pose : pose);
                const Pose& movement_yaw_pose = left_pose;
                write_directional_movement_velocity(movement_stick_pose, movement_yaw_pose,
                                                    hmd_pose, player_yaw_delta_deg, dt);
            } else {
                g_app.dbg_directional_move_active = false;
                g_app.dbg_directional_move_stick_mag = 0.0f;
                g_directional_move_speed = 0.0f;
            }
            Pose adjusted_pose = pose;
            apply_openvr_world_yaw(adjusted_pose, player_yaw_delta_deg);
            Matrix3x4 controller_mat_no_offset = OpenVRManager::pose_to_prime_matrix(
                adjusted_pose,
                0.0f, 0.0f, 0.0f,
                g_settings.rot_offset_x, g_settings.rot_offset_y, g_settings.rot_offset_z,
                g_settings.world_scale
            );
            if (g_app.recenter_requested.exchange(false, std::memory_order_relaxed) ||
                !g_translation_base_valid) {
                g_camera_base_prime_x = 0.0f;
                g_camera_base_prime_y = 0.0f;
                g_camera_base_prime_z = 0.0f;
                g_controller_base_prime_x = 0.0f;
                g_controller_base_prime_y = 0.0f;
                g_controller_base_prime_z = 0.0f;
                g_translation_base_valid = true;
            }

            Matrix3x4 mat = OpenVRManager::pose_to_prime_matrix(
                adjusted_pose,
                g_settings.offset_x, g_settings.offset_y, g_settings.offset_z,
                g_settings.rot_offset_x, g_settings.rot_offset_y, g_settings.rot_offset_z,
                g_settings.world_scale
            );
            if (g_translation_base_valid) {
                mat.at(0, 3) = g_camera_base_prime_x + (controller_mat_no_offset.at(0, 3) - g_controller_base_prime_x) + (g_settings.offset_x * g_settings.world_scale);
                mat.at(1, 3) = g_camera_base_prime_y + (controller_mat_no_offset.at(1, 3) - g_controller_base_prime_y) - (g_settings.offset_z * g_settings.world_scale);
                mat.at(2, 3) = g_camera_base_prime_z + (controller_mat_no_offset.at(2, 3) - g_controller_base_prime_z) + (g_settings.offset_y * g_settings.world_scale);
            }
            g_app.last_matrix = mat;

            if (g_app.active && g_app.dolphin_ok) {
                write_scan_pitch_from_controller_matrix(mat);
                const bool should_write = !g_settings.require_trigger ||
                                          pose.trigger >= g_settings.trigger_threshold;
                if (should_write) {
                    const auto write_start = clock::now();
                    write_gun_matrix(mat);
                    const auto write_end = clock::now();
                    g_app.writer_write_ms = std::chrono::duration<float, std::milli>(write_end - write_start).count();
                }
            }

            const auto work_end = clock::now();
            g_app.writer_work_ms = std::chrono::duration<float, std::milli>(work_end - work_start).count();
        }

        push_timing_sample(g_app.writer_hist_ms, g_app.writer_hist_head, g_app.writer_dt_ms);
        static uint32_t last_normal_fire_count = 0xffffffffu;
        static uint32_t last_secondary_fire_count = 0xffffffffu;
        log_projectile_fire_debug("normal", k_projectile_fire_debug_addr, last_normal_fire_count);
        log_projectile_fire_debug("secondary", k_projectile_fire_debug_addr + 0x100, last_secondary_fire_count);
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - loop_start);
        const auto tick = k_tick;
        if (elapsed < tick) {
            wait_timer.wait_for(tick - elapsed);
        }
    }
}

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static bool init_d3d(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 500;
    sd.BufferDesc.Height = 650;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate = {60, 1};
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL feat;
    if (FAILED(D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
        &sd, &g_swapchain, &g_device, &feat, &g_context))) {
        return false;
    }

    ID3D11Texture2D* back = nullptr;
    g_swapchain->GetBuffer(0, IID_PPV_ARGS(&back));
    g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();
    return true;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    TimerResolutionScope timer_resolution;
    g_settings.load();

    WNDCLASSEXW wc = {
        sizeof(wc), CS_CLASSDC, WndProc, 0, 0, hInstance,
        nullptr, nullptr, nullptr, nullptr, L"PrimedGun", nullptr
    };
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowW(
        L"PrimedGun", L"PrimedGun",
        WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
        100, 100, 516, 688, nullptr, nullptr, hInstance, nullptr);

    if (!init_d3d(g_hwnd)) {
        MessageBoxW(g_hwnd, L"Failed to init D3D11", L"Error", MB_OK);
        return 1;
    }

    ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.1f, 0.1f, 0.12f, 1.0f);

    g_app.dolphin_ok = g_dolphin.connect();
    g_app.dolphin_status = g_dolphin.status();
    g_app.openvr_ok = g_openvr.init();
    g_app.openvr_status = g_openvr.status();

    std::thread tracker(tracking_thread);
    std::thread dpad(dpad_thread);
    std::thread writer(writer_thread);

    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            continue;
        }

        if (g_app.reconnect_dolphin_requested.exchange(false, std::memory_order_relaxed)) {
            g_app.active = false;
            g_app.dolphin_ok = false;
            g_dolphin.disconnect();
            g_app.dolphin_ok = g_dolphin.connect();
            g_app.dolphin_status = g_dolphin.status();
        }

        if (g_app.reconnect_openvr_requested.exchange(false, std::memory_order_relaxed)) {
            g_app.active = false;
            g_app.openvr_ok = false;
            g_openvr.shutdown();
            g_app.openvr_ok = g_openvr.init();
            g_app.openvr_status = g_openvr.status();
        }

        g_app.dolphin_status = g_dolphin.status();
        g_app.openvr_status = g_openvr.status();

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        draw_gui(g_settings, g_app, g_dolphin, g_openvr);
        ImGui::Render();

        float clear[4] = {0.1f, 0.1f, 0.12f, 1.0f};
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swapchain->Present(1, 0);
    }

    g_running.store(false, std::memory_order_relaxed);
    if (tracker.joinable()) tracker.join();
    if (dpad.joinable()) dpad.join();
    if (writer.joinable()) writer.join();

    g_settings.save();
    g_openvr.shutdown();
    g_dolphin.disconnect();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    if (g_rtv) g_rtv->Release();
    if (g_swapchain) g_swapchain->Release();
    if (g_context) g_context->Release();
    if (g_device) g_device->Release();

    DestroyWindow(g_hwnd);
    UnregisterClassW(L"PrimedGun", hInstance);
    return 0;
}
//
