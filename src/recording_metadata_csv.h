#pragma once

#include <cstdint>
#include <ostream>

namespace orange::recording_metadata {
// Leading columns shared with the external recorder's metadata CSV
// (agent-contracts citrus-orange/frame_identity_v1): recording_frame_id is the
// parent recording sequence, never the stream or hardware counter.
inline void write_header(std::ostream& out) {
    out << "recording_frame_id,timestamp,timestamp_sys\n";
}

inline void write_row(std::ostream& out, uint64_t recording_frame_id,
                      uint64_t timestamp, uint64_t timestamp_sys) {
    out << recording_frame_id << ',' << timestamp << ',' << timestamp_sys << '\n';
}
}  // namespace orange::recording_metadata
