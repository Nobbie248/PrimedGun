// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.settings.model

class AdHocFloatSetting(
    private val file: String,
    private val section: String,
    private val key: String,
    private val defaultValue: Float
) : AbstractFloatSetting {
    init {
        require(
            NativeConfig.isSettingSaveable(
                file,
                section,
                key
            )
        ) { "File/section/key is unknown or legacy" }
    }

    override val isOverridden: Boolean
        get() = NativeConfig.isOverridden(file, section, key)

    override val isRuntimeEditable: Boolean = true

    override fun delete(settings: Settings): Boolean {
        return NativeConfig.deleteKey(settings.writeLayer, file, section, key)
    }

    override val float: Float
        get() = NativeConfig.getFloat(NativeConfig.LAYER_ACTIVE, file, section, key, defaultValue)

    override fun setFloat(settings: Settings, newValue: Float) {
        NativeConfig.setFloat(settings.writeLayer, file, section, key, newValue)
    }
}
