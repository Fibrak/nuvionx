// Settings: theme, languages, playback options, LAN server, history, self-update.
#include <algorithm>
#include <utility>

#include "input.hpp"
#include "lan.hpp"
#include "screens.hpp"
#include "store.hpp"
#include "update.hpp"

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
        if (sel_ >= 11 && sel_ <= 13 && input::pressed(input::X)) {
            auto& s = store::settings();
            (sel_ == 11 ? s.realDebridKey : sel_ == 12 ? s.torboxKey : s.premiumizeKey).clear();
            store::saveSettings();
            App::get().toast("Key removed");
        }
        animate(dt);
    }

    void animate(float dt) override {
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
                        input::inject(ri <= 10 ? input::Left : input::A);
                    });
                    ui::hit(x + w / 2, hy, w / 2, hb - hy, [this, ri] {
                        sel_ = ri;
                        input::inject(ri <= 10 ? input::Right : input::A);
                    });
                }
            }
            text(label(i), x + 22, y + 15, 18, t.text, true);
            std::string v = value(i);
            if (!v.empty()) {
                bool cyc = i <= 10;
                text(cyc ? "<  " + v + "  >" : v, x + w - 22, y + 15, 17, f ? t.text : t.text2, f, Right);
            }
        }
        popClip();
    }

    std::vector<ui::Hint> hints() override {
        if (sel_ >= 11 && sel_ <= 13) return {{"A", "Edit"}, {"X", "Remove"}};
        if (sel_ == 14) {
            switch (upd_) {
                case UpdState::Available: return {{"A", "Download & install"}};
                case UpdState::Done: return {{"A", "Restart"}};
                case UpdState::Checking:
                case UpdState::Downloading: return {};
                default: return {{"A", "Check for updates"}};
            }
        }
        if (sel_ == 15) return {{"A", confirm_ && now() < confirmUntil_ ? "Press again to clear" : "Clear"}};
        if (sel_ == 16) return {{"A", "Toggle"}};
        if (sel_ >= 17) return {};
        return {{"<>", "Change"}};
    }

private:
    static const int kCount = 18;

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
            case 7: return "Hide 4K / above-1080p streams";
            case 8: return "Phone addon install (LAN)";
            case 9: return "Cache artwork on SD card";
            case 10: return "CPU boost during playback";
            case 11: return "Real-Debrid API key";
            case 12: return "TorBox API key";
            case 13: return "Premiumize API key";
            case 14: return "Software update";
            case 15: return "Clear watch history";
            case 16: return "GPU video (experimental)";
            default:
                return std::string("NuvioNX ") + update::currentVersion() +
                       "  -  unofficial Switch port of NuvioTV (GPL-3.0)";
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
            case 7: return s.hide4k ? "On (1080p and below)" : "Off (show all)";
            case 8: {
                if (!s.lanServer) return "Off";
                std::string u = App::get().lanUrl();
                return u.empty() ? "On" : u;
            }
            case 9: return s.diskImageCache ? "On (faster, writes to SD)" : "Off (RAM only)";
            case 10: return s.cpuBoost ? "On (1785 MHz, smoother)" : "Off";
            case 11: return masked(s.realDebridKey);
            case 12: return masked(s.torboxKey);
            case 13: return masked(s.premiumizeKey);
            case 14: return updateValue();
            case 15: return std::to_string(store::continueWatching(1000).size()) + " in progress";
            case 16: return s.gpuVideo ? "On (GPU - smooth, less CPU)" : "Off (software/CPU)";
            default: return "";
        }
    }

    std::string updateValue() const {
        switch (upd_) {
            case UpdState::Checking: return "Checking...";
            case UpdState::UpToDate: return std::string("Up to date (v") + update::currentVersion() + ")";
            case UpdState::Available: return "v" + updVersion_ + " available";
            case UpdState::Downloading: return "Downloading... please wait";
            case UpdState::Done: return "Installed - press A to restart";
            case UpdState::Failed: return updMsg_.empty() ? "Check failed" : updMsg_;
            default: return std::string("v") + update::currentVersion();
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
            case 7: s.hide4k = !s.hide4k; break;
            case 8:
                s.lanServer = !s.lanServer;
                if (s.lanServer) {
                    if (!lan::start(8080)) App::get().toast("Couldn't start the LAN server");
                } else {
                    lan::stop();
                }
                break;
            case 9: s.diskImageCache = !s.diskImageCache; break;
            case 10: s.cpuBoost = !s.cpuBoost; break;
            case 11:
            case 12:
            case 13: {
                if (!viaA) return;
                std::string* key = sel_ == 11 ? &s.realDebridKey : sel_ == 12 ? &s.torboxKey : &s.premiumizeKey;
                std::string v;
                if (!ui::keyboard(label(sel_), *key, v, true)) return;  // cancelled: keep the old key
                *key = v;
                break;
            }
            case 14:
                if (!viaA) return;
                doUpdateAction();
                return;
            case 15:
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
            case 16:
                s.gpuVideo = !s.gpuVideo;
                App::get().toast(s.gpuVideo ? "GPU video on - applies to the next video"
                                            : "GPU video off - applies to the next video");
                break;
            default: return;
        }
        store::saveSettings();
    }

    // Drive the self-update state machine on each A press of the update row.
    void doUpdateAction() {
        switch (upd_) {
            case UpdState::Idle:
            case UpdState::UpToDate:
            case UpdState::Failed:
                upd_ = UpdState::Checking;
                updMsg_.clear();
                async([] { return update::check(); },
                      [this](update::Info& r) {
                          if (!r.ok) {
                              upd_ = UpdState::Failed;
                              updMsg_ = r.error;
                              App::get().toast(r.error);
                          } else if (r.available) {
                              upd_ = UpdState::Available;
                              updVersion_ = r.version;
                              updUrl_ = r.url;
                              App::get().toast("Update available: v" + r.version);
                          } else {
                              upd_ = UpdState::UpToDate;
                              App::get().toast("You're on the latest version");
                          }
                      });
                break;
            case UpdState::Available: {
                upd_ = UpdState::Downloading;
                std::string url = updUrl_;
                async(
                    [url] {
                        std::string err;
                        bool ok = update::download(url, err);
                        return std::make_pair(ok, err);
                    },
                    [this](std::pair<bool, std::string>& r) {
                        if (r.first) {
                            upd_ = UpdState::Done;
                            App::get().toast(update::canRelaunch() ? "Update installed - press A to restart"
                                                                   : "Update installed - reopen NuvioNX");
                        } else {
                            upd_ = UpdState::Failed;
                            updMsg_ = r.second;
                            App::get().toast(r.second);
                        }
                    });
                break;
            }
            case UpdState::Done:
                if (update::canRelaunch()) {
                    update::relaunchOnExit();
                    App::get().requestQuit();
                } else {
                    App::get().toast("Please close and reopen NuvioNX to finish updating");
                }
                break;
            default:
                break;  // Checking / Downloading: ignore further presses
        }
    }

    enum class UpdState { Idle, Checking, UpToDate, Available, Downloading, Done, Failed };

    int sel_ = 0;
    float scroll_ = 0;
    bool confirm_ = false;
    double confirmUntil_ = 0;
    UpdState upd_ = UpdState::Idle;
    std::string updVersion_, updUrl_, updMsg_;
};

}  // namespace

std::shared_ptr<Page> makeSettingsPage() { return std::make_shared<SettingsPage>(); }
