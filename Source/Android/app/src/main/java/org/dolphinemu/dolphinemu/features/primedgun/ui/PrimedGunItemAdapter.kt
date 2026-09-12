// Copyright 2026 PrimedGun Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.primedgun.ui

import android.annotation.SuppressLint
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.TextView
import androidx.appcompat.app.AlertDialog
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.materialswitch.MaterialSwitch
import com.google.android.material.slider.Slider
import org.dolphinemu.dolphinemu.R
import java.util.Locale
import kotlin.math.roundToInt

/**
 * Renders [PrimedGunItem] rows. Every edit is written to the runtime immediately, matching the Qt
 * launcher, which also applies on change and persists only when Save Settings is pressed.
 */
class PrimedGunItemAdapter(private val items: List<PrimedGunItem>) :
    RecyclerView.Adapter<PrimedGunItemAdapter.ViewHolder>() {

    companion object {
        private const val TYPE_HEADER = 0
        private const val TYPE_NOTE = 1
        private const val TYPE_ACTION = 2
        private const val TYPE_SWITCH = 3
        private const val TYPE_SLIDER = 4
        private const val TYPE_CHOICE = 5
        private const val TYPE_TOGGLE2 = 6
    }

    class ViewHolder(view: View) : RecyclerView.ViewHolder(view)

    override fun getItemCount(): Int = items.size

    override fun getItemViewType(position: Int): Int = when (items[position]) {
        is PrimedGunItem.Header -> TYPE_HEADER
        is PrimedGunItem.Note -> TYPE_NOTE
        is PrimedGunItem.Action -> TYPE_ACTION
        is PrimedGunItem.Switch -> TYPE_SWITCH
        is PrimedGunItem.Slider -> TYPE_SLIDER
        is PrimedGunItem.Choice -> TYPE_CHOICE
        is PrimedGunItem.Toggle2 -> TYPE_TOGGLE2
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): ViewHolder {
        val layout = when (viewType) {
            TYPE_HEADER -> R.layout.list_item_primedgun_header
            TYPE_NOTE -> R.layout.list_item_primedgun_note
            TYPE_ACTION -> R.layout.list_item_primedgun_action
            TYPE_SWITCH -> R.layout.list_item_primedgun_switch
            TYPE_SLIDER -> R.layout.list_item_primedgun_slider
            TYPE_CHOICE -> R.layout.list_item_primedgun_choice
            TYPE_TOGGLE2 -> R.layout.list_item_primedgun_toggle2
            else -> throw IllegalArgumentException("Unknown PrimedGun item type $viewType")
        }
        return ViewHolder(LayoutInflater.from(parent.context).inflate(layout, parent, false))
    }

    override fun onBindViewHolder(holder: ViewHolder, position: Int) {
        when (val item = items[position]) {
            is PrimedGunItem.Header -> bindHeader(holder, item)
            is PrimedGunItem.Note -> bindNote(holder, item)
            is PrimedGunItem.Action -> bindAction(holder, item)
            is PrimedGunItem.Switch -> bindSwitch(holder, item)
            is PrimedGunItem.Slider -> bindSlider(holder, item)
            is PrimedGunItem.Choice -> bindChoice(holder, item)
            is PrimedGunItem.Toggle2 -> bindToggle2(holder, item)
        }
    }

    /** Re-reads every row from the runtime. Used after an action button rewrites many settings. */
    @SuppressLint("NotifyDataSetChanged")
    fun refresh() = notifyDataSetChanged()

    private fun bindHeader(holder: ViewHolder, item: PrimedGunItem.Header) {
        holder.itemView.findViewById<TextView>(R.id.primedgun_header_title).text = item.title
    }

    private fun bindNote(holder: ViewHolder, item: PrimedGunItem.Note) {
        holder.itemView.findViewById<TextView>(R.id.primedgun_note_text).text = item.text
    }

    private fun bindAction(holder: ViewHolder, item: PrimedGunItem.Action) {
        val button = holder.itemView.findViewById<Button>(R.id.primedgun_action_button)
        button.text = item.title
        button.setOnClickListener {
            item.onClick()
            // Action buttons rewrite whole sections, so every visible row may be stale now.
            refresh()
        }
    }

    private fun bindSwitch(holder: ViewHolder, item: PrimedGunItem.Switch) {
        val title = holder.itemView.findViewById<TextView>(R.id.primedgun_switch_title)
        val description = holder.itemView.findViewById<TextView>(R.id.primedgun_switch_description)
        val switch = holder.itemView.findViewById<MaterialSwitch>(R.id.primedgun_switch)

        title.text = item.title
        if (item.description.isNullOrEmpty()) {
            description.visibility = View.GONE
        } else {
            description.visibility = View.VISIBLE
            description.text = item.description
        }

        // Clear before assigning: a recycled row would otherwise write the previous row's value.
        switch.setOnCheckedChangeListener(null)
        switch.isChecked = item.get()
        switch.setOnCheckedChangeListener { _, checked -> item.set(checked) }
        holder.itemView.setOnClickListener { switch.isChecked = !switch.isChecked }
    }

    private fun bindSlider(holder: ViewHolder, item: PrimedGunItem.Slider) {
        val title = holder.itemView.findViewById<TextView>(R.id.primedgun_slider_title)
        val value = holder.itemView.findViewById<TextView>(R.id.primedgun_slider_value)
        val slider = holder.itemView.findViewById<Slider>(R.id.primedgun_slider)
        val minus = holder.itemView.findViewById<Button>(R.id.primedgun_slider_minus)
        val plus = holder.itemView.findViewById<Button>(R.id.primedgun_slider_plus)

        title.text = item.title

        // The slider runs in whole step units, the way MainWindow scales its QSlider. Handing
        // Material a fractional stepSize risks its "not divisible" validation on float rounding.
        val steps = ((item.max - item.min) / item.step).roundToInt()
        val toUnits = { v: Float -> ((v - item.min) / item.step).roundToInt().coerceIn(0, steps) }
        val toValue = { units: Float -> item.min + units * item.step }

        slider.clearOnChangeListeners()
        slider.valueFrom = 0f
        slider.valueTo = steps.toFloat()
        slider.stepSize = 1f
        slider.value = toUnits(item.get()).toFloat()
        value.text = format(toValue(slider.value), item.decimals)

        slider.addOnChangeListener { _, units, _ ->
            val newValue = toValue(units)
            value.text = format(newValue, item.decimals)
            item.set(newValue)
        }
        minus.setOnClickListener {
            slider.value = (slider.value - 1f).coerceAtLeast(0f)
        }
        plus.setOnClickListener {
            slider.value = (slider.value + 1f).coerceAtMost(steps.toFloat())
        }
    }

    private fun bindChoice(holder: ViewHolder, item: PrimedGunItem.Choice) {
        val title = holder.itemView.findViewById<TextView>(R.id.primedgun_choice_title)
        val value = holder.itemView.findViewById<TextView>(R.id.primedgun_choice_value)

        title.text = item.title
        // Read on every use rather than caching: the dialog can be reopened without a rebind.
        val selectedIndex = { item.values.indexOf(item.get()).takeIf { it >= 0 } ?: 0 }
        value.text = item.labels[selectedIndex()]

        val showDialog = View.OnClickListener { view ->
            AlertDialog.Builder(view.context)
                .setTitle(item.title)
                .setSingleChoiceItems(item.labels.toTypedArray(), selectedIndex()) { dialog, which ->
                    item.set(item.values[which])
                    value.text = item.labels[which]
                    dialog.dismiss()
                }
                .show()
        }
        holder.itemView.setOnClickListener(showDialog)
        value.setOnClickListener(showDialog)
    }

    private fun bindToggle2(holder: ViewHolder, item: PrimedGunItem.Toggle2) {
        val title = holder.itemView.findViewById<TextView>(R.id.primedgun_toggle2_title)
        val group = holder.itemView.findViewById<RadioGroup>(R.id.primedgun_toggle2_group)
        val optionA = holder.itemView.findViewById<RadioButton>(R.id.primedgun_toggle2_a)
        val optionB = holder.itemView.findViewById<RadioButton>(R.id.primedgun_toggle2_b)

        if (item.title.isNullOrEmpty()) {
            title.visibility = View.GONE
        } else {
            title.visibility = View.VISIBLE
            title.text = item.title
        }
        optionA.text = item.labelA
        optionB.text = item.labelB

        group.setOnCheckedChangeListener(null)
        group.check(if (item.get()) optionB.id else optionA.id)
        group.setOnCheckedChangeListener { _, checkedId -> item.set(checkedId == optionB.id) }
    }

    private fun format(value: Float, decimals: Int): String =
        String.format(Locale.getDefault(), "%.${decimals}f", value)
}
