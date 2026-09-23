// tools/shaman_v2_reader_probe.cpp
//
// Reference reader for the Shaman v2 live-state queues (/shm_cam_<serial>_v2):
// drains one queue per camera for a fixed time, counts what a consumer such
// as Citrus would see, and prints a JSON summary plus one sample pose slot.
// It is the reader rule in executable form: a slot carries pose when
// pose_status == kPoses and an object has kObjectHasPose and keypoint_count>0;
// keypoints are full-frame pixels; label_id is the keypoint index in the
// model's training order. The queue is single-consumer with a destructive
// pop, so run only one reader per camera.
//
// usage: shaman_v2_reader_probe --serials 2010093,2010094 --seconds 90 [--json out.json]
#include "shaman_v2.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct CameraCounts {
    uint64_t slots = 0;
    uint64_t base_only = 0;          // detection pending/not scheduled, no pose
    uint64_t with_yolo = 0;          // detection_status detections/zero
    uint64_t pose_status_poses = 0;  // pose_status == kPoses
    uint64_t pose_objects = 0;       // objects with kObjectHasPose && keypoint_count>0
    uint64_t pose_no_result = 0;
    uint64_t pose_failed = 0;
    uint64_t inconsistent_pose = 0;  // pose_status poses but no object carries pose
    uint64_t sequence_gaps = 0;
    uint64_t last_sequence = 0;
    uint64_t first_state_frame_id = 0;
    uint64_t last_state_frame_id = 0;
    uint64_t max_recording_frame_id = 0;
    std::set<uint64_t> state_frame_ids;
    bool have_sample = false;
    shaman_v2::Slot sample{};
};

std::string slot_json(const std::string& serial, const shaman_v2::Slot& s)
{
    std::ostringstream o;
    o << "{\"serial\":\"" << serial << "\",\"state_frame_id\":" << s.state_frame_id
      << ",\"source_frame_id\":" << s.source_frame_id << ",\"recording_frame_id\":" << s.recording_frame_id
      << ",\"camera_frame_id\":" << s.camera_frame_id << ",\"sequence_id\":" << s.sequence_id
      << ",\"camera_timestamp_ns\":" << s.camera_timestamp_ns << ",\"detection_status\":" << s.detection_status
      << ",\"pose_status\":" << s.pose_status << ",\"pose_model_id_hash\":" << s.pose_model_id_hash
      << ",\"pose_skeleton_id_hash\":" << s.pose_skeleton_id_hash << ",\"objects\":[";
    for (uint32_t i = 0; i < s.object_count && i < shaman_v2::kMaxObjects; ++i) {
        const auto& ob = s.objects[i];
        if (i) o << ",";
        o << "{\"bbox\":[" << ob.x_px << "," << ob.y_px << "," << ob.width_px << "," << ob.height_px << "]"
          << ",\"confidence\":" << ob.confidence << ",\"flags\":" << ob.flags << ",\"track_id\":" << ob.track_id
          << ",\"keypoints\":[";
        for (uint32_t k = 0; k < ob.keypoint_count && k < shaman_v2::kMaxKeypointsPerObject; ++k) {
            const auto& kp = ob.keypoints[k];
            if (k) o << ",";
            o << "{\"label_id\":" << kp.label_id << ",\"x_px\":" << kp.x_px << ",\"y_px\":" << kp.y_px
              << ",\"confidence\":" << kp.confidence << ",\"visible\":" << ((kp.flags & shaman_v2::kKeypointVisible) ? 1 : 0) << "}";
        }
        o << "]}";
    }
    o << "]}";
    return o.str();
}

}  // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> serials;
    double seconds = 60.0;
    std::string json_out;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--serials" && i + 1 < argc) {
            std::stringstream ss(argv[++i]);
            std::string item;
            while (std::getline(ss, item, ',')) if (!item.empty()) serials.push_back(item);
        } else if (a == "--seconds" && i + 1 < argc) {
            seconds = std::atof(argv[++i]);
        } else if (a == "--json" && i + 1 < argc) {
            json_out = argv[++i];
        } else {
            std::cerr << "usage: " << argv[0] << " --serials <csv> [--seconds N] [--json out.json]" << std::endl;
            return 2;
        }
    }
    if (serials.empty()) {
        std::cerr << "no serials" << std::endl;
        return 2;
    }
    std::map<std::string, std::unique_ptr<shaman_v2::SharedLiveStateQueue>> readers;
    std::map<std::string, CameraCounts> counts;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        for (const auto& serial : serials) {
            if (!readers.count(serial)) {
                const std::string name = shaman_v2::queue_name_for_camera_serial(serial);
                try {
                    readers[serial] = std::make_unique<shaman_v2::SharedLiveStateQueue>(name, false);
                    std::cout << "[v2_reader] attached " << name << std::endl;
                } catch (const std::exception&) {
                    continue;  // queue not created yet; retry
                }
            }
            auto& reader = *readers[serial];
            auto& c = counts[serial];
            shaman_v2::Slot s;
            while (reader.pop(s)) {
                c.slots++;
                if (c.last_sequence && s.sequence_id > c.last_sequence + 1) c.sequence_gaps++;
                c.last_sequence = s.sequence_id;
                if (!c.first_state_frame_id) c.first_state_frame_id = s.state_frame_id;
                c.last_state_frame_id = s.state_frame_id;
                c.state_frame_ids.insert(s.state_frame_id);
                if (s.recording_frame_id > c.max_recording_frame_id) c.max_recording_frame_id = s.recording_frame_id;
                const auto det = static_cast<shaman_v2::DetectionStatus>(s.detection_status);
                const auto pose = static_cast<shaman_v2::PoseStatus>(s.pose_status);
                if (det == shaman_v2::DetectionStatus::kDetections || det == shaman_v2::DetectionStatus::kZeroDetections) c.with_yolo++;
                else c.base_only++;
                if (pose == shaman_v2::PoseStatus::kNoResult) c.pose_no_result++;
                if (pose == shaman_v2::PoseStatus::kFailed) c.pose_failed++;
                if (pose == shaman_v2::PoseStatus::kPoses) {
                    c.pose_status_poses++;
                    bool any = false;
                    for (uint32_t i = 0; i < s.object_count && i < shaman_v2::kMaxObjects; ++i) {
                        if ((s.objects[i].flags & shaman_v2::kObjectHasPose) && s.objects[i].keypoint_count > 0) {
                            any = true;
                            c.pose_objects++;
                        }
                    }
                    if (!any) c.inconsistent_pose++;
                    else if (!c.have_sample) { c.have_sample = true; c.sample = s; }
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::ostringstream summary;
    summary << "{\"seconds\":" << seconds << ",\"cameras\":{";
    bool first = true;
    for (const auto& serial : serials) {
        const auto& c = counts[serial];
        if (!first) summary << ",";
        first = false;
        summary << "\"" << serial << "\":{\"attached\":" << (readers.count(serial) ? 1 : 0) << ",\"slots\":" << c.slots
                << ",\"distinct_state_frame_ids\":" << c.state_frame_ids.size() << ",\"first_state_frame_id\":" << c.first_state_frame_id
                << ",\"last_state_frame_id\":" << c.last_state_frame_id << ",\"max_recording_frame_id\":" << c.max_recording_frame_id
                << ",\"base_only\":" << c.base_only << ",\"with_yolo\":" << c.with_yolo << ",\"pose_status_poses\":" << c.pose_status_poses
                << ",\"pose_objects\":" << c.pose_objects << ",\"pose_no_result\":" << c.pose_no_result << ",\"pose_failed\":" << c.pose_failed
                << ",\"inconsistent_pose\":" << c.inconsistent_pose << ",\"sequence_gaps\":" << c.sequence_gaps << "}";
    }
    summary << "}}";
    std::cout << "[v2_reader] summary " << summary.str() << std::endl;
    for (const auto& serial : serials) {
        const auto& c = counts[serial];
        if (c.have_sample) std::cout << "[v2_reader] sample_pose_slot " << slot_json(serial, c.sample) << std::endl;
    }
    if (!json_out.empty()) {
        std::ofstream f(json_out);
        f << summary.str() << std::endl;
    }
    return 0;
}
