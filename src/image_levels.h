// Image levels: brightness and saturation of one full-resolution Mono8 frame,
// over the whole image and inside the dish (the camera's active rim
// observation, inner rim on the water side), checked against the expectation
// in each camera's config (camera JSON `image_levels`,
// orange.camera.image_levels v2; v1 blocks still load and check the whole
// frame). The result goes into the recording start snapshot
// (session.image_levels_check, orange.recording.image_levels_check v3).
// Policy is warn-only: a failed check never refuses a stream or a recording.
//
// One GUI process can make several recordings on one stream. The check measures
// after stream start and again after every recording (never during one: the copy
// reads the landing die), so each record start seals the newest measurement with
// its age and whether it is fresh: measured after the previous recording ended,
// at most kFreshMaxAgeSeconds old, and not superseded by a measurement still in
// progress. A stale one is still sealed, marked with its reasons; the start never
// waits for a measurement.
#pragma once

#include "image_levels_expectation.h"
#include "json.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace orange::image_levels {

inline constexpr const char* kCameraImageLevelsSchemaId = "orange.camera.image_levels";
inline constexpr int kCameraImageLevelsSchemaVersion = 2;
inline constexpr int kCameraImageLevelsSchemaVersionWholeFrameOnly = 1;  // read, never written
inline constexpr const char* kRecordingImageLevelsCheckSchemaId = "orange.recording.image_levels_check";
inline constexpr int kRecordingImageLevelsCheckSchemaVersion = 3;
// A sealed measurement older than this is stale (older_than_max_age).
inline constexpr double kFreshMaxAgeSeconds = 600.0;
inline constexpr const char* kDishInteriorLevelsSchemaId = "orange.image_levels_dish_interior";
inline constexpr int kDishInteriorLevelsSchemaVersion = 1;
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

// The dish interior on the native raster: pixel (x, y), integer indices,
// counts when (x - center_x)^2 + (y - center_y)^2 <= radius^2, in double
// arithmetic on the circle as stored in the rim observation.
struct DishRegion {
    bool available = false;
    std::string unavailable_reason;
    double center_x = 0.0, center_y = 0.0, radius = 0.0;
    // Where the circle came from: the active physical registration pointer
    // and the rim observation it selects (artifact id, schema, checksums).
    std::string source_json;
};
nlohmann::json DishRegionToJson(const DishRegion& region);
// Tightly packed width x height Mono8; pixel_count is the number of pixels
// inside the circle (0 when it misses the raster).
Statistics ComputeStatisticsInCircle(const uint8_t* pixels, int width, int height,
                                     double center_x, double center_y, double radius);
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

// The limits a GUI Save writes into the camera config, rounded to the decimals
// the panel shows, so the file holds their shortest decimal text (0.1, not
// 0.10000000149011612 from a float input; 180.1, not the fast-math
// 180.10000000000002; src/outputs/common/decimal_round.h).
inline constexpr int kExpectedMeanDecimals = 1;
inline constexpr int kMeanToleranceDecimals = 2;
inline constexpr int kMaxSaturatedDecimals = 4;
void SetSavedLimits(CameraExpectation* expectation, double measured_mean, double mean_tolerance_fraction,
                    double max_saturated_fraction);

struct Measurement {
    bool ok = false;
    std::string error;
    std::string measured_at_utc;
    int64_t measured_at_unix_ms = 0;  // same instant, for the age at record start (0: unknown)
    std::string source;  // e.g. full_resolution_native_stream_snapshot
    std::string native_source_storage;
    uint64_t camera_frame_id = 0;
    uint64_t camera_timestamp_ns = 0;
    uint64_t timestamp_sys_ns = 0;
    LevelSettings settings;
    Statistics statistics;  // whole frame
    DishRegion dish_region;
    std::optional<Statistics> dish_interior;  // set when dish_region.available
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
// The region a new expectation saved from this measurement checks: the dish
// interior when the measurement has it, else the whole frame.
LevelsRegion SaveRegion(const Measurement& measurement);
// The statistics an expectation on this region is judged on, or nullptr when
// the measurement has none for it (no dish region).
const Statistics* StatisticsFor(const Measurement& measurement, LevelsRegion region);
const char* RegionName(LevelsRegion region);

// Settings in the expectation's basis that differ from the live ones
// (names only; both values are in the block).
std::vector<std::string> BasisSettingsDiffer(const CameraExpectation& expectation, const LevelSettings& live);

nlohmann::json BuildRecordingBlock(bool enabled, bool rehome_iris, const nlohmann::json& cameras_by_serial);

// What started the measurement a camera's result holds (measurement_trigger).
enum class MeasurementTrigger { kNone, kStreamStart, kOperator, kAfterRecording };
const char* TriggerName(MeasurementTrigger trigger);  // nullptr for kNone
int64_t NowUnixMs();

// Per-serial results produced on the GUI thread and read by the recording
// start (which may run on another thread).
class ResultStore {
public:
    // A finished check (EvaluateCamera output) and what started it; measured_at_unix_ms
    // is the measurement's instant (0: none or unknown). Clears any refresh in progress.
    void SetCompleted(const std::string& serial, nlohmann::json result, MeasurementTrigger trigger,
                      int64_t measured_at_unix_ms);
    // Set(serial, result) = SetCompleted(serial, result, kNone, 0): not_streaming, disabled, tests.
    void Set(const std::string& serial, nlohmann::json result);
    // A measurement is under way. With a completed measurement already held, that one
    // stays the result and is sealed stale (refresh_in_progress); without one, this
    // in-progress result (not_measured, measurement_in_progress_<phase>) is the result.
    void SetInProgress(const std::string& serial, nlohmann::json in_progress_result);
    // A refresh ended without a result (a recording started first): the held result
    // stands, no longer marked refresh_in_progress.
    void EndRefreshWithoutResult(const std::string& serial);
    // A recording (or a start that failed) is over: every result held now was
    // measured before it (measured_before_previous_recording at the next start).
    void NoteRecordingEnded();
    void Clear();
    std::optional<nlohmann::json> Get(const std::string& serial) const;
    void SetCheckSettings(bool enabled, bool rehome_iris);
    // The recording block for these cameras; a camera with no result is
    // not_measured (no_check_result), and every measured camera lists the
    // settings that changed between its measurement and the record start, the
    // measurement's age at now_unix_ms and its freshness.
    nlohmann::json BuildRecordingBlockAt(const std::vector<std::pair<std::string, LevelSettings>>& cameras) const;
    nlohmann::json BuildRecordingBlockAt(const std::vector<std::pair<std::string, LevelSettings>>& cameras,
                                         int64_t now_unix_ms) const;

private:
    struct Entry {
        nlohmann::json result;
        MeasurementTrigger trigger = MeasurementTrigger::kNone;
        int64_t measured_at_unix_ms = 0;
        uint64_t generation = 0;
        bool refresh_in_progress = false;
    };
    mutable std::mutex mutex_;
    std::map<std::string, Entry> by_serial_;
    uint64_t generation_ = 0;
    bool enabled_ = false;
    bool rehome_iris_ = false;
};
ResultStore& GuiResultStore();

}  // namespace orange::image_levels
