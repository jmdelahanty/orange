#pragma once

#include "yolo_event_log_config.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_set>

namespace yolo_event_log {

struct YoloEventLogValidationStats {
    std::string path;
    std::string status = "disabled";
    bool present = false;
    uint64_t rows = 0;
    uint64_t detection_rows = 0;
    uint64_t zero_rows = 0;
    uint64_t parse_errors = 0;
    uint64_t schema_errors = 0;
    uint64_t sequence_errors = 0;
    uint64_t cadence_errors = 0;
    uint64_t metadata_rows = 0;
    uint64_t metadata_join_misses = 0;
    std::string error;
};

std::unordered_set<uint64_t> read_recording_metadata_frame_ids(
    const std::filesystem::path& metadata_path,
    uint64_t* row_count);

YoloEventLogValidationStats summarize_yolo_event_log(
    const std::string& recording_folder,
    const std::string& camera_serial,
    const SyntheticYoloEventConfig& config);

}  // namespace yolo_event_log
