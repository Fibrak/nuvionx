#include "input.hpp"

#include <switch.h>

#include <cmath>

#include "gfx.hpp"

namespace input {

static PadState g_pad;
static uint32_t g_held = 0, g_prev = 0, g_pressed = 0;
static double g_since[32];
static double g_nextRepeat[32];

static const uint32_t kRepeatable = Up | Down | Left | Right | L | R | ZL | ZR;

// ---- touch state
struct TouchState {
    bool down = false, dragging = false;
    float x = 0, y = 0, startX = 0, startY = 0, lastX = 0, lastY = 0;
    float accX = 0, accY = 0;
    double startT = 0;
    bool tap = false;
    float tapX = 0, tapY = 0;
};
static TouchState g_touch;
static const float kDragSlop = 14.f;   // px before a touch becomes a drag
static const float kStepX = 110.f;     // px of horizontal drag per Left/Right step
static const float kStepY = 95.f;      // px of vertical drag per Up/Down step

static void updateTouch() {
    g_touch.tap = false;
    HidTouchScreenState st = {0};
    bool down = hidGetTouchScreenStates(&st, 1) > 0 && st.count > 0;
    double t = gfx::now();
    if (down) {
        float x = (float)st.touches[0].x, y = (float)st.touches[0].y;
        if (!g_touch.down) {
            g_touch = TouchState{};
            g_touch.down = true;
            g_touch.startX = g_touch.lastX = x;
            g_touch.startY = g_touch.lastY = y;
            g_touch.startT = t;
        }
        g_touch.x = x;
        g_touch.y = y;
        if (!g_touch.dragging &&
            (std::fabs(x - g_touch.startX) > kDragSlop || std::fabs(y - g_touch.startY) > kDragSlop))
            g_touch.dragging = true;
        if (g_touch.dragging) {
            g_touch.accX += x - g_touch.lastX;
            g_touch.accY += y - g_touch.lastY;
            // content follows the finger: dragging left reveals items to the right
            while (g_touch.accX <= -kStepX) { g_pressed |= Right; g_touch.accX += kStepX; }
            while (g_touch.accX >= kStepX) { g_pressed |= Left; g_touch.accX -= kStepX; }
            while (g_touch.accY <= -kStepY) { g_pressed |= Down; g_touch.accY += kStepY; }
            while (g_touch.accY >= kStepY) { g_pressed |= Up; g_touch.accY -= kStepY; }
        }
        g_touch.lastX = x;
        g_touch.lastY = y;
    } else if (g_touch.down) {
        if (!g_touch.dragging && t - g_touch.startT < 0.8) {
            g_touch.tap = true;
            g_touch.tapX = g_touch.startX;
            g_touch.tapY = g_touch.startY;
        }
        g_touch.down = false;
        g_touch.dragging = false;
    }
}

void inject(uint32_t b) { g_pressed |= b; }

bool tapped(float* x, float* y) {
    if (!g_touch.tap) return false;
    if (x) *x = g_touch.tapX;
    if (y) *y = g_touch.tapY;
    return true;
}

bool touching() { return g_touch.down; }

void init() {
    hidInitializeTouchScreen();
    // Standard style set covers Pro Controller, handheld, dual Joy-Con and a
    // single Joy-Con held sideways.
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    hidSetNpadJoyHoldType(HidNpadJoyHoldType_Horizontal);
    padInitializeDefault(&g_pad);
    for (int i = 0; i < 32; i++) g_since[i] = g_nextRepeat[i] = 0;
}

static uint32_t mapButtons(u64 b) {
    uint32_t o = 0;
    if (b & HidNpadButton_AnyUp) o |= Up;
    if (b & HidNpadButton_AnyDown) o |= Down;
    if (b & HidNpadButton_AnyLeft) o |= Left;
    if (b & HidNpadButton_AnyRight) o |= Right;
    if (b & HidNpadButton_A) o |= A;
    if (b & HidNpadButton_B) o |= B;
    if (b & HidNpadButton_X) o |= X;
    if (b & HidNpadButton_Y) o |= Y;
    // A single sideways Joy-Con reports its rail buttons as SL/SR.
    if (b & (HidNpadButton_L | HidNpadButton_LeftSL | HidNpadButton_RightSL)) o |= L;
    if (b & (HidNpadButton_R | HidNpadButton_LeftSR | HidNpadButton_RightSR)) o |= R;
    if (b & HidNpadButton_ZL) o |= ZL;
    if (b & HidNpadButton_ZR) o |= ZR;
    if (b & HidNpadButton_Plus) o |= Plus;
    if (b & HidNpadButton_Minus) o |= Minus;
    return o;
}

// A lone Joy-Con held sideways: the stick navigates and the four face buttons
// are used by position (right = A, bottom = B, top = X, left = Y).
static uint32_t mapSingleJoyCon(u64 b, bool left) {
    uint32_t o = 0;
    if (b & (HidNpadButton_StickLUp | HidNpadButton_StickRUp)) o |= Up;
    if (b & (HidNpadButton_StickLDown | HidNpadButton_StickRDown)) o |= Down;
    if (b & (HidNpadButton_StickLLeft | HidNpadButton_StickRLeft)) o |= Left;
    if (b & (HidNpadButton_StickLRight | HidNpadButton_StickRRight)) o |= Right;
    if (left) {
        // rotated counter-clockwise: Down->right, Left->bottom, Right->top, Up->left
        if (b & HidNpadButton_Down) o |= A;
        if (b & HidNpadButton_Left) o |= B;
        if (b & HidNpadButton_Right) o |= X;
        if (b & HidNpadButton_Up) o |= Y;
        if (b & HidNpadButton_Minus) o |= Plus;  // only menu button on a left Joy-Con
    } else {
        if (b & HidNpadButton_A) o |= A;
        if (b & HidNpadButton_B) o |= B;
        if (b & HidNpadButton_X) o |= X;
        if (b & HidNpadButton_Y) o |= Y;
        if (b & HidNpadButton_Plus) o |= Plus;
    }
    if (b & (HidNpadButton_LeftSL | HidNpadButton_RightSL)) o |= L;
    if (b & (HidNpadButton_LeftSR | HidNpadButton_RightSR)) o |= R;
    // ZL/ZR on a single Joy-Con act as Minus (info / extra action)
    if (b & (HidNpadButton_ZL | HidNpadButton_ZR | HidNpadButton_L | HidNpadButton_R)) o |= Minus;
    return o;
}

void update() {
    padUpdate(&g_pad);
    g_prev = g_held;
    u64 raw = padGetButtons(&g_pad);
    u32 style = padGetStyleSet(&g_pad);
    const u32 full = HidNpadStyleTag_NpadFullKey | HidNpadStyleTag_NpadHandheld | HidNpadStyleTag_NpadJoyDual;
    if (!(style & full) && (style & HidNpadStyleTag_NpadJoyLeft))
        g_held = mapSingleJoyCon(raw, true);
    else if (!(style & full) && (style & HidNpadStyleTag_NpadJoyRight))
        g_held = mapSingleJoyCon(raw, false);
    else
        g_held = mapButtons(raw);
    double t = gfx::now();
    g_pressed = g_held & ~g_prev;
    for (int i = 0; i < 14; i++) {
        uint32_t bit = 1u << i;
        if (g_pressed & bit) {
            g_since[i] = t;
            g_nextRepeat[i] = t + 0.38;
        } else if ((g_held & bit) && (kRepeatable & bit) && t >= g_nextRepeat[i]) {
            g_pressed |= bit;
            // accelerate the longer it's held
            double heldT = t - g_since[i];
            g_nextRepeat[i] = t + (heldT > 1.6 ? 0.045 : 0.085);
        }
    }
    updateTouch();
}

bool pressed(uint32_t b) { return (g_pressed & b) != 0; }
bool held(uint32_t b) { return (g_held & b) != 0; }
bool released(uint32_t b) { return (g_prev & b) && !(g_held & b); }
bool any() { return g_pressed != 0; }

float heldFor(uint32_t b) {
    for (int i = 0; i < 14; i++)
        if (b == (1u << i)) return (g_held & b) ? (float)(gfx::now() - g_since[i]) : 0.f;
    return 0.f;
}

}  // namespace input
