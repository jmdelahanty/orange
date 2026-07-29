// Unit tests for the GUI autorun state machine extracted to src/gui/autorun.
//
// gui_autorun_update() is driven once per GUI frame. Its contract:
//   - disabled config (or null state/camera_control) never emits requests;
//   - each action stage emits its request exactly once (guarded by
//     state->action_requested) and advances when the corresponding
//     CameraControl flag flips, with stream readiness additionally requiring a
//     complete startup-timing report;
//   - stage timeouts are hardcoded wall-clock checks against
//     state->stage_started_at (std::chrono::steady_clock::now() is read
//     directly); tests exercise them by backdating stage_started_at.

#include "gui/autorun.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using orange::gui::GuiAutorunConfig;
using orange::gui::GuiAutorunRequests;
using orange::gui::GuiAutorunStage;
using orange::gui::GuiAutorunState;
using orange::gui::GuiRecordingRunState;
using orange::gui::GuiStartupTimingStatus;
using orange::gui::gui_autorun_stage_name;
using orange::gui::gui_autorun_update;
using orange::gui::resolve_gui_autorun_config;

namespace {

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class ScopedEnv {
public:
    explicit ScopedEnv(std::string name) : name_(std::move(name))
    {
        if (const char* value = std::getenv(name_.c_str())) {
            had_original_ = true;
            original_ = value;
        }
        unsetenv(name_.c_str());
    }

    ~ScopedEnv()
    {
        if (had_original_) {
            setenv(name_.c_str(), original_.c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
    }

    void Set(const std::string& value)
    {
        setenv(name_.c_str(), value.c_str(), 1);
    }

private:
    std::string name_;
    bool had_original_ = false;
    std::string original_;
};

bool no_requests(const GuiAutorunRequests& requests)
{
    return !requests.open_cameras &&
           !requests.toggle_streaming &&
           !requests.cancel_stream_startup &&
           !requests.toggle_recording &&
           !requests.close_window;
}

struct RequestTotals {
    int open_cameras = 0;
    int toggle_streaming = 0;
    int cancel_stream_startup = 0;
    int toggle_recording = 0;
    int close_window = 0;

    void Add(const GuiAutorunRequests& requests)
    {
        open_cameras += requests.open_cameras ? 1 : 0;
        toggle_streaming += requests.toggle_streaming ? 1 : 0;
        cancel_stream_startup += requests.cancel_stream_startup ? 1 : 0;
        toggle_recording += requests.toggle_recording ? 1 : 0;
        close_window += requests.close_window ? 1 : 0;
    }
};

GuiAutorunConfig make_enabled_config(const std::string& config_dir)
{
    GuiAutorunConfig config;
    config.enabled = true;
    config.config_dir = config_dir;
    config.stream_warmup_seconds = 0;
    config.record_seconds = 1;
    config.exit_after_finalize = false;
    return config;
}

void backdate_stage(GuiAutorunState* state, const int seconds)
{
    state->stage_started_at -= std::chrono::seconds(seconds);
}

void test_resolve_config_defaults_and_env_overrides()
{
    ScopedEnv autorun("ORANGE_GUI_AUTORUN");
    ScopedEnv warmup("ORANGE_GUI_AUTORUN_STREAM_WARMUP_SECONDS");
    ScopedEnv record("ORANGE_GUI_AUTORUN_RECORD_SECONDS");
    ScopedEnv exit_after("ORANGE_GUI_AUTORUN_EXIT_AFTER_FINALIZE");
    ScopedEnv start_recording("ORANGE_GUI_AUTORUN_START_RECORDING");
    ScopedEnv stop_after_warmup(
        "ORANGE_GUI_AUTORUN_STOP_STREAMING_AFTER_WARMUP");
    ScopedEnv cancel_startup_after(
        "ORANGE_GUI_AUTORUN_CANCEL_STREAM_STARTUP_AFTER_MS");
    ScopedEnv config_dir("ORANGE_GUI_CONFIG_DIR");

    const GuiAutorunConfig defaults = resolve_gui_autorun_config();
    require(!defaults.enabled, "autorun must default to disabled");
    require(defaults.stream_warmup_seconds == 3, "default warmup is 3s");
    require(defaults.record_seconds == 10, "default record window is 10s");
    require(defaults.start_recording, "recording defaults to on");
    require(!defaults.stop_streaming_after_warmup,
            "stream-only autorun defaults to leaving the stream active");
    require(defaults.cancel_stream_startup_after_ms == -1,
            "startup cancellation diagnostic defaults to disabled");
    require(defaults.config_dir.empty(), "config dir defaults to empty");

    autorun.Set("1");
    warmup.Set("0");
    record.Set("0");  // below the minimum of 1; must be raised
    exit_after.Set("true");
    start_recording.Set("off");
    stop_after_warmup.Set("on");
    cancel_startup_after.Set("250");
    config_dir.Set("/tmp/orange_autorun_cfg");

    const GuiAutorunConfig overridden = resolve_gui_autorun_config();
    require(overridden.enabled, "ORANGE_GUI_AUTORUN=1 enables autorun");
    require(overridden.stream_warmup_seconds == 0, "warmup override applies");
    require(overridden.record_seconds == 1, "record seconds clamps to minimum 1");
    require(overridden.exit_after_finalize, "exit_after_finalize override applies");
    require(!overridden.start_recording, "start_recording=off applies");
    require(overridden.stop_streaming_after_warmup,
            "explicit stream-only teardown applies");
    require(overridden.cancel_stream_startup_after_ms == 250,
            "startup cancellation delay applies");
    require(overridden.config_dir == "/tmp/orange_autorun_cfg",
            "config dir override applies");
}

void test_disabled_config_never_emits_requests()
{
    GuiAutorunConfig config;  // enabled == false
    GuiAutorunState state;
    CameraControl camera_control;
    camera_control.open = true;
    camera_control.subscribe = true;
    camera_control.record_video = true;
    GuiRecordingRunState recording_run;
    GuiStartupTimingStatus startup_status;
    const std::vector<std::string> folders = {"/tmp/orange_autorun_cfg"};
    int select = -1;

    for (int frame = 0; frame < 10; ++frame) {
        const GuiAutorunRequests requests = gui_autorun_update(
            &state, config, folders, &select, &camera_control,
            &recording_run, false, &startup_status);
        require(no_requests(requests), "disabled config emits no requests");
        require(state.stage == GuiAutorunStage::kDisabled,
                "disabled config never leaves kDisabled");
    }
    require(select == -1, "disabled config leaves config selection untouched");

    // Null camera_control is likewise inert, even with an enabled config.
    GuiAutorunConfig enabled = make_enabled_config("/tmp/orange_autorun_cfg");
    const GuiAutorunRequests requests = gui_autorun_update(
        &state, enabled, folders, &select, nullptr, &recording_run, false);
    require(no_requests(requests), "null camera_control emits no requests");
}

void test_happy_path_progression()
{
    const std::string config_dir = "/tmp/orange_autorun_cfg";
    const GuiAutorunConfig config = make_enabled_config(config_dir);
    GuiAutorunState state;
    CameraControl camera_control;
    GuiRecordingRunState recording_run;
    GuiStartupTimingStatus startup_status;
    const std::vector<std::string> folders = {"/tmp/other_cfg", config_dir};
    int select = -1;
    RequestTotals totals;

    const auto step = [&]() {
        const GuiAutorunRequests requests = gui_autorun_update(
            &state, config, folders, &select, &camera_control,
            &recording_run, false, &startup_status);
        totals.Add(requests);
        return requests;
    };

    // kDisabled -> kSelectConfig (no request).
    require(no_requests(step()), "kDisabled transition emits no request");
    require(state.stage == GuiAutorunStage::kSelectConfig, "enters kSelectConfig");

    // kSelectConfig -> kOpenCameras, selecting the matching folder.
    require(no_requests(step()), "config selection emits no request");
    require(select == 1, "matching config folder index is selected");
    require(state.stage == GuiAutorunStage::kOpenCameras, "enters kOpenCameras");

    // kOpenCameras: exactly one open request while cameras stay closed.
    require(step().open_cameras, "kOpenCameras requests camera open");
    require(no_requests(step()), "camera open is requested only once");
    camera_control.open = true;
    require(no_requests(step()), "camera open transition emits no request");
    require(state.stage == GuiAutorunStage::kStartStreaming, "enters kStartStreaming");

    // kStartStreaming: exactly one stream toggle.
    require(step().toggle_streaming, "kStartStreaming requests stream start");
    require(no_requests(step()), "stream start is requested only once");
    camera_control.subscribe = true;
    startup_status.available = true;
    startup_status.transaction_id = "stream_start_test";
    startup_status.operation = "stream_start";
    startup_status.status = "awaiting_first_frames";
    startup_status.expected_first_frame_camera_count = 2;
    startup_status.observed_first_frame_camera_count = 1;
    require(no_requests(step()), "pending first frames emit no request");
    require(state.stage == GuiAutorunStage::kStartStreaming,
            "subscribe alone does not satisfy stream readiness");
    startup_status.status = "complete";
    startup_status.observed_first_frame_camera_count = 2;
    require(no_requests(step()), "complete stream startup emits no request");
    require(state.stage == GuiAutorunStage::kStreamWarmup, "enters kStreamWarmup");

    // kStreamWarmup: zero-second warmup advances immediately.
    require(no_requests(step()), "warmup emits no request");
    require(state.stage == GuiAutorunStage::kStartRecording, "enters kStartRecording");

    // kStartRecording: exactly one recording toggle.
    require(step().toggle_recording, "kStartRecording requests recording start");
    require(no_requests(step()), "recording start is requested only once");
    camera_control.record_video = true;
    require(no_requests(step()), "recording start transition emits no request");
    require(state.stage == GuiAutorunStage::kRecording, "enters kRecording");

    // kRecording: the 1s record window elapses (backdated to avoid sleeping).
    backdate_stage(&state, 2);
    require(no_requests(step()), "record window expiry emits no request");
    require(state.stage == GuiAutorunStage::kStopRecording, "enters kStopRecording");

    // kStopRecording: exactly one recording toggle to stop.
    require(step().toggle_recording, "kStopRecording requests recording stop");
    require(no_requests(step()), "recording stop is requested only once");
    camera_control.record_video = false;
    require(no_requests(step()), "recording stop transition emits no request");
    require(state.stage == GuiAutorunStage::kStopStreaming, "enters kStopStreaming");

    // kStopStreaming: exactly one stream toggle to stop.
    require(step().toggle_streaming, "kStopStreaming requests stream stop");
    require(no_requests(step()), "stream stop is requested only once");
    camera_control.subscribe = false;
    require(no_requests(step()), "stream stop transition emits no request");
    require(state.stage == GuiAutorunStage::kDone, "reaches terminal kDone");

    // Terminal stage without exit_after_finalize stays quiet.
    for (int frame = 0; frame < 5; ++frame) {
        require(no_requests(step()), "kDone emits no requests");
        require(state.stage == GuiAutorunStage::kDone, "kDone is terminal");
    }

    require(totals.open_cameras == 1, "camera open requested exactly once");
    require(totals.toggle_streaming == 2,
            "stream toggled exactly twice (start + stop)");
    require(totals.toggle_recording == 2,
            "recording toggled exactly twice (start + stop)");
    require(totals.close_window == 0,
            "no close request without exit_after_finalize");
}

void test_missing_config_dir_fails()
{
    GuiAutorunConfig config = make_enabled_config("/tmp/orange_missing_cfg");
    GuiAutorunState state;
    CameraControl camera_control;
    GuiRecordingRunState recording_run;
    const std::vector<std::string> folders = {"/tmp/other_cfg"};
    int select = -1;

    const auto step = [&]() {
        return gui_autorun_update(
            &state, config, folders, &select, &camera_control,
            &recording_run, false);
    };

    require(no_requests(step()), "kDisabled transition emits no request");
    require(no_requests(step()), "config-dir failure emits no request");
    require(state.stage == GuiAutorunStage::kFailed,
            "missing config dir reaches kFailed");
    require(!state.error_message.empty(), "failure records an error message");
    require(select == -1, "failed selection leaves index untouched");
    require(std::string(gui_autorun_stage_name(state.stage)) == "failed",
            "failed stage name matches");

    for (int frame = 0; frame < 5; ++frame) {
        require(no_requests(step()), "kFailed emits no further requests");
        require(state.stage == GuiAutorunStage::kFailed, "kFailed is terminal");
    }

    // With exit_after_finalize the failed terminal stage asks to close the
    // window exactly once.
    config.exit_after_finalize = true;
    require(step().close_window, "kFailed requests window close once");
    require(no_requests(step()), "window close is requested only once");
}

void test_open_cameras_timeout_fails()
{
    const std::string config_dir = "/tmp/orange_autorun_cfg";
    const GuiAutorunConfig config = make_enabled_config(config_dir);
    GuiAutorunState state;
    CameraControl camera_control;
    GuiRecordingRunState recording_run;
    const std::vector<std::string> folders = {config_dir};
    int select = -1;

    const auto step = [&]() {
        return gui_autorun_update(
            &state, config, folders, &select, &camera_control,
            &recording_run, false);
    };

    require(no_requests(step()), "kDisabled transition emits no request");
    require(no_requests(step()), "config selection emits no request");
    require(state.stage == GuiAutorunStage::kOpenCameras, "enters kOpenCameras");
    require(step().open_cameras, "kOpenCameras requests camera open");

    // Cameras never open; backdate past the hardcoded 30s stage timeout.
    backdate_stage(&state, 31);
    require(no_requests(step()), "timeout transition emits no request");
    require(state.stage == GuiAutorunStage::kFailed,
            "camera open timeout reaches kFailed");
    require(state.error_message == "timed out opening cameras",
            "timeout failure records its message");

    for (int frame = 0; frame < 5; ++frame) {
        require(no_requests(step()), "kFailed emits no further requests");
        require(state.stage == GuiAutorunStage::kFailed, "kFailed is terminal");
    }
}

void test_warmup_abort_and_calibration_busy_gate()
{
    const std::string config_dir = "/tmp/orange_autorun_cfg";
    GuiAutorunConfig config = make_enabled_config(config_dir);
    config.stream_warmup_seconds = 60;  // keep warmup pending
    GuiAutorunState state;
    CameraControl camera_control;
    GuiRecordingRunState recording_run;
    GuiStartupTimingStatus startup_status;
    const std::vector<std::string> folders = {config_dir};
    int select = -1;

    const auto step = [&](const bool calibration_busy) {
        return gui_autorun_update(
            &state, config, folders, &select, &camera_control,
            &recording_run, calibration_busy, &startup_status);
    };

    require(no_requests(step(false)), "kDisabled transition emits no request");
    require(no_requests(step(false)), "config selection emits no request");

    // A busy calibration tool defers the open request without failing.
    require(no_requests(step(true)), "calibration busy defers camera open");
    require(state.stage == GuiAutorunStage::kOpenCameras,
            "busy calibration tool does not fail the stage");
    require(step(false).open_cameras, "camera open requested once tool is idle");

    camera_control.open = true;
    require(no_requests(step(false)), "camera open transition emits no request");
    require(step(false).toggle_streaming, "stream start requested");
    camera_control.subscribe = true;
    startup_status.available = true;
    startup_status.operation = "stream_start";
    startup_status.status = "complete";
    startup_status.expected_first_frame_camera_count = 4;
    startup_status.observed_first_frame_camera_count = 4;
    require(no_requests(step(false)), "stream start transition emits no request");
    require(state.stage == GuiAutorunStage::kStreamWarmup, "enters kStreamWarmup");

    // Stream drops during warmup -> failure.
    camera_control.subscribe = false;
    require(no_requests(step(false)), "warmup abort emits no request");
    require(state.stage == GuiAutorunStage::kFailed,
            "stream drop during warmup reaches kFailed");
    require(state.error_message == "stream stopped during warmup",
            "warmup failure records its message");
}

void test_stream_startup_failure_fails_immediately()
{
    const std::string config_dir = "/tmp/orange_autorun_cfg";
    const GuiAutorunConfig config = make_enabled_config(config_dir);
    GuiAutorunState state;
    state.stage = GuiAutorunStage::kStartStreaming;
    state.stage_started_at = std::chrono::steady_clock::now();
    state.action_requested = true;
    CameraControl camera_control;
    camera_control.open = true;
    GuiRecordingRunState recording_run;
    GuiStartupTimingStatus startup_status;
    startup_status.available = true;
    startup_status.operation = "stream_start";
    startup_status.status = "failed";
    startup_status.failure_reason = "recording_preflight_failed";
    const std::vector<std::string> folders = {config_dir};
    int select = 0;

    const GuiAutorunRequests requests = gui_autorun_update(
        &state, config, folders, &select, &camera_control,
        &recording_run, false, &startup_status);
    require(no_requests(requests), "failed startup emits no further request");
    require(state.stage == GuiAutorunStage::kFailed,
            "failed timing report fails autorun immediately");
    require(state.error_message.find("recording_preflight_failed") !=
                std::string::npos,
            "autorun failure includes startup failure evidence");
}

void test_stream_startup_completion_timeout_fails()
{
    const std::string config_dir = "/tmp/orange_autorun_cfg";
    const GuiAutorunConfig config = make_enabled_config(config_dir);
    GuiAutorunState state;
    state.stage = GuiAutorunStage::kStartStreaming;
    state.stage_started_at =
        std::chrono::steady_clock::now() - std::chrono::seconds(46);
    state.action_requested = true;
    CameraControl camera_control;
    camera_control.open = true;
    camera_control.subscribe = true;
    GuiRecordingRunState recording_run;
    GuiStartupTimingStatus startup_status;
    startup_status.available = true;
    startup_status.operation = "stream_start";
    startup_status.status = "awaiting_first_frames";
    startup_status.expected_first_frame_camera_count = 4;
    startup_status.observed_first_frame_camera_count = 3;
    const std::vector<std::string> folders = {config_dir};
    int select = 0;

    const GuiAutorunRequests requests = gui_autorun_update(
        &state, config, folders, &select, &camera_control,
        &recording_run, false, &startup_status);
    require(no_requests(requests), "startup readiness timeout emits no request");
    require(state.stage == GuiAutorunStage::kFailed,
            "missing first frame fails autorun after its startup timeout");
    require(state.error_message ==
                "timed out waiting for complete stream startup timing",
            "startup timeout identifies the readiness barrier");
}

void test_stream_only_autorun_stops_before_done()
{
    const std::string config_dir = "/tmp/orange_autorun_cfg";
    GuiAutorunConfig config = make_enabled_config(config_dir);
    config.start_recording = false;
    config.stop_streaming_after_warmup = true;
    GuiAutorunState state;
    state.stage = GuiAutorunStage::kStreamWarmup;
    state.stage_started_at = std::chrono::steady_clock::now();
    CameraControl camera_control;
    camera_control.open = true;
    camera_control.subscribe = true;
    GuiRecordingRunState recording_run;
    const std::vector<std::string> folders = {config_dir};
    int select = 0;

    GuiAutorunRequests requests = gui_autorun_update(
        &state, config, folders, &select, &camera_control,
        &recording_run, false);
    require(no_requests(requests), "warmup-to-stop transition emits no request");
    require(state.stage == GuiAutorunStage::kStopStreaming,
            "stream-only lifecycle enters explicit stop stage");

    requests = gui_autorun_update(
        &state, config, folders, &select, &camera_control,
        &recording_run, false);
    require(requests.toggle_streaming,
            "stream-only lifecycle requests ordinary stream stop");
    camera_control.subscribe = false;
    requests = gui_autorun_update(
        &state, config, folders, &select, &camera_control,
        &recording_run, false);
    require(no_requests(requests), "completed stream stop emits no request");
    require(state.stage == GuiAutorunStage::kDone,
            "stream-only lifecycle finishes only after stream teardown");
}

void test_expected_stream_startup_cancellation_is_terminal_success()
{
    const std::string config_dir = "/tmp/orange_autorun_cfg";
    GuiAutorunConfig config = make_enabled_config(config_dir);
    config.start_recording = false;
    config.cancel_stream_startup_after_ms = 100;
    config.exit_after_finalize = true;
    GuiAutorunState state;
    state.stage = GuiAutorunStage::kStartStreaming;
    state.stage_started_at =
        std::chrono::steady_clock::now() - std::chrono::seconds(1);
    state.action_requested = true;
    CameraControl camera_control;
    camera_control.open = true;
    GuiRecordingRunState recording_run;
    GuiStartupTimingStatus startup_status;
    startup_status.available = true;
    startup_status.transaction_id = "stream_start_cancel_test";
    startup_status.operation = "stream_start";
    startup_status.status = "running";
    const std::vector<std::string> folders = {config_dir};
    int select = 0;

    GuiAutorunRequests requests = gui_autorun_update(
        &state, config, folders, &select, &camera_control,
        &recording_run, false, &startup_status);
    require(requests.cancel_stream_startup,
            "cancellation diagnostic requests the startup-cancel path");
    require(!requests.toggle_streaming,
            "startup cancellation is distinct from an ordinary stream toggle");
    require(state.startup_cancel_requested,
            "cancellation diagnostic latches its request");

    requests = gui_autorun_update(
        &state, config, folders, &select, &camera_control,
        &recording_run, false, &startup_status);
    require(no_requests(requests), "cancellation request is emitted only once");

    startup_status.status = "stopped";
    startup_status.stop_reason = "stream_stop_requested_during_startup";
    requests = gui_autorun_update(
        &state, config, folders, &select, &camera_control,
        &recording_run, false, &startup_status);
    require(no_requests(requests), "expected stopped report emits no new action");
    require(state.stage == GuiAutorunStage::kDone,
            "expected startup cancellation is a successful terminal state");
    require(state.error_message.empty(),
            "expected startup cancellation does not create an autorun error");

    requests = gui_autorun_update(
        &state, config, folders, &select, &camera_control,
        &recording_run, false, &startup_status);
    require(requests.close_window,
            "successful cancellation closes through the autorun exit path");
}

void test_unexpected_stream_startup_stop_still_fails()
{
    const std::string config_dir = "/tmp/orange_autorun_cfg";
    const GuiAutorunConfig config = make_enabled_config(config_dir);
    GuiAutorunState state;
    state.stage = GuiAutorunStage::kStartStreaming;
    state.stage_started_at = std::chrono::steady_clock::now();
    state.action_requested = true;
    CameraControl camera_control;
    camera_control.open = true;
    GuiRecordingRunState recording_run;
    GuiStartupTimingStatus startup_status;
    startup_status.available = true;
    startup_status.operation = "stream_start";
    startup_status.status = "stopped";
    startup_status.stop_reason = "unexpected_stop";
    const std::vector<std::string> folders = {config_dir};
    int select = 0;

    const GuiAutorunRequests requests = gui_autorun_update(
        &state, config, folders, &select, &camera_control,
        &recording_run, false, &startup_status);
    require(no_requests(requests), "unexpected stopped report emits no action");
    require(state.stage == GuiAutorunStage::kFailed,
            "unexpected stopped report remains a hard autorun failure");
}

void test_expected_cancellation_rejects_completed_startup()
{
    const std::string config_dir = "/tmp/orange_autorun_cfg";
    GuiAutorunConfig config = make_enabled_config(config_dir);
    config.start_recording = false;
    config.cancel_stream_startup_after_ms = 500;
    GuiAutorunState state;
    state.stage = GuiAutorunStage::kStartStreaming;
    state.stage_started_at = std::chrono::steady_clock::now();
    CameraControl camera_control;
    camera_control.open = true;
    camera_control.subscribe = true;
    GuiRecordingRunState recording_run;
    GuiStartupTimingStatus startup_status;
    startup_status.available = true;
    startup_status.operation = "stream_start";
    startup_status.status = "complete";
    startup_status.expected_first_frame_camera_count = 4;
    startup_status.observed_first_frame_camera_count = 4;
    const std::vector<std::string> folders = {config_dir};
    int select = 0;

    const GuiAutorunRequests requests = gui_autorun_update(
        &state, config, folders, &select, &camera_control,
        &recording_run, false, &startup_status);
    require(no_requests(requests), "completed startup emits no cancellation action");
    require(state.stage == GuiAutorunStage::kFailed,
            "cancellation diagnostic rejects an ordinary completed startup");
}

void test_wait_finalize_stage()
{
    const std::string config_dir = "/tmp/orange_autorun_cfg";
    const GuiAutorunConfig config = make_enabled_config(config_dir);
    GuiAutorunState state;
    state.stage = GuiAutorunStage::kWaitFinalize;
    state.stage_started_at = std::chrono::steady_clock::now();
    CameraControl camera_control;
    GuiRecordingRunState recording_run;
    recording_run.finalizing = true;
    const std::vector<std::string> folders = {config_dir};
    int select = -1;

    const auto step = [&]() {
        return gui_autorun_update(
            &state, config, folders, &select, &camera_control,
            &recording_run, false);
    };

    require(no_requests(step()), "kWaitFinalize emits no request while active");
    require(state.stage == GuiAutorunStage::kWaitFinalize,
            "kWaitFinalize holds while the run is finalizing");

    recording_run.finalizing = false;
    require(no_requests(step()), "finalize completion emits no request");
    require(state.stage == GuiAutorunStage::kStopStreaming,
            "finalize completion advances to kStopStreaming");
}

}  // namespace

int main()
{
    const struct {
        const char* name;
        void (*run)();
    } tests[] = {
        {"resolve_config_defaults_and_env_overrides",
         test_resolve_config_defaults_and_env_overrides},
        {"disabled_config_never_emits_requests",
         test_disabled_config_never_emits_requests},
        {"happy_path_progression", test_happy_path_progression},
        {"missing_config_dir_fails", test_missing_config_dir_fails},
        {"open_cameras_timeout_fails", test_open_cameras_timeout_fails},
        {"warmup_abort_and_calibration_busy_gate",
         test_warmup_abort_and_calibration_busy_gate},
        {"stream_startup_failure_fails_immediately",
         test_stream_startup_failure_fails_immediately},
        {"stream_startup_completion_timeout_fails",
         test_stream_startup_completion_timeout_fails},
        {"stream_only_autorun_stops_before_done",
         test_stream_only_autorun_stops_before_done},
        {"expected_stream_startup_cancellation_is_terminal_success",
         test_expected_stream_startup_cancellation_is_terminal_success},
        {"unexpected_stream_startup_stop_still_fails",
         test_unexpected_stream_startup_stop_still_fails},
        {"expected_cancellation_rejects_completed_startup",
         test_expected_cancellation_rejects_completed_startup},
        {"wait_finalize_stage", test_wait_finalize_stage},
    };

    int failures = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            std::cout << "[PASS] " << test.name << std::endl;
        } catch (const std::exception& ex) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << ex.what() << std::endl;
        }
    }

    if (failures != 0) {
        std::cerr << failures << " gui_autorun test(s) failed" << std::endl;
        return 1;
    }
    std::cout << "gui_autorun_tests passed" << std::endl;
    return 0;
}
