// In-app self-updater: checks the GitHub "latest release" for a newer nuvionx.nro,
// downloads it over the running NRO, and chain-reloads on exit.
#pragma once
#include <string>

namespace update {

struct Info {
    bool ok = false;         // the query reached GitHub and parsed
    bool available = false;  // the latest release is newer than this build
    std::string version;     // remote version without a leading 'v', e.g. "0.1.2"
    std::string url;         // download URL for the nuvionx.nro asset
    std::string notes;       // release body (changelog), may be empty
    std::string error;       // human-readable reason when ok == false
};

// This build's version string, e.g. "0.1.1" (from the Makefile's APP_VERSION).
const char* currentVersion();

// Record the running NRO's SD path (argv[0]). Call once at startup. Ignored unless
// it points at a ".nro"; without it, updating is disabled.
void setSelfPath(const std::string& path);
const std::string& selfPath();

// Blocking GitHub query. Run on a worker thread.
Info check();

// Blocking download of `url`, verified and written over the running NRO.
// Returns true on success; otherwise sets `err`. Run on a worker thread.
bool download(const std::string& url, std::string& err);

// True if we know our own path and the loader supports chain-loading.
bool canRelaunch();
// Queue the freshly-written NRO to load when the app exits.
void relaunchOnExit();

}  // namespace update
