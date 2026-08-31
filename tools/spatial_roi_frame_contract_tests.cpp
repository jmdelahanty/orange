#include "spatial_roi_frame_contract.h"

#include "shaman_v2_recording_identity.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using orange::spatial_roi::SpatialRoiFrameDescriptor;
using orange::spatial_roi::SpatialRoiFrameMetadata;
using orange::spatial_roi::SpatialRoiFrameIdentityRegistry;

int failures = 0;

void expect(const bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

SpatialRoiFrameDescriptor make_descriptor()
{
    SpatialRoiFrameDescriptor descriptor;
    descriptor.recording_id = "recording-20260831T120000Z";
    descriptor.recording_identity_token =
        orange::shaman_v2_recording_identity::token_for_recording_id(
            descriptor.recording_id);
    descriptor.producer_generation = "generation_1";
    descriptor.camera_id = 3;
    descriptor.camera_serial = "2010096";
    descriptor.local_frame_id = 91;
    descriptor.camera_frame_id = 7001;
    descriptor.recording_frame_id = 17;
    descriptor.roi_stream_frame_index = 12;
    descriptor.camera_timestamp_ns = 123456789;
    descriptor.timestamp_sys_ns = 987654321;
    descriptor.roi_id = "roi_1";
    descriptor.region_id = "region_1";
    descriptor.arena_group_id = "group_1";
    descriptor.arena_id = "arena_1";
    descriptor.logical_stream_id = "2010096_spatial_roi_roi_1";
    descriptor.spatial_roi_plan_sha256 =
        "sha256:" + std::string(64, 'a');
    descriptor.native_raster = {100, 80};
    descriptor.content_rect = {5, 7, 13, 11};
    descriptor.encoded_raster = {16, 16};
    descriptor.encoded_content_rect = {0, 0, 13, 11};
    descriptor.padding = {0, 0, 3, 5, 0};
    descriptor.bytes = 16 * 16;
    descriptor.source_gpu_id = 5;
    descriptor.assigned_gpu_id = 6;
    descriptor.assigned_shard_id = 0;
    descriptor.routing_policy = "single_shard";
    return descriptor;
}

void test_descriptor_round_trip_and_closed_schema()
{
    const SpatialRoiFrameDescriptor source = make_descriptor();
    std::string error;
    expect(orange::spatial_roi::validate_spatial_roi_frame_descriptor(
               source, &error),
           "valid descriptor validates: " + error);
    const nlohmann::json wire =
        orange::spatial_roi::spatial_roi_frame_descriptor_to_json(source);
    SpatialRoiFrameDescriptor parsed;
    expect(orange::spatial_roi::spatial_roi_frame_descriptor_from_json(
               wire, &parsed, &error),
           "descriptor parses: " + error);
    expect(parsed.logical_stream_id == source.logical_stream_id,
           "logical stream survives round trip");
    expect(parsed.key() == source.key(),
           "logical stream plus recording frame is the stable key");
    expect(orange::spatial_roi::spatial_roi_frame_descriptor_to_json(parsed) == wire,
           "descriptor JSON round trip is exact");

    nlohmann::json unknown = wire;
    unknown["future_field"] = true;
    expect(!orange::spatial_roi::spatial_roi_frame_descriptor_from_json(
               unknown, &parsed, &error),
           "unknown descriptor fields fail closed");

    unknown = wire;
    unknown["schema_version"] = 2;
    expect(!orange::spatial_roi::spatial_roi_frame_descriptor_from_json(
               unknown, &parsed, &error),
           "unsupported descriptor version fails closed");
}

void test_descriptor_identity_and_geometry_validation()
{
    std::string error;
    SpatialRoiFrameDescriptor descriptor = make_descriptor();

    descriptor.recording_frame_id = 0;
    expect(!orange::spatial_roi::validate_spatial_roi_frame_descriptor(
               descriptor, &error),
           "zero source recording frame fails");
    descriptor = make_descriptor();
    descriptor.recording_identity_token = "sha256:" + std::string(64, 'b');
    expect(!orange::spatial_roi::validate_spatial_roi_frame_descriptor(
               descriptor, &error),
           "mismatched recording token fails");
    descriptor = make_descriptor();
    descriptor.logical_stream_id = "2010096_spatial_roi_wrong_name";
    expect(!orange::spatial_roi::validate_spatial_roi_frame_descriptor(
               descriptor, &error),
           "non-canonical logical stream name fails");
    descriptor = make_descriptor();
    descriptor.encoded_content_rect.x = 1;
    descriptor.padding.left = 1;
    expect(!orange::spatial_roi::validate_spatial_roi_frame_descriptor(
               descriptor, &error),
           "nonzero encoded left offset fails v1 geometry");
    descriptor = make_descriptor();
    descriptor.encoded_content_rect.y = 1;
    descriptor.padding.top = 1;
    expect(!orange::spatial_roi::validate_spatial_roi_frame_descriptor(
               descriptor, &error),
           "nonzero encoded top offset fails v1 geometry");
    descriptor = make_descriptor();
    descriptor.padding.value_mono8 = 1;
    expect(!orange::spatial_roi::validate_spatial_roi_frame_descriptor(
               descriptor, &error),
           "nonzero v1 padding value fails");
    descriptor = make_descriptor();
    descriptor.padding.right++;
    expect(!orange::spatial_roi::validate_spatial_roi_frame_descriptor(
               descriptor, &error),
           "inconsistent padding fails");
    descriptor = make_descriptor();
    descriptor.content_rect.width++;
    expect(!orange::spatial_roi::validate_spatial_roi_frame_descriptor(
               descriptor, &error),
           "resizing geometry fails");
    descriptor = make_descriptor();
    descriptor.bytes--;
    expect(!orange::spatial_roi::validate_spatial_roi_frame_descriptor(
               descriptor, &error),
           "encoded byte count mismatch fails");
    descriptor = make_descriptor();
    descriptor.encoded_content_rect.x = 16;
    expect(!orange::spatial_roi::validate_spatial_roi_frame_descriptor(
               descriptor, &error),
           "encoded content outside raster fails");
}

void test_metadata_round_trip_and_outcomes()
{
    SpatialRoiFrameMetadata metadata;
    metadata.frame = make_descriptor();
    metadata.submission_outcome = "encoded";
    metadata.output_frame_index = 12;
    metadata.packet_count = 1;
    metadata.encoded_bytes = 4096;
    std::string error;
    expect(orange::spatial_roi::validate_spatial_roi_frame_metadata(
               metadata, &error),
           "encoded metadata validates: " + error);
    const nlohmann::json wire =
        orange::spatial_roi::spatial_roi_frame_metadata_to_json(metadata);
    SpatialRoiFrameMetadata parsed;
    expect(orange::spatial_roi::spatial_roi_frame_metadata_from_json(
               wire, &parsed, &error),
           "metadata parses: " + error);
    expect(parsed.packet_count == metadata.packet_count &&
               parsed.encoded_bytes == metadata.encoded_bytes,
           "metadata encode evidence survives round trip");

    metadata.submission_outcome = "accepted";
    metadata.packet_count = 0;
    metadata.encoded_bytes = 0;
    expect(orange::spatial_roi::validate_spatial_roi_frame_metadata(
               metadata, &error),
           "admitted pre-encode metadata validates: " + error);
    metadata.output_frame_index++;
    expect(!orange::spatial_roi::validate_spatial_roi_frame_metadata(
               metadata, &error),
           "output index drift from ROI-local index fails");
    metadata.output_frame_index--;
    metadata.submission_outcome = "unknown";
    expect(!orange::spatial_roi::validate_spatial_roi_frame_metadata(
               metadata, &error),
           "unknown metadata outcome fails closed");
}

void test_collision_free_registry()
{
    SpatialRoiFrameIdentityRegistry registry;
    std::string error;
    const SpatialRoiFrameDescriptor roi_one = make_descriptor();
    SpatialRoiFrameDescriptor roi_two = roi_one;
    roi_two.roi_id = "roi_2";
    roi_two.region_id = "region_2";
    roi_two.logical_stream_id = "2010096_spatial_roi_roi_2";
    expect(registry.note(roi_one, &error), "first ROI identity is accepted: " + error);
    expect(registry.note(roi_two, &error),
           "same source frame on another ROI stream is accepted: " + error);
    expect(registry.size() == 2, "registry retains both logical streams");
    expect(!registry.note(roi_one, &error),
           "duplicate logical stream plus recording frame is rejected");
    expect(registry.contains(roi_two.key()), "registry lookup finds second ROI");
}

}  // namespace

int main()
{
    test_descriptor_round_trip_and_closed_schema();
    test_descriptor_identity_and_geometry_validation();
    test_metadata_round_trip_and_outcomes();
    test_collision_free_registry();
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "spatial ROI frame contract tests passed\n";
    return 0;
}
