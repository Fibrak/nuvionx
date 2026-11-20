// Settings: theme, languages, playback options, LAN server, history.
#include <algorithm>

#include "input.hpp"
#include "lan.hpp"
#include "screens.hpp"
#include "store.hpp"

using namespace gfx;

namespace {

struct LangOpt {
    const char* code;
    const char* name;
};
const LangOpt kSubLangs[] = {{"", "Off"},         {"eng", "English"},  {"por", "Portuguese"},
                             {"pob", "Portuguese (BR)"}, {"spa", "Spanish"}, {"fre", "French"},
                             {"ger", "German"},   {"ita", "Italian"},  {"dut", "Dutch"},
                             {"pol", "Polish"},   {"rus", "Russian"},  {"tur", "Turkish"},
                             {"ara", "Arabic"},   {"jpn", "Japanese"}, {"kor", "Korean"},
                             {"chi", "Chinese"}};
const LangOpt kAudioLangs[] = {{"", "File default"}, {"eng", "English"}, {"por", "Portuguese"},
                               {"spa", "Spanish"},   {"fre", "French"},  {"ger", "German"},
                               {"ita", "Italian"},   {"jpn", "Japanese"}};

template <size_t N>
int langIndex(const LangOpt (&opts)[N], const std::string& code) {
    for (size_t i = 0; i < N; i++)
        if (code == opts[i].code) return (int)i;
    return 0;
}

class SettingsPage : public Page {
public:
    void update(float dt) override {
        const int n = kCount;
        if (input::pressed(input::Up)) {
            if (sel_ == 0) wantsTabBar = true;
            else sel_--;
        }
        if (input::pressed(input::Down)) sel_ = std::min(n - 1, sel_ + 1);
        int d = 0;
        if (input::pressed(input::Right) || input::pressed(input::A)) d = 1;
        if (input::pressed(input::Left)) d = -1;
        if (d) change(d, input::pressed(input::A));
        if (sel_ >= 10 && sel_ <= 12 && input::pressed(input::X)) {
            auto& s = store::settings();
            (sel_ == 10 ? s.realDebridKey : sel_ == 11 ? s.torboxKey : s.premiumizeKey).clear();
            store::saveSettings();
            App::get().toast("Key removed");
        }
        float target = std::max(0.f, sel_ * kRowH - kRowH * 5);
        scroll_ = ui::approach(scroll_, target, dt, 16);
    }

    void render() override {
        const Theme& t = theme();
        float x = 48, w = W - 96, top = 92;
        pushClip(0, top - 8, W, 650 - top);
        for (int i = 0; i < kCount; i++) {
            float y = top + i * kRowH - scroll_;
            if (y + kRowH < top - 10 || y > 650) continue;
            bool f = i == sel_;
            roundRect(x, y, w, kRowH - 10, 12, f ? withA(t.surface, 250) : withA(t.elevated, 200));
            if (f) ui::focusRing(x, y, w, kRowH - 10, 12);
            {
                // tap the right half of a setting = next value, left half = previous
                int ri = i;
                float hy = std::max(y, top), hb = std::min(y + kRowH - 10, 650.f);
                if (hb > hy) {
                    ui::hit(x, hy, w / 2, hb - hy, [this, ri] {
                        sel_ = ri;
                        input::inject(ri <= 9 ? input::Left : input::A);
                    });
                    ui::hit(x + w / 2, hy, w / 2, hb - hy, [this, ri] {
                        sel_ = ri;
                        input::inject(ri <= 9 ? input::Right : input::A);
                    });
                }
            }
            text(label(i), x + 22, y + 15, 18, t.text, true);
            std::string v = value(i);
            if (!v.empty()) {
                bool cyc = i <= 9;
                text(cyc ? "<  " + v + "  >" : v, x + w - 22, y + 15, 17, f ? t.text : t.text2, f, Right);
            }
        }
        popClip();
    }

    std::vector<ui::Hint> hints() override {
        if (sel_ >= 10 && sel_ <= 12) return {{"A", "Edit"}, {"X", "Remove"}};
        if (sel_ == 13) return {{"A", confirm_ && now() < confirmUntil_ ? "Press again to clear" : "Clear"}};
        if (sel_ >= 14) return {};
        return {{"<>", "Change"}};
    }

private:
    static const int kCount = 15;

    static std::string masked(const std::string& k) {
        if (k.empty()) return "Not set";
        return "Set (..." + k.substr(k.size() > 4 ? k.size() - 4 : 0) + ")";
    }
    static constexpr float kRowH = 58;

    std::string label(int i) {
        switch (i) {
            case 0: return "Theme";
            case 1: return "Subtitle language";
            case 2: return "Audio language";
            case 3: return "Autoplay next episode";
            case 4: return "Hardware decoding";
            case 5: return "Video render resolution";
            case 6: return "Show unsupported streams";
            case 7: return "Phone addon install (LAN)";
            case 8: return "Cache artwork on SD card";
            case 9: return "CPU boost during playback";
            case 10: return "Real-Debrid API key";
            case 11: return "TorBox API key";
            case 12: return "Premiumize API key";
            case 13: return "Clear watch history";
            default: return "NuvioNX 0.1.0  -  unofficial Switch port of NuvioTV (GPL-3.0)";
        }
    }

    std::string value(int i) {
        auto& s = store::settings();
        auto onoff = [](bool b) { return std::string(b ? "On" : "Off"); };
        switch (i) {
            case 0: return themeAt(s.theme).name;
            case 1: return kSubLangs[langIndex(kSubLangs, s.subLang)].name;
            case 2: return kAudioLangs[langIndex(kAudioLangs, s.audioLang)].name;
            case 3: return onoff(s.autoplayNext);
            case 4: return onoff(s.hwdec);
            case 5: return s.render1080 ? "1080p (sharper, more CPU)" : "720p (smoother)";
            case 6: return onoff(s.showUnsupported);
            case 7: {
                if (!s.lanServer) return "Off";
                std::string u = App::get().lanUrl();
                return u.empty() ? "On" : u;
            }
            case 8: return s.diskImageCache ? "On (faster, writes to SD)" : "Off (RAM only)";
            case 9: return s.cpuBoost ? "On (1785 MHz, smoother)" : "Off";
            case 10: return masked(s.realDebridKey);
            case 11: return masked(s.torboxKey);
            case 12: return masked(s.premiumizeKey);
            case 13: return std::to_string(store::continueWatching(1000).size()) + " in progress";
            default: return "";
        }
    }

    void change(int d, bool viaA) {
        auto& s = store::settings();
        const int nSub = (int)(sizeof(kSubLangs) / sizeof(kSubLangs[0]));
        const int nAud = (int)(sizeof(kAudioLangs) / sizeof(kAudioLangs[0]));
        switch (sel_) {
            case 0: s.theme = ((s.theme + d) % themeCount() + themeCount()) % themeCount(); break;
            case 1: s.subLang = kSubLangs[((langIndex(kSubLangs, s.subLang) + d) % nSub + nSub) % nSub].code; break;
            case 2:
                s.audioLang = kAudioLangs[((langIndex(kAudioLangs, s.audioLang) + d) % nAud + nAud) % nAud].code;
                break;
            case 3: s.autoplayNext = !s.autoplayNext; break;
            case 4: s.hwdec = !s.hwdec; break;
            case 5: s.render1080 = !s.render1080; break;
            case 6: s.showUnsupported = !s.showUnsupported; break;
            case 7:
                s.lanServer = !s.lanServer;
                if (s.lanServer) {
                    if (!lan::start(8080)) App::get().toast("Couldn't start the LAN server");
                } else {
                    lan::stop();
                }
                break;
            case 8: s.diskImageCache = !s.diskImageCache; break;
            case 9: s.cpuBoost = !s.cpuBoost; break;
            case 10:
            case 11:
            case 12: {
                if (!viaA) return;
                std::string* key = sel_ == 10 ? &s.realDebridKey : sel_ == 11 ? &s.torboxKey : &s.premiumizeKey;
                std::string v;
                if (!ui::keyboard(label(sel_), *key, v, true)) return;  // cancelled: keep the old key
                *key = v;
                break;
            }
            case 13:
                if (!viaA) return;
                if (confirm_ && now() < confirmUntil_) {
                    store::clearProgress();
                    App::get().toast("Watch history cleared");
                    confirm_ = false;
                } else {
                    confirm_ = true;
                    confirmUntil_ = now() + 3;
                }
                return;
            default: return;
        }
        store::saveSettings();
    }

    int sel_ = 0;
    float scroll_ = 0;
    bool confirm_ = false;
    double confirmUntil_ = 0;
};

}  // namespace

std::shared_ptr<Page> makeSettingsPage() { return std::make_shared<SettingsPage>(); }
