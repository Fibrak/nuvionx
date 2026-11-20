// Direct debrid resolution for torrent (infoHash) streams, like NuvioTV's
// DirectDebridResolver: Real-Debrid, TorBox and Premiumize.
#pragma once
#include <string>

#include "store.hpp"
#include "stremio.hpp"

namespace debrid {

// Provider that will be used ("Real-Debrid", ...), or "" if no key is set.
std::string activeProvider(const store::Settings& s);
std::string shortLabel(const store::Settings& s);  // "RD", "TB", "PM"
inline bool available(const store::Settings& s) { return !activeProvider(s).empty(); }
inline bool canPlay(const stremio::Stream& st, const store::Settings& s) {
    return st.playable() || (!st.infoHash.empty() && available(s));
}

struct Result {
    bool ok = false;
    std::string url, error;
};
// Blocking: run on a worker thread.
Result resolve(const stremio::Stream& st, const store::Settings& s);

}  // namespace debrid
