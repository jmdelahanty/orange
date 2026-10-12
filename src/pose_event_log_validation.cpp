#include "pose_event_log_validation.h"
#include "event_log_reader.h"

#include "yolo_event_log_validation.h"

#include <fstream>
#include <optional>
#include <variant>

namespace pose_event_log {

PoseEventLogValidationStats summarize_pose_event_log(
    const std::string& recording_folder,
    const std::string& camera_serial,
    const PoseEventLogValidationConfig& config,
    const std::filesystem::path& source_metadata)
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
        source_metadata.empty() ? std::filesystem::path(recording_folder) / ("Cam" + camera_serial + "_meta.csv") : source_metadata;
    const std::unordered_set<uint64_t> metadata_frame_ids =
        yolo_event_log::read_recording_metadata_frame_ids(metadata_path, &stats.metadata_rows);
    if (!source_metadata.empty() && metadata_frame_ids.empty()) {
        stats.status = "fail"; stats.error = "required source metadata is missing or empty"; return stats;
    }

    std::ifstream file(event_path);
    if (!file) {
        stats.status = "fail";
        stats.error = "failed to open pose event log";
        return stats;
    }

    std::string line;
    std::optional<event_log_reader::pose::OrangePoseEventV3SessionHeader> header;
    while (std::getline(file, line)) {
        if (line.empty()) {
            continue;
        }
        event_log_reader::pose::OrangePoseEventV3 parsed;
        std::string parse_error;
        const event_log_reader::LineStatus line_status =
            event_log_reader::parse_pose_line(line, &parsed, &parse_error);
        if (line_status != event_log_reader::LineStatus::kOk) {
            stats.rows++;
            if (line_status == event_log_reader::LineStatus::kInvalidJson) {
                stats.parse_errors++;
            } else {
                stats.schema_errors++;
                if (stats.error.empty()) stats.error = parse_error;
            }
            continue;
        }
        if (const auto* h = std::get_if<event_log_reader::pose::OrangePoseEventV3SessionHeader>(&parsed)) {
            stats.header_rows++;
            if (stats.rows != 0 || header) stats.schema_errors++;  // one header, first
            header = *h;
            continue;
        }
        const auto& event = std::get<event_log_reader::pose::OrangePoseEventV3PoseResult>(parsed);
        stats.rows++;
        if (!header) {
            stats.schema_errors++;
        }
        if (event.event_sequence != stats.rows) {
            stats.sequence_errors++;
        }

        const uint64_t recording_frame_id = event.frame.recording_frame_id;
        if (recording_frame_id == 0) {
            stats.schema_errors++;
        } else if (!metadata_frame_ids.empty() &&
                   metadata_frame_ids.find(recording_frame_id) == metadata_frame_ids.end()) {
            stats.metadata_join_misses++;
        }

        const std::string& status = event.pose.status;
        if (status == "no_result") {
            stats.no_result_rows++;
        } else if (status == "poses") {
            stats.result_rows++;
        } else {
            stats.failed_rows++;  // the schema allows no other status
        }
        if (event.pose.instance_count != event.poses.size()) {
            stats.schema_errors++;
        }

        if (config.validate_noop()) {
            if (!header || header->pose.backend != "noop" || header->pose.mode != "noop" ||
                status != "no_result" || !event.poses.empty()) {
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
        stats.error = stats.error.empty() ? "validation errors" : "validation errors; first line not matching "
                      "orange.pose_event v3: " + stats.error;
    } else {
        stats.status = "pass";
    }
    return stats;
}

}  // namespace pose_event_log
