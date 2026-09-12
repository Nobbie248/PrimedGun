// Copyright 2026 PrimedGun Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.primedgun.ui

import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.fragment.app.Fragment
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import org.dolphinemu.dolphinemu.R

/** One tab of the PrimedGun settings UI. */
class PrimedGunSettingsFragment : Fragment() {

    companion object {
        private const val ARG_TAB = "tab"

        fun newInstance(tab: PrimedGunTabs.Tab): PrimedGunSettingsFragment =
            PrimedGunSettingsFragment().apply {
                arguments = Bundle().apply { putString(ARG_TAB, tab.name) }
            }
    }

    private var adapter: PrimedGunItemAdapter? = null

    private val tab: PrimedGunTabs.Tab
        get() = PrimedGunTabs.Tab.valueOf(
            requireArguments().getString(ARG_TAB) ?: PrimedGunTabs.Tab.CONTROLLER.name
        )

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View = inflater.inflate(R.layout.fragment_primedgun_settings, container, false)

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        val list = view.findViewById<RecyclerView>(R.id.primedgun_settings_list)
        val itemAdapter = PrimedGunItemAdapter(PrimedGunTabs.itemsFor(requireContext(), tab))
        adapter = itemAdapter
        list.layoutManager = LinearLayoutManager(requireContext())
        list.adapter = itemAdapter
    }

    override fun onResume() {
        super.onResume()
        // The in-headset menu writes the same settings while the game runs, so re-read on return
        // rather than trusting what was on screen when this tab lost focus.
        adapter?.refresh()
    }

    override fun onDestroyView() {
        super.onDestroyView()
        adapter = null
    }
}
