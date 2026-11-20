#include "lan.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include "net.hpp"

namespace lan {

static std::atomic<bool> g_run{false};
static std::thread g_thread;
static int g_listen = -1;
static int g_port = 8080;
static std::mutex g_mx;
static std::vector<std::string> g_submitted;

static const char* kPage =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>NuvioNX addons</title><style>"
    "body{margin:0;font-family:system-ui,sans-serif;background:#0d0d0f;color:#f5f5f7;"
    "display:flex;min-height:100vh;align-items:center;justify-content:center}"
    "form{width:min(560px,92vw);background:#1a1a1e;padding:28px;border-radius:18px}"
    "h1{margin:0 0 6px;font-size:22px}p{color:#b4b4bc;margin:0 0 18px;font-size:14px}"
    "input{width:100%;box-sizing:border-box;padding:14px;border-radius:12px;border:1px solid #333;"
    "background:#0d0d0f;color:#fff;font-size:15px}"
    "button{margin-top:14px;width:100%;padding:14px;border:0;border-radius:12px;background:#3b82f6;"
    "color:#fff;font-size:16px;font-weight:600}</style></head><body>"
    "<form method=post action=/add><h1>Add an addon to NuvioNX</h1>"
    "<p>Paste a Stremio-compatible addon manifest URL. It will be installed on your Switch.</p>"
    "<input name=url placeholder='https://.../manifest.json' autofocus required>"
    "<button>Send to Switch</button>%MSG%"
    "<p style='margin-top:18px'><a style='color:#93c5fd' href=/account>Nuvio account server settings</a></p>"
    "</form></body></html>";

static const char* kAccountPage =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>NuvioNX account</title><style>"
    "body{margin:0;font-family:system-ui,sans-serif;background:#0d0d0f;color:#f5f5f7;"
    "display:flex;min-height:100vh;align-items:center;justify-content:center}"
    "form{width:min(560px,92vw);background:#1a1a1e;padding:28px;border-radius:18px}"
    "h1{margin:0 0 6px;font-size:22px}p{color:#b4b4bc;margin:0 0 14px;font-size:14px}"
    "label{display:block;margin:12px 0 6px;font-size:13px;color:#b4b4bc}"
    "input{width:100%;box-sizing:border-box;padding:14px;border-radius:12px;border:1px solid #333;"
    "background:#0d0d0f;color:#fff;font-size:15px}"
    "button{margin-top:16px;width:100%;padding:14px;border:0;border-radius:12px;background:#3b82f6;"
    "color:#fff;font-size:16px;font-weight:600}</style></head><body>"
    "<form method=post action=/account><h1>NuvioNX accounts</h1>"
    "<p>Fill in either section (or both). Empty fields are left unchanged on the Switch.</p>"
    "<h1 style='font-size:17px;margin-top:18px'>Nuvio sync server</h1>"
    "<label>Server URL</label><input name=url placeholder='https://xxxx.supabase.co'>"
    "<label>Publishable key</label><input name=key>"
    "<h1 style='font-size:17px;margin-top:22px'>Trakt API app</h1>"
    "<p>Create one at trakt.tv/oauth/applications (redirect URI: urn:ietf:wg:oauth:2.0:oob). "
    "The Client Secret is optional - newer Trakt (PKCE) apps don't have one.</p>"
    "<label>Client ID</label><input name=trakt_id>"
    "<label>Client Secret (optional)</label><input name=trakt_secret>"
    "<button>Send to Switch</button>%MSG%</form></body></html>";

static bool g_haveConfig = false;
static std::string g_cfgUrl, g_cfgKey;
static bool g_haveTrakt = false;
static std::string g_traktId, g_traktSecret;
static bool g_haveImport = false;
static std::string g_import;

static void sendAll(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = send(fd, s.data() + off, s.size() - off, 0);
        if (n <= 0) return;
        off += (size_t)n;
    }
}

static std::string page(const std::string& msg, const char* tpl = kPage) {
    std::string p = tpl;
    size_t i = p.find("%MSG%");
    p.replace(i, 5, msg.empty() ? "" : "<p style='margin-top:16px;color:#6ee7b7'>" + msg + "</p>");
    return p;
}

static std::string formValue(const std::string& body, const std::string& key) {
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t amp = body.find('&', pos);
        std::string kv = body.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        size_t eq = kv.find('=');
        if (eq != std::string::npos && kv.substr(0, eq) == key) return net::urlDecode(kv.substr(eq + 1));
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return "";
}

static void handle(int fd) {
    std::string req;
    char buf[4096];
    size_t headerEnd = std::string::npos;
    size_t contentLen = 0;
    while (true) {
        struct pollfd pfd{fd, POLLIN, 0};
        if (poll(&pfd, 1, 4000) <= 0) break;
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n <= 0) break;
        req.append(buf, (size_t)n);
        if (req.size() > 16u * 1024u * 1024u) break;  // imports can be a few hundred KB
        if (headerEnd == std::string::npos) {
            headerEnd = req.find("\r\n\r\n");
            if (headerEnd != std::string::npos) {
                std::string lower;
                for (size_t k = 0; k < headerEnd; k++) lower += (char)tolower((unsigned char)req[k]);
                size_t cl = lower.find("content-length:");
                if (cl != std::string::npos) contentLen = (size_t)atoi(lower.c_str() + cl + 15);
            }
        }
        if (headerEnd != std::string::npos && req.size() >= headerEnd + 4 + contentLen) break;
    }
    if (headerEnd == std::string::npos) return;
    std::string first = req.substr(0, req.find("\r\n"));
    std::string body = req.substr(headerEnd + 4);
    std::string msg;
    std::string url;
    if (first.compare(0, 12, "POST /import") == 0) {
        bool complete = body.size() >= contentLen && !body.empty();
        if (complete) {
            std::lock_guard<std::mutex> lk(g_mx);
            g_import = body;
            g_haveImport = true;
        }
        std::string reply = complete ? "OK - import received, check your Switch" : "ERROR - incomplete upload";
        sendAll(fd, std::string("HTTP/1.1 ") + (complete ? "200 OK" : "400 Bad Request") +
                        "\r\nContent-Type: text/plain\r\nConnection: close\r\nContent-Length: " +
                        std::to_string(reply.size()) + "\r\n\r\n" + reply);
        return;
    }
    if (first.compare(0, 12, "GET /account") == 0 || first.compare(0, 13, "POST /account") == 0) {
        if (first[0] == 'P') {
            std::string u = formValue(body, "url"), k = formValue(body, "key");
            std::string ti = formValue(body, "trakt_id"), ts = formValue(body, "trakt_secret");
            std::lock_guard<std::mutex> lk(g_mx);
            if (!u.empty() && !k.empty()) {
                g_cfgUrl = u;
                g_cfgKey = k;
                g_haveConfig = true;
            }
            if (!ti.empty()) {  // Client ID is enough; secret is optional (PKCE apps have none)
                g_traktId = ti;
                g_traktSecret = ts;
                g_haveTrakt = true;
            }
            msg = (g_haveConfig || g_haveTrakt) ? "Saved! Go to the Account tab on your Switch."
                                                : "Nothing saved: enter the Nuvio URL + key, or a Trakt Client ID.";
        }
        std::string html = page(msg, kAccountPage);
        sendAll(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n"
                    "Content-Length: " + std::to_string(html.size()) + "\r\n\r\n" + html);
        return;
    }
    if (first.compare(0, 9, "POST /add") == 0) {
        url = formValue(body, "url");
    } else if (first.compare(0, 9, "GET /add?") == 0) {
        size_t sp = first.find(' ', 9);
        url = formValue(first.substr(9, sp == std::string::npos ? std::string::npos : sp - 9), "url");
    }
    if (!url.empty()) {
        {
            std::lock_guard<std::mutex> lk(g_mx);
            g_submitted.push_back(url);
        }
        msg = "Sent! Check your Switch.";
    }
    std::string html = page(msg);
    std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n"
                       "Content-Length: " + std::to_string(html.size()) + "\r\n\r\n" + html;
    sendAll(fd, resp);
}

static void loop() {
    while (g_run.load()) {
        struct pollfd pfd{g_listen, POLLIN, 0};
        int r = poll(&pfd, 1, 250);
        if (r <= 0) continue;
        int c = accept(g_listen, nullptr, nullptr);
        if (c < 0) continue;
        handle(c);
        close(c);
    }
}

bool start(int port) {
    if (g_run.load()) return true;
    g_port = port;
    g_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen < 0) return false;
    int one = 1;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(g_listen, (struct sockaddr*)&addr, sizeof addr) < 0 || listen(g_listen, 4) < 0) {
        close(g_listen);
        g_listen = -1;
        return false;
    }
    g_run = true;
    g_thread = std::thread(loop);
    return true;
}

void stop() {
    if (!g_run.load()) return;
    g_run = false;
    if (g_thread.joinable()) g_thread.join();
    if (g_listen >= 0) close(g_listen);
    g_listen = -1;
}

bool running() { return g_run.load(); }
int port() { return g_port; }

bool takeTraktConfig(std::string& clientId, std::string& clientSecret) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_haveTrakt) return false;
    g_haveTrakt = false;
    clientId = g_traktId;
    clientSecret = g_traktSecret;
    return true;
}

bool takeImport(std::string& body) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_haveImport) return false;
    g_haveImport = false;
    body.swap(g_import);
    g_import.clear();
    return true;
}

bool takeServerConfig(std::string& url, std::string& key) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_haveConfig) return false;
    g_haveConfig = false;
    url = g_cfgUrl;
    key = g_cfgKey;
    return true;
}

std::vector<std::string> takeSubmitted() {
    std::lock_guard<std::mutex> lk(g_mx);
    std::vector<std::string> out;
    out.swap(g_submitted);
    return out;
}

}  // namespace lan
