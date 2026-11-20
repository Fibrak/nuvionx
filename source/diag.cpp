#include "diag.hpp"

#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>

#include "gfx.hpp"
#include "sdfile.hpp"

namespace diag {

static std::mutex g_mx;
static std::deque<std::string> g_lines;
static const size_t kMax = 800;

void log(const std::string& line) {
    char ts[24];
    snprintf(ts, sizeof ts, "[%9.3f] ", gfx::now());
    std::lock_guard<std::mutex> lk(g_mx);
    g_lines.push_back(ts + line);
    while (g_lines.size() > kMax) g_lines.pop_front();
}

void clear() {
    std::lock_guard<std::mutex> lk(g_mx);
    g_lines.clear();
}

void writeFile(const char* path) {
    std::string data;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        for (auto& l : g_lines) {
            data += l;
            data += '\n';
        }
    }
    sdfile::write(path, data);  // native FS API, not stdio (see sdfile.hpp)
}

}  // namespace diag
