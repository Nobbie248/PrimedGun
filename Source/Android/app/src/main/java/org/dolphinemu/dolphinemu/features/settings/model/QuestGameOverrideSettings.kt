// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.settings.model

import org.dolphinemu.dolphinemu.utils.DirectoryInitialization
import java.io.File

object QuestGameOverrideSettings {
    enum class Category(
        val mainSection: String,
        val enabledSection: String,
        val defaultEnabledWithoutSection: Boolean
    ) {
        HIDE_OBJECTS("HideObjectCodes", "HideObjectCodes_Enabled", false),
        SHADER_OVERRIDES("ShaderOverride", "ShaderOverride_Enable", true),
        ELEMENTS_GROUP_OVERRIDES(
            "ElementsGroupOverride",
            "ElementsGroupOverride_Enable",
            true
        )
    }

    data class Entry(val name: String, val enabled: Boolean)

    fun loadEntries(gameId: String, revision: Int, category: Category): List<Entry> {
        return readMergedEntries(gameId, revision, category)
    }

    fun isEnabled(gameId: String, revision: Int, category: Category, name: String): Boolean {
        return loadEntries(gameId, revision, category).firstOrNull { it.name == name }?.enabled ?: false
    }

    fun setEnabled(gameId: String, revision: Int, category: Category, name: String, enabled: Boolean) {
        val lines = readLines(getUserFile(gameId))
        val entries = readMergedEntries(gameId, revision, category)
        if (entries.none { it.name == name }) {
            return
        }

        val enabledNames = entries
            .filter { if (it.name == name) enabled else it.enabled }
            .mapTo(LinkedHashSet()) { it.name }

        writeEnabledSection(gameId, lines, category, enabledNames)
    }

    private data class SectionNames(val hasSection: Boolean, val names: Set<String>)

    private fun getUserFile(gameId: String): File =
        File(DirectoryInitialization.getUserDirectory(), "GameSettingsVR/$gameId.ini")

    private fun getSysIniFile(filename: String): File =
        File(DirectoryInitialization.getSysDirectory(), "GameSettingsVR/$filename")

    private fun getUserIniFile(filename: String): File =
        File(DirectoryInitialization.getUserDirectory(), "GameSettingsVR/$filename")

    private data class ParsedFile(
        val names: List<String>,
        val enabledSection: SectionNames
    )

    private fun readLines(file: File): List<String> {
        return if (file.isFile) file.readLines() else emptyList()
    }

    private fun parseFile(lines: List<String>, category: Category): ParsedFile =
        ParsedFile(
            readEntryNames(lines, category.mainSection),
            readEntryNamesWithSectionState(lines, category.enabledSection)
        )

    private fun getGameIniFilenames(gameId: String, revision: Int): List<String> {
        if (gameId.isEmpty()) {
            return emptyList()
        }

        val filenames = ArrayList<String>()
        if (gameId.length == 6) {
            filenames.add("${gameId.substring(0, 1)}.ini")
            filenames.add("${gameId.substring(0, 3)}.ini")
        }
        filenames.add("$gameId.ini")
        if (revision != 0) {
            filenames.add("${gameId}r$revision.ini")
        }
        return filenames
    }

    private fun applyParsedFile(
        entriesByName: LinkedHashMap<String, Entry>,
        parsed: ParsedFile,
        category: Category
    ) {
        parsed.names.forEach { name ->
            val enabled = if (parsed.enabledSection.hasSection) {
                parsed.enabledSection.names.contains(name)
            } else {
                category.defaultEnabledWithoutSection
            }
            entriesByName[name] = Entry(name, enabled)
        }

        if (parsed.enabledSection.hasSection) {
            entriesByName.replaceAll { name, entry ->
                entry.copy(enabled = parsed.enabledSection.names.contains(name))
            }
        }
    }

    private fun readMergedEntries(gameId: String, revision: Int, category: Category): List<Entry> {
        val entriesByName = LinkedHashMap<String, Entry>()
        val filenames = getGameIniFilenames(gameId, revision)

        filenames.forEach { filename ->
            applyParsedFile(
                entriesByName,
                parseFile(readLines(getSysIniFile(filename)), category),
                category
            )
        }
        filenames.forEach { filename ->
            applyParsedFile(
                entriesByName,
                parseFile(readLines(getUserIniFile(filename)), category),
                category
            )
        }

        return entriesByName.values.toList()
    }

    private fun readEntryNames(lines: List<String>, section: String): List<String> {
        val result = ArrayList<String>()
        var inSection = false
        val header = "[$section]"
        for (line in lines.map { it.removeSuffix("\r") }) {
            if (line == header) {
                inSection = true
            } else if (inSection && line.startsWith("[")) {
                break
            } else if (inSection && line.isNotEmpty() && line.startsWith("$")) {
                val name = line.substring(1)
                if (name.isNotEmpty()) {
                    result.add(name)
                }
            }
        }
        return result
    }

    private fun readEntryNamesWithSectionState(lines: List<String>, section: String): SectionNames {
        var hasSection = false
        val names = readEntryNamesWithSectionFlag(lines, section) { hasSection = true }
        return SectionNames(hasSection, names)
    }

    private fun readEntryNamesWithSectionFlag(
        lines: List<String>,
        section: String,
        onSectionFound: () -> Unit
    ): Set<String> {
        val result = LinkedHashSet<String>()
        var inSection = false
        val header = "[$section]"
        for (line in lines.map { it.removeSuffix("\r") }) {
            if (line == header) {
                inSection = true
                onSectionFound()
            } else if (inSection && line.startsWith("[")) {
                break
            } else if (inSection && line.isNotEmpty() && line.startsWith("$")) {
                val name = line.substring(1)
                if (name.isNotEmpty()) {
                    result.add(name)
                }
            }
        }
        return result
    }

    private fun writeEnabledSection(
        gameId: String,
        lines: List<String>,
        category: Category,
        enabledNames: Set<String>
    ) {
        val stripped = stripSection(lines, category.enabledSection).toMutableList()
        val sectionLines = ArrayList<String>()
        sectionLines.add("[${category.enabledSection}]")
        enabledNames.forEach { sectionLines.add("$" + it) }

        val insertIndex = stripped.indexOfFirst { it.removeSuffix("\r") == "[${category.mainSection}]" }
            .let { if (it >= 0) it else stripped.size }
        if (insertIndex > 0 && stripped[insertIndex - 1].isNotEmpty()) {
            sectionLines.add(0, "")
        }

        stripped.addAll(insertIndex, sectionLines)

        val file = getUserFile(gameId)
        file.parentFile?.mkdirs()
        file.writeText(stripped.joinToString("\n").trimEnd() + "\n")
    }

    private fun stripSection(lines: List<String>, section: String): List<String> {
        val result = ArrayList<String>()
        var skipping = false
        val header = "[$section]"
        for (line in lines) {
            val trimmed = line.removeSuffix("\r")
            if (trimmed == header) {
                skipping = true
                continue
            }
            if (skipping && trimmed.startsWith("[")) {
                skipping = false
            }
            if (!skipping) {
                result.add(trimmed)
            }
        }
        return result
    }
}

class QuestGameOverrideEnabledSetting(
    private val gameId: String,
    private val revision: Int,
    private val category: QuestGameOverrideSettings.Category,
    private val name: String
) : AbstractBooleanSetting {
    override val isOverridden: Boolean = true

    override val isRuntimeEditable: Boolean = false

    override fun delete(settings: Settings): Boolean {
        QuestGameOverrideSettings.setEnabled(gameId, revision, category, name, false)
        return true
    }

    override val boolean: Boolean
        get() = QuestGameOverrideSettings.isEnabled(gameId, revision, category, name)

    override fun setBoolean(settings: Settings, newValue: Boolean) {
        QuestGameOverrideSettings.setEnabled(gameId, revision, category, name, newValue)
    }
}
