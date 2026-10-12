// Golden bytes for the pose event log (Cam<serial>_pose_events.jsonl).
//
// Writes a fixed set of records covering every line kind and optional field through
// PoseEventLogger and compares the file with
// tools/fixtures/generated_outputs/golden/Cam2010093_pose_events.jsonl, which was produced by the
// hand-written writer before it switched to the generated orange.pose_event v3 code (2026-10-12).
// Any byte change to the log fails here.
//
//   pose_event_log_golden_tests <golden file>            compare
//   pose_event_log_golden_tests <golden file> --write    regenerate (only on a deliberate change)
#include "pose_event_log.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>

namespace {
pose_event_log::PoseResultRecord base(const std::string& folder, uint64_t frame) {
    pose_event_log::PoseResultRecord r;
    r.recording_folder = folder;
    r.backend = "tensorrt";
    r.mode = "real";
    r.model_id = "golden_pose_model";
    r.skeleton_id = "golden_skeleton";
    r.skeleton_path = "/golden/skeleton.json";
    r.skeleton_sha256 = std::string(64, 'b');
    r.engine_sha256 = "c" + std::string(63, '0');
    r.ipc_skeleton_id_hash = 0x1234567890abcdefULL;
    r.ipc_model_id_hash = 0xfedcba0987654321ULL;
    r.gpu_id = 5;
    r.keypoint_labels = {"head", "mid", "tail"};
    r.local_frame_id = 1000 + frame;
    r.camera_frame_id = 65530 + frame;
    r.recording_frame_id = frame;
    r.record_active = true;
    r.camera_timestamp = 1791000000000000000ULL + frame * 10000000ULL;
    r.timestamp_sys_ns = r.camera_timestamp - 36990000000ULL;
    r.event_epoch_us = 1791000000000000ULL + frame * 10000ULL;
    r.event_monotonic_us = 5000000ULL + frame * 10000ULL;
    r.source_width = 4512;
    r.source_height = 4512;
    r.crop_x_px = 1000;
    r.crop_y_px = 1200;
    r.crop_width_px = 384;
    r.crop_height_px = 384;
    r.timing.capture_to_detect_done_ms = 1.8456789;
    r.timing.detect_to_crop_worker_start_ms = 0.0123456;
    r.timing.crop_worker_start_to_crop_ready_ms = 0.1004999;
    r.timing.detect_to_crop_ready_ms = 0.1128455;
    r.timing.crop_ready_to_pose_start_ms = 0.05;
    r.timing.pose_start_to_pose_done_ms = 0.4444444;
    r.timing.capture_to_pose_done_ms = 2.4321;
    return r;
}

pose_event_log::PoseInstanceRecord instance(int index, const std::string& label, double confidence) {
    pose_event_log::PoseInstanceRecord p;
    p.index = index;
    p.label = label;
    p.confidence = confidence;
    for (int k = 0; k < 3; ++k) {
        pose_event_log::PoseKeypointRecord kp;
        kp.x_px = 10.0 + 33.33333 * k;
        kp.y_px = 200.98765 - 1.23456 * k;
        kp.confidence = 0.912345 - 0.3 * k;
        kp.visible = k != 2;
        p.keypoints.push_back(kp);
    }
    return p;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: pose_event_log_golden_tests <golden file> [--write]\n";
        return 2;
    }
    char root_template[] = "/tmp/pose_golden_XXXXXX";
    if (!mkdtemp(root_template)) return 2;
    const std::string folder = std::string(root_template) + "/golden_recording";
    {
        pose_event_log::PoseEventLogger logger("2010093", 3, "PoseWorker");
        // 1. Header + a frame without a detection.
        auto r1 = base(folder, 1);
        r1.status = "no_result";
        logger.Enqueue(r1);
        // 2. Two instances, one without a label.
        auto r2 = base(folder, 2);
        r2.status = "poses";
        r2.has_detection = true;
        r2.detection_confidence = 0.876543;
        r2.poses = {instance(0, "fish", 0.9876543), instance(1, "", 0.5)};
        logger.Enqueue(r2);
        // 3. An error on a blank frame outside recording.
        auto r3 = base(folder, 3);
        r3.status = "failed";
        r3.error = "golden: example error text";
        r3.blank_frame = true;
        r3.record_active = false;
        r3.recording_frame_id = 0;
        logger.Enqueue(r3);
        logger.Close();
        logger.Stop();
    }
    std::ifstream produced_in(folder + "/Cam2010093_pose_events.jsonl");
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
