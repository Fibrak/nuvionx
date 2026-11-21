# Changelog

All notable changes to NuvioNX are documented here.

## [0.1.5] - 2026-11-21

Playback is now spread across the Switch's CPU cores, pacing is vsync-locked and smooth, and
buffering a seek no longer pegs a single core. (Includes everything from 0.1.3 and 0.1.4, which
were never published to GitHub — if you're updating from 0.1.2 see the note at the bottom.)

### Added
- **Multithreaded GPU video rendering.** A dedicated thread now owns mpv's OpenGL context and
  renders each video frame into off-screen textures on its own CPU core; the main thread only
  composites the finished frame and draws the UI. The per-frame upload/convert no longer competes
  with input, UI and present on one core. Falls back to the single-thread GPU path, then the
  software path, if the off-screen targets can't be created.
- **Parallel segmented buffering.** On a fast connection, seeking to an unbuffered spot used to
  pour the whole read-ahead burst through one relay connection on a single core (TLS + copies).
  For progressive files the relay now pulls the byte range in 2 MiB chunks over two parallel
  connections and forwards them to the player in order, spreading the burst across cores. Capped
  at two connections to stay within the console's SSL-session budget; HLS, bounded ranges and
  range-less origins keep the single-stream path.
- **Async GPU texture upload (`opengl-pbo`).** Each decoded frame is uploaded to the GPU through
  an asynchronous pixel buffer (DMA) instead of a blocking CPU copy.
- **CPU-work readout in the perf overlay** (toggle with `ZL`+`ZR`): a true per-frame CPU time,
  separate from the vsync-padded wall-clock frame time, so compute-bound and vsync-bound frames
  can be told apart.

### Changed
- **Smooth, power-saving frame pacing during playback ("movie mode").** Steady playback is now
  paced by the GPU swap interval at a hardware-locked **30 fps** (even divisor of the panel's
  60 Hz, so motion stays smooth and the SoC stays cool/quiet); the instant the controls appear or
  you interact, it snaps to **60 fps**. High-frame-rate content (48/50/60) always runs at 60.
- **Lower CPU while browsing menus.** Ellipsised labels (card titles, rows) were re-measured with
  the font engine every frame; the result is now cached per label, so the measuring happens once
  instead of on every frame on the one UI core.
- **Hardware decode requests `nvtegra-copy` directly.** Verified against the bundled mpv/ffmpeg
  that true zero-copy (`nvtegra`) has no GL-interop in this build and always fell back to the copy
  path, so the dead probe was removed.

### Fixed
- **Frame pacing halving.** The earlier movie-mode cap slept to hit the target rate *and* waited
  on vsync; the two fought and halved the rate (a 30 fps target played at 15). Pacing is now done
  purely with the vsync swap interval.

## [0.1.4] - 2026-11-21

### Added
- **Font fallback for CJK / Korean / symbols.** Text from sources that used characters the
  standard font lacks (Chinese, Korean, many symbols) showed "tofu" boxes. The app now loads
  the Switch's other system fonts and picks a covering font per character, compositing the
  runs (cached, so no per-frame cost). Note: the Switch has no colour-emoji font, so genuine
  emoji still can't render without bundling one.

### Changed
- **Don't render while off screen.** When the HOME menu is open or the screen is dimming
  toward sleep, the app stops drawing entirely (background work and playback state still run),
  so the GPU/SoC can idle — cooler and less battery.
- **Lower power use when idle.** The UI now drops to 30 fps after ~2 s with no input or
  background activity, and 15 fps after ~10 s, instead of always rendering at 60. Any
  input, background result, or toast snaps it back to 60 fps instantly; video playback is
  never throttled. Saves battery and heat while browsing menus.

### Added
- **GPU video rendering (experimental)** — Settings → *GPU video*. Renders video
  through mpv's OpenGL path so the Tegra GPU does the YUV→RGB convert and scaling
  instead of the CPU. The software path overloaded the CPU at 1080p and dropped
  frames; this offloads that work to the (otherwise idle) GPU. mpv runs on its own
  GL context that shares objects with SDL's, so mpv resetting GL state to defaults
  on each frame can't corrupt the SDL-rendered UI. Off by default; falls back to the
  software renderer automatically if a shared GL context can't be created. With GPU video
  on, decoding also attempts true zero-copy (`hwdec=nvtegra`, no GPU→RAM→GPU round trip),
  falling back to the copy path if the interop isn't available.

### Fixed
- **Hard crash (data abort / kernel panic) after a while during playback.** The
  0.1.2/0.1.3 attempt at smoother video called `mpv_render_context_report_swap()`
  from the main thread while `mpv_render_context_render()` ran on the background
  render thread. mpv requires both on the same thread; the race corrupted mpv's
  display-sync state and then the heap, crashing later inside libc's number/locale
  formatting code. Reverted to mpv's default audio sync (the pre-0.1.2 behaviour).
  Smoothness tuning will be revisited in a way that fits the threaded renderer.

## [0.1.3] - 2026-11-21

### Fixed
- **In-app update failing with "Failed writing received data to disk/application."**
  The HTTP layer capped every response at 24 MiB, but the update binary is ~30 MB,
  so the download was aborted just as it finished. The updater now buffers up to
  48 MiB; the cap stays 24 MiB for normal API/catalog/image traffic.
  - Note: builds 0.1.2 and earlier carry the old cap, so their in-app updater can't
    fetch this fix — install 0.1.3 once from the SD zip, and future updates work
    from inside the app.

## [0.1.2] - 2026-11-21

### Added
- **In-app updater.** A new *Software update* row in Settings checks GitHub for the
  latest release, downloads `nuvionx.nro`, verifies it (NRO0 magic), writes it over
  the running binary, and chain-reloads into the new version on exit — no PC needed.
- **Hide 4K / above-1080p streams.** A Settings toggle filters out streams tagged
  2160p/4320p/4K/8K/UHD/1440p from every source (scans name, title, description and
  filename). Auto-play honours the same filter.
- **Animated home screen.** Continue Watching is ordered most-recently-watched first
  and each card pops in one at a time, in order, revealing only once its own artwork
  has decoded. The hero backdrop + logo wait for the first entry to be fully ready.
- **Sequential intro.** On launch the top bar animates in piece by piece
  (NUVIO NX → Home → Search → Library → clock/battery); catalog rows and the Library
  grid use the same staggered pop-in.
- **"Loading…" wave.** While Continue Watching and the first hero load, an accent-colour
  "Loading…" with each letter bobbing in a wave sits in the Continue Watching title
  slot; it fades out and the real title takes its place once the first entry is ready.

### Changed
- **FPS capped at 60.** Frame pacing now sleeps to hold a steady 60 fps and never above.
- **Smoother playback.** mpv uses `video-sync=display-resample` and reports swaps to the
  render context for display-synced 1080p output.
- **Faster poster loading.** Image decode worker threads raised from 3 to 5.

### Fixed
- Removed the ~1% sliver of the next/previous row that leaked in at the top and bottom
  edges while scrolling (tightened row clipping, no overscan).
- Poster corner cut-outs now fade in together with the poster, so cards no longer flash
  opaque black corners before their image loads.
- Continue Watching pop-in no longer replays when the last entry appears — reveal timing
  is keyed by item id and survives row rebuilds as addons finish loading.

## [0.1.1]
- Fix scroll flicker (rounded corners) and cut RAM use.

## [0.1.0]
- Initial native Nintendo Switch port of NuvioTV.
