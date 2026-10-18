#pragma once

#include <chrono>

namespace orange::control {

// Durations use Orange's monotonic clock; consumers need no shared clock epoch.
struct RecordingFinalizationTiming {
    bool started = false;
    bool stop_requested = false;
    double recording_duration_seconds = 0.0;
    double seconds_since_stop = 0.0;
};

inline RecordingFinalizationTiming ObserveRecordingFinalizationTiming(
    std::chrono::steady_clock::time_point start,
    std::chrono::steady_clock::time_point stop,
    std::chrono::steady_clock::time_point now) {
    RecordingFinalizationTiming result;
    if (start == std::chrono::steady_clock::time_point{} || now < start)
        return result;
    result.started = true;
    result.stop_requested = stop != std::chrono::steady_clock::time_point{} &&
                            stop >= start && stop <= now;
    result.recording_duration_seconds = std::chrono::duration<double>(
        (result.stop_requested ? stop : now) - start).count();
    if (result.stop_requested)
        result.seconds_since_stop = std::chrono::duration<double>(now - stop).count();
    return result;
}

}  // namespace orange::control
