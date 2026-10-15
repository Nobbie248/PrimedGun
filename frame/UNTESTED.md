# Built but not confirmed in the headset

One line per claim. When I have tested one in the headset, I delete its line here.

## Not tested

- Other discs and regions than the US disc (GM8E01, revision 0).
- Sessions longer than 15 minutes.
- The pipelined loop (`PRIMEDFRAME_PACING=pipelined`): it showed doubled images in the headset. Kept as a switch, not the default.
- The HUD as a composition layer of its own: only a test ring exists (`PRIMEDFRAME_HUD_TEST=1`).
- The drain and inline loops (`PRIMEDFRAME_PACING=drain` and `inline`) as fallbacks.
- The `PRIMEDFRAME_*` switches other than the defaults, and `session.env`.
- The installer on a clean Frame with no existing user folder, and updating over an install.
- `uninstall.sh`.
- `collect-logs.sh`.
