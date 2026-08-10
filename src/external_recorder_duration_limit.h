#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace orange::external_recorder {

// Independent frame-count backstop for a recorder whose primary lifetime is
// controlled by Orange's monotonic session deadline.  The grace admits the
// frames already in flight while the normal drain request crosses the IPC
// boundary, but it is deliberately bounded to two nominal seconds or one GOP,
// whichever is larger. Two seconds covers the configured 32-frame pipeline at
// 30 FPS without turning the backstop into another long-running timer.
struct DurationSafetyLimit {
    bool enabled = false;
    uint64_t target_frame_count = 0;
    uint64_t grace_frame_count = 0;
    uint64_t ceiling_frame_count = 0;
    std::string policy = "disabled_unbounded_duration";
};

inline bool ResolveDurationSafetyLimit(const uint64_t record_for_seconds,
                                       const uint64_t fps,
                                       const uint64_t gop,
                                       DurationSafetyLimit* limit_out,
                                       std::string* error_out = nullptr)
{
    if (!limit_out) {
        if (error_out) {
            *error_out = "duration safety limit destination is null";
        }
        return false;
    }

    DurationSafetyLimit limit;
    if (record_for_seconds == 0) {
        *limit_out = std::move(limit);
        if (error_out) {
            error_out->clear();
        }
        return true;
    }
    if (fps == 0) {
        if (error_out) {
            *error_out = "duration safety limit requires a positive frame rate";
        }
        return false;
    }
    if (record_for_seconds > std::numeric_limits<uint64_t>::max() / fps) {
        if (error_out) {
            *error_out = "duration target frame count overflows uint64";
        }
        return false;
    }

    limit.enabled = true;
    limit.target_frame_count = record_for_seconds * fps;
    if (fps > std::numeric_limits<uint64_t>::max() / 2) {
        if (error_out) {
            *error_out = "duration safety grace frame count overflows uint64";
        }
        return false;
    }
    limit.grace_frame_count =
        std::max<uint64_t>(fps * 2, std::max<uint64_t>(gop, 1));
    if (limit.target_frame_count >
        std::numeric_limits<uint64_t>::max() - limit.grace_frame_count) {
        if (error_out) {
            *error_out = "duration safety ceiling overflows uint64";
        }
        return false;
    }
    limit.ceiling_frame_count =
        limit.target_frame_count + limit.grace_frame_count;
    limit.policy = "target_plus_max_two_seconds_or_one_gop";
    *limit_out = std::move(limit);
    if (error_out) {
        error_out->clear();
    }
    return true;
}

}  // namespace orange::external_recorder
