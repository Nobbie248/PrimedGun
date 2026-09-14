// Copyright 2026 PrimedGun Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.primedgun.ui

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.activity.result.contract.ActivityResultContracts
import androidx.annotation.StringRes
import androidx.core.view.isVisible
import androidx.fragment.app.Fragment
import androidx.lifecycle.lifecycleScope
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.dolphinemu.dolphinemu.NativeLibrary
import org.dolphinemu.dolphinemu.R
import org.dolphinemu.dolphinemu.activities.EmulationActivity
import org.dolphinemu.dolphinemu.databinding.FragmentPrimedgunSetupBinding
import org.dolphinemu.dolphinemu.dialogs.GamePropertiesDialog
import org.dolphinemu.dolphinemu.features.primedgun.model.PrimedGunCannonTextures
import org.dolphinemu.dolphinemu.features.primedgun.model.PrimedGunSelectedGame
import org.dolphinemu.dolphinemu.features.primedgun.model.PrimedGunSettings
import org.dolphinemu.dolphinemu.model.GameFile
import org.dolphinemu.dolphinemu.ui.main.MainActivity
import org.dolphinemu.dolphinemu.utils.AfterDirectoryInitializationRunner
import org.dolphinemu.dolphinemu.utils.ContentHandler
import org.dolphinemu.dolphinemu.utils.FileBrowserHelper
import org.dolphinemu.dolphinemu.utils.Log
import java.io.File
import java.io.IOException
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * The Setup tab: game selection and launch, and the old-save transfer.
 *
 * The desktop window's Transfer button searches folders next to the executable for an old
 * install. There is no such neighbourhood on a headset, so here the user picks the old memory
 * card file, and then optionally the old PrimedGun.ini, through the system file picker.
 */
class PrimedGunSetupFragment : Fragment(), PrimedGunRefreshable {

    companion object {
        private val MEMORY_CARD_EXTENSIONS = setOf("raw", "gcp")
    }

    private class TransferStep(val report: String?, val error: String?)

    private var binding: FragmentPrimedgunSetupBinding? = null
    private var selectedGame: GameFile? = null
    private var transferReport = ""

    private val requestGameFile = registerForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri: Uri? ->
        if (uri != null)
            onGamePicked(uri)
    }

    private val requestMemoryCard = registerForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri: Uri? ->
        if (uri != null)
            transferMemoryCard(uri)
    }

    private val requestSettingsFile = registerForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri: Uri? -> finishTransfer(uri) }

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        val inflated = FragmentPrimedgunSetupBinding.inflate(inflater, container, false)
        binding = inflated
        return inflated.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        val b = binding ?: return
        b.setupNotes.setOnClickListener { showSetupNotes() }
        b.setupSelectGame.setOnClickListener { requestGameFile.launch(arrayOf("*/*")) }
        b.setupPlay.setOnClickListener { play() }
        b.setupGameOptions.setOnClickListener { showGameOptions() }
        b.setupTransferOldSave.setOnClickListener { confirmTransfer() }
        refreshSelectedGame()
    }

    override fun onDestroyView() {
        super.onDestroyView()
        binding = null
    }

    override fun refresh() {
        // Nothing on this tab mirrors a runtime setting directly.
    }

    // ---------- Game selection ----------

    private fun refreshSelectedGame() {
        val b = binding ?: return
        val context = requireContext()
        val path = PrimedGunSelectedGame.getPath(context)
        selectedGame = null
        b.setupSelectedGame.isVisible = path != null
        b.setupSelectedGame.text = path?.let {
            getString(R.string.primedgun_selected_game, PrimedGunSelectedGame.displayName(it))
        }
        b.setupPlay.isEnabled = path != null
        b.setupGameOptions.isEnabled = path != null
        b.setupRevisionWarning.isVisible = false
        if (path == null)
            return

        // Parsing reads the disc header, which needs the native directories and should stay off
        // the main thread.
        AfterDirectoryInitializationRunner().runWithLifecycle(viewLifecycleOwner) {
            viewLifecycleOwner.lifecycleScope.launch {
                val game = withContext(Dispatchers.IO) { GameFile.parse(path) }
                val current = binding ?: return@launch
                if (PrimedGunSelectedGame.getPath(context) != path)
                    return@launch
                selectedGame = game
                val warning = game?.let { PrimedGunSelectedGame.warningFor(it) }
                current.setupRevisionWarning.isVisible = warning != null
                if (warning != null)
                    current.setupRevisionWarning.setText(warning)
            }
        }
    }

    private fun onGamePicked(uri: Uri) {
        val context = requireContext()
        val canonical = context.contentResolver.canonicalize(uri) ?: uri
        FileBrowserHelper.runAfterExtensionCheck(
            context, canonical, FileBrowserHelper.GAME_EXTENSIONS
        ) {
            try {
                context.contentResolver.takePersistableUriPermission(
                    canonical, Intent.FLAG_GRANT_READ_URI_PERMISSION
                )
            } catch (e: SecurityException) {
                Log.warning("[PrimedGunSetupFragment] No persistable permission for $canonical")
            }
            PrimedGunSelectedGame.setPath(context, canonical.toString())
            refreshSelectedGame()
        }
    }

    private fun play() {
        val path = PrimedGunSelectedGame.getPath(requireContext()) ?: return
        EmulationActivity.launch(requireActivity(), path, false)
    }

    private fun showGameOptions() {
        if (PrimedGunSelectedGame.getPath(requireContext()) == null) {
            showMessage(R.string.primedgun_information, getString(R.string.primedgun_no_game_selected))
            return
        }
        val game = selectedGame
        if (game == null) {
            showMessage(R.string.primedgun_error, getString(R.string.primedgun_game_unreadable))
            return
        }
        GamePropertiesDialog.newInstance(game).show(parentFragmentManager, GamePropertiesDialog.TAG)
    }

    private fun showSetupNotes() {
        MaterialAlertDialogBuilder(requireContext())
            .setTitle(R.string.primedgun_setup_notes)
            .setMessage(R.string.primedgun_setup_notes_text)
            .setPositiveButton(R.string.primedgun_close, null)
            .show()
    }

    // ---------- Transfer old memory card / settings ----------

    private fun confirmTransfer() {
        val context = requireContext()
        if (!NativeLibrary.IsUninitialized()) {
            showMessage(R.string.primedgun_transfer_title, getString(R.string.primedgun_transfer_running))
            return
        }

        MaterialAlertDialogBuilder(context)
            .setTitle(R.string.primedgun_transfer_title)
            .setMessage(
                getString(R.string.primedgun_transfer_prompt) + "\n\n" +
                    getString(R.string.primedgun_transfer_prompt_detail)
            )
            .setPositiveButton(R.string.primedgun_transfer) { _, _ ->
                requestMemoryCard.launch(arrayOf("*/*"))
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun transferMemoryCard(uri: Uri) {
        val context = requireContext()
        if (PrimedGunCannonTextures.extensionOf(uri) !in MEMORY_CARD_EXTENSIONS) {
            showMessage(
                R.string.primedgun_transfer_title,
                getString(R.string.primedgun_transfer_wrong_extension)
            )
            return
        }

        viewLifecycleOwner.lifecycleScope.launch {
            val step = withContext(Dispatchers.IO) { copyMemoryCard(context, uri) }
            if (step.error != null) {
                showMessage(R.string.primedgun_transfer_title, step.error)
                return@launch
            }
            transferReport = step.report.orEmpty()
            MaterialAlertDialogBuilder(context)
                .setTitle(R.string.primedgun_transfer_title)
                .setMessage(R.string.primedgun_transfer_import_settings_prompt)
                .setPositiveButton(R.string.primedgun_transfer_import_settings) { _, _ ->
                    requestSettingsFile.launch(arrayOf("*/*"))
                }
                .setNegativeButton(R.string.primedgun_transfer_skip) { _, _ -> finishTransfer(null) }
                .setCancelable(false)
                .show()
        }
    }

    /** Backs up the current card, then copies the picked one into its place. */
    private fun copyMemoryCard(context: Context, uri: Uri): TransferStep {
        val destination = File(PrimedGunSettings.getMemoryCardPath())
        val directory = destination.parentFile
            ?: return TransferStep(null, context.getString(R.string.primedgun_transfer_folder_failed, destination.path))
        if (!directory.isDirectory && !directory.mkdirs())
            return TransferStep(null, context.getString(R.string.primedgun_transfer_folder_failed, directory.path))

        var backup: File? = null
        if (destination.exists()) {
            val timestamp = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())
            val name = destination.name
            val dot = name.lastIndexOf('.')
            val backupName = if (dot <= 0) {
                "$name.backup-$timestamp"
            } else {
                "${name.substring(0, dot)}.backup-$timestamp${name.substring(dot)}"
            }
            backup = File(directory, backupName)
            if (!destination.renameTo(backup))
                return TransferStep(null, context.getString(R.string.primedgun_transfer_backup_failed, destination.path))
        }

        val copied = try {
            context.contentResolver.openInputStream(uri)?.use { input ->
                destination.outputStream().use { output -> input.copyTo(output) }
            } != null
        } catch (e: IOException) {
            false
        }
        if (!copied) {
            destination.delete()
            backup?.renameTo(destination)
            return TransferStep(null, context.getString(R.string.primedgun_transfer_copy_failed, destination.path))
        }

        var report = context.getString(R.string.primedgun_transfer_done, destination.path)
        if (backup != null)
            report += context.getString(R.string.primedgun_transfer_backup_done, backup.path)
        return TransferStep(report, null)
    }

    private fun finishTransfer(settingsUri: Uri?) {
        val context = requireContext()
        var message = transferReport
        transferReport = ""
        if (settingsUri == null) {
            message += getString(R.string.primedgun_transfer_settings_skipped)
        } else {
            val name = ContentHandler.getDisplayName(settingsUri)
                ?: settingsUri.lastPathSegment
                ?: settingsUri.toString()
            val imported = importSettingsFrom(context, settingsUri)
            message += getString(
                if (imported) R.string.primedgun_transfer_settings_done else R.string.primedgun_transfer_settings_failed,
                name
            )
            if (imported)
                (activity as? MainActivity)?.refreshVisibleTab()
        }
        message += getString(R.string.primedgun_transfer_footer)
        showMessage(R.string.primedgun_transfer_title, message)
    }

    // The core importer takes a file path, so the picked document is staged in the cache folder.
    private fun importSettingsFrom(context: Context, uri: Uri): Boolean {
        val staging = File(context.cacheDir, "primedgun-import-${System.currentTimeMillis()}.ini")
        return try {
            val copied = context.contentResolver.openInputStream(uri)?.use { input ->
                staging.outputStream().use { output -> input.copyTo(output) }
            } != null
            copied && PrimedGunSettings.importSettings(staging.absolutePath)
        } catch (e: IOException) {
            false
        } finally {
            staging.delete()
        }
    }

    private fun showMessage(@StringRes title: Int, message: String) {
        MaterialAlertDialogBuilder(requireContext())
            .setTitle(title)
            .setMessage(message)
            .setPositiveButton(R.string.ok, null)
            .show()
    }
}
