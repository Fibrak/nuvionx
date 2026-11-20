// Stremio-compatible addon protocol (the same protocol NuvioTV speaks).
#pragma once
#include <string>
#include <vector>

#include "json.hpp"

namespace stremio {

struct ExtraDesc {
    std::string name;
    bool required = false;
    std::vector<std::string> options;
};

struct CatalogDesc {
    std::string type, id, name;
    std::vector<ExtraDesc> extras;
    bool supportsExtra(const std::string& n) const;
    // True if the catalog needs an extra we can't provide on the home screen.
    bool needsUserInput() const;
};

struct ResourceDesc {
    std::string name;
    std::vector<std::string> types;
    std::vector<std::string> idPrefixes;
    bool hasTypes = false, hasPrefixes = false;
};

struct Addon {
    std::string manifestUrl;  // canonical ".../manifest.json"
    std::string base;         // manifest URL without "/manifest.json" (query kept separately)
    std::string query;        // "?..." suffix from the manifest URL, if any
    std::string id, name, version, description, logo;
    std::vector<CatalogDesc> catalogs;
    std::vector<ResourceDesc> resources;
    std::vector<std::string> types, idPrefixes;
    bool enabled = true;
    bool loaded = false;
    std::string error;

    bool parseManifest(const json::Value& m);
    bool hasResource(const std::string& res) const;
    bool supports(const std::string& res, const std::string& type, const std::string& id) const;
    std::string resourceUrl(const std::string& res, const std::string& type, const std::string& id,
                            const std::string& extra = "") const;
};

struct MetaPreview {
    std::string id, type, name, poster, background, logo, description, releaseInfo, imdbRating, posterShape;
    std::vector<std::string> genres;
    // Used for "Continue watching" cards.
    std::string videoId, subtitle;
    float progress = -1;
};

struct Video {
    std::string id, title, overview, thumbnail, released;
    int season = 0, episode = 0;
};

struct Meta : MetaPreview {
    std::string runtime, country;
    std::vector<std::string> cast, director;
    std::vector<Video> videos;
    std::string trailerYt;
};

struct Subtitle {
    std::string id, url, lang, label;
};

struct Stream {
    std::string addonName, addonId;
    std::string name, title, description;
    std::string url, infoHash, ytId, externalUrl;
    int fileIdx = -1;
    std::vector<std::string> headers;  // "Key: Value"
    std::vector<std::string> sources;  // torrent trackers ("tracker:udp://...")
    std::vector<Subtitle> subtitles;
    std::string bingeGroup, filename;
    long long size = 0;
    bool playable() const { return !url.empty(); }
    std::string kindLabel() const;  // "Torrent", "YouTube", "External", ""
};

std::string normalizeManifestUrl(std::string url);
void splitManifestUrl(const std::string& manifestUrl, std::string& base, std::string& query);
MetaPreview parsePreview(const json::Value& v);
std::string typeLabel(const std::string& type, bool plural = true);

// Blocking network calls (run them on worker threads).
bool fetchManifest(const std::string& url, Addon& out, std::string& raw, std::string& err);
std::vector<MetaPreview> fetchCatalog(const Addon& a, const CatalogDesc& c, const std::string& extra, bool* ok);
bool fetchMeta(const Addon& a, const std::string& type, const std::string& id, Meta& out);
std::vector<Stream> fetchStreams(const Addon& a, const std::string& type, const std::string& videoId, bool* ok);
std::vector<Subtitle> fetchSubtitles(const Addon& a, const std::string& type, const std::string& videoId);

}  // namespace stremio
