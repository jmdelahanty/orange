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
#include <utility>
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

void write_nonempty_artifact(
    const std::filesystem::path& path,
    const std::string& bytes = "fixture\n")
{
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output),
            "failed to open full-frame artifact fixture " + path.string());
    output << bytes;
    require(static_cast<bool>(output),
            "failed to write full-frame artifact fixture " + path.string());
}

void materialize_full_frame_artifacts(
    const std::filesystem::path& folder,
    const orange::session::RecordingSessionCameraArtifact& artifact)
{
    write_nonempty_artifact(folder / artifact.video_path, "video-fixture\n");
    write_nonempty_artifact(folder / artifact.keyframe_path,
                            "{\"keyframes\":[1]}\n");
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

orange::session::RecordingOutputDescriptor make_spatial_roi_output(
    const std::string& serial,
    const std::string& logical_stream_id)
{
    orange::session::RecordingOutputDescriptor output;
    output.camera_serial = serial;
    output.output_kind = orange::session::kSpatialRoiRecordingOutputKind;
    output.logical_stream_id = logical_stream_id;
    output.role = "sidecar";
    output.backend = "external_ipc";
    output.status = "completed";
    output.video_path = logical_stream_id + ".mp4";
    output.frame_count = 10;
    output.first_recording_frame_id = 1;
    output.last_recording_frame_id = 10;
    output.recording_frame_id_gaps = 0;
    output.packet_count = 10;
    output.packet_count_source = "spatial_roi_finalized_receipt.packet_count";
    const std::string roi_id = logical_stream_id.substr(
        logical_stream_id.rfind('_') + 1);
    const nlohmann::json artifacts = {
        {"video", "external_spatial_roi_recorder/" + logical_stream_id + ".mp4"},
        {"metadata", "external_spatial_roi_recorder/" + logical_stream_id + "_meta.csv"},
        {"keyframes", "external_spatial_roi_recorder/" + logical_stream_id + "_keyframe.json"},
        {"perf", "external_spatial_roi_recorder/" + logical_stream_id + "_perf.csv"},
        {"summary", "external_spatial_roi_recorder/" + logical_stream_id + "_summary.json"},
        {"status", "external_spatial_roi_recorder/" + logical_stream_id + "_status.json"},
        {"video_sanity", "external_spatial_roi_recorder/" + logical_stream_id + "_video_sanity.json"},
        {"finalization", "external_spatial_roi_recorder/" + logical_stream_id + ".mp4.finalization.json"},
        {"recorder_log", "external_spatial_roi_recorder/" + logical_stream_id + "_recorder.log"},
        {"transport_sidecar", "external_spatial_roi_recorder/" + logical_stream_id + "_transport.jsonl"},
        {"evidence", "external_spatial_roi_recorder/" + logical_stream_id + "_evidence.jsonl"},
        {"evidence_manifest", "external_spatial_roi_recorder/" + logical_stream_id + "_evidence_manifest.json"}
    };
    output.details = {
        {"artifact_path_scope", "recording_root_relative"},
        {"artifact_root_relative", "external_spatial_roi_recorder"},
        {"artifacts", artifacts},
        {"camera_id", 7},
        {"camera_serial", serial},
        {"assigned_gpu_id", 3},
        {"session_id", "session_spatial_roi"},
        {"frame_identity", {
            {"key_fields", {"recording_id", "logical_stream_id", "roi_id"}},
            {"roi_stream_frame_index", "dense_accepted_frame_order"},
            {"recording_frame_id_source", "parent_camera_recording"}
        }},
        {"identity", {
            {"camera_id", 7},
            {"camera_serial", serial},
            {"logical_stream_id", logical_stream_id},
            {"producer_generation", "generation_001"},
            {"recording_id", "session_spatial_roi"},
            {"recording_identity_token", "sha256:" + std::string(64, 'a')},
            {"arena_group_id", "arena_group_0"},
            {"region_id", "region_" + roi_id},
            {"roi_id", roi_id},
            {"spatial_roi_plan_sha256", "sha256:" + std::string(64, 'b')}
        }},
        {"logical_stream_id", logical_stream_id},
        {"producer_generation", "generation_001"},
        {"recording_id", "session_spatial_roi"},
        {"recording_identity_token", "sha256:" + std::string(64, 'a')},
        {"region_id", "region_" + roi_id},
        {"roi_id", roi_id},
        {"spatial_roi_plan_sha256", "sha256:" + std::string(64, 'b')}
    };
    nlohmann::json receipt_artifacts = nlohmann::json::array();
    for (const char* kind : {"video", "metadata", "keyframes", "perf",
                             "summary", "status", "video_sanity", "finalization",
                             "recorder_log", "transport_sidecar", "evidence",
                             "evidence_manifest"}) {
        receipt_artifacts.push_back({{"kind", kind}});
    }
    output.details["finalized_receipt"] = {
        {"logical_stream_id", logical_stream_id},
        {"identity", {
            {"recording_id", "session_spatial_roi"},
            {"session_id", "session_spatial_roi"},
            {"recording_identity_token", "sha256:" + std::string(64, 'a')},
            {"producer_generation", "generation_001"},
            {"spatial_roi_plan_sha256", "sha256:" + std::string(64, 'b')},
            {"camera_id", 7},
            {"camera_serial", serial},
            {"roi_id", roi_id},
            {"region_id", "region_" + roi_id},
            {"arena_group_id", "arena_group_0"},
            {"logical_stream_id", logical_stream_id},
            {"assigned_gpu_id", 3},
            {"assigned_shard_id", 0}
        }},
        {"counts", {
            {"detach_successes", 10},
            {"dispatch_admitted", 10},
            {"dispatch_rejected", 0},
            {"ack_attempted", 10},
            {"ack_sent", 10},
            {"ack_accepted", 10},
            {"release_attempted", 10},
            {"release_sent", 10},
            {"encoded_frames", 10},
            {"failed_frames", 0},
            {"packet_count", 10},
            {"encoded_bytes", 100},
            {"keyframes", 10},
            {"ack_write_failures", 0},
            {"release_write_failures", 0},
            {"lifecycle_failures", 0}
        }},
        {"ranges", {
            {"recording_frame_id", {{"first", 1}, {"last", 10}}},
            {"roi_stream_frame_index", {{"first", 1}, {"last", 10}}},
            {"has_frames", true},
            {"frame_count", 10}
        }},
        {"finalized_receipt_digest", "sha256:" + std::string(64, 'c')},
        {"artifacts", receipt_artifacts}
    };
    for (nlohmann::json& artifact : output.details["finalized_receipt"]["artifacts"]) {
        const std::string recording_relative_path =
            artifacts.at(artifact.at("kind")).get<std::string>();
        artifact["relative_path"] = recording_relative_path.substr(
            std::string("external_spatial_roi_recorder/").size());
        artifact["size_bytes"] = 1;
        artifact["sha256"] = "sha256:" + std::string(64, 'd');
    }
    return output;
}

orange::session::RecordingOutputDescriptor make_gop25_spatial_roi_output(
    const std::string& serial,
    const std::string& logical_stream_id)
{
    auto output = make_spatial_roi_output(serial, logical_stream_id);
    output.details["encode_profile"] = {
        {"profile_id", "hevc_p1_low_latency_vbr_q20_gop25_v1"},
        {"codec", "hevc"},
        {"preset", "p1"},
        {"tuning", "ll"},
        {"lossless", false},
        {"rate_control_mode", "vbr"},
        {"quality_value", 20},
        {"gop_length", 25},
        {"aq", false},
        {"temporal_aq", false},
        {"lookahead", false},
        {"lookahead_depth", 0},
        {"frame_rate", 100},
        {"input_format", "mono8"},
        {"encoded_format", "nv12"},
        {"no_resize", true},
        {"luma_preserved_exactly", false},
        {"neutral_chroma_value", 128}};
    output.details["rate_control_mode"] = "vbr";
    output.details["quality_value"] = 20;
    output.details["gop"] = 25;
    output.codec = "hevc";
    output.tuning = "ll";
    output.pixel_source_format = "mono8";
    output.encoded_format = "nv12";
    output.details["finalized_receipt"]["counts"]["keyframes"] = 1;
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
    require(camera_outputs["full"].contains("encoding_budget"),
            "single-clip full output should carry an encoding budget contract");
    require(camera_outputs["crop"].contains("encoding_budget"),
            "single-clip crop output should carry an encoding budget contract");
    require(camera_outputs["crop"]["encoding_budget"]["target"].value(
                "status", std::string()) == "not_applicable",
            "lossless crop should declare target bitrate not applicable");

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
    require(top_crop.contains("encoding_budget"),
            "rolling session crop output should carry an encoding budget contract");
    require(clip_camera_outputs["full"].contains("encoding_budget"),
            "rolling clip full output should carry an encoding budget contract");
    require(clip_camera_outputs["crop"].contains("encoding_budget"),
            "rolling clip crop output should carry an encoding budget contract");
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

void test_single_clip_manifest_indexes_multiple_spatial_roi_streams()
{
    orange::session::SingleClipRecordingSessionManifestOptions options;
    options.producer = "orange_spatial_roi_test";
    options.session_id = "session_spatial_roi";
    options.recording_folder = "/tmp/orange_session_manifest_spatial_roi";
    options.status = "completed";
    options.recording_backend = {
        {"mode", "external_ipc"},
        {"full_frame", {{"first_class", true}}},
    };
    options.cameras.push_back(make_camera_artifact("2010096", 10));

    const std::string roi_one = "2010096_spatial_roi_roi_1";
    const std::string roi_two = "2010096_spatial_roi_roi_2";
    options.recording_outputs.push_back(
        make_external_crop_output("2010096", "single_clip", 10));
    options.recording_outputs.push_back(
        make_spatial_roi_output("2010096", roi_one));
    options.recording_outputs.push_back(
        make_spatial_roi_output("2010096", roi_two));

    const nlohmann::json manifest =
        orange::session::build_single_clip_recording_session_manifest(options);
    const auto& legacy = manifest.at("recording_outputs").at("2010096");
    require(legacy.contains("full") && legacy.contains("crop"),
            "schema-2 manifest view should retain full and scalar crop");
    require(manifest.at("recording_backend").at("mode") == "external_ipc" &&
                manifest.at("recording_backend").at("full_frame").at(
                    "first_class") == true,
            "combined manifest should preserve the selected external full-frame backend");
    require(!legacy.contains("spatial_roi"),
            "schema-2 manifest view should not collapse ROI streams");

    const auto& versioned = manifest.at("recording_outputs_v3");
    require(versioned.value("schema_version", 0) == 3,
            "single-clip manifest should emit output schema v3");
    const auto& camera = versioned.at("cameras").at("2010096");
    require(camera.contains("full") && camera.contains("crop"),
            "v3 manifest view should retain first-class full and crop");
    require(camera.at("spatial_roi").size() == 2,
            "v3 manifest should retain every spatial ROI stream");
    require(camera.at("spatial_roi").at(roi_one).value(
                "logical_stream_id", std::string()) == roi_one,
            "v3 manifest should key ROI streams by logical_stream_id");
    require(manifest.at("clips")[0].at("recording_outputs_v3") == versioned,
            "clip compatibility entry should carry the same v3 output index");
    const auto& evidence = manifest.at("spatial_roi_recording_evidence");
    require(evidence.value("schema_id", std::string()) ==
                "orange.spatial_roi_recording.manifest_evidence_index" &&
                evidence.value("schema_version", 0) == 1 &&
                evidence.value("status", std::string()) == "complete" &&
                evidence.value("stream_count", 0) == 2 &&
                evidence.value("status_semantics", std::string()) ==
                    "stream_lifecycle_only" &&
                evidence.at("verification").value("status", std::string()) ==
                    "validated" &&
                evidence.at("verification").value(
                    "receipt_promotion_required", true) == false,
            "complete manifest should promote closed ROI receipts");
    const auto& evidence_stream = evidence.at("streams").at("2010096").at(roi_one);
    require(evidence_stream.at("descriptor_ref") ==
                "#/recording_outputs_v3/cameras/2010096/spatial_roi/" + roi_one &&
                evidence_stream.at("frame_identity").at("recording_frame_id_source") ==
                    "parent_camera_recording" &&
                evidence_stream.at("artifact_bindings").size() == 12 &&
                evidence_stream.at("artifact_bindings").at("video").value(
                    "relative_path", std::string()) ==
                    options.recording_outputs[1].details.at("artifacts").at("video") &&
                evidence_stream.at("artifact_bindings").at("video").value(
                    "receipt_relative_path", std::string()) == roi_one + ".mp4" &&
                evidence_stream.at("artifact_bindings").at("video").value(
                    "size_bytes", 0) == 1 &&
                evidence_stream.at("artifact_bindings").at("video").value(
                    "verification_status", std::string()) ==
                    "finalized_receipt_validated" &&
                evidence_stream.at("counts").at("encoded_frames") == 10 &&
                evidence_stream.at("ranges").at("frame_count") == 10 &&
                evidence_stream.at("finalized_receipt_digest") ==
                    "sha256:" + std::string(64, 'c') &&
                evidence_stream.at("evidence_manifest_relative_path") ==
                    "external_spatial_roi_recorder/" + roi_one + "_evidence_manifest.json",
            "ROI evidence binding should promote exact finalized artifact receipts and stream ranges");
    require(manifest.at("clips")[0].at("spatial_roi_recording_evidence") ==
                evidence,
            "clip compatibility entry should carry the same ROI evidence binding");
}

void test_single_clip_manifest_accepts_gop25_keyframe_cadence()
{
    orange::session::SingleClipRecordingSessionManifestOptions options;
    options.producer = "orange_spatial_roi_gop25_test";
    options.session_id = "session_spatial_roi_gop25";
    options.recording_folder = "/tmp/orange_session_manifest_spatial_roi_gop25";
    options.status = "completed";
    options.recording_backend = {
        {"mode", "external_ipc"},
        {"full_frame", {{"first_class", true}}},
    };
    options.cameras.push_back(make_camera_artifact("2010096", 10));
    options.recording_outputs.push_back(
        make_external_crop_output("2010096", "single_clip", 10));
    options.recording_outputs.push_back(
        make_gop25_spatial_roi_output(
            "2010096", "2010096_spatial_roi_roi_1"));
    options.recording_outputs.push_back(
        make_gop25_spatial_roi_output(
            "2010096", "2010096_spatial_roi_roi_2"));

    const nlohmann::json manifest =
        orange::session::build_single_clip_recording_session_manifest(options);
    require(!manifest.empty(),
            "complete manifest should accept GOP25 receipt cadence");
    require(manifest.at("spatial_roi_recording_evidence")
                    .at("streams").at("2010096")
                    .at("2010096_spatial_roi_roi_1").at("counts")
                    .at("keyframes") == 1,
            "GOP25 manifest evidence should retain the profile-derived keyframe count");

    options.recording_outputs[1].details["finalized_receipt"]["counts"]
        ["keyframes"] = 10;
    require(orange::session::build_single_clip_recording_session_manifest(options)
                .empty(),
            "complete manifest must reject all-keyframe evidence for GOP25");
}

void test_complete_manifest_rejects_invalid_spatial_roi_receipt()
{
    orange::session::SingleClipRecordingSessionManifestOptions options;
    options.producer = "orange_spatial_roi_receipt_test";
    options.session_id = "session_spatial_roi_receipt";
    options.recording_folder = "/tmp/orange_session_manifest_spatial_roi_receipt";
    options.status = "completed";
    options.cameras.push_back(make_camera_artifact("2010096", 10));
    options.recording_outputs.push_back(
        make_spatial_roi_output("2010096", "2010096_spatial_roi_roi_1"));

    auto expect_rejected = [&](const std::string& message) {
        require(
            orange::session::build_single_clip_recording_session_manifest(options)
                .empty(),
            message);
    };

    options.recording_outputs[0].details.erase("finalized_receipt");
    expect_rejected("complete manifest must reject a missing finalized receipt");
    options.recording_outputs[0] =
        make_spatial_roi_output("2010096", "2010096_spatial_roi_roi_1");

    auto& receipt =
        options.recording_outputs[0].details["finalized_receipt"];
    receipt["artifacts"][0]["relative_path"] = "substituted.mp4";
    expect_rejected("complete manifest must reject a substituted artifact path");
    options.recording_outputs[0] =
        make_spatial_roi_output("2010096", "2010096_spatial_roi_roi_1");
    options.recording_outputs[0].details["finalized_receipt"]["artifacts"][0]["kind"] =
        "metadata";
    expect_rejected("complete manifest must reject artifact kind/order substitution");
    options.recording_outputs[0] =
        make_spatial_roi_output("2010096", "2010096_spatial_roi_roi_1");
    options.recording_outputs[0].details["finalized_receipt"]["artifacts"][0]["size_bytes"] =
        0;
    expect_rejected("complete manifest must reject zero-sized receipts");
    options.recording_outputs[0] =
        make_spatial_roi_output("2010096", "2010096_spatial_roi_roi_1");
    options.recording_outputs[0].details["finalized_receipt"]["artifacts"][0]["sha256"] =
        "sha256:" + std::string(64, 'A');
    expect_rejected("complete manifest must reject non-canonical hashes");
    options.recording_outputs[0] =
        make_spatial_roi_output("2010096", "2010096_spatial_roi_roi_1");
    options.recording_outputs[0].details["finalized_receipt"]["artifacts"].erase(11);
    expect_rejected("complete manifest must reject omitted artifact receipts");
    options.recording_outputs[0] =
        make_spatial_roi_output("2010096", "2010096_spatial_roi_roi_1");
    options.recording_outputs[0].details["finalized_receipt"]["counts"]
        ["dispatch_rejected"] = 1;
    expect_rejected("complete manifest must reject a failed admission count");
    options.recording_outputs[0] =
        make_spatial_roi_output("2010096", "2010096_spatial_roi_roi_1");
    options.recording_outputs[0].details["finalized_receipt"]["counts"]
        ["encoded_frames"] = 9;
    expect_rejected("complete manifest must reject incomplete encode counts");
    options.recording_outputs[0] =
        make_spatial_roi_output("2010096", "2010096_spatial_roi_roi_1");
    options.recording_outputs[0].details["finalized_receipt"]["counts"]
        ["encoded_bytes"] = 0;
    expect_rejected("complete manifest must reject an empty encoded payload");
    options.recording_outputs[0] =
        make_spatial_roi_output("2010096", "2010096_spatial_roi_roi_1");
    options.recording_outputs[0].details["finalized_receipt"]["ranges"]
        ["roi_stream_frame_index"]["first"] = 0;
    expect_rejected("complete manifest must reject a non-one-based ROI range");
    options.recording_outputs[0] =
        make_spatial_roi_output("2010096", "2010096_spatial_roi_roi_1");
    options.recording_outputs[0].details["finalized_receipt"]["ranges"]
        ["recording_frame_id"]["last"] = 11;
    expect_rejected("complete manifest must reject a sparse recording-frame range");
    options.recording_outputs[0] =
        make_spatial_roi_output("2010096", "2010096_spatial_roi_roi_1");
    options.recording_outputs[0].frame_count = 9;
    expect_rejected("complete manifest must reject descriptor/receipt count disagreement");
    options.recording_outputs[0] =
        make_spatial_roi_output("2010096", "2010096_spatial_roi_roi_1");
    options.recording_outputs[0].first_recording_frame_id = 2;
    options.recording_outputs[0].last_recording_frame_id = 11;
    options.recording_outputs[0].details["finalized_receipt"]["ranges"]
        ["recording_frame_id"] = {{"first", 2}, {"last", 11}};
    const nlohmann::json coverage_downgraded =
        orange::session::build_single_clip_recording_session_manifest(options);
    require(!coverage_downgraded.empty() &&
                coverage_downgraded.at("status") == "incomplete",
            "full-frame/ROI coverage disagreement must downgrade aggregate completion while preserving product evidence");

    options.recording_outputs.clear();
    options.recording_outputs.push_back(
        make_spatial_roi_output("2010096", "2010096_spatial_roi_roi_1"));
    options.recording_outputs.push_back(
        make_spatial_roi_output("2010096", "2010096_spatial_roi_roi_2"));
    options.recording_outputs[1].first_recording_frame_id = 2;
    options.recording_outputs[1].last_recording_frame_id = 11;
    options.recording_outputs[1].details["finalized_receipt"]["ranges"]
        ["recording_frame_id"] = {{"first", 2}, {"last", 11}};
    expect_rejected(
        "complete manifest must reject partitioned ROI lane coverage");
}

void test_pending_manifest_retains_path_only_spatial_roi_bindings()
{
    orange::session::SingleClipRecordingSessionManifestOptions options;
    options.producer = "orange_spatial_roi_pending_test";
    options.session_id = "session_spatial_roi_pending";
    options.recording_folder = "/tmp/orange_session_manifest_spatial_roi_pending";
    options.status = "recording";
    options.cameras.push_back(make_camera_artifact("2010096", 10));
    options.recording_outputs.push_back(
        make_spatial_roi_output("2010096", "2010096_spatial_roi_roi_1"));
    options.recording_outputs[0].status = "pending";
    options.recording_outputs[0].details.erase("finalized_receipt");

    const nlohmann::json manifest =
        orange::session::build_single_clip_recording_session_manifest(options);
    const auto& evidence = manifest.at("spatial_roi_recording_evidence");
    require(evidence.at("status") == "pending" &&
                evidence.at("verification").at("status") == "not_validated" &&
                evidence.at("verification").at("receipt_promotion_required") &&
                evidence.at("streams").at("2010096")
                    .at("2010096_spatial_roi_roi_1")
                    .at("artifact_bindings").at("video")
                    .at("verification_status") == "declared_expected_path",
            "pending manifest must retain path-only ROI bindings");
}

void test_manifest_rejects_unbound_spatial_roi_evidence()
{
    orange::session::SingleClipRecordingSessionManifestOptions options;
    options.producer = "orange_spatial_roi_test";
    options.session_id = "session_spatial_roi_unbound";
    options.recording_folder = "/tmp/orange_session_manifest_spatial_roi_unbound";
    options.status = "completed";
    options.cameras.push_back(make_camera_artifact("2010096", 10));
    options.recording_outputs.push_back(
        make_spatial_roi_output("2010096", "2010096_spatial_roi_roi_1"));
    options.recording_outputs.back().details.erase("frame_identity");

    const nlohmann::json manifest =
        orange::session::build_single_clip_recording_session_manifest(options);
    require(manifest.empty(),
            "manifest builder must fail closed when ROI evidence is unbound");

    options.recording_outputs.back() =
        make_spatial_roi_output("2010096", "2010096_spatial_roi_roi_1");
    options.recording_outputs.back().details["artifacts"]["video"] =
        "/tmp/absolute_roi.mp4";
    require(orange::session::build_single_clip_recording_session_manifest(options)
                .empty(),
            "manifest builder must reject absolute ROI evidence paths");

    options.recording_outputs.back() =
        make_spatial_roi_output("2010096", "2010096_spatial_roi_roi_1");
    options.recording_outputs.back().details["camera_serial"] = "other-camera";
    require(orange::session::build_single_clip_recording_session_manifest(options)
                .empty(),
            "manifest builder must reject mismatched ROI evidence identity");
}

void test_manifest_full_frame_completion_gate()
{
    orange::session::SingleClipRecordingSessionManifestOptions options;
    options.producer = "orange_full_frame_gate_test";
    options.session_id = "session_full_frame_gate";
    options.recording_folder = "/tmp/orange_session_manifest_full_frame_gate";
    options.status = "incomplete";
    options.recording_started = false;
    options.recording_drain_completed = true;
    options.recording_backend = {
        {"mode", "in_process"},
        {"full_frame", {{"status", "finalized"}, {"first_class", true}}},
        {"spatial_roi_recording", {{"status", "failed"}}}
    };
    options.cameras.push_back(make_camera_artifact("2010096", 0));
    options.recording_outputs.push_back(
        make_spatial_roi_output(
            "2010096", "2010096_spatial_roi_roi_1"));
    options.recording_outputs.back().status = "failed";
    options.recording_outputs.back().details.erase("finalized_receipt");

    const nlohmann::json manifest =
        orange::session::build_single_clip_recording_session_manifest(options);
    require(manifest.at("recording_backend").at("full_frame").at("status") ==
                "failed" &&
                manifest.at("recording_backend").at("full_frame").at(
                    "completion_gate") == "failed" &&
                manifest.at("recording_backend").at("full_frame").at(
                    "recording_started") == false,
            "full-frame backend must not claim finalized without the start gate");

    options.status = "completed";
    options.recording_started = true;
    options.recording_drain_completed = true;
    options.recording_backend["full_frame"]["status"] = "pending";
    options.cameras[0] = make_camera_artifact("2010096", 10);
    const nlohmann::json completed_manifest =
        orange::session::build_single_clip_recording_session_manifest(options);
    require(completed_manifest.at("recording_backend").at("full_frame").at(
                "status") == "finalized" &&
                completed_manifest.at("recording_backend").at("full_frame").at(
                    "completion_gate") == "passed",
            "full-frame backend should finalize only after both gates pass");

    // ROI failure belongs to the aggregate lifecycle only. Once the full
    // descriptor has its own frame/packet evidence and drained successfully,
    // it remains a valid first-class product.
    options.status = "incomplete";
    options.recording_backend["full_frame"]["status"] = "finalized";
    const nlohmann::json roi_failed_manifest =
        orange::session::build_single_clip_recording_session_manifest(options);
    require(roi_failed_manifest.at("recording_backend").at("full_frame").at(
                "status") == "finalized" &&
                roi_failed_manifest.at("recording_backend").at("full_frame").at(
                    "completion_gate") == "passed" &&
                roi_failed_manifest.at("recording_outputs_v3")
                    .at("cameras").at("2010096").at("full")
                    .at("status") == "finalized" &&
                roi_failed_manifest.at("recording_outputs_v3")
                    .at("cameras").at("2010096").at("spatial_roi")
                    .at("2010096_spatial_roi_roi_1").at("status") ==
                    "failed",
            "ROI failure must not demote an independently valid full-frame product");
}

void test_full_frame_completion_gate_is_spatial_roi_scoped()
{
    orange::session::SingleClipRecordingSessionManifestOptions options;
    options.producer = "orange_non_roi_compatibility_test";
    options.session_id = "session_non_roi_compatibility";
    options.recording_folder = "/tmp/orange_session_non_roi_compatibility";
    options.status = "completed";
    options.recording_started = false;
    options.recording_drain_completed = false;
    options.recording_backend = {
        {"mode", "in_process"},
        {"full_frame", {{"status", "pending"}, {"first_class", true}}}
    };
    options.cameras.push_back(make_camera_artifact("2010096", 10));

    const nlohmann::json manifest =
        orange::session::build_single_clip_recording_session_manifest(options);
    require(manifest.at("recording_backend") == options.recording_backend,
            "absent spatial ROI must not rewrite the established full-frame backend payload");
    require(!manifest.at("recording_backend").at("full_frame").contains(
                "completion_gate"),
            "absent spatial ROI must not add a new full-frame completion contract");
}

void test_rolling_manifest_gates_clip_full_frame_status_independently()
{
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() /
        ("orange_session_manifest_rolling_full_frame_gate_" +
         std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);

    orange::session::RollingRecordingSessionManifestOptions options;
    options.producer = "orange_rolling_full_frame_gate_test";
    options.session_id = "session_rolling_full_frame_gate";
    options.recording_folder = folder.string();
    options.status = "incomplete";
    options.recording_started = true;
    options.recording_drain_completed = true;
    options.recording_backend = {
        {"mode", "in_process"},
        {"full_frame", {{"status", "pending"}, {"first_class", true}}}
    };
    options.camera_serials.push_back("2010096");

    orange::session::RollingClipManifestOptions clip;
    clip.producer = options.producer;
    clip.output_backend = "in_process";
    clip.session_id = options.session_id;
    clip.clip_index = 0;
    clip.clip_id = "clip_000000";
    clip.recording_folder = options.recording_folder;
    clip.directory = "clips/clip_000000";
    // The clip aggregate can be incomplete because an independent ROI
    // stream failed, while its full-frame evidence remains valid.
    clip.status = "incomplete";
    clip.drain_completed = true;
    clip.cameras.push_back(make_camera_artifact("2010096", 10));
    clip.cameras[0].first_recording_frame_id = 101;
    clip.cameras[0].last_recording_frame_id = 110;
    materialize_full_frame_artifacts(folder, clip.cameras[0]);
    write_metadata_csv(folder / "Cam2010096_meta.csv",
                       {101, 102, 103, 104, 105, 106, 107, 108, 109, 110});
    orange::session::RecordingOutputDescriptor roi =
        make_spatial_roi_output("2010096", "2010096_spatial_roi_roi_1");
    roi.status = "failed";
    roi.details.erase("finalized_receipt");
    clip.recording_outputs.push_back(roi);
    // The session-level collection can legitimately contain only ROI/crop
    // products while full-frame authority remains clip-scoped. Its mere
    // non-emptiness must not suppress the per-clip full-frame evidence gate.
    options.recording_outputs.push_back(std::move(roi));
    options.clips.push_back(clip);

    const nlohmann::json manifest =
        orange::session::build_rolling_clip_recording_session_manifest(options);
    require(manifest.at("recording_backend").at("full_frame").at("status") ==
                "finalized" &&
                manifest.at("recording_backend").at("full_frame").at(
                    "completion_gate") == "passed",
            "rolling full-frame backend should use independent clip evidence");
    const nlohmann::json& clip_v3 =
        manifest.at("clips")[0].at("recording_outputs_v3");
    require(clip_v3.at("cameras").at("2010096").at("full").at("status") ==
                "finalized" &&
                manifest.at("clips")[0].at("status") == "incomplete",
            "rolling v3 full-frame status must not copy an incomplete clip status");

    options.clips[0].cameras[0].recording_frame_id_gaps = 1;
    const nlohmann::json gapped_manifest =
        orange::session::build_rolling_clip_recording_session_manifest(options);
    require(gapped_manifest.at("recording_backend").at("full_frame").at(
                "status") == "failed" &&
                gapped_manifest.at("clips")[0].at("recording_outputs_v3")
                        .at("cameras").at("2010096").at("full").at("status") ==
                    "failed",
                "rolling full-frame status must fail closed for clip frame gaps");

    options.clips[0].cameras[0].recording_frame_id_gaps = 0;
    options.clips[0].drain_completed = false;
    const nlohmann::json undrained_manifest =
        orange::session::build_rolling_clip_recording_session_manifest(options);
    require(undrained_manifest.at("recording_backend").at("full_frame").at(
                "status") == "failed" &&
                undrained_manifest.at("clips")[0].at("recording_outputs_v3")
                        .at("cameras").at("2010096").at("full").at("status") ==
                    "failed",
            "rolling full-frame status must fail closed for an undrained clip");

    std::filesystem::remove_all(folder);
}

void test_manifest_full_frame_gate_rejects_unsealed_metadata_evidence()
{
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() /
        ("orange_session_full_frame_metadata_gate_" +
         std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);

    orange::session::SingleClipRecordingSessionManifestOptions options;
    options.producer = "orange_full_frame_metadata_gate_test";
    options.session_id = "session_full_frame_metadata_gate";
    options.recording_folder = folder.string();
    options.status = "completed";
    options.recording_started = true;
    options.recording_drain_completed = true;
    options.recording_backend = {
        {"mode", "external_ipc"},
        {"full_frame", {{"status", "pending"}}},
        {"spatial_roi_recording", {{"status", "failed"}}}
    };
    options.cameras.push_back(make_camera_artifact("2010096", 2));
    materialize_full_frame_artifacts(folder, options.cameras[0]);
    options.recording_outputs.push_back(
        make_spatial_roi_output(
            "2010096", "2010096_spatial_roi_roi_1"));
    options.recording_outputs.back().status = "failed";
    options.recording_outputs.back().details.erase("finalized_receipt");
    const std::filesystem::path metadata_path =
        folder / "Cam2010096_meta.csv";

    const auto write_metadata = [&](const std::string& bytes) {
        std::ofstream output(metadata_path, std::ios::binary);
        require(static_cast<bool>(output), "failed to open metadata gate fixture");
        output << bytes;
        require(static_cast<bool>(output), "failed to write metadata gate fixture");
    };
    const std::string valid_header =
        "frame_id,timestamp,timestamp_sys,recording_frame_id\n";
    const std::vector<std::pair<std::string, std::string>> invalid_metadata = {
        {"malformed_header", "frame_id,,timestamp_sys,recording_frame_id\n"
                              "1,1000,2000,1\n2,1001,2001,2\n"},
        {"malformed_row", valid_header + "1,1000,2000,1\n"
                           "not-a-frame,1001,2001,2\n"},
        {"alias_mismatch", valid_header + "1,1000,2000,1\n"
                            "9,1001,2001,2\n"},
        {"frame_gap", valid_header + "1,1000,2000,1\n"
                       "3,1001,2001,3\n"},
        {"descriptor_mapping_mismatch", valid_header + "1,1000,2000,1\n"
                                         "2,1001,2001,2\n"}
    };

    for (const auto& [name, bytes] : invalid_metadata) {
        write_metadata(bytes);
        if (name == "descriptor_mapping_mismatch") {
            options.cameras[0].frame_count = 3;
            options.cameras[0].last_recording_frame_id = 3;
        } else {
            options.cameras[0].frame_count = 2;
            options.cameras[0].last_recording_frame_id = 2;
        }
        const nlohmann::json manifest =
            orange::session::build_single_clip_recording_session_manifest(options);
        require(manifest.at("recording_backend").at("full_frame").at("status") ==
                    "failed" &&
                    manifest.at("recording_backend").at("full_frame").at(
                        "completion_gate") == "failed",
                "full-frame gate should reject " + name + " metadata evidence");
    }

    write_metadata("frame_id,timestamp,timestamp_sys\n"
                   "1,1000,2000\n2,1001,2001\n");
    options.cameras[0].frame_count = 2;
    options.cameras[0].last_recording_frame_id = 2;
    const nlohmann::json valid_manifest =
        orange::session::build_single_clip_recording_session_manifest(options);
    require(valid_manifest.at("recording_backend").at("full_frame").at("status") ==
                "finalized" &&
                valid_manifest.at("recording_backend").at("full_frame").at(
                    "completion_gate") == "passed",
            "full-frame gate should preserve the current in-process frame_id metadata evidence");

    std::filesystem::remove(folder / options.cameras[0].video_path);
    const nlohmann::json missing_video_manifest =
        orange::session::build_single_clip_recording_session_manifest(options);
    require(missing_video_manifest.at("recording_backend").at("full_frame").at(
                "status") == "failed",
            "full-frame gate must reject a missing declared video artifact");
    write_nonempty_artifact(folder / options.cameras[0].video_path, "");
    const nlohmann::json empty_video_manifest =
        orange::session::build_single_clip_recording_session_manifest(options);
    require(empty_video_manifest.at("recording_backend").at("full_frame").at(
                "status") == "failed",
            "full-frame gate must reject an empty declared video artifact");
    write_nonempty_artifact(folder / options.cameras[0].video_path,
                            "video-fixture\n");

    std::filesystem::remove(folder / options.cameras[0].keyframe_path);
    std::filesystem::create_directory(folder / options.cameras[0].keyframe_path);
    const nlohmann::json nonregular_keyframe_manifest =
        orange::session::build_single_clip_recording_session_manifest(options);
    require(nonregular_keyframe_manifest.at("recording_backend").at("full_frame").at(
                "status") == "failed",
            "full-frame gate must reject a non-regular keyframe artifact");
    std::filesystem::remove_all(folder / options.cameras[0].keyframe_path);
    write_nonempty_artifact(folder / options.cameras[0].keyframe_path,
                            "{\"keyframes\":[1]}\n");

    options.cameras[0].packet_count = 1;
    const nlohmann::json truncated_packet_manifest =
        orange::session::build_single_clip_recording_session_manifest(options);
    require(truncated_packet_manifest.at("recording_backend").at("full_frame").at(
                "status") == "failed",
            "full-frame gate must reject packet/frame cardinality disagreement");
    options.cameras[0].packet_count = 2;

    std::filesystem::remove(folder / options.cameras[0].keyframe_path);
    std::filesystem::create_hard_link(
        folder / options.cameras[0].video_path,
        folder / options.cameras[0].keyframe_path);
    const nlohmann::json aliased_artifacts_manifest =
        orange::session::build_single_clip_recording_session_manifest(options);
    require(aliased_artifacts_manifest.at("recording_backend").at("full_frame").at(
                "status") == "failed",
            "full-frame gate must reject artifact paths that alias one inode");

    std::filesystem::remove_all(folder);
}

void test_complete_spatial_roi_aggregate_requires_full_metadata_evidence()
{
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() /
        ("orange_session_spatial_roi_aggregate_metadata_gate_" +
         std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);

    orange::session::SingleClipRecordingSessionManifestOptions options;
    options.producer = "orange_spatial_roi_aggregate_metadata_gate_test";
    options.session_id = "session_spatial_roi";
    options.recording_folder = folder.string();
    options.status = "completed";
    options.recording_started = true;
    options.recording_drain_completed = true;
    options.recording_backend = {
        {"mode", "external_ipc"},
        {"full_frame", {{"status", "pending"}, {"first_class", true}}},
        {"spatial_roi_recording", {{"status", "complete"}}},
    };
    options.cameras.push_back(make_camera_artifact("2010096", 10));
    materialize_full_frame_artifacts(folder, options.cameras[0]);
    options.recording_outputs.push_back(
        make_spatial_roi_output(
            "2010096", "2010096_spatial_roi_roi_1"));

    const auto require_aggregate_incomplete = [&](const std::string& reason) {
        const nlohmann::json manifest =
            orange::session::build_single_clip_recording_session_manifest(options);
        require(!manifest.empty() && manifest.at("status") == "incomplete",
                reason + " must make aggregate completion incomplete");
        const nlohmann::json& camera = manifest.at("recording_outputs_v3")
            .at("cameras").at("2010096");
        require(camera.at("full").at("status") == "failed" &&
                    camera.at("spatial_roi")
                        .at("2010096_spatial_roi_roi_1")
                        .at("status") == "completed",
                reason + " must preserve truthful independent product states");
    };

    require_aggregate_incomplete("missing full-frame metadata");

    {
        std::ofstream output(folder / "Cam2010096_meta.csv", std::ios::binary);
        require(static_cast<bool>(output), "failed to open malformed metadata fixture");
        output << "frame_id,recording_frame_id\n1,1\n2,3\n";
    }
    require_aggregate_incomplete("contradictory full-frame metadata");

    {
        std::ofstream output(folder / "Cam2010096_meta.csv", std::ios::binary);
        require(static_cast<bool>(output),
                "failed to open current in-process metadata fixture");
        output << "frame_id,timestamp,timestamp_sys\n";
        for (std::uint64_t frame_id = 1; frame_id <= 10; ++frame_id) {
            output << frame_id << ",1000,2000\n";
        }
        require(static_cast<bool>(output),
                "failed to write current in-process metadata fixture");
    }
    const nlohmann::json complete =
        orange::session::build_single_clip_recording_session_manifest(options);
    require(!complete.empty() && complete.at("status") == "completed" &&
                complete.at("recording_outputs_v3")
                    .at("cameras").at("2010096")
                    .at("full").at("status") == "finalized",
            "current in-process dense full metadata and complete ROI evidence must permit aggregate completion");

    std::filesystem::remove_all(folder);
}

void test_fixed_roi_only_manifest_omits_full_frame_product_by_policy()
{
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() /
        ("orange_session_fixed_roi_only_identity_" +
         std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);

    orange::session::SingleClipRecordingSessionManifestOptions options;
    options.producer = "orange_fixed_roi_only_test";
    options.session_id = "session_spatial_roi";
    options.recording_folder = folder.string();
    options.status = "completed";
    options.recording_started = true;
    options.recording_drain_completed = true;
    options.include_full_frame_product = false;
    options.recording_backend = {
        {"mode", "fixed_roi_external_ipc"},
        {"media_policy", "fixed_rois_with_registered_context"},
        {"full_frame",
         {{"status", "omitted_by_policy"},
          {"required", false},
          {"continuous", false}}},
        {"registered_context", {{"status", "finalized"}}},
        {"spatial_roi_recording", {{"status", "complete"}}},
    };
    options.cameras.push_back(make_camera_artifact("2010096", 10));
    options.recording_outputs.push_back(make_spatial_roi_output(
        "2010096", "2010096_spatial_roi_roi_1"));
    options.recording_outputs.push_back(make_spatial_roi_output(
        "2010096", "2010096_spatial_roi_roi_2"));
    options.recording_outputs.push_back(make_spatial_roi_output(
        "2010096", "2010096_spatial_roi_roi_3"));
    options.recording_outputs.push_back(make_spatial_roi_output(
        "2010096", "2010096_spatial_roi_roi_4"));

    const nlohmann::json manifest =
        orange::session::build_single_clip_recording_session_manifest(options);
    require(!manifest.empty() && manifest.at("status") == "completed",
            "an explicit fixed-ROI-only policy must complete without a synthetic full-frame descriptor");
    require(manifest.at("cameras").size() == 1 &&
                manifest.at("cameras").at(0) == "2010096",
            "ROI-only manifest must preserve the camera identity");
    require(manifest.at("camera_artifacts").empty() &&
                manifest.at("clips").at(0).at("artifacts").at("videos").empty(),
            "ROI-only manifest must not mint absent continuous full-frame artifact paths");
    const nlohmann::json& camera_outputs =
        manifest.at("recording_outputs_v3").at("cameras").at("2010096");
    require(!camera_outputs.contains("full") &&
                camera_outputs.at("spatial_roi").size() == 4,
            "ROI-only output index must contain only the retained fixed ROI streams");
    require(manifest.at("recording_backend").at("full_frame").at("status") ==
                "omitted_by_policy" &&
                !manifest.at("recording_backend").at("full_frame").contains(
                    "completion_gate"),
            "manifest builder must preserve explicit full-frame omission semantics");

    options.recording_outputs.back().last_recording_frame_id = 9;
    const nlohmann::json mismatched =
        orange::session::build_single_clip_recording_session_manifest(options);
    require(mismatched.empty() || mismatched.at("status") == "incomplete",
            "ROI-only aggregate completion must reject unequal ROI frame coverage");

    options.recording_outputs.back().last_recording_frame_id = 10;
    const nlohmann::json complete_manifest =
        orange::session::build_single_clip_recording_session_manifest(options);
    const std::filesystem::path manifest_path =
        folder / "recording_session.json";
    std::string error;
    require(
        orange::session::write_recording_session_manifest(
            manifest_path.string(), complete_manifest, &error),
        "fixed-ROI-only manifest finalization should succeed: " + error);
    const nlohmann::json written = read_json(manifest_path);
    require(written.at("status") == "completed",
            "fixed-ROI-only finalization must preserve completed session status");
    require(written.at("camera_artifacts").empty() &&
                written.at("clips").at(0).at("artifacts").at("videos").empty() &&
                written.at("clips").at(0).at("artifacts").at("metadata").empty() &&
                !std::filesystem::exists(folder / "Cam2010096.mp4") &&
                !std::filesystem::exists(folder / "Cam2010096_meta.csv"),
            "ROI-only finalization must not mint fake full-frame media or metadata");

    const nlohmann::json& frame_identity =
        written.at("frame_identity_contract");
    require(frame_identity.at("status") == "finalized" &&
                frame_identity.at("verification").at("result") == "passed" &&
                frame_identity.at("verification").at("authority") ==
                    "authenticated_spatial_roi_finalized_receipts" &&
                frame_identity.at("camera_streams").at("2010096").at(
                    "backend") == "fixed_roi_external_ipc" &&
                frame_identity.at("camera_streams").at("2010096").at(
                    "verification_status") == "passed" &&
                frame_identity.at("camera_streams").at("2010096").at(
                    "stream_proofs").size() == 4,
            "frame identity must use authenticated fixed-ROI receipts, not an in-process full-frame producer claim");

    const nlohmann::json& historical_mapping =
        written.at("acquisition_index_mapping");
    require(historical_mapping.at("status") ==
                "not_applicable_by_media_policy" &&
                historical_mapping.at("reason_code") ==
                    "continuous_full_frame_product_omitted_by_media_policy" &&
                historical_mapping.at("camera_streams").empty() &&
                !written.contains("acquisition_index_mapping_sha256"),
            "the historical metadata-backed mapping must be explicitly not applicable and unsealed");

    const nlohmann::json& roi_mapping =
        written.at("spatial_roi_acquisition_index_mapping");
    const nlohmann::json& roi_camera =
        roi_mapping.at("camera_streams").at("2010096");
    require(roi_mapping.at("schema_id") ==
                "orange.spatial_roi_recording.acquisition_index_mapping" &&
                roi_mapping.at("schema_version") == 1 &&
                roi_mapping.at("status") == "finalized" &&
                roi_mapping.at("source_evidence_index_ref") ==
                    "#/spatial_roi_recording_evidence" &&
                roi_mapping.at("source_evidence_index_sha256") ==
                    canonical_semantic_sha256(
                        written.at("spatial_roi_recording_evidence")) &&
                written.at("spatial_roi_acquisition_index_mapping_sha256") ==
                    canonical_semantic_sha256(roi_mapping),
            "ROI-derived acquisition mapping must be a closed, evidence-bound, digested finalized record");
    require(roi_camera.at("coverage").at("first_recording_frame_id") == 1 &&
                roi_camera.at("coverage").at("last_recording_frame_id") == 10 &&
                roi_camera.at("coverage").at("total_acquisitions") == 10 &&
                roi_camera.at("coverage").at("gap_count") == 0 &&
                roi_camera.at("source_authority").at("kind") ==
                    "authenticated_spatial_roi_finalized_receipts" &&
                roi_camera.at("source_authority").at(
                    "finalized_receipts").size() == 4 &&
                roi_camera.at("conversion").at("constant") == 1,
            "ROI acquisition mapping must derive one shared dense camera range from all four retained streams");

    const nlohmann::json frozen_roi_mapping = roi_mapping;
    const std::string frozen_roi_mapping_digest =
        written.at("spatial_roi_acquisition_index_mapping_sha256")
            .get<std::string>();
    error.clear();
    require(
        orange::session::write_recording_session_manifest(
            manifest_path.string(), written, &error),
        "rewriting a sealed ROI-only manifest should succeed: " + error);
    const nlohmann::json rewritten = read_json(manifest_path);
    require(rewritten.at("spatial_roi_acquisition_index_mapping") ==
                frozen_roi_mapping &&
                rewritten.at("spatial_roi_acquisition_index_mapping_sha256") ==
                    frozen_roi_mapping_digest,
            "finalized ROI-derived acquisition mapping evidence must remain immutable on rewrite");

    std::filesystem::remove_all(folder);
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
        {"single_clip_manifest_indexes_multiple_spatial_roi_streams",
         test_single_clip_manifest_indexes_multiple_spatial_roi_streams},
        {"single_clip_manifest_accepts_gop25_keyframe_cadence",
         test_single_clip_manifest_accepts_gop25_keyframe_cadence},
        {"complete_manifest_rejects_invalid_spatial_roi_receipt",
         test_complete_manifest_rejects_invalid_spatial_roi_receipt},
        {"pending_manifest_retains_path_only_spatial_roi_bindings",
         test_pending_manifest_retains_path_only_spatial_roi_bindings},
        {"manifest_rejects_unbound_spatial_roi_evidence",
         test_manifest_rejects_unbound_spatial_roi_evidence},
        {"manifest_full_frame_completion_gate",
         test_manifest_full_frame_completion_gate},
        {"full_frame_completion_gate_is_spatial_roi_scoped",
         test_full_frame_completion_gate_is_spatial_roi_scoped},
        {"rolling_manifest_gates_clip_full_frame_status_independently",
         test_rolling_manifest_gates_clip_full_frame_status_independently},
        {"manifest_full_frame_gate_rejects_unsealed_metadata_evidence",
         test_manifest_full_frame_gate_rejects_unsealed_metadata_evidence},
        {"complete_spatial_roi_aggregate_requires_full_metadata_evidence",
         test_complete_spatial_roi_aggregate_requires_full_metadata_evidence},
        {"fixed_roi_only_manifest_omits_full_frame_product_by_policy",
         test_fixed_roi_only_manifest_omits_full_frame_product_by_policy},
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
