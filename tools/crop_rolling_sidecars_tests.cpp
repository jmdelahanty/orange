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
