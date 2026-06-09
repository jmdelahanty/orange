#include "gui/spatial_layout/session_io.h"

#include "dish_top_rim_observation.h"
#include "fsuid_guard.h"
#include "project.h"

#include <opencv2/opencv.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace orange::gui::spatial_layout {
namespace {

inline constexpr const char* kCalibrationSessionSchemaId = "orange.calibration.session";
inline constexpr int kCalibrationSessionSchemaVersion = 1;
inline constexpr const char* kCalibrationSessionIndexSchemaId = "orange.calibration.session_index";
inline constexpr int kCalibrationSessionIndexSchemaVersion = 1;
inline constexpr uint64_t kFnv1a64Offset = 14695981039346656037ULL;
inline constexpr uint64_t kFnv1a64Prime = 1099511628211ULL;

void fnv1a64_update_bytes(uint64_t* hash, const void* data, size_t size)
{
    if (hash == nullptr || data == nullptr) {
        return;
    }
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) {
        *hash ^= static_cast<uint64_t>(bytes[i]);
        *hash *= kFnv1a64Prime;
    }
}

} // namespace

std::string sanitize_artifact_component(const std::string& value)
{
    std::string sanitized;
    sanitized.reserve(value.size());
    for (char ch : value) {
        if ((ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9')) {
            sanitized.push_back(ch);
        } else {
            sanitized.push_back('_');
        }
    }
    while (!sanitized.empty() && sanitized.front() == '_') {
        sanitized.erase(sanitized.begin());
    }
    while (!sanitized.empty() && sanitized.back() == '_') {
        sanitized.pop_back();
    }
    if (sanitized.empty()) {
        sanitized = "spatial_layout";
    }
    return sanitized;
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
    fnv1a64_update_bytes(&hash, payload.data(), payload.size());

    std::ostringstream oss;
    oss << kCalibrationFingerprintAlgorithm << ':'
        << std::hex << std::nouppercase << hash;
    return oss.str();
}

std::string compute_file_fingerprint(const std::filesystem::path& path, std::string* error_out)
{
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in.is_open()) {
        if (error_out) {
            *error_out = "Failed to open file for fingerprinting: " + path.string();
        }
        return "";
    }

    uint64_t hash = kFnv1a64Offset;
    std::array<char, 64 * 1024> buffer{};
    while (in.good()) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = in.gcount();
        if (count > 0) {
            fnv1a64_update_bytes(&hash, buffer.data(), static_cast<size_t>(count));
        }
    }
    if (in.bad()) {
        if (error_out) {
            *error_out = "Failed while reading file for fingerprinting: " + path.string();
        }
        return "";
    }

    std::ostringstream oss;
    oss << kCalibrationFingerprintAlgorithm << ':'
        << std::hex << std::nouppercase << hash;
    return oss.str();
}

bool write_image_file(const std::filesystem::path& path, const cv::Mat& image, std::string* error_out)
{
    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    std::filesystem::create_directories(path.parent_path());
    if (image.empty()) {
        if (error_out) {
            *error_out = "Cannot write empty image: " + path.string();
        }
        return false;
    }
    try {
        if (!cv::imwrite(path.string(), image)) {
            if (error_out) {
                *error_out = "cv::imwrite failed: " + path.string();
            }
            return false;
        }
    } catch (const std::exception& ex) {
        if (error_out) {
            *error_out = std::string("cv::imwrite exception for ") + path.string() + ": " + ex.what();
        }
        return false;
    }
    return true;
}

bool write_json_file(const std::filesystem::path& path,
                     const nlohmann::json& value,
                     std::string* error_out)
{
    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    std::ofstream out(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        if (error_out) {
            *error_out = "Failed to open JSON output path: " + path.string();
        }
        return false;
    }
    out << value.dump(2) << '\n';
    if (!out.good()) {
        if (error_out) {
            *error_out = "Failed to write JSON output path: " + path.string();
        }
        return false;
    }
    return true;
}

bool read_json_file(const std::filesystem::path& path,
                    nlohmann::json* value,
                    std::string* error_out)
{
    if (value == nullptr) {
        if (error_out) {
            *error_out = "Null JSON destination.";
        }
        return false;
    }
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in.is_open()) {
        if (error_out) {
            *error_out = "Failed to open JSON input path: " + path.string();
        }
        return false;
    }

    try {
        in >> *value;
    } catch (const std::exception& ex) {
        if (error_out) {
            *error_out = std::string("Failed to parse JSON from ") + path.string() + ": " + ex.what();
        }
        return false;
    }
    return true;
}

std::filesystem::path calibration_base_dir_from_artifact_root(const std::string& artifact_root_dir)
{
    const std::filesystem::path root(artifact_root_dir.empty() ? "." : artifact_root_dir);
    if (root.filename() == "artifacts" && !root.parent_path().empty()) {
        return root.parent_path();
    }
    return root;
}

std::filesystem::path calibration_sessions_dir_from_artifact_root(const std::string& artifact_root_dir)
{
    return calibration_base_dir_from_artifact_root(artifact_root_dir) / "sessions";
}

std::string build_spatial_calibration_session_id(
    const SpatialLayoutUiState* ui_state,
    const std::string& timestamp)
{
    std::ostringstream oss;
    oss << "calsess_" << sanitize_artifact_component(timestamp);
    if (ui_state != nullptr && ui_state->citrus_template.available) {
        if (!ui_state->citrus_template.source_canvas_name.empty()) {
            oss << "_" << sanitize_artifact_component(ui_state->citrus_template.source_canvas_name);
        }
    }
    return oss.str();
}

bool ensure_directory_for_spatial_session(const std::filesystem::path& path, std::string* error_out)
{
    try {
        orange::ScopedFsuid fsuid_guard;
        (void)fsuid_guard;
        std::filesystem::create_directories(path);
    } catch (const std::exception& ex) {
        if (error_out) {
            *error_out = "Failed to create calibration session directory " +
                         path.string() + ": " + ex.what();
        }
        return false;
    }
    return true;
}

bool write_spatial_calibration_session_manifest(
    const SpatialLayoutUiState* ui_state,
    const CameraParams& selected_camera,
    const std::string& artifact_root_dir,
    const std::filesystem::path& session_dir,
    const std::filesystem::path& session_artifact_root,
    std::string* error_out)
{
    if (ui_state == nullptr) {
        if (error_out) {
            *error_out = "Cannot write calibration session manifest with null UI state.";
        }
        return false;
    }
    nlohmann::json context = {
        {"initial_camera_serial", selected_camera.camera_serial},
        {"initial_camera_name", selected_camera.camera_name}
    };
    if (ui_state->citrus_template.available) {
        nlohmann::json citrus = nlohmann::json::object();
        if (!ui_state->citrus_template.source_rig_name.empty()) {
            citrus["rig_id"] = ui_state->citrus_template.source_rig_name;
        }
        if (!ui_state->citrus_template.source_canvas_name.empty()) {
            citrus["canvas_id"] = ui_state->citrus_template.source_canvas_name;
        }
        if (!ui_state->citrus_template.source_arena_name.empty()) {
            citrus["arena_id"] = ui_state->citrus_template.source_arena_name;
        }
        if (!ui_state->citrus_template.source_camera_id.empty()) {
            citrus["camera_id"] = ui_state->citrus_template.source_camera_id;
        }
        if (!citrus.empty()) {
            context["citrus"] = citrus;
        }
    }

    const nlohmann::json session = {
        {"schema_id", kCalibrationSessionSchemaId},
        {"schema_version", kCalibrationSessionSchemaVersion},
        {"session_id", ui_state->calibration_session_id},
        {"created_utc", ui_state->calibration_session_created_utc},
        {"producer", "orange_spatial_layout_ui"},
        {"artifact_root_legacy", artifact_root_dir},
        {"session_dir", session_dir.generic_string()},
        {"artifacts_dir", session_artifact_root.generic_string()},
        {"context", context},
        {"files", {
            {"session_index", kCalibrationSessionIndexFilename},
            {"artifacts_dir", "artifacts"}
        }}
    };
    return write_json_file(session_dir / kCalibrationSessionFilename, session, error_out);
}

bool ensure_spatial_calibration_session(
    SpatialLayoutUiState* ui_state,
    const CameraParams& selected_camera,
    const std::string& artifact_root_dir,
    std::string* session_artifact_root_out,
    std::string* error_out)
{
    if (ui_state == nullptr) {
        if (error_out) {
            *error_out = "Spatial layout UI state is null.";
        }
        return false;
    }
    if (artifact_root_dir.empty()) {
        if (error_out) {
            *error_out = "Calibration artifact root directory is empty.";
        }
        return false;
    }

    const std::filesystem::path sessions_dir =
        calibration_sessions_dir_from_artifact_root(artifact_root_dir);
    if (ui_state->calibration_session_id.empty()) {
        ui_state->calibration_session_created_utc = get_current_utc_timestamp();
        ui_state->calibration_session_id =
            build_spatial_calibration_session_id(
                ui_state,
                ui_state->calibration_session_created_utc);
        ui_state->calibration_session_dir =
            (sessions_dir / ui_state->calibration_session_id).generic_string();
    } else if (ui_state->calibration_session_dir.empty()) {
        ui_state->calibration_session_dir =
            (sessions_dir / ui_state->calibration_session_id).generic_string();
    }
    if (ui_state->calibration_session_created_utc.empty()) {
        ui_state->calibration_session_created_utc = get_current_utc_timestamp();
    }

    const std::filesystem::path session_dir(ui_state->calibration_session_dir);
    const std::filesystem::path session_artifact_root = session_dir / "artifacts";
    if (!ensure_directory_for_spatial_session(session_artifact_root, error_out)) {
        return false;
    }
    if (!write_spatial_calibration_session_manifest(
            ui_state,
            selected_camera,
            artifact_root_dir,
            session_dir,
            session_artifact_root,
            error_out)) {
        return false;
    }
    if (session_artifact_root_out) {
        *session_artifact_root_out = session_artifact_root.generic_string();
    }
    return true;
}

void clear_spatial_calibration_session(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr) {
        return;
    }
    ui_state->calibration_session_id.clear();
    ui_state->calibration_session_dir.clear();
    ui_state->calibration_session_created_utc.clear();
}

bool update_spatial_calibration_session_index(
    const std::string& session_dir_string,
    const std::string& session_artifact_root_string,
    const nlohmann::json& manifest,
    std::string* error_out)
{
    if (session_dir_string.empty()) {
        return true;
    }
    if (!manifest.is_object()) {
        if (error_out) {
            *error_out = "Cannot update calibration session index with non-object manifest.";
        }
        return false;
    }
    const std::filesystem::path session_dir(session_dir_string);
    const std::filesystem::path session_artifact_root(session_artifact_root_string);
    if (!ensure_directory_for_spatial_session(session_dir, error_out)) {
        return false;
    }

    const std::string artifact_id = manifest.value("artifact_id", "");
    if (artifact_id.empty()) {
        if (error_out) {
            *error_out = "Cannot update calibration session index: artifact manifest has no artifact_id.";
        }
        return false;
    }
    const std::filesystem::path index_path = session_dir / kCalibrationSessionIndexFilename;
    nlohmann::json index = nlohmann::json::object();
    if (std::filesystem::exists(index_path) &&
        !read_json_file(index_path, &index, error_out)) {
        return false;
    }
    if (!index.is_object()) {
        index = nlohmann::json::object();
    }
    if (!index.contains("artifact_order") || !index["artifact_order"].is_array()) {
        index["artifact_order"] = nlohmann::json::array();
    }
    if (!index.contains("artifacts_by_id") || !index["artifacts_by_id"].is_object()) {
        index["artifacts_by_id"] = nlohmann::json::object();
    }

    const std::string session_id = session_dir.filename().generic_string();
    index["schema_id"] = kCalibrationSessionIndexSchemaId;
    index["schema_version"] = kCalibrationSessionIndexSchemaVersion;
    index["session_id"] = session_id;
    index["session_dir"] = session_dir.generic_string();
    index["artifacts_dir"] = session_artifact_root.generic_string();
    index["updated_utc"] =
        manifest.value("updated_utc", manifest.value("created_utc", get_current_utc_timestamp()));

    bool already_ordered = false;
    for (const auto& value : index["artifact_order"]) {
        if (value.is_string() && value.get<std::string>() == artifact_id) {
            already_ordered = true;
            break;
        }
    }
    if (!already_ordered) {
        index["artifact_order"].push_back(artifact_id);
    }

    std::error_code rel_error;
    const std::filesystem::path manifest_path =
        session_artifact_root / artifact_id / kSpatialLayoutManifestFilename;
    std::filesystem::path relative_manifest =
        std::filesystem::relative(manifest_path, session_dir, rel_error);
    if (rel_error || relative_manifest.empty()) {
        relative_manifest = std::filesystem::path("artifacts") / artifact_id / kSpatialLayoutManifestFilename;
    }

    nlohmann::json entry = {
        {"artifact_id", artifact_id},
        {"artifact_schema_id", manifest.value("artifact_schema_id", "")},
        {"artifact_schema_version", manifest.value("artifact_schema_version", 0)},
        {"created_utc", manifest.value("created_utc", "")},
        {"relative_manifest_path", relative_manifest.generic_string()},
        {"fingerprint", manifest.value("calibration_ref", nlohmann::json::object()).value("fingerprint", "")}
    };
    if (manifest.contains("summary")) {
        entry["summary"] = manifest["summary"];
    }
    if (manifest.contains("producer")) {
        entry["producer"] = manifest["producer"];
    }
    index["artifacts_by_id"][artifact_id] = entry;
    const std::string artifact_schema_id = manifest.value("artifact_schema_id", "");
    if (artifact_schema_id == orange::calibration::kDishTopRimObservationSchemaId) {
        const nlohmann::json summary =
            manifest.value("summary", nlohmann::json::object());
        const nlohmann::json compatibility =
            manifest.value("compatibility", nlohmann::json::object());
        const std::string camera_serial =
            summary.value(
                "camera_serial",
                compatibility.value("camera_serial", std::string()));
        const std::string associated_image_set_artifact_id =
            summary.value("associated_image_set_artifact_id", std::string());
        if (!camera_serial.empty()) {
            if (!index.contains("latest_top_rim_observation_by_camera_serial") ||
                !index["latest_top_rim_observation_by_camera_serial"].is_object()) {
                index["latest_top_rim_observation_by_camera_serial"] =
                    nlohmann::json::object();
            }
            index["latest_top_rim_observation_by_camera_serial"][camera_serial] =
                artifact_id;
        }
        if (!associated_image_set_artifact_id.empty()) {
            if (!index.contains("latest_top_rim_observation_by_arena_artifact_id") ||
                !index["latest_top_rim_observation_by_arena_artifact_id"].is_object()) {
                index["latest_top_rim_observation_by_arena_artifact_id"] =
                    nlohmann::json::object();
            }
            index["latest_top_rim_observation_by_arena_artifact_id"]
                 [associated_image_set_artifact_id] = artifact_id;
        }
    }
    index["artifact_count"] = index["artifacts_by_id"].size();
    return write_json_file(index_path, index, error_out);
}

std::string build_arena_layout_artifact_id(
    const std::string& prefix_base,
    const CameraParams& camera_params,
    const std::string& timestamp_label)
{
    std::ostringstream oss;
    oss << "arenalayout_" << sanitize_artifact_component(prefix_base)
        << "_" << sanitize_artifact_component(timestamp_label)
        << "_Cam" << camera_params.camera_serial;
    return oss.str();
}

std::string build_camera_arena_calibration_image_set_artifact_id(
    const SpatialLayoutUiState* ui_state,
    const CameraParams& camera_params)
{
    std::ostringstream oss;
    oss << "Cam" << sanitize_artifact_component(camera_params.camera_serial);
    std::string arena = "arena_unknown";
    if (ui_state != nullptr &&
        ui_state->citrus_template.available &&
        !ui_state->citrus_template.source_arena_name.empty()) {
        arena = ui_state->citrus_template.source_arena_name;
    }
    oss << "_" << sanitize_artifact_component(arena);
    return oss.str();
}

std::string build_calibration_capture_filename(
    const std::string& purpose,
    const std::string& timestamp_label)
{
    std::ostringstream oss;
    oss << sanitize_artifact_component(purpose.empty() ? std::string("capture") : purpose)
        << "_" << sanitize_artifact_component(timestamp_label)
        << ".png";
    return oss.str();
}

SpatialLayoutPersistedFiles make_spatial_layout_persisted_files(
    const std::string& artifact_root_dir,
    const std::string& artifact_id)
{
    SpatialLayoutPersistedFiles files;
    files.artifact_dir = std::filesystem::path(artifact_root_dir) / artifact_id;
    files.measurement_path = files.artifact_dir / kSpatialLayoutMeasurementFilename;
    files.manifest_path = files.artifact_dir / kSpatialLayoutManifestFilename;
    files.arena_layout_runtime_path = files.artifact_dir / kSpatialLayoutArenaLayoutRuntimeFilename;
    files.dish_mask_runtime_path = files.artifact_dir / kSpatialLayoutDishMaskRuntimeFilename;
    return files;
}

GenericCalibrationImageSetFiles make_generic_calibration_image_set_files(
    const std::string& artifact_root_dir,
    const std::string& artifact_id,
    const std::string& source_frame_filename)
{
    GenericCalibrationImageSetFiles files;
    files.artifact_dir = std::filesystem::path(artifact_root_dir) / artifact_id;
    files.image_set_path = files.artifact_dir / "image_set.json";
    files.manifest_path = files.artifact_dir / "manifest.json";
    const std::filesystem::path capture_filename =
        source_frame_filename.empty()
            ? std::filesystem::path("source_frame.png")
            : std::filesystem::path(source_frame_filename).filename();
    files.source_frame_relative_path = std::filesystem::path("captures") / capture_filename;
    files.source_frame_path = files.artifact_dir / files.source_frame_relative_path;
    return files;
}

} // namespace orange::gui::spatial_layout
