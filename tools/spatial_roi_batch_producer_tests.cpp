#include "spatial_roi_batch_producer.h"

#include "session/spatial_roi_recording_config.h"
#include "shaman_v2_recording_identity.h"

#include <cuda_runtime.h>

#include <atomic>
#include <algorithm>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using orange::spatial_roi::SpatialRoiBatchLimits;
using orange::spatial_roi::SpatialRoiBatchProducer;
using orange::spatial_roi::SpatialRoiBatchResult;
using orange::spatial_roi::SpatialRoiBatchStatus;
using orange::spatial_roi::SpatialRoiFrameIdentity;
using orange::spatial_roi::SpatialRoiOutputGeometry;
using orange::spatial_roi::SpatialRoiRaster;
using orange::spatial_roi::SpatialRoiRect;
using orange::spatial_roi::SpatialRoiSourceView;
using orange::spatial_roi::SpatialRoiSourcePixelFormat;
using orange::spatial_roi::SpatialRoiWorkItem;
using orange::spatial_roi::validate_spatial_roi_batch;

std::shared_ptr<void> make_source_lease()
{
    // The producer treats this as an opaque ownership token. A real
    // acquisition integration will substitute its WORKER_ENTRY lease.
    return std::make_shared<int>(0);
}

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void require_cuda(cudaError_t status, const char* operation)
{
    if (status != cudaSuccess) {
        throw std::runtime_error(
            std::string(operation) + " failed: " + cudaGetErrorString(status));
    }
}

SpatialRoiFrameIdentity make_identity(std::uint64_t recording_frame_id = 17)
{
    SpatialRoiFrameIdentity identity;
    identity.recording_id = "recording-test";
    identity.recording_identity_token =
        orange::shaman_v2_recording_identity::token_for_recording_id(
            identity.recording_id);
    identity.producer_generation = "generation_1";
    identity.camera_id = 3;
    identity.camera_serial = "10000";
    identity.local_frame_id = 91;
    identity.camera_frame_id = 7001;
    identity.recording_frame_id = recording_frame_id;
    identity.camera_timestamp_ns = 123456789;
    identity.timestamp_sys_ns = 987654321;
    return identity;
}

SpatialRoiWorkItem make_item(
    std::string roi_id,
    std::string region_id,
    SpatialRoiRect content_rect,
    SpatialRoiRaster encoded_raster,
    SpatialRoiRect encoded_content_rect)
{
    SpatialRoiWorkItem item;
    item.roi_id = std::move(roi_id);
    item.region_id = std::move(region_id);
    item.arena_group_id = "arena_group_1";
    item.logical_stream_id =
        "10000_spatial_roi_" + item.roi_id;
    item.spatial_roi_plan_sha256 = "sha256:" + std::string(64, 'b');
    item.source = make_identity();
    item.geometry.content_rect = content_rect;
    item.geometry.encoded_raster = encoded_raster;
    item.geometry.encoded_content_rect = encoded_content_rect;
    return item;
}

std::vector<SpatialRoiWorkItem> make_four_items()
{
    return {
        make_item(
            "roi_1",
            "region_1",
            SpatialRoiRect{1, 1, 3, 2},
            SpatialRoiRaster{4, 4},
            SpatialRoiRect{0, 0, 3, 2}),
        make_item(
            "roi_2",
            "region_2",
            SpatialRoiRect{4, 0, 2, 3},
            SpatialRoiRaster{4, 4},
            SpatialRoiRect{1, 1, 2, 3}),
        make_item(
            "roi_3",
            "region_3",
            SpatialRoiRect{0, 3, 4, 2},
            SpatialRoiRaster{6, 2},
            SpatialRoiRect{1, 0, 4, 2}),
        make_item(
            "roi_4",
            "region_4",
            SpatialRoiRect{7, 5, 1, 1},
            SpatialRoiRaster{2, 2},
            SpatialRoiRect{1, 1, 1, 1}),
    };
}

SpatialRoiBatchLimits make_limits(int gpu_id = 0)
{
    const std::vector<SpatialRoiWorkItem> items = make_four_items();
    SpatialRoiBatchLimits limits;
    limits.gpu_id = gpu_id;
    limits.batch_slot_count = 2;
    limits.max_rois_per_batch = 4;
    limits.pool_frames_per_stream = 2;
    limits.expected_recording_id = make_identity().recording_id;
    limits.expected_recording_identity_token =
        make_identity().recording_identity_token;
    limits.expected_producer_generation = make_identity().producer_generation;
    limits.expected_camera_id = 3;
    limits.expected_camera_serial = "10000";
    limits.expected_native_raster = {8, 6};
    limits.expected_spatial_roi_plan_sha256 =
        "sha256:" + std::string(64, 'b');
    limits.output_capacities = {{4, 4}, {4, 4}, {6, 2}, {2, 2}};
    for (const SpatialRoiWorkItem& item : items) {
        orange::spatial_roi::SpatialRoiPlanRoiBinding descriptor;
        descriptor.roi_id = item.roi_id;
        descriptor.region_id = item.region_id;
        descriptor.arena_group_id = item.arena_group_id;
        descriptor.arena_id = item.arena_id;
        descriptor.logical_stream_id = item.logical_stream_id;
        descriptor.source_rect = item.geometry.content_rect;
        descriptor.encoded_raster = item.geometry.encoded_raster;
        descriptor.encoded_content_rect = item.geometry.encoded_content_rect;
        descriptor.output_bytes =
            static_cast<std::size_t>(descriptor.encoded_raster.width) *
            descriptor.encoded_raster.height;
        limits.expected_roi_descriptors.push_back(std::move(descriptor));
    }
    limits.admission_pool_bytes = (16 + 16 + 12 + 4) * 2;
    limits.expected_pool_bytes = limits.admission_pool_bytes;
    return limits;
}

void test_geometry_validation()
{
    const SpatialRoiBatchLimits limits = make_limits();
    SpatialRoiSourceView source;
    source.device_data = reinterpret_cast<const unsigned char*>(0x1000);
    source.pitch_bytes = 8;
    source.allocation_bytes = 8 * 6;
    source.width = 8;
    source.height = 6;
    source.gpu_id = 0;
    source.pixel_format = SpatialRoiSourcePixelFormat::kMono8;
    source.source_lease = make_source_lease();
    source.ready_event = reinterpret_cast<cudaEvent_t>(0x1);
    source.identity = make_identity();

    const std::vector<SpatialRoiWorkItem> valid_items = make_four_items();
    std::string error;
    require(
        validate_spatial_roi_batch(source, valid_items, limits, &error),
        "valid four-ROI geometry was rejected: " + error);

    {
        auto invalid_source = source;
        invalid_source.source_lease.reset();
        require(
            !validate_spatial_roi_batch(
                invalid_source, valid_items, limits, &error),
            "source without an ownership lease was accepted");
        require(error.find("source_lease") != std::string::npos,
                "missing source-lease rejection did not explain the contract");
    }

    {
        auto invalid_source = source;
        invalid_source.ready_event = nullptr;
        require(
            !validate_spatial_roi_batch(
                invalid_source, valid_items, limits, &error),
            "source without a ready event was accepted");
        require(error.find("ready_event") != std::string::npos,
                "missing ready-event rejection did not explain the contract");
    }

    {
        auto invalid_source = source;
        invalid_source.pixel_format = SpatialRoiSourcePixelFormat::kUnknown;
        require(
            !validate_spatial_roi_batch(
                invalid_source, valid_items, limits, &error),
            "source without an explicit Mono8 format was accepted");
        require(error.find("pixel_format") != std::string::npos,
                "pixel-format rejection did not explain the contract");
    }

    {
        auto invalid_source = source;
        invalid_source.allocation_bytes--;
        require(
            !validate_spatial_roi_batch(
                invalid_source, valid_items, limits, &error),
            "source allocation smaller than pitch*height was accepted");
        require(error.find("allocation_bytes") != std::string::npos,
                "allocation-size rejection did not explain the contract");
    }

    {
        auto invalid = valid_items;
        invalid[0].geometry.encoded_content_rect.width = 2;
        require(
            !validate_spatial_roi_batch(source, invalid, limits, &error),
            "resizing geometry was accepted");
        require(error.find("resizing is forbidden") != std::string::npos,
                "resize rejection did not explain the invariant");
    }
    {
        auto invalid = valid_items;
        invalid[0].geometry.content_rect.x = 7;
        require(
            !validate_spatial_roi_batch(source, invalid, limits, &error),
            "out-of-bounds source geometry was accepted");
    }
    {
        auto invalid = valid_items;
        invalid[1].roi_id = invalid[0].roi_id;
        require(
            !validate_spatial_roi_batch(source, invalid, limits, &error),
            "duplicate ROI identity was accepted");
    }
    {
        auto invalid = valid_items;
        invalid[1].source.recording_frame_id++;
        require(
            !validate_spatial_roi_batch(source, invalid, limits, &error),
            "mixed source identities were accepted");
    }
    {
        auto invalid_source = source;
        invalid_source.identity.recording_identity_token = "not-a-digest";
        require(
            !validate_spatial_roi_batch(
                invalid_source, valid_items, limits, &error),
            "invalid recording identity token was accepted");
    }
    {
        auto invalid_source = source;
        invalid_source.identity.recording_identity_token =
            "sha256:" + std::string(64, 'a');
        require(
            !validate_spatial_roi_batch(
                invalid_source, valid_items, limits, &error),
            "canonical token for another recording was accepted");
        require(error.find("does not match recording_id") != std::string::npos,
                "recording token mismatch did not explain the binding");
    }
    {
        auto invalid = valid_items;
        invalid[2].spatial_roi_plan_sha256 =
            "sha256:" + std::string(64, 'A');
        require(
            !validate_spatial_roi_batch(source, invalid, limits, &error),
            "non-canonical plan digest was accepted");
    }
    {
        auto invalid = valid_items;
        invalid.push_back(make_item(
            "roi_5",
            "region_5",
            SpatialRoiRect{0, 0, 1, 1},
            SpatialRoiRaster{1, 1},
            SpatialRoiRect{0, 0, 1, 1}));
        require(
            !validate_spatial_roi_batch(source, invalid, limits, &error),
            "batch above its configured ROI bound was accepted");
    }
    {
        auto invalid = valid_items;
        invalid[1].geometry.encoded_raster = {6, 2};
        require(
            !validate_spatial_roi_batch(source, invalid, limits, &error),
            "work item raster was accepted in the wrong capacity order");
        require(error.find("ordered output capacity") != std::string::npos,
                "capacity-order rejection did not explain the invariant");
    }
    {
        auto invalid = valid_items;
        invalid[0].spatial_roi_plan_sha256 =
            "sha256:" + std::string(64, 'c');
        require(
            !validate_spatial_roi_batch(source, invalid, limits, &error),
            "work item from another spatial ROI plan was accepted");
    }
    {
        auto invalid_source = source;
        invalid_source.identity.camera_id++;
        require(
            !validate_spatial_roi_batch(
                invalid_source, valid_items, limits, &error),
            "unexpected numeric camera identity was accepted");
    }
    {
        auto invalid_source = source;
        invalid_source.width++;
        require(
            !validate_spatial_roi_batch(
                invalid_source, valid_items, limits, &error),
            "unexpected source native raster was accepted");
    }
    {
        auto invalid_source = source;
        invalid_source.pitch_bytes = std::numeric_limits<std::size_t>::max();
        require(
            !validate_spatial_roi_batch(
                invalid_source, valid_items, limits, &error),
            "source pitch arithmetic overflow was accepted");
        require(error.find("allocation byte calculation overflows") !=
                    std::string::npos,
                "pitch-overflow rejection did not explain the arithmetic failure");
    }
    {
        auto invalid_limits = limits;
        invalid_limits.output_capacities = {
            {std::numeric_limits<std::uint32_t>::max(),
             std::numeric_limits<std::uint32_t>::max()},
            {std::numeric_limits<std::uint32_t>::max(),
             std::numeric_limits<std::uint32_t>::max()},
            {std::numeric_limits<std::uint32_t>::max(),
             std::numeric_limits<std::uint32_t>::max()},
            {std::numeric_limits<std::uint32_t>::max(),
             std::numeric_limits<std::uint32_t>::max()}};
        require(
            !validate_spatial_roi_batch(source, valid_items, invalid_limits, &error),
            "output pool byte-count overflow was accepted");
    }
}

void test_failure_contract_status_names()
{
    require(
        std::string(spatial_roi_batch_status_name(
            SpatialRoiBatchStatus::kCudaError)) == "cuda_error",
        "internal CUDA admission status name changed unexpectedly");
    require(
        std::string(spatial_roi_batch_status_name(
            SpatialRoiBatchStatus::kSourceQuarantined)) ==
            "source_quarantined",
        "source-quarantined status name is not explicit");
    SpatialRoiBatchResult result;
    require(result.source_release_safe(),
            "an empty result should not claim an unsafe source lease");
}

orange::session::spatial_roi::Config make_plan_config()
{
    namespace config_api = orange::session::spatial_roi;
    config_api::Config config = config_api::default_config();
    config.enabled = true;
    config.output_alignment_px = 2;
    config.buffering.pool_frames_per_stream = 2;
    config.buffering.queue_frames_per_stream = 4;
    config.admission.max_rois_per_camera = 8;
    config.admission.max_total_rois = 8;
    config.admission.max_total_encoder_streams = 8;
    config.admission.max_total_pixel_rate = 1000000;
    config.admission.max_total_pool_bytes = 1000000;
    config.admission.max_total_queue_frames = 1000000;

    config_api::CameraConfig camera;
    camera.camera_id = 3;
    camera.camera_serial = "10000";
    camera.native_raster = {8, 6};
    camera.source_frame_rate = 30;
    camera.arena_group_id = "arena_group_1";
    camera.layout = {"layout_1", "sha256:" + std::string(64, '1')};
    camera.materialization = {
        "materialization_1", "sha256:" + std::string(64, '2')};
    camera.registration = {
        "registration_1", "sha256:" + std::string(64, '3')};
    const std::vector<std::pair<std::string, SpatialRoiRect>> rois = {
        {"roi_1", {1, 1, 3, 2}},
        {"roi_2", {4, 0, 2, 3}},
        {"roi_3", {0, 3, 4, 2}},
        {"roi_4", {7, 5, 1, 1}},
    };
    for (const auto& [roi_id, rect] : rois) {
        config_api::RoiConfig roi;
        roi.roi_id = roi_id;
        roi.region_id = "region_" + roi_id.substr(4);
        roi.content_rect = {rect.x, rect.y, rect.width, rect.height};
        roi.logical_stream_id =
            config_api::expected_logical_stream_id(camera.camera_serial, roi_id);
        roi.artifact_stem =
            config_api::expected_artifact_stem(camera.camera_serial, roi_id);
        camera.rois.push_back(std::move(roi));
    }
    config.cameras.emplace(camera.camera_serial, std::move(camera));
    return config;
}

std::vector<SpatialRoiWorkItem> make_items_from_limits(
    const SpatialRoiBatchLimits& limits)
{
    std::vector<SpatialRoiWorkItem> items;
    SpatialRoiFrameIdentity identity;
    identity.recording_id = limits.expected_recording_id;
    identity.recording_identity_token = limits.expected_recording_identity_token;
    identity.producer_generation = limits.expected_producer_generation;
    identity.camera_id = limits.expected_camera_id;
    identity.camera_serial = limits.expected_camera_serial;
    identity.local_frame_id = 91;
    identity.camera_frame_id = 7001;
    identity.recording_frame_id = 17;
    identity.camera_timestamp_ns = 123456789;
    identity.timestamp_sys_ns = 987654321;
    items.reserve(limits.expected_roi_descriptors.size());
    for (const auto& descriptor : limits.expected_roi_descriptors) {
        SpatialRoiWorkItem item;
        item.roi_id = descriptor.roi_id;
        item.region_id = descriptor.region_id;
        item.arena_group_id = descriptor.arena_group_id;
        item.arena_id = descriptor.arena_id;
        item.logical_stream_id = descriptor.logical_stream_id;
        item.spatial_roi_plan_sha256 =
            limits.expected_spatial_roi_plan_sha256;
        item.source = identity;
        item.geometry.content_rect = descriptor.source_rect;
        item.geometry.encoded_raster = descriptor.encoded_raster;
        item.geometry.encoded_content_rect = descriptor.encoded_content_rect;
        items.push_back(std::move(item));
    }
    return items;
}

SpatialRoiBatchLimits make_verified_limits_for_producer(int gpu_id)
{
    namespace config_api = orange::session::spatial_roi;
    const config_api::Config config = make_plan_config();
    config_api::PlanContext context;
    context.recording_id = make_identity().recording_id;
    context.recording_identity_token = make_identity().recording_identity_token;
    context.generated_at_utc = "2026-08-30T23:00:00Z";
    context.producer_generation = make_identity().producer_generation;
    nlohmann::json plan;
    std::string error;
    require(config_api::build_plan(config, context, &plan, nullptr, &error), error);
    require(config_api::verify_plan(plan, &error), error);
    SpatialRoiBatchLimits limits;
    require(
        orange::spatial_roi::spatial_roi_batch_limits_from_verified_plan(
            plan, make_identity().camera_serial, gpu_id, &limits, &error),
        error);
    return limits;
}

void test_verified_plan_materializes_exact_limits()
{
    namespace config_api = orange::session::spatial_roi;
    const config_api::Config config = make_plan_config();
    config_api::PlanContext context;
    context.recording_id = std::string(512, 'r');
    context.recording_identity_token =
        orange::shaman_v2_recording_identity::token_for_recording_id(
            context.recording_id);
    context.generated_at_utc = "2026-08-30T23:00:00Z";
    context.producer_generation = "generation_1";

    nlohmann::json plan;
    std::string error;
    require(config_api::build_plan(config, context, &plan, nullptr, &error), error);
    require(config_api::verify_plan(plan, &error), error);

    SpatialRoiBatchLimits limits;
    require(spatial_roi_batch_limits_from_verified_plan(
                plan, "10000", 0, &limits, &error),
            error);
    SpatialRoiBatchLimits repeat;
    require(spatial_roi_batch_limits_from_verified_plan(
                plan, "10000", 0, &repeat, &error),
            error);
    require(limits.expected_recording_id == context.recording_id,
            "producer limits lost printable recording identity");
    require(limits.batch_slot_count == 2 &&
                limits.pool_frames_per_stream == limits.batch_slot_count,
            "producer limits did not bind pool depth");
    require(limits.admission_pool_bytes == limits.expected_pool_bytes &&
                limits.admission_pool_bytes > 0,
            "producer limits did not bind exact pool bytes");
    require(limits.expected_roi_descriptors.size() == 4,
            "producer limits lost ordered plan descriptors");
    require(limits.expected_roi_descriptors[0].roi_id == "roi_1" &&
                limits.expected_roi_descriptors[1].roi_id == "roi_2",
            "producer limits did not preserve plan ROI order");
    require(limits.expected_roi_descriptors[0].source_rect.x == 1 &&
                limits.expected_roi_descriptors[0].encoded_content_rect.x == 0,
            "producer limits did not bind source/encoded placement");
    require(limits.expected_recording_id == repeat.expected_recording_id &&
                limits.expected_roi_descriptors.size() ==
                    repeat.expected_roi_descriptors.size() &&
                limits.admission_pool_bytes == repeat.admission_pool_bytes,
            "verified plan did not deterministically materialize limits");

    SpatialRoiSourceView source;
    source.device_data = reinterpret_cast<const unsigned char*>(0x1000);
    source.pitch_bytes = 8;
    source.allocation_bytes = 8 * 6;
    source.width = 8;
    source.height = 6;
    source.gpu_id = 0;
    source.pixel_format = SpatialRoiSourcePixelFormat::kMono8;
    source.source_lease = make_source_lease();
    source.ready_event = reinterpret_cast<cudaEvent_t>(0x1);
    source.identity = make_items_from_limits(limits).front().source;
    auto items = make_items_from_limits(limits);
    require(validate_spatial_roi_batch(source, items, limits, &error), error);

    auto swapped = items;
    std::swap(swapped[0], swapped[1]);
    require(!validate_spatial_roi_batch(source, swapped, limits, &error),
            "swapped ROI order was accepted");

    auto relabeled = items;
    relabeled[0].region_id = "region_relabelled";
    require(!validate_spatial_roi_batch(source, relabeled, limits, &error),
            "relabelled ROI was accepted");

    auto moved = items;
    moved[0].geometry.content_rect.x++;
    require(!validate_spatial_roi_batch(source, moved, limits, &error),
            "changed source rectangle was accepted");

    auto pool_mismatch = limits;
    pool_mismatch.admission_pool_bytes++;
    require(!validate_spatial_roi_batch(source, items, pool_mismatch, &error),
            "pool admission mismatch was accepted");

    nlohmann::json tampered = plan;
    tampered["plan"]["resolved_cameras"]["10000"]["rois"][0]
            ["region_id"] = "region_tampered";
    require(!spatial_roi_batch_limits_from_verified_plan(
                tampered, "10000", 0, &repeat, &error),
            "unverified plan was used to create producer limits");

    bool arbitrary_limits_rejected = false;
    try {
        SpatialRoiBatchProducer arbitrary(make_limits());
    } catch (const std::invalid_argument&) {
        arbitrary_limits_rejected = true;
    }
    require(arbitrary_limits_rejected,
            "arbitrary public limits must not instantiate a CUDA producer");

    auto mutated_verified_limits = make_verified_limits_for_producer(0);
    mutated_verified_limits.output_capacities[0].width++;
    bool mutated_limits_rejected = false;
    try {
        SpatialRoiBatchProducer mutated(mutated_verified_limits);
    } catch (const std::invalid_argument&) {
        mutated_limits_rejected = true;
    }
    require(mutated_limits_rejected,
            "limits mutated after verified-plan materialization were accepted");
}

struct GpuSource {
    unsigned char* device_data = nullptr;
    std::size_t pitch_bytes = 0;
    cudaStream_t stream = nullptr;
    cudaEvent_t ready_event = nullptr;

    ~GpuSource()
    {
        if (stream) {
            (void)cudaStreamSynchronize(stream);
        }
        if (ready_event) {
            (void)cudaEventDestroy(ready_event);
        }
        if (device_data) {
            (void)cudaFree(device_data);
        }
        if (stream) {
            (void)cudaStreamDestroy(stream);
        }
    }
};

void verify_output_pixels(
    const std::vector<unsigned char>& source,
    std::uint32_t source_width,
    const SpatialRoiWorkItem& item,
    const unsigned char* device_output,
    std::size_t output_pitch)
{
    const SpatialRoiOutputGeometry& geometry = item.geometry;
    std::vector<unsigned char> actual(
        static_cast<std::size_t>(geometry.encoded_raster.width) *
        geometry.encoded_raster.height,
        0xff);
    require_cuda(
        cudaMemcpy2D(
            actual.data(),
            geometry.encoded_raster.width,
            device_output,
            output_pitch,
            geometry.encoded_raster.width,
            geometry.encoded_raster.height,
            cudaMemcpyDeviceToHost),
        "cudaMemcpy2D(output to host)");

    std::vector<unsigned char> expected(actual.size(), 0);
    for (std::uint32_t row = 0; row < geometry.content_rect.height; ++row) {
        for (std::uint32_t column = 0; column < geometry.content_rect.width; ++column) {
            const std::size_t source_index =
                static_cast<std::size_t>(geometry.content_rect.y + row) *
                    source_width +
                geometry.content_rect.x + column;
            const std::size_t output_index =
                static_cast<std::size_t>(
                    geometry.encoded_content_rect.y + row) *
                    geometry.encoded_raster.width +
                geometry.encoded_content_rect.x + column;
            expected[output_index] = source[source_index];
        }
    }
    require(
        actual == expected,
        "ROI " + item.roi_id +
            " did not preserve native pixels with explicit zero padding");
}

void test_gpu_batch_copy(int gpu_id)
{
    require_cuda(cudaSetDevice(gpu_id), "cudaSetDevice(test)");

    constexpr std::uint32_t kSourceWidth = 8;
    constexpr std::uint32_t kSourceHeight = 6;
    std::vector<unsigned char> source_bytes(
        static_cast<std::size_t>(kSourceWidth) * kSourceHeight);
    for (std::size_t index = 0; index < source_bytes.size(); ++index) {
        source_bytes[index] = static_cast<unsigned char>(index + 1);
    }

    GpuSource gpu_source;
    require_cuda(
        cudaMallocPitch(
            reinterpret_cast<void**>(&gpu_source.device_data),
            &gpu_source.pitch_bytes,
            kSourceWidth,
            kSourceHeight),
        "cudaMallocPitch(source)");
    require_cuda(
        cudaStreamCreateWithFlags(&gpu_source.stream, cudaStreamNonBlocking),
        "cudaStreamCreateWithFlags(source)");
    require_cuda(
        cudaEventCreateWithFlags(
            &gpu_source.ready_event,
            cudaEventDisableTiming),
        "cudaEventCreateWithFlags(source ready)");
    require_cuda(
        cudaMemcpy2DAsync(
            gpu_source.device_data,
            gpu_source.pitch_bytes,
            source_bytes.data(),
            kSourceWidth,
            kSourceWidth,
            kSourceHeight,
            cudaMemcpyHostToDevice,
            gpu_source.stream),
        "cudaMemcpy2DAsync(source to device)");
    require_cuda(
        cudaEventRecord(gpu_source.ready_event, gpu_source.stream),
        "cudaEventRecord(source ready)");

    SpatialRoiSourceView source_view;
    source_view.device_data = gpu_source.device_data;
    source_view.pitch_bytes = gpu_source.pitch_bytes;
    source_view.allocation_bytes = gpu_source.pitch_bytes * kSourceHeight;
    source_view.width = kSourceWidth;
    source_view.height = kSourceHeight;
    source_view.gpu_id = gpu_id;
    source_view.pixel_format = SpatialRoiSourcePixelFormat::kMono8;
    source_view.ready_event = gpu_source.ready_event;
    source_view.identity = make_identity();

    std::shared_ptr<int> source_lease = std::make_shared<int>(0);
    const std::weak_ptr<int> weak_source_lease = source_lease;
    source_view.source_lease = source_lease;

    const SpatialRoiBatchLimits limits = make_verified_limits_for_producer(gpu_id);
    // The producer is admitted from the verified plan above.  Materialize the
    // routed work from those exact descriptors/digest instead of using the
    // geometry-only fixture (whose placeholder digest intentionally differs
    // from the plan built for this test).
    const std::vector<SpatialRoiWorkItem> work_items =
        make_items_from_limits(limits);
    SpatialRoiBatchProducer producer(limits);
    require(producer.slot_capacity() == 2, "producer did not create its bounded slot pool");

    auto first = producer.TryProduce(source_view, work_items);
    require(
        first.accepted(),
        "first four-ROI batch was rejected: " + first.error());
    source_view.source_lease.reset();
    source_lease.reset();
    require(!weak_source_lease.expired(),
            "accepted result did not retain the source ownership lease");
    source_view.source_lease = weak_source_lease.lock();
    require(first.outputs().size() == work_items.size(), "first batch output count mismatch");
    require(first.completion_event() != nullptr, "first batch has no completion event");
    require_cuda(
        cudaEventSynchronize(first.completion_event()),
        "cudaEventSynchronize(batch complete)");
    for (const auto& output : first.outputs()) {
        require(
            output.pitch_bytes == output.work_item.geometry.encoded_raster.width,
            "ROI output is not tightly packed for the existing CUDA-IPC descriptor");
        verify_output_pixels(
            source_bytes,
            kSourceWidth,
            output.work_item,
            output.device_data,
            output.pitch_bytes);
    }

    auto second = producer.TryProduce(source_view, work_items);
    require(second.accepted(), "second bounded slot was not available");
    auto exhausted = producer.TryProduce(source_view, work_items);
    require(
        exhausted.status() == SpatialRoiBatchStatus::kPoolExhausted,
        "producer did not fail nonblocking when all bounded slots were retained");

    first.Reset();
    require(
        producer.available_slot_count() == 1,
        "completed released batch did not return one slot to the pool");
    auto recycled = producer.TryProduce(source_view, work_items);
    require(recycled.accepted(), "recycled batch slot was not reusable");
    require_cuda(
        cudaEventSynchronize(recycled.completion_event()),
        "cudaEventSynchronize(recycled batch)");
}

void test_stop_accepting_linearization(int gpu_id)
{
    require_cuda(cudaSetDevice(gpu_id), "cudaSetDevice(stop test)");

    GpuSource gpu_source;
    require_cuda(
        cudaMallocPitch(
            reinterpret_cast<void**>(&gpu_source.device_data),
            &gpu_source.pitch_bytes,
            8,
            6),
        "cudaMallocPitch(stop source)");
    require_cuda(
        cudaStreamCreateWithFlags(&gpu_source.stream, cudaStreamNonBlocking),
        "cudaStreamCreateWithFlags(stop source)");
    require_cuda(
        cudaEventCreateWithFlags(
            &gpu_source.ready_event,
            cudaEventDisableTiming),
        "cudaEventCreateWithFlags(stop source ready)");
    require_cuda(
        cudaEventRecord(gpu_source.ready_event, gpu_source.stream),
        "cudaEventRecord(stop source ready)");

    SpatialRoiSourceView source_view;
    source_view.device_data = gpu_source.device_data;
    source_view.pitch_bytes = gpu_source.pitch_bytes;
    source_view.allocation_bytes = gpu_source.pitch_bytes * 6;
    source_view.width = 8;
    source_view.height = 6;
    source_view.gpu_id = gpu_id;
    source_view.pixel_format = SpatialRoiSourcePixelFormat::kMono8;
    source_view.source_lease = make_source_lease();
    source_view.ready_event = gpu_source.ready_event;
    source_view.identity = make_identity();

    const SpatialRoiBatchLimits limits = make_verified_limits_for_producer(gpu_id);
    const std::vector<SpatialRoiWorkItem> work_items =
        make_items_from_limits(limits);
    SpatialRoiBatchProducer producer(limits);

    std::atomic<bool> worker_started{false};
    std::atomic<bool> stop_returned{false};
    std::atomic<bool> accepted_after_stop{false};
    std::thread worker([&] {
        worker_started.store(true, std::memory_order_release);
        auto result = producer.TryProduce(source_view, work_items);
        result.Reset();
        while (!stop_returned.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        auto post_stop_result = producer.TryProduce(source_view, work_items);
        if (post_stop_result.accepted()) {
            accepted_after_stop.store(true, std::memory_order_release);
        }
    });
    while (!worker_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    producer.StopAccepting();
    stop_returned.store(true, std::memory_order_release);
    worker.join();

    require(!accepted_after_stop.load(std::memory_order_acquire),
            "a batch was accepted after StopAccepting returned");
    auto stopped = producer.TryProduce(source_view, work_items);
    require(stopped.status() == SpatialRoiBatchStatus::kStopped,
            "TryProduce accepted work after StopAccepting");
}

}  // namespace

int main()
{
    try {
        test_geometry_validation();
        test_failure_contract_status_names();
        test_verified_plan_materializes_exact_limits();

        int device_count = 0;
        const cudaError_t device_status = cudaGetDeviceCount(&device_count);
        if (device_status != cudaSuccess || device_count <= 0) {
            std::cout << "[PASS] spatial ROI geometry/identity validation\n";
            std::cout << "[SKIP] spatial ROI CUDA copy test: "
                      << (device_status == cudaSuccess
                              ? "no CUDA device"
                              : cudaGetErrorString(device_status))
                      << "\n";
            (void)cudaGetLastError();
            return 0;
        }

        test_gpu_batch_copy(0);
        test_stop_accepting_linearization(0);
        std::cout << "[PASS] spatial ROI geometry/identity validation\n";
        std::cout << "[PASS] detector-independent four-ROI CUDA batch copy, padding, fence, and pool bound\n";
        std::cout << "[PASS] StopAccepting linearization\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << "\n";
        return 1;
    }
}
