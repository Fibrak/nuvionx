// Persistent state on the SD card: sdmc:/switch/NuvioNX/
#pragma once
#include <string>
#include <vector>

#include "stremio.hpp"

namespace store {

struct Settings {
    int theme = 0;
    std::string subLang = "eng";   // ISO 639-2, "" = off
    std::string audioLang = "";    // "" = file default
    bool render1080 = false;       // render video at 1080p (heavier on CPU)
    bool showUnsupported = true;   // list torrent/YouTube streams (greyed out)
    bool autoplayNext = true;
    bool hwdec = true;
    bool lanServer = true;
    bool diskImageCache = false;  // cache artwork on the SD card (off = fewer SD writes)
    bool cpuBoost = false;        // CPU boost mode (1785 MHz) while a video plays
    bool perfOverlay = false;     // debug FPS/cache overlay (toggle with ZL+ZR)
    // Debrid API keys (imported from the Nuvio account or entered manually)
    std::string realDebridKey, torboxKey, premiumizeKey;
};

struct AddonEntry {
    std::string url;
    bool enabled = true;
};

struct Progress {
    std::string videoId, metaId, type, name, poster, background, logo, epTitle;
    int season = 0, episode = 0;
    double pos = 0, dur = 0;
    long long updated = 0;
    // pct: position known only as a percentage (from Trakt): pos = 0..100, dur = 100.
    // Replaced by real seconds as soon as the title is played on the Switch.
    bool pct = false;
    bool watched() const { return dur > 0 && pos / dur >= 0.92; }
    bool resumable() const { return !watched() && (pct ? pos >= 1.0 : pos > 30.0); }
};

void load();
// Batched SD writes: tick() each frame writes pending progress/manifests at most
// every 5 minutes; flush() writes them now (exit, leaving the player, HOME).
void tick();
void flush();

Settings& settings();
void saveSettings();

std::vector<AddonEntry>& addonEntries();
void saveAddons();
std::string cachedManifest(const std::string& url);
void cacheManifest(const std::string& url, const std::string& raw);

const Progress* progress(const std::string& videoId);
void setProgress(const Progress& p);
const Progress* latestForMeta(const std::string& metaId);
std::vector<Progress> continueWatching(size_t max = 20);
void removeFromContinue(const std::string& metaId);
void clearProgress();
// Merge progress pulled from the account: newer timestamps win, local titles/artwork are kept.
void mergeProgress(const std::vector<Progress>& remote);
// Fill in title/artwork for entries that only have ids (doesn't touch timestamps).
void updateProgressMeta(const stremio::Meta& m);
int progressRevision();

bool inLibrary(const std::string& id);
void toggleLibrary(const stremio::MetaPreview& m);  // add or remove
std::vector<stremio::MetaPreview> library();
void replaceLibrary(const std::vector<stremio::MetaPreview>& items);
void replaceAddons(const std::vector<AddonEntry>& entries);

}  // namespace store
