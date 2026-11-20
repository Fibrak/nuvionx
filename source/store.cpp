#include "store.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <map>
#include <unordered_set>

#include "json.hpp"
#include "sdfile.hpp"

namespace store {

static const char* kDir = "sdmc:/switch/NuvioNX";

static Settings g_settings;
static std::vector<AddonEntry> g_addons;
static json::Value g_manifests = json::Value::object();
static std::map<std::string, Progress> g_progress;
static std::vector<stremio::MetaPreview> g_library;
static bool g_progressDirty = false;   // pending progress write (see flush())
static bool g_manifestsDirty = false;  // pending manifest-cache write

static std::string path(const char* name) { return std::string(kDir) + "/" + name; }

// Native FS I/O (see sdfile.hpp): immune to descriptor-table issues in the
// network libraries, skips unchanged writes, atomic tmp + rename.
static bool readFile(const std::string& p, std::string& out) { return sdfile::read(p, out); }
static bool writeFile(const std::string& p, const std::string& data) { return sdfile::write(p, data); }

static bool readJson(const char* name, json::Value& v) {
    std::string s;
    if (!readFile(path(name), s)) return false;
    return json::parse(s, v);
}

static json::Value previewToJson(const stremio::MetaPreview& m) {
    json::Value o = json::Value::object();
    o.set("id", m.id);
    o.set("type", m.type);
    o.set("name", m.name);
    o.set("poster", m.poster);
    o.set("background", m.background);
    o.set("logo", m.logo);
    o.set("description", m.description);
    o.set("releaseInfo", m.releaseInfo);
    o.set("imdbRating", m.imdbRating);
    json::Value g = json::Value::array();
    for (auto& x : m.genres) g.push(x);
    o.set("genres", g);
    return o;
}

void load() {
    sdfile::mkdirs(kDir);

    json::Value v;
    if (readJson("settings.json", v)) {
        g_settings.theme = (int)v["theme"].integer(0);
        g_settings.subLang = v["subLang"].str("eng");
        g_settings.audioLang = v["audioLang"].str("");
        g_settings.render1080 = v["render1080"].boolean(false);
        g_settings.showUnsupported = v["showUnsupported"].boolean(true);
        g_settings.autoplayNext = v["autoplayNext"].boolean(true);
        g_settings.hwdec = v["hwdec"].boolean(true);
        g_settings.lanServer = v["lanServer"].boolean(true);
        g_settings.diskImageCache = v["diskImageCache"].boolean(false);
        g_settings.cpuBoost = v["cpuBoost"].boolean(false);
        g_settings.perfOverlay = v["perfOverlay"].boolean(false);
        g_settings.realDebridKey = v["realDebridKey"].str();
        g_settings.torboxKey = v["torboxKey"].str();
        g_settings.premiumizeKey = v["premiumizeKey"].str();
    }

    g_addons.clear();
    if (readJson("addons.json", v) && v.isArray()) {
        for (auto& a : v.items()) {
            AddonEntry e;
            e.url = a["url"].str();
            e.enabled = a["enabled"].boolean(true);
            if (!e.url.empty()) g_addons.push_back(e);
        }
    } else {
        // Same defaults NuvioTV ships with.
        g_addons.push_back({"https://v3-cinemeta.strem.io/manifest.json", true});
        g_addons.push_back({"https://opensubtitles-v3.strem.io/manifest.json", true});
        saveAddons();
    }

    if (!readJson("manifests.json", g_manifests) || !g_manifests.isObject()) g_manifests = json::Value::object();

    g_progress.clear();
    if (readJson("progress.json", v) && v.isObject()) {
        for (auto& p : v.items()) {
            Progress pr;
            pr.videoId = p["videoId"].str();
            pr.metaId = p["metaId"].str();
            pr.type = p["type"].str();
            pr.name = p["name"].str();
            pr.poster = p["poster"].str();
            pr.background = p["background"].str();
            pr.logo = p["logo"].str();
            pr.epTitle = p["epTitle"].str();
            pr.season = (int)p["season"].integer(0);
            pr.episode = (int)p["episode"].integer(0);
            pr.pos = p["pos"].num(0);
            pr.pct = p["pct"].boolean(false);
            pr.dur = p["dur"].num(0);
            pr.updated = p["updated"].integer(0);
            if (!pr.videoId.empty()) g_progress[pr.videoId] = pr;
        }
    }

    g_library.clear();
    if (readJson("library.json", v) && v.isArray()) {
        for (auto& m : v.items()) {
            auto p = stremio::parsePreview(m);
            if (!p.id.empty()) g_library.push_back(p);
        }
    }
}

Settings& settings() { return g_settings; }

void saveSettings() {
    json::Value o = json::Value::object();
    o.set("theme", g_settings.theme);
    o.set("subLang", g_settings.subLang);
    o.set("audioLang", g_settings.audioLang);
    o.set("render1080", g_settings.render1080);
    o.set("showUnsupported", g_settings.showUnsupported);
    o.set("autoplayNext", g_settings.autoplayNext);
    o.set("hwdec", g_settings.hwdec);
    o.set("lanServer", g_settings.lanServer);
    o.set("diskImageCache", g_settings.diskImageCache);
    o.set("cpuBoost", g_settings.cpuBoost);
    o.set("perfOverlay", g_settings.perfOverlay);
    o.set("realDebridKey", g_settings.realDebridKey);
    o.set("torboxKey", g_settings.torboxKey);
    o.set("premiumizeKey", g_settings.premiumizeKey);
    writeFile(path("settings.json"), o.dump());
}

std::vector<AddonEntry>& addonEntries() { return g_addons; }

void saveAddons() {
    json::Value a = json::Value::array();
    for (auto& e : g_addons) {
        json::Value o = json::Value::object();
        o.set("url", e.url);
        o.set("enabled", e.enabled);
        a.push(o);
    }
    writeFile(path("addons.json"), a.dump());
}

std::string cachedManifest(const std::string& url) { return g_manifests[url].str(); }

void cacheManifest(const std::string& url, const std::string& raw) {
    if (g_manifests[url].str() == raw) return;
    g_manifests.set(url, raw);
    // drop entries for addons that are no longer installed
    json::Value pruned = json::Value::object();
    for (size_t i = 0; i < g_manifests.keys().size(); i++) {
        const auto& k = g_manifests.keys()[i];
        for (auto& e : g_addons)
            if (e.url == k) {
                pruned.set(k, g_manifests.items()[i]);
                break;
            }
    }
    g_manifests = pruned;
    g_manifestsDirty = true;  // written with the next flush, not once per addon
}

static int g_progressRev = 0;
int progressRevision() { return g_progressRev; }

// Progress changes constantly during playback, so it lives in RAM and is written
// at most every few minutes (plus on exit / leaving the player / HOME button).
static time_t g_lastFlush = 0;
static const int kFlushIntervalSec = 300;

static void writeProgressFile();

static void saveProgress() {
    g_progressRev++;
    g_progressDirty = true;
}

void tick() {
    time_t t = time(nullptr);
    if (g_lastFlush == 0) g_lastFlush = t;
    if ((g_progressDirty || g_manifestsDirty) && t - g_lastFlush >= kFlushIntervalSec) flush();
}

void flush() {
    if (g_progressDirty) {
        writeProgressFile();
        g_progressDirty = false;
    }
    if (g_manifestsDirty) {
        writeFile(path("manifests.json"), g_manifests.dump());
        g_manifestsDirty = false;
    }
    g_lastFlush = time(nullptr);
}

static void writeProgressFile() {
    json::Value o = json::Value::object();
    for (auto& kv : g_progress) {
        const Progress& p = kv.second;
        json::Value e = json::Value::object();
        e.set("videoId", p.videoId);
        e.set("metaId", p.metaId);
        e.set("type", p.type);
        e.set("name", p.name);
        e.set("poster", p.poster);
        e.set("background", p.background);
        e.set("logo", p.logo);
        e.set("epTitle", p.epTitle);
        e.set("season", p.season);
        e.set("episode", p.episode);
        e.set("pos", p.pos);
        if (p.pct) e.set("pct", true);
        e.set("dur", p.dur);
        e.set("updated", p.updated);
        o.set(kv.first, e);
    }
    writeFile(path("progress.json"), o.dump());
}

const Progress* progress(const std::string& videoId) {
    auto it = g_progress.find(videoId);
    return it == g_progress.end() ? nullptr : &it->second;
}

void setProgress(const Progress& p) {
    Progress c = p;
    c.updated = (long long)time(nullptr);
    g_progress[c.videoId] = c;
    // keep the file bounded
    if (g_progress.size() > 600) {
        std::vector<std::pair<long long, std::string>> v;
        for (auto& kv : g_progress) v.push_back({kv.second.updated, kv.first});
        std::sort(v.begin(), v.end());
        for (size_t i = 0; i < v.size() - 500; i++) g_progress.erase(v[i].second);
    }
    saveProgress();
}

const Progress* latestForMeta(const std::string& metaId) {
    const Progress* best = nullptr;
    for (auto& kv : g_progress)
        if (kv.second.metaId == metaId && (!best || kv.second.updated > best->updated)) best = &kv.second;
    return best;
}

std::vector<Progress> continueWatching(size_t max) {
    std::vector<Progress> all;
    for (auto& kv : g_progress) all.push_back(kv.second);
    std::sort(all.begin(), all.end(), [](const Progress& a, const Progress& b) { return a.updated > b.updated; });
    std::vector<Progress> out;
    std::unordered_set<std::string> seen;
    for (auto& p : all) {
        if (seen.count(p.metaId)) continue;
        seen.insert(p.metaId);  // only the most recent entry per title counts
        if (p.updated < 0) continue;  // hidden by the user
        if (!p.resumable()) continue;
        out.push_back(p);
        if (out.size() >= max) break;
    }
    return out;
}

void removeFromContinue(const std::string& metaId) {
    for (auto& kv : g_progress)
        if (kv.second.metaId == metaId) kv.second.updated = -1;
    saveProgress();
}

void clearProgress() {
    g_progress.clear();
    saveProgress();
}

void mergeProgress(const std::vector<Progress>& remote) {
    bool changed = false;
    for (auto& r : remote) {
        if (r.videoId.empty()) continue;
        auto it = g_progress.find(r.videoId);
        if (it == g_progress.end()) {
            g_progress[r.videoId] = r;
            changed = true;
        } else if (r.updated > it->second.updated && it->second.updated >= 0) {
            Progress& l = it->second;
            l.pos = r.pos;
            l.dur = r.dur;
            l.pct = r.pct;
            l.updated = r.updated;
            if (l.name.empty()) l.name = r.name;
            if (l.poster.empty()) l.poster = r.poster;
            if (l.background.empty()) l.background = r.background;
            if (l.epTitle.empty()) l.epTitle = r.epTitle;
            if (l.season == 0) l.season = r.season;
            if (l.episode == 0) l.episode = r.episode;
            if (l.type.empty()) l.type = r.type;
            if (l.metaId.empty()) l.metaId = r.metaId;
            changed = true;
        }
    }
    if (changed) saveProgress();
}

void updateProgressMeta(const stremio::Meta& m) {
    bool changed = false;
    for (auto& kv : g_progress) {
        Progress& p = kv.second;
        if (p.metaId != m.id) continue;
        if (p.name.empty()) p.name = m.name;
        if (p.poster.empty()) p.poster = m.poster;
        if (p.background.empty()) p.background = m.background;
        if (p.logo.empty()) p.logo = m.logo;
        if (p.epTitle.empty()) {
            for (auto& v : m.videos)
                if (v.id == p.videoId) {
                    p.epTitle = v.title;
                    if (p.season == 0) p.season = v.season;
                    if (p.episode == 0) p.episode = v.episode;
                }
        }
        changed = true;
    }
    if (changed) saveProgress();
}

static void saveLibrary() {
    json::Value a = json::Value::array();
    for (auto& m : g_library) a.push(previewToJson(m));
    writeFile(path("library.json"), a.dump());
}

bool inLibrary(const std::string& id) {
    for (auto& m : g_library)
        if (m.id == id) return true;
    return false;
}

void toggleLibrary(const stremio::MetaPreview& m) {
    for (size_t i = 0; i < g_library.size(); i++) {
        if (g_library[i].id == m.id) {
            g_library.erase(g_library.begin() + i);
            saveLibrary();
            return;
        }
    }
    stremio::MetaPreview c = m;
    c.progress = -1;
    c.videoId.clear();
    c.subtitle.clear();
    g_library.insert(g_library.begin(), c);
    saveLibrary();
}

std::vector<stremio::MetaPreview> library() { return g_library; }

void replaceLibrary(const std::vector<stremio::MetaPreview>& items) {
    g_library = items;
    saveLibrary();
}

void replaceAddons(const std::vector<AddonEntry>& entries) {
    g_addons = entries;
    saveAddons();
}

}  // namespace store
