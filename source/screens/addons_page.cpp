// Addon management: install (keyboard or phone over LAN), enable/disable,
// reorder, remove.
#include <algorithm>

#include "input.hpp"
#include "lan.hpp"
#include "screens.hpp"
#include "store.hpp"

using namespace gfx;

namespace {

class AddonsPage : public Page {
public:
    void update(float dt) override {
        auto& addons = App::get().addons();
        int n = (int)addons.size();
        if (input::pressed(input::Up)) {
            if (sel_ == -1) wantsTabBar = true;
            else sel_--;
        }
        if (input::pressed(input::Down) && sel_ + 1 < n) sel_++;
        sel_ = std::max(-1, std::min(sel_, n - 1));

        if (sel_ == -1) {
            if (input::pressed(input::A)) install();
        } else {
            if (input::pressed(input::A)) App::get().toggleAddon(sel_);
            if (input::pressed(input::ZL) && sel_ > 0) {
                App::get().moveAddon(sel_, -1);
                sel_--;
            }
            if (input::pressed(input::ZR) && sel_ + 1 < n) {
                App::get().moveAddon(sel_, +1);
                sel_++;
            }
            if (input::pressed(input::X)) {
                if (confirmRemove_ == sel_ && now() < confirmUntil_) {
                    std::string name = addons[sel_].name;
                    App::get().removeAddon(sel_);
                    App::get().toast("Removed " + name);
                    confirmRemove_ = -1;
                    sel_ = std::min(sel_, (int)App::get().addons().size() - 1);
                } else {
                    confirmRemove_ = sel_;
                    confirmUntil_ = now() + 3;
                }
            }
        }
        if (input::pressed(input::Y)) install();
        float target = std::max(0.f, (sel_ + 1) * kItemH - kItemH * 2);
        scroll_ = ui::approach(scroll_, target, dt, 16);
    }

    void render() override {
        const Theme& t = theme();
        auto& addons = App::get().addons();
        float x = 48, y = 92;
        ui::button("+ Install addon", x, y, 48, sel_ == -1, true);
        ui::tapToActivate(x, y, ui::buttonWidth("+ Install addon"), 48, [this] { sel_ = -1; });
        std::string lan = App::get().lanUrl();
        if (!lan.empty()) {
            text("or open  " + lan + "  in a browser on the same Wi-Fi", x + ui::buttonWidth("+ Install addon") + 24,
                 y + 14, 16, t.text2);
        }

        float top = 160, bottom = 650;
        pushClip(0, top - 8, W, bottom - top + 8);
        for (size_t i = 0; i < addons.size(); i++) {
            float iy = top + i * kItemH - scroll_;
            if (iy + kItemH < top - 10) continue;
            if (iy > bottom) break;
            renderItem(addons[i], (int)i, x, iy, W - 96, kItemH - 12);
            // first tap selects, tapping the selected addon toggles it (like A)
            int ai = (int)i;
            float hy = std::max(iy, top), hb = std::min(iy + kItemH - 12, bottom);
            if (hb > hy)
                ui::hit(x, hy, W - 96, hb - hy, [this, ai] {
                    if (sel_ == ai) input::inject(input::A);
                    else sel_ = ai;
                });
        }
        popClip();
        if (App::get().manifestsLoading() > 0) spinner(W - 64, 116, 10, t.text2);
    }

    std::vector<ui::Hint> hints() override {
        if (sel_ == -1) return {{"A", "Install"}};
        bool en = sel_ < (int)App::get().addons().size() && App::get().addons()[sel_].enabled;
        return {{"A", en ? "Disable" : "Enable"},
                {"X", confirmRemove_ == sel_ && now() < confirmUntil_ ? "Press again to remove" : "Remove"},
                {"ZL/ZR", "Reorder"},
                {"Y", "Install"}};
    }

private:
    static constexpr float kItemH = 104;

    void install() {
        std::string url;
        if (!ui::keyboard("Addon manifest URL", "https://", url, true)) return;
        if (url == "https://" || url.size() < 10) return;
        App::get().installAddon(url);
    }

    void renderItem(const stremio::Addon& a, int idx, float x, float y, float w, float h) {
        const Theme& t = theme();
        bool f = idx == sel_;
        roundRect(x, y, w, h, 14, f ? withA(t.surface, 250) : withA(t.elevated, 220));
        if (f) ui::focusRing(x, y, w, h, 14);
        // logo
        SDL_Texture* logo = image(a.logo, 160, 160);
        if (logo) drawCover(logo, x + 16, y + 16, h - 32, h - 32, 12);
        else {
            roundRect(x + 16, y + 16, h - 32, h - 32, 12, t.card);
            std::string ini = a.name.empty() ? "?" : a.name.substr(0, 1);
            text(ini, x + 16 + (h - 32) / 2, y + h / 2 - 14, 24, t.text2, true, Center);
        }
        float tx = x + h + 4;
        Uint8 dim = a.enabled ? 255 : 120;
        std::string title = a.name.empty() ? a.manifestUrl : a.name;
        float tw = text(title, tx, y + 14, 19, withA(t.text, dim), true, Left, w - h - 260);
        if (!a.version.empty()) text("v" + a.version, tx + tw + 10, y + 18, 14, t.text3);
        std::string desc = !a.error.empty() && !a.loaded ? "Couldn't load: " + a.error : a.description;
        paragraph(desc, tx, y + 42, w - h - 260, 14, withA(t.text2, dim), 2, 1.3f);

        // capability badges
        float bx = x + w - 20;
        auto badge = [&](const std::string& s, Col c) {
            float bw = textWidth(s, 12, true) + 18;
            bx -= bw;
            roundRect(bx, y + 16, bw, 22, 11, c);
            text(s, bx + bw / 2, y + 19, 12, t.text, true, Center);
            bx -= 6;
        };
        badge(a.enabled ? "ON" : "OFF", a.enabled ? t.accentDim : withA(t.text3, 120));
        if (a.hasResource("subtitles")) badge("Subtitles", withA(t.text3, 90));
        if (a.hasResource("stream")) badge("Streams", withA(t.text3, 90));
        if (!a.catalogs.empty()) badge("Catalogs", withA(t.text3, 90));
        text("#" + std::to_string(idx + 1), x + w - 20, y + h - 30, 13, t.text3, false, Right);
    }

    int sel_ = -1;
    float scroll_ = 0;
    int confirmRemove_ = -1;
    double confirmUntil_ = 0;
};

}  // namespace

std::shared_ptr<Page> makeAddonsPage() { return std::make_shared<AddonsPage>(); }
