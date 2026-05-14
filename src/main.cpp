#include <Windows.h>
#include <mmsystem.h>
#include <d3d11.h>
#include <chrono>
#include <thread>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <fstream>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <sstream>
#include <tlhelp32.h>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

#include "settings.h"
#include "dolphin_memory.h"
#include "tracking_math.h"
#include "gui.h"
#include "resource.h"
#include "PrimedGunShared.h"

namespace fs = std::filesystem;

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
static std::wofstream g_hook_log;
static HANDLE         g_shared_mapping = nullptr;
static PrimedGun::SharedState* g_shared_state = nullptr;
static bool g_last_auto_dolphin_xr_controls = false;

static constexpr uint32_t kButtonTriggerClick = 1u << 0;
static constexpr uint32_t kButtonThumbstickClick = 1u << 1;
static constexpr uint32_t kButtonA = 1u << 2;
static constexpr uint32_t kButtonB = 1u << 3;

static float g_smooth_mat[12] = {1,0,0,0, 0,1,0,0, 0,0,1,0};
static float g_smooth_pitch = 0.0f;
static bool g_lock_pitch_have_unlocked = false;
static float g_lock_pitch_unlocked = 0.0f;
static std::atomic<bool> g_running = true;
static std::atomic<uint64_t> g_last_cannon_transform_write_ms = 0;
static std::atomic<uint32_t> g_last_cannon_transform_player = 0;
static std::atomic<uint32_t> g_last_cannon_transform_gun = 0;
static std::atomic<uint32_t> g_cannon_transform_ready_frames = 0;
static std::mutex g_pose_mutex;

struct LoadedPatch {
    uint32_t address = 0;
    uint32_t value = 0;
};
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
static std::atomic<uint32_t> g_view_height_generation = 0;
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

static fs::path local_app_data_path() {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, static_cast<DWORD>(MAX_PATH));
    if (len == 0 || len >= MAX_PATH)
        return fs::temp_directory_path();
    return fs::path(buffer);
}

static void app_hook_log(std::wstring_view message) {
    if (g_hook_log.is_open()) {
        g_hook_log << message << L"\n";
        g_hook_log.flush();
    }
}


static std::wstring hex32(uint32_t value) {
    std::wstringstream ss;
    ss << L"0x" << std::hex << std::uppercase << value;
    return ss.str();
}

static bool app_logging_enabled() {
    char value[16] = {};
    const DWORD len = GetEnvironmentVariableA("PRIMEDGUN_ENABLE_LOGS", value, sizeof(value));
    return len > 0 && len < sizeof(value) && value[0] == '1';
}

static std::wstring widen_ascii(std::string_view value) {
    return std::wstring(value.begin(), value.end());
}

static std::string trim_ascii(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

static bool parse_hex_u32(const std::string& text, uint32_t& value) {
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(text.c_str(), &end, 16);
    if (!end || *end != '\0')
        return false;
    value = static_cast<uint32_t>(parsed);
    return true;
}

static bool parse_patch_line(const std::string& raw_line, LoadedPatch& patch) {
    std::string line = trim_ascii(raw_line);
    if (line.empty() || line[0] == '#' || line[0] == '$' || line[0] == '[')
        return false;

    const size_t comment = line.find_first_of("#;");
    if (comment != std::string::npos)
        line = trim_ascii(line.substr(0, comment));
    if (line.empty())
        return false;

    const size_t dword = line.find(":dword:");
    if (dword != std::string::npos) {
        uint32_t address = 0;
        uint32_t value = 0;
        std::string addressText = line.substr(0, dword);
        std::string valueText = line.substr(dword + 7);
        if (addressText.rfind("0x", 0) == 0 || addressText.rfind("0X", 0) == 0)
            addressText = addressText.substr(2);
        if (valueText.rfind("0x", 0) == 0 || valueText.rfind("0X", 0) == 0)
            valueText = valueText.substr(2);
        if (parse_hex_u32(addressText, address) && parse_hex_u32(valueText, value)) {
            patch = {address, value};
            return true;
        }
        return false;
    }

    std::istringstream stream(line);
    std::string left;
    std::string right;
    if (!(stream >> left >> right))
        return false;

    uint32_t code = 0;
    uint32_t value = 0;
    if (!parse_hex_u32(left, code) || !parse_hex_u32(right, value))
        return false;

    if (static_cast<uint8_t>(code >> 24) == 0x04) {
        patch.address = 0x80000000u | (code & 0x01ffffffu);
        patch.value = value;
        return true;
    }

    return false;
}

static void open_app_hook_log() {
    if (!app_logging_enabled())
        return;

    std::error_code ec;
    const fs::path log_dir = local_app_data_path() / L"PrimedGun";
    fs::create_directories(log_dir, ec);
    g_hook_log.open(log_dir / L"PrimedGun_AppHook.log", std::ios::app);
}

static fs::path exe_directory() {
    wchar_t buffer[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(MAX_PATH));
    return fs::path(buffer).parent_path();
}

static fs::path user_profile_path() {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD len = GetEnvironmentVariableW(L"USERPROFILE", buffer, static_cast<DWORD>(MAX_PATH));
    if (len == 0 || len >= MAX_PATH)
        return {};
    return fs::path(buffer);
}

static fs::path dolphin_gcpad_profile_path() {
    const fs::path profile = user_profile_path();
    if (profile.empty())
        return {};
    return profile / L"Documents" / L"Dolphin Emulator" / L"Config" / L"GCPadNew.ini";
}

static fs::path dolphin_gcpad_backup_path() {
    const fs::path profile = dolphin_gcpad_profile_path();
    if (profile.empty())
        return {};
    return profile.parent_path() / L"GCPadNew.ini.primedgun-gcpad1.bak";
}

static fs::path dolphin_hotkeys_path() {
    const fs::path profile = user_profile_path();
    if (profile.empty())
        return {};
    return profile / L"Documents" / L"Dolphin Emulator" / L"Config" / L"Hotkeys.ini";
}

static fs::path dolphin_ini_path() {
    const fs::path profile = user_profile_path();
    if (profile.empty())
        return {};
    return profile / L"Documents" / L"Dolphin Emulator" / L"Config" / L"Dolphin.ini";
}

static fs::path dolphin_gfx_ini_path() {
    const fs::path profile = user_profile_path();
    if (profile.empty())
        return {};
    return profile / L"Documents" / L"Dolphin Emulator" / L"Config" / L"GFX.ini";
}

static fs::path dolphin_hotkeys_profile_path() {
    const fs::path profile = user_profile_path();
    if (profile.empty())
        return {};
    return profile / L"Documents" / L"Dolphin Emulator" / L"Config" /
        L"Profiles" / L"Hotkeys" / L"hotkeys.ini";
}

static fs::path primedgun_backup_path_for(const fs::path& path) {
    if (path.empty())
        return {};
    return path.parent_path() / (path.filename().wstring() + L".primedgun.bak");
}

static fs::path dolphin_gm8e01_vr_settings_path() {
    const fs::path profile = user_profile_path();
    if (profile.empty())
        return {};
    return profile / L"Documents" / L"Dolphin Emulator" /
        L"GameSettingsVR" / L"GM8E01.ini";
}

static fs::path dolphin_gm8e01_settings_path() {
    const fs::path profile = user_profile_path();
    if (profile.empty())
        return {};
    return profile / L"Documents" / L"Dolphin Emulator" /
        L"GameSettings" / L"GM8E01.ini";
}

static std::vector<std::string> read_text_lines(const fs::path& path) {
    std::vector<std::string> lines;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

static bool find_ini_section(const std::vector<std::string>& lines, const std::string& section,
                             size_t& section_begin, size_t& section_end) {
    const std::string section_header = "[" + section + "]";
    for (size_t i = 0; i < lines.size(); ++i) {
        if (trim_ascii(lines[i]) != section_header)
            continue;

        section_begin = i;
        section_end = lines.size();
        for (size_t j = i + 1; j < lines.size(); ++j) {
            const std::string trimmed = trim_ascii(lines[j]);
            if (trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']') {
                section_end = j;
                break;
            }
        }
        return true;
    }
    section_begin = lines.size();
    section_end = lines.size();
    return false;
}

static bool ini_line_is_section_header(const std::string& line, std::string* section_name = nullptr) {
    const std::string trimmed = trim_ascii(line);
    if (trimmed.size() < 2 || trimmed.front() != '[' || trimmed.back() != ']')
        return false;

    if (section_name)
        *section_name = trimmed.substr(1, trimmed.size() - 2);
    return true;
}


static std::vector<std::string> remove_ini_sections(
    const std::vector<std::string>& lines,
    const std::vector<std::string>& sections) {
    std::vector<std::string> output;
    output.reserve(lines.size());

    bool skipping = false;
    for (const std::string& line : lines) {
        std::string section_name;
        if (ini_line_is_section_header(line, &section_name)) {
            skipping = std::find(sections.begin(), sections.end(), section_name) != sections.end();
        }

        if (!skipping)
            output.push_back(line);
    }
    return output;
}


static std::vector<std::string> split_text_lines(const char* text) {
    std::vector<std::string> lines;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(line);
    }
    return lines;
}


static void remove_ini_section_line(std::vector<std::string>& lines,
                                    const std::string& section,
                                    const std::string& line_to_remove) {
    const std::string section_header = "[" + section + "]";
    bool in_section = false;
    std::vector<std::string> output;
    output.reserve(lines.size());

    for (const std::string& line : lines) {
        std::string section_name;
        if (ini_line_is_section_header(line, &section_name)) {
            in_section = trim_ascii(line) == section_header;
            output.push_back(line);
            continue;
        }

        if (in_section && trim_ascii(line) == line_to_remove)
            continue;

        output.push_back(line);
    }

    lines.swap(output);
}


static void add_ini_section_line(std::vector<std::string>& lines,
                                 const std::string& section,
                                 const std::string& line_to_add) {
    const std::string section_header = "[" + section + "]";
    for (size_t i = 0; i < lines.size(); ++i) {
        if (trim_ascii(lines[i]) != section_header)
            continue;

        size_t insert_at = i + 1;
        for (; insert_at < lines.size(); ++insert_at) {
            const std::string trimmed = trim_ascii(lines[insert_at]);
            if (trimmed == line_to_add)
                return;
            if (trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']')
                break;
        }
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insert_at), line_to_add);
        return;
    }

    lines.push_back(section_header);
    lines.push_back(line_to_add);
}


static void write_text_lines_if_changed(const fs::path& path, const std::vector<std::string>& lines) {
    std::ostringstream serialized;
    for (const std::string& line : lines)
        serialized << line << "\n";

    const std::string new_text = serialized.str();
    std::ifstream existing(path, std::ios::binary);
    const std::string old_text((std::istreambuf_iterator<char>(existing)),
                               std::istreambuf_iterator<char>());
    if (old_text == new_text)
        return;

    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << new_text;
}

static void backup_file_once(const fs::path& path) {
    const fs::path backup = primedgun_backup_path_for(path);
    if (path.empty() || backup.empty() || !fs::exists(path) || fs::exists(backup))
        return;

    std::error_code ec;
    fs::create_directories(backup.parent_path(), ec);
    fs::copy_file(path, backup, fs::copy_options::none, ec);
}

static void restore_file_backup(const fs::path& path) {
    const fs::path backup = primedgun_backup_path_for(path);
    if (path.empty() || backup.empty() || !fs::exists(backup))
        return;

    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    fs::copy_file(backup, path, fs::copy_options::overwrite_existing, ec);
    if (!ec)
        fs::remove(backup, ec);
}

static void backup_dolphin_gcpad1_controls() {
    const fs::path profile = dolphin_gcpad_profile_path();
    const fs::path backup = dolphin_gcpad_backup_path();
    if (profile.empty() || backup.empty() || fs::exists(backup))
        return;

    const std::vector<std::string> lines = read_text_lines(profile);
    size_t begin = 0;
    size_t end = 0;
    const bool has_section = find_ini_section(lines, "GCPad1", begin, end);

    std::vector<std::string> backup_lines;
    backup_lines.push_back("PRIMEDGUN_GCPAD1_BACKUP_V1");
    backup_lines.push_back(std::string("HadSection=") + (has_section ? "1" : "0"));
    if (has_section) {
        backup_lines.insert(backup_lines.end(), lines.begin() + static_cast<std::ptrdiff_t>(begin),
                            lines.begin() + static_cast<std::ptrdiff_t>(end));
    }
    write_text_lines_if_changed(backup, backup_lines);
}

static void restore_dolphin_gcpad1_controls() {
    const fs::path profile = dolphin_gcpad_profile_path();
    const fs::path backup = dolphin_gcpad_backup_path();
    if (profile.empty() || backup.empty() || !fs::exists(backup))
        return;

    const std::vector<std::string> backup_lines = read_text_lines(backup);
    if (backup_lines.size() < 2 || backup_lines[0] != "PRIMEDGUN_GCPAD1_BACKUP_V1")
        return;

    const bool had_section = trim_ascii(backup_lines[1]) == "HadSection=1";
    std::vector<std::string> lines = read_text_lines(profile);
    size_t begin = 0;
    size_t end = 0;
    const bool has_current_section = find_ini_section(lines, "GCPad1", begin, end);

    std::vector<std::string> output;
    if (has_current_section) {
        output.insert(output.end(), lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(begin));
        if (had_section) {
            output.insert(output.end(), backup_lines.begin() + 2, backup_lines.end());
        }
        output.insert(output.end(), lines.begin() + static_cast<std::ptrdiff_t>(end), lines.end());
    } else {
        output = lines;
        if (had_section) {
            if (!output.empty() && !output.back().empty())
                output.push_back({});
            output.insert(output.end(), backup_lines.begin() + 2, backup_lines.end());
        }
    }

    write_text_lines_if_changed(profile, output);
    std::error_code ec;
    fs::remove(backup, ec);
}

static void apply_ini_section_values(const fs::path& path, const std::string& section,
                                     const std::vector<std::pair<std::string, std::string>>& values,
                                     const std::vector<std::string>& remove_keys) {
    std::vector<std::string> lines = read_text_lines(path);

    const std::string section_header = "[" + section + "]";
    size_t section_begin = lines.size();
    size_t section_end = lines.size();
    find_ini_section(lines, section, section_begin, section_end);

    if (section_begin == lines.size()) {
        if (!lines.empty() && !lines.back().empty())
            lines.push_back({});
        lines.push_back(section_header);
        section_begin = lines.size() - 1;
        section_end = lines.size();
    }

    auto key_from_line = [](const std::string& line) -> std::string {
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            return {};
        return trim_ascii(line.substr(0, eq));
    };

    std::vector<bool> used(values.size(), false);
    std::vector<std::string> output;
    output.reserve(lines.size() + values.size());

    for (size_t i = 0; i < lines.size(); ++i) {
        if (i <= section_begin || i >= section_end) {
            output.push_back(lines[i]);
            continue;
        }

        const std::string key = key_from_line(lines[i]);
        if (key.empty()) {
            output.push_back(lines[i]);
            continue;
        }

        if (std::find(remove_keys.begin(), remove_keys.end(), key) != remove_keys.end())
            continue;

        bool replaced = false;
        for (size_t value_i = 0; value_i < values.size(); ++value_i) {
            if (values[value_i].first == key) {
                output.push_back(values[value_i].first + " = " + values[value_i].second);
                used[value_i] = true;
                replaced = true;
                break;
            }
        }
        if (!replaced)
            output.push_back(lines[i]);
    }

    size_t insert_at = output.size();
    if (section_end != lines.size()) {
        size_t sections_seen = 0;
        insert_at = output.size();
        for (size_t i = 0; i < output.size(); ++i) {
            if (trim_ascii(output[i]) == section_header) {
                sections_seen = 1;
                insert_at = i + 1;
                continue;
            }
            if (sections_seen == 1) {
                const std::string trimmed = trim_ascii(output[i]);
                if (trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']') {
                    insert_at = i;
                    break;
                }
                insert_at = i + 1;
            }
        }
    }
    

    std::vector<std::string> additions;
    for (size_t i = 0; i < values.size(); ++i) {
        if (!used[i])
            additions.push_back(values[i].first + " = " + values[i].second);
    }
    output.insert(output.begin() + static_cast<std::ptrdiff_t>(insert_at),
                  additions.begin(), additions.end());

    write_text_lines_if_changed(path, output);
}

static void disable_ini_section_entries(const fs::path& path, const std::string& section) {
    std::vector<std::string> lines = read_text_lines(path);

    const std::string section_header = "[" + section + "]";
    size_t section_begin = lines.size();
    size_t section_end = lines.size();
    find_ini_section(lines, section, section_begin, section_end);

    if (section_begin == lines.size()) {
        if (!lines.empty() && !lines.back().empty())
            lines.push_back({});
        lines.push_back(section_header);
        write_text_lines_if_changed(path, lines);
        return;
    }

    std::vector<std::string> output;
    output.reserve(lines.size());
    output.insert(output.end(), lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(section_begin + 1));

    for (size_t i = section_begin + 1; i < section_end; ++i) {
        const std::string trimmed = trim_ascii(lines[i]);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') {
            output.push_back(lines[i]);
        } else {
            output.push_back("# PrimedGun disabled: " + trimmed);
        }
    }

    output.insert(output.end(), lines.begin() + static_cast<std::ptrdiff_t>(section_end), lines.end());
    write_text_lines_if_changed(path, output);
}

static void apply_dolphin_xr_gamecube_controls() {
    restore_dolphin_gcpad1_controls();
    restore_file_backup(dolphin_ini_path());
    restore_file_backup(dolphin_hotkeys_path());
    restore_file_backup(dolphin_hotkeys_profile_path());

    const fs::path profile = dolphin_gcpad_profile_path();
    if (profile.empty())
        return;

    backup_dolphin_gcpad1_controls();

    const std::vector<std::pair<std::string, std::string>> values = {
        {"Device", "OpenXR/0/OpenXR Controller"},
        {"Buttons/A", "`Right Button A`"},
        {"Buttons/B", "`Right Button B`"},
        {"Buttons/X", "`Left Button X`"},
        {"Buttons/Y", "`Left Button Squeeze`&`Left Squeeze`"},
        {"Buttons/Z", "`Right Button Squeeze`&`Right Squeeze`"},
        {"Buttons/Start", "`Left Button Y`"},
        {"Main Stick/Up", "`Left Thumbstick Y+`"},
        {"Main Stick/Down", "`Left Thumbstick Y-`"},
        {"Main Stick/Left", "`Left Thumbstick X-`"},
        {"Main Stick/Right", "`Left Thumbstick X+`"},
        {"Main Stick/Dead Zone", "12.00"},
        {"C-Stick/Up", "`Right Thumbstick Y+`"},
        {"C-Stick/Down", "`Right Thumbstick Y-`"},
        {"C-Stick/Left", "`Right Thumbstick X-`"},
        {"C-Stick/Right", "`Right Thumbstick X+`"},
        {"C-Stick/Dead Zone", "12.00"},
        {"Triggers/L", "`Left Trigger`"},
        {"Triggers/R", "`Right Trigger`"},
        {"Triggers/L-Analog", "`Left Trigger`"},
        {"Triggers/R-Analog", "`Right Trigger`"},
        {"Rumble/Motor", "`Motor Right`"},
    };
    const std::vector<std::string> remove_keys = {
        "Main Stick/Modifier",
        "C-Stick/Modifier",
    };
    apply_ini_section_values(profile, "GCPad1", values, remove_keys);

    const std::vector<std::pair<std::string, std::string>> hotkey_values = {
        {"Device", "OpenXR/0/OpenXR Controller"},
        {"VR/Reset VR Position", "`Right Button Thumbstick`"},
    };

    const fs::path hotkeys = dolphin_hotkeys_path();
    if (!hotkeys.empty()) {
        backup_file_once(hotkeys);
        apply_ini_section_values(hotkeys, "Hotkeys", hotkey_values, {});
    }

    const fs::path hotkeys_profile = dolphin_hotkeys_profile_path();
    if (!hotkeys_profile.empty()) {
        backup_file_once(hotkeys_profile);
        apply_ini_section_values(hotkeys_profile, "Profile", hotkey_values, {});
    }

    const fs::path dolphin_ini = dolphin_ini_path();
    if (!dolphin_ini.empty()) {
        backup_file_once(dolphin_ini);
        apply_ini_section_values(dolphin_ini, "General",
                                 {{"HotkeysRequireFocus", "False"}}, {});
    }
}


static void sync_dolphin_xr_gamecube_controls(bool enabled) {
    if (enabled) {
        apply_dolphin_xr_gamecube_controls();
    } else {
        restore_dolphin_gcpad1_controls();
        restore_file_backup(dolphin_ini_path());
        restore_file_backup(dolphin_hotkeys_path());
        restore_file_backup(dolphin_hotkeys_profile_path());
    }
    g_last_auto_dolphin_xr_controls = enabled;
}

static void restore_dolphin_borrowed_controls() {
    restore_dolphin_gcpad1_controls();
    restore_file_backup(dolphin_ini_path());
    restore_file_backup(dolphin_hotkeys_path());
    restore_file_backup(dolphin_hotkeys_profile_path());
    g_last_auto_dolphin_xr_controls = false;
}

static std::vector<fs::path> dolphin_gm8e01_vr_settings_paths();

static void apply_dolphin_vr_units_per_meter() {
    const std::vector<std::pair<std::string, std::string>> forced_vr_values = {
        {"UnitsPerMeter", "1.50"},
        {"LeanBackAngle", "0."},
        {"CameraForward", "0."},
        {"HeadLockedCurvature", "0."},
        {"ElementDepth", "0.0010"},
    };
    const std::vector<std::string> inherited_vr_keys = {
        "VirtualScreen",
        "DontClearScreen",
        "OpcodeReplay",
        "AutoVBIFromHMD",
    };

    const std::vector<std::pair<std::string, std::string>> legacy_values = {
        {"UnitsPerMetre", "1.50"},
    };

    for (const fs::path& path : dolphin_gm8e01_vr_settings_paths()) {
        if (path.empty())
            continue;

        apply_ini_section_values(path, "Graphics.VR", forced_vr_values, inherited_vr_keys);
        apply_ini_section_values(path, "GFX.VR", {}, inherited_vr_keys);
        apply_ini_section_values(path, "VR", legacy_values, inherited_vr_keys);
        app_hook_log(L"Applied PrimedGun GM8E01 VR defaults in " + path.wstring());
    }
}

static void apply_dolphin_60fps_cap() {
    if (!g_settings.dolphin_60fps_cap)
        return;

    const fs::path path = dolphin_ini_path();
    if (path.empty())
        return;

    backup_file_once(path);
    apply_ini_section_values(path, "Core",
                             {{"EmulationSpeed", "1."},
                              {"OverclockEnable", "False"},
                              {"Overclock", "1."},
                              {"VIOverclockEnable", "False"},
                              {"VIOverclock", "1."}},
                             {});
    app_hook_log(L"Applied Dolphin 60 FPS cap: normal speed with CPU and VI overclock disabled.");
}

static void apply_dolphin_xr_camera_forward_zero() {
    const fs::path path = dolphin_gfx_ini_path();
    if (path.empty())
        return;

    backup_file_once(path);
    apply_ini_section_values(path, "VR", {{"CameraForward", "0."}}, {});
    app_hook_log(L"Applied Dolphin XR CameraForward = 0.");
}

static void disable_unmanaged_dolphin_codes_in(const fs::path& path) {
    if (path.empty())
        return;

    backup_file_once(path);
    apply_ini_section_values(path, "Core", {{"EnableCheats", "False"}}, {});
    disable_ini_section_entries(path, "ActionReplay_Enabled");
    disable_ini_section_entries(path, "Gecko_Enabled");
    app_hook_log(L"Disabled unmanaged Dolphin AR/Gecko enabled codes in " + path.wstring());
}

static void disable_unmanaged_dolphin_codes() {
    disable_unmanaged_dolphin_codes_in(dolphin_gm8e01_settings_path());
    disable_unmanaged_dolphin_codes_in(dolphin_gm8e01_vr_settings_path());
}

static std::optional<DWORD> find_process_id_by_name(const wchar_t* exe_name);

static std::optional<fs::path> dolphin_process_path(DWORD process_id) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (!process)
        return std::nullopt;

    std::wstring buffer(32768, L'\0');
    DWORD size = static_cast<DWORD>(buffer.size());
    const BOOL ok = QueryFullProcessImageNameW(process, 0, buffer.data(), &size);
    CloseHandle(process);
    if (!ok || size == 0)
        return std::nullopt;

    buffer.resize(size);
    return fs::path(buffer);
}


static void add_unique_path(std::vector<fs::path>& paths, const fs::path& path) {
    if (path.empty())
        return;

    std::error_code ec;
    const fs::path normalized = fs::weakly_canonical(path, ec);
    const fs::path candidate = ec ? fs::absolute(path, ec) : normalized;
    const std::wstring candidate_text = candidate.wstring();
    for (const fs::path& existing : paths) {
        std::error_code existing_ec;
        const fs::path existing_normalized = fs::weakly_canonical(existing, existing_ec);
        const std::wstring existing_text = (existing_ec ? existing : existing_normalized).wstring();
        if (_wcsicmp(existing_text.c_str(), candidate_text.c_str()) == 0)
            return;
    }
    paths.push_back(path);
}


static std::vector<fs::path> dolphin_gm8e01_vr_settings_paths() {
    std::vector<fs::path> paths;
    add_unique_path(paths, dolphin_gm8e01_vr_settings_path());

    const std::optional<DWORD> dolphin_pid = find_process_id_by_name(L"Dolphin.exe");
    if (!dolphin_pid)
        return paths;

    const std::optional<fs::path> dolphin_path = dolphin_process_path(*dolphin_pid);
    if (!dolphin_path)
        return paths;

    const fs::path exe_dir = dolphin_path->parent_path();
    add_unique_path(paths, exe_dir / L"GameSettingsVR" / L"GM8E01.ini");
    if (exe_dir.has_parent_path())
        add_unique_path(paths, exe_dir.parent_path() / L"GameSettingsVR" / L"GM8E01.ini");
    return paths;
}


static const char* primedgun_gm8e01_vr_shader_profile() {
return R"ini([ShaderOverride_Enable]
$Unnamed Shader 1
$Unnamed Shader 2
$Unnamed Shader 3
$Unnamed Shader 4
$Unnamed Shader 5
$Unnamed Shader 6
$Unnamed Shader 7
$Unnamed Shader 8
$Unnamed Shader 9
$Unnamed Shader 10
$Unnamed Shader 11
$Unnamed Shader 12
$Unnamed Shader 13
$Unnamed Shader 14
$Unnamed Shader 15
$Unnamed Shader 17
$Unnamed Shader 18
$Unnamed Shader 19
$Unnamed Shader 21
$Unnamed Shader 22
$Unnamed Shader 23
$Unnamed Shader 24
$Unnamed Shader 25
$Unnamed Shader 26
$Unnamed Shader 27
$Unnamed Shader 28
[ShaderOverride]
$Unnamed Shader 1
Hash=000000000f312e21
Type=PS
match_mode=exact_hash
handling=headlocked
layer=0
texture_mode=include
texture=5a8bf056331986c0

$Unnamed Shader 2
Hash=000000001fa17058
Type=PS
match_mode=exact_hash
handling=headlocked
texture_mode=include
texture=5a8bf056331986c0

$Unnamed Shader 3
Hash=0000000075945182
Type=PS
match_mode=exact_hash
handling=headlocked
texture_mode=include
texture=091f5d24144ede56
texture=1887bd9eea299c6f
texture=3c96a23daee3b1ba
texture=49679a7ccbbbdefb
texture=7b99549023d73d0e
texture=a63ebdc372c2c2b0
texture=b2f411682a7de4a5
texture=b9b77bed5f7fe2d2
texture=e433bfd213466a50
texture=f394c71d898aa628
texture=f6415ff79092ca08

$Unnamed Shader 4
Hash=00000000a1291ea6
Type=PS
match_mode=exact_hash
handling=headlocked
texture_mode=include
texture=5a8bf056331986c0

$Unnamed Shader 5
Hash=00000000bd217dc5
Type=PS
match_mode=exact_hash
handling=headlocked
texture_mode=include
texture=4659c82198269108

$Unnamed Shader 6
Hash=00000000c1202e58
Type=PS
match_mode=exact_hash
handling=headlocked
texture_mode=include
texture=5a8bf056331986c0

$Unnamed Shader 7
Hash=00000000d8ab2990
Type=PS
match_mode=exact_hash
handling=headlocked
texture_mode=include
texture=47382578f2df4238
texture=5a8bf056331986c0

$Unnamed Shader 8
Hash=00000000f5f6bd31
Type=PS
match_mode=exact_hash
handling=headlocked
texture_mode=include
texture=5a8bf056331986c0

$Unnamed Shader 9
Hash=00000000076435a1
Type=PS
match_mode=exact_hash
handling=headlocked
texture_mode=include
texture=5a8bf056331986c0
texture=b093592e137cb513

$Unnamed Shader 10
Hash=0000000017f46bd8
Type=PS
match_mode=exact_hash
handling=headlocked
texture_mode=include
texture=5a8bf056331986c0
texture=7b99549023d73d0e
texture=a63ebdc372c2c2b0

$Unnamed Shader 11
Hash=000000006d51147b
Type=PS
match_mode=exact_hash
handling=headlocked
texture_mode=include
texture=091f5d24144ede56
texture=1887bd9eea299c6f
texture=3c96a23daee3b1ba
texture=4659c82198269108
texture=49679a7ccbbbdefb
texture=7b99549023d73d0e
texture=a63ebdc372c2c2b0
texture=b9b77bed5f7fe2d2
texture=e433bfd213466a50
texture=f394c71d898aa628
texture=f6415ff79092ca08

$Unnamed Shader 12
Hash=00000000a5e4383c
Type=PS
match_mode=exact_hash
handling=headlocked
texture_mode=include
texture=4659c82198269108

$Unnamed Shader 13
Hash=00000000b9ec5b5f
Type=PS
match_mode=exact_hash
handling=headlocked
texture_mode=include
texture=5a8bf056331986c0

$Unnamed Shader 14
Hash=00000000c06e6c69
Type=PS
match_mode=exact_hash
handling=headlocked
texture_mode=include
texture=47382578f2df4238
texture=5a8bf056331986c0

$Unnamed Shader 15
Hash=00000000ed33f8c8
Type=PS
match_mode=exact_hash
handling=headlocked
texture_mode=include
texture=02f5631fb7a3b6ff
texture=5a8bf056331986c0

$Unnamed Shader 17
Hash=00000000d9e56ba1
Type=PS
match_mode=exact_hash
handling=headlocked
texture_mode=include
texture=5a8bf056331986c0

$Unnamed Shader 18
Hash=00000000076435a1
Type=PS
match_mode=exact_hash
handling=headlocked
texture_mode=include
texture=02f5631fb7a3b6ff
texture=847faa3fbc72fafd
texture=c2f7a381aa992132

$Unnamed Shader 19
Hash=0000000017f46bd8
Type=PS
match_mode=exact_hash
handling=headlocked
texture_mode=include
texture=6213dc7b4cea2067
texture=7b99549023d73d0e
texture=847faa3fbc72fafd

$Unnamed Shader 21
Hash=00000000b9ec5b5f
Type=PS
match_mode=exact_hash
handling=headlocked
texture_mode=include
texture=847faa3fbc72fafd
texture=d642fe322e4d3ac8
texture=de81ade3d6923bae
texture=fd423b037c7bf9d8

$Unnamed Shader 22
Hash=00000000d9e56ba1
Type=PS
match_mode=exact_hash
handling=headlocked
texture_mode=include
texture=6213dc7b4cea2067

$Unnamed Shader 23
Hash=00000000ed33f8c8
Type=PS
match_mode=exact_hash
handling=headlocked
texture_mode=include
texture=6213dc7b4cea2067

$Unnamed Shader 24
Hash=00000000c06e6c69
Type=PS
match_mode=exact_hash
handling=headlocked
texture_mode=include
texture=47382578f2df4238
texture=847faa3fbc72fafd
texture=e8f3888a8db51c51

$Unnamed Shader 25
Hash=0000000017f46bd8
Type=PS
match_mode=exact_hash
handling=skip
texture_mode=include
texture=98d66e7812c70dd8

$Unnamed Shader 26
Hash=0000000053bb9636
Type=PS
match_mode=exact_hash
handling=headlocked

$Unnamed Shader 27
Hash=00000000ed33f8c8
Type=PS
match_mode=exact_hash
handling=fullscreen
texture_mode=include
texture=847faa3fbc72fafd

$Unnamed Shader 28
Hash=00000000c06e6c69
Type=PS
match_mode=exact_hash
handling=headlocked
texture_mode=include
texture=b0f5bd8d58ca0c28
texture=c30411f60672bd23
texture=e8f3888a8db51c51
)ini";
}


static std::string primedgun_shader_test_mode() {
    const fs::path path = exe_directory() / L"primedgun_shader_test_mode.txt";
    std::ifstream in(path);
    std::string mode;
    if (!std::getline(in, mode))
        return {};
    return trim_ascii(mode);
}


static void apply_primedgun_shader_test_mode(std::vector<std::string>& shader_lines,
                                             const std::string& mode) {
    if (mode == "disable_c06e6c69" || mode == "disable_visor_beam_icon") {
        remove_ini_section_line(shader_lines, "ShaderOverride_Enable", "$HUD Visor & Beam Icon");
    } else if (mode == "disable_d8ab2990" || mode == "disable_area_variant_4") {
        remove_ini_section_line(shader_lines, "ShaderOverride_Enable", "$HUD Area Variant 4");
    } else if (mode == "disable_c1202e58" || mode == "disable_missing_map_shader") {
        remove_ini_section_line(shader_lines, "ShaderOverride_Enable", "$HUD Missing Map Shader");
    } else if (mode == "disable_hud_core_elements") {
        remove_ini_section_line(shader_lines, "ElementsGroupOverride_Enable", "$HUD Core Elements");
    }
}


static void apply_primedgun_vr_shader_profile_in(const fs::path& path) {
    if (path.empty())
        return;

    backup_file_once(path);

    std::vector<std::string> lines = remove_ini_sections(
        read_text_lines(path),
        {"ShaderOverride_Enable", "ShaderOverride",
         "ElementsGroupOverride_Enable", "ElementsGroupOverride"});

    while (!lines.empty() && lines.back().empty())
        lines.pop_back();
    if (!lines.empty())
        lines.push_back({});

    std::vector<std::string> shader_lines =
        split_text_lines(primedgun_gm8e01_vr_shader_profile());
    apply_primedgun_shader_test_mode(shader_lines, primedgun_shader_test_mode());
    lines.insert(lines.end(), shader_lines.begin(), shader_lines.end());
    write_text_lines_if_changed(path, lines);
}


static void apply_primedgun_vr_shader_profile() {
    if (fs::exists(exe_directory() / L"primedgun_shader_capture_mode.txt")) {
        app_hook_log(L"Skipped PrimedGun GM8E01 VR shader profile; shader capture mode is enabled.");
        return;
    }

    for (const fs::path& path : dolphin_gm8e01_vr_settings_paths()) {
        apply_primedgun_vr_shader_profile_in(path);
        std::wstring suffix;
        const std::string mode = primedgun_shader_test_mode();
        if (!mode.empty())
            suffix = L" (test mode: " + std::wstring(mode.begin(), mode.end()) + L")";
        app_hook_log(L"Forced PrimedGun GM8E01 VR shader profile in " + path.wstring() + suffix);
    }
}


static std::vector<LoadedPatch> load_app_patch_files() {
    std::vector<LoadedPatch> patches;
    const fs::path patchDir = exe_directory() / L"assets" / L"gecko";
    if (!fs::exists(patchDir)) {
        app_hook_log(L"Patch directory not found: " + patchDir.wstring());
        return patches;
    }

    for (const fs::directory_entry& entry : fs::directory_iterator(patchDir)) {
        if (!entry.is_regular_file())
            continue;
        const fs::path path = entry.path();
        const std::wstring ext = path.extension().wstring();
        if (_wcsicmp(ext.c_str(), L".ini") != 0 && _wcsicmp(ext.c_str(), L".txt") != 0)
            continue;

        std::ifstream file(path);
        if (!file.is_open())
            continue;

        std::string line;
        while (std::getline(file, line) && patches.size() < PrimedGun::MaxGamePatches) {
            LoadedPatch patch{};
            if (parse_patch_line(line, patch))
                patches.push_back(patch);
        }
    }

    app_hook_log(L"Loaded " + std::to_wstring(patches.size()) +
        L" app patch line(s) from " + patchDir.wstring());
    return patches;
}

static std::optional<DWORD> find_process_id_by_name(const wchar_t* exe_name) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return std::nullopt;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, exe_name) == 0) {
                CloseHandle(snapshot);
                return entry.th32ProcessID;
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return std::nullopt;
}

static bool is_module_loaded(DWORD process_id, const fs::path& module_path) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process_id);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;

    std::error_code ec;
    const std::wstring wanted = fs::weakly_canonical(module_path, ec).wstring();
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry)) {
        do {
            std::error_code current_ec;
            const fs::path current = fs::weakly_canonical(entry.szExePath, current_ec);
            if (!current_ec && _wcsicmp(current.wstring().c_str(), wanted.c_str()) == 0) {
                CloseHandle(snapshot);
                return true;
            }
        } while (Module32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return false;
}

struct StartedDolphinProcess {
    DWORD process_id = 0;
    HANDLE process = nullptr;
    HANDLE thread = nullptr;
};

static std::optional<StartedDolphinProcess> start_dolphin_suspended(const fs::path& dolphin_path) {
    if (!fs::exists(dolphin_path)) {
        app_hook_log(L"Dolphin path does not exist: " + dolphin_path.wstring());
        return std::nullopt;
    }

    std::wstring command_line = L"\"" + dolphin_path.wstring() + L"\"";
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const fs::path working_directory = dolphin_path.parent_path();
    const BOOL ok = CreateProcessW(
        dolphin_path.c_str(),
        mutable_command.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_SUSPENDED,
        nullptr,
        working_directory.c_str(),
        &startup,
        &process);

    if (!ok) {
        app_hook_log(L"CreateProcessW failed for Dolphin.exe.");
        return std::nullopt;
    }

    app_hook_log(L"PrimedGun.exe started Dolphin.exe suspended with pid " + std::to_wstring(process.dwProcessId));
    return StartedDolphinProcess{process.dwProcessId, process.hProcess, process.hThread};
}

static bool inject_hook_dll(DWORD process_id, const fs::path& dll_path) {
    if (is_module_loaded(process_id, dll_path)) {
        app_hook_log(L"Hook DLL is already loaded in Dolphin.");
        return true;
    }

    std::error_code ec;
    const std::wstring full_dll_path = fs::weakly_canonical(dll_path, ec).wstring();
    const size_t bytes = (full_dll_path.size() + 1) * sizeof(wchar_t);

    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                 PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                                 FALSE, process_id);
    if (!process) {
        app_hook_log(L"OpenProcess failed for DLL injection. Run PrimedGun at the same elevation as Dolphin.");
        return false;
    }

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    auto* set_dll_directory =
        reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(kernel32, "SetDllDirectoryW"));
    if (set_dll_directory) {
        const std::wstring dll_directory = fs::weakly_canonical(dll_path.parent_path(), ec).wstring();
        const size_t dir_bytes = (dll_directory.size() + 1) * sizeof(wchar_t);
        void* remote_dir = VirtualAllocEx(process, nullptr, dir_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (remote_dir && WriteProcessMemory(process, remote_dir, dll_directory.c_str(), dir_bytes, nullptr)) {
            HANDLE dir_thread = CreateRemoteThread(process, nullptr, 0, set_dll_directory, remote_dir, 0, nullptr);
            if (dir_thread) {
                WaitForSingleObject(dir_thread, 5000);
                DWORD dir_result = 0;
                GetExitCodeThread(dir_thread, &dir_result);
                CloseHandle(dir_thread);
                app_hook_log(dir_result != 0
                    ? L"Set Dolphin DLL search directory to " + dll_directory
                    : L"SetDllDirectoryW returned false inside Dolphin.");
            } else {
                app_hook_log(L"CreateRemoteThread failed while setting Dolphin DLL search directory.");
            }
        } else {
            app_hook_log(L"Could not write Dolphin DLL search directory before injection.");
        }
        if (remote_dir)
            VirtualFreeEx(process, remote_dir, 0, MEM_RELEASE);
    }

    void* remote_path = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote_path) {
        app_hook_log(L"VirtualAllocEx failed for DLL injection.");
        CloseHandle(process);
        return false;
    }

    const BOOL wrote = WriteProcessMemory(process, remote_path, full_dll_path.c_str(), bytes, nullptr);
    if (!wrote) {
        app_hook_log(L"WriteProcessMemory failed for DLL injection.");
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    auto* load_library = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(kernel32, "LoadLibraryW"));
    HANDLE thread = CreateRemoteThread(process, nullptr, 0, load_library, remote_path, 0, nullptr);
    if (!thread) {
        app_hook_log(L"CreateRemoteThread failed for DLL injection.");
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    WaitForSingleObject(thread, 10000);
    DWORD exit_code = 0;
    GetExitCodeThread(thread, &exit_code);
    CloseHandle(thread);
    VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
    CloseHandle(process);

    if (exit_code == 0) {
        app_hook_log(L"LoadLibraryW returned null inside Dolphin.");
        return false;
    }

    app_hook_log(L"PrimedGun.exe injected " + full_dll_path + L" into Dolphin.");
    return true;
}

static bool open_live_shared_state();

static void ensure_shared_state() {
    const std::vector<LoadedPatch> patches = load_app_patch_files();

    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                        sizeof(PrimedGun::SharedState), PrimedGun::SharedMemoryName);
    if (!mapping)
        return;

    auto* state = static_cast<PrimedGun::SharedState*>(
        MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(PrimedGun::SharedState)));
    if (!state) {
        CloseHandle(mapping);
        return;
    }

    if (state->magic != PrimedGun::SharedStateMagic || state->version != PrimedGun::SharedStateVersion)
        *state = PrimedGun::SharedState{};
    state->appHeartbeat++;
    const uint32_t next_patch_generation = state->patch.generation + 1;
    state->patch = PrimedGun::PatchState{};
    state->patch.count = static_cast<uint32_t>(std::min<size_t>(patches.size(), PrimedGun::MaxGamePatches));
    state->patch.generation = next_patch_generation;
    for (uint32_t i = 0; i < state->patch.count; ++i) {
        state->patch.patches[i].enabled = 1;
        state->patch.patches[i].address = patches[i].address;
        state->patch.patches[i].value = patches[i].value;
    }

    UnmapViewOfFile(state);
    CloseHandle(mapping);
}

static void verify_app_patches_applied() {
    if (!g_app.dolphin_ok || !g_app.game_rev0_ok || !g_dolphin.is_connected())
        return;
    if (!open_live_shared_state() || !g_shared_state)
        return;
    if (g_shared_state->patch.count == 0 || g_shared_state->patch.count > PrimedGun::MaxGamePatches) {
        ensure_shared_state();
        return;
    }

    bool needs_retry = false;
    uint32_t missing_count = 0;
    for (uint32_t i = 0; i < g_shared_state->patch.count; ++i) {
        PrimedGun::GamePatch& patch = g_shared_state->patch.patches[i];
        if (!patch.enabled || patch.address < 0x80000000)
            continue;

        const uint32_t current = g_dolphin.read_u32(patch.address);
        patch.lastSeen = current;
        if (current == patch.value) {
            patch.applied = 1;
            continue;
        }

        patch.applied = 0;
        needs_retry = true;
        ++missing_count;
    }

    if (needs_retry) {
        ++g_shared_state->patch.generation;
        app_hook_log(L"Patch watchdog requested retry for " + std::to_wstring(missing_count) +
            L" app patch line(s).");
    }
}


static bool app_patches_are_applied() {
    if (!open_live_shared_state() || !g_shared_state)
        return false;
    if (g_shared_state->patch.count == 0 || g_shared_state->patch.count > PrimedGun::MaxGamePatches)
        return false;

    for (uint32_t i = 0; i < g_shared_state->patch.count; ++i) {
        const PrimedGun::GamePatch& patch = g_shared_state->patch.patches[i];
        if (patch.enabled && patch.applied == 0)
            return false;
    }
    return true;
}

static void apply_helmet_opacity_zero() {
    if (!g_app.dolphin_ok || !g_app.game_rev0_ok || !g_dolphin.is_connected())
        return;

    constexpr uint32_t k_gp_game_state = 0x805A8C40u;
    constexpr uint32_t k_game_options_helmet_alpha_offset = 0x17Cu + 0x64u;
    const uint32_t game_state = g_dolphin.read_u32(k_gp_game_state);
    if (game_state < 0x80000000)
        return;

    const uint32_t helmet_alpha_addr = game_state + k_game_options_helmet_alpha_offset;
    const uint32_t current = g_dolphin.read_u32(helmet_alpha_addr);
    if (current == 0)
        return;

    if (current <= 0xff) {
        g_dolphin.write_u32(helmet_alpha_addr, 0);
        static uint32_t last_logged_game_state = 0;
        if (last_logged_game_state != game_state) {
            last_logged_game_state = game_state;
            app_hook_log(L"PrimedGun set Helmet Opacity to 0 for game state " + hex32(game_state) + L".");
        }
    }
}
    


static bool open_live_shared_state() {
    if (g_shared_state)
        return true;

    g_shared_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                          sizeof(PrimedGun::SharedState),
                                          PrimedGun::SharedMemoryName);
    if (!g_shared_mapping)
        return false;

    g_shared_state = static_cast<PrimedGun::SharedState*>(
        MapViewOfFile(g_shared_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(PrimedGun::SharedState)));
    if (!g_shared_state) {
        CloseHandle(g_shared_mapping);
        g_shared_mapping = nullptr;
        return false;
    }

    if (g_shared_state->magic != PrimedGun::SharedStateMagic ||
        g_shared_state->version != PrimedGun::SharedStateVersion) {
        *g_shared_state = PrimedGun::SharedState{};
    }
    return true;
}

static void close_live_shared_state() {
    if (g_shared_state) {
        UnmapViewOfFile(g_shared_state);
        g_shared_state = nullptr;
    }
    if (g_shared_mapping) {
        CloseHandle(g_shared_mapping);
        g_shared_mapping = nullptr;
    }
}


static Pose pose_from_shared(const PrimedGun::PoseState& in) {
    Pose out{};
    out.px = in.positionMeters.x;
    out.py = in.positionMeters.y;
    out.pz = in.positionMeters.z;
    out.qx = in.orientation.x;
    out.qy = in.orientation.y;
    out.qz = in.orientation.z;
    out.qw = in.orientation.w;
    out.trigger_click = (in.buttons & kButtonTriggerClick) != 0 || in.angularVelocityRadiansPerSecond.x > 0.5f;
    out.thumbstick_click = (in.buttons & kButtonThumbstickClick) != 0 || in.angularVelocityRadiansPerSecond.y > 0.5f;
    out.button_a = (in.buttons & kButtonA) != 0;
    out.button_b = (in.buttons & kButtonB) != 0;
    out.valid = true;
    return out;
}

static float normalized_view_height_meters(float height) {
    if (!std::isfinite(height))
        return kDefaultViewHeightMeters;
    return std::clamp(height, 0.4f, 2.4f);
}


static float current_view_height_meters() {
    g_settings.view_height_meters = normalized_view_height_meters(g_settings.view_height_meters);
    return g_settings.view_height_meters;
}


static float current_view_height_prime_z() {
    return 0.0f;
}


static void publish_view_height_to_shared() {
    if (!g_shared_state)
        return;
    g_shared_state->settings.viewHeightHomeMeters = current_view_height_meters();
    g_shared_state->settings.viewHeightGeneration = g_view_height_generation.load(std::memory_order_relaxed);
}


static void calibrate_view_height_from_hmd(float hmd_height_meters) {
    const float height = normalized_view_height_meters(hmd_height_meters);
    g_view_height_generation.fetch_add(1, std::memory_order_relaxed);
    g_settings.view_height_meters = height;
    g_settings.save();
    publish_view_height_to_shared();
    app_hook_log(L"PrimedGun height calibration set Dolphin HMD home height to " +
                 std::to_wstring(height) + L" m.");
}


static bool get_dolphinxr_poses(bool right_hand, Pose& controller, Pose& left_controller, Pose& hmd) {
    controller = {};
    left_controller = {};
    hmd = {};

    if (!open_live_shared_state() || !g_shared_state)
        return false;

    const uint64_t before = g_shared_state->trackingGeneration;
    if (g_shared_state->trackingSource != 1 || !g_shared_state->trackingRuntimeActive || before == 0)
        return false;

    const PrimedGun::PoseState shared_left = g_shared_state->leftHandPose;
    const PrimedGun::PoseState shared_right = g_shared_state->rightHandPose;
    const PrimedGun::PoseState shared_hmd = g_shared_state->hmdPose;
    const uint64_t after = g_shared_state->trackingGeneration;
    if (before != after)
        return false;

    Pose left = pose_from_shared(shared_left);
    left.trigger = shared_left.linearVelocityMetersPerSecond.x;
    left.stick_x = shared_left.linearVelocityMetersPerSecond.y;
    left.stick_y = shared_left.linearVelocityMetersPerSecond.z;
    left.axis_x[0] = left.stick_x;
    left.axis_y[0] = left.stick_y;
    left.stick_axis = 0;

    Pose right = pose_from_shared(shared_right);
    right.trigger = shared_right.linearVelocityMetersPerSecond.x;
    right.stick_x = shared_right.linearVelocityMetersPerSecond.y;
    right.stick_y = shared_right.linearVelocityMetersPerSecond.z;
    right.axis_x[0] = right.stick_x;
    right.axis_y[0] = right.stick_y;
    right.stick_axis = 0;

    controller = right_hand ? right : left;
    left_controller = right_hand ? left : right;
    hmd = pose_from_shared(shared_hmd);
    return controller.valid || left_controller.valid || hmd.valid;
}

static bool get_dolphinxr_hmd_pose(Pose& hmd) {
    hmd = {};

    if (!open_live_shared_state() || !g_shared_state)
        return false;

    const uint64_t before = g_shared_state->trackingGeneration;
    if (g_shared_state->trackingSource != 2 || !g_shared_state->trackingRuntimeActive || before == 0)
        return false;

    const PrimedGun::PoseState shared_hmd = g_shared_state->hmdPose;
    const uint64_t after = g_shared_state->trackingGeneration;
    if (before != after)
        return false;

    hmd = pose_from_shared(shared_hmd);
    return hmd.valid;
}

static void rotate_pose_vector(const Pose& pose, float x, float y, float z, float& ox, float& oy, float& oz) {
    const float tx = 2.0f * (pose.qy * z - pose.qz * y);
    const float ty = 2.0f * (pose.qz * x - pose.qx * z);
    const float tz = 2.0f * (pose.qx * y - pose.qy * x);
    ox = x + pose.qw * tx + (pose.qy * tz - pose.qz * ty);
    oy = y + pose.qw * ty + (pose.qz * tx - pose.qx * tz);
    oz = z + pose.qw * tz + (pose.qx * ty - pose.qy * tx);
}

static bool vr_settings_pointer_hit(const Pose& left, const Pose& right, float& x_out, float& y_out) {
    if (!left.valid || !right.valid)
        return false;

    Pose panel = left;
    panel.qx = left.qx * 0.70710678f - left.qw * 0.70710678f;
    panel.qy = left.qy * 0.70710678f - left.qz * 0.70710678f;
    panel.qz = left.qz * 0.70710678f + left.qy * 0.70710678f;
    panel.qw = left.qw * 0.70710678f + left.qx * 0.70710678f;

    float ox, oy, oz;
    rotate_pose_vector(panel, 0.0f, 0.10f, -0.18f, ox, oy, oz);
    const float cx = left.px + ox;
    const float cy = left.py + oy;
    const float cz = left.pz + oz;

    float rx, ry, rz, ux, uy, uz, nx, ny, nz, dx, dy, dz;
    rotate_pose_vector(panel, 1.0f, 0.0f, 0.0f, rx, ry, rz);
    rotate_pose_vector(panel, 0.0f, 1.0f, 0.0f, ux, uy, uz);
    rotate_pose_vector(panel, 0.0f, 0.0f, 1.0f, nx, ny, nz);
    rotate_pose_vector(right, 0.0f, 0.0f, -1.0f, dx, dy, dz);

    const float denom = dx * nx + dy * ny + dz * nz;
    if (std::fabs(denom) < 0.001f)
        return false;

    const float vx = cx - right.px;
    const float vy = cy - right.py;
    const float vz = cz - right.pz;
    const float t = (vx * nx + vy * ny + vz * nz) / denom;
    if (t < 0.02f || t > 3.0f)
        return false;

    const float hx = right.px + dx * t - cx;
    const float hy = right.py + dy * t - cy;
    const float hz = right.pz + dz * t - cz;
    x_out = 0.5f + (hx * rx + hy * ry + hz * rz) / 1.05f;
    y_out = 0.5f - (hx * ux + hy * uy + hz * uz) / 0.72f;
    return x_out >= 0.0f && x_out <= 1.0f && y_out >= 0.0f && y_out <= 1.0f;
}

static constexpr uint32_t k_vr_settings_item_count = 31;

static std::chrono::steady_clock::time_point g_vr_settings_saved_notice_until{};

static void change_vr_setting(uint32_t index, bool increase) {
    const float sign = increase ? 1.0f : -1.0f;
    switch (index) {
    case 0:
        g_settings.save();
        g_vr_settings_saved_notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        app_hook_log(L"VR settings saved from in-headset menu.");
        return;
    case 1: g_settings.reset_all(); break;
    case 2: g_settings.use_right_hand = !g_settings.use_right_hand; break;
    case 3: g_settings.require_trigger = !g_settings.require_trigger; break;
    case 4: g_settings.trigger_threshold = std::clamp(g_settings.trigger_threshold + sign * 0.05f, 0.0f, 1.0f); break;
    case 5: g_settings.world_scale = std::clamp(g_settings.world_scale + sign * 0.5f, 1.0f, 50.0f); break;
    case 6: g_settings.offset_x = std::clamp(g_settings.offset_x + sign * 0.01f, -2.0f, 2.0f); break;
    case 7: g_settings.offset_y = std::clamp(g_settings.offset_y + sign * 0.01f, -2.0f, 2.0f); break;
    case 8: g_settings.offset_z = std::clamp(g_settings.offset_z + sign * 0.01f, -2.0f, 2.0f); break;
    case 9:
        g_settings.offset_x = kDefaultOffsetX;
        g_settings.offset_y = kDefaultOffsetY;
        g_settings.offset_z = kDefaultOffsetZ;
        break;
    case 10: g_settings.rot_offset_x = std::clamp(g_settings.rot_offset_x + sign * 0.5f, -180.0f, 180.0f); break;
    case 11: g_settings.rot_offset_y = std::clamp(g_settings.rot_offset_y + sign * 0.5f, -180.0f, 180.0f); break;
    case 12: g_settings.rot_offset_z = std::clamp(g_settings.rot_offset_z + sign * 0.5f, -180.0f, 180.0f); break;
    case 13:
        g_settings.rot_offset_x = kDefaultRotOffsetX;
        g_settings.rot_offset_y = kDefaultRotOffsetY;
        g_settings.rot_offset_z = kDefaultRotOffsetZ;
        break;
    case 14: g_settings.gun_targeting_enabled = !g_settings.gun_targeting_enabled; break;
    case 15: g_settings.gun_targeting_distance = std::clamp(g_settings.gun_targeting_distance + sign * 1.0f, 10.0f, 120.0f); break;
    case 16: g_settings.gun_targeting_radius = std::clamp(g_settings.gun_targeting_radius + sign * 0.1f, 0.5f, 8.0f); break;
    case 17:
        g_settings.gun_targeting_enabled = kDefaultGunTargetingEnabled;
        g_settings.gun_targeting_distance = kDefaultGunTargetingDistance;
        g_settings.gun_targeting_radius = kDefaultGunTargetingRadius;
        break;
    case 18: g_settings.auto_dolphin_xr_controls = !g_settings.auto_dolphin_xr_controls; break;
    case 19: g_settings.xr_dpad_enabled = !g_settings.xr_dpad_enabled; break;
    case 20: g_settings.xr_dpad_head_radius = std::clamp(g_settings.xr_dpad_head_radius + sign * 0.01f, 0.08f, 0.28f); break;
    case 21: g_settings.xr_dpad_head_y_below = std::clamp(g_settings.xr_dpad_head_y_below + sign * 0.01f, 0.02f, 0.25f); break;
    case 22: g_settings.xr_dpad_deadzone = std::clamp(g_settings.xr_dpad_deadzone + sign * 0.01f, 0.20f, 0.80f); break;
    case 23:
        g_settings.xr_dpad_enabled = kDefaultXrDpadEnabled;
        g_settings.xr_dpad_head_radius = kDefaultXrDpadHeadRadius;
        g_settings.xr_dpad_head_y_below = kDefaultXrDpadHeadYBelow;
        g_settings.xr_dpad_deadzone = kDefaultXrDpadDeadzone;
        g_settings.xr_dpad_stick_axis = kDefaultXrDpadStickAxis;
        break;
    case 24: g_settings.directional_movement_enabled = !g_settings.directional_movement_enabled; break;
    case 25: g_settings.directional_movement_use_right_stick = !g_settings.directional_movement_use_right_stick; break;
    case 26: g_settings.directional_movement_deadzone = std::clamp(g_settings.directional_movement_deadzone + sign * 0.01f, 0.05f, 0.80f); break;
    case 27: g_settings.directional_movement_speed = std::clamp(g_settings.directional_movement_speed + sign * 0.25f, 4.0f, 30.0f); break;
    case 28: g_settings.directional_movement_accel = std::clamp(g_settings.directional_movement_accel + sign * 1.0f, 5.0f, 120.0f); break;
    case 29: g_settings.directional_movement_air_accel = std::clamp(g_settings.directional_movement_air_accel + sign * 0.5f, 0.0f, 60.0f); break;
    case 30:
        g_settings.directional_movement_enabled = kDefaultDirectionalMovementEnabled;
        g_settings.directional_movement_use_right_stick = kDefaultDirectionalMovementUseRightStick;
        g_settings.directional_movement_deadzone = kDefaultDirectionalMovementDeadzone;
        g_settings.directional_movement_speed = kDefaultDirectionalMovementSpeed;
        g_settings.directional_movement_accel = kDefaultDirectionalMovementAccel;
        g_settings.directional_movement_air_accel = kDefaultDirectionalMovementAirAccel;
        break;
    default: break;
    }
    g_settings.save();
}

static void sync_vr_settings_to_shared(uint32_t generation, bool visible, uint32_t selected, bool pointerActive, float pointerX, float pointerY) {
    if (!g_shared_state)
        return;
    auto& s = g_shared_state->settings;
    s.vrMenuVisible = visible ? 1u : 0u;
    s.vrMenuGeneration = generation;
    s.vrMenuSelectedIndex = selected;
    s.vrMenuItemCount = k_vr_settings_item_count;
    s.vrMenuPointerActive = pointerActive ? 1u : 0u;
    s.vrMenuSavedNotice = std::chrono::steady_clock::now() < g_vr_settings_saved_notice_until ? 1u : 0u;
    s.vrMenuPointerX = pointerX;
    s.vrMenuPointerY = pointerY;
    s.useRightHand = g_settings.use_right_hand ? 1u : 0u;
    s.requireTrigger = g_settings.require_trigger ? 1u : 0u;
    s.triggerThreshold = g_settings.trigger_threshold;
    s.worldScale = g_settings.world_scale;
    s.offsetX = g_settings.offset_x;
    s.offsetY = g_settings.offset_y;
    s.offsetZ = g_settings.offset_z;
    s.rotOffsetX = g_settings.rot_offset_x;
    s.rotOffsetY = g_settings.rot_offset_y;
    s.rotOffsetZ = g_settings.rot_offset_z;
    s.gunTargetingEnabled = g_settings.gun_targeting_enabled ? 1u : 0u;
    s.gunTargetingDistance = g_settings.gun_targeting_distance;
    s.gunTargetingRadius = g_settings.gun_targeting_radius;
    s.autoDolphinXrControls = g_settings.auto_dolphin_xr_controls ? 1u : 0u;
    s.xrDpadEnabled = g_settings.xr_dpad_enabled ? 1u : 0u;
    s.xrDpadHeadRadius = g_settings.xr_dpad_head_radius;
    s.xrDpadHeadYBelow = g_settings.xr_dpad_head_y_below;
    s.xrDpadDeadzone = g_settings.xr_dpad_deadzone;
    s.directionalMovementEnabled = g_settings.directional_movement_enabled ? 1u : 0u;
    s.directionalMovementUseRightStick = g_settings.directional_movement_use_right_stick ? 1u : 0u;
    s.directionalMovementDeadzone = g_settings.directional_movement_deadzone;
    s.directionalMovementSpeed = g_settings.directional_movement_speed;
    s.directionalMovementAccel = g_settings.directional_movement_accel;
    s.directionalMovementAirAccel = g_settings.directional_movement_air_accel;
    s.viewHeightHomeMeters = current_view_height_meters();
    s.viewHeightGeneration = g_view_height_generation.load(std::memory_order_relaxed);
}


static bool ensure_dolphin_hook_loaded() {
    ensure_shared_state();

    const fs::path dll_path = exe_directory() / L"PrimedGun_DolphinHook.dll";
    if (!fs::exists(dll_path)) {
        app_hook_log(L"Missing hook DLL next to PrimedGun.exe: " + dll_path.wstring());
        return false;
    }

    std::optional<DWORD> dolphin_pid = find_process_id_by_name(L"Dolphin.exe");
    if (dolphin_pid) {
        app_hook_log(L"PrimedGun.exe found running Dolphin.exe with pid " + std::to_wstring(*dolphin_pid));
        return inject_hook_dll(*dolphin_pid, dll_path);
    }

    app_hook_log(L"Dolphin.exe is not running; waiting for the user to start Dolphin.");
    return false;
}

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
static void apply_tracking_world_yaw(Pose& pose, float yaw_deg);

static void update_game_revision_detection() {
    if (!g_dolphin.is_connected()) {
        g_app.game_rev0_ok = false;
        g_app.game_status = "Load game: not ready, try reconnect";
        return;
    }

    const uint32_t id0 = g_dolphin.read_u32(0x80000000);
    const uint16_t id1 = g_dolphin.read_u16(0x80000004);
    const uint8_t revision = g_dolphin.read_u8(0x80000007);
    const bool is_gm8e01 = id0 == 0x474D3845u && id1 == 0x3031u; // "GM8E01"
    g_app.game_rev0_ok = is_gm8e01 && revision == 0;

    if (g_app.game_rev0_ok) {
        g_app.game_status = "Load game: Rev 0 OK";
    } else if (is_gm8e01) {
        g_app.game_status = "Load game: wrong game revision";
    } else {
        g_app.game_status = "Load game: wrong game";
    }
}


static bool prime_game_is_loaded_in_ram() {
    if (!g_dolphin.is_connected())
        return false;

    const uint32_t id0 = g_dolphin.read_u32(0x80000000);
    const uint16_t id1 = g_dolphin.read_u16(0x80000004);
    const uint8_t revision = g_dolphin.read_u8(0x80000007);
    return id0 == 0x474D3845u && id1 == 0x3031u && revision == 0; // "GM8E01", rev 0
}

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
    if (dir == XrDpadNone) {
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

static void flatten_active_camera_pitch(uint32_t camera_xf_addr) {
    if (!looks_like_transform_matrix(camera_xf_addr))
        return;

    const float m00 = g_dolphin.read_float(camera_xf_addr + 0x00);
    const float m01 = g_dolphin.read_float(camera_xf_addr + 0x04);
    const float m10 = g_dolphin.read_float(camera_xf_addr + 0x10);
    const float m11 = g_dolphin.read_float(camera_xf_addr + 0x14);
    const float len0 = std::sqrt(m00 * m00 + m01 * m01);
    if (!std::isfinite(len0) || len0 < 0.001f)
        return;

    const float det = (m00 * m11) - (m01 * m10);
    const float handedness = det < 0.0f ? -1.0f : 1.0f;
    const float right_x = m00 / len0;
    const float right_y = m01 / len0;
    const float forward_x = -right_y * handedness;
    const float forward_y = right_x * handedness;

    g_dolphin.write_float(camera_xf_addr + 0x00, right_x);
    g_dolphin.write_float(camera_xf_addr + 0x04, right_y);
    g_dolphin.write_float(camera_xf_addr + 0x08, 0.0f);
    g_dolphin.write_float(camera_xf_addr + 0x10, forward_x);
    g_dolphin.write_float(camera_xf_addr + 0x14, forward_y);
    g_dolphin.write_float(camera_xf_addr + 0x18, 0.0f);
    g_dolphin.write_float(camera_xf_addr + 0x20, 0.0f);
    g_dolphin.write_float(camera_xf_addr + 0x24, 0.0f);
    g_dolphin.write_float(camera_xf_addr + 0x28, 1.0f);
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

static bool player_is_prompt_gameplay_ready(uint32_t player) {
    if (!player_is_first_person_gun_ready(player))
        return false;

    const uint8_t input_flags = g_dolphin.read_u8(player + k_player_disable_input_flags_offset);
    if ((input_flags & k_player_disable_input_mask) != 0)
        return false;

    constexpr uint32_t k_player_movement_state_offset = 0x258;
    const uint32_t movement_state = g_dolphin.read_u32(player + k_player_movement_state_offset);
    return movement_state <= 6;
}

static void mark_cannon_transform_not_ready() {
    g_cannon_transform_ready_frames.store(0, std::memory_order_relaxed);
    g_last_cannon_transform_player.store(0, std::memory_order_relaxed);
    g_last_cannon_transform_gun.store(0, std::memory_order_relaxed);
}

static void mark_cannon_transform_ready(uint32_t player, uint32_t gun_ptr) {
    const uint32_t previous_player = g_last_cannon_transform_player.load(std::memory_order_relaxed);
    const uint32_t previous_gun = g_last_cannon_transform_gun.load(std::memory_order_relaxed);
    uint32_t frames = 1;
    if (previous_player == player && previous_gun == gun_ptr) {
        frames = std::min<uint32_t>(g_cannon_transform_ready_frames.load(std::memory_order_relaxed) + 1, 1000);
    }

    g_last_cannon_transform_player.store(player, std::memory_order_relaxed);
    g_last_cannon_transform_gun.store(gun_ptr, std::memory_order_relaxed);
    g_cannon_transform_ready_frames.store(frames, std::memory_order_relaxed);
    g_last_cannon_transform_write_ms.store(GetTickCount64(), std::memory_order_relaxed);
}

static bool cannon_transform_is_gameplay_ready(uint32_t player) {
    if (player < 0x80000000)
        return false;

    const uint64_t last_ms = g_last_cannon_transform_write_ms.load(std::memory_order_relaxed);
    if (last_ms == 0 || GetTickCount64() - last_ms > 500)
        return false;

    if (g_last_cannon_transform_player.load(std::memory_order_relaxed) != player)
        return false;

    return g_cannon_transform_ready_frames.load(std::memory_order_relaxed) >= 30;
}

struct PromptGateSnapshot {
    uint32_t player = 0;
    uint32_t camera_state = 0xffffffff;
    uint32_t morph_state = 0xffffffff;
    uint32_t movement_state = 0xffffffff;
    uint32_t holster_state = 0xffffffff;
    uint32_t free_look_word = 0xffffffff;
    uint8_t input_flags = 0xff;
    uint8_t final_held0 = 0xff;
    uint8_t final_pressed0 = 0xff;
    float gun_alpha = 0.0f;
};

static PromptGateSnapshot read_prompt_gate_snapshot(uint32_t player) {
    PromptGateSnapshot s;
    s.player = player;
    if (player < 0x80000000)
        return s;

    constexpr uint32_t k_player_movement_state_offset = 0x258;
    s.camera_state = g_dolphin.read_u32(player + 0x2f4);
    s.morph_state = g_dolphin.read_u32(player + 0x2f8);
    s.movement_state = g_dolphin.read_u32(player + k_player_movement_state_offset);
    s.gun_alpha = g_dolphin.read_float(player + 0x494);
    s.holster_state = g_dolphin.read_u32(player + 0x498);
    s.input_flags = g_dolphin.read_u8(player + k_player_disable_input_flags_offset);
    s.free_look_word = g_dolphin.read_u32(player + k_player_free_look_state_offset);
    s.final_held0 = g_dolphin.read_u8(player + k_final_input_dpad_held_0);
    s.final_pressed0 = g_dolphin.read_u8(player + k_final_input_dpad_pressed_0);
    return s;
}

static void log_prompt_gate_snapshot(const PromptGateSnapshot& s,
                                     bool ready,
                                     bool cannon_ready,
                                     bool stable,
                                     bool show,
                                     std::chrono::steady_clock::time_point now) {
    static bool last_ready = false;
    static bool last_cannon_ready = false;
    static bool last_stable = false;
    static bool last_show = false;
    static uint32_t last_player = 0;
    static auto last_log = std::chrono::steady_clock::time_point{};

    const bool changed = ready != last_ready || cannon_ready != last_cannon_ready ||
                         stable != last_stable || show != last_show || s.player != last_player;
    const bool valid_player = s.player >= 0x80000000;
    const bool periodic =
        last_log.time_since_epoch().count() == 0 ||
        ((show || valid_player) &&
         std::chrono::duration_cast<std::chrono::milliseconds>(now - last_log).count() >= 2000);
    if (!changed && !periodic)
        return;

    std::wstringstream ss;
    ss << L"Prompt gate: ready=" << (ready ? 1 : 0)
       << L" cannon=" << (cannon_ready ? 1 : 0)
       << L" cannonFrames=" << g_cannon_transform_ready_frames.load(std::memory_order_relaxed)
       << L" stable=" << (stable ? 1 : 0)
       << L" show=" << (show ? 1 : 0)
       << L" player=" << hex32(s.player)
       << L" cam=" << s.camera_state
       << L" morph=" << s.morph_state
       << L" move=" << s.movement_state
       << L" holster=" << s.holster_state
       << L" alpha=" << s.gun_alpha
       << L" inputFlags=" << hex32(s.input_flags)
       << L" freeLook=" << hex32(s.free_look_word)
       << L" held0=" << hex32(s.final_held0)
       << L" pressed0=" << hex32(s.final_pressed0);
    app_hook_log(ss.str());

    last_ready = ready;
    last_cannon_ready = cannon_ready;
    last_stable = stable;
    last_show = show;
    last_player = s.player;
    last_log = now;
}

static void log_gameplay_candidate_profile(uint32_t state_mgr,
                                           uint32_t player,
                                           bool player_ready,
                                           bool cannon_ready,
                                           bool stable,
                                           bool show,
                                           std::chrono::steady_clock::time_point now) {
    static uint32_t last_player = 0;
    static auto profile_until = std::chrono::steady_clock::time_point{};
    static auto last_profile_log = std::chrono::steady_clock::time_point{};

    if (player != last_player) {
        last_player = player;
        last_profile_log = {};
        profile_until = player >= 0x80000000 ? now + std::chrono::seconds(45)
                                             : std::chrono::steady_clock::time_point{};
    }
    if (player < 0x80000000 || profile_until.time_since_epoch().count() == 0 || now > profile_until)
        return;
    if (last_profile_log.time_since_epoch().count() != 0 &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_profile_log).count() < 500)
        return;
    last_profile_log = now;

    const auto addrs = get_addresses();
    const uint32_t gun_ptr = g_dolphin.read_u32(player + addrs.cannon_offset);
    const uint32_t gun_xf = gun_ptr >= 0x80000000 ? gun_ptr + addrs.gun_xf_offset : 0;
    const uint32_t world_xf = gun_ptr >= 0x80000000 ? gun_ptr + addrs.world_xf_offset : 0;
    const bool gun_chain_valid = gun_ptr >= 0x80000000 &&
        looks_like_transform_matrix(gun_xf) && looks_like_transform_matrix(world_xf);

    float px = 0.0f, py = 0.0f, pz = 0.0f;
    float gx = 0.0f, gy = 0.0f, gz = 0.0f;
    read_transform_translation(player + 0x34, px, py, pz);
    if (world_xf >= 0x80000000)
        read_transform_translation(world_xf, gx, gy, gz);

    std::wstringstream ss;
    ss << L"Gameplay profile: player=" << hex32(player)
       << L" ready=" << (player_ready ? 1 : 0)
       << L" cannon=" << (cannon_ready ? 1 : 0)
       << L" stable=" << (stable ? 1 : 0)
       << L" show=" << (show ? 1 : 0)
       << L" cam=" << g_dolphin.read_u32(player + 0x2f4)
       << L" morph=" << g_dolphin.read_u32(player + 0x2f8)
       << L" move=" << g_dolphin.read_u32(player + 0x258)
       << L" scan=" << g_dolphin.read_u32(player + 0x330)
       << L" free=" << hex32(g_dolphin.read_u32(player + k_player_free_look_state_offset))
       << L" alpha=" << g_dolphin.read_float(player + 0x494)
       << L" holster=" << g_dolphin.read_u32(player + 0x498)
       << L" flags=" << hex32(g_dolphin.read_u8(player + k_player_disable_input_flags_offset))
       << L" p24C=" << hex32(g_dolphin.read_u32(player + 0x24c))
       << L" p250=" << hex32(g_dolphin.read_u32(player + 0x250))
       << L" p254=" << hex32(g_dolphin.read_u32(player + 0x254))
       << L" p25C=" << hex32(g_dolphin.read_u32(player + 0x25c))
       << L" p260=" << hex32(g_dolphin.read_u32(player + 0x260))
       << L" p300=" << hex32(g_dolphin.read_u32(player + 0x300))
       << L" p304=" << hex32(g_dolphin.read_u32(player + 0x304))
       << L" p308=" << hex32(g_dolphin.read_u32(player + 0x308))
       << L" p4A0=" << hex32(g_dolphin.read_u32(player + 0x4a0))
       << L" p4A4=" << hex32(g_dolphin.read_u32(player + 0x4a4))
       << L" p4A8=" << hex32(g_dolphin.read_u32(player + 0x4a8))
       << L" held0=" << hex32(g_dolphin.read_u8(player + k_final_input_dpad_held_0))
       << L" held1=" << hex32(g_dolphin.read_u8(player + k_final_input_dpad_held_1))
       << L" press0=" << hex32(g_dolphin.read_u8(player + k_final_input_dpad_pressed_0))
       << L" stateHeld0=" << hex32(state_mgr >= 0x80000000 ? g_dolphin.read_u8(state_mgr + k_final_input_dpad_held_0) : 0xff)
       << L" gun=" << hex32(gun_ptr)
       << L" gunChain=" << (gun_chain_valid ? 1 : 0)
       << L" playerPos=" << px << L"," << py << L"," << pz
       << L" gunPos=" << gx << L"," << gy << L"," << gz;
    app_hook_log(ss.str());
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
    apply_tracking_world_yaw(adjusted, yaw_delta_deg);

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

static void rotate_prime_xy(float& x, float& y, float yaw_deg) {
    const float yaw_rad = yaw_deg * (static_cast<float>(M_PI) / 180.0f);
    const float c = std::cos(yaw_rad);
    const float s = std::sin(yaw_rad);
    const float old_x = x;
    const float old_y = y;
    x = c * old_x - s * old_y;
    y = s * old_x + c * old_y;
}

static void player_relative_cannon_offset(float yaw_delta_deg, float& x, float& y, float& z) {
    x = g_settings.offset_x * g_settings.world_scale;
    y = -(g_settings.offset_z * g_settings.world_scale);
    z = g_settings.offset_y * g_settings.world_scale;
    rotate_prime_xy(x, y, yaw_delta_deg);
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


static void apply_tracking_world_yaw(Pose& pose, float yaw_deg) {
    const float yaw_rad = yaw_deg * (static_cast<float>(M_PI) / 180.0f);
    const float half = yaw_rad * 0.5f;
    const float sy = std::sinf(half);
    const float cy = std::cosf(half);

    // Turning the in-game facing direction should rotate the controller's
    // orientation frame around the tracking up axis so pitch/roll operate in the
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

static void normalize_quat(Pose& pose) {
    const float len_sq = pose.qx * pose.qx + pose.qy * pose.qy + pose.qz * pose.qz + pose.qw * pose.qw;
    if (!std::isfinite(len_sq) || len_sq <= 0.000001f) {
        pose.qx = 0.0f;
        pose.qy = 0.0f;
        pose.qz = 0.0f;
        pose.qw = 1.0f;
        return;
    }
    const float inv_len = 1.0f / std::sqrt(len_sq);
    pose.qx *= inv_len;
    pose.qy *= inv_len;
    pose.qz *= inv_len;
    pose.qw *= inv_len;
}

static Pose smooth_openxr_gun_pose(const Pose& raw) {
    constexpr float kPositionAlpha = 0.35f;
    constexpr float kRotationAlpha = 0.40f;
    constexpr float kRotationDeadbandDegrees = 0.15f;
    constexpr float kResetDistanceMeters = 0.50f;

    static bool have_smoothed = false;
    static Pose smoothed = {};

    Pose current = raw;
    normalize_quat(current);
    if (!current.valid) {
        have_smoothed = false;
        return raw;
    }

    if (!have_smoothed || !smoothed.valid) {
        smoothed = current;
        have_smoothed = true;
        return smoothed;
    }

    const float dx = current.px - smoothed.px;
    const float dy = current.py - smoothed.py;
    const float dz = current.pz - smoothed.pz;
    const float dist_sq = dx * dx + dy * dy + dz * dz;
    if (!std::isfinite(dist_sq) || dist_sq > kResetDistanceMeters * kResetDistanceMeters) {
        smoothed = current;
        return smoothed;
    }

    smoothed.px += dx * kPositionAlpha;
    smoothed.py += dy * kPositionAlpha;
    smoothed.pz += dz * kPositionAlpha;

    float qx = current.qx;
    float qy = current.qy;
    float qz = current.qz;
    float qw = current.qw;
    float dot = smoothed.qx * qx + smoothed.qy * qy + smoothed.qz * qz + smoothed.qw * qw;
    if (dot < 0.0f) {
        dot = -dot;
        qx = -qx;
        qy = -qy;
        qz = -qz;
        qw = -qw;
    }

    const float clamped_dot = std::clamp(dot, -1.0f, 1.0f);
    const float angle_deg = 2.0f * std::acos(clamped_dot) * (180.0f / static_cast<float>(M_PI));
    if (std::isfinite(angle_deg) && angle_deg >= kRotationDeadbandDegrees) {
        smoothed.qx += (qx - smoothed.qx) * kRotationAlpha;
        smoothed.qy += (qy - smoothed.qy) * kRotationAlpha;
        smoothed.qz += (qz - smoothed.qz) * kRotationAlpha;
        smoothed.qw += (qw - smoothed.qw) * kRotationAlpha;
        normalize_quat(smoothed);
    }

    smoothed.trigger = current.trigger;
    smoothed.stick_x = current.stick_x;
    smoothed.stick_y = current.stick_y;
    smoothed.stick_axis = current.stick_axis;
    smoothed.axis_x = current.axis_x;
    smoothed.axis_y = current.axis_y;
    smoothed.valid = current.valid;
    return smoothed;
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

    g_dolphin.write_float(state_mgr + k_final_input_right_stick_y, 0.0f);
    g_dolphin.write_u8(state_mgr + k_final_input_right_stick_y_press, 0);
    g_dolphin.write_u8(player + k_player_free_look_state_offset, 0);
    g_dolphin.write_u8(player + k_player_free_look_state_offset + 1, 0);
    g_dolphin.write_u8(player + k_player_free_look_state_offset + 2, 0);
    g_dolphin.write_float(player + k_player_free_look_center_time_offset, 0.0f);
    g_dolphin.write_float(player + k_player_free_look_pitch_angle_offset, 0.0f);
    g_dolphin.write_float(player + k_player_free_look_pitch_vel_offset, 0.0f);
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
                               std::chrono::duration_cast<std::chrono::milliseconds>(now - s_last_pick_time).count() < 66;

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
        mark_cannon_transform_not_ready();
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
        mark_cannon_transform_not_ready();
        reset_lock_camera_pitch_suppression();
        disarm_memory_writes();
        return;
    }

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

        static uint32_t s_last_validated_gun_ptr = 0;
        const bool gun_chain_valid =
            s_last_validated_gun_ptr == gun_ptr ||
            (looks_like_transform_matrix(gun_xf) &&
             looks_like_transform_matrix(beam_xf) &&
             looks_like_transform_matrix(world_xf) &&
             looks_like_transform_matrix(local_xf));
        if (!gun_chain_valid) {
            s_last_validated_gun_ptr = 0;
            mark_cannon_transform_not_ready();
            disarm_memory_writes();
            return;
        }
        s_last_validated_gun_ptr = gun_ptr;
        mark_cannon_transform_ready(player, gun_ptr);
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
        mark_cannon_transform_not_ready();
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

        const auto poll_start = clock::now();
        Pose pose = {};
        Pose left_pose = {};
        Pose hmd_pose = {};
        bool got_pose = get_dolphinxr_poses(g_settings.use_right_hand, pose, left_pose, hmd_pose);
        if (got_pose) {
            g_app.tracking_ok = true;
            g_app.tracking_status = "DolphinXR OpenXR";
        } else {
            g_app.tracking_ok = false;
            g_app.tracking_status = "Waiting for DolphinXR OpenXR";
        }
        const auto poll_end = clock::now();
        g_app.tracker_poll_ms = std::chrono::duration<float, std::milli>(poll_end - poll_start).count();

        if (got_pose) {
            std::lock_guard<std::mutex> lock(g_pose_mutex);
            g_latest_pose = pose;
            g_latest_left_pose = left_pose;
            g_latest_hmd_pose = hmd_pose;
            g_app.last_pose = pose;
        }

        push_timing_sample(g_app.tracker_hist_ms, g_app.tracker_hist_head, g_app.tracker_dt_ms);

        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
}


static void dpad_thread() {
    constexpr auto k_tick = std::chrono::milliseconds(4);
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
            g_translation_base_valid = false;
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

            Pose adjusted_pose = smooth_openxr_gun_pose(pose);
            apply_tracking_world_yaw(adjusted_pose, player_yaw_delta_deg);
            Matrix3x4 controller_mat_no_offset = pose_to_prime_matrix(
                adjusted_pose,
                0.0f, 0.0f, 0.0f,
                g_settings.rot_offset_x, g_settings.rot_offset_y, g_settings.rot_offset_z,
                g_settings.world_scale
            );
            const Pose& right_controller = g_settings.use_right_hand ? pose : left_pose;
            static bool last_right_thumbstick_click = false;
            const bool right_thumbstick_click =
                g_app.active && g_app.dolphin_ok && right_controller.valid && hmd_pose.valid &&
                right_controller.thumbstick_click;
            if (right_thumbstick_click && !last_right_thumbstick_click) {
                calibrate_view_height_from_hmd(hmd_pose.py);
                g_controller_base_prime_z = hmd_pose.py * g_settings.world_scale;
                g_translation_base_valid = true;
                app_hook_log(L"PrimedGun height-only cannon baseline reset.");
            }
            last_right_thumbstick_click = right_thumbstick_click;

            if (g_app.recenter_requested.exchange(false, std::memory_order_relaxed) ||
                !g_translation_base_valid) {
                g_camera_base_prime_x = 0.0f;
                g_camera_base_prime_y = 0.0f;
                g_camera_base_prime_z = 0.0f;
                g_controller_base_prime_x = 0.0f;
                g_controller_base_prime_y = 0.0f;
                g_controller_base_prime_z = current_view_height_prime_z();
                g_translation_base_valid = true;
                publish_view_height_to_shared();
            }

            Matrix3x4 mat = pose_to_prime_matrix(
                adjusted_pose,
                0.0f, 0.0f, 0.0f,
                g_settings.rot_offset_x, g_settings.rot_offset_y, g_settings.rot_offset_z,
                g_settings.world_scale
            );
            if (g_translation_base_valid) {
                float offset_x = 0.0f;
                float offset_y = 0.0f;
                float offset_z = 0.0f;
                player_relative_cannon_offset(player_yaw_delta_deg, offset_x, offset_y, offset_z);

                float controller_delta_x = controller_mat_no_offset.at(0, 3) - g_controller_base_prime_x;
                float controller_delta_y = controller_mat_no_offset.at(1, 3) - g_controller_base_prime_y;
                const float controller_delta_z = controller_mat_no_offset.at(2, 3) - g_controller_base_prime_z;

                mat.at(0, 3) = g_camera_base_prime_x + controller_delta_x + offset_x;
                mat.at(1, 3) = g_camera_base_prime_y + controller_delta_y + offset_y;
                mat.at(2, 3) = g_camera_base_prime_z + controller_delta_z + offset_z;
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
    if (msg == WM_CLOSE) {
        restore_dolphin_borrowed_controls();
    }
    if (msg == WM_DESTROY) {
        restore_dolphin_borrowed_controls();
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
    open_app_hook_log();
    apply_dolphin_vr_units_per_meter();
    apply_dolphin_60fps_cap();
    apply_primedgun_vr_shader_profile();
    apply_dolphin_xr_camera_forward_zero();
    disable_unmanaged_dolphin_codes();
    sync_dolphin_xr_gamecube_controls(g_settings.auto_dolphin_xr_controls);
    if (find_process_id_by_name(L"Dolphin.exe")) {
        MessageBoxW(nullptr,
            L"Dolphin is already running.\n\n"
            L"For the cleanest startup, close Dolphin, start PrimedGun, then start Dolphin again.",
            L"Restart Dolphin",
            MB_OK | MB_ICONWARNING);
    }
    ensure_dolphin_hook_loaded();

    HICON app_icon = static_cast<HICON>(LoadImageW(
        hInstance, MAKEINTRESOURCEW(IDI_PRIMEDGUN), IMAGE_ICON,
        32, 32, LR_DEFAULTCOLOR));
    HICON app_icon_small = static_cast<HICON>(LoadImageW(
        hInstance, MAKEINTRESOURCEW(IDI_PRIMEDGUN), IMAGE_ICON,
        16, 16, LR_DEFAULTCOLOR));

    WNDCLASSEXW wc = {
        sizeof(wc), CS_CLASSDC, WndProc, 0, 0, hInstance,
        app_icon, nullptr, nullptr, nullptr, L"PrimedGun", app_icon_small
    };
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowW(
        L"PrimedGun", L"PrimedGun",
        WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
        100, 100, 516, 688, nullptr, nullptr, hInstance, nullptr);

    SendMessageW(g_hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(app_icon));
    SendMessageW(g_hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(app_icon_small));

    if (!init_d3d(g_hwnd)) {
        MessageBoxW(g_hwnd, L"Failed to init D3D11", L"Error", MB_OK);
        restore_dolphin_borrowed_controls();
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
    update_game_revision_detection();
    g_app.tracking_ok = false;
    g_app.tracking_status = "Waiting for DolphinXR OpenXR";
    app_hook_log(L"Tracking source is Dolphin-side OpenXR only.");

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
            apply_primedgun_vr_shader_profile();
            ensure_dolphin_hook_loaded();
            g_app.dolphin_ok = g_dolphin.connect();
            g_app.dolphin_status = g_dolphin.status();
            update_game_revision_detection();
        }

        if (g_app.remap_dolphin_controls_requested.exchange(false, std::memory_order_relaxed) ||
            g_settings.auto_dolphin_xr_controls != g_last_auto_dolphin_xr_controls) {
            sync_dolphin_xr_gamecube_controls(g_settings.auto_dolphin_xr_controls);
            g_settings.save();
        }
        if (g_app.dolphin_performance_apply_requested.exchange(false, std::memory_order_relaxed)) {
            apply_dolphin_60fps_cap();
            g_settings.save();
        }
        if (g_app.shader_profile_apply_requested.exchange(false, std::memory_order_relaxed)) {
            apply_primedgun_vr_shader_profile();
        }

        static auto last_dolphin_auto_connect = std::chrono::steady_clock::time_point{};
        static DWORD last_auto_hook_pid = 0;
        const auto now = std::chrono::steady_clock::now();
        if (!g_app.dolphin_ok &&
            (last_dolphin_auto_connect.time_since_epoch().count() == 0 ||
             std::chrono::duration_cast<std::chrono::milliseconds>(now - last_dolphin_auto_connect).count() >= 1000)) {
            last_dolphin_auto_connect = now;
            const std::optional<DWORD> dolphin_pid = find_process_id_by_name(L"Dolphin.exe");
            if (dolphin_pid && *dolphin_pid != last_auto_hook_pid) {
                apply_primedgun_vr_shader_profile();
                ensure_dolphin_hook_loaded();
                last_auto_hook_pid = *dolphin_pid;
            } else if (!dolphin_pid) {
                last_auto_hook_pid = 0;
            }
            g_app.dolphin_ok = g_dolphin.connect();
            g_app.dolphin_status = g_dolphin.status();
            update_game_revision_detection();
        }

        if (g_app.reconnect_tracking_requested.exchange(false, std::memory_order_relaxed)) {
            g_app.active = false;
            g_app.tracking_ok = false;
            g_app.tracking_status = "Waiting for DolphinXR OpenXR";
            ensure_dolphin_hook_loaded();
            app_hook_log(L"Tracking reconnect requested; Dolphin-side OpenXR remains the only tracking source.");
        }

        g_app.dolphin_status = g_dolphin.status();
        const bool has_shared_state = open_live_shared_state() && g_shared_state;
        if (has_shared_state) {
            g_shared_state->appHeartbeat++;
            static bool vr_settings_visible = false;
            static bool last_left_thumb_click = false;
            static bool last_right_select = false;
            static uint32_t vr_settings_selected = 0;
            static uint32_t vr_settings_generation = 1;
            static auto last_vr_menu_toggle_time = std::chrono::steady_clock::time_point{};
            Pose vr_left_pose = {};
            Pose vr_right_pose = {};
            {
                std::lock_guard<std::mutex> lock(g_pose_mutex);
                vr_left_pose = g_latest_left_pose;
                vr_right_pose = g_settings.use_right_hand ? g_latest_pose : g_latest_left_pose;
            }
            if (g_shared_state->trackingRuntimeActive) {
                vr_left_pose = pose_from_shared(g_shared_state->leftHandPose);
                vr_left_pose.trigger = g_shared_state->leftHandPose.linearVelocityMetersPerSecond.x;
                vr_left_pose.stick_x = g_shared_state->leftHandPose.linearVelocityMetersPerSecond.y;
                vr_left_pose.stick_y = g_shared_state->leftHandPose.linearVelocityMetersPerSecond.z;
                vr_right_pose = pose_from_shared(g_shared_state->rightHandPose);
                vr_right_pose.trigger = g_shared_state->rightHandPose.linearVelocityMetersPerSecond.x;
                vr_right_pose.stick_x = g_shared_state->rightHandPose.linearVelocityMetersPerSecond.y;
                vr_right_pose.stick_y = g_shared_state->rightHandPose.linearVelocityMetersPerSecond.z;
            }
            const bool left_thumb_click = vr_left_pose.valid && vr_left_pose.thumbstick_click;
            static bool last_logged_left_thumb_click = false;
            if (left_thumb_click != last_logged_left_thumb_click) {
                app_hook_log(std::wstring(L"VR settings left thumbstick click ") +
                    (left_thumb_click ? L"pressed." : L"released."));
                last_logged_left_thumb_click = left_thumb_click;
            }
            const bool toggle_debounced =
                last_vr_menu_toggle_time.time_since_epoch().count() == 0 ||
                std::chrono::duration_cast<std::chrono::milliseconds>(now - last_vr_menu_toggle_time).count() >= 450;
            if (left_thumb_click && !last_left_thumb_click && toggle_debounced) {
                last_vr_menu_toggle_time = now;
                vr_settings_visible = !vr_settings_visible;
                ++vr_settings_generation;
                app_hook_log(std::wstring(L"VR settings menu ") + (vr_settings_visible ? L"opened." : L"closed."));
            }
            last_left_thumb_click = left_thumb_click;

            float menu_pointer_x = 0.5f;
            float menu_pointer_y = 0.5f;
            const bool menu_pointer_active = vr_settings_visible &&
                vr_settings_pointer_hit(vr_left_pose, vr_right_pose, menu_pointer_x, menu_pointer_y);
            if (menu_pointer_active) {
                constexpr float firstY = 86.0f / 512.0f;
                constexpr float stepY = 25.0f / 512.0f;
                constexpr int rowsPerColumn = 16;
                const int column = menu_pointer_x >= 0.5f ? 1 : 0;
                int row = static_cast<int>((menu_pointer_y - firstY + stepY * 0.5f) / stepY);
                row = std::clamp(row, 0, rowsPerColumn - 1);
                int hovered = column * rowsPerColumn + row;
                hovered = std::clamp(hovered, 0, static_cast<int>(k_vr_settings_item_count) - 1);
                if (vr_settings_selected != static_cast<uint32_t>(hovered)) {
                    vr_settings_selected = static_cast<uint32_t>(hovered);
                    ++vr_settings_generation;
                }
            }
            
            const bool right_select = vr_right_pose.valid &&
                (vr_right_pose.button_a || vr_right_pose.trigger_click);
            if (vr_settings_visible && menu_pointer_active && right_select && !last_right_select) {
                constexpr float textureWidth = 1024.0f;
                constexpr float leftX = 28.0f;
                constexpr float rightX = 524.0f;
                constexpr float rowWidth = 472.0f;
                constexpr float valueWidth = 126.0f;
                constexpr float valueBoxOffset = rowWidth - valueWidth - 10.0f;
                const float pointerX = std::clamp(menu_pointer_x, 0.0f, 1.0f) * textureWidth;
                const float columnX = vr_settings_selected >= 16 ? rightX : leftX;
                const float valueBoxX = columnX + valueBoxOffset;
                if (pointerX >= valueBoxX && pointerX <= valueBoxX + valueWidth) {
                    change_vr_setting(vr_settings_selected, pointerX >= valueBoxX + valueWidth * 0.5f);
                    ++vr_settings_generation;
                }
            }
            
            last_right_select = right_select;
            sync_vr_settings_to_shared(vr_settings_generation, vr_settings_visible, vr_settings_selected,
                                       menu_pointer_active, menu_pointer_x, menu_pointer_y);

            static auto gameplay_ready_since = std::chrono::steady_clock::time_point{};
            static auto first_prompt_ready_timeout_until = std::chrono::steady_clock::time_point{};
            static auto alignment_prompt_show_until = std::chrono::steady_clock::time_point{};
            static auto game_session_inactive_since = std::chrono::steady_clock::time_point{};
            static bool skipped_first_prompt_ready = false;
            static bool waiting_for_second_prompt_ready = false;
            static bool alignment_prompt_shown_this_launch = false;
            static bool last_prompt_right_thumb_click = false;
            static bool game_loaded_session_active = false;
            const auto addrs = get_addresses();
            const uint32_t state_mgr = addrs.state_manager;
            uint32_t player = 0;
            if (g_app.active && g_app.dolphin_ok && state_mgr >= 0x80000000) {
                player = g_dolphin.read_u32(state_mgr + addrs.player_offset);
            }
            const bool game_loaded_in_ram = prime_game_is_loaded_in_ram();
            if (game_loaded_in_ram) {
                game_session_inactive_since = {};
                if (!game_loaded_session_active) {
                    game_loaded_session_active = true;
                    alignment_prompt_shown_this_launch = false;
                    skipped_first_prompt_ready = false;
                    waiting_for_second_prompt_ready = false;
                    first_prompt_ready_timeout_until = {};
                    alignment_prompt_show_until = {};
                    gameplay_ready_since = {};
                    last_prompt_right_thumb_click = false;
                    app_hook_log(L"Prompt gate: game loaded in RAM; gameplay height prompt armed.");
                }
            } else {
                if (game_loaded_session_active && game_session_inactive_since.time_since_epoch().count() == 0) {
                    game_session_inactive_since = now;
                }
                if (game_session_inactive_since.time_since_epoch().count() != 0 &&
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - game_session_inactive_since).count() >= 2000) {
                    game_loaded_session_active = false;
                    alignment_prompt_shown_this_launch = false;
                    skipped_first_prompt_ready = false;
                    waiting_for_second_prompt_ready = false;
                    first_prompt_ready_timeout_until = {};
                    alignment_prompt_show_until = {};
                    gameplay_ready_since = {};
                    last_prompt_right_thumb_click = false;
                    app_hook_log(L"Prompt gate: game unloaded from RAM; prompt session cleared.");
                }
            }
            
            const PromptGateSnapshot prompt_gate = read_prompt_gate_snapshot(player);
            const bool player_ready_for_prompt = player_is_prompt_gameplay_ready(player);
            const bool cannon_ready_for_prompt = cannon_transform_is_gameplay_ready(player);
            const bool gameplay_ready_for_prompt = player_ready_for_prompt && cannon_ready_for_prompt;
            if (gameplay_ready_for_prompt) {
                if (gameplay_ready_since.time_since_epoch().count() == 0) {
                    gameplay_ready_since = now;
                }
            } else {
                gameplay_ready_since = {};
            }

            const bool gameplay_ready_stable =
                gameplay_ready_since.time_since_epoch().count() != 0 &&
                std::chrono::duration_cast<std::chrono::milliseconds>(now - gameplay_ready_since).count() >= 1200;
            if (!game_loaded_in_ram) {
                skipped_first_prompt_ready = false;
                waiting_for_second_prompt_ready = false;
                first_prompt_ready_timeout_until = {};
                alignment_prompt_show_until = {};
                last_prompt_right_thumb_click = false;
            }
            

            bool show_alignment_prompt = gameplay_ready_stable;
            if (gameplay_ready_stable && !skipped_first_prompt_ready) {
                skipped_first_prompt_ready = true;
                waiting_for_second_prompt_ready = true;
                first_prompt_ready_timeout_until = now + std::chrono::seconds(10);
                show_alignment_prompt = false;
                app_hook_log(L"Prompt gate: skipped first ready window; waiting 10 seconds for gameplay.");
            } else if (waiting_for_second_prompt_ready) {
                show_alignment_prompt = false;
                if (first_prompt_ready_timeout_until.time_since_epoch().count() != 0 &&
                    now >= first_prompt_ready_timeout_until) {
                    waiting_for_second_prompt_ready = false;
                    show_alignment_prompt = gameplay_ready_stable;
                    app_hook_log(L"Prompt gate: first ready window timeout expired; stable ready can show prompt.");
                }
            }
            
            if (alignment_prompt_shown_this_launch) {
                show_alignment_prompt =
                    alignment_prompt_show_until.time_since_epoch().count() != 0 &&
                    now < alignment_prompt_show_until;
            } else if (show_alignment_prompt) {
                alignment_prompt_shown_this_launch = true;
                alignment_prompt_show_until = now + std::chrono::seconds(10);
                app_hook_log(L"Prompt gate: alignment prompt consumed for this game session.");
            }
            const bool prompt_right_thumb_click = vr_right_pose.valid && vr_right_pose.thumbstick_click;
            if (show_alignment_prompt && prompt_right_thumb_click && !last_prompt_right_thumb_click) {
                show_alignment_prompt = false;
                alignment_prompt_shown_this_launch = true;
                alignment_prompt_show_until = {};
                app_hook_log(L"Prompt gate: height prompt dismissed by right thumbstick.");
            }
            
            last_prompt_right_thumb_click = prompt_right_thumb_click;
            if (vr_settings_visible)
                show_alignment_prompt = false;
            g_shared_state->settings.showAlignmentPrompt = show_alignment_prompt ? 1u : 0u;
            log_gameplay_candidate_profile(state_mgr, player, player_ready_for_prompt,
                                           cannon_ready_for_prompt, gameplay_ready_stable,
                                           show_alignment_prompt, now);
            log_prompt_gate_snapshot(prompt_gate, player_ready_for_prompt, cannon_ready_for_prompt,
                                     gameplay_ready_stable, show_alignment_prompt, now);
        }
        static uint64_t last_hook_heartbeat_seen = 0;
        static auto last_hook_heartbeat_time = std::chrono::steady_clock::time_point{};
        if (has_shared_state && g_shared_state->hookHeartbeat != last_hook_heartbeat_seen) {
            last_hook_heartbeat_seen = g_shared_state->hookHeartbeat;
            last_hook_heartbeat_time = now;
        }

        const bool hook_recent =
            last_hook_heartbeat_time.time_since_epoch().count() != 0 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_hook_heartbeat_time).count() < 2500;
        static bool dolphin_running_for_hook_status = false;
        static auto last_hook_process_check = std::chrono::steady_clock::time_point{};
        if (last_hook_process_check.time_since_epoch().count() == 0 ||
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_hook_process_check).count() >= 1000) {
            dolphin_running_for_hook_status = find_process_id_by_name(L"Dolphin.exe").has_value();
            last_hook_process_check = now;
        }
        if (!dolphin_running_for_hook_status) {
            g_app.hook_status = "Hook: waiting for Dolphin";
            last_hook_heartbeat_seen = 0;
            last_hook_heartbeat_time = {};
        } else if (!hook_recent) {
            g_app.hook_status = "Hook: not alive in Dolphin";
        } else {
            const uint32_t flags = has_shared_state ? g_shared_state->hookStatusFlags : 0;
            if ((flags & PrimedGun::HookStatusOpenXrInstalled) == 0) {
                g_app.hook_status = "Hook: injected, waiting for OpenXR";
            } else if ((flags & PrimedGun::HookStatusOpenXrGetProcReady) == 0) {
                g_app.hook_status = "Hook: OpenXR seen, waiting for xrGetInstanceProcAddr";
            } else {
                g_app.hook_status = "Hook: OpenXR bridge attached";
            }
        }
        

        static auto last_hook_alive_retry = std::chrono::steady_clock::time_point{};
        if (dolphin_running_for_hook_status && !hook_recent &&
            (last_hook_alive_retry.time_since_epoch().count() == 0 ||
             std::chrono::duration_cast<std::chrono::milliseconds>(now - last_hook_alive_retry).count() >= 1000)) {
            last_hook_alive_retry = now;
            ensure_dolphin_hook_loaded();
        }

        if (has_shared_state && g_shared_state->trackingSource == 1 &&
            g_shared_state->trackingRuntimeActive && g_shared_state->trackingGeneration != 0) {
            g_app.tracking_ok = true;
            g_app.tracking_status = "DolphinXR OpenXR";
        } else {
            g_app.tracking_ok = false;
            g_app.tracking_status = "Waiting for DolphinXR OpenXR";
        }
        static auto last_game_detection = std::chrono::steady_clock::time_point{};
        if (last_game_detection.time_since_epoch().count() == 0 ||
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_game_detection).count() >= 500) {
            update_game_revision_detection();
            last_game_detection = now;
        }

        static auto last_patch_watchdog = std::chrono::steady_clock::time_point{};
        if (last_patch_watchdog.time_since_epoch().count() == 0 ||
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_patch_watchdog).count() >= 1000) {
            verify_app_patches_applied();
            apply_helmet_opacity_zero();
            last_patch_watchdog = now;
        }

        if (!g_app.active && g_app.dolphin_ok && g_app.game_rev0_ok &&
            g_app.tracking_ok && app_patches_are_applied()) {
            g_app.active = true;
            g_app.recenter_requested.store(true, std::memory_order_relaxed);
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        draw_gui(g_settings, g_app, g_dolphin);
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
    close_live_shared_state();
    g_dolphin.disconnect();
    restore_dolphin_borrowed_controls();
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

