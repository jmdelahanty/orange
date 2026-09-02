#include "session/spatial_roi_recorder_contract.h"

#include "session/spatial_roi_recording_config.h"
#include "shaman_v2_recording_identity.h"

#include <functional>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace spatial_roi = orange::session::spatial_roi;

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void validates_normative_ipc_v2_schema()
{
    const std::string schema_path =
        std::string(ORANGE_SOURCE_DIR) +
        "/docs/schemas/orange_spatial_roi_recorder_ipc_v2.schema.json";
    std::ifstream input(schema_path);
    require(input.good(), "failed to open normative IPC-v2 schema");

    nlohmann::json schema;
    input >> schema;
    require(schema.value("$id", std::string()) ==
                "orange.spatial_roi_recording.external_recorder_ipc_v2",
            "normative IPC-v2 schema id mismatch");
    require(schema.value("additionalProperties", true) == false,
            "normative IPC-v2 schema must be closed");

    const std::set<std::string> required =
        schema.at("required").get<std::set<std::string>>();
    require(required ==
                std::set<std::string>{"protocol", "version", "features",
                                      "source_lifetime_mode", "ack", "release",
                                      "drain_finalize", "bounds"},
            "normative IPC-v2 schema top-level required set drifted");
    const nlohmann::json& properties = schema.at("properties");
    require(properties.at("features").at("const") ==
                nlohmann::json{"cuda_ipc", "packed_mono8", "ack_release",
                               "terminal_error"},
            "normative IPC-v2 feature list drifted");
    require(properties.at("drain_finalize")
                    .at("properties")
                    .at("status")
                    .at("const") == "defined_not_negotiated" &&
                properties.at("drain_finalize")
                    .at("properties")
                    .at("operational")
                    .at("const") == false,
            "drain/finalize grammar must remain explicitly non-operational");
    require(properties.at("ack")
                    .at("properties")
                    .at("accepted_false")
                    .at("properties")
                    .at("source_safe_after_ack")
                    .at("const") == false &&
                properties.at("ack")
                    .at("properties")
                    .at("accepted_false")
                    .at("properties")
                    .at("release_required")
                    .at("const") == true,
            "rejected ACK must remain non-source-safe until RELEASE");
    const std::set<std::string> release_required =
        properties.at("release").at("required").get<std::set<std::string>>();
    require(release_required.count("required_after_rejected_ack") == 1,
            "normative release schema omitted rejected-ACK ownership");
}

std::string digest(char fill)
{
    return "sha256:" + std::string(64, fill);
}

spatial_roi::RoiConfig make_roi(const std::string& camera_serial,
                                const std::string& roi_id,
                                const std::string& region_id,
                                const spatial_roi::Rect& rect)
{
    spatial_roi::RoiConfig roi;
    roi.roi_id = roi_id;
    roi.region_id = region_id;
    roi.has_arena_id = true;
    roi.arena_id = "arena_" + roi_id;
    roi.required = true;
    roi.content_rect = rect;
    roi.logical_stream_id =
        spatial_roi::expected_logical_stream_id(camera_serial, roi_id);
    roi.artifact_stem =
        spatial_roi::expected_artifact_stem(camera_serial, roi_id);
    return roi;
}

nlohmann::json make_plan(std::uint32_t output_alignment_px = 2)
{
    spatial_roi::Config config = spatial_roi::default_config();
    config.enabled = true;
    config.output_alignment_px = output_alignment_px;
    config.buffering.pool_frames_per_stream = 4;
    config.buffering.queue_frames_per_stream = 8;
    config.recording_limits.max_frames_per_stream = 1000;
    config.recording_limits.max_media_bytes_per_stream = 1000000;
    config.recording_limits.max_evidence_bytes_per_stream = 100000;
    config.admission.max_rois_per_camera = 8;
    config.admission.max_total_rois = 8;
    config.admission.max_total_encoder_streams = 8;
    config.admission.max_total_pixel_rate = 100000000ULL;
    config.admission.max_total_pool_bytes = 100000000ULL;
    config.admission.max_total_queue_frames = 128;
    config.admission.max_total_media_bytes = 8000000;
    config.admission.max_total_evidence_bytes = 800000;

    spatial_roi::CameraConfig camera;
    camera.camera_id = 3;
    camera.camera_serial = "2010096";
    camera.native_raster = {100, 100};
    camera.source_frame_rate = 100;
    camera.arena_group_id = "group_1";
    camera.layout = {"layout_1", digest('1')};
    camera.materialization = {"materialization_1", digest('2')};
    camera.registration = {"registration_1", digest('3')};
    camera.rois.push_back(
        make_roi(camera.camera_serial, "roi_1", "region_1", {0, 0, 10, 10}));
    camera.rois.push_back(make_roi(
        camera.camera_serial, "roi_2", "region_2", {20, 0, 11, 12}));
    camera.rois.push_back(make_roi(
        camera.camera_serial, "roi_3", "region_3", {40, 0, 12, 13}));
    camera.rois.push_back(make_roi(
        camera.camera_serial, "roi_4", "region_4", {60, 0, 13, 14}));
    config.cameras.emplace(camera.camera_serial, std::move(camera));

    spatial_roi::PlanContext context;
    context.recording_id = "roi-contract-test";
    context.recording_identity_token =
        orange::shaman_v2_recording_identity::token_for_recording_id(
            context.recording_id);
    context.generated_at_utc = "2026-08-31T00:00:00Z";
    context.producer_generation = "generation_1";

    nlohmann::json plan;
    std::string error;
    require(spatial_roi::build_plan(config, context, &plan, nullptr, &error), error);
    return plan;
}

nlohmann::json make_oversized_packed_roi_plan()
{
    spatial_roi::Config config = spatial_roi::default_config();
    config.enabled = true;
    config.buffering.pool_frames_per_stream = 1;
    config.buffering.queue_frames_per_stream = 1;
    config.admission.max_rois_per_camera = 1;
    config.admission.max_total_rois = 1;
    config.admission.max_total_encoder_streams = 1;
    config.admission.max_total_pixel_rate =
        std::numeric_limits<std::uint64_t>::max();
    config.admission.max_total_pool_bytes =
        std::numeric_limits<std::uint64_t>::max();
    config.admission.max_total_queue_frames = 1;

    spatial_roi::CameraConfig camera;
    camera.camera_id = 3;
    camera.camera_serial = "2010096";
    camera.native_raster = {65536, 32770};
    camera.source_frame_rate = 1;
    camera.arena_group_id = "group_1";
    camera.layout = {"layout_1", digest('1')};
    camera.materialization = {"materialization_1", digest('2')};
    camera.registration = {"registration_1", digest('3')};
    camera.rois.push_back(make_roi(camera.camera_serial,
                                   "roi_1",
                                   "region_1",
                                   {0, 0, 65536, 32770}));
    config.cameras.emplace(camera.camera_serial, std::move(camera));

    spatial_roi::PlanContext context;
    context.recording_id = "roi-contract-oversized-test";
    context.recording_identity_token =
        orange::shaman_v2_recording_identity::token_for_recording_id(
            context.recording_id);
    context.generated_at_utc = "2026-08-31T00:00:00Z";
    context.producer_generation = "generation_oversized";

    nlohmann::json plan;
    std::string error;
    require(spatial_roi::build_plan(config, context, &plan, nullptr, &error),
            error);
    return plan;
}

spatial_roi::SpatialRoiRecorderRuntimeGpuMapping make_mapping()
{
    spatial_roi::SpatialRoiRecorderRuntimeGpuMapping mapping;
    mapping.analytics_gpu_by_camera_serial.emplace("2010096", 5);
    for (const std::string roi_id : {"roi_1", "roi_2", "roi_3", "roi_4"}) {
        mapping.recorder_gpu_by_logical_stream_id.emplace(
            spatial_roi::expected_logical_stream_id("2010096", roi_id), 6);
    }
    return mapping;
}

void builds_one_strict_nonrolling_stream_per_roi()
{
    const nlohmann::json plan = make_plan();
    const auto& expected_plan_profile =
        plan.at("plan").at("configuration").at("encode_profile");
    const auto mapping = make_mapping();
    nlohmann::json contract;
    std::string error;
    require(spatial_roi::build_spatial_roi_recorder_contract(
                plan, "/tmp/orange_roi_contract_test", mapping, &contract, &error),
            error);

    require(contract.value("schema_id", std::string()) ==
                spatial_roi::kSpatialRoiRecorderContractSchemaId,
            "contract schema id mismatch");
    require(contract.value("schema_version", 0) ==
                spatial_roi::kSpatialRoiRecorderContractSchemaVersion,
            "contract schema version mismatch");
    require(contract.value("strict", false), "contract must be strict");
    require(contract.value("backend", std::string()) ==
                plan.at("plan").at("configuration").at("backend"),
            "contract backend mismatch");
    require(contract.value("mode", std::string()) ==
                spatial_roi::kSpatialRoiRecorderContractMode,
            "contract mode mismatch");
    require(contract.value("recording_id", std::string()) ==
                plan.at("plan").at("recording_id").get<std::string>(),
            "recording identity was not preserved");
    require(contract.value("session_id", std::string()) ==
                contract.value("recording_id", std::string()),
            "recorder session identity must equal the recording identity");
    require(contract.value("recording_identity_token", std::string()) ==
                plan.at("plan").at("recording_identity_token").get<std::string>(),
            "recording token was not preserved");
    require(contract.value("producer_generation", std::string()) ==
                "generation_1",
            "producer generation was not preserved");
    require(contract.value("spatial_roi_plan_sha256", std::string()) ==
                plan.at("plan_sha256").get<std::string>(),
            "plan digest was not preserved");
    require(contract.value("stream_count", 0) == 4,
            "contract must contain one stream per ROI");
    require(contract.at("streams").is_object() &&
                contract.at("streams").size() == 4,
            "stream collection size mismatch");
    require(!contract.at("rollover").at("requested").get<bool>(),
            "spatial ROI contract must be non-rolling");
    require(contract.at("recording_control").at("clip_seconds").get<int>() == 0,
            "spatial ROI contract must set clip_seconds=0");
    const nlohmann::json expected_ipc_v2 = {
        {"protocol", "orange.spatial_roi.external_recorder_ipc"},
        {"version", 2},
        {"features", {"cuda_ipc", "packed_mono8", "ack_release",
                       "terminal_error"}},
        {"source_lifetime_mode", "deferred_release"},
        {"ack", {
            {"message_kind", "ACK"},
            {"accepted_true", {
                {"means", "recorder_accepted_frame_and_retains_source_access"},
                {"source_safe_after_ack", false},
                {"release_required", true},
            }},
            {"accepted_false", {
                {"means", "recorder_rejected_frame_but_source_access_is_not_yet_released"},
                {"source_safe_after_ack", false},
                {"release_required", true},
            }},
        }},
        {"release", {
            {"message_kind", "RELEASE"},
            {"means", "recorder_finished_with_source_allocation"},
            {"source_safe_after_release", true},
            {"required_after_accepted_ack", true},
            {"required_after_rejected_ack", true},
            {"does_not_mean_encode_or_disk_complete", true},
        }},
        {"drain_finalize", {
            {"status", "defined_not_negotiated"},
            {"operational", false},
            {"message_order", {"DRAIN_REQUEST", "DRAIN_STATUS",
                                "FINALIZE_REQUEST", "FINALIZE_STATUS"}},
            {"drain_request", {
                {"message_kind", "DRAIN_REQUEST"},
                {"sender", "producer"},
                {"receiver", "recorder"},
                {"correlation", "stream_identity_and_drain_sequence"},
                {"reason_required", true},
            }},
            {"drain_status", {
                {"message_kind", "DRAIN_STATUS"},
                {"sender", "recorder"},
                {"receiver", "producer"},
                {"states", {"draining", "drained", "failed"}},
                {"correlation", "stream_identity_and_drain_sequence"},
                {"reason_required", true},
                {"finalize_request_allowed_only_when", "state=drained"},
            }},
            {"finalize_request", {
                {"message_kind", "FINALIZE_REQUEST"},
                {"sender", "producer"},
                {"receiver", "recorder"},
                {"requires", "matching_drained_status"},
                {"correlation", "stream_identity_and_drain_sequence"},
                {"nonce", "fresh_16_byte_lower_hex"},
                {"reason_required", true},
            }},
            {"finalize_status", {
                {"message_kind", "FINALIZE_STATUS"},
                {"sender", "recorder"},
                {"receiver", "producer"},
                {"states", {"finalized", "failed"}},
                {"correlation", "stream_identity_drain_sequence_and_finalize_nonce"},
                {"nonce", "must_equal_request"},
                {"reason_required", true},
                {"session_finalized_only_when", "state=finalized"},
            }},
        }},
        {"bounds", {
            {"queue_capacity_frames_per_stream", 8},
            {"max_outstanding_frames_per_stream", 8},
            {"max_queue_capacity_frames_per_stream", 4096},
            {"queue_capacity_frames_total", 32},
            {"max_outstanding_frames_total", 32},
            {"overflow_action", "reject_frame_without_releasing_prior_frames"},
            {"producer_backpressure", "nonblocking_fail_closed"},
        }},
    };
    require(contract.at("ipc_v2") == expected_ipc_v2,
            "contract must contain the exact closed IPC-v2 handoff object");
    require(contract.value("schema_version", 0) ==
                spatial_roi::kSpatialRoiRecorderContractSchemaVersion &&
                contract.value("mode", std::string()) ==
                    spatial_roi::kSpatialRoiRecorderContractMode &&
                contract.at("require_storage_preflight") == true &&
                contract.at("storage_preflight_policy") == nlohmann::json{
                    {"schema_id",
                     spatial_roi::kSpatialRoiRecorderStoragePreflightPolicySchemaId},
                    {"schema_version",
                     spatial_roi::kSpatialRoiRecorderStoragePreflightPolicySchemaVersion},
                    {"required", true},
                    {"reserved_free_bytes",
                     spatial_roi::kSpatialRoiRecorderReservedFreeBytes},
                },
            "contract must authenticate the nonzero storage preflight policy");
    require(contract.at("aggregate_bounds") == nlohmann::json{
                {"max_detach_pool_bytes_total", 12160},
                {"max_queue_bytes_total", 7296},
                {"writer_queue_max_packets_total", 2048},
                {"writer_queue_max_bytes_total", 536870912},
                {"operation_timeout_ms_per_stream", 2000},
                {"max_media_bytes_total", 4000000},
                {"max_evidence_bytes_total", 400000}},
            "contract aggregate recorder bounds are not the checked stream totals");

    std::vector<std::string> stream_ids;
    std::set<std::string> recorder_log_paths;
    std::set<std::string> transport_sidecar_paths;
    std::set<std::string> evidence_paths;
    std::set<std::string> evidence_manifest_paths;
    for (const auto& roi_id : {"roi_1", "roi_2", "roi_3", "roi_4"}) {
        const std::string roi_name(roi_id);
        const std::string stream_id =
            spatial_roi::expected_logical_stream_id("2010096", roi_id);
        require(contract.at("streams").contains(stream_id),
                "expected logical stream is missing");
        const auto& stream = contract.at("streams").at(stream_id);
        require(stream.value("stream_id", std::string()) == stream_id,
                "stream_id mismatch");
        require(stream.value("logical_stream_id", std::string()) == stream_id,
                "logical_stream_id mismatch");
        require(stream.value("stream_kind", std::string()) == "spatial_roi",
                "stream_kind must be spatial_roi");
        require(stream.value("output_kind", std::string()) == "spatial_roi",
                "output_kind must be spatial_roi");
        require(stream.value("camera_serial", std::string()) == "2010096",
                "camera_serial must remain the source serial");
        require(stream.value("roi_id", std::string()) == roi_id,
                "roi_id mismatch");
        require(stream.at("identity").value("region_id", std::string()) ==
                    "region_" + roi_name.substr(4),
                "region identity mismatch");
        require(stream.at("arena_id").is_string(),
                "arena identity should be preserved");
        require(stream.at("encode_profile").value("profile_id", std::string()) ==
                    expected_plan_profile.at("name"),
                "encode profile identifier must be authenticated");
        require(stream.at("encode_profile").value("codec", std::string()) ==
                    expected_plan_profile.at("codec"),
                "codec must be HEVC");
        require(stream.at("encode_profile").value("preset", std::string()) ==
                    expected_plan_profile.at("preset"),
                "preset must be authenticated");
        require(stream.at("encode_profile").value("tuning", std::string()) ==
                    expected_plan_profile.at("tuning"),
                "tuning must match the plan profile");
        require(stream.at("encode_profile").value("lossless", true) ==
                    expected_plan_profile.at("lossless"),
                "lossless flag must match the plan profile");
        require(stream.at("encode_profile").value("rate_control_mode", std::string()) ==
                    expected_plan_profile.at("rate_control_mode") &&
                    stream.at("encode_profile").value("quality_value", 0) ==
                    expected_plan_profile.at("quality_value"),
                "rate control policy must match the plan profile");
        require(stream.at("encode_profile").value("gop_length", 0) ==
                    expected_plan_profile.at("gop_length"),
                "spatial ROI GOP must match the immutable plan profile");
        require(stream.at("encode_profile").value("aq", true) == false &&
                    stream.at("encode_profile").value("temporal_aq", true) == false &&
                    stream.at("encode_profile").value("lookahead", true) == false &&
                    stream.at("encode_profile").value("lookahead_depth", 1) == 0,
                "spatial ROI encoder controls must be explicitly disabled");
        require(stream.at("encode_profile").value("frame_rate", 0) == 100,
                "source frame rate mismatch");
        require(stream.at("encode_profile").value("input_format", std::string()) ==
                    "mono8" &&
                    stream.at("encode_profile").value("encoded_format", std::string()) ==
                    "nv12" &&
                    stream.at("encode_profile").value("luma_preserved_exactly", true) ==
                    expected_plan_profile.at("lossless") &&
                    stream.at("encode_profile").value("neutral_chroma_value", 0) == 128,
                "Mono8-to-NV12 contract must declare the luma/chroma policy");
        require(!stream.at("encode_profile").contains("no_color_conversion"),
                "encode profile must not claim that Mono8-to-NV12 avoids conversion");
        require(stream.value("codec", std::string()) ==
                    expected_plan_profile.at("codec") &&
                    stream.value("tuning", std::string()) ==
                    expected_plan_profile.at("tuning") &&
                    stream.value("gop", 0) ==
                        expected_plan_profile.at("gop_length"),
                "direct recorder encode profile must match the plan GOP");
        require(stream.value("rate_control_mode", std::string()) ==
                    expected_plan_profile.at("rate_control_mode") &&
                    stream.value("quality_value", 0) ==
                    expected_plan_profile.at("quality_value"),
                "recorder rate-control policy must match the supported profile");
        require(stream.value("encode_queue_depth", 0) == 8,
                "verified per-stream queue bound was not propagated");
        require(stream.value("detach_pool_frames", 0) == 8,
                "verified detach-pool slot bound was not propagated");
        const std::uint64_t encoded_pixels =
            stream.at("geometry_identity").at("encoded_raster").at("width")
                .get<std::uint64_t>() *
            stream.at("geometry_identity").at("encoded_raster").at("height")
                .get<std::uint64_t>();
        require(stream.value("max_queue_bytes", std::uint64_t{0}) ==
                    (encoded_pixels + encoded_pixels / 2U) * 8U,
                "detached NV12 queue byte bound was not derived from geometry/depth");
        require(stream.value("max_detach_pool_bytes", std::uint64_t{0}) ==
                    (encoded_pixels * 2U + encoded_pixels / 2U) * 8U,
                "Mono8+NV12 detach-pool byte bound was not derived from geometry/depth");
        require(stream.value("writer_queue_max_packets", std::uint64_t{0}) ==
                    spatial_roi::kSpatialRoiRecorderWriterQueueMaxPackets &&
                    stream.value("writer_queue_max_bytes", std::uint64_t{0}) ==
                        spatial_roi::kSpatialRoiRecorderWriterQueueMaxBytes &&
                    stream.value("operation_timeout_ms", 0U) ==
                        spatial_roi::kSpatialRoiRecorderOperationTimeoutMs,
                "encoder/writer construction bounds are not authenticated");
        require(stream.value("max_frames_per_stream", std::uint64_t{0}) ==
                    1000 &&
                    stream.value("max_media_bytes_per_stream", std::uint64_t{0}) ==
                        1000000 &&
                    stream.value("max_evidence_bytes_per_stream",
                                 std::uint64_t{0}) == 100000,
                "long-run per-stream admission was not authenticated");
        require(stream.value("session_id", std::string()) ==
                    contract.value("session_id", std::string()),
                "stream session identity drifted from the recording");
        require(stream.at("expected_shard_gpu_ids").size() == 1 &&
                    stream.at("expected_shard_gpu_ids").at(0) == 6,
                "each ROI must map to exactly one recorder GPU");
        require(stream.at("rollover").value("requested", true) == false,
                "each ROI stream must be non-rolling");
        require(stream.at("geometry_identity").at("source_coordinate_space") ==
                    "camera_native_full_frame_pixels",
                "source geometry coordinate space mismatch");
        require(stream.at("geometry_identity").at("video_coordinate_space") ==
                    "spatial_roi_encoded_pixels",
                "video geometry coordinate space mismatch");
        const auto& geometry = stream.at("geometry_identity");
        require(geometry.at("encoded_content_rect").at("x") == 0 &&
                    geometry.at("encoded_content_rect").at("y") == 0 &&
                    geometry.at("encoded_content_rect").at("width") ==
                        geometry.at("content_rect").at("width") &&
                    geometry.at("encoded_content_rect").at("height") ==
                        geometry.at("content_rect").at("height"),
                "encoded content must be origin-anchored, not camera-relative");
        require(stream.at("geometry_identity").at("padding").at("left") == 0 &&
                    stream.at("geometry_identity").at("padding").at("top") == 0,
                "schema v2 must make zero left/top padding explicit");
        require(stream.at("geometry_identity").at("padding").at("value_mono8") ==
                    0,
                "padding value must be zero");
        require(stream.at("frame_identity").at("roi_stream_frame_index") ==
                    "dense_one_based",
                "ROI-local frame indices must match the frame contract");
        require(stream.at("frame_identity").at("key_fields") ==
                    nlohmann::json{
                        "recording_identity_token",
                        "producer_generation",
                        "logical_stream_id",
                        "recording_frame_id",
                        "roi_stream_frame_index",
                    },
                "frame identity key fields must include every cross-session dimension");
        require(stream.at("expected_artifacts").at("video").get<std::string>()
                    .find("/tmp/orange_roi_contract_test/") == 0,
                "video artifact must be rooted under recording_root");
        require(stream.at("expected_artifacts").contains("status") &&
                    stream.at("expected_artifacts").contains("video_sanity"),
                "strict recorder contract must name status and sanity artifacts");
        require(stream.at("expected_artifacts").contains("recorder_log") &&
                    stream.at("expected_artifacts").contains("transport_sidecar"),
                "strict recorder contract must name recorder log and transport sidecar");
        require(stream.at("expected_artifacts").contains("evidence") &&
                    stream.at("expected_artifacts").contains("evidence_manifest"),
                "strict recorder contract must name evidence and finalized manifest");
        require(stream.at("recorder_log") ==
                    stream.at("expected_artifacts").at("recorder_log") &&
                    stream.at("transport_sidecar") ==
                        stream.at("expected_artifacts").at("transport_sidecar"),
                "stream recorder log and transport sidecar paths must be canonical");
        require(stream.at("evidence_jsonl") ==
                    stream.at("expected_artifacts").at("evidence") &&
                    stream.at("evidence_manifest_json") ==
                        stream.at("expected_artifacts").at("evidence_manifest"),
                "stream evidence paths must be canonical");
        require(recorder_log_paths.insert(
                    stream.at("recorder_log").get<std::string>()).second,
                "recorder log paths must be unique per logical stream");
        require(transport_sidecar_paths.insert(
                    stream.at("transport_sidecar").get<std::string>()).second,
                "transport sidecar paths must be unique per logical stream");
        require(evidence_paths.insert(
                    stream.at("evidence_jsonl").get<std::string>()).second,
                "evidence paths must be unique per logical stream");
        require(evidence_manifest_paths.insert(
                    stream.at("evidence_manifest_json").get<std::string>()).second,
                "evidence manifest paths must be unique per logical stream");
        require(stream.at("recorder_log").get<std::string>().find(
                    "/tmp/orange_roi_contract_test/") == 0 &&
                    stream.at("transport_sidecar").get<std::string>().find(
                        "/tmp/orange_roi_contract_test/") == 0,
                "recorder log and transport sidecar must be rooted under recording_root");
        stream_ids.push_back(stream_id);
    }
    require(contract.at("stream_order") == stream_ids,
            "stream order does not exactly preserve verified plan order");
    require(recorder_log_paths.size() == stream_ids.size() &&
                transport_sidecar_paths.size() == stream_ids.size() &&
                evidence_paths.size() == stream_ids.size() &&
                evidence_manifest_paths.size() == stream_ids.size(),
            "each logical stream must have unique log, transport, and evidence paths");
    require(contract.value("require_gop_routing", true) == false,
            "independent per-ROI GOP streams must not require shard routing");
}

void rejects_missing_extra_and_negative_gpu_mappings()
{
    const nlohmann::json plan = make_plan();
    std::string error;
    nlohmann::json contract = {{"sentinel", true}};

    auto mapping = make_mapping();
    mapping.recorder_gpu_by_logical_stream_id.erase(
        spatial_roi::expected_logical_stream_id("2010096", "roi_4"));
    require(!spatial_roi::build_spatial_roi_recorder_contract(
                plan, "/tmp/orange_roi_contract_test", mapping, &contract, &error),
            "missing recorder GPU mapping must fail");
    require(contract == nlohmann::json{{"sentinel", true}},
            "failed build must not publish a partial contract");

    mapping = make_mapping();
    mapping.recorder_gpu_by_logical_stream_id.emplace("unknown_stream", 6);
    require(!spatial_roi::build_spatial_roi_recorder_contract(
                plan, "/tmp/orange_roi_contract_test", mapping, &contract, &error),
            "extra recorder GPU mapping must fail");

    mapping = make_mapping();
    mapping.analytics_gpu_by_camera_serial["2010096"] = -1;
    require(!spatial_roi::build_spatial_roi_recorder_contract(
                plan, "/tmp/orange_roi_contract_test", mapping, &contract, &error),
            "negative analytics GPU mapping must fail");
}

void rejects_bad_root_and_tampered_or_duplicate_plan()
{
    const nlohmann::json plan = make_plan();
    const auto mapping = make_mapping();
    nlohmann::json contract;
    std::string error;

    require(!spatial_roi::build_spatial_roi_recorder_contract(
                plan, "relative/recording", mapping, &contract, &error),
            "relative recording root must fail");
    require(!spatial_roi::build_spatial_roi_recorder_contract(
                plan, "/", mapping, &contract, &error),
            "filesystem root must fail");

    nlohmann::json tampered = plan;
    tampered["plan"]["producer_generation"] = "different_generation";
    require(!spatial_roi::build_spatial_roi_recorder_contract(
                tampered, "/tmp/orange_roi_contract_test", mapping, &contract, &error),
            "tampered plan must fail digest verification");

    tampered = plan;
    tampered["plan"]["resolved_cameras"]["2010096"]["rois"][1]["roi_id"] =
        "roi_1";
    require(!spatial_roi::build_spatial_roi_recorder_contract(
                tampered, "/tmp/orange_roi_contract_test", mapping, &contract, &error),
            "duplicate ROI identity must fail plan verification");

    tampered = plan;
    tampered["plan"]["configuration"]["recording_limits"]
            ["max_frames_per_stream"] = 1001;
    require(!spatial_roi::build_spatial_roi_recorder_contract(
                tampered, "/tmp/orange_roi_contract_test", mapping, &contract, &error),
            "mutated authenticated recording limit must fail plan verification");

    const nlohmann::json odd_nv12_plan = make_plan(1);
    require(!spatial_roi::build_spatial_roi_recorder_contract(
                odd_nv12_plan,
                "/tmp/orange_roi_contract_test",
                mapping,
                &contract,
                &error),
            "odd encoded ROI dimensions must fail NV12/HEVC materialization");

    spatial_roi::SpatialRoiRecorderRuntimeGpuMapping oversized_mapping;
    oversized_mapping.analytics_gpu_by_camera_serial.emplace("2010096", 5);
    oversized_mapping.recorder_gpu_by_logical_stream_id.emplace(
        spatial_roi::expected_logical_stream_id("2010096", "roi_1"), 6);
    require(!spatial_roi::build_spatial_roi_recorder_contract(
                make_oversized_packed_roi_plan(),
                "/tmp/orange_roi_contract_test",
                oversized_mapping,
                &contract,
                &error),
            "ROI larger than the IPC-v2 packed Mono8 bound must fail materialization");
}

}  // namespace

int main()
{
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"validates_normative_ipc_v2_schema",
         validates_normative_ipc_v2_schema},
        {"builds_one_strict_nonrolling_stream_per_roi",
         builds_one_strict_nonrolling_stream_per_roi},
        {"rejects_missing_extra_and_negative_gpu_mappings",
         rejects_missing_extra_and_negative_gpu_mappings},
        {"rejects_bad_root_and_tampered_or_duplicate_plan",
         rejects_bad_root_and_tampered_or_duplicate_plan},
    };

    int failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
        }
    }
    if (failures != 0) {
        std::cerr << failures << " spatial ROI recorder contract test(s) failed\n";
        return 1;
    }
    std::cout << tests.size() << " spatial ROI recorder contract tests passed\n";
    return 0;
}
