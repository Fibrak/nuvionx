// Stream picker: queries every enabled addon with a "stream" resource in parallel.
#include <algorithm>

#include "debrid.hpp"
#include "input.hpp"
#include "screens.hpp"
#include "store.hpp"

using namespace gfx;

namespace {

class StreamsScreen : public Screen {
public:
    explicit StreamsScreen(const PlayContext& ctx) : ctx_(ctx) {
        const store::Progress* p = store::progress(ctx_.videoId);
        if (p && p->resumable()) {
            resumeAt_ = p->pos;
            resumePct_ = p->pct;
        }
        fetchStreams();
        if (ctx_.type == "series" && ctx_.meta.videos.empty()) fetchMeta();
    }

    void update(float dt) override {
        dt_ = dt;
        if (resolving_) {
            if (input::pressed(input::B)) {
                resolving_ = false;
                resolveGen_++;  // ignore the pending result
            }
            return;
        }
        if (input::pressed(input::B)) {
            App::get().pop();
            return;
        }
        auto chips = filterChips();
        if (input::pressed(input::L) && filter_ > 0) setFilter(filter_ - 1);
        if (input::pressed(input::R) && filter_ + 1 < (int)chips.size()) setFilter(filter_ + 1);

        auto list = visibleList();
        if (!list.empty()) {
            if (input::pressed(input::Up)) sel_ = std::max(0, sel_ - 1);
            if (input::pressed(input::Down)) sel_ = std::min((int)list.size() - 1, sel_ + 1);
            sel_ = std::min(sel_, (int)list.size() - 1);
            if (input::pressed(input::A)) play(stream(list[sel_]));
        }
        float target = std::max(0.f, sel_ * kItemH - kItemH * 1.5f);
        scroll_ = ui::approach(scroll_, target, dt, 16);
    }

    void render() override {
        const Theme& t = theme();
        bd_.set(ctx_.meta.background.empty() ? ctx_.meta.poster : ctx_.meta.background, 0);
        bd_.render(dt_, 0.55f);

        // left panel
        float x = 48;
        SDL_Texture* poster = image(ctx_.meta.poster, 300, 450);
        if (poster) drawCover(poster, x, 92, 180, 270, 12);
        else roundRect(x, 92, 180, 270, 12, t.card);
        float y = 380;
        auto lines = wrap(ctx_.meta.name, 24, 340, 2, true);
        for (auto& l : lines) {
            text(l, x, y, 24, t.text, true);
            y += 30;
        }
        if (ctx_.season > 0 || ctx_.episode > 0) {
            text(episodeLabel(ctx_.season, ctx_.episode), x, y + 4, 17, t.accent, true);
            y += 26;
            if (!ctx_.epTitle.empty()) {
                text(ctx_.epTitle, x, y + 4, 16, t.text2, false, Left, 340);
                y += 24;
            }
        }
        if (resumeAt_ > 0) {
            text(resumePct_ ? "Resumes at " + std::to_string((int)resumeAt_) + "%"
                            : "Resumes at " + ui::fmtTime(resumeAt_),
                 x, y + 10, 15, t.text3);
            y += 26;
        }
        if (pending_ > 0) {
            spinner(x + 12, y + 30, 10, t.text2);
            text("Searching " + std::to_string(pending_) + " addon" + (pending_ == 1 ? "" : "s") + "...", x + 32,
                 y + 20, 15, t.text2);
        }

        // filter chips
        auto chips = filterChips();
        float cx = 440;
        for (size_t i = 0; i < chips.size(); i++) {
            float cw0 = ui::chip(chips[i], cx, 84, false, (int)i == filter_, 15);
            int fi = (int)i;
            ui::hit(cx, 78, cw0, 46, [this, fi] { setFilter(fi); });
            cx += cw0 + 8;
            if (cx > W - 80) break;
        }

        // list
        auto list = visibleList();
        float top = 136, bottom = 664;
        if (list.empty()) {
            if (pending_ > 0) {
                spinner(836, 360, 20, t.text2);
            } else if (groups_.empty()) {
                text("No stream addons installed", 836, 320, 22, t.text, true, Center);
                text("Install one from the Addons tab to watch titles.", 836, 354, 16, t.text2, false, Center);
            } else {
                text("No streams found", 836, 340, 22, t.text, true, Center);
            }
        }
        pushClip(420, top - 6, W - 420, bottom - top + 6);
        for (size_t i = 0; i < list.size(); i++) {
            float iy = top + i * kItemH - scroll_;
            if (iy + kItemH < top - 10) continue;
            if (iy > bottom) break;
            renderItem(stream(list[i]), 440, iy, W - 48 - 440, kItemH - 10, (int)i == sel_);
            float hy = std::max(iy, top), hb = std::min(iy + kItemH - 10, bottom);
            int si = (int)i;
            if (hb > hy) ui::tapToActivate(440, hy, W - 48 - 440, hb - hy, [this, si] { sel_ = si; });
        }
        popClip();
        ui::backButton();

        if (resolving_) {
            fillRect(0, 0, W, H, withA(t.bg, 200));
            roundRect(W / 2 - 230, H / 2 - 70, 460, 140, 18, t.elevated);
            spinner(W / 2, H / 2 - 20, 16, t.text);
            text("Resolving with " + debrid::activeProvider(store::settings()) + "...", W / 2, H / 2 + 12, 18,
                 t.text, true, Center);
            text("B to cancel", W / 2, H / 2 + 40, 14, t.text3, false, Center);
            ui::hit(0, 0, W, H, [] { input::inject(input::B); });  // tap anywhere cancels
            return;
        }
        vGradient(0, 640, W, 80, withA(t.bg, 0), withA(t.bg, 230));
        std::vector<ui::Hint> h = {{"A", "Play"}, {"B", "Back"}};
        if (chips.size() > 1) h.push_back({"LR", "Filter addon"});
        ui::hints(h);
    }

private:
    static constexpr float kItemH = 96;

    struct Group {
        std::string name;
        std::vector<stremio::Stream> streams;
    };
    struct Ref {
        int g, s;
    };

    const stremio::Stream& stream(Ref r) const { return groups_[r.g].streams[r.s]; }

    std::vector<std::string> filterChips() const {
        std::vector<std::string> out{"All"};
        for (auto& g : groups_)
            if (!g.streams.empty()) out.push_back(g.name);
        return out;
    }

    void setFilter(int f) {
        filter_ = f;
        sel_ = 0;
        scroll_ = 0;
    }

    std::vector<Ref> visibleList() const {
        std::vector<Ref> out;
        const store::Settings& st = store::settings();
        bool showUnsupported = st.showUnsupported;
        int chipIdx = 0;
        for (size_t g = 0; g < groups_.size(); g++) {
            if (groups_[g].streams.empty()) continue;
            chipIdx++;
            if (filter_ != 0 && chipIdx != filter_) continue;
            for (size_t s = 0; s < groups_[g].streams.size(); s++) {
                if (!showUnsupported && !debrid::canPlay(groups_[g].streams[s], st)) continue;
                out.push_back({(int)g, (int)s});
            }
        }
        // playable streams first, keeping addon order otherwise
        std::stable_sort(out.begin(), out.end(), [this, &st](const Ref& a, const Ref& b) {
            return debrid::canPlay(stream(a), st) && !debrid::canPlay(stream(b), st);
        });
        return out;
    }

    void renderItem(const stremio::Stream& s, float x, float y, float w, float h, bool focused) {
        const Theme& t = theme();
        const store::Settings& st = store::settings();
        bool ok = debrid::canPlay(s, st);
        roundRect(x, y, w, h, 12, focused ? withA(t.surface, 250) : withA(t.elevated, 200));
        if (focused) ui::focusRing(x, y, w, h, 12);
        Col main = ok ? t.text : t.text3;
        auto nameLines = wrap(s.name, 16, 150, 3, true);
        float ny = y + 12;
        for (auto& l : nameLines) {
            text(l, x + 16, ny, 16, main, true);
            ny += 20;
        }
        std::string desc = s.description.empty() ? s.title : s.description;
        if (desc.empty()) desc = s.filename;
        paragraph(desc, x + 184, y + 12, w - 184 - 120, 14, ok ? t.text2 : t.text3, 4, 1.28f);
        text(s.addonName, x + w - 14, y + 12, 13, t.text3, false, Right, 110);
        std::string kind = s.kindLabel();
        if (kind == "Torrent" && ok) kind = debrid::shortLabel(st) + " Torrent";
        if (!kind.empty()) {
            float bw = textWidth(kind, 12, true) + 16;
            roundRect(x + w - 14 - bw, y + h - 30, bw, 20, 10, withA(t.text3, 90));
            text(kind, x + w - 14 - bw / 2, y + h - 28, 12, t.text2, true, Center);
        }
    }

    void fetchStreams() {
        for (auto& a : App::get().enabledAddons()) {
            if (!a.loaded || !a.supports("stream", ctx_.type, ctx_.videoId)) continue;
            groups_.push_back({a.name, {}});
            int gi = (int)groups_.size() - 1;
            pending_++;
            std::string type = ctx_.type, vid = ctx_.videoId;
            async([a, type, vid]() { return stremio::fetchStreams(a, type, vid, nullptr); },
                  [this, gi](std::vector<stremio::Stream>& res) {
                      pending_--;
                      groups_[gi].streams = res;
                      tryAutoPlay();
                  });
        }
    }

    void fetchMeta() {
        auto addons = App::get().enabledAddons();
        std::string type = ctx_.type, id = ctx_.meta.id;
        async(
            [addons, type, id]() {
                std::pair<bool, stremio::Meta> r{false, {}};
                for (auto& a : addons)
                    if (a.loaded && a.supports("meta", type, id) && stremio::fetchMeta(a, type, id, r.second)) {
                        r.first = true;
                        break;
                    }
                return r;
            },
            [this](std::pair<bool, stremio::Meta>& r) {
                if (!r.first) return;
                auto m = r.second;
                if (m.background.empty()) m.background = ctx_.meta.background;
                if (m.logo.empty()) m.logo = ctx_.meta.logo;
                ctx_.meta = m;
                for (auto& v : m.videos)
                    if (v.id == ctx_.videoId && ctx_.epTitle.empty()) ctx_.epTitle = v.title;
            });
    }

    void tryAutoPlay() {
        if (!ctx_.autoPlay || autoDone_) return;
        for (auto& g : groups_)
            for (auto& s : g.streams)
                if (s.playable() && !ctx_.preferBinge.empty() && s.bingeGroup == ctx_.preferBinge) {
                    autoDone_ = true;
                    play(s);
                    return;
                }
        if (pending_ == 0) {
            autoDone_ = true;
            App::get().toast("Pick a stream for the next episode");
        }
    }

    void play(const stremio::Stream& s) {
        const store::Settings st = store::settings();
        if (!s.playable() && debrid::canPlay(s, st)) {
            resolving_ = true;
            int gen = ++resolveGen_;
            async([s, st]() { return debrid::resolve(s, st); },
                  [this, s, gen](debrid::Result& r) {
                      if (gen != resolveGen_) return;
                      resolving_ = false;
                      if (!r.ok) {
                          App::get().toast(r.error);
                          return;
                      }
                      stremio::Stream resolved = s;
                      resolved.url = r.url;
                      App::get().push(makePlayerScreen(ctx_, resolved));
                  });
            return;
        }
        if (!s.playable()) {
            std::string k = s.kindLabel();
            if (k == "Torrent")
                App::get().toast("Torrent streams can't play on Switch. Use a debrid-enabled addon.");
            else
                App::get().toast(k + " streams aren't supported on Switch");
            return;
        }
        App::get().push(makePlayerScreen(ctx_, s));
    }

    PlayContext ctx_;
    std::vector<Group> groups_;
    int pending_ = 0;
    int filter_ = 0, sel_ = 0;
    float scroll_ = 0, dt_ = 1.f / 60.f;
    double resumeAt_ = -1;
    bool resumePct_ = false;
    bool autoDone_ = false;
    bool resolving_ = false;
    int resolveGen_ = 0;
    ui::Backdrop bd_;
};

}  // namespace

std::shared_ptr<Screen> makeStreamsScreen(const PlayContext& ctx) { return std::make_shared<StreamsScreen>(ctx); }
