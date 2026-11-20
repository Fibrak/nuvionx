#include "ui.hpp"

#include <switch.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "input.hpp"

namespace ui {

using namespace gfx;

// ------------------------------------------------------------------ touch
struct HitRegion {
    float x, y, w, h;
    std::function<void()> fn;
};
static std::vector<HitRegion> g_hits, g_prevHits;

void clearHits() {
    g_prevHits.swap(g_hits);
    g_hits.clear();
}

void hit(float x, float y, float w, float h, std::function<void()> fn) {
    if (w <= 0 || h <= 0) return;
    g_hits.push_back({x, y, w, h, std::move(fn)});
}

bool dispatchTap(float x, float y) {
    // regions from the last rendered frame (g_hits until clearHits() swaps them)
    const auto& regions = g_hits.empty() ? g_prevHits : g_hits;
    for (auto it = regions.rbegin(); it != regions.rend(); ++it) {
        if (x >= it->x && x < it->x + it->w && y >= it->y && y < it->y + it->h) {
            auto fn = it->fn;  // copy: the handler may trigger re-registration
            fn();
            return true;
        }
    }
    return false;
}

void tapToActivate(float x, float y, float w, float h, std::function<void()> focus) {
    hit(x, y, w, h, [focus]() {
        if (focus) focus();
        input::inject(input::A);
    });
}

void backButton(float x, float y) {
    const Theme& t = theme();
    circle(x + 22, y + 22, 22, withA(t.bg, 170));
    text("<", x + 20, y + 8, 22, t.text, true, Center);
    hit(x - 6, y - 6, 56, 56, [] { input::inject(input::B); });
}

static uint32_t hintButtons(const std::string& b, bool rightHalf) {
    if (b == "A") return input::A;
    if (b == "B") return input::B;
    if (b == "X") return input::X;
    if (b == "Y") return input::Y;
    if (b == "+") return input::Plus;
    if (b == "-") return input::Minus;
    if (b == "LR") return rightHalf ? input::R : input::L;
    if (b == "ZL/ZR") return rightHalf ? input::ZR : input::ZL;
    if (b == "<>") return rightHalf ? input::Right : input::Left;
    return 0;
}

void hints(const std::vector<Hint>& hs, float y) {
    const Theme& t = theme();
    float x = 48;
    for (auto& h : hs) {
        std::string b = h.btn;
        float x0 = x;
        float bw = std::max(26.f, textWidth(b, 13, true) + 14);
        roundRect(x, y, bw, 26, 13, withA(t.text, 230));
        text(b, x + bw / 2, y + 4, 13, t.bg, true, Center);
        x += bw + 8;
        x += text(h.label, x, y + 3, 15, t.text2) + 22;
        // whole pill + label is a touch target that presses the button
        float w = x - x0 - 8;
        if (b == "LR" || b == "ZL/ZR" || b == "<>") {
            uint32_t l = hintButtons(b, false), r = hintButtons(b, true);
            hit(x0 - 4, y - 10, w / 2 + 4, 46, [l] { input::inject(l); });
            hit(x0 + w / 2, y - 10, w / 2 + 4, 46, [r] { input::inject(r); });
        } else if (uint32_t btn = hintButtons(b, false)) {
            hit(x0 - 4, y - 10, w + 8, 46, [btn] { input::inject(btn); });
        }
    }
}

bool keyboard(const std::string& guide, const std::string& initial, std::string& out, bool url, bool password) {
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) return false;
    if (password) swkbdConfigMakePresetPassword(&kbd);
    else swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetGuideText(&kbd, guide.c_str());
    swkbdConfigSetInitialText(&kbd, initial.c_str());
    swkbdConfigSetStringLenMax(&kbd, url ? 1000 : 120);
    if (url) swkbdConfigSetType(&kbd, SwkbdType_All);
    char buf[1024] = {0};
    Result rc = swkbdShow(&kbd, buf, sizeof buf);
    swkbdClose(&kbd);
    if (R_FAILED(rc)) return false;
    out = buf;
    return !out.empty();
}

float approach(float cur, float target, float dt, float speed) {
    float k = 1.f - std::exp(-speed * dt);
    float v = cur + (target - cur) * k;
    if (std::fabs(target - v) < 0.3f) v = target;
    return v;
}

std::string fmtTime(double sec) {
    if (sec < 0 || !std::isfinite(sec)) sec = 0;
    long s = (long)sec;
    char buf[32];
    if (s >= 3600)
        snprintf(buf, sizeof buf, "%ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60);
    else
        snprintf(buf, sizeof buf, "%ld:%02ld", s / 60, s % 60);
    return buf;
}

std::string langName(const std::string& c) {
    static const char* map[][2] = {
        {"eng", "English"},    {"en", "English"},    {"por", "Portuguese"}, {"pt", "Portuguese"},
        {"pob", "Portuguese (BR)"}, {"pt-br", "Portuguese (BR)"}, {"spa", "Spanish"}, {"es", "Spanish"},
        {"fre", "French"},     {"fra", "French"},    {"fr", "French"},      {"ger", "German"},
        {"deu", "German"},     {"de", "German"},     {"ita", "Italian"},    {"it", "Italian"},
        {"dut", "Dutch"},      {"nld", "Dutch"},     {"nl", "Dutch"},       {"pol", "Polish"},
        {"pl", "Polish"},      {"rus", "Russian"},   {"ru", "Russian"},     {"tur", "Turkish"},
        {"tr", "Turkish"},     {"ara", "Arabic"},    {"ar", "Arabic"},      {"jpn", "Japanese"},
        {"ja", "Japanese"},    {"kor", "Korean"},    {"ko", "Korean"},      {"chi", "Chinese"},
        {"zho", "Chinese"},    {"zh", "Chinese"},    {"swe", "Swedish"},    {"sv", "Swedish"},
        {"nor", "Norwegian"},  {"no", "Norwegian"},  {"dan", "Danish"},     {"da", "Danish"},
        {"fin", "Finnish"},    {"fi", "Finnish"},    {"gre", "Greek"},      {"ell", "Greek"},
        {"heb", "Hebrew"},     {"hun", "Hungarian"}, {"cze", "Czech"},      {"ces", "Czech"},
        {"rum", "Romanian"},   {"ron", "Romanian"},  {"hin", "Hindi"},      {"ukr", "Ukrainian"},
    };
    std::string lc;
    for (char ch : c) lc += (char)tolower((unsigned char)ch);
    for (auto& m : map)
        if (lc == m[0]) return m[1];
    return c.empty() ? "Unknown" : c;
}

float buttonWidth(const std::string& label, int size) { return textWidth(label, size, true) + 44; }

void button(const std::string& label, float x, float y, float h, bool focused, bool primary, int size) {
    const Theme& t = theme();
    float w = buttonWidth(label, size);
    Col bg = focused ? t.text : (primary ? t.accentDim : withA(t.surface, 235));
    Col fg = focused ? t.bg : t.text;
    roundRect(x, y, w, h, h / 2, bg);
    text(label, x + w / 2, y + (h - lineHeight(size)) / 2, size, fg, true, Center);
}

float chip(const std::string& label, float x, float y, bool focused, bool selected, int size) {
    const Theme& t = theme();
    float w = textWidth(label, size, true) + 32, h = 34;
    Col bg = focused ? t.text : (selected ? t.accentDim : withA(t.surface, 235));
    Col fg = focused ? t.bg : t.text;
    roundRect(x, y, w, h, h / 2, bg);
    text(label, x + w / 2, y + (h - lineHeight(size)) / 2, size, fg, true, Center);
    return w;
}

void focusRing(float x, float y, float w, float h, float rad) {
    roundOutline(x - 4, y - 4, w + 8, h + 8, rad + 4, 3, theme().focusRing);
}

// ------------------------------------------------------------------ backdrop
void Backdrop::set(const std::string& url, float delay) {
    if (url == want_) return;
    want_ = url;
    wantSince_ = now() - (0.25f - delay);
}

void Backdrop::render(float dt, float darken) {
    const Theme& t = theme();
    if (!want_.empty() && want_ != cur_ && now() - wantSince_ >= 0.25) {
        if (image(want_, 960, 540) || imageFailed(want_)) {
            prev_ = cur_;
            cur_ = imageFailed(want_) ? "" : want_;
            fade_ = 0;
        }
    }
    fade_ = std::min(1.f, fade_ + dt * 3.f);
    SDL_Texture* p = prev_.empty() ? nullptr : image(prev_, 960, 540);
    SDL_Texture* c = cur_.empty() ? nullptr : image(cur_, 960, 540);
    // Old image stays fully opaque underneath while the new one fades in on top,
    // so the screen never dips toward the background colour mid-transition.
    if (p && fade_ < 1) drawCover(p, 0, 0, W, H, 0, 255);
    if (c) drawCover(c, 0, 0, W, H, 0, (Uint8)(255 * fade_));
    if (fade_ >= 1) prev_.clear();
    // scrims
    hGradient(0, 0, 860, H, withA(t.bg, 250), withA(t.bg, 0));
    vGradient(0, 300, W, 420, withA(t.bg, 0), withA(t.bg, 255));
    vGradient(0, 0, W, 120, withA(t.bg, 170), withA(t.bg, 0));
    if (darken > 0) fillRect(0, 0, W, H, withA(t.bg, (Uint8)(255 * std::min(1.f, darken))));
}

// ------------------------------------------------------------------ rows
static const float kGap = 14, kLeft = 48;

void cardSize(bool landscape, float& w, float& h) {
    if (landscape) {
        w = 236;
        h = 133;
    } else {
        w = 126;
        h = 189;
    }
}

void drawCard(const stremio::MetaPreview& m, float x, float y, float w, float h, bool focused, bool landscape,
              bool showTitle, float focusAmt) {
    const Theme& t = theme();
    if (focused) {
        float g = 1.f + 0.05f * focusAmt;  // eased scale, not an instant 1.05 pop
        float nw = w * g, nh = h * g;
        x -= (nw - w) / 2;
        y -= (nh - h) / 2;
        w = nw;
        h = nh;
    }
    std::string url = landscape ? (m.background.empty() ? m.poster : m.background) : m.poster;
    SDL_Texture* tex = image(url, landscape ? 480 : 300, landscape ? 270 : 450);
    float rad = debugFlat() ? 0.f : 10.f;  // debug: flat (plain copy) vs rounded (geometry)
    float fade = tex ? imageFade(url) : 0.f;
    if (fade < 1.f) {
        // placeholder (stays underneath while the artwork fades in)
        roundRect(x, y, w, h, 10, t.card);
        if (!tex || imageFailed(url)) {
            auto lines = wrap(m.name, 15, w - 16, 3, true);
            float ly = y + h / 2 - lines.size() * 10;
            for (auto& l : lines) {
                text(l, x + w / 2, ly, 15, t.text2, true, Center);
                ly += 20;
            }
        }
    }
    if (tex) {
        // Crisp square blit (flat RenderCopy path - no shimmer), then round the corners
        // with solid-colour cut-outs matching the background behind the card.
        drawCover(tex, x, y, w, h, 0, (Uint8)(255 * fade));
        if (rad > 0.5f) cornerCutouts(x, y, w, h, rad, t.bg);
    }
    if (landscape && m.progress >= 0) {
        fillRect(x + 10, y + h - 12, w - 20, 4, withA(t.text, 70));
        fillRect(x + 10, y + h - 12, (w - 20) * std::min(1.f, m.progress), 4, t.accent);
    }
    if (focused && focusAmt > 0.01f) {
        Col ring = withA(t.focusRing, (Uint8)(255 * std::min(1.f, focusAmt)));
        roundOutline(x - 4, y - 4, w + 8, h + 8, 14, 3, ring);
    }
    if (landscape) {
        text(m.name, x + 2, y + h + 8, 16, focused ? t.text : t.text2, true, Left, w);
        if (!m.subtitle.empty()) text(m.subtitle, x + 2, y + h + 28, 14, t.text3, false, Left, w);
    } else if (showTitle) {
        text(m.name, x + 2, y + h + 8, 15, focused ? t.text : t.text2, focused, Left, w);
    }
}

bool RowsView::visible(size_t i) const { return i < rows.size() && (rows[i].loading || !rows[i].items.empty()); }

bool RowsView::hasVisible() const {
    for (size_t i = 0; i < rows.size(); i++)
        if (visible(i)) return true;
    return false;
}

bool RowsView::anyLoading() const {
    for (auto& r : rows)
        if (r.loading) return true;
    return false;
}

float RowsView::rowHeight(const PosterRow& r) const {
    float w, h;
    cardSize(r.landscape, w, h);
    float extra = r.landscape ? 48 : (showTitles ? 30 : 0);
    return 34 + h + extra + 26;
}

void RowsView::ensureValidFocus() {
    if (rows.empty()) {
        row = 0;
        return;
    }
    if (row >= (int)rows.size()) row = (int)rows.size() - 1;
    if (row < 0) row = 0;
    if (!visible(row)) {
        for (size_t i = row; i < rows.size(); i++)
            if (visible(i)) {
                row = (int)i;
                break;
            }
        if (!visible(row))
            for (int i = row; i >= 0; i--)
                if (visible(i)) {
                    row = i;
                    break;
                }
    }
    for (auto& r : rows) {
        if (r.items.empty()) r.col = 0;
        else if (r.col >= (int)r.items.size()) r.col = (int)r.items.size() - 1;
    }
}

stremio::MetaPreview* RowsView::focused() {
    ensureValidFocus();
    if (!visible(row)) return nullptr;
    auto& r = rows[row];
    if (r.items.empty()) return nullptr;
    return &r.items[r.col];
}

PosterRow* RowsView::focusedRow() {
    ensureValidFocus();
    return visible(row) ? &rows[row] : nullptr;
}

bool RowsView::handleNav(bool* hitTop) {
    if (hitTop) *hitTop = false;
    if (!hasVisible()) {
        if (hitTop && input::pressed(input::Up)) *hitTop = true;
        return false;
    }
    ensureValidFocus();
    auto& r = rows[row];
    bool moved = false;
    if (input::pressed(input::Left) && r.col > 0) {
        r.col--;
        moved = true;
    }
    if (input::pressed(input::Right) && r.col + 1 < (int)r.items.size()) {
        r.col++;
        moved = true;
    }
    if (input::pressed(input::Down)) {
        for (size_t j = row + 1; j < rows.size(); j++)
            if (visible(j)) {
                row = (int)j;
                moved = true;
                break;
            }
    }
    if (input::pressed(input::Up)) {
        bool found = false;
        for (int j = row - 1; j >= 0; j--)
            if (visible(j)) {
                row = j;
                moved = found = true;
                break;
            }
        if (!found && hitTop) *hitTop = true;
    }
    auto& fr = rows[row];
    if (onNeedMore && fr.canPage && !fr.paging && !fr.exhausted && fr.col + 8 >= (int)fr.items.size())
        onNeedMore(row);
    return moved;
}

void RowsView::update(float dt) {
    ensureValidFocus();
    // Ease the focus highlight in. When focus jumps (fast scroll), reset to 0 so cards the
    // cursor only crosses for a frame barely scale — kills the pop/flicker.
    int curCol = (visible(row) && !rows[row].items.empty()) ? rows[row].col : 0;
    if (row != focusRow_ || curCol != focusCol_) {
        focusAmt_ = 0.f;
        focusRow_ = row;
        focusCol_ = curCol;
    }
    focusAmt_ = approach(focusAmt_, 1.f, dt, 13);
    float acc = 0;
    for (int i = 0; i < (int)rows.size() && i < row; i++)
        if (visible(i)) acc += rowHeight(rows[i]);
    vtarget_ = acc;
    vscroll_ = approach(vscroll_, vtarget_, dt);
    for (auto& r : rows) {
        float w, h;
        cardSize(r.landscape, w, h);
        float target = std::max(0.f, r.col * (w + kGap) - (r.landscape ? 0 : (w + kGap)));
        r.scroll = approach(r.scroll, target, dt, 16);
    }
}

// Request a card's artwork into the cache without drawing it (so it's ready
// before the card scrolls into view, preventing blank-then-pop flicker).
static void prefetchCard(const stremio::MetaPreview& m, bool landscape) {
    std::string url = landscape ? (m.background.empty() ? m.poster : m.background) : m.poster;
    image(url, landscape ? 480 : 300, landscape ? 270 : 450);
}

void RowsView::render(float top, float bottom) {
    const Theme& t = theme();
    pushClip(0, top - 30, W, bottom - top + 30);
    float y = top - vscroll_;
    for (size_t i = 0; i < rows.size(); i++) {
        if (!visible(i)) continue;
        auto& r = rows[i];
        float rh = rowHeight(r);
        if (y + rh < top - 40) {
            y += rh;
            continue;
        }
        // Rows just below the viewport: prefetch their artwork, then stop once far enough.
        if (y > bottom) {
            if (y > bottom + 2 * rh + 300) break;
            float cwp, chp;
            cardSize(r.landscape, cwp, chp);
            int firstK = std::max(0, (int)(r.scroll / (cwp + kGap)) - 1);
            for (int k = firstK; k < (int)r.items.size() && k < firstK + 12; k++)
                prefetchCard(r.items[k], r.landscape);
            y += rh;
            continue;
        }
        bool rowFocused = active && (int)i == row;
        float tw = text(r.title, kLeft, y, 20, rowFocused ? t.text : t.text2, true);
        if (!r.tag.empty()) text(r.tag, kLeft + tw + 12, y + 4, 14, t.text3);
        float cw, ch;
        cardSize(r.landscape, cw, ch);
        float cy = y + 34;
        if (r.items.empty() && r.loading) {
            float pulse = 0.5f + 0.5f * std::sin((float)now() * 4.f);
            for (int k = 0; k < 10; k++)
                roundRect(kLeft + k * (cw + kGap), cy, cw, ch, 10, withA(t.card, (Uint8)(150 + 80 * pulse)));
        } else {
            int first = std::max(0, (int)(r.scroll / (cw + kGap)) - 1);
            for (int k = first; k < (int)r.items.size(); k++) {
                float cx = kLeft + k * (cw + kGap) - r.scroll;
                if (cx > W + 10) {
                    // prefetch the next few off-screen cards to the right, then stop
                    for (int p = k; p < (int)r.items.size() && p < k + 4; p++) prefetchCard(r.items[p], r.landscape);
                    break;
                }
                if (cx + cw < -20) continue;
                bool f = rowFocused && k == r.col;
                if (cy + ch > top - 30 && cy < bottom) {
                    int ri = (int)i;
                    tapToActivate(cx, cy, cw, ch, [this, ri, k] {
                        row = ri;
                        rows[ri].col = k;
                    });
                }
                if (f) continue;  // draw focused last so it sits on top
                drawCard(r.items[k], cx, cy, cw, ch, false, r.landscape, showTitles);
            }
            if (rowFocused && r.col < (int)r.items.size()) {
                float cx = kLeft + r.col * (cw + kGap) - r.scroll;
                drawCard(r.items[r.col], cx, cy, cw, ch, true, r.landscape, showTitles, focusAmt_);
            }
            if (r.paging) {
                float cx = kLeft + r.items.size() * (cw + kGap) - r.scroll;
                if (cx < W) spinner(cx + cw / 2, cy + ch / 2, 14, t.text2);
            }
        }
        y += rh;
    }
    popClip();
}

}  // namespace ui
