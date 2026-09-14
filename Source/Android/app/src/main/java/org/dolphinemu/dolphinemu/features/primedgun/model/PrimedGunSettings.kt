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

    /** The cannon texture slot most recently applied, 0 = default. */
    const val KEY_CANNON_TEXTURE_SLOT = "cannon_texture_slot"

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

    /** Restores every runtime setting to its default, like the Qt window's Reset All button. */
    @JvmStatic
    external fun resetAll()

    /** The PrimedGun release string shown in the title bar, without a leading "v". */
    @JvmStatic
    external fun getVersion(): String

    /** Path of the slot A memory card for NTSC-U games: the file the old-save transfer replaces. */
    @JvmStatic
    external fun getMemoryCardPath(): String

    /**
     * Applies the settings stored in another install's PrimedGun.ini (or an old Qt.ini) to the
     * running mod and persists them. Returns false if the file holds no PrimedGun settings.
     */
    @JvmStatic
    external fun importSettings(path: String): Boolean

    /**
     * Copies a cannon texture slot (0 = default, 1-4 = presets, 5 = custom) into the managed
     * texture pack, refreshes the texture cache and records the slot. Returns false when the slot
     * holds no textures.
     */
    @JvmStatic
    external fun applyCannonTextureSlot(slot: Int): Boolean

    /**
     * Writes MEM1 plus a register snapshot into the user directory for crash reports. Returns the
     * dump path, or an empty string when no game is running or the files could not be written.
     */
    @JvmStatic
    external fun dumpMem1(): String
}
