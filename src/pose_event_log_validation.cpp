#include "pose_event_log_validation.h"

#include "json.hpp"
#include "yolo_event_log_validation.h"

#include <fstream>
#include <limits>

namespace pose_event_log {
namespace {

uint64_t json_u64_or_default(const nlohmann::json& value, uint64_t fallback)
{
    if (value.is_number_unsigned()) {
        return value.get<uint64_t>();
    }
    if (value.is_number_integer()) {
        const auto signed_value = value.get<int64_t>();
        return signed_value >= 0 ? static_cast<uint64_t>(signed_value) : fallback;
    }
    if (value.is_number_float()) {
        const double floating_value = value.get<double>();
        return floating_value >= 0.0 ? static_cast<uint64_t>(floating_value) : fallback;
    }
    if (value.is_string()) {
        try {
            return static_cast<uint64_t>(std::stoull(value.get<std::string>()));
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

std::string json_string_or_default(const nlohmann::json& object,
                                   const char* key,
                                   const std::string& fallback)
{
    if (!object.is_object()) {
        return fallback;
    }
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string()) {
        return fallback;
    }
    return it->get<std::string>();
}

}  // namespace

PoseEventLogValidationStats summarize_pose_event_log(
    const std::string& recording_folder,
    const std::string& camera_serial,
    const PoseEventLogValidationConfig& config)
{
    PoseEventLogValidationStats stats;
    if (!config.enabled()) {
        return stats;
    }

    stats.status = "missing";
    if (recording_folder.empty() || camera_serial.empty()) {
        stats.error = "missing recording folder or camera serial";
        return stats;
    }

    const std::filesystem::path event_path =
        std::filesystem::path(recording_folder) / ("Cam" + camera_serial + "_pose_events.jsonl");
    stats.path = event_path.string();
    std::error_code fs_error;
    if (!std::filesystem::exists(event_path, fs_error) || fs_error) {
        stats.error = "missing pose event log";
        return stats;
    }
    stats.present = true;

    const std::filesystem::path metadata_path =
        std::filesystem::path(recording_folder) / ("Cam" + camera_serial + "_meta.csv");
    const std::unordered_set<uint64_t> metadata_frame_ids =
        yolo_event_log::read_recording_metadata_frame_ids(metadata_path, &stats.metadata_rows);

    std::ifstream file(event_path);
    if (!file) {
        stats.status = "fail";
        stats.error = "failed to open pose event log";
        return stats;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) {
            continue;
        }
        stats.rows++;
        const nlohmann::json event = nlohmann::json::parse(line, nullptr, false);
        if (event.is_discarded() || !event.is_object()) {
            stats.parse_errors++;
            continue;
        }

        if (json_string_or_default(event, "schema_id", "") != "orange.pose_event" ||
            json_u64_or_default(event.value("schema_version", nlohmann::json()), 0) != 1 ||
            json_string_or_default(event, "event_kind", "") != "pose_result") {
            stats.schema_errors++;
        }

        const uint64_t sequence =
            json_u64_or_default(event.value("event_sequence", nlohmann::json()), 0);
        if (sequence != stats.rows) {
            stats.sequence_errors++;
        }

        const nlohmann::json frame = event.value("frame", nlohmann::json::object());
        const nlohmann::json pose = event.value("pose", nlohmann::json::object());
        const nlohmann::json poses = event.value("poses", nlohmann::json::array());
        if (!frame.is_object() || !pose.is_object() || !poses.is_array()) {
            stats.schema_errors++;
            continue;
        }

        const uint64_t recording_frame_id =
            json_u64_or_default(frame.value("recording_frame_id", nlohmann::json()), 0);
        if (recording_frame_id == 0) {
            stats.schema_errors++;
        } else if (!metadata_frame_ids.empty() &&
                   metadata_frame_ids.find(recording_frame_id) == metadata_frame_ids.end()) {
            stats.metadata_join_misses++;
        }

        const std::string status = json_string_or_default(pose, "status", "");
        if (status == "no_result") {
            stats.no_result_rows++;
        } else if (status == "poses") {
            stats.result_rows++;
        } else if (status == "failed") {
            stats.failed_rows++;
        } else {
            stats.schema_errors++;
        }

        if (config.validate_noop()) {
            const std::string backend = json_string_or_default(pose, "backend", "");
            const std::string mode = json_string_or_default(pose, "mode", "");
            const uint64_t instance_count =
                json_u64_or_default(pose.value("instance_count", nlohmann::json()),
                                    std::numeric_limits<uint64_t>::max());
            if (backend != "noop" ||
                mode != "noop" ||
                status != "no_result" ||
                instance_count != 0 ||
                !poses.empty()) {
                stats.noop_errors++;
            }
        }
    }

    if (stats.rows == 0) {
        stats.status = "fail";
        stats.error = "zero pose event rows";
    } else if (stats.parse_errors != 0 ||
               stats.schema_errors != 0 ||
               stats.sequence_errors != 0 ||
               stats.noop_errors != 0 ||
               stats.metadata_join_misses != 0) {
        stats.status = "fail";
        stats.error = "validation errors";
    } else {
        stats.status = "pass";
    }
    return stats;
}

}  // namespace pose_event_log
