# Changelog

All notable changes to NuvioNX are documented here.

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
