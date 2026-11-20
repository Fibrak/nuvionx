#include "trakt.hpp"

#include <cstdio>
#include <ctime>

#include "diag.hpp"
#include "json.hpp"
#include "net.hpp"
#include "sdfile.hpp"

namespace trakt {

static void tlog(const std::string& m) { diag::log("[trakt] " + m); }

static State g_state;
static const char* kPath = "sdmc:/switch/NuvioNX/trakt.json";
static const char* kApi = "https://api.trakt.tv";

State& state() { return g_state; }
bool configured() { return !g_state.clientId.empty(); }  // secret optional (PKCE apps have none)
bool signedIn() { return configured() && !g_state.refreshToken.empty(); }

void load() {
    std::string s;
    if (!sdfile::read(kPath, s)) return;
    json::Value v;
    if (!json::parse(s, v)) return;
    g_state.clientId = v["clientId"].str();
    g_state.clientSecret = v["clientSecret"].str();
    g_state.accessToken = v["accessToken"].str();
    g_state.refreshToken = v["refreshToken"].str();
    g_state.username = v["username"].str();
    g_state.expiresAt = v["expiresAt"].integer(0);
    g_state.lastSync = v["lastSync"].integer(0);
}

void save() {
    json::Value o = json::Value::object();
    o.set("clientId", g_state.clientId);
    o.set("clientSecret", g_state.clientSecret);
    o.set("accessToken", g_state.accessToken);
    o.set("refreshToken", g_state.refreshToken);
    o.set("username", g_state.username);
    o.set("expiresAt", g_state.expiresAt);
    o.set("lastSync", g_state.lastSync);
    sdfile::write(kPath, o.dump());
}

void signOut() {
    g_state.accessToken.clear();
    g_state.refreshToken.clear();
    g_state.username.clear();
    g_state.expiresAt = 0;
    g_state.lastSync = 0;
    save();
}

std::string posterFor(const std::string& imdb) {
    return imdb.empty() ? "" : "https://images.metahub.space/poster/small/" + imdb + "/img";
}
std::string backgroundFor(const std::string& imdb) {
    return imdb.empty() ? "" : "https://images.metahub.space/background/medium/" + imdb + "/img";
}
std::string logoFor(const std::string& imdb) {
    return imdb.empty() ? "" : "https://images.metahub.space/logo/medium/" + imdb + "/img";
}

// ------------------------------------------------------------------ http
static std::vector<std::string> headers(const State& s, bool auth) {
    std::vector<std::string> h = {"Content-Type: application/json", "trakt-api-version: 2",
                                  "trakt-api-key: " + s.clientId};
    if (auth && !s.accessToken.empty()) h.push_back("Authorization: Bearer " + s.accessToken);
    return h;
}

static std::string errorOf(const net::Response& r) {
    json::Value v;
    if (json::parse(r.body, v)) {
        std::string m = v["error_description"].str(v["error"].str());
        if (!m.empty()) return m;
    }
    return r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error;
}

static bool parseTokens(const std::string& body, State& s) {
    json::Value v;
    if (!json::parse(body, v) || v["access_token"].str().empty()) return false;
    s.accessToken = v["access_token"].str();
    s.refreshToken = v["refresh_token"].str(s.refreshToken);
    long long created = v["created_at"].integer((long long)time(nullptr));
    s.expiresAt = created + v["expires_in"].integer(7 * 24 * 3600);
    return true;
}

static bool refresh(State& s, std::string& err) {
    json::Value b = json::Value::object();
    b.set("refresh_token", s.refreshToken);
    b.set("client_id", s.clientId);
    if (!s.clientSecret.empty()) b.set("client_secret", s.clientSecret);  // omitted for PKCE apps
    b.set("redirect_uri", "urn:ietf:wg:oauth:2.0:oob");
    b.set("grant_type", "refresh_token");
    auto r = net::request("POST", std::string(kApi) + "/oauth/token", headers(s, false), b.dump());
    tlog("refresh status=" + std::to_string(r.status));
    if (!r.ok() || !parseTokens(r.body, s)) {
        err = errorOf(r);
        tlog("refresh failed: " + err + " body=" + r.body.substr(0, 200));
        return false;
    }
    return true;
}

static bool ensureFresh(State& s, std::string& err) {
    if (s.refreshToken.empty()) {
        err = "not signed in";
        return false;
    }
    if (!s.accessToken.empty() && s.expiresAt - 3600 > (long long)time(nullptr)) return true;
    return refresh(s, err);
}

// Authenticated request with one refresh-and-retry on 401.
static net::Response call(State& s, const std::string& method, const std::string& path, const std::string& body,
                          std::string& err) {
    net::Response r;
    for (int attempt = 0; attempt < 2; attempt++) {
        r = net::request(method, std::string(kApi) + path, headers(s, true), body, 30);
        tlog(method + " " + path.substr(0, 40) + " status=" + std::to_string(r.status));
        if (r.status == 401 && attempt == 0) {
            if (!refresh(s, err)) return r;
            continue;
        }
        if (!r.ok()) err = errorOf(r);
        return r;
    }
    return r;
}

// "2024-05-01T20:15:03.000Z" -> unix seconds (UTC)
static long long parseIso(const std::string& s) {
    int Y, M, D, h = 0, m = 0, sec = 0;
    if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &sec) < 3) return 0;
    // days from civil (Howard Hinnant)
    Y -= M <= 2;
    long long era = (Y >= 0 ? Y : Y - 399) / 400;
    unsigned yoe = (unsigned)(Y - era * 400);
    unsigned doy = (153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = era * 146097 + (long long)doe - 719468;
    return days * 86400 + h * 3600 + m * 60 + sec;
}

// ------------------------------------------------------------------ device sign-in
DeviceCode startDevice(const State& s) {
    DeviceCode d;
    json::Value b = json::Value::object();
    b.set("client_id", s.clientId);
    auto r = net::request("POST", std::string(kApi) + "/oauth/device/code", headers(s, false), b.dump());
    tlog("device/code status=" + std::to_string(r.status) + " clientIdLen=" + std::to_string(s.clientId.size()));
    json::Value v;
    if (!r.ok() || !json::parse(r.body, v)) {
        d.error = errorOf(r);
        tlog("device/code failed: " + d.error + " body=" + r.body.substr(0, 200));
        return d;
    }
    d.deviceCode = v["device_code"].str();
    d.userCode = v["user_code"].str();
    d.verifyUrl = v["verification_url"].str("https://trakt.tv/activate");
    d.interval = (int)v["interval"].integer(5);
    d.expiresIn = (int)v["expires_in"].integer(600);
    d.ok = !d.deviceCode.empty();
    if (!d.ok) d.error = "no device code returned";
    return d;
}

std::string pollDevice(const State& s, const std::string& deviceCode, State& out, std::string& err) {
    json::Value b = json::Value::object();
    b.set("code", deviceCode);
    b.set("client_id", s.clientId);
    if (!s.clientSecret.empty()) b.set("client_secret", s.clientSecret);  // omitted for PKCE apps
    auto r = net::request("POST", std::string(kApi) + "/oauth/device/token", headers(s, false), b.dump());
    tlog("device/token status=" + std::to_string(r.status) + " secret=" + (s.clientSecret.empty() ? "no" : "yes"));
    switch (r.status) {
        case 200: {
            out = s;
            if (!parseTokens(r.body, out)) {
                err = "invalid token response";
                return "error";
            }
            // fetch the username for display
            auto u = net::request("GET", std::string(kApi) + "/users/settings", headers(out, true), "", 20);
            json::Value v;
            if (u.ok() && json::parse(u.body, v)) out.username = v["user"]["username"].str();
            return "ok";
        }
        case 400: return "pending";
        case 429: return "slow_down";
        case 404:
        case 409:
        case 410: return "expired";
        case 418: return "denied";
        default: err = errorOf(r); tlog("device/token unexpected: " + err + " body=" + r.body.substr(0, 200)); return "error";
    }
}

// ------------------------------------------------------------------ pull
PullResult pull(State s) {
    PullResult res;
    std::string err;
    if (!ensureFresh(s, err)) {
        res.error = "Trakt session expired, reconnect (" + err + ")";
        return res;
    }
    json::Value v;

    // in-progress (playback) items: percent only, Trakt doesn't store seconds
    for (const char* type : {"movies", "episodes"}) {
        auto r = call(s, "GET", std::string("/sync/playback/") + type + "?limit=200", "", err);
        if (!r.ok() || !json::parse(r.body, v)) continue;
        for (auto& it : v.items()) {
            store::Progress p;
            p.pct = true;
            p.pos = it["progress"].num(0);
            p.dur = 100;
            p.updated = parseIso(it["paused_at"].str());
            if (it["type"].str() == "movie") {
                const auto& m = it["movie"];
                p.metaId = p.videoId = m["ids"]["imdb"].str();
                p.type = "movie";
                p.name = m["title"].str();
            } else {
                const auto& sh = it["show"];
                const auto& ep = it["episode"];
                p.metaId = sh["ids"]["imdb"].str();
                p.type = "series";
                p.name = sh["title"].str();
                p.season = (int)ep["season"].integer(0);
                p.episode = (int)ep["number"].integer(0);
                p.epTitle = ep["title"].str();
                p.videoId = p.metaId + ":" + std::to_string(p.season) + ":" + std::to_string(p.episode);
            }
            if (p.metaId.empty()) continue;
            p.poster = posterFor(p.metaId);
            p.background = backgroundFor(p.metaId);
            res.progress.push_back(p);
        }
    }

    // watched history
    auto r = call(s, "GET", "/sync/watched/movies", "", err);
    if (r.ok() && json::parse(r.body, v)) {
        for (auto& it : v.items()) {
            store::Progress p;
            p.metaId = p.videoId = it["movie"]["ids"]["imdb"].str();
            if (p.metaId.empty()) continue;
            p.type = "movie";
            p.name = it["movie"]["title"].str();
            p.pos = p.dur = 1;
            p.updated = parseIso(it["last_watched_at"].str());
            res.watched.push_back(p);
        }
    }
    r = call(s, "GET", "/sync/watched/shows", "", err);
    if (r.ok() && json::parse(r.body, v)) {
        for (auto& it : v.items()) {
            std::string imdb = it["show"]["ids"]["imdb"].str();
            if (imdb.empty()) continue;
            std::string title = it["show"]["title"].str();
            for (auto& season : it["seasons"].items()) {
                int sn = (int)season["number"].integer(0);
                for (auto& ep : season["episodes"].items()) {
                    store::Progress p;
                    p.metaId = imdb;
                    p.type = "series";
                    p.name = title;
                    p.season = sn;
                    p.episode = (int)ep["number"].integer(0);
                    p.videoId = imdb + ":" + std::to_string(sn) + ":" + std::to_string(p.episode);
                    p.pos = p.dur = 1;
                    p.updated = parseIso(ep["last_watched_at"].str());
                    res.watched.push_back(p);
                }
            }
        }
    }

    // watchlist -> library
    for (const char* type : {"movies", "shows"}) {
        r = call(s, "GET", std::string("/sync/watchlist/") + type, "", err);
        if (!r.ok() || !json::parse(r.body, v)) continue;
        bool movie = std::string(type) == "movies";
        for (auto& it : v.items()) {
            const auto& o = movie ? it["movie"] : it["show"];
            stremio::MetaPreview m;
            m.id = o["ids"]["imdb"].str();
            if (m.id.empty()) continue;
            m.type = movie ? "movie" : "series";
            m.name = o["title"].str();
            m.releaseInfo = o["year"].str();
            m.poster = posterFor(m.id);
            m.background = backgroundFor(m.id);
            res.watchlist.push_back(m);
        }
    }

    res.ok = true;
    res.state = s;
    tlog("pull done: progress=" + std::to_string(res.progress.size()) + " watched=" +
         std::to_string(res.watched.size()) + " watchlist=" + std::to_string(res.watchlist.size()));
    return res;
}

// ------------------------------------------------------------------ push
static json::Value itemBody(const Item& item) {
    json::Value b = json::Value::object();
    if (item.type == "movie") {
        json::Value ids = json::Value::object();
        ids.set("imdb", item.imdb);
        json::Value movie = json::Value::object();
        movie.set("ids", ids);
        b.set("movie", movie);
    } else {
        json::Value ids = json::Value::object();
        ids.set("imdb", item.imdb);
        json::Value show = json::Value::object();
        show.set("ids", ids);
        json::Value ep = json::Value::object();
        ep.set("season", item.season);
        ep.set("number", item.episode);
        b.set("show", show);
        b.set("episode", ep);
    }
    return b;
}

State scrobble(State s, const std::string& action, const Item& item, double progressPct) {
    std::string err;
    if (item.imdb.compare(0, 2, "tt") != 0 || !ensureFresh(s, err)) return s;
    json::Value b = itemBody(item);
    b.set("progress", progressPct < 0 ? 0.0 : (progressPct > 100 ? 100.0 : progressPct));
    call(s, "POST", "/scrobble/" + action, b.dump(), err);
    return s;
}

State setWatched(State s, const Item& item, bool watched) {
    std::string err;
    if (item.imdb.compare(0, 2, "tt") != 0 || !ensureFresh(s, err)) return s;
    // /sync/history expects arrays: {"movies":[...]} or {"shows":[{ids, seasons:[{number, episodes:[{number}]}]}]}
    json::Value body = json::Value::object();
    json::Value ids = json::Value::object();
    ids.set("imdb", item.imdb);
    if (item.type == "movie") {
        json::Value m = json::Value::object();
        m.set("ids", ids);
        json::Value arr = json::Value::array();
        arr.push(m);
        body.set("movies", arr);
    } else {
        json::Value ep = json::Value::object();
        ep.set("number", item.episode);
        json::Value eps = json::Value::array();
        eps.push(ep);
        json::Value season = json::Value::object();
        season.set("number", item.season);
        season.set("episodes", eps);
        json::Value seasons = json::Value::array();
        seasons.push(season);
        json::Value show = json::Value::object();
        show.set("ids", ids);
        show.set("seasons", seasons);
        json::Value arr = json::Value::array();
        arr.push(show);
        body.set("shows", arr);
    }
    call(s, "POST", watched ? "/sync/history" : "/sync/history/remove", body.dump(), err);
    return s;
}

}  // namespace trakt
