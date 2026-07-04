// Locks down the unified recording stop/drain semantics shared by the GUI
// (src/orange.cpp via RecordingSessionState) and the headless client
// (src/orange_headless_client.cpp via the pipeline-list core functions).
//
// These tests exercise the CameraControl state machine with empty pipeline
// lists (vacuously drained) plus the pure re-assert decision helper, covering:
// - request-stop with active_recorders == 0 vs > 0 (early-drain shortcut)
// - preserve_recording_session_state handling on idle stop
// - external_ipc vs in-process (mp4/"real") sink-mode drain-flag re-assertion
// - the combined drained predicate (active_recorders AND pipeline drain)
// - bounded drain waiting and post-run residual state clearing

#include "session/recording_session.h"
#include "NvEncoder/Logger.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

simplelogger::Logger* logger =
    simplelogger::LoggerFactory::CreateConsoleLogger();

namespace {

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void fill_recording_run_fields(CameraControl* camera_control)
{
    camera_control->record_video = true;
    camera_control->recording_draining = false;
    camera_control->stop_record = false;
    std::lock_guard<std::mutex> lock(camera_control->recording_folder_mutex);
    camera_control->recording_folder = "/data/session_A";
    camera_control->recording_output_folder = "/data/session_A/clip_000";
    camera_control->pending_recording_output_folder = "/data/session_A/clip_001";
    camera_control->recording_rollover_at_frame_id = 1200;
    camera_control->recording_rollover_request_id = 3;
    camera_control->recording_rollover_completed_request_id = 2;
    camera_control->recording_rollover_completed_frame_id = 600;
    camera_control->recording_rollover_completed_folder = "/data/session_A/clip_000";
    camera_control->latest_recording_frame_id.store(1234, std::memory_order_relaxed);
}

void test_idle_stop_takes_early_drain_shortcut()
{
    CameraControl camera_control;
    fill_recording_run_fields(&camera_control);
    camera_control.preserve_recording_session_state = false;

    orange::session::request_stop_recording_run(&camera_control);

    require(!camera_control.record_video, "idle stop must clear record_video");
    require(!camera_control.recording_draining,
            "idle stop with active_recorders==0 must not leave recording_draining latched");
    require(!camera_control.stop_record,
            "idle stop with active_recorders==0 must not leave stop_record latched");
    std::lock_guard<std::mutex> lock(camera_control.recording_folder_mutex);
    require(camera_control.recording_folder.empty(),
            "idle stop without preserve flag must clear recording_folder");
    require(camera_control.pending_recording_output_folder.empty(),
            "stop must clear pending_recording_output_folder");
    require(camera_control.recording_rollover_at_frame_id == 0,
            "stop must clear recording_rollover_at_frame_id");
    require(camera_control.recording_rollover_request_id == 0,
            "stop must clear recording_rollover_request_id");
}

void test_idle_stop_preserves_session_folder_when_requested()
{
    CameraControl camera_control;
    fill_recording_run_fields(&camera_control);
    camera_control.preserve_recording_session_state = true;

    orange::session::request_stop_recording_run(&camera_control);

    require(!camera_control.recording_draining && !camera_control.stop_record,
            "idle stop must still clear drain latches when preserving session state");
    std::lock_guard<std::mutex> lock(camera_control.recording_folder_mutex);
    require(camera_control.recording_folder == "/data/session_A",
            "preserve_recording_session_state must keep recording_folder on idle stop");
}

void test_active_stop_latches_drain_flags()
{
    CameraControl camera_control;
    fill_recording_run_fields(&camera_control);
    camera_control.preserve_recording_session_state = false;
    camera_control.active_recorders.store(2, std::memory_order_relaxed);

    orange::session::request_stop_recording_run(&camera_control);

    require(!camera_control.record_video, "active stop must clear record_video");
    require(camera_control.recording_draining,
            "active stop must latch recording_draining until recorders finalize");
    require(camera_control.stop_record,
            "active stop must latch stop_record until recorders finalize");
    std::lock_guard<std::mutex> lock(camera_control.recording_folder_mutex);
    require(camera_control.recording_folder == "/data/session_A",
            "active stop must not clear recording_folder before recorders finalize");
    require(camera_control.pending_recording_output_folder.empty() &&
                camera_control.recording_rollover_at_frame_id == 0 &&
                camera_control.recording_rollover_request_id == 0,
            "active stop must cancel pending rollover state");
}

void test_drain_flag_reassert_rule()
{
    require(orange::session::should_reassert_recording_drain_flags("external_ipc", false),
            "external_ipc with undrained pipelines must re-assert drain flags");
    require(!orange::session::should_reassert_recording_drain_flags("external_ipc", true),
            "external_ipc with drained pipelines must not re-assert drain flags");
    require(!orange::session::should_reassert_recording_drain_flags("real", false),
            "in-process sinks track drain via active_recorders, not re-asserted flags");
    require(!orange::session::should_reassert_recording_drain_flags("real", true),
            "drained in-process sink must not re-assert drain flags");
}

void test_request_drain_external_ipc_with_drained_pipelines()
{
    CameraControl camera_control;
    fill_recording_run_fields(&camera_control);
    std::vector<std::unique_ptr<ModernRecordingPipeline>> no_pipelines;

    orange::session::request_drain_recording_run(
        &no_pipelines, "external_ipc", &camera_control);

    require(!camera_control.recording_draining && !camera_control.stop_record,
            "external_ipc drain with vacuously drained pipelines must settle immediately");
}

void test_state_overload_delegates_to_core()
{
    CameraControl camera_control;
    fill_recording_run_fields(&camera_control);
    orange::session::RecordingSessionState state;
    state.recording_sink_mode = "external_ipc";

    orange::session::request_drain_recording_run(&state, &camera_control);

    require(!camera_control.record_video,
            "state overload must clear record_video through the shared core");
    require(!camera_control.recording_draining && !camera_control.stop_record,
            "state overload idle drain must settle through the early-drain shortcut");
    require(orange::session::recording_pipelines_drained(&state),
            "empty session pipeline list must be vacuously drained");
}

void test_combined_drained_predicate()
{
    CameraControl camera_control;
    std::vector<std::unique_ptr<ModernRecordingPipeline>> no_pipelines;

    require(orange::session::recording_run_drained(&no_pipelines, &camera_control),
            "no pipelines and zero active recorders must count as drained");
    require(orange::session::recording_run_drained(nullptr, nullptr),
            "null inputs must count as drained");

    camera_control.active_recorders.store(1, std::memory_order_relaxed);
    require(!orange::session::recording_run_drained(&no_pipelines, &camera_control),
            "active in-process recorders must block the drained predicate even "
            "when every pipeline reports drained");
}

void test_wait_for_recording_run_drain_times_out_and_completes()
{
    CameraControl camera_control;
    std::vector<std::unique_ptr<ModernRecordingPipeline>> no_pipelines;

    camera_control.active_recorders.store(1, std::memory_order_relaxed);
    require(!orange::session::wait_for_recording_run_drain(
                &no_pipelines,
                &camera_control,
                std::chrono::milliseconds(30),
                nullptr),
            "wait must report failure when recorders stay active past the timeout");

    std::thread finalizer([&camera_control]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        camera_control.active_recorders.store(0, std::memory_order_relaxed);
    });
    const bool drained = orange::session::wait_for_recording_run_drain(
        &no_pipelines,
        &camera_control,
        std::chrono::seconds(5),
        "recording_session_drain_tests wait");
    finalizer.join();
    require(drained, "wait must succeed once the last recorder finalizes");
}

void test_clear_recording_run_state_clears_all_residual_fields()
{
    CameraControl camera_control;
    fill_recording_run_fields(&camera_control);
    camera_control.recording_draining = true;
    camera_control.stop_record = true;
    camera_control.preserve_recording_session_state = true;

    orange::session::clear_recording_run_state(&camera_control);

    require(!camera_control.record_video && !camera_control.recording_draining &&
                !camera_control.stop_record,
            "clear_recording_run_state must clear run/drain latches");
    require(!camera_control.preserve_recording_session_state,
            "clear_recording_run_state must clear preserve_recording_session_state");
    require(camera_control.latest_recording_frame_id.load(std::memory_order_relaxed) == 0,
            "clear_recording_run_state must reset latest_recording_frame_id");
    std::lock_guard<std::mutex> lock(camera_control.recording_folder_mutex);
    require(camera_control.recording_folder.empty() &&
                camera_control.recording_output_folder.empty() &&
                camera_control.pending_recording_output_folder.empty(),
            "clear_recording_run_state must clear all recording folders");
    require(camera_control.recording_rollover_at_frame_id == 0 &&
                camera_control.recording_rollover_request_id == 0 &&
                camera_control.recording_rollover_completed_request_id == 0 &&
                camera_control.recording_rollover_completed_frame_id == 0 &&
                camera_control.recording_rollover_completed_folder.empty(),
            "clear_recording_run_state must clear all rollover bookkeeping");
}

void test_drain_and_shutdown_recording_run_full_sequence()
{
    CameraControl camera_control;
    fill_recording_run_fields(&camera_control);
    camera_control.preserve_recording_session_state = true;
    std::vector<std::unique_ptr<ModernRecordingPipeline>> no_pipelines;

    orange::session::drain_and_shutdown_recording_run(
        &no_pipelines,
        "external_ipc",
        &camera_control,
        std::chrono::milliseconds(100),
        "recording_session_drain_tests shutdown");

    require(!camera_control.record_video && !camera_control.recording_draining &&
                !camera_control.stop_record,
            "drain_and_shutdown must leave no run/drain latches set");
    std::lock_guard<std::mutex> lock(camera_control.recording_folder_mutex);
    require(camera_control.recording_folder.empty() &&
                camera_control.recording_output_folder.empty(),
            "drain_and_shutdown must clear residual folders even when the run "
            "requested session-state preservation");
}

}  // namespace

int main()
{
    struct TestCase {
        const char* name;
        void (*fn)();
    };

    const TestCase tests[] = {
        {"idle_stop_takes_early_drain_shortcut",
         test_idle_stop_takes_early_drain_shortcut},
        {"idle_stop_preserves_session_folder_when_requested",
         test_idle_stop_preserves_session_folder_when_requested},
        {"active_stop_latches_drain_flags",
         test_active_stop_latches_drain_flags},
        {"drain_flag_reassert_rule",
         test_drain_flag_reassert_rule},
        {"request_drain_external_ipc_with_drained_pipelines",
         test_request_drain_external_ipc_with_drained_pipelines},
        {"state_overload_delegates_to_core",
         test_state_overload_delegates_to_core},
        {"combined_drained_predicate",
         test_combined_drained_predicate},
        {"wait_for_recording_run_drain_times_out_and_completes",
         test_wait_for_recording_run_drain_times_out_and_completes},
        {"clear_recording_run_state_clears_all_residual_fields",
         test_clear_recording_run_state_clears_all_residual_fields},
        {"drain_and_shutdown_recording_run_full_sequence",
         test_drain_and_shutdown_recording_run_full_sequence},
    };

    for (const TestCase& test : tests) {
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
