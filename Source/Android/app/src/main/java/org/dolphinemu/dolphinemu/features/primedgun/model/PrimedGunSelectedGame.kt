// Copyright 2026 PrimedGun Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.primedgun.model

import android.content.Context
import android.net.Uri
import androidx.annotation.StringRes
import androidx.preference.PreferenceManager
import org.dolphinemu.dolphinemu.R
import org.dolphinemu.dolphinemu.model.GameFile
import org.dolphinemu.dolphinemu.utils.ContentHandler
import java.io.File

/**
 * The game file chosen with Select Game on the Setup tab.
 *
 * The Qt launcher keeps this in its own Qt.ini rather than in Dolphin's config, so the Android
 * counterpart lives in the app's shared preferences: it is frontend state, not emulator state,
 * and it has to be readable before the native directories are initialised.
 */
object PrimedGunSelectedGame {
    const val METROID_PRIME_GAME_ID = "GM8E01"

    private const val KEY_PATH = "primedgun_selected_game"

    fun getPath(context: Context): String? =
        PreferenceManager.getDefaultSharedPreferences(context)
            .getString(KEY_PATH, null)
            ?.takeIf { it.isNotEmpty() }

    fun setPath(context: Context, path: String?) {
        PreferenceManager.getDefaultSharedPreferences(context)
            .edit()
            .putString(KEY_PATH, path ?: "")
            .apply()
    }

    /** The file name shown after "Selected:", for both content URIs and plain paths. */
    fun displayName(path: String): String {
        if (!ContentHandler.isContentUri(path))
            return File(path).name
        return ContentHandler.getDisplayName(path)
            ?: Uri.parse(path).lastPathSegment?.substringAfterLast('/')
            ?: path
    }

    /**
     * Parses the selected game. Reads the disc header, so call it off the main thread, and only
     * after DirectoryInitialization has finished. Returns null when nothing is selected or the
     * file cannot be read.
     */
    fun parse(context: Context): GameFile? = getPath(context)?.let { GameFile.parse(it) }

    /** The desktop window's "Wrong game" / "Wrong revision" label for [game], or null if it fits. */
    @StringRes
    fun warningFor(game: GameFile): Int? = when {
        game.getGameId() != METROID_PRIME_GAME_ID -> R.string.primedgun_wrong_game
        game.getRevision() != 0 -> R.string.primedgun_wrong_revision
        else -> null
    }
}
