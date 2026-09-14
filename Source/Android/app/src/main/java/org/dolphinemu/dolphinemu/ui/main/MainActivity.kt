// Copyright 2026 PrimedGun Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.ui.main

import android.content.pm.PackageManager
import android.os.Bundle
import android.view.View
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.app.AppCompatDelegate
import androidx.core.splashscreen.SplashScreen.Companion.installSplashScreen
import androidx.core.view.ViewCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.fragment.app.Fragment
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.tabs.TabLayout
import org.dolphinemu.dolphinemu.NativeLibrary
import org.dolphinemu.dolphinemu.R
import org.dolphinemu.dolphinemu.databinding.ActivityMainBinding
import org.dolphinemu.dolphinemu.features.primedgun.model.PrimedGunSettings
import org.dolphinemu.dolphinemu.features.primedgun.ui.PrimedGunCannonTexturesFragment
import org.dolphinemu.dolphinemu.features.primedgun.ui.PrimedGunLayoutFragment
import org.dolphinemu.dolphinemu.features.primedgun.ui.PrimedGunRefreshable
import org.dolphinemu.dolphinemu.features.primedgun.ui.PrimedGunSettingsFragment
import org.dolphinemu.dolphinemu.features.primedgun.ui.PrimedGunSetupFragment
import org.dolphinemu.dolphinemu.features.primedgun.ui.PrimedGunTabs
import org.dolphinemu.dolphinemu.features.settings.model.QuestVrSettings
import org.dolphinemu.dolphinemu.utils.AfterDirectoryInitializationRunner
import org.dolphinemu.dolphinemu.utils.DirectoryInitialization
import org.dolphinemu.dolphinemu.utils.Log
import org.dolphinemu.dolphinemu.utils.PermissionsHandler
import org.dolphinemu.dolphinemu.utils.StartupHandler

/**
 * The PrimedGun launcher window: the Android counterpart of the Qt MainWindow's tabbed stack.
 *
 * Tabs swap fragments rather than riding a pager: there is no swipe gesture to preserve on a
 * headset panel. Dolphin's own game grid lives in [GameLibraryActivity], reachable from the
 * Dolphin Config tab, the way the Qt window keeps Dolphin's menus available beside its own.
 */
class MainActivity : AppCompatActivity() {

    companion object {
        /** Optional intent extra: the [PrimedGunTabs.Tab] name to open instead of Setup. */
        const val EXTRA_TAB = "tab"
    }

    private lateinit var binding: ActivityMainBinding
    private val tabs = PrimedGunTabs.Tab.entries
    private var staleQuestStopRequested = false

    override fun onCreate(savedInstanceState: Bundle?) {
        installSplashScreen().setKeepOnScreenCondition { !DirectoryInitialization.areDolphinDirectoriesReady() }

        // The launcher keeps the Qt window's dark palette whatever Dolphin theme was chosen for
        // the emulator's own screens, so Material widgets must not pick day-mode tokens.
        delegate.localNightMode = AppCompatDelegate.MODE_NIGHT_YES

        super.onCreate(savedInstanceState)

        stopStaleQuestEmulation()

        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)
        setInsets()

        val version = versionLabel()
        binding.mainToolbar.title = getString(R.string.primedgun_window_title, version)
        binding.mainCredit.text = getString(R.string.primedgun_credit, version)

        setupTabs(savedInstanceState)
        binding.mainResetAll.setOnClickListener { confirmResetAll() }
        binding.mainSaveSettings.setOnClickListener { saveSettings() }

        // Ask the user to grant write permission if relevant and not already granted
        if (DirectoryInitialization.isWaitingForWriteAccess(this)) {
            PermissionsHandler.requestWritePermission(this)
        }

        // Stuff in this block only happens when this activity is newly created (i.e. not a rotation)
        if (savedInstanceState == null) {
            StartupHandler.HandleInit(this)
        }
    }

    override fun onResume() {
        super.onResume()
        if (DirectoryInitialization.shouldStart(this)) {
            DirectoryInitialization.start(this)
        }
        stopStaleQuestEmulation()
    }

    override fun onPause() {
        super.onPause()
        // Edits already reached the running mod; persist them so leaving the window without using
        // Save Settings does not silently discard the session's calibration.
        if (DirectoryInitialization.areDolphinDirectoriesReady()) {
            PrimedGunSettings.save()
        }
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<String>,
        grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == PermissionsHandler.REQUEST_CODE_WRITE_PERMISSION) {
            if (grantResults[0] == PackageManager.PERMISSION_DENIED) {
                PermissionsHandler.setWritePermissionDenied()
            }
            DirectoryInitialization.start(this)
        }
    }

    /** Asks the visible tab to re-read the runtime after an action rewrote many settings. */
    fun refreshVisibleTab() {
        (supportFragmentManager.findFragmentById(R.id.main_content) as? PrimedGunRefreshable)?.refresh()
    }

    private fun stopStaleQuestEmulation() {
        if (!staleQuestStopRequested && QuestVrSettings.isQuestBuild() && NativeLibrary.IsRunning()) {
            staleQuestStopRequested = true
            Log.warning("[MainActivity] Stopping stale Quest emulation before showing main UI.")
            NativeLibrary.StopEmulation()
        }
    }

    private fun versionLabel(): String {
        val description = PrimedGunSettings.getVersion()
        return if (description.startsWith("v", ignoreCase = true)) description else "v$description"
    }

    private fun setupTabs(savedInstanceState: Bundle?) {
        val tabLayout = binding.mainTabs
        tabs.forEach { tab -> tabLayout.addTab(tabLayout.newTab().setText(tab.titleId)) }

        // After a recreation the fragment manager already holds the open tab's fragment, tagged
        // with the tab name; only the tab strip has to catch up.
        val restored = if (savedInstanceState == null) {
            null
        } else {
            tabs.firstOrNull { supportFragmentManager.findFragmentByTag(it.name) != null }
        }
        if (restored != null) {
            tabLayout.selectTab(tabLayout.getTabAt(tabs.indexOf(restored)))
        } else {
            val requested = intent.getStringExtra(EXTRA_TAB)
            val initial = tabs.firstOrNull { it.name == requested } ?: tabs.first()
            tabLayout.selectTab(tabLayout.getTabAt(tabs.indexOf(initial)))
            showTab(initial)
        }

        tabLayout.addOnTabSelectedListener(object : TabLayout.OnTabSelectedListener {
            override fun onTabSelected(tab: TabLayout.Tab) = showTab(tabs[tab.position])
            override fun onTabUnselected(tab: TabLayout.Tab) = Unit
            override fun onTabReselected(tab: TabLayout.Tab) = Unit
        })
    }

    private fun showTab(tab: PrimedGunTabs.Tab) {
        supportFragmentManager.beginTransaction()
            .replace(R.id.main_content, createFragment(tab), tab.name)
            .commit()
    }

    private fun createFragment(tab: PrimedGunTabs.Tab): Fragment = when (tab) {
        PrimedGunTabs.Tab.SETUP -> PrimedGunSetupFragment()
        PrimedGunTabs.Tab.CANNON_TEXTURES -> PrimedGunCannonTexturesFragment()
        PrimedGunTabs.Tab.LAYOUT -> PrimedGunLayoutFragment()
        PrimedGunTabs.Tab.CONTROLLER,
        PrimedGunTabs.Tab.CALIBRATION,
        PrimedGunTabs.Tab.DOLPHIN_CONFIG -> PrimedGunSettingsFragment.newInstance(tab)
    }

    private fun confirmResetAll() {
        // The desktop button resets immediately; a headset panel is easier to mis-tap, so the
        // in-headset menu's confirmation step is kept here.
        MaterialAlertDialogBuilder(this)
            .setTitle(R.string.primedgun_reset_all)
            .setMessage(R.string.primedgun_reset_all_confirmation)
            .setPositiveButton(R.string.yes) { _, _ -> resetAll() }
            .setNegativeButton(R.string.no, null)
            .show()
    }

    private fun resetAll() {
        AfterDirectoryInitializationRunner().runWithLifecycle(this) {
            PrimedGunSettings.resetAll()
            refreshVisibleTab()
            Toast.makeText(this, R.string.primedgun_reset_all_done, Toast.LENGTH_SHORT).show()
        }
    }

    private fun saveSettings() {
        AfterDirectoryInitializationRunner().runWithLifecycle(this) {
            val saved = PrimedGunSettings.save()
            val message =
                if (saved) R.string.primedgun_settings_saved else R.string.primedgun_settings_save_failed
            Toast.makeText(this, message, Toast.LENGTH_SHORT).show()
        }
    }

    private fun setInsets() {
        WindowCompat.getInsetsController(window, window.decorView).apply {
            isAppearanceLightStatusBars = false
            isAppearanceLightNavigationBars = false
        }
        ViewCompat.setOnApplyWindowInsetsListener(binding.root) { view: View, windowInsets: WindowInsetsCompat ->
            val insets = windowInsets.getInsets(
                WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout()
            )
            view.setPadding(insets.left, insets.top, insets.right, insets.bottom)
            WindowInsetsCompat.CONSUMED
        }
    }
}
