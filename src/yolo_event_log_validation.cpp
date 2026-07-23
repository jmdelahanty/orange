#include "yolo_event_log_validation.h"

#include "json.hpp"

#include <fstream>
#include <limits>

namespace yolo_event_log {
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

bool json_bool_or_default(const nlohmann::json& object,
                          const char* key,
                          const bool fallback,
                          bool* valid_out = nullptr)
{
    if (valid_out) {
        *valid_out = false;
    }
    if (!object.is_object()) {
        return fallback;
    }
    const auto it = object.find(key);
    if (it == object.end() || !it->is_boolean()) {
        return fallback;
    }
    if (valid_out) {
        *valid_out = true;
    }
    return it->get<bool>();
}

}  // namespace

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
    const SyntheticYoloEventConfig& config)
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
        std::filesystem::path(recording_folder) / ("Cam" + camera_serial + "_meta.csv");
    const std::unordered_set<uint64_t> metadata_frame_ids =
        read_recording_metadata_frame_ids(metadata_path, &stats.metadata_rows);

    std::ifstream file(event_path);
    if (!file) {
        stats.status = "fail";
        stats.error = "failed to open yolo event log";
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

        if (json_string_or_default(event, "schema_id", "") != "orange.yolo_event" ||
            json_u64_or_default(event.value("schema_version", nlohmann::json()), 0) != 1 ||
            json_string_or_default(event, "event_kind", "") != "yolo_result") {
            stats.schema_errors++;
        }

        const uint64_t sequence =
            json_u64_or_default(event.value("event_sequence", nlohmann::json()), 0);
        if (sequence != stats.rows) {
            stats.sequence_errors++;
        }

        const nlohmann::json frame = event.value("frame", nlohmann::json::object());
        const nlohmann::json yolo = event.value("yolo", nlohmann::json::object());
        const nlohmann::json detections = event.value("detections", nlohmann::json::array());
        if (!frame.is_object() || !yolo.is_object() || !detections.is_array()) {
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

        const std::string status = json_string_or_default(yolo, "status", "");
        const uint64_t detection_count =
            json_u64_or_default(yolo.value("detection_count", nlohmann::json()),
                                std::numeric_limits<uint64_t>::max());

        const auto spatial_it = event.find("spatial_mask");
        if (spatial_it != event.end()) {
            bool spatial_error = !spatial_it->is_object();
            const nlohmann::json spatial = spatial_it->is_object()
                ? *spatial_it
                : nlohmann::json::object();
            const std::string spatial_mode =
                json_string_or_default(spatial, "mode", "");
            const bool known_spatial_mode =
                spatial_mode == "off" ||
                spatial_mode == "audit" ||
                spatial_mode == "gate_only" ||
                spatial_mode == "gate_and_input_mask";
            const nlohmann::json input_mask = spatial.value(
                "input_mask", nlohmann::json::object());
            const nlohmann::json centroid_gate = spatial.value(
                "centroid_gate", nlohmann::json::object());
            const nlohmann::json spatial_result = spatial.value(
                "result", nlohmann::json::object());
            const nlohmann::json outside = spatial.value(
                "outside_detections", nlohmann::json::array());
            bool input_enabled_valid = false;
            const bool input_enabled = json_bool_or_default(
                input_mask, "enabled", false, &input_enabled_valid);
            bool gate_evaluated_valid = false;
            const bool gate_evaluated = json_bool_or_default(
                centroid_gate, "evaluated", false, &gate_evaluated_valid);
            bool gate_enforced_valid = false;
            const bool gate_enforced = json_bool_or_default(
                centroid_gate, "enforced", false, &gate_enforced_valid);
            const uint64_t raw_count = json_u64_or_default(
                spatial_result.value("raw_detection_count", nlohmann::json()),
                std::numeric_limits<uint64_t>::max());
            const uint64_t inside_count = json_u64_or_default(
                spatial_result.value("inside_detection_count", nlohmann::json()),
                std::numeric_limits<uint64_t>::max());
            const uint64_t outside_count = json_u64_or_default(
                spatial_result.value("outside_detection_count", nlohmann::json()),
                std::numeric_limits<uint64_t>::max());
            const uint64_t downstream_count = json_u64_or_default(
                spatial_result.value("downstream_detection_count", nlohmann::json()),
                std::numeric_limits<uint64_t>::max());
            spatial_error = spatial_error ||
                json_string_or_default(spatial, "schema_id", "") !=
                    "orange.analytics.spatial_mask_runtime" ||
                json_u64_or_default(
                    spatial.value("schema_version", nlohmann::json()), 0) != 1 ||
                !known_spatial_mode ||
                !spatial_result.is_object() ||
                !outside.is_array() ||
                downstream_count != detection_count ||
                outside.size() != outside_count ||
                !input_enabled_valid ||
                !gate_evaluated_valid ||
                !gate_enforced_valid;

            if (known_spatial_mode && spatial_mode == "off") {
                spatial_error = spatial_error ||
                    input_enabled || gate_evaluated || gate_enforced ||
                    inside_count != 0 || outside_count != 0 ||
                    raw_count != downstream_count;
            } else if (known_spatial_mode) {
                const bool audit = spatial_mode == "audit";
                const bool masked = spatial_mode == "gate_and_input_mask";
                spatial_error = spatial_error ||
                    !gate_evaluated ||
                    gate_enforced == audit ||
                    input_enabled != masked ||
                    raw_count != inside_count + outside_count ||
                    downstream_count != (audit ? raw_count : inside_count) ||
                    json_u64_or_default(
                        spatial.value("policy_generation", nlohmann::json()), 0) == 0;
                const nlohmann::json source = spatial.value(
                    "source", nlohmann::json::object());
                const std::string source_sha = json_string_or_default(
                    source, "sha256", "");
                spatial_error = spatial_error ||
                    json_string_or_default(source, "artifact_id", "").empty() ||
                    source_sha.rfind("sha256:", 0) != 0;
            }
            if (outside.is_array()) {
                for (const nlohmann::json& decision : outside) {
                    const std::string expected = spatial_mode == "audit"
                        ? "would_reject"
                        : "rejected";
                    if (!decision.is_object() ||
                        json_string_or_default(decision, "decision", "") != expected ||
                        json_string_or_default(
                            decision, "rejected_reason", "") !=
                            "outside_valid_detection_region") {
                        spatial_error = true;
                        break;
                    }
                }
            }
            if (spatial_error) {
                stats.schema_errors++;
            }
        }
        const std::string detection_source =
            json_string_or_default(yolo, "detection_source", "");
        bool synthetic_runtime_detection_valid = false;
        const bool synthetic_runtime_detection =
            json_bool_or_default(yolo,
                                 "synthetic_runtime_detection",
                                 false,
                                 &synthetic_runtime_detection_valid);
        bool production_detection_valid_present = false;
        const bool production_detection_valid =
            json_bool_or_default(yolo,
                                 "production_detection_valid",
                                 true,
                                 &production_detection_valid_present);
        const bool known_detection_source =
            detection_source.empty() ||
            detection_source == "model" ||
            detection_source == "synthetic_center_box";
        const bool invalid_runtime_synthetic_markers =
            detection_source == "synthetic_center_box" &&
            (!synthetic_runtime_detection_valid ||
             !synthetic_runtime_detection ||
             !production_detection_valid_present ||
             production_detection_valid);
        const bool invalid_production_marker =
            synthetic_runtime_detection &&
            (!production_detection_valid_present || production_detection_valid);
        if (!known_detection_source ||
            invalid_runtime_synthetic_markers ||
            invalid_production_marker) {
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
                detections.size() != 1)) {
                stats.cadence_errors++;
            }
        } else if (status == "zero_detections") {
            stats.zero_rows++;
            if (validate_synthetic_cadence &&
                (should_have_detection ||
                !config.emit_zero_detections ||
                detection_count != 0 ||
                !detections.empty())) {
                stats.cadence_errors++;
            }
        } else if (status == "timeout") {
            stats.timeout_rows++;
        } else if (status == "failed") {
            stats.failed_rows++;
        } else {
            stats.schema_errors++;
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
        stats.error = "validation errors";
    } else {
        stats.status = "pass";
    }
    return stats;
}

}  // namespace yolo_event_log
