// Golden bytes for the detector event log (Cam<serial>_yolo_events.jsonl).
//
// Writes a fixed set of records covering every line kind and optional field through
// YoloEventLogger and compares the file with
// tools/fixtures/generated_outputs/golden/Cam2010093_yolo_events.jsonl, which was produced by the
// hand-written writer before it switched to the generated orange.yolo_event v3 code (2026-10-12).
// Regenerated once on purpose the same day: a record without a policy now logs
// policy_to_json(Policy{}) (schema-valid) instead of a reduced off object, and the gate_only
// policy is written by policy_to_json. Any other byte change to the log fails here.
//
//   yolo_event_log_golden_tests <golden file>            compare
//   yolo_event_log_golden_tests <golden file> --write    regenerate (only on a deliberate change)
#include "yolo_event_log.h"
#include "yolo_spatial_mask.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>

// Filesystem seam normally provided by the application-wide project.cpp.
bool make_folder(std::string path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return !ec;
}

namespace {
yolo_event_log::YoloResultRecord base(const std::string& folder, uint64_t frame) {
    yolo_event_log::YoloResultRecord r;
    r.recording_folder = folder;
    r.recording_frame_id = frame;
    r.local_frame_id = 1000 + frame;
    r.camera_frame_id = 65530 + frame;
    r.ipc_frame_id = frame;
    r.record_active = true;
    r.camera_timestamp = 1791000000000000000ULL + frame * 10000000ULL;
    r.timestamp_sys_ns = r.camera_timestamp - 36990000000ULL;
    r.event_epoch_us = 1791000000000000ULL + frame * 10000ULL;
    r.event_monotonic_us = 5000000ULL + frame * 10000ULL;
    r.gpu_id = 5;
    r.model_id = "golden_detect_model";
    r.source_width = 4512;
    r.source_height = 4512;
    r.queue_name = "/shm_cam_2010093_v2";
    r.ipc_enabled = true;
    r.ipc_requested = true;
    r.ipc_request_status = "published";
    r.spatial_mask.policy_generation = 1;
    r.status = "zero_detections";
    return r;
}

pose::Object detection(float x, float y, float w, float h, int label, float prob, int keypoints) {
    pose::Object o{};
    o.rect.x = x; o.rect.y = y; o.rect.width = w; o.rect.height = h;
    o.label = label; o.prob = prob;
    o.num_kps = static_cast<size_t>(keypoints);
    for (int k = 0; k < keypoints; ++k) o.kps[k] = x + 1.23456f * static_cast<float>(k + 1);
    return o;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: yolo_event_log_golden_tests <golden file> [--write]\n";
        return 2;
    }
    char root_template[] = "/tmp/yolo_golden_XXXXXX";
    if (!mkdtemp(root_template)) return 2;
    const std::string folder = std::string(root_template) + "/golden_recording";
    std::filesystem::create_directories(folder);
    {
        yolo_event_log::YoloEventLogger logger("2010093", 3, "YoloWorker");
        // 1. No detections under the default (off) policy: header + result.
        logger.Enqueue(base(folder, 1));
        // 2. Detections, one with keypoints, and an error string.
        auto r2 = base(folder, 2);
        r2.status = "detections";
        r2.detections = {detection(100.123456f, 200.98765f, 30.5f, 40.25f, 0, 0.912345f, 3),
                         detection(1500.0f, 1600.5f, 25.0f, 26.0f, 1, 0.55555f, 0)};
        r2.spatial_mask.raw_detection_count = 2;
        r2.spatial_mask.inside_detection_count = 2;
        r2.spatial_mask.downstream_detection_count = 2;
        r2.error = "golden: example error text";
        logger.Enqueue(r2);
        // 3. A policy change (generation 2, gate_only) with outside detections.
        auto r3 = base(folder, 3);
        r3.status = "detections";
        orange::analytics_mask::Policy gate;
        gate.mode = orange::analytics_mask::Mode::kGateOnly;
        gate.source_width = 4512;
        gate.source_height = 4512;
        gate.centroid_gate_circle = {2256.0f, 2256.0f, 2000.0f};
        gate.input_circle = {2256.0f, 2256.0f, 2000.0f};
        gate.camera_serial = "2010093";
        gate.arena_id = "arena_golden";
        gate.registration_id = "dailyreg_golden";
        gate.artifact_id = "golden_rim";
        gate.artifact_schema_id = "orange.calibration.dish_top_rim_observation";
        gate.artifact_schema_version = 2;
        gate.artifact_fingerprint = "fnv1a64:0123456789abcdef";
        gate.source_path = "/golden/observation.json";
        gate.recording_relative_path = "recording_geometry_assets/golden/observation.json";
        gate.source_sha256 = "sha256:" + std::string(64, 'c');
        auto policy = std::make_shared<nlohmann::json>(orange::analytics_mask::policy_to_json(gate));
        r3.spatial_mask.policy = policy;
        r3.spatial_mask.policy_generation = 2;
        r3.detections = {detection(2250.0f, 2250.0f, 20.0f, 20.0f, 0, 0.8f, 0)};
        r3.spatial_mask.raw_detection_count = 3;
        r3.spatial_mask.inside_detection_count = 1;
        r3.spatial_mask.outside_detection_count = 2;
        r3.spatial_mask.downstream_detection_count = 1;
        yolo_event_log::SpatialMaskOutsideDetection a;
        a.raw_index = 1; a.x_px = 10.0f; a.y_px = 20.0f; a.width_px = 5.5f; a.height_px = 6.25f;
        a.label = 0; a.confidence = 0.71234f; a.centroid_x_px = 12.75f; a.centroid_y_px = 23.125f;
        a.signed_boundary_distance_px = -12.3456f; a.rejected = true;
        yolo_event_log::SpatialMaskOutsideDetection b = a;
        b.raw_index = 2; b.rejected = false; b.signed_boundary_distance_px = 3.5f;
        r3.spatial_mask.outside_detections = {a, b};
        logger.Enqueue(r3);
        // 4. A synthetic runtime detection outside recording.
        auto r4 = base(folder, 4);
        r4.record_active = false;
        r4.recording_frame_id = 0;
        r4.status = "detections";
        r4.synthetic_runtime_detection = true;
        r4.detection_source = "synthetic_center_box";
        r4.spatial_mask.policy = policy;
        r4.spatial_mask.policy_generation = 2;
        r4.detections = {detection(64.0f, 64.0f, 64.0f, 64.0f, 0, 1.0f, 0)};
        logger.Enqueue(r4);
        logger.Close();
    }
    std::ifstream produced_in(folder + "/Cam2010093_yolo_events.jsonl");
    std::stringstream produced;
    produced << produced_in.rdbuf();
    std::filesystem::remove_all(root_template);
    if (produced.str().empty()) {
        std::cerr << "no event log was written\n";
        return 1;
    }
    if (argc == 3 && std::string(argv[2]) == "--write") {
        std::ofstream(argv[1], std::ios::binary) << produced.str();
        std::cout << "wrote " << argv[1] << "\n";
        return 0;
    }
    std::ifstream golden_in(argv[1]);
    std::stringstream golden;
    golden << golden_in.rdbuf();
    if (golden.str() != produced.str()) {
        std::cout << "[FAIL] event log differs from the golden file\n--- golden\n" << golden.str()
                  << "--- produced\n" << produced.str();
        return 1;
    }
    std::cout << "[PASS] event log bytes equal the golden file (" << produced.str().size() << " bytes)\n";
    return 0;
}
