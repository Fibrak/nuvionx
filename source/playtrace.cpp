#include "playtrace.hpp"

#include <switch.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "sdfile.hpp"

namespace playtrace {

namespace {

// About 11 minutes at 60 fps, or 22+ at the 30 fps playback cap. Recording simply stops when
// full (and the summary says so) rather than reallocating mid-playback.
const size_t kMaxFrames = 40000, kMaxPresents = 40000, kMaxStatus = 2400;

struct FrameRow {
    int64_t t;
    float lead, render, finish, late;
    uint32_t seq, mpvFlags;
};
struct PresentRow {
    int64_t t;
    PresentTiming timing;
    float interval, expected;
    uint32_t seq;
    uint16_t lost;
    uint8_t flags;
};
struct StatusRow {
    int64_t t;
    Status s;
};

FrameRow* g_frames = nullptr;
PresentRow* g_presents = nullptr;
StatusRow* g_status = nullptr;
size_t g_nFrames = 0, g_nPresents = 0, g_nStatus = 0;
bool g_truncated = false;
int64_t g_start = 0, g_lastPresent = 0;
uint32_t g_lastSeq = 0;
std::atomic<bool> g_active{false};

void release() {
    free(g_frames);
    free(g_presents);
    free(g_status);
    g_frames = nullptr;
    g_presents = nullptr;
    g_status = nullptr;
}

void appendMs(std::string& out, float v) {
    out += ',';
    if (std::isnan(v)) return;
    char b[24];
    snprintf(b, sizeof b, "%.2f", v);
    out += b;
}

void appendRow(std::string& out, char kind, int64_t t, long long seq, int flags) {
    char b[64];
    snprintf(b, sizeof b, "%c,%.2f,%lld,%d", kind, t / 1000.0, seq, flags);
    out += b;
}

}  // namespace

int64_t nowUs() { return (int64_t)(armTicksToNs(armGetSystemTick()) / 1000); }

bool active() { return g_active.load(std::memory_order_relaxed); }

bool begin() {
    if (active()) return false;
    release();
    g_frames = (FrameRow*)malloc(kMaxFrames * sizeof(FrameRow));
    g_presents = (PresentRow*)malloc(kMaxPresents * sizeof(PresentRow));
    g_status = (StatusRow*)malloc(kMaxStatus * sizeof(StatusRow));
    if (!g_frames || !g_presents || !g_status) {
        release();
        return false;
    }
    g_nFrames = g_nPresents = g_nStatus = 0;
    g_truncated = false;
    g_lastSeq = 0;
    g_start = nowUs();
    g_lastPresent = 0;
    g_active = true;
    return true;
}

void videoFrame(uint32_t seq, uint32_t mpvFlags, float leadMs, float renderMs, float finishMs, float lateMs) {
    if (!active()) return;
    if (g_nFrames >= kMaxFrames) {
        g_truncated = true;
        return;
    }
    g_frames[g_nFrames++] = FrameRow{nowUs() - g_start, leadMs, renderMs, finishMs, lateMs, seq, mpvFlags};
}

void present(uint32_t seq, uint8_t flags, float expectedMs, const PresentTiming& timing) {
    if (!active()) return;
    int64_t t = nowUs() - g_start;
    float interval = g_lastPresent ? (t - g_lastPresent) / 1000.f : NAN;
    g_lastPresent = t;
    uint32_t lost = (seq > 0 && g_lastSeq > 0 && seq > g_lastSeq) ? seq - g_lastSeq - 1 : 0;
    if (seq > 0) g_lastSeq = seq;
    if (g_nPresents >= kMaxPresents) {
        g_truncated = true;
        return;
    }
    g_presents[g_nPresents++] =
        PresentRow{t, timing, interval, expectedMs, seq, (uint16_t)std::min<uint32_t>(lost, 65535), flags};
}

void status(const Status& s) {
    if (!active() || g_nStatus >= kMaxStatus) return;
    g_status[g_nStatus].t = nowUs() - g_start;
    g_status[g_nStatus].s = s;
    g_nStatus++;
}

std::string finish(const std::string& meta) {
    if (!active()) return "";
    g_active = false;
    if (g_nPresents == 0 && g_nFrames == 0) {
        release();
        return "";
    }

    // Summary: steady playback only (no pause, buffering, UI, or just-seeked moments).
    long long steadyPresents = 0, lost = 0, repeats = 0, missedVblanks = 0, lateFrames = 0, steadyFrames = 0;
    float worstWork = 0, worstInterval = 0, worstRender = 0, worstFinish = 0;
    PresentTiming worst;  // per stage, across the whole capture (startup stalls count here)
    uint32_t prevSeq = 0;
    for (size_t i = 0; i < g_nPresents; i++) {
        const PresentRow& p = g_presents[i];
        if (p.flags & Buffering) {
            // still loading: stalls here are expected and would hide the playback ones
        } else {
            worst.pre = std::max(worst.pre, p.timing.pre);
            worst.update = std::max(worst.update, p.timing.update);
            worst.draw = std::max(worst.draw, p.timing.draw);
            worst.video = std::max(worst.video, p.timing.video);
            worst.gc = std::max(worst.gc, p.timing.gc);
            worst.gap = std::max(worst.gap, p.timing.gap);
        }
        if (p.flags == 0) {
            steadyPresents++;
            lost += p.lost;
            if (p.seq > 0 && p.seq == prevSeq) repeats++;
            worstWork = std::max(worstWork, p.timing.work);
            if (!std::isnan(p.interval)) {
                worstInterval = std::max(worstInterval, p.interval);
                if (p.interval > p.expected * 1.5f) missedVblanks++;
            }
        }
        prevSeq = p.seq;
    }
    size_t pi = 0;
    for (size_t i = 0; i < g_nFrames; i++) {
        const FrameRow& f = g_frames[i];
        while (pi + 1 < g_nPresents && g_presents[pi + 1].t <= f.t) pi++;
        if (g_nPresents == 0 || g_presents[pi].flags != 0) continue;
        steadyFrames++;
        worstRender = std::max(worstRender, f.render);
        worstFinish = std::max(worstFinish, f.finish);
        if (!std::isnan(f.late) && f.late > 1000.f / 60.f) lateFrames++;
    }
    long long decDrops = 0, voDrops = 0;
    bool haveFirst = false;
    int64_t firstDec = 0, firstVo = 0;
    for (size_t i = 0; i < g_nStatus; i++) {
        const Status& s = g_status[i].s;
        if (s.decoderDrops < 0 || s.voDrops < 0) continue;
        if (!haveFirst) {
            haveFirst = true;
            firstDec = s.decoderDrops;
            firstVo = s.voDrops;
        }
        decDrops = s.decoderDrops - firstDec;
        voDrops = s.voDrops - firstVo;
    }

    std::string out;
    out.reserve(4096 + (g_nFrames + g_nPresents + g_nStatus) * 72);
    out += "# NuvioNX playback trace v2\n";
    size_t ls = 0;
    while (ls < meta.size()) {
        size_t le = meta.find('\n', ls);
        if (le == std::string::npos) le = meta.size();
        if (le > ls) out += "# " + meta.substr(ls, le - ls) + "\n";
        ls = le + 1;
    }
    char b[256];
    snprintf(b, sizeof b, "# summary steady_presents=%lld steady_video_frames=%lld truncated=%d\n", steadyPresents,
             steadyFrames, g_truncated ? 1 : 0);
    out += b;
    snprintf(b, sizeof b, "# summary decoder_drops=%lld vo_drops=%lld never_shown=%lld late_frames=%lld\n", decDrops,
             voDrops, lost, lateFrames);
    out += b;
    snprintf(b, sizeof b, "# summary repeated_presents=%lld missed_vblanks=%lld\n", repeats, missedVblanks);
    out += b;
    snprintf(b, sizeof b, "# summary worst_ms main_work=%.1f present_interval=%.1f mpv_render=%.1f gl_finish=%.1f\n",
             worstWork, worstInterval, worstRender, worstFinish);
    out += b;
    snprintf(b, sizeof b, "# summary worst_stage_ms pre=%.1f update=%.1f draw=%.1f video=%.1f gc=%.1f gap=%.1f\n",
             worst.pre, worst.update, worst.draw, worst.video, worst.gc, worst.gap);
    out += b;
    out += "# F=video frame, P=present, S=status. flags on P/S: 1 paused, 2 buffering, 4 UI, 8 after seek/load; "
           "on F: mpv frame-info bits.\n";
    out += "# mpv_render includes mpv's deliberate wait for the target time; late_ms is the real lateness.\n";
    out += "kind,t_ms,seq,flags,lead_ms,render_ms,finish_ms,late_ms,work_ms,block_ms,interval_ms,lost,"
           "dec_drops,vo_drops,avsync_ms,cache_s,cache_kBps,vf_fps,relay,"
           "pre_ms,update_ms,draw_ms,video_ms,gc_ms,gap_ms\n";

    // The three tables are each in time order: merge them.
    size_t fi = 0, pr = 0, si = 0;
    while (fi < g_nFrames || pr < g_nPresents || si < g_nStatus) {
        int64_t tf = fi < g_nFrames ? g_frames[fi].t : INT64_MAX;
        int64_t tp = pr < g_nPresents ? g_presents[pr].t : INT64_MAX;
        int64_t ts = si < g_nStatus ? g_status[si].t : INT64_MAX;
        if (tf <= tp && tf <= ts) {
            const FrameRow& f = g_frames[fi++];
            appendRow(out, 'F', f.t, f.seq, (int)f.mpvFlags);
            appendMs(out, f.lead);
            appendMs(out, f.render);
            appendMs(out, f.finish);
            appendMs(out, f.late);
            out += ",,,,,,,,,,,,,,,,,\n";  // 17 empty columns
        } else if (tp <= ts) {
            const PresentRow& p = g_presents[pr++];
            appendRow(out, 'P', p.t, p.seq, p.flags);
            out += ",,,,";
            appendMs(out, p.timing.work);
            appendMs(out, p.timing.block);
            appendMs(out, p.interval);
            snprintf(b, sizeof b, ",%u,,,,,,,", (unsigned)p.lost);
            out += b;
            appendMs(out, p.timing.pre);
            appendMs(out, p.timing.update);
            appendMs(out, p.timing.draw);
            appendMs(out, p.timing.video);
            appendMs(out, p.timing.gc);
            appendMs(out, p.timing.gap);
            out += '\n';
        } else {
            const StatusRow& r = g_status[si++];
            appendRow(out, 'S', r.t, 0, r.s.flags);
            snprintf(b, sizeof b, ",,,,,,,,,%lld,%lld,%.2f,%.2f,%.0f,%.2f,%d,,,,,,\n", (long long)r.s.decoderDrops,
                     (long long)r.s.voDrops, r.s.avsyncMs, r.s.cacheSec, r.s.cacheKBps, r.s.vfFps, r.s.relayTransfers);
            out += b;
        }
    }
    release();

    static int counter = 0;
    const std::string dir = "sdmc:/switch/NuvioNX/traces";
    sdfile::mkdirs(dir);
    snprintf(b, sizeof b, "%s/trace-%lld-%d.csv", dir.c_str(), (long long)time(nullptr), ++counter);
    std::string path = b;
    return sdfile::write(path, out) ? path : "";
}

}  // namespace playtrace
