# xrmock

A mock OpenXR runtime for testing the frame loop of `xr_app.cpp` without a
headset, and a script that compares what two builds do against it.

- `build.sh [out dir]` builds `libxrmock.so` and `runtime.json` (select it with
  `XR_RUNTIME_JSON=<out>/runtime.json`). Needs clang and the OpenXR headers of
  `third_party/openxr`.
- `trace-compare.sh <trace A> <trace B>` compares two traces of `frame/tools/mockrun.sh` runs
  (`MOCK_XR_TRACE`): the setup line by line, the frames as the share of compositor cycles
  following each call pattern (a game run's timing differs from run to run).
- `xrmock.c` is the runtime (frame clock, call-order checks, trace, settings
  by `MOCK_XR_*` environment variables, listed in its header comment).
- `xrmock_gfx.h` is the seam to the graphics API; `xrmock_gles.c` is the GLES
  side (swapchain images are textures of the app's EGL context).

## Vulkan

`xrmock_vk.c` is the Vulkan side (`XR_KHR_vulkan_enable2`): the app's instance
and device are made through the mock with the app's own loader, swapchain
images are VkImages on the app's device, handed out in
`COLOR_ATTACHMENT_OPTIMAL`. The game always binds Vulkan.

Checks, as `ERROR` lines in the trace and a summary on stderr at the end of
the session (`xrmock: summary ...`):

- acquire, wait and release in order per swapchain (both APIs);
- Vulkan: no swapchain with mip levels (SteamVR on the Frame allocates
  memory for level 0 only; writing the others corrupted other images);
- Vulkan, with `MOCK_XR_BUSY_MS=<ms>`: exact where the swapchain memory can be
  host visible (the Frame): a sample of the released image's bytes is taken at
  the release, an empty submission marks the queue, and once the GPU passed the
  mark the sample is taken again; a change means the GPU was still writing the
  image when it was released. Unrelated work in the queue does not count.
  Elsewhere, at each release the mock waits for the
  app's queue to go idle; longer than that means work was still queued. Off
  by default: the app records and submits the next set before it releases
  the last (its own check, per image against the timeline, logs
  "released after a wait in the release itself"). `PETARI_NO_GPU_WAIT=1` in
  the app is the negative test of the app's check;
- both eyes of a projection layer come from one `xrLocateViews` (the head
  turns slowly, so views located for different times differ), and how many
  refreshes before the display time submitted that locate was made for.

A Vulkan session makes one set of eye swapchains (three with GLES) and
transfer usage on the UI layers, so its setup trace differs from a GLES one;
`trace-compare.sh` stays a GLES-against-GLES check.

Game additions: `XR_KHR_convert_timespec_time` (with `MOCK_XR_TIME_OFFSET_S` XrTime is
not CLOCK_MONOTONIC), the recommended eye size of the Frame (1728x1728, `MOCK_XR_EYE`),
`xrRequestDisplayRefreshRateFB` honoured before the session begins unless `MOCK_XR_HZ` is set,
focus blips (`MOCK_XR_FOCUS_BLIP=<ms>[@<s>]`), a current interaction profile
(`MOCK_XR_PROFILE`), and a second summary line counting frames that showed newly released eyes
(and for which display time their one locate was made) apart from repeated layers.

PrimedFrame additions (headset session H1, doubled images when turning the head):

- `MOCK_XR_STAMP=1` (mockrun `--stamp`, which also sets the game's `PRIMEDFRAME_FRAME_STAMP=1`): the
  game writes into a 256x20 block of each eye image the oldest and the newest locate (the display
  time it was made for) whose poses the draws of the XFB shown used; the mock copies the block out
  at every release of a colour-attachment swapchain and, for every projection layer, compares both
  with the locate the layer's poses came from ("xrmock: stamps"; any image drawn with another
  locate, or with several, is an ERROR). With `ImmediateXFBEnable = False` it fails as it must
  (the XFB shown is a game frame older than the pose submitted with it).
- `MOCK_XR_HEAD_DPS=<deg/s>` and `MOCK_XR_HEAD_DEG=<deg>`: the head's peak yaw speed and amplitude
  (default 54 deg/s, 17.2 deg); the head also rises 0.1 mm a second so no two locates share a pose.
- `MOCK_XR_LEAD=<refreshes>`: predictedDisplayTime this many refreshes after the wake-up (default 3,
  SteamVR's 25 to 27 ms on the Frame; it was 2).
- "xrmock: end times": every xrEndFrame against the predicted display time of the xrWaitFrame its
  xrBeginFrame followed (an ERROR when they differ).
