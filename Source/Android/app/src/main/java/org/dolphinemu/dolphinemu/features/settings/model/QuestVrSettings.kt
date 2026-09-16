// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.settings.model

import org.dolphinemu.dolphinemu.BuildConfig
import org.dolphinemu.dolphinemu.features.input.model.controlleremu.EmulatedController

object QuestVrSettings {
    const val STEREO_MODE_OPENXR = 6
    const val CONTROLLER_PRESET_GAMECUBE = 0
    const val CONTROLLER_PRESET_WII_REMOTE = 1

    private const val GC_PROFILE_NAME = "Quest Touch GameCube.ini"
    private const val WIIMOTE_PROFILE_NAME = "OpenXR Wii Remote.ini"
    private const val VR_SECTION = "VR"

    /** Metroid Prime is the only game the mod supports, so its VR profile is the one edited. */
    private const val METROID_PRIME_GAME_ID = "GM8E01"
    private const val METROID_PRIME_REVISION = 0

    /**
     * Keys that ConfigManager's ApplyGameVRConfigOverrides pins for Metroid Prime at boot from
     * the per-game VR profile (User/GameSettingsVR/GM8E01.ini, with built-in fallbacks). A value
     * for them in the global GFX.ini is never seen by the game, so the OpenXR screen edits the
     * per-game profile for these instead. Their defaults below repeat the built-in fallbacks.
     */
    private val METROID_PINNED_KEYS = setOf(
        "UnitsPerMeter",
        "LeanBackAngle",
        "CameraForward",
        "VirtualScreen",
        "HeadLockedCurvature",
        "DontClearScreen",
        "LoadCustomShaders",
        "LayerOffset",
        "ElementDepth",
        "ClearEFBCopies"
    )

    private fun androidBooleanSetting(key: String, defaultValue: Boolean) =
        AdHocBooleanSetting(Settings.FILE_DOLPHIN, Settings.SECTION_INI_ANDROID, key, defaultValue)

    private fun androidIntSetting(key: String, defaultValue: Int) =
        AdHocIntSetting(Settings.FILE_DOLPHIN, Settings.SECTION_INI_ANDROID, key, defaultValue)

    private fun vrBooleanSetting(key: String, defaultValue: Boolean) =
        AdHocBooleanSetting(Settings.FILE_GFX, VR_SECTION, key, defaultValue)

    private fun vrIntSetting(key: String, defaultValue: Int) =
        AdHocIntSetting(Settings.FILE_GFX, VR_SECTION, key, defaultValue)

    private fun vrFloatSetting(key: String, defaultValue: Float) =
        AdHocFloatSetting(Settings.FILE_GFX, VR_SECTION, key, defaultValue)

    // The "effective" accessors pick the store the game actually reads: the per-game profile for
    // pinned keys, the global GFX.ini for everything else.
    private fun effectiveBoolean(key: String, defaultValue: Boolean): AbstractBooleanSetting =
        if (key in METROID_PINNED_KEYS) {
            QuestGameVrConfigBooleanSetting(METROID_PRIME_GAME_ID, METROID_PRIME_REVISION, key, defaultValue)
        } else {
            vrBooleanSetting(key, defaultValue)
        }

    private fun effectiveInt(key: String, defaultValue: Int): AbstractIntSetting =
        if (key in METROID_PINNED_KEYS) {
            QuestGameVrConfigIntSetting(METROID_PRIME_GAME_ID, METROID_PRIME_REVISION, key, defaultValue)
        } else {
            vrIntSetting(key, defaultValue)
        }

    private fun effectiveFloat(key: String, defaultValue: Float): AbstractFloatSetting =
        if (key in METROID_PINNED_KEYS) {
            QuestGameVrConfigFloatSetting(METROID_PRIME_GAME_ID, METROID_PRIME_REVISION, key, defaultValue)
        } else {
            vrFloatSetting(key, defaultValue)
        }

    fun isQuestBuild(): Boolean = BuildConfig.IS_QUEST

    // ---------- Runtime ----------

    fun openXrEnabledSetting() = androidBooleanSetting("QuestOpenXREnabled", true)

    fun launchInVrSetting() = androidBooleanSetting("QuestLaunchInVr", true)

    fun recenterOnLaunchSetting() = androidBooleanSetting("QuestRecenterOnLaunch", true)

    fun leftHandedSetting() = androidBooleanSetting("QuestLeftHanded", false)

    fun showMirrorSurfaceSetting() = androidBooleanSetting("QuestShowMirrorSurface", false)

    fun controllerPresetSetting() =
        androidIntSetting("QuestControllerPreset", CONTROLLER_PRESET_GAMECUBE)

    fun unitsPerMeterSetting(): AbstractFloatSetting = effectiveFloat("UnitsPerMeter", 1.5f)

    // ---------- Camera ----------

    fun leanBackAngleSetting(): AbstractFloatSetting = effectiveFloat("LeanBackAngle", 0.0f)

    fun enableCameraForwardSetting() = vrBooleanSetting("EnableCameraForward", true)

    fun cameraForwardSetting(): AbstractFloatSetting = effectiveFloat("CameraForward", 0.0f)

    fun enableCameraHeightSetting() = vrBooleanSetting("EnableCameraHeight", true)

    fun cameraHeightSetting() = vrFloatSetting("CameraHeight", 0.0f)

    // ---------- Virtual screen ----------

    fun virtualScreenSetting(): AbstractBooleanSetting = effectiveBoolean("VirtualScreen", true)

    fun screenDistanceSetting() = vrFloatSetting("ScreenDistance", 1.5f)

    fun screenSizeSetting() = vrFloatSetting("ScreenSize", 1.5f)

    fun headLockedCurvatureSetting(): AbstractFloatSetting =
        effectiveFloat("HeadLockedCurvature", 0.0f)

    fun hudThicknessSetting() = vrFloatSetting("HudThickness", 0.0f)

    fun autoLayerSpreadSetting() = vrBooleanSetting("AutoLayerSpread", false)

    fun layerOffsetSetting(): AbstractFloatSetting = effectiveFloat("LayerOffset", 0.002f)

    fun elementDepthSetting(): AbstractFloatSetting = effectiveFloat("ElementDepth", 0.001f)

    fun hud3dEnableSetting() = vrBooleanSetting("Hud3DEnable", false)

    fun hud3dCloserSetting() = vrFloatSetting("Hud3DCloser", 0.5f)

    // ---------- Rendering ----------

    // Defaults must match GraphicsSettings.cpp (Android values). These four need a restart, so
    // they are not runtime editable.
    fun resolutionScaleSetting(): AbstractFloatSetting =
        object : AbstractFloatSetting by vrFloatSetting("ResolutionScale", 0.85f) {
            override val isRuntimeEditable: Boolean = false
        }

    fun foveationLevelSetting(): AbstractIntSetting =
        object : AbstractIntSetting by vrIntSetting("FoveationLevel", 2) {
            override val isRuntimeEditable: Boolean = false
        }

    fun dynamicFoveationSetting(): AbstractBooleanSetting =
        object : AbstractBooleanSetting by vrBooleanSetting("DynamicFoveation", true) {
            override val isRuntimeEditable: Boolean = false
        }

    fun foveateEfbSetting(): AbstractBooleanSetting =
        object : AbstractBooleanSetting by vrBooleanSetting("FoveateEFB", false) {
            override val isRuntimeEditable: Boolean = false
        }

    fun clearEfbCopiesSetting(): AbstractIntSetting = effectiveInt("ClearEFBCopies", 0)

    fun vrGammaSetting() = vrFloatSetting("Gamma", 1.0f)

    // ---------- Framerate ----------

    // AutoVBIFromHMD is the legacy boolean that ForcedVBIFrequency replaced. While set it pins
    // the effective rate to 90 Hz even when the frequency reads "Off", so it is cleared whenever
    // an explicit choice is made, the same as the PC VR pane does.
    fun autoVbiFromHmdSetting() = vrBooleanSetting("AutoVBIFromHMD", false)

    fun forcedVbiFrequencySetting(): AbstractIntSetting = ForcedVbiFrequencySetting

    private object ForcedVbiFrequencySetting : AbstractIntSetting {
        private val backing = vrIntSetting("ForcedVBIFrequency", 0)

        override val isOverridden: Boolean
            get() = backing.isOverridden

        override val isRuntimeEditable: Boolean
            get() = backing.isRuntimeEditable

        override fun delete(settings: Settings): Boolean = backing.delete(settings)

        override val int: Int
            get() = backing.int

        override fun setInt(settings: Settings, newValue: Int) {
            backing.setInt(settings, newValue)
            autoVbiFromHmdSetting().setBoolean(settings, false)
        }
    }

    fun eagerHeartbeatSetting() = vrBooleanSetting("EagerHeartbeat", false)

    fun xrPacingThreadSetting() = vrBooleanSetting("UseXRPacingThread", true)

    fun autoImmediateXfbSetting() = vrBooleanSetting("AutoImmediateXFB", true)

    fun opcodeReplaySetting() = vrIntSetting("OpcodeReplay", 0)

    fun opcodeReplayTargetRefreshRateSetting() = vrIntSetting("OpcodeReplayTargetRefreshRate", -1)

    // ---------- VR hacks ----------

    fun useVulkanMultiviewSetting(): AbstractBooleanSetting =
        object : AbstractBooleanSetting by vrBooleanSetting("UseVulkanMultiview", false) {
            override val isRuntimeEditable: Boolean = false
        }

    fun lockHeadPoseSetting() = vrBooleanSetting("LockHeadPosePerFrame", false)

    fun dontClearScreenSetting(): AbstractBooleanSetting = effectiveBoolean("DontClearScreen", false)

    fun headCpuCullSetting() = vrBooleanSetting("HeadCPUCull", true)

    fun removeBarsSetting() = vrBooleanSetting("RemoveCinematicBars", true)

    fun orthoScissorFixSetting() = vrBooleanSetting("OrthoScissorFix", true)

    fun detectSkyboxSetting() = vrBooleanSetting("DetectSkybox", true)

    fun metroidVisorFixSetting() = vrBooleanSetting("MetroidVisorFix", true)

    // ---------- Passthrough ----------

    fun passthroughSetting() = vrBooleanSetting("ARMode", false)

    fun debugPassthroughSetting() = vrBooleanSetting("ARModeDebug", false)

    fun arBackgroundAlphaSetting() = vrFloatSetting("ARBackgroundAlpha", 0.0f)

    // ---------- Comfort and debug ----------

    fun androidDirectToHmdSetting() = vrBooleanSetting("AndroidDirectToHMD", true)

    fun cpuLevel5HintSetting() = vrBooleanSetting("QuestCpuLevel5Hint", false)

    fun pinEmulationCoresSetting() = vrBooleanSetting("PinEmulationCores", true)

    fun loadCustomShadersSetting(): AbstractBooleanSetting = effectiveBoolean("LoadCustomShaders", true)

    fun referenceSpaceModeSetting() = vrIntSetting("ReferenceSpaceMode", 1)

    fun trackingModeSetting() = vrIntSetting("TrackingMode", 0)

    fun openXrConfigSceneSetting() = vrBooleanSetting("EnableOpenXRConfigScene", true)

    private fun openXrRuntimeSetting() = vrBooleanSetting("EnableOpenXR", false)

    private fun perfDefaultsAppliedSetting() = androidBooleanSetting("QuestPerfProfileApplied", false)

    private fun backendMultithreadingReenabledSetting() =
        androidBooleanSetting("QuestBackendMultithreadingReenabled", false)

    private fun controllerProfilesAppliedSetting() =
        androidBooleanSetting("QuestControllerProfilesApplied", false)

    fun shouldShowMirrorSurface(): Boolean {
        if (!BuildConfig.IS_QUEST) {
            return true
        }

        return showMirrorSurfaceSetting().boolean || !isLaunchInVrEnabled()
    }

    fun isLaunchInVrEnabled(): Boolean {
        return BuildConfig.IS_QUEST &&
            openXrEnabledSetting().boolean &&
            launchInVrSetting().boolean
    }

    fun applyRecommendedDefaults(settings: Settings) {
        if (!BuildConfig.IS_QUEST) {
            return
        }

        StringSetting.MAIN_GFX_BACKEND.setString(settings, "Vulkan")
        BooleanSetting.GFX_BACKEND_MULTITHREADING.setBoolean(settings, true)
        // 3x is the highest internal resolution that holds 60fps on Quest 3; 4x produces
        // visible jitter and slow-motion.
        IntSetting.GFX_EFB_SCALE.setInt(settings, 3)
        BooleanSetting.GFX_WAIT_FOR_SHADERS_BEFORE_STARTING.setBoolean(settings, false)
        BooleanSetting.MAIN_SHOW_INPUT_OVERLAY.setBoolean(settings, false)
        applyRecommendedVrDefaults(settings)
        BooleanSetting.GFX_HACK_IMMEDIATE_XFB.setBoolean(settings, true)
        BooleanSetting.GFX_HACK_VI_SKIP.setBoolean(settings, false)
        perfDefaultsAppliedSetting().setBoolean(settings, true)
        backendMultithreadingReenabledSetting().setBoolean(settings, true)
    }

    /** The VR keys of the recommended Quest profile, shared with [resetOpenXrSettings]. */
    private fun applyRecommendedVrDefaults(settings: Settings) {
        lockHeadPoseSetting().setBoolean(settings, false)
        autoLayerSpreadSetting().setBoolean(settings, true)
        androidDirectToHmdSetting().setBoolean(settings, true)
        removeBarsSetting().setBoolean(settings, true)
        // The global key: the game reads the pinned per-game value, which stays untouched here.
        vrBooleanSetting("VirtualScreen", true).setBoolean(settings, false)
        passthroughSetting().setBoolean(settings, false)
        debugPassthroughSetting().setBoolean(settings, false)
    }

    /**
     * Restores every setting on the OpenXR screen to its built-in default and then re-applies
     * the recommended Quest profile, mirroring the PC VR pane's "Reset Settings" button.
     *
     * Deletes the keys instead of writing values so the compiled-in defaults apply; the pinned
     * per-game keys are removed from the per-game profile so the built-in Metroid Prime fallbacks
     * take over again. Non-VR graphics settings (backend, EFB scale) and controller mappings are
     * deliberately left alone; they do not belong to this screen.
     */
    fun resetOpenXrSettings(settings: Settings) {
        val resettable: List<AbstractSetting> = listOf(
            // Runtime
            openXrEnabledSetting(),
            launchInVrSetting(),
            recenterOnLaunchSetting(),
            unitsPerMeterSetting(),
            // Camera
            leanBackAngleSetting(),
            enableCameraForwardSetting(),
            cameraForwardSetting(),
            enableCameraHeightSetting(),
            cameraHeightSetting(),
            // Virtual screen
            virtualScreenSetting(),
            screenDistanceSetting(),
            screenSizeSetting(),
            headLockedCurvatureSetting(),
            hudThicknessSetting(),
            autoLayerSpreadSetting(),
            layerOffsetSetting(),
            elementDepthSetting(),
            hud3dEnableSetting(),
            hud3dCloserSetting(),
            // Rendering
            resolutionScaleSetting(),
            foveationLevelSetting(),
            dynamicFoveationSetting(),
            foveateEfbSetting(),
            clearEfbCopiesSetting(),
            vrGammaSetting(),
            // Framerate
            forcedVbiFrequencySetting(),
            autoVbiFromHmdSetting(),
            eagerHeartbeatSetting(),
            xrPacingThreadSetting(),
            autoImmediateXfbSetting(),
            opcodeReplaySetting(),
            opcodeReplayTargetRefreshRateSetting(),
            // VR hacks
            useVulkanMultiviewSetting(),
            lockHeadPoseSetting(),
            dontClearScreenSetting(),
            removeBarsSetting(),
            orthoScissorFixSetting(),
            detectSkyboxSetting(),
            metroidVisorFixSetting(),
            // Passthrough
            passthroughSetting(),
            debugPassthroughSetting(),
            arBackgroundAlphaSetting(),
            // Comfort and debug
            showMirrorSurfaceSetting(),
            BooleanSetting.GFX_SHOW_FPS,
            androidDirectToHmdSetting(),
            cpuLevel5HintSetting(),
            pinEmulationCoresSetting(),
            loadCustomShadersSetting(),
            referenceSpaceModeSetting(),
            trackingModeSetting()
        )

        resettable.forEach { it.delete(settings) }
        applyRecommendedVrDefaults(settings)
    }

    fun applySelectedControllerPreset(settings: Settings) {
        applyControllerPreset(settings, controllerPresetSetting().int)
    }

    fun prepareLaunchSettings(settings: Settings, launchSystemMenu: Boolean) {
        if (!BuildConfig.IS_QUEST) {
            return
        }

        if (!perfDefaultsAppliedSetting().boolean) {
            applyRecommendedDefaults(settings)
        }

        StringSetting.MAIN_GFX_BACKEND.setString(settings, "Vulkan")

        val launchInVr = isLaunchInVrEnabled()
        openXrRuntimeSetting().setBoolean(settings, launchInVr)

        if (launchInVr) {
            IntSetting.GFX_STEREO_MODE.setInt(settings, STEREO_MODE_OPENXR)
            if (!backendMultithreadingReenabledSetting().boolean) {
                BooleanSetting.GFX_BACKEND_MULTITHREADING.setBoolean(settings, true)
                backendMultithreadingReenabledSetting().setBoolean(settings, true)
            }
            BooleanSetting.MAIN_SHOW_INPUT_OVERLAY.setBoolean(settings, false)
            if (lockHeadPoseSetting().boolean) {
                autoImmediateXfbSetting().setBoolean(settings, false)
                BooleanSetting.GFX_HACK_IMMEDIATE_XFB.setBoolean(settings, false)
            } else if (autoImmediateXfbSetting().boolean) {
                BooleanSetting.GFX_HACK_IMMEDIATE_XFB.setBoolean(settings, true)
            }
            BooleanSetting.GFX_HACK_VI_SKIP.setBoolean(settings, false)
        } else if (IntSetting.GFX_STEREO_MODE.int == STEREO_MODE_OPENXR) {
            IntSetting.GFX_STEREO_MODE.setInt(settings, 0)
        }

        if (launchSystemMenu) {
            applyControllerPreset(settings, CONTROLLER_PRESET_WII_REMOTE)
        } else {
            applySelectedControllerPreset(settings)
        }
    }

    fun applyControllerPreset(settings: Settings, preset: Int) {
        if (!BuildConfig.IS_QUEST) {
            return
        }

        if (!controllerProfilesAppliedSetting().boolean) {
            EmulatedController.getGcPad(0).loadProfile(
                EmulatedController.getGcPad(0).getSysProfileDirectoryPath() + GC_PROFILE_NAME
            )
            EmulatedController.getWiimote(0).loadProfile(
                EmulatedController.getWiimote(0).getSysProfileDirectoryPath() + WIIMOTE_PROFILE_NAME
            )
            controllerProfilesAppliedSetting().setBoolean(settings, true)
        }

        when (preset) {
            CONTROLLER_PRESET_WII_REMOTE -> applyWiiRemoteSource(settings)
            else -> applyGameCubeSource(settings)
        }
    }

    private fun applyGameCubeSource(settings: Settings) {
        IntSetting.MAIN_SI_DEVICE_0.setInt(settings, 6)
        IntSetting.WIIMOTE_1_SOURCE.setInt(settings, 0)
    }

    private fun applyWiiRemoteSource(settings: Settings) {
        IntSetting.WIIMOTE_1_SOURCE.setInt(settings, 3)
    }
}
