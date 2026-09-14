// Copyright 2026 PrimedGun Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.primedgun.model

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.net.Uri
import org.dolphinemu.dolphinemu.R
import org.dolphinemu.dolphinemu.utils.ContentHandler
import org.dolphinemu.dolphinemu.utils.DirectoryInitialization
import org.dolphinemu.dolphinemu.utils.FileBrowserHelper
import java.io.File
import java.io.IOException
import java.util.Locale

/**
 * The cannon texture library the Cannon Textures tab manages.
 *
 * Layout and file names follow the Qt window's PrimedGunCannon* helpers so a slot folder copied
 * between a PC install and the headset keeps working. Slot folders live under
 * User/Load/PrimedGun/CannonTextures; applying a slot is done by the core (see
 * [PrimedGunSettings.applyCannonTextureSlot]), which copies the slot's files into the managed
 * 000_PrimedGunCannon texture pack. This object only edits the slot folders and reads previews.
 */
object PrimedGunCannonTextures {
    val TEXTURE_NAMES = listOf(
        "tex1_128x128_m_3c6ded49d64d30f2_14",
        "tex1_128x128_m_bec6d78ea7dd739e_14",
        "tex1_64x64_m_c7625e7ecd9cd5c2_14"
    )
    val TEXTURE_LABELS = listOf(
        R.string.primedgun_cannon_texture_a,
        R.string.primedgun_cannon_texture_b,
        R.string.primedgun_cannon_shine_mask
    )
    const val SHEEN_INDEX = 2
    const val SLOT_DEFAULT = 0
    const val SLOT_CUSTOM = 5
    const val SLOT_COUNT = 6

    private val EXTENSIONS = listOf("dds", "png")
    private const val DDS_FOURCC_DXT1 = 0x31545844

    sealed class Outcome {
        class Success(val file: File) : Outcome()
        class Failure(val message: String) : Outcome()
    }

    fun libraryDir(): File =
        File(DirectoryInitialization.getUserDirectory(), "Load/PrimedGun/CannonTextures")

    fun packDir(): File =
        File(DirectoryInitialization.getUserDirectory(), "Load/Textures/000_PrimedGunCannon")

    fun slotDir(slot: Int): File = File(libraryDir(), "slot_$slot")

    fun slotName(context: Context, slot: Int): String = when (slot) {
        SLOT_DEFAULT -> context.getString(R.string.primedgun_cannon_slot_default)
        SLOT_CUSTOM -> context.getString(R.string.primedgun_cannon_slot_custom)
        else -> context.getString(R.string.primedgun_cannon_slot_n, slot)
    }

    /**
     * Makes [slot] the active cannon textures: the core copies the slot folder into the managed
     * pack and refreshes the texture cache. Returns false when the slot holds no textures.
     */
    fun applySlot(slot: Int): Boolean = PrimedGunSettings.applyCannonTextureSlot(slot)

    /** The slot the core last applied, which is also what the in-headset menu shows. */
    fun currentSlot(): Int =
        PrimedGunSettings.getInt(PrimedGunSettings.KEY_CANNON_TEXTURE_SLOT, SLOT_DEFAULT)
            .coerceIn(SLOT_DEFAULT, SLOT_CUSTOM)

    /** The DDS or PNG a slot holds for a texture, DDS first like the core's lookup, or null. */
    fun sourceFile(slot: Int, index: Int): File? =
        EXTENSIONS.map { File(slotDir(slot), "${TEXTURE_NAMES[index]}.$it") }.firstOrNull { it.isFile }

    fun defaultPreviewFile(index: Int): File? =
        EXTENSIONS.map { File(libraryDir(), "default/${TEXTURE_NAMES[index]}.$it") }
            .firstOrNull { it.isFile }

    private fun removeShinePresetFile(): File =
        File(libraryDir(), "presets/remove_shine/${TEXTURE_NAMES[SHEEN_INDEX]}.dds")

    private fun restoreShinePresetFile(slot: Int): File =
        File(libraryDir(), "presets/restore_shine/slot_$slot/${TEXTURE_NAMES[SHEEN_INDEX]}.dds")

    private fun removeSlotTextureFiles(slot: Int, index: Int) {
        EXTENSIONS.forEach { File(slotDir(slot), "${TEXTURE_NAMES[index]}.$it").delete() }
    }

    /** Lower-case file extension of a picked document, from its display name or path. */
    fun extensionOf(uri: Uri): String? {
        val name = ContentHandler.getDisplayName(uri) ?: uri.lastPathSegment ?: return null
        return FileBrowserHelper.getExtension(name, false)?.lowercase(Locale.ROOT)
    }

    /**
     * Copies the picked file into the slot folder under the texture's canonical name, replacing
     * whichever PNG or DDS the slot held for that texture before.
     */
    fun importTexture(context: Context, slot: Int, index: Int, uri: Uri): Outcome {
        val extension = extensionOf(uri)
        if (extension != "png" && extension != "dds")
            return Outcome.Failure(context.getString(R.string.primedgun_cannon_png_or_dds))

        val dir = slotDir(slot)
        if (!dir.isDirectory && !dir.mkdirs())
            return Outcome.Failure(context.getString(R.string.primedgun_cannon_folder_failed))

        val destination = File(dir, "${TEXTURE_NAMES[index]}.$extension")
        val staging = File(dir, "${TEXTURE_NAMES[index]}.$extension.tmp")
        val copied = try {
            context.contentResolver.openInputStream(uri)?.use { input ->
                staging.outputStream().use { output -> input.copyTo(output) }
            } != null
        } catch (e: IOException) {
            false
        }
        if (!copied) {
            staging.delete()
            return Outcome.Failure(context.getString(R.string.primedgun_cannon_copy_failed))
        }

        removeSlotTextureFiles(slot, index)
        if (!staging.renameTo(destination)) {
            staging.delete()
            return Outcome.Failure(context.getString(R.string.primedgun_cannon_copy_failed))
        }
        return Outcome.Success(destination)
    }

    /**
     * Replaces the slot's shine mask with the bundled remove-shine DDS, first saving the current
     * mask so [restoreShine] can bring it back.
     */
    fun removeShine(context: Context, slot: Int): Outcome {
        val preset = removeShinePresetFile()
        if (!preset.isFile) {
            return Outcome.Failure(
                context.getString(R.string.primedgun_cannon_remove_shine_missing, preset.path)
            )
        }

        backupSlotShine(context, slot)?.let { return Outcome.Failure(it) }

        val dir = slotDir(slot)
        if (!dir.isDirectory && !dir.mkdirs())
            return Outcome.Failure(context.getString(R.string.primedgun_cannon_folder_failed))

        val destination = File(dir, "${TEXTURE_NAMES[SHEEN_INDEX]}.dds")
        removeSlotTextureFiles(slot, SHEEN_INDEX)
        if (!copyFile(preset, destination)) {
            return Outcome.Failure(
                context.getString(R.string.primedgun_cannon_remove_shine_copy_failed)
            )
        }
        return Outcome.Success(destination)
    }

    /** Puts the shine mask saved by [removeShine] (or shipped for the slot) back into the slot. */
    fun restoreShine(context: Context, slot: Int): Outcome {
        val source = restoreShinePresetFile(slot)
        if (!source.isFile) {
            return Outcome.Failure(
                context.getString(R.string.primedgun_cannon_restore_shine_missing)
            )
        }

        val dir = slotDir(slot)
        if (!dir.isDirectory && !dir.mkdirs())
            return Outcome.Failure(context.getString(R.string.primedgun_cannon_folder_failed))

        val destination = File(dir, "${TEXTURE_NAMES[SHEEN_INDEX]}.dds")
        removeSlotTextureFiles(slot, SHEEN_INDEX)
        if (!copyFile(source, destination)) {
            return Outcome.Failure(
                context.getString(R.string.primedgun_cannon_restore_shine_copy_failed)
            )
        }
        return Outcome.Success(destination)
    }

    /** Returns an error message, or null when the backup exists or was written. */
    private fun backupSlotShine(context: Context, slot: Int): String? {
        val backup = restoreShinePresetFile(slot)
        if (backup.isFile)
            return null

        val source = sourceFile(slot, SHEEN_INDEX) ?: return null
        if (source.canonicalPath == removeShinePresetFile().canonicalPath)
            return null

        backup.parentFile?.mkdirs()
        if (!copyFile(source, backup))
            return context.getString(R.string.primedgun_cannon_backup_shine_failed, backup.path)
        return null
    }

    private fun copyFile(from: File, to: File): Boolean = try {
        from.inputStream().use { input -> to.outputStream().use { output -> input.copyTo(output) } }
        true
    } catch (e: IOException) {
        false
    }

    /** Decodes a PNG with the platform decoder, or a DXT1 DDS with the decoder below. */
    fun loadPreview(file: File?): Bitmap? {
        if (file == null || !file.isFile)
            return null
        BitmapFactory.decodeFile(file.path)?.let { return it }
        if (file.extension.equals("dds", ignoreCase = true)) {
            return try {
                decodeDxt1(file.readBytes())
            } catch (e: IOException) {
                null
            }
        }
        return null
    }

    // The cannon textures are tiny DXT1 files, so a plain block decoder is enough for a preview.
    // Mirrors PrimedGunDecodeDxt1Preview in the Qt window.
    private fun decodeDxt1(data: ByteArray): Bitmap? {
        if (data.size < 128 || data[0] != 'D'.code.toByte() || data[1] != 'D'.code.toByte() ||
            data[2] != 'S'.code.toByte() || data[3] != ' '.code.toByte()
        ) {
            return null
        }

        fun u16(offset: Int): Int =
            (data[offset].toInt() and 0xff) or ((data[offset + 1].toInt() and 0xff) shl 8)

        fun u32(offset: Int): Int = u16(offset) or (u16(offset + 2) shl 16)

        val height = u32(12)
        val width = u32(16)
        if (width <= 0 || height <= 0 || width > 1024 || height > 1024 || u32(84) != DDS_FOURCC_DXT1)
            return null

        fun expand565(color: Int): Int {
            val r5 = (color shr 11) and 0x1f
            val g6 = (color shr 5) and 0x3f
            val b5 = color and 0x1f
            val r = (r5 shl 3) or (r5 shr 2)
            val g = (g6 shl 2) or (g6 shr 4)
            val b = (b5 shl 3) or (b5 shr 2)
            return (0xff shl 24) or (r shl 16) or (g shl 8) or b
        }

        fun mix(a: Int, b: Int, aw: Int, bw: Int, div: Int): Int {
            val r = (((a shr 16) and 0xff) * aw + ((b shr 16) and 0xff) * bw) / div
            val g = (((a shr 8) and 0xff) * aw + ((b shr 8) and 0xff) * bw) / div
            val bl = ((a and 0xff) * aw + (b and 0xff) * bw) / div
            return (0xff shl 24) or (r shl 16) or (g shl 8) or bl
        }

        val pixels = IntArray(width * height)
        val blocksX = (width + 3) / 4
        val blocksY = (height + 3) / 4
        val colors = IntArray(4)
        var offset = 128
        for (blockY in 0 until blocksY) {
            for (blockX in 0 until blocksX) {
                if (offset + 8 > data.size)
                    return Bitmap.createBitmap(pixels, width, height, Bitmap.Config.ARGB_8888)

                val c0 = u16(offset)
                val c1 = u16(offset + 2)
                val indices = u32(offset + 4)
                offset += 8

                colors[0] = expand565(c0)
                colors[1] = expand565(c1)
                if (c0 > c1) {
                    colors[2] = mix(colors[0], colors[1], 2, 1, 3)
                    colors[3] = mix(colors[0], colors[1], 1, 2, 3)
                } else {
                    colors[2] = mix(colors[0], colors[1], 1, 1, 2)
                    colors[3] = 0
                }

                for (y in 0 until 4) {
                    for (x in 0 until 4) {
                        val pixelX = blockX * 4 + x
                        val pixelY = blockY * 4 + y
                        if (pixelX >= width || pixelY >= height)
                            continue
                        val index = (indices ushr (2 * (y * 4 + x))) and 0x3
                        pixels[pixelY * width + pixelX] = colors[index]
                    }
                }
            }
        }

        return Bitmap.createBitmap(pixels, width, height, Bitmap.Config.ARGB_8888)
    }
}
