// Regression tests for the recording-folder reuse hazard found in the
// 2026-07 data-integrity audit: a worker-initiated stop (fail_on_drop)
// leaves CameraControl::recording_folder latched with
// preserve_recording_session_state=true and no finalize scheduled, after
// which prepare_recording_run's reuse branch sent the NEXT run into the
// PREVIOUS run's directory, truncate-overwriting its manifests.
//
// These tests encode the SAFE behavior. They are expected to FAIL on the
// unfixed tree (proving the bug reproduces) and PASS once
// prepare_recording_run always mints a fresh folder.

#include "session/recording_session.h"

#include "video_capture.h"

#include "NvEncoder/Logger.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

// The NvEnc/CUDA sources linked into this target log through this global.
simplelogger::Logger* logger =
    simplelogger::LoggerFactory::CreateConsoleLogger();

namespace {

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::filesystem::path make_temp_base_folder(const std::string& tag)
{
    const std::filesystem::path base =
        std::filesystem::temp_directory_path() /
        ("orange_folder_reuse_" + tag + "_" +
         std::to_string(static_cast<long long>(getpid())));
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    return base;
}

CameraParams make_camera_params()
{
    CameraParams camera{};
    camera.width = 320;
    camera.height = 240;
    camera.frame_rate = 100;
    camera.gpu_id = 0;
    camera.camera_id = 0;
    camera.camera_name = "reuse_test_cam";
    camera.camera_serial = "990002";
    camera.num_cameras = 1;
    return camera;
}

std::string read_file(const std::filesystem::path& path)
{
    std::ifstream in(path);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

orange::session::PreparedRecordingRunStart prepare_run(
    orange::session::RecordingSessionState* state,
    CameraControl* camera_control,
    CameraParams* camera,
    CameraEachSelect* select,
    const std::filesystem::path& base,
    PTPParams* ptp)
{
    return orange::session::prepare_recording_run(
        state, camera_control, camera, select, 1, base.string(), ptp,
        "real", nullptr);
}

// Reproduces the exact state a fail_on_drop stop leaves behind:
// 1. run 1 prepared (folder claimed, snapshot written), GUI marked it
//    started (record_video=true, preserve=true),
// 2. a worker flipped the stop triple (record_video=false,
//    recording_draining=true, stop_record=true),
// 3. the last encoder's drain-complete cleared the drain latches but kept
//    the folder because preserve was set (encoder_hw_worker drain path).
// The next prepare must NOT land in run 1's folder or touch its artifacts.
void test_stop_without_finalize_must_not_reuse_folder()
{
    const std::filesystem::path base = make_temp_base_folder("fail_on_drop");
    CameraControl camera_control;
    orange::session::RecordingSessionState state;
    state.recording_sink_mode = "real";
    CameraParams camera = make_camera_params();
    CameraEachSelect select;
    select.record = true;
    PTPParams ptp{};

    const orange::session::PreparedRecordingRunStart run1 =
        prepare_run(&state, &camera_control, &camera, &select, base, &ptp);
    require(run1.valid, "run 1 prepare must succeed: " + run1.error_message);
    require(!run1.recording_folder.empty() &&
                std::filesystem::exists(run1.recording_folder),
            "run 1 prepare must create its folder");

    // GUI start marks: record_video flips at completion; preserve is set by
    // gui_note_recording_started on every GUI start.
    camera_control.record_video = true;
    camera_control.preserve_recording_session_state = true;

    // Sentinel: run 1's snapshot as written at its start. If run 2 reuses
    // the folder it will truncate-overwrite this file.
    const std::filesystem::path run1_snapshot =
        std::filesystem::path(run1.recording_folder) /
        "recording_snapshot.json";
    require(std::filesystem::exists(run1_snapshot),
            "run 1 prepare must write recording_snapshot.json");
    const std::string run1_snapshot_content = read_file(run1_snapshot);
    require(!run1_snapshot_content.empty(),
            "run 1 snapshot sentinel must be non-empty");

    // Worker-initiated fail_on_drop stop (acquire_frames / preprocess).
    camera_control.record_video = false;
    camera_control.recording_draining = true;
    camera_control.stop_record = true;

    // Last encoder drain-complete (encoder_hw_worker): latches cleared,
    // folder KEPT because preserve is set.
    camera_control.recording_draining = false;
    camera_control.stop_record = false;
    {
        std::lock_guard<std::mutex> lock(camera_control.recording_folder_mutex);
        camera_control.recording_output_folder.clear();
    }
    require(orange::session::current_recording_folder(&camera_control) ==
                run1.recording_folder,
            "precondition: run 1's folder must still be latched (preserve)");

    // The recording id is second-granularity; make sure a fresh mint cannot
    // collide with run 1's id by timestamp alone.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    const orange::session::PreparedRecordingRunStart run2 =
        prepare_run(&state, &camera_control, &camera, &select, base, &ptp);
    require(run2.valid, "run 2 prepare must succeed: " + run2.error_message);

    require(run2.recording_folder != run1.recording_folder,
            "REGRESSION: run 2 must mint a fresh folder, not reuse run 1's "
            "latched folder (" + run1.recording_folder + ")");
    require(read_file(run1_snapshot) == run1_snapshot_content,
            "REGRESSION: run 1's recording_snapshot.json must not be "
            "rewritten by run 2's prepare");
}

// A latched folder with no active run (any future leak path) must never be
// silently reused either: prepare must mint fresh.
void test_stale_latch_is_never_silently_reused()
{
    const std::filesystem::path base = make_temp_base_folder("stale_latch");
    CameraControl camera_control;
    orange::session::RecordingSessionState state;
    state.recording_sink_mode = "real";
    CameraParams camera = make_camera_params();
    CameraEachSelect select;
    select.record = true;
    PTPParams ptp{};

    const std::string stale = (base / "STALE_LEAKED_RUN").string();
    std::filesystem::create_directories(stale);
    {
        std::lock_guard<std::mutex> lock(camera_control.recording_folder_mutex);
        camera_control.recording_folder = stale;
    }
    camera_control.preserve_recording_session_state = true;

    const orange::session::PreparedRecordingRunStart run =
        prepare_run(&state, &camera_control, &camera, &select, base, &ptp);
    require(run.valid, "prepare must succeed: " + run.error_message);
    require(run.recording_folder != stale,
            "REGRESSION: prepare must not adopt a stale latched folder");
    require(orange::session::current_recording_folder(&camera_control) ==
                run.recording_folder,
            "the fresh folder must be the new claim");
}

}  // namespace

int main()
{
    struct Case {
        const char* name;
        void (*fn)();
    };
    const Case cases[] = {
        {"stop_without_finalize_must_not_reuse_folder",
         test_stop_without_finalize_must_not_reuse_folder},
        {"stale_latch_is_never_silently_reused",
         test_stale_latch_is_never_silently_reused},
    };
    int failures = 0;
    for (const Case& c : cases) {
        try {
            c.fn();
            std::cout << "[PASS] " << c.name << std::endl;
        } catch (const std::exception& e) {
            ++failures;
            std::cout << "[FAIL] " << c.name << ": " << e.what() << std::endl;
        }
    }
    if (failures != 0) {
        std::cout << failures << " test(s) failed" << std::endl;
        return 1;
    }
    std::cout << "all folder-reuse regression tests passed" << std::endl;
    return 0;
}
