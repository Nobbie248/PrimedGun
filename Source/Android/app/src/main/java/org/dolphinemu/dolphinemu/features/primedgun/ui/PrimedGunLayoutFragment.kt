// Copyright 2026 PrimedGun Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.primedgun.ui

import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.fragment.app.Fragment
import org.dolphinemu.dolphinemu.R

/** The Layout tab: the controller binding diagram. */
class PrimedGunLayoutFragment : Fragment() {
    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View = inflater.inflate(R.layout.fragment_primedgun_layout, container, false)
}
