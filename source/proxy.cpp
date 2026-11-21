#include "proxy.hpp"

#include <arpa/inet.h>
#include <curl/curl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "diag.hpp"

namespace proxy {

namespace {

struct Target {
    std::string url;
    std::vector<std::string> headers;
};

std::atomic<bool> g_run{false};
std::atomic<int> g_active{0};
std::thread g_thread;
int g_listen = -1;
int g_port = 0;
std::mutex g_mx;
std::map<std::string, Target> g_targets;
unsigned g_counter = 0;

const char* kUA = "Mozilla/5.0 (Nintendo Switch) NuvioNX/0.1";

// Pool of idle curl handles: reusing a handle keeps its HTTPS connection alive, so
// the player's follow-up range requests (probing, seeking) skip a new TLS handshake.
std::mutex g_poolMx;
std::vector<CURL*> g_pool;

CURL* acquireHandle() {
    {
        std::lock_guard<std::mutex> lk(g_poolMx);
        if (!g_pool.empty()) {
            CURL* h = g_pool.back();
            g_pool.pop_back();
            curl_easy_reset(h);  // clears options, keeps the connection cache
            return h;
        }
    }
    return curl_easy_init();
}

void releaseHandle(CURL* h, bool reusable) {
    if (!h) return;
    if (reusable && g_run.load()) {
        std::lock_guard<std::mutex> lk(g_poolMx);
        if (g_pool.size() < 2) {  // each pooled handle may pin one TLS context
            g_pool.push_back(h);
            return;
        }
    }
    curl_easy_cleanup(h);
}

bool sendAll(int fd, const char* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, data + off, len - off, 0);
        if (n <= 0) return false;
        off += (size_t)n;
    }
    return true;
}

std::string lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

const char* reason(long code) {
    switch (code) {
        case 200: return "OK";
        case 206: return "Partial Content";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 416: return "Range Not Satisfiable";
        default: return code >= 500 ? "Bad Gateway" : "Error";
    }
}

struct Ctx {
    int fd = -1;
    bool headOnly = false;
    bool headersSent = false;
    bool clientGone = false;
    long status = 0;
    std::string contentType, contentRange;
    long long contentLength = -1;
};

bool sendHeaders(Ctx& c) {
    if (c.headersSent) return true;
    c.headersSent = true;
    long code = c.status == 206 ? 206 : (c.status >= 200 && c.status < 300 ? 200 : (c.status ? c.status : 502));
    std::string h = "HTTP/1.1 " + std::to_string(code) + " " + reason(code) + "\r\n";
    h += "Content-Type: " + (c.contentType.empty() ? std::string("application/octet-stream") : c.contentType) + "\r\n";
    if (c.contentLength >= 0) h += "Content-Length: " + std::to_string(c.contentLength) + "\r\n";
    if (!c.contentRange.empty()) h += "Content-Range: " + c.contentRange + "\r\n";
    h += "Accept-Ranges: bytes\r\nConnection: close\r\n\r\n";
    if (!sendAll(c.fd, h.data(), h.size())) {
        c.clientGone = true;
        return false;
    }
    return true;
}

size_t headerCb(char* buf, size_t size, size_t n, void* ud) {
    Ctx& c = *static_cast<Ctx*>(ud);
    size_t len = size * n;
    std::string line(buf, len);
    if (line.compare(0, 5, "HTTP/") == 0) {
        // new response (redirects produce several); reset what we know
        c.status = 0;
        c.contentType.clear();
        c.contentRange.clear();
        c.contentLength = -1;
        size_t sp = line.find(' ');
        if (sp != std::string::npos) c.status = atol(line.c_str() + sp + 1);
        return len;
    }
    size_t colon = line.find(':');
    if (colon == std::string::npos) return len;
    std::string key = lower(trim(line.substr(0, colon)));
    std::string val = trim(line.substr(colon + 1));
    if (key == "content-type") c.contentType = val;
    else if (key == "content-length") c.contentLength = atoll(val.c_str());
    else if (key == "content-range") c.contentRange = val;
    return len;
}

size_t writeCb(char* buf, size_t size, size_t n, void* ud) {
    Ctx& c = *static_cast<Ctx*>(ud);
    size_t len = size * n;
    if (!sendHeaders(c)) return 0;
    if (!sendAll(c.fd, buf, len)) {
        c.clientGone = true;
        return 0;  // client (mpv) closed the connection, e.g. after a seek: abort the transfer
    }
    return len;
}

int progressCb(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) { return g_run.load() ? 0 : 1; }

// ---------------------------------------------------------------- parallel segmented download
// A progressive file (MP4/MKV) is one long sequential read over a single connection, so on a
// fast link the whole read-ahead burst lands on one relay worker (one core) doing TLS + copies.
// To spread that across cores we pull the byte range in fixed chunks over a couple of parallel
// connections and forward them to the player strictly in order. Only 2 connections so we stay
// well inside the 4 SSL sessions the console allows.
const long long kSegBytes = 2LL * 1024 * 1024;  // chunk size
const int kSegConns = 2;                         // parallel connections
const long long kSegWindow = 3 * kSegBytes;      // how far ahead of the send point we may fetch

struct FetchResult {
    std::string body;
    long status = 0;
    long long total = -1;  // full resource size, parsed from Content-Range
    std::string contentType;
};

// Captures status / content-range total / content-type while buffering the body in RAM.
struct FetchCtx {
    FetchResult* out;
    long long want;  // expected byte count (abort if the server over-sends)
};

size_t fetchHeaderCb(char* buf, size_t size, size_t n, void* ud) {
    FetchCtx& c = *static_cast<FetchCtx*>(ud);
    size_t len = size * n;
    std::string line(buf, len);
    if (line.compare(0, 5, "HTTP/") == 0) {
        c.out->status = 0;
        size_t sp = line.find(' ');
        if (sp != std::string::npos) c.out->status = atol(line.c_str() + sp + 1);
        return len;
    }
    size_t colon = line.find(':');
    if (colon == std::string::npos) return len;
    std::string key = lower(trim(line.substr(0, colon)));
    std::string val = trim(line.substr(colon + 1));
    if (key == "content-type") c.out->contentType = val;
    else if (key == "content-range") {
        size_t slash = val.find('/');  // "bytes START-END/TOTAL"
        if (slash != std::string::npos && val.find('*', slash) == std::string::npos)
            c.out->total = atoll(val.c_str() + slash + 1);
    }
    return len;
}

size_t fetchWriteCb(char* buf, size_t size, size_t n, void* ud) {
    FetchCtx& c = *static_cast<FetchCtx*>(ud);
    size_t len = size * n;
    c.out->body.append(buf, len);
    return len;
}

// Blocking ranged GET into RAM. Returns the curl result; fills `out`.
CURLcode fetchRange(const Target& t, long long off, long long len, FetchResult& out) {
    CURL* h = acquireHandle();
    FetchCtx fc{&out, len};
    struct curl_slist* hdrs = nullptr;
    bool haveUA = false;
    for (auto& x : t.headers) {
        if (lower(x).compare(0, 11, "user-agent:") == 0) haveUA = true;
        hdrs = curl_slist_append(hdrs, x.c_str());
    }
    char range[64];
    snprintf(range, sizeof range, "%lld-%lld", off, off + len - 1);
    curl_easy_setopt(h, CURLOPT_MAXCONNECTS, 1L);
    curl_easy_setopt(h, CURLOPT_URL, t.url.c_str());
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);
    if (!haveUA) curl_easy_setopt(h, CURLOPT_USERAGENT, kUA);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(h, CURLOPT_BUFFERSIZE, 256L * 1024L);
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, fetchHeaderCb);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, &fc);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, fetchWriteCb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &fc);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, progressCb);
    curl_easy_setopt(h, CURLOPT_RANGE, range);
    CURLcode rc = curl_easy_perform(h);
    for (int attempt = 1; attempt <= 3 && rc == CURLE_SSL_CONNECT_ERROR && g_run.load(); attempt++) {
        usleep(300000 * attempt);
        out.body.clear();
        rc = curl_easy_perform(h);
    }
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, nullptr);
    curl_slist_free_all(hdrs);
    releaseHandle(h, rc == CURLE_OK);
    return rc;
}

struct Chunk {
    std::string data;
    bool done = false;
    bool failed = false;
};

bool sendSegHeaders(int fd, long long start, long long total, const std::string& ctype) {
    std::string h = "HTTP/1.1 206 Partial Content\r\n";
    h += "Content-Type: " + (ctype.empty() ? std::string("application/octet-stream") : ctype) + "\r\n";
    h += "Content-Length: " + std::to_string(total - start) + "\r\n";
    h += "Content-Range: bytes " + std::to_string(start) + "-" + std::to_string(total - 1) + "/" +
         std::to_string(total) + "\r\n";
    h += "Accept-Ranges: bytes\r\nConnection: close\r\n\r\n";
    return sendAll(fd, h.data(), h.size());
}

// Returns true if it fully handled the request (parallel or small single-shot); false to fall
// back to the normal single-stream serve() path (e.g. the origin doesn't support ranges).
bool serveSegmented(int fd, const Target& t, long long startOff) {
    // First chunk synchronously: learn the total size, status and content-type.
    FetchResult first;
    CURLcode rc = fetchRange(t, startOff, kSegBytes, first);
    if (rc != CURLE_OK || first.status != 206 || first.total <= 0 || startOff >= first.total)
        return false;  // not range-capable (or bad range) -> let the single-stream path handle it

    long long total = first.total;
    // Whole remaining body already fit in the first chunk: just send it, no threads needed.
    if (startOff + (long long)first.body.size() >= total) {
        if (sendSegHeaders(fd, startOff, total, first.contentType)) sendAll(fd, first.body.data(), first.body.size());
        return true;
    }
    if (!sendSegHeaders(fd, startOff, total, first.contentType)) return true;  // client already gone
    if (!sendAll(fd, first.body.data(), first.body.size())) return true;
    long long sendOff = startOff + (long long)first.body.size();
    first.body.clear();
    first.body.shrink_to_fit();

    std::mutex m;
    std::condition_variable cv;
    long long assignOff = sendOff;
    std::map<long long, std::shared_ptr<Chunk>> chunks;
    bool abort = false;

    auto fetcher = [&]() {
        while (true) {
            long long off, len;
            {
                std::unique_lock<std::mutex> lk(m);
                // Wait (with a timeout so we always re-check g_run/abort on shutdown) until there
                // is room to fetch another chunk ahead of the send point.
                while (!(abort || !g_run.load() || assignOff >= total || (assignOff - sendOff) < kSegWindow))
                    cv.wait_for(lk, std::chrono::milliseconds(200));
                if (abort || !g_run.load() || assignOff >= total) return;
                off = assignOff;
                len = std::min(kSegBytes, total - off);
                assignOff += len;
                chunks[off] = std::make_shared<Chunk>();
            }
            FetchResult r;
            CURLcode frc = fetchRange(t, off, len, r);
            {
                std::lock_guard<std::mutex> lk(m);
                auto& c = chunks[off];
                if (frc == CURLE_OK && (r.status == 206 || r.status == 200) && (long long)r.body.size() == len) {
                    c->data = std::move(r.body);
                    c->done = true;
                } else {
                    c->failed = true;
                    c->done = true;
                    abort = true;
                }
                cv.notify_all();
            }
        }
    };

    std::vector<std::thread> fs;
    for (int i = 0; i < kSegConns; i++) fs.emplace_back(fetcher);

    while (g_run.load()) {
        std::shared_ptr<Chunk> c;
        {
            std::unique_lock<std::mutex> lk(m);
            if (sendOff >= total || abort) break;
            // Wait (with a timeout) until the chunk at the send point is fully fetched.
            while (!abort && g_run.load()) {
                auto it = chunks.find(sendOff);
                if (it != chunks.end() && it->second->done) {
                    c = it->second;
                    break;
                }
                cv.wait_for(lk, std::chrono::milliseconds(200));
            }
            if (abort || !g_run.load() || !c) break;
            if (c->failed) {
                abort = true;
                cv.notify_all();
                break;
            }
            chunks.erase(sendOff);
        }
        if (!sendAll(fd, c->data.data(), c->data.size())) {
            std::lock_guard<std::mutex> lk(m);
            abort = true;
            cv.notify_all();
            break;
        }
        {
            std::lock_guard<std::mutex> lk(m);
            sendOff += (long long)c->data.size();
            cv.notify_all();
        }
    }
    {
        std::lock_guard<std::mutex> lk(m);
        abort = true;
        cv.notify_all();
    }
    for (auto& th : fs)
        if (th.joinable()) th.join();
    return true;
}

void serve(int fd) {  // g_active was incremented by acceptLoop
    // read the request head
    std::string req;
    char buf[4096];
    while (req.find("\r\n\r\n") == std::string::npos && req.size() < 16384) {
        struct pollfd pfd{fd, POLLIN, 0};
        if (poll(&pfd, 1, 10000) <= 0) break;
        ssize_t r = recv(fd, buf, sizeof buf, 0);
        if (r <= 0) break;
        req.append(buf, (size_t)r);
    }
    size_t lineEnd = req.find("\r\n");
    std::string first = req.substr(0, lineEnd);
    bool head = first.compare(0, 5, "HEAD ") == 0;
    size_t p1 = first.find(' '), p2 = first.find(' ', p1 + 1);
    std::string path = (p1 != std::string::npos && p2 != std::string::npos) ? first.substr(p1 + 1, p2 - p1 - 1) : "";
    std::string range;
    {
        std::string lowerReq = lower(req);
        size_t r = lowerReq.find("\r\nrange:");
        if (r != std::string::npos) {
            size_t e = req.find("\r\n", r + 2);
            std::string v = trim(req.substr(r + 8, e - (r + 8)));
            if (lower(v).compare(0, 6, "bytes=") == 0) range = v.substr(6);
        }
    }

    Target t;
    bool found = false;
    if (path.compare(0, 3, "/s/") == 0) {
        std::lock_guard<std::mutex> lk(g_mx);
        std::string key = path.substr(3);
        size_t cut = key.find_first_of(".?");
        if (cut != std::string::npos) key = key.substr(0, cut);
        auto it = g_targets.find(key);
        if (it != g_targets.end()) {
            t = it->second;
            found = true;
        }
    }
    diag::log("[relay] " + std::string(head ? "HEAD " : "GET ") + path.substr(0, 40) +
              (range.empty() ? "" : " range=" + range) + (found ? "" : " (unknown token)"));
    if (!found) {
        const char* nf = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        sendAll(fd, nf, strlen(nf));
        close(fd);
        g_active--;
        return;
    }

    // Open-ended range (mpv's sequential playback read, "bytes=N-"): pull it over a couple of
    // parallel connections so a fast burst spreads across cores instead of pegging one. Bounded
    // ranges, HEAD and non-range requests keep the simple single-stream path below. Falls through
    // if the origin turns out not to support ranges.
    if (!head && range.size() >= 2 && range.back() == '-') {
        long long startOff = atoll(range.c_str());
        if (startOff >= 0 && serveSegmented(fd, t, startOff)) {
            shutdown(fd, SHUT_RDWR);
            close(fd);
            g_active--;
            return;
        }
    }

    CURL* h = acquireHandle();
    Ctx c;
    c.fd = fd;
    c.headOnly = head;
    struct curl_slist* hdrs = nullptr;
    bool haveUA = false;
    for (auto& x : t.headers) {
        if (lower(x).compare(0, 11, "user-agent:") == 0) haveUA = true;
        hdrs = curl_slist_append(hdrs, x.c_str());
    }
    char errbuf[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(h, CURLOPT_MAXCONNECTS, 1L);
    curl_easy_setopt(h, CURLOPT_URL, t.url.c_str());
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);
    if (!haveUA) curl_easy_setopt(h, CURLOPT_USERAGENT, kUA);
    // no transfer timeout (long videos); abort only if the link stalls for 60 s
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(h, CURLOPT_BUFFERSIZE, 256L * 1024L);
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, headerCb);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, &c);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &c);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, progressCb);
    if (!range.empty()) curl_easy_setopt(h, CURLOPT_RANGE, range.c_str());
    if (head) curl_easy_setopt(h, CURLOPT_NOBODY, 1L);

    CURLcode rc = curl_easy_perform(h);
    // The SSL service can transiently refuse a new TLS context while other transfers
    // are still releasing theirs: retry a few times before giving up (nothing has been
    // sent to the player yet in that case).
    for (int attempt = 1; attempt <= 4 && rc == CURLE_SSL_CONNECT_ERROR && !c.headersSent && !c.clientGone &&
                          g_run.load();
         attempt++) {
        diag::log("[relay] SSL connect error (" + std::string(errbuf) + "), retry " + std::to_string(attempt));
        usleep(400000 * attempt);
        errbuf[0] = 0;
        c.status = 0;
        rc = curl_easy_perform(h);
    }
    {
        char* eff = nullptr;
        curl_easy_getinfo(h, CURLINFO_EFFECTIVE_URL, &eff);
        std::string host = eff ? eff : "";
        size_t s = host.find("://");
        if (s != std::string::npos) host = host.substr(0, host.find('/', s + 3));  // host only, no tokens
        diag::log("[relay] upstream " + host + " status=" + std::to_string(c.status) +
                  " len=" + std::to_string(c.contentLength) + " type=" + c.contentType +
                  " curl=" + std::to_string((int)rc) +
                  (rc != CURLE_OK ? " (" + std::string(curl_easy_strerror(rc)) + (errbuf[0] ? ": " + std::string(errbuf) : "") + ")" : "") +
                  (c.clientGone ? " (player closed connection)" : ""));
    }
    if (!c.headersSent && !c.clientGone) {
        if (rc != CURLE_OK && c.status == 0) c.status = 502;
        if (!head || rc != CURLE_OK) c.contentLength = head ? c.contentLength : 0;
        sendHeaders(c);
    }
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, nullptr);
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, nullptr);  // errbuf is about to go out of scope
    curl_slist_free_all(hdrs);
    // A transfer we aborted mid-body leaves its connection unusable; only pool clean ones.
    releaseHandle(h, rc == CURLE_OK);
    shutdown(fd, SHUT_RDWR);
    close(fd);
    g_active--;
}

// Fixed worker pool. libnx does NOT support detached threads
// (__syscall_thread_detach is unimplemented): detaching one thread per connection
// leaked a thread every request/seek until libnx aborted with 2345-0008
// (LibnxError_NotInitialized in threadExit). Workers are created once and joined.
const int kWorkers = 6;
std::vector<std::thread> g_workers;
std::mutex g_qMx;
std::condition_variable g_qCv;
std::deque<int> g_queue;

void workerLoop() {
    while (true) {
        int fd = -1;
        {
            std::unique_lock<std::mutex> lk(g_qMx);
            g_qCv.wait(lk, [] { return !g_run.load() || !g_queue.empty(); });
            if (!g_run.load()) break;
            fd = g_queue.front();
            g_queue.pop_front();
        }
        g_active++;
        serve(fd);  // decrements g_active
    }
}

void acceptLoop() {
    while (g_run.load()) {
        struct pollfd pfd{g_listen, POLLIN, 0};
        if (poll(&pfd, 1, 250) <= 0) continue;
        int c = accept(g_listen, nullptr, nullptr);
        if (c < 0) continue;
        std::lock_guard<std::mutex> lk(g_qMx);
        if (g_queue.size() >= 8) {
            // backlog full: refuse instead of piling up sockets
            const char* busy = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            sendAll(c, busy, strlen(busy));
            close(c);
            diag::log("[relay] rejected connection: queue full");
            continue;
        }
        g_queue.push_back(c);
        g_qCv.notify_one();
    }
}

}  // namespace

bool start() {
    if (g_run.load()) return true;
    g_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen < 0) return false;
    int one = 1;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    // try a few fixed ports (port 0 auto-assignment isn't reliable on every firmware)
    for (int port = 23470; port < 23490; port++) {
        addr.sin_port = htons((uint16_t)port);
        if (bind(g_listen, (struct sockaddr*)&addr, sizeof addr) == 0) {
            g_port = port;
            break;
        }
    }
    if (g_port == 0 || listen(g_listen, 8) < 0) {
        close(g_listen);
        g_listen = -1;
        g_port = 0;
        return false;
    }
    g_run = true;
    for (int i = 0; i < kWorkers; i++) g_workers.emplace_back(workerLoop);
    g_thread = std::thread(acceptLoop);
    return true;
}

void stop() {
    if (!g_run.load()) return;
    g_run = false;  // in-flight transfers abort via progressCb
    g_qCv.notify_all();
    if (g_thread.joinable()) g_thread.join();
    for (auto& w : g_workers)
        if (w.joinable()) w.join();
    g_workers.clear();
    {
        std::lock_guard<std::mutex> lk(g_qMx);
        for (int fd : g_queue) close(fd);
        g_queue.clear();
    }
    if (g_listen >= 0) close(g_listen);
    g_listen = -1;
    g_port = 0;
    std::lock_guard<std::mutex> lk(g_poolMx);
    for (CURL* h : g_pool) curl_easy_cleanup(h);
    g_pool.clear();
}

int activeTransfers() { return g_active.load(); }

std::string wrap(const std::string& url, const std::vector<std::string>& headers) {
    if (url.compare(0, 8, "https://") != 0) return url;
    if (!g_run.load() && !start()) return url;
    std::lock_guard<std::mutex> lk(g_mx);
    char tok[32];
    unsigned id = ++g_counter;
    snprintf(tok, sizeof tok, "%u-%08x", id, id * 2654435761u);
    g_targets[tok] = Target{url, headers};
    if (g_targets.size() > 128) g_targets.erase(g_targets.begin());
    // keep the original file extension visible: helps mpv/FFmpeg pick a demuxer
    std::string ext;
    size_t q = url.find('?');
    std::string pathPart = url.substr(0, q);
    size_t slash = pathPart.find_last_of('/'), dot = pathPart.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash) && pathPart.size() - dot <= 6)
        ext = pathPart.substr(dot);
    return "http://127.0.0.1:" + std::to_string(g_port) + "/s/" + tok + ext;
}

}  // namespace proxy
