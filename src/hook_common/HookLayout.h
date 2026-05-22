#pragma once

#include <cstdint>

namespace PrimedGun::HookLayout {

struct Range {
    uint32_t begin;
    uint32_t end;
    const char* owner;
};

constexpr bool Overlaps(const Range& a, const Range& b) {
    return a.begin < b.end && b.begin < a.end;
}

// MEM1 scratch owned by PrimedGun. Keep each feature in a named block so new
// hooks do not accidentally reuse cannon/reticle control fields.
inline constexpr uint32_t ScratchBase = 0x817FE000u;
inline constexpr uint32_t ScratchEnd = 0x817FF000u;

inline constexpr uint32_t CannonBasisScratch = ScratchBase + 0x000u;          // 9 floats
inline constexpr uint32_t CannonExpectedGunScratch = ScratchBase + 0x038u;    // u32
inline constexpr uint32_t ModelOffsetWorldScratch = ScratchBase + 0x040u;     // 3 floats
inline constexpr uint32_t AdjustedGunPosScratch = ScratchBase + 0x050u;       // 3 floats
inline constexpr uint32_t ProjectileDebugScratch = ScratchBase + 0x100u;      // debug only
inline constexpr uint32_t ProjectileProbeScratch = ScratchBase + 0x300u;      // debug only
inline constexpr uint32_t GunTargetScratch = ScratchBase + 0x400u;            // player + uid
inline constexpr uint32_t ReticleBillboardScratch = ScratchBase + 0x500u;     // enabled + basis
inline constexpr uint32_t ProjectileTimingScratch = ScratchBase + 0x600u;     // reserved
inline constexpr uint32_t DpadPressScratch = ScratchBase + 0x680u;            // requested ControlMapper command

inline constexpr Range ScratchRanges[] = {
    {CannonBasisScratch, CannonBasisScratch + 0x048u, "cannon transform hook"},
    {ModelOffsetWorldScratch, ModelOffsetWorldScratch + 0x00Cu, "model offset"},
    {AdjustedGunPosScratch, AdjustedGunPosScratch + 0x00Cu, "adjusted gun position"},
    {ProjectileDebugScratch, ProjectileDebugScratch + 0x200u, "projectile debug"},
    {ProjectileProbeScratch, ProjectileProbeScratch + 0x080u, "projectile probe debug"},
    {GunTargetScratch, GunTargetScratch + 0x008u, "gun target hook"},
    {ReticleBillboardScratch, ReticleBillboardScratch + 0x028u, "reticle billboard hook"},
    {ProjectileTimingScratch, ProjectileTimingScratch + 0x080u, "projectile timing hook reserve"},
    {DpadPressScratch, DpadPressScratch + 0x010u, "XR D-pad press hook"},
};

// Low MEM1 PPC caves. Static app-owned AR code caves and injected DLL dynamic
// hook caves share this area, so every block must be explicitly reserved.
inline constexpr uint32_t AppArCaveStart = 0x80001800u;
inline constexpr uint32_t AppArCaveEnd = 0x80001E00u;
inline constexpr uint32_t DllCaveStart = 0x80001B00u;
inline constexpr uint32_t DllCaveEnd = 0x80001F00u;

inline constexpr Range AppArCaveRanges[] = {
    {0x80001840u, 0x800018C0u, "cannon rotation hook A"},
    {0x80001900u, 0x80001930u, "gun ray target hook"},
    {0x80001940u, 0x80001978u, "visor effect hook"},
    {0x80001980u, 0x800019A4u, "frustum culling hook"},
    {0x800019C0u, 0x80001A10u, "cannon rotation hook B"},
    {0x80001A20u, 0x80001ACCu, "reticle hook"},
    {0x80001D00u, 0x80001DA0u, "beam projectile timing hook"},
    {0x80001DA0u, 0x80001DE8u, "XR visor D-pad timing hook"},
};

inline constexpr uint32_t FirstPersonPitchLoadCave = 0x80001B70u;
inline constexpr uint32_t RenderModelOffsetCave = 0x80001C00u;
inline constexpr uint32_t FirstPersonElevationPitchCave = 0x80001C80u;
inline constexpr uint32_t CombatPitchCave0 = 0x80001E00u;
inline constexpr uint32_t CombatPitchCave1 = 0x80001E40u;
inline constexpr uint32_t CombatPitchCave2 = 0x80001E80u;
inline constexpr uint32_t CombatElevationPitchCave = 0x80001EC0u;

inline constexpr Range DllCaveRanges[] = {
    {FirstPersonPitchLoadCave, FirstPersonPitchLoadCave + 0x020u, "first-person pitch load"},
    {RenderModelOffsetCave, RenderModelOffsetCave + 0x04Cu, "render model offset"},
    {FirstPersonElevationPitchCave, FirstPersonElevationPitchCave + 0x034u,
     "first-person elevation pitch"},
    {CombatPitchCave0, CombatPitchCave0 + 0x030u, "combat pitch hook A"},
    {CombatPitchCave1, CombatPitchCave1 + 0x030u, "combat pitch hook B"},
    {CombatPitchCave2, CombatPitchCave2 + 0x030u, "combat pitch hook C"},
    {CombatElevationPitchCave, CombatElevationPitchCave + 0x034u,
     "combat elevation pitch hook"},
};

} // namespace PrimedGun::HookLayout
