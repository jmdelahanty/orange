#include "session/recording_session.h"
#include "NvEncoder/Logger.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

simplelogger::Logger* logger =
    simplelogger::LoggerFactory::CreateConsoleLogger();

namespace {

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

orange::session::RecordingSessionCameraArtifact make_camera_artifact(
    const std::string& serial,
    const int frame_count)
{
    orange::session::RecordingSessionCameraArtifact artifact;
    artifact.camera_serial = serial;
    artifact.video_path = "Cam" + serial + ".mp4";
    artifact.metadata_path = "Cam" + serial + "_meta.csv";
    artifact.keyframe_path = "Cam" + serial + "_keyframe.json";
    artifact.frame_count = frame_count;
    artifact.first_recording_frame_id = 1;
    artifact.last_recording_frame_id = frame_count;
    artifact.recording_frame_id_gaps = 0;
    artifact.packet_count = frame_count;
    artifact.packet_count_source = "external_recorder_summary.packets_written";
    return artifact;
}

orange::session::RecordingOutputDescriptor make_external_crop_output(
    const std::string& serial,
    const std::string& scope,
    const int frame_count)
{
    orange::session::RecordingOutputDescriptor output;
    output.camera_serial = serial;
    output.output_kind = "crop";
    output.role = "sidecar";
    output.backend = "external_ipc";
    output.status = "completed";
    output.video_path = "external_crop/Cam" + serial + "_crop_external.mp4";
    output.metadata_path = "Cam" + serial + "_crop_meta.csv";
    output.keyframe_path = "external_crop/Cam" + serial + "_crop_external_keyframe.json";
    output.perf_path = "Cam" + serial + "_crop_perf.csv";
    output.summary_path = "external_crop/Cam" + serial + "_crop_external_summary.json";
    output.frame_count = frame_count;
    output.first_recording_frame_id = 1;
    output.last_recording_frame_id = frame_count;
    output.recording_frame_id_gaps = 0;
    output.packet_count = frame_count;
    output.packet_count_source = "external_crop_recorder_summary.packets_written";
    output.width = 256;
    output.height = 256;
    output.frame_rate = 100;
    output.codec = "hevc";
    output.container = "mp4";
    output.tuning = "lossless";
    output.pixel_source_format = "mono8";
    output.encoded_format = "nv12";
    output.coordinate_space = "full_frame_pixels";
    output.details = {
        {"scope", scope},
        {"stream_id", serial + "_crop"},
        {"video_backend", "external_ipc"},
        {"metadata_backend", "orange_gui_split_crop_csv"}
    };
    return output;
}

nlohmann::json read_json(const std::filesystem::path& path)
{
    std::ifstream input(path);
    require(static_cast<bool>(input), "failed to open JSON fixture " + path.string());
    nlohmann::json value;
    input >> value;
    return value;
}

void test_single_clip_manifest_preserves_full_and_crop_outputs()
{
    orange::session::SingleClipRecordingSessionManifestOptions options;
    options.producer = "orange_gui_external_ipc";
    options.session_id = "session_single";
    options.recording_folder = "/tmp/orange_session_manifest_single";
    options.status = "completed";
    options.recording_backend = {{"mode", "external_ipc"}, {"status", "completed"}};
    options.recording_stop_control = {
        {"source", "orange_gui_local_control"},
        {"method", "stop_recording"},
        {"request_id", "req-stop-1"},
        {"operation_id", "op-stop-1"},
        {"ack_state", "executed"},
        {"received_at_utc", "2026-05-29T00:00:00Z"},
        {"event_log", {
            {"source_path", "/tmp/orange_local_control.sock.events.jsonl"},
            {"copied_path", "/tmp/orange_session_manifest_single/orange_local_control.events.jsonl"},
            {"relative_path", "orange_local_control.events.jsonl"},
            {"copied", true},
            {"copied_at_utc", "2026-05-29T00:00:01Z"},
            {"bytes", 512}
        }}
    };
    options.cameras.push_back(make_camera_artifact("2010096", 300));
    options.recording_outputs.push_back(
        make_external_crop_output("2010096", "single_clip", 300));

    const nlohmann::json manifest =
        orange::session::build_single_clip_recording_session_manifest(options);
    const nlohmann::json outputs =
        manifest.value("recording_outputs", nlohmann::json::object());
    const nlohmann::json camera_outputs =
        outputs.value("2010096", nlohmann::json::object());

    require(camera_outputs.contains("full"),
            "single-clip manifest should keep generated full output descriptor");
    require(camera_outputs.contains("crop"),
            "single-clip manifest should include explicit crop output descriptor");
    require(camera_outputs["full"].value("backend", std::string()) == "external_ipc",
            "single-clip full output should inherit external backend");
    require(camera_outputs["crop"].value("backend", std::string()) == "external_ipc",
            "single-clip crop output should keep external backend");

    const nlohmann::json clip_outputs =
        manifest["clips"][0].value("recording_outputs", nlohmann::json::object());
    require(clip_outputs.value("2010096", nlohmann::json::object()).contains("full"),
            "single-clip compatibility clip should include full output");
    require(clip_outputs.value("2010096", nlohmann::json::object()).contains("crop"),
            "single-clip compatibility clip should include crop output");
    require(
        manifest["recording"]["control"].value("request_id", std::string()) == "req-stop-1",
        "single-clip manifest should preserve local-control stop request id");
    require(
        manifest["recording"]["control"].value("operation_id", std::string()) == "op-stop-1",
        "single-clip manifest should preserve local-control stop operation id");
    require(
        manifest["recording"]["control"].value("ack_state", std::string()) == "executed",
        "single-clip manifest should preserve local-control stop ACK state");
    require(
        manifest["recording"]["control"]["event_log"].value("relative_path", std::string()) ==
            "orange_local_control.events.jsonl",
        "single-clip manifest should preserve local-control event-log relative path");
    require(
        manifest["recording"]["control"]["event_log"].value("bytes", 0) == 512,
        "single-clip manifest should preserve local-control event-log byte count");
}

void test_rolling_manifest_emits_session_aggregate_and_clip_crop_outputs()
{
    orange::session::RollingRecordingSessionManifestOptions options;
    options.producer = "orange_gui_external_ipc";
    options.session_id = "session_rolling";
    options.recording_folder = "/tmp/orange_session_manifest_rolling";
    options.status = "completed";
    options.recording_control.record_for_seconds = 6;
    options.recording_control.clip_seconds = 2;
    options.recording_backend = {{"mode", "external_ipc"}, {"status", "completed"}};
    options.recording_stop_control = {
        {"source", "orange_gui_local_control"},
        {"method", "citrus_completion"},
        {"request_id", "req-completion-1"},
        {"operation_id", "op-completion-1"},
        {"terminal_state", "completed"},
        {"ack_state", "executed"},
        {"received_at_utc", "2026-05-29T00:01:00Z"},
        {"event_log", {
            {"source_path", "/tmp/orange_local_control.sock.events.jsonl"},
            {"copied_path", "/tmp/orange_session_manifest_rolling/orange_local_control.events.jsonl"},
            {"relative_path", "orange_local_control.events.jsonl"},
            {"copied", true},
            {"copied_at_utc", "2026-05-29T00:01:01Z"},
            {"bytes", 1024}
        }}
    };
    options.camera_serials.push_back("2010096");
    options.recording_outputs.push_back(
        make_external_crop_output("2010096", "session_aggregate", 601));

    orange::session::RollingClipManifestOptions clip;
    clip.producer = "orange_gui_external_ipc";
    clip.output_backend = "external_ipc";
    clip.session_id = "session_rolling";
    clip.clip_index = 0;
    clip.clip_id = "clip_000000";
    clip.recording_folder = options.recording_folder;
    clip.directory = "clips/clip_000000";
    clip.status = "completed";
    clip.first_recording_frame_id = 1;
    clip.last_recording_frame_id = 200;
    clip.cameras.push_back(make_camera_artifact("2010096", 200));
    clip.recording_outputs.push_back(
        make_external_crop_output("2010096", "clip", 200));
    options.clips.push_back(clip);

    const nlohmann::json manifest =
        orange::session::build_rolling_clip_recording_session_manifest(options);
    const nlohmann::json top_outputs =
        manifest.value("recording_outputs", nlohmann::json::object());
    const nlohmann::json top_camera_outputs =
        top_outputs.value("2010096", nlohmann::json::object());
    const nlohmann::json top_crop =
        top_camera_outputs.value("crop", nlohmann::json::object());

    require(top_camera_outputs.contains("crop"),
            "rolling manifest should emit top-level session crop output");
    require(top_crop["details"].value("scope", std::string()) == "session_aggregate",
            "rolling top-level crop output should be session aggregate");
    require(!top_camera_outputs.contains("full"),
            "rolling manifest should not invent a session-wide full output descriptor");

    const nlohmann::json clip_outputs =
        manifest["clips"][0].value("recording_outputs", nlohmann::json::object());
    const nlohmann::json clip_camera_outputs =
        clip_outputs.value("2010096", nlohmann::json::object());
    require(clip_camera_outputs.contains("full"),
            "rolling clip should include clip-scoped full output");
    require(clip_camera_outputs["full"].value("backend", std::string()) == "external_ipc",
            "external rolling clip full output should preserve external_ipc backend");
    require(clip_camera_outputs.contains("crop"),
            "rolling clip should include clip-scoped crop output");
    require(clip_camera_outputs["crop"]["details"].value("scope", std::string()) == "clip",
            "rolling clip crop output should remain clip-scoped");
    require(
        manifest["recording"]["control"].value("method", std::string()) == "citrus_completion",
        "rolling manifest should preserve local-control stop method");
    require(
        manifest["recording"]["control"].value("operation_id", std::string()) == "op-completion-1",
        "rolling manifest should preserve local-control stop operation id");
    require(
        manifest["recording"]["control"]["event_log"].value("relative_path", std::string()) ==
            "orange_local_control.events.jsonl",
        "rolling manifest should preserve local-control event-log relative path");
    require(
        manifest["recording"]["control"]["event_log"].value("bytes", 0) == 1024,
        "rolling manifest should preserve local-control event-log byte count");
}

void test_camera_preferred_recording_sink_resolution()
{
    unsetenv("ORANGE_GUI_RECORDING_SINK_MODE");
    unsetenv("ORANGE_GUI_DIAGNOSTIC_NO_FULL_FRAME");

    AppStorageConfig app_config;
    app_config.gui_recording_sink_mode = "real";
    app_config.gui_recording_sink_mode_configured = false;

    CameraParams cameras[2]{};
    cameras[0].camera_serial = "2012632";
    cameras[0].recording.preferred_sink_mode = "external_ipc";
    cameras[1].camera_serial = "2010096";
    cameras[1].recording.preferred_sink_mode.clear();

    CameraEachSelect selections[2]{};
    selections[0].record = true;
    selections[1].record = true;

    require(
        orange::session::resolve_gui_recording_sink_mode(
            &app_config, cameras, selections, 2) == "external_ipc",
        "camera preferred external_ipc should resolve when app sink is not configured");

    app_config.gui_recording_sink_mode_configured = true;
    app_config.gui_recording_sink_mode = "real";
    require(
        orange::session::resolve_gui_recording_sink_mode(
            &app_config, cameras, selections, 2) == "real",
        "explicit app sink mode should override camera preference");

    app_config.gui_recording_sink_mode_configured = false;
    setenv("ORANGE_GUI_RECORDING_SINK_MODE", "real", 1);
    require(
        orange::session::resolve_gui_recording_sink_mode(
            &app_config, cameras, selections, 2) == "real",
        "environment sink mode should override camera preference");
    unsetenv("ORANGE_GUI_RECORDING_SINK_MODE");
}

void test_manifest_references_recording_geometry_contract()
{
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() /
        ("orange_session_geometry_" +
         std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    {
        std::ofstream output(folder / "recording_geometry_contract.json");
        output << nlohmann::json{
            {"schema_id", "orange.recording.geometry_contract"},
            {"schema_version", 1},
            {"status", "resolved"},
            {"materialized_assets", {
                {"schema_id", "orange.recording.geometry_assets"},
                {"schema_version", 1},
                {"status", "complete"},
                {"relative_path", "recording_geometry_assets/manifest.json"},
                {"sha256", "sha256:" + std::string(64, 'a')},
            }},
        }.dump(2) << '\n';
    }

    orange::session::SingleClipRecordingSessionManifestOptions options;
    options.producer = "test";
    options.session_id = "geometry_session";
    options.recording_folder = folder.string();
    options.status = "completed";
    const nlohmann::json manifest =
        orange::session::build_single_clip_recording_session_manifest(options);
    const nlohmann::json reference = manifest.at("metadata").at(
        "recording_geometry_contract");
    require(reference.at("status") == "resolved",
            "session manifest should preserve geometry contract status");
    require(reference.at("relative_path") == "recording_geometry_contract.json",
            "session manifest should use a recording-relative geometry path");
    require(reference.at("sha256").get<std::string>().rfind("sha256:", 0) == 0,
            "session manifest should checksum the exact geometry contract bytes");
    require(reference.at("materialized_assets").at("status") == "complete",
            "session manifest should expose the recording-local geometry asset bundle");
    require(reference.at("materialized_assets").at("relative_path") ==
                "recording_geometry_assets/manifest.json",
            "session manifest should preserve the recording-relative asset path");
    std::filesystem::remove_all(folder);
}

void test_manifest_freezes_inferred_ptp_tai_clock_contract()
{
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() /
        ("orange_session_clock_contract_" +
         std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);

    const nlohmann::json ptp_summary = {
        {"schema_version", 1},
        {"sync", {
            {"camera_sync_enabled", true},
            {"mode", "ptp_local"}
        }},
        {"cameras", {
            {"2010096", {
                {"camera_serial", "2010096"},
                {"sync_camera_enabled", true},
                {"finalized", true},
                {"last_recording_frame_id", 100},
                {"ptp_offset_ns", {
                    {"samples", 5}, {"min", -700}, {"max", 900},
                    {"last", 300}, {"mean", 125.0}
                }},
                {"latch_minus_frame_ns", {
                    {"samples", 5}, {"min", 4000000}, {"max", 7000000},
                    {"last", 5000000}, {"mean", 5100000.0}
                }},
                {"recording_camera_minus_realtime_ns", {
                    {"samples", 100},
                    {"min", 36985000000LL},
                    {"max", 36995000000LL},
                    {"last", 36990000000LL},
                    {"mean", 36990000000.0}
                }},
                {"ptp_mode_readback", {
                    {"samples", 5}, {"first", "TwoStep"},
                    {"last", "TwoStep"}, {"changes", 0}
                }},
                {"ptp_status_readback", {
                    {"samples", 5}, {"first", "Slave"},
                    {"last", "Slave"}, {"changes", 0}
                }},
                {"ptp_readback_observations", nlohmann::json::array({{
                    {"local_frame_id", 1},
                    {"recording_frame_id", 1},
                    {"ptp_mode", "TwoStep"},
                    {"ptp_status", "Slave"}
                }})}
            }}
        }}
    };
    {
        std::ofstream output(folder / "ptp_sync_summary.json");
        output << ptp_summary.dump(2) << '\n';
    }

    orange::session::SingleClipRecordingSessionManifestOptions options;
    options.producer = "test";
    options.session_id = "clock_session";
    options.recording_folder = folder.string();
    options.status = "completed";
    options.cameras.push_back(make_camera_artifact("2010096", 100));
    const nlohmann::json manifest =
        orange::session::build_single_clip_recording_session_manifest(options);

    std::string error;
    const std::filesystem::path manifest_path = folder / "recording_session.json";
    require(
        orange::session::write_recording_session_manifest(
            manifest_path.string(), manifest, &error),
        "clock contract manifest write should succeed: " + error);

    const nlohmann::json written = read_json(manifest_path);
    const nlohmann::json& contract = written.at("timestamp_clock_contract");
    require(contract.at("schema_id") == "orange.recording.timestamp_clocks",
            "clock contract should carry its schema id");
    require(contract.at("timestamp_fields").at("timestamp_sys").at("clock_id") ==
                "host_realtime",
            "timestamp_sys should resolve to the host realtime clock");
    require(contract.at("timestamp_fields").at("timestamp").at("csv_columns").is_array(),
            "timestamp field aliases should be machine-readable arrays");
    const nlohmann::json& host_clock = contract.at("clocks").at("host_realtime");
    require(host_clock.at("timescale") == "POSIX",
            "host clock should declare POSIX semantics");
    require(host_clock.at("semantic_authority") == "producer_declared",
            "host clock semantics should be producer-declared");

    const nlohmann::json& camera_clock = contract.at("clocks").at("camera_2010096");
    require(camera_clock.at("classification") == "ieee1588_tai",
            "complete PTP evidence should classify the camera clock as inferred TAI");
    require(camera_clock.at("origin") == "1970-01-01T00:00:00 TAI",
            "inferred TAI clock should declare the PTP epoch");
    require(camera_clock.at("semantic_authority") ==
                "inferred_from_recording_evidence",
            "camera TAI semantics should remain explicitly inferred");
    require(camera_clock.at("inference").at("result") == "pass",
            "camera clock should preserve the inference result");
    require(camera_clock.at("evidence_snapshot")
                .at("recording_camera_minus_realtime_ns").at("samples") == 100,
            "final manifest should freeze paired timestamp evidence");
    require(contract.at("evidence").at("ptp_sync_summary").at("sha256")
                .get<std::string>().rfind("sha256:", 0) == 0,
            "clock contract should fingerprint the source PTP summary");
    require(contract.at("clock_state_intervals").at("2010096")[0]
                .at("last_recording_frame_id") == 100,
            "clock assignment should cover the recording frame interval");

    const std::string frozen_summary_sha = contract.at("evidence")
        .at("ptp_sync_summary").at("sha256").get<std::string>();
    {
        std::ofstream output(folder / "ptp_sync_summary.json");
        output << nlohmann::json{{"sync", {{"camera_sync_enabled", false}}}}.dump(2)
               << '\n';
    }
    require(
        orange::session::write_recording_session_manifest(
            manifest_path.string(), written, &error),
        "rewriting a finalized session should preserve its clock contract: " + error);
    const nlohmann::json rewritten = read_json(manifest_path);
    require(rewritten.at("timestamp_clock_contract").at("evidence")
                .at("ptp_sync_summary").at("sha256") == frozen_summary_sha,
            "finalized clock evidence fingerprint should be immutable on later manifest updates");
    require(rewritten.at("timestamp_clock_contract").at("clocks")
                .at("camera_2010096").at("classification") == "ieee1588_tai",
            "later manifest updates should not reclassify a finalized clock");

    std::filesystem::remove_all(folder);
}

void test_manifest_keeps_camera_epoch_unspecified_without_ptp_evidence()
{
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() /
        ("orange_session_clock_unspecified_" +
         std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);

    orange::session::SingleClipRecordingSessionManifestOptions options;
    options.producer = "test";
    options.session_id = "clock_unspecified";
    options.recording_folder = folder.string();
    options.status = "completed";
    options.cameras.push_back(make_camera_artifact("2010096", 10));
    const nlohmann::json manifest =
        orange::session::build_single_clip_recording_session_manifest(options);

    std::string error;
    const std::filesystem::path manifest_path = folder / "recording_session.json";
    require(
        orange::session::write_recording_session_manifest(
            manifest_path.string(), manifest, &error),
        "unspecified clock manifest write should succeed: " + error);

    const nlohmann::json written = read_json(manifest_path);
    const nlohmann::json& contract = written.at("timestamp_clock_contract");
    const nlohmann::json& camera_clock = contract.at("clocks").at("camera_2010096");
    require(camera_clock.at("classification") == "device_defined",
            "missing PTP evidence must not classify a camera clock as TAI");
    require(camera_clock.at("origin").is_null(),
            "missing PTP evidence should leave the camera epoch unspecified");
    require(camera_clock.at("timescale") == "device_defined",
            "missing PTP evidence should retain the device-defined timescale");
    require(contract.at("evidence").at("ptp_sync_summary").at("available") == false,
            "manifest should record that PTP evidence was unavailable");
    require(contract.at("clock_state_intervals").at("2010096")[0]
                .at("last_recording_frame_id") == 10,
            "device-defined clocks should still bind to the recorded frame interval");

    std::filesystem::remove_all(folder);
}

}  // namespace

int main()
{
    struct TestCase {
        const char* name;
        void (*fn)();
    };

    const TestCase tests[] = {
        {"single_clip_manifest_preserves_full_and_crop_outputs",
         test_single_clip_manifest_preserves_full_and_crop_outputs},
        {"rolling_manifest_emits_session_aggregate_and_clip_crop_outputs",
         test_rolling_manifest_emits_session_aggregate_and_clip_crop_outputs},
        {"camera_preferred_recording_sink_resolution",
         test_camera_preferred_recording_sink_resolution},
        {"manifest_references_recording_geometry_contract",
         test_manifest_references_recording_geometry_contract},
        {"manifest_freezes_inferred_ptp_tai_clock_contract",
         test_manifest_freezes_inferred_ptp_tai_clock_contract},
        {"manifest_keeps_camera_epoch_unspecified_without_ptp_evidence",
         test_manifest_keeps_camera_epoch_unspecified_without_ptp_evidence},
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
