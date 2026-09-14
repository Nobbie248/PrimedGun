// Copyright 2026 PrimedGun Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.primedgun.ui

import android.net.Uri
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.ImageView
import android.widget.RadioButton
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.annotation.StringRes
import androidx.core.content.ContextCompat
import androidx.core.view.isVisible
import androidx.fragment.app.Fragment
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import org.dolphinemu.dolphinemu.R
import org.dolphinemu.dolphinemu.databinding.FragmentPrimedgunCannonTexturesBinding
import org.dolphinemu.dolphinemu.features.primedgun.model.PrimedGunCannonTextures
import org.dolphinemu.dolphinemu.features.primedgun.model.PrimedGunCannonTextures.Outcome
import org.dolphinemu.dolphinemu.utils.AfterDirectoryInitializationRunner
import java.io.File

/**
 * The Cannon Textures tab, following the Qt window: pick a slot, review and import its files,
 * then Apply. The desktop "Open ..." buttons can only show the folder path here, since a headset
 * has no file manager to hand the folder to.
 */
class PrimedGunCannonTexturesFragment : Fragment(), PrimedGunRefreshable {

    private class TextureRow(
        val preview: ImageView,
        val noPreview: TextView,
        val path: TextView,
        val import: Button
    )

    private var binding: FragmentPrimedgunCannonTexturesBinding? = null
    private val rows = ArrayList<TextureRow>()
    private val radios = ArrayList<RadioButton>()
    private var selectedSlot = PrimedGunCannonTextures.SLOT_DEFAULT
    private var importIndex = -1
    private var directoriesReady = false

    private val requestTexture = registerForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri: Uri? ->
        if (uri != null)
            importTexture(uri)
    }

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        val inflated = FragmentPrimedgunCannonTexturesBinding.inflate(inflater, container, false)
        binding = inflated
        return inflated.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        val b = binding ?: return
        val context = requireContext()
        val inflater = LayoutInflater.from(context)

        radios.clear()
        for (slot in 0 until PrimedGunCannonTextures.SLOT_COUNT) {
            val radio = RadioButton(context).apply {
                id = View.generateViewId()
                text = PrimedGunCannonTextures.slotName(context, slot)
                tag = slot
                setTextColor(ContextCompat.getColor(context, R.color.primedgun_text))
                minHeight = resources.getDimensionPixelSize(R.dimen.primedgun_touch_target)
                setPadding(0, 0, resources.getDimensionPixelSize(R.dimen.spacing_medlarge), 0)
            }
            b.cannonSlotGroup.addView(radio)
            radios.add(radio)
        }
        b.cannonSlotGroup.setOnCheckedChangeListener { group, checkedId ->
            val radio = group.findViewById<RadioButton>(checkedId) ?: return@setOnCheckedChangeListener
            val slot = radio.tag as Int
            if (slot == selectedSlot)
                return@setOnCheckedChangeListener
            selectedSlot = slot
            refreshRows()
            setStatus(
                if (slot == PrimedGunCannonTextures.SLOT_DEFAULT) {
                    getString(R.string.primedgun_cannon_default_selected)
                } else {
                    getString(R.string.primedgun_cannon_slot_selected, slotName(slot))
                }
            )
        }

        rows.clear()
        for (index in PrimedGunCannonTextures.TEXTURE_NAMES.indices) {
            val row = inflater.inflate(
                R.layout.list_item_primedgun_cannon_texture, b.cannonTextureRows, false
            )
            row.findViewById<TextView>(R.id.cannon_texture_label)
                .setText(PrimedGunCannonTextures.TEXTURE_LABELS[index])
            val import = row.findViewById<Button>(R.id.cannon_texture_import)
            import.setOnClickListener { onImportClicked(index) }
            b.cannonTextureRows.addView(row)
            rows.add(
                TextureRow(
                    row.findViewById(R.id.cannon_texture_preview),
                    row.findViewById(R.id.cannon_texture_no_preview),
                    row.findViewById(R.id.cannon_texture_path),
                    import
                )
            )
        }

        b.cannonApply.setOnClickListener { applySlot() }
        b.cannonRemoveShine.setOnClickListener { removeShine() }
        b.cannonRestoreShine.setOnClickListener { restoreShine() }
        b.cannonSlotFolder.setOnClickListener {
            showPath(R.string.primedgun_cannon_slot_folder_message, PrimedGunCannonTextures.libraryDir())
        }
        b.cannonActivePack.setOnClickListener {
            showPath(R.string.primedgun_cannon_active_pack_message, PrimedGunCannonTextures.packDir())
        }

        // The slot folders live in the user directory, which is only known once initialisation
        // has run; the launcher normally waits for that behind its splash screen.
        AfterDirectoryInitializationRunner().runWithLifecycle(viewLifecycleOwner) {
            directoriesReady = true
            refresh()
        }
    }

    override fun onDestroyView() {
        super.onDestroyView()
        binding = null
        rows.clear()
        radios.clear()
    }

    /** Re-reads the applied slot: after Reset All, an import, or a change from the VR menu. */
    override fun refresh() {
        if (!directoriesReady || binding == null)
            return
        selectedSlot = PrimedGunCannonTextures.currentSlot()
        radios.firstOrNull { it.tag == selectedSlot }?.isChecked = true
        refreshRows()
        setStatus(
            if (selectedSlot == PrimedGunCannonTextures.SLOT_DEFAULT) {
                getString(R.string.primedgun_cannon_default_active)
            } else {
                getString(R.string.primedgun_cannon_slot_active, slotName(selectedSlot))
            }
        )
    }

    private fun refreshRows() {
        if (!directoriesReady)
            return
        val custom = selectedSlot == PrimedGunCannonTextures.SLOT_CUSTOM
        for ((index, row) in rows.withIndex()) {
            row.import.isVisible = custom
            if (selectedSlot == PrimedGunCannonTextures.SLOT_DEFAULT) {
                row.path.setText(R.string.primedgun_cannon_default_no_override)
                setPreview(row, PrimedGunCannonTextures.defaultPreviewFile(index))
                continue
            }
            val source = PrimedGunCannonTextures.sourceFile(selectedSlot, index)
            row.path.text = source?.path ?: getString(R.string.primedgun_cannon_no_texture_imported)
            setPreview(row, source)
        }
    }

    private fun setPreview(row: TextureRow, file: File?) {
        val bitmap = PrimedGunCannonTextures.loadPreview(file)
        row.preview.setImageBitmap(bitmap)
        row.noPreview.isVisible = bitmap == null
    }

    private fun applySlot() {
        if (!directoriesReady)
            return
        val slot = selectedSlot
        if (!PrimedGunCannonTextures.applySlot(slot)) {
            showMessage(getString(R.string.primedgun_cannon_apply_failed, slotName(slot)))
            return
        }
        refreshRows()
        setStatus(
            if (slot == PrimedGunCannonTextures.SLOT_DEFAULT) {
                getString(R.string.primedgun_cannon_default_applied)
            } else {
                getString(R.string.primedgun_cannon_slot_applied, slotName(slot))
            }
        )
    }

    private fun onImportClicked(index: Int) {
        if (selectedSlot != PrimedGunCannonTextures.SLOT_CUSTOM) {
            showMessage(getString(R.string.primedgun_cannon_choose_custom))
            return
        }
        importIndex = index
        requestTexture.launch(arrayOf("*/*"))
    }

    private fun importTexture(uri: Uri) {
        val index = importIndex
        importIndex = -1
        if (index < 0 || !directoriesReady)
            return
        when (val outcome = PrimedGunCannonTextures.importTexture(requireContext(), selectedSlot, index, uri)) {
            is Outcome.Failure -> showMessage(outcome.message)
            is Outcome.Success -> {
                refreshRows()
                setStatus(getString(R.string.primedgun_cannon_imported, slotName(selectedSlot)))
            }
        }
    }

    private fun removeShine() {
        if (!directoriesReady)
            return
        val slot = selectedSlot
        if (slot <= PrimedGunCannonTextures.SLOT_DEFAULT) {
            showMessage(getString(R.string.primedgun_cannon_choose_slot_remove_shine))
            return
        }
        when (val outcome = PrimedGunCannonTextures.removeShine(requireContext(), slot)) {
            is Outcome.Failure -> showMessage(outcome.message)
            is Outcome.Success -> applyAfterShineChange(
                slot, getString(R.string.primedgun_cannon_remove_shine_applied, slotName(slot))
            )
        }
    }

    private fun restoreShine() {
        if (!directoriesReady)
            return
        val slot = selectedSlot
        if (slot <= PrimedGunCannonTextures.SLOT_DEFAULT) {
            showMessage(getString(R.string.primedgun_cannon_choose_slot_restore_shine))
            return
        }
        when (val outcome = PrimedGunCannonTextures.restoreShine(requireContext(), slot)) {
            is Outcome.Failure -> showMessage(outcome.message)
            is Outcome.Success -> applyAfterShineChange(
                slot, getString(R.string.primedgun_cannon_restore_shine_applied, slotName(slot))
            )
        }
    }

    private fun applyAfterShineChange(slot: Int, successStatus: String) {
        if (!PrimedGunCannonTextures.applySlot(slot)) {
            showMessage(getString(R.string.primedgun_cannon_apply_failed, slotName(slot)))
            return
        }
        refreshRows()
        setStatus(successStatus)
    }

    private fun showPath(@StringRes message: Int, directory: File) {
        if (!directoriesReady)
            return
        showMessage(getString(message, directory.path))
    }

    private fun slotName(slot: Int): String = PrimedGunCannonTextures.slotName(requireContext(), slot)

    private fun setStatus(text: String) {
        binding?.cannonStatus?.text = text
    }

    private fun showMessage(message: String) {
        MaterialAlertDialogBuilder(requireContext())
            .setTitle(R.string.primedgun_tab_cannon_textures)
            .setMessage(message)
            .setPositiveButton(R.string.ok, null)
            .show()
    }
}
