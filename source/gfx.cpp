#include "gfx.hpp"

#include <SDL_image.h>
#include <SDL_ttf.h>
#include <switch.h>

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "net.hpp"
#include "sdfile.hpp"
#include "store.hpp"

namespace gfx {

SDL_Renderer* R = nullptr;
float S = 1.5f;

// ---------------------------------------------------------------- themes
// Dark palettes modelled on NuvioTV's built-in themes.
static const Theme kThemes[] = {
    {"Ocean", rgba(0x0D0D0F), rgba(0x1A1A1E), rgba(0x1A1F24), rgba(0x232329), rgba(0x3B82F6), rgba(0x1D4ED8),
     rgba(0x93C5FD), rgba(0x1A2D3D), rgba(0xF5F5F7), rgba(0xB4B4BC), rgba(0x7C7C86)},
    {"Crimson", rgba(0x0F0D0D), rgba(0x1E1A1A), rgba(0x241A1A), rgba(0x2A2222), rgba(0xEF4444), rgba(0xDC2626),
     rgba(0xFCA5A5), rgba(0x3D1A1A), rgba(0xF5F5F7), rgba(0xB4B4BC), rgba(0x7C7C86)},
    {"Violet", rgba(0x0D0D0F), rgba(0x1A1A1E), rgba(0x1F1A24), rgba(0x26222C), rgba(0x8B5CF6), rgba(0x6D28D9),
     rgba(0xC4B5FD), rgba(0x2D1A3D), rgba(0xF5F5F7), rgba(0xB4B4BC), rgba(0x7C7C86)},
    {"Emerald", rgba(0x0D0F0D), rgba(0x1A1E1A), rgba(0x1A241A), rgba(0x222A22), rgba(0x10B981), rgba(0x047857),
     rgba(0x6EE7B7), rgba(0x1A3D1E), rgba(0xF5F5F7), rgba(0xB4B4BC), rgba(0x7C7C86)},
    {"Amber", rgba(0x0F0D0D), rgba(0x1E1A1A), rgba(0x24201A), rgba(0x2A2622), rgba(0xF59E0B), rgba(0xB45309),
     rgba(0xFCD34D), rgba(0x3D2D1A), rgba(0xF5F5F7), rgba(0xB4B4BC), rgba(0x7C7C86)},
    {"Graphite", rgba(0x0B0B0C), rgba(0x18181A), rgba(0x1E1E21), rgba(0x262629), rgba(0xE5E5E5), rgba(0xA3A3A3),
     rgba(0xFFFFFF), rgba(0x303030), rgba(0xF5F5F7), rgba(0xB4B4BC), rgba(0x7C7C86)},
};

int themeCount() { return (int)(sizeof(kThemes) / sizeof(kThemes[0])); }
const Theme& themeAt(int i) { return kThemes[(i % themeCount() + themeCount()) % themeCount()]; }
const Theme& theme() { return themeAt(store::settings().theme); }

// ---------------------------------------------------------------- timing
static std::atomic<double> g_now{0};
static Uint64 g_freq = 1, g_start = 0;

double now() { return g_now.load(); }

// ---------------------------------------------------------------- fonts / text
static void* g_fontData = nullptr;
static size_t g_fontSize = 0;
static std::map<int, TTF_Font*> g_fonts;

struct TextEntry {
    SDL_Texture* tex = nullptr;
    int w = 0, h = 0;
    double lastUse = 0;
};
static std::unordered_map<std::string, TextEntry> g_text;

static int px(int size) { return std::max(6, (int)std::lround(size * S)); }

static TTF_Font* font(int size, bool bold) {
    int p = px(size);
    int key = p * 2 + (bold ? 1 : 0);
    auto it = g_fonts.find(key);
    if (it != g_fonts.end()) return it->second;
    SDL_RWops* rw = SDL_RWFromConstMem(g_fontData, (int)g_fontSize);
    TTF_Font* f = rw ? TTF_OpenFontRW(rw, 1, p) : nullptr;
    if (f && bold) TTF_SetFontStyle(f, TTF_STYLE_BOLD);
    g_fonts[key] = f;
    return f;
}

std::string fontFamily() {
    TTF_Font* f = font(20, false);
    const char* fam = f ? TTF_FontFaceFamilyName(f) : nullptr;
    return fam ? fam : "";
}

const void* fontData(size_t* size) {
    if (size) *size = g_fontSize;
    return g_fontData;
}

float lineHeight(int size) {
    TTF_Font* f = font(size, false);
    return f ? TTF_FontHeight(f) / S : size * 1.2f;
}

float textWidth(const std::string& s, int size, bool bold) {
    if (s.empty()) return 0;
    TTF_Font* f = font(size, bold);
    int w = 0, h = 0;
    if (!f || TTF_SizeUTF8(f, s.c_str(), &w, &h) != 0) return 0;
    return w / S;
}

static size_t prevCodepoint(const std::string& s, size_t i) {
    if (i == 0) return 0;
    i--;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    return i;
}

static std::string ellipsize(const std::string& s, int size, bool bold, float maxW) {
    if (textWidth(s, size, bold) <= maxW) return s;
    const std::string dots = "...";
    // binary search on byte length, snapped to codepoint starts
    size_t lo = 0, hi = s.size();
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        size_t cut = mid;
        while (cut > 0 && cut < s.size() && ((unsigned char)s[cut] & 0xC0) == 0x80) cut--;
        if (textWidth(s.substr(0, cut) + dots, size, bold) <= maxW)
            lo = mid;
        else
            hi = mid - 1;
    }
    size_t cut = lo;
    while (cut > 0 && cut < s.size() && ((unsigned char)s[cut] & 0xC0) == 0x80) cut--;
    std::string out = s.substr(0, cut);
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out + dots;
}

static TextEntry* textTex(const std::string& s, int size, bool bold) {
    std::string key = s;
    key += '\x1f';
    key += std::to_string(px(size));
    key += bold ? 'b' : 'r';
    auto it = g_text.find(key);
    if (it != g_text.end()) {
        it->second.lastUse = now();
        return &it->second;
    }
    TTF_Font* f = font(size, bold);
    if (!f) return nullptr;
    SDL_Surface* surf = TTF_RenderUTF8_Blended(f, s.c_str(), SDL_Color{255, 255, 255, 255});
    if (!surf) return nullptr;
    TextEntry e;
    e.tex = SDL_CreateTextureFromSurface(R, surf);
    e.w = surf->w;
    e.h = surf->h;
    e.lastUse = now();
    SDL_FreeSurface(surf);
    if (!e.tex) return nullptr;
    SDL_SetTextureBlendMode(e.tex, SDL_BLENDMODE_BLEND);
    auto res = g_text.emplace(key, e);
    return &res.first->second;
}

float text(const std::string& s0, float x, float y, int size, Col c, bool bold, Align al, float maxW) {
    if (s0.empty() || c.a == 0) return 0;
    std::string s = maxW > 0 ? ellipsize(s0, size, bold, maxW) : s0;
    TextEntry* e = textTex(s, size, bold);
    if (!e) return 0;
    float w = e->w / S;
    float dx = x;
    if (al == Center) dx = x - w / 2;
    else if (al == Right) dx = x - w;
    SDL_SetTextureColorMod(e->tex, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(e->tex, c.a);
    SDL_FRect dst{dx * S, y * S, (float)e->w, (float)e->h};
    SDL_RenderCopyF(R, e->tex, nullptr, &dst);
    return w;
}

std::vector<std::string> wrap(const std::string& s, int size, float maxW, int maxLines, bool bold) {
    std::vector<std::string> lines;
    if (s.empty()) return lines;
    // tokenise into words, keeping explicit newlines
    std::vector<std::string> words;
    std::string cur;
    for (char ch : s) {
        if (ch == ' ' || ch == '\t' || ch == '\r') {
            if (!cur.empty()) words.push_back(cur), cur.clear();
        } else if (ch == '\n') {
            if (!cur.empty()) words.push_back(cur), cur.clear();
            words.push_back("\n");
        } else {
            cur += ch;
        }
    }
    if (!cur.empty()) words.push_back(cur);

    std::string line;
    size_t i = 0;
    bool truncated = false;
    while (i < words.size()) {
        const std::string& w = words[i];
        if (w == "\n") {
            lines.push_back(line);
            line.clear();
            i++;
        } else {
            std::string cand = line.empty() ? w : line + " " + w;
            if (textWidth(cand, size, bold) <= maxW) {
                line = cand;
                i++;
            } else if (line.empty()) {
                // single word wider than the line: hard break
                std::string part;
                size_t k = 0;
                while (k < w.size()) {
                    size_t n = k + 1;
                    while (n < w.size() && ((unsigned char)w[n] & 0xC0) == 0x80) n++;
                    if (textWidth(part + w.substr(k, n - k), size, bold) > maxW && !part.empty()) break;
                    part += w.substr(k, n - k);
                    k = n;
                }
                lines.push_back(part);
                words[i] = w.substr(k);
                if (words[i].empty()) i++;
            } else {
                lines.push_back(line);
                line.clear();
            }
        }
        if (maxLines > 0 && (int)lines.size() >= maxLines) {
            truncated = i < words.size() || !line.empty();
            break;
        }
    }
    if (!line.empty() && (maxLines <= 0 || (int)lines.size() < maxLines)) lines.push_back(line);
    if (truncated && !lines.empty()) {
        std::string rest = lines.back();
        for (size_t k = i; k < words.size(); k++) rest += (words[k] == "\n" ? " " : " " + words[k]);
        lines.back() = ellipsize(rest, size, bold, maxW);
    }
    (void)prevCodepoint;
    return lines;
}

float paragraph(const std::string& s, float x, float y, float maxW, int size, Col c, int maxLines, float lineGap,
                bool bold) {
    auto lines = wrap(s, size, maxW, maxLines, bold);
    float lh = size * lineGap;
    for (size_t i = 0; i < lines.size(); i++) text(lines[i], x, y + i * lh, size, c, bold);
    return lines.size() * lh;
}

// ---------------------------------------------------------------- shapes
static std::vector<SDL_Vertex> g_verts;
static std::vector<int> g_idx;

static SDL_Vertex vtx(float x, float y, Col c, float u = 0, float v = 0) {
    SDL_Vertex out;
    out.position.x = x * S;
    out.position.y = y * S;
    out.color = SDL_Color{c.r, c.g, c.b, c.a};
    out.tex_coord.x = u;
    out.tex_coord.y = v;
    return out;
}

static void roundPoly(float x, float y, float w, float h, float r, std::vector<SDL_FPoint>& pts) {
    pts.clear();
    r = std::max(0.f, std::min(r, std::min(w, h) / 2));
    const int seg = 6;
    struct C {
        float cx, cy, a0;
    } cs[4] = {{x + w - r, y + r, -90.f}, {x + w - r, y + h - r, 0.f}, {x + r, y + h - r, 90.f}, {x + r, y + r, 180.f}};
    for (auto& c : cs) {
        for (int i = 0; i <= seg; i++) {
            float a = (c.a0 + 90.f * i / seg) * (float)M_PI / 180.f;
            pts.push_back(SDL_FPoint{c.cx + r * cosf(a), c.cy + r * sinf(a)});
        }
    }
}

void fillRect(float x, float y, float w, float h, Col c) {
    if (c.a == 0 || w <= 0 || h <= 0) return;
    SDL_SetRenderDrawColor(R, c.r, c.g, c.b, c.a);
    SDL_FRect rc{x * S, y * S, w * S, h * S};
    SDL_RenderFillRectF(R, &rc);
}

// Rounded rect as a 9-slice: the centre and four edges are axis-aligned quads (drawn
// exactly like a plain textured quad, so they don't shimmer during motion), and only
// the four small corners are triangle fans. The old version fanned the WHOLE rect from
// its centre, whose many diagonal internal edges shimmered while scrolling.
static void texturedPoly(SDL_Texture* t, float x, float y, float w, float h, float r, Col c, float u0, float v0,
                         float uw, float vh) {
    r = std::max(0.f, std::min(r, std::min(w, h) / 2));
    g_verts.clear();
    g_idx.clear();
    auto V = [&](float px, float py) -> int {
        g_verts.push_back(vtx(px, py, c, u0 + (px - x) / w * uw, v0 + (py - y) / h * vh));
        return (int)g_verts.size() - 1;
    };
    auto quad = [&](float x0, float y0, float x1, float y1) {
        int a = V(x0, y0), b = V(x1, y0), cc = V(x1, y1), d = V(x0, y1);
        g_idx.insert(g_idx.end(), {a, b, cc, a, cc, d});
    };
    quad(x + r, y + r, x + w - r, y + h - r);  // centre
    quad(x + r, y, x + w - r, y + r);          // top edge
    quad(x + r, y + h - r, x + w - r, y + h);  // bottom edge
    quad(x, y + r, x + r, y + h - r);          // left edge
    quad(x + w - r, y + r, x + w, y + h - r);  // right edge
    if (r > 0.5f) {
        const int seg = 6;
        struct Corner {
            float cx, cy, a0;
        } cs[4] = {{x + r, y + r, 180.f}, {x + w - r, y + r, 270.f}, {x + w - r, y + h - r, 0.f}, {x + r, y + h - r, 90.f}};
        for (auto& k : cs) {
            int center = V(k.cx, k.cy);
            int prev = -1;
            for (int i = 0; i <= seg; i++) {
                float a = (k.a0 + 90.f * i / seg) * (float)M_PI / 180.f;
                int p = V(k.cx + r * cosf(a), k.cy + r * sinf(a));
                if (prev >= 0) g_idx.insert(g_idx.end(), {center, prev, p});
                prev = p;
            }
        }
    }
    SDL_RenderGeometry(R, t, g_verts.data(), (int)g_verts.size(), g_idx.data(), (int)g_idx.size());
}

void roundRect(float x, float y, float w, float h, float rad, Col c) {
    if (c.a == 0 || w <= 0 || h <= 0) return;
    if (rad <= 0.5f) return fillRect(x, y, w, h, c);
    texturedPoly(nullptr, x, y, w, h, rad, c, 0, 0, 1, 1);
}

void cornerCutouts(float x, float y, float w, float h, float r, Col col) {
    if (col.a == 0) return;
    r = std::max(0.f, std::min(r, std::min(w, h) / 2));
    if (r <= 0.5f) return;
    g_verts.clear();
    g_idx.clear();
    auto V = [&](float px, float py) -> int {
        g_verts.push_back(vtx(px, py, col));
        return (int)g_verts.size() - 1;
    };
    // outer square corner + arc centre + start angle for each corner
    struct Corner {
        float ox, oy, cx, cy, a0;
    } cs[4] = {{x, y, x + r, y + r, 180.f},
               {x + w, y, x + w - r, y + r, 270.f},
               {x + w, y + h, x + w - r, y + h - r, 0.f},
               {x, y + h, x + r, y + h - r, 90.f}};
    const int seg = 6;
    for (auto& k : cs) {
        int o = V(k.ox, k.oy);
        int prev = -1;
        for (int i = 0; i <= seg; i++) {
            float a = (k.a0 + 90.f * i / seg) * (float)M_PI / 180.f;
            int p = V(k.cx + r * cosf(a), k.cy + r * sinf(a));
            if (prev >= 0) g_idx.insert(g_idx.end(), {o, prev, p});
            prev = p;
        }
    }
    SDL_RenderGeometry(R, nullptr, g_verts.data(), (int)g_verts.size(), g_idx.data(), (int)g_idx.size());
}

void roundOutline(float x, float y, float w, float h, float rad, float thick, Col c) {
    if (c.a == 0) return;
    static std::vector<SDL_FPoint> outer, inner;
    roundPoly(x, y, w, h, rad, outer);
    roundPoly(x + thick, y + thick, w - 2 * thick, h - 2 * thick, std::max(0.f, rad - thick), inner);
    g_verts.clear();
    g_idx.clear();
    int n = (int)outer.size();
    for (int i = 0; i < n; i++) {
        g_verts.push_back(vtx(outer[i].x, outer[i].y, c));
        g_verts.push_back(vtx(inner[i].x, inner[i].y, c));
    }
    for (int i = 0; i < n; i++) {
        int a = 2 * i, b = 2 * i + 1, a2 = 2 * ((i + 1) % n), b2 = a2 + 1;
        g_idx.insert(g_idx.end(), {a, b, a2, b, b2, a2});
    }
    SDL_RenderGeometry(R, nullptr, g_verts.data(), (int)g_verts.size(), g_idx.data(), (int)g_idx.size());
}

static void quad(float x, float y, float w, float h, Col tl, Col tr, Col br, Col bl) {
    SDL_Vertex v[4] = {vtx(x, y, tl), vtx(x + w, y, tr), vtx(x + w, y + h, br), vtx(x, y + h, bl)};
    int idx[6] = {0, 1, 2, 0, 2, 3};
    SDL_RenderGeometry(R, nullptr, v, 4, idx, 6);
}

void vGradient(float x, float y, float w, float h, Col top, Col bottom) { quad(x, y, w, h, top, top, bottom, bottom); }
void hGradient(float x, float y, float w, float h, Col l, Col r) { quad(x, y, w, h, l, r, r, l); }

void circle(float cx, float cy, float r, Col c) { roundRect(cx - r, cy - r, 2 * r, 2 * r, r, c); }

void spinner(float cx, float cy, float r, Col c) {
    const int n = 10;
    double t = now();
    int head = (int)(t * 12) % n;
    for (int i = 0; i < n; i++) {
        float a = (float)(2 * M_PI * i / n - M_PI / 2);
        int age = (head - i + n) % n;
        Uint8 al = (Uint8)(c.a * (1.0f - age / (float)n));
        circle(cx + r * cosf(a), cy + r * sinf(a), r * 0.18f, withA(c, al));
    }
}

// ---------------------------------------------------------------- clip
static std::vector<SDL_Rect> g_clips;

void pushClip(float x, float y, float w, float h) {
    SDL_Rect r{(int)std::floor(x * S), (int)std::floor(y * S), (int)std::ceil(w * S), (int)std::ceil(h * S)};
    if (!g_clips.empty()) {
        SDL_Rect out;
        if (!SDL_IntersectRect(&r, &g_clips.back(), &out)) out = SDL_Rect{0, 0, 0, 0};
        r = out;
    }
    g_clips.push_back(r);
    SDL_RenderSetClipRect(R, &r);
}

void popClip() {
    if (!g_clips.empty()) g_clips.pop_back();
    SDL_RenderSetClipRect(R, g_clips.empty() ? nullptr : &g_clips.back());
}

// ---------------------------------------------------------------- images
enum ImgState { Queued, Loading, Decoded, Ready, Failed };
struct Img {
    ImgState st = Queued;
    SDL_Texture* tex = nullptr;
    SDL_Surface* surf = nullptr;
    int maxW = 0, maxH = 0;
    double lastUse = 0;
    double readyAt = 0;  // for fade-in
    size_t bytes = 0;
};

static std::mutex g_imgMutex;
static std::condition_variable g_imgCv;
static std::unordered_map<std::string, Img> g_imgs;
static std::deque<std::string> g_imgStack;  // LIFO: newest requests first
static std::unordered_set<std::string> g_shownOnce;  // URLs that have faded in before (no re-fade)
static std::vector<std::thread> g_imgThreads;
static std::atomic<bool> g_imgStop{false};
// GPU cache budget: keep plenty of posters around so scrolling back never reloads.
static const size_t kMaxTextures = 600;
static size_t kMaxTextureBytes = 128u * 1024u * 1024u;  // set from available memory at startup

void setImageBudget(size_t bytes) { kMaxTextureBytes = bytes; }

// ---- SD card image cache (survives restarts, avoids re-downloading)
static const char* kImgCacheDir = "sdmc:/switch/NuvioNX/imgcache";

static std::string cachePath(const std::string& url) {
    // FNV-1a 64-bit -> 16 hex chars
    unsigned long long h = 1469598103934665603ULL;
    for (unsigned char c : url) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    char buf[24];
    snprintf(buf, sizeof buf, "%016llx", h);
    return std::string(kImgCacheDir) + "/" + buf;
}

static bool readCache(const std::string& url, std::string& out) {
    return sdfile::read(cachePath(url), out) && !out.empty();
}

static void writeCache(const std::string& url, const std::string& data) {
    static std::once_flag dirOnce;
    std::call_once(dirOnce, [] { sdfile::mkdirs(kImgCacheDir); });
    sdfile::write(cachePath(url), data);
}

// Wipe the disk cache when it grows too large (checked once at startup).
static void trimDiskCache() {
    DIR* d = opendir(kImgCacheDir);  // created lazily, only if the disk cache is enabled
    if (!d) return;
    std::vector<std::string> files;
    while (struct dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        files.push_back(std::string(kImgCacheDir) + "/" + e->d_name);
    }
    closedir(d);
    if (files.size() > 4000)
        for (auto& f : files) sdfile::remove(f);
}

static SDL_Surface* boxDownscale(SDL_Surface* src, int dw, int dh) {
    SDL_Surface* dst = SDL_CreateRGBSurfaceWithFormat(0, dw, dh, 32, SDL_PIXELFORMAT_RGBA32);
    if (!dst) return nullptr;
    const int sw = src->w, sh = src->h;
    for (int dy = 0; dy < dh; dy++) {
        int y0 = (int)((long long)dy * sh / dh);
        int y1 = std::max(y0 + 1, (int)((long long)(dy + 1) * sh / dh));
        Uint8* drow = (Uint8*)dst->pixels + dy * dst->pitch;
        for (int dx = 0; dx < dw; dx++) {
            int x0 = (int)((long long)dx * sw / dw);
            int x1 = std::max(x0 + 1, (int)((long long)(dx + 1) * sw / dw));
            unsigned acc[4] = {0, 0, 0, 0};
            for (int y = y0; y < y1; y++) {
                const Uint8* p = (const Uint8*)src->pixels + y * src->pitch + x0 * 4;
                for (int x = x0; x < x1; x++, p += 4) {
                    acc[0] += p[0];
                    acc[1] += p[1];
                    acc[2] += p[2];
                    acc[3] += p[3];
                }
            }
            unsigned n = (unsigned)((y1 - y0) * (x1 - x0));
            for (int k = 0; k < 4; k++) drow[dx * 4 + k] = (Uint8)(acc[k] / n);
        }
    }
    return dst;
}

static SDL_Surface* decode(const std::string& data, int maxW, int maxH) {
    SDL_RWops* rw = SDL_RWFromConstMem(data.data(), (int)data.size());
    if (!rw) return nullptr;
    SDL_Surface* s = IMG_Load_RW(rw, 1);
    if (!s) return nullptr;
    SDL_Surface* rgba = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(s);
    if (!rgba) return nullptr;
    if (rgba->w > maxW || rgba->h > maxH) {
        float k = std::min(maxW / (float)rgba->w, maxH / (float)rgba->h);
        int dw = std::max(1, (int)(rgba->w * k)), dh = std::max(1, (int)(rgba->h * k));
        SDL_LockSurface(rgba);
        SDL_Surface* small = boxDownscale(rgba, dw, dh);
        SDL_UnlockSurface(rgba);
        if (small) {
            SDL_FreeSurface(rgba);
            rgba = small;
        }
    }
    return rgba;
}

static void imageWorker() {
    while (true) {
        std::string url;
        int maxW = 0, maxH = 0;
        {
            std::unique_lock<std::mutex> lk(g_imgMutex);
            g_imgCv.wait(lk, [] { return g_imgStop.load() || !g_imgStack.empty(); });
            if (g_imgStop.load()) break;
            url = g_imgStack.back();
            g_imgStack.pop_back();
            auto it = g_imgs.find(url);
            if (it == g_imgs.end() || it->second.st != Queued) continue;
            if (now() - it->second.lastUse > 1.5) {
                // scrolled away before we got to it; forget so it can be re-requested
                g_imgs.erase(it);
                continue;
            }
            it->second.st = Loading;
            maxW = it->second.maxW;
            maxH = it->second.maxH;
        }
        SDL_Surface* surf = nullptr;
        std::string data;
        bool useDisk = store::settings().diskImageCache;
        if (useDisk && readCache(url, data)) surf = decode(data, maxW, maxH);
        if (!surf) {
            auto res = net::get(url, {}, 20);
            if (res.ok()) {
                surf = decode(res.body, maxW, maxH);
                if (surf && useDisk) writeCache(url, res.body);
            }
        }
        std::lock_guard<std::mutex> lk(g_imgMutex);
        auto it = g_imgs.find(url);
        if (it == g_imgs.end()) {
            if (surf) SDL_FreeSurface(surf);
            continue;
        }
        if (surf) {
            it->second.surf = surf;
            it->second.st = Decoded;
        } else {
            it->second.st = Failed;
        }
    }
    net::threadCleanup();
}

static int g_uploadsThisFrame = 0;
static int g_uploadsLastFrame = 0;
static int g_missThisFrame = 0;   // cards that fell back to placeholder (image() returned null)
static int g_missLastFrame = 0;
static int g_evictTotal = 0;      // cumulative evictions (overlay shows the rate)

void imageStats(int& ready, int& total, double& megabytes, int& pending, int& uploadsLastFrame) {
    std::lock_guard<std::mutex> lk(g_imgMutex);
    ready = 0;
    pending = 0;
    size_t bytes = 0;
    total = (int)g_imgs.size();
    for (auto& kv : g_imgs) {
        if (kv.second.st == Ready) {
            ready++;
            bytes += kv.second.bytes;
        } else if (kv.second.st != Failed) {
            pending++;
        }
    }
    megabytes = bytes / 1048576.0;
    uploadsLastFrame = g_uploadsLastFrame;
}

static bool g_debugFlat = false;
void setDebugFlat(bool on) { g_debugFlat = on; }
bool debugFlat() { return g_debugFlat; }

void imageStats2(int& missLastFrame, int& evictTotal) {
    std::lock_guard<std::mutex> lk(g_imgMutex);
    missLastFrame = g_missLastFrame;
    evictTotal = g_evictTotal;
}

void releaseImageCache() {
    std::lock_guard<std::mutex> lk(g_imgMutex);
    for (auto& kv : g_imgs) {
        if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
        if (kv.second.surf) SDL_FreeSurface(kv.second.surf);
    }
    g_imgs.clear();
    g_imgStack.clear();
    // keep g_shownOnce so reloaded images appear instantly rather than re-fading
}

SDL_Texture* image(const std::string& url, int maxW, int maxH) {
    if (url.empty() || (url.compare(0, 4, "http") != 0)) return nullptr;
    maxW = (int)(maxW * S / 1.5f);
    maxH = (int)(maxH * S / 1.5f);
    std::lock_guard<std::mutex> lk(g_imgMutex);
    auto it = g_imgs.find(url);
    if (it == g_imgs.end()) {
        Img im;
        im.maxW = maxW;
        im.maxH = maxH;
        im.lastUse = now();
        g_imgs.emplace(url, im);
        g_imgStack.push_back(url);
        if (g_imgStack.size() > 2000) {
            auto old = g_imgStack.front();
            g_imgStack.pop_front();
            auto o = g_imgs.find(old);
            // only drop if still queued AND not requested recently (not on screen)
            if (o != g_imgs.end() && o->second.st == Queued && now() - o->second.lastUse > 2.0) g_imgs.erase(o);
        }
        g_imgCv.notify_one();
        g_missThisFrame++;
        return nullptr;
    }
    Img& im = it->second;
    im.lastUse = now();
    if (im.st == Queued) {
        // re-prioritise: move to top of the LIFO stack
        auto pos = std::find(g_imgStack.begin(), g_imgStack.end(), url);
        if (pos != g_imgStack.end() && pos + 1 != g_imgStack.end()) {
            g_imgStack.erase(pos);
            g_imgStack.push_back(url);
        }
    }
    if (im.st == Decoded && g_uploadsThisFrame < 8) {  // upload budget per frame (prefetch fills ahead)
        g_uploadsThisFrame++;
        im.tex = SDL_CreateTextureFromSurface(R, im.surf);
        SDL_FreeSurface(im.surf);
        im.surf = nullptr;
        if (im.tex) {
            SDL_SetTextureBlendMode(im.tex, SDL_BLENDMODE_BLEND);
            int tw = 0, th = 0;
            SDL_QueryTexture(im.tex, nullptr, nullptr, &tw, &th);
            im.bytes = (size_t)tw * th * 4;
            // Fade in only the first time this URL is ever shown; a re-decoded image
            // (e.g. after eviction) appears instantly so it can't visibly flicker.
            if (g_shownOnce.count(url)) {
                im.readyAt = now() - 10.0;
            } else {
                im.readyAt = now();
                g_shownOnce.insert(url);
                if (g_shownOnce.size() > 4000) g_shownOnce.clear();
            }
            im.st = Ready;
        } else {
            im.st = Failed;
        }
    }
    if (im.st != Ready) g_missThisFrame++;
    return im.st == Ready ? im.tex : nullptr;
}

float imageFade(const std::string& url, float duration) {
    std::lock_guard<std::mutex> lk(g_imgMutex);
    auto it = g_imgs.find(url);
    if (it == g_imgs.end() || it->second.st != Ready) return 0.f;
    float k = (float)((now() - it->second.readyAt) / duration);
    return k >= 1.f ? 1.f : (k <= 0.f ? 0.f : k);
}

bool imageFailed(const std::string& url) {
    if (url.empty()) return true;
    std::lock_guard<std::mutex> lk(g_imgMutex);
    auto it = g_imgs.find(url);
    return it != g_imgs.end() && it->second.st == Failed;
}

bool imageReady(const std::string& url) {
    if (url.empty()) return false;
    std::lock_guard<std::mutex> lk(g_imgMutex);
    auto it = g_imgs.find(url);
    return it != g_imgs.end() && it->second.st == Ready;
}

void drawCover(SDL_Texture* t, float x, float y, float w, float h, float rad, Uint8 alpha) {
    if (!t || w <= 0 || h <= 0) return;
    int tw, th;
    SDL_QueryTexture(t, nullptr, nullptr, &tw, &th);
    float a = w / h, ta = tw / (float)th;
    float u0 = 0, v0 = 0, uw = 1, vh = 1;
    if (ta > a) {
        uw = a / ta;
        u0 = (1 - uw) / 2;
    } else {
        vh = ta / a;
        v0 = (1 - vh) / 2;
    }
    if (rad <= 0.5f) {
        SDL_SetTextureAlphaMod(t, alpha);
        SDL_SetTextureColorMod(t, 255, 255, 255);
        SDL_Rect src{(int)(u0 * tw), (int)(v0 * th), (int)(uw * tw), (int)(vh * th)};
        SDL_FRect dst{x * S, y * S, w * S, h * S};
        SDL_RenderCopyF(R, t, &src, &dst);
        return;
    }
    SDL_SetTextureAlphaMod(t, 255);
    SDL_SetTextureColorMod(t, 255, 255, 255);
    texturedPoly(t, x, y, w, h, rad, Col{255, 255, 255, alpha}, u0, v0, uw, vh);
}

void drawFit(SDL_Texture* t, float x, float y, float w, float h, Uint8 alpha, Align al) {
    if (!t) return;
    int tw, th;
    SDL_QueryTexture(t, nullptr, nullptr, &tw, &th);
    float k = std::min(w / tw, h / th);
    float dw = tw * k, dh = th * k;
    float dx = x;
    if (al == Center) dx = x + (w - dw) / 2;
    else if (al == Right) dx = x + w - dw;
    float dy = y + (h - dh);  // bottom aligned (logos sit on the text below)
    SDL_SetTextureAlphaMod(t, alpha);
    SDL_SetTextureColorMod(t, 255, 255, 255);
    SDL_FRect dst{dx * S, dy * S, dw * S, dh * S};
    SDL_RenderCopyF(R, t, nullptr, &dst);
}

static void gcImages() {
    std::lock_guard<std::mutex> lk(g_imgMutex);
    size_t ready = 0, bytes = 0;
    for (auto& kv : g_imgs)
        if (kv.second.st == Ready) {
            ready++;
            bytes += kv.second.bytes;
        }
    if (ready <= kMaxTextures && bytes <= kMaxTextureBytes) return;
    // Evict off-screen textures (not drawn in the last few seconds). Among those, drop
    // the LARGEST first (backdrops), so many small posters survive and don't reload.
    std::vector<std::pair<size_t, std::string>> v;
    for (auto& kv : g_imgs)
        if (kv.second.st == Ready && now() - kv.second.lastUse > 3.0) v.push_back({kv.second.bytes, kv.first});
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (auto& e : v) {
        if (ready <= kMaxTextures * 9 / 10 && bytes <= kMaxTextureBytes * 9 / 10) break;
        auto it = g_imgs.find(e.second);
        if (it == g_imgs.end()) continue;
        bytes -= it->second.bytes;
        ready--;
        g_evictTotal++;
        SDL_DestroyTexture(it->second.tex);
        g_imgs.erase(it);
    }
}

static void gcText() {
    if (g_text.size() < 600) return;
    double t = now();
    for (auto it = g_text.begin(); it != g_text.end();) {
        if (t - it->second.lastUse > 3.0) {
            SDL_DestroyTexture(it->second.tex);
            it = g_text.erase(it);
        } else {
            ++it;
        }
    }
}

// ---------------------------------------------------------------- lifecycle
bool init(SDL_Renderer* r, int pixelW, int pixelH) {
    R = r;
    S = pixelW / (float)W;
    (void)pixelH;
    g_freq = SDL_GetPerformanceFrequency();
    g_start = SDL_GetPerformanceCounter();
    SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_BLEND);

    if (TTF_Init() != 0) return false;
    IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG | IMG_INIT_WEBP);

    PlFontData fd;
    if (R_FAILED(plGetSharedFontByType(&fd, PlSharedFontType_Standard))) return false;
    g_fontData = fd.address;
    g_fontSize = fd.size;

    trimDiskCache();
    g_imgStop = false;
    // More decode/download workers fill the poster grid faster. Kept within the socket
    // budget (5 here + 4 in the net API pool = 9, under num_bsd_sessions = 12).
    for (int i = 0; i < 5; i++) g_imgThreads.emplace_back(imageWorker);
    return true;
}

void shutdown() {
    g_imgStop = true;
    g_imgCv.notify_all();
    for (auto& t : g_imgThreads)
        if (t.joinable()) t.join();
    g_imgThreads.clear();
    for (auto& kv : g_imgs) {
        if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
        if (kv.second.surf) SDL_FreeSurface(kv.second.surf);
    }
    g_imgs.clear();
    for (auto& kv : g_text) SDL_DestroyTexture(kv.second.tex);
    g_text.clear();
    for (auto& kv : g_fonts)
        if (kv.second) TTF_CloseFont(kv.second);
    g_fonts.clear();
    IMG_Quit();
    TTF_Quit();
}

void beginFrame() {
    g_now = (double)(SDL_GetPerformanceCounter() - g_start) / (double)g_freq;
    g_uploadsLastFrame = g_uploadsThisFrame;
    g_uploadsThisFrame = 0;
    g_missLastFrame = g_missThisFrame;
    g_missThisFrame = 0;
    const Theme& t = theme();
    SDL_SetRenderDrawColor(R, t.bg.r, t.bg.g, t.bg.b, 255);
    SDL_RenderClear(R);
}

void endFrame() {
    gcImages();
    gcText();
}

}  // namespace gfx
