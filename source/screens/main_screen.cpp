// Top-level screen: tab bar (Home / Search / Library / Addons / Settings).
#include <ctime>

#include "input.hpp"
#include "screens.hpp"
#include "store.hpp"

using namespace gfx;

namespace {

class MainScreen : public Screen {
public:
    MainScreen() {
        pages_ = {makeHomePage(),   makeSearchPage(),  makeLibraryPage(),
                  makeAddonsPage(), makeAccountPage(), makeSettingsPage()};
        names_ = {"Home", "Search", "Library", "Addons", "Account", "Settings"};
        pages_[0]->onShow();
    }

    void update(float dt) override {
        // Debug: ZL+ZR cycles  off -> overlay -> overlay+flat cards -> off.
        if ((input::held(input::ZL) && input::pressed(input::ZR)) ||
            (input::held(input::ZR) && input::pressed(input::ZL))) {
            dbgMode_ = (dbgMode_ + 1) % 3;
            auto& s = store::settings();
            s.perfOverlay = dbgMode_ > 0;
            gfx::setDebugFlat(dbgMode_ == 2);
            store::saveSettings();
            const char* msg[] = {"Debug OFF", "Overlay ON", "Overlay + FLAT cards (no rounded corners)"};
            App::get().toast(msg[dbgMode_]);
        }
        if (input::pressed(input::Plus)) {
            App::get().requestQuit();
            return;
        }
        if (input::pressed(input::L)) switchTab(tab_ - 1);
        if (input::pressed(input::R)) switchTab(tab_ + 1);

        if (onTabs_) {
            if (input::pressed(input::Left)) switchTab(tab_ - 1, true);
            if (input::pressed(input::Right)) switchTab(tab_ + 1, true);
            if (input::pressed(input::Down) || input::pressed(input::A)) onTabs_ = false;
            if (input::pressed(input::B)) {
                if (tab_ != 0) switchTab(0, true);
            }
            // keep animations alive without delivering input to the page
            pages_[tab_]->wantsTabBar = false;
            return;
        }

        auto& p = pages_[tab_];
        p->update(dt);
        if (p->wantsTabBar) {
            p->wantsTabBar = false;
            onTabs_ = true;
        }
    }

    void onResume() override { pages_[tab_]->onResume(); }

    void render() override {
        const Theme& t = theme();
        pages_[tab_]->render();
        // tap targets registered by the page stay below the bar's own targets

        // top bar
        vGradient(0, 0, W, 90, withA(t.bg, 235), withA(t.bg, 0));
        // Intro: on first open the logo, then each tab, then the clock/battery ease in one
        // after another (fade + a small drop). Plays once; after ~1s every amount is 1.
        if (introStart_ <= 0) introStart_ = now();
        auto iAmt = [&](int order) -> float {
            float p = (float)((now() - introStart_ - order * 0.09) / 0.34);
            if (p <= 0.f) return 0.f;
            if (p >= 1.f) return 1.f;
            return 1.f - (1.f - p) * (1.f - p);  // ease-out
        };
        auto fade = [](Col c, float a) { return withA(c, (Uint8)(c.a * a)); };

        float x = 48;
        {
            float a0 = iAmt(0), dy = (1.f - a0) * -8.f;
            text("NUVIO", x, 22 + dy, 24, fade(t.text, a0), true);
            text("NX", x + textWidth("NUVIO", 24, true) + 6, 22 + dy, 24, fade(t.accent, a0), true);
        }

        float tx = 230;
        for (size_t i = 0; i < names_.size(); i++) {
            bool sel = (int)i == tab_;
            float w = textWidth(names_[i], 18, true) + 30;
            float ia = iAmt((int)i + 1), dy = (1.f - ia) * -8.f;
            if (onTabs_ && sel) {
                roundRect(tx, 18 + dy, w, 36, 18, fade(t.text, ia));
                text(names_[i], tx + w / 2, 25 + dy, 18, fade(t.bg, ia), true, Center);
            } else {
                text(names_[i], tx + w / 2, 25 + dy, 18, fade(sel ? t.text : t.text3, ia), true, Center);
                if (sel) roundRect(tx + w / 2 - 10, 50 + dy, 20, 3, 1.5f, fade(t.accent, ia));
            }
            int ti = (int)i;
            ui::hit(tx, 8, w, 54, [this, ti] { switchTab(ti); });
            tx += w + 6;
        }

        float iBar = iAmt((int)names_.size() + 1);  // clock + battery come in last
        time_t now = time(nullptr);
        struct tm* lt = localtime(&now);
        if (lt) {
            char buf[16];
            strftime(buf, sizeof buf, "%H:%M", lt);
            text(buf, W - 48, 25, 18, fade(t.text2, iBar), true, Right);
        }

        // battery (to the left of the clock)
        bool charging = false;
        int batt = App::get().batteryPercent(&charging);
        if (batt >= 0) {
            float clockW = lt ? textWidth("00:00", 18, true) : 0;
            float right = W - 48 - clockW - 20;  // gap before the clock
            std::string pct = std::to_string(batt) + "%";
            float pctW = textWidth(pct, 18, true);
            float ty = 25;  // percentage text top (same as the clock)
            // battery glyph, vertically centred on the percentage text
            float bw = 30, bh = 16;
            float yc = ty + lineHeight(18) / 2;  // text vertical centre
            float bx = right - pctW - 12 - bw, by = yc - bh / 2;
            Col level = charging ? t.accent : (batt <= 15 ? rgba(0xEF4444) : t.text2);
            roundOutline(bx, by, bw, bh, 3, 1.5f, fade(t.text2, iBar));
            fillRect(bx + bw, by + 4, 2.5f, bh - 8, fade(t.text2, iBar));  // terminal nub
            float fillW = (bw - 4) * std::min(1.f, std::max(0.f, batt / 100.f));
            roundRect(bx + 2, by + 2, fillW, bh - 4, 1.5f, fade(level, iBar));
            if (charging) text("+", bx + bw / 2, yc - lineHeight(15) / 2, 15, fade(t.bg, iBar), true, Center);
            text(pct, right, ty, 18, fade(t.text2, iBar), true, Right);
        }

        // bottom hint bar
        vGradient(0, 640, W, 80, withA(t.bg, 0), withA(t.bg, 230));
        std::vector<ui::Hint> h;
        if (onTabs_) {
            h = {{"A", "Open"}, {"LR", "Switch tab"}};
        } else {
            h = pages_[tab_]->hints();
            h.push_back({"LR", "Tabs"});
        }
        h.push_back({"+", "Exit"});
        ui::hints(h);
    }

private:
    void switchTab(int t, bool stayOnTabs = false) {
        int n = (int)pages_.size();
        t = (t % n + n) % n;
        if (t != tab_) {
            tab_ = t;
            pages_[tab_]->onShow();
        }
        onTabs_ = stayOnTabs;
    }

    std::vector<std::shared_ptr<Page>> pages_;
    std::vector<std::string> names_;
    int tab_ = 0;
    int dbgMode_ = 0;
    bool onTabs_ = false;
    double introStart_ = 0;  // top-bar intro animation start (set on first render)
};

}  // namespace

std::shared_ptr<Screen> makeMainScreen() { return std::make_shared<MainScreen>(); }
