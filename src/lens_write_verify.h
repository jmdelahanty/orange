// Pure helpers for verifying EF-mount lens writes.
//
// The Emergent EF mount silently drops a Focus/Iris write that arrives while
// LensBusy is true, yet the commanded register still reads back the new value.
// The only truthful feedback is IrisCurrent/FocusCurrent, which update when the
// move completes. These helpers are SDK-free so they can be unit tested; the
// camera layer supplies the pollers.
#pragma once

#include <functional>

namespace orange::lens {

struct LensPollers {
    // Return false when the node cannot be read (old firmware, transport error).
    std::function<bool(bool* busy)> read_busy;
    std::function<bool(unsigned int* current)> read_current;
    std::function<double()> now_ms;
    std::function<void()> sleep_poll_interval;
};

struct WaitIdleResult {
    bool idle = false;
    bool busy_readable = true;
    double waited_ms = 0.0;
    int polls = 0;
};

// Poll LensBusy until it clears or timeout_ms elapses. An unreadable LensBusy
// node counts as idle (the caller falls back to legacy behaviour).
inline WaitIdleResult wait_lens_idle(const LensPollers& p, double timeout_ms)
{
    WaitIdleResult r;
    const double t0 = p.now_ms();
    for (;;) {
        bool busy = false;
        ++r.polls;
        if (!p.read_busy(&busy)) {
            r.busy_readable = false;
            r.idle = true;
            break;
        }
        r.waited_ms = p.now_ms() - t0;
        if (!busy) {
            r.idle = true;
            break;
        }
        if (r.waited_ms >= timeout_ms) {
            r.idle = false;
            break;
        }
        p.sleep_poll_interval();
    }
    r.waited_ms = p.now_ms() - t0;
    return r;
}

struct SettleResult {
    bool settled = false;          // busy cleared and current == target
    bool current_readable = true;  // false: no *Current node, verification impossible
    bool busy_readable = true;
    bool busy_seen = false;        // the mount reported a move in progress
    unsigned int current = 0;      // last value read from the *Current node
    double settle_ms = 0.0;        // time from call start to settled (or timeout)
    int polls = 0;
};

inline unsigned int abs_diff(unsigned int a, unsigned int b) { return a > b ? a - b : b - a; }

// After a write, poll until LensBusy is false and *Current is within
// `tolerance` counts of target. Focus encoders land a count or two off the
// commanded position (3168 -> 3169 was measured), so focus uses a small
// tolerance; iris is exact.
inline SettleResult wait_lens_settled(const LensPollers& p, unsigned int target, double timeout_ms,
                                      unsigned int tolerance = 0)
{
    SettleResult r;
    const double t0 = p.now_ms();
    for (;;) {
        ++r.polls;
        bool busy = false;
        if (!p.read_busy(&busy)) {
            r.busy_readable = false;
            busy = false;
        }
        if (busy) {
            r.busy_seen = true;
        }
        unsigned int current = 0;
        if (!p.read_current(&current)) {
            r.current_readable = false;
            r.settle_ms = p.now_ms() - t0;
            return r;
        }
        r.current = current;
        r.settle_ms = p.now_ms() - t0;
        if (!busy && abs_diff(current, target) <= tolerance) {
            r.settled = true;
            return r;
        }
        if (r.settle_ms >= timeout_ms) {
            return r;
        }
        p.sleep_poll_interval();
    }
}

}  // namespace orange::lens
