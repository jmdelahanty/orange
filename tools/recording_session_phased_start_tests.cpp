// Locks down the phased recording-run start API
// (orange::session::prepare_recording_run /
// start_prepared_recording_run_supervisors / complete_recording_run /
// abort_prepared_recording_run) that the GUI operator path drives across a
// background thread, plus the synchronous begin_recording_run wrapper the
// headless-style callers keep using.
//
// Semantics locked in (no cameras or NVENC hardware required):
// - prepare_recording_run claims the recording folder on CameraControl,
//   creates the run scaffolding (folder, snapshot, external recorder
//   contract artifact), and never flips record_video.
// - start_prepared_recording_run_supervisors with a nonexistent recorder
//   binary fails fast (the forked child _exit(127)s before socket
//   readiness) and reports the failure without touching CameraControl.
// - complete_recording_run on a failed outcome performs exactly the
//   synchronous failed-start cleanup: record_video stays false, the drain
//   latches stay clear, the folder claim is released, and the failed
//   lifecycle state (with its error) is installed for the GUI status line.
// - complete_recording_run on a successful outcome flips record_video only
//   at completion and writes the supervisor plan artifact.
// - abort_prepared_recording_run stops the already-spawned recorder
//   processes and restores the pre-start state without flipping
//   record_video (the cancel-during-pending path).
// - begin_recording_run (the synchronous wrapper) reports the same failure
//   with the same cleanup.

#include "session/recording_session.h"

#include "external_recorder_lifecycle.h"
#include "video_capture.h"

#include "NvEncoder/Logger.h"

#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <sys/types.h>
#include <unistd.h>

// The NvEnc/CUDA sources linked into this target log through this global.
simplelogger::Logger* logger =
    simplelogger::LoggerFactory::CreateConsoleLogger();

namespace {

std::filesystem::path g_binary_dir;

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
        ("orange_phased_start_" + tag + "_" +
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
    camera.camera_name = "phased_test_cam";
    camera.camera_serial = "990001";
    camera.num_cameras = 1;
    return camera;
}

std::string make_socket_path(const std::string& tag)
{
    return "/tmp/orange_phased_start_" + tag + "_" +
           std::to_string(static_cast<long long>(getpid())) + ".sock";
}

nlohmann::json make_contract_config(const std::string& socket_path)
{
    return {
        {"streams", {
            {"990001", {
                {"socket_path", socket_path},
            }},
        }},
    };
}

// Shortens the supervisor timeouts and points the recorder tool at the
// given binary so the tests stay fast and hardware-free.
void configure_lifecycle_options_for_test(
    orange::session::PreparedRecordingRunStart* prepared,
    const std::string& recorder_tool_path)
{
    prepared->external_recorder_lifecycle_options.recorder_tool_path =
        recorder_tool_path;
    auto& process_options =
        prepared->external_recorder_lifecycle_options.process_options;
    process_options.socket_ready_timeout_ms = 2000;
    process_options.graceful_shutdown_timeout_ms = 20;
    process_options.terminate_timeout_ms = 2000;
    process_options.allow_regular_file_socket_ready_for_tests = true;
}

void require_failed_start_state_restored(
    CameraControl* camera_control,
    const std::string& message_prefix)
{
    require(!camera_control->record_video,
            message_prefix + ": record_video must stay false");
    require(!camera_control->recording_draining,
            message_prefix + ": recording_draining must stay clear");
    require(!camera_control->stop_record,
            message_prefix + ": stop_record must stay clear");
    const std::string folder =
        orange::session::current_recording_folder(camera_control);
    require(folder.empty(),
            message_prefix + ": the recording folder claim must be released");
}

void test_prepare_creates_run_scaffolding()
{
    const std::filesystem::path base = make_temp_base_folder("prepare");
    CameraControl camera_control;
    orange::session::RecordingSessionState state;
    state.recording_sink_mode = "external_ipc";
    CameraParams camera = make_camera_params();
    CameraEachSelect select;
    select.record = true;
    PTPParams ptp{};
    const std::string socket_path = make_socket_path("prepare");
    const nlohmann::json contract_config = make_contract_config(socket_path);

    orange::session::PreparedRecordingRunStart prepared =
        orange::session::prepare_recording_run(
            &state,
            &camera_control,
            &camera,
            &select,
            1,
            base.string(),
            &ptp,
            "external_ipc",
            &contract_config);

    require(prepared.valid, "prepare must succeed: " + prepared.error_message);
    require(prepared.external_recorder_requested,
            "external_ipc sink must request the external recorder");
    require(prepared.requires_supervisor_start(),
            "external_ipc prepare requires a supervisor start phase");
    require(!prepared.recording_folder.empty() &&
                std::filesystem::exists(prepared.recording_folder),
            "prepare must create the recording folder");
    require(prepared.recording_folder.rfind(base.string(), 0) == 0,
            "recording folder must live under the base folder");
    require(std::filesystem::exists(prepared.external_recorder_contract_path),
            "prepare must write the external recorder contract artifact");
    require(orange::session::current_recording_folder(&camera_control) ==
                prepared.recording_folder,
            "prepare must claim the recording folder on CameraControl");
    require(!camera_control.record_video,
            "prepare must never flip record_video");
    require(state.external_recorder_contract_path ==
                prepared.external_recorder_contract_path,
            "prepare must record the contract path in the session state");

    // Cancel the never-started run; nothing was spawned, so this only
    // releases the folder claim.
    orange::session::abort_prepared_recording_run(
        &state,
        &camera_control,
        prepared,
        orange::session::RecordingRunSupervisorStartOutcome{},
        "test_prepare_cleanup");
    require_failed_start_state_restored(&camera_control, "prepare cleanup");
    std::filesystem::remove_all(base);
    std::cout << "PASS test_prepare_creates_run_scaffolding" << std::endl;
}

void test_supervisor_failure_fails_loudly_and_cleans_up()
{
    const std::filesystem::path base = make_temp_base_folder("fail");
    CameraControl camera_control;
    orange::session::RecordingSessionState state;
    state.recording_sink_mode = "external_ipc";
    CameraParams camera = make_camera_params();
    CameraEachSelect select;
    select.record = true;
    PTPParams ptp{};
    const std::string socket_path = make_socket_path("fail");
    const nlohmann::json contract_config = make_contract_config(socket_path);

    orange::session::PreparedRecordingRunStart prepared =
        orange::session::prepare_recording_run(
            &state,
            &camera_control,
            &camera,
            &select,
            1,
            base.string(),
            &ptp,
            "external_ipc",
            &contract_config);
    require(prepared.valid, "prepare must succeed: " + prepared.error_message);
    configure_lifecycle_options_for_test(
        &prepared, "/nonexistent/orange_phased_start_recorder_binary");

    orange::session::RecordingRunSupervisorStartOutcome outcome =
        orange::session::start_prepared_recording_run_supervisors(prepared);
    require(!outcome.ok, "a nonexistent recorder binary must fail the start");
    require(!outcome.error_message.empty(),
            "the failed start must carry an error message");
    require(outcome.external_recorder_attempted,
            "the failed start must record the attempt");
    require(!outcome.external_recorder_lifecycle.started,
            "the failed lifecycle must not report started");
    require(!camera_control.record_video,
            "the background phase must never touch record_video");

    const orange::session::RecordingRunStartResult result =
        orange::session::complete_recording_run(
            &state,
            &camera_control,
            &camera,
            1,
            &ptp,
            prepared,
            std::move(outcome));
    require(!result.ok, "completion of a failed outcome must fail");
    require(!result.error_message.empty(),
            "the failed completion must surface the error message");
    require(result.recording_folder == prepared.recording_folder,
            "the failed completion must name the abandoned folder");
    require(result.external_recorder_contract_path ==
                prepared.external_recorder_contract_path,
            "the failed completion must reference the contract artifact");
    require(!state.external_recorder_last_error.empty(),
            "the session state must carry the loud error for the GUI");
    require(!state.external_recorder_lifecycle.started,
            "no lifecycle may remain started after a failed completion");
    require_failed_start_state_restored(&camera_control, "failed completion");
    std::filesystem::remove_all(base);
    std::cout << "PASS test_supervisor_failure_fails_loudly_and_cleans_up"
              << std::endl;
}

void test_sync_begin_recording_run_failure_parity()
{
    const std::filesystem::path base = make_temp_base_folder("sync_fail");
    CameraControl camera_control;
    orange::session::RecordingSessionState state;
    state.recording_sink_mode = "external_ipc";
    CameraParams camera = make_camera_params();
    CameraEachSelect select;
    select.record = true;
    PTPParams ptp{};
    const std::string socket_path = make_socket_path("sync_fail");
    const nlohmann::json contract_config = make_contract_config(socket_path);

    // begin_recording_run resolves the recorder tool through the
    // environment; point it at a nonexistent binary for the failure case.
    setenv("ORANGE_EXTERNAL_RECORDER_TOOL",
           "/nonexistent/orange_phased_start_recorder_binary", 1);
    const orange::session::RecordingRunStartResult result =
        orange::session::begin_recording_run(
            &state,
            &camera_control,
            &camera,
            &select,
            1,
            base.string(),
            &ptp,
            "external_ipc",
            &contract_config);
    unsetenv("ORANGE_EXTERNAL_RECORDER_TOOL");

    require(!result.ok, "the synchronous wrapper must report the failure");
    require(!result.error_message.empty(),
            "the synchronous failure must carry an error message");
    require(!state.external_recorder_last_error.empty(),
            "the synchronous failure must set the session error state");
    require(!state.external_recorder_lifecycle.started,
            "no lifecycle may remain started after a synchronous failure");
    require_failed_start_state_restored(&camera_control, "synchronous failure");
    std::filesystem::remove_all(base);
    std::cout << "PASS test_sync_begin_recording_run_failure_parity" << std::endl;
}

void test_successful_start_flips_record_video_at_completion()
{
    const std::filesystem::path stub_path =
        g_binary_dir / "external_recorder_supervisor_socket_stub";
    require(std::filesystem::exists(stub_path), "socket stub helper is missing");

    const std::filesystem::path base = make_temp_base_folder("success");
    CameraControl camera_control;
    orange::session::RecordingSessionState state;
    state.recording_sink_mode = "external_ipc";
    CameraParams camera = make_camera_params();
    CameraEachSelect select;
    select.record = true;
    PTPParams ptp{};
    const std::string socket_path = make_socket_path("success");
    const nlohmann::json contract_config = make_contract_config(socket_path);

    orange::session::PreparedRecordingRunStart prepared =
        orange::session::prepare_recording_run(
            &state,
            &camera_control,
            &camera,
            &select,
            1,
            base.string(),
            &ptp,
            "external_ipc",
            &contract_config);
    require(prepared.valid, "prepare must succeed: " + prepared.error_message);
    configure_lifecycle_options_for_test(&prepared, stub_path.string());

    orange::session::RecordingRunSupervisorStartOutcome outcome =
        orange::session::start_prepared_recording_run_supervisors(prepared);
    require(outcome.ok, "stub-backed start must succeed: " + outcome.error_message);
    require(outcome.external_recorder_lifecycle.started,
            "the started lifecycle must report started");
    require(!camera_control.record_video,
            "record_video must stay false until completion (recording"
            " semantics unchanged)");

    const orange::session::RecordingRunStartResult result =
        orange::session::complete_recording_run(
            &state,
            &camera_control,
            &camera,
            1,
            &ptp,
            prepared,
            std::move(outcome));
    require(result.ok, "completion must succeed: " + result.error_message);
    require(camera_control.record_video,
            "completion must flip record_video true");
    require(state.external_recorder_lifecycle.started,
            "the lifecycle must be installed and started after completion");
    require(state.external_recorder_last_error.empty(),
            "a successful completion must leave no error");
    require(!result.external_recorder_supervisor_plan_path.empty() &&
                std::filesystem::exists(
                    result.external_recorder_supervisor_plan_path),
            "completion must write the supervisor plan artifact");

    // Tear down the stub-backed run.
    std::string stop_error;
    (void)orange::external_recorder::StopSupervisedRecorderLifecycle(
        &state.external_recorder_lifecycle, &stop_error);
    orange::session::clear_recording_run_state(&camera_control);
    std::filesystem::remove(socket_path);
    std::filesystem::remove_all(base);
    std::cout << "PASS test_successful_start_flips_record_video_at_completion"
              << std::endl;
}

void test_abort_pending_start_stops_recorders_and_restores_state()
{
    const std::filesystem::path stub_path =
        g_binary_dir / "external_recorder_supervisor_socket_stub";
    require(std::filesystem::exists(stub_path), "socket stub helper is missing");

    const std::filesystem::path base = make_temp_base_folder("abort");
    CameraControl camera_control;
    orange::session::RecordingSessionState state;
    state.recording_sink_mode = "external_ipc";
    CameraParams camera = make_camera_params();
    CameraEachSelect select;
    select.record = true;
    PTPParams ptp{};
    const std::string socket_path = make_socket_path("abort");
    const nlohmann::json contract_config = make_contract_config(socket_path);

    orange::session::PreparedRecordingRunStart prepared =
        orange::session::prepare_recording_run(
            &state,
            &camera_control,
            &camera,
            &select,
            1,
            base.string(),
            &ptp,
            "external_ipc",
            &contract_config);
    require(prepared.valid, "prepare must succeed: " + prepared.error_message);
    configure_lifecycle_options_for_test(&prepared, stub_path.string());

    orange::session::RecordingRunSupervisorStartOutcome outcome =
        orange::session::start_prepared_recording_run_supervisors(prepared);
    require(outcome.ok, "stub-backed start must succeed: " + outcome.error_message);
    require(outcome.external_recorder_lifecycle.runtime.processes.size() == 1,
            "the stub run must have one recorder process");
    const pid_t stub_pid =
        outcome.external_recorder_lifecycle.runtime.processes[0].pid;
    require(stub_pid > 0, "the stub recorder must have a pid");

    orange::session::abort_prepared_recording_run(
        &state,
        &camera_control,
        prepared,
        std::move(outcome),
        "test_cancel");

    require(!camera_control.record_video,
            "abort must never flip record_video");
    require(!state.external_recorder_lifecycle.started,
            "abort must stop the pending lifecycle");
    require(kill(stub_pid, 0) != 0,
            "abort must reap the spawned recorder process");
    require_failed_start_state_restored(&camera_control, "abort");
    std::filesystem::remove(socket_path);
    std::filesystem::remove_all(base);
    std::cout << "PASS test_abort_pending_start_stops_recorders_and_restores_state"
              << std::endl;
}

}  // namespace

int main(int, char** argv)
{
    if (argv && argv[0]) {
        g_binary_dir = std::filesystem::absolute(argv[0]).parent_path();
    }
    try {
        test_prepare_creates_run_scaffolding();
        test_supervisor_failure_fails_loudly_and_cleans_up();
        test_sync_begin_recording_run_failure_parity();
        test_successful_start_flips_record_video_at_completion();
        test_abort_pending_start_stops_recorders_and_restores_state();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << std::endl;
        return 1;
    }
    std::cout << "All recording_session_phased_start tests passed." << std::endl;
    return 0;
}
