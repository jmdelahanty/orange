#include "session/spatial_roi_session_snapshot.h"

#include "shaman_v2_recording_identity.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using orange::session::spatial_roi::SpatialRoiRecorderCameraContractView;
using orange::session::spatial_roi::SpatialRoiRecorderStreamView;
using orange::session::spatial_roi::SpatialRoiSessionArtifactReference;

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string digest(const char fill)
{
    return "sha256:" + std::string(64, fill);
}

nlohmann::json make_complete_recorder_process_status()
{
    const nlohmann::json preflight = {
        {"schema_id", "orange.spatial_roi_recording.storage_preflight"},
        {"schema_version", 1},
        {"checked", true},
        {"passed", true},
        {"status", "passed"},
        {"error", ""},
        {"policy", {
            {"schema_id", "orange.spatial_roi_recorder_storage_preflight_policy"},
            {"schema_version", 1},
            {"required", true},
            {"reserved_free_bytes", 10}}},
        {"artifact_root", {{"device", 1}, {"inode", 3}}},
        {"filesystem", {
            {"block_size_bytes", 1},
            {"total_blocks", 100000},
            {"available_blocks", 100000},
            {"capacity_bytes", 100000},
            {"available_bytes", 100000}}},
        {"budgets", {
            {"max_media_bytes_total", 1000},
            {"max_evidence_bytes_total", 5000},
            {"reserved_free_bytes", 10},
            {"required_bytes", 6010}}}};
    const nlohmann::json empty_child = {
        {"event", ""},
        {"status", ""},
        {"state", ""},
        {"ready", false},
        {"clean_eof", false},
        {"completed", false},
        {"failed", false},
        {"first_failure_stream_id", ""},
        {"first_failure", ""},
        {"error", ""},
        {"payload", nlohmann::json::object()}};
    const nlohmann::json child = {
        {"event", "ready"},
        {"status", "ready"},
        {"state", "ready"},
        {"ready", true},
        {"clean_eof", false},
        {"completed", false},
        {"failed", false},
        {"first_failure_stream_id", ""},
        {"first_failure", ""},
        {"error", ""},
        {"payload", {
            {"event", "ready"},
            {"status", "ready"},
            {"state", "ready"},
            {"ready", true},
            {"clean_eof", false},
            {"completed", false},
            {"failed", false},
            {"first_failure_stream_id", ""},
            {"first_failure", ""},
            {"error", ""},
            {"storage_preflight", preflight}}}};
    const nlohmann::json terminal = {
        {"event", "terminal"},
        {"status", "complete"},
        {"state", "completed"},
        {"ready", true},
        {"clean_eof", true},
        {"completed", true},
        {"failed", false},
        {"first_failure_stream_id", ""},
        {"first_failure", ""},
        {"error", ""},
        {"payload", {
            {"event", "terminal"},
            {"status", "complete"},
            {"state", "completed"},
            {"ready", true},
            {"clean_eof", true},
            {"completed", true},
            {"failed", false},
            {"first_failure_stream_id", ""},
            {"first_failure", ""},
            {"error", ""},
            {"storage_preflight", preflight}}}};
    return {
        {"schema_id", "orange.spatial_roi_recording.headless_process_status"},
        {"schema_version", 1},
        {"session_state", "finished"},
        {"process_state", "exited"},
        {"pid", 1234},
        {"started", true},
        {"sockets_bound", true},
        {"ready", true},
        {"terminal_seen", true},
        {"exited", true},
        {"reaped", true},
        {"exit_code", 0},
        {"term_signal", 0},
        {"stdout_bytes_read", 100},
        {"cleanup_complete", true},
        {"first_failure", ""},
        {"error", ""},
        {"starting", empty_child},
        {"ready_snapshot", child},
        {"heartbeat", empty_child},
        {"terminal", terminal},
        {"last", terminal}};
}

nlohmann::json make_complete_producer_status(
    const SpatialRoiRecorderCameraContractView& view,
    const std::uint64_t frame_count)
{
    return {
        {"schema_id", "orange.spatial_roi_recording.headless_producer_status"},
        {"schema_version", 1},
        {"state", "stopped"},
        {"recording_id", view.recording_id},
        {"session_id", view.session_id},
        {"recording_identity_token", view.recording_identity_token},
        {"producer_generation", view.producer_generation},
        {"spatial_roi_plan_sha256", view.spatial_roi_plan_sha256},
        {"camera_id", view.camera_id},
        {"camera_serial", view.camera_serial},
        {"stream_count", 4},
        {"submit_attempted", frame_count},
        {"submitted", frame_count},
        {"incomplete", 0},
        {"rejected", 0},
        {"acquisition_armed", false},
        {"first_failure", ""}};
}

SpatialRoiRecorderCameraContractView make_camera_contract()
{
    SpatialRoiRecorderCameraContractView view;
    view.schema_id =
        orange::session::spatial_roi::kSpatialRoiRecorderCameraContractSchemaId;
    view.schema_version =
        orange::session::spatial_roi::kSpatialRoiRecorderCameraContractSchemaVersion;
    view.product_kind =
        orange::session::spatial_roi::kSpatialRoiRecorderCameraProductKind;
    view.recording_id = "2026_09_01_00_00_00";
    view.session_id = view.recording_id;
    view.recording_identity_token =
        orange::shaman_v2_recording_identity::token_for_recording_id(
            view.recording_id);
    view.producer_generation = "generation_001";
    view.spatial_roi_plan_sha256 = digest('b');
    view.recording_root = "/tmp/spatial_roi_session_snapshot_recording";
    view.artifact_root = view.recording_root + "/external_spatial_roi_recorder";
    view.camera_id = 7;
    view.camera_serial = "2010096";
    view.native_raster = {640, 480};
    view.analytics_gpu_id = 2;
    view.stream_count = 4;
    view.analytics_gpu_by_camera_serial[view.camera_serial] =
        view.analytics_gpu_id;

    const std::array<const char*, 3> authority_ids = {
        "layout_1", "materialization_1", "registration_1"};
    const std::array<const char*, 12> artifact_kinds = {
        "video", "metadata", "keyframes", "perf", "summary", "status",
        "video_sanity", "finalization", "recorder_log", "transport_sidecar",
        "evidence", "evidence_manifest"};
    for (int index = 1; index <= 4; ++index) {
        SpatialRoiRecorderStreamView stream;
        stream.stream_id = view.camera_serial + "_spatial_roi_roi_" +
                           std::to_string(index);
        stream.logical_stream_id = stream.stream_id;
        view.stream_order.push_back(stream.logical_stream_id);
        stream.stream_kind = "spatial_roi";
        stream.output_kind = "spatial_roi";
        stream.camera_id = view.camera_id;
        stream.camera_serial = view.camera_serial;
        stream.analytics_gpu_id = view.analytics_gpu_id;
        stream.source_gpu_id = view.analytics_gpu_id;
        stream.recorder_gpu_id = index + 2;
        stream.assigned_gpu_id = stream.recorder_gpu_id;
        stream.expected_shard_gpu_ids = {stream.recorder_gpu_id};
        view.recorder_gpu_by_logical_stream_id[stream.logical_stream_id] =
            stream.recorder_gpu_id;
        stream.roi_id = "roi_" + std::to_string(index);
        stream.region_id = "region_" + std::to_string(index);
        stream.arena_group_id = "arena_group_1";
        stream.has_arena_id = index == 1;
        stream.arena_id = stream.has_arena_id ? "arena_1" : "";
        stream.recording_id = view.recording_id;
        stream.session_id = view.session_id;
        stream.recording_identity_token = view.recording_identity_token;
        stream.producer_generation = view.producer_generation;
        stream.spatial_roi_plan_sha256 = view.spatial_roi_plan_sha256;
        stream.geometry.layout = {authority_ids.at(0), digest('1')};
        stream.geometry.materialization = {authority_ids.at(1), digest('2')};
        stream.geometry.registration = {authority_ids.at(2), digest('3')};
        stream.geometry.native_raster = view.native_raster;
        stream.geometry.content_rect = {
            static_cast<std::uint32_t>((index - 1) * 32),
            static_cast<std::uint32_t>((index - 1) * 24),
            24,
            24};
        stream.geometry.encoded_raster = {26, 26};
        stream.geometry.encoded_content_rect = {0, 0, 24, 24};
        stream.geometry.padding = {0, 0, 2, 2, 0};
        stream.geometry.source_coordinate_space =
            "camera_native_full_frame_pixels";
        stream.geometry.video_coordinate_space =
            "spatial_roi_encoded_pixels";
        stream.encode_profile.profile_id =
            "hevc_p7_lossless_cqp0_gop1_v1";
        stream.encode_profile.codec = "hevc";
        stream.encode_profile.preset = "p7";
        stream.encode_profile.tuning = "lossless";
        stream.encode_profile.lossless = true;
        stream.encode_profile.rate_control_mode = "cqp";
        stream.encode_profile.quality_value = 0;
        stream.encode_profile.gop_length = 1;
        stream.encode_profile.frame_rate = 100;
        stream.encode_profile.input_format = "mono8";
        stream.encode_profile.encoded_format = "nv12";
        stream.encode_profile.no_resize = true;
        stream.encode_profile.luma_preserved_exactly = true;
        stream.encode_profile.neutral_chroma_value = 128;
        stream.encode_fps = 100;
        stream.codec = "hevc";
        stream.tuning = "lossless";
        for (const char* kind : artifact_kinds) {
            const std::string relative_path =
                stream.logical_stream_id + "/" + kind + ".artifact";
            stream.artifacts[kind] = {
                view.artifact_root + "/" + relative_path,
                relative_path};
        }
        view.streams.push_back(std::move(stream));
    }
    return view;
}

SpatialRoiRecorderCameraContractView make_gop25_camera_contract()
{
    SpatialRoiRecorderCameraContractView view = make_camera_contract();
    for (SpatialRoiRecorderStreamView& stream : view.streams) {
        stream.encode_profile.profile_id =
            "hevc_p1_low_latency_vbr_q20_gop25_v1";
        stream.encode_profile.codec = "hevc";
        stream.encode_profile.preset = "p1";
        stream.encode_profile.tuning = "ll";
        stream.encode_profile.lossless = false;
        stream.encode_profile.rate_control_mode = "vbr";
        stream.encode_profile.quality_value = 20;
        stream.encode_profile.gop_length = 25;
        stream.encode_profile.frame_rate = 100;
        stream.encode_profile.input_format = "mono8";
        stream.encode_profile.encoded_format = "nv12";
        stream.encode_profile.no_resize = true;
        stream.encode_profile.luma_preserved_exactly = false;
        stream.encode_profile.neutral_chroma_value = 128;
        stream.encode_fps = 100;
        stream.codec = "hevc";
        stream.tuning = "ll";
    }
    return view;
}

SpatialRoiSessionArtifactReference make_artifact(const std::string& path,
                                                 const std::uint64_t size,
                                                 const char fill)
{
    return {path, size, digest(fill)};
}

nlohmann::json make_finalized_receipt(
    const SpatialRoiRecorderCameraContractView& view,
    const std::uint64_t frame_count = 1)
{
    const std::array<const char*, 16> count_keys = {
        "detach_successes", "dispatch_admitted", "dispatch_rejected",
        "ack_attempted", "ack_sent", "ack_accepted", "release_attempted",
        "release_sent", "encoded_frames", "failed_frames", "packet_count",
        "encoded_bytes", "keyframes", "ack_write_failures",
        "release_write_failures", "lifecycle_failures"};
    const std::array<const char*, 12> artifact_kinds = {
        "video", "metadata", "keyframes", "perf", "summary", "status",
        "video_sanity", "finalization", "recorder_log", "transport_sidecar",
        "evidence", "evidence_manifest"};
    nlohmann::json streams = nlohmann::json::array();
    const std::uint64_t gop_length =
        view.streams.front().encode_profile.gop_length;
    const std::uint64_t expected_keyframes =
        frame_count == 0 || gop_length == 0
            ? 0
            : 1U + ((frame_count - 1U) / gop_length);
    for (const auto& stream : view.streams) {
        nlohmann::json counts = nlohmann::json::object();
        for (const char* key : count_keys) {
            const std::string name(key);
            counts[key] = (name == "dispatch_rejected" ||
                           name == "failed_frames" ||
                           name == "ack_write_failures" ||
                           name == "release_write_failures" ||
                           name == "lifecycle_failures")
                              ? static_cast<std::uint64_t>(0)
                              : (name == "keyframes"
                                     ? expected_keyframes
                                     : (name == "encoded_bytes"
                                            ? frame_count * 100U
                                            : frame_count));
        }
        const nlohmann::json ranges = {
            {"recording_frame_id", {{"first", 1}, {"last", frame_count}}},
            {"roi_stream_frame_index", {{"first", 1}, {"last", frame_count}}},
            {"has_frames", frame_count != 0},
            {"frame_count", frame_count}};
        nlohmann::json artifacts = nlohmann::json::array();
        for (std::size_t index = 0; index < artifact_kinds.size(); ++index) {
            const char* kind = artifact_kinds[index];
            const auto artifact = stream.artifacts.at(kind);
            artifacts.push_back({
                {"kind", kind},
                {"relative_path", artifact.relative_path},
                {"size_bytes", static_cast<std::uint64_t>(100 + index)},
                {"sha256", digest(static_cast<char>('a' + index % 6))}});
        }
        streams.push_back({
            {"logical_stream_id", stream.logical_stream_id},
            {"identity", {
                {"recording_id", view.recording_id},
                {"session_id", view.session_id},
                {"recording_identity_token", view.recording_identity_token},
                {"producer_generation", view.producer_generation},
                {"spatial_roi_plan_sha256", view.spatial_roi_plan_sha256},
                {"camera_id", view.camera_id},
                {"camera_serial", view.camera_serial},
                {"roi_id", stream.roi_id},
                {"region_id", stream.region_id},
                {"arena_group_id", stream.arena_group_id},
                {"logical_stream_id", stream.logical_stream_id},
                {"assigned_gpu_id", stream.assigned_gpu_id},
                {"assigned_shard_id", 0}}},
            {"counts", counts},
            {"ranges", ranges},
            {"finalized_receipt_digest", digest('f')},
            {"artifacts", artifacts}});
    }
    return {
        {"schema_id", "orange.spatial_roi_recording.finalized_session_receipt"},
        {"schema_version", 1},
        {"canonicalization", "canonical_json_utf8_sort_keys_compact_v1"},
        {"stream_kind", "fixed_region"},
        {"status", "complete"},
        {"stream_count", 4},
        {"stream_order", view.stream_order},
        {"identity", {
            {"recording_id", view.recording_id},
            {"session_id", view.session_id},
            {"recording_identity_token", view.recording_identity_token},
            {"producer_generation", view.producer_generation},
            {"spatial_roi_plan_sha256", view.spatial_roi_plan_sha256},
        {"camera_id", view.camera_id},
        {"camera_serial", view.camera_serial},
            {"stream_count", 4},
            {"stream_order", view.stream_order}}},
        {"root_authority", {
            {"artifact_root_relative", "external_spatial_roi_recorder"},
            {"recording_root_identity", {{"device", 1}, {"inode", 2}}},
            {"artifact_root_identity", {{"device", 1}, {"inode", 3}}},
            {"root_continuity", {
                {"proven", {"opened recording root"}},
                {"not_proven", {"historical continuity"}}}}}},
        {"streams", streams}};
}

void test_builds_closed_plan_ordered_snapshot()
{
    const SpatialRoiRecorderCameraContractView view = make_camera_contract();
    const SpatialRoiSessionArtifactReference config =
        make_artifact("session/spatial_roi_config.json", 101, 'c');
    const SpatialRoiSessionArtifactReference plan =
        make_artifact("session/spatial_roi_plan.json", 202, 'd');
    const SpatialRoiSessionArtifactReference contract =
        make_artifact("session/spatial_roi_contract.json", 303, 'e');
    const nlohmann::json recorder_status = {
        {"state", "ready"}, {"pid", 1234}};
    const nlohmann::json producer_status = {
        {"state", "armed"}, {"accepted_frames", 0}};
    nlohmann::json snapshot;
    std::string error;
    require(orange::session::spatial_roi::
                build_spatial_roi_session_snapshot_json(
                    view,
                    config,
                    plan,
                    contract,
                    "pending",
                    &recorder_status,
                    &producer_status,
                    &snapshot,
                    &error),
            "valid session snapshot should build: " + error);
    require(snapshot.at("schema_id") ==
                orange::session::spatial_roi::
                    kSpatialRoiSessionSnapshotSchemaId &&
                snapshot.at("schema_version") ==
                    orange::session::spatial_roi::
                        kSpatialRoiSessionSnapshotSchemaVersion &&
                snapshot.at("status") == "pending",
            "snapshot schema and status");
    require(snapshot.at("identity").at("recording_identity_token") ==
                view.recording_identity_token &&
                snapshot.at("identity").at("producer_generation") ==
                    view.producer_generation &&
                snapshot.at("identity").at("spatial_roi_plan_sha256") ==
                    view.spatial_roi_plan_sha256,
            "snapshot recording identity");
    require(snapshot.at("camera").at("camera_id") == view.camera_id &&
                snapshot.at("camera").at("camera_serial") ==
                    view.camera_serial &&
                snapshot.at("camera").at("native_raster").at("width") ==
                    view.native_raster.width,
            "snapshot camera identity and raster");
    require(snapshot.at("authorities").at("layout").at("id") ==
                "layout_1" &&
                snapshot.at("authorities").at("materialization").at("id") ==
                    "materialization_1" &&
                snapshot.at("authorities").at("registration").at("id") ==
                    "registration_1",
            "snapshot geometry authorities");
    require(snapshot.at("rois").size() == 4,
            "snapshot must contain exactly four ROIs");
    for (std::size_t index = 0; index < 4; ++index) {
        require(snapshot.at("rois").at(index).at("logical_stream_id") ==
                    view.stream_order.at(index) &&
                    snapshot.at("rois").at(index).at("geometry")
                            .at("source_coordinate_space") ==
                        "camera_native_full_frame_pixels",
                "ROI order and geometry");
    }
    require(snapshot.at("gpu_mapping")
                    .at("analytics_gpu_by_camera_serial")
                    .at(view.camera_serial) == view.analytics_gpu_id &&
                snapshot.at("gpu_mapping")
                    .at("recorder_gpu_by_logical_stream_id")
                    .size() == 4,
            "exact GPU mapping");
    require(snapshot.at("artifacts").at("normalized_config")
                    .at("relative_path") == config.relative_path &&
                snapshot.at("artifacts").at("verified_plan")
                    .at("size_bytes") == plan.size_bytes &&
                snapshot.at("artifacts").at("recorder_contract")
                    .at("sha256") == contract.sha256,
            "authority artifact references");
    require(snapshot.at("recorder_process_status").at("state") == "ready" &&
                snapshot.at("producer_status").at("state") == "armed",
            "optional runtime statuses");
    orange::session::spatial_roi::SpatialRoiSessionSnapshotValidation validation;
    const bool pending_valid =
        orange::session::spatial_roi::validate_spatial_roi_session_snapshot_json(
            snapshot, &validation, &error);
    require(pending_valid && validation.status == "pending" &&
                validation.camera_serial == view.camera_serial &&
                validation.stream_order == view.stream_order,
            "public session snapshot validator should accept pending snapshot: " +
                error);
    require(snapshot.dump().find(view.recording_root) == std::string::npos,
            "snapshot must not emit an absolute recording root");
}

void test_rejects_unsafe_refs_and_inconsistent_views()
{
    const SpatialRoiRecorderCameraContractView view = make_camera_contract();
    const SpatialRoiSessionArtifactReference config =
        make_artifact("session/spatial_roi_config.json", 101, 'c');
    const SpatialRoiSessionArtifactReference plan =
        make_artifact("session/spatial_roi_plan.json", 202, 'd');
    const SpatialRoiSessionArtifactReference contract =
        make_artifact("session/spatial_roi_contract.json", 303, 'e');
    nlohmann::json snapshot;
    std::string error;

    const auto rejected = [&](const SpatialRoiRecorderCameraContractView& candidate,
                              const SpatialRoiSessionArtifactReference& candidate_config,
                              const std::string& candidate_status,
                              const nlohmann::json* recorder_status = nullptr) {
        require(!orange::session::spatial_roi::
                     build_spatial_roi_session_snapshot_json(
                         candidate,
                         candidate_config,
                         plan,
                         contract,
                         candidate_status,
                         recorder_status,
                         nullptr,
                         &snapshot,
                         &error) &&
                    snapshot.is_object() && snapshot.empty() && !error.empty(),
                "invalid snapshot input must fail closed: " + error);
    };

    rejected(view, make_artifact("/tmp/absolute.json", 1, 'f'), "pending");
    rejected(view, make_artifact("../escape.json", 1, 'f'), "pending");
    rejected(view, make_artifact("session/empty.json", 0, 'f'), "pending");
    rejected(view, config, "completed");

    SpatialRoiRecorderCameraContractView invalid = view;
    invalid.streams[1].camera_id = 99;
    rejected(invalid, config, "failed");

    invalid = view;
    invalid.stream_order[0] = invalid.stream_order[1];
    rejected(invalid, config, "failed");

    const nlohmann::json invalid_status = nlohmann::json::array();
    rejected(view, config, "pending", &invalid_status);

    invalid = view;
    invalid.stream_count = 3;
    rejected(invalid, config, "failed");
}

void test_complete_snapshot_requires_and_embeds_finalized_receipt()
{
    const SpatialRoiRecorderCameraContractView view = make_camera_contract();
    const SpatialRoiSessionArtifactReference config =
        make_artifact("session/spatial_roi_config.json", 101, 'c');
    const SpatialRoiSessionArtifactReference plan =
        make_artifact("session/spatial_roi_plan.json", 202, 'd');
    const SpatialRoiSessionArtifactReference contract =
        make_artifact("session/spatial_roi_contract.json", 303, 'e');
    const nlohmann::json receipt = make_finalized_receipt(view);
    const nlohmann::json process_status = make_complete_recorder_process_status();
    const nlohmann::json producer_status = make_complete_producer_status(view, 1);
    nlohmann::json snapshot;
    std::string error;

    require(!orange::session::spatial_roi::
                 build_spatial_roi_session_snapshot_json(
                     view, config, plan, contract, "complete", nullptr,
                     nullptr, &snapshot, &error) &&
                snapshot.empty() && !error.empty(),
            "complete snapshot must reject a missing finalized receipt");

    require(orange::session::spatial_roi::
                build_spatial_roi_session_snapshot_json(
                    view, config, plan, contract, "complete", &process_status,
                    &producer_status, &receipt, &snapshot, &error),
            "complete snapshot should accept a valid finalized receipt: " + error);
    require(snapshot.at("schema_version") == 3 &&
                snapshot.at("finalized_session_receipt") == receipt,
            "complete snapshot should embed the validated raw receipt");
    orange::session::spatial_roi::SpatialRoiSessionSnapshotValidation validation;
    const bool complete_valid =
        orange::session::spatial_roi::validate_spatial_roi_session_snapshot_json(
            snapshot, &validation, &error);
    require(complete_valid && validation.status == "complete" &&
                validation.finalized_session_receipt == receipt,
            "public session snapshot validator should accept complete snapshot: " +
                error);

    nlohmann::json partitioned = snapshot;
    partitioned["finalized_session_receipt"]["streams"][1]
               ["ranges"]["recording_frame_id"]["last"] = 2;
    partitioned["finalized_session_receipt"]["streams"][1]
               ["ranges"]["roi_stream_frame_index"]["last"] = 2;
    partitioned["finalized_session_receipt"]["streams"][1]
               ["ranges"]["frame_count"] = 2;
    const std::array<const char*, 10> frame_counters = {
        "detach_successes", "dispatch_admitted", "ack_attempted", "ack_sent",
        "ack_accepted", "release_attempted", "release_sent", "encoded_frames",
        "packet_count", "keyframes"};
    for (const char* key : frame_counters) {
        partitioned["finalized_session_receipt"]["streams"][1]
                   ["counts"][key] = 2;
    }
    partitioned["finalized_session_receipt"]["streams"][1]
               ["counts"]["encoded_bytes"] = 200;
    require(!orange::session::spatial_roi::validate_spatial_roi_session_snapshot_json(
                partitioned, &validation, &error),
            "complete receipt must reject partitioned cross-lane frame ranges");

    nlohmann::json bad_producer = snapshot;
    bad_producer["producer_status"]["state"] = "ready";
    require(!orange::session::spatial_roi::validate_spatial_roi_session_snapshot_json(
                bad_producer, &validation, &error),
            "complete snapshot must reject a non-stopped producer");
    bad_producer = snapshot;
    bad_producer["producer_status"]["submitted"] = 0;
    require(!orange::session::spatial_roi::validate_spatial_roi_session_snapshot_json(
                bad_producer, &validation, &error),
            "complete snapshot must reject a producer count mismatch");

    nlohmann::json bad_budget = snapshot;
    for (const char* child : {"ready_snapshot", "terminal"}) {
        bad_budget["recorder_process_status"][child]["payload"]
                  ["storage_preflight"]["budgets"]["max_evidence_bytes_total"] =
            100;
        bad_budget["recorder_process_status"][child]["payload"]
                  ["storage_preflight"]["budgets"]["required_bytes"] = 1110;
    }
    require(!orange::session::spatial_roi::validate_spatial_roi_session_snapshot_json(
                bad_budget, &validation, &error),
            "complete snapshot must reject receipt artifacts over storage budget");

    nlohmann::json bad_process = snapshot;
    bad_process["recorder_process_status"]["reaped"] = false;
    require(!orange::session::spatial_roi::validate_spatial_roi_session_snapshot_json(
                bad_process, &validation, &error),
            "complete snapshot must reject an unreaped recorder process");
    bad_process = snapshot;
    bad_process["recorder_process_status"]["terminal"]["completed"] = false;
    require(!orange::session::spatial_roi::validate_spatial_roi_session_snapshot_json(
                bad_process, &validation, &error),
            "complete snapshot must reject an incomplete terminal child event");
    bad_process = snapshot;
    bad_process["recorder_process_status"]["terminal"]["failed"] = true;
    require(!orange::session::spatial_roi::validate_spatial_roi_session_snapshot_json(
                bad_process, &validation, &error),
            "complete snapshot must reject a failed terminal child event");
    bad_process = snapshot;
    bad_process["recorder_process_status"]["last"]["failed"] = true;
    require(!orange::session::spatial_roi::validate_spatial_roi_session_snapshot_json(
                bad_process, &validation, &error),
            "complete snapshot must reject a last child event diverging from terminal");

    bad_process = snapshot;
    bad_process["recorder_process_status"]["stdout_bytes_read"] = 0;
    require(!orange::session::spatial_roi::validate_spatial_roi_session_snapshot_json(
                bad_process, &validation, &error),
            "complete snapshot must reject a recorder with no observed stdout");

    const auto mismatched = [](const nlohmann::json& original) {
        if (original.is_boolean()) {
            return nlohmann::json(!original.get<bool>());
        }
        return nlohmann::json(original.get<std::string>() + "_mismatch");
    };
    for (const char* child_name : {"ready_snapshot", "terminal"}) {
        for (const char* key : {"event", "status", "state", "ready",
                                "clean_eof", "completed", "failed",
                                "first_failure_stream_id", "first_failure",
                                "error"}) {
            bad_process = snapshot;
            const auto& child =
                bad_process["recorder_process_status"][child_name];
            bad_process["recorder_process_status"][child_name]["payload"][key] =
                mismatched(child.at(key));
            require(!orange::session::spatial_roi::
                         validate_spatial_roi_session_snapshot_json(
                             bad_process, &validation, &error),
                    std::string("complete snapshot must reject a raw ") +
                        child_name + " payload " + key + " mismatch");
        }
    }

    require(orange::session::spatial_roi::
                build_spatial_roi_session_snapshot_json(
                    view, config, plan, contract, "pending", nullptr,
                    nullptr, &snapshot, &error),
            "pending snapshot should not require a receipt");
    require(snapshot.at("finalized_session_receipt").is_null(),
            "pending snapshot should emit a null receipt");
    require(!orange::session::spatial_roi::
                 build_spatial_roi_session_snapshot_json(
                     view, config, plan, contract, "failed", nullptr, nullptr,
                     &receipt, &snapshot, &error) &&
                snapshot.empty(),
            "failed snapshot must reject a supplied finalized receipt");
}

void test_gop25_snapshot_uses_configured_keyframe_cadence()
{
    const SpatialRoiRecorderCameraContractView view =
        make_gop25_camera_contract();
    const SpatialRoiSessionArtifactReference config =
        make_artifact("session/spatial_roi_config.json", 101, 'c');
    const SpatialRoiSessionArtifactReference plan =
        make_artifact("session/spatial_roi_plan.json", 202, 'd');
    const SpatialRoiSessionArtifactReference contract =
        make_artifact("session/spatial_roi_contract.json", 303, 'e');
    const nlohmann::json receipt = make_finalized_receipt(view, 26);
    const nlohmann::json process_status = make_complete_recorder_process_status();
    const nlohmann::json producer_status = make_complete_producer_status(view, 26);
    nlohmann::json snapshot;
    std::string error;
    require(orange::session::spatial_roi::
                build_spatial_roi_session_snapshot_json(
                    view, config, plan, contract, "complete", &process_status,
                    &producer_status, &receipt, &snapshot, &error),
            "GOP25 complete snapshot should accept two keyframes for 26 frames: " +
                error);

    orange::session::spatial_roi::SpatialRoiSessionSnapshotValidation validation;
    require(orange::session::spatial_roi::
                validate_spatial_roi_session_snapshot_json(
                    snapshot, &validation, &error),
            "GOP25 public snapshot validator should accept interior non-keyframes: " +
                error);

    nlohmann::json all_keyframes = receipt;
    for (auto& stream : all_keyframes["streams"]) {
        stream["counts"]["keyframes"] = 26;
    }
    require(!orange::session::spatial_roi::
                 build_spatial_roi_session_snapshot_json(
                     view, config, plan, contract, "complete", &process_status,
                     &producer_status, &all_keyframes, &snapshot, &error),
            "GOP25 snapshot must reject a keyframe count that ignores cadence");
}

void test_gop25_keyframe_boundary_counts_and_gop1_compatibility()
{
    const SpatialRoiSessionArtifactReference config =
        make_artifact("session/spatial_roi_config.json", 101, 'c');
    const SpatialRoiSessionArtifactReference plan =
        make_artifact("session/spatial_roi_plan.json", 202, 'd');
    const SpatialRoiSessionArtifactReference contract =
        make_artifact("session/spatial_roi_contract.json", 303, 'e');
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 5> gop25_cases = {{
        {1, 1}, {2, 1}, {25, 1}, {26, 2}, {51, 3}}};
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 5> gop1_cases = {{
        {1, 1}, {2, 2}, {25, 25}, {26, 26}, {51, 51}}};
    const nlohmann::json process_status = make_complete_recorder_process_status();

    const auto verify_cases = [&](
        const SpatialRoiRecorderCameraContractView& view,
        const auto& cases,
        const char* profile_name) {
        for (const auto& [frame_count, expected_keyframes] : cases) {
            const nlohmann::json receipt =
                make_finalized_receipt(view, frame_count);
            for (const auto& stream : receipt.at("streams")) {
                const std::uint64_t actual_keyframes =
                    stream.at("counts").at("keyframes").get<std::uint64_t>();
                require(actual_keyframes == expected_keyframes,
                        std::string(profile_name) + " frame-count boundary " +
                            std::to_string(frame_count) +
                            " produced the wrong keyframe count");
                const std::uint64_t non_keyframes =
                    frame_count - actual_keyframes;
                require(non_keyframes == frame_count - expected_keyframes,
                        std::string(profile_name) +
                            " frame-count boundary produced the wrong non-keyframe count");
            }

            nlohmann::json snapshot;
            std::string error;
            const nlohmann::json producer_status =
                make_complete_producer_status(view, frame_count);
            require(orange::session::spatial_roi::
                        build_spatial_roi_session_snapshot_json(
                            view, config, plan, contract, "complete",
                            &process_status, &producer_status, &receipt,
                            &snapshot, &error),
                    std::string(profile_name) +
                        " boundary receipt should validate: " + error);

            orange::session::spatial_roi::SpatialRoiSessionSnapshotValidation
                validation;
            require(orange::session::spatial_roi::
                        validate_spatial_roi_session_snapshot_json(
                            snapshot, &validation, &error),
                    std::string(profile_name) +
                        " boundary snapshot should validate: " + error);

            nlohmann::json wrong_keyframe_count = receipt;
            const std::uint64_t wrong_count =
                expected_keyframes == 0 ? 1 : expected_keyframes - 1;
            for (auto& stream : wrong_keyframe_count["streams"]) {
                stream["counts"]["keyframes"] = wrong_count;
            }
            require(!orange::session::spatial_roi::
                         build_spatial_roi_session_snapshot_json(
                             view, config, plan, contract, "complete",
                             &process_status, &producer_status,
                             &wrong_keyframe_count, &snapshot, &error),
                    std::string(profile_name) +
                        " boundary receipt should reject the wrong keyframe count");
        }
    };

    const SpatialRoiRecorderCameraContractView gop25_view =
        make_gop25_camera_contract();
    verify_cases(gop25_view, gop25_cases, "GOP25");

    const SpatialRoiRecorderCameraContractView gop1_view = make_camera_contract();
    verify_cases(gop1_view, gop1_cases, "GOP1");
}

void test_public_validator_rejects_invariant_substitution()
{
    const SpatialRoiRecorderCameraContractView view = make_camera_contract();
    const SpatialRoiSessionArtifactReference config =
        make_artifact("session/spatial_roi_config.json", 101, 'c');
    const SpatialRoiSessionArtifactReference plan =
        make_artifact("session/spatial_roi_plan.json", 202, 'd');
    const SpatialRoiSessionArtifactReference contract =
        make_artifact("session/spatial_roi_contract.json", 303, 'e');
    nlohmann::json snapshot;
    std::string error;
    require(orange::session::spatial_roi::build_spatial_roi_session_snapshot_json(
                view, config, plan, contract, "pending", &snapshot, &error),
            "valid pending snapshot should build: " + error);

    const auto rejected = [&](const nlohmann::json& candidate,
                              const std::string& message) {
        orange::session::spatial_roi::SpatialRoiSessionSnapshotValidation validation;
        std::string candidate_error;
        require(!orange::session::spatial_roi::validate_spatial_roi_session_snapshot_json(
                    candidate, &validation, &candidate_error),
                message + ": substituted snapshot was accepted");
    };

    nlohmann::json substituted = snapshot;
    substituted["rois"][0]["geometry"]["native_raster"]["width"] = 639;
    rejected(substituted, "native raster substitution must reject");

    substituted = snapshot;
    substituted["rois"][0]["geometry"]["source_coordinate_space"] =
        "wrong_coordinate_space";
    rejected(substituted, "coordinate-space substitution must reject");

    substituted = snapshot;
    substituted["rois"][0]["geometry"]["padding"]["right"] = 1;
    rejected(substituted, "padding substitution must reject");

    substituted = snapshot;
    substituted["rois"][0]["geometry"]["layout"]["id"] = "other_layout";
    rejected(substituted, "authority substitution must reject");

    substituted = snapshot;
    substituted["rois"][0]["encode_profile"]["lossless"] = false;
    rejected(substituted, "profile substitution must reject");

    substituted = snapshot;
    substituted["gpu_mapping"]["analytics_gpu_by_camera_serial"]
               [view.camera_serial] = view.analytics_gpu_id + 1;
    rejected(substituted, "analytics GPU mapping substitution must reject");

    substituted = snapshot;
    substituted["rois"][0]["expected_shard_gpu_ids"][0] =
        substituted["rois"][0]["recorder_gpu_id"].get<int>() + 1;
    rejected(substituted, "recorder shard substitution must reject");

    substituted = snapshot;
    substituted["recording_identity_token"] = digest('f');
    substituted["identity"]["recording_identity_token"] = digest('f');
    rejected(substituted, "recording identity token derivation substitution must reject");
}

void test_finalized_receipt_gate_rejects_substitution_and_duplicates()
{
    const SpatialRoiRecorderCameraContractView view = make_camera_contract();
    const SpatialRoiSessionArtifactReference config =
        make_artifact("session/spatial_roi_config.json", 101, 'c');
    const SpatialRoiSessionArtifactReference plan =
        make_artifact("session/spatial_roi_plan.json", 202, 'd');
    const SpatialRoiSessionArtifactReference contract =
        make_artifact("session/spatial_roi_contract.json", 303, 'e');
    const nlohmann::json valid_receipt = make_finalized_receipt(view);
    const nlohmann::json process_status = make_complete_recorder_process_status();
    const nlohmann::json producer_status = make_complete_producer_status(view, 1);
    nlohmann::json snapshot;
    std::string error;

    const auto rejected = [&](const nlohmann::json& candidate,
                              const std::string& message) {
        require(!orange::session::spatial_roi::
                     build_spatial_roi_session_snapshot_json(
                         view, config, plan, contract, "complete", &process_status,
                         &producer_status, &candidate, &snapshot, &error) &&
                    snapshot.empty() && !error.empty(),
                message + ": " + error);
    };

    nlohmann::json substituted = valid_receipt;
    substituted["identity"]["camera_serial"] = "other_camera";
    rejected(substituted, "receipt camera identity substitution must reject");

    substituted = valid_receipt;
    substituted["identity"]["recording_root"] = "/tmp/absolute_root";
    rejected(substituted, "receipt identity must not carry an absolute root");

    substituted = valid_receipt;
    std::swap(substituted["stream_order"][0], substituted["stream_order"][1]);
    rejected(substituted, "receipt stream order substitution must reject");

    substituted = valid_receipt;
    substituted["streams"][0]["identity"]["logical_stream_id"] = "other_stream";
    rejected(substituted, "receipt stream identity substitution must reject");

    substituted = valid_receipt;
    substituted["streams"][0]["artifacts"][0]["relative_path"] =
        "other/path.mp4";
    rejected(substituted, "receipt artifact path substitution must reject");

    substituted = valid_receipt;
    substituted["streams"][0]["artifacts"][0]["sha256"] = "sha256:bad";
    rejected(substituted, "receipt non-canonical artifact hash must reject");

    substituted = valid_receipt;
    substituted["streams"][0]["artifacts"][1]["kind"] = "video";
    rejected(substituted, "receipt artifact kind substitution must reject");

    substituted = valid_receipt;
    substituted["streams"][0]["artifacts"][1]["relative_path"] =
        substituted["streams"][0]["artifacts"][0]["relative_path"];
    rejected(substituted, "receipt duplicate artifact path must reject");

    substituted = valid_receipt;
    substituted["root_authority"]["artifact_root_identity"]["inode"] = 4;
    rejected(substituted, "receipt artifact-root identity substitution must reject");

    substituted = valid_receipt;
    substituted["streams"][0]["counts"]["ack_accepted"] = 0;
    rejected(substituted, "receipt count substitution must reject");

    substituted = valid_receipt;
    substituted["streams"][0]["artifacts"][0]["size_bytes"] = 0;
    rejected(substituted, "zero-sized receipt artifact must reject");
}

}  // namespace

int main()
{
    try {
        test_builds_closed_plan_ordered_snapshot();
        std::cout << "[PASS] builds_closed_plan_ordered_snapshot\n";
        test_rejects_unsafe_refs_and_inconsistent_views();
        std::cout << "[PASS] rejects_unsafe_refs_and_inconsistent_views\n";
        test_complete_snapshot_requires_and_embeds_finalized_receipt();
        std::cout << "[PASS] complete_snapshot_requires_and_embeds_finalized_receipt\n";
        test_gop25_snapshot_uses_configured_keyframe_cadence();
        std::cout << "[PASS] gop25_snapshot_uses_configured_keyframe_cadence\n";
        test_gop25_keyframe_boundary_counts_and_gop1_compatibility();
        std::cout << "[PASS] gop25_keyframe_boundary_counts_and_gop1_compatibility\n";
        test_public_validator_rejects_invariant_substitution();
        std::cout << "[PASS] public_validator_rejects_invariant_substitution\n";
        test_finalized_receipt_gate_rejects_substitution_and_duplicates();
        std::cout << "[PASS] finalized_receipt_gate_rejects_substitution_and_duplicates\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "[FAIL] spatial_roi_session_snapshot_tests: "
                  << exception.what() << '\n';
        return 1;
    }
}
