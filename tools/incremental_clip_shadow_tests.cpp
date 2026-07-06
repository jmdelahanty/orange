// Tests for the shadow-mode incremental clip splitter (stage 4,
// ORANGE_GUI_INCREMENTAL_CLIP_SHADOW): announcement dedup/ordering, the
// per-clip shadow split attempt (including retry and parking semantics),
// the durable JSONL partial index, the finalize cross-check comparator,
// and an end-to-end run of the real worker thread over synthetic root CSVs
// grown over time.

#include "gui/incremental_clip_shadow.h"

#include "session/crop_rolling_sidecars.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
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
        ("orange_incremental_clip_shadow_" + name + "_" + std::to_string(stamp));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
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

std::vector<nlohmann::json> read_index_lines(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input) {
        return {};  // Not written yet; polling callers retry.
    }
    std::vector<nlohmann::json> lines;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty()) {
            lines.push_back(nlohmann::json::parse(line));
        }
    }
    return lines;
}

size_t count_index_events(const std::vector<nlohmann::json>& lines,
                          const std::string& event)
{
    size_t count = 0;
    for (const nlohmann::json& line : lines) {
        if (line.value("event", std::string()) == event) {
            ++count;
        }
    }
    return count;
}

bool wait_until(const std::function<bool()>& predicate,
                const std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

const std::string kCsvHeader =
    "recording_frame_id,crop_video_frame_index,session_crop_video_frame_index,value\n";

std::string csv_row(const uint64_t recording_frame_id,
                    const uint64_t session_index)
{
    std::ostringstream out;
    out << recording_frame_id << ',' << session_index << ',' << session_index
        << ",v" << recording_frame_id << '\n';
    return out.str();
}

std::string csv_rows(const uint64_t first_id,
                     const uint64_t last_id,
                     uint64_t* session_index)
{
    std::ostringstream out;
    for (uint64_t id = first_id; id <= last_id; ++id) {
        out << csv_row(id, (*session_index)++);
    }
    return out.str();
}

GuiShadowClipAnnouncement announcement(const int clip_index,
                                       const uint64_t first,
                                       const uint64_t last)
{
    GuiShadowClipAnnouncement out;
    out.clip_index = clip_index;
    out.first_recording_frame_id = first;
    out.last_recording_frame_id = last;
    return out;
}

// Whole-file reference split (the exact code the authoritative finalize
// uses) for byte-comparing shadow outputs.
void reference_split(const std::filesystem::path& root_csv,
                     std::vector<orange::session::RecordingFrameCsvRange> ranges,
                     const orange::session::RecordingFrameCsvOrphanRowPolicy policy)
{
    std::string error;
    require(
        orange::session::split_recording_frame_csv_by_ranges(
            root_csv.string(),
            &ranges,
            &error,
            policy),
        "reference split of " + root_csv.string() + " should pass: " + error);
}

void test_classify_announcement_orders_dedups_and_gaps()
{
    // Invalid announcements are ignored regardless of chain progress.
    require(gui_shadow_classify_announcement(-1, announcement(-1, 1, 10)) ==
                GuiShadowAnnouncementIntake::kIgnored,
            "negative clip index should be ignored");
    require(gui_shadow_classify_announcement(-1, announcement(0, 0, 10)) ==
                GuiShadowAnnouncementIntake::kIgnored,
            "zero first frame id should be ignored");
    require(gui_shadow_classify_announcement(-1, announcement(0, 10, 5)) ==
                GuiShadowAnnouncementIntake::kIgnored,
            "inverted range should be ignored");

    // Indices must arrive in order; repeats are tolerated.
    require(gui_shadow_classify_announcement(-1, announcement(0, 1, 10)) ==
                GuiShadowAnnouncementIntake::kAccepted,
            "first clip should be accepted");
    require(gui_shadow_classify_announcement(0, announcement(0, 1, 10)) ==
                GuiShadowAnnouncementIntake::kIgnored,
            "repeated announcement should be ignored");
    require(gui_shadow_classify_announcement(3, announcement(2, 21, 30)) ==
                GuiShadowAnnouncementIntake::kIgnored,
            "stale announcement should be ignored");
    require(gui_shadow_classify_announcement(0, announcement(1, 11, 20)) ==
                GuiShadowAnnouncementIntake::kAccepted,
            "next clip should be accepted");
    require(gui_shadow_classify_announcement(0, announcement(2, 21, 30)) ==
                GuiShadowAnnouncementIntake::kGap,
            "skipped clip index should be a gap");
    require(gui_shadow_classify_announcement(-1, announcement(1, 11, 20)) ==
                GuiShadowAnnouncementIntake::kGap,
            "first observed announcement past clip 0 should be a gap");
    std::cout << "test_classify_announcement_orders_dedups_and_gaps passed"
              << std::endl;
}

void test_intake_announcement_appends_in_order()
{
    GuiShadowCameraChainState chain;
    require(gui_shadow_intake_announcement(&chain, announcement(0, 1, 10)) ==
                GuiShadowAnnouncementIntake::kAccepted,
            "clip 0 intake should accept");
    require(chain.last_accepted_clip_index == 0, "intake should advance index");
    require(chain.pending.size() == 1, "intake should enqueue clip 0");
    require(gui_shadow_intake_announcement(&chain, announcement(0, 1, 10)) ==
                GuiShadowAnnouncementIntake::kIgnored,
            "duplicate intake should ignore");
    require(chain.pending.size() == 1, "duplicate should not enqueue");
    require(gui_shadow_intake_announcement(&chain, announcement(1, 11, 20)) ==
                GuiShadowAnnouncementIntake::kAccepted,
            "clip 1 intake should accept");
    require(chain.pending.size() == 2, "clip 1 should enqueue behind clip 0");
    require(chain.pending.front().clip_index == 0, "queue should stay ordered");
    require(gui_shadow_intake_announcement(&chain, announcement(3, 31, 40)) ==
                GuiShadowAnnouncementIntake::kGap,
            "skipping clip 2 should report a gap");
    require(chain.pending.size() == 2, "gap should not enqueue");
    require(chain.last_accepted_clip_index == 1, "gap should not advance index");
    std::cout << "test_intake_announcement_appends_in_order passed" << std::endl;
}

void test_shadow_paths()
{
    require(gui_shadow_index_path("/data/run") ==
                "/data/run/recording_clip_index.shadow.jsonl",
            "index path");
    require(gui_shadow_clip_directory("/data/run/clips", 7) ==
                "/data/run/clips/clip_000007",
            "clip directory naming should match clips/clip_%06d");
    require(gui_shadow_meta_csv_path("/data/run/clips", 0, "2010096") ==
                "/data/run/clips/clip_000000/Cam2010096_crop_meta.shadow.csv",
            "shadow meta path");
    require(gui_shadow_perf_csv_path("/data/run/clips", 12, "2010095") ==
                "/data/run/clips/clip_000012/Cam2010095_crop_perf.shadow.csv",
            "shadow perf path");
    require(gui_shadow_sibling_csv_path(
                "/data/run/clips/clip_000000/Cam2010096_crop_meta.csv") ==
                "/data/run/clips/clip_000000/Cam2010096_crop_meta.shadow.csv",
            "shadow sibling of an authoritative CSV");
    require(gui_shadow_sibling_csv_path("/data/run/notes.txt").empty(),
            "non-CSV paths have no shadow sibling");
    std::cout << "test_shadow_paths passed" << std::endl;
}

void test_index_line_builders_and_append()
{
    const std::filesystem::path dir = make_temp_dir("index");
    const std::string index_path = gui_shadow_index_path(dir.string());

    const nlohmann::json completed = gui_shadow_clip_completed_index_line(
        "2010096",
        announcement(3, 301, 400),
        100,
        102,
        "/x/meta.shadow.csv",
        "/x/perf.shadow.csv",
        "2026-07-05T00:00:00Z");
    require(completed.value("event", std::string()) == "clip_completed",
            "completed line event");
    require(completed.value("clip_index", -1) == 3, "completed line clip index");
    require(completed.value("first_recording_frame_id", 0ULL) == 301,
            "completed line first frame");
    require(completed.value("last_recording_frame_id", 0ULL) == 400,
            "completed line last frame");
    require(completed.value("meta_rows", 0ULL) == 100, "completed line meta rows");
    require(completed.value("perf_rows", 0ULL) == 102, "completed line perf rows");
    require(completed["files"].size() == 2, "completed line file list");
    require(completed.value("camera_serial", std::string()) == "2010096",
            "completed line camera");

    const nlohmann::json parked = gui_shadow_chain_parked_index_line(
        "2010096", 4, "why", "2026-07-05T00:00:00Z");
    require(parked.value("event", std::string()) == "chain_parked",
            "parked line event");
    require(parked.value("reason", std::string()) == "why", "parked line reason");

    std::string error;
    require(gui_shadow_append_index_line(index_path, completed, &error),
            "append completed line: " + error);
    require(gui_shadow_append_index_line(index_path, parked, &error),
            "append parked line: " + error);
    const auto lines = read_index_lines(index_path);
    require(lines.size() == 2, "index should hold two lines");
    require(lines[0] == completed && lines[1] == parked,
            "index lines should round-trip");

    require(!gui_shadow_append_index_line("", completed, &error),
            "empty index path should fail");
    std::filesystem::remove_all(dir);
    std::cout << "test_index_line_builders_and_append passed" << std::endl;
}

void test_process_clip_attempt_matches_whole_file_split()
{
    const std::filesystem::path dir = make_temp_dir("attempt");
    const std::filesystem::path root_meta = dir / "Cam2010096_crop_meta.csv";
    const std::filesystem::path root_perf = dir / "Cam2010096_crop_perf.csv";

    // Meta rows exactly cover the clip ranges [3,10],[11,20],[21,30]; perf
    // additionally has head-orphan rows (ids 1-2, dropped before the first
    // clip) that the kCountAndSkip policy must skip identically in both the
    // incremental and the whole-file splits.
    uint64_t meta_session_index = 0;
    write_text(root_meta, kCsvHeader + csv_rows(3, 30, &meta_session_index));
    uint64_t perf_session_index = 0;
    write_text(root_perf, kCsvHeader + csv_rows(1, 30, &perf_session_index));

    GuiShadowCameraChainState chain;
    chain.camera_serial = "2010096";
    chain.root_meta_csv_path = root_meta.string();
    chain.root_perf_csv_path = root_perf.string();
    chain.clips_root = (dir / "clips").string();

    const std::vector<GuiShadowClipAnnouncement> clips = {
        announcement(0, 3, 10),
        announcement(1, 11, 20),
        announcement(2, 21, 30),
    };
    for (const GuiShadowClipAnnouncement& clip : clips) {
        const GuiShadowClipAttemptResult result =
            gui_shadow_process_clip_attempt(&chain, clip);
        require(result.status == GuiShadowClipAttemptStatus::kCompleted,
                "clip " + std::to_string(clip.clip_index) +
                    " should complete: " + result.error);
        // Repeating a completed clip is tolerated and idempotent.
        const GuiShadowClipAttemptResult repeat =
            gui_shadow_process_clip_attempt(&chain, clip);
        require(repeat.status == GuiShadowClipAttemptStatus::kCompleted,
                "repeated clip attempt should stay completed");
        require(repeat.meta_rows == result.meta_rows &&
                    repeat.perf_rows == result.perf_rows,
                "repeated clip attempt should not change row counts");
    }

    // Whole-file reference splits (the authoritative finalize code path).
    const std::filesystem::path ref = dir / "reference";
    std::vector<orange::session::RecordingFrameCsvRange> meta_ranges;
    std::vector<orange::session::RecordingFrameCsvRange> perf_ranges;
    for (const GuiShadowClipAnnouncement& clip : clips) {
        meta_ranges.push_back(
            {clip.first_recording_frame_id,
             clip.last_recording_frame_id,
             (ref / ("meta_" + std::to_string(clip.clip_index) + ".csv")).string(),
             0});
        perf_ranges.push_back(
            {clip.first_recording_frame_id,
             clip.last_recording_frame_id,
             (ref / ("perf_" + std::to_string(clip.clip_index) + ".csv")).string(),
             0});
    }
    reference_split(root_meta, meta_ranges,
                    orange::session::RecordingFrameCsvOrphanRowPolicy::kFail);
    reference_split(root_perf, perf_ranges,
                    orange::session::RecordingFrameCsvOrphanRowPolicy::kCountAndSkip);
    for (const GuiShadowClipAnnouncement& clip : clips) {
        const std::string shadow_meta = gui_shadow_meta_csv_path(
            chain.clips_root, clip.clip_index, chain.camera_serial);
        const std::string shadow_perf = gui_shadow_perf_csv_path(
            chain.clips_root, clip.clip_index, chain.camera_serial);
        require(read_text(shadow_meta) ==
                    read_text(ref / ("meta_" + std::to_string(clip.clip_index) +
                                     ".csv")),
                "shadow meta CSV should byte-match the whole-file split for"
                " clip " + std::to_string(clip.clip_index));
        require(read_text(shadow_perf) ==
                    read_text(ref / ("perf_" + std::to_string(clip.clip_index) +
                                     ".csv")),
                "shadow perf CSV should byte-match the whole-file split for"
                " clip " + std::to_string(clip.clip_index));
    }
    std::filesystem::remove_all(dir);
    std::cout << "test_process_clip_attempt_matches_whole_file_split passed"
              << std::endl;
}

void test_process_clip_attempt_incomplete_then_retry()
{
    const std::filesystem::path dir = make_temp_dir("retry");
    const std::filesystem::path root_meta = dir / "Cam2010096_crop_meta.csv";
    const std::filesystem::path root_perf = dir / "Cam2010096_crop_perf.csv";

    // Clip 0 covers [1,10] but only rows 1..7 are flushed, plus a trailing
    // partial line for row 8 (the writer is mid-row).
    uint64_t meta_session_index = 0;
    write_text(root_meta, kCsvHeader + csv_rows(1, 7, &meta_session_index));
    append_text(root_meta, "8,7");  // no newline: partial line
    uint64_t perf_session_index = 0;
    write_text(root_perf, kCsvHeader + csv_rows(1, 7, &perf_session_index));

    GuiShadowCameraChainState chain;
    chain.camera_serial = "2010096";
    chain.root_meta_csv_path = root_meta.string();
    chain.root_perf_csv_path = root_perf.string();
    chain.clips_root = (dir / "clips").string();

    const GuiShadowClipAnnouncement clip0 = announcement(0, 1, 10);
    const GuiShadowClipAttemptResult first =
        gui_shadow_process_clip_attempt(&chain, clip0);
    require(first.status == GuiShadowClipAttemptStatus::kIncomplete,
            "attempt on a partially flushed clip should be incomplete");
    require(first.meta_rows == 7, "rows seen so far should be appended");

    // The writer finishes row 8 and flushes the rest of the clip.
    append_text(root_meta, ",7,v8\n");
    meta_session_index = 8;
    append_text(root_meta, csv_rows(9, 10, &meta_session_index));
    append_text(root_perf, csv_rows(8, 10, &perf_session_index));
    const GuiShadowClipAttemptResult second =
        gui_shadow_process_clip_attempt(&chain, clip0);
    require(second.status == GuiShadowClipAttemptStatus::kCompleted,
            "retry should complete the clip: " + second.error);
    require(second.meta_rows == 10 && second.perf_rows == 10,
            "retry should account for all clip rows");

    // Byte-match against the whole-file reference (crop_video_frame_index
    // numbering must continue seamlessly across the retry).
    const std::filesystem::path ref = dir / "reference";
    std::vector<orange::session::RecordingFrameCsvRange> meta_ranges = {
        {1, 10, (ref / "meta_0.csv").string(), 0}};
    reference_split(root_meta, meta_ranges,
                    orange::session::RecordingFrameCsvOrphanRowPolicy::kFail);
    require(read_text(gui_shadow_meta_csv_path(chain.clips_root, 0, "2010096")) ==
                read_text(ref / "meta_0.csv"),
            "shadow meta CSV should byte-match the reference across a retry");
    std::filesystem::remove_all(dir);
    std::cout << "test_process_clip_attempt_incomplete_then_retry passed"
              << std::endl;
}

void test_process_clip_attempt_missing_root_is_incomplete()
{
    const std::filesystem::path dir = make_temp_dir("missing_root");
    GuiShadowCameraChainState chain;
    chain.camera_serial = "2010096";
    chain.root_meta_csv_path = (dir / "Cam2010096_crop_meta.csv").string();
    chain.root_perf_csv_path = (dir / "Cam2010096_crop_perf.csv").string();
    chain.clips_root = (dir / "clips").string();
    const GuiShadowClipAttemptResult result =
        gui_shadow_process_clip_attempt(&chain, announcement(0, 1, 10));
    require(result.status == GuiShadowClipAttemptStatus::kIncomplete,
            "missing root CSVs should be retryable, not fatal");
    std::filesystem::remove_all(dir);
    std::cout << "test_process_clip_attempt_missing_root_is_incomplete passed"
              << std::endl;
}

void test_process_clip_attempt_fails_on_meta_orphan()
{
    const std::filesystem::path dir = make_temp_dir("failed");
    const std::filesystem::path root_meta = dir / "Cam2010096_crop_meta.csv";
    const std::filesystem::path root_perf = dir / "Cam2010096_crop_perf.csv";
    // Meta rows 1..10 but the clip claims [3,10]: rows 1-2 are orphans, a
    // hole in the recorded data under the kFail policy.
    uint64_t meta_session_index = 0;
    write_text(root_meta, kCsvHeader + csv_rows(1, 10, &meta_session_index));
    uint64_t perf_session_index = 0;
    write_text(root_perf, kCsvHeader + csv_rows(1, 10, &perf_session_index));

    GuiShadowCameraChainState chain;
    chain.camera_serial = "2010096";
    chain.root_meta_csv_path = root_meta.string();
    chain.root_perf_csv_path = root_perf.string();
    chain.clips_root = (dir / "clips").string();
    const GuiShadowClipAttemptResult result =
        gui_shadow_process_clip_attempt(&chain, announcement(0, 3, 10));
    require(result.status == GuiShadowClipAttemptStatus::kFailed,
            "meta orphan rows should fail the attempt");
    require(result.error.find("crop meta") != std::string::npos,
            "failure should name the meta split");
    std::filesystem::remove_all(dir);
    std::cout << "test_process_clip_attempt_fails_on_meta_orphan passed"
              << std::endl;
}

void test_cross_check_identical_mismatched_missing()
{
    const std::filesystem::path dir = make_temp_dir("cross_check");
    const std::string index_path = gui_shadow_index_path(dir.string());

    const std::filesystem::path identical = dir / "a_meta.csv";
    write_text(identical, "recording_frame_id\n1\n2\n");
    write_text(dir / "a_meta.shadow.csv", "recording_frame_id\n1\n2\n");

    const std::filesystem::path mismatched = dir / "b_meta.csv";
    write_text(mismatched, "recording_frame_id\n1\n2\n");
    write_text(dir / "b_meta.shadow.csv", "recording_frame_id\n1\n3\n");

    const std::filesystem::path missing = dir / "c_meta.csv";
    write_text(missing, "recording_frame_id\n1\n");

    // d_meta.csv does not exist: the authoritative split never produced it,
    // so there is nothing to judge and it must not be counted.
    const std::vector<std::string> authoritative = {
        identical.string(),
        mismatched.string(),
        missing.string(),
        (dir / "d_meta.csv").string(),
    };
    const GuiShadowCrossCheckOutcome outcome =
        gui_cross_check_incremental_clip_shadow(index_path, authoritative);
    require(outcome.total == 3, "three authoritative CSVs should be considered");
    require(outcome.identical == 1, "one identical");
    require(outcome.mismatched == 1, "one mismatched");
    require(outcome.missing == 1, "one missing");
    require(outcome.summary ==
                "shadow cross-check: 1/3 clip CSVs identical, 1 mismatched,"
                " 1 missing",
            "summary format: " + outcome.summary);

    const auto lines = read_index_lines(index_path);
    require(lines.size() == 1, "cross-check should append one index line");
    require(lines[0].value("event", std::string()) == "cross_check",
            "cross-check line event");
    require(lines[0].value("identical", -1) == 1 &&
                lines[0].value("mismatched", -1) == 1 &&
                lines[0].value("missing", -1) == 1,
            "cross-check line counts");
    require(lines[0]["mismatched_files"].size() == 1 &&
                lines[0]["missing_files"].size() == 1,
            "cross-check line file lists");
    std::filesystem::remove_all(dir);
    std::cout << "test_cross_check_identical_mismatched_missing passed"
              << std::endl;
}

void test_env_flag_default_off()
{
    unsetenv(kGuiIncrementalClipShadowEnvFlag);
    require(!gui_incremental_clip_shadow_enabled(),
            "shadow mode must default OFF");
    setenv(kGuiIncrementalClipShadowEnvFlag, "1", 1);
    require(gui_incremental_clip_shadow_enabled(), "flag=1 enables");
    setenv(kGuiIncrementalClipShadowEnvFlag, "0", 1);
    require(!gui_incremental_clip_shadow_enabled(), "flag=0 disables");
    unsetenv(kGuiIncrementalClipShadowEnvFlag);
    std::cout << "test_env_flag_default_off passed" << std::endl;
}

void test_join_without_start_is_noop()
{
    GuiIncrementalClipShadowState state;
    gui_join_incremental_clip_shadow(&state);
    gui_join_incremental_clip_shadow(&state);
    gui_join_incremental_clip_shadow(nullptr);
    require(!state.active, "state should stay idle");
    std::cout << "test_join_without_start_is_noop passed" << std::endl;
}

void test_worker_end_to_end_grows_root_csv_over_time()
{
    const std::filesystem::path dir = make_temp_dir("worker_e2e");
    const std::filesystem::path root_meta = dir / "Cam2010096_crop_meta.csv";
    const std::filesystem::path root_perf = dir / "Cam2010096_crop_perf.csv";
    write_text(root_meta, kCsvHeader);
    write_text(root_perf, kCsvHeader);
    uint64_t meta_session_index = 0;
    uint64_t perf_session_index = 0;

    GuiIncrementalClipShadowState state;
    {
        GuiShadowCameraChainState chain;
        chain.camera_serial = "2010096";
        chain.root_meta_csv_path = root_meta.string();
        chain.root_perf_csv_path = root_perf.string();
        chain.clips_root = (dir / "clips").string();
        std::vector<GuiShadowCameraChainState> chains;
        chains.push_back(std::move(chain));
        require(gui_start_incremental_clip_shadow(&state, std::move(chains),
                                                  dir.string()),
                "worker should start");
        require(state.active, "state should be active after start");
    }
    const std::string index_path = gui_shadow_index_path(dir.string());

    // Clip 0 rows land plus the first rows of clip 1 (the boundary-crossing
    // row is what completes clip 0's extraction), then clip 0 is announced.
    append_text(root_meta, csv_rows(1, 13, &meta_session_index));
    append_text(root_perf, csv_rows(1, 13, &perf_session_index));
    gui_push_incremental_clip_shadow_announcement(
        &state, "2010096", announcement(0, 1, 10));
    require(wait_until(
                [&]() {
                    return std::filesystem::exists(index_path) &&
                           count_index_events(read_index_lines(index_path),
                                              "clip_completed") >= 1;
                },
                std::chrono::seconds(5)),
            "clip 0 should shadow-split promptly");
    // The same announcement repeated must be tolerated.
    gui_push_incremental_clip_shadow_announcement(
        &state, "2010096", announcement(0, 1, 10));

    // Clip 1 is announced BEFORE its tail rows are flushed: the worker must
    // go through the kIncomplete bounded-retry path, then complete once the
    // writer catches up.
    gui_push_incremental_clip_shadow_announcement(
        &state, "2010096", announcement(1, 11, 20));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    append_text(root_meta, csv_rows(14, 21, &meta_session_index));
    append_text(root_perf, csv_rows(14, 21, &perf_session_index));
    require(wait_until(
                [&]() {
                    return count_index_events(read_index_lines(index_path),
                                              "clip_completed") >= 2;
                },
                std::chrono::seconds(5)),
            "clip 1 should shadow-split after the retry");

    gui_join_incremental_clip_shadow(&state);
    require(!state.active, "state should be idle after join");
    require(state.done.load(), "worker should have published done");
    require(state.clips_completed.load() == 2, "two clips should be shadowed");
    require(state.chains_parked.load() == 0, "no chain should be parked");
    require(state.chains.size() == 1 && state.chains[0].clips_completed == 2,
            "chain should record both clips");

    // The JSONL partial index is complete.
    const auto lines = read_index_lines(index_path);
    require(count_index_events(lines, "shadow_started") == 1,
            "index should record the worker start");
    require(count_index_events(lines, "clip_completed") == 2,
            "index should record both clips");
    require(count_index_events(lines, "shadow_stopped") == 1,
            "index should record the worker stop");
    for (const nlohmann::json& line : lines) {
        if (line.value("event", std::string()) != "clip_completed") {
            continue;
        }
        require(line.value("meta_rows", 0ULL) == 10 &&
                    line.value("perf_rows", 0ULL) == 10,
                "clip index lines should carry the row counts");
        require(line["files"].size() == 2,
                "clip index lines should list the per-camera files");
    }

    // Shadow CSVs byte-match a whole-file reference split.
    const std::filesystem::path ref = dir / "reference";
    std::vector<orange::session::RecordingFrameCsvRange> meta_ranges = {
        {1, 10, (ref / "meta_0.csv").string(), 0},
        {11, 20, (ref / "meta_1.csv").string(), 0},
        {21, 21, (ref / "meta_2.csv").string(), 0},
    };
    std::vector<orange::session::RecordingFrameCsvRange> perf_ranges = {
        {1, 10, (ref / "perf_0.csv").string(), 0},
        {11, 20, (ref / "perf_1.csv").string(), 0},
        {21, 21, (ref / "perf_2.csv").string(), 0},
    };
    reference_split(root_meta, meta_ranges,
                    orange::session::RecordingFrameCsvOrphanRowPolicy::kFail);
    reference_split(root_perf, perf_ranges,
                    orange::session::RecordingFrameCsvOrphanRowPolicy::kCountAndSkip);
    for (int clip = 0; clip <= 1; ++clip) {
        require(read_text(gui_shadow_meta_csv_path((dir / "clips").string(),
                                                   clip, "2010096")) ==
                    read_text(ref / ("meta_" + std::to_string(clip) + ".csv")),
                "shadow meta clip " + std::to_string(clip) +
                    " should byte-match the whole-file split");
        require(read_text(gui_shadow_perf_csv_path((dir / "clips").string(),
                                                   clip, "2010096")) ==
                    read_text(ref / ("perf_" + std::to_string(clip) + ".csv")),
                "shadow perf clip " + std::to_string(clip) +
                    " should byte-match the whole-file split");
    }
    std::filesystem::remove_all(dir);
    std::cout << "test_worker_end_to_end_grows_root_csv_over_time passed"
              << std::endl;
}

void test_worker_parks_failed_chain_without_blocking_other_cameras()
{
    const std::filesystem::path dir = make_temp_dir("worker_park");
    const std::filesystem::path root_meta_a = dir / "Cam2010095_crop_meta.csv";
    const std::filesystem::path root_perf_a = dir / "Cam2010095_crop_perf.csv";
    const std::filesystem::path root_meta_b = dir / "Cam2010096_crop_meta.csv";
    const std::filesystem::path root_perf_b = dir / "Cam2010096_crop_perf.csv";
    // Camera A's announced clip 0 range starts at 3 while its meta CSV
    // holds rows from 1: rows 1-2 orphan under the kFail policy, so the
    // attempt fails and the chain parks. Camera B is healthy.
    uint64_t session_index_a = 0;
    write_text(root_meta_a, kCsvHeader + csv_rows(1, 11, &session_index_a));
    uint64_t perf_index_a = 0;
    write_text(root_perf_a, kCsvHeader + csv_rows(1, 11, &perf_index_a));
    uint64_t session_index_b = 0;
    write_text(root_meta_b, kCsvHeader + csv_rows(1, 11, &session_index_b));
    uint64_t perf_index_b = 0;
    write_text(root_perf_b, kCsvHeader + csv_rows(1, 11, &perf_index_b));

    GuiIncrementalClipShadowState state;
    {
        std::vector<GuiShadowCameraChainState> chains(2);
        chains[0].camera_serial = "2010095";
        chains[0].root_meta_csv_path = root_meta_a.string();
        chains[0].root_perf_csv_path = root_perf_a.string();
        chains[0].clips_root = (dir / "clips").string();
        chains[1].camera_serial = "2010096";
        chains[1].root_meta_csv_path = root_meta_b.string();
        chains[1].root_perf_csv_path = root_perf_b.string();
        chains[1].clips_root = (dir / "clips").string();
        require(gui_start_incremental_clip_shadow(&state, std::move(chains),
                                                  dir.string()),
                "worker should start");
    }
    const std::string index_path = gui_shadow_index_path(dir.string());
    gui_push_incremental_clip_shadow_announcement(
        &state, "2010095", announcement(0, 3, 10));
    gui_push_incremental_clip_shadow_announcement(
        &state, "2010096", announcement(0, 1, 10));
    require(wait_until(
                [&]() {
                    const auto lines = read_index_lines(index_path);
                    return count_index_events(lines, "chain_parked") >= 1 &&
                           count_index_events(lines, "clip_completed") >= 1;
                },
                std::chrono::seconds(5)),
            "camera A should park while camera B completes");

    gui_join_incremental_clip_shadow(&state);
    require(state.chains[0].parked, "camera A chain should be parked");
    require(state.chains[0].parked_clip_index == 0,
            "camera A should park at clip 0");
    require(!state.chains[1].parked, "camera B chain should not be parked");
    require(state.chains[1].clips_completed == 1,
            "camera B should have shadowed clip 0");
    require(!std::filesystem::exists(
                gui_shadow_meta_csv_path((dir / "clips").string(), 1, "2010095")),
            "no later clip of the parked camera should be attempted");
    std::filesystem::remove_all(dir);
    std::cout <<
        "test_worker_parks_failed_chain_without_blocking_other_cameras passed"
              << std::endl;
}

void test_worker_parks_on_announcement_gap()
{
    const std::filesystem::path dir = make_temp_dir("worker_gap");
    const std::filesystem::path root_meta = dir / "Cam2010096_crop_meta.csv";
    const std::filesystem::path root_perf = dir / "Cam2010096_crop_perf.csv";
    uint64_t session_index = 0;
    write_text(root_meta, kCsvHeader + csv_rows(1, 31, &session_index));
    uint64_t perf_index = 0;
    write_text(root_perf, kCsvHeader + csv_rows(1, 31, &perf_index));

    GuiIncrementalClipShadowState state;
    {
        std::vector<GuiShadowCameraChainState> chains(1);
        chains[0].camera_serial = "2010096";
        chains[0].root_meta_csv_path = root_meta.string();
        chains[0].root_perf_csv_path = root_perf.string();
        chains[0].clips_root = (dir / "clips").string();
        require(gui_start_incremental_clip_shadow(&state, std::move(chains),
                                                  dir.string()),
                "worker should start");
    }
    const std::string index_path = gui_shadow_index_path(dir.string());
    gui_push_incremental_clip_shadow_announcement(
        &state, "2010096", announcement(0, 1, 10));
    require(wait_until(
                [&]() {
                    return std::filesystem::exists(index_path) &&
                           count_index_events(read_index_lines(index_path),
                                              "clip_completed") >= 1;
                },
                std::chrono::seconds(5)),
            "clip 0 should complete");
    // Clip 1's announcement is never observed; clip 2 arrives instead. The
    // skipped clip's range is unknown, so the chain must park.
    gui_push_incremental_clip_shadow_announcement(
        &state, "2010096", announcement(2, 21, 30));
    require(wait_until(
                [&]() {
                    return count_index_events(read_index_lines(index_path),
                                              "chain_parked") >= 1;
                },
                std::chrono::seconds(5)),
            "a skipped announcement should park the chain");
    gui_join_incremental_clip_shadow(&state);
    require(state.chains[0].parked, "chain should be parked after the gap");
    require(state.chains[0].clips_completed == 1,
            "clip 0 should have completed before the gap");
    require(!std::filesystem::exists(
                gui_shadow_meta_csv_path((dir / "clips").string(), 2, "2010096")),
            "the gap clip must not be shadow-split");
    std::filesystem::remove_all(dir);
    std::cout << "test_worker_parks_on_announcement_gap passed" << std::endl;
}

void test_worker_join_is_prompt_with_pending_retries()
{
    const std::filesystem::path dir = make_temp_dir("worker_join");
    const std::filesystem::path root_meta = dir / "Cam2010096_crop_meta.csv";
    const std::filesystem::path root_perf = dir / "Cam2010096_crop_perf.csv";
    // Clip 0's rows never fully arrive: the worker sits in the bounded
    // retry/backoff wait, which the stop signal must interrupt promptly.
    uint64_t session_index = 0;
    write_text(root_meta, kCsvHeader + csv_rows(1, 5, &session_index));
    uint64_t perf_index = 0;
    write_text(root_perf, kCsvHeader + csv_rows(1, 5, &perf_index));

    GuiIncrementalClipShadowState state;
    {
        std::vector<GuiShadowCameraChainState> chains(1);
        chains[0].camera_serial = "2010096";
        chains[0].root_meta_csv_path = root_meta.string();
        chains[0].root_perf_csv_path = root_perf.string();
        chains[0].clips_root = (dir / "clips").string();
        require(gui_start_incremental_clip_shadow(&state, std::move(chains),
                                                  dir.string()),
                "worker should start");
    }
    gui_push_incremental_clip_shadow_announcement(
        &state, "2010096", announcement(0, 1, 10));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto join_started = std::chrono::steady_clock::now();
    gui_join_incremental_clip_shadow(&state);
    const auto join_elapsed =
        std::chrono::steady_clock::now() - join_started;
    require(join_elapsed < std::chrono::seconds(1),
            "join must be prompt even with a retry pending");
    require(!state.chains[0].parked,
            "an in-budget retry must not park on stop");
    std::filesystem::remove_all(dir);
    std::cout << "test_worker_join_is_prompt_with_pending_retries passed"
              << std::endl;
}

}  // namespace

int main()
{
    try {
        test_classify_announcement_orders_dedups_and_gaps();
        test_intake_announcement_appends_in_order();
        test_shadow_paths();
        test_index_line_builders_and_append();
        test_process_clip_attempt_matches_whole_file_split();
        test_process_clip_attempt_incomplete_then_retry();
        test_process_clip_attempt_missing_root_is_incomplete();
        test_process_clip_attempt_fails_on_meta_orphan();
        test_cross_check_identical_mismatched_missing();
        test_env_flag_default_off();
        test_join_without_start_is_noop();
        test_worker_end_to_end_grows_root_csv_over_time();
        test_worker_parks_failed_chain_without_blocking_other_cameras();
        test_worker_parks_on_announcement_gap();
        test_worker_join_is_prompt_with_pending_retries();
    } catch (const std::exception& ex) {
        std::cerr << "incremental_clip_shadow_tests failed: " << ex.what()
                  << std::endl;
        return 1;
    }
    std::cout << "incremental_clip_shadow_tests passed" << std::endl;
    return 0;
}
