#include "account.hpp"

#include <switch.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>

#include "json.hpp"
#include "net.hpp"
#include "sdfile.hpp"

namespace account {

static State g_state;
static const char* kPath = "sdmc:/switch/NuvioNX/account.json";

State& state() { return g_state; }
bool configured() { return !g_state.serverUrl.empty() && !g_state.publishableKey.empty(); }
bool signedIn() { return configured() && g_state.session.valid(); }

static std::string trimSlash(std::string s) {
    while (!s.empty() && (s.back() == '/' || s.back() == ' ')) s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(0, 1);
    return s;
}

void load() {
    std::string s;
    if (!sdfile::read(kPath, s)) return;
    json::Value v;
    if (!json::parse(s, v)) return;
    g_state.serverUrl = trimSlash(v["serverUrl"].str());
    g_state.publishableKey = v["publishableKey"].str();
    if (!v["linkBaseUrl"].str().empty()) g_state.linkBaseUrl = v["linkBaseUrl"].str();
    if (!v["legacyLinkBaseUrl"].str().empty()) g_state.legacyLinkBaseUrl = v["legacyLinkBaseUrl"].str();
    g_state.session.accessToken = v["accessToken"].str();
    g_state.session.refreshToken = v["refreshToken"].str();
    g_state.session.userId = v["userId"].str();
    g_state.session.email = v["email"].str();
    g_state.session.expiresAt = v["expiresAt"].integer(0);
    g_state.profileId = (int)v["profileId"].integer(1);
    g_state.lastSync = v["lastSync"].integer(0);
}

void save() {
    json::Value o = json::Value::object();
    o.set("serverUrl", g_state.serverUrl);
    o.set("publishableKey", g_state.publishableKey);
    o.set("linkBaseUrl", g_state.linkBaseUrl);
    o.set("legacyLinkBaseUrl", g_state.legacyLinkBaseUrl);
    o.set("accessToken", g_state.session.accessToken);
    o.set("refreshToken", g_state.session.refreshToken);
    o.set("userId", g_state.session.userId);
    o.set("email", g_state.session.email);
    o.set("expiresAt", g_state.session.expiresAt);
    o.set("profileId", g_state.profileId);
    o.set("lastSync", g_state.lastSync);
    sdfile::write(kPath, o.dump());  // skipped automatically when unchanged
}

void signOut() {
    g_state.session = Session();
    g_state.lastSync = 0;
    save();
}

// ------------------------------------------------------------------ http helpers
static std::vector<std::string> headers(const State& cfg, const std::string& token = "") {
    std::vector<std::string> h = {"apikey: " + cfg.publishableKey, "Content-Type: application/json"};
    if (!token.empty()) h.push_back("Authorization: Bearer " + token);
    return h;
}

static std::string errorFrom(const net::Response& r) {
    json::Value v;
    if (json::parse(r.body, v)) {
        for (const char* k : {"message", "msg", "error_description", "error"}) {
            std::string m = v[k].str();
            if (!m.empty()) return m;
        }
    }
    if (!r.error.empty()) return r.error;
    return "HTTP " + std::to_string(r.status);
}

static net::Response post(const State& cfg, const std::string& path, const std::string& body,
                          const std::string& token = "") {
    return net::request("POST", trimSlash(cfg.serverUrl) + path, headers(cfg, token), body, 25);
}

static std::string randomNonce() {
    unsigned char b[16];
    randomGet(b, sizeof b);
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (unsigned char c : b) {
        s += hex[c >> 4];
        s += hex[c & 15];
    }
    return s;
}

static bool parseTokens(const std::string& body, Session& out, std::string& err) {
    json::Value v;
    if (!json::parse(body, v) || v["access_token"].str().empty()) {
        err = "unexpected token response";
        return false;
    }
    out.accessToken = v["access_token"].str();
    if (!v["refresh_token"].str().empty()) out.refreshToken = v["refresh_token"].str();
    long long exp = v["expires_in"].integer(3600);
    out.expiresAt = (long long)time(nullptr) + exp;
    if (v["user"].isObject()) {
        out.userId = v["user"]["id"].str(out.userId);
        out.email = v["user"]["email"].str(out.email);
    }
    return true;
}

// ------------------------------------------------------------------ login
LoginStart startLogin(const State& cfg) {
    LoginStart ls;
    ls.nonce = randomNonce();
    json::Value p = json::Value::object();
    p.set("p_device_nonce", ls.nonce);
    p.set("p_redirect_base_url", cfg.linkBaseUrl);
    p.set("p_device_type", "tv");
    p.set("p_device_name", "Nintendo Switch");
    auto r = post(cfg, "/rest/v1/rpc/start_device_login_session", p.dump());
    json::Value v;
    if (r.ok() && json::parse(r.body, v)) {
        const json::Value& row = v.isArray() ? v.at(0) : v;
        ls.code = row["device_code"].str();
        ls.userCode = row["user_code"].str();
        ls.url = row["verification_uri_complete"].str();
        ls.verifyUri = row["verification_uri"].str();
        ls.interval = std::max(2, (int)row["poll_interval_seconds"].integer(3));
        ls.ok = !ls.code.empty();
        if (!ls.ok) ls.error = "empty login session";
        return ls;
    }
    std::string e = errorFrom(r);
    std::string lower;
    for (char c : e) lower += (char)tolower((unsigned char)c);
    if (lower.find("could not find the function") == std::string::npos) {
        ls.error = e;
        return ls;
    }
    // older backends: legacy TV login
    json::Value lp = json::Value::object();
    lp.set("p_device_nonce", ls.nonce);
    lp.set("p_redirect_base_url", cfg.legacyLinkBaseUrl);
    lp.set("p_device_name", "Nintendo Switch");
    r = post(cfg, "/rest/v1/rpc/start_tv_login_session", lp.dump());
    if (!r.ok()) {
        lp = json::Value::object();
        lp.set("p_device_nonce", ls.nonce);
        lp.set("p_redirect_base_url", cfg.legacyLinkBaseUrl);
        r = post(cfg, "/rest/v1/rpc/start_tv_login_session", lp.dump());
    }
    if (!r.ok() || !json::parse(r.body, v)) {
        ls.error = errorFrom(r);
        return ls;
    }
    const json::Value& row = v.isArray() ? v.at(0) : v;
    ls.code = row["code"].str();
    ls.userCode = ls.code;
    ls.url = row["web_url"].str();
    ls.verifyUri = cfg.legacyLinkBaseUrl;
    ls.interval = std::max(2, (int)row["poll_interval_seconds"].integer(3));
    ls.ok = !ls.code.empty();
    if (!ls.ok) ls.error = "empty login session";
    return ls;
}

std::string pollLogin(const State& cfg, const std::string& code, const std::string& nonce, std::string& err) {
    json::Value p = json::Value::object();
    p.set("p_code", code);
    p.set("p_device_nonce", nonce);
    auto r = post(cfg, "/rest/v1/rpc/poll_tv_login_session", p.dump());
    json::Value v;
    if (!r.ok() || !json::parse(r.body, v)) {
        err = errorFrom(r);
        return "";
    }
    const json::Value& row = v.isArray() ? v.at(0) : v;
    return row["status"].str();
}

bool exchangeLogin(const State& cfg, const std::string& code, const std::string& nonce, Session& out,
                   std::string& err) {
    json::Value p = json::Value::object();
    p.set("code", code);
    p.set("device_nonce", nonce);
    auto r = post(cfg, "/functions/v1/tv-logins-exchange", p.dump());
    if (!r.ok()) {
        err = errorFrom(r);
        return false;
    }
    return parseTokens(r.body, out, err);
}

bool passwordLogin(const State& cfg, const std::string& email, const std::string& password, Session& out,
                   std::string& err) {
    json::Value p = json::Value::object();
    p.set("email", email);
    p.set("password", password);
    auto r = post(cfg, "/auth/v1/token?grant_type=password", p.dump());
    if (!r.ok()) {
        err = errorFrom(r);
        return false;
    }
    return parseTokens(r.body, out, err);
}

static bool refresh(const State& cfg, Session& s, std::string& err) {
    json::Value p = json::Value::object();
    p.set("refresh_token", s.refreshToken);
    auto r = post(cfg, "/auth/v1/token?grant_type=refresh_token", p.dump());
    if (!r.ok()) {
        err = errorFrom(r);
        return false;
    }
    return parseTokens(r.body, s, err);
}

// Authenticated call with one refresh-and-retry on 401.
static bool authed(const State& cfg, Session& s, const std::string& method, const std::string& path,
                   const std::string& body, json::Value& out, std::string& err) {
    for (int attempt = 0; attempt < 2; attempt++) {
        auto r = net::request(method, trimSlash(cfg.serverUrl) + path, headers(cfg, s.accessToken), body, 30);
        if (r.status == 401 && attempt == 0) {
            if (!refresh(cfg, s, err)) return false;
            continue;
        }
        if (!r.ok()) {
            err = errorFrom(r);
            return false;
        }
        if (r.body.empty()) {
            out = json::Value();
            return true;
        }
        if (!json::parse(r.body, out)) {
            err = "invalid JSON from " + path;
            return false;
        }
        return true;
    }
    return false;
}

static bool rpc(const State& cfg, Session& s, const std::string& name, const json::Value& params, json::Value& out,
                std::string& err) {
    return authed(cfg, s, "POST", "/rest/v1/rpc/" + name, params.dump(), out, err);
}

// ------------------------------------------------------------------ sync pull
SyncResult pull(State cfg) {
    SyncResult res;
    Session& s = cfg.session;
    std::string err;

    if (s.accessToken.empty() || s.expiresAt - 60 < (long long)time(nullptr)) {
        if (!refresh(cfg, s, err)) {
            res.error = "Session expired, please sign in again (" + err + ")";
            return res;
        }
    }

    json::Value v;
    std::string owner = s.userId;
    if (rpc(cfg, s, "get_sync_owner", json::Value::object(), v, err)) {
        std::string o = v.isArray() ? v.at(0).str() : v.str();
        if (!o.empty()) owner = o;
    }

    // profiles
    if (rpc(cfg, s, "sync_pull_profiles", json::Value::object(), v, err)) {
        for (auto& p : v.items()) {
            Profile pr;
            pr.index = (int)p["profile_index"].integer(1);
            pr.name = p["name"].str();
            pr.usesPrimaryAddons = p["uses_primary_addons"].boolean(false);
            res.profiles.push_back(pr);
        }
        std::sort(res.profiles.begin(), res.profiles.end(),
                  [](const Profile& a, const Profile& b) { return a.index < b.index; });
    }
    int pid = cfg.profileId > 0 ? cfg.profileId : 1;
    int addonProfile = pid;
    for (auto& p : res.profiles)
        if (p.index == pid && pid != 1 && p.usesPrimaryAddons) addonProfile = 1;

    // addons (direct table read, like AddonSyncService)
    if (!owner.empty()) {
        std::string path = "/rest/v1/addons?select=url,name,enabled,sort_order&user_id=eq." + net::urlEncode(owner) +
                           "&profile_id=eq." + std::to_string(addonProfile) + "&order=sort_order.asc";
        if (authed(cfg, s, "GET", path, "", v, err) && v.isArray()) {
            res.haveAddons = true;
            for (auto& a : v.items()) {
                store::AddonEntry e;
                e.url = stremio::normalizeManifestUrl(a["url"].str());
                e.enabled = a["enabled"].boolean(true);
                if (!a["url"].str().empty()) res.addons.push_back(e);
            }
        }
    }

    // provider credentials (debrid keys)
    json::Value pp = json::Value::object();
    pp.set("p_profile_id", pid);
    if (rpc(cfg, s, "sync_pull_provider_credentials", pp, v, err)) {
        res.haveCredentials = true;
        for (auto& row : v.items()) {
            std::string prov = row["provider"].str();
            const json::Value& cj = row["credential_json"];
            std::string key = cj["api_key"].str();
            if (key.empty() && cj.isString()) {
                json::Value inner;
                if (json::parse(cj.str(), inner)) key = inner["api_key"].str();
            }
            if (prov == "debrid:realdebrid") res.realDebridKey = key;
            else if (prov == "debrid:torbox") res.torboxKey = key;
            else if (prov == "debrid:premiumize") res.premiumizeKey = key;
        }
    }

    // library (paged)
    for (int offset = 0; offset < 5000; offset += 500) {
        json::Value lp = json::Value::object();
        lp.set("p_profile_id", pid);
        lp.set("p_limit", 500);
        lp.set("p_offset", offset);
        if (!rpc(cfg, s, "sync_pull_library", lp, v, err) || !v.isArray()) break;
        res.haveLibrary = true;
        for (auto& row : v.items()) {
            stremio::MetaPreview m;
            m.id = row["content_id"].str();
            m.type = row["content_type"].str();
            m.name = row["name"].str();
            m.poster = row["poster"].str();
            m.background = row["background"].str();
            m.description = row["description"].str();
            m.releaseInfo = row["release_info"].str();
            double r = row["imdb_rating"].num(0);
            if (r > 0) {
                char b[16];
                snprintf(b, sizeof b, "%.1f", r);
                m.imdbRating = b;
            }
            m.genres = row["genres"].strings();
            if (!m.id.empty()) res.library.push_back(m);
        }
        if (v.size() < 500) break;
    }

    // watch progress
    json::Value wp = json::Value::object();
    wp.set("p_profile_id", pid);
    if (rpc(cfg, s, "sync_pull_watch_progress", wp, v, err)) {
        for (auto& row : v.items()) {
            store::Progress p;
            p.metaId = row["content_id"].str();
            p.type = row["content_type"].str();
            p.videoId = row["video_id"].str();
            if (p.videoId.empty()) p.videoId = p.metaId;
            p.season = (int)row["season"].integer(0);
            p.episode = (int)row["episode"].integer(0);
            double pos = row["position"].num(0), dur = row["duration"].num(0);
            // NuvioTV (ExoPlayer) stores milliseconds
            if (dur > 50000 || pos > 50000) {
                pos /= 1000.0;
                dur /= 1000.0;
            }
            p.pos = pos;
            p.dur = dur;
            long long lw = row["last_watched"].integer(0);
            p.updated = lw > 100000000000LL ? lw / 1000 : lw;
            if (!p.metaId.empty()) res.progress.push_back(p);
        }
    }

    res.ok = true;
    res.session = s;
    return res;
}

}  // namespace account
