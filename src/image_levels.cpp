#include "image_levels.h"

#include <cmath>
#include <stdexcept>

namespace orange::image_levels {

Statistics StatisticsFromHistogram(const uint64_t counts[256])
{
    Statistics s;
    uint64_t total = 0;
    long double sum = 0.0L;
    for (int v = 0; v < 256; ++v) {
        total += counts[v];
        sum += static_cast<long double>(counts[v]) * v;
    }
    s.pixel_count = total;
    if (total == 0) {
        return s;
    }
    const long double n = static_cast<long double>(total);
    s.mean = static_cast<double>(sum / n);
    s.zero_fraction = static_cast<double>(counts[0] / n);
    s.saturated_fraction = static_cast<double>(counts[kSaturationValue] / n);
    // Nearest rank on the histogram, in integers: rank = ceil(k * n / 1000),
    // pK = the smallest level whose cumulative count reaches the rank
    // (docs/schemas/orange_image_levels_v1.schema.json).
    struct Level {
        uint64_t per_mille;
        int* out;
        uint64_t rank = 0;
        bool done = false;
    } levels[] = {{10, &s.p01}, {50, &s.p05}, {500, &s.p50}, {950, &s.p95}, {990, &s.p99}, {999, &s.p999}};
    for (auto& level : levels) {
        const unsigned __int128 product = static_cast<unsigned __int128>(level.per_mille) * total;
        level.rank = static_cast<uint64_t>((product + 999) / 1000);
    }
    uint64_t cumulative = 0;
    for (int v = 0; v < 256; ++v) {
        cumulative += counts[v];
        for (auto& level : levels) {
            if (!level.done && cumulative >= level.rank) {
                *level.out = v;
                level.done = true;
            }
        }
    }
    return s;
}

Statistics ComputeStatistics(const uint8_t* pixels, size_t count)
{
    uint64_t counts[256] = {};
    if (pixels) {
        for (size_t i = 0; i < count; ++i) {
            ++counts[pixels[i]];
        }
    }
    return StatisticsFromHistogram(counts);
}

namespace {
// Half away from zero, as the schema states (std::round's rule).
double round_to(double value, int decimals)
{
    const double scale = std::pow(10.0, decimals);
    return std::round(value * scale) / scale;
}
}  // namespace

nlohmann::json StatisticsToJson(const Statistics& s)
{
    return {{"pixel_count", s.pixel_count},
            {"saturation_value", kSaturationValue},
            {"mean", round_to(s.mean, 3)},
            {"zero_fraction", round_to(s.zero_fraction, 8)},
            {"saturated_fraction", round_to(s.saturated_fraction, 8)},
            {"p01", s.p01},
            {"p05", s.p05},
            {"p50", s.p50},
            {"p95", s.p95},
            {"p99", s.p99},
            {"p999", s.p999}};
}

nlohmann::json SettingsToJson(const LevelSettings& s)
{
    return {{"exposure", s.exposure}, {"gain", s.gain},   {"iris", s.iris},         {"focus", s.focus},
            {"width", s.width},       {"height", s.height}, {"offset_x", s.offset_x}, {"offset_y", s.offset_y}};
}

namespace {

double require_number(const nlohmann::json& block, const char* key, double lo, double hi)
{
    const auto& v = block.at(key);
    if (!v.is_number() || !std::isfinite(v.get<double>()) || v.get<double>() < lo || v.get<double>() > hi) {
        throw std::runtime_error(std::string("image_levels.") + key + " must be a number in [" + std::to_string(lo) +
                                 ", " + std::to_string(hi) + "]");
    }
    return v.get<double>();
}

}  // namespace

CameraExpectation ParseCameraExpectation(const nlohmann::json& camera_config)
{
    CameraExpectation e;
    if (!camera_config.is_object() || !camera_config.contains("image_levels")) {
        return e;
    }
    const nlohmann::json& block = camera_config.at("image_levels");
    if (!block.is_object()) {
        throw std::runtime_error("image_levels must be an object");
    }
    if (block.value("schema_id", std::string()) != kCameraImageLevelsSchemaId ||
        !block.contains("schema_version") || !block.at("schema_version").is_number_integer() ||
        block.at("schema_version").get<int>() != kCameraImageLevelsSchemaVersion) {
        throw std::runtime_error(std::string("image_levels must declare schema_id ") + kCameraImageLevelsSchemaId +
                                 " and schema_version " + std::to_string(kCameraImageLevelsSchemaVersion));
    }
    static const char* kKnown[] = {"schema_id", "schema_version", "expected_mean", "mean_tolerance_fraction",
                                   "max_saturated_fraction", "basis"};
    for (auto it = block.begin(); it != block.end(); ++it) {
        bool known = false;
        for (const char* k : kKnown) known = known || it.key() == k;
        if (!known) {
            throw std::runtime_error("image_levels has an unknown field: " + it.key());
        }
    }
    e.configured = true;
    if (block.contains("expected_mean") && !block.at("expected_mean").is_null()) {
        e.expected_mean = require_number(block, "expected_mean", 1.0, 254.0);
        if (!block.contains("mean_tolerance_fraction")) {
            throw std::runtime_error("image_levels.expected_mean needs mean_tolerance_fraction");
        }
        e.mean_tolerance_fraction = require_number(block, "mean_tolerance_fraction", 0.01, 0.9);
    } else if (block.contains("mean_tolerance_fraction")) {
        throw std::runtime_error("image_levels.mean_tolerance_fraction without expected_mean");
    }
    if (block.contains("max_saturated_fraction") && !block.at("max_saturated_fraction").is_null()) {
        e.max_saturated_fraction = require_number(block, "max_saturated_fraction", 0.0, 1.0);
    }
    if (!e.expected_mean && !e.max_saturated_fraction) {
        throw std::runtime_error("image_levels needs expected_mean or max_saturated_fraction");
    }
    if (block.contains("basis")) {
        if (!block.at("basis").is_object()) {
            throw std::runtime_error("image_levels.basis must be an object");
        }
        e.basis_json = block.at("basis").dump();
    }
    return e;
}

nlohmann::json BuildCameraExpectation(const CameraExpectation& e)
{
    if (!e.configured) {
        return nlohmann::json::object();
    }
    nlohmann::json block = {{"schema_id", kCameraImageLevelsSchemaId},
                            {"schema_version", kCameraImageLevelsSchemaVersion}};
    if (e.expected_mean) {
        block["expected_mean"] = *e.expected_mean;
        block["mean_tolerance_fraction"] = e.mean_tolerance_fraction;
    }
    if (e.max_saturated_fraction) {
        block["max_saturated_fraction"] = *e.max_saturated_fraction;
    }
    if (!e.basis_json.empty()) {
        block["basis"] = nlohmann::json::parse(e.basis_json);
    }
    return block;
}

nlohmann::json MeasurementToJson(const Measurement& m)
{
    if (!m.ok) {
        return {{"ok", false}, {"error", m.error}, {"measured_at_utc", m.measured_at_utc}};
    }
    return {{"ok", true},
            {"measured_at_utc", m.measured_at_utc},
            {"source", m.source},
            {"native_source_storage", m.native_source_storage},
            {"camera_frame_id", m.camera_frame_id},
            {"camera_timestamp_ns", m.camera_timestamp_ns},
            {"timestamp_sys_ns", m.timestamp_sys_ns},
            {"settings", SettingsToJson(m.settings)},
            {"statistics", StatisticsToJson(m.statistics)}};
}

bool MeanInBand(const CameraExpectation& e, double mean)
{
    if (!e.expected_mean) {
        return true;
    }
    return std::fabs(mean - *e.expected_mean) <= e.mean_tolerance_fraction * *e.expected_mean;
}

std::vector<std::string> BasisSettingsDiffer(const CameraExpectation& e, const LevelSettings& live)
{
    std::vector<std::string> differ;
    if (e.basis_json.empty()) {
        return differ;
    }
    const nlohmann::json basis = nlohmann::json::parse(e.basis_json);
    if (!basis.contains("settings") || !basis.at("settings").is_object()) {
        return differ;
    }
    const nlohmann::json live_json = SettingsToJson(live);
    for (auto it = live_json.begin(); it != live_json.end(); ++it) {
        const auto& recorded = basis.at("settings");
        if (recorded.contains(it.key()) && recorded.at(it.key()).is_number() &&
            recorded.at(it.key()).get<double>() != it.value().get<double>()) {
            differ.push_back(it.key());
        }
    }
    return differ;
}

nlohmann::json EvaluateCamera(const CameraExpectation& e,
                              const LevelSettings& live,
                              const std::optional<Measurement>& measurement,
                              const std::optional<Measurement>& first_measurement,
                              bool rehomed,
                              const std::string& not_measured_reason)
{
    nlohmann::json out = {{"status", "not_measured"},
                          {"warnings", nlohmann::json::array()},
                          {"expectation", e.configured ? BuildCameraExpectation(e) : nlohmann::json(nullptr)},
                          {"expectation_basis_settings_differ", BasisSettingsDiffer(e, live)},
                          {"live_settings", SettingsToJson(live)},
                          {"measurement", nullptr},
                          {"first_measurement", nullptr},
                          {"rehomed_iris", rehomed},
                          {"not_measured_reason", nullptr}};
    if (first_measurement) {
        out["first_measurement"] = MeasurementToJson(*first_measurement);
    }
    if (!measurement) {
        out["not_measured_reason"] = not_measured_reason.empty() ? "not_requested" : not_measured_reason;
        return out;
    }
    out["measurement"] = MeasurementToJson(*measurement);
    if (!measurement->ok) {
        out["status"] = "measurement_failed";
        return out;
    }
    if (!e.configured) {
        out["status"] = "no_expectation";
        return out;
    }
    const Statistics& s = measurement->statistics;
    if (!MeanInBand(e, s.mean)) {
        out["warnings"].push_back("mean_out_of_band");
    }
    if (e.max_saturated_fraction && s.saturated_fraction > *e.max_saturated_fraction) {
        out["warnings"].push_back("saturated_fraction_above_limit");
    }
    out["status"] = out["warnings"].empty() ? "ok" : "warning";
    return out;
}

nlohmann::json BuildRecordingBlock(bool enabled, bool rehome_iris, const nlohmann::json& cameras_by_serial)
{
    return {{"schema_id", kRecordingImageLevelsCheckSchemaId},
            {"schema_version", kRecordingImageLevelsCheckSchemaVersion},
            {"policy", kPolicy},
            {"enabled", enabled},
            {"rehome_iris", rehome_iris},
            {"source", "one full-resolution native frame per camera after stream start, before recording"},
            {"cameras", cameras_by_serial}};
}

void ResultStore::Set(const std::string& serial, nlohmann::json result)
{
    std::lock_guard<std::mutex> lock(mutex_);
    by_serial_[serial] = std::move(result);
}

void ResultStore::Clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    by_serial_ = nlohmann::json::object();
}

std::optional<nlohmann::json> ResultStore::Get(const std::string& serial) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!by_serial_.contains(serial)) {
        return std::nullopt;
    }
    return by_serial_.at(serial);
}

void ResultStore::SetCheckSettings(bool enabled, bool rehome_iris)
{
    std::lock_guard<std::mutex> lock(mutex_);
    enabled_ = enabled;
    rehome_iris_ = rehome_iris;
}

nlohmann::json ResultStore::BuildRecordingBlockAt(
    const std::vector<std::pair<std::string, LevelSettings>>& cameras) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json by_serial = nlohmann::json::object();
    for (const auto& [serial, live] : cameras) {
        nlohmann::json result;
        if (by_serial_.contains(serial)) {
            result = by_serial_.at(serial);
        } else {
            result = EvaluateCamera(CameraExpectation{}, live, std::nullopt, std::nullopt, false, "no_check_result");
        }
        nlohmann::json changed = nlohmann::json::array();
        const auto& m = result.at("measurement");
        if (m.is_object() && m.value("ok", false)) {
            const nlohmann::json at_start = SettingsToJson(live);
            for (auto it = at_start.begin(); it != at_start.end(); ++it) {
                if (m.at("settings").at(it.key()) != it.value()) changed.push_back(it.key());
            }
        }
        result["live_settings"] = SettingsToJson(live);
        result["settings_changed_since_measurement"] = changed;
        by_serial[serial] = result;
    }
    return BuildRecordingBlock(enabled_, rehome_iris_, by_serial);
}

ResultStore& GuiResultStore()
{
    static ResultStore store;
    return store;
}

}  // namespace orange::image_levels
