// Home: hero for the focused title + Continue Watching + addon catalog rows.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>

#include "input.hpp"
#include "screens.hpp"
#include "store.hpp"

using namespace gfx;

namespace {

std::string rowTitle(const stremio::CatalogDesc& c) {
    std::string type = stremio::typeLabel(c.type);
    std::string n = c.name;
    // "Popular" -> "Popular Movies"; keep names that already mention the type
    std::string lowerName, lowerType;
    for (char ch : n) lowerName += (char)tolower((unsigned char)ch);
    for (char ch : type) lowerType += (char)tolower((unsigned char)ch);
    if (type.empty() || lowerName.find(lowerType) != std::string::npos) return n;
    return n + " " + type;
}

class HomePage : public Page {
public:
    void onShow() override {
        if (builtRev_ != App::get().addonsRevision()) rebuild();
        else refreshContinue();
    }
    void onResume() override { refreshContinue(); }

    void update(float dt) override {
        if (builtRev_ != App::get().addonsRevision()) rebuild();
        if (progRev_ != store::progressRevision()) {
            progRev_ = store::progressRevision();
            refreshContinue();
        }
        if (now() - lastEnrich_ > 2.0) {
            lastEnrich_ = now();
            App::get().enrichProgress();
        }

        bool hitTop = false;
        rows_.handleNav(&hitTop);
        if (hitTop) wantsTabBar = true;
        rows_.update(dt);

        auto* f = rows_.focused();
        auto* fr = rows_.focusedRow();
        if (f) backdrop_.set(f->background.empty() ? f->poster : f->background);

        // Everything opens the detail page first (description + season/episode picker),
        // like Nuvio. For Continue Watching the detail page's Play button becomes
        // "Resume" on the right episode.
        if (f && fr && input::pressed(input::A)) {
            App::get().push(makeDetailScreen(*f));
        }
        if (f && fr && input::pressed(input::Y) && !fr->landscape) {
            store::toggleLibrary(*f);
            App::get().toast(store::inLibrary(f->id) ? "Added to Library" : "Removed from Library");
        }
        if (f && fr && fr->landscape && input::pressed(input::X)) {
            store::removeFromContinue(f->id);
            App::get().toast("Removed from Continue Watching");
            refreshContinue();
        }
    }

    void render() override {
        const Theme& t = theme();
        float dt = 1.f / 60.f;

        auto* f = rows_.focused();
        auto* fr = rows_.focusedRow();
        // The hero backdrop + logo belong to the focused entry; hold them back until that
        // entry is fully revealed (its card popped in), so the top doesn't show artwork
        // for a card that hasn't appeared yet. Once ready, the backdrop fades itself in.
        bool firstReady = f && rows_.focusedAppear() >= 1.f;
        if (firstReady) {
            backdrop_.render(dt);
            renderHero(*f, fr && fr->landscape);
        }
        updateLoading(dt, firstReady);

        if (!rows_.hasVisible()) {
            // While catalogs load, the animated "Loading..." (renderLoading) stands in.
            renderLoading();
            if (!(App::get().manifestsLoading() > 0 || rows_.anyLoading())) {
                text("Nothing to show yet", W / 2, 330, 26, t.text, true, Center);
                text("Install an addon with catalogs in the Addons tab (press R).", W / 2, 372, 18, t.text2,
                     false, Center);
            }
            return;
        }
        // Hide the top row's "Continue Watching" header while loading; the wave sits in its
        // place and the real title appears once the first entry is ready.
        rows_.suppressFocusedTitle = loadAlpha_ > 0.05f;
        rows_.render(372, 650);
        renderLoading();  // over the (now title-less) header slot
    }

    std::vector<ui::Hint> hints() override {
        auto* fr = rows_.focusedRow();
        if (fr && fr->landscape) return {{"A", "Details"}, {"X", "Remove"}};
        return {{"A", "Details"}, {"Y", "Library"}};
    }

private:
    void renderHero(const stremio::MetaPreview& m, bool isContinue) {
        const Theme& t = theme();
        // Fixed layout: the title slot is always 92..202, the meta line always at 214,
        // so nothing jumps when a logo finishes loading.
        float x = 48, y = 92;
        const float slotH = 110;
        SDL_Texture* logo = m.logo.empty() ? nullptr : image(m.logo, 600, 200);
        float lf = logo ? imageFade(m.logo, 0.2f) : 0.f;
        if (lf < 1.f) {
            // text title, bottom-aligned in the slot, fading out as the logo fades in
            auto lines = wrap(m.name, 40, 640, 2, true);
            float ty = y + slotH - lines.size() * 50;
            for (auto& l : lines) {
                text(l, x, ty, 40, withA(t.text, (Uint8)(255 * (1.f - lf))), true);
                ty += 50;
            }
        }
        if (logo) drawFit(logo, x, y, 420, slotH, (Uint8)(255 * lf));
        y = 214;

        std::string line;
        auto add = [&](const std::string& s) {
            if (s.empty()) return;
            if (!line.empty()) line += "   ";
            line += s;
        };
        if (isContinue) {
            add(m.subtitle);
            const store::Progress* p = store::progress(m.videoId);
            if (p && p->dur > 0)
                add(p->pct ? std::to_string((int)p->pos) + "% watched" : ui::fmtTime(p->dur - p->pos) + " left");
        } else {
            add(stremio::typeLabel(m.type, false));
            add(m.releaseInfo);
            if (!m.imdbRating.empty()) add("IMDb " + m.imdbRating);
            std::string g;
            for (size_t i = 0; i < m.genres.size() && i < 3; i++) g += (i ? ", " : "") + m.genres[i];
            add(g);
        }
        text(line, x, y, 17, t.text2, true, Left, 700);
        y += 32;
        if (!m.description.empty()) paragraph(m.description, x, y, 560, 16, t.text2, 3);
    }

    void refreshContinue() {
        if (rows_.rows.empty()) return;
        auto& r = rows_.rows[0];
        r.items.clear();
        for (auto& p : store::continueWatching(20)) {
            if (p.name.empty()) continue;  // imported from the account; artwork still loading
            stremio::MetaPreview m;
            m.id = p.metaId;
            m.type = p.type;
            m.name = p.name;
            m.poster = p.poster;
            m.background = p.background;
            m.logo = p.logo;
            m.videoId = p.videoId;
            if (p.season > 0 || p.episode > 0) {
                m.subtitle = episodeLabel(p.season, p.episode);
                if (!p.epTitle.empty()) m.subtitle += " - " + p.epTitle;
            } else {
                m.subtitle = "Movie";
            }
            m.progress = p.dur > 0 ? (float)(p.pos / p.dur) : 0.f;
            r.items.push_back(m);
        }
        r.loading = false;
        rows_.ensureValidFocus();
    }

    void rebuild() {
        builtRev_ = App::get().addonsRevision();
        gen_++;
        rows_.rows.clear();
        rows_.row = 0;

        ui::PosterRow cw;
        cw.title = "Continue Watching";
        cw.landscape = true;
        cw.loading = false;
        cw.deferUntilReady = true;  // each card pops in only once its artwork is ready
        rows_.rows.push_back(cw);
        refreshContinue();

        for (auto& a : App::get().enabledAddons()) {
            if (!a.loaded) continue;
            for (auto& c : a.catalogs) {
                if (c.needsUserInput()) continue;
                ui::PosterRow r;
                r.title = rowTitle(c);
                r.tag = a.name;
                r.addon = a;
                r.cat = c;
                r.canPage = c.supportsExtra("skip");
                r.loading = true;
                rows_.rows.push_back(r);
            }
        }
        for (size_t i = 1; i < rows_.rows.size(); i++) load((int)i, 0);
        rows_.onNeedMore = [this](int i) {
            auto& r = rows_.rows[i];
            r.paging = true;
            load(i, (int)r.items.size());
        };
    }

    void load(int i, int skip) {
        auto addon = rows_.rows[i].addon;
        auto cat = rows_.rows[i].cat;
        int gen = gen_;
        std::string extra = skip > 0 ? "skip=" + std::to_string(skip) : "";
        async(
            [addon, cat, extra]() {
                bool ok = false;
                auto items = stremio::fetchCatalog(addon, cat, extra, &ok);
                return std::make_pair(ok, items);
            },
            [this, i, gen, skip](std::pair<bool, std::vector<stremio::MetaPreview>>& res) {
                if (gen != gen_ || i >= (int)rows_.rows.size()) return;
                auto& r = rows_.rows[i];
                r.loading = false;
                r.paging = false;
                std::set<std::string> have;
                for (auto& m : r.items) have.insert(m.id);
                size_t added = 0;
                for (auto& m : res.second) {
                    if (have.count(m.id)) continue;
                    have.insert(m.id);
                    r.items.push_back(m);
                    added++;
                }
                if (!res.first || added == 0) r.exhausted = true;
                (void)skip;
                rows_.ensureValidFocus();
            });
    }

    // Animated "Loading..." shown (in the accent colour, letters bobbing in a wave) while
    // the top is coming in and the first entry/hero isn't ready yet. Fades in after the top
    // bar, fades out once the first entry is displayed, then never returns this session.
    void updateLoading(float dt, bool firstReady) {
        if (firstShow_ <= 0) firstShow_ = now();
        if (firstReady) doneLoading_ = true;
        bool want = !doneLoading_ && now() - firstShow_ > 0.35;  // let the top bar settle first
        loadAlpha_ = ui::approach(loadAlpha_, want ? 1.f : 0.f, dt, 7.f);
    }
    // Drawn in the Continue Watching header's place while the first entry loads: "Loading..."
    // in the accent colour, each letter bobbing up and down to form a travelling wave.
    void renderLoading() {
        if (loadAlpha_ <= 0.01f) return;
        const Theme& t = theme();
        const std::string s = "Loading...";
        const int size = 20;        // matches the row title
        float x = 48, baseY = 372;  // exactly where the "Continue Watching" title sits
        double tt = now();
        Col c = withA(t.accent, (Uint8)(255 * loadAlpha_));
        for (size_t i = 0; i < s.size(); i++) {
            float dy = std::sin((float)(tt * 6.0 - (double)i * 0.55)) * 6.f;
            text(std::string(1, s[i]), x, baseY + dy, size, c, true, Left);
            x += textWidth(std::string(1, s[i]), size, true);
        }
    }

    ui::RowsView rows_;
    ui::Backdrop backdrop_;
    int builtRev_ = -1;
    int gen_ = 0;
    int progRev_ = -1;
    double lastEnrich_ = -10;
    double firstShow_ = 0;
    float loadAlpha_ = 0;
    bool doneLoading_ = false;
};

}  // namespace

std::shared_ptr<Page> makeHomePage() { return std::make_shared<HomePage>(); }

std::string episodeLabel(int season, int episode) {
    if (season <= 0 && episode <= 0) return "";
    if (season <= 0) return "Special " + std::to_string(episode);
    return "S" + std::to_string(season) + " E" + std::to_string(episode);
}
