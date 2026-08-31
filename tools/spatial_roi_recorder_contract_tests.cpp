#include "session/spatial_roi_recorder_contract.h"

#include "session/spatial_roi_recording_config.h"
#include "shaman_v2_recording_identity.h"

#include <functional>
#include <iostream>
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
    config.admission.max_rois_per_camera = 8;
    config.admission.max_total_rois = 8;
    config.admission.max_total_encoder_streams = 8;
    config.admission.max_total_pixel_rate = 100000000ULL;
    config.admission.max_total_pool_bytes = 100000000ULL;
    config.admission.max_total_queue_frames = 128;

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
                "independent_lossless_external_ipc",
            "contract backend mismatch");
    require(contract.value("mode", std::string()) ==
                "spatial_roi_external_ipc_v1",
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

    std::vector<std::string> stream_ids;
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
        require(stream.at("encode_profile").value("codec", std::string()) ==
                    "hevc",
                "codec must be HEVC");
        require(stream.at("encode_profile").value("tuning", std::string()) ==
                    "lossless",
                "tuning must be lossless");
        require(stream.at("encode_profile").value("lossless", false),
                "lossless flag must be true");
        require(stream.at("encode_profile").value("gop_length", 0) == 1,
                "spatial ROI GOP must be one");
        require(stream.at("encode_profile").value("frame_rate", 0) == 100,
                "source frame rate mismatch");
        require(stream.value("codec", std::string()) == "hevc" &&
                    stream.value("tuning", std::string()) == "lossless" &&
                    stream.value("gop", 0) == 1,
                "direct recorder encode profile must be HEVC/lossless/GOP1");
        require(stream.value("rate_control_mode", std::string()) == "cqp",
                "recorder rate-control spelling must match the supported profile");
        require(stream.value("encode_queue_depth", 0) == 8,
                "verified per-stream queue bound was not propagated");
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
                "schema v1 must make zero left/top padding explicit");
        require(stream.at("geometry_identity").at("padding").at("value_mono8") ==
                    0,
                "padding value must be zero");
        require(stream.at("frame_identity").at("roi_stream_frame_index") ==
                    "dense_one_based",
                "ROI-local frame indices must match the frame contract");
        require(stream.at("expected_artifacts").at("video").get<std::string>()
                    .find("/tmp/orange_roi_contract_test/") == 0,
                "video artifact must be rooted under recording_root");
        require(stream.at("expected_artifacts").contains("status") &&
                    stream.at("expected_artifacts").contains("video_sanity"),
                "strict recorder contract must name status and sanity artifacts");
        stream_ids.push_back(stream_id);
    }
    require(contract.at("stream_order") == stream_ids,
            "stream order does not exactly preserve verified plan order");
    require(contract.value("require_gop_routing", true) == false,
            "independent GOP-1 ROI streams must not require shard routing");
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

    const nlohmann::json odd_nv12_plan = make_plan(1);
    require(!spatial_roi::build_spatial_roi_recorder_contract(
                odd_nv12_plan,
                "/tmp/orange_roi_contract_test",
                mapping,
                &contract,
                &error),
            "odd encoded ROI dimensions must fail NV12/HEVC materialization");
}

}  // namespace

int main()
{
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
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
