// Search across every catalog that supports the "search" extra.
#include <algorithm>
#include <set>

#include "input.hpp"
#include "screens.hpp"
#include "store.hpp"

using namespace gfx;

namespace {

class SearchPage : public Page {
public:
    SearchPage() { rows_.showTitles = true; }

    void update(float dt) override {
        if (input::pressed(input::Y)) {
            openKeyboard();
            return;
        }
        if (onBox_) {
            if (input::pressed(input::A)) openKeyboard();
            if (input::pressed(input::Up)) wantsTabBar = true;
            if (input::pressed(input::Down) && rows_.hasVisible()) onBox_ = false;
        } else {
            bool hitTop = false;
            rows_.handleNav(&hitTop);
            if (hitTop) onBox_ = true;
            auto* f = rows_.focused();
            if (f && input::pressed(input::A)) App::get().push(makeDetailScreen(*f));
        }
        rows_.active = !onBox_;
        rows_.update(dt);
    }

    void render() override {
        const Theme& t = theme();
        float bx = 48, by = 86, bw = W - 96, bh = 54;
        roundRect(bx, by, bw, bh, 27, onBox_ ? t.surface : t.elevated);
        ui::tapToActivate(bx, by, bw, bh, [this] { onBox_ = true; });
        if (onBox_) ui::focusRing(bx, by, bw, bh, 27);
        // magnifier glyph
        roundOutline(bx + 22, by + 15, 20, 20, 10, 2.5f, t.text2);
        fillRect(bx + 39, by + 34, 8, 3, t.text2);
        if (query_.empty())
            text("Search movies & series", bx + 62, by + 15, 19, t.text3);
        else
            text(query_, bx + 62, by + 15, 19, t.text, true, Left, bw - 90);

        if (query_.empty()) {
            text("Press A to type with the on-screen keyboard", W / 2, 330, 20, t.text2, false, Center);
            return;
        }
        if (!rows_.hasVisible()) {
            if (rows_.anyLoading()) spinner(W / 2, 360, 22, t.text2);
            else if (rows_.rows.empty())
                text("No installed addon supports search", W / 2, 340, 20, t.text2, false, Center);
            else
                text("No results for \"" + query_ + "\"", W / 2, 340, 20, t.text2, false, Center);
            return;
        }
        rows_.render(166, 650);
    }

    std::vector<ui::Hint> hints() override {
        if (onBox_) return {{"A", "Type"}};
        return {{"A", "Details"}, {"Y", "New search"}};
    }

private:
    void openKeyboard() {
        std::string q;
        if (!ui::keyboard("Search movies & series", query_, q)) return;
        query_ = q;
        runSearch();
    }

    void runSearch() {
        gen_++;
        rows_.rows.clear();
        rows_.row = 0;
        std::string extra = "search=" + net::urlEncode(query_);
        for (auto& a : App::get().enabledAddons()) {
            if (!a.loaded) continue;
            for (auto& c : a.catalogs) {
                if (!c.supportsExtra("search")) continue;
                // skip catalogs that need some other required extra (e.g. genre)
                bool otherRequired = false;
                for (auto& e : c.extras)
                    if (e.required && e.name != "search") otherRequired = true;
                if (otherRequired) continue;
                ui::PosterRow r;
                r.title = stremio::typeLabel(c.type);
                r.tag = a.name + (c.name.empty() ? "" : " - " + c.name);
                r.addon = a;
                r.cat = c;
                r.loading = true;
                rows_.rows.push_back(r);
            }
        }
        int gen = gen_;
        for (size_t i = 0; i < rows_.rows.size(); i++) {
            auto addon = rows_.rows[i].addon;
            auto cat = rows_.rows[i].cat;
            async([addon, cat, extra]() { return stremio::fetchCatalog(addon, cat, extra, nullptr); },
                  [this, i, gen](std::vector<stremio::MetaPreview>& items) {
                      if (gen != gen_ || i >= rows_.rows.size()) return;
                      rows_.rows[i].loading = false;
                      rows_.rows[i].items = items;
                      rows_.ensureValidFocus();
                      if (onBox_ && rows_.hasVisible() && !typedOnce_) {
                          onBox_ = false;
                          typedOnce_ = true;
                      }
                  });
        }
        typedOnce_ = false;
        onBox_ = rows_.rows.empty();
    }

    ui::RowsView rows_;
    std::string query_;
    bool onBox_ = true;
    bool typedOnce_ = false;
    int gen_ = 0;
};

class LibraryPage : public Page {
public:
    LibraryPage() { rows_.showTitles = true; }
    void onShow() override { refresh(); }
    void onResume() override { refresh(); }

    void update(float dt) override {
        bool hitTop = false;
        rows_.handleNav(&hitTop);
        if (hitTop) wantsTabBar = true;
        auto* f = rows_.focused();
        if (f && input::pressed(input::A)) App::get().push(makeDetailScreen(*f));
        if (f && input::pressed(input::Y)) {
            store::toggleLibrary(*f);
            App::get().toast("Removed from Library");
            refresh();
        }
        rows_.update(dt);
    }

    void render() override {
        const Theme& t = theme();
        if (!rows_.hasVisible()) {
            text("Your library is empty", W / 2, 320, 26, t.text, true, Center);
            text("Press Y on any title to save it here.", W / 2, 362, 18, t.text2, false, Center);
            return;
        }
        rows_.render(96, 650);
    }

    std::vector<ui::Hint> hints() override { return {{"A", "Details"}, {"Y", "Remove"}}; }

private:
    void refresh() {
        auto lib = store::library();
        std::vector<std::string> order;
        for (auto& m : lib) {
            std::string ty = m.type.empty() ? "other" : m.type;
            if (std::find(order.begin(), order.end(), ty) == order.end()) order.push_back(ty);
        }
        // movies first, then series, then anything else
        std::stable_sort(order.begin(), order.end(), [](const std::string& a, const std::string& b) {
            auto rank = [](const std::string& s) { return s == "movie" ? 0 : s == "series" ? 1 : 2; };
            return rank(a) < rank(b);
        });
        int keepRow = rows_.row;
        std::vector<int> cols;
        for (auto& r : rows_.rows) cols.push_back(r.col);
        rows_.rows.clear();
        for (size_t k = 0; k < order.size(); k++) {
            ui::PosterRow r;
            r.title = stremio::typeLabel(order[k]);
            r.loading = false;
            for (auto& m : lib)
                if ((m.type.empty() ? "other" : m.type) == order[k]) r.items.push_back(m);
            // same pop-in as the home rows; plays once (reveal times persist per item id)
            r.tag = std::to_string(r.items.size()) + " titles";
            if (k < cols.size()) r.col = cols[k];
            rows_.rows.push_back(r);
        }
        rows_.row = keepRow;
        rows_.ensureValidFocus();
    }

    ui::RowsView rows_;
};

}  // namespace

std::shared_ptr<Page> makeSearchPage() { return std::make_shared<SearchPage>(); }
std::shared_ptr<Page> makeLibraryPage() { return std::make_shared<LibraryPage>(); }
