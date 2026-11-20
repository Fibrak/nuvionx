// Top-level screen: tab bar (Home / Search / Library / Addons / Settings).
#include <ctime>

#include "input.hpp"
#include "screens.hpp"

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
        float x = 48;
        text("NUVIO", x, 22, 24, t.text, true);
        text("NX", x + textWidth("NUVIO", 24, true) + 6, 22, 24, t.accent, true);

        float tx = 230;
        for (size_t i = 0; i < names_.size(); i++) {
            bool sel = (int)i == tab_;
            float w = textWidth(names_[i], 18, true) + 30;
            if (onTabs_ && sel) {
                roundRect(tx, 18, w, 36, 18, t.text);
                text(names_[i], tx + w / 2, 25, 18, t.bg, true, Center);
            } else {
                text(names_[i], tx + w / 2, 25, 18, sel ? t.text : t.text3, true, Center);
                if (sel) roundRect(tx + w / 2 - 10, 50, 20, 3, 1.5f, t.accent);
            }
            int ti = (int)i;
            ui::hit(tx, 8, w, 54, [this, ti] { switchTab(ti); });
            tx += w + 6;
        }

        time_t now = time(nullptr);
        struct tm* lt = localtime(&now);
        if (lt) {
            char buf[16];
            strftime(buf, sizeof buf, "%H:%M", lt);
            text(buf, W - 48, 25, 18, t.text2, true, Right);
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
            roundOutline(bx, by, bw, bh, 3, 1.5f, t.text2);
            fillRect(bx + bw, by + 4, 2.5f, bh - 8, t.text2);  // terminal nub
            float fillW = (bw - 4) * std::min(1.f, std::max(0.f, batt / 100.f));
            roundRect(bx + 2, by + 2, fillW, bh - 4, 1.5f, level);
            if (charging) text("+", bx + bw / 2, yc - lineHeight(15) / 2, 15, t.bg, true, Center);
            text(pct, right, ty, 18, t.text2, true, Right);
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
    bool onTabs_ = false;
};

}  // namespace

std::shared_ptr<Screen> makeMainScreen() { return std::make_shared<MainScreen>(); }
