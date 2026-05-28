#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace orange::session {

struct RecordingFrameCsvRange {
    uint64_t first_recording_frame_id = 0;
    uint64_t last_recording_frame_id = 0;
    std::string output_path;
    uint64_t rows_written = 0;
};

bool split_recording_frame_csv_by_ranges(
    const std::string& input_path,
    std::vector<RecordingFrameCsvRange>* ranges,
    std::string* error_out = nullptr);

}  // namespace orange::session
