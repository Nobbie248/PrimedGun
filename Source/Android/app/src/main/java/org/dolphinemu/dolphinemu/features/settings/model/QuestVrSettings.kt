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

    fun isQuestBuild(): Boolean = BuildConfig.IS_QUEST

    fun openXrEnabledSetting() = androidBooleanSetting("QuestOpenXREnabled", true)

    fun launchInVrSetting() = androidBooleanSetting("QuestLaunchInVr", true)

    fun recenterOnLaunchSetting() = androidBooleanSetting("QuestRecenterOnLaunch", true)

    fun leftHandedSetting() = androidBooleanSetting("QuestLeftHanded", false)

    fun showMirrorSurfaceSetting() = androidBooleanSetting("QuestShowMirrorSurface", false)

    fun controllerPresetSetting() =
        androidIntSetting("QuestControllerPreset", CONTROLLER_PRESET_GAMECUBE)

    fun passthroughSetting() = vrBooleanSetting("ARMode", false)

    fun debugPassthroughSetting() = vrBooleanSetting("ARModeDebug", false)

    fun arBackgroundAlphaSetting() = vrFloatSetting("ARBackgroundAlpha", 0.0f)

    fun autoImmediateXfbSetting() = vrBooleanSetting("AutoImmediateXFB", true)

    fun autoVbiFromHmdSetting() = vrBooleanSetting("AutoVBIFromHMD", false)

    fun unitsPerMeterSetting(): AbstractFloatSetting = FloatSetting.GFX_VR_UNITS_PER_METER

    fun leanBackAngleSetting() = vrFloatSetting("LeanBackAngle", 0.0f)

    fun cameraForwardSetting() = vrFloatSetting("CameraForward", 0.0f)

    fun cameraHeightSetting() = vrFloatSetting("CameraHeight", 0.0f)

    fun lockHeadPoseSetting() = vrBooleanSetting("LockHeadPosePerFrame", false)

    fun opcodeReplaySetting() = vrIntSetting("OpcodeReplay", 0)

    fun vrGammaSetting() = vrFloatSetting("Gamma", 1.0f)

    fun autoLayerSpreadSetting() = vrBooleanSetting("AutoLayerSpread", true)

    fun layerOffsetSetting() = vrFloatSetting("LayerOffset", 0.002f)

    fun elementDepthSetting() = vrFloatSetting("ElementDepth", 0.001f)

    fun removeBarsSetting() = vrBooleanSetting("RemoveCinematicBars", true)

    fun useVulkanMultiviewSetting() = vrBooleanSetting("UseVulkanMultiview", true)

    fun androidDirectToHmdSetting() = vrBooleanSetting("AndroidDirectToHMD", true)

    fun cpuLevel5HintSetting() = vrBooleanSetting("QuestCpuLevel5Hint", false)

    fun virtualScreenSetting() = vrBooleanSetting("VirtualScreen", false)

    fun screenDistanceSetting() = vrFloatSetting("ScreenDistance", 1.5f)

    fun screenSizeSetting() = vrFloatSetting("ScreenSize", 1.5f)

    fun headLockedCurvatureSetting() = vrFloatSetting("HeadLockedCurvature", 0.0f)

    fun dontClearScreenSetting() = vrBooleanSetting("DontClearScreen", false)

    fun disableCpuCullSetting() = vrBooleanSetting("DisableCPUCull", false)

    fun clearEfbCopiesSetting() = vrIntSetting("ClearEFBCopies", 0)

    fun loadCustomShadersSetting() = vrBooleanSetting("LoadCustomShaders", false)

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
        // Quest 3: 2x internal res balances readability/fidelity against the mobile GPU (matches
        // the GFX_EFB_SCALE Android code default). 4x is too heavy for a steady framerate here.
        IntSetting.GFX_EFB_SCALE.setInt(settings, 2)
        BooleanSetting.GFX_WAIT_FOR_SHADERS_BEFORE_STARTING.setBoolean(settings, false)
        // Async "skip-drawing" shader compilation (mode 3): compile new pipelines on background
        // worker threads instead of stalling the emulation thread on room load. Mirrors the
        // GFX_SHADER_COMPILATION_MODE Android code default; a handful of geometry pop-in frames on
        // first encounter beats a 130-305 ms hard freeze in VR. The worker-thread count is set by
        // the Android code default (no IntSetting for ShaderCompilerThreads on the Java side).
        IntSetting.GFX_SHADER_COMPILATION_MODE.setInt(settings, 3)
        BooleanSetting.MAIN_SHOW_INPUT_OVERLAY.setBoolean(settings, false)
        lockHeadPoseSetting().setBoolean(settings, false)
        autoLayerSpreadSetting().setBoolean(settings, true)
        androidDirectToHmdSetting().setBoolean(settings, true)
        removeBarsSetting().setBoolean(settings, true)
        virtualScreenSetting().setBoolean(settings, false)
        passthroughSetting().setBoolean(settings, false)
        debugPassthroughSetting().setBoolean(settings, false)
        BooleanSetting.GFX_HACK_IMMEDIATE_XFB.setBoolean(settings, true)
        BooleanSetting.GFX_HACK_VI_SKIP.setBoolean(settings, false)
        perfDefaultsAppliedSetting().setBoolean(settings, true)
        backendMultithreadingReenabledSetting().setBoolean(settings, true)
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
        // GPU (compute-shader) texture decoding SIGSEGVs the Adreno driver in vkUpdateDescriptorSets
        // (VK_EXT_robustness2 nullDescriptor was tried and is insufficient -- crash repro 2026-06-22,
        // see quest-gpu-shader-plan.md). Force CPU texture decoding on every Quest launch. The native
        // bSupportsGPUTextureDecoding=false gate (VulkanContext::PopulateBackendInfoFeatures) is the
        // real safety net; this is defense in depth.
        BooleanSetting.GFX_ENABLE_GPU_TEXTURE_DECODING.setBoolean(settings, false)

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
