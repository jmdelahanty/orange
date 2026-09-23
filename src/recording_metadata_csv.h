#pragma once

#include <cstdint>
#include <ostream>

namespace orange::recording_metadata {
// Keep the original columns and meanings. Both ID columns are the parent
// recording sequence, never the stream-local or hardware counter.
inline void write_header(std::ostream& out) {
    out << "frame_id,timestamp,timestamp_sys,recording_frame_id\n";
}

inline void write_row(std::ostream& out, uint64_t recording_frame_id,
                      uint64_t timestamp, uint64_t timestamp_sys) {
    out << recording_frame_id << ',' << timestamp << ',' << timestamp_sys
        << ',' << recording_frame_id << '\n';
}
}  // namespace orange::recording_metadata
