#pragma once

// Camera frame-id gap counting for the EVT cameras.
//
// The camera numbers its frames 1..65535 and then starts again at 1: frame 0
// is never sent (seen on the rig, 2026-11-10: 65534, 65535, 1, 2). A frame id
// of 0 therefore means "no previous frame" in the acquisition state.

#include <cstdint>

namespace orange::camera_frame_id {

constexpr uint32_t kFirstId = 1;
constexpr uint32_t kLastId = 65535;
constexpr uint32_t kCycleLength = kLastId - kFirstId + 1;  // 65535 ids

// Frames missing between two consecutively received frame ids, across the
// wrap. 0 when there is no previous frame, when the id is the invalid 0, or
// when the id repeats (a repeat cannot be told apart from a whole lost cycle;
// the timestamp check in the validator catches that case).
inline uint64_t MissingBetween(uint16_t prev_id, uint16_t current_id)
{
    if (prev_id == 0 || current_id == 0) {
        return 0;
    }
    const uint32_t steps =
        (static_cast<uint32_t>(current_id) + kCycleLength - static_cast<uint32_t>(prev_id)) %
        kCycleLength;
    return steps == 0 ? 0 : steps - 1;
}

}  // namespace orange::camera_frame_id
