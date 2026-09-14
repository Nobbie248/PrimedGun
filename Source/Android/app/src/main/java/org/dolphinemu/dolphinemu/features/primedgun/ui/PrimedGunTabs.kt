// Copyright 2026 PrimedGun Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.primedgun.ui

import android.app.Activity
import android.content.Context
import android.content.Intent
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import org.dolphinemu.dolphinemu.R
import org.dolphinemu.dolphinemu.activities.UserDataActivity
import org.dolphinemu.dolphinemu.features.cheats.ui.CheatsActivity
import org.dolphinemu.dolphinemu.features.primedgun.model.PrimedGunSelectedGame
import org.dolphinemu.dolphinemu.features.primedgun.model.PrimedGunSettings
import org.dolphinemu.dolphinemu.features.settings.model.AdHocBooleanSetting
import org.dolphinemu.dolphinemu.features.settings.model.QuestVrSettings
import org.dolphinemu.dolphinemu.features.settings.model.Settings
import org.dolphinemu.dolphinemu.features.settings.ui.MenuTag
import org.dolphinemu.dolphinemu.features.settings.ui.SettingsActivity
import org.dolphinemu.dolphinemu.ui.main.GameLibraryActivity
import org.dolphinemu.dolphinemu.utils.ThreadUtil

/**
 * The launcher's tabs, in the order MainWindow::ConnectStack() adds them, and the row lists for
 * the tabs that are plain settings lists.
 *
 * Order, labels, ranges and step sizes are taken from MainWindow::ConnectStack() so the Quest UI
 * and the Qt launcher stay the same product. Defaults repeat the RuntimeSettings struct defaults
 * because they are the fallback the JNI bridge uses when a key is missing.
 *
 * Settings the Qt window only resets but never exposes as an editable row (the grip press
 * thresholds, gun_targeting_enabled) are deliberately absent here too.
 */
object PrimedGunTabs {

    enum class Tab(val titleId: Int) {
        SETUP(R.string.primedgun_tab_setup),
        CONTROLLER(R.string.primedgun_tab_controller),
        CALIBRATION(R.string.primedgun_tab_calibration),
        CANNON_TEXTURES(R.string.primedgun_tab_cannon_textures),
        LAYOUT(R.string.primedgun_tab_layout),
        DOLPHIN_CONFIG(R.string.primedgun_tab_dolphin_config)
    }

    /** Rows for the list-based tabs. The other tabs have their own fragments. */
    fun itemsFor(context: Context, tab: Tab): List<PrimedGunItem> = when (tab) {
        Tab.CONTROLLER -> controllerItems(context)
        Tab.CALIBRATION -> calibrationItems(context)
        Tab.DOLPHIN_CONFIG -> dolphinConfigItems(context)
        Tab.SETUP, Tab.CANNON_TEXTURES, Tab.LAYOUT -> emptyList()
    }

    private fun controllerItems(context: Context): List<PrimedGunItem> = buildList {
        add(header(context, R.string.primedgun_section_controller_mapping))
        add(PrimedGunItem.Action(context.getString(R.string.primedgun_reset_controller)) {
            PrimedGunSettings.resetController()
        })
        // Stored as use_right_hand, so the "left hand" option is the inverted case.
        add(
            PrimedGunItem.Toggle2(
                labelA = context.getString(R.string.primedgun_right_hand),
                labelB = context.getString(R.string.primedgun_left_hand),
                get = { !PrimedGunSettings.getBoolean("use_right_hand", true) },
                set = { PrimedGunSettings.setBoolean("use_right_hand", !it) }
            )
        )
        add(switch(context, R.string.primedgun_vr_menu_hold_left_stick, "vr_menu_hold_left_stick", false))
        add(
            switch(
                context,
                R.string.primedgun_vr_menu_requires_head_zone,
                "vr_menu_requires_head_zone",
                false
            )
        )
        add(
            switch(
                context,
                R.string.primedgun_combat_jump_use_primary_button,
                "combat_jump_use_primary_button",
                false
            )
        )

        add(header(context, R.string.primedgun_section_rumble_grip))
        add(switch(context, R.string.primedgun_rumble, "rumble_enabled", true))
        add(
            PrimedGunItem.Choice(
                title = context.getString(R.string.primedgun_rumble_target),
                labels = listOf(
                    context.getString(R.string.primedgun_rumble_both),
                    context.getString(R.string.primedgun_rumble_left_only),
                    context.getString(R.string.primedgun_rumble_right_only)
                ),
                values = listOf(0, 1, 2),
                get = { PrimedGunSettings.getInt("rumble_hand_mode", 2) },
                set = { PrimedGunSettings.setInt("rumble_hand_mode", it) }
            )
        )
        add(
            switch(
                context,
                R.string.primedgun_grip_inputs_enabled,
                "primedgun_grip_inputs_enabled",
                true
            )
        )
        add(
            switch(
                context,
                R.string.primedgun_grip_inputs_use_trackpad,
                "primedgun_grip_inputs_use_trackpad",
                false
            )
        )
        add(PrimedGunItem.Note(context.getString(R.string.primedgun_grip_inputs_use_trackpad_note)))
        add(slider(context, R.string.primedgun_rumble_intensity, "rumble_intensity", 0.00f, 1.00f, 0.05f, 0.35f))

        add(header(context, R.string.primedgun_section_dpad))
        add(switch(context, R.string.primedgun_dpad_enabled, "xr_dpad_enabled", true))
        add(slider(context, R.string.primedgun_dpad_head_radius, "xr_dpad_head_radius", 0.08f, 0.28f, 0.01f, 0.28f))
        add(slider(context, R.string.primedgun_dpad_head_y_below, "xr_dpad_head_y_below", 0.02f, 0.25f, 0.01f, 0.02f))
        add(slider(context, R.string.primedgun_dpad_deadzone, "xr_dpad_deadzone", 0.20f, 0.80f, 0.01f, 0.45f))

        add(header(context, R.string.primedgun_section_directional_movement))
        add(
            switch(
                context,
                R.string.primedgun_movement_enabled,
                "directional_movement_enabled",
                true
            )
        )
        add(
            PrimedGunItem.Toggle2(
                labelA = context.getString(R.string.primedgun_left_stick),
                labelB = context.getString(R.string.primedgun_right_stick),
                get = { PrimedGunSettings.getBoolean("directional_movement_use_right_stick", false) },
                set = { PrimedGunSettings.setBoolean("directional_movement_use_right_stick", it) }
            )
        )
        add(
            PrimedGunItem.Toggle2(
                labelA = context.getString(R.string.primedgun_controller_direction),
                labelB = context.getString(R.string.primedgun_hmd_direction),
                get = {
                    PrimedGunSettings.getBoolean("directional_movement_use_hmd_direction", false)
                },
                set = {
                    PrimedGunSettings.setBoolean("directional_movement_use_hmd_direction", it)
                }
            )
        )
        add(
            slider(
                context, R.string.primedgun_movement_deadzone,
                "directional_movement_deadzone", 0.05f, 0.80f, 0.01f, 0.25f
            )
        )
        add(
            slider(
                context, R.string.primedgun_movement_speed,
                "directional_movement_speed", 4.0f, 30.0f, 0.25f, 14.0f
            )
        )
        add(
            slider(
                context, R.string.primedgun_movement_accel,
                "directional_movement_accel", 5.0f, 120.0f, 1.0f, 45.0f
            )
        )
        add(
            slider(
                context, R.string.primedgun_movement_air_accel,
                "directional_movement_air_accel", 0.0f, 60.0f, 0.5f, 8.0f
            )
        )
        add(
            slider(
                context, R.string.primedgun_look_yaw_sensitivity,
                "look_yaw_sensitivity", 0.20f, 3.00f, 0.05f, 1.0f
            )
        )
        add(switch(context, R.string.primedgun_snap_turn, "snap_turn_enabled", false))
        add(
            PrimedGunItem.Choice(
                title = context.getString(R.string.primedgun_snap_turn_angle),
                labels = listOf(
                    context.getString(R.string.primedgun_snap_turn_30),
                    context.getString(R.string.primedgun_snap_turn_45),
                    context.getString(R.string.primedgun_snap_turn_60),
                    context.getString(R.string.primedgun_snap_turn_90)
                ),
                values = listOf(30, 45, 60, 90),
                get = { PrimedGunSettings.getInt("snap_turn_degrees", 45) },
                set = { PrimedGunSettings.setInt("snap_turn_degrees", it) }
            )
        )
    }

    private fun calibrationItems(context: Context): List<PrimedGunItem> = buildList {
        add(header(context, R.string.primedgun_section_in_headset_display))
        add(switch(context, R.string.primedgun_vr_overlays_enabled, "vr_overlays_enabled", true))
        add(switch(context, R.string.primedgun_height_prompt_enabled, "height_prompt_enabled", true))
        add(
            switch(
                context,
                R.string.primedgun_cinematic_screen_enabled,
                "cinematic_screen_enabled",
                false
            )
        )
        add(switch(context, R.string.primedgun_vr_menu_floating, "vr_menu_floating", false))
        add(
            switch(
                context,
                R.string.primedgun_game_menu_screen_enabled,
                "game_menu_screen_enabled",
                false
            )
        )
        add(switch(context, R.string.primedgun_visor_helmet_enabled, "visor_helmet_enabled", false))
        add(PrimedGunItem.Note(context.getString(R.string.primedgun_visor_helmet_note)))
        add(
            switch(
                context,
                R.string.primedgun_position_marker_enabled,
                "position_marker_enabled",
                false
            )
        )

        add(header(context, R.string.primedgun_section_culling))
        add(
            switch(
                context,
                R.string.primedgun_frustum_culling_enabled,
                "frustum_culling_enabled",
                true
            )
        )
        add(
            slider(
                context, R.string.primedgun_frustum_culling_degrees,
                "frustum_culling_degrees", 70.0f, 175.0f, 1.0f, 115.0f
            )
        )

        add(header(context, R.string.primedgun_section_hud))
        add(PrimedGunItem.Action(context.getString(R.string.primedgun_reset_hud)) {
            PrimedGunSettings.resetHud()
        })
        add(slider(context, R.string.primedgun_hud_distance, "metroid_hud_distance", 0.10f, 3.00f, 0.05f, 0.5f))
        add(slider(context, R.string.primedgun_hud_size, "metroid_hud_size", 0.10f, 3.00f, 0.05f, 0.5f))
        add(
            slider(
                context, R.string.primedgun_hud_vertical,
                PrimedGunSettings.KEY_HUD_OFFSET_VERTICAL, -1.00f, 1.00f, 0.01f, 0.0f
            )
        )
        add(
            slider(
                context, R.string.primedgun_hud_horizontal,
                PrimedGunSettings.KEY_HUD_OFFSET_HORIZONTAL, -1.00f, 1.00f, 0.01f, 0.0f
            )
        )

        add(header(context, R.string.primedgun_section_targeting))
        add(PrimedGunItem.Action(context.getString(R.string.primedgun_reset_targeting)) {
            PrimedGunSettings.resetTargeting()
        })
        add(
            slider(
                context, R.string.primedgun_target_distance,
                "gun_targeting_distance", 10.0f, 120.0f, 1.0f, 60.0f
            )
        )
        add(
            slider(
                context, R.string.primedgun_target_radius,
                "gun_targeting_radius", 0.5f, 8.0f, 0.1f, 4.0f
            )
        )

        add(header(context, R.string.primedgun_section_offset_tuning))
        add(PrimedGunItem.Action(context.getString(R.string.primedgun_reset_calibration)) {
            PrimedGunSettings.resetCalibration()
        })

        add(header(context, R.string.primedgun_section_position))
        add(slider(context, R.string.primedgun_offset_left_right, "model_offset_x", -2.0f, 2.0f, 0.01f, 0.0f))
        add(slider(context, R.string.primedgun_offset_forward_back, "model_offset_y", -2.0f, 2.0f, 0.01f, 0.0f))
        add(slider(context, R.string.primedgun_offset_up_down, "model_offset_z", -2.0f, 2.0f, 0.01f, 0.0f))

        add(header(context, R.string.primedgun_section_rotation))
        add(slider(context, R.string.primedgun_pitch_offset, "rot_offset_x", -180.0f, 180.0f, 0.5f, 0.0f))
        add(slider(context, R.string.primedgun_yaw_offset, "rot_offset_y", -180.0f, 180.0f, 0.5f, 0.0f))
        add(slider(context, R.string.primedgun_roll_offset, "rot_offset_z", -180.0f, 180.0f, 0.5f, 0.0f))

        add(header(context, R.string.primedgun_section_presets))
        add(PrimedGunItem.Action(context.getString(R.string.primedgun_preset_default_arm)) {
            PrimedGunSettings.resetCalibration()
        })
        add(PrimedGunItem.Action(context.getString(R.string.primedgun_preset_samus_arm)) {
            PrimedGunSettings.applySamusArmPreset()
        })
    }

    // The Qt tab opens Dolphin's own windows. Their Android counterparts are the settings
    // activity's sections, the cheats activity, and the user data screen; the entries with no
    // Android equivalent (hotkeys, memory card manager, resource packs) are replaced by the
    // screens that hold the same settings here.
    private fun dolphinConfigItems(context: Context): List<PrimedGunItem> = buildList {
        add(header(context, R.string.primedgun_section_dolphin_config))
        add(PrimedGunItem.Note(context.getString(R.string.primedgun_dolphin_config_note)))

        add(header(context, R.string.primedgun_section_configuration))
        add(action(context, R.string.primedgun_dolphin_settings) {
            SettingsActivity.launch(context, MenuTag.SETTINGS)
        })
        add(action(context, R.string.primedgun_graphics_settings) {
            SettingsActivity.launch(context, MenuTag.GRAPHICS)
        })
        if (QuestVrSettings.isQuestBuild()) {
            add(action(context, R.string.primedgun_openxr_settings) {
                SettingsActivity.launch(context, MenuTag.OPENXR)
            })
        }
        add(action(context, R.string.primedgun_controller_settings) {
            SettingsActivity.launch(context, MenuTag.GCPAD_TYPE)
        })

        add(header(context, R.string.primedgun_section_tools))
        add(action(context, R.string.primedgun_game_library) {
            context.startActivity(Intent(context, GameLibraryActivity::class.java))
        })
        add(action(context, R.string.primedgun_gamecube_settings) {
            SettingsActivity.launch(context, MenuTag.CONFIG_GAME_CUBE)
        })
        add(action(context, R.string.primedgun_cheat_manager) { launchCheatManager(context) })
        add(action(context, R.string.primedgun_user_data) { UserDataActivity.launch(context) })

        add(header(context, R.string.primedgun_section_warnings))
        add(
            PrimedGunItem.Switch(
                title = context.getString(R.string.primedgun_suppress_cpu_thread_warnings),
                get = { suppressCpuThreadWarnings().boolean },
                set = { value ->
                    Settings().use { settings ->
                        settings.loadSettings()
                        suppressCpuThreadWarnings().setBoolean(settings, value)
                        settings.saveSettings()
                    }
                }
            )
        )

        add(header(context, R.string.primedgun_section_debug))
        add(action(context, R.string.primedgun_dump_ram) { dumpRam(context) })
    }

    private fun suppressCpuThreadWarnings() = AdHocBooleanSetting(
        Settings.FILE_DOLPHIN, Settings.SECTION_INI_INTERFACE, "SuppressCPUThreadWarnings", false
    )

    private fun launchCheatManager(context: Context) {
        // Metroid Prime is the only game the mod supports, so its ids are the fallback when no
        // readable game has been selected yet.
        val game = PrimedGunSelectedGame.parse(context)
        val gameId = game?.getGameId() ?: PrimedGunSelectedGame.METROID_PRIME_GAME_ID
        CheatsActivity.launch(
            context,
            gameId,
            game?.getGameTdbId() ?: gameId,
            game?.getRevision() ?: 0,
            false
        )
    }

    private fun dumpRam(context: Context) {
        val activity = context as? Activity
        if (activity == null) {
            showRamDumpResult(context, PrimedGunSettings.dumpMem1())
            return
        }
        // MEM1 is 24 MiB, so the write runs behind a progress dialog rather than on the UI thread.
        ThreadUtil.runOnThreadAndShowResult(activity, R.string.primedgun_ram_dump_title, 0, {
            val path = PrimedGunSettings.dumpMem1()
            if (path.isEmpty()) {
                context.getString(R.string.primedgun_ram_dump_failed)
            } else {
                context.getString(R.string.primedgun_ram_dump_done, path)
            }
        })
    }

    private fun showRamDumpResult(context: Context, path: String) {
        MaterialAlertDialogBuilder(context)
            .setTitle(R.string.primedgun_ram_dump_title)
            .setMessage(
                if (path.isEmpty()) {
                    context.getString(R.string.primedgun_ram_dump_failed)
                } else {
                    context.getString(R.string.primedgun_ram_dump_done, path)
                }
            )
            .setPositiveButton(R.string.ok, null)
            .show()
    }

    private fun header(context: Context, titleId: Int) =
        PrimedGunItem.Header(context.getString(titleId))

    private fun action(context: Context, titleId: Int, onClick: () -> Unit) =
        PrimedGunItem.Action(context.getString(titleId), onClick)

    private fun switch(context: Context, titleId: Int, key: String, default: Boolean) =
        PrimedGunItem.Switch(
            title = context.getString(titleId),
            get = { PrimedGunSettings.getBoolean(key, default) },
            set = { PrimedGunSettings.setBoolean(key, it) }
        )

    private fun slider(
        context: Context,
        titleId: Int,
        key: String,
        min: Float,
        max: Float,
        step: Float,
        default: Float
    ) = PrimedGunItem.Slider(
        title = context.getString(titleId),
        min = min,
        max = max,
        step = step,
        get = { PrimedGunSettings.getFloat(key, default) },
        set = { PrimedGunSettings.setFloat(key, it) }
    )
}
