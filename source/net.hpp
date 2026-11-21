// HTTP (libcurl), worker thread pool and main-thread dispatch queue.
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace net {

struct Response {
    long status = 0;
    std::string body;
    std::string error;
    bool ok() const { return status >= 200 && status < 300; }
};

void init();
void beginShutdown();  // abort in-flight transfers (call before joining other threads)
void shutdown();

// Blocking GET. Safe to call from any worker thread (one curl handle per thread).
// maxBytes caps how much of the response is buffered in RAM; a larger response is
// aborted with CURLE_WRITE_ERROR. The self-updater raises it for the ~30 MB .nro.
Response get(const std::string& url, const std::vector<std::string>& headers = {}, long timeoutSec = 25,
             size_t maxBytes = 24u * 1024u * 1024u);
// Generic request: method "GET", "POST", "DELETE"...; body sent as-is.
Response request(const std::string& method, const std::string& url, const std::vector<std::string>& headers,
                 const std::string& body = "", long timeoutSec = 25, size_t maxBytes = 24u * 1024u * 1024u);
// multipart/form-data POST with simple text fields.
Response postMultipart(const std::string& url, const std::vector<std::string>& headers,
                       const std::vector<std::pair<std::string, std::string>>& fields, long timeoutSec = 30);
// Release the calling thread's cached curl handle.
void threadCleanup();
// True once shutdown has begun; long-running workers should bail out.
bool aborting();

std::string urlEncode(const std::string& s);  // like Java URLEncoder, but space -> %20
std::string urlDecode(const std::string& s);  // '+' -> space

// Run on the API worker pool.
void runAsync(std::function<void()> fn);
// Queue a function to run on the main thread (inside pumpMain).
void postMain(std::function<void()> fn);
int pumpMain(int maxTasks = 64);  // returns how many queued tasks ran

std::string localIp();

}  // namespace net
