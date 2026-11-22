#include "app.hpp"

#include <SDL.h>
#include <switch.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <tuple>

#include "account.hpp"
#include "diag.hpp"
#include "gfx.hpp"
#include "json.hpp"
#include "input.hpp"
#include "lan.hpp"
#include "playtrace.hpp"
#include "proxy.hpp"
#include "sdfile.hpp"
#include "screens/screens.hpp"
#include "store.hpp"
#include "trakt.hpp"
#include "ui.hpp"

App& App::get() {
    static App app;
    return app;
}

// libass on the Switch has no font provider; export the system font so mpv can render subtitles.
static void exportSystemFont() {
    size_t size = 0;
    const void* data = gfx::fontData(&size);
    if (!data || size == 0) return;
    const char* path = "sdmc:/switch/NuvioNX/fonts/system.ttf";
    struct stat st;
    if (stat(path, &st) == 0 && (size_t)st.st_size == size) return;  // already exported: no write
    sdfile::mkdirs("sdmc:/switch/NuvioNX/fonts");
    sdfile::write(path, std::string((const char*)data, size));
}

bool App::init() {
    romfsUp_ = R_SUCCEEDED(romfsInit());
    // More BSD sessions than libnx's default of 3: addon/image workers, the HTTPS relay
    // and the player all block in socket calls at the same time, and with only 3
    // sessions they could starve each other (player gives up with "loading failed").
    {
        SocketInitConfig cfg = *socketGetDefaultInitConfig();
        cfg.tcp_tx_buf_size = 0x8000;
        cfg.tcp_rx_buf_size = 0x10000;
        cfg.tcp_tx_buf_max_size = 0x40000;
        cfg.tcp_rx_buf_max_size = 0x40000;
        cfg.sb_efficiency = 8;
        cfg.num_bsd_sessions = 12;
        socketsUp_ = R_SUCCEEDED(socketInitialize(&cfg));
        if (!socketsUp_) {
            cfg.num_bsd_sessions = 8;
            cfg.sb_efficiency = 4;
            socketsUp_ = R_SUCCEEDED(socketInitialize(&cfg));
        }
        if (!socketsUp_) socketsUp_ = R_SUCCEEDED(socketInitializeDefault());
    }
    // How much memory this launch has (title override ~3 GB, Album/applet ~400 MB)
    {
        u64 total = 0;
        if (R_SUCCEEDED(svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0)))
            memoryMB_ = (int)(total / (1024 * 1024));
    }
    // System TLS service, used by both libcurl and FFmpeg (HTTPS streams) through libnx.
    // 0x4 is the max; the extra session helps the parallel poster downloads.
    sslUp_ = R_SUCCEEDED(sslInitialize(0x4));
    plUp_ = R_SUCCEEDED(plInitialize(PlServiceType_User));
    psmUp_ = R_SUCCEEDED(psmInitialize());
    store::load();

    if (SDL_Init(SDL_INIT_VIDEO) != 0) return false;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    // SDL_WINDOW_OPENGL so SDL_GL_GetProcAddress works for the mpv OpenGL render path
    // (the Switch SDL renderer is GLES2 anyway); the 2D UI still goes through SDL_Renderer.
    window_ = SDL_CreateWindow("NuvioNX", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 1920, 1080,
                               SDL_WINDOW_OPENGL);
    if (!window_) return false;
    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer_) return false;
    int pw = 1920, ph = 1080;
    SDL_GetRendererOutputSize(renderer_, &pw, &ph);
    if (!plUp_ || !gfx::init(renderer_, pw, ph)) return false;
    // Artwork cache budget: modest. (Eviction isn't what caused scroll flicker - that was
    // the rounded-corner drawing - so a big cache is pointless and just starves playback.)
    {
        size_t mb = (size_t)std::max(64, std::min(160, memoryMB_ / 16));
        gfx::setImageBudget(mb * 1024 * 1024);
    }

    input::init();
    net::init();
    exportSystemFont();
    account::load();
    loadAddons();
    if (store::settings().lanServer && socketsUp_) lan::start(8080);
    if (account::signedIn()) syncAccount(false);
    trakt::load();
    if (trakt::signedIn()) syncTrakt(false);

    // SD-card import dropped by tools/import-from-windows.ps1
    {
        const char* importPath = "sdmc:/switch/NuvioNX/nuvio-import.json";
        std::string body;
        if (sdfile::read(importPath, body)) {
            std::string summary;
            bool ok = applyImport(body, summary);
            std::string done = std::string(importPath) + (ok ? ".imported" : ".failed");
            remove(done.c_str());
            rename(importPath, done.c_str());
            toast(summary);
        }
    }

    push(makeMainScreen());
    return true;
}

void App::run() {
    double last = 0;
    double lastActive = gfx::now();  // last time something changed (input, background result, toast)
    int curSwapInterval = 1;         // GPU vsync divisor currently set (1 = 60 Hz)
    int64_t tailStartUs = 0, gcEndUs = 0;  // previous frame: start and end of endFrame()
    double lastStoreTick = 0;
    while (!quit_ && appletMainLoop()) {
        int64_t frameStartUs = playtrace::nowUs();  // gfx::now() only advances once per frame
        FrameTiming timing;
        if (tailStartUs) {
            timing.prevGcMs = (gcEndUs - tailStartUs) / 1000.0;
            timing.prevGapMs = (frameStartUs - gcEndUs) / 1000.0;
        }
        gfx::beginFrame();
        double t = gfx::now();
        float dt = (float)std::min(0.1, std::max(0.0, t - last));
        last = t;

        input::update();
        bool activity = input::any() || input::touching();
        if (t - lastStoreTick >= 1.0) {  // batched SD writes (at most every 5 min)
            lastStoreTick = t;
            store::tick();
        }
        bool focused = appletGetFocusState() == AppletFocusState_InFocus;
        {
            // HOME button / sleep: write pending data now, the app may be closed from there
            static bool wasFocused = true;
            if (wasFocused && !focused) store::flush();
            wasFocused = focused;
        }
        {
            // touch: hit regions from the frame the user saw
            float tx, ty;
            if (input::tapped(&tx, &ty)) ui::dispatchTap(tx, ty);
        }
        if (net::pumpMain() > 0) activity = true;  // a background result arrived -> likely a visual change
        if (lan::hasPending()) {
            for (auto& u : lan::takeSubmitted()) installAddon(u);
            std::string cu, ck;
            std::string importBody;
            if (lan::takeImport(importBody)) {
                std::string summary;
                applyImport(importBody, summary);
                toast(summary);
            }
            std::string tid, tsec;
            if (lan::takeTraktConfig(tid, tsec)) {
                trakt::state().clientId = tid;
                trakt::state().clientSecret = tsec;
                trakt::save();
                toast("Trakt app saved - open Account to connect");
            }
            if (lan::takeServerConfig(cu, ck)) {
                account::state().serverUrl = cu;
                account::state().publishableKey = ck;
                account::save();
                toast("Account server saved");
            }
        }

        // apply navigation queued by background callbacks before updating
        auto runOps = [this] {
            while (!ops_.empty()) {
                auto ops = std::move(ops_);
                ops_.clear();
                for (auto& op : ops) op();
            }
        };
        runOps();
        int64_t updateStartUs = playtrace::nowUs();
        if (!stack_.empty()) stack_.back()->update(dt);
        runOps();
        if (stack_.empty()) break;
        int64_t drawStartUs = playtrace::nowUs();
        timing.preMs = (updateStartUs - frameStartUs) / 1000.0;
        timing.updateMs = (drawStartUs - updateStartUs) / 1000.0;

        ui::clearHits();
        // Skip all drawing while we're not on screen (HOME menu open, screen dimming/sleep):
        // the GPU and SoC can idle. update()/background work above still runs so we resume
        // instantly and keep downloads going.
        if (focused) {
            int first = (int)stack_.size() - 1;
            while (first > 0 && !stack_[first]->opaque()) first--;
            for (size_t i = first; i < stack_.size(); i++) stack_[i]->render();
            drawToasts();
            if (store::settings().perfOverlay) drawPerfOverlay(dt);
            // CPU work this frame = everything up to the present; the present then
            // blocks on vsync (not counted). High fps + low cpu ms = we're vsync-bound,
            // not compute-bound.
            int64_t presentStartUs = playtrace::nowUs();
            cpuMs_ = (presentStartUs - frameStartUs) / 1000.0;
            SDL_RenderPresent(renderer_);
            if (playtrace::active() && !stack_.empty()) {
                timing.drawMs = (presentStartUs - drawStartUs) / 1000.0;
                timing.workMs = cpuMs_;
                timing.presentMs = (playtrace::nowUs() - presentStartUs) / 1000.0;
                stack_.back()->onPresented(timing);
            }
        }
        tailStartUs = playtrace::nowUs();
        gfx::endFrame();
        gcEndUs = playtrace::nowUs();

        // Frame-rate cap. Normally 60 (vsync), but when the UI is idle - no input, no
        // background results, no toasts, and the top screen doesn't need video frames - drop
        // the rate so the SoC/GPU can idle, saving power and heat. Any activity snaps back to
        // 60 instantly (input is still polled every loop, so response stays quick).
        if (activity || !toasts_.empty()) lastActive = t;
        bool maxFps = stack_.empty() || stack_.back()->wantsMaxFps();
        double sinceActive = t - lastActive;

        // A screen that must present every produced frame (the player) is paced by the GPU
        // swap interval, NOT by a manual sleep. Sleeping *and* waiting on vsync fight each
        // other - the sleep pushes the present past a vblank, so vsync waits for the next one
        // and the rate halves (a 30 fps target became 15). Translate the screen's frameCap
        // into a vsync divisor (30 fps = present every 2nd vblank) and let SDL_RenderPresent
        // block to pace it exactly and smoothly.
        double cap = (focused && !stack_.empty()) ? stack_.back()->frameCap() : 0.0;
        int wantInterval = 1;
        if (cap > 0.0) {
            wantInterval = (int)(60.0 / cap + 0.5);
            if (wantInterval < 1) wantInterval = 1;
            if (wantInterval > 4) wantInterval = 4;
        }
        if (focused && wantInterval != curSwapInterval) {
            SDL_GL_SetSwapInterval(wantInterval);
            curSwapInterval = wantInterval;
        }

        // Manual frame limiter for everything that ISN'T vsync-paced: menus when idle, and any
        // screen while off-screen (we skip the present there, so vsync can't pace us).
        // A menu in use is paced by vsync as well: adding a sleep on top of the vsync wait made
        // it miss vblanks and run below 60 fps while scrolling. Only idle menus sleep.
        bool vsyncPaced = focused && (maxFps || sinceActive <= 2.0);
        double kFrame = 1.0 / 60.0;
        if (!focused) {
            kFrame = 1.0 / 10.0;  // off-screen: idle hard (no present to vsync-pace us)
        } else if (!maxFps) {
            if (sinceActive > 10.0) kFrame = 1.0 / 15.0;      // deep idle
            else if (sinceActive > 2.0) kFrame = 1.0 / 30.0;  // idle
        }
        if (!vsyncPaced) svcSleepThread((u64)(kFrame * 1e9));
    }
}

void App::shutdown() {
    ops_.clear();
    stack_.clear();  // destroys any player (and its mpv instance) first
    store::flush();
    lan::stop();
    proxy::stop();
    net::beginShutdown();
    if (renderer_) gfx::shutdown();
    // Only tear down libcurl once no relay transfer can still be using it.
    if (proxy::activeTransfers() == 0) net::shutdown();
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
    SDL_Quit();
    if (plUp_) plExit();
    if (psmUp_) psmExit();
    if (sslUp_) sslExit();
    if (socketsUp_) socketExit();
    if (romfsUp_) romfsExit();
}

void App::push(std::shared_ptr<Screen> s) {
    ops_.push_back([this, s] { stack_.push_back(s); });
}

void App::pop() {
    ops_.push_back([this] {
        if (stack_.size() <= 1) {
            quit_ = true;
            return;
        }
        stack_.pop_back();
        stack_.back()->onResume();
    });
}

void App::replaceTop(std::shared_ptr<Screen> s) {
    ops_.push_back([this, s] {
        if (!stack_.empty()) stack_.pop_back();
        stack_.push_back(s);
    });
}

void App::toast(const std::string& msg) {
    toasts_.push_back({msg, gfx::now() + 2.8});
    if (toasts_.size() > 3) toasts_.erase(toasts_.begin());
}

void App::drawPerfOverlay(float dt) {
    using namespace gfx;
    // smoothed fps + worst frame time over a window
    static double acc = 0, worst = 0;
    static int frames = 0;
    static double fps = 0, shownWorst = 0;
    acc += dt;
    worst = std::max(worst, (double)dt);
    frames++;
    if (acc >= 0.3) {
        fps = frames / acc;
        shownWorst = worst;
        acc = 0;
        worst = 0;
        frames = 0;
    }
    int ready = 0, total = 0, pending = 0, up = 0, miss = 0, evicts = 0;
    double mb = 0;
    gfx::imageStats(ready, total, mb, pending, up);
    gfx::imageStats2(miss, evicts);
    static int lastEvicts = 0, evictPerSec = 0;
    static double evAcc = 0;
    evAcc += dt;
    if (evAcc >= 1.0) {
        evictPerSec = evicts - lastEvicts;
        lastEvicts = evicts;
        evAcc = 0;
    }
    char l1[96], l2[96], l3[96];
    snprintf(l1, sizeof l1, "%.0f fps  frame %.1f ms  cpu %.1f ms (worst %.0f)", fps, dt * 1000, cpuMs_, shownWorst * 1000);
    snprintf(l2, sizeof l2, "img ready %d/%d  %.0f MB  pending %d  up/f %d", ready, total, mb, pending, up);
    snprintf(l3, sizeof l3, "MISS/f %d   evict/s %d", miss, evictPerSec);
    float w = 430, x = W - w - 12, y = 70;
    fillRect(x, y, w, 72, Col{0, 0, 0, 205});
    Col c = (fps < 55) ? rgba(0xEF4444) : rgba(0x6EE7B7);
    text(l1, x + 10, y + 6, 15, c, true);
    text(l2, x + 10, y + 27, 14, Col{255, 255, 255, 230});
    Col mc = (miss > 0 || evictPerSec > 0) ? rgba(0xFCD34D) : Col{255, 255, 255, 200};
    text(l3, x + 10, y + 48, 14, mc, true);
}

void App::drawToasts() {
    using namespace gfx;
    const Theme& t = theme();
    double n = now();
    toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(), [n](const Toast& x) { return x.until < n; }),
                  toasts_.end());
    float y = 600;
    for (auto it = toasts_.rbegin(); it != toasts_.rend(); ++it) {
        float w = std::min(900.f, textWidth(it->msg, 17, true) + 48);
        float a = (float)std::min(1.0, (it->until - n) / 0.3);
        roundRect(W / 2 - w / 2, y, w, 42, 21, withA(t.text, (Uint8)(240 * a)));
        text(it->msg, W / 2, y + 11, 17, withA(t.bg, (Uint8)(255 * a)), true, Center, w - 40);
        y -= 50;
    }
}

// ------------------------------------------------------------------ addons
std::vector<stremio::Addon> App::enabledAddons() const {
    std::vector<stremio::Addon> out;
    for (auto& a : addons_)
        if (a.enabled) out.push_back(a);
    return out;
}

void App::loadAddons() {
    addons_.clear();
    auto& entries = store::addonEntries();
    bool changed = false;
    for (auto& e : entries) {
        std::string norm = stremio::normalizeManifestUrl(e.url);
        if (norm != e.url) {
            e.url = norm;
            changed = true;
        }
    }
    if (changed) store::saveAddons();

    for (auto& e : entries) {
        stremio::Addon a;
        a.manifestUrl = e.url;
        a.enabled = e.enabled;
        stremio::splitManifestUrl(e.url, a.base, a.query);
        std::string raw = store::cachedManifest(e.url);
        json::Value v;
        if (!raw.empty() && json::parse(raw, v) && a.parseManifest(v)) a.loaded = true;
        if (a.name.empty()) a.name = a.base;
        addons_.push_back(a);
    }
    rev_++;
    for (auto& a : addons_) refreshManifest(a.manifestUrl);
}

void App::refreshManifest(const std::string& url) {
    loading_++;
    net::runAsync([url] {
        auto res = std::make_shared<std::tuple<bool, stremio::Addon, std::string, std::string>>();
        std::get<0>(*res) = stremio::fetchManifest(url, std::get<1>(*res), std::get<2>(*res), std::get<3>(*res));
        net::postMain([url, res] {
            App& app = App::get();
            app.loading_--;
            for (auto& a : app.addons_) {
                if (a.manifestUrl != url) continue;
                if (std::get<0>(*res)) {
                    bool changed = !a.loaded || store::cachedManifest(url) != std::get<2>(*res);
                    bool enabled = a.enabled;
                    a = std::get<1>(*res);
                    a.enabled = enabled;
                    a.error.clear();
                    store::cacheManifest(url, std::get<2>(*res));
                    if (changed) app.rev_++;
                } else {
                    a.error = std::get<3>(*res);
                }
                break;
            }
        });
    });
}

void App::syncEntries() {
    auto& entries = store::addonEntries();
    entries.clear();
    for (auto& a : addons_) entries.push_back({a.manifestUrl, a.enabled});
    store::saveAddons();
    rev_++;
}

void App::installAddon(const std::string& rawUrl) {
    std::string url = stremio::normalizeManifestUrl(rawUrl);
    for (auto& a : addons_)
        if (a.manifestUrl == url) {
            toast("Addon already installed");
            return;
        }
    toast("Installing addon...");
    loading_++;
    net::runAsync([url] {
        auto res = std::make_shared<std::tuple<bool, stremio::Addon, std::string, std::string>>();
        std::get<0>(*res) = stremio::fetchManifest(url, std::get<1>(*res), std::get<2>(*res), std::get<3>(*res));
        net::postMain([url, res] {
            App& app = App::get();
            app.loading_--;
            if (!std::get<0>(*res)) {
                app.toast("Couldn't install addon: " + std::get<3>(*res));
                return;
            }
            for (auto& a : app.addons_)
                if (a.manifestUrl == url) return;
            stremio::Addon a = std::get<1>(*res);
            a.enabled = true;
            app.addons_.push_back(a);
            app.syncEntries();
            store::cacheManifest(url, std::get<2>(*res));
            app.toast("Installed " + a.name);
        });
    });
}

void App::removeAddon(size_t i) {
    if (i >= addons_.size()) return;
    addons_.erase(addons_.begin() + i);
    syncEntries();
}

void App::toggleAddon(size_t i) {
    if (i >= addons_.size()) return;
    addons_[i].enabled = !addons_[i].enabled;
    syncEntries();
}

void App::moveAddon(size_t i, int dir) {
    size_t j = (size_t)((int)i + dir);
    if (i >= addons_.size() || j >= addons_.size()) return;
    std::swap(addons_[i], addons_[j]);
    syncEntries();
}

// ------------------------------------------------------------------ account sync
void App::syncAccount(bool userInitiated) {
    if (syncing_ || !account::signedIn()) return;
    syncing_ = true;
    syncStatus_ = "Syncing...";
    account::State cfg = account::state();
    net::runAsync([cfg, userInitiated] {
        auto res = std::make_shared<account::SyncResult>(account::pull(cfg));
        net::postMain([res, userInitiated] {
            App& app = App::get();
            app.syncing_ = false;
            if (!res->ok) {
                app.syncStatus_ = "Sync failed: " + res->error;
                if (userInitiated) app.toast(app.syncStatus_);
                return;
            }
            auto& st = account::state();
            bool tokensChanged = st.session.accessToken != res->session.accessToken ||
                                 st.session.refreshToken != res->session.refreshToken;
            st.session = res->session;
            st.lastSync = (long long)time(nullptr);
            if (tokensChanged) account::save();  // don't rewrite the file just for the sync timestamp

            app.profiles_.clear();
            for (auto& p : res->profiles)
                app.profiles_.push_back({p.index, p.name.empty() ? "Profile " + std::to_string(p.index) : p.name});

            std::string summary;
            if (res->haveAddons && !res->addons.empty()) {
                store::replaceAddons(res->addons);
                app.loadAddons();
                summary += std::to_string(res->addons.size()) + " addons";
            }
            if (res->haveCredentials) {
                auto& s = store::settings();
                if (!res->realDebridKey.empty()) s.realDebridKey = res->realDebridKey;
                if (!res->torboxKey.empty()) s.torboxKey = res->torboxKey;
                if (!res->premiumizeKey.empty()) s.premiumizeKey = res->premiumizeKey;
                store::saveSettings();
                if (!res->realDebridKey.empty() || !res->torboxKey.empty() || !res->premiumizeKey.empty())
                    summary += (summary.empty() ? "" : ", ") + std::string("debrid");
            }
            if (res->haveLibrary && !res->library.empty()) {
                store::replaceLibrary(res->library);
                summary += (summary.empty() ? "" : ", ") + std::to_string(res->library.size()) + " in library";
            }
            if (!res->progress.empty()) {
                store::mergeProgress(res->progress);
                summary += (summary.empty() ? "" : ", ") + std::to_string(res->progress.size()) + " progress";
            }
            app.syncStatus_ = summary.empty() ? "Synced (account has no data yet)" : "Synced: " + summary;
            app.enriched_.clear();
            if (userInitiated) app.toast(app.syncStatus_);
        });
    });
}

// ------------------------------------------------------------------ Trakt
// Worker results may carry refreshed tokens; keep them (main thread only).
static void adoptTraktState(const trakt::State& s) {
    auto& cur = trakt::state();
    if (cur.refreshToken.empty()) return;  // signed out meanwhile
    bool changed = cur.accessToken != s.accessToken || cur.refreshToken != s.refreshToken ||
                   (!s.username.empty() && cur.username != s.username);
    if (!changed) return;
    cur.accessToken = s.accessToken;
    cur.refreshToken = s.refreshToken;
    cur.expiresAt = s.expiresAt;
    if (!s.username.empty()) cur.username = s.username;
    trakt::save();
}

void App::syncTrakt(bool userInitiated) {
    if (traktSyncing_ || !trakt::signedIn()) return;
    traktSyncing_ = true;
    traktStatus_ = "Syncing with Trakt...";
    trakt::State s = trakt::state();
    net::runAsync([s, userInitiated] {
        auto res = std::make_shared<trakt::PullResult>(trakt::pull(s));
        net::postMain([res, userInitiated] {
            App& app = App::get();
            app.traktSyncing_ = false;
            diag::writeFile("sdmc:/switch/NuvioNX/trakt.log");  // for troubleshooting
            if (!res->ok) {
                app.traktStatus_ = "Trakt sync failed: " + res->error;
                if (userInitiated) app.toast(app.traktStatus_);
                return;
            }
            adoptTraktState(res->state);
            // watched history first, then in-progress items (newest timestamp wins per video)
            std::vector<store::Progress> all = res->watched;
            all.insert(all.end(), res->progress.begin(), res->progress.end());
            if (!all.empty()) store::mergeProgress(all);
            // watchlist -> library (add what's missing, keep local order)
            size_t added = 0;
            if (!res->watchlist.empty()) {
                auto lib = store::library();
                for (auto& m : res->watchlist) {
                    bool have = false;
                    for (auto& x : lib)
                        if (x.id == m.id) have = true;
                    if (!have) {
                        lib.push_back(m);
                        added++;
                    }
                }
                if (added) store::replaceLibrary(lib);
            }
            trakt::state().lastSync = (long long)time(nullptr);
            app.enriched_.clear();
            app.traktStatus_ = "Trakt: " + std::to_string(res->progress.size()) + " in progress, " +
                               std::to_string(res->watched.size()) + " watched, " + std::to_string(added) +
                               " new in library";
            if (userInitiated) app.toast(app.traktStatus_);
        });
    });
}

void App::traktScrobble(const std::string& action, const std::string& imdb, const std::string& type, int season,
                        int episode, double pct) {
    if (!trakt::signedIn() || imdb.compare(0, 2, "tt") != 0) return;
    trakt::Item item{imdb, type == "movie" ? "movie" : "series", season, episode};
    trakt::State s = trakt::state();
    net::runAsync([s, action, item, pct] {
        auto out = std::make_shared<trakt::State>(trakt::scrobble(s, action, item, pct));
        net::postMain([out] { adoptTraktState(*out); });
    });
}

void App::traktSetWatched(const std::string& imdb, const std::string& type, int season, int episode, bool watched) {
    if (!trakt::signedIn() || imdb.compare(0, 2, "tt") != 0) return;
    trakt::Item item{imdb, type == "movie" ? "movie" : "series", season, episode};
    trakt::State s = trakt::state();
    net::runAsync([s, item, watched] {
        auto out = std::make_shared<trakt::State>(trakt::setWatched(s, item, watched));
        net::postMain([out] { adoptTraktState(*out); });
    });
}

bool App::applyImport(const std::string& body, std::string& summary) {
    json::Value v;
    if (!json::parse(body, v) || !v.isObject()) {
        summary = "Import failed: invalid file";
        return false;
    }
    std::vector<std::string> parts;

    // addons: replace with the Windows setup (order + enabled state)
    std::vector<store::AddonEntry> addons;
    for (auto& a : v["addons"].items()) {
        std::string url = a["url"].str();
        if (url.empty()) continue;
        addons.push_back({stremio::normalizeManifestUrl(url), a["enabled"].boolean(true)});
    }
    if (!addons.empty()) {
        store::replaceAddons(addons);
        loadAddons();
        parts.push_back(std::to_string(addons.size()) + " addons");
    }

    // library: imported titles first, keep anything only saved on the Switch
    if (v["library"].size() > 0) {
        std::vector<stremio::MetaPreview> merged;
        for (auto& it : v["library"].items()) {
            auto m = stremio::parsePreview(it);
            if (!m.id.empty()) merged.push_back(m);
        }
        size_t imported = merged.size();
        for (auto& m : store::library()) {
            bool dup = false;
            for (auto& x : merged)
                if (x.id == m.id) dup = true;
            if (!dup) merged.push_back(m);
        }
        store::replaceLibrary(merged);
        parts.push_back(std::to_string(imported) + " library");
    }

    // progress + watched history
    std::vector<store::Progress> prog;
    auto readProgress = [](const json::Value& p) {
        store::Progress r;
        r.videoId = p["videoId"].str();
        r.metaId = p["metaId"].str();
        r.type = p["type"].str();
        r.name = p["name"].str();
        r.poster = p["poster"].str();
        r.background = p["background"].str();
        r.logo = p["logo"].str();
        r.epTitle = p["epTitle"].str();
        r.season = (int)p["season"].integer(0);
        r.episode = (int)p["episode"].integer(0);
        r.pos = p["pos"].num(0);
        r.dur = p["dur"].num(0);
        r.updated = p["updated"].integer(0);
        return r;
    };
    size_t nProgress = 0, nWatched = 0;
    for (auto& p : v["progress"].items()) {
        auto r = readProgress(p);
        if (r.videoId.empty() || r.metaId.empty()) continue;
        prog.push_back(r);
        nProgress++;
    }
    for (auto& p : v["watched"].items()) {
        auto r = readProgress(p);
        if (r.videoId.empty() || r.metaId.empty()) continue;
        bool dup = false;
        for (auto& x : prog)
            if (x.videoId == r.videoId) dup = true;
        if (dup) continue;
        r.pos = 1;
        r.dur = 1;  // fully watched
        prog.push_back(r);
        nWatched++;
    }
    if (!prog.empty()) store::mergeProgress(prog);
    if (nProgress) parts.push_back(std::to_string(nProgress) + " in progress");
    if (nWatched) parts.push_back(std::to_string(nWatched) + " watched");

    enriched_.clear();
    if (parts.empty()) {
        summary = "Import file was empty";
        return false;
    }
    summary = "Imported from Nuvio for Windows: ";
    for (size_t i = 0; i < parts.size(); i++) summary += (i ? ", " : "") + parts[i];
    return true;
}

void App::enrichProgress() {
    if (loading_ > 0) return;  // wait for manifests
    // Nothing new to look up unless progress changed or the enriched list was reset.
    int rev = store::progressRevision();
    if (rev == enrichRev_ && enriched_.size() == enrichedSize_) return;
    enrichRev_ = rev;
    auto addons = enabledAddons();
    for (auto& p : store::continueWatching(30)) {
        if (!p.name.empty() || p.metaId.empty()) continue;
        if (std::find(enriched_.begin(), enriched_.end(), p.metaId) != enriched_.end()) continue;
        enriched_.push_back(p.metaId);
        std::string type = p.type, id = p.metaId;
        net::runAsync([addons, type, id] {
            auto m = std::make_shared<stremio::Meta>();
            bool ok = false;
            for (auto& a : addons)
                if (a.loaded && a.supports("meta", type, id) && stremio::fetchMeta(a, type, id, *m)) {
                    ok = true;
                    break;
                }
            if (!ok) return;
            net::postMain([m] { store::updateProgressMeta(*m); });
        });
    }
    enrichedSize_ = enriched_.size();
}

int App::batteryPercent(bool* charging) const {
    if (psmUp_ && gfx::now() - battChecked_ > 5.0) {  // poll at most every 5 s
        battChecked_ = gfx::now();
        u32 pct = 0;
        if (R_SUCCEEDED(psmGetBatteryChargePercentage(&pct))) battPct_ = (int)pct;
        PsmChargerType ch;
        if (R_SUCCEEDED(psmGetChargerType(&ch))) battCharging_ = ch != PsmChargerType_Unconnected;
    }
    if (charging) *charging = battCharging_;
    return battPct_;
}

std::string App::lanUrl() const {
    if (!lan::running()) return "";
    std::string ip = net::localIp();
    if (ip.empty()) return "";
    return "http://" + ip + ":" + std::to_string(lan::port());
}
