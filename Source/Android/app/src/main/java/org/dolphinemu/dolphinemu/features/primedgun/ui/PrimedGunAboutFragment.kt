// Copyright 2026 PrimedGun Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.primedgun.ui

import android.content.ActivityNotFoundException
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.Toast
import androidx.core.view.isVisible
import androidx.fragment.app.Fragment
import org.dolphinemu.dolphinemu.BuildConfig
import org.dolphinemu.dolphinemu.R
import org.dolphinemu.dolphinemu.databinding.FragmentPrimedgunAboutBinding
import org.dolphinemu.dolphinemu.features.primedgun.model.PrimedGunSettings
import org.dolphinemu.dolphinemu.utils.AfterDirectoryInitializationRunner
import org.dolphinemu.dolphinemu.utils.DirectoryInitialization

/**
 * The About tab: version and build details, tips for playing on Quest, links and credits.
 *
 * This tab exists only on Android. The Qt window shows its version in the title bar and leaves
 * the credits to the README, which the credits here mirror.
 */
class PrimedGunAboutFragment : Fragment() {

    companion object {
        private const val GITHUB_URL = "https://github.com/Nobbie248/PrimedGun"
        private const val DISCORD_URL = "https://discord.gg/GdmffzCTrh"
    }

    private var binding: FragmentPrimedgunAboutBinding? = null

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        val inflated = FragmentPrimedgunAboutBinding.inflate(inflater, container, false)
        binding = inflated
        return inflated.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        val b = binding ?: return
        b.aboutVersion.text =
            getString(R.string.primedgun_about_version, PrimedGunSettings.getVersionLabel())
        b.aboutBuild.text = getString(
            R.string.primedgun_about_build,
            BuildConfig.FLAVOR,
            BuildConfig.BUILD_TYPE,
            BuildConfig.GIT_HASH.take(10)
        )
        b.aboutGithub.setOnClickListener { openLink(GITHUB_URL) }
        b.aboutDiscord.setOnClickListener { openLink(DISCORD_URL) }

        // The user folder is only known once directory initialization has run.
        AfterDirectoryInitializationRunner().runWithLifecycle(viewLifecycleOwner) {
            val current = binding ?: return@runWithLifecycle
            current.aboutUserFolder.text = getString(
                R.string.primedgun_about_user_folder,
                DirectoryInitialization.getUserDirectory()
            )
            current.aboutUserFolder.isVisible = true
        }
    }

    override fun onDestroyView() {
        super.onDestroyView()
        binding = null
    }

    private fun openLink(url: String) {
        try {
            startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)))
        } catch (e: ActivityNotFoundException) {
            Toast.makeText(
                requireContext(),
                getString(R.string.primedgun_about_no_browser, url),
                Toast.LENGTH_LONG
            ).show()
        }
    }
}
