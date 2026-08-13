#include "recording_output_descriptor.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_full_output_descriptor_from_camera_artifact()
{
    orange::session::RecordingSessionCameraArtifact artifact;
    artifact.camera_serial = "2010096";
    artifact.video_path = "Cam2010096.mp4";
    artifact.metadata_path = "Cam2010096_meta.csv";
    artifact.keyframe_path = "Cam2010096_keyframe.json";
    artifact.frame_count = 601;
    artifact.first_recording_frame_id = 1;
    artifact.last_recording_frame_id = 601;
    artifact.recording_frame_id_gaps = 0;
    artifact.packet_count = 601;
    artifact.packet_count_source = "ffprobe_nb_read_packets";

    const auto descriptor = orange::session::build_full_recording_output_descriptor(
        artifact,
        "external_ipc",
        "completed");
    const auto json = orange::session::build_recording_output_descriptor_json(descriptor);

    require(json.value("schema_version", 0) == 1, "full descriptor schema version");
    require(json.value("camera_serial", std::string()) == "2010096", "full camera serial");
    require(json.value("output_kind", std::string()) == "full", "full output kind");
    require(json.value("role", std::string()) == "ingest_authoritative", "full role");
    require(json.value("backend", std::string()) == "external_ipc", "full backend");
    require(json.value("status", std::string()) == "completed", "full status");
    require(json.value("video", std::string()) == "Cam2010096.mp4", "full video path");
    require(json.value("metadata", std::string()) == "Cam2010096_meta.csv", "full metadata path");
    require(json.value("keyframes", std::string()) == "Cam2010096_keyframe.json", "full keyframe path");
    require(json.value("container", std::string()) == "mp4", "full container");
    require(json.value("coordinate_space", std::string()) == "full_frame_pixels", "full coordinate space");
    require(!json.contains("video_pixel_coordinate_space"),
            "full output does not need split crop coordinate fields");
    require(!json.contains("source_geometry_coordinate_space"),
            "full output does not need source placement geometry");
    require(json.value("frame_count", 0) == 601, "full frame count");
    require(json.value("packet_count", 0) == 601, "full packet count");
    require(json.value("packet_count_source", std::string()) == "ffprobe_nb_read_packets",
            "full packet count source");
}

void test_crop_output_descriptor_serialization()
{
    orange::session::RecordingOutputDescriptor crop;
    crop.camera_serial = "2010096";
    orange::session::apply_crop_recording_output_media_contract(&crop);
    crop.backend = "in_process";
    crop.status = "completed";
    crop.video_path = "Cam2010096_crop.mp4";
    crop.metadata_path = "Cam2010096_crop_meta.csv";
    crop.keyframe_path = "Cam2010096_crop_keyframe.json";
    crop.perf_path = "Cam2010096_crop_perf.csv";
    crop.sidecar_perf_path = "Cam2010096_crop_sidecar_perf.csv";
    crop.width = 328;
    crop.height = 328;
    crop.frame_rate = 100;
    crop.codec = "hevc";
    crop.container = "mp4";
    crop.tuning = "lossless";
    crop.pixel_source_format = "mono8";
    crop.encoded_format = "nv12";
    crop.details = {
        {"selection_policy", "largest_detection_by_confidence"},
        {"blank_frame_policy", "encode_black_frame_when_no_detection"}
    };

    const auto grouped = orange::session::build_recording_outputs_json({crop});
    require(grouped.contains("2010096"), "grouped crop camera key");
    require(grouped["2010096"].contains("crop"), "grouped crop output key");
    const auto& json = grouped["2010096"]["crop"];
    require(json.value("schema_version", 0) == 2, "crop descriptor schema version");
    require(json.value("role", std::string()) ==
                "runtime_derived_acquisition_input",
            "crop role");
    require(json.value("coordinate_space", std::string()) ==
                "full_frame_pixels",
            "crop legacy coordinate alias");
    require(json.value("video_pixel_coordinate_space", std::string()) ==
                "crop_frame_pixels",
            "crop video pixel coordinate space");
    require(json.value("source_geometry_coordinate_space", std::string()) ==
                "full_frame_pixels",
            "crop source geometry coordinate space");
    require(json.value("video", std::string()) == "Cam2010096_crop.mp4", "crop video path");
    require(json.value("perf", std::string()) == "Cam2010096_crop_perf.csv", "crop perf path");
    require(json.value("sidecar_perf", std::string()) == "Cam2010096_crop_sidecar_perf.csv",
            "crop sidecar perf path");
    require(json.value("width", 0) == 328, "crop width");
    require(json.value("height", 0) == 328, "crop height");
    require(json.value("frame_rate", 0) == 100, "crop frame rate");
    require(json.value("codec", std::string()) == "hevc", "crop codec");
    require(json.value("pixel_source_format", std::string()) == "mono8", "crop source format");
    require(json.value("encoded_format", std::string()) == "nv12", "crop encoded format");
    require(json.contains("details") && json["details"].is_object(), "crop details object");
}

void test_full_and_crop_statuses_are_independent()
{
    orange::session::RecordingOutputDescriptor full;
    full.camera_serial = "2010096";
    full.output_kind = "full";
    full.role = "ingest_authoritative";
    full.backend = "external_ipc";
    full.status = "completed";
    full.video_path = "Cam2010096.mp4";

    orange::session::RecordingOutputDescriptor crop;
    crop.camera_serial = "2010096";
    orange::session::apply_crop_recording_output_media_contract(&crop);
    crop.backend = "external_ipc";
    crop.status = "incomplete";
    crop.metadata_path = "Cam2010096_crop_meta.csv";
    crop.perf_path = "Cam2010096_crop_perf.csv";
    crop.details = {
        {"status_reason", "external crop recorder output incomplete"}
    };

    const auto grouped = orange::session::build_recording_outputs_json({full, crop});
    require(grouped["2010096"]["full"].value("status", std::string()) == "completed",
            "full output status should remain completed");
    require(grouped["2010096"]["crop"].value("status", std::string()) == "incomplete",
            "crop sidecar status should be independently incomplete");
    require(grouped["2010096"]["crop"]["details"].value("status_reason", std::string()) ==
                "external crop recorder output incomplete",
            "crop sidecar status reason should be preserved");
}

void test_default_fallback_values()
{
    orange::session::RecordingOutputDescriptor descriptor;
    descriptor.camera_serial = "2010095";
    descriptor.output_kind.clear();
    descriptor.role.clear();
    descriptor.backend.clear();
    descriptor.status.clear();

    const auto json = orange::session::build_recording_output_descriptor_json(descriptor);
    require(json.value("output_kind", std::string()) == "full", "default output kind");
    require(json.value("role", std::string()) == "sidecar", "default role");
    require(json.value("backend", std::string()) == "unknown", "default backend");
    require(json.value("status", std::string()) == "unknown", "default status");
    require(!json.contains("frame_count"), "zero frame counts omitted");
    require(!json.contains("packet_count"), "packet counts omitted without source");
}

}  // namespace

int main()
{
    struct TestCase {
        const char* name;
        void (*fn)();
    };

    const TestCase tests[] = {
        {"full_output_descriptor_from_camera_artifact", test_full_output_descriptor_from_camera_artifact},
        {"crop_output_descriptor_serialization", test_crop_output_descriptor_serialization},
        {"full_and_crop_statuses_are_independent", test_full_and_crop_statuses_are_independent},
        {"default_fallback_values", test_default_fallback_values},
    };

    for (const auto& test : tests) {
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
