#include "net.hpp"

#include <arpa/inet.h>
#include <curl/curl.h>
#include <netinet/in.h>
#include <switch.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

namespace net {

static const int kWorkers = 4;
static const char* kUserAgent = "NuvioNX/0.1 (Nintendo Switch; libcurl)";

static std::atomic<bool> g_abort{false};

static std::mutex g_qMutex;
static std::condition_variable g_qCv;
static std::deque<std::function<void()>> g_queue;
static std::vector<std::thread> g_threads;

static std::mutex g_mainMutex;
static std::deque<std::function<void()>> g_mainQueue;

static thread_local CURL* t_curl = nullptr;

bool aborting() { return g_abort.load(); }

static void workerLoop() {
    while (true) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lk(g_qMutex);
            g_qCv.wait(lk, [] { return g_abort.load() || !g_queue.empty(); });
            if (g_abort.load()) break;
            job = std::move(g_queue.front());
            g_queue.pop_front();
        }
        if (job) job();
    }
    threadCleanup();
}

void init() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    g_abort = false;
    for (int i = 0; i < kWorkers; i++) g_threads.emplace_back(workerLoop);
}

void beginShutdown() {
    g_abort = true;
    g_qCv.notify_all();
}

void shutdown() {
    beginShutdown();
    for (auto& t : g_threads)
        if (t.joinable()) t.join();
    g_threads.clear();
    {
        std::lock_guard<std::mutex> lk(g_qMutex);
        g_queue.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_mainMutex);
        g_mainQueue.clear();
    }
    threadCleanup();
    curl_global_cleanup();
}

void threadCleanup() {
    if (t_curl) {
        curl_easy_cleanup(t_curl);
        t_curl = nullptr;
    }
}

struct WriteCtx {
    std::string* buf;
    size_t cap;  // refuse to grow past this many bytes (protects RAM)
};

static size_t writeCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<WriteCtx*>(userdata);
    size_t n = size * nmemb;
    // Returning < n makes curl abort with CURLE_WRITE_ERROR ("Failed writing
    // received data to disk/application"), which is what we want when a response
    // is bigger than the caller is willing to buffer.
    if (ctx->buf->size() + n > ctx->cap) return 0;
    ctx->buf->append(ptr, n);
    return n;
}

static int progressCb(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return g_abort.load() ? 1 : 0;
}

Response get(const std::string& url, const std::vector<std::string>& headers, long timeoutSec, size_t maxBytes) {
    return request("GET", url, headers, "", timeoutSec, maxBytes);
}

static Response perform(const std::string& method, const std::string& url, const std::vector<std::string>& headers,
                        const std::string* body, curl_mime* (*makeMime)(CURL*, const void*), const void* mimeArg,
                        long timeoutSec, size_t maxBytes);

Response request(const std::string& method, const std::string& url, const std::vector<std::string>& headers,
                 const std::string& body, long timeoutSec, size_t maxBytes) {
    return perform(method, url, headers, method == "GET" ? nullptr : &body, nullptr, nullptr, timeoutSec, maxBytes);
}

static curl_mime* buildMime(CURL* h, const void* arg) {
    auto* fields = static_cast<const std::vector<std::pair<std::string, std::string>>*>(arg);
    curl_mime* mime = curl_mime_init(h);
    for (auto& f : *fields) {
        curl_mimepart* part = curl_mime_addpart(mime);
        curl_mime_name(part, f.first.c_str());
        curl_mime_data(part, f.second.c_str(), CURL_ZERO_TERMINATED);
    }
    return mime;
}

Response postMultipart(const std::string& url, const std::vector<std::string>& headers,
                       const std::vector<std::pair<std::string, std::string>>& fields, long timeoutSec) {
    return perform("POST", url, headers, nullptr, buildMime, &fields, timeoutSec, 24u * 1024u * 1024u);
}

static Response perform(const std::string& method, const std::string& url, const std::vector<std::string>& headers,
                        const std::string* body, curl_mime* (*makeMime)(CURL*, const void*), const void* mimeArg,
                        long timeoutSec, size_t maxBytes) {
    Response res;
    if (g_abort.load()) {
        res.error = "aborted";
        return res;
    }
    if (!t_curl) t_curl = curl_easy_init();
    if (!t_curl) {
        res.error = "curl init failed";
        return res;
    }
    CURL* h = t_curl;
    curl_easy_reset(h);
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 12L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, timeoutSec);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(h, CURLOPT_USERAGENT, kUserAgent);
    WriteCtx wctx{&res.body, maxBytes};
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &wctx);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, progressCb);
    // Don't keep idle connections around: with the libnx TLS backend every idle HTTPS
    // connection pins an SslContext in the system SSL service, which only allows a few
    // per process. Leftover contexts from browsing made the playback relay fail
    // instantly with "SSL connect error" (sslCreateContext refused).
    curl_easy_setopt(h, CURLOPT_FORBID_REUSE, 1L);
    curl_easy_setopt(h, CURLOPT_MAXCONNECTS, 1L);
    // devkitPro's libcurl uses the Switch's system SSL service (libnx backend), which verifies
    // against the console's built-in trusted CA store. Don't pass CAINFO: that backend can't
    // load a PEM bundle and every HTTPS request fails with "SSL connect error".
    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Accept: application/json, */*");
    for (auto& hd : headers) hdrs = curl_slist_append(hdrs, hd.c_str());
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);

    curl_mime* mime = nullptr;
    if (makeMime) {
        mime = makeMime(h, mimeArg);
        curl_easy_setopt(h, CURLOPT_MIMEPOST, mime);
    } else if (body) {
        if (method == "POST") {
            curl_easy_setopt(h, CURLOPT_POST, 1L);
        } else {
            curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, method.c_str());
        }
        if (method == "POST" || !body->empty()) {
            curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, (long)body->size());
            curl_easy_setopt(h, CURLOPT_COPYPOSTFIELDS, body->c_str());
        }
    }

    CURLcode rc = curl_easy_perform(h);
    if (mime) curl_mime_free(mime);
    if (rc != CURLE_OK) {
        res.error = curl_easy_strerror(rc);
        res.status = 0;
    } else {
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &res.status);
        if (!res.ok()) {
            char buf[48];
            snprintf(buf, sizeof buf, "HTTP %ld", res.status);
            res.error = buf;
        }
    }
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, nullptr);
    curl_slist_free_all(hdrs);
    return res;
}

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                    c == '_' || c == '.' || c == '*';
        if (keep) {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string urlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == '+') {
            out += ' ';
        } else if (c == '%' && i + 2 < s.size()) {
            auto hv = [](char x) -> int {
                if (x >= '0' && x <= '9') return x - '0';
                if (x >= 'a' && x <= 'f') return x - 'a' + 10;
                if (x >= 'A' && x <= 'F') return x - 'A' + 10;
                return -1;
            };
            int a = hv(s[i + 1]), b = hv(s[i + 2]);
            if (a >= 0 && b >= 0) {
                out += (char)(a * 16 + b);
                i += 2;
            } else {
                out += c;
            }
        } else {
            out += c;
        }
    }
    return out;
}

void runAsync(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> lk(g_qMutex);
        g_queue.push_back(std::move(fn));
    }
    g_qCv.notify_one();
}

void postMain(std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(g_mainMutex);
    g_mainQueue.push_back(std::move(fn));
}

int pumpMain(int maxTasks) {
    int ran = 0;
    for (int i = 0; i < maxTasks; i++) {
        std::function<void()> fn;
        {
            std::lock_guard<std::mutex> lk(g_mainMutex);
            if (g_mainQueue.empty()) return ran;
            fn = std::move(g_mainQueue.front());
            g_mainQueue.pop_front();
        }
        if (fn) {
            fn();
            ran++;
        }
    }
    return ran;
}

std::string localIp() {
    struct in_addr a;
    a.s_addr = (in_addr_t)gethostid();
    if (a.s_addr == 0 || a.s_addr == htonl(INADDR_LOOPBACK)) return "";
    return inet_ntoa(a);
}

}  // namespace net
