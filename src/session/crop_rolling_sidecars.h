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

// Policy for input rows whose recording_frame_id falls in no requested range.
enum class RecordingFrameCsvOrphanRowPolicy {
    // Fail the split when any row matches no range. This is the default and
    // the required policy for crop metadata CSVs: every metadata row names a
    // frame that was accepted for encoding, so an orphan row is a hole in the
    // recorded data and must fail loudly.
    kFail,
    // Count and skip rows that match no range. Use for crop perf CSVs, where
    // rows are also written for frames dropped before external submission
    // (drop_reason set, no clip ever contains them), so head/tail orphans can
    // be legitimate.
    kCountAndSkip,
};

// Diagnostics reported by split_recording_frame_csv_by_ranges.
struct RecordingFrameCsvSplitStats {
    // Number of non-empty data rows whose recording_frame_id fell in no range.
    uint64_t orphan_rows = 0;
    // recording_frame_id of the first orphan row (0 when orphan_rows == 0).
    uint64_t first_orphan_recording_frame_id = 0;
};

// Splits a session-global root CSV (first column recording_frame_id) into one
// CSV per range. Ranges must be non-overlapping and contiguous: consecutive
// ranges (ordered by first_recording_frame_id) must satisfy
// next.first == previous.last + 1. The recorder assigns frames to clips as
// clip_index = (recording_frame_id - 1) / clip_span_frames with terminal-tail
// coalescing into the final clip, so reported clip ranges are contiguous
// unless frames were lost at a clip boundary; a gap therefore fails
// validation. The first range is not required to start at the session's first
// recording_frame_id; head/tail coverage is enforced by the orphan-row policy
// instead.
//
// When the header contains a crop_video_frame_index column, that column is
// rewritten per output starting at 0. A session_crop_video_frame_index column,
// when present, is preserved verbatim (it stays session-global); when absent,
// rows pass through unchanged apart from the crop_video_frame_index rewrite.
bool split_recording_frame_csv_by_ranges(
    const std::string& input_path,
    std::vector<RecordingFrameCsvRange>* ranges,
    std::string* error_out = nullptr,
    RecordingFrameCsvOrphanRowPolicy orphan_row_policy =
        RecordingFrameCsvOrphanRowPolicy::kFail,
    RecordingFrameCsvSplitStats* stats_out = nullptr);

}  // namespace orange::session
