// Title details: hero, play/resume, library, seasons + episodes.
#include <algorithm>

#include "input.hpp"
#include "screens.hpp"
#include "store.hpp"

using namespace gfx;

const stremio::Video* videoAfter(const stremio::Meta& m, const std::string& videoId) {
    for (size_t i = 0; i < m.videos.size(); i++) {
        if (m.videos[i].id != videoId) continue;
        int curSeason = m.videos[i].season;
        for (size_t j = i + 1; j < m.videos.size(); j++) {
            // don't wander from a regular season into specials
            if (curSeason > 0 && m.videos[j].season == 0) continue;
            return &m.videos[j];
        }
        return nullptr;
    }
    return nullptr;
}

const stremio::Video* nextUpVideo(const stremio::Meta& m, double* resumeAt) {
    if (resumeAt) *resumeAt = -1;
    if (m.videos.empty()) return nullptr;
    const store::Progress* lp = store::latestForMeta(m.id);
    if (lp) {
        for (auto& v : m.videos) {
            if (v.id != lp->videoId) continue;
            if (!lp->watched()) {
                if (resumeAt) *resumeAt = lp->resumable() ? lp->pos : -1;
                return &v;
            }
            const stremio::Video* n = videoAfter(m, v.id);
            return n ? n : &v;
        }
    }
    for (auto& v : m.videos)
        if (v.season > 0) return &v;
    return &m.videos.front();
}

namespace {

class DetailScreen : public Screen {
public:
    explicit DetailScreen(const stremio::MetaPreview& p) {
        static_cast<stremio::MetaPreview&>(meta_) = p;
        meta_.progress = -1;
        fetch();
    }

    void onResume() override { computeSeasons(false); }

    void update(float dt) override {
        if (input::pressed(input::B)) {
            App::get().pop();
            return;
        }
        bool series = !seasons_.empty();
        if (series && input::pressed(input::L)) changeSeason(-1);
        if (series && input::pressed(input::R)) changeSeason(+1);

        if (zone_ == 0) {
            if (input::pressed(input::Left)) btn_ = std::max(0, btn_ - 1);
            if (input::pressed(input::Right)) btn_ = std::min(1, btn_ + 1);
            if (input::pressed(input::Down) && series) zone_ = 1;
            if (input::pressed(input::A)) {
                if (btn_ == 0) playMain();
                else toggleLibrary();
            }
        } else if (zone_ == 1) {
            if (input::pressed(input::Left)) changeSeason(-1);
            if (input::pressed(input::Right)) changeSeason(+1);
            if (input::pressed(input::Up)) zone_ = 0;
            if (input::pressed(input::Down) || input::pressed(input::A)) {
                if (!eps_.empty()) zone_ = 2;
            }
        } else {
            if (input::pressed(input::Left)) ep_ = std::max(0, ep_ - 1);
            if (input::pressed(input::Right)) ep_ = std::min((int)eps_.size() - 1, ep_ + 1);
            if (input::pressed(input::Up)) zone_ = 1;
            if (!eps_.empty() && input::pressed(input::A)) playVideo(meta_.videos[eps_[ep_]]);
            if (!eps_.empty() && input::pressed(input::Y)) toggleWatched(meta_.videos[eps_[ep_]]);
        }
        if (zone_ == 0 && input::pressed(input::Y)) toggleLibrary();

        float target = std::max(0.f, ep_ * (236.f + 14.f) - (236.f + 14.f));
        epScroll_ = ui::approach(epScroll_, target, dt, 16);
        dt_ = dt;
    }

    void render() override {
        const Theme& t = theme();
        bd_.set(meta_.background.empty() ? meta_.poster : meta_.background, 0);
        bd_.render(dt_);
        bool series = meta_.type == "series" || !seasons_.empty();

        float x = 48;
        SDL_Texture* logo = meta_.logo.empty() ? nullptr : image(meta_.logo, 600, 200);
        float lf = logo ? imageFade(meta_.logo, 0.2f) : 0.f;
        if (lf < 1.f) {
            auto lines = wrap(meta_.name, 38, 700, 2, true);
            float ty = lines.size() == 1 ? 112 : 64;
            for (auto& l : lines) {
                text(l, x, ty, 38, withA(t.text, (Uint8)(255 * (1.f - lf))), true);
                ty += 50;
            }
        }
        if (logo) drawFit(logo, x, 64, 440, 100, (Uint8)(255 * lf));

        std::string line;
        auto add = [&](const std::string& s) {
            if (s.empty()) return;
            if (!line.empty()) line += "   ";
            line += s;
        };
        add(stremio::typeLabel(meta_.type, false));
        add(meta_.releaseInfo);
        add(meta_.runtime);
        if (!meta_.imdbRating.empty()) add("IMDb " + meta_.imdbRating);
        text(line, x, 176, 17, t.text, true, Left, 760);
        std::string g;
        for (size_t i = 0; i < meta_.genres.size() && i < 4; i++) g += (i ? "  /  " : "") + meta_.genres[i];
        text(g, x, 202, 15, t.text3, false, Left, 760);

        paragraph(meta_.description, x, 232, 620, 16, t.text2, series ? 3 : 5);
        std::string cast;
        for (size_t i = 0; i < meta_.cast.size() && i < 4; i++) cast += (i ? ", " : "") + meta_.cast[i];
        float by;
        if (series) {
            if (!cast.empty()) text("Starring  " + cast, x, 306, 15, t.text3, false, Left, 700);
            by = 344;
        } else {
            if (!cast.empty()) text("Starring  " + cast, x, 350, 15, t.text3, false, Left, 700);
            if (!meta_.director.empty())
                text("Director  " + meta_.director[0], x, 374, 15, t.text3, false, Left, 700);
            by = 416;
        }

        // buttons
        const std::string& playLabel = playButtonLabel();
        ui::button(playLabel, x, by, 48, zone_ == 0 && btn_ == 0, true);
        ui::tapToActivate(x, by, ui::buttonWidth(playLabel), 48, [this] {
            zone_ = 0;
            btn_ = 0;
        });
        float bx = x + ui::buttonWidth(playLabel) + 14;
        std::string libLabel = store::inLibrary(meta_.id) ? "In Library" : "+ Library";
        ui::button(libLabel, bx, by, 48, zone_ == 0 && btn_ == 1);
        ui::tapToActivate(bx, by, ui::buttonWidth(libLabel), 48, [this] {
            zone_ = 0;
            btn_ = 1;
        });
        ui::backButton();
        if (loading_) spinner(bx + ui::buttonWidth("+ Library") + 40, by + 24, 12, t.text2);

        if (!seasons_.empty()) renderEpisodes(406);

        // hint bar (adapts to where focus is)
        vGradient(0, 642, W, 78, withA(t.bg, 0), withA(t.bg, 235));
        std::vector<ui::Hint> h;
        if (zone_ == 0) {
            h.push_back({"A", btn_ == 0 ? "Play" : (store::inLibrary(meta_.id) ? "In Library" : "Library")});
            if (series) h.push_back({"Down", "Episodes"});
            h.push_back({"Y", "Library"});
        } else if (zone_ == 1) {
            h = {{"LR", "Season"}, {"Down", "Episodes"}, {"Up", "Back up"}};
        } else {
            h = {{"A", "Play"}, {"Y", "Watched"}, {"<>", "Episode"}, {"Up", "Seasons"}};
        }
        h.push_back({"B", "Back"});
        ui::hints(h);
    }

private:
    void fetch() {
        auto addons = App::get().enabledAddons();
        std::string type = meta_.type, id = meta_.id;
        async(
            [addons, type, id]() {
                std::pair<bool, stremio::Meta> res{false, {}};
                for (auto& a : addons) {
                    if (!a.loaded || !a.supports("meta", type, id)) continue;
                    if (stremio::fetchMeta(a, type, id, res.second)) {
                        res.first = true;
                        break;
                    }
                }
                return res;
            },
            [this](std::pair<bool, stremio::Meta>& res) {
                loading_ = false;
                if (res.first) {
                    stremio::Meta m = res.second;
                    // keep catalog artwork if the meta lacks it
                    if (m.poster.empty()) m.poster = meta_.poster;
                    if (m.background.empty()) m.background = meta_.background;
                    if (m.logo.empty()) m.logo = meta_.logo;
                    if (m.description.empty()) m.description = meta_.description;
                    meta_ = m;
                }
                computeSeasons(true);
            });
    }

    void computeSeasons(bool selectNextUp) {
        playLabelRev_ = -1;  // the metadata may have changed
        seasons_.clear();
        for (auto& v : meta_.videos)
            if (std::find(seasons_.begin(), seasons_.end(), v.season) == seasons_.end()) seasons_.push_back(v.season);
        // regular seasons first, specials last
        std::sort(seasons_.begin(), seasons_.end(), [](int a, int b) {
            if ((a == 0) != (b == 0)) return b == 0;
            return a < b;
        });
        if (meta_.type != "series" && seasons_.size() <= 1 && meta_.videos.size() <= 1) seasons_.clear();
        if (seasons_.empty()) return;
        if (selectNextUp) {
            double r;
            const stremio::Video* nu = nextUpVideo(meta_, &r);
            seasonIdx_ = 0;
            if (nu) {
                for (size_t i = 0; i < seasons_.size(); i++)
                    if (seasons_[i] == nu->season) seasonIdx_ = (int)i;
            }
            fillEpisodes();
            if (nu)
                for (size_t i = 0; i < eps_.size(); i++)
                    if (meta_.videos[eps_[i]].id == nu->id) ep_ = (int)i;
        } else {
            seasonIdx_ = std::min(seasonIdx_, (int)seasons_.size() - 1);
            int keep = ep_;
            fillEpisodes();
            ep_ = std::min(keep, std::max(0, (int)eps_.size() - 1));
        }
    }

    void fillEpisodes() {
        eps_.clear();
        ep_ = 0;
        if (seasons_.empty()) return;
        int s = seasons_[seasonIdx_];
        for (size_t i = 0; i < meta_.videos.size(); i++)
            if (meta_.videos[i].season == s) eps_.push_back((int)i);
    }

    void changeSeason(int d) {
        if (seasons_.empty()) return;
        int n = std::max(0, std::min((int)seasons_.size() - 1, seasonIdx_ + d));
        if (n != seasonIdx_) {
            seasonIdx_ = n;
            fillEpisodes();
            epScroll_ = 0;
        }
    }

    // Finding the next episode scans the watch history and the episode list, so only redo it
    // when progress or the metadata changed.
    const std::string& playButtonLabel() {
        int rev = store::progressRevision();
        if (rev != playLabelRev_) {
            playLabelRev_ = rev;
            playLabel_ = computePlayLabel();
        }
        return playLabel_;
    }

    std::string computePlayLabel() {
        if (!seasons_.empty()) {
            double r;
            const stremio::Video* nu = nextUpVideo(meta_, &r);
            if (!nu) return "Play";
            std::string ep = episodeLabel(nu->season, nu->episode);
            return (r > 0 ? "Resume " : "Play ") + ep;
        }
        const store::Progress* p = store::progress(meta_.id);
        if (p && p->resumable())
            return p->pct ? "Resume (" + std::to_string((int)p->pos) + "%)" : "Resume " + ui::fmtTime(p->pos);
        return "Play";
    }

    void playMain() {
        if (!seasons_.empty()) {
            double r;
            const stremio::Video* nu = nextUpVideo(meta_, &r);
            if (nu) playVideo(*nu);
            return;
        }
        if (meta_.type == "series" && loading_) {
            App::get().toast("Loading episodes...");
            return;
        }
        PlayContext ctx;
        ctx.meta = meta_;
        ctx.type = meta_.type;
        // channels / single-video metas use their only video id
        ctx.videoId = meta_.videos.size() == 1 ? meta_.videos[0].id : meta_.id;
        App::get().push(makeStreamsScreen(ctx));
    }

    void playVideo(const stremio::Video& v) {
        PlayContext ctx;
        ctx.meta = meta_;
        ctx.type = meta_.type;
        ctx.videoId = v.id;
        ctx.season = v.season;
        ctx.episode = v.episode;
        ctx.epTitle = v.title;
        App::get().push(makeStreamsScreen(ctx));
    }

    void toggleLibrary() {
        store::toggleLibrary(meta_);
        App::get().toast(store::inLibrary(meta_.id) ? "Added to Library" : "Removed from Library");
    }

    void toggleWatched(const stremio::Video& v) {
        const store::Progress* p = store::progress(v.id);
        store::Progress np;
        np.videoId = v.id;
        np.metaId = meta_.id;
        np.type = meta_.type;
        np.name = meta_.name;
        np.poster = meta_.poster;
        np.background = meta_.background;
        np.logo = meta_.logo;
        np.epTitle = v.title;
        np.season = v.season;
        np.episode = v.episode;
        bool nowWatched = !(p && p->watched());
        if (!nowWatched) {
            np.pos = 0;
            np.dur = 0;
            App::get().toast("Marked as unwatched");
        } else {
            np.pos = 1;
            np.dur = 1;
            App::get().toast("Marked as watched");
        }
        store::setProgress(np);
        App::get().traktSetWatched(meta_.id, meta_.type, v.season, v.episode, nowWatched);
    }

    void renderEpisodes(float y) {
        const Theme& t = theme();
        float x = 48;
        for (size_t i = 0; i < seasons_.size(); i++) {
            int s = seasons_[i];
            std::string label = s == 0 ? "Specials" : "Season " + std::to_string(s);
            bool sel = (int)i == seasonIdx_;
            float cw0 = ui::chip(label, x, y, zone_ == 1 && sel, sel);
            int si = (int)i;
            ui::hit(x, y - 6, cw0, 46, [this, si] {
                zone_ = 1;
                if (si != seasonIdx_) {
                    seasonIdx_ = si;
                    fillEpisodes();
                    epScroll_ = 0;
                }
            });
            x += cw0 + 10;
            if (x > W - 60) break;
        }
        float cy = y + 52, cw = 236, ch = 133;
        pushClip(0, cy - 12, W, 720 - cy + 12);
        for (size_t i = 0; i < eps_.size(); i++) {
            float cx = 48 + i * (cw + 14) - epScroll_;
            if (cx > W) break;
            if (cx + cw < 0) continue;
            const stremio::Video& v = meta_.videos[eps_[i]];
            bool f = zone_ == 2 && (int)i == ep_;
            int ei = (int)i;
            ui::tapToActivate(cx, cy, cw, ch + 44, [this, ei] {
                zone_ = 2;
                ep_ = ei;
            });
            SDL_Texture* th = image(v.thumbnail.empty() ? meta_.background : v.thumbnail, 480, 270);
            if (th) {
                // Flat blit + solid corner cut-outs, as on the home cards: textured
                // rounded geometry shimmers while scrolling on the Switch.
                drawCover(th, cx, cy, cw, ch);
                if (!debugFlat()) cornerCutouts(cx, cy, cw, ch, 10, t.bg);
            } else {
                roundRect(cx, cy, cw, ch, 10, t.card);
            }
            const store::Progress* p = store::progress(v.id);
            if (p && p->watched()) {
                roundRect(cx + cw - 82, cy + 8, 74, 24, 12, withA(t.bg, 210));
                text("Watched", cx + cw - 45, cy + 11, 13, t.text, true, Center);
            } else if (p && p->dur > 0 && p->pos > 0) {
                fillRect(cx + 10, cy + ch - 12, cw - 20, 4, withA(t.text, 70));
                fillRect(cx + 10, cy + ch - 12, (float)((cw - 20) * std::min(1.0, p->pos / p->dur)), 4, t.accent);
            }
            if (f) ui::focusRing(cx, cy, cw, ch, 10);
            std::string title = "E" + std::to_string(v.episode) + "  " + v.title;
            text(title, cx + 2, cy + ch + 8, 15, f ? t.text : t.text2, true, Left, cw);
            std::string sub = v.released.size() >= 10 ? v.released.substr(0, 10) : v.released;
            text(sub, cx + 2, cy + ch + 28, 13, t.text3, false, Left, cw);
        }
        popClip();
        if (zone_ == 2 && ep_ < (int)eps_.size()) {
            // overview of the focused episode, top-right
            const stremio::Video& v = meta_.videos[eps_[ep_]];
            if (!v.overview.empty()) {
                roundRect(760, 64, 472, 150, 14, withA(t.bg, 200));
                text(episodeLabel(v.season, v.episode) + "  " + v.title, 778, 78, 16, t.text, true, Left, 436);
                paragraph(v.overview, 778, 104, 436, 14, t.text2, 5);
            }
        }
    }

    stremio::Meta meta_;
    bool loading_ = true;
    int zone_ = 0, btn_ = 0;
    std::vector<int> seasons_;
    int seasonIdx_ = 0;
    std::vector<int> eps_;
    int ep_ = 0;
    float epScroll_ = 0;
    float dt_ = 1.f / 60.f;
    std::string playLabel_;
    int playLabelRev_ = -1;
    ui::Backdrop bd_;
};

}  // namespace

std::shared_ptr<Screen> makeDetailScreen(const stremio::MetaPreview& m) { return std::make_shared<DetailScreen>(m); }
