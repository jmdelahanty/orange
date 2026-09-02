#include "session/spatial_roi_recorder_camera_contract.h"

#include "session/spatial_roi_recording_config.h"
#include "shaman_v2_recording_identity.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using json = nlohmann::json;
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
                                std::size_t index)
{
    const std::string suffix = std::to_string(index + 1);
    spatial_roi::RoiConfig roi;
    roi.roi_id = "roi_" + suffix;
    roi.region_id = "region_" + suffix;
    roi.has_arena_id = true;
    roi.arena_id = "arena_" + suffix;
    roi.required = true;
    roi.content_rect = {
        static_cast<std::uint32_t>(index * 15), 0, 10, 10};
    roi.logical_stream_id =
        spatial_roi::expected_logical_stream_id(camera_serial, roi.roi_id);
    roi.artifact_stem =
        spatial_roi::expected_artifact_stem(camera_serial, roi.roi_id);
    return roi;
}

struct Fixture {
    json plan;
    spatial_roi::SpatialRoiRecorderRuntimeGpuMapping mapping;
    json contract;
};

Fixture make_fixture(std::size_t camera_count = 1,
                     std::size_t rois_per_camera = 4)
{
    spatial_roi::Config config = spatial_roi::default_config();
    config.enabled = true;
    config.buffering.pool_frames_per_stream = 4;
    config.buffering.queue_frames_per_stream = 8;
    config.recording_limits.max_frames_per_stream = 2000;
    config.recording_limits.max_media_bytes_per_stream = 2000000;
    config.recording_limits.max_evidence_bytes_per_stream = 200000;
    config.admission.max_rois_per_camera = 8;
    config.admission.max_total_rois = 16;
    config.admission.max_total_encoder_streams = 16;
    config.admission.max_total_pixel_rate = 100000000ULL;
    config.admission.max_total_pool_bytes = 100000000ULL;
    config.admission.max_total_queue_frames = 128;
    config.admission.max_total_media_bytes = 32000000;
    config.admission.max_total_evidence_bytes = 3200000;

    for (std::size_t camera_index = 0; camera_index < camera_count;
         ++camera_index) {
        spatial_roi::CameraConfig camera;
        camera.camera_id = static_cast<int>(3 + camera_index);
        camera.camera_serial = "201009" + std::to_string(6 + camera_index);
        camera.native_raster = {100, 100};
        camera.source_frame_rate = 100;
        camera.arena_group_id =
            "group_" + std::to_string(camera_index + 1);
        camera.layout = {"layout_" + std::to_string(camera_index + 1),
                         digest(static_cast<char>('1' + camera_index))};
        camera.materialization = {
            "materialization_" + std::to_string(camera_index + 1),
            digest(static_cast<char>('a' + camera_index))};
        camera.registration = {
            "registration_" + std::to_string(camera_index + 1),
            digest(static_cast<char>('c' + camera_index))};
        for (std::size_t roi_index = 0; roi_index < rois_per_camera;
             ++roi_index) {
            camera.rois.push_back(
                make_roi(camera.camera_serial, roi_index));
        }
        config.cameras.emplace(camera.camera_serial, std::move(camera));
    }

    spatial_roi::PlanContext context;
    context.recording_id = "camera-contract-test";
    context.recording_identity_token =
        orange::shaman_v2_recording_identity::token_for_recording_id(
            context.recording_id);
    context.generated_at_utc = "2026-08-31T00:00:00Z";
    context.producer_generation = "generation_camera_contract";

    Fixture fixture;
    std::string error;
    require(spatial_roi::build_plan(
                config, context, &fixture.plan, nullptr, &error),
            error);
    for (std::size_t camera_index = 0; camera_index < camera_count;
         ++camera_index) {
        const std::string serial =
            "201009" + std::to_string(6 + camera_index);
        fixture.mapping.analytics_gpu_by_camera_serial.emplace(
            serial, static_cast<int>(5 + camera_index));
        for (std::size_t roi_index = 0; roi_index < rois_per_camera;
             ++roi_index) {
            const std::string roi_id =
                spatial_roi::expected_logical_stream_id(
                    serial, "roi_" + std::to_string(roi_index + 1));
            fixture.mapping.recorder_gpu_by_logical_stream_id.emplace(
                roi_id, static_cast<int>(6 + roi_index));
        }
    }
    require(spatial_roi::build_spatial_roi_recorder_contract(
                fixture.plan,
                "/tmp/orange_camera_contract_test",
                fixture.mapping,
                &fixture.contract,
                &error),
            error);
    return fixture;
}

bool parses(const Fixture& fixture,
            const json& candidate,
            spatial_roi::SpatialRoiRecorderCameraContractView* view,
            std::string* error)
{
    return spatial_roi::parse_spatial_roi_recorder_camera_contract(
        candidate,
        fixture.plan,
        "/tmp/orange_camera_contract_test",
        fixture.mapping,
        view,
        error);
}

void require_rejected(const Fixture& fixture,
                      const json& candidate,
                      const std::string& message)
{
    spatial_roi::SpatialRoiRecorderCameraContractView view;
    std::string error;
    require(!parses(fixture, candidate, &view, &error), message);
    require(!error.empty(), message + " produced no diagnostic");
}

void accepts_exactly_four_fixed_regions_in_verified_order()
{
    const Fixture fixture = make_fixture();
    spatial_roi::SpatialRoiRecorderCameraContractView view;
    std::string error;
    require(parses(fixture, fixture.contract, &view, &error), error);
    require(view.schema_id ==
                spatial_roi::kSpatialRoiRecorderCameraContractSchemaId &&
                view.schema_version ==
                    spatial_roi::kSpatialRoiRecorderCameraContractSchemaVersion &&
                view.product_kind ==
                    spatial_roi::kSpatialRoiRecorderCameraProductKind,
            "camera-level view schema/product mismatch");
    require(view.stream_count == 4 && view.streams.size() == 4 &&
                view.stream_order.size() == 4,
            "camera-level view did not expose four streams");
    require(view.camera_id == 3 && view.camera_serial == "2010096" &&
                view.native_raster.width == 100 &&
                view.native_raster.height == 100 && view.analytics_gpu_id == 5,
            "camera identity/raster/GPU mismatch");
    require(view.stream_order ==
                std::vector<std::string>{
                    "2010096_spatial_roi_roi_1",
                    "2010096_spatial_roi_roi_2",
                    "2010096_spatial_roi_roi_3",
                    "2010096_spatial_roi_roi_4"},
            "view order is not the authenticated plan order");
    for (std::size_t index = 0; index < view.streams.size(); ++index) {
        require(view.streams[index].logical_stream_id ==
                    view.stream_order[index] &&
                    view.streams[index].stream_kind == "spatial_roi" &&
                    view.streams[index].output_kind == "spatial_roi",
                "stream vector is not four fixed-region outputs in order");
    }
    require(view.analytics_gpu_by_camera_serial.size() == 1 &&
                view.recorder_gpu_by_logical_stream_id.size() == 4,
            "camera GPU maps were not retained as authenticated maps");
}

void rejects_shuffled_order_and_candidate_arrays()
{
    const Fixture fixture = make_fixture();
    json candidate = fixture.contract;
    std::reverse(candidate["stream_order"].begin(),
                 candidate["stream_order"].end());
    require_rejected(fixture,
                     candidate,
                     "shuffled candidate stream_order was accepted");

    candidate = fixture.contract;
    json streams = json::array();
    for (const auto& stream_id_value : fixture.contract["stream_order"]) {
        streams.push_back(
            fixture.contract["streams"].at(stream_id_value.get<std::string>()));
    }
    candidate["streams"] = std::move(streams);
    require_rejected(fixture,
                     candidate,
                     "candidate stream array bypassed closed contract");
}

void rejects_wrong_camera_and_stream_counts()
{
    const Fixture second_camera = make_fixture(2, 4);
    require_rejected(second_camera,
                     second_camera.contract,
                     "second camera was accepted by first slice");

    const Fixture three_streams = make_fixture(1, 3);
    require_rejected(three_streams,
                     three_streams.contract,
                     "three streams were accepted by first slice");

    const Fixture five_streams = make_fixture(1, 5);
    require_rejected(five_streams,
                     five_streams.contract,
                     "five streams were accepted by first slice");
}

void rejects_duplicate_collisions_and_mutated_fields()
{
    const Fixture fixture = make_fixture();
    const std::string first = fixture.contract["stream_order"].at(0);
    const std::string second = fixture.contract["stream_order"].at(1);

    json candidate = fixture.contract;
    candidate["streams"].at(second)["region_id"] =
        candidate["streams"].at(first)["region_id"];
    require_rejected(fixture,
                     candidate,
                     "duplicate region identity was accepted");

    candidate = fixture.contract;
    candidate["streams"].at(second)["socket_path"] =
        candidate["streams"].at(first)["socket_path"];
    require_rejected(fixture,
                     candidate,
                     "duplicate socket path was accepted");

    candidate = fixture.contract;
    candidate["streams"].at(second)["mp4"] =
        candidate["streams"].at(first)["mp4"];
    require_rejected(fixture,
                     candidate,
                     "colliding artifact path was accepted");

    candidate = fixture.contract;
    candidate["recording_id"] = "other-recording";
    require_rejected(fixture,
                     candidate,
                     "mutated parent recording identity was accepted");

    candidate = fixture.contract;
    candidate["streams"].at(first)["camera_serial"] = "other-camera";
    require_rejected(fixture,
                     candidate,
                     "mutated stream camera identity was accepted");

    candidate = fixture.contract;
    candidate["streams"].at(first)["geometry_identity"]["native_raster"]["width"] =
        101;
    require_rejected(fixture,
                     candidate,
                     "mutated stream raster was accepted");
}

}  // namespace

int main()
{
    try {
        accepts_exactly_four_fixed_regions_in_verified_order();
        rejects_shuffled_order_and_candidate_arrays();
        rejects_wrong_camera_and_stream_counts();
        rejects_duplicate_collisions_and_mutated_fields();
        std::cout << "spatial ROI recorder camera contract tests passed\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "spatial ROI recorder camera contract tests failed: "
                  << ex.what() << '\n';
        return 1;
    }
}
