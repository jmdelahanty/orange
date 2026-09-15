#ifndef ORANGE_SPATIAL_SNAPSHOT_ALIGNMENT_H
#define ORANGE_SPATIAL_SNAPSHOT_ALIGNMENT_H

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

// Camera timestamps are PTP-domain nanoseconds, not CLOCK_REALTIME. Allow a
// wall-time arming interval as well as three camera periods: three periods are
// only 30 ms at 100 fps, too short for a four-camera GUI capture to dispatch.
constexpr uint64_t kSpatialSnapshotMinArmLeadNs = 1000000000ull;
// Full-resolution snapshot conversion is intentionally off the acquisition
// hot path and cannot sustain one 20 MP copy per camera period at 100 fps.
// Temporal means therefore sample a shared, lower-rate subset of the PTP
// cadence while retaining exact cross-camera frame correspondence.
constexpr uint64_t kSpatialSnapshotMinSampleIntervalNs = 500000000ull;
struct SpatialSnapshotAlignmentPlan {
    uint64_t first_camera_timestamp_ns = 0;
    uint64_t frame_period_ns = 0;
    uint64_t tolerance_ns = 0;
    uint32_t capture_stride_frames = 1;

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
    const uint64_t min_arm_periods =
        (kSpatialSnapshotMinArmLeadNs + period_ns - 1u) / period_ns;
    const uint64_t arm_periods = std::max<uint64_t>(3u, min_arm_periods);
    const uint64_t capture_stride_frames = std::max<uint64_t>(
        1u,
        (kSpatialSnapshotMinSampleIntervalNs + period_ns - 1u) / period_ns);
    if (period_ns == 0 || tolerance_ns == 0 ||
        newest - oldest > period_ns + tolerance_ns ||
        capture_stride_frames > std::numeric_limits<uint32_t>::max() ||
        arm_periods > (std::numeric_limits<uint64_t>::max() - newest) / period_ns) {
        return false;
    }
    *plan_out = {
        newest + arm_periods * period_ns,
        period_ns,
        tolerance_ns,
        static_cast<uint32_t>(capture_stride_frames),
    };
    return true;
}

inline bool spatial_snapshot_expected_frame_timestamp(
    const SpatialSnapshotAlignmentPlan& plan,
    uint32_t frame_index,
    uint64_t* expected_out)
{
    if (!plan.enabled() || plan.frame_period_ns == 0 ||
        plan.capture_stride_frames == 0 || expected_out == nullptr ||
        plan.capture_stride_frames >
            std::numeric_limits<uint64_t>::max() / plan.frame_period_ns) {
        return false;
    }
    const uint64_t capture_interval_ns =
        plan.frame_period_ns * plan.capture_stride_frames;
    if (
        frame_index >
            (std::numeric_limits<uint64_t>::max() -
             plan.first_camera_timestamp_ns) / capture_interval_ns) {
        return false;
    }
    *expected_out = plan.first_camera_timestamp_ns +
                    static_cast<uint64_t>(frame_index) * capture_interval_ns;
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
