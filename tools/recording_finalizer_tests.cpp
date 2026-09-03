// Tests for src/gui/recording_finalizer.{h,cpp} — the recording-session
// finalizer cluster extracted from orange.cpp. Covers the pure JSON/stream
// builders, the external rolling manifest writer (against a temp dir), and
// the gating contract of gui_finalize_recording_session_if_ready that the
// orange.cpp call sites rely on.

#include "gui/recording_finalizer.h"
#include "gui/env_util.h"

#include "external_recorder_contract_utils.h"
#include "external_recorder_supervisor.h"
#include "session/recording_session.h"
#include "video_capture.h"
#include "NvEncoder/Logger.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

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

class ScopedTempDir {
public:
    ScopedTempDir()
    {
        std::string tmpl =
            (std::filesystem::temp_directory_path() /
             "recording_finalizer_tests_XXXXXX").string();
        std::vector<char> buffer(tmpl.begin(), tmpl.end());
        buffer.push_back('\0');
        require(mkdtemp(buffer.data()) != nullptr,
                "failed to create temp directory for tests");
        path_ = buffer.data();
    }

    ~ScopedTempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

void write_text_file(const std::filesystem::path& path, const std::string& contents)
{
    std::ofstream out(path);
    require(static_cast<bool>(out), "failed to open " + path.string());
    out << contents;
    require(static_cast<bool>(out), "failed to write " + path.string());
}

void test_gui_env_int()
{
    ScopedEnv env("ORANGE_RECORDING_FINALIZER_TESTS_INT");
    require(gui_env_int("ORANGE_RECORDING_FINALIZER_TESTS_INT", 7, 0) == 7,
            "unset env should return the default");
    env.Set("42");
    require(gui_env_int("ORANGE_RECORDING_FINALIZER_TESTS_INT", 7, 0) == 42,
            "valid env value should be returned");
    env.Set("garbage");
    require(gui_env_int("ORANGE_RECORDING_FINALIZER_TESTS_INT", 7, 0) == 7,
            "invalid env value should fall back to the default");
    env.Set("-3");
    require(gui_env_int("ORANGE_RECORDING_FINALIZER_TESTS_INT", 7, 2) == 2,
            "value below the minimum should be raised to the minimum");
}

void test_external_recorder_clip_id()
{
    require(gui_external_recorder_clip_id(0) == "clip_000000",
            "clip 0 should format as clip_000000");
    require(gui_external_recorder_clip_id(7) == "clip_000007",
            "clip 7 should be zero padded to six digits");
    require(gui_external_recorder_clip_id(123456) == "clip_123456",
            "six digit clip indexes should pass through unpadded");
}

void test_external_stream_classification()
{
    orange::external_recorder::RecorderStreamPlan stream;
    stream.stream_id = "700001";
    stream.camera_serial = "700001";
    stream.output_kind = "full";
    require(gui_external_stream_is_full(stream),
            "output_kind full should classify as full");
    require(!gui_external_stream_is_crop(stream),
            "output_kind full should not classify as crop");

    stream.output_kind.clear();
    require(gui_external_stream_is_full(stream),
            "empty output_kind should classify as full");

    stream.output_kind = "crop";
    require(!gui_external_stream_is_full(stream),
            "output_kind crop should not classify as full");
    require(gui_external_stream_is_crop(stream),
            "output_kind crop should classify as crop");

    // Legacy plans without output_kind rely on the _crop suffix.
    orange::external_recorder::RecorderStreamPlan legacy;
    legacy.output_kind.clear();
    legacy.stream_id = "700001_crop";
    legacy.camera_serial = "700001_crop";
    require(gui_external_stream_is_crop(legacy),
            "_crop suffixed stream_id should classify as crop");
    legacy.stream_id = "700001";
    require(gui_external_stream_is_crop(legacy),
            "_crop suffixed camera_serial should classify as crop");
    legacy.camera_serial = "700001";
    require(!gui_external_stream_is_crop(legacy),
            "plain serial without suffix should not classify as crop");
}

void test_json_string_or()
{
    const nlohmann::json object = {
        {"present", "value"},
        {"empty", ""},
        {"number", 12}
    };
    require(gui_json_string_or(object, "present", "fallback") == "value",
            "present string key should return the stored value");
    require(gui_json_string_or(object, "empty", "fallback") == "fallback",
            "empty string value should fall back");
    require(gui_json_string_or(object, "number", "fallback") == "fallback",
            "non-string value should fall back");
    require(gui_json_string_or(object, "missing", "fallback") == "fallback",
            "missing key should fall back");
    require(gui_json_string_or(nlohmann::json::array(), "present", "fallback") ==
                "fallback",
            "non-object json should fall back");
}

void test_crop_rollover_json()
{
    orange::session::RecordingControlConfig rolling;
    rolling.record_for_seconds = 10;
    rolling.clip_seconds = 5;

    const nlohmann::json requested = gui_crop_rollover_json(rolling, "");
    require(requested.value("requested", false),
            "clip_seconds > 0 should mark rollover requested");
    require(requested.value("status", std::string()) == "completed",
            "empty status should default to completed");
    require(requested.value("implementation", std::string()) ==
                orange::external_recorder::kExternalRecorderRollingImplementation,
            "requested rollover should carry the external implementation");
    require(requested.value("boundary", std::string()) == "recording_frame_id",
            "crop rollover boundary should be recording_frame_id");
    require(requested.value("output_kind", std::string()) == "crop",
            "rollover json should be marked as crop output");
    require(requested.value("supported_mode", std::string()) == "rolling_clips",
            "requested rollover should advertise rolling_clips");
    require(requested.value("seamless_writer_switch", false) &&
                requested.value("records_during_rollover", false) &&
                requested.value("rolling_supported", false),
            "requested rollover flags should be set");
    require(requested.value("next_writer_preopened", true) == false,
            "next_writer_preopened should be false");

    const nlohmann::json custom = gui_crop_rollover_json(rolling, "incomplete");
    require(custom.value("status", std::string()) == "incomplete",
            "explicit status should be preserved");

    orange::session::RecordingControlConfig single;
    single.record_for_seconds = 10;
    single.clip_seconds = 0;
    const nlohmann::json not_requested = gui_crop_rollover_json(single, "ignored");
    require(!not_requested.value("requested", true),
            "clip_seconds == 0 should mark rollover not requested");
    require(not_requested.value("status", std::string()) == "not_requested",
            "not-requested rollover should report not_requested");
    require(not_requested.value("implementation", std::string()) == "none",
            "not-requested rollover should have implementation none");
    require(not_requested.value("supported_mode", std::string()) == "single_clip",
            "not-requested rollover should advertise single_clip");
}

void test_attach_crop_rolling_outputs()
{
    std::map<int, orange::session::RollingClipManifestOptions> clips_by_index;
    clips_by_index[0] = orange::session::RollingClipManifestOptions{};

    nlohmann::json clip = {
        {"clip_index", 0},
        {"clip_id", "clip_000000"},
        {"stream_id", "700001_crop"},
        {"status", "completed"},
        {"video", "/tmp/clip0/Cam700001_crop.mp4"},
        {"metadata", "/tmp/clip0/Cam700001_crop_meta.csv"},
        {"keyframes", "/tmp/clip0/Cam700001_crop_keyframe.json"},
        {"frame_count", 10},
        {"first_recording_frame_id", 1},
        {"last_recording_frame_id", 10},
        {"packet_count", 10},
        {"width", 320},
        {"height", 320},
        {"frame_rate", 100}
    };
    nlohmann::json backend;
    backend["crop_recording"]["recording_control"] = nlohmann::json::object();
    backend["crop_recording"]["rollover"] = nlohmann::json::object();
    backend["crop_recording"]["rolling_clips"]["700001"] =
        nlohmann::json::array({clip});

    std::string error;
    require(gui_attach_crop_rolling_outputs_to_clips(backend, &clips_by_index, &error),
            "matching clip_index should attach cleanly: " + error);
    require(error.empty(), "no error expected on clean attach");
    require(clips_by_index[0].recording_outputs.size() == 1,
            "one crop output should be attached to clip 0");
    const orange::session::RecordingOutputDescriptor& output =
        clips_by_index[0].recording_outputs.front();
    require(output.camera_serial == "700001", "crop output camera serial");
    require(output.output_kind == "crop", "crop output kind");
    require(output.role == "runtime_derived_acquisition_input", "crop output role");
    require(output.video_pixel_coordinate_space == "crop_frame_pixels",
            "crop video pixel coordinate space");
    require(output.source_geometry_coordinate_space == "full_frame_pixels",
            "crop source geometry coordinate space");
    require(output.backend == "external_ipc", "crop output backend");
    require(output.frame_count == 10, "crop output frame count");
    require(output.video_path == "/tmp/clip0/Cam700001_crop.mp4",
            "crop output video path");

    // A crop clip index without a matching full-frame clip must be reported.
    clip["clip_index"] = 7;
    backend["crop_recording"]["rolling_clips"]["700001"] =
        nlohmann::json::array({clip});
    std::string mismatch_error;
    require(!gui_attach_crop_rolling_outputs_to_clips(
                backend, &clips_by_index, &mismatch_error),
            "unmatched clip_index should fail attachment");
    require(mismatch_error.find("camera 700001") != std::string::npos,
            "mismatch error should name the camera");
}

void test_build_rolling_snapshot_update()
{
    nlohmann::json manifest = {
        {"mode", "rolling_clips"},
        {"status", "completed"},
        {"cameras", nlohmann::json::array({"700001", "700002"})},
        {"clips", nlohmann::json::array(
                      {nlohmann::json::object(),
                       nlohmann::json::object(),
                       nlohmann::json::object()})},
        {"indexes", {{"row_count", 42}}},
        {"recording_backend", {{"mode", "external_ipc"}}}
    };
    orange::session::RecordingSessionIndexArtifacts artifacts;
    artifacts.clip_index_json_path = "/data/session/clip_index.json";
    artifacts.clip_index_csv_path = "/data/session/clip_index.csv";
    const nlohmann::json frame_rate = {{"avg_fps", 60.0}};

    const nlohmann::json update =
        gui_build_rolling_recording_session_snapshot_update(
            "/data/session", manifest, artifacts, frame_rate);

    require(update.value("recording_mode", std::string()) == "rolling_clips",
            "recording_mode should come from the manifest");
    require(update.value("recording_session_manifest_path", std::string()) ==
                (std::filesystem::path("/data/session") /
                 "recording_session.json").string(),
            "manifest path should live under the recording folder");
    require(update.value("recording_session_status", std::string()) == "completed",
            "status should propagate");
    require(update.value("recording_session_camera_count", 0) == 2,
            "camera count should match the manifest cameras array");
    require(update.value("rolling_clip_count", 0) == 3,
            "clip count should match the manifest clips array");
    require(update.value("rolling_index_row_count", 0) == 42,
            "row count should come from the manifest indexes");
    require(update["recording_session_index"].value("clip_index_json_path",
                                                    std::string()) ==
                artifacts.clip_index_json_path,
            "index json path should be overridden by the artifacts");
    require(update["recording_session_index"].value("clip_index_csv_path",
                                                    std::string()) ==
                artifacts.clip_index_csv_path,
            "index csv path should be overridden by the artifacts");
    require(update["gui_display_frame_rate"] == frame_rate,
            "display frame rate should propagate untouched");
    require(update["recording_backend"] == manifest["recording_backend"],
            "recording_backend should propagate when present");

    manifest.erase("recording_backend");
    manifest.erase("status");
    const nlohmann::json defaulted =
        gui_build_rolling_recording_session_snapshot_update(
            "/data/session", manifest, artifacts, frame_rate);
    require(!defaulted.contains("recording_backend"),
            "recording_backend should be omitted when the manifest lacks it");
    require(defaulted.value("recording_session_status", std::string()) ==
                "incomplete",
            "missing status should default to incomplete");
}

GuiRecordingRunState make_finalizing_run(const std::string& folder)
{
    GuiRecordingRunState run;
    run.active = true;
    run.finalizing = true;
    run.recording_folder = folder;
    const auto now = std::chrono::steady_clock::now();
    run.recording_started_at = now - std::chrono::seconds(20);
    run.recording_started_at_utc = "2026-07-05T00:00:00Z";
    run.recording_stop_requested_at = now - std::chrono::seconds(10);
    run.recording_stop_requested_at_utc = "2026-07-05T00:00:10Z";
    return run;
}

void test_write_external_rolling_manifest_requires_clip_seconds()
{
    ScopedTempDir tmp;
    const std::filesystem::path folder = tmp.path() / "never_created";

    orange::external_recorder::SupervisorPlan plan;
    orange::external_recorder::RecorderStreamPlan stream;
    stream.stream_id = "700001";
    stream.camera_serial = "700001";
    stream.output_kind = "full";
    stream.record_for_seconds = 10;
    stream.clip_seconds = 0;
    plan.streams.push_back(stream);

    GuiRecordingRunState run = make_finalizing_run(folder.string());
    nlohmann::json manifest;
    nlohmann::json bridge;
    std::string error;
    require(!gui_write_external_rolling_recording_session_manifest(
                run,
                plan,
                nlohmann::json::object(),
                {},
                true,
                nlohmann::json::object(),
                &manifest,
                &bridge,
                &error),
            "plan without clip_seconds should be rejected");
    require(error.find("without clip_seconds") != std::string::npos,
            "error should explain the missing clip_seconds");
    require(!std::filesystem::exists(folder),
            "rejected manifest write should not create the recording folder");
}

void test_write_external_rolling_manifest_success()
{
    ScopedTempDir tmp;
    const std::filesystem::path session = tmp.path() / "session_0001";
    const std::filesystem::path clip0 = session / "clip_000000";
    const std::filesystem::path clip1 = session / "clip_000001";
    std::filesystem::create_directories(clip0);
    std::filesystem::create_directories(clip1);

    // The snapshot updater requires an existing recording_snapshot.json.
    write_text_file(session / "recording_snapshot.json", "{\"session\":{}}");

    auto make_clip = [](const std::filesystem::path& dir,
                        const int clip_index,
                        const uint64_t first_frame,
                        const uint64_t last_frame) {
        const std::string mp4 = (dir / "Cam700001.mp4").string();
        const std::string metadata = (dir / "Cam700001_meta.csv").string();
        const std::string keyframes = (dir / "Cam700001_keyframe.json").string();
        write_text_file(mp4, "mp4");
        std::ostringstream metadata_csv;
        metadata_csv
            << "frame_id,timestamp,timestamp_sys,recording_frame_id,local_frame_id,"
               "gop_index,frame_index_within_gop,source_gpu_id,assigned_gpu_id,"
               "assigned_shard_id,bytes\n";
        for (uint64_t frame_id = first_frame; frame_id <= last_frame; ++frame_id) {
            metadata_csv
                << frame_id << ','
                << (1000000 + frame_id) << ','
                << (2000000 + frame_id) << ','
                << frame_id << ','
                << frame_id << ",0,0,5,6,0,256\n";
        }
        write_text_file(metadata, metadata_csv.str());
        write_text_file(keyframes, "{}");
        const uint64_t frame_count = last_frame - first_frame + 1;
        return nlohmann::json{
            {"clip_index", clip_index},
            {"directory", dir.string()},
            {"mp4", mp4},
            {"metadata", metadata},
            {"keyframes", keyframes},
            {"failed", false},
            {"frame_count", frame_count},
            {"first_recording_frame_id", first_frame},
            {"last_recording_frame_id", last_frame},
            {"packets_written", frame_count}
        };
    };

    nlohmann::json summary = {
        {"fps", 100},
        {"rolling_output",
         {{"enabled", true},
          {"clips", nlohmann::json::array({make_clip(clip0, 0, 1, 500),
                                           make_clip(clip1, 1, 501, 900)})}}}
    };
    const std::filesystem::path summary_path = tmp.path() / "summary_700001.json";
    write_text_file(summary_path, summary.dump(2));

    orange::external_recorder::SupervisorPlan plan;
    orange::external_recorder::RecorderStreamPlan stream;
    stream.stream_id = "700001";
    stream.camera_serial = "700001";
    stream.output_kind = "full";
    stream.record_for_seconds = 10;
    stream.clip_seconds = 5;
    stream.encode_fps = 100;
    stream.codec = "hevc";
    stream.tuning = "lossless";
    stream.summary_json = summary_path.string();
    plan.streams.push_back(stream);

    GuiRecordingRunState run = make_finalizing_run(session.string());
    run.recording_drained_at = std::chrono::steady_clock::now();
    run.recording_drained_at_utc = "2026-07-05T00:00:11Z";

    const nlohmann::json recording_backend = {
        {"mode", "external_ipc"},
        {"status", "completed"}
    };
    nlohmann::json manifest;
    nlohmann::json bridge;
    std::string error;
    require(gui_write_external_rolling_recording_session_manifest(
                run,
                plan,
                recording_backend,
                {},
                true,
                nlohmann::json{{"avg_fps", 60.0}},
                &manifest,
                &bridge,
                &error),
            "rolling manifest write should succeed: " + error);

    require(bridge.value("pass", false), "bridge should report pass");
    require(bridge.value("mode", std::string()) == "rolling_clips",
            "bridge mode should be rolling_clips");
    require(bridge.value("clip_count", 0) == 2, "bridge should count both clips");
    require(bridge.value("camera_count", 0) == 1, "bridge should count one camera");

    const std::filesystem::path manifest_path = session / "recording_session.json";
    require(std::filesystem::exists(manifest_path),
            "recording_session.json should be written");
    nlohmann::json manifest_on_disk;
    {
        std::ifstream input(manifest_path);
        input >> manifest_on_disk;
    }
    require(manifest_on_disk.is_object(), "manifest on disk should parse");
    require(manifest_on_disk.value("status", std::string()) == "completed",
            "manifest status should be completed");
    require(manifest_on_disk.value("clips", nlohmann::json::array()).size() == 2,
            "manifest should describe both clips");
    require(
        manifest_on_disk["clips"][0]["recording_outputs"]["700001"]["full"]
                .value("backend", std::string()) == "external_ipc",
        "rolling clip full output should preserve the external_ipc backend");
    require(
        manifest_on_disk["clips"][0]["recording_outputs"]["700001"]["full"]
                .contains("encoding_budget"),
        "rolling clip full output should carry an encoding budget contract");
    require(manifest == manifest_on_disk,
            "manifest_out should match the manifest on disk");

    require(std::filesystem::exists(clip0 / "clip_manifest.json"),
            "clip 0 manifest should be written");
    require(std::filesystem::exists(clip1 / "clip_manifest.json"),
            "clip 1 manifest should be written");

    const std::string index_json =
        bridge["indexes"].value("clip_index_json", std::string());
    require(!index_json.empty() && std::filesystem::exists(index_json),
            "clip index json should be written");

    nlohmann::json snapshot;
    {
        std::ifstream input(session / "recording_snapshot.json");
        input >> snapshot;
    }
    require(snapshot["session"].value("recording_session_status", std::string()) ==
                "completed",
            "snapshot should record the completed session status");
    require(snapshot["session"].value("rolling_clip_count", 0) == 2,
            "snapshot should record the rolling clip count");
}

void test_finalizer_gating()
{
    // Keep the diagnostic finalize stall knobs out of the picture.
    ScopedEnv gui_stall("ORANGE_GUI_LOCAL_CONTROL_DIAGNOSTIC_FINALIZE_STALL_SECONDS");
    ScopedEnv stall("ORANGE_LOCAL_CONTROL_DIAGNOSTIC_FINALIZE_STALL_SECONDS");

    ScopedTempDir tmp;
    const std::filesystem::path folder = tmp.path() / "never_created";
    const nlohmann::json frame_rate = nlohmann::json::object();

    auto folder_untouched = [&]() {
        return !std::filesystem::exists(folder);
    };

    require(!gui_finalize_recording_session_if_ready(
                nullptr, nullptr, nullptr, nullptr, nullptr, 0, 320, frame_rate),
            "null run should not finalize");

    {
        GuiRecordingRunState run = make_finalizing_run(folder.string());
        run.active = false;
        require(!gui_finalize_recording_session_if_ready(
                    &run, nullptr, nullptr, nullptr, nullptr, 0, 320, frame_rate),
                "inactive run should not finalize");
        require(!run.finalized, "inactive run should stay unfinalized");
    }

    {
        GuiRecordingRunState run = make_finalizing_run(folder.string());
        run.finalizing = false;
        require(!gui_finalize_recording_session_if_ready(
                    &run, nullptr, nullptr, nullptr, nullptr, 0, 320, frame_rate),
                "run that is not finalizing should not finalize");
    }

    {
        GuiRecordingRunState run = make_finalizing_run("");
        require(!gui_finalize_recording_session_if_ready(
                    &run, nullptr, nullptr, nullptr, nullptr, 0, 320, frame_rate),
                "empty recording folder should not finalize");
    }

    {
        // external_ipc with the recorder still recording: bail out before
        // touching drain flags or the filesystem.
        GuiRecordingRunState run = make_finalizing_run(folder.string());
        run.recording_sink_mode = "external_ipc";
        CameraControl control;
        control.record_video = true;
        require(!gui_finalize_recording_session_if_ready(
                    &run, nullptr, &control, nullptr, nullptr, 0, 320, frame_rate),
                "external_ipc with record_video should not finalize");
        require(!control.recording_draining && !control.stop_record,
                "record_video gate should not re-assert drain flags");
        require(run.active && !run.finalized,
                "record_video gate should leave the run pending");
    }

    {
        // external_ipc with recorders still active: current behavior re-asserts
        // the drain flags and returns false. Lock that contract in.
        GuiRecordingRunState run = make_finalizing_run(folder.string());
        run.recording_sink_mode = "external_ipc";
        CameraControl control;
        control.active_recorders.store(2);
        require(!gui_finalize_recording_session_if_ready(
                    &run, nullptr, &control, nullptr, nullptr, 0, 320, frame_rate),
                "external_ipc with active recorders should not finalize");
        require(control.recording_draining && control.stop_record,
                "active recorders should re-assert recording_draining/stop_record");
        require(run.active && !run.finalized,
                "active recorders should leave the run pending");
    }

    {
        // Non-external ("real") sink mode: any recorder activity defers.
        GuiRecordingRunState run = make_finalizing_run(folder.string());
        CameraControl control;
        control.record_video = true;
        require(!gui_finalize_recording_session_if_ready(
                    &run, nullptr, &control, nullptr, nullptr, 0, 320, frame_rate),
                "real mode with record_video should not finalize");
    }
    {
        GuiRecordingRunState run = make_finalizing_run(folder.string());
        CameraControl control;
        control.recording_draining = true;
        require(!gui_finalize_recording_session_if_ready(
                    &run, nullptr, &control, nullptr, nullptr, 0, 320, frame_rate),
                "real mode while draining should not finalize");
        require(!control.stop_record,
                "real mode gate should not re-assert stop_record");
    }
    {
        GuiRecordingRunState run = make_finalizing_run(folder.string());
        CameraControl control;
        control.active_recorders.store(1);
        require(!gui_finalize_recording_session_if_ready(
                    &run, nullptr, &control, nullptr, nullptr, 0, 320, frame_rate),
                "real mode with active recorders should not finalize");
    }

    require(folder_untouched(),
            "no gating path should create the recording folder");

    // Note: the external_ipc "pipelines undrained" gate needs a
    // ModernRecordingPipeline mid-drain; constructing one standalone drags in
    // the live encoder stack, so that branch is exercised only via the
    // equivalent active_recorders re-assert contract above.
}

// --- Phased finalize (prepare / run / complete + async drivers) --------------

struct PhasedFinalizeFixture {
    CameraParams camera{};
    CameraEachSelect select{};
    CameraControl camera_control;
    orange::session::RecordingSessionState recording_session;
    GuiRecordingRunState run;

    explicit PhasedFinalizeFixture(const std::string& folder)
    {
        camera.camera_serial = "990001";
        camera.camera_id = 0;
        camera.frame_rate = 100;
        select.record = true;
        select.crop_and_encode = false;
        recording_session.recording_sink_mode = "real";
        run = make_finalizing_run(folder);
        run.recording_sink_mode = "real";
        // The claim CameraControl holds for the duration of the run; ONLY
        // the completion phase may release it.
        camera_control.recording_folder = folder;
        camera_control.preserve_recording_session_state = true;
    }
};

void test_phased_finalize_failure_leaves_retryable_state()
{
    // Keep the diagnostic finalize stall knobs out of the picture.
    ScopedEnv gui_stall("ORANGE_GUI_LOCAL_CONTROL_DIAGNOSTIC_FINALIZE_STALL_SECONDS");
    ScopedEnv stall("ORANGE_LOCAL_CONTROL_DIAGNOSTIC_FINALIZE_STALL_SECONDS");

    ScopedTempDir tmp;
    const std::filesystem::path session = tmp.path() / "session_fail";
    std::filesystem::create_directories(session);
    // No recording_snapshot.json: the run phase writes the session manifest
    // and then fails at the snapshot update, exercising the failure path.

    PhasedFinalizeFixture fixture(session.string());
    require(gui_recording_finalize_gate_ready(
                &fixture.run, &fixture.recording_session, &fixture.camera_control),
            "a drained real-mode run must pass the gate");

    GuiRecordingFinalizeInputs inputs = gui_prepare_recording_finalize(
        &fixture.run,
        &fixture.recording_session,
        &fixture.camera,
        &fixture.select,
        1,
        320,
        nlohmann::json::object());
    require(inputs.valid, "prepare must produce valid inputs");
    require(!inputs.external_ipc, "real sink mode must not mark external_ipc");
    require(inputs.cameras.size() == 1 && inputs.cameras[0].record,
            "prepare must snapshot the camera inputs");
    require(!fixture.run.recording_drained_at_utc.empty(),
            "prepare must stamp the drain timestamp on the live run");

    GuiRecordingFinalizeProgress progress;
    GuiRecordingFinalizeOutcome outcome =
        gui_run_recording_finalize(&inputs, &progress);
    require(!outcome.ok, "missing recording_snapshot.json must fail the run phase");
    require(outcome.error_message.find("recording_snapshot.json") != std::string::npos,
            "the failure must name the snapshot update");
    require(progress.stage.load() ==
                static_cast<int>(GuiRecordingFinalizeStage::kUpdatingSnapshot),
            "the failure must happen in the updating-snapshot stage");

    require(!gui_complete_recording_finalize(
                &inputs,
                std::move(outcome),
                &fixture.run,
                &fixture.recording_session,
                &fixture.camera_control),
            "completion of a failed outcome must report false");
    // Retry property: finalizing stays latched, the folder stays claimed,
    // the loud error is installed for the GUI status line.
    require(fixture.run.active && fixture.run.finalizing && !fixture.run.finalized,
            "a failed finalize must leave the run latched finalizing");
    require(fixture.camera_control.recording_folder == session.string(),
            "a failed finalize must leave the folder claimed");
    require(fixture.camera_control.preserve_recording_session_state,
            "a failed finalize must not clear the preserve flag");
    require(!fixture.recording_session.external_recorder_last_error.empty(),
            "a failed finalize must install a loud error");

    // Make the snapshot update succeed and retry the WHOLE finalize through
    // the synchronous composition (exactly what the per-frame gate does).
    write_text_file(session / "recording_snapshot.json", "{\"session\":{}}");
    require(gui_finalize_recording_session_if_ready(
                &fixture.run,
                &fixture.recording_session,
                &fixture.camera_control,
                &fixture.camera,
                &fixture.select,
                1,
                320,
                nlohmann::json::object()),
            "the retried finalize must succeed");
    require(!fixture.run.active && !fixture.run.finalizing && fixture.run.finalized,
            "the retried finalize must flip the terminal run state");
    require(fixture.camera_control.recording_folder.empty(),
            "the successful finalize must release the folder claim");
    require(!fixture.camera_control.preserve_recording_session_state,
            "the successful finalize must clear the preserve flag");
    require(std::filesystem::exists(session / "recording_session.json"),
            "the finalize must leave a session manifest on disk");
    nlohmann::json manifest;
    {
        std::ifstream input(session / "recording_session.json");
        input >> manifest;
    }
    require(manifest.value("status", std::string()) == "completed",
            "real-mode manifest should complete");

    // Terminal state flips exactly once: a further poll must be a no-op.
    require(!gui_finalize_recording_session_if_ready(
                &fixture.run,
                &fixture.recording_session,
                &fixture.camera_control,
                &fixture.camera,
                &fixture.select,
                1,
                320,
                nlohmann::json::object()),
            "a finalized run must not finalize again");
    std::cout << "PASS test_phased_finalize_failure_leaves_retryable_state"
              << std::endl;
}

void test_async_finalize_poll_launches_and_completes()
{
    ScopedEnv gui_stall("ORANGE_GUI_LOCAL_CONTROL_DIAGNOSTIC_FINALIZE_STALL_SECONDS");
    ScopedEnv stall("ORANGE_LOCAL_CONTROL_DIAGNOSTIC_FINALIZE_STALL_SECONDS");

    ScopedTempDir tmp;
    const std::filesystem::path session = tmp.path() / "session_async";
    std::filesystem::create_directories(session);
    write_text_file(session / "recording_snapshot.json", "{\"session\":{}}");

    PhasedFinalizeFixture fixture(session.string());
    GuiAsyncRecordingFinalizeState async_finalize;
    const nlohmann::json frame_rate = nlohmann::json::object();
    int frame_rate_builder_calls = 0;
    const auto frame_rate_builder = [&]() {
        ++frame_rate_builder_calls;
        return frame_rate;
    };

    // Ordinary recording frames poll this function continuously. The
    // telemetry builder must remain completely lazy until the drain gate
    // opens; otherwise its cost grows with the recording history.
    fixture.run.finalizing = false;
    require(!gui_poll_async_recording_finalize(
                &async_finalize,
                &fixture.run,
                &fixture.recording_session,
                &fixture.camera_control,
                &fixture.camera,
                &fixture.select,
                1,
                320,
                frame_rate_builder),
            "an active recording must not launch finalization");
    require(frame_rate_builder_calls == 0,
            "an active recording must not invoke the telemetry snapshot builder");
    fixture.run.finalizing = true;

    // Frame 1: the gate opens and the poll launches the background phase.
    require(!gui_poll_async_recording_finalize(
                &async_finalize,
                &fixture.run,
                &fixture.recording_session,
                &fixture.camera_control,
                &fixture.camera,
                &fixture.select,
                1,
                320,
                frame_rate_builder),
            "the launching poll must not report completion");
    require(frame_rate_builder_calls == 1,
            "the snapshot builder must run exactly once when finalization launches");
    require(async_finalize.active, "the poll must launch the background phase");
    require(fixture.run.finalizing && !fixture.run.finalized,
            "the run stays finalizing while the background phase runs");
    const GuiRecordingFinalizeProgressView view =
        gui_async_recording_finalize_progress_view(async_finalize);
    require(view.pending, "the progress view must report pending");

    // Later frames: poll until the worker publishes done and the completion
    // phase flips the terminal state.
    bool finalized = false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!finalized && std::chrono::steady_clock::now() < deadline) {
        finalized = gui_poll_async_recording_finalize(
            &async_finalize,
            &fixture.run,
            &fixture.recording_session,
            &fixture.camera_control,
            &fixture.camera,
            &fixture.select,
            1,
            320,
            frame_rate_builder);
        if (!finalized) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    require(finalized, "the poll must complete the background finalize");
    require(!async_finalize.active && !async_finalize.worker.joinable(),
            "completion must join and clear the worker");
    require(frame_rate_builder_calls == 1,
            "polling an active finalize must not rebuild the telemetry snapshot");
    require(!fixture.run.active && !fixture.run.finalizing && fixture.run.finalized,
            "completion must flip the terminal run state exactly once");
    require(fixture.camera_control.recording_folder.empty(),
            "completion must release the folder claim");
    require(std::filesystem::exists(session / "recording_session.json"),
            "the async finalize must write the session manifest");

    // The next poll observes the finalized run and never relaunches.
    require(!gui_poll_async_recording_finalize(
                &async_finalize,
                &fixture.run,
                &fixture.recording_session,
                &fixture.camera_control,
                &fixture.camera,
                &fixture.select,
                1,
                320,
                frame_rate_builder),
            "a finalized run must not relaunch the background phase");
    require(frame_rate_builder_calls == 1,
            "a finalized run must not invoke the telemetry snapshot builder");
    require(!async_finalize.active, "no background phase after the terminal state");
    std::cout << "PASS test_async_finalize_poll_launches_and_completes" << std::endl;
}

void test_async_finalize_join_completes_in_flight_work()
{
    ScopedEnv gui_stall("ORANGE_GUI_LOCAL_CONTROL_DIAGNOSTIC_FINALIZE_STALL_SECONDS");
    ScopedEnv stall("ORANGE_LOCAL_CONTROL_DIAGNOSTIC_FINALIZE_STALL_SECONDS");

    ScopedTempDir tmp;
    const std::filesystem::path session = tmp.path() / "session_join";
    std::filesystem::create_directories(session);
    write_text_file(session / "recording_snapshot.json", "{\"session\":{}}");

    PhasedFinalizeFixture fixture(session.string());
    GuiAsyncRecordingFinalizeState async_finalize;

    // Join with nothing in flight: no-op, no completion.
    require(!gui_join_async_recording_finalize(
                &async_finalize,
                &fixture.run,
                &fixture.recording_session,
                &fixture.camera_control),
            "joining an idle finalize state must be a no-op");
    require(fixture.run.finalizing, "the idle join must not touch the run");

    // Launch, then join (the teardown/window-close path): the join must
    // block until the background phase finishes and complete it.
    require(!gui_poll_async_recording_finalize(
                &async_finalize,
                &fixture.run,
                &fixture.recording_session,
                &fixture.camera_control,
                &fixture.camera,
                &fixture.select,
                1,
                320,
                []() { return nlohmann::json::object(); }),
            "the launching poll must not report completion");
    require(async_finalize.active, "the poll must launch the background phase");
    require(gui_join_async_recording_finalize(
                &async_finalize,
                &fixture.run,
                &fixture.recording_session,
                &fixture.camera_control),
            "the join must complete the in-flight finalize");
    require(!fixture.run.active && !fixture.run.finalizing && fixture.run.finalized,
            "the joined finalize must flip the terminal run state");
    require(fixture.camera_control.recording_folder.empty(),
            "the joined finalize must release the folder claim");
    std::cout << "PASS test_async_finalize_join_completes_in_flight_work"
              << std::endl;
}

}  // namespace

int main()
{
    struct NamedTest {
        const char* name;
        void (*fn)();
    };

    const NamedTest tests[] = {
        {"gui_env_int", &test_gui_env_int},
        {"external_recorder_clip_id", &test_external_recorder_clip_id},
        {"external_stream_classification", &test_external_stream_classification},
        {"json_string_or", &test_json_string_or},
        {"crop_rollover_json", &test_crop_rollover_json},
        {"attach_crop_rolling_outputs", &test_attach_crop_rolling_outputs},
        {"build_rolling_snapshot_update", &test_build_rolling_snapshot_update},
        {"write_external_rolling_manifest_requires_clip_seconds",
         &test_write_external_rolling_manifest_requires_clip_seconds},
        {"write_external_rolling_manifest_success",
         &test_write_external_rolling_manifest_success},
        {"finalizer_gating", &test_finalizer_gating},
        {"phased_finalize_failure_leaves_retryable_state",
         &test_phased_finalize_failure_leaves_retryable_state},
        {"async_finalize_poll_launches_and_completes",
         &test_async_finalize_poll_launches_and_completes},
        {"async_finalize_join_completes_in_flight_work",
         &test_async_finalize_join_completes_in_flight_work},
    };

    for (const auto& test : tests) {
        try {
            test.fn();
            std::cout << "[PASS] " << test.name << "\n";
        } catch (const std::exception& e) {
            std::cerr << "[FAIL] " << test.name << ": " << e.what() << "\n";
            return EXIT_FAILURE;
        }
    }

    std::cout << "All recording finalizer tests passed.\n";
    return EXIT_SUCCESS;
}
