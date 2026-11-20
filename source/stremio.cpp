#include "stremio.hpp"

#include <algorithm>
#include <cctype>

#include "net.hpp"

namespace stremio {

static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

static bool startsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

static bool endsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

bool CatalogDesc::supportsExtra(const std::string& n) const {
    for (auto& e : extras)
        if (e.name == n) return true;
    return false;
}

bool CatalogDesc::needsUserInput() const {
    for (auto& e : extras)
        if (e.required) return true;
    return false;
}

std::string normalizeManifestUrl(std::string url) {
    url = trim(url);
    if (startsWith(url, "stremio://")) url = "https://" + url.substr(10);
    if (!startsWith(url, "http://") && !startsWith(url, "https://")) url = "https://" + url;
    std::string query;
    size_t q = url.find('?');
    if (q != std::string::npos) {
        query = url.substr(q);
        url = url.substr(0, q);
    }
    while (!url.empty() && url.back() == '/') url.pop_back();
    if (!endsWith(url, "/manifest.json")) url += "/manifest.json";
    return url + query;
}

void splitManifestUrl(const std::string& manifestUrl, std::string& base, std::string& query) {
    std::string u = manifestUrl;
    query.clear();
    size_t q = u.find('?');
    if (q != std::string::npos) {
        query = u.substr(q);
        u = u.substr(0, q);
    }
    if (endsWith(u, "/manifest.json")) u = u.substr(0, u.size() - 14);
    while (!u.empty() && u.back() == '/') u.pop_back();
    base = u;
}

bool Addon::parseManifest(const json::Value& m) {
    if (!m.isObject()) return false;
    id = m["id"].str();
    name = m["name"].str(id);
    version = m["version"].str();
    description = m["description"].str();
    logo = m["logo"].str();
    types = m["types"].strings();
    idPrefixes = m["idPrefixes"].strings();
    catalogs.clear();
    resources.clear();
    for (auto& c : m["catalogs"].items()) {
        CatalogDesc cd;
        cd.type = c["type"].str();
        cd.id = c["id"].str();
        cd.name = c["name"].str(cd.id);
        if (cd.type.empty() || cd.id.empty()) continue;
        if (c["extra"].isArray()) {
            for (auto& e : c["extra"].items()) {
                ExtraDesc ed;
                if (e.isString()) {
                    ed.name = e.str();
                } else {
                    ed.name = e["name"].str();
                    ed.required = e["isRequired"].boolean(false);
                    ed.options = e["options"].strings();
                }
                if (!ed.name.empty()) cd.extras.push_back(ed);
            }
        }
        // legacy form
        for (auto& n : c["extraSupported"].strings()) {
            if (!cd.supportsExtra(n)) cd.extras.push_back({n, false, {}});
        }
        for (auto& n : c["extraRequired"].strings()) {
            bool found = false;
            for (auto& e : cd.extras)
                if (e.name == n) {
                    e.required = true;
                    found = true;
                }
            if (!found) cd.extras.push_back({n, true, {}});
        }
        catalogs.push_back(cd);
    }
    for (auto& r : m["resources"].items()) {
        ResourceDesc rd;
        if (r.isString()) {
            rd.name = r.str();
        } else {
            rd.name = r["name"].str();
            if (r.has("types")) {
                rd.types = r["types"].strings();
                rd.hasTypes = true;
            }
            if (r.has("idPrefixes") && r["idPrefixes"].isArray()) {
                rd.idPrefixes = r["idPrefixes"].strings();
                rd.hasPrefixes = true;
            }
        }
        if (!rd.name.empty()) resources.push_back(rd);
    }
    return !id.empty();
}

bool Addon::hasResource(const std::string& res) const {
    for (auto& r : resources)
        if (r.name == res) return true;
    return false;
}

bool Addon::supports(const std::string& res, const std::string& type, const std::string& vid) const {
    for (auto& r : resources) {
        if (r.name != res) continue;
        const auto& ts = r.hasTypes ? r.types : types;
        if (!ts.empty() && std::find(ts.begin(), ts.end(), type) == ts.end()) continue;
        const auto& ps = r.hasPrefixes ? r.idPrefixes : idPrefixes;
        if (ps.empty()) return true;
        for (auto& p : ps)
            if (startsWith(vid, p)) return true;
    }
    return false;
}

std::string Addon::resourceUrl(const std::string& res, const std::string& type, const std::string& rid,
                               const std::string& extra) const {
    std::string u = base + "/" + res + "/" + net::urlEncode(type) + "/" + net::urlEncode(rid);
    if (!extra.empty()) u += "/" + extra;
    u += ".json" + query;
    return u;
}

std::string Stream::kindLabel() const {
    if (!url.empty()) return "";
    if (!infoHash.empty()) return "Torrent";
    if (!ytId.empty()) return "YouTube";
    if (!externalUrl.empty()) return "External";
    return "Unsupported";
}

std::string typeLabel(const std::string& type, bool plural) {
    if (type == "movie") return plural ? "Movies" : "Movie";
    if (type == "series") return plural ? "Series" : "Series";
    if (type == "channel") return plural ? "Channels" : "Channel";
    if (type == "tv") return "TV";
    if (type == "anime") return "Anime";
    if (type.empty()) return "";
    std::string t = type;
    t[0] = (char)toupper((unsigned char)t[0]);
    return t;
}

MetaPreview parsePreview(const json::Value& v) {
    MetaPreview m;
    m.id = v["id"].str();
    if (m.id.empty()) m.id = v["imdb_id"].str();
    m.type = v["type"].str();
    m.name = v["name"].str();
    m.poster = v["poster"].str();
    m.background = v["background"].str();
    m.logo = v["logo"].str();
    m.description = v["description"].str();
    m.releaseInfo = v["releaseInfo"].str();
    if (m.releaseInfo.empty()) m.releaseInfo = v["year"].str();
    m.imdbRating = v["imdbRating"].str();
    m.posterShape = v["posterShape"].str("poster");
    m.genres = v["genres"].strings();
    if (m.genres.empty()) m.genres = v["genre"].strings();
    if (m.genres.empty()) {
        for (auto& l : v["links"].items())
            if (l["category"].str() == "Genres") m.genres.push_back(l["name"].str());
    }
    return m;
}

bool fetchManifest(const std::string& url, Addon& out, std::string& raw, std::string& err) {
    std::string mu = normalizeManifestUrl(url);
    auto r = net::get(mu, {}, 20);
    if (!r.ok()) {
        err = r.error.empty() ? "request failed" : r.error;
        return false;
    }
    json::Value v;
    if (!json::parse(r.body, v)) {
        err = "invalid manifest JSON";
        return false;
    }
    Addon a;
    if (!a.parseManifest(v)) {
        err = "manifest missing id";
        return false;
    }
    a.manifestUrl = mu;
    splitManifestUrl(mu, a.base, a.query);
    a.loaded = true;
    a.enabled = out.enabled;
    out = a;
    raw = r.body;
    return true;
}

std::vector<MetaPreview> fetchCatalog(const Addon& a, const CatalogDesc& c, const std::string& extra, bool* ok) {
    std::vector<MetaPreview> out;
    auto r = net::get(a.resourceUrl("catalog", c.type, c.id, extra));
    json::Value v;
    if (!r.ok() || !json::parse(r.body, v)) {
        if (ok) *ok = false;
        return out;
    }
    if (ok) *ok = true;
    for (auto& m : v["metas"].items()) {
        if (!m.isObject()) continue;
        auto p = parsePreview(m);
        if (p.id.empty() || p.name.empty()) continue;
        if (p.type.empty()) p.type = c.type;
        out.push_back(p);
    }
    return out;
}

bool fetchMeta(const Addon& a, const std::string& type, const std::string& id, Meta& out) {
    auto r = net::get(a.resourceUrl("meta", type, id));
    json::Value v;
    if (!r.ok() || !json::parse(r.body, v)) return false;
    const auto& m = v["meta"];
    if (!m.isObject()) return false;
    MetaPreview p = parsePreview(m);
    if (p.name.empty()) return false;
    static_cast<MetaPreview&>(out) = p;
    if (out.id.empty()) out.id = id;
    if (out.type.empty()) out.type = type;
    out.runtime = m["runtime"].str();
    out.country = m["country"].str();
    out.cast = m["cast"].strings();
    out.director = m["director"].strings();
    for (auto& l : m["links"].items()) {
        std::string cat = l["category"].str();
        if (cat == "Cast" && out.cast.size() < 12) {
            std::string n = l["name"].str();
            if (std::find(out.cast.begin(), out.cast.end(), n) == out.cast.end()) out.cast.push_back(n);
        } else if (cat == "Directors" && out.director.empty()) {
            out.director.push_back(l["name"].str());
        }
    }
    for (auto& t : m["trailerStreams"].items()) {
        if (!t["ytId"].str().empty()) {
            out.trailerYt = t["ytId"].str();
            break;
        }
    }
    for (auto& vid : m["videos"].items()) {
        Video ve;
        ve.id = vid["id"].str();
        if (ve.id.empty()) continue;
        ve.title = vid["title"].str();
        if (ve.title.empty()) ve.title = vid["name"].str();
        ve.overview = vid["overview"].str();
        if (ve.overview.empty()) ve.overview = vid["description"].str();
        ve.thumbnail = vid["thumbnail"].str();
        ve.released = vid["released"].str();
        ve.season = (int)vid["season"].integer(0);
        ve.episode = (int)vid["episode"].integer(0);
        if (ve.episode == 0) ve.episode = (int)vid["number"].integer(0);
        out.videos.push_back(ve);
    }
    std::stable_sort(out.videos.begin(), out.videos.end(), [](const Video& x, const Video& y) {
        if (x.season != y.season) return x.season < y.season;
        return x.episode < y.episode;
    });
    return true;
}

static Subtitle parseSubtitle(const json::Value& s) {
    Subtitle sub;
    sub.id = s["id"].str();
    sub.url = s["url"].str();
    sub.lang = s["lang"].str();
    sub.label = s["label"].str();
    return sub;
}

std::vector<Stream> fetchStreams(const Addon& a, const std::string& type, const std::string& videoId, bool* ok) {
    std::vector<Stream> out;
    auto r = net::get(a.resourceUrl("stream", type, videoId), {}, 30);
    json::Value v;
    if (!r.ok() || !json::parse(r.body, v)) {
        if (ok) *ok = false;
        return out;
    }
    if (ok) *ok = true;
    for (auto& s : v["streams"].items()) {
        if (!s.isObject()) continue;
        Stream st;
        st.addonName = a.name;
        st.addonId = a.id;
        st.name = s["name"].str();
        st.title = s["title"].str();
        st.description = s["description"].str();
        st.url = s["url"].str();
        st.infoHash = s["infoHash"].str();
        st.ytId = s["ytId"].str();
        st.externalUrl = s["externalUrl"].str();
        st.fileIdx = (int)s["fileIdx"].integer(-1);
        st.sources = s["sources"].strings();
        const auto& bh = s["behaviorHints"];
        st.bingeGroup = bh["bingeGroup"].str();
        st.filename = bh["filename"].str();
        st.size = bh["videoSize"].integer(0);
        const auto& req = bh["proxyHeaders"]["request"];
        for (size_t i = 0; i < req.keys().size(); i++)
            st.headers.push_back(req.keys()[i] + ": " + req.items()[i].str());
        for (auto& sub : s["subtitles"].items()) {
            auto x = parseSubtitle(sub);
            if (!x.url.empty()) st.subtitles.push_back(x);
        }
        if (st.name.empty()) st.name = a.name;
        if (st.url.empty() && st.infoHash.empty() && st.ytId.empty() && st.externalUrl.empty()) continue;
        out.push_back(st);
    }
    return out;
}

std::vector<Subtitle> fetchSubtitles(const Addon& a, const std::string& type, const std::string& videoId) {
    std::vector<Subtitle> out;
    auto r = net::get(a.resourceUrl("subtitles", type, videoId), {}, 20);
    json::Value v;
    if (!r.ok() || !json::parse(r.body, v)) return out;
    for (auto& s : v["subtitles"].items()) {
        auto x = parseSubtitle(s);
        if (!x.url.empty()) out.push_back(x);
    }
    return out;
}

}  // namespace stremio
