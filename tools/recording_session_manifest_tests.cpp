#include "session/recording_session.h"
#include "session/acquisition_index_authority.h"
#include "NvEncoder/Logger.h"
#include "gui/spatial_layout/sha256.h"
#include "shaman_v2_recording_identity.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

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

std::string write_metadata_csv(
    const std::filesystem::path& path,
    const std::vector<uint64_t>& recording_frame_ids)
{
    std::string bytes =
        "frame_id,timestamp,timestamp_sys,recording_frame_id,local_frame_id\n";
    for (const uint64_t frame_id : recording_frame_ids) {
        const std::string frame_text = std::to_string(frame_id);
        bytes += frame_text + ",1000,2000," + frame_text + ",3000\n";
    }
    std::ofstream output(path, std::ios::binary);
    require(static_cast<bool>(output), "failed to open metadata fixture " + path.string());
    output << bytes;
    require(static_cast<bool>(output), "failed to write metadata fixture " + path.string());
    return bytes;
}

orange::session::RecordingOutputDescriptor make_external_crop_output(
    const std::string& serial,
    const std::string& scope,
    const int frame_count)
{
    orange::session::RecordingOutputDescriptor output;
    output.camera_serial = serial;
    orange::session::apply_crop_recording_output_media_contract(&output);
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

std::string canonical_semantic_sha256(const nlohmann::json& value)
{
    return "sha256:" + orange::gui::spatial_layout::checksum::sha256_hex(
        value.dump(-1, ' ', false, nlohmann::json::error_handler_t::strict));
}

nlohmann::json make_locked_camera_ptp_evidence(const uint64_t frame_count)
{
    return {
        {"camera_serial", "2010096"},
        {"sync_camera_enabled", true},
        {"finalized", true},
        {"last_recording_frame_id", frame_count},
        {"ptp_offset_ns", {
            {"samples", 5}, {"min", -700}, {"max", 900},
            {"last", 300}, {"mean", 125.0}
        }},
        {"latch_minus_frame_ns", {
            {"samples", 5}, {"min", 4000000}, {"max", 7000000},
            {"last", 5000000}, {"mean", 5100000.0}
        }},
        {"recording_camera_minus_realtime_ns", {
            {"samples", frame_count},
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
    };
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
            {"2010096", make_locked_camera_ptp_evidence(100)}
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
    require(camera_clock.at("inference").at("result") ==
                "fallback_inference_pass",
            "camera clock should preserve the fallback inference result");
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

void test_manifest_preserves_untraceable_ptp_timescale_evidence()
{
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() /
        ("orange_session_ptp_management_" +
         std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    const std::filesystem::path evidence_path = folder / "management_evidence.txt";
    {
        std::ofstream output(evidence_path);
        output
            << "captured_at_utc=2026-08-04T22:00:00Z\n"
            << "socket=/var/run/ptp4l\n"
            << "pmc_exit_status=0\n"
            << "a088c2.fffe.69119e-0 seq 0 RESPONSE MANAGEMENT TIME_PROPERTIES_DATA_SET\n"
            << "currentUtcOffset 37\n"
            << "currentUtcOffsetValid 0\n"
            << "ptpTimescale 1\n"
            << "timeTraceable 0\n"
            << "frequencyTraceable 0\n"
            << "timeSource 0xa0\n"
            << "a088c2.fffe.69119e-0 seq 1 RESPONSE MANAGEMENT PARENT_DATA_SET\n"
            << "grandmasterIdentity a088c2.fffe.69119e\n"
            << "a088c2.fffe.69119e-0 seq 2 RESPONSE MANAGEMENT DEFAULT_DATA_SET\n"
            << "twoStepFlag 1\n"
            << "clockIdentity a088c2.fffe.69119e\n"
            << "domainNumber 0\n";
    }
    require(
        ::setenv(
            "ORANGE_PTP_MANAGEMENT_EVIDENCE_PATH",
            evidence_path.c_str(),
            1) == 0,
        "failed to set PTP evidence test path");
    require(
        initialize_ptp_sync_summary(folder.string(), "ptp_direct", 1, true, nullptr),
        "PTP summary initialization should capture management evidence");
    require(
        update_ptp_sync_summary_camera(
            folder.string(), "2010096", make_locked_camera_ptp_evidence(100)),
        "PTP summary should accept camera evidence");
    ::unsetenv("ORANGE_PTP_MANAGEMENT_EVIDENCE_PATH");

    const nlohmann::json summary = read_json(folder / "ptp_sync_summary.json");
    const nlohmann::json& host = summary.at("host_ptp_management_evidence");
    require(host.at("available") == true,
            "all three pmc datasets should be available");
    require(host.at("time_properties").at("ptpTimescale") == true,
            "pmc ptpTimescale should parse as a boolean");
    require(host.at("time_properties").at("currentUtcOffsetValid") == false,
            "invalid UTC-offset evidence must remain invalid");
    require(host.at("default").at("domainNumber") == 0,
            "PTP domain should be preserved");
    require(host.at("parent").at("grandmasterIdentity") ==
                "a088c2.fffe.69119e",
            "grandmaster identity should be preserved");

    orange::session::SingleClipRecordingSessionManifestOptions options;
    options.producer = "test";
    options.session_id = "ptp_direct";
    options.recording_folder = folder.string();
    options.status = "completed";
    options.cameras.push_back(make_camera_artifact("2010096", 100));
    const nlohmann::json manifest =
        orange::session::build_single_clip_recording_session_manifest(options);
    std::string error;
    require(
        orange::session::write_recording_session_manifest(
            (folder / "recording_session.json").string(), manifest, &error),
        "PTP management manifest write should succeed: " + error);
    const nlohmann::json written = read_json(folder / "recording_session.json");
    const nlohmann::json& camera_clock = written.at("timestamp_clock_contract")
        .at("clocks").at("camera_2010096");
    require(camera_clock.at("classification") ==
                "ieee1588_ptp_timescale_untraceable",
            "invalid UTC-offset flag must prevent authoritative TAI classification");
    require(camera_clock.at("timescale") == "PTP",
            "the advertised PTP timescale should still be preserved");
    require(camera_clock.at("semantic_authority") ==
                "ptp_timescale_advertised_utc_relationship_unvalidated",
            "untraceable PTP evidence should remain explicitly qualified");

    std::filesystem::remove_all(folder);
}

void test_manifest_freezes_external_returned_identity_proof()
{
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() /
        ("orange_session_frame_identity_" +
         std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder / "external_recorder");

    {
        std::ofstream output(folder / "external_recorder_contract.json");
        output << nlohmann::json{
            {"schema_id", "orange.external_recorder.contract"},
            {"schema_version", 1},
            {"require_frame_identity_proof", true},
        }.dump(2) << '\n';
    }
    const std::filesystem::path summary_path =
        folder / "external_recorder" / "Cam2010096_external_summary.json";
    const nlohmann::json proof = {
        {"schema_id", "orange.external_recorder.frame_identity_proof"},
        {"schema_version", 1},
        {"status", "passed"},
        {"canonical_field", "recording_frame_id"},
        {"scope", "recording_session_and_camera_stream"},
        {"assignment_event", "orange_acquisition_recording_frame_sequence"},
        {"row_granularity", "one_encoded_video_frame"},
        {"legacy_aliases", {{"frame_id", "recording_frame_id"}}},
        {"continuity_policy", "encoded_subset"},
        {"recording_frame_id_gaps_allowed", true},
        {"source_frames_skipped_by_policy", 0},
        {"source_frames_dropped", 0},
        {"video_binding", {
            {"method", "nvenc_input_timestamp_to_output_timestamp_registry"},
            {"metadata_write_event", "completed_gop_after_returned_identity_match"},
            {"submitted_frame_identities", 100},
            {"returned_identity_matches", 100},
            {"identity_mismatches", 0},
            {"outstanding_submitted_identities", 0},
            {"encoded_video_frames", 100},
            {"packets_written", 100},
            {"metadata_rows", 100},
            {"verification_rule_id", "orange.external_recorder.frame_identity.v1"},
            {"verified", true},
        }},
    };
    {
        std::ofstream output(summary_path);
        output << nlohmann::json{
            {"schema_id", "orange.external_recorder.summary"},
            {"schema_version", 1},
            {"frames_encoded", 100},
            {"frame_identity_proof", proof},
        }.dump(2) << '\n';
    }

    orange::session::SingleClipRecordingSessionManifestOptions options;
    options.producer = "test";
    options.session_id = "identity_session";
    options.recording_folder = folder.string();
    options.status = "completed";
    options.recording_backend = {
        {"mode", "external_ipc"},
        {"summary_json", {{"2010096", summary_path.string()}}},
    };
    options.cameras.push_back(make_camera_artifact("2010096", 100));
    const nlohmann::json manifest =
        orange::session::build_single_clip_recording_session_manifest(options);

    std::string error;
    const std::filesystem::path manifest_path = folder / "recording_session.json";
    require(
        orange::session::write_recording_session_manifest(
            manifest_path.string(), manifest, &error),
        "returned identity manifest write should succeed: " + error);
    const nlohmann::json written = read_json(manifest_path);
    const nlohmann::json& contract = written.at("frame_identity_contract");
    require(contract.at("status") == "finalized",
            "frame identity contract should finalize with the session");
    require(contract.at("verification").at("result") == "passed",
            "returned identity contract should pass");
    require(contract.at("camera_streams").at("2010096")
                .at("verification_status") == "passed",
            "camera stream should preserve its returned identity proof");
    require(contract.at("camera_streams").at("2010096").at("summary")
                .at("sha256").get<std::string>().rfind("sha256:", 0) == 0,
            "frame identity source summary should be checksummed");

    const nlohmann::json frozen_contract = contract;
    {
        std::ofstream output(summary_path);
        output << nlohmann::json{{"frames_encoded", 100}}.dump(2) << '\n';
    }
    require(
        orange::session::write_recording_session_manifest(
            manifest_path.string(), written, &error),
        "rewriting should preserve a finalized frame identity contract: " + error);
    require(read_json(manifest_path).at("frame_identity_contract") == frozen_contract,
            "finalized frame identity evidence should be immutable");

    std::filesystem::remove_all(folder);
}

void test_manifest_seals_dense_acquisition_index_mapping_and_exact_metadata_checksum()
{
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() /
        ("orange_session_acquisition_mapping_dense_" +
         std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    const std::string metadata_bytes = write_metadata_csv(
        folder / "Cam2010096_meta.csv", {1, 2, 3});

    orange::session::SingleClipRecordingSessionManifestOptions options;
    options.producer = "test";
    options.session_id = "acquisition_mapping_dense";
    options.recording_folder = folder.string();
    options.status = "completed";
    options.cameras.push_back(make_camera_artifact("2010096", 3));
    const nlohmann::json manifest =
        orange::session::build_single_clip_recording_session_manifest(options);
    const std::filesystem::path manifest_path = folder / "recording_session.json";
    std::string error;
    require(
        orange::session::write_recording_session_manifest(
            manifest_path.string(), manifest, &error),
        "dense acquisition mapping manifest write should succeed: " + error);

    const nlohmann::json written = read_json(manifest_path);
    const nlohmann::json& mapping = written.at("acquisition_index_mapping");
    require(mapping.at("schema_id") == "orange.recording.acquisition_index_mapping",
            "acquisition mapping should use the closed Orange schema");
    require(mapping.at("schema_version") == 1,
            "acquisition mapping should use schema v1");
    require(mapping.at("status") == "finalized",
            "a dense recording sequence should receive a mapping seal");
    require(mapping.at("recording_id") == "acquisition_mapping_dense",
            "mapping must bind the recording session id");
    require(mapping.at("frame_identity_contract_ref") == "#/frame_identity_contract",
            "mapping must bind the immutable frame identity sibling");
    require(mapping.at("frame_identity_contract_sha256") ==
                canonical_semantic_sha256(written.at("frame_identity_contract")),
            "mapping must bind the exact canonical frame-identity digest");
    const nlohmann::json& stream = mapping.at("camera_streams").at("2010096");
    require(stream.at("coverage").at("first_recording_frame_id") == 1 &&
                stream.at("coverage").at("last_recording_frame_id") == 3 &&
                stream.at("coverage").at("total_acquisitions") == 3 &&
                stream.at("coverage").at("metadata_row_count") == 3 &&
                stream.at("coverage").at("gap_count") == 0,
            "dense mapping coverage should faithfully report metadata rows and range");
    require(stream.at("conversion").at("expression") ==
                "source_acquisition_frame_index = recording_frame_id - 1",
            "mapping must declare the one-based to zero-based subtraction");
    require(stream.at("source_metadata_artifact").at("relative_path") ==
                "Cam2010096_meta.csv",
            "mapping metadata evidence must remain recording-relative");
    require(stream.at("source_metadata_artifact").at("sha256") ==
                "sha256:" + orange::gui::spatial_layout::checksum::sha256_hex(metadata_bytes),
            "mapping must checksum exact metadata artifact bytes");
    require(written.at("acquisition_index_mapping_sha256") ==
                canonical_semantic_sha256(mapping),
            "a finalized mapping must publish its exact canonical semantic digest");

    orange::session::AcquisitionIndexAuthority authority;
    require(
        orange::session::resolve_acquisition_index_authority(
            written,
            manifest_path,
            "2010096",
            &authority,
            &error),
        "the emitted manifest must resolve through the strict acquisition authority: " +
            error);
    std::int64_t source_index = -1;
    require(
        authority.recording_frame_id_to_source_acquisition_index(
            3, &source_index, &error) && source_index == 2,
        "the emitted dense recording domain must map recording frame 3 to index 2");

    const nlohmann::json frozen_mapping = mapping;
    const std::string frozen_mapping_digest =
        written.at("acquisition_index_mapping_sha256").get<std::string>();
    {
        std::ofstream output(folder / "Cam2010096_meta.csv", std::ios::binary);
        output << "recording_frame_id,timestamp\n1,1001\n2,1001\n3,1001\n";
    }
    require(
        orange::session::write_recording_session_manifest(
            manifest_path.string(), written, &error),
        "rewriting a sealed acquisition mapping should succeed: " + error);
    const nlohmann::json rewritten = read_json(manifest_path);
    require(rewritten.at("acquisition_index_mapping") == frozen_mapping &&
                rewritten.at("acquisition_index_mapping_sha256") == frozen_mapping_digest,
            "finalized acquisition mapping evidence must remain immutable on rewrite");

    std::filesystem::remove_all(folder);
}

void test_manifest_leaves_gapped_acquisition_index_mapping_unsealed()
{
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() /
        ("orange_session_acquisition_mapping_gap_" +
         std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    write_metadata_csv(folder / "Cam2010096_meta.csv", {1, 3});

    orange::session::SingleClipRecordingSessionManifestOptions options;
    options.producer = "test";
    options.session_id = "acquisition_mapping_gap";
    options.recording_folder = folder.string();
    options.status = "completed";
    options.cameras.push_back(make_camera_artifact("2010096", 2));
    const nlohmann::json manifest =
        orange::session::build_single_clip_recording_session_manifest(options);
    const std::filesystem::path manifest_path = folder / "recording_session.json";
    std::string error;
    require(
        orange::session::write_recording_session_manifest(
            manifest_path.string(), manifest, &error),
        "gapped acquisition mapping manifest write should retain provenance: " + error);

    const nlohmann::json written = read_json(manifest_path);
    const nlohmann::json& mapping = written.at("acquisition_index_mapping");
    const nlohmann::json& stream = mapping.at("camera_streams").at("2010096");
    require(mapping.at("status") == "unsealed" &&
                mapping.at("reason_code") == "camera_stream_not_sealable_v1",
            "a gapped source sequence must fail closed at mapping scope");
    require(stream.at("status") == "unsealed" &&
                stream.at("reason_code") == "noncontiguous_recording_frame_ids",
            "a gapped source sequence must state the stable stream reason");
    require(stream.at("coverage").at("first_recording_frame_id") == 1 &&
                stream.at("coverage").at("last_recording_frame_id") == 3 &&
                stream.at("coverage").at("metadata_row_count") == 2 &&
                stream.at("coverage").at("gap_count") == 1,
            "unsealed evidence must preserve the observed sparse coverage");
    require(!stream.contains("conversion") &&
                !written.contains("acquisition_index_mapping_sha256"),
            "v1 must omit a conversion seal and digest for a gapped recording");

    std::filesystem::remove_all(folder);
}

void test_manifest_rejects_malformed_acquisition_metadata_ids()
{
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() /
        ("orange_session_acquisition_mapping_malformed_" +
         std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    {
        std::ofstream output(folder / "Cam2010096_meta.csv", std::ios::binary);
        output
            << "frame_id,timestamp,timestamp_sys,recording_frame_id\n"
            << "1,1000,2000,1\n"
            << "1junk,1001,2001,1junk\n"
            << "-2,1002,2002,-2\n"
            << " 3,1003,2003, 3\n"
            << "0,1004,2004,0\n";
    }

    orange::session::SingleClipRecordingSessionManifestOptions options;
    options.producer = "test";
    options.session_id = "acquisition_mapping_malformed";
    options.recording_folder = folder.string();
    options.status = "completed";
    options.cameras.push_back(make_camera_artifact("2010096", 2));
    const std::filesystem::path manifest_path = folder / "recording_session.json";
    std::string error;
    require(
        orange::session::write_recording_session_manifest(
            manifest_path.string(),
            orange::session::build_single_clip_recording_session_manifest(options),
            &error),
        "malformed metadata should be retained as unsealed provenance: " + error);

    const nlohmann::json written = read_json(manifest_path);
    const nlohmann::json& stream = written.at("acquisition_index_mapping")
        .at("camera_streams").at("2010096");
    require(stream.at("status") == "unsealed" &&
                stream.at("reason_code") == "noncontiguous_recording_frame_ids" &&
                stream.at("coverage").at("metadata_row_count") == 5 &&
                stream.at("coverage").at("total_acquisitions") == 1,
            "trailing, signed, whitespace-prefixed, and zero IDs must not be dense evidence");
    require(!stream.contains("conversion"),
            "malformed recording IDs must not receive a conversion declaration");

    std::filesystem::remove_all(folder);
}

void test_manifest_requires_explicit_equal_frame_identity_aliases()
{
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() /
        ("orange_session_acquisition_mapping_aliases_" +
         std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);

    orange::session::SingleClipRecordingSessionManifestOptions options;
    options.producer = "test";
    options.session_id = "acquisition_mapping_aliases";
    options.recording_folder = folder.string();
    options.status = "completed";
    options.cameras.push_back(make_camera_artifact("2010096", 2));
    const std::filesystem::path manifest_path = folder / "recording_session.json";
    std::string error;

    {
        std::ofstream output(folder / "Cam2010096_meta.csv", std::ios::binary);
        output << "frame_id,timestamp,timestamp_sys\n"
               << "1,1000,2000\n"
               << "2,1001,2001\n";
    }
    require(
        orange::session::write_recording_session_manifest(
            manifest_path.string(),
            orange::session::build_single_clip_recording_session_manifest(options),
            &error),
        "legacy-only frame metadata should remain inspectable: " + error);
    nlohmann::json written = read_json(manifest_path);
    require(
        written.at("acquisition_index_mapping").at("camera_streams")
                .at("2010096").at("reason_code") ==
            "metadata_identity_alias_unavailable" &&
            !written.contains("acquisition_index_mapping_sha256"),
        "a metadata CSV without both aliases must not receive an authority seal");

    {
        std::ofstream output(folder / "Cam2010096_meta.csv", std::ios::binary);
        output << "frame_id,timestamp,timestamp_sys,recording_frame_id\n"
               << "1,1000,2000,1\n"
               << "9,1001,2001,2\n";
    }
    // Start from the original manifest shape so the prior unsealed diagnostic
    // does not influence construction of the next candidate.
    require(
        orange::session::write_recording_session_manifest(
            manifest_path.string(),
            orange::session::build_single_clip_recording_session_manifest(options),
            &error),
        "mismatched aliases should remain inspectable: " + error);
    written = read_json(manifest_path);
    require(
        written.at("acquisition_index_mapping").at("camera_streams")
                .at("2010096").at("reason_code") ==
            "metadata_identity_alias_mismatch" &&
            !written.contains("acquisition_index_mapping_sha256"),
        "checksum-valid but unequal aliases must not receive an authority seal");

    std::filesystem::remove_all(folder);
}

void test_manifest_rejects_frame_identity_camera_binding_mismatch()
{
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() /
        ("orange_session_acquisition_mapping_identity_mismatch_" +
         std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    write_metadata_csv(folder / "Cam2010096_meta.csv", {1, 2});

    orange::session::SingleClipRecordingSessionManifestOptions options;
    options.producer = "test";
    options.session_id = "acquisition_mapping_identity_mismatch";
    options.recording_folder = folder.string();
    options.status = "completed";
    options.cameras.push_back(make_camera_artifact("2010096", 2));
    nlohmann::json manifest =
        orange::session::build_single_clip_recording_session_manifest(options);
    // This has the recognized v1 outer identity so the existing writer must
    // preserve it, but the declared stream is bound to another camera.
    manifest["frame_identity_contract"] = {
        {"schema_id", "orange.recording.frame_identity"},
        {"schema_version", 1},
        {"status", "finalized"},
        {"canonical_field", "recording_frame_id"},
        {"scope", "recording_session_and_camera_stream"},
        {"assignment_event", "orange_acquisition_recording_frame_sequence"},
        {"legacy_aliases", {{"frame_id", "recording_frame_id"}}},
        {"camera_streams", {
            {"2010096", {{"camera_serial", "2010095"}}},
        }},
    };
    const std::filesystem::path manifest_path = folder / "recording_session.json";
    std::string error;
    require(
        orange::session::write_recording_session_manifest(
            manifest_path.string(), manifest, &error),
        "identity mismatch should be retained as unsealed provenance: " + error);

    const nlohmann::json written = read_json(manifest_path);
    const nlohmann::json& mapping = written.at("acquisition_index_mapping");
    const nlohmann::json& stream = mapping.at("camera_streams").at("2010096");
    require(mapping.at("status") == "unsealed" &&
                stream.at("reason_code") == "frame_identity_camera_binding_mismatch" &&
                !stream.contains("conversion"),
            "a mismatched frame-identity camera binding must fail closed");
    require(mapping.contains("frame_identity_contract_ref") &&
                mapping.contains("frame_identity_contract_sha256"),
            "a supported finalized identity remains digest-bound even when a stream mismatches");

    std::filesystem::remove_all(folder);
}

void test_manifest_rejects_corrupt_finalized_acquisition_mapping()
{
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() /
        ("orange_session_acquisition_mapping_corrupt_" +
         std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    write_metadata_csv(folder / "Cam2010096_meta.csv", {1, 2});

    orange::session::SingleClipRecordingSessionManifestOptions options;
    options.producer = "test";
    options.session_id = "acquisition_mapping_corrupt";
    options.recording_folder = folder.string();
    options.status = "completed";
    options.cameras.push_back(make_camera_artifact("2010096", 2));
    const std::filesystem::path manifest_path = folder / "recording_session.json";
    std::string error;
    require(
        orange::session::write_recording_session_manifest(
            manifest_path.string(),
            orange::session::build_single_clip_recording_session_manifest(options),
            &error),
        "initial sealed mapping write should succeed: " + error);
    const nlohmann::json sealed = read_json(manifest_path);

    nlohmann::json corrupt = sealed;
    corrupt["acquisition_index_mapping_sha256"] = "sha256:" + std::string(64, '0');
    error.clear();
    require(
        !orange::session::write_recording_session_manifest(
            manifest_path.string(), corrupt, &error),
        "a finalized mapping with a mismatched digest must be rejected");
    require(error.find("invalid semantic-record digest") != std::string::npos &&
                read_json(manifest_path) == sealed,
            "a corrupt finalized mapping must not rewrite the existing manifest");

    corrupt = sealed;
    corrupt.erase("acquisition_index_mapping_sha256");
    error.clear();
    require(
        !orange::session::write_recording_session_manifest(
            manifest_path.string(), corrupt, &error),
        "a finalized mapping without its sibling digest must be rejected");
    require(read_json(manifest_path) == sealed,
            "a missing finalized mapping digest must not rewrite the existing manifest");

    std::filesystem::remove_all(folder);
}

void test_manifest_rejects_duplicate_camera_serial_mapping()
{
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() /
        ("orange_session_acquisition_mapping_duplicate_camera_" +
         std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    write_metadata_csv(folder / "Cam2010096_meta.csv", {1, 2});

    orange::session::SingleClipRecordingSessionManifestOptions options;
    options.producer = "test";
    options.session_id = "acquisition_mapping_duplicate_camera";
    options.recording_folder = folder.string();
    options.status = "completed";
    options.cameras.push_back(make_camera_artifact("2010096", 2));
    options.cameras.push_back(make_camera_artifact("2010096", 2));
    const std::filesystem::path manifest_path = folder / "recording_session.json";
    std::string error;
    require(
        orange::session::write_recording_session_manifest(
            manifest_path.string(),
            orange::session::build_single_clip_recording_session_manifest(options),
            &error),
        "duplicate camera provenance should be retained as unsealed mapping: " + error);

    const nlohmann::json written = read_json(manifest_path);
    const nlohmann::json& mapping = written.at("acquisition_index_mapping");
    require(mapping.at("status") == "unsealed" &&
                mapping.at("reason_code") == "duplicate_camera_serial" &&
                !mapping.contains("acquisition_index_mapping_sha256") &&
                !written.contains("acquisition_index_mapping_sha256") &&
                !mapping.at("camera_streams").at("2010096").contains("conversion"),
            "duplicate camera entries must not silently collapse into a dense mapping");

    std::filesystem::remove_all(folder);
}

void test_manifest_omits_identity_digest_when_identity_is_unfinalized()
{
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() /
        ("orange_session_acquisition_mapping_identity_pending_" +
         std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    {
        std::ofstream output(folder / "Cam2010096_meta.csv", std::ios::binary);
    }

    orange::session::SingleClipRecordingSessionManifestOptions options;
    options.producer = "test";
    options.session_id = "acquisition_mapping_identity_pending";
    options.recording_folder = folder.string();
    options.status = "recording";
    options.cameras.push_back(make_camera_artifact("2010096", 1));
    const std::filesystem::path manifest_path = folder / "recording_session.json";
    std::string error;
    require(
        orange::session::write_recording_session_manifest(
            manifest_path.string(),
            orange::session::build_single_clip_recording_session_manifest(options),
            &error),
        "pending identity manifest write should retain provenance: " + error);

    const nlohmann::json written = read_json(manifest_path);
    const nlohmann::json& mapping = written.at("acquisition_index_mapping");
    require(mapping.at("status") == "unsealed" &&
                mapping.at("reason_code") == "recording_not_completed" &&
                !mapping.contains("frame_identity_contract_ref") &&
                !mapping.contains("frame_identity_contract_sha256"),
            "an unfinalized identity must not be represented by a placeholder digest");
    const nlohmann::json& stream = mapping.at("camera_streams").at("2010096");
    require(stream.at("source_metadata_artifact").at("sha256") ==
                "sha256:" + orange::gui::spatial_layout::checksum::sha256_hex(""),
            "a readable zero-byte metadata artifact must retain its exact byte digest");
    require(!stream.at("source_metadata_artifact").contains("available"),
            "closed source metadata artifacts must not gain undeclared availability fields");

    std::filesystem::remove_all(folder);
}

void test_manifest_binds_shaman_v2_recording_identity_to_session_id()
{
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() /
        ("orange_session_shaman_v2_identity_" +
         std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);

    orange::session::SingleClipRecordingSessionManifestOptions options;
    options.producer = "test";
    options.session_id = "stable-parent-recording";
    options.recording_folder = folder.string();
    options.status = "recording";
    const std::filesystem::path manifest_path = folder / "recording_session.json";
    const nlohmann::json camera_identity = {
        {"camera_bindings", nlohmann::json::array({{
            {"acquisition_camera_id", "CAM-42"},
            {"camera_serial", "CAM-42"},
            {"shaman_numeric_camera_id", 0},
        }})},
        {"canonicalization", "canonical_json_utf8_sort_keys_compact_v1"},
        {"recording_id", options.session_id},
        {"schema_id", "orange.shaman_v2.camera_identity"},
        {"schema_version", 1},
    };
    {
        std::ofstream snapshot(folder / "recording_snapshot_start.json");
        snapshot << nlohmann::json{
            {"recording_id", options.session_id},
            {"shaman_v2_camera_identity", camera_identity},
            {"shaman_v2_camera_identity_sha256",
             canonical_semantic_sha256(camera_identity)},
        }.dump(2) << '\n';
    }
    std::string error;
    require(
        orange::session::write_recording_session_manifest(
            manifest_path.string(),
            orange::session::build_single_clip_recording_session_manifest(options),
            &error),
        "Shaman-v2 recording identity manifest write should succeed: " + error);

    const nlohmann::json written = read_json(manifest_path);
    const nlohmann::json& identity =
        written.at("shaman_v2_recording_identity");
    require(identity ==
                orange::shaman_v2_recording_identity::binding_record(
                    options.session_id),
            "manifest recording identity must be the closed session_id binding");
    require(written.at("shaman_v2_recording_identity_sha256") ==
                canonical_semantic_sha256(identity),
            "manifest recording identity must carry its semantic digest");
    require(written.at("shaman_v2_camera_identity") == camera_identity &&
                written.at("shaman_v2_camera_identity_sha256") ==
                    canonical_semantic_sha256(camera_identity),
            "manifest must project the immutable acquisition/serial/numeric camera binding");

    nlohmann::json mismatched = written;
    mismatched["session_id"] = "different-recording";
    error.clear();
    require(!orange::session::write_recording_session_manifest(
                manifest_path.string(), mismatched, &error),
            "an existing recording token must not survive a session_id change");
    require(error.find("does not match session_id") != std::string::npos,
            "session/token mismatch should report a stable diagnostic");

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
        {"manifest_preserves_untraceable_ptp_timescale_evidence",
         test_manifest_preserves_untraceable_ptp_timescale_evidence},
        {"manifest_freezes_external_returned_identity_proof",
         test_manifest_freezes_external_returned_identity_proof},
        {"manifest_seals_dense_acquisition_index_mapping_and_exact_metadata_checksum",
         test_manifest_seals_dense_acquisition_index_mapping_and_exact_metadata_checksum},
        {"manifest_leaves_gapped_acquisition_index_mapping_unsealed",
         test_manifest_leaves_gapped_acquisition_index_mapping_unsealed},
        {"manifest_rejects_malformed_acquisition_metadata_ids",
         test_manifest_rejects_malformed_acquisition_metadata_ids},
        {"manifest_requires_explicit_equal_frame_identity_aliases",
         test_manifest_requires_explicit_equal_frame_identity_aliases},
        {"manifest_rejects_frame_identity_camera_binding_mismatch",
         test_manifest_rejects_frame_identity_camera_binding_mismatch},
        {"manifest_rejects_corrupt_finalized_acquisition_mapping",
         test_manifest_rejects_corrupt_finalized_acquisition_mapping},
        {"manifest_rejects_duplicate_camera_serial_mapping",
         test_manifest_rejects_duplicate_camera_serial_mapping},
        {"manifest_omits_identity_digest_when_identity_is_unfinalized",
         test_manifest_omits_identity_digest_when_identity_is_unfinalized},
        {"manifest_binds_shaman_v2_recording_identity_to_session_id",
         test_manifest_binds_shaman_v2_recording_identity_to_session_id},
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
