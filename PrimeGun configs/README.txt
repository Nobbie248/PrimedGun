PrimeGun fallback Dolphin configs

PrimeGun normally asks Dolphin where its active config folder is and writes controller bindings automatically.
If PrimeGun shows "Failed to locate local Dolphin configs", copy only this file:

Config\GCPadNew.ini

Copy it into the Config folder inside the Dolphin user folder your build is using.

Do not replace your whole Dolphin folder. This fallback only provides the OpenXR GameCube controller map.

PrimeGun does not write Dolphin emulator/game settings. Set these manually in Dolphin:
- Enable OpenXR / VR.
- Enable background input.
- For Metroid Prime GM8E01 VR config, set Units Per Meter to 1.50.
- For Metroid Prime GM8E01 VR config, set Camera Forward to 0.0.
- Recommended game setting: Dual Core on.

After copying, restart Dolphin and PrimeGun.
