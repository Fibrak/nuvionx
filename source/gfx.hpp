// Rendering helpers on top of SDL_Renderer. All coordinates are in a 1280x720
// design space and scaled to the real output (1920x1080) internally.
#pragma once
#include <SDL.h>

#include <string>
#include <vector>

namespace gfx {

struct Col {
    Uint8 r, g, b, a;
};
inline Col rgba(unsigned hex, Uint8 a = 255) { return Col{(Uint8)(hex >> 16), (Uint8)(hex >> 8), (Uint8)hex, a}; }
inline Col withA(Col c, Uint8 a) { return Col{c.r, c.g, c.b, a}; }

struct Theme {
    const char* name;
    Col bg, elevated, card, surface, accent, accentDim, focusRing, focusBg;
    Col text, text2, text3;
};
const Theme& theme();
int themeCount();
const Theme& themeAt(int i);

extern SDL_Renderer* R;
extern float S;  // design -> pixel scale
const int W = 1280, H = 720;

bool init(SDL_Renderer* r, int pixelW, int pixelH);
void shutdown();
void beginFrame();
void endFrame();
double now();  // seconds since start

// shapes
void fillRect(float x, float y, float w, float h, Col c);
void roundRect(float x, float y, float w, float h, float rad, Col c);
void roundOutline(float x, float y, float w, float h, float rad, float thick, Col c);
void vGradient(float x, float y, float w, float h, Col top, Col bottom);
void hGradient(float x, float y, float w, float h, Col left, Col right);
void circle(float cx, float cy, float r, Col c);
void spinner(float cx, float cy, float r, Col c);

// clipping
void pushClip(float x, float y, float w, float h);
void popClip();

// text (size is in design pixels)
enum Align { Left = 0, Center = 1, Right = 2 };
float textWidth(const std::string& s, int size, bool bold = false);
float lineHeight(int size);
// Draws one line; ellipsizes if maxW > 0. Returns drawn width.
float text(const std::string& s, float x, float y, int size, Col c, bool bold = false, Align al = Left,
           float maxW = 0);
std::vector<std::string> wrap(const std::string& s, int size, float maxW, int maxLines, bool bold = false);
// Draws wrapped text, returns height used.
float paragraph(const std::string& s, float x, float y, float maxW, int size, Col c, int maxLines, float lineGap = 1.3f,
                bool bold = false);

// System font (used for mpv/libass subtitles too)
std::string fontFamily();
const void* fontData(size_t* size);

// images (downloaded + decoded on background threads, cached on the GPU)
// Returns nullptr until ready. maxW/maxH bound the decoded size in pixels.
SDL_Texture* image(const std::string& url, int maxW = 600, int maxH = 900);
bool imageFailed(const std::string& url);
// Upper bound for decoded artwork kept in GPU memory.
void setImageBudget(size_t bytes);
// Free all cached artwork textures (e.g. before video playback, to give mpv the RAM).
// Images reload on next use; already-seen ones reappear instantly (no fade).
void releaseImageCache();
// 0..1 ramp since the image became ready (for fade-in; 0 if not ready).
float imageFade(const std::string& url, float duration = 0.18f);
// Diagnostics: cache contents and how many textures were uploaded last frame.
void imageStats(int& ready, int& total, double& megabytes, int& pending, int& uploadsLastFrame);
void imageStats2(int& missLastFrame, int& evictTotal);
// Debug: draw cards as plain textured quads (no rounded-corner geometry) to bisect flicker.
void setDebugFlat(bool on);
bool debugFlat();
// Draw texture to cover (crop) the rect, with rounded corners.
void drawCover(SDL_Texture* t, float x, float y, float w, float h, float rad = 0, Uint8 alpha = 255);
// Fill the four corner cut-outs (square corner minus the rounded arc) with a solid
// colour. Used to round a crisply-blitted image without textured geometry (which
// shimmers during motion on the Switch). `col` should match what's behind the card.
void cornerCutouts(float x, float y, float w, float h, float rad, Col col);
// Draw texture fitted (letterboxed) inside the rect, aligned left/center.
void drawFit(SDL_Texture* t, float x, float y, float w, float h, Uint8 alpha = 255, Align al = Left);

}  // namespace gfx
