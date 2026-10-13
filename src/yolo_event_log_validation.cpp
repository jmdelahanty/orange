#include "yolo_event_log_validation.h"
#include "event_log_reader.h"

#include "json.hpp"

#include <fstream>
#include <variant>

namespace yolo_event_log {
std::unordered_set<uint64_t> read_recording_metadata_frame_ids(
    const std::filesystem::path& metadata_path,
    uint64_t* row_count)
{
    std::unordered_set<uint64_t> frame_ids;
    if (row_count) {
        *row_count = 0;
    }

    std::ifstream file(metadata_path);
    if (!file) {
        return frame_ids;
    }

    std::string line;
    bool first_line = true;
    while (std::getline(file, line)) {
        if (first_line) {
            first_line = false;
            continue;
        }
        const size_t comma = line.find(',');
        const std::string frame_id_text =
            comma == std::string::npos ? line : line.substr(0, comma);
        try {
            const uint64_t frame_id = static_cast<uint64_t>(std::stoull(frame_id_text));
            if (frame_id > 0) {
                frame_ids.insert(frame_id);
                if (row_count) {
                    (*row_count)++;
                }
            }
        } catch (...) {
        }
    }
    return frame_ids;
}

YoloEventLogValidationStats summarize_yolo_event_log(
    const std::string& recording_folder,
    const std::string& camera_serial,
    const SyntheticYoloEventConfig& config,
    const std::filesystem::path& source_metadata)
{
    YoloEventLogValidationStats stats;
    const bool validate_synthetic_cadence = config.enabled();
    const bool validate_real_worker = config.mode == "real";
    if (!validate_synthetic_cadence && !validate_real_worker) {
        return stats;
    }
    stats.status = "missing";
    if (recording_folder.empty() || camera_serial.empty()) {
        stats.error = "missing recording folder or camera serial";
        return stats;
    }

    const std::filesystem::path event_path =
        std::filesystem::path(recording_folder) / ("Cam" + camera_serial + "_yolo_events.jsonl");
    stats.path = event_path.string();
    std::error_code fs_error;
    if (!std::filesystem::exists(event_path, fs_error) || fs_error) {
        stats.error = "missing yolo event log";
        return stats;
    }
    stats.present = true;

    const std::filesystem::path metadata_path =
        source_metadata.empty() ? std::filesystem::path(recording_folder) / ("Cam" + camera_serial + "_meta.csv") : source_metadata;
    const std::unordered_set<uint64_t> metadata_frame_ids =
        read_recording_metadata_frame_ids(metadata_path, &stats.metadata_rows);
    if (!source_metadata.empty() && metadata_frame_ids.empty()) {
        stats.status = "fail"; stats.error = "required source metadata is missing or empty"; return stats;
    }

    std::ifstream file(event_path);
    if (!file) {
        stats.status = "fail";
        stats.error = "failed to open yolo event log";
        return stats;
    }

    std::string line;
    event_log_reader::YoloSession session;
    while (std::getline(file, line)) {
        if (line.empty()) {
            continue;
        }
        event_log_reader::yolo::OrangeYoloEventV3 parsed;
        std::string parse_error;
        const event_log_reader::LineStatus line_status =
            event_log_reader::parse_yolo_line(line, &parsed, &parse_error);
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
        if (const auto* header = std::get_if<event_log_reader::yolo::OrangeYoloEventV3SessionHeader>(&parsed)) {
            stats.header_rows++;
            if (stats.rows != 0 || session.header) stats.schema_errors++;  // one header, first
            session.Absorb(*header);
            continue;
        }
        if (const auto* policy = std::get_if<event_log_reader::yolo::OrangeYoloEventV3SpatialMaskPolicy>(&parsed)) {
            stats.header_rows++;
            session.Absorb(*policy);
            continue;
        }
        const auto& event = std::get<event_log_reader::yolo::OrangeYoloEventV3YoloResult>(parsed);
        stats.rows++;
        if (!session.header) {
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

        const std::string& status = event.yolo.status;
        const uint64_t detection_count = event.yolo.detection_count;

        // The result's counts against the policy its generation names. The policy itself (mode,
        // gate and input-mask flags, source identity and digest form) was checked against
        // orange.analytics.spatial_mask_runtime v1 when its header / policy line was read; a
        // generation without a valid policy makes every frame line naming it an error.
        {
            const auto* policy = session.Policy(event);
            if (!policy) {
                const auto why = session.policy_errors.find(event.spatial_mask.policy_generation);
                if (why != session.policy_errors.end() && stats.error.empty()) {
                    stats.error = why->second;
                }
            }
            const auto& result = event.spatial_mask.result;
            const uint64_t raw_count = result.raw_detection_count;
            const uint64_t inside_count = result.inside_detection_count;
            const uint64_t outside_count = result.outside_detection_count;
            const uint64_t downstream_count = result.downstream_detection_count;
            bool spatial_error =
                policy == nullptr ||
                downstream_count != detection_count ||
                event.spatial_mask.outside_detections.size() != outside_count;
            if (policy != nullptr && policy->mode == "off") {
                spatial_error = spatial_error ||
                    inside_count != 0 || outside_count != 0 ||
                    raw_count != downstream_count;
            } else if (policy != nullptr) {
                const bool audit = policy->mode == "audit";
                spatial_error = spatial_error ||
                    raw_count != inside_count + outside_count ||
                    downstream_count != (audit ? raw_count : inside_count) ||
                    event.spatial_mask.policy_generation == 0;
            }
            const std::string expected_decision =
                policy != nullptr && policy->mode == "audit" ? "would_reject" : "rejected";
            for (const auto& decision : event.spatial_mask.outside_detections) {
                spatial_error = spatial_error || decision.decision != expected_decision;
            }
            if (spatial_error) {
                stats.schema_errors++;
            }
        }
        const bool synthetic_center_box = event.yolo.detection_source == "synthetic_center_box";
        const bool synthetic_runtime_detection = event.yolo.synthetic_runtime_detection;
        const bool production_detection_valid = event.yolo.production_detection_valid;
        if ((synthetic_center_box && (!synthetic_runtime_detection || production_detection_valid)) ||
            (synthetic_runtime_detection && production_detection_valid)) {
            stats.schema_errors++;
        }
        const bool should_have_detection =
            config.every_n_frames > 0 &&
            recording_frame_id > 0 &&
            (recording_frame_id % static_cast<uint64_t>(config.every_n_frames)) == 0;

        if (status == "detections") {
            stats.detection_rows++;
            if (validate_synthetic_cadence &&
                (!should_have_detection ||
                detection_count != 1 ||
                event.detections.size() != 1)) {
                stats.cadence_errors++;
            }
        } else if (status == "zero_detections") {
            stats.zero_rows++;
            if (validate_synthetic_cadence &&
                (should_have_detection ||
                !config.emit_zero_detections ||
                detection_count != 0 ||
                !event.detections.empty())) {
                stats.cadence_errors++;
            }
        } else if (status == "timeout") {
            stats.timeout_rows++;
        } else {
            stats.failed_rows++;  // the schema allows no other status
        }
    }

    if (stats.rows == 0) {
        stats.status = "fail";
        stats.error = "zero yolo event rows";
    } else if (stats.parse_errors != 0 ||
               stats.schema_errors != 0 ||
               stats.sequence_errors != 0 ||
               stats.cadence_errors != 0 ||
               stats.metadata_join_misses != 0) {
        stats.status = "fail";
        stats.error = stats.error.empty() ? "validation errors" : "validation errors; first line not matching "
                      "orange.yolo_event v3: " + stats.error;
    } else {
        stats.status = "pass";
    }
    return stats;
}

}  // namespace yolo_event_log
