// Nuvio account: server setup, device-code / email sign-in, profile choice, sync.
#include <algorithm>
#include <ctime>
#include <tuple>

#include "account.hpp"
#include "third_party/qrcodegen.h"
#include "input.hpp"
#include "screens.hpp"
#include "store.hpp"
#include "trakt.hpp"

using namespace gfx;

namespace {

class AccountPage : public Page {
public:
    void update(float dt) override {
        (void)dt;
        if (login_.active) {
            updateLogin();
            return;
        }
        if (tlogin_.active) {
            updateTraktLogin();
            return;
        }
        auto btns = buttons();
        if (input::pressed(input::Up)) {
            if (sel_ == 0) wantsTabBar = true;
            else sel_--;
        }
        if (input::pressed(input::Down)) sel_ = std::min((int)btns.size() - 1, sel_ + 1);
        // Left/Right jump between the Nuvio (left) and Trakt (right) columns
        if (input::pressed(input::Right) || input::pressed(input::Left)) {
            bool toRight = input::pressed(input::Right);
            for (size_t i = 0; i < btns.size(); i++)
                if (btns[i].right == toRight) {
                    sel_ = (int)i;
                    break;
                }
        }
        sel_ = std::max(0, std::min(sel_, (int)btns.size() - 1));
        if (input::pressed(input::A) && !btns.empty()) activate(btns[sel_].id);
    }

    void render() override {
        const Theme& t = theme();
        float x = 48, y = 96;
        if (tlogin_.active) {
            renderTraktLogin(x, y + 46);  // draws its own title
            return;
        }
        text("Nuvio account", x, y, 30, t.text, true);
        y += 46;

        if (login_.active) {
            renderLogin(x, y);
            return;
        }
        renderTraktColumn();

        auto& st = account::state();
        const float colW = 560;
        if (!account::configured()) {
            paragraph("Sign in with your Nuvio account to import your addons, debrid setup, library and watch "
                      "progress. First enter the sync server your account lives on (its URL and publishable key).",
                      x, y, colW, 16, t.text2, 5);
            y += 104;
            std::string lan = App::get().lanUrl();
            if (!lan.empty()) paragraph("Easier: open  " + lan + "/account  on your phone.", x, y, colW, 15, t.accent, 2);
            y += 44;
        } else if (!account::signedIn()) {
            text("Server  " + st.serverUrl, x, y, 15, t.text3, false, Left, colW);
            y += 28;
            paragraph("Sign in with a code (approve it on your phone, like on NuvioTV) or with email and password.",
                      x, y, colW, 16, t.text2, 3);
            y += 72;
        } else {
            std::string who = st.session.email.empty() ? "Signed in" : "Signed in as " + st.session.email;
            text(who, x, y, 19, t.text, true, Left, colW);
            y += 30;
            std::string status = App::get().syncing() ? "Syncing..." : App::get().syncStatus();
            if (status.empty() && st.lastSync > 0) status = "Last synced " + ago(st.lastSync);
            paragraph(status, x, y, colW, 15, t.text2, 2);
            y += 44;
            auto& s = store::settings();
            std::string deb = s.realDebridKey.empty() ? "" : "Real-Debrid";
            if (!s.torboxKey.empty()) deb += (deb.empty() ? "" : ", ") + std::string("TorBox");
            if (!s.premiumizeKey.empty()) deb += (deb.empty() ? "" : ", ") + std::string("Premiumize");
            text("Debrid: " + (deb.empty() ? std::string("none") : deb) + "   Addons: " +
                     std::to_string(App::get().addons().size()) + "   Library: " +
                     std::to_string(store::library().size()),
                 x, y, 14, t.text3, false, Left, colW);
            y += 36;
        }

        // buttons: left column (Nuvio) flows below the text, right column (Trakt) has its own flow
        auto btns = buttons();
        float ry = traktButtonsY_;
        for (size_t i = 0; i < btns.size(); i++) {
            float bx = btns[i].right ? 680 : x;
            float& by = btns[i].right ? ry : y;
            ui::button(btns[i].label, bx, by, 46, (int)i == sel_, false);
            int bi = (int)i;
            ui::tapToActivate(bx, by, ui::buttonWidth(btns[i].label), 46, [this, bi] { sel_ = bi; });
            by += 58;
        }
    }

    void renderTraktColumn() {
        const Theme& t = theme();
        float x = 680, y = 96, w = W - 48 - x;
        fillRect(x - 24, 100, 2, 520, withA(t.text3, 60));
        text("Trakt", x, y, 30, t.text, true);
        y += 46;
        auto& ts = trakt::state();
        if (!trakt::configured()) {
            paragraph("Sync watch progress, history and watchlist with Trakt, and scrobble what you watch. "
                      "Create a free API app at trakt.tv/oauth/applications (redirect URI: "
                      "urn:ietf:wg:oauth:2.0:oob) and enter its Client ID (a Secret isn't needed for newer "
                      "PKCE apps).",
                      x, y, w, 15, t.text2, 7);
            y += 134;
            std::string lan = App::get().lanUrl();
            if (!lan.empty()) paragraph("Or paste it at  " + lan + "/account", x, y, w, 15, t.accent, 2);
            y += 40;
        } else if (!trakt::signedIn()) {
            paragraph("Connect your Trakt account with a code, like on NuvioTV.", x, y, w, 16, t.text2, 2);
            y += 56;
        } else {
            text(ts.username.empty() ? "Connected" : "Connected as " + ts.username, x, y, 19, t.text, true, Left, w);
            y += 30;
            std::string status = App::get().traktSyncing() ? "Syncing..." : App::get().traktStatus();
            if (status.empty() && ts.lastSync > 0) status = "Last synced " + ago(ts.lastSync);
            if (status.empty()) status = "Scrobbling is on while you watch.";
            paragraph(status, x, y, w, 15, t.text2, 3);
            y += 66;
        }
        traktButtonsY_ = y;
    }

    std::vector<ui::Hint> hints() override {
        if (login_.active || tlogin_.active) return {{"B", "Cancel"}};
        return {{"A", "Select"}, {"<>", "Nuvio / Trakt"}};
    }

private:
    enum Id {
        UseOfficial, ServerUrl, ServerKey, LoginCode, LoginEmail, SyncNow, Profile, SignOut, EditServer,
        TraktId, TraktSecret, TraktConnect, TraktSync, TraktDisconnect, TraktChangeApp
    };
    struct Btn {
        Id id;
        std::string label;
        bool right = false;  // Trakt column
    };
    float traktButtonsY_ = 300;

    std::vector<Btn> buttons() {
        auto& st = account::state();
        std::vector<Btn> b;
        if (!account::configured()) {
            b.push_back({UseOfficial, "Use official Nuvio server"});
            b.push_back({ServerUrl, st.serverUrl.empty() ? "Enter server URL" : "Server URL: " + st.serverUrl});
            b.push_back({ServerKey, st.publishableKey.empty() ? "Enter publishable key" : "Publishable key: set"});
        } else if (!account::signedIn()) {
            b.push_back({LoginCode, "Sign in with code"});
            b.push_back({LoginEmail, "Sign in with email"});
            b.push_back({EditServer, "Change server"});
        } else {
            b.push_back({SyncNow, "Sync now"});
            if (App::get().profiles().size() > 1) b.push_back({Profile, "Profile: " + profileName()});
            b.push_back({SignOut, "Sign out"});
        }
        // Trakt column
        auto& ts = trakt::state();
        if (!trakt::configured()) {
            b.push_back({TraktId, ts.clientId.empty() ? "Enter Client ID" : "Client ID: set", true});
        } else if (!trakt::signedIn()) {
            b.push_back({TraktConnect, "Connect Trakt", true});
            b.push_back({TraktSecret, ts.clientSecret.empty() ? "Set Client Secret (optional)" : "Client Secret: set",
                         true});
            b.push_back({TraktChangeApp, "Change Trakt app", true});
        } else {
            b.push_back({TraktSync, "Sync Trakt now", true});
            b.push_back({TraktDisconnect, "Disconnect Trakt", true});
        }
        return b;
    }

    std::string profileName() {
        int pid = account::state().profileId;
        for (auto& p : App::get().profiles())
            if (p.index == pid) return p.name;
        return "Profile " + std::to_string(pid);
    }

    static std::string ago(long long t) {
        long long d = (long long)time(nullptr) - t;
        if (d < 60) return "just now";
        if (d < 3600) return std::to_string(d / 60) + " min ago";
        if (d < 86400) return std::to_string(d / 3600) + " h ago";
        return std::to_string(d / 86400) + " days ago";
    }

    void activate(Id id) {
        auto& st = account::state();
        std::string v;
        switch (id) {
            case UseOfficial:
                // Nuvio's public backend URL. The publishable (anon) key is published in
                // Nuvio's own API docs; it isn't bundled here - paste it when prompted.
                st.serverUrl = "https://api.nuvio.tv";
                account::save();
                if (ui::keyboard("Nuvio publishable key (from Nuvio's API docs)",
                                 st.publishableKey.empty() ? "sb_publishable_" : st.publishableKey, v, true)) {
                    st.publishableKey = v;
                    account::save();
                }
                sel_ = 0;
                break;
            case ServerUrl:
                if (ui::keyboard("Sync server URL (https://...)", st.serverUrl.empty() ? "https://" : st.serverUrl, v,
                                 true)) {
                    while (!v.empty() && v.back() == '/') v.pop_back();
                    st.serverUrl = v;
                    account::save();
                }
                break;
            case ServerKey:
                if (ui::keyboard("Publishable (anon) key", st.publishableKey, v, true)) {
                    st.publishableKey = v;
                    account::save();
                }
                break;
            case EditServer:
                st.serverUrl.clear();
                st.publishableKey.clear();
                account::save();
                sel_ = 0;
                break;
            case LoginCode: startCodeLogin(); break;
            case LoginEmail: emailLogin(); break;
            case SyncNow: App::get().syncAccount(true); break;
            case Profile: {
                auto& ps = App::get().profiles();
                size_t i = 0;
                for (; i < ps.size(); i++)
                    if (ps[i].index == st.profileId) break;
                st.profileId = ps[(i + 1) % ps.size()].index;
                account::save();
                App::get().syncAccount(true);
                break;
            }
            case SignOut:
                account::signOut();
                App::get().toast("Signed out (imported data stays on this Switch)");
                sel_ = 0;
                break;
            case TraktId:
                if (ui::keyboard("Trakt API Client ID", trakt::state().clientId, v, true)) {
                    trakt::state().clientId = v;
                    trakt::save();
                }
                break;
            case TraktSecret:
                if (ui::keyboard("Trakt API Client Secret", trakt::state().clientSecret, v, true)) {
                    trakt::state().clientSecret = v;
                    trakt::save();
                }
                break;
            case TraktChangeApp:
                trakt::state().clientId.clear();
                trakt::state().clientSecret.clear();
                trakt::save();
                break;
            case TraktConnect: startTraktLogin(); break;
            case TraktSync: App::get().syncTrakt(true); break;
            case TraktDisconnect:
                trakt::signOut();
                App::get().toast("Trakt disconnected (synced data stays on this Switch)");
                break;
        }
    }

    // ------------------------------------------------------------ Trakt device login
    struct TraktLogin {
        bool active = false, starting = false, polling = false;
        trakt::DeviceCode d;
        std::string status;
        double nextPoll = 0, expiresAt = 0;
    } tlogin_;
    int tgen_ = 0;

    void startTraktLogin() {
        tlogin_ = TraktLogin{};
        tlogin_.active = true;
        tlogin_.starting = true;
        tlogin_.status = "Requesting a code from Trakt...";
        int gen = ++tgen_;
        trakt::State s = trakt::state();
        async([s]() { return trakt::startDevice(s); },
              [this, gen](trakt::DeviceCode& d) {
                  if (gen != tgen_ || !tlogin_.active) return;
                  tlogin_.starting = false;
                  if (!d.ok) {
                      tlogin_.status = "Couldn't start Trakt sign-in: " + d.error +
                                       " (check the Client ID / Secret)";
                      return;
                  }
                  tlogin_.d = d;
                  tlogin_.status = "Waiting for approval...";
                  tlogin_.nextPoll = now() + d.interval;
                  tlogin_.expiresAt = now() + d.expiresIn;
              });
    }

    void updateTraktLogin() {
        if (input::pressed(input::B)) {
            tlogin_.active = false;
            tgen_++;
            return;
        }
        if (tlogin_.starting || tlogin_.polling || tlogin_.d.deviceCode.empty()) return;
        if (now() > tlogin_.expiresAt) {
            tlogin_.status = "Code expired. Press B and try again.";
            tlogin_.d.deviceCode.clear();
            return;
        }
        if (now() < tlogin_.nextPoll) return;
        tlogin_.polling = true;
        int gen = tgen_;
        trakt::State s = trakt::state();
        std::string code = tlogin_.d.deviceCode;
        async(
            [s, code]() {
                auto r = std::make_tuple(std::string(), trakt::State(), std::string());
                std::get<0>(r) = trakt::pollDevice(s, code, std::get<1>(r), std::get<2>(r));
                return r;
            },
            [this, gen](std::tuple<std::string, trakt::State, std::string>& r) {
                if (gen != tgen_ || !tlogin_.active) return;
                tlogin_.polling = false;
                const std::string& st = std::get<0>(r);
                tlogin_.nextPoll = now() + tlogin_.d.interval + (st == "slow_down" ? 5 : 0);
                if (st == "ok") {
                    auto& cur = trakt::state();
                    const trakt::State& got = std::get<1>(r);
                    cur.accessToken = got.accessToken;
                    cur.refreshToken = got.refreshToken;
                    cur.expiresAt = got.expiresAt;
                    cur.username = got.username;
                    trakt::save();
                    tlogin_.active = false;
                    App::get().toast("Trakt connected" + (got.username.empty() ? "" : " as " + got.username));
                    App::get().syncTrakt(true);
                } else if (st == "denied") {
                    tlogin_.status = "Access denied on Trakt. Press B and try again.";
                    tlogin_.d.deviceCode.clear();
                } else if (st == "expired") {
                    tlogin_.status = "Code expired or already used. Press B and try again.";
                    tlogin_.d.deviceCode.clear();
                } else if (st == "error") {
                    tlogin_.status = "Waiting... (" + std::get<2>(r) + ")";
                }
            });
    }

    void renderTraktLogin(float x, float y) {
        const Theme& t = theme();
        text("Connect Trakt", x, y - 46 + 0, 30, t.text, true);
        if (!tlogin_.d.deviceCode.empty()) {
            std::string url = tlogin_.d.verifyUrl;
            buildQr(url);
            float qs = 330, qx = W - 48 - qs, qy = 120;
            drawQr(qx, qy, qs);
            text("Scan with your phone camera", qx + qs / 2, qy + qs + 14, 16, t.text2, true, Center);
            float tw = qx - x - 40;
            text("Open  " + url, x, y + 4, 22, t.text, true, Left, tw);
            text("and enter this code:", x, y + 40, 18, t.text2);
            float cw = textWidth(tlogin_.d.userCode, 50, true) + 60;
            roundRect(x, y + 74, cw, 80, 16, t.elevated);
            text(tlogin_.d.userCode, x + 30, y + 86, 50, t.accent, true);
            y += 190;
        }
        if (tlogin_.starting || tlogin_.polling) spinner(x + 10, y + 12, 9, t.text2);
        text(tlogin_.status, x + 30, y + 2, 17, t.text2, false, Left, 700);
    }

    // ------------------------------------------------------------ code login
    struct Login {
        bool active = false, starting = false, polling = false, exchanging = false;
        account::LoginStart s;
        std::string status;
        double nextPoll = 0;
        int gen = 0;
    } login_;

    void startCodeLogin() {
        login_ = Login{};
        login_.active = true;
        login_.starting = true;
        login_.status = "Requesting a sign-in code...";
        int gen = ++gen_;
        login_.gen = gen;
        account::State cfg = account::state();
        async([cfg]() { return account::startLogin(cfg); },
              [this, gen](account::LoginStart& r) {
                  if (gen != gen_ || !login_.active) return;
                  login_.starting = false;
                  if (!r.ok) {
                      login_.status = "Couldn't start sign-in: " + r.error;
                      return;
                  }
                  login_.s = r;
                  login_.status = "Waiting for approval...";
                  login_.nextPoll = now() + r.interval;
              });
    }

    void updateLogin() {
        if (input::pressed(input::B)) {
            login_.active = false;
            gen_++;
            return;
        }
        if (login_.starting || login_.s.code.empty() || login_.polling || login_.exchanging) return;
        if (now() < login_.nextPoll) return;
        login_.polling = true;
        int gen = gen_;
        account::State cfg = account::state();
        std::string code = login_.s.code, nonce = login_.s.nonce;
        async(
            [cfg, code, nonce]() {
                std::string err;
                std::string status = account::pollLogin(cfg, code, nonce, err);
                return std::make_pair(status, err);
            },
            [this, gen](std::pair<std::string, std::string>& r) {
                if (gen != gen_ || !login_.active) return;
                login_.polling = false;
                login_.nextPoll = now() + login_.s.interval;
                const std::string& status = r.first;
                if (status == "approved") {
                    exchange();
                } else if (status == "pending" || (status.empty() && !r.second.empty())) {
                    login_.status = status.empty() ? "Waiting... (" + r.second + ")" : "Waiting for approval...";
                } else if (!status.empty()) {
                    login_.status = "Code " + status + ". Press B and try again.";
                    login_.s.code.clear();
                }
            });
    }

    void exchange() {
        login_.exchanging = true;
        login_.status = "Signing in...";
        int gen = gen_;
        account::State cfg = account::state();
        std::string code = login_.s.code, nonce = login_.s.nonce;
        async(
            [cfg, code, nonce]() {
                auto res = std::make_tuple(false, account::Session(), std::string());
                std::get<0>(res) = account::exchangeLogin(cfg, code, nonce, std::get<1>(res), std::get<2>(res));
                return res;
            },
            [this, gen](std::tuple<bool, account::Session, std::string>& r) {
                if (gen != gen_) return;
                login_.exchanging = false;
                if (!std::get<0>(r)) {
                    login_.status = "Sign-in failed: " + std::get<2>(r);
                    login_.s.code.clear();
                    return;
                }
                finishLogin(std::get<1>(r));
            });
    }

    void finishLogin(const account::Session& s) {
        account::state().session = s;
        account::state().profileId = 1;
        account::save();
        login_.active = false;
        sel_ = 0;
        App::get().toast("Signed in - importing your account...");
        App::get().syncAccount(true);
    }

    void emailLogin() {
        std::string email, pw;
        if (!ui::keyboard("Email", "", email, true)) return;
        if (!ui::keyboard("Password", "", pw, false, true)) return;
        login_ = Login{};
        login_.active = true;
        login_.starting = true;
        login_.status = "Signing in...";
        int gen = ++gen_;
        account::State cfg = account::state();
        async(
            [cfg, email, pw]() {
                auto res = std::make_tuple(false, account::Session(), std::string());
                std::get<0>(res) = account::passwordLogin(cfg, email, pw, std::get<1>(res), std::get<2>(res));
                return res;
            },
            [this, gen](std::tuple<bool, account::Session, std::string>& r) {
                if (gen != gen_) return;
                login_.starting = false;
                if (!std::get<0>(r)) {
                    login_.status = "Sign-in failed: " + std::get<2>(r);
                    return;
                }
                finishLogin(std::get<1>(r));
            });
    }

    // Encode the verification link once per login session.
    void buildQr(const std::string& url) {
        if (url == qrUrl_) return;
        qrUrl_ = url;
        qrSize_ = 0;
        qrBits_.clear();
        if (url.empty()) return;
        std::vector<uint8_t> tmp(qrcodegen_BUFFER_LEN_MAX), qr(qrcodegen_BUFFER_LEN_MAX);
        if (!qrcodegen_encodeText(url.c_str(), tmp.data(), qr.data(), qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
                                  qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true))
            return;
        qrSize_ = qrcodegen_getSize(qr.data());
        qrBits_.resize((size_t)qrSize_ * qrSize_);
        for (int y = 0; y < qrSize_; y++)
            for (int x = 0; x < qrSize_; x++) qrBits_[(size_t)y * qrSize_ + x] = qrcodegen_getModule(qr.data(), x, y);
    }

    void drawQr(float x, float y, float size) {
        if (qrSize_ <= 0) return;
        const int quiet = 3;  // quiet-zone modules on each side
        float m = size / (qrSize_ + 2 * quiet);
        roundRect(x, y, size, size, 14, Col{255, 255, 255, 255});
        float ox = x + quiet * m, oy = y + quiet * m;
        for (int r = 0; r < qrSize_; r++) {
            // merge horizontal runs into one rect each
            int c = 0;
            while (c < qrSize_) {
                if (!qrBits_[(size_t)r * qrSize_ + c]) {
                    c++;
                    continue;
                }
                int start = c;
                while (c < qrSize_ && qrBits_[(size_t)r * qrSize_ + c]) c++;
                fillRect(ox + start * m, oy + r * m, (c - start) * m + 0.6f, m + 0.6f, Col{0, 0, 0, 255});
            }
        }
    }

    void renderLogin(float x, float y) {
        const Theme& t = theme();
        if (!login_.s.code.empty()) {
            std::string site = login_.s.verifyUri.empty() ? account::state().linkBaseUrl : login_.s.verifyUri;
            std::string qrTarget = login_.s.url.empty() ? site : login_.s.url;
            buildQr(qrTarget);

            // QR on the right, like NuvioTV's sign-in screen
            float qs = 330, qx = W - 48 - qs, qy = 120;
            drawQr(qx, qy, qs);
            text("Scan with your phone camera", qx + qs / 2, qy + qs + 14, 16, t.text2, true, Center);

            float tw = qx - x - 40;
            text("Scan the QR code to sign in", x, y, 24, t.text, true);
            paragraph("Point your phone's camera at the code, sign in to Nuvio and approve this Switch. "
                      "Your addons, debrid setup, library and progress will import automatically.",
                      x, y + 40, tw, 17, t.text2, 3);
            text("Or open  " + site + "  and enter:", x, y + 124, 17, t.text2, false, Left, tw);
            float cw = textWidth(login_.s.userCode, 50, true) + 60;
            roundRect(x, y + 156, cw, 80, 16, t.elevated);
            text(login_.s.userCode, x + 30, y + 168, 50, t.accent, true);
            y += 270;
        }
        if (login_.starting || login_.polling || login_.exchanging) spinner(x + 10, y + 12, 9, t.text2);
        text(login_.status, x + 30, y + 2, 17, t.text2, false, Left, 700);
    }

    std::string qrUrl_;
    int qrSize_ = 0;
    std::vector<bool> qrBits_;

    int sel_ = 0;
    int gen_ = 0;
};

}  // namespace

std::shared_ptr<Page> makeAccountPage() { return std::make_shared<AccountPage>(); }
