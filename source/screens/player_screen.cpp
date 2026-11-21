// Video player: libmpv (FFmpeg) with the software render API, drawn into an
// SDL streaming texture. Addon subtitles, audio/subtitle track menus,
// resume + progress saving, next-episode autoplay.
#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>
#include <switch.h>
#include <SDL2/SDL_opengles2.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <set>
#include <thread>

#include "input.hpp"
#include "diag.hpp"
#include "json.hpp"
#include "proxy.hpp"
#include "screens.hpp"
#include "store.hpp"

using namespace gfx;

namespace {

// GLES2 framebuffer entry points, resolved at runtime via SDL_GL_GetProcAddress so we don't
// depend on link-time GLESv2 symbols (and get exactly the GL the SDL/mpv stack uses).
typedef void (*PFN_glGenFramebuffers)(GLsizei, GLuint*);
typedef void (*PFN_glBindFramebuffer)(GLenum, GLuint);
typedef void (*PFN_glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum (*PFN_glCheckFramebufferStatus)(GLenum);
typedef void (*PFN_glDeleteFramebuffers)(GLsizei, const GLuint*);
typedef void (*PFN_glGetIntegerv)(GLenum, GLint*);
typedef void (*PFN_glFinish)(void);

bool sameLang(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return false;
    return ui::langName(a) == ui::langName(b);
}

std::string mpvString(mpv_handle* h, const char* prop) {
    char* s = mpv_get_property_string(h, prop);
    if (!s) return "";
    std::string out = s;
    mpv_free(s);
    return out;
}

class PlayerScreen : public Screen {
public:
    PlayerScreen(const PlayContext& ctx, const stremio::Stream& s) : ctx_(ctx), stream_(s) {
        start();
        fetchSubtitles();
    }

    ~PlayerScreen() override {
        appletSetMediaPlaybackState(false);
        if (boosted_) appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
        // Trakt: final position (>= 80% marks it watched on Trakt's side)
        if (scrobbleStarted_ && dur_ > 0) {
            double pct = ended_ ? 100.0 : pos_ / dur_ * 100.0;
            App::get().traktScrobble("stop", ctx_.meta.id, ctx_.type, ctx_.season, ctx_.episode, pct);
        }
        saveProgress();
        stopRenderThread();   // (software path) must finish before the render context is freed
        stopVideoGlThread();  // (threaded GPU path) joins the video thread + releases its context
        if (rctx_) {
            // free mpv's GL objects on its own context, then hand the context back to SDL
            if (useGl_ && win_ && mpvGlCtx_) SDL_GL_MakeCurrent(win_, mpvGlCtx_);
            mpv_render_context_free(rctx_);
            if (useGl_ && win_ && sdlGlCtx_) SDL_GL_MakeCurrent(win_, sdlGlCtx_);
        }
        if (mpvGlCtx_) SDL_GL_DeleteContext(mpvGlCtx_);
        if (mpv_) mpv_terminate_destroy(mpv_);
        if (tex_) SDL_DestroyTexture(tex_);
        store::flush();  // one SD write per viewing session instead of one every 15 s
        if (!error_.empty()) writeLogFile();
    }

    void update(float dt) override {
        (void)dt;
        pumpEvents();
        if (ended_) return;  // navigation already queued by onEnded()

        if (!error_.empty()) {
            if (input::pressed(input::B) || input::pressed(input::A)) App::get().pop();
            return;
        }

        // pause when the HOME menu / sleep takes focus away
        if (appletGetFocusState() != AppletFocusState_InFocus && !paused_ && loaded_) setPause(true);

        if (menu_ != None && menu_ != Info) {
            if (input::pressed(input::Up)) menuSel_ = std::max(0, menuSel_ - 1);
            if (input::pressed(input::Down)) menuSel_ = std::min((int)items_.size() - 1, menuSel_ + 1);
            if (input::pressed(input::A) && !items_.empty()) {
                applyMenu(items_[menuSel_]);
                menu_ = None;
            }
            if (input::pressed(input::B) || input::pressed(input::Y) || input::pressed(input::X)) menu_ = None;
            return;
        }

        if (input::pressed(input::B)) {
            if (menu_ == Info) {
                menu_ = None;
                return;
            }
            App::get().pop();
            return;
        }
        if (input::pressed(input::A)) {
            setPause(!paused_);
            poke();
        }
        if (input::pressed(input::Left)) seek(-10);
        if (input::pressed(input::Right)) seek(+10);
        if (input::pressed(input::L) || input::pressed(input::ZL)) seek(-60);
        if (input::pressed(input::R) || input::pressed(input::ZR)) seek(+60);
        if (input::pressed(input::Up) || input::pressed(input::Down)) poke();
        if (menu_ == Info && input::pressed(input::Y)) {
            diag::writeFile();  // on demand only: one SD write
            App::get().toast("Player log saved to sdmc:/switch/NuvioNX/mpv.log");
            return;
        }
        if (input::pressed(input::Y)) openMenu(Subs);
        if (input::pressed(input::X)) openMenu(Audio);
        if (input::pressed(input::Minus)) menu_ = (menu_ == Info) ? None : Info;

        updateScrobble();

        if (loaded_ && !paused_ && now() - lastSave_ > 15) {
            lastSave_ = now();
            saveProgress();
        }
    }

    void render() override {
        const Theme& t = theme();
        SDL_SetRenderDrawColor(R, 0, 0, 0, 255);
        SDL_RenderClear(R);

        if (useGl_ && glThreaded_) {
            // Threaded GPU path: the video thread already rendered the frame into an off-screen
            // texture on another core; here we only composite the latest finished one.
            int idx = vpub_.load(std::memory_order_acquire);
            if (idx >= 0 && videoTex_[idx]) SDL_RenderCopy(R, videoTex_[idx], nullptr, nullptr);
        } else if (useGl_) {
            // GPU path (single-thread fallback): mpv draws the video straight onto the window
            // framebuffer (GPU does the convert + scale). UI drawn by SDL on top afterwards.
            renderVideoGl();
        } else {
            // Software path: frames are converted on a background thread; here we only upload
            // the newest finished one, so the UI thread never stalls on conversion.
            if (tex_) {
                std::lock_guard<std::mutex> lk(frameMx_);
                if (frameReady_) {
                    SDL_UpdateTexture(tex_, nullptr, frames_[front_], vw_ * 4);
                    frameReady_ = false;
                    haveFrame_ = true;
                }
            }
            if (tex_ && haveFrame_) SDL_RenderCopy(R, tex_, nullptr, nullptr);
        }

        if (!error_.empty()) {
            fillRect(0, 0, W, H, withA(t.bg, 230));
            text("Playback failed", W / 2, 300, 28, t.text, true, Center);
            paragraph(error_, 290, 346, 700, 16, t.text2, 9);
            ui::hints({{"B", "Back"}});
            return;
        }

        // Touch: tap the picture to show controls, tap again to play/pause.
        ui::hit(0, 0, W, H, [this] {
            bool shown = paused_ || now() < overlayUntil_;
            if (menu_ != None) input::inject(input::B);
            else if (shown) input::inject(input::A);
            else poke();
        });

        bool busy = !loaded_ || buffering_;
        if (busy) {
            spinner(W / 2, H / 2, 26, t.text);
            if (cachePct_ > 0 && cachePct_ < 100)
                text(std::to_string((int)cachePct_) + "%", W / 2, H / 2 + 44, 16, t.text2, true, Center);
        }

        bool show = paused_ || busy || now() < overlayUntil_ || menu_ != None;
        if (show) renderOverlay();
        if (menu_ == Subs || menu_ == Audio) renderMenu();
        if (menu_ == Info) renderInfo();
    }

    // Video needs a fresh frame every vsync, so never let the idle throttle kick in.
    bool wantsMaxFps() const override { return true; }

    // Movie mode: during steady playback cap the loop to the video's own frame rate, so we
    // stop re-compositing and re-presenting the same frame ~60x/s (big power saving, the main
    // win during playback). Returns 0 (full 60 fps) whenever the user is interacting or the
    // controls/menus are visible, so scrubbing and overlay animations stay smooth.
    double frameCap() const override {
        if (!loaded_ || buffering_ || paused_ || menu_ != None || now() < overlayUntil_)
            return 0.0;  // interacting / controls visible: full 60 fps
        // Steady playback: cap to 30 fps. The panel is a fixed 60 Hz, and only rates that
        // divide 60 evenly (60/30/20/15) present on consistent vblanks. A literal 24 cap can't
        // align to 60 Hz - it oscillates between the 2nd and 3rd vblank (20<->30) and reads as
        // a hard stutter. 30 is the lowest clean divisor, so motion stays smooth while we still
        // halve the per-frame present/UI work for a cooler, quieter idle. High-frame-rate
        // content (48/50/60) just runs at full 60.
        if (videoFps_ > 32.0) return 0.0;
        return 30.0;
    }

private:
    enum Menu { None, Subs, Audio, Info };
    struct Item {
        std::string label, sub;
        long long track = -1;  // mpv track id
        int addonSub = -1;     // index into addonSubs_
        bool off = false, selected = false;
    };

    void start() {
        const store::Settings& st = store::settings();
        // Free cached artwork so mpv/FFmpeg get the RAM (biggest single win for playback
        // stability). Posters reload instantly when you return to browsing.
        gfx::releaseImageCache();
        mpv_ = mpv_create();
        if (!mpv_) {
            error_ = "Could not create the mpv player instance.";
            return;
        }
        mpv_set_option_string(mpv_, "vo", "libmpv");
        mpv_set_option_string(mpv_, "terminal", "no");
        // (no msg-level filter: we capture mpv's messages ourselves via the client log)
        mpv_set_option_string(mpv_, "input-default-bindings", "no");
        mpv_set_option_string(mpv_, "osc", "no");
        // Tegra hardware decoding (FFmpeg nvtegra): NVDEC decodes on dedicated hardware, then
        // the frame is copied into system RAM so mpv can render it. True zero-copy (plain
        // "nvtegra", importing the decoder surface straight into GL) is NOT possible with this
        // mpv/ffmpeg build: libmpv ships no Tegra/dmabuf GL-interop driver and ffmpeg's nvtegra
        // doesn't export DRM_PRIME, so "nvtegra" always falls back to "nvtegra-copy". We request
        // the copy path directly to skip that dead probe. Falls back to CPU decoding per file.
        const char* hw = !st.hwdec ? "no" : "nvtegra-copy";
        mpv_set_option_string(mpv_, "hwdec", hw);
        mpv_set_option_string(mpv_, "hwdec-codecs", "h264,hevc,vp9,vp8,mpeg2video,mpeg4,vc1");
        // Audio: native Switch renderer first, SDL as fallback; the console output is
        // stereo, so downmix 5.1/7.1 (E-AC3, DTS, TrueHD...) instead of failing.
        mpv_set_option_string(mpv_, "ao", "hos,sdl");
        mpv_set_option_string(mpv_, "audio-channels", "stereo");
        mpv_set_option_string(mpv_, "audio-samplerate", "48000");
        // Performance: the app may use 3 CPU cores (core 3 belongs to the system).
        mpv_set_option_string(mpv_, "vd-lavc-threads", "3");
        // Fastest scaling/colour conversion in the software renderer (quality cost is
        // negligible on a 720p/1080p screen).
        mpv_set_option_string(mpv_, "sws-fast", "yes");
        mpv_set_option_string(mpv_, "sws-scaler", "fast-bilinear");
        mpv_set_option_string(mpv_, "zimg-fast", "yes");
        mpv_set_option_string(mpv_, "zimg-threads", "2");
        // Software decoding only: skip H.264 deblocking on non-reference frames (big CPU
        // saving, hard to see). Has no effect when hardware decoding is active.
        mpv_set_option_string(mpv_, "vd-lavc-skiploopfilter", "nonref");
        mpv_set_option_string(mpv_, "vd-lavc-fast", "yes");
        // GPU path only: upload each decoded frame to the GPU through an asynchronous pixel
        // buffer (DMA) instead of a blocking CPU copy. hwdec here is nvtegra-COPY (no zero-copy
        // interop exists in this mpv build), so every frame is pushed GPU->RAM->GPU on the main
        // render thread; PBO uploads move that stall off the CPU and are the lowest-risk way to
        // cut the one pegged core during playback. No effect on the software renderer.
        if (st.gpuVideo) mpv_set_option_string(mpv_, "opengl-pbo", "yes");
        // NOTE: video-sync=display-resample was tried here but reverted. It only works with
        // mpv_render_context_report_swap(), which mpv requires to be called from the SAME
        // thread as mpv_render_context_render(). Our renderer runs render() on a background
        // thread while the SDL swap happens on the main thread, so reporting the swap from
        // the main thread raced mpv's display-sync timing state and corrupted the heap
        // (hard crash after a while in playback). Default audio sync is what we ship.
        // Drop late frames at the decoder instead of stuttering audio/video sync.
        mpv_set_option_string(mpv_, "framedrop", "decoder+vo");
        mpv_set_option_string(mpv_, "cache", "yes");
        mpv_set_option_string(mpv_, "cache-on-disk", "no");  // buffer in RAM only, never on the SD card
        // Network buffer: keep it bounded and modest so RAM doesn't creep up over a long
        // movie. A few seconds of forward buffer is plenty for a direct HTTP(S) stream.
        bool bigMemory = App::get().memoryBudgetMB() >= 1500;
        mpv_set_option_string(mpv_, "demuxer-max-bytes", bigMemory ? "48MiB" : "24MiB");
        mpv_set_option_string(mpv_, "demuxer-max-back-bytes", bigMemory ? "16MiB" : "8MiB");
        mpv_set_option_string(mpv_, "demuxer-readahead-secs", "20");
        mpv_set_option_string(mpv_, "network-timeout", "30");
        mpv_set_option_string(mpv_, "user-agent", "Mozilla/5.0 (Nintendo Switch) NuvioNX/0.1");
        mpv_set_option_string(mpv_, "keep-open", "no");
        // libass has no system fonts here: point it at the Switch system font we exported.
        mpv_set_option_string(mpv_, "sub-font-provider", "none");
        mpv_set_option_string(mpv_, "sub-fonts-dir", "sdmc:/switch/NuvioNX/fonts");
        mpv_set_option_string(mpv_, "osd-fonts-dir", "sdmc:/switch/NuvioNX/fonts");
        std::string fam = fontFamily();
        if (!fam.empty()) {
            mpv_set_option_string(mpv_, "sub-font", fam.c_str());
            mpv_set_option_string(mpv_, "osd-font", fam.c_str());
        }
        mpv_set_option_string(mpv_, "sub-font-size", "46");
        mpv_set_option_string(mpv_, "sub-border-size", "2.5");
        if (!st.subLang.empty()) mpv_set_option_string(mpv_, "slang", st.subLang.c_str());
        if (!st.audioLang.empty()) mpv_set_option_string(mpv_, "alang", st.audioLang.c_str());
        // https:// goes through the local relay (proxy.cpp), which applies the stream's
        // headers itself; only plain http:// streams need mpv to send them.
        playUrl_ = proxy::wrap(stream_.url, stream_.headers);
        bool relayed = playUrl_ != stream_.url;
        if (!stream_.headers.empty() && !relayed) {
            std::string hf;
            for (auto& h : stream_.headers) {
                if (h.compare(0, 11, "User-Agent:") == 0 || h.compare(0, 11, "user-agent:") == 0) {
                    std::string ua = h.substr(11);
                    while (!ua.empty() && ua[0] == ' ') ua.erase(0, 1);
                    mpv_set_option_string(mpv_, "user-agent", ua.c_str());
                    continue;
                }
                if (!hf.empty()) hf += ",";
                hf += h;
            }
            if (!hf.empty()) mpv_set_option_string(mpv_, "http-header-fields", hf.c_str());
        }

        if (mpv_initialize(mpv_) < 0) {
            error_ = "mpv failed to initialise.";
            return;
        }
        // GPU path (opt-in): mpv renders on the GPU via OpenGL, so the YUV->RGB convert and
        // scaling run on the Tegra GPU instead of the CPU - the fix for CPU-bound frame drops
        // at 1080p. Falls back to the software renderer if the GL context can't be created.
        useGl_ = store::settings().gpuVideo;
        if (useGl_) {
            win_ = App::get().window();
            sdlGlCtx_ = SDL_GL_GetCurrentContext();  // SDL_Renderer's GL context
            // Give mpv its OWN GL context that shares objects with SDL's. mpv resets GL state
            // to defaults on every render; a separate context keeps that from corrupting
            // SDL's cached state (which otherwise left the UI "bugged out").
            SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
            mpvGlCtx_ = (win_ && sdlGlCtx_) ? SDL_GL_CreateContext(win_) : nullptr;  // becomes current
            if (!mpvGlCtx_) {
                useGl_ = false;
                if (win_ && sdlGlCtx_) SDL_GL_MakeCurrent(win_, sdlGlCtx_);
                log("NuvioNX: couldn't create a shared GL context, using software renderer");
            } else {
                mpv_opengl_init_params gl{};
                gl.get_proc_address = [](void*, const char* name) -> void* {
                    return (void*)SDL_GL_GetProcAddress(name);
                };
                gl.get_proc_address_ctx = nullptr;
                mpv_render_param glp[] = {
                    {MPV_RENDER_PARAM_API_TYPE, (void*)MPV_RENDER_API_TYPE_OPENGL},
                    {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl},
                    {MPV_RENDER_PARAM_INVALID, nullptr},
                };
                int rc = mpv_render_context_create(&rctx_, mpv_, glp);  // on mpv's context
                SDL_GL_MakeCurrent(win_, sdlGlCtx_);                    // hand the context back to SDL
                if (rc < 0) {
                    rctx_ = nullptr;
                    useGl_ = false;
                    SDL_GL_DeleteContext(mpvGlCtx_);
                    mpvGlCtx_ = nullptr;
                    log("NuvioNX: OpenGL render unavailable, using software renderer");
                } else {
                    log("NuvioNX: using OpenGL (GPU) renderer with a shared context");
                }
            }
        }
        if (!rctx_) {
            mpv_render_param params[] = {
                {MPV_RENDER_PARAM_API_TYPE, (void*)MPV_RENDER_API_TYPE_SW},
                {MPV_RENDER_PARAM_INVALID, nullptr},
            };
            if (mpv_render_context_create(&rctx_, mpv_, params) < 0) {
                rctx_ = nullptr;
                error_ = "mpv renderer unavailable.";
                return;
            }
        }
        mpv_observe_property(mpv_, 1, "time-pos", MPV_FORMAT_DOUBLE);
        mpv_observe_property(mpv_, 2, "duration", MPV_FORMAT_DOUBLE);
        mpv_observe_property(mpv_, 3, "pause", MPV_FORMAT_FLAG);
        mpv_observe_property(mpv_, 4, "paused-for-cache", MPV_FORMAT_FLAG);
        mpv_observe_property(mpv_, 5, "cache-buffering-state", MPV_FORMAT_INT64);
        mpv_observe_property(mpv_, 6, "hwdec-current", MPV_FORMAT_STRING);
        mpv_observe_property(mpv_, 7, "current-ao", MPV_FORMAT_STRING);
        mpv_observe_property(mpv_, 8, "audio-params/channel-count", MPV_FORMAT_INT64);
        mpv_observe_property(mpv_, 9, "video-params/h", MPV_FORMAT_INT64);
        mpv_observe_property(mpv_, 10, "container-fps", MPV_FORMAT_DOUBLE);

        if (!useGl_) {
            // Software path: convert into an SDL streaming texture on a background thread.
            vw_ = st.render1080 ? 1920 : 1280;
            vh_ = st.render1080 ? 1080 : 720;
            tex_ = SDL_CreateTexture(R, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, vw_, vh_);
            if (tex_) SDL_SetTextureBlendMode(tex_, SDL_BLENDMODE_NONE);
            startRenderThread();
        } else {
            // GPU path: render on a dedicated thread/core into off-screen textures. Falls back
            // to the single-thread main-thread render (renderVideoGl) if setup fails.
            setupVideoGlThread();
        }
        if (st.cpuBoost) boosted_ = R_SUCCEEDED(appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad));

        const store::Progress* p = store::progress(ctx_.videoId);
        if (p && p->resumable()) {
            char buf[32];
            if (p->pct)  // from Trakt: only a percentage is known
                snprintf(buf, sizeof buf, "%.2f%%", std::max(0.0, p->pos - 0.5));
            else
                snprintf(buf, sizeof buf, "%.1f", std::max(0.0, p->pos - 3));
            mpv_set_property_string(mpv_, "start", buf);
        }
        openLog();
        // Warnings/errors only. Verbose logging during playback is heavy churn and
        // floods the diag buffer; it isn't needed once things work.
        mpv_request_log_messages(mpv_, "warn");
        log("NuvioNX: memory available " + std::to_string(App::get().memoryBudgetMB()) + " MB, applet type " +
            std::to_string((int)appletGetAppletType()));
        log("NuvioNX: playing " + std::string(relayed ? "(via local HTTPS relay) " : "") + redact(stream_.url) +
            " -> " + playUrl_.substr(0, playUrl_.find("/s/") + 3) + "...");
        const char* cmd[] = {"loadfile", playUrl_.c_str(), nullptr};
        mpv_command_async(mpv_, 0, cmd);
        appletSetMediaPlaybackState(true);
        poke();
    }

    void pumpEvents() {
        while (mpv_) {
            mpv_event* ev = mpv_wait_event(mpv_, 0);
            if (!ev || ev->event_id == MPV_EVENT_NONE) break;
            switch (ev->event_id) {
                case MPV_EVENT_FILE_LOADED:
                    loaded_ = true;
                    buffering_ = false;
                    if (subsReady_) applyAddonSubs();
                    break;
                case MPV_EVENT_LOG_MESSAGE: {
                    auto* m = (mpv_event_log_message*)ev->data;
                    std::string line = std::string("[") + m->prefix + "] " + m->level + ": " + m->text;
                    while (!line.empty() && line.back() == '\n') line.pop_back();
                    log(line);
                    if (m->log_level <= MPV_LOG_LEVEL_ERROR) lastError_ = trimmed(m->text);
                    break;
                }
                case MPV_EVENT_END_FILE: {
                    auto* ef = (mpv_event_end_file*)ev->data;
                    if (ef->reason == MPV_END_FILE_REASON_ERROR) {
                        error_ = std::string("The stream could not be played (") + mpv_error_string(ef->error) + ").";
                        if (!lastError_.empty()) error_ += "\n" + lastError_;
                        error_ += "\nDetails: sdmc:/switch/NuvioNX/mpv.log";
                        log("NuvioNX: playback failed: " + std::string(mpv_error_string(ef->error)));
                        // log file is written when the player closes (after mpv and the relay
                        // have shut their connections down), not in the middle of the teardown
                    } else if (ef->reason == MPV_END_FILE_REASON_EOF) {
                        onEnded();
                    }
                    break;
                }
                case MPV_EVENT_PROPERTY_CHANGE: {
                    auto* p = (mpv_event_property*)ev->data;
                    if (p->format == MPV_FORMAT_NONE || !p->data) break;
                    switch (ev->reply_userdata) {
                        case 1: pos_ = *(double*)p->data; break;
                        case 2: dur_ = *(double*)p->data; break;
                        case 3: paused_ = *(int*)p->data != 0; break;
                        case 4: buffering_ = *(int*)p->data != 0; break;
                        case 5: cachePct_ = *(int64_t*)p->data; break;
                        case 6:
                            hwdecCurrent_ = *(char**)p->data ? *(char**)p->data : "";
                            log("NuvioNX: hwdec-current = " + hwdecCurrent_);
                            checkResolution();
                            break;
                        case 7: log(std::string("NuvioNX: audio output = ") + (*(char**)p->data ? *(char**)p->data : "")); break;
                        case 8: log("NuvioNX: audio channels out = " + std::to_string(*(int64_t*)p->data)); break;
                        case 9:
                            videoH_ = *(int64_t*)p->data;
                            log("NuvioNX: video height = " + std::to_string(videoH_));
                            checkResolution();
                            break;
                        case 10:
                            videoFps_ = *(double*)p->data;
                            log("NuvioNX: container fps = " + std::to_string(videoFps_));
                            break;
                    }
                    break;
                }
                default: break;
            }
        }
    }

    void poke() { overlayUntil_ = now() + 4.0; }

    // Trakt scrobbling: "start" when playback (re)starts, "pause" when paused.
    void updateScrobble() {
        if (!loaded_ || dur_ <= 0 || !error_.empty()) return;
        bool playing = !paused_;
        if (scrobbleStarted_ && playing == scrobblePlaying_) return;
        double pct = pos_ / dur_ * 100.0;
        App::get().traktScrobble(playing ? "start" : "pause", ctx_.meta.id, ctx_.type, ctx_.season, ctx_.episode,
                                 pct);
        scrobbleStarted_ = true;
        scrobblePlaying_ = playing;
    }

    // GPU render: draw the current mpv video frame onto the window framebuffer (FBO 0).
    // Runs on the main thread with SDL's GL context current. SDL_RenderFlush lets SDL and
    // raw GL (mpv) share the context; mpv restores GL state afterwards so SDL keeps drawing.
    void renderVideoGl() {
        if (!rctx_ || !win_ || !mpvGlCtx_) return;
        SDL_RenderFlush(R);                    // finish SDL's queued GL work on its context
        SDL_GL_MakeCurrent(win_, mpvGlCtx_);   // switch to mpv's isolated context
        int w = 0, h = 0;
        SDL_GetRendererOutputSize(R, &w, &h);
        mpv_opengl_fbo fbo{};
        fbo.fbo = 0;  // default framebuffer = the shared window surface
        fbo.w = w;
        fbo.h = h;
        int flip = 1;  // GL is bottom-up; flip to match SDL's top-left origin
        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
            {MPV_RENDER_PARAM_FLIP_Y, &flip},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };
        mpv_render_context_render(rctx_, params);
        SDL_GL_MakeCurrent(win_, sdlGlCtx_);   // hand the context back to SDL for the UI
    }

    // ------------------------------------------------- threaded GPU render (dedicated core)
    // Create two off-screen SDL target textures and hand their GL names to the video thread,
    // which attaches them to FBOs in mpv's context and renders each frame into them on its own
    // core. The main thread then just SDL_RenderCopy's the latest finished one. Runs on the
    // main thread with SDL's context current. On any failure we leave glThreaded_ false and the
    // player falls back to the single-thread renderVideoGl path.
    void setupVideoGlThread() {
        if (!rctx_ || !win_ || !mpvGlCtx_ || !sdlGlCtx_) return;
        // Resolve the GLES2 framebuffer functions (SDL's GL context is current here).
        glGenFramebuffers_ = (PFN_glGenFramebuffers)SDL_GL_GetProcAddress("glGenFramebuffers");
        glBindFramebuffer_ = (PFN_glBindFramebuffer)SDL_GL_GetProcAddress("glBindFramebuffer");
        glFramebufferTexture2D_ = (PFN_glFramebufferTexture2D)SDL_GL_GetProcAddress("glFramebufferTexture2D");
        glCheckFramebufferStatus_ = (PFN_glCheckFramebufferStatus)SDL_GL_GetProcAddress("glCheckFramebufferStatus");
        glDeleteFramebuffers_ = (PFN_glDeleteFramebuffers)SDL_GL_GetProcAddress("glDeleteFramebuffers");
        glGetIntegerv_ = (PFN_glGetIntegerv)SDL_GL_GetProcAddress("glGetIntegerv");
        glFinish_ = (PFN_glFinish)SDL_GL_GetProcAddress("glFinish");
        if (!glGenFramebuffers_ || !glBindFramebuffer_ || !glFramebufferTexture2D_ ||
            !glCheckFramebufferStatus_ || !glDeleteFramebuffers_ || !glGetIntegerv_ || !glFinish_) {
            log("NuvioNX: GL framebuffer functions unavailable; threaded render disabled");
            return;
        }
        SDL_GetRendererOutputSize(R, &vw2_, &vh2_);
        if (vw2_ <= 0 || vh2_ <= 0) return;
        for (int i = 0; i < 2; i++) {
            videoTex_[i] = SDL_CreateTexture(R, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_TARGET, vw2_, vh2_);
            if (!videoTex_[i]) { teardownVideoGl(); return; }
            SDL_SetTextureBlendMode(videoTex_[i], SDL_BLENDMODE_NONE);
            // Force SDL to allocate the backing GL texture at full size.
            SDL_SetRenderTarget(R, videoTex_[i]);
            SDL_SetRenderDrawColor(R, 0, 0, 0, 255);
            SDL_RenderClear(R);
            SDL_SetRenderTarget(R, nullptr);
            // Discover the underlying GL texture name (shared with mpv's context by name).
            float tw = 0, th = 0;
            if (SDL_GL_BindTexture(videoTex_[i], &tw, &th) != 0) { teardownVideoGl(); return; }
            GLint id = 0;
            glGetIntegerv_(GL_TEXTURE_BINDING_2D, &id);
            SDL_GL_UnbindTexture(videoTex_[i]);
            if (id == 0) { teardownVideoGl(); return; }
            videoGlId_[i] = (unsigned)id;
        }
        SDL_RenderFlush(R);  // submit SDL's setup GL work before the thread touches the textures
        renderRun_ = true;
        glThreaded_ = true;
        mpv_render_context_set_update_callback(rctx_, onMpvUpdate, this);
        videoThread_ = std::thread([this] { videoGlLoop(); });
        log("NuvioNX: threaded GPU render started (" + std::to_string(vw2_) + "x" + std::to_string(vh2_) + ")");
    }

    void videoGlLoop() {
        if (SDL_GL_MakeCurrent(win_, mpvGlCtx_) != 0) {
            log("NuvioNX: video thread couldn't bind the GL context");
            glThreaded_ = false;
            return;
        }
        glGenFramebuffers_(2, videoFbo_);
        for (int i = 0; i < 2; i++) {
            glBindFramebuffer_(GL_FRAMEBUFFER, videoFbo_[i]);
            glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, videoGlId_[i], 0);
            if (glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                log("NuvioNX: video FBO incomplete; threaded render disabled");
                glBindFramebuffer_(GL_FRAMEBUFFER, 0);
                glDeleteFramebuffers_(2, videoFbo_);
                videoFbo_[0] = videoFbo_[1] = 0;
                SDL_GL_MakeCurrent(win_, nullptr);
                glThreaded_ = false;  // main thread falls back to renderVideoGl
                return;
            }
        }
        glBindFramebuffer_(GL_FRAMEBUFFER, 0);

        while (true) {
            {
                std::unique_lock<std::mutex> lk(wakeMx_);
                wakeCv_.wait_for(lk, std::chrono::milliseconds(100), [this] { return wake_ || !renderRun_; });
                if (!renderRun_) break;
                wake_ = false;
            }
            if (!(mpv_render_context_update(rctx_) & MPV_RENDER_UPDATE_FRAME)) continue;
            int back = (vpub_.load(std::memory_order_relaxed) == 0) ? 1 : 0;  // the un-published buffer
            mpv_opengl_fbo fbo{};
            fbo.fbo = (int)videoFbo_[back];
            fbo.w = vw2_;
            fbo.h = vh2_;
            int flip = 0;  // rendering into a texture SDL will sample top-left; no extra flip
            mpv_render_param params[] = {
                {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
                {MPV_RENDER_PARAM_FLIP_Y, &flip},
                {MPV_RENDER_PARAM_INVALID, nullptr},
            };
            mpv_render_context_render(rctx_, params);
            glFinish_();  // the main thread (other context) samples this texture; make sure it's done
            vpub_.store(back, std::memory_order_release);
        }
        if (videoFbo_[0] || videoFbo_[1]) glDeleteFramebuffers_(2, videoFbo_);
        videoFbo_[0] = videoFbo_[1] = 0;
        SDL_GL_MakeCurrent(win_, nullptr);  // release so the main thread can free the context
    }

    void stopVideoGlThread() {
        if (videoThread_.joinable()) {
            {
                std::lock_guard<std::mutex> lk(wakeMx_);
                renderRun_ = false;
                wake_ = true;
            }
            wakeCv_.notify_one();
            videoThread_.join();
        }
        if (rctx_ && glThreaded_) mpv_render_context_set_update_callback(rctx_, nullptr, nullptr);
        glThreaded_ = false;
        teardownVideoGl();
    }

    void teardownVideoGl() {
        for (int i = 0; i < 2; i++) {
            if (videoTex_[i]) {
                SDL_DestroyTexture(videoTex_[i]);
                videoTex_[i] = nullptr;
            }
        }
        vpub_.store(-1);
    }

    // ---------------------------------------------------------- render thread (software path)
    // mpv's software renderer (YUV->RGB, scaling to the output size) is the heaviest
    // per-frame step. Running it here, double-buffered, lets it use another CPU core in
    // parallel with the UI thread instead of eating into every 16 ms frame.
    static void onMpvUpdate(void* ctx) {
        auto* self = static_cast<PlayerScreen*>(ctx);
        {
            std::lock_guard<std::mutex> lk(self->wakeMx_);
            self->wake_ = true;
        }
        self->wakeCv_.notify_one();
    }

    void startRenderThread() {
        if (!rctx_) return;
        size_t bytes = (size_t)vw_ * vh_ * 4;
        for (int i = 0; i < 2; i++) {
            frameStore_[i].assign(bytes + 64, 0);
            uintptr_t p = (uintptr_t)frameStore_[i].data();
            frames_[i] = (uint8_t*)((p + 63) & ~(uintptr_t)63);  // 64-byte aligned for mpv's sw renderer
        }
        renderRun_ = true;
        mpv_render_context_set_update_callback(rctx_, onMpvUpdate, this);
        renderThread_ = std::thread([this] { renderLoop(); });
    }

    void stopRenderThread() {
        if (!renderThread_.joinable()) return;
        {
            std::lock_guard<std::mutex> lk(wakeMx_);
            renderRun_ = false;
            wake_ = true;
        }
        wakeCv_.notify_one();
        renderThread_.join();
        if (rctx_) mpv_render_context_set_update_callback(rctx_, nullptr, nullptr);
    }

    void renderLoop() {
        while (true) {
            {
                std::unique_lock<std::mutex> lk(wakeMx_);
                wakeCv_.wait_for(lk, std::chrono::milliseconds(100), [this] { return wake_ || !renderRun_; });
                if (!renderRun_) break;
                wake_ = false;
            }
            if (!(mpv_render_context_update(rctx_) & MPV_RENDER_UPDATE_FRAME)) continue;
            int back = 1 - front_.load();
            int size[2] = {vw_, vh_};
            size_t stride = (size_t)vw_ * 4;
            mpv_render_param params[] = {
                {MPV_RENDER_PARAM_SW_SIZE, size},
                {MPV_RENDER_PARAM_SW_FORMAT, (void*)"rgb0"},
                {MPV_RENDER_PARAM_SW_STRIDE, &stride},
                {MPV_RENDER_PARAM_SW_POINTER, frames_[back]},
                {MPV_RENDER_PARAM_INVALID, nullptr},
            };
            mpv_render_context_render(rctx_, params);
            std::lock_guard<std::mutex> lk(frameMx_);
            front_ = back;
            frameReady_ = true;
        }
    }

    // CPU-decoding anything above 1440p overwhelms the Switch (that's what crashed on 4K).
    // If hardware decoding didn't engage for such a stream, stop cleanly with an explanation.
    void checkResolution() {
        if (!mpv_ || !error_.empty() || videoH_ <= 1440) return;
        bool hw = !hwdecCurrent_.empty() && hwdecCurrent_ != "no";
        if (hw) return;
        // hwdec-current is reported once decoding starts; give it that chance first
        if (!hwdecReported_) {
            hwdecReported_ = true;
            if (hwdecCurrent_.empty()) return;
        }
        log("NuvioNX: stopping " + std::to_string(videoH_) + "p stream: no hardware decoding");
        const char* cmd[] = {"stop", nullptr};
        mpv_command_async(mpv_, 0, cmd);
        error_ = std::to_string(videoH_) + "p video can't be decoded on this stream without hardware decoding, "
                 "and software decoding at this resolution is too heavy for the Switch.\n"
                 "Pick a 1080p stream instead" +
                 std::string(store::settings().hwdec ? "." : ", or turn on Settings > Hardware decoding.");
    }

    void setPause(bool p) {
        if (!mpv_) return;
        int flag = p ? 1 : 0;
        mpv_set_property_async(mpv_, 0, "pause", MPV_FORMAT_FLAG, &flag);
        paused_ = p;
    }

    void seekAbsolute(double frac) {
        if (!mpv_ || !loaded_ || dur_ <= 0) return;
        std::string pct = std::to_string(std::max(0.0, std::min(100.0, frac * 100.0)));
        const char* cmd[] = {"seek", pct.c_str(), "absolute-percent", nullptr};
        mpv_command_async(mpv_, 0, cmd);
        pos_ = dur_ * frac;
        poke();
    }

    void seek(int secs) {
        if (!mpv_ || !loaded_) return;
        std::string s = std::to_string(secs);
        const char* cmd[] = {"seek", s.c_str(), "relative", nullptr};
        mpv_command_async(mpv_, 0, cmd);
        pos_ = std::max(0.0, std::min(dur_ > 0 ? dur_ : 1e9, pos_ + secs));
        poke();
    }

    void saveProgress() {
        if (dur_ <= 0 || pos_ <= 0) return;
        store::Progress p;
        p.videoId = ctx_.videoId;
        p.metaId = ctx_.meta.id;
        p.type = ctx_.type;
        p.name = ctx_.meta.name;
        p.poster = ctx_.meta.poster;
        p.background = ctx_.meta.background;
        p.logo = ctx_.meta.logo;
        p.epTitle = ctx_.epTitle;
        p.season = ctx_.season;
        p.episode = ctx_.episode;
        p.pos = ended_ ? dur_ : pos_;
        p.dur = dur_;
        store::setProgress(p);
    }

    void onEnded() {
        if (ended_) return;
        ended_ = true;
        if (dur_ > 0) pos_ = dur_;
        saveProgress();
        const stremio::Video* next = nullptr;
        if (ctx_.type == "series" && store::settings().autoplayNext) next = videoAfter(ctx_.meta, ctx_.videoId);
        if (!next) {
            App::get().pop();
            return;
        }
        PlayContext n = ctx_;
        n.videoId = next->id;
        n.season = next->season;
        n.episode = next->episode;
        n.epTitle = next->title;
        n.preferBinge = stream_.bingeGroup;
        n.autoPlay = !stream_.bingeGroup.empty();
        App::get().pop();                              // this player
        App::get().replaceTop(makeStreamsScreen(n));  // previous episode's stream list
        App::get().toast("Up next: " + episodeLabel(n.season, n.episode));
    }

    // ---------------------------------------------------------- subtitles
    void fetchSubtitles() {
        auto addons = App::get().enabledAddons();
        std::string type = ctx_.type, vid = ctx_.videoId;
        auto own = stream_.subtitles;
        async(
            [addons, type, vid, own]() {
                std::vector<stremio::Subtitle> all = own;
                for (auto& a : addons) {
                    if (!a.loaded || !a.supports("subtitles", type, vid)) continue;
                    auto subs = stremio::fetchSubtitles(a, type, vid);
                    for (auto& s : subs) {
                        if (s.label.empty()) s.label = a.name;
                        all.push_back(s);
                    }
                }
                return all;
            },
            [this](std::vector<stremio::Subtitle>& subs) {
                std::string pref = store::settings().subLang;
                std::stable_sort(subs.begin(), subs.end(), [&](const stremio::Subtitle& a, const stremio::Subtitle& b) {
                    return sameLang(a.lang, pref) && !sameLang(b.lang, pref);
                });
                addonSubs_ = subs;
                subsReady_ = true;
                if (loaded_) applyAddonSubs();
            });
    }

    void applyAddonSubs() {
        if (autoSubsDone_ || !mpv_) return;
        autoSubsDone_ = true;
        std::string pref = store::settings().subLang;
        if (pref.empty()) return;
        bool haveEmbedded = mpvString(mpv_, "sid") != "no" && !mpvString(mpv_, "sid").empty();
        int added = 0;
        for (size_t i = 0; i < addonSubs_.size() && added < 2; i++) {
            if (!sameLang(addonSubs_[i].lang, pref)) continue;
            addSub((int)i, (!haveEmbedded && added == 0) ? "select" : "auto");
            added++;
        }
    }

    void addSub(int i, const char* flag) {
        const auto& s = addonSubs_[i];
        if (added_.count(s.url)) return;
        added_.insert(s.url);
        std::string title = s.label.empty() ? ui::langName(s.lang) : s.label;
        std::string subUrl = proxy::wrap(s.url);  // subtitle files are usually https too
        const char* cmd[] = {"sub-add", subUrl.c_str(), flag, title.c_str(), s.lang.c_str(), nullptr};
        mpv_command_async(mpv_, 0, cmd);
    }

    // ---------------------------------------------------------- menus
    void openMenu(Menu m) {
        if (!mpv_) return;
        items_.clear();
        menuSel_ = 0;
        json::Value tracks;
        json::parse(mpvString(mpv_, "track-list"), tracks);
        const char* want = m == Subs ? "sub" : "audio";
        bool anySelected = false;
        if (m == Subs) {
            Item off;
            off.label = "Off";
            off.off = true;
            items_.push_back(off);
        }
        for (auto& tr : tracks.items()) {
            if (tr["type"].str() != want) continue;
            Item it;
            it.track = tr["id"].integer(-1);
            std::string lang = tr["lang"].str();
            std::string title = tr["title"].str();
            it.label = lang.empty() ? (title.empty() ? "Track " + std::to_string(it.track) : title) : ui::langName(lang);
            std::string sub = (!lang.empty() && !title.empty()) ? title : "";
            std::string codec = tr["codec"].str();
            if (m == Audio) {
                long long ch = tr["demux-channel-count"].integer(0);
                if (ch > 0) codec += " " + std::to_string(ch) + "ch";
            }
            std::string src = tr["external"].boolean() ? "Addon" : "Embedded";
            it.sub = sub.empty() ? src + "  " + codec : sub + "  -  " + src + "  " + codec;
            it.selected = tr["selected"].boolean();
            anySelected = anySelected || it.selected;
            items_.push_back(it);
        }
        if (m == Subs) {
            items_[0].selected = !anySelected;
            int shown = 0;
            for (size_t i = 0; i < addonSubs_.size() && shown < 60; i++) {
                if (added_.count(addonSubs_[i].url)) continue;
                Item it;
                it.addonSub = (int)i;
                it.label = ui::langName(addonSubs_[i].lang);
                it.sub = addonSubs_[i].label.empty() ? "Addon subtitle" : addonSubs_[i].label;
                items_.push_back(it);
                shown++;
            }
        }
        for (size_t i = 0; i < items_.size(); i++)
            if (items_[i].selected) menuSel_ = (int)i;
        menu_ = m;
        if (items_.empty()) {
            menu_ = None;
            App::get().toast(m == Subs ? "No subtitles available" : "No audio tracks");
        }
    }

    void applyMenu(const Item& it) {
        const char* prop = menu_ == Subs ? "sid" : "aid";
        if (it.off) {
            mpv_set_property_string(mpv_, prop, "no");
        } else if (it.addonSub >= 0) {
            addSub(it.addonSub, "select");
        } else if (it.track >= 0) {
            mpv_set_property_string(mpv_, prop, std::to_string(it.track).c_str());
        }
        poke();
    }

    // ---------------------------------------------------------- drawing
    void renderOverlay() {
        const Theme& t = theme();
        vGradient(0, 0, W, 150, withA(Col{0, 0, 0, 255}, 200), withA(Col{0, 0, 0, 255}, 0));
        vGradient(0, 520, W, 200, withA(Col{0, 0, 0, 255}, 0), withA(Col{0, 0, 0, 255}, 220));
        text(ctx_.meta.name, 76, 34, 26, t.text, true, Left, 800);
        std::string sub = episodeLabel(ctx_.season, ctx_.episode);
        if (!ctx_.epTitle.empty()) sub += (sub.empty() ? "" : "  -  ") + ctx_.epTitle;
        if (!sub.empty()) text(sub, 76, 70, 17, t.text2, false, Left, 800);
        text(stream_.name, W - 48, 40, 15, t.text3, true, Right, 360);

        if (paused_ && loaded_ && !buffering_) {
            roundRect(W / 2 - 40, H / 2 - 40, 80, 80, 40, withA(Col{0, 0, 0, 255}, 150));
            fillRect(W / 2 - 14, H / 2 - 18, 9, 36, t.text);
            fillRect(W / 2 + 5, H / 2 - 18, 9, 36, t.text);
        }

        float by = 628, bx = 48, bw = W - 96;
        float frac = dur_ > 0 ? (float)std::min(1.0, pos_ / dur_) : 0.f;
        // tap on the bar to jump; fine-grained via a dense set of regions
        if (dur_ > 0 && menu_ == None) {
            const int slices = 200;
            for (int i = 0; i < slices; i++) {
                float sx = bx + bw * i / slices;
                double target = (i + 0.5) / slices;
                ui::hit(sx, by - 22, bw / slices + 0.5f, 50, [this, target] { seekAbsolute(target); });
            }
        }
        ui::backButton(14, 22);
        roundRect(bx, by, bw, 6, 3, withA(t.text, 70));
        roundRect(bx, by, std::max(6.f, bw * frac), 6, 3, t.accent);
        circle(bx + bw * frac, by + 3, 8, t.text);
        text(ui::fmtTime(pos_), bx, by + 16, 16, t.text, true);
        text(dur_ > 0 ? "-" + ui::fmtTime(dur_ - pos_) : "--:--", bx + bw, by + 16, 16, t.text2, true, Right);
        ui::hints({{"A", paused_ ? "Play" : "Pause"},
                   {"<>", "10s"},
                   {"LR", "60s"},
                   {"Y", "Subtitles"},
                   {"X", "Audio"},
                   {"-", "Info"},
                   {"B", "Back"}},
                  680);
    }

    void renderMenu() {
        const Theme& t = theme();
        float w = 440, x = W - w - 32, y = 40, h = H - 80;
        roundRect(x, y, w, h, 18, withA(t.elevated, 245));
        text(menu_ == Subs ? "Subtitles" : "Audio", x + 24, y + 20, 22, t.text, true);
        float iy0 = y + 64, ih = 58;
        int visible = (int)((h - 80) / ih);
        int first = std::max(0, std::min(menuSel_ - visible / 2, (int)items_.size() - visible));
        pushClip(x, iy0, w, h - 76);
        for (int i = first; i < (int)items_.size() && i < first + visible + 1; i++) {
            float iy = iy0 + (i - first) * ih;
            const Item& it = items_[i];
            bool f = i == menuSel_;
            if (f) roundRect(x + 12, iy, w - 24, ih - 6, 12, withA(t.text, 235));
            Col c1 = f ? t.bg : t.text, c2 = f ? withA(t.bg, 200) : t.text3;
            text(it.label, x + 28, iy + (it.sub.empty() ? 15 : 7), 17, c1, true, Left, w - 100);
            if (!it.sub.empty()) text(it.sub, x + 28, iy + 29, 13, c2, false, Left, w - 100);
            if (it.selected) circle(x + w - 40, iy + ih / 2 - 3, 6, f ? t.bg : t.accent);
            float hy = std::max(iy, iy0), hb = std::min(iy + ih - 6, y + h - 12);
            if (hb > hy) ui::tapToActivate(x + 12, hy, w - 24, hb - hy, [this, i] { menuSel_ = i; });
        }
        popClip();
        // swallow taps on the panel background so they don't hit the video behind
        ui::hit(x, y, w, 60, [] {});
    }

    void renderInfo() {
        const Theme& t = theme();
        roundRect(32, 120, 470, 340, 16, withA(t.elevated, 235));
        text("Y  Save log      B  Close", 52, 424, 14, t.text3, true);
        ui::hit(32, 410, 470, 50, [] { input::inject(input::Y); });
        float y = 140;
        auto row = [&](const char* k, const std::string& v) {
            text(k, 52, y, 15, t.text3, true);
            text(v.empty() ? "-" : v, 200, y, 15, t.text, false, Left, 290);
            y += 26;
        };
        row("Stream", stream_.name);
        row("Addon", stream_.addonName);
        row("Video", mpvString(mpv_, "video-codec"));
        row("Resolution", mpvString(mpv_, "video-params/w") + "x" + mpvString(mpv_, "video-params/h"));
        row("HW decode", mpvString(mpv_, "hwdec-current"));
        row("Audio", mpvString(mpv_, "audio-codec-name") + "  ->  " + mpvString(mpv_, "current-ao"));
        row("Dropped", mpvString(mpv_, "frame-drop-count"));
        std::string cache = mpvString(mpv_, "demuxer-cache-duration");
        if (!cache.empty()) cache = cache.substr(0, cache.find('.')) + " s";
        row("Buffered", cache);
        row("Output", std::to_string(vw_) + "x" + std::to_string(vh_) + " (software)");
    }

    // ---------------------------------------------------------- diagnostics log
    // Log lines (player + relay) are kept in RAM and only written to the SD card
    // when playback fails, so normal viewing never writes a log file.
    void openLog() { diag::clear(); }
    void log(const std::string& line) { diag::log(line); }
    void writeLogFile() {
        if (logWritten_) return;
        logWritten_ = true;
        diag::writeFile();
    }
    static std::string trimmed(std::string s) {
        while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
        return s;
    }
    // Keep debrid/API tokens out of the log file: show scheme + host only.
    static std::string redact(const std::string& url) {
        size_t s = url.find("://");
        if (s == std::string::npos) return "(url)";
        size_t e = url.find('/', s + 3);
        return url.substr(0, e == std::string::npos ? url.size() : e) + "/...";
    }

    PlayContext ctx_;
    stremio::Stream stream_;
    std::string playUrl_;
    std::string lastError_;
    std::string hwdecCurrent_;
    long long videoH_ = 0;
    double videoFps_ = 0;  // container frame rate; drives movie-mode frame cap
    bool hwdecReported_ = false;
    bool logWritten_ = false;
    mpv_handle* mpv_ = nullptr;
    mpv_render_context* rctx_ = nullptr;
    SDL_Texture* tex_ = nullptr;
    int vw_ = 1280, vh_ = 720;
    bool haveFrame_ = false;
    bool useGl_ = false;  // GPU (OpenGL) render path instead of the software path
    SDL_Window* win_ = nullptr;
    SDL_GLContext sdlGlCtx_ = nullptr;  // SDL_Renderer's context
    SDL_GLContext mpvGlCtx_ = nullptr;  // mpv's own context, sharing objects with SDL's
    bool boosted_ = false;
    bool scrobbleStarted_ = false, scrobblePlaying_ = false;
    // render thread state
    std::thread renderThread_;
    std::mutex wakeMx_, frameMx_;
    std::condition_variable wakeCv_;
    bool wake_ = false, renderRun_ = false;
    std::vector<uint8_t> frameStore_[2];
    uint8_t* frames_[2] = {nullptr, nullptr};
    std::atomic<int> front_{0};
    bool frameReady_ = false;

    // Multithreaded GPU render: a dedicated thread owns mpv's GL context and renders each
    // video frame into one of two off-screen textures (double-buffered). The main thread just
    // SDL_RenderCopy's the latest finished one, so the heavy upload/convert runs on its own
    // core instead of competing with input/UI/present on the main thread.
    std::thread videoThread_;
    SDL_Texture* videoTex_[2] = {nullptr, nullptr};  // SDL target textures mpv renders into
    unsigned videoGlId_[2] = {0, 0};                 // their underlying GL texture names
    unsigned videoFbo_[2] = {0, 0};                  // FBOs (live in mpv's context only)
    std::atomic<int> vpub_{-1};                      // index of the last finished frame (-1 = none)
    int vw2_ = 0, vh2_ = 0;                           // off-screen render size
    bool glThreaded_ = false;                        // true once the threaded path is live
    PFN_glGenFramebuffers glGenFramebuffers_ = nullptr;
    PFN_glBindFramebuffer glBindFramebuffer_ = nullptr;
    PFN_glFramebufferTexture2D glFramebufferTexture2D_ = nullptr;
    PFN_glCheckFramebufferStatus glCheckFramebufferStatus_ = nullptr;
    PFN_glDeleteFramebuffers glDeleteFramebuffers_ = nullptr;
    PFN_glGetIntegerv glGetIntegerv_ = nullptr;
    PFN_glFinish glFinish_ = nullptr;

    double pos_ = 0, dur_ = 0;
    bool paused_ = false, buffering_ = true, loaded_ = false, ended_ = false;
    long long cachePct_ = 0;
    std::string error_;
    double overlayUntil_ = 0, lastSave_ = 0;

    Menu menu_ = None;
    int menuSel_ = 0;
    std::vector<Item> items_;
    std::vector<stremio::Subtitle> addonSubs_;
    std::set<std::string> added_;
    bool subsReady_ = false, autoSubsDone_ = false;
};

}  // namespace

std::shared_ptr<Screen> makePlayerScreen(const PlayContext& ctx, const stremio::Stream& s) {
    return std::make_shared<PlayerScreen>(ctx, s);
}
