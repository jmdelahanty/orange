// Image levels: whole-image brightness and saturation of one full-resolution
// Mono8 frame, checked against the expectation stored in each camera's config
// (camera JSON `image_levels`, orange.camera.image_levels v1). The result goes
// into the recording start snapshot (session.image_levels_check,
// orange.recording.image_levels_check v1). Policy is warn-only: a failed check
// never refuses a stream or a recording.
#pragma once

#include "image_levels_expectation.h"
#include "json.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace orange::image_levels {

inline constexpr const char* kCameraImageLevelsSchemaId = "orange.camera.image_levels";
inline constexpr int kCameraImageLevelsSchemaVersion = 1;
inline constexpr const char* kRecordingImageLevelsCheckSchemaId = "orange.recording.image_levels_check";
inline constexpr int kRecordingImageLevelsCheckSchemaVersion = 1;
inline constexpr const char* kPolicy = "warn_only";
inline constexpr int kSaturationValue = 255;

// Statistic names are shared with the empty-background capture descriptor.
struct Statistics {
    uint64_t pixel_count = 0;
    double mean = 0.0;
    double zero_fraction = 0.0;
    double saturated_fraction = 0.0;
    // Smallest value whose cumulative fraction reaches the level.
    int p01 = 0, p05 = 0, p50 = 0, p95 = 0, p99 = 0, p999 = 0;
};

Statistics ComputeStatistics(const uint8_t* pixels, size_t count);
Statistics StatisticsFromHistogram(const uint64_t counts[256]);
nlohmann::json StatisticsToJson(const Statistics& stats);

// The camera settings an expectation depends on.
// Unsigned integers, as CameraParams holds them, so equality is exact.
struct LevelSettings {
    unsigned exposure = 0, gain = 0, iris = 0, focus = 0;
    unsigned width = 0, height = 0, offset_x = 0, offset_y = 0;
};
nlohmann::json SettingsToJson(const LevelSettings& settings);

// Throws std::runtime_error naming the field when the block is malformed.
CameraExpectation ParseCameraExpectation(const nlohmann::json& camera_config);
// Empty object when not configured.
nlohmann::json BuildCameraExpectation(const CameraExpectation& expectation);

struct Measurement {
    bool ok = false;
    std::string error;
    std::string measured_at_utc;
    std::string source;  // e.g. full_resolution_native_stream_snapshot
    std::string native_source_storage;
    uint64_t camera_frame_id = 0;
    uint64_t camera_timestamp_ns = 0;
    uint64_t timestamp_sys_ns = 0;
    LevelSettings settings;
    Statistics statistics;
};
nlohmann::json MeasurementToJson(const Measurement& measurement);

// One camera's check. status: ok, warning, no_expectation, measurement_failed,
// not_measured. warnings: mean_out_of_band, saturated_fraction_above_limit.
nlohmann::json EvaluateCamera(const CameraExpectation& expectation,
                              const LevelSettings& live_settings,
                              const std::optional<Measurement>& measurement,
                              const std::optional<Measurement>& first_measurement,
                              bool rehomed,
                              const std::string& not_measured_reason);

bool MeanInBand(const CameraExpectation& expectation, double mean);

// Why this measurement may not be saved as an expectation with this
// saturation limit, or empty when it may: an expectation the measurement
// already breaks warns on every check (2026-11-03, all four cameras).
std::string SaveRefusal(const Measurement& measurement, double max_saturated_fraction);

// Settings in the expectation's basis that differ from the live ones
// (names only; both values are in the block).
std::vector<std::string> BasisSettingsDiffer(const CameraExpectation& expectation, const LevelSettings& live);

nlohmann::json BuildRecordingBlock(bool enabled, bool rehome_iris, const nlohmann::json& cameras_by_serial);

// Per-serial results produced on the GUI thread and read by the recording
// start (which may run on another thread).
class ResultStore {
public:
    void Set(const std::string& serial, nlohmann::json result);
    void Clear();
    std::optional<nlohmann::json> Get(const std::string& serial) const;
    void SetCheckSettings(bool enabled, bool rehome_iris);
    // The recording block for these cameras; a camera with no result is
    // not_measured (no_check_result), and every measured camera lists the
    // settings that changed between its measurement and the record start.
    nlohmann::json BuildRecordingBlockAt(const std::vector<std::pair<std::string, LevelSettings>>& cameras) const;

private:
    mutable std::mutex mutex_;
    nlohmann::json by_serial_ = nlohmann::json::object();
    bool enabled_ = false;
    bool rehome_iris_ = false;
};
ResultStore& GuiResultStore();

}  // namespace orange::image_levels
