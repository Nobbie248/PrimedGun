// Copyright 2026 PrimedGun Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.primedgun.ui

import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.widget.Button
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.appbar.MaterialToolbar
import com.google.android.material.tabs.TabLayout
import org.dolphinemu.dolphinemu.R
import org.dolphinemu.dolphinemu.features.primedgun.model.PrimedGunSettings

/**
 * The PrimedGun settings window for the Android/Quest build, standing in for the Qt launcher's
 * tabbed main window. Tabs swap fragments rather than riding a pager: there is no swipe gesture
 * to preserve on a headset panel, and it keeps the dependency list unchanged.
 */
class PrimedGunActivity : AppCompatActivity() {

    companion object {
        fun launch(context: Context) {
            context.startActivity(Intent(context, PrimedGunActivity::class.java))
        }
    }

    private val tabs = PrimedGunTabs.Tab.entries

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_primedgun)

        val toolbar = findViewById<MaterialToolbar>(R.id.primedgun_toolbar)
        setSupportActionBar(toolbar)
        supportActionBar?.setDisplayHomeAsUpEnabled(true)
        toolbar.setNavigationOnClickListener { finish() }

        val tabLayout = findViewById<TabLayout>(R.id.primedgun_tabs)
        tabs.forEach { tab ->
            tabLayout.addTab(tabLayout.newTab().setText(getString(tab.titleId)).setTag(tab))
        }
        tabLayout.addOnTabSelectedListener(object : TabLayout.OnTabSelectedListener {
            override fun onTabSelected(tab: TabLayout.Tab) = showTab(tabs[tab.position])
            override fun onTabUnselected(tab: TabLayout.Tab) = Unit
            override fun onTabReselected(tab: TabLayout.Tab) = Unit
        })

        findViewById<Button>(R.id.primedgun_save_settings).setOnClickListener { save() }

        if (savedInstanceState == null)
            showTab(tabs.first())
    }

    private fun showTab(tab: PrimedGunTabs.Tab) {
        supportFragmentManager.beginTransaction()
            .replace(R.id.primedgun_content, PrimedGunSettingsFragment.newInstance(tab), tab.name)
            .commit()
    }

    private fun save() {
        val saved = PrimedGunSettings.save()
        val message =
            if (saved) R.string.primedgun_settings_saved else R.string.primedgun_settings_save_failed
        Toast.makeText(this, message, Toast.LENGTH_SHORT).show()
    }

    override fun onPause() {
        super.onPause()
        // Edits already reached the running mod; persist them so leaving the screen without using
        // Save Settings does not silently discard the session's calibration.
        PrimedGunSettings.save()
    }
}
