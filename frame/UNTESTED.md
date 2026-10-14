# Built but not confirmed in the headset

One line per claim. The documents carry `[to confirm in the headset]` at the first group. When I have tested one, I delete its line here and complete the text in README.md, docs/RELEASE_NOTES_v1.0.0.md and docs/TECHNICAL_NOTES.md.

## Marked in the documents

- Starting from the title screen.
- Loading and saving the memory card save, and the card kept between launches.
- The controls under SteamVR's automatic binding to the Frame controllers: aiming, the visor, the arm cannon, jump, scan, missiles, map.
- Setting the height with the right stick, and opening the in-VR menu with the left thumbstick; Exit Game leaving cleanly and writing the shader caches.
- Sharpness against the container setup.

## Other things not tested

- Other discs and regions than the US disc (GM8E01, revision 0).
- Sessions longer than 15 minutes.
- The pipelined loop (`PRIMEFRAME_PACING=pipelined`): it showed doubled images in the headset. Kept as a switch, not the default.
- The HUD as a composition layer of its own: only a test ring exists (`PRIMEFRAME_HUD_TEST=1`).
- The drain and inline loops (`PRIMEFRAME_PACING=drain` and `inline`) as fallbacks.
- The `PRIMEFRAME_*` switches other than the defaults, and `session.env`.
- The installer on a clean Frame with no existing user folder, and updating over an install.
- `uninstall.sh`.
- `collect-logs.sh`.
