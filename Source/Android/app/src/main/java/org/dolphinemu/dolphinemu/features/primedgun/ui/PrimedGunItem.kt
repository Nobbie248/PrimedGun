// Copyright 2026 PrimedGun Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.primedgun.ui

/**
 * One row of the PrimedGun settings list, matching the control vocabulary of the Qt launcher.
 *
 * Rows carry getter/setter lambdas rather than a settings key so that derived values (the signed
 * HUD axes) and inverted ones (a radio pair whose second option is the `true` case) need no
 * special handling in the adapter.
 */
sealed class PrimedGunItem {
    /** Orange section title, e.g. "Directional Movement". */
    class Header(val title: String) : PrimedGunItem()

    /** Muted explanatory line, used where the Qt window shows a tooltip or a note label. */
    class Note(val text: String) : PrimedGunItem()

    /** Full-width button, e.g. "Reset Controller". */
    class Action(val title: String, val onClick: () -> Unit) : PrimedGunItem()

    class Switch(
        val title: String,
        val description: String? = null,
        val get: () -> Boolean,
        val set: (Boolean) -> Unit
    ) : PrimedGunItem()

    class Slider(
        val title: String,
        val min: Float,
        val max: Float,
        val step: Float,
        val get: () -> Float,
        val set: (Float) -> Unit
    ) : PrimedGunItem() {
        /** Matches the Qt spin box, which shows three decimals only for sub-0.1 steps. */
        val decimals: Int get() = if (step < 0.1f) 3 else 2
    }

    /** Drop-down of mutually exclusive values, e.g. "Rumble target". */
    class Choice(
        val title: String,
        val labels: List<String>,
        val values: List<Int>,
        val get: () -> Int,
        val set: (Int) -> Unit
    ) : PrimedGunItem()

    /** Exclusive pair of radio buttons; [get] returns true when option B is selected. */
    class Toggle2(
        val title: String? = null,
        val labelA: String,
        val labelB: String,
        val get: () -> Boolean,
        val set: (Boolean) -> Unit
    ) : PrimedGunItem()
}
