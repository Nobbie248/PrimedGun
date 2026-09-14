// Copyright 2026 PrimedGun Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.primedgun.ui

/**
 * A launcher tab whose widgets mirror runtime settings. The launcher calls [refresh] after an
 * action that rewrote many settings at once (Reset All, importing an old PrimedGun.ini) so the
 * visible tab re-reads them instead of showing stale values.
 */
interface PrimedGunRefreshable {
    fun refresh()
}
