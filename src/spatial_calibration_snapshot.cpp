#include "spatial_calibration_snapshot.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace orange::spatial {
namespace {

constexpr const char* kMeasurementFilename = "measurement.json";
constexpr const char* kArenaLayoutRuntimeFilename = "arena_layout_runtime.json";
constexpr const char* kDishMaskRuntimeFilename = "dish_mask_runtime.json";
constexpr const char* kFingerprintAlgorithm = "fnv1a64";
constexpr uint64_t kFnv1a64Offset = 14695981039346656037ULL;
constexpr uint64_t kFnv1a64Prime = 1099511628211ULL;

bool set_error(std::string* error_out, const std::string& message)
{
    if (error_out) {
        *error_out = message;
    }
    return false;
}

bool read_json_file(const std::filesystem::path& path,
                    nlohmann::json* value_out,
                    std::string* error_out)
{
    if (!value_out) {
        return set_error(error_out, "null JSON destination");
    }
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in.is_open()) {
        return set_error(error_out, "failed to open JSON file: " + path.string());
    }
    try {
        in >> *value_out;
    } catch (const std::exception& ex) {
        return set_error(error_out, "failed to parse JSON file " + path.string() + ": " + ex.what());
    }
    return true;
}

void fnv1a64_update(uint64_t* hash, const char* data, size_t size)
{
    if (!hash || !data) {
        return;
    }
    for (size_t idx = 0; idx < size; ++idx) {
        *hash ^= static_cast<uint64_t>(static_cast<unsigned char>(data[idx]));
        *hash *= kFnv1a64Prime;
    }
}

std::string compute_json_fingerprint(const nlohmann::json& value)
{
    nlohmann::json fingerprint_payload = value;
    if (fingerprint_payload.contains("calibration_ref") &&
        fingerprint_payload["calibration_ref"].is_object()) {
        fingerprint_payload["calibration_ref"]["fingerprint"] = "";
    }

    const std::string payload = fingerprint_payload.dump();
    uint64_t hash = kFnv1a64Offset;
    fnv1a64_update(&hash, payload.data(), payload.size());

    std::ostringstream oss;
    oss << kFingerprintAlgorithm << ':' << std::hex << std::nouppercase << hash;
    return oss.str();
}

} // namespace

bool load_camera_spatial_calibration_from_artifact_dir(
    const std::string& artifact_dir,
    CameraSpatialCalibration* calibration_out,
    std::string* error_out)
{
    if (error_out) {
        error_out->clear();
    }
    if (!calibration_out) {
        return set_error(error_out, "null CameraSpatialCalibration destination");
    }
    if (artifact_dir.empty()) {
        return set_error(error_out, "spatial calibration artifact directory is empty");
    }

    const std::filesystem::path root(artifact_dir);
    std::error_code exists_error;
    if (!std::filesystem::is_directory(root, exists_error)) {
        return set_error(error_out, "spatial calibration artifact directory does not exist: " + root.string());
    }

    nlohmann::json measurement_json;
    if (!read_json_file(root / kMeasurementFilename, &measurement_json, error_out)) {
        return false;
    }
    ArenaLayoutArtifact arena_artifact;
    if (!parse_arena_layout_artifact_json(measurement_json, &arena_artifact, error_out)) {
        return false;
    }

    nlohmann::json arena_runtime_json;
    if (!read_json_file(root / kArenaLayoutRuntimeFilename, &arena_runtime_json, error_out)) {
        return false;
    }
    ArenaLayoutRuntime arena_runtime;
    if (!parse_arena_layout_runtime_json(arena_runtime_json, &arena_runtime, error_out) ||
        !validate_arena_layout_runtime_against_artifact(arena_runtime, arena_artifact, error_out)) {
        return false;
    }

    nlohmann::json dish_runtime_json;
    if (!read_json_file(root / kDishMaskRuntimeFilename, &dish_runtime_json, error_out)) {
        return false;
    }
    DishMaskRuntime dish_runtime;
    if (!parse_dish_mask_runtime_json(dish_runtime_json, &dish_runtime, error_out)) {
        return false;
    }

    CameraSpatialCalibration calibration;
    calibration.has_dish_mask = true;
    calibration.dish_mask.calibration_ref = CalibrationRef{
        arena_artifact.artifact_id + ".dish_mask_runtime",
        kDishMaskArtifactSchemaId,
        kDishMaskArtifactSchemaVersion,
        compute_json_fingerprint(dish_runtime_json)
    };
    calibration.dish_mask.runtime = std::move(dish_runtime);

    calibration.has_arena_layout = true;
    calibration.arena_layout.calibration_ref = arena_artifact.calibration_ref;
    calibration.arena_layout.runtime = std::move(arena_runtime);

    if (!validate_camera_spatial_calibration(calibration, error_out)) {
        return false;
    }

    *calibration_out = std::move(calibration);
    return true;
}

bool load_camera_spatial_calibration_json_from_artifact_dir(
    const std::string& artifact_dir,
    nlohmann::json* calibration_json_out,
    std::string* error_out)
{
    if (!calibration_json_out) {
        return set_error(error_out, "null JSON destination");
    }
    CameraSpatialCalibration calibration;
    if (!load_camera_spatial_calibration_from_artifact_dir(artifact_dir, &calibration, error_out)) {
        return false;
    }
    *calibration_json_out = camera_spatial_calibration_to_json(calibration);
    return true;
}

bool apply_camera_spatial_calibration_to_snapshot_json(
    nlohmann::json* snapshot,
    const std::string& camera_serial,
    const CameraSpatialCalibration& calibration,
    std::string* error_out)
{
    if (error_out) {
        error_out->clear();
    }
    if (!snapshot) {
        return set_error(error_out, "null recording snapshot destination");
    }
    if (camera_serial.empty()) {
        return set_error(error_out, "camera serial is empty");
    }
    if (!validate_camera_spatial_calibration(calibration, error_out)) {
        return false;
    }

    if (!snapshot->is_object()) {
        *snapshot = nlohmann::json::object();
    }
    if (!snapshot->contains("calibrations") || !(*snapshot)["calibrations"].is_object()) {
        (*snapshot)["calibrations"] = nlohmann::json::object();
    }
    (*snapshot)["calibrations"][camera_serial] = camera_spatial_calibration_to_json(calibration);
    return true;
}

} // namespace orange::spatial
