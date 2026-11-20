#include "debrid.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <thread>

#include "json.hpp"
#include "net.hpp"

namespace debrid {

std::string activeProvider(const store::Settings& s) {
    if (!s.realDebridKey.empty()) return "Real-Debrid";
    if (!s.torboxKey.empty()) return "TorBox";
    if (!s.premiumizeKey.empty()) return "Premiumize";
    return "";
}

std::string shortLabel(const store::Settings& s) {
    if (!s.realDebridKey.empty()) return "RD";
    if (!s.torboxKey.empty()) return "TB";
    if (!s.premiumizeKey.empty()) return "PM";
    return "";
}

namespace {

struct File {
    std::string id, name;
    long long size = 0;
};

std::string lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::string baseName(const std::string& p) {
    size_t i = p.find_last_of('/');
    return i == std::string::npos ? p : p.substr(i + 1);
}

bool isVideo(const std::string& name) {
    static const char* exts[] = {".mkv", ".mp4", ".avi", ".m4v", ".mov", ".ts", ".webm", ".wmv", ".m2ts", ".mpg"};
    std::string n = lower(name);
    for (auto e : exts) {
        size_t el = strlen(e);
        if (n.size() >= el && n.compare(n.size() - el, el, e) == 0) return true;
    }
    return false;
}

// Pick the file the addon meant: exact filename, then fileIdx (torrent order), then largest video.
int chooseFile(const std::vector<File>& files, const stremio::Stream& st, bool idxIsOrder) {
    if (files.empty()) return -1;
    if (!st.filename.empty()) {
        std::string want = lower(baseName(st.filename));
        for (size_t i = 0; i < files.size(); i++)
            if (lower(baseName(files[i].name)) == want) return (int)i;
    }
    if (idxIsOrder && st.fileIdx >= 0 && st.fileIdx < (int)files.size() && isVideo(files[st.fileIdx].name))
        return st.fileIdx;
    int best = -1;
    for (size_t i = 0; i < files.size(); i++) {
        if (!isVideo(files[i].name)) continue;
        if (best < 0 || files[i].size > files[best].size) best = (int)i;
    }
    if (best < 0) {
        for (size_t i = 0; i < files.size(); i++)
            if (best < 0 || files[i].size > files[best].size) best = (int)i;
    }
    return best;
}

std::string magnetFor(const stremio::Stream& st) {
    std::string m = "magnet:?xt=urn:btih:" + st.infoHash;
    if (!st.filename.empty()) m += "&dn=" + net::urlEncode(st.filename);
    for (auto& src : st.sources)
        if (src.compare(0, 8, "tracker:") == 0) m += "&tr=" + net::urlEncode(src.substr(8));
    return m;
}

std::string apiError(const net::Response& r) {
    json::Value v;
    if (json::parse(r.body, v)) {
        for (const char* k : {"error", "message", "detail"}) {
            std::string m = v[k].str();
            if (!m.empty()) return m;
        }
    }
    return r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error;
}

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// ------------------------------------------------------------ Real-Debrid
Result realDebrid(const stremio::Stream& st, const std::string& key) {
    Result res;
    const std::string api = "https://api.real-debrid.com/rest/1.0/";
    std::vector<std::string> h = {"Authorization: Bearer " + key,
                                  "Content-Type: application/x-www-form-urlencoded"};
    auto r = net::request("POST", api + "torrents/addMagnet", h, "magnet=" + net::urlEncode(magnetFor(st)));
    json::Value v;
    if (!r.ok() || !json::parse(r.body, v) || v["id"].str().empty()) {
        res.error = "Real-Debrid: " + apiError(r);
        return res;
    }
    std::string id = v["id"].str();
    auto info = [&](json::Value& out) {
        auto ir = net::request("GET", api + "torrents/info/" + id, h);
        return ir.ok() && json::parse(ir.body, out);
    };
    // wait for the file list
    for (int i = 0; i < 10; i++) {
        if (!info(v)) break;
        if (v["files"].size() > 0) break;
        sleepMs(700);
    }
    std::vector<File> files;
    for (auto& f : v["files"].items()) files.push_back({f["id"].str(), f["path"].str(), f["bytes"].integer(0)});
    int pick = chooseFile(files, st, true);
    if (pick < 0) {
        net::request("DELETE", api + "torrents/delete/" + id, h);
        res.error = "Real-Debrid: torrent has no files";
        return res;
    }
    net::request("POST", api + "torrents/selectFiles/" + id, h, "files=" + files[pick].id);
    std::string link;
    for (int i = 0; i < 12 && !net::aborting(); i++) {
        if (!info(v)) break;
        std::string status = v["status"].str();
        if (status == "downloaded" && v["links"].size() > 0) {
            link = v["links"].at(0).str();
            break;
        }
        if (status == "magnet_error" || status == "error" || status == "virus" || status == "dead") break;
        sleepMs(800);
    }
    if (link.empty()) {
        net::request("DELETE", api + "torrents/delete/" + id, h);
        res.error = "Not cached on Real-Debrid (try another stream)";
        return res;
    }
    r = net::request("POST", api + "unrestrict/link", h, "link=" + net::urlEncode(link));
    if (!r.ok() || !json::parse(r.body, v) || v["download"].str().empty()) {
        res.error = "Real-Debrid: " + apiError(r);
        return res;
    }
    res.url = v["download"].str();
    res.ok = true;
    return res;
}

// ------------------------------------------------------------ TorBox
Result torbox(const stremio::Stream& st, const std::string& key) {
    Result res;
    const std::string api = "https://api.torbox.app/v1/api/";
    std::vector<std::string> h = {"Authorization: Bearer " + key};
    auto r = net::postMultipart(api + "torrents/createtorrent", h, {{"magnet", magnetFor(st)}});
    json::Value v;
    if (!r.ok() || !json::parse(r.body, v) || !v["success"].boolean(false)) {
        res.error = "TorBox: " + apiError(r);
        return res;
    }
    std::string tid = v["data"]["torrent_id"].str();
    if (tid.empty()) tid = v["data"]["id"].str();
    if (tid.empty()) {
        res.error = "TorBox: no torrent id returned";
        return res;
    }
    json::Value t;
    bool ready = false;
    for (int i = 0; i < 12 && !net::aborting(); i++) {
        auto lr = net::request("GET", api + "torrents/mylist?id=" + tid + "&bypass_cache=true", h);
        if (lr.ok() && json::parse(lr.body, v)) {
            t = v["data"].isArray() ? v["data"].at(0) : v["data"];
            if (t["download_present"].boolean(false) || t["download_finished"].boolean(false)) {
                ready = true;
                break;
            }
        }
        sleepMs(800);
    }
    if (!ready) {
        res.error = "Not cached on TorBox (try another stream)";
        return res;
    }
    std::vector<File> files;
    for (auto& f : t["files"].items()) {
        std::string name = f["name"].str();
        if (name.empty()) name = f["short_name"].str();
        files.push_back({f["id"].str(), name, f["size"].integer(0)});
    }
    int pick = chooseFile(files, st, true);
    std::string url = api + "torrents/requestdl?token=" + net::urlEncode(key) + "&torrent_id=" + tid;
    if (pick >= 0) url += "&file_id=" + files[pick].id;
    r = net::request("GET", url, h);
    if (!r.ok() || !json::parse(r.body, v) || v["data"].str().empty()) {
        res.error = "TorBox: " + apiError(r);
        return res;
    }
    res.url = v["data"].str();
    res.ok = true;
    return res;
}

// ------------------------------------------------------------ Premiumize
Result premiumize(const stremio::Stream& st, const std::string& key) {
    Result res;
    std::vector<std::string> h = {"Content-Type: application/x-www-form-urlencoded"};
    std::string body = "apikey=" + net::urlEncode(key) + "&src=" + net::urlEncode(magnetFor(st));
    auto r = net::request("POST", "https://www.premiumize.me/api/transfer/directdl", h, body, 40);
    json::Value v;
    if (!r.ok() || !json::parse(r.body, v) || v["status"].str() != "success") {
        std::string m = v["message"].str();
        res.error = "Premiumize: " + (m.empty() ? apiError(r) : m);
        return res;
    }
    std::vector<File> files;
    std::vector<std::string> links;
    for (auto& f : v["content"].items()) {
        files.push_back({"", f["path"].str(), f["size"].integer(0)});
        links.push_back(f["link"].str().empty() ? f["stream_link"].str() : f["link"].str());
    }
    int pick = chooseFile(files, st, false);
    if (pick < 0 || links[pick].empty()) {
        res.error = "Not cached on Premiumize (try another stream)";
        return res;
    }
    res.url = links[pick];
    res.ok = true;
    return res;
}

}  // namespace

Result resolve(const stremio::Stream& st, const store::Settings& s) {
    if (st.infoHash.empty()) return {false, "", "Not a torrent stream"};
    if (!s.realDebridKey.empty()) return realDebrid(st, s.realDebridKey);
    if (!s.torboxKey.empty()) return torbox(st, s.torboxKey);
    if (!s.premiumizeKey.empty()) return premiumize(st, s.premiumizeKey);
    return {false, "", "No debrid service configured"};
}

}  // namespace debrid
