// Controller input: Joy-Con (attached, dual, or a single sideways Joy-Con),
// Pro Controller and handheld mode, via libnx's PadState. D-pad and both
// sticks drive navigation, with key-repeat when held.
#pragma once
#include <cstdint>

namespace input {

enum Btn : uint32_t {
    Up = 1u << 0,
    Down = 1u << 1,
    Left = 1u << 2,
    Right = 1u << 3,
    A = 1u << 4,
    B = 1u << 5,
    X = 1u << 6,
    Y = 1u << 7,
    L = 1u << 8,
    R = 1u << 9,
    ZL = 1u << 10,
    ZR = 1u << 11,
    Plus = 1u << 12,
    Minus = 1u << 13,
};

void init();
void update();
bool pressed(uint32_t b);  // edge, or auto-repeat for directions / shoulders
bool held(uint32_t b);
bool released(uint32_t b);
bool any();                // anything pressed this frame (incl. repeats)
// Seconds the given button has been held (0 if not held).
float heldFor(uint32_t b);

// Simulate a button press for the current frame (used by touch).
void inject(uint32_t b);

// Touch screen (handheld). Coordinates are in the 1280x720 design space.
// A tap fires on release when the finger didn't move; swipes are converted
// into directional presses so every list/row scrolls by touch.
bool tapped(float* x, float* y);
bool touching();

}  // namespace input
