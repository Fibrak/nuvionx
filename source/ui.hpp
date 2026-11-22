// Shared widgets: hint bar, buttons, chips, poster rows.
#pragma once
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "gfx.hpp"
#include "stremio.hpp"

namespace ui {

struct Hint {
    const char* btn;  // "A", "B", "X", "Y", "L", "R", "+", "-", "ZL", "ZR", "LR"
    std::string label;
};

// ---- touch hit regions: registered while rendering, consumed on the next tap.
void clearHits();
void hit(float x, float y, float w, float h, std::function<void()> fn);
bool dispatchTap(float x, float y);  // topmost (last registered) region wins
// Region that focuses something and then "presses A" on it.
void tapToActivate(float x, float y, float w, float h, std::function<void()> focus);
// Small round back button (top-left) that presses B.
void backButton(float x = 14, float y = 14);

void hints(const std::vector<Hint>& h, float y = 684);
// Opens the system software keyboard (blocking). Returns false if cancelled.
bool keyboard(const std::string& guide, const std::string& initial, std::string& out, bool url = false,
              bool password = false);
float approach(float cur, float target, float dt, float speed = 14.f);
std::string fmtTime(double sec);
std::string langName(const std::string& code);

// Pill button; returns its width.
float buttonWidth(const std::string& label, int size = 18);
void button(const std::string& label, float x, float y, float h, bool focused, bool primary = false, int size = 18);
float chip(const std::string& label, float x, float y, bool focused, bool selected, int size = 16);
void focusRing(float x, float y, float w, float h, float rad);

// Full-bleed backdrop with NuvioTV-style left + bottom scrims, cross-faded.
class Backdrop {
public:
    void set(const std::string& url, float delay = 0.25f);
    void render(float dt, float darken = 0.f);

private:
    std::string want_, cur_, prev_;
    double wantSince_ = 0;
    float fade_ = 1;
};

// ------------------------------------------------------------------ rows
struct PosterRow {
    std::string title, tag;
    std::vector<stremio::MetaPreview> items;
    bool loading = true;
    bool landscape = false;
    int col = 0;
    float scroll = 0;
    bool deferUntilReady = false;  // reveal each card only once its poster is decoded (recency order)
    // paging
    stremio::Addon addon;
    stremio::CatalogDesc cat;
    std::string extraPrefix;  // e.g. "search=foo&" for search rows
    bool canPage = false, paging = false, exhausted = false;
};

class RowsView {
public:
    std::vector<PosterRow> rows;
    int row = 0;
    bool showTitles = false;
    bool active = true;             // draws focus
    bool suppressFocusedTitle = false;  // hide the top row's title (shown by a loading overlay instead)

    // Returns true if focus moved. Sets hitTop when Up is pressed on the first row.
    bool handleNav(bool* hitTop = nullptr);
    stremio::MetaPreview* focused();
    PosterRow* focusedRow();
    bool hasVisible() const;
    bool anyLoading() const;
    void ensureValidFocus();
    void update(float dt);
    // topFade > 0: rows leaving through the top dissolve over that many pixels above `top`
    // (that strip must be free of other content) instead of being cut off at the edge.
    void render(float top, float bottom, float topFade = 0.f);
    // Reveal amount (0..1) of the currently focused card; 1 unless a deferred row is
    // still waiting for its artwork. The hero waits on this so it tracks the first entry.
    float focusedAppear() const { return focusedAppear_; }

    // Called when the focused row nears its end and can page.
    std::function<void(int rowIndex)> onNeedMore;

private:
    bool visible(size_t i) const;
    float rowHeight(const PosterRow& r) const;
    float vscroll_ = 0, vtarget_ = 0;
    float focusAmt_ = 1.f;        // eased focus highlight (0 just after focus jumps)
    float focusedAppear_ = 1.f;   // reveal amount of the focused card (for the hero)
    int focusRow_ = -1, focusCol_ = -1;
    // Per-card reveal start time, keyed by item id so it survives row rebuilds (addons
    // loading, library refresh) - the pop-in plays once and never restarts.
    std::unordered_map<std::string, double> revealTime_;
};

void cardSize(bool landscape, float& w, float& h);
// focusAmt (0..1) eases the focus scale + ring in, so a card the cursor only
// crosses for a frame during fast scrolling barely changes (no pop/flicker).
// appear (0..1) scales + fades a card in; combined with a per-card stagger it makes
// posters pop in one at a time when a row first appears.
void drawCard(const stremio::MetaPreview& m, float x, float y, float w, float h, bool focused, bool landscape,
              bool showTitle, float focusAmt = 1.f, float appear = 1.f, float opacity = 1.f);

}  // namespace ui
