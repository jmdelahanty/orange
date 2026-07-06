#include "session/crop_rolling_sidecars.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::filesystem::path make_temp_dir(const std::string& name)
{
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("orange_crop_rolling_sidecars_" + name + "_" + std::to_string(stamp));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

std::vector<std::string> read_lines(const std::filesystem::path& path)
{
    std::ifstream input(path);
    require(static_cast<bool>(input), "failed to open " + path.string());
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        lines.push_back(line);
    }
    return lines;
}

void write_text(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path);
    require(static_cast<bool>(output), "failed to write " + path.string());
    output << text;
}

void append_text(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream output(path, std::ios::out | std::ios::app);
    require(static_cast<bool>(output), "failed to append " + path.string());
    output << text;
}

std::string read_text(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::in | std::ios::binary);
    require(static_cast<bool>(input), "failed to open " + path.string());
    std::ostringstream out;
    out << input.rdbuf();
    return out.str();
}

void test_splits_crop_metadata_by_recording_frame_range()
{
    const std::filesystem::path dir = make_temp_dir("split");
    const std::filesystem::path input = dir / "Cam2010096_crop_meta.csv";
    write_text(
        input,
        "recording_frame_id,local_frame_id,crop_x,crop_y\n"
        "1,101,10,20\n"
        "2,102,11,21\n"
        "3,103,12,22\n"
        "4,104,13,23\n"
        "5,105,14,24\n");

    std::vector<orange::session::RecordingFrameCsvRange> ranges = {
        {1, 2, (dir / "clips" / "clip_000000" / "Cam2010096_crop_meta.csv").string()},
        {3, 5, (dir / "clips" / "clip_000001" / "Cam2010096_crop_meta.csv").string()},
    };
    std::string error;
    require(
        orange::session::split_recording_frame_csv_by_ranges(
            input.string(),
            &ranges,
            &error),
        "split should pass: " + error);
    require(ranges[0].rows_written == 2, "first clip row count");
    require(ranges[1].rows_written == 3, "second clip row count");

    const auto first_lines = read_lines(ranges[0].output_path);
    const auto second_lines = read_lines(ranges[1].output_path);
    require(first_lines.size() == 3, "first output line count");
    require(second_lines.size() == 4, "second output line count");
    require(first_lines[0] == "recording_frame_id,local_frame_id,crop_x,crop_y", "header preserved");
    require(first_lines[1] == "1,101,10,20", "first clip first data row");
    require(first_lines[2] == "2,102,11,21", "first clip second data row");
    require(second_lines[1] == "3,103,12,22", "second clip first data row");
    require(second_lines[3] == "5,105,14,24", "second clip last data row");
    std::filesystem::remove_all(dir);
}

void test_rewrites_crop_video_frame_index_per_split_output()
{
    const std::filesystem::path dir = make_temp_dir("split_crop_index");
    const std::filesystem::path input = dir / "Cam2010096_crop_meta.csv";
    write_text(
        input,
        "recording_frame_id,local_frame_id,crop_video_frame_index,crop_state\n"
        "1,101,0,detected_crop\n"
        "2,102,1,detected_crop\n"
        "3,103,2,detected_crop\n"
        "4,104,3,detected_crop\n"
        "5,105,4,detected_crop\n");

    std::vector<orange::session::RecordingFrameCsvRange> ranges = {
        {1, 2, (dir / "clips" / "clip_000000" / "Cam2010096_crop_meta.csv").string()},
        {3, 5, (dir / "clips" / "clip_000001" / "Cam2010096_crop_meta.csv").string()},
    };
    std::string error;
    require(
        orange::session::split_recording_frame_csv_by_ranges(
            input.string(),
            &ranges,
            &error),
        "split should pass: " + error);

    const auto first_lines = read_lines(ranges[0].output_path);
    const auto second_lines = read_lines(ranges[1].output_path);
    require(first_lines[1] == "1,101,0,detected_crop", "first clip index starts at 0");
    require(first_lines[2] == "2,102,1,detected_crop", "first clip index increments");
    require(second_lines[1] == "3,103,0,detected_crop", "second clip index resets to 0");
    require(second_lines[3] == "5,105,2,detected_crop", "second clip index increments");
    std::filesystem::remove_all(dir);
}

void test_counts_and_skips_orphan_rows_when_policy_allows()
{
    const std::filesystem::path dir = make_temp_dir("subset");
    const std::filesystem::path input = dir / "Cam2010096_crop_perf.csv";
    write_text(
        input,
        "recording_frame_id,total_ms\n"
        "1,0.1\n"
        "2,0.2\n"
        "3,0.3\n"
        "4,0.4\n");

    std::vector<orange::session::RecordingFrameCsvRange> ranges = {
        {2, 3, (dir / "clip.csv").string()},
    };
    std::string error;
    orange::session::RecordingFrameCsvSplitStats stats;
    require(
        orange::session::split_recording_frame_csv_by_ranges(
            input.string(),
            &ranges,
            &error,
            orange::session::RecordingFrameCsvOrphanRowPolicy::kCountAndSkip,
            &stats),
        "subset split should pass: " + error);
    require(ranges[0].rows_written == 2, "subset row count");
    require(stats.orphan_rows == 2, "subset orphan row count");
    require(stats.first_orphan_recording_frame_id == 1,
            "subset first orphan recording_frame_id");
    const auto lines = read_lines(ranges[0].output_path);
    require(lines.size() == 3, "subset output line count");
    require(lines[1] == "2,0.2", "subset first row");
    require(lines[2] == "3,0.3", "subset last row");
    std::filesystem::remove_all(dir);
}

void test_fails_on_orphan_rows_by_default()
{
    const std::filesystem::path dir = make_temp_dir("orphan_fail");
    const std::filesystem::path input = dir / "Cam2010096_crop_meta.csv";
    write_text(
        input,
        "recording_frame_id,local_frame_id\n"
        "1,101\n"
        "2,102\n"
        "3,103\n"
        "4,104\n"
        "7,107\n");

    // Contiguous ranges [2,3] + [4,6]; rows 1 and 7 fall outside every range.
    std::vector<orange::session::RecordingFrameCsvRange> ranges = {
        {2, 3, (dir / "a.csv").string()},
        {4, 6, (dir / "b.csv").string()},
    };
    std::string error;
    orange::session::RecordingFrameCsvSplitStats stats;
    require(
        !orange::session::split_recording_frame_csv_by_ranges(
            input.string(),
            &ranges,
            &error,
            orange::session::RecordingFrameCsvOrphanRowPolicy::kFail,
            &stats),
        "orphan rows should fail the split by default");
    require(error.find("2 row(s)") != std::string::npos,
            "orphan error should name the orphan row count: " + error);
    require(error.find("first orphaned recording_frame_id 1") != std::string::npos,
            "orphan error should name the first orphaned recording_frame_id: " + error);
    require(stats.orphan_rows == 2, "orphan stats row count");
    require(stats.first_orphan_recording_frame_id == 1,
            "orphan stats first recording_frame_id");
    std::filesystem::remove_all(dir);
}

void test_rejects_gap_between_consecutive_ranges()
{
    const std::filesystem::path dir = make_temp_dir("gap");
    const std::filesystem::path input = dir / "input.csv";
    write_text(input, "recording_frame_id,total_ms\n1,0.1\n");
    std::vector<orange::session::RecordingFrameCsvRange> ranges = {
        {1, 3, (dir / "a.csv").string()},
        {5, 6, (dir / "b.csv").string()},
    };
    std::string error;
    require(
        !orange::session::split_recording_frame_csv_by_ranges(
            input.string(),
            &ranges,
            &error),
        "gapped ranges should fail");
    require(error.find("gap between recording_frame_id ranges") != std::string::npos,
            "gap error should mention the gap: " + error);
    require(error.find("ending at 3") != std::string::npos,
            "gap error should name the lower boundary: " + error);
    require(error.find("starting at 5") != std::string::npos,
            "gap error should name the upper boundary: " + error);
    require(error.find("expected 4") != std::string::npos,
            "gap error should name the expected next frame id: " + error);
    std::filesystem::remove_all(dir);
}

void test_preserves_session_crop_video_frame_index_verbatim()
{
    const std::filesystem::path dir = make_temp_dir("session_index");
    const std::filesystem::path input = dir / "Cam2010096_crop_meta.csv";
    write_text(
        input,
        "recording_frame_id,local_frame_id,crop_video_frame_index,"
        "selection_policy,session_crop_video_frame_index\n"
        "1,101,0,largest_detection_by_confidence,0\n"
        "2,102,1,largest_detection_by_confidence,1\n"
        "3,103,2,largest_detection_by_confidence,2\n"
        "4,104,3,largest_detection_by_confidence,3\n"
        "5,105,4,largest_detection_by_confidence,4\n");

    std::vector<orange::session::RecordingFrameCsvRange> ranges = {
        {1, 2, (dir / "clips" / "clip_000000" / "Cam2010096_crop_meta.csv").string()},
        {3, 5, (dir / "clips" / "clip_000001" / "Cam2010096_crop_meta.csv").string()},
    };
    std::string error;
    require(
        orange::session::split_recording_frame_csv_by_ranges(
            input.string(),
            &ranges,
            &error),
        "session index split should pass: " + error);

    const auto first_lines = read_lines(ranges[0].output_path);
    const auto second_lines = read_lines(ranges[1].output_path);
    require(
        first_lines[0] ==
            "recording_frame_id,local_frame_id,crop_video_frame_index,"
            "selection_policy,session_crop_video_frame_index",
        "session index header preserved");
    require(first_lines[1] == "1,101,0,largest_detection_by_confidence,0",
            "first clip keeps session index 0 and clip index 0");
    require(first_lines[2] == "2,102,1,largest_detection_by_confidence,1",
            "first clip keeps session index 1 and clip index 1");
    require(second_lines[1] == "3,103,0,largest_detection_by_confidence,2",
            "second clip rewrites clip index to 0 but keeps session index 2");
    require(second_lines[2] == "4,104,1,largest_detection_by_confidence,3",
            "second clip rewrites clip index to 1 but keeps session index 3");
    require(second_lines[3] == "5,105,2,largest_detection_by_confidence,4",
            "second clip rewrites clip index to 2 but keeps session index 4");
    std::filesystem::remove_all(dir);
}

void test_rejects_invalid_csv_contracts()
{
    const std::filesystem::path dir = make_temp_dir("invalid");
    const std::filesystem::path bad_header = dir / "bad_header.csv";
    write_text(bad_header, "frame_id,total_ms\n1,0.1\n");
    std::vector<orange::session::RecordingFrameCsvRange> ranges = {
        {1, 1, (dir / "out.csv").string()},
    };
    std::string error;
    require(
        !orange::session::split_recording_frame_csv_by_ranges(
            bad_header.string(),
            &ranges,
            &error),
        "bad header should fail");
    require(
        error.find("recording_frame_id") != std::string::npos,
        "bad header error should mention recording_frame_id");

    const std::filesystem::path bad_row = dir / "bad_row.csv";
    write_text(bad_row, "recording_frame_id,total_ms\nx,0.1\n");
    error.clear();
    require(
        !orange::session::split_recording_frame_csv_by_ranges(
            bad_row.string(),
            &ranges,
            &error),
        "bad row should fail");
    require(error.find("invalid recording_frame_id") != std::string::npos,
            "bad row error should mention invalid frame id");
    std::filesystem::remove_all(dir);
}

void test_rejects_overlapping_ranges()
{
    const std::filesystem::path dir = make_temp_dir("overlap");
    const std::filesystem::path input = dir / "input.csv";
    write_text(input, "recording_frame_id,total_ms\n1,0.1\n");
    std::vector<orange::session::RecordingFrameCsvRange> ranges = {
        {1, 3, (dir / "a.csv").string()},
        {3, 4, (dir / "b.csv").string()},
    };
    std::string error;
    require(
        !orange::session::split_recording_frame_csv_by_ranges(
            input.string(),
            &ranges,
            &error),
        "overlapping ranges should fail");
    require(error.find("overlapping") != std::string::npos,
            "overlap error should mention overlapping ranges");
    std::filesystem::remove_all(dir);
}

using orange::session::IncrementalClipSplitCursor;
using orange::session::RecordingFrameCsvClipSplitStatus;
using orange::session::RecordingFrameCsvOrphanRowPolicy;
using orange::session::RecordingFrameCsvRange;
using orange::session::RecordingFrameCsvSplitResult;
using orange::session::append_clip_rows_from_root_csv;

void test_incremental_split_matches_whole_file_split()
{
    const std::filesystem::path dir = make_temp_dir("incremental_equivalence");
    // 3 clips of 3 frames each, with both crop_video_frame_index (rewritten
    // per clip) and session_crop_video_frame_index (verbatim) columns.
    const std::string header =
        "recording_frame_id,local_frame_id,crop_video_frame_index,"
        "selection_policy,session_crop_video_frame_index\n";
    std::vector<std::string> rows;
    for (uint64_t id = 1; id <= 9; ++id) {
        rows.push_back(
            std::to_string(id) + ',' + std::to_string(100 + id) + ',' +
            std::to_string(id - 1) + ",largest_detection_by_confidence," +
            std::to_string(id - 1) + '\n');
    }

    // Whole-file reference split.
    const std::filesystem::path whole_input = dir / "whole_root.csv";
    std::string whole_text = header;
    for (const auto& row : rows) {
        whole_text += row;
    }
    write_text(whole_input, whole_text);
    std::vector<RecordingFrameCsvRange> ranges = {
        {1, 3, (dir / "whole" / "clip0.csv").string()},
        {4, 6, (dir / "whole" / "clip1.csv").string()},
        {7, 9, (dir / "whole" / "clip2.csv").string()},
    };
    std::string error;
    require(
        orange::session::split_recording_frame_csv_by_ranges(
            whole_input.string(), &ranges, &error),
        "whole-file reference split should pass: " + error);

    // Incremental split over a root CSV grown in awkward pieces: partial
    // header, partial rows, clip boundaries mid-append.
    const std::filesystem::path incr_input = dir / "incr_root.csv";
    write_text(incr_input, header.substr(0, 20));  // partial header only
    IncrementalClipSplitCursor cursor;
    const std::string clip0 = (dir / "incr" / "clip0.csv").string();
    const std::string clip1 = (dir / "incr" / "clip1.csv").string();
    const std::string clip2 = (dir / "incr" / "clip2.csv").string();

    RecordingFrameCsvSplitResult result =
        append_clip_rows_from_root_csv(incr_input.string(), &cursor,
                                       RecordingFrameCsvRange{1, 3, {}}, clip0);
    require(result.status == RecordingFrameCsvClipSplitStatus::kIncomplete,
            "partial header should report incomplete");
    require(result.rows_written == 0, "partial header should write no rows");

    // Rest of header + row 1 + half of row 2.
    append_text(incr_input, header.substr(20) + rows[0] + rows[1].substr(0, 4));
    result = append_clip_rows_from_root_csv(
        incr_input.string(), &cursor, RecordingFrameCsvRange{1, 3, {}}, clip0);
    require(result.status == RecordingFrameCsvClipSplitStatus::kIncomplete,
            "clip 0 should be incomplete after row 1");
    require(result.rows_written == 1, "clip 0 first call should write row 1");
    require(cursor.pending_partial_line == rows[1].substr(0, 4),
            "partial row 2 should be stashed in pending_partial_line");

    // Rest of row 2, row 3, and row 4 (clip 1's first row) plus a partial
    // row 5 arrive together: the call must complete clip 0 and leave rows
    // 4+ unconsumed.
    append_text(
        incr_input, rows[1].substr(4) + rows[2] + rows[3] + rows[4].substr(0, 2));
    result = append_clip_rows_from_root_csv(
        incr_input.string(), &cursor, RecordingFrameCsvRange{1, 3, {}}, clip0);
    require(result.status == RecordingFrameCsvClipSplitStatus::kCompleted,
            "clip 0 should complete");
    require(result.rows_written == 2, "clip 0 second call should write rows 2-3");
    require(result.clip_rows_written == 3, "clip 0 cumulative rows");

    // Clip 1: rows 4-6. Row 4 is already in the file; row 5 completes and
    // row 6 arrives without its trailing newline (writer mid-row).
    append_text(
        incr_input, rows[4].substr(2) + rows[5].substr(0, rows[5].size() - 1));
    result = append_clip_rows_from_root_csv(
        incr_input.string(), &cursor, RecordingFrameCsvRange{4, 6, {}}, clip1);
    require(result.status == RecordingFrameCsvClipSplitStatus::kIncomplete,
            "clip 1 should be incomplete before row 6 has a newline");
    require(result.rows_written == 2, "clip 1 first call writes rows 4-5");
    // Retry with no new data: still incomplete, no duplicates.
    result = append_clip_rows_from_root_csv(
        incr_input.string(), &cursor, RecordingFrameCsvRange{4, 6, {}}, clip1);
    require(result.status == RecordingFrameCsvClipSplitStatus::kIncomplete,
            "clip 1 retry without new data stays incomplete");
    require(result.rows_written == 0, "clip 1 retry writes nothing");
    require(result.clip_rows_written == 2, "clip 1 cumulative rows stable");
    // Row 6's newline plus all of clip 2 arrive together; clip 1 completes
    // without consuming clip 2's rows.
    append_text(incr_input, "\n" + rows[6] + rows[7] + rows[8]);
    result = append_clip_rows_from_root_csv(
        incr_input.string(), &cursor, RecordingFrameCsvRange{4, 6, {}}, clip1);
    require(result.status == RecordingFrameCsvClipSplitStatus::kCompleted,
            "clip 1 completes once row 6 is a full line");
    require(result.rows_written == 1, "clip 1 completion consumes only row 6");
    require(result.clip_rows_written == 3, "clip 1 cumulative rows");
    // Re-calling a fully-consumed clip is a cheap idempotent no-op.
    result = append_clip_rows_from_root_csv(
        incr_input.string(), &cursor, RecordingFrameCsvRange{4, 6, {}}, clip1);
    require(result.status == RecordingFrameCsvClipSplitStatus::kCompleted,
            "clip 1 re-call after completion stays completed");
    require(result.rows_written == 0, "clip 1 re-call writes nothing");

    result = append_clip_rows_from_root_csv(
        incr_input.string(), &cursor, RecordingFrameCsvRange{7, 9, {}}, clip2);
    require(result.status == RecordingFrameCsvClipSplitStatus::kCompleted,
            "clip 2 should complete");
    require(result.rows_written == 3, "clip 2 should write rows 7-9");

    // Byte-identical outputs versus the whole-file reference split.
    require(read_text(clip0) == read_text(ranges[0].output_path),
            "clip 0 incremental output must match whole-file split");
    require(read_text(clip1) == read_text(ranges[1].output_path),
            "clip 1 incremental output must match whole-file split");
    require(read_text(clip2) == read_text(ranges[2].output_path),
            "clip 2 incremental output must match whole-file split");
    // Spot-check the semantics the equivalence relies on:
    // crop_video_frame_index restarts at 0 per clip while
    // session_crop_video_frame_index stays session-global.
    const auto clip1_lines = read_lines(clip1);
    require(clip1_lines[1] == "4,104,0,largest_detection_by_confidence,3",
            "clip 1 first row rewrites clip index, keeps session index");
    require(clip1_lines[3] == "6,106,2,largest_detection_by_confidence,5",
            "clip 1 last row rewrites clip index, keeps session index");
    std::filesystem::remove_all(dir);
}

void test_incremental_split_passes_old_schema_rows_through()
{
    const std::filesystem::path dir = make_temp_dir("incremental_old_schema");
    const std::filesystem::path input = dir / "root.csv";
    write_text(
        input,
        "recording_frame_id,local_frame_id,crop_x\n"
        "1,101,10\n"
        "2,102,11\n");
    IncrementalClipSplitCursor cursor;
    const std::string out = (dir / "clip0.csv").string();
    const RecordingFrameCsvSplitResult result = append_clip_rows_from_root_csv(
        input.string(), &cursor, RecordingFrameCsvRange{1, 2, {}}, out);
    require(result.status == RecordingFrameCsvClipSplitStatus::kCompleted,
            "old schema clip should complete: " + result.error);
    const auto lines = read_lines(out);
    require(lines.size() == 3, "old schema output line count");
    require(lines[0] == "recording_frame_id,local_frame_id,crop_x",
            "old schema header preserved");
    require(lines[1] == "1,101,10", "old schema row 1 verbatim");
    require(lines[2] == "2,102,11", "old schema row 2 verbatim");
    std::filesystem::remove_all(dir);
}

void test_incremental_split_reports_orphans_below_range()
{
    const std::filesystem::path dir = make_temp_dir("incremental_orphans");
    const std::filesystem::path input = dir / "root.csv";
    write_text(
        input,
        "recording_frame_id,total_ms\n"
        "1,0.1\n"
        "2,0.2\n"
        "3,0.3\n"
        "4,0.4\n");

    // Default policy: fail loudly on the first below-range row.
    IncrementalClipSplitCursor fail_cursor;
    RecordingFrameCsvSplitResult result = append_clip_rows_from_root_csv(
        input.string(), &fail_cursor, RecordingFrameCsvRange{3, 4, {}},
        (dir / "fail_clip.csv").string());
    require(result.status == RecordingFrameCsvClipSplitStatus::kFailed,
            "below-range rows should fail under kFail policy");
    require(result.error.find("first orphaned recording_frame_id 1") !=
                std::string::npos,
            "orphan error should name the first orphaned id: " + result.error);
    require(result.stats.orphan_rows == 1, "kFail reports the first orphan");

    // kCountAndSkip: orphans are counted, skipped, and never reappear.
    IncrementalClipSplitCursor skip_cursor;
    const std::string out = (dir / "skip_clip.csv").string();
    result = append_clip_rows_from_root_csv(
        input.string(), &skip_cursor, RecordingFrameCsvRange{3, 4, {}}, out,
        RecordingFrameCsvOrphanRowPolicy::kCountAndSkip);
    require(result.status == RecordingFrameCsvClipSplitStatus::kCompleted,
            "kCountAndSkip clip should complete: " + result.error);
    require(result.stats.orphan_rows == 2, "kCountAndSkip orphan count");
    require(result.stats.first_orphan_recording_frame_id == 1,
            "kCountAndSkip first orphan id");
    require(result.rows_written == 2, "kCountAndSkip in-range rows written");
    const auto lines = read_lines(out);
    require(lines.size() == 3, "kCountAndSkip output line count");
    require(lines[1] == "3,0.3", "kCountAndSkip first in-range row");
    require(lines[2] == "4,0.4", "kCountAndSkip last in-range row");
    std::filesystem::remove_all(dir);
}

void test_incremental_split_leaves_above_range_rows_unconsumed()
{
    const std::filesystem::path dir = make_temp_dir("incremental_above_range");
    const std::filesystem::path input = dir / "root.csv";
    // Row 3 (the clip boundary row) was lost: the first row above the range
    // proves clip 0's rows are finished, and must be left for a later call.
    write_text(
        input,
        "recording_frame_id,total_ms\n"
        "1,0.1\n"
        "2,0.2\n"
        "4,0.4\n"
        "5,0.5\n");
    IncrementalClipSplitCursor cursor;
    const std::string out = (dir / "clip0.csv").string();
    RecordingFrameCsvSplitResult result = append_clip_rows_from_root_csv(
        input.string(), &cursor, RecordingFrameCsvRange{1, 3, {}}, out);
    require(result.status == RecordingFrameCsvClipSplitStatus::kCompleted,
            "clip 0 completes when a row above its range appears: " +
                result.error);
    require(result.rows_written == 2, "clip 0 writes only rows 1-2");
    const auto lines = read_lines(out);
    require(lines.size() == 3, "clip 0 output has header plus two rows");
    require(lines[2] == "2,0.2", "clip 0 last row is id 2");
    // Row 4 was not consumed: it is still pending for a future call, and a
    // next clip starting past the highest consumed id is rejected as a gap
    // rather than silently swallowing it.
    require(cursor.last_consumed_recording_frame_id == 2,
            "cursor stops at the last in-range row");
    require(cursor.pending_partial_line.rfind("4,0.4", 0) == 0,
            "row 4 stays unconsumed ahead of the cursor");
    result = append_clip_rows_from_root_csv(
        input.string(), &cursor, RecordingFrameCsvRange{4, 5, {}},
        (dir / "clip1.csv").string());
    require(result.status == RecordingFrameCsvClipSplitStatus::kFailed,
            "next clip after a lost boundary row is rejected as a gap");
    require(result.error.find("expected 3") != std::string::npos,
            "gap error names the expected id: " + result.error);
    std::filesystem::remove_all(dir);
}

void test_incremental_split_rejects_gap_to_next_clip()
{
    const std::filesystem::path dir = make_temp_dir("incremental_gap");
    const std::filesystem::path input = dir / "root.csv";
    write_text(
        input,
        "recording_frame_id,total_ms\n"
        "1,0.1\n"
        "2,0.2\n"
        "5,0.5\n"
        "6,0.6\n");
    IncrementalClipSplitCursor cursor;
    RecordingFrameCsvSplitResult result = append_clip_rows_from_root_csv(
        input.string(), &cursor, RecordingFrameCsvRange{1, 2, {}},
        (dir / "clip0.csv").string());
    require(result.status == RecordingFrameCsvClipSplitStatus::kCompleted,
            "clip 0 should complete: " + result.error);

    // Clip range [5, 6] leaves a gap after consumed id 2 and must be
    // rejected before consuming anything.
    result = append_clip_rows_from_root_csv(
        input.string(), &cursor, RecordingFrameCsvRange{5, 6, {}},
        (dir / "clip1.csv").string());
    require(result.status == RecordingFrameCsvClipSplitStatus::kFailed,
            "gapped clip range should fail");
    require(result.error.find("gap between recording_frame_id ranges") !=
                std::string::npos,
            "gap error should mention the gap: " + result.error);
    require(result.error.find("expected 3") != std::string::npos,
            "gap error should name the expected next id: " + result.error);
    std::filesystem::remove_all(dir);
}

}  // namespace

int main()
{
    struct TestCase {
        const char* name;
        void (*fn)();
    };

    const TestCase tests[] = {
        {"splits_crop_metadata_by_recording_frame_range", test_splits_crop_metadata_by_recording_frame_range},
        {"rewrites_crop_video_frame_index_per_split_output", test_rewrites_crop_video_frame_index_per_split_output},
        {"counts_and_skips_orphan_rows_when_policy_allows", test_counts_and_skips_orphan_rows_when_policy_allows},
        {"fails_on_orphan_rows_by_default", test_fails_on_orphan_rows_by_default},
        {"rejects_gap_between_consecutive_ranges", test_rejects_gap_between_consecutive_ranges},
        {"preserves_session_crop_video_frame_index_verbatim", test_preserves_session_crop_video_frame_index_verbatim},
        {"rejects_invalid_csv_contracts", test_rejects_invalid_csv_contracts},
        {"rejects_overlapping_ranges", test_rejects_overlapping_ranges},
        {"incremental_split_matches_whole_file_split",
         test_incremental_split_matches_whole_file_split},
        {"incremental_split_passes_old_schema_rows_through",
         test_incremental_split_passes_old_schema_rows_through},
        {"incremental_split_reports_orphans_below_range",
         test_incremental_split_reports_orphans_below_range},
        {"incremental_split_leaves_above_range_rows_unconsumed",
         test_incremental_split_leaves_above_range_rows_unconsumed},
        {"incremental_split_rejects_gap_to_next_clip",
         test_incremental_split_rejects_gap_to_next_clip},
    };

    for (const auto& test : tests) {
        try {
            test.fn();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& ex) {
            std::cerr << "[FAIL] " << test.name << ": " << ex.what() << '\n';
            return 1;
        }
    }
    return 0;
}
