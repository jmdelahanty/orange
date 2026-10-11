// src/timing/clock.h
//
// The one clock vocabulary for Orange, the external recorder and the tools.
// Design: docs/unified_timing_and_diagnostics_design_2026_10_11.md, section 2.
//
// Four time bases, never mixed:
//   CameraNs     the Emergent camera's frame timestamp (camera / PTP clock).
//                Written as `timestamp`, `camera_timestamp_ns`.
//   RealtimeNs   the host wall clock, CLOCK_REALTIME (== system_clock on
//                Linux): ns since 1970-01-01 UTC. Comparable across machines,
//                but it can jump when the host clock is stepped. Written as
//                `timestamp_sys`, `host_realtime_ns`, `*_realtime_ns`.
//   MonotonicNs  the host monotonic clock, CLOCK_MONOTONIC (== steady_clock
//                in libstdc++, == Python time.monotonic_ns()). Never jumps;
//                meaningless across a reboot or another machine. Every
//                duration and latency. Written as `*_steady_ns`, `*_host_ns`.
//   GPU elapsed  cudaEventElapsedTime durations; not represented here.
//
// The helpers read exactly the clocks the code read before this header
// existed, so moving a call site here never changes a value.
#pragma once

#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

namespace orange::timing {

struct CameraNs {
    uint64_t value = 0;
};
struct RealtimeNs {
    uint64_t value = 0;
};
struct MonotonicNs {
    uint64_t value = 0;
};
// A difference of two stamps of ONE time base.
struct DurationNs {
    int64_t value = 0;
    double ms() const { return static_cast<double>(value) / 1e6; }
};

inline DurationNs between(CameraNs from, CameraNs to)
{
    return {static_cast<int64_t>(to.value) - static_cast<int64_t>(from.value)};
}
inline DurationNs between(RealtimeNs from, RealtimeNs to)
{
    return {static_cast<int64_t>(to.value) - static_cast<int64_t>(from.value)};
}
inline DurationNs between(MonotonicNs from, MonotonicNs to)
{
    return {static_cast<int64_t>(to.value) - static_cast<int64_t>(from.value)};
}
// No overloads across time bases: between(MonotonicNs, RealtimeNs) does not compile.

inline uint64_t monotonic_now_ns()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
inline uint64_t realtime_now_ns()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}
inline uint64_t monotonic_now_us() { return monotonic_now_ns() / 1000; }
inline uint64_t realtime_now_us() { return realtime_now_ns() / 1000; }

inline MonotonicNs monotonic_now() { return {monotonic_now_ns()}; }
inline RealtimeNs realtime_now() { return {realtime_now_ns()}; }

// Monotonic and realtime read back to back (monotonic, realtime, monotonic),
// stamped at the midpoint of the two monotonic reads; uncertainty_ns is the
// gap between them. Places any monotonic stamp of this boot on the calendar.
struct ClockAnchor {
    MonotonicNs monotonic;
    RealtimeNs realtime;
    uint64_t uncertainty_ns = 0;
};
inline ClockAnchor anchor_now()
{
    const uint64_t before = monotonic_now_ns();
    const uint64_t wall = realtime_now_ns();
    const uint64_t after = monotonic_now_ns();
    return {{before + (after - before) / 2}, {wall}, after - before};
}

// Calendar strings. Thread-safe (gmtime_r / localtime_r).
//
// utc_iso8601(t) gives "YYYY-MM-DDTHH:MM:SSZ" (one-second resolution), the
// format of every existing *_utc field; fractional_digits (1..9) adds
// ".fff" before the Z.
inline std::string utc_iso8601(RealtimeNs t, int fractional_digits = 0)
{
    const std::time_t seconds = static_cast<std::time_t>(t.value / 1000000000ULL);
    std::tm utc{};
    gmtime_r(&seconds, &utc);
    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S");
    if (fractional_digits > 0) {
        if (fractional_digits > 9) {
            fractional_digits = 9;
        }
        uint64_t fraction = t.value % 1000000000ULL;
        for (int i = fractional_digits; i < 9; ++i) {
            fraction /= 10;
        }
        out << '.' << std::setfill('0') << std::setw(fractional_digits) << fraction;
    }
    out << 'Z';
    return out.str();
}
inline std::string utc_iso8601_now(int fractional_digits = 0)
{
    return utc_iso8601(realtime_now(), fractional_digits);
}

// Host LOCAL time, no zone marker: strftime `format` (for example
// "%Y_%m_%d_%H_%M_%S", the recording folder name). Names and labels only.
inline std::string local_time_string(RealtimeNs t, const char* format)
{
    const std::time_t seconds = static_cast<std::time_t>(t.value / 1000000000ULL);
    std::tm local{};
    localtime_r(&seconds, &local);
    std::ostringstream out;
    out << std::put_time(&local, format);
    return out.str();
}

} // namespace orange::timing
