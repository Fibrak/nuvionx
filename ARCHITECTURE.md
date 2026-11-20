# NuvioNX architecture & Switch gotchas

High-level notes for anyone continuing this project. The code is plain C++17 with libnx + SDL2 +
libmpv. There is no framework; `App` owns a stack of `Screen`s and runs a single render loop.

## Threading model

- **UI/render thread** (main): runs the loop in `App::run()` — input, one `Screen::update()`, then
  `render()` for the visible stack, then present.
- **Worker pool** (`net.cpp`): background threads for all blocking HTTP. Screens call
  `Screen::async(work, done)`: `work()` runs on a worker, `done(result)` is posted back to the main
  thread via `net::postMain()` and only runs if the screen still exists (weak_ptr guard).
- **Image threads** (`gfx.cpp`): download + decode artwork off-thread; textures are created on the
  main thread (SDL is not thread-safe for GPU calls).
- **Render thread** (`player_screen.cpp`): mpv's software renderer (YUV→RGB + scale) runs on its own
  thread, double-buffered, so it doesn't stall the UI.

> **libnx has no detached threads.** `std::thread::detach()` leaks and eventually aborts with
> `2345-0008` (LibnxError_NotInitialized in `threadExit`). Use joinable worker pools and join them on
> shutdown. The HTTPS relay (`proxy.cpp`) learned this the hard way.

## Networking & TLS

- TLS goes through the **Switch system SSL service** (libnx backend in devkitPro's curl/FFmpeg),
  which verifies against the console's built-in CA store. **Do not** set `CURLOPT_CAINFO` — that
  backend can't load a PEM and every request fails with "SSL connect error". `App::init()` calls
  `sslInitialize()` so both curl and FFmpeg can use it.
- Each idle HTTPS connection pins an `SslContext`, and the service allows only a few per process.
  Leftover idle connections from browsing made playback fail instantly. Fixes: `CURLOPT_FORBID_REUSE`
  + `MAXCONNECTS=1` on general requests (`net.cpp`), a tiny reuse pool in the relay, and an
  SSL-connect-error retry.
- `socketInitialize()` is called with **more BSD sessions** than the default 3 — the relay, image
  workers, API workers and the player all block on sockets simultaneously; 3 sessions starved each
  other and the player gave up with "loading failed".

## Video playback

- devkitPro's **FFmpeg is built without the `https` protocol.** mpv can't open `https://` URLs. So
  `proxy.cpp` runs a localhost HTTP→HTTPS relay: the player opens
  `http://127.0.0.1:<port>/s/<token>.<ext>` and the relay fetches the real URL with curl (system
  TLS), forwarding `Range` so seeking works. Addon request headers and subtitle URLs go through it
  too.
- Hardware decode is FFmpeg **nvtegra** (`hwdec=nvtegra-copy`). Frames are copied back and rendered
  in software, so 4K is CPU-bound on scaling and not usable. Audio is downmixed to stereo 48 kHz
  (`ao=hos,sdl`) because most files are 5.1/7.1 and the Switch outputs stereo.
- Resolutions above 1440p without hardware decode are stopped with a message instead of crashing.

## Storage (SD-card wear matters — many users run emuNAND off the same card)

- All file I/O uses **native Switch FS** (`sdfile.cpp`, `fsFs*`/`fsFile*`), not stdio. stdio FILEs
  and sockets share newlib's fd table; during network teardown a log `fclose()` crashed
  (`Data Abort`). Native FS handles live outside that table.
- Writes are **skipped when content is unchanged**, and watch progress is **batched** (RAM, flushed
  at most every 5 min, plus on exit / HOME / leaving the player) instead of every 15 s.
- The disk artwork cache is **off by default** (toggle in Settings) to avoid write wear. Video never
  caches to disk.

## Images (`gfx.cpp`)

- One entry per URL, keyed by URL, decoded off-thread and uploaded ≤8 textures/frame.
- GPU budget scales with available memory (bigger on a title-override launch). Eviction drops the
  **largest** off-screen textures first (backdrops) so small posters survive scrolling.
- A URL fades in only the first time it is ever shown (`g_shownOnce`), so a re-decoded image can't
  visibly re-flicker. `RowsView` prefetches artwork for rows just below the viewport.

## Auth

- Both **Nuvio account** (`account.cpp`) and **Trakt** (`trakt.cpp`) use OAuth **device-code** flow:
  show a code + QR, poll until approved. Keys/URLs are user-supplied (Account tab or the `:8080`
  `/account` page), never bundled.
- Nuvio sync reads Supabase REST/RPC endpoints (the same ones NuvioTV calls). Trakt supports
  secret-less **PKCE** apps (newer Trakt default): the client secret is omitted when empty.

## Input

- `input.cpp` maps libnx `PadState` + touch to a small button set with key-repeat. It special-cases a
  **single sideways Joy-Con** (stick + rail buttons). Touch taps are dispatched against hit regions
  registered by the UI during render (`ui::hit` / `ui::tapToActivate`).

## Known rough edges / next steps

- 1080p can still judder on heavy streams; moving YUV→RGB + scaling to the GPU (shader) is the real
  fix and is not done.
- Sync is pull-only for Nuvio (no push of Switch activity).
- No CJK font.
