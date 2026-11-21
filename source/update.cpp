#include "update.hpp"

#include <switch.h>

#include "json.hpp"
#include "net.hpp"
#include "sdfile.hpp"

#ifndef APP_VERSION
#define APP_VERSION "0.0.0"
#endif

namespace update {
namespace {

const char* kRepo = "Fibrak/nuvionx";
const char* kAsset = "nuvionx.nro";
std::string g_self;

// Parse "1.2.3" (ignoring a leading 'v') into three ints; stops at any
// pre-release suffix. Missing components are 0.
void parseVer(const std::string& s, int out[3]) {
    out[0] = out[1] = out[2] = 0;
    size_t i = 0;
    if (i < s.size() && (s[i] == 'v' || s[i] == 'V')) i++;
    int part = 0;
    for (; i < s.size() && part < 3; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9')
            out[part] = out[part] * 10 + (c - '0');
        else if (c == '.')
            part++;
        else
            break;
    }
}

// > 0 if a is newer than b, < 0 if older, 0 if equal.
int cmpVer(const std::string& a, const std::string& b) {
    int va[3], vb[3];
    parseVer(a, va);
    parseVer(b, vb);
    for (int i = 0; i < 3; i++)
        if (va[i] != vb[i]) return va[i] - vb[i];
    return 0;
}

}  // namespace

const char* currentVersion() { return APP_VERSION; }

void setSelfPath(const std::string& path) {
    if (path.size() > 4 && path.compare(path.size() - 4, 4, ".nro") == 0) g_self = path;
}
const std::string& selfPath() { return g_self; }

Info check() {
    Info r;
    std::string url = std::string("https://api.github.com/repos/") + kRepo + "/releases/latest";
    net::Response resp = net::get(url, {"Accept: application/vnd.github+json"}, 20);
    if (!resp.ok()) {
        r.error = resp.error.empty() ? ("GitHub returned " + std::to_string(resp.status)) : resp.error;
        return r;
    }
    json::Value root;
    if (!json::parse(resp.body, root)) {
        r.error = "Couldn't read the release info";
        return r;
    }
    std::string tag = root["tag_name"].str();
    if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) tag = tag.substr(1);
    r.version = tag;
    r.notes = root["body"].str();
    for (const json::Value& a : root["assets"].items()) {
        if (a["name"].str() == kAsset) {
            r.url = a["browser_download_url"].str();
            break;
        }
    }
    if (r.version.empty() || r.url.empty()) {
        r.error = "The latest release has no downloadable build";
        return r;
    }
    r.ok = true;
    r.available = cmpVer(r.version, currentVersion()) > 0;
    return r;
}

bool download(const std::string& url, std::string& err) {
    std::string dest = selfPath();
    if (dest.empty()) {
        err = "Can't locate the app's .nro on the SD card";
        return false;
    }
    // The .nro is ~30 MB; raise net's default 24 MiB buffer cap so the download
    // isn't aborted mid-stream (that showed up as "Failed writing received data").
    net::Response resp = net::get(url, {}, 180, 48u * 1024u * 1024u);
    if (!resp.ok()) {
        err = resp.error.empty() ? ("Download failed (" + std::to_string(resp.status) + ")") : resp.error;
        return false;
    }
    // Guard against writing a truncated/HTML error page over our own binary:
    // every NRO carries the magic "NRO0" at offset 0x10.
    if (resp.body.size() < 0x40 || resp.body.compare(0x10, 4, "NRO0") != 0) {
        err = "The downloaded file isn't a valid NRO";
        return false;
    }
    if (!sdfile::write(dest, resp.body)) {
        err = "Couldn't write the update to the SD card";
        return false;
    }
    return true;
}

bool canRelaunch() { return !g_self.empty() && envHasNextLoad(); }

void relaunchOnExit() {
    if (canRelaunch()) envSetNextLoad(g_self.c_str(), g_self.c_str());
}

}  // namespace update
