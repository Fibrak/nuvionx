// Per-frame playback trace, for finding where dropped frames come from.
//
// Records into preallocated RAM while a video plays and writes one CSV to the SD card only
// when the player closes, so tracing doesn't add SD or allocation stalls to playback.
// Enabled from the debug cycle (ZL+ZR on the main menu). Contains no titles or stream URLs.
#pragma once
#include <cstdint>
#include <string>

namespace playtrace {

// Why a moment isn't "steady playback"; the summary only counts rows with no flags set.
enum Flag : uint8_t {
    Paused = 1,
    Buffering = 2,    // loading or waiting on the network
    UiVisible = 4,    // controls/menu up (the loop runs at 60 fps then)
    Disturbed = 8,    // just after a load, seek or rebuffer
};

struct Status {
    uint8_t flags = 0;
    int64_t decoderDrops = -1, voDrops = -1;
    double avsyncMs = 0, cacheSec = -1, cacheKBps = -1, vfFps = -1;
    int relayTransfers = 0;
};

int64_t nowUs();  // monotonic, valid from any thread

// Main thread. begin() returns false if the buffers couldn't be allocated.
bool begin();
bool active();

// Video thread only. `seq` numbers new video frames. lead/late are NaN if mpv gave no target time.
void videoFrame(uint32_t seq, uint32_t mpvFlags, float leadMs, float renderMs, float finishMs, float lateMs);
// One main-loop frame, in milliseconds. gc/gap belong to the tail of the previous frame.
struct PresentTiming {
    float pre = 0, update = 0, draw = 0, video = 0, work = 0, block = 0, gc = 0, gap = 0;
};
// Main thread only. `seq` is the newest video frame composited (0 = none yet).
void present(uint32_t seq, uint8_t flags, float expectedMs, const PresentTiming& timing);
void status(const Status& s);

// Call after the video thread has stopped. `meta` is "key=value" lines. Returns the file
// path, or "" if nothing was written.
std::string finish(const std::string& meta);

}  // namespace playtrace
