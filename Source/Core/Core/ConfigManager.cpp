// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/ConfigManager.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include <fmt/format.h>

#include "AudioCommon/AudioCommon.h"

#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"

#include "Core/AchievementManager.h"
#include "Core/Boot/Boot.h"
#include "Core/Config/DefaultLocale.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/SYSCONFSettings.h"
#include "Core/ConfigLoaders/GameConfigLoader.h"
#include "Core/Core.h"
#include "Core/DolphinAnalytics.h"
#include "Core/FifoPlayer/FifoDataFile.h"
#include "Core/HLE/HLE.h"
#include "Core/HW/DVD/DVDInterface.h"
#include "Core/HW/EXI/EXI_Device.h"
#include "Core/HW/GCKeyboard.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/Wiimote.h"
#include "Core/Host.h"
#include "Core/IOS/ES/ES.h"
#include "Core/IOS/ES/Formats.h"
#include "Core/PatchEngine.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/System.h"
#include "Core/TitleDatabase.h"
#include "Core/WC24PatchEngine.h"

#include "VideoCommon/HiresTextures.h"
#include "VideoCommon/VideoBackendBase.h"

#include "DiscIO/Enums.h"
#include "DiscIO/Volume.h"
#include "DiscIO/VolumeWad.h"

namespace
{
constexpr std::string_view VR_SECTION_NAME = "Graphics.VR";
constexpr std::string_view LEGACY_VR_SECTION_NAME = "GFX.VR";

using VRSettingMap = std::map<std::string, std::string, Common::CaseInsensitiveLess>;

static void EnsurePrimedGunDefaultGFXConfig()
{
  const std::string path = File::GetUserPath(F_GFXCONFIG_IDX);
  if (File::Exists(path) && File::GetSize(path) > 0)
    return;

  File::CreateFullPath(path);
  std::ofstream file(path, std::ios::trunc);
  file << R"([Enhancements]
ArbitraryMipmapDetection = True
DisableCopyFilter = True
ForceTrueColor = True
PostProcessingShader = 
ForceTextureFiltering = 0
ForceFiltering = False
OutputResampling = 0
HDROutput = False
[Hacks]
BBoxEnable = False
DeferEFBCopies = True
EFBEmulateFormatChanges = False
EFBScaledCopy = True
EFBToTextureEnable = True
SkipDuplicateXFBs = False
XFBToTextureEnable = True
EFBAccessEnable = False
ForceProgressive = True
EFBCopyEnable = False
EFBCopyClearDisable = False
ImmediateXFBEnable = True
VISkip = True
EFBAccessDeferInvalidation = False
FastTextureSampling = True
[Settings]
BackendMultithreading = True
FastDepthCalc = True
InternalResolution = 4
SaveTextureCacheToState = True
MSAA = 0x00000001
SSAA = False
SWDrawEnd = 100000
SWDrawStart = 0
wideScreenHack = False
DumpBaseTextures = False
DumpMipTextures = False
ShowSpeedColors = True
AspectRatio = 1
Crop = False
UseXFB = False
UseRealXFB = False
SafeTextureCacheColorSamples = 512
ShowFPS = False
LogRenderTimeToFile = False
OverlayStats = False
OverlayProjStats = False
DumpTextures = False
HiresTextures = True
ConvertHiresTextures = False
CacheHiresTextures = False
DumpEFBTarget = False
FreeLook = False
UseFFV1 = False
EnablePixelLighting = False
EFBScale = 9
TexFmtOverlayEnable = False
TexFmtOverlayCenter = False
Wireframe = False
DisableFog = False
BorderlessFullscreen = False
SWZComploc = True
SWZFreeze = True
SWDumpObjects = False
SWDumpTevStages = False
SWDumpTevTexFetches = False
FrameDumpsResolutionType = 1
EnableMods = False
WaitForShadersBeforeStarting = True
EnableGPUTextureDecoding = True
ShaderCompilationMode = 0
CPUCull = False
UseLossless = False
[Stereoscopy]
StereoMode = 6
StereoSwapEyes = False
StereoDepth = 20
StereoConvergencePercentage = 100
[Hardware]
Adapter = 0
VSync = False
[VR]
UseVulkanMultiview = False
CameraForward = 0.
EnableOpenXR = True
AutoVBIFromHMD = False
DisableCPUCull = True
OpcodeReplay = 0
DontClearScreen = False
ClearEFBCopies = 0
ElementDepth = 0.0009999999
Gamma = 1.
LayerOffset = 0.0019999999
VirtualScreen = True
LoadCustomShaders = True
AutoImmediateXFB = False
LockHeadPosePerFrame = False
ReferenceSpaceMode = 1
MetroidVisorFix = True
LimitOpenXRTo60FPS = False
UnitsPerMeter = 1.5
)";
}

static std::string GetVRGameINIPath(std::string_view game_id)
{
  return File::GetUserPath(D_GAMESETTINGSVR_IDX) + std::string(game_id) + ".ini";
}

static bool IsVRSectionHeader(std::string_view line)
{
  if (line.size() < 3 || line.front() != '[' || line.back() != ']')
    return false;

  const std::string_view section_name = line.substr(1, line.size() - 2);
  return Common::CaseInsensitiveEquals(section_name, VR_SECTION_NAME) ||
         Common::CaseInsensitiveEquals(section_name, LEGACY_VR_SECTION_NAME);
}

static VRSettingMap LoadVRSettingsFromINI(std::string_view game_id)
{
  VRSettingMap values;

  if (game_id.empty() || game_id == DEFAULT_GAME_ID)
    return values;

  if (Common::CaseInsensitiveEquals(game_id, "GM8E01"))
  {
    values.insert_or_assign("EnableOpenXR", "True");
    values.insert_or_assign("UnitsPerMeter", "1.50");
    values.insert_or_assign("LeanBackAngle", "0.0");
    values.insert_or_assign("CameraForward", "0.0");
    values.insert_or_assign("VirtualScreen", "True");
    values.insert_or_assign("HeadLockedCurvature", "0.0");
    values.insert_or_assign("DontClearScreen", "False");
    values.insert_or_assign("LoadCustomShaders", "True");
    values.insert_or_assign("DisableCPUCull", "True");
    values.insert_or_assign("AutoVBIFromHMD", "False");
    values.insert_or_assign("LayerOffset", "0.0020");
    values.insert_or_assign("ElementDepth", "0.0010");
    values.insert_or_assign("ClearEFBCopies", "0");
  }

  std::ifstream file(GetVRGameINIPath(game_id));
  if (!file.is_open())
    return values;

  bool in_vr_section = false;
  std::string line;
  while (std::getline(file, line))
  {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();

    const std::string_view trimmed = StripWhitespace(line);
    if (trimmed.empty())
      continue;

    if (!trimmed.empty() && trimmed.front() == '[')
    {
      if (IsVRSectionHeader(trimmed))
      {
        in_vr_section = true;
        continue;
      }

      if (in_vr_section)
        break;
    }

    if (!in_vr_section)
      continue;

    if (trimmed.front() == '#' || trimmed.front() == ';' || trimmed.front() == '$' ||
        trimmed.front() == '*')
    {
      continue;
    }

    std::string key;
    std::string value;
    Common::IniFile::ParseLine(trimmed, &key, &value);
    if (!key.empty())
      values.insert_or_assign(std::move(key), std::move(value));
  }

  return values;
}

static void ApplyPrimedGunMetroidDefaults(Common::IniFile* game_ini)
{
  auto* core = game_ini->GetOrCreateSection("Core");
  core->Set("MMU", "True");
  core->Set("EnableCheats", "False");
  core->Set("CPUThread", "True");
  core->Set("FPRF", "False");
  core->Set("SyncGPU", "False");
  core->Set("FastDiscSpeed", "True");
  core->Set("DSPHLE", "True");
  core->Set("GPUDeterminismMode", "auto");
#ifdef _WIN32
  core->Set("GFXBackend", "D3D");
#elif defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(ANDROID)
  core->Set("GFXBackend", "Vulkan");
#else
  core->Set("GFXBackend", VideoBackendBase::GetDefaultBackendConfigName());
#endif

  auto* stereo = game_ini->GetOrCreateSection("Video_Stereoscopy");
  stereo->Set("StereoDepthPercentage", "100");
  stereo->Set("StereoConvergence", "20.00");
  stereo->Set("StereoEFBMonoDepth", "False");

  auto* video = game_ini->GetOrCreateSection("Video_Settings");
  video->Set("ShaderCompilationMode", "0");
  video->Set("MSAA", "0x00000001");
  video->Set("SSAA", "False");
  video->Set("InternalResolution", "4");
  video->Set("EnableGPUTextureDecoding", "True");
  video->Set("WaitForShadersBeforeStarting", "True");
  video->Set("SafeTextureCacheColorSamples", "512");

  auto* hacks = game_ini->GetOrCreateSection("Video_Hacks");
  hacks->Set("EFBScaledCopy", "True");
  hacks->Set("VISkip", "True");
}

template <typename T>
static void ApplyVRSetting(const VRSettingMap& values, const char* key, const Config::Info<T>& info)
{
  const auto it = values.find(key);
  if (it == values.end())
  {
    Config::DeleteKey(Config::LayerType::CurrentRun, info);
    return;
  }

  T parsed_value{};
  if (TryParse(it->second, &parsed_value))
    Config::SetCurrent(info, parsed_value);
  else
    Config::DeleteKey(Config::LayerType::CurrentRun, info);
}

static void ApplyGameVRConfigOverrides(std::string_view game_id)
{
  const Config::ConfigChangeCallbackGuard guard;
  const VRSettingMap values = LoadVRSettingsFromINI(game_id);

  ApplyVRSetting(values, "EnableOpenXR", Config::GFX_VR_ENABLE_OPENXR);
  ApplyVRSetting(values, "UnitsPerMeter", Config::GFX_VR_UNITS_PER_METER);
  ApplyVRSetting(values, "LeanBackAngle", Config::GFX_VR_LEAN_BACK_ANGLE);
  ApplyVRSetting(values, "CameraForward", Config::GFX_VR_CAMERA_FORWARD);
  ApplyVRSetting(values, "VirtualScreen", Config::GFX_VR_VIRTUAL_SCREEN);
  ApplyVRSetting(values, "ScreenDistance", Config::GFX_VR_SCREEN_DISTANCE);
  ApplyVRSetting(values, "ScreenSize", Config::GFX_VR_SCREEN_SIZE);
  ApplyVRSetting(values, "HeadLockedCurvature", Config::GFX_VR_HEAD_LOCKED_CURVATURE);
  ApplyVRSetting(values, "DontClearScreen", Config::GFX_VR_DONT_CLEAR_SCREEN);
  ApplyVRSetting(values, "LoadCustomShaders", Config::GFX_VR_LOAD_CUSTOM_SHADERS);
  ApplyVRSetting(values, "DisableCPUCull", Config::GFX_VR_DISABLE_CPU_CULL);
  ApplyVRSetting(values, "AutoVBIFromHMD", Config::GFX_VR_AUTO_VBI_FROM_HMD);
  ApplyVRSetting(values, "AutoLayerSpread", Config::GFX_VR_AUTO_LAYER_SPREAD);
  ApplyVRSetting(values, "LayerOffset", Config::GFX_VR_LAYER_OFFSET);
  ApplyVRSetting(values, "ElementDepth", Config::GFX_VR_ELEMENT_DEPTH);
  ApplyVRSetting(values, "ClearEFBCopies", Config::GFX_VR_CLEAR_EFB_COPIES);
}
}  // namespace

SConfig* SConfig::m_Instance;

SConfig::SConfig()
{
  LoadDefaults();
  EnsurePrimedGunDefaultGFXConfig();
  // Make sure we have log manager
  LoadSettings();
}

void SConfig::Init()
{
  m_Instance = new SConfig;
}

void SConfig::Shutdown()
{
  delete m_Instance;
  m_Instance = nullptr;
}

SConfig::~SConfig()
{
  SaveSettings();
}

void SConfig::SaveSettings()
{
  NOTICE_LOG_FMT(BOOT, "Saving settings to {}", File::GetUserPath(F_DOLPHINCONFIG_IDX));
  Config::Save();
}

void SConfig::LoadSettings()
{
  INFO_LOG_FMT(BOOT, "Loading Settings from {}", File::GetUserPath(F_DOLPHINCONFIG_IDX));
  Config::Load();
}

void SConfig::ResetAllSettings()
{
  Config::ConfigChangeCallbackGuard config_guard;

  File::Delete(File::GetUserPath(F_DOLPHINCONFIG_IDX));
  File::Delete(File::GetUserPath(F_GFXCONFIG_IDX));
  File::Delete(File::GetUserPath(F_LOGGERCONFIG_IDX));
  File::Delete(File::GetUserPath(F_DUALSHOCKUDPCLIENTCONFIG_IDX));
  File::Delete(File::GetUserPath(F_FREELOOKCONFIG_IDX));
  File::Delete(File::GetUserPath(F_RETROACHIEVEMENTSCONFIG_IDX));
  File::Delete(File::GetUserPath(F_WIISYSCONF_IDX));

  for (Config::LayerType layer_type : Config::SEARCH_ORDER)
  {
    const std::shared_ptr<Config::Layer> layer = Config::GetLayer(layer_type);
    if (!layer)
      continue;
    layer->DeleteAllKeys();
  }

  Config::OnConfigChanged();
}

const std::string SConfig::GetGameID() const
{
  std::lock_guard<std::recursive_mutex> lock(m_metadata_lock);
  return m_game_id;
}

const std::string SConfig::GetGameTDBID() const
{
  std::lock_guard<std::recursive_mutex> lock(m_metadata_lock);
  return m_gametdb_id;
}

const std::string SConfig::GetTitleName() const
{
  std::lock_guard<std::recursive_mutex> lock(m_metadata_lock);
  return m_title_name;
}

const std::string SConfig::GetTitleDescription() const
{
  std::lock_guard<std::recursive_mutex> lock(m_metadata_lock);
  return m_title_description;
}

u64 SConfig::GetTitleID() const
{
  std::lock_guard<std::recursive_mutex> lock(m_metadata_lock);
  return m_title_id;
}

u16 SConfig::GetRevision() const
{
  std::lock_guard<std::recursive_mutex> lock(m_metadata_lock);
  return m_revision;
}

void SConfig::ResetRunningGameMetadata()
{
  std::lock_guard<std::recursive_mutex> lock(m_metadata_lock);
  SetRunningGameMetadata("00000000", "", 0, 0, DiscIO::Region::Unknown);
}

void SConfig::SetRunningGameMetadata(const DiscIO::Volume& volume,
                                     const DiscIO::Partition& partition)
{
  std::lock_guard<std::recursive_mutex> lock(m_metadata_lock);
  if (partition == volume.GetGamePartition())
  {
    SetRunningGameMetadata(volume.GetGameID(), volume.GetGameTDBID(),
                           volume.GetTitleID().value_or(0), volume.GetRevision().value_or(0),
                           volume.GetRegion());
  }
  else
  {
    SetRunningGameMetadata(volume.GetGameID(partition), volume.GetGameTDBID(partition),
                           volume.GetTitleID(partition).value_or(0),
                           volume.GetRevision(partition).value_or(0), volume.GetRegion());
  }
}

void SConfig::SetRunningGameMetadata(const IOS::ES::TMDReader& tmd, DiscIO::Platform platform)
{
  std::lock_guard<std::recursive_mutex> lock(m_metadata_lock);
  const u64 tmd_title_id = tmd.GetTitleId();

  // If we're launching a disc game, we want to read the revision from
  // the disc header instead of the TMD. They can differ.
  // (IOS HLE ES calls us with a TMDReader rather than a volume when launching
  // a disc game, because ES has no reason to be accessing the disc directly.)
  if (platform == DiscIO::Platform::WiiWAD ||
      !Core::System::GetInstance().GetDVDInterface().UpdateRunningGameMetadata(tmd_title_id))
  {
    // If not launching a disc game, just read everything from the TMD.
    SetRunningGameMetadata(tmd.GetGameID(), tmd.GetGameTDBID(), tmd_title_id, tmd.GetTitleVersion(),
                           tmd.GetRegion());
  }
}

void SConfig::SetRunningGameMetadata(const std::string& game_id)
{
  std::lock_guard<std::recursive_mutex> lock(m_metadata_lock);
  SetRunningGameMetadata(game_id, "", 0, 0, DiscIO::Region::Unknown);
}

void SConfig::SetRunningGameMetadata(const std::string& game_id, const std::string& gametdb_id,
                                     u64 title_id, u16 revision, DiscIO::Region region)
{
  std::lock_guard<std::recursive_mutex> lock(m_metadata_lock);
  const bool was_changed = m_game_id != game_id || m_gametdb_id != gametdb_id ||
                           m_title_id != title_id || m_revision != revision;
  m_game_id = game_id;
  m_gametdb_id = gametdb_id;
  m_title_id = title_id;
  m_revision = revision;

  if (game_id.length() == 6)
  {
    m_debugger_game_id = game_id;
  }
  else if (title_id != 0)
  {
    m_debugger_game_id =
        fmt::format("{:08X}_{:08X}", static_cast<u32>(title_id >> 32), static_cast<u32>(title_id));
  }
  else
  {
    m_debugger_game_id.clear();
  }

  if (!was_changed)
    return;

  if (game_id == "00000000")
  {
    m_title_name.clear();
    m_title_description.clear();
    return;
  }

  AchievementManager::GetInstance().CloseGame();

  const Core::TitleDatabase title_database;
  auto& system = Core::System::GetInstance();
  const DiscIO::Language language = GetLanguageAdjustedForRegion(system.IsWii(), region);
  m_title_name = title_database.GetTitleName(m_gametdb_id, language);
  m_title_description = title_database.Describe(m_gametdb_id, language);
  NOTICE_LOG_FMT(CORE, "Active title: {}", m_title_description);
  Host_TitleChanged();

  const bool is_running_or_starting = Core::IsRunningOrStarting(system);
  if (is_running_or_starting)
    Core::UpdateTitle(system);

  Config::AddLayer(ConfigLoaders::GenerateGlobalGameConfigLoader(game_id, revision));
  Config::AddLayer(ConfigLoaders::GenerateLocalGameConfigLoader(game_id, revision));
  ApplyGameVRConfigOverrides(game_id);

  if (is_running_or_starting)
    DolphinAnalytics::Instance().ReportGameStart();
}

void SConfig::ReloadGameVRConfigOverrides()
{
  std::lock_guard<std::recursive_mutex> lock(m_metadata_lock);
  ApplyGameVRConfigOverrides(m_game_id);
}

void SConfig::OnESTitleChanged()
{
  auto& system = Core::System::GetInstance();
  Pad::LoadConfig();
  Keyboard::LoadConfig();
  if (system.IsWii() && !Config::Get(Config::MAIN_BLUETOOTH_PASSTHROUGH_ENABLED))
  {
    Wiimote::LoadConfig();
  }

  ReloadTextures(system);
}

void SConfig::OnTitleDirectlyBooted(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  if (!Core::IsRunningOrStarting(system))
    return;

  auto& ppc_symbol_db = system.GetPPCSymbolDB();

  if (ppc_symbol_db.LoadMapOnBoot(guard))
    Host_PPCSymbolsChanged();
  HLE::Reload(system);

  PatchEngine::Reload(system);
  WC24PatchEngine::Reload();

  // Note: Wii is handled by ES title change
  if (!system.IsWii())
  {
    ReloadTextures(system);
  }
}

void SConfig::ReloadTextures(Core::System& system)
{
  Pad::GenerateDynamicInputTextures();
  Keyboard::GenerateDynamicInputTextures();
  if (system.IsWii() && !Config::Get(Config::MAIN_BLUETOOTH_PASSTHROUGH_ENABLED))
  {
    Wiimote::GenerateDynamicInputTextures();
  }

  HiresTexture::Update();
}

void SConfig::LoadDefaults()
{
  bBootToPause = false;

  auto& system = Core::System::GetInstance();
  system.SetIsWii(false);
  system.SetIsTriforce(false);

  ResetRunningGameMetadata();
}

// Static method to make a simple game ID for elf/dol files
std::string SConfig::MakeGameID(std::string_view file_name)
{
  size_t lastdot = file_name.find_last_of(".");
  if (lastdot == std::string::npos)
    return "ID-" + std::string(file_name);
  return "ID-" + std::string(file_name.substr(0, lastdot));
}

struct SetGameMetadata
{
  SetGameMetadata(SConfig* config_, Core::System& system_, DiscIO::Region* region_)
      : config(config_), system(system_), region(region_)
  {
  }
  bool operator()(const BootParameters::Disc& disc) const
  {
    *region = disc.volume->GetRegion();
    system.SetIsWii(disc.volume->GetVolumeType() == DiscIO::Platform::WiiDisc);
    system.SetIsTriforce(disc.volume->GetVolumeType() == DiscIO::Platform::Triforce);
    config->m_disc_booted_from_game_list = true;
    config->SetRunningGameMetadata(*disc.volume, disc.volume->GetGamePartition());
    return true;
  }

  bool operator()(const BootParameters::Executable& executable) const
  {
    if (!executable.reader->IsValid())
      return false;

    *region = DiscIO::Region::Unknown;
    system.SetIsWii(executable.reader->IsWii());

    // Strip the .elf/.dol file extension and directories before the name
    SplitPath(executable.path, nullptr, &config->m_debugger_game_id, nullptr);

    // Set DOL/ELF game ID appropriately
    std::string executable_path = executable.path;
    constexpr char BACKSLASH = '\\';
    constexpr char FORWARDSLASH = '/';
    std::ranges::replace(executable_path, BACKSLASH, FORWARDSLASH);
    config->SetRunningGameMetadata(SConfig::MakeGameID(PathToFileName(executable_path)));

    Host_TitleChanged();

    return true;
  }

  bool operator()(const DiscIO::VolumeWAD& wad) const
  {
    if (!wad.GetTMD().IsValid())
    {
      PanicAlertFmtT("This WAD is not valid.");
      return false;
    }
    if (!IOS::ES::IsChannel(wad.GetTMD().GetTitleId()))
    {
      PanicAlertFmtT("This WAD is not bootable.");
      return false;
    }

    const IOS::ES::TMDReader& tmd = wad.GetTMD();
    *region = tmd.GetRegion();
    system.SetIsWii(true);
    config->SetRunningGameMetadata(tmd, DiscIO::Platform::WiiWAD);

    return true;
  }

  bool operator()(const BootParameters::NANDTitle& nand_title) const
  {
    IOS::HLE::Kernel ios;
    const IOS::ES::TMDReader tmd = ios.GetESCore().FindInstalledTMD(nand_title.id);
    if (!tmd.IsValid() || !IOS::ES::IsChannel(nand_title.id))
    {
      PanicAlertFmtT("This title cannot be booted.");
      return false;
    }

    *region = tmd.GetRegion();
    system.SetIsWii(true);
    config->SetRunningGameMetadata(tmd, DiscIO::Platform::WiiWAD);

    return true;
  }

  bool operator()(const BootParameters::IPL& ipl) const
  {
    *region = ipl.region;
    system.SetIsWii(false);
    Host_TitleChanged();

    return true;
  }

  bool operator()(const BootParameters::DFF& dff) const
  {
    std::unique_ptr<FifoDataFile> dff_file(FifoDataFile::Load(dff.dff_path, true));
    if (!dff_file)
      return false;

    *region = DiscIO::Region::NTSC_U;
    system.SetIsWii(dff_file->GetIsWii());

    const std::string& game_id = dff_file->GetGameId();
    if (game_id == DEFAULT_GAME_ID)
    {
      Host_TitleChanged();
    }
    else
    {
      config->SetRunningGameMetadata(game_id);
    }

    return true;
  }

private:
  SConfig* config;
  Core::System& system;
  DiscIO::Region* region;
};

bool SConfig::SetPathsAndGameMetadata(Core::System& system, const BootParameters& boot)
{
  system.SetIsMIOS(false);
  m_disc_booted_from_game_list = false;
  if (!std::visit(SetGameMetadata(this, system, &m_region), boot.parameters))
    return false;

  if (m_region == DiscIO::Region::Unknown)
    m_region = Config::Get(Config::MAIN_FALLBACK_REGION);

  if (system.IsTriforce())
    m_region = DiscIO::Region::DEV;

  // Set up paths
  const std::string region_dir = Config::GetDirectoryForRegion(Config::ToGameCubeRegion(m_region));
  m_strSRAM = File::GetUserPath(F_GCSRAM_IDX);
  m_strBootROM = Config::GetBootROMPath(region_dir);

  return true;
}

DiscIO::Language SConfig::GetCurrentLanguage(bool wii) const
{
  DiscIO::Language language;
  if (wii)
    language = static_cast<DiscIO::Language>(Config::Get(Config::SYSCONF_LANGUAGE));
  else
    language = DiscIO::FromGameCubeLanguage(Config::Get(Config::MAIN_GC_LANGUAGE));

  // Get rid of invalid values (probably doesn't matter, but might as well do it)
  if (language > DiscIO::Language::Unknown || language < DiscIO::Language::Japanese)
    language = DiscIO::Language::Unknown;
  return language;
}

DiscIO::Language SConfig::GetLanguageAdjustedForRegion(bool wii, DiscIO::Region region) const
{
  const DiscIO::Language language = GetCurrentLanguage(wii);

  if (!wii && region == DiscIO::Region::NTSC_K)
    region = DiscIO::Region::NTSC_J;  // NTSC-K only exists on Wii, so use a fallback

  if (!wii && region == DiscIO::Region::NTSC_J && language == DiscIO::Language::English)
    return DiscIO::Language::Japanese;  // English and Japanese both use the value 0 in GC SRAM

  if (!Config::Get(Config::MAIN_OVERRIDE_REGION_SETTINGS))
  {
    if (region == DiscIO::Region::NTSC_J)
      return DiscIO::Language::Japanese;

    if (region == DiscIO::Region::NTSC_U && language != DiscIO::Language::English &&
        (!wii || (language != DiscIO::Language::French && language != DiscIO::Language::Spanish)))
    {
      return DiscIO::Language::English;
    }

    if (region == DiscIO::Region::PAL &&
        (language < DiscIO::Language::English || language > DiscIO::Language::Dutch))
    {
      return DiscIO::Language::English;
    }

    if (region == DiscIO::Region::NTSC_K)
      return DiscIO::Language::Korean;
  }

  return language;
}

Common::IniFile SConfig::LoadDefaultGameIni() const
{
  return LoadDefaultGameIni(GetGameID(), m_revision);
}

Common::IniFile SConfig::LoadLocalGameIni() const
{
  return LoadLocalGameIni(GetGameID(), m_revision);
}

Common::IniFile SConfig::LoadGameIni() const
{
  return LoadGameIni(GetGameID(), m_revision);
}

Common::IniFile SConfig::LoadDefaultGameIni(std::string_view id, std::optional<u16> revision)
{
  Common::IniFile game_ini;
  for (const std::string& filename : ConfigLoaders::GetGameIniFilenames(id, revision))
    game_ini.Load(File::GetSysDirectory() + GAMESETTINGS_DIR DIR_SEP + filename, true);
  if (Common::CaseInsensitiveEquals(id, "GM8E01"))
    ApplyPrimedGunMetroidDefaults(&game_ini);
  return game_ini;
}

Common::IniFile SConfig::LoadLocalGameIni(std::string_view id, std::optional<u16> revision)
{
  Common::IniFile game_ini;
  for (const std::string& filename : ConfigLoaders::GetGameIniFilenames(id, revision))
    game_ini.Load(File::GetUserPath(D_GAMESETTINGS_IDX) + filename, true);
  return game_ini;
}

Common::IniFile SConfig::LoadGameIni(std::string_view id, std::optional<u16> revision)
{
  Common::IniFile game_ini;
  for (const std::string& filename : ConfigLoaders::GetGameIniFilenames(id, revision))
    game_ini.Load(File::GetSysDirectory() + GAMESETTINGS_DIR DIR_SEP + filename, true);
  if (Common::CaseInsensitiveEquals(id, "GM8E01"))
    ApplyPrimedGunMetroidDefaults(&game_ini);
  for (const std::string& filename : ConfigLoaders::GetGameIniFilenames(id, revision))
    game_ini.Load(File::GetUserPath(D_GAMESETTINGS_IDX) + filename, true);
  return game_ini;
}

std::string SConfig::GetGameTDBImageRegionCode(bool wii, DiscIO::Region region) const
{
  switch (region)
  {
  case DiscIO::Region::NTSC_J:
  {
    // Taiwanese games share the Japanese region code however their title ID ends with 'W'.
    // GameTDB differentiates the covers using the code "ZH".
    if (m_game_id.size() >= 4 && m_game_id.at(3) == 'W')
      return "ZH";

    return "JA";
  }
  case DiscIO::Region::NTSC_U:
    return "US";
  case DiscIO::Region::NTSC_K:
    return "KO";
  case DiscIO::Region::PAL:
  {
    const auto user_lang = GetCurrentLanguage(wii);
    switch (user_lang)
    {
    case DiscIO::Language::German:
      return "DE";
    case DiscIO::Language::French:
      return "FR";
    case DiscIO::Language::Spanish:
      return "ES";
    case DiscIO::Language::Italian:
      return "IT";
    case DiscIO::Language::Dutch:
      return "NL";
    case DiscIO::Language::English:
    default:
      return "EN";
    }
  }
  default:
    return "EN";
  }
}
