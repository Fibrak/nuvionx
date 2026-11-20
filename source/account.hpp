// Nuvio account: TV/device-code login + pulling synced data (addons, debrid
// keys, library, watch progress, profiles). Port of NuvioTV's AuthManager and
// sync services (GPL-3.0). The backend URL and publishable key are user
// configuration (Settings / LAN page / sdmc:/switch/NuvioNX/account.json).
#pragma once
#include <string>
#include <vector>

#include "store.hpp"
#include "stremio.hpp"

namespace account {

struct Session {
    std::string accessToken, refreshToken, userId, email;
    long long expiresAt = 0;  // unix seconds
    bool valid() const { return !refreshToken.empty(); }
};

struct State {
    std::string serverUrl;       // e.g. https://xxxx.supabase.co
    std::string publishableKey;  // Supabase anon / publishable key
    std::string linkBaseUrl = "https://nuvio.tv/link";
    std::string legacyLinkBaseUrl = "https://nuvio.tv/tv-login";
    Session session;
    int profileId = 1;
    long long lastSync = 0;
};

void load();
void save();
State& state();
bool configured();
bool signedIn();
void signOut();

// ---- blocking calls (run on worker threads) ----
struct LoginStart {
    bool ok = false;
    std::string error;
    std::string code;      // device_code (or legacy code) used for polling
    std::string userCode;  // what the user types on the website
    std::string url;       // verification_uri_complete
    std::string verifyUri; // verification_uri (short)
    std::string nonce;
    int interval = 3;
};
LoginStart startLogin(const State& cfg);
// Returns "approved", "pending", "expired", "used", "cancelled" or "" on error.
std::string pollLogin(const State& cfg, const std::string& code, const std::string& nonce, std::string& err);
bool exchangeLogin(const State& cfg, const std::string& code, const std::string& nonce, Session& out,
                   std::string& err);
bool passwordLogin(const State& cfg, const std::string& email, const std::string& password, Session& out,
                   std::string& err);

struct Profile {
    int index = 1;
    std::string name;
    bool usesPrimaryAddons = false;
};

struct SyncResult {
    bool ok = false;
    std::string error;
    Session session;  // possibly refreshed tokens
    std::vector<Profile> profiles;
    bool haveAddons = false;
    std::vector<store::AddonEntry> addons;
    std::string realDebridKey, torboxKey, premiumizeKey;
    bool haveCredentials = false;
    bool haveLibrary = false;
    std::vector<stremio::MetaPreview> library;
    std::vector<store::Progress> progress;
};
SyncResult pull(State cfg);

}  // namespace account
