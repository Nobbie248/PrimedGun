#pragma once
#include <string>
#include <fstream>
#include <algorithm>
#include <vector>

inline constexpr bool kDefaultUseRightHand = true;
inline constexpr float kDefaultOffsetX = 0.0f;
inline constexpr float kDefaultOffsetY = 0.0f;
inline constexpr float kDefaultOffsetZ = 0.0f;
inline constexpr float kDefaultModelOffsetX = 0.0f;
inline constexpr float kDefaultModelOffsetY = 0.0f;
inline constexpr float kDefaultModelOffsetZ = 0.0f;
inline constexpr float kDefaultRotOffsetX = 0.0f;
inline constexpr float kDefaultRotOffsetY = 0.0f;
inline constexpr float kDefaultRotOffsetZ = 0.0f;
inline constexpr float kDefaultWorldScale = 1.50f;
inline constexpr bool kDefaultGunTargetingEnabled = true;
inline constexpr float kDefaultGunTargetingDistance = 60.0f;
inline constexpr float kDefaultGunTargetingRadius = 4.0f;
inline constexpr bool kDefaultXrDpadEnabled = true;
inline constexpr bool kDefaultAutoDolphinXrControls = true;
inline constexpr float kDefaultXrDpadHeadRadius = 0.18f;
inline constexpr float kDefaultXrDpadHeadYBelow = 0.14f;
inline constexpr float kDefaultXrDpadDeadzone = 0.45f;
inline constexpr int kDefaultXrDpadStickAxis = -1;
inline constexpr bool kDefaultDirectionalMovementEnabled = true;
inline constexpr bool kDefaultDirectionalMovementUseRightStick = false;
inline constexpr float kDefaultDirectionalMovementDeadzone = 0.25f;
inline constexpr float kDefaultDirectionalMovementSpeed = 14.0f;
inline constexpr float kDefaultDirectionalMovementAccel = 45.0f;
inline constexpr float kDefaultDirectionalMovementAirAccel = 8.0f;
inline constexpr float kDefaultViewHeightMeters = 1.6374f;

// Metroid Prime GCN NTSC Rev 0 (GM8E01 / 0-00)
// Restored to the simple working-position baseline.
struct GameAddresses {
    uint32_t state_manager;
    uint32_t player_offset;
    uint32_t camera_manager_offset;
    uint32_t gun_pos;
    uint32_t transform_offset;
    uint32_t cannon_offset;
    uint32_t gun_xf_offset;
    uint32_t beam_xf_offset;
    uint32_t world_xf_offset;
    uint32_t local_xf_offset;
    uint32_t tweak_player;
    uint32_t pitch_offset;
    uint32_t angvel_offset;
    uint32_t gun_matrix_offset;
    uint32_t gun_scale_addr;
};

inline GameAddresses get_addresses() {
    return {
        0x8045A1A8,  // state_manager
        0x84C,       // player_offset
        0x86C,       // camera_manager_offset
        0x8045BCE8,  // gun_pos (static)
        0x34,        // transform_offset
        0x490,       // cannon_offset (player -> gun ptr)
        0x3E8,       // gun_xf_offset
        0x418,       // beam_xf_offset
        0x4A8,       // world_xf_offset
        0x4D8,       // local_xf_offset
        0x8045C208,  // tweak_player
        0x3EC,       // firstperson_pitch
        0x14C,       // angular_vel_z
        0,           // gun_matrix_offset
        0,           // gun_scale_addr
    };
}

struct Settings {
    struct ArCodeToggle {
        std::string name;
        bool enabled = true;
    };

    bool use_right_hand = kDefaultUseRightHand;
    float offset_x = kDefaultOffsetX;
    float offset_y = kDefaultOffsetY;
    float offset_z = kDefaultOffsetZ;
    float model_offset_x = kDefaultModelOffsetX;
    float model_offset_y = kDefaultModelOffsetY;
    float model_offset_z = kDefaultModelOffsetZ;
    float rot_offset_x = kDefaultRotOffsetX;
    float rot_offset_y = kDefaultRotOffsetY;
    float rot_offset_z = kDefaultRotOffsetZ;
    float world_scale = kDefaultWorldScale;
    bool require_trigger = false;
    float trigger_threshold = 0.5f;
    bool show_matrix_debug = false;
    bool show_controller_debug = false;
    bool log_cannon_rotation_debug = false;
    bool gun_targeting_enabled = kDefaultGunTargetingEnabled;
    float gun_targeting_distance = kDefaultGunTargetingDistance;
    float gun_targeting_radius = kDefaultGunTargetingRadius;
    bool auto_dolphin_xr_controls = kDefaultAutoDolphinXrControls;
    bool xr_dpad_enabled = kDefaultXrDpadEnabled;
    float xr_dpad_head_radius = kDefaultXrDpadHeadRadius;
    float xr_dpad_head_y_below = kDefaultXrDpadHeadYBelow;
    float xr_dpad_deadzone = kDefaultXrDpadDeadzone;
    int xr_dpad_stick_axis = kDefaultXrDpadStickAxis;
    bool directional_movement_enabled = kDefaultDirectionalMovementEnabled;
    bool directional_movement_use_right_stick = kDefaultDirectionalMovementUseRightStick;
    float directional_movement_deadzone = kDefaultDirectionalMovementDeadzone;
    float directional_movement_speed = kDefaultDirectionalMovementSpeed;
    float directional_movement_accel = kDefaultDirectionalMovementAccel;
    float directional_movement_air_accel = kDefaultDirectionalMovementAirAccel;
    float view_height_meters = kDefaultViewHeightMeters;
    std::vector<ArCodeToggle> ar_code_toggles;

    static const char* filename() { return "primedgun_settings.ini"; }

    static std::string normalized_ar_code_name(const std::string& name) {
        if (name == "Relax Frustum Side Culling For 360 View")
            return "Disable Frustum Culling";
        if (name == "Restore VR rotation after 80040FE8 Hook")
            return "Cannon Rotation Hook";
        if (name == "Visor Patch" ||
            name == "Disable Visor Screen Effects" ||
            name == "Disable Visor Filter A" ||
            name == "Disable Visor Filter B" ||
            name == "Disable Visor Distortion" ||
            name == "Disable Visor Effects")
            return "Disable Visor Blur";
        return name;
    }

    void reset_all() {
        use_right_hand = kDefaultUseRightHand;
        offset_x = kDefaultOffsetX;
        offset_y = kDefaultOffsetY;
        offset_z = kDefaultOffsetZ;
        model_offset_x = kDefaultModelOffsetX;
        model_offset_y = kDefaultModelOffsetY;
        model_offset_z = kDefaultModelOffsetZ;
        rot_offset_x = kDefaultRotOffsetX;
        rot_offset_y = kDefaultRotOffsetY;
        rot_offset_z = kDefaultRotOffsetZ;
        world_scale = kDefaultWorldScale;
        require_trigger = false;
        trigger_threshold = 0.5f;
        show_matrix_debug = false;
        show_controller_debug = false;
        log_cannon_rotation_debug = false;
        gun_targeting_enabled = kDefaultGunTargetingEnabled;
        gun_targeting_distance = kDefaultGunTargetingDistance;
        gun_targeting_radius = kDefaultGunTargetingRadius;
        auto_dolphin_xr_controls = kDefaultAutoDolphinXrControls;
        xr_dpad_enabled = kDefaultXrDpadEnabled;
        xr_dpad_head_radius = kDefaultXrDpadHeadRadius;
        xr_dpad_head_y_below = kDefaultXrDpadHeadYBelow;
        xr_dpad_deadzone = kDefaultXrDpadDeadzone;
        xr_dpad_stick_axis = kDefaultXrDpadStickAxis;
        directional_movement_enabled = kDefaultDirectionalMovementEnabled;
        directional_movement_use_right_stick = kDefaultDirectionalMovementUseRightStick;
        directional_movement_deadzone = kDefaultDirectionalMovementDeadzone;
        directional_movement_speed = kDefaultDirectionalMovementSpeed;
        directional_movement_accel = kDefaultDirectionalMovementAccel;
        directional_movement_air_accel = kDefaultDirectionalMovementAirAccel;
        view_height_meters = kDefaultViewHeightMeters;
        for (ArCodeToggle& toggle : ar_code_toggles)
            toggle.enabled = true;
    }

    bool ensure_ar_code_toggle(const std::string& name) {
        const std::string normalized = normalized_ar_code_name(name);
        if (normalized.empty())
            return false;
        const auto found = std::find_if(ar_code_toggles.begin(), ar_code_toggles.end(),
            [&](const ArCodeToggle& toggle) { return toggle.name == normalized; });
        if (found != ar_code_toggles.end())
            return false;
        ar_code_toggles.push_back({normalized, true});
        return true;
    }

    bool ar_code_enabled(const std::string& name) const {
        const std::string normalized = normalized_ar_code_name(name);
        const auto found = std::find_if(ar_code_toggles.begin(), ar_code_toggles.end(),
            [&](const ArCodeToggle& toggle) { return toggle.name == normalized; });
        return found == ar_code_toggles.end() || found->enabled;
    }

    bool prune_ar_code_toggles(const std::vector<std::string>& valid_names) {
        const size_t old_size = ar_code_toggles.size();
        ar_code_toggles.erase(
            std::remove_if(ar_code_toggles.begin(), ar_code_toggles.end(),
                [&](const ArCodeToggle& toggle) {
                    return std::find(valid_names.begin(), valid_names.end(), toggle.name) ==
                        valid_names.end();
                }),
            ar_code_toggles.end());
        return ar_code_toggles.size() != old_size;
    }

    void save() const {
        std::ofstream f(filename());
        if (!f) return;
        f << "use_right_hand=" << use_right_hand << "\n";
        f << "offset_x=" << offset_x << "\n";
        f << "offset_y=" << offset_y << "\n";
        f << "offset_z=" << offset_z << "\n";
        f << "model_offset_x=" << model_offset_x << "\n";
        f << "model_offset_y=" << model_offset_y << "\n";
        f << "model_offset_z=" << model_offset_z << "\n";
        f << "rot_offset_x=" << rot_offset_x << "\n";
        f << "rot_offset_y=" << rot_offset_y << "\n";
        f << "rot_offset_z=" << rot_offset_z << "\n";
        f << "world_scale=" << world_scale << "\n";
        f << "require_trigger=" << require_trigger << "\n";
        f << "trigger_threshold=" << trigger_threshold << "\n";
        f << "show_matrix_debug=" << show_matrix_debug << "\n";
        f << "show_controller_debug=" << show_controller_debug << "\n";
        f << "log_cannon_rotation_debug=" << log_cannon_rotation_debug << "\n";
        f << "gun_targeting_enabled=" << gun_targeting_enabled << "\n";
        f << "gun_targeting_distance=" << gun_targeting_distance << "\n";
        f << "gun_targeting_radius=" << gun_targeting_radius << "\n";
        f << "auto_dolphin_xr_controls=" << auto_dolphin_xr_controls << "\n";
        f << "xr_dpad_enabled=" << xr_dpad_enabled << "\n";
        f << "xr_dpad_head_radius=" << xr_dpad_head_radius << "\n";
        f << "xr_dpad_head_y_below=" << xr_dpad_head_y_below << "\n";
        f << "xr_dpad_deadzone=" << xr_dpad_deadzone << "\n";
        f << "xr_dpad_stick_axis=" << xr_dpad_stick_axis << "\n";
        f << "directional_movement_enabled=" << directional_movement_enabled << "\n";
        f << "directional_movement_use_right_stick=" << directional_movement_use_right_stick << "\n";
        f << "directional_movement_deadzone=" << directional_movement_deadzone << "\n";
        f << "directional_movement_speed=" << directional_movement_speed << "\n";
        f << "directional_movement_accel=" << directional_movement_accel << "\n";
        f << "directional_movement_air_accel=" << directional_movement_air_accel << "\n";
        f << "view_height_meters=" << view_height_meters << "\n";
        for (const ArCodeToggle& toggle : ar_code_toggles)
            f << "ar_code." << toggle.name << "=" << toggle.enabled << "\n";
    }

    void load() {
        std::ifstream f(filename());
        if (!f) return;
        bool migrated_settings = false;
        std::string line;
        while (std::getline(f, line)) {
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);
            if      (key == "use_right_hand")        use_right_hand = std::stoi(val);
            else if (key == "offset_x")              offset_x = std::stof(val);
            else if (key == "offset_y")              offset_y = std::stof(val);
            else if (key == "offset_z")              offset_z = std::stof(val);
            else if (key == "model_offset_x")        model_offset_x = std::stof(val);
            else if (key == "model_offset_y")        model_offset_y = std::stof(val);
            else if (key == "model_offset_z")        model_offset_z = std::stof(val);
            else if (key == "rot_offset_x")          rot_offset_x = std::stof(val);
            else if (key == "rot_offset_y")          rot_offset_y = std::stof(val);
            else if (key == "rot_offset_z")          rot_offset_z = std::stof(val);
            else if (key == "world_scale")           world_scale = std::stof(val);
            else if (key == "require_trigger")       require_trigger = std::stoi(val);
            else if (key == "trigger_threshold")     trigger_threshold = std::stof(val);
            else if (key == "show_matrix_debug")     show_matrix_debug = std::stoi(val);
            else if (key == "show_controller_debug") show_controller_debug = std::stoi(val);
            else if (key == "log_cannon_rotation_debug") log_cannon_rotation_debug = std::stoi(val);
            else if (key == "gun_targeting_enabled") gun_targeting_enabled = std::stoi(val);
            else if (key == "gun_targeting_distance") gun_targeting_distance = std::stof(val);
            else if (key == "gun_targeting_radius") gun_targeting_radius = std::stof(val);
            else if (key == "auto_dolphin_xr_controls") auto_dolphin_xr_controls = std::stoi(val);
            else if (key == "xr_dpad_enabled")       xr_dpad_enabled = std::stoi(val);
            else if (key == "xr_dpad_head_radius")   xr_dpad_head_radius = std::stof(val);
            else if (key == "xr_dpad_head_y_below")  xr_dpad_head_y_below = std::stof(val);
            else if (key == "xr_dpad_deadzone")      xr_dpad_deadzone = std::stof(val);
            else if (key == "xr_dpad_stick_axis")    xr_dpad_stick_axis = std::stoi(val);
            else if (key == "directional_movement_enabled") directional_movement_enabled = std::stoi(val);
            else if (key == "directional_movement_use_right_stick") directional_movement_use_right_stick = std::stoi(val);
            else if (key == "directional_movement_deadzone") directional_movement_deadzone = std::stof(val);
            else if (key == "directional_movement_speed") directional_movement_speed = std::stof(val);
            else if (key == "directional_movement_accel") directional_movement_accel = std::stof(val);
            else if (key == "directional_movement_air_accel") directional_movement_air_accel = std::stof(val);
            else if (key == "view_height_meters") view_height_meters = std::stof(val);
            else if (key.rfind("ar_code.", 0) == 0) {
                const std::string raw_name = key.substr(8);
                const std::string name = normalized_ar_code_name(raw_name);
                if (name != raw_name)
                    migrated_settings = true;
                ensure_ar_code_toggle(name);
                for (ArCodeToggle& toggle : ar_code_toggles) {
                    if (toggle.name == name) {
                        toggle.enabled = std::stoi(val) != 0;
                        break;
                    }
                }
            }
        }
        xr_dpad_head_radius = std::clamp(xr_dpad_head_radius, 0.08f, 0.28f);
        xr_dpad_head_y_below = std::clamp(xr_dpad_head_y_below, 0.02f, 0.25f);
        xr_dpad_deadzone = std::clamp(xr_dpad_deadzone, 0.20f, 0.80f);
        xr_dpad_stick_axis = std::clamp(xr_dpad_stick_axis, -1, 4);
        directional_movement_deadzone = std::clamp(directional_movement_deadzone, 0.05f, 0.80f);
        directional_movement_speed = std::clamp(directional_movement_speed, 4.0f, 30.0f);
        directional_movement_accel = std::clamp(directional_movement_accel, 5.0f, 120.0f);
        directional_movement_air_accel = std::clamp(directional_movement_air_accel, 0.0f, 60.0f);
        gun_targeting_distance = std::clamp(gun_targeting_distance, 10.0f, 120.0f);
        gun_targeting_radius = std::clamp(gun_targeting_radius, 0.5f, 8.0f);
        view_height_meters = std::clamp(view_height_meters, 0.4f, 2.4f);
        model_offset_x = std::clamp(model_offset_x, -2.0f, 2.0f);
        model_offset_y = std::clamp(model_offset_y, -2.0f, 2.0f);
        model_offset_z = std::clamp(model_offset_z, -2.0f, 2.0f);
        if (migrated_settings)
            save();
    }
};
