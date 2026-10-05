# PMJS: Native Runtime for RPG Maker MV/MZ

Run RPG Maker MV/MZ games natively on Linux handhelds without NW.js or a browser. PMJS runs the game's original JavaScript through Node/V8 while native C++ handles rendering, audio, input, canvas operations, and storage.

RPG Maker MV games are essentially web-games, which means to run it, you have to run an entire browser. Low-end handhelds like the RG35XX Plus have a lot of limitations like 1GB of RAM, a really slow CPU, and an SD card for I/O. Not a great combination.
PMJS replaces the browser side with the small set of APIs these games need. There is no DOM layout engine and no JavaScript renderer.


PMJS is experimental and still has a lot of work to be done before I consider it usable. Currently, I'm focusing on getting it to work properly with OMORI on the RG35XX Plus.


## Requirements

- CMake 3.20 or newer
- A C++20 compiler
- Node.js and Node API headers
- pkg-config
- Git and Python 3
- Default x64 Skia65 build: Clang/LLVM 20.1.2 (`clang-20`, `clang++-20`, `llvm-ar-20`, `llvm-strip-20`)
- ARM64 Skia65 build: `PMJS_SKIA65_ARM64_SDK` pointing to the prepared Zig 0.15.2 SDK, plus `llvm-strip-20` 20.1.2
- SDL2, EGL, and OpenGL ES 3.0 or newer
- libpng, libjpeg, FreeType, and HarfBuzz
- FFmpeg libraries: avformat, avcodec, avutil, swresample, and swscale

> [!NOTE]
> I do use AI for this project. No, I am not proud of it. It's still very early in development so it's likely going to be a mess. Don't expect much right now.

### Third-party software

PMJS uses the following software

- [Node.js](https://nodejs.org/) and [V8](https://v8.dev/) to execute game JavaScript
- [SDL2](https://www.libsdl.org/) for windows, input, and platform integration
- [EGL and OpenGL ES](https://www.khronos.org/opengles/) for native rendering
- [libpng](http://www.libpng.org/pub/png/libpng.html) for PNG images
- [libjpeg](https://ijg.org/) for JPEG images
- [FreeType](https://freetype.org/) for glyph rasterization
- [HarfBuzz](https://harfbuzz.github.io/) for text shaping
- [FFmpeg](https://ffmpeg.org/) for audio and video decoding
- [Effekseer](https://effekseer.github.io/) for RPG Maker MZ animation effects
- [Skia](https://skia.org/) and [ICU](https://icu.unicode.org/), with private FreeType/HarfBuzz builds, for the Skia65 text component

The build and test workflow uses CMake, pkg-config, ESLint, and Xvfb.


## On-device performance profiling

Use `tools/profile-device.sh` to profile a real game launch on the target handheld. It only requires a POSIX shell plus Linux `/proc` and `/sys`; optional `perf stat` counters are collected automatically when the kernel permits them.

```sh
# Profile the normal launcher at 1-second intervals.
sh ./tools/profile-device.sh -- ./example/run-game.sh /path/to/game /path/to/saves

# PortMaster-style launcher, sampling twice per second.
sh ./tools/profile-device.sh -i 0.5 -o ./profiles/device-run -- ./MyGame.sh

# On minimal firmware where perf_event_open is unavailable.
sh ./tools/profile-device.sh --no-perf -- ./MyGame.sh
```

The output directory contains:

- `samples.csv`: CPU %, RSS/HWM/PSS, threads, file descriptors, I/O bytes, context switches, free memory/swap, average CPU frequency, detected Mali/GPU devfreq, maximum thermal-zone temperature, and system load over time.
- `summary.txt`: aggregate peak/average values and the game exit code.
- `game.log`: game/launcher stdout and stderr.
- `device.txt`: kernel, CPU, initial memory, devfreq, and thermal metadata.
- `perf-stat.txt`: hardware/software perf counters when `perf` is installed and allowed.

The profiler follows child processes, so it can wrap the existing PortMaster shell launcher rather than requiring the final Node process to be invoked directly.

For ~1 GB handhelds, PMJS also has an opt-in resource profile. It lowers only cache residency; rendering and game semantics are unchanged. Explicit cache variables still override the profile.

```sh
export PMJS_RESOURCE_PROFILE=low
# Defaults under the low profile:
#   image warm cache: 2 MiB
#   transient image CPU readbacks: 30 frames (default profile: 60)
#   premultiply scratch retention: 1 MiB (default profile: unbounded/eager reuse)
#   canvas upload scratch retention: 2 MiB (default profile: unbounded/eager reuse)
#   glyph cache:      4 MiB / 2048 entries
#   prepared audio:   4 MiB cache, 1 MiB per asset, 128 KiB sync-decode ceiling
#   stream buffer:     16000 frames (~333 ms at 48 kHz; default profile: 24000)
#
# Optional per-game overrides:
export PMJS_IMAGE_WARM_CACHE_BYTES=2097152
export PMJS_IMAGE_CPU_PIXEL_FRAMES=30
export PMJS_IMAGE_PREMULTIPLY_SCRATCH_RETAIN_BYTES=1048576
export PMJS_CANVAS_UPLOAD_SCRATCH_RETAIN_BYTES=2097152
export PMJS_GLYPH_CACHE_MAX_BYTES=4194304
export PMJS_GLYPH_CACHE_MAX_ENTRIES=2048
export PMJS_AUDIO_CACHE_BYTES=4194304
export PMJS_AUDIO_MAX_ASSET_BYTES=1048576
export PMJS_AUDIO_MAX_SYNC_BYTES=131072
export PMJS_AUDIO_STREAM_BUFFER_FRAMES=16000
```

For RPG Maker MV ports, several runtime A/B switches are available for device-specific bottlenecks:

```sh
# Load a port-owned patch after guest plugins/PMJS wrappers, before js/main.js.
export PMJS_PORT_SCRIPT=js/fnh-port.js

# Coalesce expensive map refreshIfNeeded() work. Default behavior is unchanged
# unless this variable is set; 3 or 4 are useful A/B values on 60 Hz games.
export PMJS_REFRESH_COALESCE_TICKS=3

# Reuse Game_Map.events() results inside one updateScene call.
export PMJS_CACHE_MAP_EVENTS=1

# Cache Game_Switches/Game_Variables value() results for one updateScene tick.
# setValue() invalidates the affected entry immediately.
export PMJS_CACHE_GAME_VALUES=1

# Avoid rescanning the same immutable event-command list for image prefetch on
# every interpreter setup. Entries are periodically rescanned.
export PMJS_CACHE_REQUEST_IMAGES=1
export PMJS_REQUEST_IMAGES_CACHE_MS=5000

# Batch image-cache truncation after decode bursts instead of sorting the full
# cache after every completion wave. Default 0 preserves immediate trimming.
export PMJS_IMAGE_CACHE_TRIM_DELAY_MS=100

# Emit a breakdown only for unusually slow frames/transitions.
export PMJS_TRANSITION_PROFILE=1
export PMJS_TRANSITION_PROFILE_MS=80
```

All logic/cache fast paths above are opt-in. This makes it possible to A/B them independently on plugin-heavy games before promoting any of them into a port's normal launcher. The transition profiler records only frames above the configured threshold and is intended for map loads, scene changes, large tilemap rebuilds, and image-cache spikes.

### Port scripts (runtime-side fixes without game file edits)

`PMJS_PORT_PRE_SCRIPT` loads a port-owned script immediately before guest plugin evaluation; use it for configuration globals or compatibility shims that a plugin reads only while it is loading. `PMJS_PORT_SCRIPT` runs after guest plugins and PMJS method wrappers but before `js/main.js`; use it for runtime patching or configuring shared PMJS plugin capabilities. Both run in the game's context and avoid changing original game files. A worked post-plugin example ships as `example/port-script-fnh.js` (Fear & Hunger): it sets the game plugin's runtime config channel and configures the shared Terrax capability with `PMJS.plugins.terraxLighting.configureMaskScale(0.25)`. The shared adapter owns bitmap replacement, primitive-recorder setup, and retained-sprite scaling, avoiding port-specific prototype races while keeping the game files untouched. Measured on a TrimUI Smart Pro with stock game files plus the port's install patches: the mask's per-update cost dropped from ~14.3 ms to ~6.7 ms (`Spriteset_Map.update` average), the mask texture upload from 2.18 MB to 137 KB, and the game-logic ratio (game time / wall time) returned to ~1.0 while walking.
