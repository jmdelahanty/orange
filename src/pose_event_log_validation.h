#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_set>

namespace pose_event_log {

struct PoseEventLogValidationConfig {
    std::string mode = "off";

    bool enabled() const {
        return mode == "noop" || mode == "real";
    }

    bool validate_noop() const {
        return mode == "noop";
    }
};

struct PoseEventLogValidationStats {
    std::string path;
    std::string status = "disabled";
    bool present = false;
    uint64_t rows = 0;
    uint64_t no_result_rows = 0;
    uint64_t result_rows = 0;
    uint64_t failed_rows = 0;
    uint64_t parse_errors = 0;
    uint64_t schema_errors = 0;
    uint64_t sequence_errors = 0;
    uint64_t noop_errors = 0;
    uint64_t metadata_rows = 0;
    uint64_t metadata_join_misses = 0;
    std::string error;
};

PoseEventLogValidationStats summarize_pose_event_log(
    const std::string& recording_folder,
    const std::string& camera_serial,
    const PoseEventLogValidationConfig& config);

}  // namespace pose_event_log
