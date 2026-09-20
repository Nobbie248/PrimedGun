# Prime's GUI-camera draw calls and the invisible helmet — 2026-09-20

Follow-up to the ~800 GUI-camera draws per frame noted in `Quest-Open-Area-Performance.md`.
This note records what those draws are made of (from the game's code, then measured), why the
hidden helmet still costs a full draw of its models, and the source-level skip that removes it.
The HUD itself and the minimap are left intact on purpose.

## How Prime builds the HUD family

Everything in the family is drawn by `CInGameGuiManager::Draw` (Rev 0: `0x80108754`), after the
world and gun passes. Each element is a `CGuiFrame`, and `CGuiFrame::Draw` (`0x802C2734`) starts
by drawing the frame's `CGuiCamera`, which loads that frame's own projection: a perspective one
with a 4096 far plane for the HUD frames. That projection is what the video-side element
classifier keys on ("GUI camera": far 4096, near 1, about 82x65 degrees), so one classifier
"segment" is one frame camera load. In draw order (PrimeDecomp `CInGameGuiManager.cpp`):

1. Targeting manager, face plate decoration, player visor effects and the Samus reflection
   (`x40_samusReflection`), only while the pause blur is not covering the game.
2. `CSamusHud::Draw` (`0x80065964`): the selected per-visor HUD frame (energy, missiles, threat,
   visor and beam menus...), the base HUD frame, the decoration interface, then the radar dots.
3. The automapper minimap: it borrows the **base HUD frame's camera** (`camera->Draw`), then
   draws the minimap frame model and `CAutoMapper::Draw` (`0x80099004`).
   `CMapWorld::DrawAreas` sorts every visible map surface, door and object by depth and draws
   them one by one: `CMapArea::CMapAreaSurface::Draw` issues one primitive per surface polygon
   list plus one line strip per outline (twice when the outline is thick), each with its own
   `GXSetTevKColor`, so a room with many map surfaces alone accounts for hundreds of draws.
4. **The helmet**: `CSamusHud::DrawHelmet` (`0x80065DC0`, size `0xA8`) draws the helmet
   `CGuiFrame` (`FRME_Helmet`: helmet, glow and helmet-light models under a pivot widget) with
   its own camera, whenever the player is not in morph ball and not in a cinematic.
5. Save UI, message screen, pause screen and the final camera filter.

So in a first-person gameplay frame the helmet frame is the **last** GUI-camera item; only the
pause, save, log book, map and message screens draw after it.

## Why "helmet opacity 0" still draws the helmet

PrimedGun hides the helmet by writing 0 to `CGameOptions::x64_helmetAlpha` every 60 frames
(`ApplyHelmetOpacityZero` in `NativeRuntime.cpp`), which is the game's own Helmet Opacity option.
The game applies that option in `CHudHelmetInterface::UpdateHelmetAlpha` (`0x80190D84`):

```cpp
x44_BaseWidget_Pivot->SetColor(CColor::White().WithAlphaOf(gpGameState->GameOptions().GetHelmetAlpha()));
```

It only recolours the pivot widget. `CGuiModel::Draw` (`0x802C40D8`) multiplies that colour into
`CModelFlags::AlphaBlended(col)` and calls `CModel::Draw` unconditionally; neither it nor
`CCubeModel::Draw` has a zero-alpha early-out, and the widgets stay visible
(`CHudHelmetInterface::UpdateVisibility` only looks at the debug and game visibility flags).
Every surface of the helmet, glow and light models is therefore still converted, uploaded and
rasterised at alpha 0 by Dolphin and the GPU, exactly as on real hardware when a player sets the
option to 0%.

## The skip

`UpdateHelmetDrawSkipPatch` writes `blr` over the first instruction of `CSamusHud::DrawHelmet`
(`stwu r1, -0x20(r1)`, verified in the Rev 0 DOL: the function's only `bl` lands on
`CGuiFrame::Draw`). No cave, no scratch. It is applied from `OnFrameEnd` when all of these hold:

- built-in patches are enabled (`builtin_patches_enabled`),
- the helmet is disabled (`visor_helmet_enabled = False`, the existing "Enable visor helmet"
  toggle) and the new `visor_helmet_skip_hidden_draw` is on (default on, `PrimedGun.ini`
  `[Runtime]`, no UI; set it to False for an A/B against the zero-alpha draw),
- a player exists and it is not in a menu, the map or morph ball (`PlayerIsInMenuMapOrMorphball`).

Outside those states the original word is restored, and the word is re-read every frame so a
savestate carrying either version is corrected on its next frame. The restore in menus is
deliberate: the pause, save and message screens draw after the helmet, and the element
classifier's Prime 1 rules count GUI-camera draws in sequence, so removing the helmet there would
renumber those screens' layers. In first-person play nothing draws after the helmet, so the HUD
and minimap layers are untouched.

## How this was measured

The numbers below come from a temporary probe in `VertexManagerBase` (`[VR] HudDrawProbe`),
**removed again once the questions were answered**; nothing of it is in the tree. It is written
down here because rebuilding it is the only way to repeat these measurements in another room,
and because two of its details were wrong on the first two attempts.

What it did, in `Flush` under a config flag, for every perspective draw with a 4000-4200 far
plane:

- Grouped consecutive draws sharing a projection and viewport into "segments" (one segment is
  one `CGuiFrame` camera load), and per segment counted draws, indices, classifier layers, and
  a blend/depth state key (`alpha` = src alpha / inv src alpha, `add` = src alpha / one,
  `opaque`, plus `zt`, `zge`, `zw`, `at`, `nocol`).
- Recognised the minimap by its GEqual depth test and the window quad as the family's only
  depth-writing draw, projected each batch to NDC, and compared the two boxes.
- Logged a summary every 60 frames at Video Info level and dumped three frames draw by draw to
  `Dump/Shaders/GM8E01/primedgun_hud_probe.log`.

Two traps in the batch projection, both of which silently produced "behind camera" for every
draw until fixed:

- `m_base_buffer_pointer` is the start of the backend's whole vertex stream buffer, not of the
  pending batch. The batch is the last `m_index_generator.GetNumVerts() * stride` bytes before
  `m_cur_buffer_pointer`.
- The position matrix index must come from `g_main_cp_state.matrix_index_a.PosNormalMtxIdx`
  (what `VertexShaderManager` uses), not from `xfmem.MatrixIndexA`. Per-vertex indices, when the
  declaration has `posmtx`, override it.

The game was booted straight into the scene from adb, which the Android port now supports
(`EmulationActivity` gained a `SavestatePath` string extra, the counterpart of the Qt build's
`-s`); that part was kept:

```
adb shell am broadcast -a com.oculus.vrpowermanager.prox_close
adb shell am start -n org.primedgun.primedgun.quest.debug/org.dolphinemu.dolphinemu.activities.EmulationActivity \
  -c com.oculus.intent.category.VR \
  --esa SelectedGames '<content URI from primedgun_selected_game in shared_prefs>' \
  --es SavestatePath /sdcard/Android/data/org.primedgun.primedgun.quest.debug/files/StateSaves/GM8E01.s01
```

## Measurements (Quest 3, heavy-area savestate `GM8E01.s01`, 2026-09-20)

Three boots into the same savestate, nobody touching the controls, 3x internal resolution,
head cull on. "Frame draws" is the probe's GUI-camera count plus its "other draws" count.

| Configuration | GUI-camera draws | GUI indices | Frame draws | GPU busy (640 MHz) | FPS |
| --- | --- | --- | --- | --- | --- |
| Helmet enabled (drawn) | 442 | 18,537 | ~1,834 | not sampled | ~48.1 |
| Helmet disabled, skip on (default) | 420 | 15,258 | ~1,810 | 92.4% | ~49.3 (two intervals only) |
| Helmet disabled, skip off (alpha-0 draw) | 442 | 18,537 | ~1,834 | 92.9% | ~48.1 |

The whole family is **one projection segment** (82.4 x 65.0 degrees, near 1.0, far 4096,
viewport 320x224): every HUD frame, the minimap and the helmet frame use the same camera
parameters and are drawn back to back, so the segment count cannot separate them. The
draw-by-draw dump (`hud_probe_helmet_on.log`, kept with the other capture artifacts) does:

| Part of the 442 | Draws | Indices | How it is recognised |
| --- | --- | --- | --- |
| HUD frames (selected visor HUD, base HUD, decorations, radar) | ~66 | ~4,000 | Textured triangles, 14 additive glow quads first, then small alpha-blended widgets |
| Minimap (`CMapWorld::DrawAreas`) | ~354 | ~11,200 | Strict alternation of one alpha-blended triangle fill and one to eight line strips per map surface, 232 line-strip draws in all, one texture (`e58bb0d39c821c79`) and one pixel shader (`7bc725ca`) |
| Helmet frame (`DrawHelmet`) | 22 | 3,279 | Last 22 draws: 2 additive, 13 alpha-blended surfaces (2,787 indices), 7 additive glow draws; gone with the skip |

So the invisible helmet was 5% of the GUI-camera draws and 18% of their indices but only
about 1.2% of the frame's draw calls, and its removal is not visible in GPU busy at this
GPU-bound operating point; treat the FPS difference as indicative, not measured. The minimap is
80% of the family's draw calls (about 20% of the whole frame) and is left as is, as asked.

All 442 draws are classified `Scan Text` (`METROID_SCAN_TEXT`): the Prime 1 rules match this
projection by shape (`CompactHudProjection`, 78-84 x 56-68 degrees at far 4096) before any of
the sequence-counted layers, so `METROID_HELMET`, `METROID_HUD`, `METROID_MAP` and friends never
occur in first-person play and the family is placed with the Scan Text settings. A video-side
skip of the helmet would therefore have needed a texture or draw-position rule; the game-side
skip needs none, and the menu restore described above is more conservative than the classifier
requires.

## Is the minimap culled? (same savestate, 2026-09-20)

Not by the game, and it does not need to be in this scene. `CMapWorld::DrawAreas` pushes every
surface of every area within the BFS hop depth into its depth-sorted list with no frustum or
window test. The window itself is a depth trick, not a scissor: `CInGameGuiManager::Draw`
switches to depth range 0 to 1/512, draws the base HUD frame's `model_automapper` quad with
depth write, then draws the map with `SetDepthWriteMode(true, kE_GEqual, false)`: GEqual test,
no write, full 640x448 viewport. Map pixels outside the quad fail against the world's depth,
so anything outside is transformed, rasterised and rejected per pixel.

Measured by projecting each map batch and the window quad to NDC, in the heavy-area scene:

| Measurement | Value |
| --- | --- |
| Window quad, NDC | x 0.44 to 0.78, y 0.43 to 0.83 (17% x 20% of the viewport, top right) |
| Minimap draws (GEqual) | 358 draws, 11,217 indices |
| Union of all minimap geometry, NDC | x 0.45 to 0.74, y 0.42 to 0.78 |
| Draws entirely outside the window | 1 (a 24-index outline), none off screen |

So 357 of 358 draws overlap the window: the minimap camera (`GetDesiredMiniMapCameraDistance`,
zoomed to the current area) frames the submitted geometry inside the window and the deeper
neighbours already drop out through their zero alpha (`CMapAreaSurface::Draw` skips fills and
outlines whose colour alpha is 0). A visibility cull would recover nothing here. The cost is
structural: about 354 draws averaging 31 indices, one fill plus one to eight line strips per
map surface, each preceded by its own konst-colour register write. Only merging consecutive
map draws (same pipeline, texture, matrix and vertex array; only `GX_KCOLOR0` and the primitive
type change) could shrink it, and that needs the konst colour moved per vertex or per draw
instance, which Dolphin's draw path does not do today. Other rooms may differ: a hub with many
neighbours at hop depth 1 could spill past the window, and checking that needs the probe above
rebuilt.

## Follow-ups

- `ApplyHelmetOpacityZero` only ever writes 0. Re-enabling the helmet in PrimedGun leaves the
  game's Helmet Opacity option at whatever the save or savestate carries, so a helmet enabled in
  the mod can still be invisible until the player raises the option in Prime's own menu. The
  Quest profile used here had the helmet enabled and the runs above cannot tell whether it was
  actually visible; a restore of the option (to 255 or a remembered value) on re-enable would
  make the toggle mean what it says.
- The minimap's 350-odd tiny draws all share one pipeline, texture and vertex array; only the
  konst colour and the primitive change between them. If the family's CPU cost matters later,
  merging consecutive minimap draws on the video thread is the only large target left in it.
