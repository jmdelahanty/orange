#ifndef ORANGE_SPATIAL_SNAPSHOT_ALIGNMENT_H
#define ORANGE_SPATIAL_SNAPSHOT_ALIGNMENT_H

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

// Camera timestamps are PTP-domain nanoseconds, not CLOCK_REALTIME. A group
// starts three camera periods after the most recent observed frame so every
// worker can be armed before the same future frame arrives.
struct SpatialSnapshotAlignmentPlan {
    uint64_t first_camera_timestamp_ns = 0;
    uint64_t frame_period_ns = 0;
    uint64_t tolerance_ns = 0;

    bool enabled() const { return first_camera_timestamp_ns != 0; }
};

enum class SpatialSnapshotFrameDecision {
    wait,
    accept,
    missed,
};

inline bool make_spatial_snapshot_alignment_plan(
    const std::vector<uint64_t>& latest_camera_timestamps_ns,
    uint32_t frame_rate_hz,
    SpatialSnapshotAlignmentPlan* plan_out)
{
    if (plan_out == nullptr || latest_camera_timestamps_ns.empty() ||
        frame_rate_hz == 0 || frame_rate_hz > 1000000000u ||
        std::any_of(latest_camera_timestamps_ns.begin(),
                    latest_camera_timestamps_ns.end(),
                    [](uint64_t timestamp) { return timestamp == 0; })) {
        return false;
    }
    const uint64_t period_ns = 1000000000ull / frame_rate_hz;
    const uint64_t newest = *std::max_element(
        latest_camera_timestamps_ns.begin(), latest_camera_timestamps_ns.end());
    const uint64_t oldest = *std::min_element(
        latest_camera_timestamps_ns.begin(), latest_camera_timestamps_ns.end());
    const uint64_t tolerance_ns = std::min<uint64_t>(5000000ull, period_ns / 8u);
    if (period_ns == 0 || tolerance_ns == 0 ||
        newest - oldest > period_ns + tolerance_ns ||
        newest > std::numeric_limits<uint64_t>::max() - 3u * period_ns) {
        return false;
    }
    *plan_out = {newest + 3u * period_ns, period_ns, tolerance_ns};
    return true;
}

inline bool spatial_snapshot_expected_frame_timestamp(
    const SpatialSnapshotAlignmentPlan& plan,
    uint32_t frame_index,
    uint64_t* expected_out)
{
    if (!plan.enabled() || plan.frame_period_ns == 0 || expected_out == nullptr ||
        frame_index >
            (std::numeric_limits<uint64_t>::max() -
             plan.first_camera_timestamp_ns) / plan.frame_period_ns) {
        return false;
    }
    *expected_out = plan.first_camera_timestamp_ns +
                    static_cast<uint64_t>(frame_index) * plan.frame_period_ns;
    return true;
}

inline SpatialSnapshotFrameDecision spatial_snapshot_frame_decision(
    uint64_t observed_timestamp_ns,
    uint64_t expected_timestamp_ns,
    uint64_t tolerance_ns)
{
    if (observed_timestamp_ns == 0 || expected_timestamp_ns == 0 ||
        tolerance_ns == 0) {
        return SpatialSnapshotFrameDecision::missed;
    }
    if (observed_timestamp_ns < expected_timestamp_ns &&
        expected_timestamp_ns - observed_timestamp_ns > tolerance_ns) {
        return SpatialSnapshotFrameDecision::wait;
    }
    if (observed_timestamp_ns > expected_timestamp_ns &&
        observed_timestamp_ns - expected_timestamp_ns > tolerance_ns) {
        return SpatialSnapshotFrameDecision::missed;
    }
    return SpatialSnapshotFrameDecision::accept;
}

#endif
