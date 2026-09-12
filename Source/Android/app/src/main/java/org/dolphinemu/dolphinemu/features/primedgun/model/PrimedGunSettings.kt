// Copyright 2026 PrimedGun Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.primedgun.model

/**
 * Access to PrimedGun's RuntimeSettings, which live in PrimedGun.ini rather than in Dolphin's
 * config system. Keys are the INI key names from VisitPersistedSettings() in Core, plus the two
 * derived HUD axes the C++ bridge synthesises from the stored one-sided offsets.
 *
 * Reads and writes go straight to the running mod, so changes take effect immediately. They are
 * only persisted when [save] is called.
 */
object PrimedGunSettings {
    // Derived keys: the runtime stores four one-sided HUD offsets, the UI edits two signed axes.
    const val KEY_HUD_OFFSET_VERTICAL = "hud_offset_vertical"
    const val KEY_HUD_OFFSET_HORIZONTAL = "hud_offset_horizontal"

    @JvmStatic
    external fun getBoolean(key: String, defaultValue: Boolean): Boolean

    @JvmStatic
    external fun setBoolean(key: String, value: Boolean)

    @JvmStatic
    external fun getFloat(key: String, defaultValue: Float): Float

    @JvmStatic
    external fun setFloat(key: String, value: Float)

    @JvmStatic
    external fun getInt(key: String, defaultValue: Int): Int

    @JvmStatic
    external fun setInt(key: String, value: Int)

    /** Writes the current settings to PrimedGun.ini. Returns false if the file could not be written. */
    @JvmStatic
    external fun save(): Boolean

    @JvmStatic
    external fun resetController()

    @JvmStatic
    external fun resetHud()

    @JvmStatic
    external fun resetTargeting()

    @JvmStatic
    external fun resetCalibration()

    @JvmStatic
    external fun applySamusArmPreset()
}
