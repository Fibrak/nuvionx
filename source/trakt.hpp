// Trakt.tv integration (port of NuvioTV's Trakt support): device-code sign-in,
// pulling playback progress / watched history / watchlist, and scrobbling.
// Uses the user's own Trakt API app (client id + secret), stored in
// sdmc:/switch/NuvioNX/trakt.json.
#pragma once
#include <string>
#include <vector>

#include "store.hpp"
#include "stremio.hpp"

namespace trakt {

struct State {
    std::string clientId, clientSecret;
    std::string accessToken, refreshToken, username;
    long long expiresAt = 0;  // unix seconds
    long long lastSync = 0;
};

void load();
void save();
State& state();
bool configured();  // client id entered (secret is optional: Trakt PKCE apps have none)
bool signedIn();
void signOut();

// ---- blocking calls (worker threads) ----
struct DeviceCode {
    bool ok = false;
    std::string error, deviceCode, userCode, verifyUrl;
    int interval = 5, expiresIn = 600;
};
DeviceCode startDevice(const State& s);
// Returns "ok" (tokens written into `out`), "pending", "slow_down", "expired", "denied" or "error".
std::string pollDevice(const State& s, const std::string& deviceCode, State& out, std::string& err);

struct PullResult {
    bool ok = false;
    std::string error;
    State state;  // possibly refreshed tokens + username
    std::vector<store::Progress> progress;  // in-progress (percent-based)
    std::vector<store::Progress> watched;   // fully watched
    std::vector<stremio::MetaPreview> watchlist;
};
PullResult pull(State s);

// What's playing, for scrobbling / history.
struct Item {
    std::string imdb;   // show or movie IMDb id (tt...)
    std::string type;   // "movie" or "series"
    int season = 0, episode = 0;
};
// action: "start", "pause" or "stop". Returns possibly-refreshed state.
State scrobble(State s, const std::string& action, const Item& item, double progressPct);
State setWatched(State s, const Item& item, bool watched);

// Artwork for ids we only know from Trakt (Cinemeta/Metahub images by IMDb id).
std::string posterFor(const std::string& imdb);
std::string backgroundFor(const std::string& imdb);
std::string logoFor(const std::string& imdb);

}  // namespace trakt
