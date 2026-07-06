#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
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

// ---------------------------------------------------------------------------
// Incremental single-clip splitting (append_clip_rows_from_root_csv).
//
// split_recording_frame_csv_by_ranges re-reads the whole root CSV per call,
// which is fine at finalize but not for per-clip completion during a long
// recording. The incremental helper below extracts one completed clip's rows
// from the (still growing) root CSV per call, resuming from a caller-owned
// cursor so each root-CSV byte is read once across the whole session.
// ---------------------------------------------------------------------------

// Caller-owned resume state for append_clip_rows_from_root_csv. Default
// construct once per root CSV and pass the same instance to every call; the
// fields are managed by the helper and should not be mutated by callers.
struct IncrementalClipSplitCursor {
    // File offset of the first byte the helper has not fully consumed. This
    // is always the start of a line (immediately after the header once the
    // header has been parsed) and only advances past fully-consumed lines.
    uint64_t byte_offset = 0;
    // Trailing partial line (bytes at byte_offset with no '\n' yet — the
    // writer may be mid-row). Stashed here and retried on the next call,
    // which resumes reading the file at byte_offset + pending_partial_line
    // .size() and prepends the stash.
    std::string pending_partial_line;
    // Header state captured by the first call that saw a complete header
    // line. The header text is replayed into each per-clip output CSV.
    bool header_parsed = false;
    std::string header;
    size_t crop_video_frame_index_column = std::numeric_limits<size_t>::max();
    // Highest recording_frame_id consumed into a clip so far. Used to reject
    // clip ranges that would leave a gap (see gap policy below).
    bool any_row_consumed = false;
    uint64_t last_consumed_recording_frame_id = 0;
    // State of the clip currently being extracted, so a retry after an
    // incomplete call appends to the same output (header written once,
    // crop_video_frame_index numbering continues).
    uint64_t active_clip_first_recording_frame_id = 0;
    uint64_t active_clip_last_recording_frame_id = 0;
    uint64_t active_clip_rows_written = 0;
    bool active_clip_output_created = false;
};

enum class RecordingFrameCsvClipSplitStatus {
    // The call failed; RecordingFrameCsvSplitResult::error explains why. The
    // cursor still reflects every row consumed before the failure.
    kFailed,
    // The root CSV ended (or ended in a partial line) before the clip's rows
    // were all seen — the writer has not flushed the tail yet. Not an error:
    // rows seen so far are already in the output CSV and the cursor is
    // positioned to resume; call again later with the same arguments.
    kIncomplete,
    // The clip's rows are finished: either the row with the range's
    // last_recording_frame_id was consumed, or a row past the range was
    // encountered (left unconsumed for the next clip's call).
    kCompleted,
};

// Result of one append_clip_rows_from_root_csv call.
struct RecordingFrameCsvSplitResult {
    RecordingFrameCsvClipSplitStatus status =
        RecordingFrameCsvClipSplitStatus::kFailed;
    // Data rows appended to the output CSV by this call.
    uint64_t rows_written = 0;
    // Cumulative data rows written for this clip across calls.
    uint64_t clip_rows_written = 0;
    // Orphan rows (below the clip range, never consumed into any clip)
    // observed by this call. Under kFail policy the call fails on the first
    // orphan; under kCountAndSkip orphans are counted and skipped, matching
    // split_recording_frame_csv_by_ranges semantics.
    RecordingFrameCsvSplitStats stats;
    std::string error;
};

// Appends one completed clip's rows from the session-root CSV to
// output_csv_path, resuming from *cursor. Column semantics match
// split_recording_frame_csv_by_ranges exactly: crop_video_frame_index (when
// present) is rewritten from 0 per output clip (numbering continues across
// incomplete retries of the same clip); session_crop_video_frame_index is
// preserved verbatim; old-schema files without crop_video_frame_index pass
// rows through unchanged.
//
// Contract:
// - clip_range must be a valid range (non-zero ids, first <= last);
//   clip_range.output_path and clip_range.rows_written are ignored — the
//   output file is output_csv_path and per-call counts are in the result.
// - Rows with recording_frame_id above the range are never consumed: the
//   cursor stops at the start of that line so the next clip's call resumes
//   exactly there.
// - Gap policy (same spirit as validate_ranges): once the cursor has
//   consumed clip rows, a new clip range must start at exactly
//   last_consumed_recording_frame_id + 1. Re-calling with the active clip's
//   own range resumes it instead.
// - The first call for a clip creates output_csv_path (truncating) and
//   writes the header; later calls for the same clip append rows only.
RecordingFrameCsvSplitResult append_clip_rows_from_root_csv(
    const std::string& root_csv_path,
    IncrementalClipSplitCursor* cursor,
    const RecordingFrameCsvRange& clip_range,
    const std::string& output_csv_path,
    RecordingFrameCsvOrphanRowPolicy orphan_row_policy =
        RecordingFrameCsvOrphanRowPolicy::kFail);

}  // namespace orange::session
