#include "recording_output_descriptor.h"
#include "session/spatial_roi_recording_outputs.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using orange::session::RecordingOutputDescriptor;
using orange::session::spatial_roi::SpatialRoiRecorderArtifactPathView;
using orange::session::spatial_roi::SpatialRoiRecorderCameraContractView;
using orange::session::spatial_roi::SpatialRoiRecorderStreamView;

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void set_profile(SpatialRoiRecorderCameraContractView* view,
                 const std::string& profile_id,
                 const std::string& preset,
                 const std::string& tuning,
                 const bool lossless,
                 const std::string& rate_control_mode,
                 const std::uint32_t quality_value,
                 const std::uint32_t gop_length)
{
    require(view != nullptr, "profile test view is null");
    for (SpatialRoiRecorderStreamView& stream : view->streams) {
        stream.encode_profile.profile_id = profile_id;
        stream.encode_profile.codec = "hevc";
        stream.encode_profile.preset = preset;
        stream.encode_profile.tuning = tuning;
        stream.encode_profile.lossless = lossless;
        stream.encode_profile.rate_control_mode = rate_control_mode;
        stream.encode_profile.quality_value = quality_value;
        stream.encode_profile.gop_length = gop_length;
        stream.encode_profile.frame_rate = 100;
        stream.encode_profile.input_format = "mono8";
        stream.encode_profile.encoded_format = "nv12";
        stream.encode_profile.no_resize = true;
        stream.encode_profile.luma_preserved_exactly = lossless;
        stream.encode_profile.neutral_chroma_value = 128;
        stream.encode_fps = 100;
        stream.codec = "hevc";
        stream.tuning = tuning;
        stream.rate_control_mode = rate_control_mode;
        stream.quality_value = quality_value;
        stream.gop = gop_length;
    }
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
    view.recording_identity_token = "sha256:" + std::string(64, 'a');
    view.producer_generation = "generation_001";
    view.spatial_roi_plan_sha256 = "sha256:" + std::string(64, 'b');
    view.recording_root = "/tmp/spatial_roi_output_builder_recording";
    view.artifact_root = view.recording_root + "/external_spatial_roi_recorder";
    view.camera_id = 7;
    view.camera_serial = "2010096";
    view.native_raster = {640, 480};
    view.analytics_gpu_id = 2;
    view.stream_count = 4;
    view.analytics_gpu_by_camera_serial[view.camera_serial] =
        view.analytics_gpu_id;

    const std::array<const char*, 12> artifact_suffixes = {
        ".mp4",
        "_meta.csv",
        "_keyframe.json",
        "_perf.csv",
        "_summary.json",
        "_status.json",
        "_video_sanity.json",
        ".mp4.finalization.json",
        "_recorder.log",
        "_transport.jsonl",
        "_evidence.jsonl",
        "_evidence_manifest.json"};
    const std::array<const char*, 12> artifact_kinds = {
        "video",
        "metadata",
        "keyframes",
        "perf",
        "summary",
        "status",
        "video_sanity",
        "finalization",
        "recorder_log",
        "transport_sidecar",
        "evidence",
        "evidence_manifest"};

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
        stream.geometry.native_raster = view.native_raster;
        stream.geometry.content_rect = {
            static_cast<std::uint32_t>((index - 1) * 32),
            static_cast<std::uint32_t>((index - 1) * 24),
            24,
            24};
        stream.geometry.encoded_raster = {26, 26};
        stream.geometry.encoded_content_rect = {0, 0, 24, 24};
        stream.geometry.content_offset_x = 0;
        stream.geometry.content_offset_y = 0;
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
        stream.rate_control_mode = "cqp";
        stream.quality_value = 0;
        stream.gop = 1;
        stream.encode_queue_depth = 64;
        stream.routing_policy = "single_shard";
        stream.expected_shard_gpu_ids = {stream.recorder_gpu_id};

        const std::string stem = "Cam" + view.camera_serial +
                                 "_spatial_roi_roi_" + std::to_string(index);
        for (std::size_t artifact_index = 0;
             artifact_index < artifact_kinds.size();
             ++artifact_index) {
            const std::string relative =
                stem + artifact_suffixes.at(artifact_index);
            stream.artifacts[artifact_kinds.at(artifact_index)] = {
                view.artifact_root + "/" + relative,
                relative};
        }
        view.streams.push_back(std::move(stream));
    }
    return view;
}

void test_builds_four_ordered_relative_outputs()
{
    const SpatialRoiRecorderCameraContractView view = make_camera_contract();
    std::vector<RecordingOutputDescriptor> outputs;
    std::string error;
    require(orange::session::spatial_roi::build_spatial_roi_recording_outputs(
                view, "pending", &outputs, &error),
            "valid camera contract should build outputs: " + error);
    require(outputs.size() == 4, "builder must emit exactly four outputs");

    for (std::size_t index = 0; index < outputs.size(); ++index) {
        const RecordingOutputDescriptor& output = outputs.at(index);
        const std::string expected_id = view.stream_order.at(index);
        require(output.logical_stream_id == expected_id,
                "output order must follow authenticated stream_order");
        require(output.camera_serial == view.camera_serial &&
                    output.output_kind == "spatial_roi" &&
                    output.role == "runtime_derived_acquisition_input" &&
                    output.backend == "external_ipc" &&
                    output.status == "pending",
                "output identity/role/backend/status");
        require(output.width == 26 && output.height == 26 &&
                    output.frame_rate == 100 && output.codec == "hevc" &&
                    output.container == "mp4" && output.tuning == "lossless" &&
                    output.pixel_source_format == "mono8" &&
                    output.encoded_format == "nv12",
                "output dimensions and encoding profile");
        const auto& profile = output.details.at("encode_profile");
        require(profile.is_object() && profile.size() == 18 &&
                    profile.at("profile_id") ==
                        "hevc_p7_lossless_cqp0_gop1_v1" &&
                    profile.at("codec") == "hevc" &&
                    profile.at("preset") == "p7" &&
                    profile.at("tuning") == "lossless" &&
                    profile.at("lossless") == true &&
                    profile.at("rate_control_mode") == "cqp" &&
                    profile.at("quality_value") == 0 &&
                    profile.at("gop_length") == 1 &&
                    profile.at("aq") == false &&
                    profile.at("temporal_aq") == false &&
                    profile.at("lookahead") == false &&
                    profile.at("lookahead_depth") == 0 &&
                    profile.at("frame_rate") == 100 &&
                    profile.at("input_format") == "mono8" &&
                    profile.at("encoded_format") == "nv12" &&
                    profile.at("no_resize") == true &&
                    profile.at("luma_preserved_exactly") == true &&
                    profile.at("neutral_chroma_value") == 128,
                "details must carry the complete lossless profile");
        require(output.video_path.rfind("external_spatial_roi_recorder/", 0) ==
                    0 &&
                    output.metadata_path.rfind(
                        "external_spatial_roi_recorder/", 0) == 0 &&
                    output.keyframe_path.rfind(
                        "external_spatial_roi_recorder/", 0) == 0 &&
                    output.perf_path.rfind(
                        "external_spatial_roi_recorder/", 0) == 0 &&
                    output.summary_path.rfind(
                        "external_spatial_roi_recorder/", 0) == 0,
                "top-level artifact paths must be recording-root relative");
        require(output.coordinate_space ==
                        "camera_native_full_frame_pixels" &&
                    output.source_geometry_coordinate_space ==
                        "camera_native_full_frame_pixels" &&
                    output.video_pixel_coordinate_space ==
                        "spatial_roi_encoded_pixels",
                "output must carry both coordinate spaces");
        require(output.details.at("recording_identity_token") ==
                    view.recording_identity_token &&
                    output.details.at("logical_stream_id") == expected_id &&
                    output.details.at("producer_generation") ==
                        view.producer_generation &&
                    output.details.at("spatial_roi_plan_sha256") ==
                        view.spatial_roi_plan_sha256 &&
                    output.details.at("camera_id") == view.camera_id,
                "output must carry recording and camera identity");
        require(output.details.at("artifacts").size() == 12,
                "output details must carry all twelve artifacts");
        for (const auto& artifact : output.details.at("artifacts")) {
            require(artifact.is_string() &&
                        !artifact.get<std::string>().empty() &&
                        artifact.get<std::string>().front() != '/',
                    "artifact details must not emit absolute paths");
        }
        const std::string serialized =
            orange::session::build_recording_output_descriptor_json(output)
                .dump();
        require(serialized.find(view.recording_root) == std::string::npos &&
                    serialized.find(view.artifact_root) == std::string::npos,
                "descriptor must not emit recording or artifact roots");
        require(output.details.at("source_geometry").at("coordinate_space") ==
                    "camera_native_full_frame_pixels" &&
                    output.details.at("encoded_geometry")
                            .at("coordinate_space") ==
                        "spatial_roi_encoded_pixels",
                "details must preserve source and encoded geometry");
    }
}

void test_accepts_gop25_and_does_not_claim_exact_luma()
{
    SpatialRoiRecorderCameraContractView view = make_camera_contract();
    set_profile(&view,
                "hevc_p1_low_latency_vbr_q20_gop25_v1",
                "p1",
                "ll",
                false,
                "vbr",
                20,
                25);

    std::vector<RecordingOutputDescriptor> outputs;
    std::string error;
    require(orange::session::spatial_roi::build_spatial_roi_recording_outputs(
                view, "pending", &outputs, &error),
            "GOP25 camera contract should build outputs: " + error);
    require(outputs.size() == 4, "GOP25 builder must emit four outputs");
    for (const RecordingOutputDescriptor& output : outputs) {
        const auto& profile = output.details.at("encode_profile");
        require(profile.size() == 18 &&
                    profile.at("profile_id") ==
                        "hevc_p1_low_latency_vbr_q20_gop25_v1" &&
                    profile.at("preset") == "p1" &&
                    profile.at("tuning") == "ll" &&
                    profile.at("lossless") == false &&
                    profile.at("rate_control_mode") == "vbr" &&
                    profile.at("quality_value") == 20 &&
                    profile.at("gop_length") == 25 &&
                    profile.at("aq") == false &&
                    profile.at("temporal_aq") == false &&
                    profile.at("lookahead") == false &&
                    profile.at("lookahead_depth") == 0 &&
                    profile.at("luma_preserved_exactly") == false,
                "GOP25 profile projection is incomplete or overclaims luma");
    }
}

void test_preserves_legacy_p1_gop1_profile()
{
    SpatialRoiRecorderCameraContractView view = make_camera_contract();
    set_profile(&view,
                "hevc_p1_low_latency_vbr_q20_gop1_v1",
                "p1",
                "ll",
                false,
                "vbr",
                20,
                1);

    std::vector<RecordingOutputDescriptor> outputs;
    std::string error;
    require(orange::session::spatial_roi::build_spatial_roi_recording_outputs(
                view, "complete", &outputs, &error),
            "legacy P1 GOP1 camera contract should build outputs: " + error);
    require(outputs.size() == 4, "legacy P1 GOP1 builder must emit four outputs");
    require(outputs.front().details.at("encode_profile").at("profile_id") ==
                "hevc_p1_low_latency_vbr_q20_gop1_v1" &&
            outputs.front().details.at("encode_profile").at("gop_length") == 1 &&
            outputs.front().details.at("encode_profile").at(
                "luma_preserved_exactly") == false,
            "legacy P1 GOP1 profile was not preserved exactly");
}

void test_rejects_lossy_exact_luma_claim()
{
    SpatialRoiRecorderCameraContractView invalid = make_camera_contract();
    set_profile(&invalid,
                "hevc_p1_low_latency_vbr_q20_gop25_v1",
                "p1",
                "ll",
                false,
                "vbr",
                20,
                25);
    invalid.streams.front().encode_profile.luma_preserved_exactly = true;

    std::vector<RecordingOutputDescriptor> outputs;
    std::string error;
    require(!orange::session::spatial_roi::build_spatial_roi_recording_outputs(
                invalid, "pending", &outputs, &error) && outputs.empty(),
            "lossy GOP25 profile must not claim exact luma");
}

void test_rejects_invalid_identity_status_and_artifacts()
{
    SpatialRoiRecorderCameraContractView invalid = make_camera_contract();
    invalid.streams[1].camera_serial = "other-camera";
    std::vector<RecordingOutputDescriptor> outputs;
    std::string error;
    require(!orange::session::spatial_roi::build_spatial_roi_recording_outputs(
                invalid, "complete", &outputs, &error) && outputs.empty(),
            "camera identity mismatch must fail closed");

    invalid = make_camera_contract();
    invalid.streams[0].artifacts.erase("summary");
    require(!orange::session::spatial_roi::build_spatial_roi_recording_outputs(
                invalid, "complete", &outputs, &error) && outputs.empty(),
            "missing exact artifact kind must fail closed");

    invalid = make_camera_contract();
    require(!orange::session::spatial_roi::build_spatial_roi_recording_outputs(
                invalid, "completed", &outputs, &error) && outputs.empty(),
            "unsupported status must fail closed");

    invalid = make_camera_contract();
    invalid.stream_order[0] = invalid.stream_order[1];
    require(!orange::session::spatial_roi::build_spatial_roi_recording_outputs(
                invalid, "failed", &outputs, &error) && outputs.empty(),
            "duplicate authenticated stream order must fail closed");
}

}  // namespace

int main()
{
    try {
        test_builds_four_ordered_relative_outputs();
        std::cout << "[PASS] builds_four_ordered_relative_outputs\n";
        test_accepts_gop25_and_does_not_claim_exact_luma();
        std::cout << "[PASS] accepts_gop25_and_does_not_claim_exact_luma\n";
        test_preserves_legacy_p1_gop1_profile();
        std::cout << "[PASS] preserves_legacy_p1_gop1_profile\n";
        test_rejects_lossy_exact_luma_claim();
        std::cout << "[PASS] rejects_lossy_exact_luma_claim\n";
        test_rejects_invalid_identity_status_and_artifacts();
        std::cout << "[PASS] rejects_invalid_identity_status_and_artifacts\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "[FAIL] spatial_roi_recording_outputs_tests: "
                  << exception.what() << '\n';
        return 1;
    }
}
