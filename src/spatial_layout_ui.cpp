#include "spatial_layout_ui.h"

#include "camera_preview_utils.h"
#include "dish_top_rim_observation.h"
#include "fsuid_guard.h"
#include "imgui.h"
#include "implot.h"
#include "misc/cpp/imgui_stdlib.h"
#include <ImGuiFileDialog.h>
#include <opencv2/opencv.hpp>
#include "project.h"
#include "spatial_snapshot_worker.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <condition_variable>
#include <cmath>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <vector>

namespace {

using orange::spatial::ArenaLayoutArtifact;
using orange::spatial::ArenaLayoutProvenanceSource;
using orange::spatial::ArenaLayoutRuntime;
using orange::spatial::ArenaLayoutZone;
using orange::spatial::CalibrationRef;
using orange::spatial::CircleGeometry;
using orange::spatial::CoordinateSpace;
using orange::spatial::DishMaskGeometry;
using orange::spatial::DishMaskRuntime;
using orange::spatial::LayoutGeometry;
using orange::spatial::LayoutGeometryType;
using orange::spatial::ObservationSource;
using orange::spatial::OrientationStatus;
using orange::spatial::RegistrationSource;
using orange::spatial::RegistrationType;
using orange::spatial::ResolvedZoneOverlay;
using orange::spatial::RuntimeGeometry;
using orange::spatial::RuntimeGeometryType;
using orange::spatial::ViewRegistration;
using orange::spatial::VisibilityStatus;

constexpr int kSpatialCaptureBufferCount = 2;
constexpr double kPi = 3.14159265358979323846;
constexpr const char* kCalibrationManifestSchemaId = "orange.calibration.manifest";
constexpr int kCalibrationManifestSchemaVersion = 1;
constexpr const char* kCalibrationFingerprintAlgorithm = "fnv1a64";
constexpr uint64_t kFnv1a64Offset = 14695981039346656037ULL;
constexpr uint64_t kFnv1a64Prime = 1099511628211ULL;
constexpr const char* kSpatialLayoutMeasurementFilename = "measurement.json";
constexpr const char* kSpatialLayoutManifestFilename = "manifest.json";
constexpr const char* kSpatialLayoutArenaLayoutRuntimeFilename = "arena_layout_runtime.json";
constexpr const char* kSpatialLayoutDishMaskRuntimeFilename = "dish_mask_runtime.json";
constexpr const char* kCalibrationSessionSchemaId = "orange.calibration.session";
constexpr int kCalibrationSessionSchemaVersion = 1;
constexpr const char* kCalibrationSessionIndexSchemaId = "orange.calibration.session_index";
constexpr int kCalibrationSessionIndexSchemaVersion = 1;
constexpr const char* kCalibrationSessionFilename = "session.json";
constexpr const char* kCalibrationSessionIndexFilename = "session_index.json";
constexpr const char* kLoadSpatialLayoutDialogId = "LoadSpatialLayoutArtifact";
constexpr const char* kLoadCitrusArenaConfigDialogId = "LoadCitrusArenaConfig";
constexpr int kProjectedCircleSampleCount = 96;
constexpr const char* kExperimentalAreaZoneId = "experimental_area";
constexpr const char* kExperimentalAreaZoneLabel = "Experimental Area";
constexpr const char* kHoyaR72FilterInstalled =
    "installed: HOYA Creative Filter Infrared R72 67 mm (Kenko Tokina)";
constexpr const char* kHoyaR72FilterRemoved =
    "removed: HOYA Creative Filter Infrared R72 67 mm (Kenko Tokina)";
constexpr const char* kCalibrationCaptureProfileId =
    "spatial_layout_visible_long_exposure_v1";
constexpr unsigned int kCalibrationCaptureFrameRateHz = 10;
constexpr unsigned int kCalibrationCaptureExposureUs = 10000;

struct Point2d {
    double x = 0.0;
    double y = 0.0;
};

struct SpatialLayoutPersistedFiles {
    std::filesystem::path artifact_dir;
    std::filesystem::path measurement_path;
    std::filesystem::path manifest_path;
    std::filesystem::path arena_layout_runtime_path;
    std::filesystem::path dish_mask_runtime_path;
};

struct GenericCalibrationImageSetFiles {
    std::filesystem::path artifact_dir;
    std::filesystem::path image_set_path;
    std::filesystem::path manifest_path;
    std::filesystem::path source_frame_path;
    std::filesystem::path source_frame_relative_path;
};

Point2d transform_point(const std::array<double, 9>& matrix, double x, double y);
void set_registration_transform(SpatialLayoutUiState* ui_state,
                                RegistrationType type,
                                const Point2d& desired_outer_center,
                                double scale,
                                double rotation_deg_clockwise,
                                RegistrationSource source);

std::string default_citrus_rigs_root()
{
    const std::filesystem::path citrus_rigs_root("/home/jeremy/citrus/targets/rigs");
    if (std::filesystem::exists(citrus_rigs_root)) {
        return citrus_rigs_root.string();
    }
    return ".";
}

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

bool render_string_preset_combo(
    const char* label,
    std::string* value,
    const char* const* presets,
    int preset_count)
{
    if (value == nullptr || presets == nullptr || preset_count <= 0) {
        return false;
    }

    const char* preview = value->empty() ? "unknown" : value->c_str();
    bool changed = false;
    if (ImGui::BeginCombo(label, preview)) {
        for (int idx = 0; idx < preset_count; ++idx) {
            const char* preset = presets[idx] ? presets[idx] : "";
            const bool selected = *value == preset;
            if (ImGui::Selectable(preset, selected)) {
                *value = preset;
                changed = true;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

void apply_illumination_preset(SpatialLayoutUiState* ui_state, const std::string& preset)
{
    if (ui_state == nullptr) {
        return;
    }
    if (preset == "custom_ttl_nir_strobe_855nm") {
        ui_state->calibration_light_state = "ttl_nir_strobe_active";
        ui_state->calibration_illumination_spectrum = "narrowband_nir";
        ui_state->calibration_illumination_source = "custom_ttl_nir_strobe";
        ui_state->calibration_illumination_center_wavelength_nm = 855.0;
        ui_state->calibration_has_illumination_center_wavelength_nm = true;
        ui_state->calibration_has_illumination_min_wavelength_nm = false;
        ui_state->calibration_has_illumination_max_wavelength_nm = false;
        ui_state->calibration_has_illumination_bandwidth_fwhm_nm = false;
        ui_state->calibration_illumination_wavelength_confidence = "nominal";
    } else if (preset == "visible_projector_broadband") {
        ui_state->calibration_light_state = "visible_projector_only";
        ui_state->calibration_illumination_spectrum = "broadband_visible";
        ui_state->calibration_illumination_source = "visible_projector";
        ui_state->calibration_has_illumination_center_wavelength_nm = false;
        ui_state->calibration_illumination_min_wavelength_nm = 400.0;
        ui_state->calibration_has_illumination_min_wavelength_nm = true;
        ui_state->calibration_illumination_max_wavelength_nm = 700.0;
        ui_state->calibration_has_illumination_max_wavelength_nm = true;
        ui_state->calibration_has_illumination_bandwidth_fwhm_nm = false;
        ui_state->calibration_illumination_wavelength_confidence = "approximate_range";
    } else if (preset == "ambient_room_light_visible") {
        ui_state->calibration_light_state = "ambient_room_light";
        ui_state->calibration_illumination_spectrum = "broadband_visible";
        ui_state->calibration_illumination_source = "ambient_room_light";
        ui_state->calibration_has_illumination_center_wavelength_nm = false;
        ui_state->calibration_illumination_min_wavelength_nm = 400.0;
        ui_state->calibration_has_illumination_min_wavelength_nm = true;
        ui_state->calibration_illumination_max_wavelength_nm = 700.0;
        ui_state->calibration_has_illumination_max_wavelength_nm = true;
        ui_state->calibration_has_illumination_bandwidth_fwhm_nm = false;
        ui_state->calibration_illumination_wavelength_confidence = "approximate_range";
    } else if (preset == "external_continuous_visible_light") {
        ui_state->calibration_light_state = "external_continuous_visible_light";
        ui_state->calibration_illumination_spectrum = "broadband_visible";
        ui_state->calibration_illumination_source = "external_continuous_visible_light";
        ui_state->calibration_has_illumination_center_wavelength_nm = false;
        ui_state->calibration_illumination_min_wavelength_nm = 400.0;
        ui_state->calibration_has_illumination_min_wavelength_nm = true;
        ui_state->calibration_illumination_max_wavelength_nm = 700.0;
        ui_state->calibration_has_illumination_max_wavelength_nm = true;
        ui_state->calibration_has_illumination_bandwidth_fwhm_nm = false;
        ui_state->calibration_illumination_wavelength_confidence = "approximate_range";
    } else if (preset == "external_continuous_ir_nir_light") {
        ui_state->calibration_light_state = "external_continuous_ir_nir_light";
        ui_state->calibration_illumination_spectrum = "unknown_ir_nir";
        ui_state->calibration_illumination_source = "external_continuous_ir_nir_light";
        ui_state->calibration_has_illumination_center_wavelength_nm = false;
        ui_state->calibration_has_illumination_min_wavelength_nm = false;
        ui_state->calibration_has_illumination_max_wavelength_nm = false;
        ui_state->calibration_has_illumination_bandwidth_fwhm_nm = false;
        ui_state->calibration_illumination_wavelength_confidence = "unknown";
    } else if (preset == "lights_off") {
        ui_state->calibration_light_state = "lights_off";
        ui_state->calibration_illumination_spectrum = "none";
        ui_state->calibration_illumination_source = "none";
        ui_state->calibration_has_illumination_center_wavelength_nm = false;
        ui_state->calibration_has_illumination_min_wavelength_nm = false;
        ui_state->calibration_has_illumination_max_wavelength_nm = false;
        ui_state->calibration_has_illumination_bandwidth_fwhm_nm = false;
        ui_state->calibration_illumination_wavelength_confidence = "not_applicable";
    } else if (preset == "unknown") {
        ui_state->calibration_light_state = "unknown";
        ui_state->calibration_illumination_spectrum = "unknown";
        ui_state->calibration_illumination_source = "unknown";
        ui_state->calibration_has_illumination_center_wavelength_nm = false;
        ui_state->calibration_has_illumination_min_wavelength_nm = false;
        ui_state->calibration_has_illumination_max_wavelength_nm = false;
        ui_state->calibration_has_illumination_bandwidth_fwhm_nm = false;
        ui_state->calibration_illumination_wavelength_confidence = "unknown";
    }
}

std::string make_spatial_rig_io_connection_key(const CameraParams& camera_params,
                                               const CameraRigIoConnection& connection)
{
    return camera_params.camera_serial + ":" + connection.camera_line + ":" + connection.purpose;
}

const CameraRigIoConnection* find_mapped_nir_strobe_output_connection(const CameraParams& camera_params)
{
    for (const CameraRigIoConnection& connection : camera_params.rig_io_connections) {
        if (connection.purpose == "nir_strobe_trigger" && connection.direction == "output") {
            return &connection;
        }
    }
    return nullptr;
}

bool camera_has_exposed_mapped_nir_strobe(const CameraParams& camera_params)
{
    return camera_params.gpio_pinout_access == "exposed" &&
           find_mapped_nir_strobe_output_connection(camera_params) != nullptr;
}

bool calibration_light_handling_needs_mapped_strobe(const std::string& requested_handling)
{
    const std::string handling = requested_handling.empty() ? "leave_current" : requested_handling;
    return handling == "suppress_mapped_strobe" ||
           handling == "keep_or_restore_mapped_pulse" ||
           handling == "force_manual_active";
}

void set_calibration_preflight_result(SpatialLayoutUiState* ui_state,
                                      const bool ok,
                                      const std::string& message)
{
    if (ui_state == nullptr) {
        return;
    }
    if (ok) {
        ui_state->calibration_preflight_status = message;
        ui_state->calibration_preflight_error.clear();
    } else {
        ui_state->calibration_preflight_error = message;
        ui_state->calibration_preflight_status.clear();
    }
}

void clear_calibration_capture_profile_state(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr) {
        return;
    }
    ui_state->calibration_capture_profile_active = false;
    ui_state->calibration_capture_profile_id.clear();
    ui_state->calibration_capture_profile_operation_id.clear();
    ui_state->calibration_capture_profile_camera_serial.clear();
    ui_state->calibration_capture_profile_light_camera_serial.clear();
}

void mark_calibration_capture_profile_active(
    SpatialLayoutUiState* ui_state,
    const CameraParams& capture_params,
    const CameraParams* light_params)
{
    if (ui_state == nullptr) {
        return;
    }
    ui_state->calibration_capture_profile_active = true;
    ui_state->calibration_capture_profile_id = kCalibrationCaptureProfileId;
    ui_state->calibration_capture_profile_operation_id =
        std::string(kCalibrationCaptureProfileId) + "_" +
        sanitize_artifact_component(get_current_utc_timestamp()) +
        "_Cam" + sanitize_artifact_component(capture_params.camera_serial);
    ui_state->calibration_capture_profile_camera_serial = capture_params.camera_serial;
    ui_state->calibration_capture_profile_light_camera_serial =
        light_params ? light_params->camera_serial : "";
}

void mark_calibration_capture_profile_active_for_cameras(
    SpatialLayoutUiState* ui_state,
    CameraParams* cameras_params,
    const std::vector<int>& camera_indices,
    const CameraParams* light_params)
{
    if (ui_state == nullptr) {
        return;
    }
    std::ostringstream camera_serials;
    for (size_t idx = 0; idx < camera_indices.size(); ++idx) {
        const int camera_index = camera_indices[idx];
        if (camera_index < 0 || cameras_params == nullptr) {
            continue;
        }
        if (camera_serials.tellp() > 0) {
            camera_serials << ",";
        }
        camera_serials << cameras_params[camera_index].camera_serial;
    }
    ui_state->calibration_capture_profile_active = true;
    ui_state->calibration_capture_profile_id = kCalibrationCaptureProfileId;
    ui_state->calibration_capture_profile_operation_id =
        std::string(kCalibrationCaptureProfileId) + "_" +
        sanitize_artifact_component(get_current_utc_timestamp()) +
        "_all_open_cameras";
    ui_state->calibration_capture_profile_camera_serial =
        "all_open:" + camera_serials.str();
    ui_state->calibration_capture_profile_light_camera_serial =
        light_params ? light_params->camera_serial : "";
}

bool set_camera_uint32_param_for_calibration(
    Emergent::CEmergentCamera* camera,
    CameraParams* camera_params,
    const char* node_name,
    const unsigned int requested_value,
    unsigned int* cached_value,
    unsigned int* cached_min,
    unsigned int* cached_max,
    unsigned int* cached_inc,
    std::string* status_out)
{
    if (camera == nullptr || camera_params == nullptr || node_name == nullptr ||
        cached_value == nullptr || cached_min == nullptr || cached_max == nullptr ||
        cached_inc == nullptr) {
        if (status_out) {
            *status_out = "Calibration capture settings failed: camera parameter input was invalid.";
        }
        return false;
    }

    if (!get_camera_uint32_param_range(camera, node_name, cached_min, cached_max, cached_inc)) {
        if (status_out) {
            *status_out = std::string("Calibration capture settings failed: unable to query ") +
                          node_name + " range for camera " + camera_params->camera_serial + ".";
        }
        return false;
    }

    const unsigned int clamped_value = std::clamp(requested_value, *cached_min, *cached_max);
    const EVT_ERROR set_err =
        Emergent::EVT_CameraSetUInt32Param(camera, node_name, clamped_value);
    if (set_err != EVT_SUCCESS) {
        if (status_out) {
            std::ostringstream oss;
            oss << "Calibration capture settings failed: " << node_name
                << " requested=" << requested_value
                << " clamped=" << clamped_value
                << " camera=" << camera_params->camera_serial
                << " error=" << get_evt_error_string(set_err) << ".";
            *status_out = oss.str();
        }
        return false;
    }

    unsigned int readback = 0;
    const EVT_ERROR get_err =
        Emergent::EVT_CameraGetUInt32Param(camera, node_name, &readback);
    if (get_err != EVT_SUCCESS) {
        *cached_value = clamped_value;
        if (status_out) {
            std::ostringstream oss;
            oss << "Calibration capture settings warning: " << node_name
                << " set to " << clamped_value
                << " for camera " << camera_params->camera_serial
                << ", but readback failed: " << get_evt_error_string(get_err) << ".";
            *status_out = oss.str();
        }
        return true;
    }

    *cached_value = readback;
    if (status_out) {
        std::ostringstream oss;
        oss << node_name << " requested=" << requested_value
            << " applied=" << readback;
        if (readback != requested_value) {
            oss << " range=[" << *cached_min << "," << *cached_max << "]";
            if (*cached_inc > 0) {
                oss << " inc=" << *cached_inc;
            }
        }
        *status_out = oss.str();
    }
    return true;
}

bool capture_calibration_acquisition_restore_state_if_needed(
    SpatialLayoutUiState* ui_state,
    const CameraParams& camera_params)
{
    if (ui_state == nullptr) {
        return true;
    }
    for (const CalibrationCaptureCameraRestoreState& restore_state :
         ui_state->calibration_capture_restore_states) {
        if (restore_state.valid &&
            restore_state.camera_serial == camera_params.camera_serial) {
            return true;
        }
    }
    CalibrationCaptureCameraRestoreState restore_state;
    restore_state.valid = true;
    restore_state.camera_serial = camera_params.camera_serial;
    restore_state.exposure_us = camera_params.exposure;
    restore_state.frame_rate_hz = camera_params.frame_rate;
    ui_state->calibration_capture_restore_states.push_back(std::move(restore_state));
    return true;
}

CalibrationCaptureCameraRestoreState* find_calibration_capture_restore_state(
    SpatialLayoutUiState* ui_state,
    const std::string& camera_serial)
{
    if (ui_state == nullptr) {
        return nullptr;
    }
    for (CalibrationCaptureCameraRestoreState& restore_state :
         ui_state->calibration_capture_restore_states) {
        if (restore_state.valid && restore_state.camera_serial == camera_serial) {
            return &restore_state;
        }
    }
    return nullptr;
}

bool has_calibration_capture_restore_state(
    const SpatialLayoutUiState* ui_state,
    const std::string& camera_serial)
{
    if (ui_state == nullptr) {
        return false;
    }
    for (const CalibrationCaptureCameraRestoreState& restore_state :
         ui_state->calibration_capture_restore_states) {
        if (restore_state.valid && restore_state.camera_serial == camera_serial) {
            return true;
        }
    }
    return false;
}

void clear_calibration_capture_restore_state(
    SpatialLayoutUiState* ui_state,
    const std::string& camera_serial)
{
    if (ui_state == nullptr) {
        return;
    }
    auto& restore_states = ui_state->calibration_capture_restore_states;
    restore_states.erase(
        std::remove_if(
            restore_states.begin(),
            restore_states.end(),
            [&](const CalibrationCaptureCameraRestoreState& restore_state) {
                return restore_state.camera_serial == camera_serial;
            }),
        restore_states.end());
}

bool restore_calibration_capture_settings_to_camera(
    SpatialLayoutUiState* ui_state,
    CameraEmergent* ecam,
    CameraParams* camera_params,
    bool recording_mutation_locked,
    std::string* status_out);

bool apply_calibration_capture_settings_to_camera(
    SpatialLayoutUiState* ui_state,
    CameraEmergent* ecam,
    CameraParams* camera_params,
    const bool recording_mutation_locked,
    std::string* status_out)
{
    if (ecam == nullptr || camera_params == nullptr) {
        if (status_out) {
            *status_out = "Calibration capture settings failed: selected camera is not open.";
        }
        return false;
    }
    if (recording_mutation_locked) {
        if (status_out) {
            *status_out = "Calibration capture settings blocked while recording/finalizing.";
        }
        return false;
    }

    capture_calibration_acquisition_restore_state_if_needed(ui_state, *camera_params);

    std::string frame_status;
    if (!set_camera_uint32_param_for_calibration(
            &ecam->camera,
            camera_params,
            "FrameRate",
            kCalibrationCaptureFrameRateHz,
            &camera_params->frame_rate,
            &camera_params->frame_rate_min,
            &camera_params->frame_rate_max,
            &camera_params->frame_rate_inc,
            &frame_status)) {
        if (status_out) {
            *status_out = frame_status;
        }
        return false;
    }

    std::string exposure_status;
    if (!set_camera_uint32_param_for_calibration(
            &ecam->camera,
            camera_params,
            "Exposure",
            kCalibrationCaptureExposureUs,
            &camera_params->exposure,
            &camera_params->exposure_min,
            &camera_params->exposure_max,
            &camera_params->exposure_inc,
            &exposure_status)) {
        if (status_out) {
            std::string restore_status;
            restore_calibration_capture_settings_to_camera(
                ui_state,
                ecam,
                camera_params,
                recording_mutation_locked,
                &restore_status);
            *status_out = exposure_status + " Restore attempted: " + restore_status;
        }
        return false;
    }

    if (status_out) {
        std::ostringstream oss;
        oss << "Calibration capture settings applied to " << camera_params->camera_serial
            << ": " << frame_status << "; " << exposure_status << ".";
        *status_out = oss.str();
    }
    return true;
}

bool restore_calibration_capture_settings_to_camera(
    SpatialLayoutUiState* ui_state,
    CameraEmergent* ecam,
    CameraParams* camera_params,
    const bool recording_mutation_locked,
    std::string* status_out)
{
    if (ui_state == nullptr || ecam == nullptr || camera_params == nullptr) {
        if (status_out) {
            *status_out = "Calibration capture restore failed: selected camera is not open.";
        }
        return false;
    }
    if (recording_mutation_locked) {
        if (status_out) {
            *status_out = "Calibration capture restore blocked while recording/finalizing.";
        }
        return false;
    }
    const CalibrationCaptureCameraRestoreState* restore_state =
        find_calibration_capture_restore_state(ui_state, camera_params->camera_serial);
    if (restore_state == nullptr) {
        if (status_out) {
            *status_out = "Calibration capture restore skipped: no saved settings for this camera.";
        }
        return false;
    }

    std::string exposure_status;
    if (!set_camera_uint32_param_for_calibration(
            &ecam->camera,
            camera_params,
            "Exposure",
            restore_state->exposure_us,
            &camera_params->exposure,
            &camera_params->exposure_min,
            &camera_params->exposure_max,
            &camera_params->exposure_inc,
            &exposure_status)) {
        if (status_out) {
            *status_out = exposure_status;
        }
        return false;
    }

    std::string frame_status;
    if (!set_camera_uint32_param_for_calibration(
            &ecam->camera,
            camera_params,
            "FrameRate",
            restore_state->frame_rate_hz,
            &camera_params->frame_rate,
            &camera_params->frame_rate_min,
            &camera_params->frame_rate_max,
            &camera_params->frame_rate_inc,
            &frame_status)) {
        if (status_out) {
            *status_out = frame_status;
        }
        return false;
    }

    clear_calibration_capture_restore_state(ui_state, camera_params->camera_serial);
    if (status_out) {
        std::ostringstream oss;
        oss << "Calibration capture settings restored for " << camera_params->camera_serial
            << ": " << exposure_status << "; " << frame_status << ".";
        *status_out = oss.str();
    }
    return true;
}

bool capture_calibration_light_restore_state_if_needed(
    SpatialLayoutUiState* ui_state,
    CameraEmergent* ecam,
    const CameraParams& camera_params,
    const CameraRigIoConnection& connection,
    const std::string& connection_key,
    std::string* status_out)
{
    if (ui_state == nullptr) {
        if (status_out) {
            *status_out = "Calibration light action failed: UI state is null.";
        }
        return false;
    }
    if (ui_state->calibration_light_restore_state.valid &&
        ui_state->calibration_light_restore_key == connection_key) {
        return true;
    }

    CameraRigIoOutputState captured_state;
    if (!read_rig_io_output_diagnostic_state(
            ecam ? &ecam->camera : nullptr,
            &camera_params,
            connection,
            &captured_state,
            status_out)) {
        return false;
    }
    ui_state->calibration_light_restore_state = std::move(captured_state);
    ui_state->calibration_light_restore_key = connection_key;
    return true;
}

bool restore_calibration_mapped_strobe_pulse(
    SpatialLayoutUiState* ui_state,
    CameraEmergent* ecam,
    const CameraParams& camera_params,
    const CameraRigIoConnection& connection,
    const std::string& connection_key,
    std::string* status_out)
{
    if (ui_state != nullptr &&
        ui_state->calibration_light_restore_state.valid &&
        ui_state->calibration_light_restore_key == connection_key) {
        return restore_rig_io_output_diagnostic_state(
            ecam ? &ecam->camera : nullptr,
            &camera_params,
            ui_state->calibration_light_restore_state,
            status_out);
    }
    return restore_rig_io_output_normal_mode(
        ecam ? &ecam->camera : nullptr,
        &camera_params,
        connection,
        status_out);
}

bool apply_calibration_light_handling_to_camera(
    SpatialLayoutUiState* ui_state,
    CameraEmergent* ecam,
    const CameraParams& camera_params,
    const bool recording_mutation_locked,
    const std::string& requested_handling,
    std::string* status_out)
{
    const std::string handling = requested_handling.empty() ? "leave_current" : requested_handling;
    if (handling == "leave_current") {
        if (status_out) {
            *status_out = "Calibration light handling left current light/GPO state unchanged.";
        }
        return true;
    }
    if (handling == "operator_manual") {
        if (status_out) {
            *status_out = "Calibration light handling recorded as operator_manual; no Orange GPO write was made.";
        }
        return true;
    }

    if (recording_mutation_locked) {
        if (status_out) {
            *status_out = "Calibration light action blocked while recording/finalizing.";
        }
        return false;
    }
    if (camera_params.gpio_pinout_access == "not_exposed") {
        if (status_out) {
            *status_out =
                "Calibration light action blocked: GPIO pinout access is not exposed for the light-control camera.";
        }
        return false;
    }

    const CameraRigIoConnection* connection =
        find_mapped_nir_strobe_output_connection(camera_params);
    if (connection == nullptr) {
        if (status_out) {
            *status_out =
                "Calibration light action failed: light-control camera has no nir_strobe_trigger output mapping.";
        }
        return false;
    }
    const std::string connection_key =
        make_spatial_rig_io_connection_key(camera_params, *connection);

    if (handling == "suppress_mapped_strobe") {
        std::string capture_status;
        if (!capture_calibration_light_restore_state_if_needed(
                ui_state,
                ecam,
                camera_params,
                *connection,
                connection_key,
                &capture_status)) {
            if (status_out) {
                *status_out = capture_status;
            }
            return false;
        }
        if (!set_rig_io_output_diagnostic(
                ecam ? &ecam->camera : nullptr,
                &camera_params,
                *connection,
                false,
                status_out)) {
            return false;
        }
        if (ui_state != nullptr) {
            ui_state->calibration_light_state = "ttl_nir_strobe_inactive";
        }
        return true;
    }

    if (handling == "keep_or_restore_mapped_pulse") {
        if (!restore_calibration_mapped_strobe_pulse(
                ui_state,
                ecam,
                camera_params,
                *connection,
                connection_key,
                status_out)) {
            return false;
        }
        if (ui_state != nullptr) {
            ui_state->calibration_light_state = "ttl_nir_strobe_active";
        }
        return true;
    }

    if (handling == "force_manual_active") {
        std::string capture_status;
        if (!capture_calibration_light_restore_state_if_needed(
                ui_state,
                ecam,
                camera_params,
                *connection,
                connection_key,
                &capture_status)) {
            if (status_out) {
                *status_out = capture_status;
            }
            return false;
        }
        if (!set_rig_io_output_diagnostic(
                ecam ? &ecam->camera : nullptr,
                &camera_params,
                *connection,
                true,
                status_out)) {
            return false;
        }
        if (ui_state != nullptr) {
            ui_state->calibration_light_state = "ttl_nir_strobe_active";
        }
        return true;
    }

    if (status_out) {
        *status_out = "Calibration light action failed: unsupported light_handling `" + handling + "`.";
    }
    return false;
}

bool restore_calibration_capture_preflight(
    SpatialLayoutUiState* ui_state,
    CameraEmergent* capture_ecam,
    CameraParams* capture_params,
    CameraEmergent* light_ecam,
    const CameraParams* light_params,
    const bool mapped_strobe_available,
    const bool recording_mutation_locked,
    std::string* status_out)
{
    bool ok = true;
    std::vector<std::string> statuses;

    if (ui_state != nullptr && capture_params != nullptr &&
        has_calibration_capture_restore_state(ui_state, capture_params->camera_serial)) {
        std::string capture_status;
        const bool capture_ok = restore_calibration_capture_settings_to_camera(
            ui_state,
            capture_ecam,
            capture_params,
            recording_mutation_locked,
            &capture_status);
        ok &= capture_ok;
        statuses.push_back(capture_status);
    } else {
        statuses.push_back("Capture settings already match the current loaded/original state for the selected camera.");
    }

    if (mapped_strobe_available && light_ecam != nullptr && light_params != nullptr) {
        std::string light_status;
        bool light_ok = false;
        const CameraRigIoConnection* connection =
            find_mapped_nir_strobe_output_connection(*light_params);
        if (connection == nullptr) {
            light_status = "Mapped strobe restore failed: light-control camera mapping disappeared.";
        } else if (recording_mutation_locked) {
            light_status = "Mapped strobe restore blocked while recording/finalizing.";
        } else {
            const std::string connection_key =
                make_spatial_rig_io_connection_key(*light_params, *connection);
            light_ok = restore_calibration_mapped_strobe_pulse(
                ui_state,
                light_ecam,
                *light_params,
                *connection,
                connection_key,
                &light_status);
            if (light_ok && ui_state != nullptr) {
                if (ui_state->calibration_light_restore_key == connection_key) {
                    ui_state->calibration_light_restore_state = CameraRigIoOutputState{};
                    ui_state->calibration_light_restore_key.clear();
                }
                ui_state->calibration_light_state = "ttl_nir_strobe_active";
            }
        }
        ok &= light_ok;
        statuses.push_back(light_status);
    } else {
        statuses.push_back("No mapped strobe restore was available.");
    }

    if (ok) {
        clear_calibration_capture_profile_state(ui_state);
    }

    if (status_out) {
        std::ostringstream oss;
        oss << "Calibration restore " << (ok ? "complete" : "incomplete") << ": ";
        for (size_t i = 0; i < statuses.size(); ++i) {
            if (i > 0) {
                oss << " ";
            }
            oss << statuses[i];
        }
        *status_out = oss.str();
    }
    return ok;
}

bool prepare_calibration_capture_preflight(
    SpatialLayoutUiState* ui_state,
    CameraEmergent* capture_ecam,
    CameraParams* capture_params,
    CameraEmergent* light_ecam,
    const CameraParams* light_params,
    const bool mapped_strobe_available,
    const bool recording_mutation_locked,
    const std::string& requested_light_handling,
    std::string* status_out)
{
    if (capture_ecam == nullptr || capture_params == nullptr) {
        if (status_out) {
            *status_out = "Calibration prepare failed: selected capture camera is not open.";
        }
        return false;
    }
    if (recording_mutation_locked) {
        if (status_out) {
            *status_out = "Calibration prepare blocked while recording/finalizing.";
        }
        return false;
    }

    const bool needs_mapped_strobe =
        calibration_light_handling_needs_mapped_strobe(requested_light_handling);
    CameraEmergent* action_light_ecam =
        mapped_strobe_available ? light_ecam : capture_ecam;
    const CameraParams* action_light_params =
        mapped_strobe_available ? light_params : capture_params;
    if (needs_mapped_strobe && !mapped_strobe_available) {
        if (status_out) {
            *status_out = "Calibration prepare failed: no exposed nir_strobe_trigger light-control camera is available.";
        }
        return false;
    }

    std::string light_status;
    const bool light_ok = apply_calibration_light_handling_to_camera(
        ui_state,
        action_light_ecam,
        *action_light_params,
        recording_mutation_locked,
        requested_light_handling,
        &light_status);
    if (!light_ok) {
        if (status_out) {
            *status_out = light_status;
        }
        return false;
    }

    std::string capture_status;
    const bool capture_ok = apply_calibration_capture_settings_to_camera(
        ui_state,
        capture_ecam,
        capture_params,
        recording_mutation_locked,
        &capture_status);
    if (!capture_ok) {
        std::string restore_status;
        restore_calibration_capture_preflight(
            ui_state,
            capture_ecam,
            capture_params,
            light_ecam,
            light_params,
            mapped_strobe_available,
            recording_mutation_locked,
            &restore_status);
        if (status_out) {
            *status_out = capture_status + " Restore attempted: " + restore_status;
        }
        return false;
    }

    mark_calibration_capture_profile_active(
        ui_state,
        *capture_params,
        mapped_strobe_available ? light_params : nullptr);
    if (status_out) {
        std::ostringstream oss;
        oss << "Calibration capture profile " << kCalibrationCaptureProfileId
            << " prepared transactionally: " << light_status << " "
            << capture_status;
        *status_out = oss.str();
    }
    return true;
}

bool restore_calibration_capture_preflight_all_cameras(
    SpatialLayoutUiState* ui_state,
    CameraEmergent* ecams,
    CameraParams* cameras_params,
    int num_cameras,
    CameraEmergent* light_ecam,
    const CameraParams* light_params,
    const bool mapped_strobe_available,
    const bool recording_mutation_locked,
    std::string* status_out)
{
    if (ui_state == nullptr || ecams == nullptr || cameras_params == nullptr || num_cameras <= 0) {
        if (status_out) {
            *status_out = "All-camera calibration restore failed: no open cameras are available.";
        }
        return false;
    }
    if (recording_mutation_locked) {
        if (status_out) {
            *status_out = "All-camera calibration restore blocked while recording/finalizing.";
        }
        return false;
    }

    bool ok = true;
    int restored_count = 0;
    std::vector<std::string> statuses;
    for (int camera_index = 0; camera_index < num_cameras; ++camera_index) {
        CameraParams* camera_params = &cameras_params[camera_index];
        if (!has_calibration_capture_restore_state(ui_state, camera_params->camera_serial)) {
            continue;
        }
        std::string camera_status;
        const bool camera_ok = restore_calibration_capture_settings_to_camera(
            ui_state,
            &ecams[camera_index],
            camera_params,
            recording_mutation_locked,
            &camera_status);
        ok &= camera_ok;
        if (camera_ok) {
            ++restored_count;
        }
        statuses.push_back(camera_status);
    }
    if (restored_count == 0 && statuses.empty()) {
        statuses.push_back("No saved camera timing settings needed restore.");
    }

    if (mapped_strobe_available && light_ecam != nullptr && light_params != nullptr) {
        std::string light_status;
        bool light_ok = false;
        const CameraRigIoConnection* connection =
            find_mapped_nir_strobe_output_connection(*light_params);
        if (connection == nullptr) {
            light_status = "Mapped strobe restore failed: light-control camera mapping disappeared.";
        } else {
            const std::string connection_key =
                make_spatial_rig_io_connection_key(*light_params, *connection);
            light_ok = restore_calibration_mapped_strobe_pulse(
                ui_state,
                light_ecam,
                *light_params,
                *connection,
                connection_key,
                &light_status);
            if (light_ok) {
                if (ui_state->calibration_light_restore_key == connection_key) {
                    ui_state->calibration_light_restore_state = CameraRigIoOutputState{};
                    ui_state->calibration_light_restore_key.clear();
                }
                ui_state->calibration_light_state = "ttl_nir_strobe_active";
            }
        }
        ok &= light_ok;
        statuses.push_back(light_status);
    } else {
        statuses.push_back("No mapped strobe restore was available.");
    }

    if (ok && ui_state->calibration_capture_restore_states.empty()) {
        clear_calibration_capture_profile_state(ui_state);
    }

    if (status_out) {
        std::ostringstream oss;
        oss << "All-camera calibration restore "
            << (ok ? "complete" : "incomplete")
            << " (" << restored_count << " camera(s) restored): ";
        for (size_t idx = 0; idx < statuses.size(); ++idx) {
            if (idx > 0) {
                oss << " ";
            }
            oss << statuses[idx];
        }
        *status_out = oss.str();
    }
    return ok;
}

bool prepare_calibration_capture_preflight_all_cameras(
    SpatialLayoutUiState* ui_state,
    CameraEmergent* ecams,
    CameraParams* cameras_params,
    int num_cameras,
    CameraEmergent* light_ecam,
    const CameraParams* light_params,
    const bool mapped_strobe_available,
    const bool recording_mutation_locked,
    const std::string& requested_light_handling,
    std::string* status_out)
{
    if (ui_state == nullptr || ecams == nullptr || cameras_params == nullptr || num_cameras <= 0) {
        if (status_out) {
            *status_out = "All-camera calibration prepare failed: no open cameras are available.";
        }
        return false;
    }
    if (recording_mutation_locked) {
        if (status_out) {
            *status_out = "All-camera calibration prepare blocked while recording/finalizing.";
        }
        return false;
    }

    const bool needs_mapped_strobe =
        calibration_light_handling_needs_mapped_strobe(requested_light_handling);
    if (needs_mapped_strobe && !mapped_strobe_available) {
        if (status_out) {
            *status_out = "All-camera calibration prepare failed: no exposed nir_strobe_trigger light-control camera is available.";
        }
        return false;
    }

    CameraEmergent* action_light_ecam =
        mapped_strobe_available ? light_ecam : &ecams[0];
    const CameraParams* action_light_params =
        mapped_strobe_available ? light_params : &cameras_params[0];

    std::string light_status;
    const bool light_ok = apply_calibration_light_handling_to_camera(
        ui_state,
        action_light_ecam,
        *action_light_params,
        recording_mutation_locked,
        requested_light_handling,
        &light_status);
    if (!light_ok) {
        if (status_out) {
            *status_out = light_status;
        }
        return false;
    }

    std::vector<int> prepared_indices;
    for (int camera_index = 0; camera_index < num_cameras; ++camera_index) {
        std::string camera_status;
        const bool camera_ok = apply_calibration_capture_settings_to_camera(
            ui_state,
            &ecams[camera_index],
            &cameras_params[camera_index],
            recording_mutation_locked,
            &camera_status);
        if (!camera_ok) {
            std::string restore_status;
            restore_calibration_capture_preflight_all_cameras(
                ui_state,
                ecams,
                cameras_params,
                num_cameras,
                light_ecam,
                light_params,
                mapped_strobe_available,
                recording_mutation_locked,
                &restore_status);
            if (status_out) {
                std::ostringstream oss;
                oss << "All-camera calibration prepare failed after light handling was applied: "
                    << camera_status << " Rollback attempted: " << restore_status;
                *status_out = oss.str();
            }
            return false;
        }
        prepared_indices.push_back(camera_index);
    }

    mark_calibration_capture_profile_active_for_cameras(
        ui_state,
        cameras_params,
        prepared_indices,
        mapped_strobe_available ? light_params : nullptr);

    if (status_out) {
        std::ostringstream oss;
        oss << "All-camera calibration capture profile " << kCalibrationCaptureProfileId
            << " prepared transactionally: " << light_status
            << " Applied 10 FPS / 10 ms to " << prepared_indices.size()
            << " camera(s).";
        *status_out = oss.str();
    }
    return true;
}

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

bool parse_required_json_number(const nlohmann::json& node,
                                const char* field,
                                double* out,
                                std::string* error_out)
{
    if (out == nullptr) {
        if (error_out) {
            *error_out = "Null number destination.";
        }
        return false;
    }
    if (!node.contains(field) || !node.at(field).is_number()) {
        if (error_out) {
            *error_out = std::string("Missing numeric field: ") + field;
        }
        return false;
    }
    *out = node.at(field).get<double>();
    return true;
}

bool parse_optional_json_number(const nlohmann::json& node,
                                const char* field,
                                double* out)
{
    if (out == nullptr || !node.contains(field) || !node.at(field).is_number()) {
        return false;
    }
    *out = node.at(field).get<double>();
    return true;
}

bool json_string_equals_ignore_case(const nlohmann::json& node,
                                    const char* field,
                                    const std::string& expected_uppercase)
{
    if (!node.contains(field) || !node.at(field).is_string()) {
        return false;
    }
    std::string value = node.at(field).get<std::string>();
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return value == expected_uppercase;
}

template <typename T>
T clamp_index(T value, T count)
{
    if (count <= 0) {
        return 0;
    }
    return std::clamp(value, static_cast<T>(0), static_cast<T>(count - 1));
}

Point2d make_point(double x, double y)
{
    return Point2d{x, y};
}

Point2d citrus_arena_origin_canvas_px(const CitrusSpatialTemplateState& template_state)
{
    if (!template_state.has_arena_canvas_region) {
        return make_point(0.0, 0.0);
    }
    return make_point(
        template_state.arena_center_x_px - template_state.arena_width_px * 0.5,
        template_state.arena_center_y_px - template_state.arena_height_px * 0.5);
}

Point2d citrus_arena_relative_to_canvas_px(
    const CitrusSpatialTemplateState& template_state,
    const Point2d& arena_relative_point)
{
    const Point2d origin = citrus_arena_origin_canvas_px(template_state);
    return make_point(origin.x + arena_relative_point.x,
                      origin.y + arena_relative_point.y);
}

Point2d citrus_canvas_to_arena_relative_px(
    const CitrusSpatialTemplateState& template_state,
    const Point2d& canvas_point)
{
    const Point2d origin = citrus_arena_origin_canvas_px(template_state);
    return make_point(canvas_point.x - origin.x,
                      canvas_point.y - origin.y);
}

Point2d layout_geometry_center(const LayoutGeometry& geometry)
{
    if (geometry.type == LayoutGeometryType::kCircle) {
        return make_point(geometry.circle.cx, geometry.circle.cy);
    }
    return make_point(geometry.rectangle.x + geometry.rectangle.width * 0.5,
                      geometry.rectangle.y + geometry.rectangle.height * 0.5);
}

double layout_geometry_max_dimension(const LayoutGeometry& geometry)
{
    if (geometry.type == LayoutGeometryType::kCircle) {
        return geometry.circle.r * 2.0;
    }
    return std::max(geometry.rectangle.width, geometry.rectangle.height);
}

RuntimeGeometry runtime_circle(double cx, double cy, double r)
{
    RuntimeGeometry geometry;
    geometry.type = RuntimeGeometryType::kCircle;
    geometry.circle.cx = cx;
    geometry.circle.cy = cy;
    geometry.circle.r = r;
    return geometry;
}

RuntimeGeometry runtime_oriented_rectangle(
    double cx,
    double cy,
    double width,
    double height,
    double rotation_deg_clockwise)
{
    RuntimeGeometry geometry;
    geometry.type = RuntimeGeometryType::kOrientedRectangle;
    geometry.oriented_rectangle.cx = cx;
    geometry.oriented_rectangle.cy = cy;
    geometry.oriented_rectangle.width = width;
    geometry.oriented_rectangle.height = height;
    geometry.oriented_rectangle.rotation_deg_clockwise = rotation_deg_clockwise;
    return geometry;
}

LayoutGeometry default_outer_geometry()
{
    LayoutGeometry geometry;
    geometry.type = LayoutGeometryType::kCircle;
    geometry.circle.cx = 0.0;
    geometry.circle.cy = 0.0;
    geometry.circle.r = 50.0;
    return geometry;
}

ArenaLayoutZone make_experimental_area_zone(const LayoutGeometry& outer_geometry)
{
    ArenaLayoutZone zone;
    zone.zone_id = kExperimentalAreaZoneId;
    zone.has_zone_index = true;
    zone.zone_index = 0;
    zone.display_label = kExperimentalAreaZoneLabel;
    zone.geometry = outer_geometry;
    return zone;
}

bool has_single_experimental_area_zone(const SpatialLayoutUiState& ui_state)
{
    return ui_state.layout_artifact.layout.zones.size() == 1 &&
           ui_state.layout_artifact.layout.zones.front().zone_id == kExperimentalAreaZoneId;
}

void sync_single_experimental_area_zone(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr || !has_single_experimental_area_zone(*ui_state)) {
        return;
    }

    ArenaLayoutZone& zone = ui_state->layout_artifact.layout.zones.front();
    zone.has_zone_index = true;
    zone.zone_index = 0;
    if (zone.display_label.empty() || zone.display_label == "Zone 0") {
        zone.display_label = kExperimentalAreaZoneLabel;
    }
    zone.geometry = ui_state->layout_artifact.layout.outer_geometry;
    ui_state->selected_zone_index = 0;
}

void reset_to_single_experimental_area_zone(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr) {
        return;
    }

    ui_state->layout_artifact.layout.zones.clear();
    ui_state->layout_artifact.layout.zones.push_back(
        make_experimental_area_zone(ui_state->layout_artifact.layout.outer_geometry));
    ui_state->selected_zone_index = 0;
}

ArenaLayoutZone make_default_zone(const LayoutGeometry& outer_geometry, int zone_index)
{
    ArenaLayoutZone zone;
    zone.zone_id = "z" + std::to_string(zone_index);
    zone.has_zone_index = true;
    zone.zone_index = zone_index;
    zone.display_label = "Zone " + std::to_string(zone_index);

    const int col = zone_index % 3;
    const int row = (zone_index / 3) % 3;
    const double norm_x = (static_cast<double>(col) - 1.0) * 0.35;
    const double norm_y = (static_cast<double>(row) - 1.0) * 0.35;

    zone.geometry.type = LayoutGeometryType::kCircle;
    if (outer_geometry.type == LayoutGeometryType::kCircle) {
        zone.geometry.circle.cx = outer_geometry.circle.cx + norm_x * outer_geometry.circle.r;
        zone.geometry.circle.cy = outer_geometry.circle.cy + norm_y * outer_geometry.circle.r;
        zone.geometry.circle.r = outer_geometry.circle.r * 0.18;
    } else {
        zone.geometry.circle.cx = outer_geometry.rectangle.x + outer_geometry.rectangle.width * (0.5 + norm_x * 0.8);
        zone.geometry.circle.cy = outer_geometry.rectangle.y + outer_geometry.rectangle.height * (0.5 + norm_y * 0.8);
        zone.geometry.circle.r = std::min(outer_geometry.rectangle.width, outer_geometry.rectangle.height) * 0.12;
    }
    return zone;
}

void initialize_spatial_layout_defaults(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr || !ui_state->layout_artifact.layout_id.empty()) {
        return;
    }

    ArenaLayoutArtifact artifact;
    artifact.artifact_id = "preview.arena_layout";
    artifact.created_utc = get_current_utc_timestamp();
    artifact.calibration_ref.artifact_id = artifact.artifact_id;
    artifact.calibration_ref.artifact_schema_id = orange::spatial::kArenaLayoutArtifactSchemaId;
    artifact.calibration_ref.artifact_schema_version = orange::spatial::kArenaLayoutArtifactSchemaVersion;
    artifact.calibration_ref.fingerprint = "preview-only";
    artifact.layout_id = "layout_preview";
    artifact.layout.coordinate_space = CoordinateSpace::kLayoutUnits;
    artifact.layout.outer_geometry = default_outer_geometry();
    artifact.layout.zones.push_back(make_experimental_area_zone(artifact.layout.outer_geometry));
    artifact.provenance.source = ArenaLayoutProvenanceSource::kManualTemplate;
    artifact.provenance.ordering_rule = "row_major_top_left";
    artifact.provenance.notes = "Preview-only layout authoring state.";
    artifact.context.canvas_id = "preview_canvas";
    ui_state->layout_artifact = std::move(artifact);

    ui_state->registration.type = RegistrationType::kSimilarity;
    ui_state->registration.layout_coordinate_space = ui_state->layout_artifact.layout.coordinate_space;
    ui_state->registration.source = RegistrationSource::kManualFit;
    ui_state->registration.fit_point_count = 0;
    ui_state->registration.residual_px = 0.0;
    ui_state->registration.has_orientation_status = true;
    ui_state->registration.orientation_status = OrientationStatus::kUnknown;

    ui_state->registration_tx_px = 512.0;
    ui_state->registration_ty_px = 512.0;
    ui_state->registration_scale = 8.0;
    ui_state->registration_rotation_deg_clockwise = 0.0;
    ui_state->edge_margin_px = 12.0;
}

void reset_registration_from_frame(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr || ui_state->captured_texture_width <= 0 || ui_state->captured_texture_height <= 0) {
        return;
    }

    const LayoutGeometry& outer = ui_state->layout_artifact.layout.outer_geometry;
    const Point2d center = layout_geometry_center(outer);
    const double layout_dim = std::max(layout_geometry_max_dimension(outer), 1e-6);
    const double frame_dim = static_cast<double>(std::min(ui_state->captured_texture_width, ui_state->captured_texture_height));
    ui_state->registration_scale = std::max(0.1, 0.85 * frame_dim / layout_dim);
    ui_state->registration_rotation_deg_clockwise = 0.0;
    ui_state->registration_tx_px = static_cast<double>(ui_state->captured_texture_width) * 0.5 - ui_state->registration_scale * center.x;
    ui_state->registration_ty_px = static_cast<double>(ui_state->captured_texture_height) * 0.5 - ui_state->registration_scale * center.y;
    ui_state->captured_canvas_view.fit_requested = true;
}

void clear_detected_experimental_area_circle(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr) {
        return;
    }
    ui_state->has_detected_experimental_area_circle = false;
    ui_state->detected_experimental_area_geometry = RuntimeGeometry{};
    ui_state->detection_status.clear();
    ui_state->detection_error.clear();
}

void clear_citrus_template_import(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr) {
        return;
    }
    ui_state->citrus_template = {};
    ui_state->citrus_canvas_templates.clear();
    ui_state->citrus_canvas_template_index = -1;
    ui_state->citrus_canvas_config_path.clear();
    ui_state->has_citrus_projected_circle = false;
    ui_state->citrus_projected_circle_geometry = RuntimeGeometry{};
    ui_state->citrus_import_status.clear();
    ui_state->citrus_import_error.clear();
}

bool transform_point_projective(const std::array<double, 9>& matrix,
                                const Point2d& input,
                                Point2d* output)
{
    if (output == nullptr) {
        return false;
    }
    const double w = matrix[6] * input.x + matrix[7] * input.y + matrix[8];
    if (!std::isfinite(w) || std::abs(w) < 1e-12) {
        return false;
    }
    output->x = (matrix[0] * input.x + matrix[1] * input.y + matrix[2]) / w;
    output->y = (matrix[3] * input.x + matrix[4] * input.y + matrix[5]) / w;
    return std::isfinite(output->x) && std::isfinite(output->y);
}

std::vector<Point2d> sample_circle_boundary_points(double cx, double cy, double radius, int point_count)
{
    std::vector<Point2d> points;
    point_count = std::max(3, point_count);
    points.reserve(static_cast<size_t>(point_count));
    for (int idx = 0; idx < point_count; ++idx) {
        const double theta = (2.0 * kPi * static_cast<double>(idx)) / static_cast<double>(point_count);
        points.push_back(make_point(cx + radius * std::cos(theta),
                                    cy + radius * std::sin(theta)));
    }
    return points;
}

bool fit_circle_to_points(const std::vector<Point2d>& points,
                          CircleGeometry* circle_out,
                          double* rms_error_out,
                          std::string* error_out)
{
    if (circle_out == nullptr) {
        if (error_out) {
            *error_out = "Null circle destination.";
        }
        return false;
    }
    if (points.size() < 3) {
        if (error_out) {
            *error_out = "Need at least three points to fit a circle.";
        }
        return false;
    }

    cv::Mat design(static_cast<int>(points.size()), 3, CV_64F);
    cv::Mat rhs(static_cast<int>(points.size()), 1, CV_64F);
    for (int row = 0; row < static_cast<int>(points.size()); ++row) {
        const double x = points[static_cast<size_t>(row)].x;
        const double y = points[static_cast<size_t>(row)].y;
        design.at<double>(row, 0) = x;
        design.at<double>(row, 1) = y;
        design.at<double>(row, 2) = 1.0;
        rhs.at<double>(row, 0) = -(x * x + y * y);
    }

    cv::Mat solution;
    if (!cv::solve(design, rhs, solution, cv::DECOMP_SVD) || solution.rows != 3) {
        if (error_out) {
            *error_out = "Failed to solve circle fit.";
        }
        return false;
    }

    const double d = solution.at<double>(0, 0);
    const double e = solution.at<double>(1, 0);
    const double f = solution.at<double>(2, 0);
    const double cx = -0.5 * d;
    const double cy = -0.5 * e;
    const double radius_sq = cx * cx + cy * cy - f;
    if (!std::isfinite(radius_sq) || radius_sq <= 0.0) {
        if (error_out) {
            *error_out = "Circle fit produced an invalid radius.";
        }
        return false;
    }

    circle_out->cx = cx;
    circle_out->cy = cy;
    circle_out->r = std::sqrt(radius_sq);

    if (rms_error_out != nullptr) {
        double sum_sq = 0.0;
        for (const Point2d& point : points) {
            const double dx = point.x - circle_out->cx;
            const double dy = point.y - circle_out->cy;
            const double radial_error = std::sqrt(dx * dx + dy * dy) - circle_out->r;
            sum_sq += radial_error * radial_error;
        }
        *rms_error_out = std::sqrt(sum_sq / static_cast<double>(points.size()));
    }

    return true;
}

std::string join_strings(const std::vector<std::string>& values, const std::string& separator)
{
    std::ostringstream oss;
    for (size_t idx = 0; idx < values.size(); ++idx) {
        if (idx > 0) {
            oss << separator;
        }
        oss << values[idx];
    }
    return oss.str();
}

bool sample_citrus_experimental_area_outline_in_camera_px(
    const CitrusSpatialTemplateState& template_state,
    const Point2d& center_arena_relative_px,
    std::vector<Point2d>* camera_points_out,
    std::string* error_out)
{
    if (camera_points_out == nullptr) {
        if (error_out) {
            *error_out = "Null Citrus outline destination.";
        }
        return false;
    }
    camera_points_out->clear();
    if (!template_state.available ||
        !template_state.has_arena_canvas_region ||
        !template_state.has_canvas_to_camera_homography ||
        template_state.experimental_area_radius_px <= 0.0) {
        if (error_out) {
            *error_out = "Citrus outline projection requires arena canvas region, canvas-to-camera homography, and positive radius.";
        }
        return false;
    }

    const std::vector<Point2d> arena_relative_points = sample_circle_boundary_points(
        center_arena_relative_px.x,
        center_arena_relative_px.y,
        template_state.experimental_area_radius_px,
        kProjectedCircleSampleCount);
    camera_points_out->reserve(arena_relative_points.size());
    for (const Point2d& arena_relative_point : arena_relative_points) {
        const Point2d canvas_point =
            citrus_arena_relative_to_canvas_px(template_state, arena_relative_point);
        Point2d camera_point{};
        if (!transform_point_projective(
                template_state.canvas_to_camera_homography,
                canvas_point,
                &camera_point)) {
            camera_points_out->clear();
            if (error_out) {
                *error_out = "Failed to project Citrus outline point into camera space.";
            }
            return false;
        }
        camera_points_out->push_back(camera_point);
    }
    return !camera_points_out->empty();
}

bool update_citrus_projected_circle_preview(SpatialLayoutUiState* ui_state,
                                            double* rms_error_out,
                                            std::string* error_out)
{
    if (ui_state == nullptr) {
        if (error_out) {
            *error_out = "Spatial layout UI state is null.";
        }
        return false;
    }
    if (!ui_state->citrus_template.available ||
        !ui_state->citrus_template.has_canvas_to_camera_homography) {
        if (error_out) {
            *error_out = "Imported Citrus template does not have a canvas-to-camera homography.";
        }
        return false;
    }

    std::vector<Point2d> camera_points;
    if (!sample_citrus_experimental_area_outline_in_camera_px(
            ui_state->citrus_template,
            make_point(ui_state->citrus_template.experimental_area_center_x_px,
                       ui_state->citrus_template.experimental_area_center_y_px),
            &camera_points,
            error_out)) {
        return false;
    }

    CircleGeometry fitted_circle;
    double rms_error = 0.0;
    if (!fit_circle_to_points(camera_points, &fitted_circle, &rms_error, error_out)) {
        return false;
    }

    ui_state->has_citrus_projected_circle = true;
    ui_state->citrus_projected_circle_geometry =
        runtime_circle(fitted_circle.cx, fitted_circle.cy, fitted_circle.r);
    if (rms_error_out != nullptr) {
        *rms_error_out = rms_error;
    }
    return true;
}

bool seed_registration_from_citrus_homography(SpatialLayoutUiState* ui_state,
                                              std::string* error_out)
{
    if (ui_state == nullptr) {
        if (error_out) {
            *error_out = "Spatial layout UI state is null.";
        }
        return false;
    }
    if (ui_state->layout_artifact.layout.outer_geometry.type != LayoutGeometryType::kCircle) {
        if (error_out) {
            *error_out = "Citrus homography seeding currently supports only circular outer geometry.";
        }
        return false;
    }

    double rms_error = 0.0;
    if (!update_citrus_projected_circle_preview(ui_state, &rms_error, error_out)) {
        return false;
    }

    const double canonical_radius = ui_state->layout_artifact.layout.outer_geometry.circle.r;
    if (canonical_radius <= 0.0) {
        if (error_out) {
            *error_out = "Canonical outer radius must be positive.";
        }
        return false;
    }

    const Point2d center(
        make_point(ui_state->citrus_projected_circle_geometry.circle.cx,
                   ui_state->citrus_projected_circle_geometry.circle.cy));
    const double scale =
        ui_state->citrus_projected_circle_geometry.circle.r / canonical_radius;
    set_registration_transform(
        ui_state,
        RegistrationType::kSimilarity,
        center,
        scale,
        0.0,
        RegistrationSource::kImported);
    ui_state->registration.fit_point_count = kProjectedCircleSampleCount;
    ui_state->registration.residual_px = std::max(0.0, rms_error);
    return true;
}

bool load_homography_matrix_from_citrus_sidecar(const std::filesystem::path& config_path,
                                                const std::string& config_name,
                                                const std::string& camera_id,
                                                CitrusSpatialTemplateState* template_state,
                                                std::string* error_out)
{
    if (template_state == nullptr) {
        if (error_out) {
            *error_out = "Null Citrus template state.";
        }
        return false;
    }
    if (config_name.empty() || camera_id.empty()) {
        return false;
    }

    std::string safe_camera_id = camera_id;
    std::replace(safe_camera_id.begin(), safe_camera_id.end(), ' ', '_');
    const std::filesystem::path homography_path =
        config_path.parent_path() / "calibration_artifacts" /
        ("homography_" + config_name + "_" + safe_camera_id + ".yml");
    if (!std::filesystem::exists(homography_path)) {
        return false;
    }

    try {
        cv::FileStorage fs(homography_path.string(), cv::FileStorage::READ);
        if (!fs.isOpened()) {
            if (error_out) {
                *error_out = "Found Citrus homography sidecar but could not open it: " + homography_path.string();
            }
            return false;
        }
        cv::Mat homography;
        fs["homography_matrix"] >> homography;
        fs.release();
        if (homography.empty() || homography.rows != 3 || homography.cols != 3) {
            if (error_out) {
                *error_out = "Citrus homography sidecar did not contain a valid 3x3 matrix: " + homography_path.string();
            }
            return false;
        }

        homography.convertTo(homography, CV_64F);
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) {
                template_state->camera_to_canvas_homography[static_cast<size_t>(row * 3 + col)] =
                    homography.at<double>(row, col);
            }
        }
        template_state->has_camera_to_canvas_homography = true;

        const double det = cv::determinant(homography);
        if (std::isfinite(det) && std::abs(det) > 1e-12) {
            const cv::Mat inverse = homography.inv();
            for (int row = 0; row < 3; ++row) {
                for (int col = 0; col < 3; ++col) {
                    template_state->canvas_to_camera_homography[static_cast<size_t>(row * 3 + col)] =
                        inverse.at<double>(row, col);
                }
            }
            template_state->has_canvas_to_camera_homography = true;
        }
    } catch (const cv::Exception& ex) {
        if (error_out) {
            *error_out = std::string("Failed to load Citrus homography sidecar: ") + ex.what();
        }
        return false;
    }

    return true;
}

std::string citrus_template_display_label(const CitrusSpatialTemplateState& template_state)
{
    std::ostringstream label;
    label << (template_state.source_arena_name.empty()
                  ? std::string("arena_unknown")
                  : template_state.source_arena_name);
    if (!template_state.source_camera_id.empty()) {
        label << " / Cam" << template_state.source_camera_id;
    }
    if (!template_state.source_config_name.empty() &&
        template_state.source_config_name != template_state.source_arena_name) {
        label << " / " << template_state.source_config_name;
    }
    return label.str();
}

bool build_citrus_single_circle_template_state(
    const std::filesystem::path& config_path,
    const nlohmann::json& arena_json,
    const std::string& arena_name,
    const nlohmann::json& camera_calibration_json,
    CitrusSpatialTemplateState* template_state_out,
    std::string* error_out)
{
    if (template_state_out == nullptr) {
        if (error_out) {
            *error_out = "Null Citrus template destination.";
        }
        return false;
    }
    if (!arena_json.is_object() || !camera_calibration_json.is_object()) {
        if (error_out) {
            *error_out = "Citrus arena/camera calibration entries must be JSON objects.";
        }
        return false;
    }
    if (!json_string_equals_ignore_case(arena_json, "experimental_area_shape", "CIRCLE")) {
        if (error_out) {
            *error_out = "Citrus import v1 currently supports only experimental_area_shape = CIRCLE.";
        }
        return false;
    }

    CitrusSpatialTemplateState template_state;
    template_state.available = true;
    template_state.source_config_path = config_path.string();
    template_state.source_canvas_name = config_path.parent_path().filename().string();
    template_state.source_rig_name = config_path.parent_path().parent_path().filename().string();
    template_state.source_arena_name = arena_name;
    template_state.source_config_name = arena_json.value("config_name", arena_name);
    template_state.source_camera_id = camera_calibration_json.value("camera_id", "");
    template_state.source_dish_type_name = arena_json.value("selected_dish_type_name", "");
    const bool has_arena_center =
        parse_optional_json_number(camera_calibration_json, "arena_center_x_px", &template_state.arena_center_x_px) &&
        parse_optional_json_number(camera_calibration_json, "arena_center_y_px", &template_state.arena_center_y_px);
    const bool has_arena_size =
        parse_optional_json_number(camera_calibration_json, "arena_width_px", &template_state.arena_width_px) &&
        parse_optional_json_number(camera_calibration_json, "arena_height_px", &template_state.arena_height_px);
    template_state.has_arena_canvas_region =
        has_arena_center &&
        has_arena_size &&
        template_state.arena_width_px > 0.0 &&
        template_state.arena_height_px > 0.0;

    if (!parse_required_json_number(arena_json, "experimental_area_center_x_px",
                                    &template_state.experimental_area_center_x_px, error_out) ||
        !parse_required_json_number(arena_json, "experimental_area_center_y_px",
                                    &template_state.experimental_area_center_y_px, error_out) ||
        !parse_required_json_number(arena_json, "experimental_area_radius_px",
                                    &template_state.experimental_area_radius_px, error_out)) {
        return false;
    }
    if (arena_json.contains("experimental_area_radius_mm") &&
        arena_json.at("experimental_area_radius_mm").is_number()) {
        template_state.has_radius_mm = true;
        template_state.experimental_area_radius_mm =
            arena_json.at("experimental_area_radius_mm").get<double>();
    }
    if (camera_calibration_json.contains("pixels_per_mm_projector") &&
        camera_calibration_json.at("pixels_per_mm_projector").is_number()) {
        template_state.has_pixels_per_mm_projector = true;
        template_state.pixels_per_mm_projector =
            camera_calibration_json.at("pixels_per_mm_projector").get<double>();
    }

    std::string homography_error;
    load_homography_matrix_from_citrus_sidecar(
        config_path,
        template_state.source_config_name,
        template_state.source_camera_id,
        &template_state,
        &homography_error);

    *template_state_out = std::move(template_state);
    return true;
}

bool collect_citrus_single_circle_templates(
    const std::filesystem::path& config_path,
    const nlohmann::json& root,
    std::vector<CitrusSpatialTemplateState>* templates_out,
    std::vector<std::string>* available_camera_ids_out,
    std::string* error_out)
{
    if (templates_out == nullptr) {
        if (error_out) {
            *error_out = "Null Citrus template list destination.";
        }
        return false;
    }
    templates_out->clear();
    if (available_camera_ids_out != nullptr) {
        available_camera_ids_out->clear();
    }

    auto append_arena_templates =
        [&](const nlohmann::json& arena_json, const std::string& arena_name) -> bool {
            if (!arena_json.is_object() ||
                !arena_json.contains("camera_calibrations") ||
                !arena_json.at("camera_calibrations").is_array()) {
                return true;
            }
            for (const auto& camera_json : arena_json.at("camera_calibrations")) {
                if (!camera_json.is_object()) {
                    continue;
                }
                const std::string camera_id = camera_json.value("camera_id", "");
                if (!camera_id.empty() && available_camera_ids_out != nullptr) {
                    available_camera_ids_out->push_back(camera_id);
                }
                if (!json_string_equals_ignore_case(arena_json, "experimental_area_shape", "CIRCLE")) {
                    continue;
                }
                CitrusSpatialTemplateState template_state;
                std::string template_error;
                if (!build_citrus_single_circle_template_state(
                        config_path,
                        arena_json,
                        arena_name,
                        camera_json,
                        &template_state,
                        &template_error)) {
                    if (error_out) {
                        std::ostringstream oss;
                        oss << "Failed to import Citrus arena " << arena_name;
                        if (!camera_id.empty()) {
                            oss << " camera " << camera_id;
                        }
                        if (!template_error.empty()) {
                            oss << ": " << template_error;
                        }
                        *error_out = oss.str();
                    }
                    return false;
                }
                templates_out->push_back(std::move(template_state));
            }
            return true;
        };

    if (root.contains("arenas") && root.at("arenas").is_object()) {
        for (auto it = root.at("arenas").begin(); it != root.at("arenas").end(); ++it) {
            if (!append_arena_templates(it.value(), it.key())) {
                return false;
            }
        }
    } else {
        const std::string fallback_arena_name = root.value("config_name", config_path.stem().string());
        if (!append_arena_templates(root, fallback_arena_name)) {
            return false;
        }
    }

    if (available_camera_ids_out != nullptr) {
        std::sort(available_camera_ids_out->begin(), available_camera_ids_out->end());
        available_camera_ids_out->erase(
            std::unique(available_camera_ids_out->begin(), available_camera_ids_out->end()),
            available_camera_ids_out->end());
    }

    if (templates_out->empty()) {
        if (error_out) {
            std::ostringstream oss;
            oss << "No supported Citrus single-circle arena templates found in "
                << config_path.string();
            if (available_camera_ids_out != nullptr && !available_camera_ids_out->empty()) {
                oss << ". Available camera_ids: "
                    << join_strings(*available_camera_ids_out, ", ");
            }
            *error_out = oss.str();
        }
        return false;
    }
    return true;
}

int find_citrus_template_index_for_camera(
    const SpatialLayoutUiState& ui_state,
    const std::string& camera_serial)
{
    if (camera_serial.empty()) {
        return -1;
    }
    for (size_t idx = 0; idx < ui_state.citrus_canvas_templates.size(); ++idx) {
        if (ui_state.citrus_canvas_templates[idx].source_camera_id == camera_serial) {
            return static_cast<int>(idx);
        }
    }
    return -1;
}

bool apply_citrus_template_to_spatial_layout(
    SpatialLayoutUiState* ui_state,
    const CitrusSpatialTemplateState& template_state,
    std::string* status_out)
{
    if (ui_state == nullptr || !template_state.available) {
        return false;
    }

    ui_state->citrus_template = template_state;
    ui_state->has_citrus_projected_circle = false;
    ui_state->citrus_projected_circle_geometry = RuntimeGeometry{};

    LayoutGeometry imported_outer;
    imported_outer.type = LayoutGeometryType::kCircle;
    imported_outer.circle.cx = template_state.experimental_area_center_x_px;
    imported_outer.circle.cy = template_state.experimental_area_center_y_px;
    imported_outer.circle.r = template_state.experimental_area_radius_px;

    ui_state->layout_artifact.artifact_id = "preview.citrus_import";
    ui_state->layout_artifact.created_utc = get_current_utc_timestamp();
    ui_state->layout_artifact.calibration_ref.artifact_id = ui_state->layout_artifact.artifact_id;
    ui_state->layout_artifact.calibration_ref.artifact_schema_id = orange::spatial::kArenaLayoutArtifactSchemaId;
    ui_state->layout_artifact.calibration_ref.artifact_schema_version = orange::spatial::kArenaLayoutArtifactSchemaVersion;
    ui_state->layout_artifact.calibration_ref.fingerprint = "preview-only";
    ui_state->layout_artifact.layout_id =
        "citrus_" + sanitize_artifact_component(template_state.source_canvas_name) + "_" +
        sanitize_artifact_component(template_state.source_config_name);
    ui_state->layout_artifact.layout.coordinate_space = CoordinateSpace::kLayoutUnits;
    ui_state->layout_artifact.layout.outer_geometry = imported_outer;
    reset_to_single_experimental_area_zone(ui_state);
    ui_state->layout_artifact.context.canvas_id = template_state.source_canvas_name;
    ui_state->layout_artifact.context.dish_design_id = template_state.source_dish_type_name;
    ui_state->layout_artifact.provenance.source = ArenaLayoutProvenanceSource::kImportedTemplate;
    ui_state->layout_artifact.provenance.ordering_rule = "single_circle_imported_from_citrus";
    ui_state->layout_artifact.provenance.notes =
        "Imported from Citrus config " + template_state.source_config_path +
        " for camera " + template_state.source_camera_id + ".";
    ui_state->selected_zone_index = 0;

    ui_state->registration.layout_coordinate_space = ui_state->layout_artifact.layout.coordinate_space;
    if (ui_state->has_capture) {
        reset_registration_from_frame(ui_state);
    }

    std::ostringstream status;
    status << "Selected Citrus circle from " << template_state.source_canvas_name
           << " / " << template_state.source_config_name
           << " for camera " << template_state.source_camera_id;
    if (template_state.has_canvas_to_camera_homography) {
        double preview_rms = 0.0;
        std::string preview_error;
        if (update_citrus_projected_circle_preview(ui_state, &preview_rms, &preview_error)) {
            status << ". Homography loaded; projected-circle seed RMS " << std::fixed << std::setprecision(2)
                   << preview_rms << " px.";
        } else {
            status << ". Homography loaded but preview seed failed (" << preview_error << ").";
        }
    } else {
        status << ". No homography sidecar found.";
    }

    if (status_out) {
        *status_out = status.str();
    }
    return true;
}

bool select_citrus_template_by_index(
    SpatialLayoutUiState* ui_state,
    int index,
    std::string* status_out,
    std::string* error_out)
{
    if (ui_state == nullptr) {
        if (error_out) {
            *error_out = "Spatial layout UI state is null.";
        }
        return false;
    }
    if (index < 0 || index >= static_cast<int>(ui_state->citrus_canvas_templates.size())) {
        if (error_out) {
            *error_out = "Citrus canvas template index is out of range.";
        }
        return false;
    }
    ui_state->citrus_canvas_template_index = index;
    if (!apply_citrus_template_to_spatial_layout(
            ui_state,
            ui_state->citrus_canvas_templates[static_cast<size_t>(index)],
            status_out)) {
        if (error_out) {
            *error_out = "Failed to apply Citrus canvas template.";
        }
        return false;
    }
    return true;
}

bool import_citrus_canvas_templates(SpatialLayoutUiState* ui_state,
                                    const CameraParams& selected_camera,
                                    const std::filesystem::path& config_path,
                                    std::string* status_out,
                                    std::string* error_out)
{
    if (ui_state == nullptr) {
        if (error_out) {
            *error_out = "Spatial layout UI state is null.";
        }
        return false;
    }

    nlohmann::json root;
    if (!read_json_file(config_path, &root, error_out)) {
        return false;
    }
    if (!root.is_object()) {
        if (error_out) {
            *error_out = "Citrus canvas config root must be a JSON object.";
        }
        return false;
    }

    std::vector<CitrusSpatialTemplateState> templates;
    std::vector<std::string> available_camera_ids;
    if (!collect_citrus_single_circle_templates(
            config_path,
            root,
            &templates,
            &available_camera_ids,
            error_out)) {
        return false;
    }

    ui_state->citrus_canvas_templates = std::move(templates);
    ui_state->citrus_canvas_config_path = config_path.string();
    ui_state->citrus_canvas_template_index = -1;

    int selected_index = find_citrus_template_index_for_camera(
        *ui_state,
        selected_camera.camera_serial);
    if (selected_index < 0 && !ui_state->citrus_canvas_templates.empty()) {
        selected_index = 0;
    }

    std::string selected_status;
    std::string selected_error;
    if (!select_citrus_template_by_index(
            ui_state,
            selected_index,
            &selected_status,
            &selected_error)) {
        if (error_out) {
            *error_out = selected_error;
        }
        return false;
    }

    std::ostringstream status;
    status << "Loaded Citrus canvas " << config_path.parent_path().filename().string()
           << " with " << ui_state->citrus_canvas_templates.size()
           << " supported single-circle arena template(s). "
           << selected_status;
    if (ui_state->citrus_template.source_camera_id != selected_camera.camera_serial) {
        status << " No template matched selected Orange camera "
               << selected_camera.camera_serial << ".";
        if (!available_camera_ids.empty()) {
            status << " Available camera_ids: "
                   << join_strings(available_camera_ids, ", ") << ".";
        }
    }
    if (status_out) {
        *status_out = status.str();
    }
    return true;
}

bool detect_experimental_area_circle_from_capture(
    SpatialLayoutUiState* ui_state,
    std::string* error_out)
{
    if (ui_state == nullptr) {
        if (error_out) {
            *error_out = "Experimental-area detection requires non-null UI state.";
        }
        return false;
    }
    if (!ui_state->has_capture || ui_state->captured_texture_width <= 0 || ui_state->captured_texture_height <= 0 ||
        ui_state->captured_rgba.empty()) {
        if (error_out) {
            *error_out = "Capture a frame before running experimental-area detection.";
        }
        return false;
    }

    cv::Mat rgba(ui_state->captured_texture_height,
                 ui_state->captured_texture_width,
                 CV_8UC4,
                 ui_state->captured_rgba.data());
    cv::Mat gray;
    cv::cvtColor(rgba, gray, cv::COLOR_RGBA2GRAY);

    ui_state->hough_dp = std::clamp(ui_state->hough_dp, 1.0, 3.0);
    ui_state->hough_min_dist_fraction =
        std::clamp(ui_state->hough_min_dist_fraction, 0.01, 2.0);
    ui_state->hough_param1 = std::clamp(ui_state->hough_param1, 1.0, 500.0);
    ui_state->hough_param2 = std::clamp(ui_state->hough_param2, 1.0, 500.0);
    ui_state->hough_min_radius_fraction =
        std::clamp(ui_state->hough_min_radius_fraction, 0.001, 1.0);
    ui_state->hough_max_radius_fraction =
        std::clamp(ui_state->hough_max_radius_fraction, 0.001, 1.5);
    if (ui_state->hough_max_radius_fraction < ui_state->hough_min_radius_fraction) {
        std::swap(ui_state->hough_max_radius_fraction, ui_state->hough_min_radius_fraction);
    }
    ui_state->hough_max_detection_dimension_px =
        std::clamp(ui_state->hough_max_detection_dimension_px, 256, 8192);
    ui_state->hough_median_blur_ksize =
        std::clamp(ui_state->hough_median_blur_ksize, 1, 31);
    if ((ui_state->hough_median_blur_ksize % 2) == 0) {
        ++ui_state->hough_median_blur_ksize;
    }
    double detection_scale = 1.0;
    cv::Mat detection_gray = gray;
    const int max_dim = std::max(gray.cols, gray.rows);
    if (max_dim > ui_state->hough_max_detection_dimension_px) {
        detection_scale =
            static_cast<double>(ui_state->hough_max_detection_dimension_px) /
            static_cast<double>(max_dim);
        cv::resize(gray, detection_gray, cv::Size(), detection_scale, detection_scale, cv::INTER_AREA);
    }

    cv::Mat blurred;
    if (ui_state->hough_median_blur_ksize > 1) {
        cv::medianBlur(detection_gray, blurred, ui_state->hough_median_blur_ksize);
    } else {
        blurred = detection_gray;
    }

    const double min_dim = static_cast<double>(std::min(blurred.cols, blurred.rows));
    if (min_dim < 32.0) {
        if (error_out) {
            *error_out = "Experimental-area detection requires a larger captured frame.";
        }
        return false;
    }

    const int min_radius = std::max(
        1,
        static_cast<int>(std::round(min_dim * ui_state->hough_min_radius_fraction)));
    const int max_radius = std::max(
        min_radius + 1,
        static_cast<int>(std::round(min_dim * ui_state->hough_max_radius_fraction)));
    const double min_dist =
        std::max(1.0, min_dim * ui_state->hough_min_dist_fraction);

    std::vector<cv::Vec3f> circles;
    try {
        cv::HoughCircles(
            blurred,
            circles,
            cv::HOUGH_GRADIENT,
            ui_state->hough_dp,
            min_dist,
            ui_state->hough_param1,
            ui_state->hough_param2,
            min_radius,
            max_radius);

        if (circles.empty() && ui_state->hough_fallback_enabled) {
            cv::HoughCircles(
                blurred,
                circles,
                cv::HOUGH_GRADIENT,
                std::max(1.0, ui_state->hough_dp * 0.96),
                min_dist,
                std::max(1.0, ui_state->hough_param1 * 0.75),
                std::max(1.0, ui_state->hough_param2 * 0.73),
                min_radius,
                max_radius);
        }
    } catch (const cv::Exception& ex) {
        if (error_out) {
            *error_out = std::string("Hough circle detection failed: ") + ex.what();
        }
        return false;
    }

    if (circles.empty()) {
        if (error_out) {
            *error_out = "No experimental-area circle was detected in the captured frame.";
        }
        return false;
    }

    const Point2d image_center = make_point(blurred.cols * 0.5, blurred.rows * 0.5);
    double best_score = std::numeric_limits<double>::lowest();
    cv::Vec3f best_circle = circles.front();
    for (const cv::Vec3f& circle : circles) {
        const double dx = static_cast<double>(circle[0]) - image_center.x;
        const double dy = static_cast<double>(circle[1]) - image_center.y;
        const double dist = std::sqrt(dx * dx + dy * dy);
        const double score = static_cast<double>(circle[2]) - 0.35 * dist;
        if (score > best_score) {
            best_score = score;
            best_circle = circle;
        }
    }

    const double inv_scale = 1.0 / detection_scale;
    ui_state->detected_experimental_area_geometry =
        runtime_circle(
            static_cast<double>(best_circle[0]) * inv_scale,
            static_cast<double>(best_circle[1]) * inv_scale,
            std::max(
                1.0,
                static_cast<double>(best_circle[2]) * inv_scale +
                    ui_state->hough_radius_adjustment_px));
    ui_state->has_detected_experimental_area_circle = true;
    ui_state->detection_error.clear();

    std::ostringstream status;
    status << "Detected experimental-area circle at ("
           << std::lround(ui_state->detected_experimental_area_geometry.circle.cx) << ", "
           << std::lround(ui_state->detected_experimental_area_geometry.circle.cy) << ")"
           << " r=" << std::lround(ui_state->detected_experimental_area_geometry.circle.r);
    if (circles.size() > 1) {
        status << " from " << circles.size() << " Hough candidates";
    }
    status << " using dp=" << ui_state->hough_dp
           << " param1=" << ui_state->hough_param1
           << " param2=" << ui_state->hough_param2
           << " radius=[" << std::lround(static_cast<double>(min_radius) * inv_scale)
           << "," << std::lround(static_cast<double>(max_radius) * inv_scale) << "]";
    ui_state->detection_status = status.str();
    return true;
}

bool seed_registration_from_detected_experimental_area_circle(
    SpatialLayoutUiState* ui_state,
    std::string* error_out)
{
    if (ui_state == nullptr) {
        if (error_out) {
            *error_out = "Registration seeding requires non-null UI state.";
        }
        return false;
    }
    if (!ui_state->has_detected_experimental_area_circle) {
        if (error_out) {
            *error_out = "Run experimental-area detection before seeding registration.";
        }
        return false;
    }

    const LayoutGeometry& canonical_outer = ui_state->layout_artifact.layout.outer_geometry;
    if (canonical_outer.type != LayoutGeometryType::kCircle || canonical_outer.circle.r <= 0.0) {
        if (error_out) {
            *error_out =
                "Experimental-area circle seeding currently requires a circular canonical experimental area.";
        }
        return false;
    }

    ui_state->registration.type = RegistrationType::kSimilarity;
    ui_state->registration.source = RegistrationSource::kDetectedFit;
    ui_state->registration.fit_point_count = 3;
    ui_state->registration.residual_px = 0.0;
    ui_state->registration.has_orientation_status = true;
    ui_state->registration.orientation_status = OrientationStatus::kAmbiguous;

    const double detected_radius = ui_state->detected_experimental_area_geometry.circle.r;
    const double scale = std::max(1e-6, detected_radius / canonical_outer.circle.r);
    const Point2d desired_center = make_point(
        ui_state->detected_experimental_area_geometry.circle.cx,
        ui_state->detected_experimental_area_geometry.circle.cy);
    const double theta = ui_state->registration_rotation_deg_clockwise * kPi / 180.0;
    const double rotated_center_x =
        scale * (std::cos(theta) * canonical_outer.circle.cx - std::sin(theta) * canonical_outer.circle.cy);
    const double rotated_center_y =
        scale * (std::sin(theta) * canonical_outer.circle.cx + std::cos(theta) * canonical_outer.circle.cy);

    ui_state->registration.type = RegistrationType::kSimilarity;
    ui_state->registration_scale = scale;
    ui_state->registration_tx_px = desired_center.x - rotated_center_x;
    ui_state->registration_ty_px = desired_center.y - rotated_center_y;
    return true;
}

double normalize_angle_deg(double angle_deg)
{
    while (angle_deg <= -180.0) {
        angle_deg += 360.0;
    }
    while (angle_deg > 180.0) {
        angle_deg -= 360.0;
    }
    return angle_deg;
}

double effective_registration_scale(const SpatialLayoutUiState& ui_state)
{
    if (ui_state.registration.type == RegistrationType::kIdentity ||
        ui_state.registration.type == RegistrationType::kTranslation) {
        return 1.0;
    }
    return std::max(1e-6, ui_state.registration_scale);
}

double effective_registration_rotation_deg(const SpatialLayoutUiState& ui_state)
{
    return ui_state.registration.type == RegistrationType::kSimilarity
        ? ui_state.registration_rotation_deg_clockwise
        : 0.0;
}

Point2d runtime_geometry_center(const RuntimeGeometry& geometry)
{
    if (geometry.type == RuntimeGeometryType::kCircle) {
        return make_point(geometry.circle.cx, geometry.circle.cy);
    }
    return make_point(geometry.oriented_rectangle.cx, geometry.oriented_rectangle.cy);
}

Point2d point_from_local_offset(const Point2d& center, double local_x, double local_y, double rotation_deg_clockwise)
{
    const double theta = rotation_deg_clockwise * kPi / 180.0;
    const double cos_theta = std::cos(theta);
    const double sin_theta = std::sin(theta);
    return make_point(
        center.x + cos_theta * local_x - sin_theta * local_y,
        center.y + sin_theta * local_x + cos_theta * local_y);
}

Point2d runtime_geometry_right_handle(const RuntimeGeometry& geometry)
{
    const Point2d center = runtime_geometry_center(geometry);
    if (geometry.type == RuntimeGeometryType::kCircle) {
        return make_point(geometry.circle.cx + geometry.circle.r, geometry.circle.cy);
    }
    return point_from_local_offset(
        center,
        geometry.oriented_rectangle.width * 0.5,
        0.0,
        geometry.oriented_rectangle.rotation_deg_clockwise);
}

Point2d runtime_geometry_bottom_handle(const RuntimeGeometry& geometry)
{
    const Point2d center = runtime_geometry_center(geometry);
    if (geometry.type == RuntimeGeometryType::kCircle) {
        return make_point(geometry.circle.cx, geometry.circle.cy + geometry.circle.r);
    }
    return point_from_local_offset(
        center,
        0.0,
        geometry.oriented_rectangle.height * 0.5,
        geometry.oriented_rectangle.rotation_deg_clockwise);
}

Point2d runtime_geometry_rotation_handle(const RuntimeGeometry& geometry)
{
    const Point2d center = runtime_geometry_center(geometry);
    Point2d top_boundary = center;
    if (geometry.type == RuntimeGeometryType::kCircle) {
        top_boundary = make_point(geometry.circle.cx, geometry.circle.cy - geometry.circle.r);
    } else {
        top_boundary = point_from_local_offset(
            center,
            0.0,
            -geometry.oriented_rectangle.height * 0.5,
            geometry.oriented_rectangle.rotation_deg_clockwise);
    }

    const ImVec2 center_px = ImPlot::PlotToPixels(ImPlotPoint(center.x, center.y));
    const ImVec2 top_px = ImPlot::PlotToPixels(ImPlotPoint(top_boundary.x, top_boundary.y));
    ImVec2 direction(top_px.x - center_px.x, top_px.y - center_px.y);
    const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y);
    if (length < 1e-3f) {
        direction = ImVec2(0.0f, -1.0f);
    } else {
        direction.x /= length;
        direction.y /= length;
    }
    const ImVec2 handle_px(
        center_px.x + direction.x * (length + 28.0f),
        center_px.y + direction.y * (length + 28.0f));
    const ImPlotPoint handle_plot = ImPlot::PixelsToPlot(handle_px);
    return make_point(handle_plot.x, handle_plot.y);
}

Point2d camera_point_to_layout_point(const SpatialLayoutUiState& ui_state, const Point2d& camera_point)
{
    return transform_point(
        ui_state.registration.camera_to_layout_matrix,
        camera_point.x,
        camera_point.y);
}

void set_registration_transform(SpatialLayoutUiState* ui_state,
                                RegistrationType type,
                                const Point2d& desired_outer_center,
                                double scale,
                                double rotation_deg_clockwise,
                                RegistrationSource source)
{
    if (ui_state == nullptr) {
        return;
    }

    if (type == RegistrationType::kIdentity) {
        scale = 1.0;
        rotation_deg_clockwise = 0.0;
    } else if (type == RegistrationType::kTranslation) {
        scale = 1.0;
        rotation_deg_clockwise = 0.0;
    } else {
        scale = std::max(1e-6, scale);
    }

    const Point2d canonical_center = layout_geometry_center(ui_state->layout_artifact.layout.outer_geometry);
    const double theta = rotation_deg_clockwise * kPi / 180.0;
    const double rotated_center_x =
        scale * (std::cos(theta) * canonical_center.x - std::sin(theta) * canonical_center.y);
    const double rotated_center_y =
        scale * (std::sin(theta) * canonical_center.x + std::cos(theta) * canonical_center.y);

    ui_state->registration.type = type;
    ui_state->registration.source = source;
    ui_state->registration_tx_px = desired_outer_center.x - rotated_center_x;
    ui_state->registration_ty_px = desired_outer_center.y - rotated_center_y;
    ui_state->registration_scale = scale;
    ui_state->registration_rotation_deg_clockwise = normalize_angle_deg(rotation_deg_clockwise);
}

bool update_drag_point(int id,
                       const Point2d& initial_point,
                       const ImVec4& color,
                       float radius_px,
                       Point2d* updated_point)
{
    if (updated_point == nullptr) {
        return false;
    }

    double x = initial_point.x;
    double y = initial_point.y;
    const bool changed = ImPlot::DragPoint(
        id,
        &x,
        &y,
        color,
        radius_px,
        ImPlotDragToolFlags_NoFit);
    *updated_point = make_point(x, y);
    return changed;
}

bool handle_registration_canvas_edit(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr || !ui_state->dish_mask_runtime.has_geometry) {
        return false;
    }

    const RuntimeGeometry& outer = ui_state->dish_mask_runtime.geometry.outer_geometry;
    const LayoutGeometry& canonical_outer = ui_state->layout_artifact.layout.outer_geometry;
    const Point2d current_center = runtime_geometry_center(outer);
    const double current_scale = effective_registration_scale(*ui_state);
    const double current_rotation_deg = effective_registration_rotation_deg(*ui_state);

    bool changed = false;
    Point2d center_handle{};
    if (update_drag_point(4100, current_center, ImVec4(0.25f, 0.80f, 1.0f, 1.0f), 6.0f, &center_handle)) {
        const RegistrationType new_type =
            ui_state->registration.type == RegistrationType::kIdentity
                ? RegistrationType::kTranslation
                : ui_state->registration.type;
        set_registration_transform(
            ui_state,
            new_type,
            center_handle,
            current_scale,
            current_rotation_deg,
            RegistrationSource::kManualFit);
        ui_state->registration.fit_point_count = 0;
        ui_state->registration.residual_px = 0.0;
        changed = true;
    }

    if (ui_state->registration.type != RegistrationType::kSimilarity) {
        return changed;
    }

    Point2d scale_handle{};
    if (update_drag_point(
            4101,
            runtime_geometry_right_handle(outer),
            ImVec4(1.0f, 0.82f, 0.18f, 1.0f),
            5.0f,
            &scale_handle)) {
        double new_scale = current_scale;
        if (canonical_outer.type == LayoutGeometryType::kCircle && canonical_outer.circle.r > 0.0) {
            const double dx = scale_handle.x - current_center.x;
            const double dy = scale_handle.y - current_center.y;
            new_scale = std::max(1e-6, std::sqrt(dx * dx + dy * dy) / canonical_outer.circle.r);
        } else if (canonical_outer.type == LayoutGeometryType::kRectangle && canonical_outer.rectangle.width > 0.0) {
            const Point2d layout_center = layout_geometry_center(canonical_outer);
            const Point2d mouse_layout = camera_point_to_layout_point(*ui_state, scale_handle);
            new_scale = std::max(
                1e-6,
                (2.0 * std::abs(mouse_layout.x - layout_center.x)) / canonical_outer.rectangle.width);
        }
        set_registration_transform(
            ui_state,
            RegistrationType::kSimilarity,
            current_center,
            new_scale,
            current_rotation_deg,
            RegistrationSource::kManualFit);
        ui_state->registration.fit_point_count = 0;
        ui_state->registration.residual_px = 0.0;
        changed = true;
    }

    Point2d rotation_handle{};
    if (update_drag_point(
            4102,
            runtime_geometry_rotation_handle(outer),
            ImVec4(1.0f, 0.35f, 0.80f, 1.0f),
            5.0f,
            &rotation_handle)) {
        const double dx = rotation_handle.x - current_center.x;
        const double dy = rotation_handle.y - current_center.y;
        if ((dx * dx + dy * dy) > 1e-8) {
            const double angle_deg = std::atan2(dy, dx) * 180.0 / kPi;
            const double new_rotation_deg = normalize_angle_deg(angle_deg + 90.0);
            set_registration_transform(
                ui_state,
                RegistrationType::kSimilarity,
                current_center,
                current_scale,
                new_rotation_deg,
                RegistrationSource::kManualFit);
            ui_state->registration.fit_point_count = 0;
            ui_state->registration.residual_px = 0.0;
            ui_state->registration.has_orientation_status = true;
            ui_state->registration.orientation_status = OrientationStatus::kManualConfirmed;
            changed = true;
        }
    }

    return changed;
}

bool handle_selected_zone_canvas_edit(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr ||
        !ui_state->registration.has_camera_to_layout_matrix ||
        ui_state->selected_zone_index < 0 ||
        ui_state->selected_zone_index >= static_cast<int>(ui_state->layout_artifact.layout.zones.size()) ||
        ui_state->selected_zone_index >= static_cast<int>(ui_state->arena_layout_runtime.zones.size())) {
        return false;
    }

    ArenaLayoutZone& zone =
        ui_state->layout_artifact.layout.zones[static_cast<size_t>(ui_state->selected_zone_index)];
    const ResolvedZoneOverlay& overlay =
        ui_state->arena_layout_runtime.zones[static_cast<size_t>(ui_state->selected_zone_index)];
    const Point2d current_center = runtime_geometry_center(overlay.geometry);
    bool changed = false;

    Point2d center_handle{};
    if (update_drag_point(4200, current_center, ImVec4(0.15f, 0.95f, 0.55f, 1.0f), 6.0f, &center_handle)) {
        const Point2d layout_center = camera_point_to_layout_point(*ui_state, center_handle);
        if (zone.geometry.type == LayoutGeometryType::kCircle) {
            zone.geometry.circle.cx = layout_center.x;
            zone.geometry.circle.cy = layout_center.y;
        } else {
            zone.geometry.rectangle.x = layout_center.x - zone.geometry.rectangle.width * 0.5;
            zone.geometry.rectangle.y = layout_center.y - zone.geometry.rectangle.height * 0.5;
        }
        changed = true;
    }

    const double current_scale = std::max(1e-6, effective_registration_scale(*ui_state));
    if (zone.geometry.type == LayoutGeometryType::kCircle) {
        Point2d radius_handle{};
        if (update_drag_point(
                4201,
                runtime_geometry_right_handle(overlay.geometry),
                ImVec4(0.95f, 0.95f, 0.25f, 1.0f),
                5.0f,
                &radius_handle)) {
            const double dx = radius_handle.x - current_center.x;
            const double dy = radius_handle.y - current_center.y;
            zone.geometry.circle.r = std::max(0.0, std::sqrt(dx * dx + dy * dy) / current_scale);
            changed = true;
        }
        return changed;
    }

    const Point2d layout_center = layout_geometry_center(zone.geometry);
    Point2d width_handle{};
    if (update_drag_point(
            4202,
            runtime_geometry_right_handle(overlay.geometry),
            ImVec4(0.95f, 0.95f, 0.25f, 1.0f),
            5.0f,
            &width_handle)) {
        const Point2d layout_handle = camera_point_to_layout_point(*ui_state, width_handle);
        zone.geometry.rectangle.width = std::max(0.0, 2.0 * std::abs(layout_handle.x - layout_center.x));
        zone.geometry.rectangle.x = layout_center.x - zone.geometry.rectangle.width * 0.5;
        changed = true;
    }

    Point2d height_handle{};
    if (update_drag_point(
            4203,
            runtime_geometry_bottom_handle(overlay.geometry),
            ImVec4(0.95f, 0.60f, 0.25f, 1.0f),
            5.0f,
            &height_handle)) {
        const Point2d layout_handle = camera_point_to_layout_point(*ui_state, height_handle);
        zone.geometry.rectangle.height = std::max(0.0, 2.0 * std::abs(layout_handle.y - layout_center.y));
        zone.geometry.rectangle.y = layout_center.y - zone.geometry.rectangle.height * 0.5;
        changed = true;
    }

    return changed;
}

bool capture_single_camera_frame(
    SpatialLayoutUiState* ui_state,
    CameraEmergent* ecams,
    CameraParams* cameras_params,
    std::string* error_out)
{
    if (ui_state == nullptr || ecams == nullptr || cameras_params == nullptr) {
        if (error_out) {
            *error_out = "Capture frame received invalid state or camera pointers.";
        }
        return false;
    }

    const int selected_camera = ui_state->selected_camera;
    CameraEmergent* ecam = &ecams[selected_camera];
    CameraParams* camera_params = &cameras_params[selected_camera];

    int dropped_frames = 0;
    int width = 0;
    int height = 0;
    std::string capture_error;
    if (!orange::preview::capture_single_frame_rgba(
            &ecam->camera,
            camera_params,
            kSpatialCaptureBufferCount,
            1000,
            &ui_state->captured_rgba,
            &width,
            &height,
            &dropped_frames,
            &capture_error)) {
        if (error_out) {
            *error_out = capture_error;
        }
        return false;
    }

    orange::preview::update_rgba_texture(
        &ui_state->captured_texture,
        &ui_state->captured_texture_width,
        &ui_state->captured_texture_height,
        ui_state->captured_rgba,
        width,
        height);
    clear_detected_experimental_area_circle(ui_state);
    ui_state->has_capture = true;
    ui_state->captured_camera_serial = camera_params->camera_serial;
    ui_state->captured_source_array_role = "images_full";
    ui_state->captured_capture_mode = "single_camera_direct_still";
    ui_state->captured_capture_group_id.clear();
    ui_state->captured_source_frame_count = 1;
    ui_state->captured_first_local_frame_id = 0;
    ui_state->captured_last_local_frame_id = 0;
    ui_state->captured_first_camera_frame_id = 0;
    ui_state->captured_last_camera_frame_id = 0;
    ui_state->preview_error.clear();

    std::ostringstream status;
    status << "Captured " << width << "x" << height << " from " << camera_params->camera_serial;
    if (dropped_frames > 0) {
        status << " (dropped " << dropped_frames << " buffered frames)";
    }
    ui_state->preview_status = status.str();
    return true;
}

bool capture_live_stream_preview_texture(
    SpatialLayoutUiState* ui_state,
    const CameraParams& camera_params,
    const CameraEachSelect& camera_select,
    GLuint preview_texture,
    std::string* error_out)
{
    if (ui_state == nullptr) {
        if (error_out) {
            *error_out = "Live stream snapshot received a null UI state.";
        }
        return false;
    }
    if (!camera_select.stream_on || preview_texture == 0) {
        if (error_out) {
            *error_out = "Start streaming and wait for a live preview frame before taking a live stream snapshot.";
        }
        return false;
    }

    const int width = std::max(1, static_cast<int>(camera_params.width) / std::max(1, camera_select.downsample));
    const int height = std::max(1, static_cast<int>(camera_params.height) / std::max(1, camera_select.downsample));
    std::vector<unsigned char> rgba(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);

    GLint previous_texture = 0;
    GLint previous_pack_alignment = 4;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);
    glGetIntegerv(GL_PACK_ALIGNMENT, &previous_pack_alignment);
    glBindTexture(GL_TEXTURE_2D, preview_texture);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    const GLenum gl_status = glGetError();
    glPixelStorei(GL_PACK_ALIGNMENT, previous_pack_alignment);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous_texture));
    if (gl_status != GL_NO_ERROR) {
        if (error_out) {
            std::ostringstream oss;
            oss << "Failed to read live preview texture for " << camera_params.camera_serial
                << " (GL error 0x" << std::hex << gl_status << ").";
            *error_out = oss.str();
        }
        return false;
    }

    ui_state->captured_rgba = std::move(rgba);
    ui_state->captured_texture_width = width;
    ui_state->captured_texture_height = height;
    ui_state->captured_camera_serial = camera_params.camera_serial;
    ui_state->captured_source_array_role =
        camera_select.downsample > 1 ? "images_ds" : "images_full";
    ui_state->captured_capture_mode = "live_stream_preview_snapshot";
    ui_state->captured_capture_group_id.clear();
    ui_state->captured_source_frame_count = 1;
    ui_state->captured_first_local_frame_id = 0;
    ui_state->captured_last_local_frame_id = 0;
    ui_state->captured_first_camera_frame_id = 0;
    ui_state->captured_last_camera_frame_id = 0;
    ui_state->has_capture = true;
    ui_state->preview_error.clear();
    clear_detected_experimental_area_circle(ui_state);

    std::string texture_error;
    if (!orange::preview::update_rgba_texture(
            &ui_state->captured_texture,
            &ui_state->captured_texture_width,
            &ui_state->captured_texture_height,
            ui_state->captured_rgba,
            width,
            height,
            &texture_error)) {
        if (error_out) {
            *error_out = texture_error;
        }
        return false;
    }

    reset_registration_from_frame(ui_state);

    std::ostringstream status;
    status << "Captured live stream preview " << width << "x" << height
           << " from " << camera_params.camera_serial
           << " as " << ui_state->captured_source_array_role;
    if (camera_select.downsample > 1) {
        status << " (display downsample " << camera_select.downsample << "x)";
    }
    ui_state->preview_status = status.str();
    return true;
}

bool apply_full_resolution_stream_snapshot(
    SpatialLayoutUiState* ui_state,
    const SpatialSnapshotResult& result,
    std::string* error_out)
{
    if (ui_state == nullptr) {
        if (error_out) {
            *error_out = "Full-resolution stream snapshot received a null UI state.";
        }
        return false;
    }
    if (!result.ok) {
        if (error_out) {
            *error_out = result.error.empty()
                             ? "Full-resolution stream snapshot failed."
                             : result.error;
        }
        return false;
    }
    if (result.width <= 0 || result.height <= 0 || result.rgba.empty()) {
        if (error_out) {
            *error_out = "Full-resolution stream snapshot returned an empty image.";
        }
        return false;
    }

    ui_state->captured_rgba = result.rgba;
    ui_state->captured_camera_serial = result.camera_serial;
    ui_state->captured_source_array_role = "images_full";
    ui_state->captured_capture_mode =
        result.capture_mode.empty() ? "full_resolution_stream_snapshot" : result.capture_mode;
    ui_state->captured_capture_group_id.clear();
    ui_state->captured_source_frame_count =
        std::max<uint32_t>(1u, result.completed_frame_count);
    ui_state->captured_first_local_frame_id = result.first_local_frame_id;
    ui_state->captured_last_local_frame_id = result.last_local_frame_id;
    ui_state->captured_first_camera_frame_id = result.first_camera_frame_id;
    ui_state->captured_last_camera_frame_id = result.last_camera_frame_id;
    ui_state->has_capture = true;
    ui_state->pending_full_res_snapshot_request_id = 0;
    ui_state->pending_full_res_snapshot_camera_serial.clear();
    ui_state->pending_full_res_snapshot_target_frame_count = 1;
    ui_state->preview_error.clear();
    clear_detected_experimental_area_circle(ui_state);

    std::string texture_error;
    if (!orange::preview::update_rgba_texture(
            &ui_state->captured_texture,
            &ui_state->captured_texture_width,
            &ui_state->captured_texture_height,
            ui_state->captured_rgba,
            result.width,
            result.height,
            &texture_error)) {
        if (error_out) {
            *error_out = texture_error;
        }
        return false;
    }

    reset_registration_from_frame(ui_state);

    std::ostringstream status;
    status << "Captured full-resolution stream snapshot "
           << result.width << "x" << result.height
           << " from " << result.camera_serial
           << " frame=" << result.local_frame_id
           << " camera_frame=" << result.camera_frame_id;
    if (ui_state->captured_source_frame_count > 1) {
        status << " averaged_frames=" << ui_state->captured_source_frame_count
               << " local_frame_range=" << ui_state->captured_first_local_frame_id
               << "-" << ui_state->captured_last_local_frame_id
               << " camera_frame_range=" << ui_state->captured_first_camera_frame_id
               << "-" << ui_state->captured_last_camera_frame_id;
    }
    ui_state->preview_status = status.str();
    return true;
}

std::string metadata_or_unknown(const std::string& value)
{
    return value.empty() ? std::string("unknown") : value;
}

void populate_calibration_domain_metadata_from_runtime(
    SpatialLayoutCalibrationImageSetMetadata* metadata,
    const SpatialLayoutUiState& ui_state)
{
    if (metadata == nullptr || !ui_state.dish_mask_runtime.has_geometry) {
        return;
    }

    const DishMaskGeometry& geometry = ui_state.dish_mask_runtime.geometry;
    metadata->has_calibration_domain = true;
    metadata->calibration_domain_source =
        std::string("orange_spatial_layout_runtime:") +
        orange::spatial::observation_source_to_string(ui_state.dish_mask_runtime.source);
    metadata->calibration_domain_coordinate_space =
        orange::spatial::coordinate_space_to_string(geometry.coordinate_space);
    metadata->calibration_domain_edge_margin_px = geometry.edge_margin_px;

    const RuntimeGeometry& outer = geometry.outer_geometry;
    const RuntimeGeometry& valid = geometry.valid_geometry;
    if (outer.type == RuntimeGeometryType::kCircle && outer.circle.r > 0.0) {
        metadata->calibration_domain_shape = "circle";
        metadata->calibration_domain_center_x_px = outer.circle.cx;
        metadata->calibration_domain_center_y_px = outer.circle.cy;
        metadata->calibration_domain_radius_px = outer.circle.r;
        if (valid.type == RuntimeGeometryType::kCircle && valid.circle.r > 0.0) {
            metadata->has_calibration_domain_valid_circle = true;
            metadata->calibration_domain_valid_center_x_px = valid.circle.cx;
            metadata->calibration_domain_valid_center_y_px = valid.circle.cy;
            metadata->calibration_domain_valid_radius_px = valid.circle.r;
        }
        return;
    }

    if (outer.type == RuntimeGeometryType::kOrientedRectangle &&
        outer.oriented_rectangle.width > 0.0 &&
        outer.oriented_rectangle.height > 0.0) {
        metadata->calibration_domain_shape = "oriented_rectangle";
        metadata->calibration_domain_center_x_px = outer.oriented_rectangle.cx;
        metadata->calibration_domain_center_y_px = outer.oriented_rectangle.cy;
        metadata->calibration_domain_width_px = outer.oriented_rectangle.width;
        metadata->calibration_domain_height_px = outer.oriented_rectangle.height;
        metadata->calibration_domain_rotation_deg_clockwise =
            outer.oriented_rectangle.rotation_deg_clockwise;
        if (valid.type == RuntimeGeometryType::kOrientedRectangle &&
            valid.oriented_rectangle.width > 0.0 &&
            valid.oriented_rectangle.height > 0.0) {
            metadata->has_calibration_domain_valid_rectangle = true;
            metadata->calibration_domain_valid_width_px =
                valid.oriented_rectangle.width;
            metadata->calibration_domain_valid_height_px =
                valid.oriented_rectangle.height;
        }
    }
}

nlohmann::json calibration_domain_geometry_json(
    const SpatialLayoutCalibrationImageSetMetadata& metadata,
    bool valid_geometry)
{
    if (metadata.calibration_domain_shape == "circle") {
        const double cx = valid_geometry && metadata.has_calibration_domain_valid_circle
                              ? metadata.calibration_domain_valid_center_x_px
                              : metadata.calibration_domain_center_x_px;
        const double cy = valid_geometry && metadata.has_calibration_domain_valid_circle
                              ? metadata.calibration_domain_valid_center_y_px
                              : metadata.calibration_domain_center_y_px;
        const double r = valid_geometry && metadata.has_calibration_domain_valid_circle
                             ? metadata.calibration_domain_valid_radius_px
                             : metadata.calibration_domain_radius_px;
        return {
            {"type", "circle"},
            {"cx", cx},
            {"cy", cy},
            {"r", r}
        };
    }

    if (metadata.calibration_domain_shape == "oriented_rectangle") {
        const double width =
            valid_geometry && metadata.has_calibration_domain_valid_rectangle
                ? metadata.calibration_domain_valid_width_px
                : metadata.calibration_domain_width_px;
        const double height =
            valid_geometry && metadata.has_calibration_domain_valid_rectangle
                ? metadata.calibration_domain_valid_height_px
                : metadata.calibration_domain_height_px;
        return {
            {"type", "oriented_rectangle"},
            {"cx", metadata.calibration_domain_center_x_px},
            {"cy", metadata.calibration_domain_center_y_px},
            {"width", width},
            {"height", height},
            {"rotation_deg_clockwise",
             metadata.calibration_domain_rotation_deg_clockwise}
        };
    }

    return nlohmann::json::object();
}

nlohmann::json calibration_domain_observation_json(
    const SpatialLayoutCalibrationImageSetMetadata& metadata,
    const std::string& target_plane)
{
    if (!metadata.has_calibration_domain ||
        (metadata.calibration_domain_shape != "circle" &&
         metadata.calibration_domain_shape != "oriented_rectangle")) {
        return nlohmann::json::object();
    }

    nlohmann::json domain = {
        {"shape", metadata.calibration_domain_shape},
        {"source", metadata.calibration_domain_source},
        {"target_plane", metadata_or_unknown(target_plane)},
        {"coordinate_space", metadata.calibration_domain_coordinate_space},
        {"outer_geometry", calibration_domain_geometry_json(metadata, false)},
        {"edge_margin_px", metadata.calibration_domain_edge_margin_px}
    };

    if (metadata.calibration_domain_shape == "circle") {
        domain["center_px"] = {
            metadata.calibration_domain_center_x_px,
            metadata.calibration_domain_center_y_px
        };
        domain["radius_px"] = metadata.calibration_domain_radius_px;
        if (metadata.has_calibration_domain_valid_circle) {
            domain["valid_geometry"] =
                calibration_domain_geometry_json(metadata, true);
        }
    } else if (metadata.calibration_domain_shape == "oriented_rectangle") {
        domain["center_px"] = {
            metadata.calibration_domain_center_x_px,
            metadata.calibration_domain_center_y_px
        };
        domain["width_px"] = metadata.calibration_domain_width_px;
        domain["height_px"] = metadata.calibration_domain_height_px;
        domain["rotation_deg_clockwise"] =
            metadata.calibration_domain_rotation_deg_clockwise;
        if (metadata.has_calibration_domain_valid_rectangle) {
            domain["valid_geometry"] =
                calibration_domain_geometry_json(metadata, true);
        }
    }

    return domain;
}

bool should_attach_observed_domain_for_target_plane(const std::string& target_plane)
{
    return target_plane == "tank_bottom_inner_surface" ||
           target_plane == "tank_bottom_outer_surface" ||
           target_plane == "estimated_fish_plane" ||
           target_plane == "dish_top_rim";
}

void attach_calibration_domain_observation(
    orange::calibration::CalibrationImageSetRequest* request,
    const SpatialLayoutCalibrationImageSetMetadata& metadata)
{
    if (request == nullptr) {
        return;
    }
    if (!should_attach_observed_domain_for_target_plane(request->target_plane)) {
        return;
    }

    const nlohmann::json domain =
        calibration_domain_observation_json(metadata, request->target_plane);
    if (domain.empty()) {
        return;
    }
    if (!request->observations.is_object()) {
        request->observations = nlohmann::json::object();
    }
    request->observations["calibration_domain"] = domain;
    request->observations["observed_domain"] = domain;

    if (request->purpose == "homography_grid") {
        request->observations["homography_fit_intent"] = {
            {"authority", "citrus_fits_and_accepts"},
            {"target_plane", request->target_plane},
            {"domain_shape", domain.value("shape", "unknown")},
            {"expected_destination_coordinate_space", "final_display_canvas_px"},
            {"orange_role", "image_acquisition_and_camera_space_observation"}
        };
    }
}

void attach_projection_surface_authored_domain_hint(
    orange::calibration::CalibrationImageSetRequest* request)
{
    if (request == nullptr || request->target_plane != "projected_surface") {
        return;
    }
    if (!request->observations.is_object()) {
        request->observations = nlohmann::json::object();
    }
    request->observations["authored_domain"] = {
        {"shape", "oriented_rectangle"},
        {"source", "operator_selected_projection_surface_default"},
        {"target_plane", "projected_surface"},
        {"coordinate_space", "final_display_canvas_px"},
        {"geometry_available", false},
        {"authority", "citrus_provides_geometry"}
    };
}

void attach_runtime_role_metadata(
    orange::calibration::CalibrationImageSetRequest* request)
{
    if (request == nullptr || request->target_plane != "tank_bottom_inner_surface") {
        return;
    }
    request->runtime_role = {
        {"role", "behavior_plane_proxy"},
        {"behavior_plane_id", "estimated_fish_plane"},
        {"source", "fallback_to_tank_bottom_inner_surface"},
        {"authority", "citrus_decides_runtime_application"}
    };
}

int pending_group_snapshot_count(const SpatialLayoutUiState& ui_state)
{
    int count = 0;
    for (const SpatialLayoutPendingGroupSnapshotRequest& request :
         ui_state.pending_group_snapshot_requests) {
        if (!request.completed && !request.failed) {
            ++count;
        }
    }
    return count;
}

int failed_group_snapshot_count(const SpatialLayoutUiState& ui_state)
{
    int count = 0;
    for (const SpatialLayoutPendingGroupSnapshotRequest& request :
         ui_state.pending_group_snapshot_requests) {
        if (request.failed) {
            ++count;
        }
    }
    return count;
}

int find_camera_index_by_serial(
    const CameraParams* cameras_params,
    int num_cameras,
    const std::string& camera_serial)
{
    if (cameras_params == nullptr || camera_serial.empty()) {
        return -1;
    }
    for (int i = 0; i < num_cameras; ++i) {
        if (cameras_params[i].camera_serial == camera_serial) {
            return i;
        }
    }
    return -1;
}

SpatialLayoutCalibrationImageSetMetadata make_calibration_image_set_metadata_from_ui(
    const SpatialLayoutUiState& ui_state)
{
    SpatialLayoutCalibrationImageSetMetadata metadata;
    metadata.filter_state = ui_state.calibration_filter_state;
    metadata.runtime_filter_state = ui_state.calibration_runtime_filter_state;
    metadata.light_handling = ui_state.calibration_light_handling;
    metadata.light_state = ui_state.calibration_light_state;
    metadata.illumination_spectrum = ui_state.calibration_illumination_spectrum;
    metadata.illumination_source = ui_state.calibration_illumination_source;
    metadata.illumination_center_wavelength_nm =
        ui_state.calibration_illumination_center_wavelength_nm;
    metadata.has_illumination_center_wavelength_nm =
        ui_state.calibration_has_illumination_center_wavelength_nm;
    metadata.illumination_min_wavelength_nm =
        ui_state.calibration_illumination_min_wavelength_nm;
    metadata.has_illumination_min_wavelength_nm =
        ui_state.calibration_has_illumination_min_wavelength_nm;
    metadata.illumination_max_wavelength_nm =
        ui_state.calibration_illumination_max_wavelength_nm;
    metadata.has_illumination_max_wavelength_nm =
        ui_state.calibration_has_illumination_max_wavelength_nm;
    metadata.illumination_bandwidth_fwhm_nm =
        ui_state.calibration_illumination_bandwidth_fwhm_nm;
    metadata.has_illumination_bandwidth_fwhm_nm =
        ui_state.calibration_has_illumination_bandwidth_fwhm_nm;
    metadata.illumination_wavelength_confidence =
        ui_state.calibration_illumination_wavelength_confidence;
    metadata.projector_state = ui_state.calibration_projector_state;
    metadata.projector_visible_to_camera = ui_state.calibration_projector_visible_to_camera;
    metadata.requires_filter_reinstalled_repeatably =
        ui_state.calibration_requires_filter_reinstalled_repeatably;
    metadata.operator_notes = ui_state.calibration_operator_notes;
    metadata.image_set_purpose = ui_state.calibration_image_set_purpose;
    metadata.image_set_target_plane = ui_state.calibration_image_set_target_plane;
    metadata.image_set_image_role = ui_state.calibration_image_set_image_role;
    metadata.image_set_projected_pattern_id =
        ui_state.calibration_image_set_projected_pattern_id;
    metadata.image_set_projected_pattern_type =
        ui_state.calibration_image_set_projected_pattern_type;
    metadata.image_set_scale_target_type = ui_state.calibration_image_set_scale_target_type;
    metadata.image_set_notes = ui_state.calibration_image_set_notes;
    populate_calibration_domain_metadata_from_runtime(&metadata, ui_state);
    return metadata;
}

void apply_calibration_image_set_metadata_to_ui(
    SpatialLayoutUiState* ui_state,
    const SpatialLayoutCalibrationImageSetMetadata& metadata)
{
    if (ui_state == nullptr) {
        return;
    }
    ui_state->calibration_filter_state = metadata.filter_state;
    ui_state->calibration_runtime_filter_state = metadata.runtime_filter_state;
    ui_state->calibration_light_handling = metadata.light_handling;
    ui_state->calibration_light_state = metadata.light_state;
    ui_state->calibration_illumination_spectrum = metadata.illumination_spectrum;
    ui_state->calibration_illumination_source = metadata.illumination_source;
    ui_state->calibration_illumination_center_wavelength_nm =
        metadata.illumination_center_wavelength_nm;
    ui_state->calibration_has_illumination_center_wavelength_nm =
        metadata.has_illumination_center_wavelength_nm;
    ui_state->calibration_illumination_min_wavelength_nm =
        metadata.illumination_min_wavelength_nm;
    ui_state->calibration_has_illumination_min_wavelength_nm =
        metadata.has_illumination_min_wavelength_nm;
    ui_state->calibration_illumination_max_wavelength_nm =
        metadata.illumination_max_wavelength_nm;
    ui_state->calibration_has_illumination_max_wavelength_nm =
        metadata.has_illumination_max_wavelength_nm;
    ui_state->calibration_illumination_bandwidth_fwhm_nm =
        metadata.illumination_bandwidth_fwhm_nm;
    ui_state->calibration_has_illumination_bandwidth_fwhm_nm =
        metadata.has_illumination_bandwidth_fwhm_nm;
    ui_state->calibration_illumination_wavelength_confidence =
        metadata.illumination_wavelength_confidence;
    ui_state->calibration_projector_state = metadata.projector_state;
    ui_state->calibration_projector_visible_to_camera = metadata.projector_visible_to_camera;
    ui_state->calibration_requires_filter_reinstalled_repeatably =
        metadata.requires_filter_reinstalled_repeatably;
    ui_state->calibration_operator_notes = metadata.operator_notes;
    ui_state->calibration_image_set_purpose = metadata.image_set_purpose;
    ui_state->calibration_image_set_target_plane = metadata.image_set_target_plane;
    ui_state->calibration_image_set_image_role = metadata.image_set_image_role;
    ui_state->calibration_image_set_projected_pattern_id =
        metadata.image_set_projected_pattern_id;
    ui_state->calibration_image_set_projected_pattern_type =
        metadata.image_set_projected_pattern_type;
    ui_state->calibration_image_set_scale_target_type = metadata.image_set_scale_target_type;
    ui_state->calibration_image_set_notes = metadata.image_set_notes;
}

SpatialLayoutGroupCaptureFrame make_group_capture_from_snapshot(
    const SpatialSnapshotResult& result,
    const CameraParams* cameras_params,
    int num_cameras,
    const std::string& capture_group_id,
    const std::string& capture_mode,
    const SpatialLayoutCalibrationImageSetMetadata& metadata)
{
    SpatialLayoutGroupCaptureFrame capture;
    capture.valid = result.ok && result.width > 0 && result.height > 0 && !result.rgba.empty();
    capture.capture_group_id = capture_group_id;
    capture.metadata = metadata;
    capture.camera_serial = result.camera_serial;
    capture.camera_index =
        find_camera_index_by_serial(cameras_params, num_cameras, result.camera_serial);
    if (capture.camera_index >= 0) {
        const CameraParams& camera_params = cameras_params[capture.camera_index];
        capture.camera_name = camera_params.camera_name;
        capture.camera_configured_width = camera_params.width;
        capture.camera_configured_height = camera_params.height;
        capture.camera_pixel_format = camera_params.pixel_format;
        capture.camera_exposure_us = static_cast<double>(camera_params.exposure);
        capture.has_camera_exposure_us = true;
        capture.camera_frame_rate_hz = static_cast<double>(camera_params.frame_rate);
        capture.has_camera_frame_rate_hz = true;
        capture.camera_gain = static_cast<double>(camera_params.gain);
        capture.has_camera_gain = true;
    }
    capture.width = result.width;
    capture.height = result.height;
    capture.rgba = result.rgba;
    capture.source_array_role =
        result.source_array_role.empty() ? "images_full" : result.source_array_role;
    capture.capture_mode = capture_mode.empty() ? "operator_group_next_frame" : capture_mode;
    capture.source_frame_count = std::max<uint32_t>(1u, result.completed_frame_count);
    capture.first_local_frame_id = result.first_local_frame_id;
    capture.last_local_frame_id = result.last_local_frame_id;
    capture.first_camera_frame_id = result.first_camera_frame_id;
    capture.last_camera_frame_id = result.last_camera_frame_id;
    return capture;
}

void upsert_group_capture(
    SpatialLayoutUiState* ui_state,
    SpatialLayoutGroupCaptureFrame capture)
{
    if (ui_state == nullptr || capture.camera_serial.empty()) {
        return;
    }
    for (SpatialLayoutGroupCaptureFrame& existing : ui_state->group_captures) {
        if (existing.camera_serial == capture.camera_serial) {
            orange::preview::clear_texture(
                &existing.texture,
                &existing.texture_width,
                &existing.texture_height);
            existing = std::move(capture);
            return;
        }
    }
    ui_state->group_captures.push_back(std::move(capture));
}

void clear_group_captures(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr) {
        return;
    }
    for (SpatialLayoutGroupCaptureFrame& capture : ui_state->group_captures) {
        orange::preview::clear_texture(
            &capture.texture,
            &capture.texture_width,
            &capture.texture_height);
    }
    ui_state->group_captures.clear();
    ui_state->pending_group_snapshot_requests.clear();
}

bool apply_group_capture_to_active_preview(
    SpatialLayoutUiState* ui_state,
    const SpatialLayoutGroupCaptureFrame& capture,
    std::string* error_out)
{
    SpatialSnapshotResult result;
    result.ok = capture.valid;
    result.camera_serial = capture.camera_serial;
    result.capture_mode = capture.capture_mode;
    result.source_array_role = capture.source_array_role;
    result.width = capture.width;
    result.height = capture.height;
    result.completed_frame_count = std::max<uint32_t>(1u, capture.source_frame_count);
    result.first_local_frame_id = capture.first_local_frame_id;
    result.last_local_frame_id = capture.last_local_frame_id;
    result.first_camera_frame_id = capture.first_camera_frame_id;
    result.last_camera_frame_id = capture.last_camera_frame_id;
    result.local_frame_id = capture.last_local_frame_id;
    result.camera_frame_id = capture.last_camera_frame_id;
    result.rgba = capture.rgba;
    const bool ok = apply_full_resolution_stream_snapshot(ui_state, result, error_out);
    if (ok) {
        ui_state->captured_capture_group_id = capture.capture_group_id;
        ui_state->preview_status =
            "Showing grouped full-resolution capture from " + capture.camera_serial +
            " (" + capture.capture_group_id + ").";
    }
    return ok;
}

bool consume_group_snapshot_result(
    SpatialLayoutUiState* ui_state,
    const SpatialSnapshotResult& result,
    const CameraParams* cameras_params,
    int num_cameras,
    int selected_camera_index)
{
    if (ui_state == nullptr || ui_state->pending_group_snapshot_requests.empty()) {
        return false;
    }

    for (SpatialLayoutPendingGroupSnapshotRequest& request :
         ui_state->pending_group_snapshot_requests) {
        if (request.camera_serial != result.camera_serial ||
            request.request_id != result.request_id ||
            request.completed ||
            request.failed) {
            continue;
        }

        if (result.ok && result.width > 0 && result.height > 0 && !result.rgba.empty()) {
            SpatialLayoutGroupCaptureFrame capture =
                make_group_capture_from_snapshot(
                    result,
                    cameras_params,
                    num_cameras,
                    ui_state->group_capture_id,
                    ui_state->group_capture_mode,
                    ui_state->group_capture_metadata);
            std::string texture_error;
            if (!orange::preview::update_rgba_texture(
                    &capture.texture,
                    &capture.texture_width,
                    &capture.texture_height,
                    capture.rgba,
                    capture.width,
                    capture.height,
                    &texture_error)) {
                request.failed = true;
                request.completed = false;
                request.error = texture_error.empty()
                                    ? "Grouped capture texture upload failed."
                                    : texture_error;
            } else {
                request.completed = true;
                upsert_group_capture(ui_state, capture);
                if (selected_camera_index >= 0 &&
                    selected_camera_index < num_cameras &&
                    cameras_params[selected_camera_index].camera_serial == result.camera_serial) {
                    std::string preview_error;
                    if (!apply_group_capture_to_active_preview(ui_state, capture, &preview_error)) {
                        ui_state->preview_error = preview_error;
                    }
                }
            }
        } else {
            request.failed = true;
            request.error = result.error.empty()
                                ? "Grouped full-resolution snapshot failed."
                                : result.error;
        }

        const int pending = pending_group_snapshot_count(*ui_state);
        const int failed = failed_group_snapshot_count(*ui_state);
        std::ostringstream status;
        status << "Grouped capture " << ui_state->group_capture_id
               << ": completed=" << ui_state->group_captures.size()
               << " pending=" << pending
               << " failed=" << failed << ".";
        ui_state->group_capture_status = status.str();
        if (failed > 0) {
            std::ostringstream error;
            for (const SpatialLayoutPendingGroupSnapshotRequest& pending_request :
                 ui_state->pending_group_snapshot_requests) {
                if (!pending_request.failed) {
                    continue;
                }
                if (error.tellp() > 0) {
                    error << " ";
                }
                error << pending_request.camera_serial << ": "
                      << (pending_request.error.empty()
                              ? "capture failed"
                              : pending_request.error);
            }
            ui_state->group_capture_error = error.str();
        } else {
            ui_state->group_capture_error.clear();
        }
        if (pending == 0 && failed == 0) {
            ui_state->group_capture_status =
                "Grouped capture " + ui_state->group_capture_id +
                " complete for " + std::to_string(ui_state->group_captures.size()) +
                " camera(s) as " + ui_state->group_capture_metadata.image_set_purpose +
                " on " + ui_state->group_capture_metadata.image_set_target_plane + ".";
        }
        return true;
    }
    return false;
}

std::string build_group_capture_id(
    const SpatialLayoutUiState& ui_state,
    const SpatialLayoutCalibrationImageSetMetadata& metadata,
    const std::string& timestamp)
{
    std::ostringstream oss;
    oss << "calgrp_" << sanitize_artifact_component(timestamp);
    if (ui_state.citrus_template.available &&
        !ui_state.citrus_template.source_canvas_name.empty()) {
        oss << "_" << sanitize_artifact_component(ui_state.citrus_template.source_canvas_name);
    }
    if (!metadata.image_set_purpose.empty()) {
        oss << "_" << sanitize_artifact_component(metadata.image_set_purpose);
    }
    if (!metadata.image_set_target_plane.empty()) {
        oss << "_" << sanitize_artifact_component(metadata.image_set_target_plane);
    }
    return oss.str();
}

bool camera_is_group_capture_eligible(
    const CameraEachSelect* cameras_select,
    SpatialSnapshotWorker* const* spatial_snapshot_workers,
    int camera_index)
{
    return cameras_select != nullptr &&
           spatial_snapshot_workers != nullptr &&
           camera_index >= 0 &&
           cameras_select[camera_index].stream_on &&
           spatial_snapshot_workers[camera_index] != nullptr;
}

int eligible_group_capture_camera_count(
    const CameraEachSelect* cameras_select,
    SpatialSnapshotWorker* const* spatial_snapshot_workers,
    int num_cameras)
{
    int count = 0;
    for (int i = 0; i < num_cameras; ++i) {
        if (camera_is_group_capture_eligible(cameras_select, spatial_snapshot_workers, i)) {
            ++count;
        }
    }
    return count;
}

bool request_group_full_resolution_snapshots(
    SpatialLayoutUiState* ui_state,
    const CameraParams* cameras_params,
    const CameraEachSelect* cameras_select,
    int num_cameras,
    SpatialSnapshotWorker* const* spatial_snapshot_workers,
    uint32_t target_frame_count,
    std::string* error_out)
{
    if (ui_state == nullptr || cameras_params == nullptr || cameras_select == nullptr ||
        spatial_snapshot_workers == nullptr || num_cameras <= 0) {
        if (error_out) {
            *error_out = "Grouped capture requires open cameras and snapshot workers.";
        }
        return false;
    }
    if (pending_group_snapshot_count(*ui_state) > 0) {
        if (error_out) {
            *error_out = "A grouped capture is already pending.";
        }
        return false;
    }

    clear_group_captures(ui_state);
    ui_state->group_capture_error.clear();

    const std::string timestamp = get_current_utc_timestamp();
    ui_state->group_capture_metadata =
        make_calibration_image_set_metadata_from_ui(*ui_state);
    ui_state->group_capture_id =
        build_group_capture_id(*ui_state, ui_state->group_capture_metadata, timestamp);
    ui_state->group_capture_mode =
        target_frame_count > 1 ? "operator_group_temporal_mean" : "operator_group_next_frame";

    int requested = 0;
    std::ostringstream request_errors;
    for (int camera_index = 0; camera_index < num_cameras; ++camera_index) {
        if (!camera_is_group_capture_eligible(cameras_select, spatial_snapshot_workers, camera_index)) {
            continue;
        }

        SpatialSnapshotWorker* worker = spatial_snapshot_workers[camera_index];
        uint64_t request_id = 0;
        std::string request_error;
        std::ostringstream operation_id;
        operation_id << ui_state->group_capture_id
                     << "_Cam" << cameras_params[camera_index].camera_serial;
        if (!worker->RequestSnapshot(
                operation_id.str(),
                &request_id,
                &request_error,
                std::max<uint32_t>(1u, target_frame_count))) {
            if (request_errors.tellp() > 0) {
                request_errors << " ";
            }
            request_errors << cameras_params[camera_index].camera_serial
                           << ": "
                           << (request_error.empty()
                                   ? "request rejected"
                                   : request_error);
            SpatialLayoutPendingGroupSnapshotRequest failed_request;
            failed_request.camera_serial = cameras_params[camera_index].camera_serial;
            failed_request.failed = true;
            failed_request.error = request_error.empty() ? "request rejected" : request_error;
            ui_state->pending_group_snapshot_requests.push_back(std::move(failed_request));
            continue;
        }

        SpatialLayoutPendingGroupSnapshotRequest pending_request;
        pending_request.camera_serial = cameras_params[camera_index].camera_serial;
        pending_request.request_id = request_id;
        ui_state->pending_group_snapshot_requests.push_back(std::move(pending_request));
        ++requested;
    }

    if (requested == 0) {
        ui_state->group_capture_status.clear();
        ui_state->group_capture_error =
            request_errors.tellp() > 0
                ? request_errors.str()
                : "No streaming cameras with spatial snapshot workers are available.";
        if (error_out) {
            *error_out = ui_state->group_capture_error;
        }
        return false;
    }

    std::ostringstream status;
    status << "Requested grouped full-resolution capture "
           << ui_state->group_capture_id
           << " from " << requested << " camera(s)"
           << " as " << ui_state->group_capture_metadata.image_set_purpose
           << " on " << ui_state->group_capture_metadata.image_set_target_plane;
    if (target_frame_count > 1) {
        status << " averaging " << target_frame_count << " frames";
    }
    status << ".";
    ui_state->group_capture_status = status.str();
    ui_state->group_capture_error =
        request_errors.tellp() > 0 ? request_errors.str() : std::string();
    return true;
}

bool invert_affine_3x3(const std::array<double, 9>& matrix, std::array<double, 9>* out)
{
    if (out == nullptr) {
        return false;
    }

    const double a = matrix[0];
    const double b = matrix[1];
    const double c = matrix[2];
    const double d = matrix[3];
    const double e = matrix[4];
    const double f = matrix[5];
    const double det = a * e - b * d;
    if (!std::isfinite(det) || std::abs(det) < 1e-12) {
        return false;
    }

    *out = {
        e / det, -b / det, (b * f - e * c) / det,
        -d / det, a / det, (d * c - a * f) / det,
        0.0, 0.0, 1.0
    };
    return true;
}

std::array<double, 9> build_layout_to_camera_matrix(const SpatialLayoutUiState& ui_state)
{
    double tx = ui_state.registration_tx_px;
    double ty = ui_state.registration_ty_px;
    double scale = ui_state.registration_scale;
    double rotation_deg_clockwise = ui_state.registration_rotation_deg_clockwise;

    if (ui_state.registration.type == RegistrationType::kIdentity) {
        tx = 0.0;
        ty = 0.0;
        scale = 1.0;
        rotation_deg_clockwise = 0.0;
    } else if (ui_state.registration.type == RegistrationType::kTranslation) {
        scale = 1.0;
        rotation_deg_clockwise = 0.0;
    }

    scale = std::max(scale, 1e-6);
    const double theta = rotation_deg_clockwise * kPi / 180.0;
    const double cos_theta = std::cos(theta);
    const double sin_theta = std::sin(theta);

    return {
        scale * cos_theta, -scale * sin_theta, tx,
        scale * sin_theta, scale * cos_theta, ty,
        0.0, 0.0, 1.0
    };
}

void apply_view_registration_to_editor_state(
    SpatialLayoutUiState* ui_state,
    const ViewRegistration& registration)
{
    if (ui_state == nullptr) {
        return;
    }

    ui_state->registration = registration;
    ui_state->registration.layout_coordinate_space = ui_state->layout_artifact.layout.coordinate_space;
    ui_state->registration_tx_px = registration.layout_to_camera_matrix[2];
    ui_state->registration_ty_px = registration.layout_to_camera_matrix[5];

    if (registration.type == RegistrationType::kSimilarity) {
        ui_state->registration_scale = std::max(
            1e-6,
            std::sqrt(registration.layout_to_camera_matrix[0] * registration.layout_to_camera_matrix[0] +
                      registration.layout_to_camera_matrix[3] * registration.layout_to_camera_matrix[3]));
        ui_state->registration_rotation_deg_clockwise = normalize_angle_deg(
            std::atan2(registration.layout_to_camera_matrix[3], registration.layout_to_camera_matrix[0]) * 180.0 / kPi);
    } else {
        ui_state->registration_scale = 1.0;
        ui_state->registration_rotation_deg_clockwise = 0.0;
    }
}

nlohmann::json make_arena_layout_manifest_json(
    const ArenaLayoutArtifact& artifact,
    const CameraParams& camera_params,
    const SpatialLayoutPersistedFiles& files)
{
    nlohmann::json manifest = {
        {"schema_id", kCalibrationManifestSchemaId},
        {"schema_version", kCalibrationManifestSchemaVersion},
        {"artifact_id", artifact.artifact_id},
        {"artifact_schema_id", orange::spatial::kArenaLayoutArtifactSchemaId},
        {"artifact_schema_version", orange::spatial::kArenaLayoutArtifactSchemaVersion},
        {"created_utc", artifact.created_utc},
        {"producer", {
            {"name", "orange"},
            {"artifact_schema_id", orange::spatial::kArenaLayoutArtifactSchemaId},
            {"artifact_schema_version", orange::spatial::kArenaLayoutArtifactSchemaVersion}
        }},
        {"calibration_ref", orange::spatial::calibration_ref_to_json(artifact.calibration_ref)},
        {"compatibility", {
            {"camera_serial", camera_params.camera_serial},
            {"focus", camera_params.focus},
            {"iris", camera_params.iris},
            {"exposure", camera_params.exposure},
            {"gain", camera_params.gain},
            {"pixel_format", camera_params.pixel_format},
            {"width", camera_params.width},
            {"height", camera_params.height}
        }},
        {"summary", {
            {"layout_id", artifact.layout_id},
            {"zone_count", static_cast<int>(artifact.layout.zones.size())},
            {"coordinate_space", orange::spatial::coordinate_space_to_string(artifact.layout.coordinate_space)},
            {"outer_geometry_type", orange::spatial::layout_geometry_type_to_string(artifact.layout.outer_geometry.type)}
        }},
        {"files", {
            {"manifest", files.manifest_path.filename().string()},
            {"measurement_json", files.measurement_path.filename().string()},
            {"arena_layout_runtime_json", files.arena_layout_runtime_path.filename().string()},
            {"dish_mask_runtime_json", files.dish_mask_runtime_path.filename().string()}
        }}
    };

    if (!artifact.context.canvas_id.empty()) {
        manifest["summary"]["canvas_id"] = artifact.context.canvas_id;
    }
    if (!artifact.context.dish_design_id.empty()) {
        manifest["summary"]["dish_design_id"] = artifact.context.dish_design_id;
    }
    return manifest;
}

bool save_spatial_layout_artifact(
    SpatialLayoutUiState* ui_state,
    const CameraParams& selected_camera,
    const std::string& artifact_root_dir,
    const std::string& session_dir,
    std::string* status_out,
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

    if (!ensure_directory_exists(artifact_root_dir, error_out)) {
        return false;
    }

    ArenaLayoutArtifact artifact = ui_state->layout_artifact;
    if (artifact.artifact_id.empty() || artifact.artifact_id == "preview.arena_layout") {
        artifact.artifact_id = build_arena_layout_artifact_id(
            artifact.layout_id.empty() ? "layout" : artifact.layout_id,
            selected_camera,
            get_current_utc_timestamp());
    }
    artifact.created_utc = get_current_utc_timestamp();
    artifact.calibration_ref.artifact_id = artifact.artifact_id;
    artifact.calibration_ref.artifact_schema_id = orange::spatial::kArenaLayoutArtifactSchemaId;
    artifact.calibration_ref.artifact_schema_version = orange::spatial::kArenaLayoutArtifactSchemaVersion;
    artifact.calibration_ref.fingerprint = "pending";

    std::string validation_error;
    if (!orange::spatial::validate_arena_layout_artifact(artifact, &validation_error)) {
        if (error_out) {
            *error_out = "Arena layout artifact is invalid: " + validation_error;
        }
        return false;
    }
    if (!orange::spatial::validate_dish_mask_runtime(ui_state->dish_mask_runtime, &validation_error)) {
        if (error_out) {
            *error_out = "Dish-mask runtime is invalid: " + validation_error;
        }
        return false;
    }
    if (!orange::spatial::validate_arena_layout_runtime_against_artifact(
            ui_state->arena_layout_runtime,
            artifact,
            &validation_error)) {
        if (error_out) {
            *error_out = "Arena-layout runtime is invalid: " + validation_error;
        }
        return false;
    }

    nlohmann::json measurement_json = orange::spatial::arena_layout_artifact_to_json(artifact);
    artifact.calibration_ref.fingerprint = compute_json_fingerprint(measurement_json);
    measurement_json = orange::spatial::arena_layout_artifact_to_json(artifact);

    const SpatialLayoutPersistedFiles files =
        make_spatial_layout_persisted_files(artifact_root_dir, artifact.artifact_id);
    if (!ensure_directory_exists(files.artifact_dir.string(), error_out)) {
        return false;
    }

    const nlohmann::json manifest_json = make_arena_layout_manifest_json(artifact, selected_camera, files);
    const nlohmann::json arena_layout_runtime_json =
        orange::spatial::arena_layout_runtime_to_json(ui_state->arena_layout_runtime);
    const nlohmann::json dish_mask_runtime_json =
        orange::spatial::dish_mask_runtime_to_json(ui_state->dish_mask_runtime);

    if (!write_json_file(files.measurement_path, measurement_json, error_out) ||
        !write_json_file(files.arena_layout_runtime_path, arena_layout_runtime_json, error_out) ||
        !write_json_file(files.dish_mask_runtime_path, dish_mask_runtime_json, error_out) ||
        !write_json_file(files.manifest_path, manifest_json, error_out)) {
        return false;
    }
    if (!update_spatial_calibration_session_index(
            session_dir,
            artifact_root_dir,
            manifest_json,
            error_out)) {
        return false;
    }
    if (!update_calibration_artifact_registry(artifact_root_dir, manifest_json, error_out)) {
        return false;
    }

    ui_state->layout_artifact = std::move(artifact);
    if (status_out) {
        *status_out = "Saved arena layout artifact to " + files.artifact_dir.string();
        if (!session_dir.empty()) {
            *status_out += " in calibration session " +
                           std::filesystem::path(session_dir).filename().generic_string();
        }
    }
    return true;
}

bool runtime_geometry_to_top_rim_circle(
    const RuntimeGeometry& geometry,
    orange::calibration::DishTopRimCircle* circle_out,
    std::string* error_out)
{
    if (circle_out == nullptr) {
        if (error_out) {
            *error_out = "Top-rim circle destination is null.";
        }
        return false;
    }
    if (geometry.type != RuntimeGeometryType::kCircle || geometry.circle.r <= 0.0) {
        if (error_out) {
            *error_out = "Top-rim observation requires a circular resolved experimental boundary.";
        }
        return false;
    }
    circle_out->center.x = geometry.circle.cx;
    circle_out->center.y = geometry.circle.cy;
    circle_out->radius_px = geometry.circle.r;
    return true;
}

bool captured_frame_to_gray8(const SpatialLayoutUiState& ui_state, cv::Mat* image_out, std::string* error_out)
{
    if (image_out == nullptr) {
        if (error_out) {
            *error_out = "Top-rim source image destination is null.";
        }
        return false;
    }
    if (!ui_state.has_capture ||
        ui_state.captured_texture_width <= 0 ||
        ui_state.captured_texture_height <= 0 ||
        ui_state.captured_rgba.empty()) {
        if (error_out) {
            *error_out = "Capture a frame before saving a top-rim observation.";
        }
        return false;
    }

    const size_t expected_size =
        static_cast<size_t>(ui_state.captured_texture_width) *
        static_cast<size_t>(ui_state.captured_texture_height) *
        4u;
    if (ui_state.captured_rgba.size() < expected_size) {
        if (error_out) {
            *error_out = "Captured RGBA buffer is smaller than the recorded image dimensions.";
        }
        return false;
    }

    const cv::Mat rgba(
        ui_state.captured_texture_height,
        ui_state.captured_texture_width,
        CV_8UC4,
        const_cast<unsigned char*>(ui_state.captured_rgba.data()));
    cv::Mat gray;
    cv::cvtColor(rgba, gray, cv::COLOR_RGBA2GRAY);
    *image_out = gray.clone();
    return true;
}

void apply_captured_frame_provenance_to_capture(
    const SpatialLayoutUiState& ui_state,
    orange::calibration::CalibrationImageSetCaptureContext* capture)
{
    if (capture == nullptr) {
        return;
    }
    capture->source_frame_count = std::max<uint32_t>(1u, ui_state.captured_source_frame_count);
    capture->has_source_frame_count = true;
    capture->capture_group_id = ui_state.captured_capture_group_id;
    if (capture->source_frame_count > 1 ||
        ui_state.captured_capture_mode == "temporal_mean_stream_frames_v1") {
        capture->temporal_compositing_method = "temporal_mean_stream_frames_v1";
    }
    if (ui_state.captured_first_local_frame_id != 0 ||
        ui_state.captured_last_local_frame_id != 0) {
        capture->first_local_frame_id = ui_state.captured_first_local_frame_id;
        capture->last_local_frame_id = ui_state.captured_last_local_frame_id;
        capture->has_local_frame_range = true;
    }
    if (ui_state.captured_first_camera_frame_id != 0 ||
        ui_state.captured_last_camera_frame_id != 0) {
        capture->first_camera_frame_id = ui_state.captured_first_camera_frame_id;
        capture->last_camera_frame_id = ui_state.captured_last_camera_frame_id;
        capture->has_camera_frame_range = true;
    }
}

void apply_captured_frame_provenance_to_capture(
    const SpatialLayoutUiState& ui_state,
    orange::calibration::DishTopRimCaptureContext* capture)
{
    if (capture == nullptr) {
        return;
    }
    capture->source_frame_count = std::max<uint32_t>(1u, ui_state.captured_source_frame_count);
    capture->has_source_frame_count = true;
    if (capture->source_frame_count > 1 ||
        ui_state.captured_capture_mode == "temporal_mean_stream_frames_v1") {
        capture->temporal_compositing_method = "temporal_mean_stream_frames_v1";
    }
    if (ui_state.captured_first_local_frame_id != 0 ||
        ui_state.captured_last_local_frame_id != 0) {
        capture->first_local_frame_id = ui_state.captured_first_local_frame_id;
        capture->last_local_frame_id = ui_state.captured_last_local_frame_id;
        capture->has_local_frame_range = true;
    }
    if (ui_state.captured_first_camera_frame_id != 0 ||
        ui_state.captured_last_camera_frame_id != 0) {
        capture->first_camera_frame_id = ui_state.captured_first_camera_frame_id;
        capture->last_camera_frame_id = ui_state.captured_last_camera_frame_id;
        capture->has_camera_frame_range = true;
    }
}

orange::calibration::DishTopRimHoughParams make_top_rim_hough_params(
    const SpatialLayoutUiState& ui_state,
    const orange::calibration::DishTopRimCircle& accepted_circle,
    int width,
    int height)
{
    orange::calibration::DishTopRimHoughParams params;
    const double min_dim = static_cast<double>(std::max(1, std::min(width, height)));
    const double max_dim = static_cast<double>(std::max(width, height));
    const double radius = std::max(1.0, accepted_circle.radius_px);
    params.dp = std::clamp(ui_state.hough_dp, 1.0, 3.0);
    params.min_dist_px =
        std::max(1.0, min_dim * std::clamp(ui_state.hough_min_dist_fraction, 0.01, 2.0));
    params.param1 = std::clamp(ui_state.hough_param1, 1.0, 500.0);
    params.param2 = std::clamp(ui_state.hough_param2, 1.0, 500.0);
    params.min_radius_px =
        std::max(
            4,
            static_cast<int>(
                std::floor(min_dim * std::clamp(ui_state.hough_min_radius_fraction, 0.001, 1.0))));
    params.max_radius_px =
        std::max(params.min_radius_px + 1,
                 static_cast<int>(
                     std::ceil(
                         std::min(
                             max_dim,
                             min_dim *
                                 std::clamp(ui_state.hough_max_radius_fraction, 0.001, 1.5)))));
    if (params.min_radius_px > static_cast<int>(std::floor(radius * 1.10)) ||
        params.max_radius_px < static_cast<int>(std::ceil(radius * 0.90))) {
        params.min_radius_px = std::max(4, static_cast<int>(std::floor(radius * 0.75)));
        params.max_radius_px =
            std::max(
                params.min_radius_px + 1,
                static_cast<int>(std::ceil(std::min(max_dim, radius * 1.25))));
    }
    params.radius_adjustment_px = ui_state.hough_radius_adjustment_px;
    return params;
}

struct TopRimObservationSaveJob {
    std::string artifact_root_dir;
    std::string session_dir;
    orange::calibration::DishTopRimObservationRequest request;
    orange::calibration::DishTopRimHoughParams hough_params;
    orange::calibration::DishTopRimCircle accepted_circle;
    cv::Mat source_gray;
};

struct TopRimObservationSaveResult {
    bool ok = false;
    std::string status;
    std::string error;
};

nlohmann::json make_top_rim_observation_link_json(
    const std::string& artifact_root_dir,
    const orange::calibration::DishTopRimObservationRequest& request,
    const orange::calibration::DishTopRimObservationWriteResult& write_result)
{
    const orange::calibration::DishTopRimObservationArtifactPaths paths =
        orange::calibration::make_dish_top_rim_observation_artifact_paths(
            artifact_root_dir,
            write_result.artifact_id);
    const std::filesystem::path aggregate_dir =
        std::filesystem::path(artifact_root_dir) /
        request.arena_context.value("associated_image_set_artifact_id", std::string());

    std::error_code rel_error;
    std::filesystem::path relative_manifest =
        std::filesystem::relative(paths.manifest_path, aggregate_dir, rel_error);
    if (rel_error || relative_manifest.empty()) {
        relative_manifest =
            std::filesystem::path("..") / write_result.artifact_id /
            kSpatialLayoutManifestFilename;
    }
    rel_error.clear();
    std::filesystem::path relative_observation =
        std::filesystem::relative(paths.observation_json_path, aggregate_dir, rel_error);
    if (rel_error || relative_observation.empty()) {
        relative_observation =
            std::filesystem::path("..") / write_result.artifact_id / "observation.json";
    }

    nlohmann::json link = {
        {"artifact_id", write_result.artifact_id},
        {"artifact_schema_id", orange::calibration::kDishTopRimObservationSchemaId},
        {"artifact_schema_version", orange::calibration::kDishTopRimObservationSchemaVersion},
        {"fingerprint", write_result.fingerprint},
        {"relative_manifest_path", relative_manifest.generic_string()},
        {"relative_observation_path", relative_observation.generic_string()},
        {"selection_policy", "latest_saved_for_camera_arena"},
        {"target_plane", "dish_top_rim"},
        {"coordinate_space", "camera_native_pixels"},
        {"camera_serial", request.camera.serial},
        {"accepted_at_utc", request.created_utc}
    };
    if (!request.arena_context.empty()) {
        link["arena_context"] = request.arena_context;
        const std::string arena_id =
            request.arena_context.value("arena_id", std::string());
        const std::string canvas_id =
            request.arena_context.value("canvas_id", std::string());
        if (!arena_id.empty()) {
            link["arena_id"] = arena_id;
        }
        if (!canvas_id.empty()) {
            link["canvas_id"] = canvas_id;
        }
    }
    if (write_result.observation.is_object()) {
        link["accepted_mask"] =
            write_result.observation.value("accepted_mask", nlohmann::json::object());
        link["observed_boundary"] =
            write_result.observation.value("observed_boundary", nlohmann::json::object());
        link["valid_detection_region"] =
            write_result.observation.value(
                "valid_detection_region",
                nlohmann::json::object());
    }
    return link;
}

bool link_top_rim_observation_to_camera_arena_aggregate(
    const TopRimObservationSaveJob& job,
    const orange::calibration::DishTopRimObservationWriteResult& write_result,
    std::string* error_out)
{
    if (!job.request.arena_context.is_object()) {
        return true;
    }
    const std::string associated_image_set_artifact_id =
        job.request.arena_context.value(
            "associated_image_set_artifact_id",
            std::string());
    if (associated_image_set_artifact_id.empty()) {
        return true;
    }

    const std::filesystem::path aggregate_dir =
        std::filesystem::path(job.artifact_root_dir) /
        associated_image_set_artifact_id;
    const std::filesystem::path image_set_path =
        aggregate_dir / "image_set.json";
    const std::filesystem::path manifest_path =
        aggregate_dir / kSpatialLayoutManifestFilename;
    if (!std::filesystem::exists(image_set_path) ||
        !std::filesystem::exists(manifest_path)) {
        return true;
    }

    nlohmann::json image_set;
    if (!read_json_file(image_set_path, &image_set, error_out)) {
        return false;
    }
    if (!image_set.is_object()) {
        if (error_out) {
            *error_out = "Cannot link top-rim observation: aggregate image_set.json is not an object: " +
                         image_set_path.generic_string();
        }
        return false;
    }

    const nlohmann::json link =
        make_top_rim_observation_link_json(
            job.artifact_root_dir,
            job.request,
            write_result);
    if (!image_set.contains("linked_observations") ||
        !image_set["linked_observations"].is_object()) {
        image_set["linked_observations"] = nlohmann::json::object();
    }
    image_set["linked_observations"]["accepted_top_rim_observation"] = link;
    image_set["updated_utc"] = job.request.created_utc;
    const std::string image_set_fingerprint = compute_json_fingerprint(image_set);
    if (!write_json_file(image_set_path, image_set, error_out)) {
        return false;
    }

    nlohmann::json manifest;
    if (!read_json_file(manifest_path, &manifest, error_out)) {
        return false;
    }
    if (!manifest.is_object()) {
        if (error_out) {
            *error_out = "Cannot link top-rim observation: aggregate manifest is not an object: " +
                         manifest_path.generic_string();
        }
        return false;
    }
    if (!manifest.contains("linked_observations") ||
        !manifest["linked_observations"].is_object()) {
        manifest["linked_observations"] = nlohmann::json::object();
    }
    manifest["linked_observations"]["accepted_top_rim_observation"] = link;
    if (!manifest.contains("summary") || !manifest["summary"].is_object()) {
        manifest["summary"] = nlohmann::json::object();
    }
    manifest["summary"]["accepted_top_rim_observation_artifact_id"] =
        write_result.artifact_id;
    manifest["summary"]["accepted_top_rim_observation_fingerprint"] =
        write_result.fingerprint;
    manifest["summary"]["accepted_top_rim_observation_created_utc"] =
        job.request.created_utc;
    manifest["updated_utc"] = job.request.created_utc;
    if (!manifest.contains("calibration_ref") ||
        !manifest["calibration_ref"].is_object()) {
        manifest["calibration_ref"] = {
            {"artifact_id", associated_image_set_artifact_id},
            {"artifact_schema_id", orange::calibration::kCalibrationImageSetSchemaId},
            {"artifact_schema_version",
             orange::calibration::kCalibrationImageSetSchemaVersion}
        };
    }
    manifest["calibration_ref"]["fingerprint"] = image_set_fingerprint;
    if (!write_json_file(manifest_path, manifest, error_out)) {
        return false;
    }
    if (!update_spatial_calibration_session_index(
            job.session_dir,
            job.artifact_root_dir,
            manifest,
            error_out)) {
        return false;
    }
    return update_calibration_artifact_registry(
        job.artifact_root_dir,
        manifest,
        error_out);
}

TopRimObservationSaveResult run_top_rim_observation_save_job(TopRimObservationSaveJob job)
{
    TopRimObservationSaveResult save_result;
    try {
        orange::calibration::DishTopRimObservationWriteResult write_result;
        if (!orange::calibration::write_dish_top_rim_observation_artifact(
                job.artifact_root_dir,
                job.request,
                job.source_gray,
                job.hough_params,
                job.accepted_circle,
                &write_result,
                &save_result.error)) {
            save_result.ok = false;
            if (save_result.error.empty()) {
                save_result.error = "Top-rim observation save failed.";
            }
            return save_result;
        }

        const orange::calibration::DishTopRimObservationArtifactPaths paths =
            orange::calibration::make_dish_top_rim_observation_artifact_paths(
                job.artifact_root_dir,
                write_result.artifact_id);
        if (!update_spatial_calibration_session_index(
                job.session_dir,
                job.artifact_root_dir,
                write_result.manifest,
                &save_result.error)) {
            save_result.ok = false;
            return save_result;
        }
        if (!link_top_rim_observation_to_camera_arena_aggregate(
                job,
                write_result,
                &save_result.error)) {
            save_result.ok = false;
            return save_result;
        }
        save_result.ok = true;
        save_result.status =
            "Saved top-rim observation to " + write_result.artifact_dir +
            ", image-set companion to " + paths.image_set_json_path +
            " and spatial dish-mask runtime export to " +
            paths.spatial_dish_mask_runtime_export_path;
    } catch (const std::exception& ex) {
        save_result.ok = false;
        save_result.error = std::string("Top-rim observation save threw: ") + ex.what();
    } catch (...) {
        save_result.ok = false;
        save_result.error = "Top-rim observation save threw an unknown exception.";
    }
    return save_result;
}

class TopRimObservationSaveWorker {
public:
    TopRimObservationSaveWorker()
        : worker_thread_(&TopRimObservationSaveWorker::thread_main, this)
    {
    }

    ~TopRimObservationSaveWorker()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_requested_ = true;
        }
        cv_.notify_all();
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
    }

    TopRimObservationSaveWorker(const TopRimObservationSaveWorker&) = delete;
    TopRimObservationSaveWorker& operator=(const TopRimObservationSaveWorker&) = delete;

    bool Submit(TopRimObservationSaveJob job, std::string* error_out)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_requested_) {
            if (error_out) {
                *error_out = "Top-rim save worker is stopping.";
            }
            return false;
        }
        if (running_ || queued_job_.has_value()) {
            if (error_out) {
                *error_out = "A top-rim observation save is already running.";
            }
            return false;
        }
        queued_job_ = std::move(job);
        running_ = true;
        cv_.notify_one();
        return true;
    }

    bool IsBusy() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return running_ || queued_job_.has_value();
    }

    bool PopCompleted(TopRimObservationSaveResult* result_out)
    {
        if (result_out == nullptr) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (completed_results_.empty()) {
            return false;
        }
        *result_out = std::move(completed_results_.front());
        completed_results_.pop_front();
        return true;
    }

private:
    void thread_main()
    {
        for (;;) {
            std::optional<TopRimObservationSaveJob> job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] {
                    return stop_requested_ || queued_job_.has_value();
                });
                if (stop_requested_ && !queued_job_.has_value()) {
                    return;
                }
                job = std::move(queued_job_);
                queued_job_.reset();
            }

            TopRimObservationSaveResult result =
                run_top_rim_observation_save_job(std::move(*job));

            {
                std::lock_guard<std::mutex> lock(mutex_);
                running_ = false;
                completed_results_.push_back(std::move(result));
            }
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_thread_;
    bool stop_requested_ = false;
    bool running_ = false;
    std::optional<TopRimObservationSaveJob> queued_job_;
    std::deque<TopRimObservationSaveResult> completed_results_;
};

TopRimObservationSaveWorker& top_rim_observation_save_worker()
{
    static TopRimObservationSaveWorker worker;
    return worker;
}

void poll_top_rim_observation_save_worker(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr) {
        return;
    }
    TopRimObservationSaveResult result;
    while (top_rim_observation_save_worker().PopCompleted(&result)) {
        if (result.ok) {
            ui_state->persistence_status = result.status;
            ui_state->persistence_error.clear();
        } else {
            ui_state->persistence_error =
                result.error.empty() ? "Top-rim observation save failed." : result.error;
            ui_state->persistence_status.clear();
        }
    }
}

struct GenericCalibrationImageSetSaveJob {
    std::string artifact_root_dir;
    std::string session_dir;
    std::string image_role;
    std::string image_description;
    std::string capture_filename;
    cv::Mat source_gray;
    orange::calibration::CalibrationImageSetRequest request;
};

struct GenericCalibrationImageSetSaveResult {
    bool ok = false;
    std::string status;
    std::string error;
};

nlohmann::json make_generic_calibration_image_set_manifest(
    const orange::calibration::CalibrationImageSetRequest& request,
    const GenericCalibrationImageSetFiles& files,
    const nlohmann::json& image_set,
    const std::string& image_set_fingerprint)
{
    nlohmann::json available_purposes = nlohmann::json::array();
    if (image_set.contains("available_purposes") &&
        image_set["available_purposes"].is_array()) {
        available_purposes = image_set["available_purposes"];
    }
    const int image_count =
        image_set.contains("images") && image_set["images"].is_array()
            ? static_cast<int>(image_set["images"].size())
            : 0;
    const std::string latest_source_frame =
        files.source_frame_relative_path.generic_string();
    return {
        {"schema_id", kCalibrationManifestSchemaId},
        {"schema_version", kCalibrationManifestSchemaVersion},
        {"artifact_id", request.artifact_id},
        {"artifact_schema_id", orange::calibration::kCalibrationImageSetSchemaId},
        {"artifact_schema_version", orange::calibration::kCalibrationImageSetSchemaVersion},
        {"created_utc", image_set.value("created_utc", request.created_utc)},
        {"updated_utc", request.created_utc},
        {"producer", "orange_spatial_layout_ui"},
        {"calibration_ref", {
            {"artifact_id", request.artifact_id},
            {"artifact_schema_id", orange::calibration::kCalibrationImageSetSchemaId},
            {"artifact_schema_version", orange::calibration::kCalibrationImageSetSchemaVersion},
            {"fingerprint", image_set_fingerprint}
        }},
        {"files", {
            {"image_set_json", files.image_set_path.filename().generic_string()},
            {"latest_source_frame", latest_source_frame},
            {"captures_dir", "captures"}
        }},
        {"summary", {
            {"purpose", "camera_arena_calibration_set"},
            {"target_plane", "multiple"},
            {"latest_purpose", request.purpose},
            {"latest_target_plane", request.target_plane},
            {"available_purposes", available_purposes},
            {"image_count", image_count},
            {"coordinate_space", request.coordinate_space},
            {"camera_serial", request.camera.serial},
            {"capture_mode", request.capture.capture_mode},
            {"capture_group_id", request.capture.capture_group_id}
        }}
    };
}

nlohmann::json make_generic_calibration_image_set_image_entry(
    const orange::calibration::CalibrationImageSetRequest& request,
    const orange::calibration::CalibrationImageSetImageRef& image)
{
    orange::calibration::CalibrationImageSetRequest single_image_request = request;
    single_image_request.images.clear();
    single_image_request.images.push_back(image);
    const nlohmann::json single_image_set =
        orange::calibration::calibration_image_set_to_json(single_image_request);

    nlohmann::json entry = single_image_set["images"].at(0);
    entry["purpose"] = request.purpose;
    entry["target_plane"] = request.target_plane;
    entry["capture"] = single_image_set.value("capture", nlohmann::json::object());
    if (!request.projected_pattern.empty()) {
        entry["projected_pattern"] = request.projected_pattern;
    }
    if (!request.scale_target.empty()) {
        entry["scale_target"] = request.scale_target;
    }
    if (!request.runtime_role.empty()) {
        entry["runtime_role"] = request.runtime_role;
    }
    if (!request.observations.empty()) {
        entry["observations"] = request.observations;
    }
    if (!request.operator_notes.empty()) {
        entry["operator_notes"] = request.operator_notes;
    }
    return entry;
}

nlohmann::json make_empty_aggregate_calibration_image_set(
    const orange::calibration::CalibrationImageSetRequest& request)
{
    orange::calibration::CalibrationImageSetRequest aggregate_request = request;
    aggregate_request.purpose = "camera_arena_calibration_set";
    aggregate_request.target_plane = "multiple";
    aggregate_request.images.clear();
    aggregate_request.projected_pattern = nlohmann::json::object();
    aggregate_request.scale_target = nlohmann::json::object();
    aggregate_request.runtime_role = nlohmann::json::object();
    aggregate_request.observations = nlohmann::json::object();
    aggregate_request.review_artifacts = nlohmann::json::object();
    aggregate_request.operator_notes.clear();

    nlohmann::json image_set =
        orange::calibration::calibration_image_set_to_json(aggregate_request);
    image_set["images"] = nlohmann::json::array();
    image_set["description"] =
        "Session-scoped camera/arena calibration image set assembled by Orange Spatial Layout.";
    return image_set;
}

void refresh_aggregate_calibration_image_set_summary(nlohmann::json* image_set)
{
    if (image_set == nullptr) {
        return;
    }
    if (!image_set->contains("images") || !(*image_set)["images"].is_array()) {
        (*image_set)["images"] = nlohmann::json::array();
    }

    nlohmann::json available_purposes = nlohmann::json::array();
    for (const auto& image : (*image_set)["images"]) {
        const std::string purpose = image.value("purpose", "");
        if (purpose.empty()) {
            continue;
        }
        bool already_present = false;
        for (const auto& existing : available_purposes) {
            if (existing.is_string() && existing.get<std::string>() == purpose) {
                already_present = true;
                break;
            }
        }
        if (!already_present) {
            available_purposes.push_back(purpose);
        }
    }

    (*image_set)["purpose"] = "camera_arena_calibration_set";
    (*image_set)["target_plane"] = "multiple";
    (*image_set)["available_purposes"] = available_purposes;
    (*image_set)["image_count"] = (*image_set)["images"].size();
}

GenericCalibrationImageSetSaveResult run_generic_calibration_image_set_save_job(
    GenericCalibrationImageSetSaveJob job)
{
    GenericCalibrationImageSetSaveResult result;
    try {
        const GenericCalibrationImageSetFiles files =
            make_generic_calibration_image_set_files(
                job.artifact_root_dir,
                job.request.artifact_id,
                job.capture_filename);
        if (!write_image_file(files.source_frame_path, job.source_gray, &result.error)) {
            result.ok = false;
            return result;
        }

        const std::string source_checksum =
            compute_file_fingerprint(files.source_frame_path, &result.error);
        if (source_checksum.empty()) {
            result.ok = false;
            return result;
        }

        job.request.images.clear();
        const orange::calibration::CalibrationImageSetImageRef image_ref{
            job.image_role.empty() ? std::string("source") : job.image_role,
            files.source_frame_relative_path.generic_string(),
            kCalibrationFingerprintAlgorithm,
            source_checksum,
            "camera_native_pixels",
            orange::calibration::CalibrationImageSetShape{
                job.request.camera.image_shape.height,
                job.request.camera.image_shape.width},
            job.image_description};

        nlohmann::json image_set = nlohmann::json::object();
        if (std::filesystem::exists(files.image_set_path)) {
            if (!read_json_file(files.image_set_path, &image_set, &result.error)) {
                result.ok = false;
                return result;
            }
            if (!image_set.is_object()) {
                result.ok = false;
                result.error = "Existing image_set.json is not a JSON object: " +
                               files.image_set_path.generic_string();
                return result;
            }
        } else {
            image_set = make_empty_aggregate_calibration_image_set(job.request);
        }
        if (!image_set.contains("created_utc") ||
            !image_set["created_utc"].is_string() ||
            image_set["created_utc"].get<std::string>().empty()) {
            image_set["created_utc"] = job.request.created_utc;
        }
        image_set["schema_id"] = orange::calibration::kCalibrationImageSetSchemaId;
        image_set["schema_version"] = orange::calibration::kCalibrationImageSetSchemaVersion;
        image_set["artifact_id"] = job.request.artifact_id;
        image_set["coordinate_space"] = job.request.coordinate_space;
        image_set.erase("projected_pattern");
        image_set.erase("scale_target");
        image_set.erase("runtime_role");
        image_set.erase("observations");
        image_set.erase("operator_notes");
        image_set["updated_utc"] = job.request.created_utc;
        image_set["camera"] =
            orange::calibration::calibration_image_set_to_json(job.request).at("camera");
        if (!job.request.rig_context.empty()) {
            image_set["rig_context"] = job.request.rig_context;
        }
        image_set["capture"] =
            orange::calibration::calibration_image_set_to_json(job.request).at("capture");
        image_set["latest_capture"] = {
            {"purpose", job.request.purpose},
            {"target_plane", job.request.target_plane},
            {"path", files.source_frame_relative_path.generic_string()},
            {"timestamp_utc", job.request.capture.timestamp_utc}
        };
        if (!job.request.capture.capture_group_id.empty()) {
            image_set["latest_capture"]["capture_group_id"] =
                job.request.capture.capture_group_id;
        }
        if (!image_set.contains("images") || !image_set["images"].is_array()) {
            image_set["images"] = nlohmann::json::array();
        }
        image_set["images"].push_back(
            make_generic_calibration_image_set_image_entry(job.request, image_ref));
        refresh_aggregate_calibration_image_set_summary(&image_set);

        if (!write_json_file(files.image_set_path, image_set, &result.error)) {
            result.ok = false;
            return result;
        }

        const std::string image_set_fingerprint =
            compute_json_fingerprint(image_set);
        const nlohmann::json manifest =
            make_generic_calibration_image_set_manifest(
                job.request,
                files,
                image_set,
                image_set_fingerprint);
        if (!write_json_file(files.manifest_path, manifest, &result.error)) {
            result.ok = false;
            return result;
        }
        if (!update_spatial_calibration_session_index(
                job.session_dir,
                job.artifact_root_dir,
                manifest,
                &result.error)) {
            result.ok = false;
            return result;
        }
        if (!update_calibration_artifact_registry(job.artifact_root_dir, manifest, &result.error)) {
            result.ok = false;
            return result;
        }

        result.ok = true;
        result.status =
            "Saved " + job.request.purpose + " capture to " +
            files.source_frame_path.generic_string() +
            " and updated image_set.json (" +
            std::to_string(image_set.value("image_count", 0)) + " images).";
    } catch (const std::exception& ex) {
        result.ok = false;
        result.error = std::string("Calibration image-set save threw: ") + ex.what();
    } catch (...) {
        result.ok = false;
        result.error = "Calibration image-set save threw an unknown exception.";
    }
    return result;
}

class GenericCalibrationImageSetSaveWorker {
public:
    GenericCalibrationImageSetSaveWorker()
        : worker_thread_(&GenericCalibrationImageSetSaveWorker::thread_main, this)
    {
    }

    ~GenericCalibrationImageSetSaveWorker()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_requested_ = true;
        }
        cv_.notify_all();
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
    }

    GenericCalibrationImageSetSaveWorker(const GenericCalibrationImageSetSaveWorker&) = delete;
    GenericCalibrationImageSetSaveWorker& operator=(const GenericCalibrationImageSetSaveWorker&) = delete;

    bool Submit(GenericCalibrationImageSetSaveJob job, std::string* error_out)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_requested_) {
            if (error_out) {
                *error_out = "Calibration image-set save worker is stopping.";
            }
            return false;
        }
        if (running_ || queued_job_.has_value()) {
            if (error_out) {
                *error_out = "A calibration image-set save is already running.";
            }
            return false;
        }
        queued_job_ = std::move(job);
        running_ = true;
        cv_.notify_one();
        return true;
    }

    bool IsBusy() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return running_ || queued_job_.has_value();
    }

    bool PopCompleted(GenericCalibrationImageSetSaveResult* result_out)
    {
        if (result_out == nullptr) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (completed_results_.empty()) {
            return false;
        }
        *result_out = std::move(completed_results_.front());
        completed_results_.pop_front();
        return true;
    }

private:
    void thread_main()
    {
        for (;;) {
            std::optional<GenericCalibrationImageSetSaveJob> job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] {
                    return stop_requested_ || queued_job_.has_value();
                });
                if (stop_requested_ && !queued_job_.has_value()) {
                    return;
                }
                job = std::move(queued_job_);
                queued_job_.reset();
            }

            GenericCalibrationImageSetSaveResult result =
                run_generic_calibration_image_set_save_job(std::move(*job));

            {
                std::lock_guard<std::mutex> lock(mutex_);
                running_ = false;
                completed_results_.push_back(std::move(result));
            }
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_thread_;
    bool stop_requested_ = false;
    bool running_ = false;
    std::optional<GenericCalibrationImageSetSaveJob> queued_job_;
    std::deque<GenericCalibrationImageSetSaveResult> completed_results_;
};

GenericCalibrationImageSetSaveWorker& generic_calibration_image_set_save_worker()
{
    static GenericCalibrationImageSetSaveWorker worker;
    return worker;
}

std::deque<GenericCalibrationImageSetSaveJob>& queued_generic_calibration_image_set_save_jobs()
{
    static std::deque<GenericCalibrationImageSetSaveJob> jobs;
    return jobs;
}

size_t queued_generic_calibration_image_set_save_job_count()
{
    return queued_generic_calibration_image_set_save_jobs().size();
}

bool submit_next_queued_generic_calibration_image_set_save_job(std::string* error_out)
{
    auto& queue = queued_generic_calibration_image_set_save_jobs();
    if (queue.empty() || generic_calibration_image_set_save_worker().IsBusy()) {
        return true;
    }

    GenericCalibrationImageSetSaveJob job = std::move(queue.front());
    queue.pop_front();
    if (!generic_calibration_image_set_save_worker().Submit(std::move(job), error_out)) {
        return false;
    }
    return true;
}

void poll_generic_calibration_image_set_save_worker(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr) {
        return;
    }
    std::string submit_error;
    if (!submit_next_queued_generic_calibration_image_set_save_job(&submit_error)) {
        ui_state->persistence_error = submit_error;
        ui_state->persistence_status.clear();
    }
    GenericCalibrationImageSetSaveResult result;
    while (generic_calibration_image_set_save_worker().PopCompleted(&result)) {
        if (result.ok) {
            ui_state->persistence_status = result.status;
            ui_state->persistence_error.clear();
        } else {
            ui_state->persistence_error =
                result.error.empty() ? "Calibration image-set save failed." : result.error;
            ui_state->persistence_status.clear();
        }
        if (!submit_next_queued_generic_calibration_image_set_save_job(&submit_error)) {
            ui_state->persistence_error = submit_error;
            ui_state->persistence_status.clear();
            break;
        }
        const size_t queued = queued_generic_calibration_image_set_save_job_count();
        if (queued > 0 && ui_state->persistence_error.empty()) {
            ui_state->persistence_status +=
                " " + std::to_string(queued) + " grouped save job(s) remain queued.";
        }
    }
}

void apply_calibration_image_set_purpose_defaults(
    SpatialLayoutUiState* ui_state,
    const std::string& purpose)
{
    if (ui_state == nullptr) {
        return;
    }
    ui_state->calibration_image_set_purpose = purpose;
    if (purpose == "homography_grid") {
        ui_state->calibration_filter_state = kHoyaR72FilterRemoved;
        ui_state->calibration_runtime_filter_state = kHoyaR72FilterInstalled;
        ui_state->calibration_image_set_target_plane = "projected_surface";
        ui_state->calibration_image_set_image_role = "grid_on";
        ui_state->calibration_image_set_projected_pattern_id = "citrus_homography_grid_v1";
        ui_state->calibration_image_set_projected_pattern_type = "dot_grid";
        ui_state->calibration_light_handling = "suppress_mapped_strobe";
        apply_illumination_preset(ui_state, "visible_projector_broadband");
        ui_state->calibration_projector_state = "homography_grid_on";
        ui_state->calibration_projector_visible_to_camera = true;
    } else if (purpose == "arena_projection") {
        ui_state->calibration_filter_state = kHoyaR72FilterRemoved;
        ui_state->calibration_runtime_filter_state = kHoyaR72FilterInstalled;
        ui_state->calibration_image_set_target_plane = "projected_surface";
        ui_state->calibration_image_set_image_role = "projected_arena";
        ui_state->calibration_image_set_projected_pattern_id = "citrus_arena_projection";
        ui_state->calibration_image_set_projected_pattern_type = "arena_fill";
        ui_state->calibration_light_handling = "suppress_mapped_strobe";
        apply_illumination_preset(ui_state, "visible_projector_broadband");
        ui_state->calibration_projector_state = "normal_stimulus_active";
        ui_state->calibration_projector_visible_to_camera = true;
    } else if (purpose == "scale_image") {
        ui_state->calibration_filter_state = kHoyaR72FilterInstalled;
        ui_state->calibration_runtime_filter_state = kHoyaR72FilterInstalled;
        ui_state->calibration_image_set_target_plane = "estimated_fish_plane";
        ui_state->calibration_image_set_image_role = "scale_target";
        ui_state->calibration_image_set_projected_pattern_id = "none";
        ui_state->calibration_image_set_projected_pattern_type = "none";
        ui_state->calibration_image_set_scale_target_type = "clear_plastic_ruler";
        ui_state->calibration_light_handling = "keep_or_restore_mapped_pulse";
        apply_illumination_preset(ui_state, "custom_ttl_nir_strobe_855nm");
        ui_state->calibration_projector_state = "off";
        ui_state->calibration_projector_visible_to_camera = false;
    } else if (purpose == "crosshair_alignment") {
        ui_state->calibration_filter_state = kHoyaR72FilterRemoved;
        ui_state->calibration_runtime_filter_state = kHoyaR72FilterInstalled;
        ui_state->calibration_image_set_target_plane = "estimated_fish_plane";
        ui_state->calibration_image_set_image_role = "crosshair_on";
        ui_state->calibration_image_set_projected_pattern_id = "citrus_crosshair_alignment";
        ui_state->calibration_image_set_projected_pattern_type = "crosshair";
        ui_state->calibration_light_handling = "suppress_mapped_strobe";
        apply_illumination_preset(ui_state, "visible_projector_broadband");
        ui_state->calibration_projector_state = "crosshair_on";
        ui_state->calibration_projector_visible_to_camera = true;
    }
}

void apply_projection_surface_scale_image_defaults(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr) {
        return;
    }
    ui_state->calibration_image_set_purpose = "scale_image";
    ui_state->calibration_filter_state = kHoyaR72FilterInstalled;
    ui_state->calibration_runtime_filter_state = kHoyaR72FilterInstalled;
    ui_state->calibration_image_set_target_plane = "projected_surface";
    ui_state->calibration_image_set_image_role = "scale_target";
    ui_state->calibration_image_set_projected_pattern_id = "none";
    ui_state->calibration_image_set_projected_pattern_type = "none";
    ui_state->calibration_image_set_scale_target_type = "clear_plastic_ruler";
    ui_state->calibration_light_handling = "keep_or_restore_mapped_pulse";
    apply_illumination_preset(ui_state, "custom_ttl_nir_strobe_855nm");
    ui_state->calibration_projector_state = "off";
    ui_state->calibration_projector_visible_to_camera = false;
}

void apply_tank_bottom_inner_surface_scale_image_defaults(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr) {
        return;
    }
    ui_state->calibration_image_set_purpose = "scale_image";
    ui_state->calibration_filter_state = kHoyaR72FilterInstalled;
    ui_state->calibration_runtime_filter_state = kHoyaR72FilterInstalled;
    ui_state->calibration_image_set_target_plane = "tank_bottom_inner_surface";
    ui_state->calibration_image_set_image_role = "scale_target";
    ui_state->calibration_image_set_projected_pattern_id = "none";
    ui_state->calibration_image_set_projected_pattern_type = "none";
    ui_state->calibration_image_set_scale_target_type = "clear_plastic_ruler";
    ui_state->calibration_light_handling = "keep_or_restore_mapped_pulse";
    apply_illumination_preset(ui_state, "custom_ttl_nir_strobe_855nm");
    ui_state->calibration_projector_state = "off";
    ui_state->calibration_projector_visible_to_camera = false;
}

void apply_tank_bottom_inner_surface_homography_defaults(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr) {
        return;
    }
    ui_state->calibration_image_set_purpose = "homography_grid";
    ui_state->calibration_filter_state = kHoyaR72FilterRemoved;
    ui_state->calibration_runtime_filter_state = kHoyaR72FilterInstalled;
    ui_state->calibration_image_set_target_plane = "tank_bottom_inner_surface";
    ui_state->calibration_image_set_image_role = "grid_on";
    ui_state->calibration_image_set_projected_pattern_id =
        "citrus_tank_bottom_circular_grid_v1";
    ui_state->calibration_image_set_projected_pattern_type = "circular_dot_grid";
    ui_state->calibration_image_set_scale_target_type = "unknown";
    ui_state->calibration_light_handling = "suppress_mapped_strobe";
    apply_illumination_preset(ui_state, "visible_projector_broadband");
    ui_state->calibration_projector_state = "tank_bottom_homography_grid_on";
    ui_state->calibration_projector_visible_to_camera = true;
}

bool is_projection_surface_workflow_purpose(const SpatialLayoutUiState& ui_state)
{
    if (ui_state.calibration_image_set_purpose == "arena_projection" ||
        (ui_state.calibration_image_set_purpose == "homography_grid" &&
         ui_state.calibration_image_set_target_plane != "estimated_fish_plane" &&
         ui_state.calibration_image_set_target_plane != "tank_bottom_inner_surface")) {
        return true;
    }
    return ui_state.calibration_image_set_purpose == "scale_image" &&
           ui_state.calibration_image_set_target_plane == "projected_surface";
}

bool is_estimated_fish_plane_workflow_purpose(const SpatialLayoutUiState& ui_state)
{
    if (ui_state.calibration_image_set_purpose == "crosshair_alignment") {
        return true;
    }
    if (ui_state.calibration_image_set_purpose == "homography_grid" &&
        (ui_state.calibration_image_set_target_plane == "estimated_fish_plane" ||
         ui_state.calibration_image_set_target_plane == "tank_bottom_inner_surface")) {
        return true;
    }
    return ui_state.calibration_image_set_purpose == "scale_image" &&
           (ui_state.calibration_image_set_target_plane == "estimated_fish_plane" ||
            ui_state.calibration_image_set_target_plane == "tank_bottom_inner_surface");
}

void apply_calibration_workflow_tab_defaults(SpatialLayoutUiState* ui_state, const int tab)
{
    if (ui_state == nullptr) {
        return;
    }
    if (tab == 0) {
        if (!is_projection_surface_workflow_purpose(*ui_state)) {
            apply_calibration_image_set_purpose_defaults(ui_state, "homography_grid");
        }
    } else if (tab == 1) {
        if (!is_estimated_fish_plane_workflow_purpose(*ui_state)) {
            apply_tank_bottom_inner_surface_scale_image_defaults(ui_state);
        }
    } else if (tab == 2) {
        ui_state->calibration_light_handling = "keep_or_restore_mapped_pulse";
        apply_illumination_preset(ui_state, "custom_ttl_nir_strobe_855nm");
        ui_state->calibration_projector_state = "off";
        ui_state->calibration_projector_visible_to_camera = false;
    }
}

bool prepare_dish_top_rim_observation_save_job_from_spatial_layout(
    SpatialLayoutUiState* ui_state,
    const CameraParams& selected_camera,
    const std::string& artifact_root_dir,
    TopRimObservationSaveJob* job_out,
    std::string* error_out)
{
    if (job_out == nullptr) {
        if (error_out) {
            *error_out = "Top-rim save job destination is null.";
        }
        return false;
    }
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
    if (!ui_state->dish_mask_runtime.has_geometry) {
        if (error_out) {
            *error_out = "Resolved dish-mask geometry is not available yet.";
        }
        return false;
    }
    const std::string source_array_role =
        ui_state->captured_source_array_role.empty()
            ? "images_full"
            : ui_state->captured_source_array_role;
    if (source_array_role != "images_full") {
        if (error_out) {
            *error_out =
                "Top-rim observations must be saved in full-resolution camera coordinates. "
                "The current capture is a downsampled live preview; recapture with stream downsample=1 "
                "or use a full-resolution capture path.";
        }
        return false;
    }

    orange::calibration::DishTopRimCircle accepted_circle;
    if (!runtime_geometry_to_top_rim_circle(
            ui_state->dish_mask_runtime.geometry.outer_geometry,
            &accepted_circle,
            error_out)) {
        return false;
    }

    cv::Mat source_gray;
    if (!captured_frame_to_gray8(*ui_state, &source_gray, error_out)) {
        return false;
    }

    TopRimObservationSaveJob job;
    job.artifact_root_dir = artifact_root_dir;
    job.accepted_circle = accepted_circle;
    job.source_gray = std::move(source_gray);

    const std::string timestamp = get_current_utc_timestamp();
    auto& request = job.request;
    request.artifact_id =
        orange::calibration::build_dish_top_rim_observation_artifact_id(
            selected_camera.camera_serial,
            timestamp);
    request.created_utc = timestamp;
    request.camera.serial = selected_camera.camera_serial;
    request.camera.name = selected_camera.camera_name;
    request.camera.width = ui_state->captured_texture_width;
    request.camera.height = ui_state->captured_texture_height;
    request.camera.pixel_format = selected_camera.pixel_format.empty()
                                      ? "captured_rgba_converted_to_gray8"
                                      : selected_camera.pixel_format;
    request.capture.operation_id = "spatial_layout_top_rim_" + request.artifact_id;
    request.capture.capture_mode = ui_state->captured_capture_mode.empty()
                                       ? "session_local_operator_still"
                                       : ui_state->captured_capture_mode;
    apply_captured_frame_provenance_to_capture(*ui_state, &request.capture);
    const auto metadata_or_unknown = [](const std::string& value) {
        return value.empty() ? std::string("unknown") : value;
    };
    request.capture.filter_state = metadata_or_unknown(ui_state->calibration_filter_state);
    request.capture.runtime_filter_state =
        metadata_or_unknown(ui_state->calibration_runtime_filter_state);
    request.capture.light_handling = metadata_or_unknown(ui_state->calibration_light_handling);
    request.capture.light_state = metadata_or_unknown(ui_state->calibration_light_state);
    request.capture.illumination_spectrum =
        metadata_or_unknown(ui_state->calibration_illumination_spectrum);
    request.capture.illumination_source =
        metadata_or_unknown(ui_state->calibration_illumination_source);
    request.capture.illumination_center_wavelength_nm =
        ui_state->calibration_illumination_center_wavelength_nm;
    request.capture.has_illumination_center_wavelength_nm =
        ui_state->calibration_has_illumination_center_wavelength_nm;
    request.capture.illumination_min_wavelength_nm =
        ui_state->calibration_illumination_min_wavelength_nm;
    request.capture.has_illumination_min_wavelength_nm =
        ui_state->calibration_has_illumination_min_wavelength_nm;
    request.capture.illumination_max_wavelength_nm =
        ui_state->calibration_illumination_max_wavelength_nm;
    request.capture.has_illumination_max_wavelength_nm =
        ui_state->calibration_has_illumination_max_wavelength_nm;
    request.capture.illumination_bandwidth_fwhm_nm =
        ui_state->calibration_illumination_bandwidth_fwhm_nm;
    request.capture.has_illumination_bandwidth_fwhm_nm =
        ui_state->calibration_has_illumination_bandwidth_fwhm_nm;
    request.capture.illumination_wavelength_confidence =
        metadata_or_unknown(ui_state->calibration_illumination_wavelength_confidence);
    request.capture.projector_state = metadata_or_unknown(ui_state->calibration_projector_state);
    request.capture.projector_visible_to_camera =
        ui_state->calibration_projector_visible_to_camera;
    request.capture.exposure_us = static_cast<double>(selected_camera.exposure);
    request.capture.frame_rate_hz = static_cast<double>(selected_camera.frame_rate);
    request.capture.requires_filter_reinstalled_repeatably =
        ui_state->calibration_requires_filter_reinstalled_repeatably;
    request.source_array_role = source_array_role;
    request.source_frame_index = 0;
    request.valid_region_erosion_px = std::max(0.0, ui_state->edge_margin_px);
    request.operator_confirmed = true;
    request.operator_status = "orange_spatial_layout_ui_confirmed";
    request.operator_notes = ui_state->calibration_operator_notes;
    request.runtime_verification.status = "unknown";
    request.runtime_verification.reason = "runtime_850nm_rim_not_verified";
    request.write_palette_export = true;
    request.arena_context = {
        {"camera_serial", selected_camera.camera_serial},
        {"associated_image_set_artifact_id",
         build_camera_arena_calibration_image_set_artifact_id(ui_state, selected_camera)}
    };
    if (ui_state->citrus_template.available) {
        nlohmann::json rig_context = nlohmann::json::object();
        if (!ui_state->citrus_template.source_rig_name.empty()) {
            rig_context["rig_id"] = ui_state->citrus_template.source_rig_name;
            request.arena_context["rig_id"] =
                ui_state->citrus_template.source_rig_name;
        }
        if (!ui_state->citrus_template.source_canvas_name.empty()) {
            rig_context["canvas_id"] = ui_state->citrus_template.source_canvas_name;
            request.arena_context["canvas_id"] =
                ui_state->citrus_template.source_canvas_name;
        }
        if (!ui_state->citrus_template.source_arena_name.empty()) {
            rig_context["arena_id"] = ui_state->citrus_template.source_arena_name;
            request.arena_context["arena_id"] =
                ui_state->citrus_template.source_arena_name;
        }
        if (!ui_state->citrus_template.source_camera_id.empty()) {
            rig_context["camera_id"] = ui_state->citrus_template.source_camera_id;
            request.arena_context["citrus_camera_id"] =
                ui_state->citrus_template.source_camera_id;
        }
        if (!ui_state->citrus_template.source_config_path.empty() ||
            !ui_state->citrus_template.source_config_name.empty()) {
            rig_context["citrus_config_ref"] = {
                {"source", "spatial_layout_import"},
                {"path", ui_state->citrus_template.source_config_path},
                {"config_name", ui_state->citrus_template.source_config_name}
            };
            request.arena_context["citrus_config_ref"] =
                rig_context["citrus_config_ref"];
        }
        if (ui_state->citrus_template.has_camera_to_canvas_homography) {
            rig_context["citrus_homography_ref"] = {
                {"available", true},
                {"source", "citrus_homography_sidecar"},
                {"direction", "camera_view_px_to_final_display_canvas_px"}
            };
            request.arena_context["citrus_homography_ref"] =
                rig_context["citrus_homography_ref"];
        }
        rig_context["associated_image_set_artifact_id"] =
            request.arena_context["associated_image_set_artifact_id"];
        request.image_set_rig_context = rig_context;
    }

    job.hough_params = make_top_rim_hough_params(
        *ui_state,
        accepted_circle,
        ui_state->captured_texture_width,
        ui_state->captured_texture_height);

    *job_out = std::move(job);
    return true;
}

bool prepare_generic_calibration_image_set_save_job_from_spatial_layout(
    SpatialLayoutUiState* ui_state,
    const CameraParams& selected_camera,
    const std::string& artifact_root_dir,
    GenericCalibrationImageSetSaveJob* job_out,
    std::string* error_out)
{
    if (job_out == nullptr) {
        if (error_out) {
            *error_out = "Calibration image-set save job destination is null.";
        }
        return false;
    }
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
    const std::string source_array_role =
        ui_state->captured_source_array_role.empty()
            ? "images_full"
            : ui_state->captured_source_array_role;
    if (source_array_role != "images_full") {
        if (error_out) {
            *error_out =
                "Calibration image sets must be saved in full-resolution camera coordinates. "
                "Use Capture Full-Resolution Stream Snapshot before saving this artifact.";
        }
        return false;
    }

    cv::Mat source_gray;
    if (!captured_frame_to_gray8(*ui_state, &source_gray, error_out)) {
        return false;
    }

    const auto metadata_or_unknown = [](const std::string& value) {
        return value.empty() ? std::string("unknown") : value;
    };

    const std::string timestamp = get_current_utc_timestamp();
    GenericCalibrationImageSetSaveJob job;
    job.artifact_root_dir = artifact_root_dir;
    job.image_role =
        ui_state->calibration_image_set_image_role.empty()
            ? "source"
            : ui_state->calibration_image_set_image_role;
    job.image_description =
        "full-resolution source frame for " +
        metadata_or_unknown(ui_state->calibration_image_set_purpose);
    job.capture_filename = build_calibration_capture_filename(
        ui_state->calibration_image_set_purpose,
        timestamp);
    job.source_gray = std::move(source_gray);

    auto& request = job.request;
    request.artifact_id =
        build_camera_arena_calibration_image_set_artifact_id(ui_state, selected_camera);
    request.created_utc = timestamp;
    request.purpose = metadata_or_unknown(ui_state->calibration_image_set_purpose);
    request.target_plane = metadata_or_unknown(ui_state->calibration_image_set_target_plane);
    request.coordinate_space = "camera_native_pixels";
    request.camera.serial = selected_camera.camera_serial;
    request.camera.name = selected_camera.camera_name;
    request.camera.image_shape.height = ui_state->captured_texture_height;
    request.camera.image_shape.width = ui_state->captured_texture_width;
    request.camera.pixel_format = selected_camera.pixel_format.empty()
                                      ? "captured_rgba_converted_to_gray8"
                                      : selected_camera.pixel_format;
    request.camera.configured_height = selected_camera.height;
    request.camera.configured_width = selected_camera.width;

    request.capture.operation_id =
        "spatial_layout_image_set_" + request.artifact_id +
        "_" + sanitize_artifact_component(request.purpose) +
        "_" + sanitize_artifact_component(timestamp);
    request.capture.timestamp_utc = timestamp;
    request.capture.capture_mode = ui_state->captured_capture_mode.empty()
                                       ? "session_local_operator_still"
                                       : ui_state->captured_capture_mode;
    apply_captured_frame_provenance_to_capture(*ui_state, &request.capture);
    request.capture.exposure_us = static_cast<double>(selected_camera.exposure);
    request.capture.has_exposure_us = true;
    request.capture.frame_rate_hz = static_cast<double>(selected_camera.frame_rate);
    request.capture.has_frame_rate_hz = true;
    request.capture.filter_state = metadata_or_unknown(ui_state->calibration_filter_state);
    request.capture.runtime_filter_state =
        metadata_or_unknown(ui_state->calibration_runtime_filter_state);
    request.capture.light_handling = metadata_or_unknown(ui_state->calibration_light_handling);
    request.capture.light_state = metadata_or_unknown(ui_state->calibration_light_state);
    request.capture.illumination_spectrum =
        metadata_or_unknown(ui_state->calibration_illumination_spectrum);
    request.capture.illumination_source =
        metadata_or_unknown(ui_state->calibration_illumination_source);
    request.capture.illumination_center_wavelength_nm =
        ui_state->calibration_illumination_center_wavelength_nm;
    request.capture.has_illumination_center_wavelength_nm =
        ui_state->calibration_has_illumination_center_wavelength_nm;
    request.capture.illumination_min_wavelength_nm =
        ui_state->calibration_illumination_min_wavelength_nm;
    request.capture.has_illumination_min_wavelength_nm =
        ui_state->calibration_has_illumination_min_wavelength_nm;
    request.capture.illumination_max_wavelength_nm =
        ui_state->calibration_illumination_max_wavelength_nm;
    request.capture.has_illumination_max_wavelength_nm =
        ui_state->calibration_has_illumination_max_wavelength_nm;
    request.capture.illumination_bandwidth_fwhm_nm =
        ui_state->calibration_illumination_bandwidth_fwhm_nm;
    request.capture.has_illumination_bandwidth_fwhm_nm =
        ui_state->calibration_has_illumination_bandwidth_fwhm_nm;
    request.capture.illumination_wavelength_confidence =
        metadata_or_unknown(ui_state->calibration_illumination_wavelength_confidence);
    request.capture.projector_state = metadata_or_unknown(ui_state->calibration_projector_state);
    request.capture.projector_visible_to_camera =
        ui_state->calibration_projector_visible_to_camera;
    request.capture.has_projector_visible_to_camera = true;
    request.capture.requires_camera_mount_unchanged = true;
    request.capture.has_requires_camera_mount_unchanged = true;
    request.capture.requires_filter_reinstalled_repeatably =
        ui_state->calibration_requires_filter_reinstalled_repeatably;
    request.capture.has_requires_filter_reinstalled_repeatably = true;

    if (ui_state->citrus_template.available) {
        if (!ui_state->citrus_template.source_rig_name.empty()) {
            request.rig_context["rig_id"] = ui_state->citrus_template.source_rig_name;
        }
        if (!ui_state->citrus_template.source_canvas_name.empty()) {
            request.rig_context["canvas_id"] = ui_state->citrus_template.source_canvas_name;
        }
        if (!ui_state->citrus_template.source_arena_name.empty()) {
            request.rig_context["arena_id"] = ui_state->citrus_template.source_arena_name;
        }
        if (!ui_state->citrus_template.source_camera_id.empty()) {
            request.rig_context["camera_id"] = ui_state->citrus_template.source_camera_id;
        }
        if (!ui_state->citrus_template.source_config_path.empty() ||
            !ui_state->citrus_template.source_config_name.empty()) {
            request.rig_context["citrus_config_ref"] = {
                {"source", "spatial_layout_import"},
                {"path", ui_state->citrus_template.source_config_path},
                {"config_name", ui_state->citrus_template.source_config_name}
            };
        }
        if (ui_state->citrus_template.has_camera_to_canvas_homography) {
            request.rig_context["citrus_homography_ref"] = {
                {"available", true},
                {"source", "citrus_homography_sidecar"},
                {"direction", "camera_view_px_to_final_display_canvas_px"}
            };
        }
    }

    if (request.purpose == "homography_grid" || request.purpose == "crosshair_alignment") {
        request.projected_pattern = {
            {"pattern_id", metadata_or_unknown(ui_state->calibration_image_set_projected_pattern_id)},
            {"type", metadata_or_unknown(ui_state->calibration_image_set_projected_pattern_type)},
            {"source", "operator_entered"},
            {"target_plane", request.target_plane}
        };
    }
    if (request.purpose == "scale_image") {
        request.scale_target = {
            {"target_type", metadata_or_unknown(ui_state->calibration_image_set_scale_target_type)},
            {"source", "operator_entered"},
            {"target_plane", request.target_plane}
        };
    }
    attach_runtime_role_metadata(&request);
    attach_projection_surface_authored_domain_hint(&request);
    attach_calibration_domain_observation(
        &request,
        make_calibration_image_set_metadata_from_ui(*ui_state));
    request.citrus_preview = {
        {"available", false},
        {"diagnostic_only", true},
        {"authority", "citrus_recomputes_before_acceptance"}
    };
    request.operator_notes =
        ui_state->calibration_image_set_notes.empty()
            ? ui_state->calibration_operator_notes
            : ui_state->calibration_image_set_notes;

    *job_out = std::move(job);
    return true;
}

struct SpatialLayoutCaptureStateBackup {
    bool has_capture = false;
    int captured_texture_width = 0;
    int captured_texture_height = 0;
    std::vector<unsigned char> captured_rgba;
    std::string captured_camera_serial;
    std::string captured_source_array_role;
    std::string captured_capture_mode;
    std::string captured_capture_group_id;
    uint32_t captured_source_frame_count = 1;
    uint64_t captured_first_local_frame_id = 0;
    uint64_t captured_last_local_frame_id = 0;
    uint64_t captured_first_camera_frame_id = 0;
    uint64_t captured_last_camera_frame_id = 0;
    CitrusSpatialTemplateState citrus_template;
    SpatialLayoutCalibrationImageSetMetadata metadata;
};

SpatialLayoutCaptureStateBackup backup_spatial_layout_capture_state(
    const SpatialLayoutUiState& ui_state)
{
    SpatialLayoutCaptureStateBackup backup;
    backup.has_capture = ui_state.has_capture;
    backup.captured_texture_width = ui_state.captured_texture_width;
    backup.captured_texture_height = ui_state.captured_texture_height;
    backup.captured_rgba = ui_state.captured_rgba;
    backup.captured_camera_serial = ui_state.captured_camera_serial;
    backup.captured_source_array_role = ui_state.captured_source_array_role;
    backup.captured_capture_mode = ui_state.captured_capture_mode;
    backup.captured_capture_group_id = ui_state.captured_capture_group_id;
    backup.captured_source_frame_count = ui_state.captured_source_frame_count;
    backup.captured_first_local_frame_id = ui_state.captured_first_local_frame_id;
    backup.captured_last_local_frame_id = ui_state.captured_last_local_frame_id;
    backup.captured_first_camera_frame_id = ui_state.captured_first_camera_frame_id;
    backup.captured_last_camera_frame_id = ui_state.captured_last_camera_frame_id;
    backup.citrus_template = ui_state.citrus_template;
    backup.metadata = make_calibration_image_set_metadata_from_ui(ui_state);
    return backup;
}

void restore_spatial_layout_capture_state(
    SpatialLayoutUiState* ui_state,
    SpatialLayoutCaptureStateBackup backup)
{
    if (ui_state == nullptr) {
        return;
    }
    ui_state->has_capture = backup.has_capture;
    ui_state->captured_texture_width = backup.captured_texture_width;
    ui_state->captured_texture_height = backup.captured_texture_height;
    ui_state->captured_rgba = std::move(backup.captured_rgba);
    ui_state->captured_camera_serial = std::move(backup.captured_camera_serial);
    ui_state->captured_source_array_role = std::move(backup.captured_source_array_role);
    ui_state->captured_capture_mode = std::move(backup.captured_capture_mode);
    ui_state->captured_capture_group_id = std::move(backup.captured_capture_group_id);
    ui_state->captured_source_frame_count = backup.captured_source_frame_count;
    ui_state->captured_first_local_frame_id = backup.captured_first_local_frame_id;
    ui_state->captured_last_local_frame_id = backup.captured_last_local_frame_id;
    ui_state->captured_first_camera_frame_id = backup.captured_first_camera_frame_id;
    ui_state->captured_last_camera_frame_id = backup.captured_last_camera_frame_id;
    ui_state->citrus_template = std::move(backup.citrus_template);
    apply_calibration_image_set_metadata_to_ui(ui_state, backup.metadata);
}

bool prepare_generic_calibration_image_set_save_job_from_group_capture(
    SpatialLayoutUiState* ui_state,
    const SpatialLayoutGroupCaptureFrame& capture,
    const CameraParams& camera_params,
    const std::string& artifact_root_dir,
    GenericCalibrationImageSetSaveJob* job_out,
    std::string* error_out)
{
    if (ui_state == nullptr) {
        if (error_out) {
            *error_out = "Spatial layout UI state is null.";
        }
        return false;
    }
    if (!capture.valid || capture.width <= 0 || capture.height <= 0 || capture.rgba.empty()) {
        if (error_out) {
            *error_out = "Grouped capture for " + capture.camera_serial + " is empty.";
        }
        return false;
    }
    if (capture.source_array_role != "images_full") {
        if (error_out) {
            *error_out = "Grouped capture for " + capture.camera_serial +
                         " is not in full-resolution camera coordinates.";
        }
        return false;
    }

    SpatialLayoutCaptureStateBackup backup =
        backup_spatial_layout_capture_state(*ui_state);

    ui_state->has_capture = true;
    ui_state->captured_texture_width = capture.width;
    ui_state->captured_texture_height = capture.height;
    ui_state->captured_rgba = capture.rgba;
    ui_state->captured_camera_serial = capture.camera_serial;
    ui_state->captured_source_array_role = capture.source_array_role;
    ui_state->captured_capture_mode = capture.capture_mode;
    ui_state->captured_capture_group_id = capture.capture_group_id;
    ui_state->captured_source_frame_count = std::max<uint32_t>(1u, capture.source_frame_count);
    ui_state->captured_first_local_frame_id = capture.first_local_frame_id;
    ui_state->captured_last_local_frame_id = capture.last_local_frame_id;
    ui_state->captured_first_camera_frame_id = capture.first_camera_frame_id;
    ui_state->captured_last_camera_frame_id = capture.last_camera_frame_id;
    apply_calibration_image_set_metadata_to_ui(ui_state, capture.metadata);

    bool template_ok = true;
    if (!ui_state->citrus_canvas_templates.empty()) {
        const int citrus_index =
            find_citrus_template_index_for_camera(*ui_state, capture.camera_serial);
        if (citrus_index < 0) {
            template_ok = false;
            if (error_out) {
                *error_out = "No loaded Citrus canvas template matches camera " +
                             capture.camera_serial + ".";
            }
        } else {
            ui_state->citrus_template =
                ui_state->citrus_canvas_templates[static_cast<size_t>(citrus_index)];
        }
    } else if (ui_state->citrus_template.available &&
               !ui_state->citrus_template.source_camera_id.empty() &&
               ui_state->citrus_template.source_camera_id != capture.camera_serial) {
        ui_state->citrus_template = CitrusSpatialTemplateState{};
    }

    bool ok = false;
    if (template_ok) {
        ok = prepare_generic_calibration_image_set_save_job_from_spatial_layout(
            ui_state,
            camera_params,
            artifact_root_dir,
            job_out,
            error_out);
        if (ok && job_out != nullptr) {
            if (capture.camera_configured_width > 0) {
                job_out->request.camera.configured_width = capture.camera_configured_width;
            }
            if (capture.camera_configured_height > 0) {
                job_out->request.camera.configured_height = capture.camera_configured_height;
            }
            if (!capture.camera_pixel_format.empty()) {
                job_out->request.camera.pixel_format = capture.camera_pixel_format;
            }
            if (capture.has_camera_exposure_us) {
                job_out->request.capture.exposure_us = capture.camera_exposure_us;
                job_out->request.capture.has_exposure_us = true;
            }
            if (capture.has_camera_frame_rate_hz) {
                job_out->request.capture.frame_rate_hz = capture.camera_frame_rate_hz;
                job_out->request.capture.has_frame_rate_hz = true;
            }
            if (capture.has_camera_gain) {
                job_out->request.capture.gain = capture.camera_gain;
                job_out->request.capture.has_gain = true;
            }
            attach_calibration_domain_observation(
                &job_out->request,
                capture.metadata);
        }
    }
    restore_spatial_layout_capture_state(
        ui_state,
        std::move(backup));
    return ok;
}

bool queue_group_calibration_image_set_save_jobs(
    SpatialLayoutUiState* ui_state,
    const CameraParams* cameras_params,
    int num_cameras,
    const CameraParams& selected_camera,
    const std::string& artifact_root_dir,
    std::string* status_out,
    std::string* error_out)
{
    if (ui_state == nullptr || cameras_params == nullptr || num_cameras <= 0) {
        if (error_out) {
            *error_out = "Grouped save requires open cameras.";
        }
        return false;
    }
    if (ui_state->group_captures.empty()) {
        if (error_out) {
            *error_out = "Capture grouped full-resolution snapshots before saving grouped image sets.";
        }
        return false;
    }
    if (pending_group_snapshot_count(*ui_state) > 0) {
        if (error_out) {
            *error_out = "Grouped capture is still pending.";
        }
        return false;
    }

    std::string session_artifact_root;
    if (!ensure_spatial_calibration_session(
            ui_state,
            selected_camera,
            artifact_root_dir,
            &session_artifact_root,
            error_out)) {
        return false;
    }

    std::deque<GenericCalibrationImageSetSaveJob> jobs;
    for (const SpatialLayoutGroupCaptureFrame& capture : ui_state->group_captures) {
        const int camera_index =
            capture.camera_index >= 0
                ? capture.camera_index
                : find_camera_index_by_serial(cameras_params, num_cameras, capture.camera_serial);
        if (camera_index < 0 || camera_index >= num_cameras) {
            if (error_out) {
                *error_out = "Grouped capture camera is no longer open: " +
                             capture.camera_serial;
            }
            return false;
        }
        GenericCalibrationImageSetSaveJob job;
        if (!prepare_generic_calibration_image_set_save_job_from_group_capture(
                ui_state,
                capture,
                cameras_params[camera_index],
                session_artifact_root,
                &job,
                error_out)) {
            return false;
        }
        job.session_dir = ui_state->calibration_session_dir;
        jobs.push_back(std::move(job));
    }

    auto& save_queue = queued_generic_calibration_image_set_save_jobs();
    for (GenericCalibrationImageSetSaveJob& job : jobs) {
        save_queue.push_back(std::move(job));
    }
    std::string submit_error;
    if (!submit_next_queued_generic_calibration_image_set_save_job(&submit_error)) {
        if (error_out) {
            *error_out = submit_error;
        }
        return false;
    }

    if (status_out) {
        *status_out =
            "Queued " + std::to_string(ui_state->group_captures.size()) +
            " grouped calibration image-set save job(s) in session " +
            ui_state->calibration_session_id + ".";
    }
    return true;
}

bool resolve_measurement_json_path_from_selection(
    const std::filesystem::path& selected_path,
    std::filesystem::path* measurement_path_out,
    std::string* error_out)
{
    if (measurement_path_out == nullptr) {
        if (error_out) {
            *error_out = "Null measurement-path destination.";
        }
        return false;
    }

    nlohmann::json selected_json;
    if (!read_json_file(selected_path, &selected_json, error_out)) {
        return false;
    }

    const std::string schema_id = selected_json.value("schema_id", "");
    if (schema_id == orange::spatial::kArenaLayoutArtifactSchemaId) {
        *measurement_path_out = selected_path;
        return true;
    }
    if (schema_id != kCalibrationManifestSchemaId) {
        if (error_out) {
            *error_out = "Selected JSON is neither an arena layout artifact nor a calibration manifest.";
        }
        return false;
    }

    std::string measurement_filename = kSpatialLayoutMeasurementFilename;
    if (selected_json.contains("files") &&
        selected_json["files"].is_object() &&
        selected_json["files"].contains("measurement_json") &&
        selected_json["files"]["measurement_json"].is_string()) {
        measurement_filename = selected_json["files"]["measurement_json"].get<std::string>();
    }
    *measurement_path_out = selected_path.parent_path() / measurement_filename;
    return true;
}

bool load_spatial_layout_artifact(
    SpatialLayoutUiState* ui_state,
    const std::filesystem::path& selected_json_path,
    std::string* status_out,
    std::string* error_out)
{
    if (ui_state == nullptr) {
        if (error_out) {
            *error_out = "Spatial layout UI state is null.";
        }
        return false;
    }

    std::filesystem::path measurement_path;
    if (!resolve_measurement_json_path_from_selection(selected_json_path, &measurement_path, error_out)) {
        return false;
    }

    nlohmann::json measurement_json;
    if (!read_json_file(measurement_path, &measurement_json, error_out)) {
        return false;
    }

    ArenaLayoutArtifact artifact;
    if (!orange::spatial::parse_arena_layout_artifact_json(measurement_json, &artifact, error_out)) {
        return false;
    }

    ui_state->layout_artifact = artifact;
    ui_state->selected_zone_index = clamp_index(
        ui_state->selected_zone_index,
        static_cast<int>(ui_state->layout_artifact.layout.zones.size()));
    clear_citrus_template_import(ui_state);
    clear_detected_experimental_area_circle(ui_state);

    bool loaded_runtime_registration = false;
    std::vector<std::string> loaded_parts;
    const std::filesystem::path artifact_dir = measurement_path.parent_path();
    const std::filesystem::path arena_layout_runtime_path =
        artifact_dir / kSpatialLayoutArenaLayoutRuntimeFilename;
    const std::filesystem::path dish_mask_runtime_path =
        artifact_dir / kSpatialLayoutDishMaskRuntimeFilename;

    if (std::filesystem::exists(arena_layout_runtime_path)) {
        nlohmann::json runtime_json;
        if (!read_json_file(arena_layout_runtime_path, &runtime_json, error_out)) {
            return false;
        }
        ArenaLayoutRuntime runtime;
        if (!orange::spatial::parse_arena_layout_runtime_json(runtime_json, &runtime, error_out) ||
            !orange::spatial::validate_arena_layout_runtime_against_artifact(runtime, artifact, error_out)) {
            return false;
        }
        apply_view_registration_to_editor_state(ui_state, runtime.registration);
        loaded_runtime_registration = true;
        loaded_parts.push_back("registration");
    }

    if (std::filesystem::exists(dish_mask_runtime_path)) {
        nlohmann::json dish_mask_json;
        if (!read_json_file(dish_mask_runtime_path, &dish_mask_json, error_out)) {
            return false;
        }
        DishMaskRuntime dish_mask_runtime;
        if (!orange::spatial::parse_dish_mask_runtime_json(dish_mask_json, &dish_mask_runtime, error_out)) {
            return false;
        }
        if (dish_mask_runtime.has_geometry) {
            ui_state->edge_margin_px = std::max(0.0, dish_mask_runtime.geometry.edge_margin_px);
            loaded_parts.push_back("edge_margin");
        }
    }

    if (!loaded_runtime_registration) {
        ui_state->registration.layout_coordinate_space = ui_state->layout_artifact.layout.coordinate_space;
        if (ui_state->has_capture) {
            reset_registration_from_frame(ui_state);
        }
    }

    if (status_out) {
        std::ostringstream status;
        status << "Loaded arena layout artifact from " << measurement_path.string();
        if (!loaded_parts.empty()) {
            status << " with ";
            for (size_t idx = 0; idx < loaded_parts.size(); ++idx) {
                if (idx > 0) {
                    status << (idx + 1 == loaded_parts.size() ? " and " : ", ");
                }
                status << loaded_parts[idx];
            }
            status << " sidecar";
            if (loaded_parts.size() > 1) {
                status << "s";
            }
        }
        *status_out = status.str();
    }
    return true;
}

Point2d transform_point(const std::array<double, 9>& matrix, double x, double y)
{
    return make_point(
        matrix[0] * x + matrix[1] * y + matrix[2],
        matrix[3] * x + matrix[4] * y + matrix[5]);
}

RuntimeGeometry transform_layout_geometry(
    const LayoutGeometry& layout_geometry,
    const std::array<double, 9>& layout_to_camera_matrix,
    double rotation_deg_clockwise)
{
    if (layout_geometry.type == LayoutGeometryType::kCircle) {
        const Point2d center = transform_point(layout_to_camera_matrix, layout_geometry.circle.cx, layout_geometry.circle.cy);
        const double scale =
            std::sqrt(layout_to_camera_matrix[0] * layout_to_camera_matrix[0] +
                      layout_to_camera_matrix[3] * layout_to_camera_matrix[3]);
        return runtime_circle(center.x, center.y, std::abs(scale) * layout_geometry.circle.r);
    }

    const double center_x = layout_geometry.rectangle.x + layout_geometry.rectangle.width * 0.5;
    const double center_y = layout_geometry.rectangle.y + layout_geometry.rectangle.height * 0.5;
    const Point2d center = transform_point(layout_to_camera_matrix, center_x, center_y);
    const double scale =
        std::sqrt(layout_to_camera_matrix[0] * layout_to_camera_matrix[0] +
                  layout_to_camera_matrix[3] * layout_to_camera_matrix[3]);
    return runtime_oriented_rectangle(
        center.x,
        center.y,
        std::abs(scale) * layout_geometry.rectangle.width,
        std::abs(scale) * layout_geometry.rectangle.height,
        rotation_deg_clockwise);
}

RuntimeGeometry inset_runtime_geometry(const RuntimeGeometry& geometry, double edge_margin_px)
{
    if (geometry.type == RuntimeGeometryType::kCircle) {
        return runtime_circle(
            geometry.circle.cx,
            geometry.circle.cy,
            std::max(0.0, geometry.circle.r - edge_margin_px));
    }

    return runtime_oriented_rectangle(
        geometry.oriented_rectangle.cx,
        geometry.oriented_rectangle.cy,
        std::max(0.0, geometry.oriented_rectangle.width - 2.0 * edge_margin_px),
        std::max(0.0, geometry.oriented_rectangle.height - 2.0 * edge_margin_px),
        geometry.oriented_rectangle.rotation_deg_clockwise);
}

std::array<Point2d, 4> oriented_rectangle_corners(const RuntimeGeometry& geometry)
{
    const auto& rect = geometry.oriented_rectangle;
    const double half_width = rect.width * 0.5;
    const double half_height = rect.height * 0.5;
    const double theta = rect.rotation_deg_clockwise * kPi / 180.0;
    const double cos_theta = std::cos(theta);
    const double sin_theta = std::sin(theta);

    auto rotate_and_translate = [&](double local_x, double local_y) -> Point2d {
        return make_point(
            rect.cx + cos_theta * local_x - sin_theta * local_y,
            rect.cy + sin_theta * local_x + cos_theta * local_y);
    };

    return {
        rotate_and_translate(-half_width, -half_height),
        rotate_and_translate(half_width, -half_height),
        rotate_and_translate(half_width, half_height),
        rotate_and_translate(-half_width, half_height)
    };
}

bool point_inside_image(const Point2d& point, int image_width, int image_height)
{
    return point.x >= 0.0 && point.x <= static_cast<double>(image_width) &&
           point.y >= 0.0 && point.y <= static_cast<double>(image_height);
}

VisibilityStatus compute_visibility_status(const RuntimeGeometry& geometry, int image_width, int image_height)
{
    if (image_width <= 0 || image_height <= 0) {
        return VisibilityStatus::kFull;
    }

    if (geometry.type == RuntimeGeometryType::kCircle) {
        const double left = geometry.circle.cx - geometry.circle.r;
        const double right = geometry.circle.cx + geometry.circle.r;
        const double top = geometry.circle.cy - geometry.circle.r;
        const double bottom = geometry.circle.cy + geometry.circle.r;
        if (right < 0.0 || bottom < 0.0 ||
            left > static_cast<double>(image_width) ||
            top > static_cast<double>(image_height)) {
            return VisibilityStatus::kOccluded;
        }
        if (left >= 0.0 && top >= 0.0 &&
            right <= static_cast<double>(image_width) &&
            bottom <= static_cast<double>(image_height)) {
            return VisibilityStatus::kFull;
        }
        return VisibilityStatus::kPartial;
    }

    const std::array<Point2d, 4> corners = oriented_rectangle_corners(geometry);
    int inside_count = 0;
    double min_x = std::numeric_limits<double>::max();
    double max_x = std::numeric_limits<double>::lowest();
    double min_y = std::numeric_limits<double>::max();
    double max_y = std::numeric_limits<double>::lowest();
    for (const Point2d& corner : corners) {
        if (point_inside_image(corner, image_width, image_height)) {
            ++inside_count;
        }
        min_x = std::min(min_x, corner.x);
        max_x = std::max(max_x, corner.x);
        min_y = std::min(min_y, corner.y);
        max_y = std::max(max_y, corner.y);
    }

    if (inside_count == 4) {
        return VisibilityStatus::kFull;
    }
    if (max_x < 0.0 || max_y < 0.0 ||
        min_x > static_cast<double>(image_width) ||
        min_y > static_cast<double>(image_height)) {
        return VisibilityStatus::kOccluded;
    }
    return VisibilityStatus::kPartial;
}

void rebuild_schema_preview(SpatialLayoutUiState* ui_state, const CameraParams* selected_camera)
{
    if (ui_state == nullptr) {
        return;
    }

    ui_state->layout_artifact.calibration_ref.artifact_id = ui_state->layout_artifact.artifact_id;
    ui_state->layout_artifact.calibration_ref.artifact_schema_id = orange::spatial::kArenaLayoutArtifactSchemaId;
    ui_state->layout_artifact.calibration_ref.artifact_schema_version = orange::spatial::kArenaLayoutArtifactSchemaVersion;
    if (ui_state->layout_artifact.calibration_ref.fingerprint.empty()) {
        ui_state->layout_artifact.calibration_ref.fingerprint = "preview-only";
    }

    ui_state->registration.layout_coordinate_space = ui_state->layout_artifact.layout.coordinate_space;
    ui_state->registration.layout_to_camera_matrix = build_layout_to_camera_matrix(*ui_state);
    ui_state->registration.has_camera_to_layout_matrix =
        invert_affine_3x3(ui_state->registration.layout_to_camera_matrix, &ui_state->registration.camera_to_layout_matrix);

    const double effective_rotation_deg =
        (ui_state->registration.type == RegistrationType::kSimilarity)
            ? ui_state->registration_rotation_deg_clockwise
            : 0.0;

    ui_state->arena_layout_runtime = {};
    ui_state->arena_layout_runtime.enabled = true;
    ui_state->arena_layout_runtime.layout_id = ui_state->layout_artifact.layout_id;
    ui_state->arena_layout_runtime.coordinate_space = CoordinateSpace::kCameraNativePixels;
    ui_state->arena_layout_runtime.registration = ui_state->registration;
    ui_state->arena_layout_runtime.zones.clear();

    for (const ArenaLayoutZone& zone : ui_state->layout_artifact.layout.zones) {
        ResolvedZoneOverlay overlay;
        overlay.zone_id = zone.zone_id;
        overlay.has_zone_index = zone.has_zone_index;
        overlay.zone_index = zone.zone_index;
        overlay.geometry =
            transform_layout_geometry(zone.geometry, ui_state->registration.layout_to_camera_matrix, effective_rotation_deg);
        overlay.visibility_status = compute_visibility_status(
            overlay.geometry,
            ui_state->captured_texture_width,
            ui_state->captured_texture_height);
        ui_state->arena_layout_runtime.zones.push_back(std::move(overlay));
    }

    ui_state->dish_mask_runtime = {};
    ui_state->dish_mask_runtime.enabled = true;
    ui_state->dish_mask_runtime.has_geometry = true;
    switch (ui_state->registration.source) {
        case RegistrationSource::kDetectedFit:
            ui_state->dish_mask_runtime.source = ObservationSource::kDetectedFit;
            break;
        case RegistrationSource::kImported:
            ui_state->dish_mask_runtime.source = ObservationSource::kImported;
            break;
        case RegistrationSource::kManual:
            ui_state->dish_mask_runtime.source = ObservationSource::kManual;
            break;
        case RegistrationSource::kIdentity:
        case RegistrationSource::kManualFit:
        default:
            ui_state->dish_mask_runtime.source = ObservationSource::kManualFit;
            break;
    }
    ui_state->dish_mask_runtime.geometry.coordinate_space = CoordinateSpace::kCameraNativePixels;
    ui_state->dish_mask_runtime.geometry.outer_geometry =
        transform_layout_geometry(ui_state->layout_artifact.layout.outer_geometry,
                                  ui_state->registration.layout_to_camera_matrix,
                                  effective_rotation_deg);
    ui_state->dish_mask_runtime.geometry.valid_geometry =
        inset_runtime_geometry(ui_state->dish_mask_runtime.geometry.outer_geometry, ui_state->edge_margin_px);
    ui_state->dish_mask_runtime.geometry.edge_margin_px = ui_state->edge_margin_px;

    ui_state->preview_calibration = {};
    ui_state->preview_calibration.has_dish_mask = true;
    ui_state->preview_calibration.dish_mask.calibration_ref = CalibrationRef{
        "preview.dish_mask",
        orange::spatial::kDishMaskArtifactSchemaId,
        orange::spatial::kDishMaskArtifactSchemaVersion,
        "preview-only"
    };
    ui_state->preview_calibration.dish_mask.runtime = ui_state->dish_mask_runtime;
    ui_state->preview_calibration.has_arena_layout = true;
    ui_state->preview_calibration.arena_layout.calibration_ref = ui_state->layout_artifact.calibration_ref;
    ui_state->preview_calibration.arena_layout.runtime = ui_state->arena_layout_runtime;

    ui_state->canonical_layout_json = orange::spatial::arena_layout_artifact_to_json(ui_state->layout_artifact).dump(2);
    ui_state->runtime_preview_json = orange::spatial::camera_spatial_calibration_to_json(ui_state->preview_calibration).dump(2);

    std::string error;
    ui_state->preview_valid =
        orange::spatial::validate_arena_layout_artifact(ui_state->layout_artifact, &error) &&
        orange::spatial::validate_dish_mask_runtime(ui_state->dish_mask_runtime, &error) &&
        orange::spatial::validate_arena_layout_runtime_against_artifact(
            ui_state->arena_layout_runtime,
            ui_state->layout_artifact,
            &error) &&
        orange::spatial::validate_camera_spatial_calibration(ui_state->preview_calibration, &error);

    if (selected_camera != nullptr) {
        std::ostringstream status;
        status << (ui_state->preview_valid ? "Preview valid" : "Preview invalid")
               << " for " << selected_camera->camera_serial;
        if (ui_state->has_capture) {
            status << " (" << ui_state->captured_texture_width << "x" << ui_state->captured_texture_height << ")";
        }
        ui_state->preview_status = status.str();
    }
    ui_state->preview_error = ui_state->preview_valid ? std::string() : error;
}

void draw_circle_geometry(const RuntimeGeometry& geometry, ImU32 color, float thickness)
{
    const ImPlotPoint center_point(geometry.circle.cx, geometry.circle.cy);
    const ImPlotPoint edge_point(geometry.circle.cx + geometry.circle.r, geometry.circle.cy);
    const ImVec2 center = ImPlot::PlotToPixels(center_point);
    const ImVec2 edge = ImPlot::PlotToPixels(edge_point);
    const float radius_px = std::abs(edge.x - center.x);
    ImPlot::GetPlotDrawList()->AddCircle(center, radius_px, color, 0, thickness);
}

void draw_oriented_rectangle_geometry(const RuntimeGeometry& geometry, ImU32 color, float thickness)
{
    const std::array<Point2d, 4> corners = oriented_rectangle_corners(geometry);
    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    for (size_t i = 0; i < corners.size(); ++i) {
        const Point2d& a = corners[i];
        const Point2d& b = corners[(i + 1) % corners.size()];
        draw_list->AddLine(
            ImPlot::PlotToPixels(ImPlotPoint(a.x, a.y)),
            ImPlot::PlotToPixels(ImPlotPoint(b.x, b.y)),
            color,
            thickness);
    }
}

void draw_runtime_geometry(const RuntimeGeometry& geometry, ImU32 color, float thickness)
{
    if (geometry.type == RuntimeGeometryType::kCircle) {
        draw_circle_geometry(geometry, color, thickness);
        return;
    }
    draw_oriented_rectangle_geometry(geometry, color, thickness);
}

void draw_hough_proposal_overlay(const RuntimeGeometry& geometry)
{
    if (geometry.type != RuntimeGeometryType::kCircle || geometry.circle.r <= 0.0) {
        return;
    }

    const ImU32 color = IM_COL32(255, 30, 190, 255);
    draw_circle_geometry(geometry, color, 3.2f);

    const ImVec2 center = ImPlot::PlotToPixels(
        ImPlotPoint(geometry.circle.cx, geometry.circle.cy));
    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    constexpr float cross = 9.0f;
    draw_list->AddLine(
        ImVec2(center.x - cross, center.y),
        ImVec2(center.x + cross, center.y),
        color,
        2.2f);
    draw_list->AddLine(
        ImVec2(center.x, center.y - cross),
        ImVec2(center.x, center.y + cross),
        color,
        2.2f);
    draw_list->AddText(
        ImVec2(center.x + 10.0f, center.y - 20.0f),
        color,
        "Hough");
}

void draw_citrus_projected_circle_overlay(const RuntimeGeometry& geometry)
{
    if (geometry.type != RuntimeGeometryType::kCircle || geometry.circle.r <= 0.0) {
        return;
    }

    const ImU32 color = IM_COL32(100, 190, 255, 230);
    draw_circle_geometry(geometry, color, 2.0f);

    const ImVec2 center = ImPlot::PlotToPixels(
        ImPlotPoint(geometry.circle.cx, geometry.circle.cy));
    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    constexpr float marker = 9.0f;
    const ImVec2 p0(center.x, center.y - marker);
    const ImVec2 p1(center.x - marker * 0.866f, center.y + marker * 0.5f);
    const ImVec2 p2(center.x + marker * 0.866f, center.y + marker * 0.5f);
    draw_list->AddTriangleFilled(p0, p1, p2, color);
    draw_list->AddTriangle(p0, p1, p2, IM_COL32(10, 20, 35, 240), 1.5f);
    draw_list->AddText(
        ImVec2(center.x + 10.0f, center.y + 6.0f),
        color,
        "Citrus");
}

bool compute_citrus_corrected_outline_overlay(
    const SpatialLayoutUiState& ui_state,
    Point2d* corrected_center_camera_out,
    std::vector<Point2d>* corrected_outline_camera_out)
{
    if (!ui_state.has_detected_experimental_area_circle ||
        ui_state.detected_experimental_area_geometry.type != RuntimeGeometryType::kCircle ||
        !ui_state.citrus_template.available ||
        !ui_state.citrus_template.has_camera_to_canvas_homography ||
        !ui_state.citrus_template.has_canvas_to_camera_homography ||
        !ui_state.citrus_template.has_arena_canvas_region) {
        return false;
    }

    const Point2d observed_center_camera = make_point(
        ui_state.detected_experimental_area_geometry.circle.cx,
        ui_state.detected_experimental_area_geometry.circle.cy);
    Point2d observed_center_canvas{};
    if (!transform_point_projective(
            ui_state.citrus_template.camera_to_canvas_homography,
            observed_center_camera,
            &observed_center_canvas)) {
        return false;
    }

    const Point2d corrected_center_arena_relative =
        citrus_canvas_to_arena_relative_px(
            ui_state.citrus_template,
            observed_center_canvas);
    if (!sample_citrus_experimental_area_outline_in_camera_px(
            ui_state.citrus_template,
            corrected_center_arena_relative,
            corrected_outline_camera_out,
            nullptr)) {
        return false;
    }

    const Point2d corrected_center_canvas =
        citrus_arena_relative_to_canvas_px(
            ui_state.citrus_template,
            corrected_center_arena_relative);
    Point2d corrected_center_camera{};
    if (!transform_point_projective(
            ui_state.citrus_template.canvas_to_camera_homography,
            corrected_center_canvas,
            &corrected_center_camera)) {
        return false;
    }
    if (corrected_center_camera_out != nullptr) {
        *corrected_center_camera_out = corrected_center_camera;
    }
    return true;
}

void draw_citrus_corrected_center_overlay(
    const RuntimeGeometry& current_citrus_geometry,
    const Point2d& corrected_center_camera,
    const std::vector<Point2d>& corrected_outline_camera)
{
    if (corrected_outline_camera.size() < 3) {
        return;
    }

    const ImU32 color = IM_COL32(70, 255, 150, 255);
    const ImVec2 corrected = ImPlot::PlotToPixels(
        ImPlotPoint(corrected_center_camera.x, corrected_center_camera.y));
    ImDrawList* draw_list = ImPlot::GetPlotDrawList();

    for (size_t idx = 0; idx < corrected_outline_camera.size(); ++idx) {
        const Point2d& a = corrected_outline_camera[idx];
        const Point2d& b = corrected_outline_camera[(idx + 1) % corrected_outline_camera.size()];
        draw_list->AddLine(
            ImPlot::PlotToPixels(ImPlotPoint(a.x, a.y)),
            ImPlot::PlotToPixels(ImPlotPoint(b.x, b.y)),
            color,
            2.6f);
    }

    if (current_citrus_geometry.type == RuntimeGeometryType::kCircle &&
        current_citrus_geometry.circle.r > 0.0) {
        const ImVec2 current = ImPlot::PlotToPixels(
            ImPlotPoint(current_citrus_geometry.circle.cx, current_citrus_geometry.circle.cy));
        draw_list->AddLine(current, corrected, color, 2.0f);
        draw_list->AddCircleFilled(current, 3.0f, color, 12);
    }

    constexpr float marker = 8.0f;
    const ImVec2 p0(corrected.x, corrected.y - marker);
    const ImVec2 p1(corrected.x + marker, corrected.y);
    const ImVec2 p2(corrected.x, corrected.y + marker);
    const ImVec2 p3(corrected.x - marker, corrected.y);
    draw_list->AddQuadFilled(p0, p1, p2, p3, color);
    draw_list->AddQuad(p0, p1, p2, p3, IM_COL32(10, 35, 20, 240), 1.5f);
    draw_list->AddText(
        ImVec2(corrected.x + 10.0f, corrected.y - 4.0f),
        color,
        "Corrected");
}

void draw_zone_label(const ResolvedZoneOverlay& zone, const ArenaLayoutArtifact& artifact, ImU32 color)
{
    std::string label = zone.zone_id;
    for (const ArenaLayoutZone& canonical_zone : artifact.layout.zones) {
        if (canonical_zone.zone_id == zone.zone_id && !canonical_zone.display_label.empty()) {
            label = canonical_zone.display_label;
            break;
        }
    }

    Point2d center{};
    if (zone.geometry.type == RuntimeGeometryType::kCircle) {
        center = make_point(zone.geometry.circle.cx, zone.geometry.circle.cy);
    } else {
        center = make_point(zone.geometry.oriented_rectangle.cx, zone.geometry.oriented_rectangle.cy);
    }
    ImPlot::GetPlotDrawList()->AddText(
        ImPlot::PlotToPixels(ImPlotPoint(center.x, center.y)),
        color,
        label.c_str());
}

bool draw_runtime_preview(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr || ui_state->captured_texture == 0 ||
        ui_state->captured_texture_width <= 0 || ui_state->captured_texture_height <= 0) {
        return false;
    }

    if (!orange::ui::begin_image_canvas(
            "Spatial Layout View",
            ui_state->captured_texture,
            ui_state->captured_texture_width,
            ui_state->captured_texture_height,
            &ui_state->captured_canvas_view,
            0.62f)) {
        return false;
    }

    bool changed = false;
    ImPlot::PushPlotClipRect();
    if (ui_state->has_citrus_projected_circle &&
        (ui_state->citrus_template.source_camera_id.empty() ||
         ui_state->captured_camera_serial.empty() ||
         ui_state->citrus_template.source_camera_id == ui_state->captured_camera_serial)) {
        draw_citrus_projected_circle_overlay(ui_state->citrus_projected_circle_geometry);
    }
    draw_runtime_geometry(
        ui_state->dish_mask_runtime.geometry.outer_geometry,
        IM_COL32(255, 165, 0, 230),
        2.0f);
    draw_runtime_geometry(
        ui_state->dish_mask_runtime.geometry.valid_geometry,
        IM_COL32(255, 220, 70, 210),
        2.0f);

    for (size_t idx = 0; idx < ui_state->arena_layout_runtime.zones.size(); ++idx) {
        const ResolvedZoneOverlay& zone = ui_state->arena_layout_runtime.zones[idx];
        const bool selected = static_cast<int>(idx) == ui_state->selected_zone_index;
        const ImU32 color = selected
            ? IM_COL32(90, 235, 255, 255)
            : IM_COL32(40, 220, 120, 235);
        const float thickness = selected ? 2.6f : 1.8f;
        draw_runtime_geometry(zone.geometry, color, thickness);
        draw_zone_label(zone, ui_state->layout_artifact, color);
    }
    if (ui_state->show_hough_proposal_overlay &&
        ui_state->has_detected_experimental_area_circle) {
        draw_hough_proposal_overlay(ui_state->detected_experimental_area_geometry);
    }
    if (ui_state->show_citrus_corrected_center_overlay &&
        ui_state->has_detected_experimental_area_circle &&
        ui_state->has_citrus_projected_circle) {
        Point2d corrected_center_camera{};
        std::vector<Point2d> corrected_outline_camera;
        if (compute_citrus_corrected_outline_overlay(
                *ui_state,
                &corrected_center_camera,
                &corrected_outline_camera)) {
            draw_citrus_corrected_center_overlay(
                ui_state->citrus_projected_circle_geometry,
                corrected_center_camera,
                corrected_outline_camera);
        }
    }
    ImPlot::PopPlotClipRect();

    if (ui_state->canvas_edit_mode == 0) {
        changed = handle_registration_canvas_edit(ui_state) || changed;
    } else {
        changed = handle_selected_zone_canvas_edit(ui_state) || changed;
    }
    ImPlot::EndPlot();
    return changed;
}

ImVec2 fit_group_capture_image_size(
    int image_width,
    int image_height,
    const ImVec2& available,
    float max_height)
{
    if (image_width <= 0 || image_height <= 0) {
        return ImVec2(1.0f, 1.0f);
    }
    const float width = std::max(1.0f, static_cast<float>(image_width));
    const float height = std::max(1.0f, static_cast<float>(image_height));
    const float width_scale = available.x > 0.0f ? available.x / width : 1.0f;
    const float height_scale = max_height > 0.0f ? max_height / height : 1.0f;
    const float scale = std::min(1.0f, std::min(width_scale, height_scale));
    return ImVec2(std::max(1.0f, width * scale), std::max(1.0f, height * scale));
}

void render_group_capture_panels(
    SpatialLayoutUiState* ui_state,
    const CameraParams* cameras_params,
    int num_cameras)
{
    if (ui_state == nullptr || ui_state->group_captures.empty()) {
        return;
    }

    ImGui::SeparatorText("Grouped Captures");
    if (!ui_state->group_capture_status.empty()) {
        ImGui::TextWrapped("%s", ui_state->group_capture_status.c_str());
    }
    if (!ui_state->group_capture_error.empty()) {
        ImGui::TextColored(
            ImVec4(1.0f, 0.65f, 0.35f, 1.0f),
            "%s",
            ui_state->group_capture_error.c_str());
    }

    const float available_width = ImGui::GetContentRegionAvail().x;
    const int columns =
        std::clamp(static_cast<int>(available_width / 230.0f), 1, 4);
    if (!ImGui::BeginTable("SpatialGroupCapturePanels", columns, ImGuiTableFlags_SizingStretchSame)) {
        return;
    }

    for (size_t idx = 0; idx < ui_state->group_captures.size(); ++idx) {
        SpatialLayoutGroupCaptureFrame& capture = ui_state->group_captures[idx];
        ImGui::TableNextColumn();
        const std::string child_id =
            "SpatialGroupCapturePanel_" + capture.camera_serial;
        ImGui::BeginChild(child_id.c_str(), ImVec2(0.0f, 235.0f), true);
        ImGui::Text("Cam%s", capture.camera_serial.c_str());
        ImGui::TextDisabled(
            "%s / %s",
            capture.metadata.image_set_purpose.c_str(),
            capture.metadata.image_set_target_plane.c_str());
        ImGui::TextDisabled(
            "%dx%d %s",
            capture.width,
            capture.height,
            capture.source_frame_count > 1 ? "mean" : "frame");
        if (capture.texture != 0 && capture.width > 0 && capture.height > 0) {
            const ImVec2 image_size =
                fit_group_capture_image_size(
                    capture.width,
                    capture.height,
                    ImGui::GetContentRegionAvail(),
                    145.0f);
            const float x_offset =
                std::max(0.0f, (ImGui::GetContentRegionAvail().x - image_size.x) * 0.5f);
            if (x_offset > 0.0f) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + x_offset);
            }
            ImGui::Image(
                (ImTextureID)(intptr_t)capture.texture,
                image_size,
                ImVec2(0, 1),
                ImVec2(1, 0));
        }
        const bool can_use =
            capture.valid &&
            capture.camera_index >= 0 &&
            capture.camera_index < num_cameras &&
            cameras_params != nullptr;
        ImGui::BeginDisabled(!can_use);
        const std::string use_button = "Use for fit##" + capture.camera_serial;
        if (ImGui::SmallButton(use_button.c_str())) {
            ui_state->selected_camera = capture.camera_index;
            ui_state->configured_camera_index = capture.camera_index;
            std::string preview_error;
            if (!apply_group_capture_to_active_preview(ui_state, capture, &preview_error)) {
                ui_state->preview_error = preview_error;
            } else if (cameras_params != nullptr) {
                rebuild_schema_preview(ui_state, &cameras_params[capture.camera_index]);
            }
        }
        ImGui::EndDisabled();
        ImGui::EndChild();
    }
    ImGui::EndTable();
}

const char* layout_coordinate_space_label(CoordinateSpace value)
{
    return value == CoordinateSpace::kLayoutMm ? "layout_mm" : "layout_units";
}

void render_layout_geometry_editor(const char* label_prefix, LayoutGeometry* geometry)
{
    if (geometry == nullptr) {
        return;
    }

    int geometry_type = geometry->type == LayoutGeometryType::kCircle ? 0 : 1;
    const char* geometry_items[] = {"circle", "rectangle"};
    if (ImGui::Combo((std::string(label_prefix) + " shape").c_str(), &geometry_type, geometry_items, IM_ARRAYSIZE(geometry_items))) {
        geometry->type = geometry_type == 0 ? LayoutGeometryType::kCircle : LayoutGeometryType::kRectangle;
    }

    if (geometry->type == LayoutGeometryType::kCircle) {
        ImGui::InputDouble((std::string(label_prefix) + " cx").c_str(), &geometry->circle.cx, 0.5);
        ImGui::InputDouble((std::string(label_prefix) + " cy").c_str(), &geometry->circle.cy, 0.5);
        ImGui::InputDouble((std::string(label_prefix) + " r").c_str(), &geometry->circle.r, 0.5);
        geometry->circle.r = std::max(0.0, geometry->circle.r);
    } else {
        ImGui::InputDouble((std::string(label_prefix) + " x").c_str(), &geometry->rectangle.x, 0.5);
        ImGui::InputDouble((std::string(label_prefix) + " y").c_str(), &geometry->rectangle.y, 0.5);
        ImGui::InputDouble((std::string(label_prefix) + " width").c_str(), &geometry->rectangle.width, 0.5);
        ImGui::InputDouble((std::string(label_prefix) + " height").c_str(), &geometry->rectangle.height, 0.5);
        geometry->rectangle.width = std::max(0.0, geometry->rectangle.width);
        geometry->rectangle.height = std::max(0.0, geometry->rectangle.height);
    }
}

void render_registration_editor(SpatialLayoutUiState* ui_state)
{
    const char* registration_type_items[] = {"identity", "translation", "similarity"};
    int registration_type = 2;
    if (ui_state->registration.type == RegistrationType::kIdentity) {
        registration_type = 0;
    } else if (ui_state->registration.type == RegistrationType::kTranslation) {
        registration_type = 1;
    }
    if (ImGui::Combo("Registration type", &registration_type, registration_type_items, IM_ARRAYSIZE(registration_type_items))) {
        ui_state->registration.type =
            registration_type == 0 ? RegistrationType::kIdentity :
            (registration_type == 1 ? RegistrationType::kTranslation : RegistrationType::kSimilarity);
    }

    const char* source_items[] = {"manual", "manual_fit", "detected_fit", "imported", "identity"};
    const RegistrationSource source_values[] = {
        RegistrationSource::kManual,
        RegistrationSource::kManualFit,
        RegistrationSource::kDetectedFit,
        RegistrationSource::kImported,
        RegistrationSource::kIdentity
    };
    int current_source = 1;
    for (int idx = 0; idx < IM_ARRAYSIZE(source_values); ++idx) {
        if (ui_state->registration.source == source_values[idx]) {
            current_source = idx;
            break;
        }
    }
    if (ImGui::Combo("Registration source", &current_source, source_items, IM_ARRAYSIZE(source_items))) {
        ui_state->registration.source = source_values[current_source];
    }

    const char* orientation_items[] = {"unknown", "trusted", "manual_confirmed", "ambiguous"};
    const OrientationStatus orientation_values[] = {
        OrientationStatus::kUnknown,
        OrientationStatus::kTrusted,
        OrientationStatus::kManualConfirmed,
        OrientationStatus::kAmbiguous
    };
    int current_orientation = 0;
    if (!ui_state->registration.has_orientation_status) {
        current_orientation = 0;
    } else {
        for (int idx = 0; idx < IM_ARRAYSIZE(orientation_values); ++idx) {
            if (ui_state->registration.orientation_status == orientation_values[idx]) {
                current_orientation = idx;
                break;
            }
        }
    }
    if (ImGui::Combo("Orientation status", &current_orientation, orientation_items, IM_ARRAYSIZE(orientation_items))) {
        ui_state->registration.has_orientation_status = true;
        ui_state->registration.orientation_status = orientation_values[current_orientation];
    }

    const bool translation_enabled =
        ui_state->registration.type == RegistrationType::kTranslation ||
        ui_state->registration.type == RegistrationType::kSimilarity;
    const bool similarity_enabled = ui_state->registration.type == RegistrationType::kSimilarity;

    ImGui::BeginDisabled(!translation_enabled);
    ImGui::InputDouble("Translate X (px)", &ui_state->registration_tx_px, 1.0, 20.0, "%.2f");
    ImGui::InputDouble("Translate Y (px)", &ui_state->registration_ty_px, 1.0, 20.0, "%.2f");
    ImGui::EndDisabled();

    ImGui::BeginDisabled(!similarity_enabled);
    ImGui::InputDouble("Scale (px / layout unit)", &ui_state->registration_scale, 0.05, 1.0, "%.4f");
    ImGui::InputDouble("Rotation CW (deg)", &ui_state->registration_rotation_deg_clockwise, 0.25, 2.0, "%.2f");
    ImGui::EndDisabled();
    ui_state->registration_scale = std::max(0.0001, ui_state->registration_scale);

    ImGui::InputInt("Fit point count", &ui_state->registration.fit_point_count);
    ui_state->registration.fit_point_count = std::max(0, ui_state->registration.fit_point_count);
    ImGui::InputDouble("Residual (px)", &ui_state->registration.residual_px, 0.1, 1.0, "%.3f");
    ui_state->registration.residual_px = std::max(0.0, ui_state->registration.residual_px);
}

void render_zone_editor(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr) {
        return;
    }

    if (ui_state->layout_artifact.layout.zones.empty()) {
        ImGui::TextDisabled("No zones yet.");
        if (ImGui::Button("Use experimental area as single zone")) {
            reset_to_single_experimental_area_zone(ui_state);
        }
        return;
    }

    sync_single_experimental_area_zone(ui_state);
    const bool single_experimental_area_zone = has_single_experimental_area_zone(*ui_state);
    if (single_experimental_area_zone) {
        ImGui::TextDisabled("Single-zone mode: zone 0 mirrors the experimental area.");
    } else if (ImGui::Button("Reset to single experimental-area zone")) {
        reset_to_single_experimental_area_zone(ui_state);
        return;
    }

    ui_state->selected_zone_index = clamp_index(
        ui_state->selected_zone_index,
        static_cast<int>(ui_state->layout_artifact.layout.zones.size()));

    std::vector<std::string> zone_labels_storage;
    std::vector<const char*> zone_labels;
    zone_labels_storage.reserve(ui_state->layout_artifact.layout.zones.size());
    zone_labels.reserve(ui_state->layout_artifact.layout.zones.size());
    for (const ArenaLayoutZone& zone : ui_state->layout_artifact.layout.zones) {
        std::ostringstream label;
        label << zone.zone_id;
        if (!zone.display_label.empty()) {
            label << " (" << zone.display_label << ")";
        }
        zone_labels_storage.push_back(label.str());
    }
    for (const std::string& label : zone_labels_storage) {
        zone_labels.push_back(label.c_str());
    }
    ImGui::Combo(
        "Selected zone",
        &ui_state->selected_zone_index,
        zone_labels.data(),
        static_cast<int>(zone_labels.size()));

    ArenaLayoutZone& zone =
        ui_state->layout_artifact.layout.zones[static_cast<size_t>(ui_state->selected_zone_index)];
    ImGui::BeginDisabled(single_experimental_area_zone);
    ImGui::InputText("Zone ID", &zone.zone_id);
    ImGui::Checkbox("Has zone index", &zone.has_zone_index);
    if (zone.has_zone_index) {
        ImGui::InputInt("Zone index", &zone.zone_index);
        zone.zone_index = std::max(0, zone.zone_index);
    }
    ImGui::InputText("Display label", &zone.display_label);
    render_layout_geometry_editor("Zone", &zone.geometry);
    ImGui::EndDisabled();

    if (ImGui::Button("Add zone")) {
        const int next_index = static_cast<int>(ui_state->layout_artifact.layout.zones.size());
        ui_state->layout_artifact.layout.zones.push_back(
            make_default_zone(ui_state->layout_artifact.layout.outer_geometry, next_index));
        ui_state->selected_zone_index = next_index;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(single_experimental_area_zone);
    if (ImGui::Button("Remove zone") && !ui_state->layout_artifact.layout.zones.empty()) {
        ui_state->layout_artifact.layout.zones.erase(
            ui_state->layout_artifact.layout.zones.begin() + ui_state->selected_zone_index);
        ui_state->selected_zone_index = std::max(0, ui_state->selected_zone_index - 1);
    }
    ImGui::EndDisabled();
}

void render_hough_registration_actions(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr) {
        return;
    }

    ImGui::BeginDisabled(!ui_state->has_capture);
    if (ImGui::Button("Fit Hough circle from capture")) {
        std::string detect_error;
        if (!detect_experimental_area_circle_from_capture(ui_state, &detect_error)) {
            ui_state->detection_error = detect_error;
            ui_state->detection_status = "Experimental-area detection failed.";
        }
    }
    ImGui::EndDisabled();

    ImGui::BeginDisabled(!ui_state->has_detected_experimental_area_circle);
    if (ImGui::Button("Use Hough fit for registration")) {
        std::string seed_error;
        if (!seed_registration_from_detected_experimental_area_circle(ui_state, &seed_error)) {
            ui_state->detection_error = seed_error;
        } else {
            ui_state->detection_error.clear();
            if (ui_state->detection_status.empty()) {
                ui_state->detection_status = "Seeded registration from detected experimental area.";
            } else {
                ui_state->detection_status += " Applied to registration.";
            }
        }
    }
    ImGui::EndDisabled();

    ImGui::BeginDisabled(!ui_state->has_capture);
    if (ImGui::Button("Reset registration from frame")) {
        reset_registration_from_frame(ui_state);
    }
    ImGui::EndDisabled();
}

void render_hough_circle_tuning(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr) {
        return;
    }
    if (!ImGui::CollapsingHeader("Hough Circle Tuning", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    render_hough_registration_actions(ui_state);
    ImGui::Separator();
    if (ImGui::Button("Reset Hough Defaults")) {
        ui_state->hough_dp = 1.25;
        ui_state->hough_min_dist_fraction = 0.20;
        ui_state->hough_param1 = 120.0;
        ui_state->hough_param2 = 30.0;
        ui_state->hough_min_radius_fraction = 0.18;
        ui_state->hough_max_radius_fraction = 0.49;
        ui_state->hough_radius_adjustment_px = 0.0;
        ui_state->hough_median_blur_ksize = 5;
        ui_state->hough_max_detection_dimension_px = 2048;
        ui_state->hough_fallback_enabled = true;
        ui_state->show_hough_proposal_overlay = true;
        ui_state->show_citrus_corrected_center_overlay = true;
    }
    ImGui::Checkbox("Show Hough proposal overlay", &ui_state->show_hough_proposal_overlay);
    ImGui::Checkbox(
        "Show corrected Citrus outline overlay",
        &ui_state->show_citrus_corrected_center_overlay);
    ImGui::InputDouble("Hough dp", &ui_state->hough_dp, 0.05, 0.25, "%.3f");
    ImGui::InputDouble("Hough min distance fraction", &ui_state->hough_min_dist_fraction, 0.01, 0.05, "%.3f");
    ImGui::InputDouble("Hough param1", &ui_state->hough_param1, 5.0, 25.0, "%.1f");
    ImGui::InputDouble("Hough param2", &ui_state->hough_param2, 1.0, 5.0, "%.1f");
    ImGui::InputDouble("Hough min radius fraction", &ui_state->hough_min_radius_fraction, 0.01, 0.05, "%.3f");
    ImGui::InputDouble("Hough max radius fraction", &ui_state->hough_max_radius_fraction, 0.01, 0.05, "%.3f");
    ImGui::InputDouble("Hough radius adjustment px", &ui_state->hough_radius_adjustment_px, 1.0, 10.0, "%.2f");
    ImGui::InputInt("Hough median blur kernel", &ui_state->hough_median_blur_ksize, 2, 4);
    ImGui::InputInt("Hough max detection dimension px", &ui_state->hough_max_detection_dimension_px, 128, 512);
    ImGui::Checkbox("Hough fallback pass", &ui_state->hough_fallback_enabled);

    if (ui_state->has_detected_experimental_area_circle &&
        ui_state->detected_experimental_area_geometry.type == RuntimeGeometryType::kCircle) {
        bool edited_detection = false;
        edited_detection |= ImGui::InputDouble(
            "Detected circle cx",
            &ui_state->detected_experimental_area_geometry.circle.cx,
            0.5,
            5.0,
            "%.2f");
        edited_detection |= ImGui::InputDouble(
            "Detected circle cy",
            &ui_state->detected_experimental_area_geometry.circle.cy,
            0.5,
            5.0,
            "%.2f");
        edited_detection |= ImGui::InputDouble(
            "Detected circle r",
            &ui_state->detected_experimental_area_geometry.circle.r,
            0.5,
            5.0,
            "%.2f");
        ui_state->detected_experimental_area_geometry.circle.r =
            std::max(1.0, ui_state->detected_experimental_area_geometry.circle.r);
        if (edited_detection) {
            ui_state->detection_error.clear();
            ui_state->detection_status = "Edited detected experimental-area circle.";
        }
    }
}

} // namespace

void clear_spatial_layout_texture(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr) {
        return;
    }
    orange::preview::clear_texture(
        &ui_state->captured_texture,
        &ui_state->captured_texture_width,
        &ui_state->captured_texture_height);
    clear_detected_experimental_area_circle(ui_state);
    ui_state->captured_rgba.clear();
    ui_state->has_capture = false;
    ui_state->captured_camera_serial.clear();
    ui_state->captured_source_array_role = "images_full";
    ui_state->captured_capture_mode = "single_camera_direct_still";
    ui_state->captured_capture_group_id.clear();
    ui_state->captured_source_frame_count = 1;
    ui_state->captured_first_local_frame_id = 0;
    ui_state->captured_last_local_frame_id = 0;
    ui_state->captured_first_camera_frame_id = 0;
    ui_state->captured_last_camera_frame_id = 0;
    ui_state->pending_full_res_snapshot_request_id = 0;
    ui_state->pending_full_res_snapshot_camera_serial.clear();
    ui_state->pending_full_res_snapshot_target_frame_count = 1;
    ui_state->captured_canvas_view.fit_requested = true;
    ui_state->captured_canvas_view.last_image_width = 0;
    ui_state->captured_canvas_view.last_image_height = 0;
}

void render_spatial_layout_window(
    SpatialLayoutUiState* ui_state,
    CameraControl* camera_control,
    CameraEmergent* ecams,
    CameraParams* cameras_params,
    CameraEachSelect* cameras_select,
    int num_cameras,
    bool other_calibration_tool_busy,
    const std::string& artifact_root_dir,
    const GLuint* live_preview_texture_ids,
    const uint64_t* live_preview_uploaded_serials,
    SpatialSnapshotWorker* const* spatial_snapshot_workers)
{
    if (ui_state == nullptr) {
        return;
    }

    initialize_spatial_layout_defaults(ui_state);

    if (!ui_state->show_window) {
        return;
    }

    if (!ImGui::Begin("Spatial Layout / Experimental Area Registration", &ui_state->show_window)) {
        ImGui::End();
        return;
    }

    poll_top_rim_observation_save_worker(ui_state);
    poll_generic_calibration_image_set_save_worker(ui_state);

    if (num_cameras <= 0 ||
        cameras_params == nullptr ||
        cameras_select == nullptr ||
        ecams == nullptr ||
        camera_control == nullptr ||
        !camera_control->open) {
        ImGui::TextDisabled("Open cameras before using spatial layout view registration.");
        ImGui::End();
        return;
    }

    ui_state->selected_camera = std::clamp(ui_state->selected_camera, 0, std::max(0, num_cameras - 1));

    std::vector<std::string> camera_labels_storage;
    std::vector<const char*> camera_labels;
    camera_labels_storage.reserve(num_cameras);
    camera_labels.reserve(num_cameras);
    for (int i = 0; i < num_cameras; ++i) {
        std::ostringstream label;
        label << i << ": " << cameras_params[i].camera_serial;
        camera_labels_storage.push_back(label.str());
    }
    for (const std::string& label : camera_labels_storage) {
        camera_labels.push_back(label.c_str());
    }

    ImGui::Combo("Camera", &ui_state->selected_camera, camera_labels.data(), num_cameras);
    if (ui_state->configured_camera_index != ui_state->selected_camera) {
        ui_state->configured_camera_index = ui_state->selected_camera;
        clear_spatial_layout_texture(ui_state);
        ui_state->preview_status = "Capture a frame to preview the selected camera.";
        ui_state->preview_error.clear();
        clear_detected_experimental_area_circle(ui_state);
    }

    CameraParams& selected_camera = cameras_params[ui_state->selected_camera];
    if (!ui_state->citrus_canvas_templates.empty()) {
        const int matching_citrus_index =
            find_citrus_template_index_for_camera(*ui_state, selected_camera.camera_serial);
        if (matching_citrus_index >= 0 &&
            matching_citrus_index != ui_state->citrus_canvas_template_index) {
            std::string status;
            std::string error;
            if (select_citrus_template_by_index(
                    ui_state,
                    matching_citrus_index,
                    &status,
                    &error)) {
                ui_state->citrus_import_status =
                    status + " Auto-selected from loaded Citrus canvas for selected Orange camera.";
                ui_state->citrus_import_error.clear();
            } else {
                ui_state->citrus_import_error = error;
            }
        }
    }
    SpatialSnapshotWorker* selected_snapshot_worker =
        spatial_snapshot_workers ? spatial_snapshot_workers[ui_state->selected_camera] : nullptr;
    if (spatial_snapshot_workers != nullptr) {
        for (int camera_index = 0; camera_index < num_cameras; ++camera_index) {
            SpatialSnapshotWorker* worker = spatial_snapshot_workers[camera_index];
            if (worker == nullptr) {
                continue;
            }
            SpatialSnapshotResult snapshot_result;
            while (worker->PopCompletedSnapshot(&snapshot_result)) {
                if (consume_group_snapshot_result(
                        ui_state,
                        snapshot_result,
                        cameras_params,
                        num_cameras,
                        ui_state->selected_camera)) {
                    continue;
                }
                const bool is_single_request =
                    ui_state->pending_full_res_snapshot_request_id != 0 &&
                    ui_state->pending_full_res_snapshot_request_id == snapshot_result.request_id &&
                    ui_state->pending_full_res_snapshot_camera_serial == snapshot_result.camera_serial;
                if (!is_single_request) {
                    continue;
                }
                std::string snapshot_error;
                if (!apply_full_resolution_stream_snapshot(
                        ui_state,
                        snapshot_result,
                        &snapshot_error)) {
                    ui_state->pending_full_res_snapshot_request_id = 0;
                    ui_state->pending_full_res_snapshot_camera_serial.clear();
                    ui_state->pending_full_res_snapshot_target_frame_count = 1;
                    ui_state->preview_error = snapshot_error;
                    ui_state->preview_status = "Full-resolution stream snapshot failed.";
                }
            }
        }
    }
    ImGui::Text("Current settings: focus=%u iris=%u exposure=%u frame_rate=%u gain=%u size=%ux%u",
                selected_camera.focus,
                selected_camera.iris,
                selected_camera.exposure,
                selected_camera.frame_rate,
                selected_camera.gain,
                selected_camera.width,
                selected_camera.height);

    const ImGuiTableFlags layout_table_flags =
        ImGuiTableFlags_Resizable |
        ImGuiTableFlags_BordersInnerV |
        ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("SpatialLayoutPanels", 2, layout_table_flags, ImGui::GetContentRegionAvail())) {
        ImGui::End();
        return;
    }
    ImGui::TableSetupColumn("Controls", ImGuiTableColumnFlags_WidthStretch, 0.46f);
    ImGui::TableSetupColumn("Fit Preview", ImGuiTableColumnFlags_WidthStretch, 0.54f);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::BeginChild("SpatialLayoutControlsPanel", ImVec2(0.0f, 0.0f), false);

    const bool can_capture =
        !camera_control->subscribe &&
        !camera_control->record_video &&
        !other_calibration_tool_busy;
    const bool can_capture_live_preview =
        camera_control->subscribe &&
        !camera_control->record_video &&
        !other_calibration_tool_busy &&
        cameras_select[ui_state->selected_camera].stream_on &&
        live_preview_texture_ids != nullptr &&
        live_preview_texture_ids[ui_state->selected_camera] != 0 &&
        live_preview_uploaded_serials != nullptr &&
        live_preview_uploaded_serials[ui_state->selected_camera] !=
            std::numeric_limits<uint64_t>::max();
    const bool full_res_request_pending_for_selected =
        ui_state->pending_full_res_snapshot_request_id != 0 &&
        ui_state->pending_full_res_snapshot_camera_serial == selected_camera.camera_serial;
    const int eligible_group_camera_count =
        eligible_group_capture_camera_count(
            cameras_select,
            spatial_snapshot_workers,
            num_cameras);
    const bool group_capture_pending =
        pending_group_snapshot_count(*ui_state) > 0;
    const bool can_capture_full_resolution_stream_snapshot =
        camera_control->subscribe &&
        !camera_control->record_video &&
        !other_calibration_tool_busy &&
        cameras_select[ui_state->selected_camera].stream_on &&
        selected_snapshot_worker != nullptr &&
        !full_res_request_pending_for_selected;
    const bool can_capture_group_full_resolution_stream_snapshot =
        camera_control->subscribe &&
        !camera_control->record_video &&
        !other_calibration_tool_busy &&
        eligible_group_camera_count > 0 &&
        ui_state->pending_full_res_snapshot_request_id == 0 &&
        !group_capture_pending;

    if (!can_capture) {
        ImGui::TextDisabled("Direct still capture requires streaming, recording, and other calibration tools to be stopped.");
    }

    if (ImGui::Button("Capture Frame") && can_capture) {
        std::string capture_error;
        if (!capture_single_camera_frame(ui_state, ecams, cameras_params, &capture_error)) {
            ui_state->preview_error = capture_error;
            ui_state->preview_status = "Capture failed.";
        } else {
            reset_registration_from_frame(ui_state);
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!can_capture_live_preview);
    if (ImGui::Button("Capture Live Stream Snapshot")) {
        std::string capture_error;
        if (!capture_live_stream_preview_texture(
                ui_state,
                selected_camera,
                cameras_select[ui_state->selected_camera],
                live_preview_texture_ids ? live_preview_texture_ids[ui_state->selected_camera] : 0,
                &capture_error)) {
            ui_state->preview_error = capture_error;
            ui_state->preview_status = "Live stream snapshot failed.";
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!can_capture_full_resolution_stream_snapshot);
    if (ImGui::Button("Capture Full-Resolution Stream Snapshot")) {
        std::string request_error;
        uint64_t request_id = 0;
        std::ostringstream operation_id;
        operation_id << "spatial_layout_full_res_" << selected_camera.camera_serial;
        if (!selected_snapshot_worker ||
            !selected_snapshot_worker->RequestSnapshot(
                operation_id.str(),
                &request_id,
                &request_error)) {
            ui_state->preview_error = request_error.empty()
                                          ? "Failed to request full-resolution stream snapshot."
                                          : request_error;
            ui_state->preview_status = "Full-resolution stream snapshot request failed.";
        } else {
            ui_state->pending_full_res_snapshot_request_id = request_id;
            ui_state->pending_full_res_snapshot_camera_serial = selected_camera.camera_serial;
            ui_state->pending_full_res_snapshot_target_frame_count = 1;
            ui_state->preview_error.clear();
            ui_state->preview_status =
                "Waiting for full-resolution stream snapshot from " +
                selected_camera.camera_serial + ".";
        }
    }
    ImGui::EndDisabled();
    ui_state->calibration_average_frame_count =
        std::clamp(ui_state->calibration_average_frame_count, 2, 256);
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputInt("Average frames", &ui_state->calibration_average_frame_count, 1, 10);
    ui_state->calibration_average_frame_count =
        std::clamp(ui_state->calibration_average_frame_count, 2, 256);
    ImGui::SameLine();
    ImGui::BeginDisabled(!can_capture_full_resolution_stream_snapshot);
    if (ImGui::Button("Capture Averaged Full-Resolution Snapshot")) {
        std::string request_error;
        uint64_t request_id = 0;
        std::ostringstream operation_id;
        operation_id << "spatial_layout_avg_full_res_"
                     << selected_camera.camera_serial
                     << "_n" << ui_state->calibration_average_frame_count;
        if (!selected_snapshot_worker ||
            !selected_snapshot_worker->RequestSnapshot(
                operation_id.str(),
                &request_id,
                &request_error,
                static_cast<uint32_t>(ui_state->calibration_average_frame_count))) {
            ui_state->preview_error = request_error.empty()
                                          ? "Failed to request averaged full-resolution stream snapshot."
                                          : request_error;
            ui_state->preview_status = "Averaged full-resolution stream snapshot request failed.";
        } else {
            ui_state->pending_full_res_snapshot_request_id = request_id;
            ui_state->pending_full_res_snapshot_camera_serial = selected_camera.camera_serial;
            ui_state->pending_full_res_snapshot_target_frame_count =
                static_cast<uint32_t>(ui_state->calibration_average_frame_count);
            ui_state->preview_error.clear();
            ui_state->preview_status =
                "Waiting for averaged full-resolution stream snapshot from " +
                selected_camera.camera_serial + " (" +
                std::to_string(ui_state->calibration_average_frame_count) + " frames).";
        }
    }
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!can_capture_group_full_resolution_stream_snapshot);
    if (ImGui::Button("Capture Group Full-Resolution Snapshots")) {
        std::string request_error;
        if (!request_group_full_resolution_snapshots(
                ui_state,
                cameras_params,
                cameras_select,
                num_cameras,
                spatial_snapshot_workers,
                1,
                &request_error)) {
            ui_state->group_capture_error = request_error;
            ui_state->group_capture_status = "Grouped full-resolution capture request failed.";
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!can_capture_group_full_resolution_stream_snapshot);
    if (ImGui::Button("Capture Averaged Group Snapshots")) {
        std::string request_error;
        if (!request_group_full_resolution_snapshots(
                ui_state,
                cameras_params,
                cameras_select,
                num_cameras,
                spatial_snapshot_workers,
                static_cast<uint32_t>(ui_state->calibration_average_frame_count),
                &request_error)) {
            ui_state->group_capture_error = request_error;
            ui_state->group_capture_status = "Grouped averaged capture request failed.";
        }
    }
    ImGui::EndDisabled();
    if (!camera_control->subscribe) {
        ImGui::TextDisabled("Stream snapshots use the active GUI stream, useful for TTL-lit rigs.");
    } else if (!can_capture_live_preview) {
        ImGui::TextDisabled("Wait for the selected camera's live preview texture before taking a live preview snapshot.");
    }
    if (full_res_request_pending_for_selected) {
        ImGui::TextDisabled(
            "Full-resolution snapshot request %llu is collecting %u frame(s).",
            static_cast<unsigned long long>(ui_state->pending_full_res_snapshot_request_id),
            static_cast<unsigned int>(
                std::max<uint32_t>(1u, ui_state->pending_full_res_snapshot_target_frame_count)));
    } else if (camera_control->subscribe && !can_capture_full_resolution_stream_snapshot) {
        ImGui::TextDisabled("Full-resolution stream snapshot worker is not available for the selected camera.");
    }
    if (group_capture_pending) {
        ImGui::TextDisabled(
            "Grouped capture %s is waiting on %d camera(s).",
            ui_state->group_capture_id.c_str(),
            pending_group_snapshot_count(*ui_state));
    } else if (camera_control->subscribe && eligible_group_camera_count <= 0) {
        ImGui::TextDisabled("No streaming cameras with spatial snapshot workers are available for grouped capture.");
    }
    if (!ui_state->group_capture_status.empty()) {
        ImGui::TextWrapped("%s", ui_state->group_capture_status.c_str());
    }
    if (!ui_state->group_capture_error.empty()) {
        ImGui::TextColored(
            ImVec4(1.0f, 0.65f, 0.35f, 1.0f),
            "%s",
            ui_state->group_capture_error.c_str());
    }

    const bool citrus_template_matches_selected_camera =
        !ui_state->citrus_template.available ||
        ui_state->citrus_template.source_camera_id.empty() ||
        ui_state->citrus_template.source_camera_id == selected_camera.camera_serial;

    ImGui::SeparatorText("Citrus Canvas Import");
    if (ImGui::Button("Import Citrus Canvas Config...")) {
        IGFD::FileDialogConfig config;
        config.path = default_citrus_rigs_root();
        config.countSelectionMax = 1;
        ImGuiFileDialog::Instance()->OpenDialog(
            kLoadCitrusArenaConfigDialogId,
            "Choose Citrus Canvas Config JSON",
            ".json",
            config);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(
        !ui_state->citrus_template.available ||
        !ui_state->citrus_template.has_canvas_to_camera_homography ||
        !citrus_template_matches_selected_camera);
    if (ImGui::Button("Seed From Citrus Global Homography")) {
        std::string seed_error;
        if (!seed_registration_from_citrus_homography(ui_state, &seed_error)) {
            ui_state->citrus_import_error = seed_error;
        } else {
            ui_state->citrus_import_error.clear();
            if (ui_state->citrus_import_status.empty()) {
                ui_state->citrus_import_status =
                    "Seeded registration from Citrus global-canvas homography projection.";
            } else {
                ui_state->citrus_import_status += " Applied to registration.";
            }
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Clear Citrus Import State")) {
        clear_citrus_template_import(ui_state);
    }

    if (!ui_state->citrus_canvas_templates.empty()) {
        std::vector<std::string> template_labels_storage;
        std::vector<const char*> template_labels;
        template_labels_storage.reserve(ui_state->citrus_canvas_templates.size());
        template_labels.reserve(ui_state->citrus_canvas_templates.size());
        for (const CitrusSpatialTemplateState& template_state : ui_state->citrus_canvas_templates) {
            template_labels_storage.push_back(citrus_template_display_label(template_state));
        }
        for (const std::string& label : template_labels_storage) {
            template_labels.push_back(label.c_str());
        }
        ui_state->citrus_canvas_template_index =
            clamp_index(
                ui_state->citrus_canvas_template_index,
                static_cast<int>(ui_state->citrus_canvas_templates.size()));
        int selected_template_index = ui_state->citrus_canvas_template_index;
        if (ImGui::Combo(
                "Citrus canvas arena/camera",
                &selected_template_index,
                template_labels.data(),
                static_cast<int>(template_labels.size())) &&
            selected_template_index != ui_state->citrus_canvas_template_index) {
            std::string status;
            std::string error;
            if (!select_citrus_template_by_index(
                    ui_state,
                    selected_template_index,
                    &status,
                    &error)) {
                ui_state->citrus_import_error = error;
            } else {
                ui_state->citrus_import_status = status;
                ui_state->citrus_import_error.clear();
                rebuild_schema_preview(ui_state, &selected_camera);
            }
        }
        if (!ui_state->citrus_canvas_config_path.empty()) {
            ImGui::TextDisabled("%s", ui_state->citrus_canvas_config_path.c_str());
        }
    }

    if (!ui_state->citrus_template.available) {
        ImGui::TextDisabled("Import a Citrus canvas config to seed the single circular experimental area.");
    } else {
        ImGui::TextWrapped(
            "Imported: rig=%s canvas=%s arena=%s config=%s camera=%s",
            ui_state->citrus_template.source_rig_name.c_str(),
            ui_state->citrus_template.source_canvas_name.c_str(),
            ui_state->citrus_template.source_arena_name.c_str(),
            ui_state->citrus_template.source_config_name.c_str(),
            ui_state->citrus_template.source_camera_id.c_str());
        ImGui::TextWrapped(
            "Citrus circle: arena-relative center=(%.2f, %.2f) r=%.2f canvas px",
            ui_state->citrus_template.experimental_area_center_x_px,
            ui_state->citrus_template.experimental_area_center_y_px,
            ui_state->citrus_template.experimental_area_radius_px);
        if (ui_state->citrus_template.has_arena_canvas_region) {
            const Point2d origin = citrus_arena_origin_canvas_px(ui_state->citrus_template);
            const Point2d global_center = citrus_arena_relative_to_canvas_px(
                ui_state->citrus_template,
                make_point(
                    ui_state->citrus_template.experimental_area_center_x_px,
                    ui_state->citrus_template.experimental_area_center_y_px));
            ImGui::TextWrapped(
                "Citrus arena canvas region: center=(%.2f, %.2f) size=(%.2f, %.2f) origin=(%.2f, %.2f)",
                ui_state->citrus_template.arena_center_x_px,
                ui_state->citrus_template.arena_center_y_px,
                ui_state->citrus_template.arena_width_px,
                ui_state->citrus_template.arena_height_px,
                origin.x,
                origin.y);
            ImGui::TextWrapped(
                "Citrus current experimental center in global canvas: (%.2f, %.2f)",
                global_center.x,
                global_center.y);
            if (ui_state->has_citrus_projected_circle &&
                ui_state->citrus_projected_circle_geometry.type == RuntimeGeometryType::kCircle) {
                ImGui::TextWrapped(
                    "Citrus current experimental center in camera px: (%.2f, %.2f), r=%.2f",
                    ui_state->citrus_projected_circle_geometry.circle.cx,
                    ui_state->citrus_projected_circle_geometry.circle.cy,
                    ui_state->citrus_projected_circle_geometry.circle.r);
            }
        } else {
            ImGui::TextColored(
                ImVec4(1.0f, 0.75f, 0.35f, 1.0f),
                "Citrus arena canvas region fields are missing; arena-relative points cannot be mapped to global canvas correctly.");
        }
        if (ui_state->citrus_template.has_radius_mm) {
            ImGui::Text("Radius: %.3f mm", ui_state->citrus_template.experimental_area_radius_mm);
        }
        if (ui_state->citrus_template.has_pixels_per_mm_projector) {
            ImGui::Text("Projector scale: %.4f px/mm", ui_state->citrus_template.pixels_per_mm_projector);
        }
        ImGui::TextDisabled(
            "%s",
            ui_state->citrus_template.has_canvas_to_camera_homography
                ? "Canvas-to-camera homography loaded."
                : "No canvas-to-camera homography sidecar found.");
        if (!citrus_template_matches_selected_camera) {
            ImGui::TextColored(
                ImVec4(1.0f, 0.75f, 0.35f, 1.0f),
                "Imported config targets camera %s, but the selected Orange camera is %s.",
                ui_state->citrus_template.source_camera_id.c_str(),
                selected_camera.camera_serial.c_str());
        }
        if (ui_state->has_detected_experimental_area_circle &&
            ui_state->detected_experimental_area_geometry.type == RuntimeGeometryType::kCircle &&
            ui_state->citrus_template.has_camera_to_canvas_homography &&
            ui_state->citrus_template.has_arena_canvas_region) {
            const Point2d detected_camera_center = make_point(
                ui_state->detected_experimental_area_geometry.circle.cx,
                ui_state->detected_experimental_area_geometry.circle.cy);
            Point2d detected_canvas_center{};
            if (transform_point_projective(
                    ui_state->citrus_template.camera_to_canvas_homography,
                    detected_camera_center,
                    &detected_canvas_center)) {
                const Point2d detected_arena_relative =
                    citrus_canvas_to_arena_relative_px(
                        ui_state->citrus_template,
                        detected_canvas_center);
                const double delta_x =
                    detected_arena_relative.x -
                    ui_state->citrus_template.experimental_area_center_x_px;
                const double delta_y =
                    detected_arena_relative.y -
                    ui_state->citrus_template.experimental_area_center_y_px;
                ImGui::TextWrapped(
                    "Detected top-rim center maps to Citrus global canvas=(%.2f, %.2f), arena-relative=(%.2f, %.2f), delta=(%+.2f, %+.2f) px.",
                    detected_canvas_center.x,
                    detected_canvas_center.y,
                    detected_arena_relative.x,
                    detected_arena_relative.y,
                    delta_x,
                    delta_y);
            }
        }
    }

    ImGui::SeparatorText("Detection And Canonical Layout");
    ImGui::InputText("Layout ID", &ui_state->layout_artifact.layout_id);
    ImGui::InputText("Artifact ID", &ui_state->layout_artifact.artifact_id);
    ImGui::InputText("Canvas ID", &ui_state->layout_artifact.context.canvas_id);
    ImGui::InputText("Dish design ID", &ui_state->layout_artifact.context.dish_design_id);

    int coordinate_space = ui_state->layout_artifact.layout.coordinate_space == CoordinateSpace::kLayoutMm ? 0 : 1;
    const char* coordinate_items[] = {"layout_mm", "layout_units"};
    if (ImGui::Combo("Layout coordinate space", &coordinate_space, coordinate_items, IM_ARRAYSIZE(coordinate_items))) {
        ui_state->layout_artifact.layout.coordinate_space =
            coordinate_space == 0 ? CoordinateSpace::kLayoutMm : CoordinateSpace::kLayoutUnits;
    }
    ImGui::InputText("Ordering rule", &ui_state->layout_artifact.provenance.ordering_rule);
    render_layout_geometry_editor("Experimental area", &ui_state->layout_artifact.layout.outer_geometry);

    ImGui::SeparatorText("View Registration");
    render_registration_editor(ui_state);
    ImGui::InputDouble("Experimental area edge margin (px)", &ui_state->edge_margin_px, 0.5, 5.0, "%.2f");
    ui_state->edge_margin_px = std::max(0.0, ui_state->edge_margin_px);

    ImGui::SeparatorText("Zones");
    render_zone_editor(ui_state);
    sync_single_experimental_area_zone(ui_state);

    rebuild_schema_preview(ui_state, &selected_camera);

    ImGui::SeparatorText("Calibration Workflow");
    if (ImGui::BeginTabBar("SpatialCalibrationWorkflowTabs")) {
        if (ImGui::BeginTabItem("Projection Surface")) {
            if (ui_state->calibration_workflow_tab != 0) {
                ui_state->calibration_workflow_tab = 0;
                apply_calibration_workflow_tab_defaults(ui_state, 0);
            }
            ImGui::TextWrapped(
                "Use for Citrus arena projection and homography images at the projector/diffuser plane. "
                "These captures usually suppress mapped NIR strobe pulses and rely on visible projector output.");
            if (ImGui::Button("Arena projection defaults")) {
                apply_calibration_image_set_purpose_defaults(ui_state, "arena_projection");
            }
            ImGui::SameLine();
            if (ImGui::Button("Homography grid defaults")) {
                apply_calibration_image_set_purpose_defaults(ui_state, "homography_grid");
            }
            ImGui::SameLine();
            if (ImGui::Button("Projection scale defaults")) {
                apply_projection_surface_scale_image_defaults(ui_state);
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Estimated Fish Plane")) {
            if (ui_state->calibration_workflow_tab != 1) {
                ui_state->calibration_workflow_tab = 1;
                apply_calibration_workflow_tab_defaults(ui_state, 1);
            }
            ImGui::TextWrapped(
                "Use for homography, ruler/scale, and crosshair images near the tank-bottom/fish plane. "
                "Tank-bottom homography can use a different pattern/domain shape than the projection surface; "
                "Scale images usually keep or restore the mapped TTL NIR pulse path so the target is visible to the camera.");
            if (ImGui::Button("Tank-bottom homography defaults")) {
                apply_tank_bottom_inner_surface_homography_defaults(ui_state);
            }
            ImGui::SameLine();
            if (ImGui::Button("Tank-bottom scale defaults")) {
                apply_tank_bottom_inner_surface_scale_image_defaults(ui_state);
            }
            ImGui::SameLine();
            if (ImGui::Button("Crosshair alignment defaults")) {
                apply_calibration_image_set_purpose_defaults(ui_state, "crosshair_alignment");
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Dish / Valid Area")) {
            if (ui_state->calibration_workflow_tab != 2) {
                ui_state->calibration_workflow_tab = 2;
                apply_calibration_workflow_tab_defaults(ui_state, 2);
            }
            ImGui::TextWrapped(
                "Use for daily dish top-rim fits and valid-area/mask artifacts. "
                "This tab prepares capture metadata for top-rim observation saves; Citrus still owns accepted runtime geometry.");
            if (ImGui::Button("Top-rim capture defaults")) {
                apply_calibration_workflow_tab_defaults(ui_state, 2);
            }
            render_hough_circle_tuning(ui_state);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::SeparatorText("Calibration Capture Metadata");
    static constexpr const char* kFilterStatePresets[] = {
        "unknown",
        kHoyaR72FilterInstalled,
        kHoyaR72FilterRemoved,
        "no_filter_installed"
    };
    static constexpr const char* kRuntimeFilterStatePresets[] = {
        "unknown",
        kHoyaR72FilterInstalled,
        kHoyaR72FilterRemoved,
        "not_applicable"
    };
    static constexpr const char* kLightStatePresets[] = {
        "unknown",
        "ttl_nir_strobe_active",
        "ttl_nir_strobe_inactive",
        "visible_projector_only",
        "ambient_room_light",
        "external_continuous_visible_light",
        "external_continuous_ir_nir_light",
        "lights_off"
    };
    static constexpr const char* kLightHandlingPresets[] = {
        "leave_current",
        "suppress_mapped_strobe",
        "keep_or_restore_mapped_pulse",
        "force_manual_active",
        "operator_manual"
    };
    static constexpr const char* kProjectorStatePresets[] = {
        "unknown",
        "off",
        "black_or_idle",
        "crosshair_on",
        "homography_grid_on",
        "scale_pattern_on",
        "normal_stimulus_active"
    };
    static constexpr const char* kIlluminationSpectrumPresets[] = {
        "unknown",
        "narrowband_nir",
        "unknown_ir_nir",
        "broadband_visible",
        "broadband_visible_nir",
        "none"
    };
    static constexpr const char* kIlluminationSourcePresets[] = {
        "unknown",
        "custom_ttl_nir_strobe",
        "visible_projector",
        "ambient_room_light",
        "external_continuous_visible_light",
        "external_continuous_ir_nir_light",
        "none"
    };
    static constexpr const char* kIlluminationConfidencePresets[] = {
        "unknown",
        "nominal",
        "measured",
        "approximate_range",
        "not_applicable"
    };
    static constexpr const char* kIlluminationPresetIds[] = {
        "unknown",
        "custom_ttl_nir_strobe_855nm",
        "visible_projector_broadband",
        "ambient_room_light_visible",
        "external_continuous_visible_light",
        "external_continuous_ir_nir_light",
        "lights_off"
    };
    static constexpr const char* kIlluminationPresetLabels[] = {
        "Unknown",
        "Custom TTL NIR strobe, 855 nm nominal",
        "Visible projector, broadband visible",
        "Ambient room light, broadband visible",
        "External continuous visible light",
        "External continuous IR/NIR light",
        "Lights off"
    };
    render_string_preset_combo(
        "Filter state",
        &ui_state->calibration_filter_state,
        kFilterStatePresets,
        IM_ARRAYSIZE(kFilterStatePresets));
    render_string_preset_combo(
        "Runtime filter state",
        &ui_state->calibration_runtime_filter_state,
        kRuntimeFilterStatePresets,
        IM_ARRAYSIZE(kRuntimeFilterStatePresets));
    render_string_preset_combo(
        "Light handling",
        &ui_state->calibration_light_handling,
        kLightHandlingPresets,
        IM_ARRAYSIZE(kLightHandlingPresets));
    if (ImGui::BeginCombo("Illumination preset", "Apply preset...")) {
        for (int idx = 0; idx < IM_ARRAYSIZE(kIlluminationPresetIds); ++idx) {
            if (ImGui::Selectable(kIlluminationPresetLabels[idx])) {
                apply_illumination_preset(ui_state, kIlluminationPresetIds[idx]);
            }
        }
        ImGui::EndCombo();
    }
    render_string_preset_combo(
        "Illumination source",
        &ui_state->calibration_illumination_source,
        kIlluminationSourcePresets,
        IM_ARRAYSIZE(kIlluminationSourcePresets));
    const std::string selected_light_handling =
        ui_state->calibration_light_handling.empty()
            ? "leave_current"
            : ui_state->calibration_light_handling;
    const bool light_handling_needs_mapped_strobe =
        calibration_light_handling_needs_mapped_strobe(selected_light_handling);
    std::vector<int> light_control_camera_indices;
    light_control_camera_indices.reserve(num_cameras);
    for (int i = 0; i < num_cameras; ++i) {
        if (camera_has_exposed_mapped_nir_strobe(cameras_params[i])) {
            light_control_camera_indices.push_back(i);
        }
    }
    auto light_control_candidate_index = [&](const int camera_index) -> int {
        const auto it = std::find(
            light_control_camera_indices.begin(),
            light_control_camera_indices.end(),
            camera_index);
        if (it == light_control_camera_indices.end()) {
            return -1;
        }
        return static_cast<int>(std::distance(light_control_camera_indices.begin(), it));
    };
    if (light_control_camera_indices.empty()) {
        ui_state->calibration_light_control_camera = -1;
    } else if (light_control_candidate_index(ui_state->calibration_light_control_camera) < 0) {
        const int selected_light_control_index =
            light_control_candidate_index(ui_state->selected_camera);
        ui_state->calibration_light_control_camera =
            selected_light_control_index >= 0
                ? ui_state->selected_camera
                : light_control_camera_indices.front();
    }
    if (!light_control_camera_indices.empty()) {
        std::vector<std::string> light_control_labels_storage;
        std::vector<const char*> light_control_labels;
        light_control_labels_storage.reserve(light_control_camera_indices.size());
        light_control_labels.reserve(light_control_camera_indices.size());
        for (const int camera_index : light_control_camera_indices) {
            const CameraParams& light_camera = cameras_params[camera_index];
            const CameraRigIoConnection* connection =
                find_mapped_nir_strobe_output_connection(light_camera);
            std::ostringstream label;
            label << light_camera.camera_serial << " / "
                  << (connection != nullptr && !connection->camera_line.empty()
                          ? connection->camera_line
                          : "mapped output");
            light_control_labels_storage.push_back(label.str());
            light_control_labels.push_back(light_control_labels_storage.back().c_str());
        }
        int light_control_combo_index =
            light_control_candidate_index(ui_state->calibration_light_control_camera);
        if (light_control_combo_index < 0) {
            light_control_combo_index = 0;
        }
        if (ImGui::Combo(
                "Light control camera",
                &light_control_combo_index,
                light_control_labels.data(),
                static_cast<int>(light_control_labels.size()))) {
            ui_state->calibration_light_control_camera =
                light_control_camera_indices[light_control_combo_index];
        }
    }
    const int light_control_camera = ui_state->calibration_light_control_camera;
    const bool light_control_camera_valid =
        light_control_camera >= 0 && light_control_camera < num_cameras;
    CameraParams* light_control_params =
        light_control_camera_valid ? &cameras_params[light_control_camera] : nullptr;
    CameraEmergent* light_control_ecam =
        light_control_camera_valid ? &ecams[light_control_camera] : nullptr;
    const CameraRigIoConnection* mapped_strobe_connection =
        light_control_params != nullptr
            ? find_mapped_nir_strobe_output_connection(*light_control_params)
            : nullptr;
    const bool mapped_strobe_available =
        light_control_params != nullptr &&
        light_control_params->gpio_pinout_access == "exposed" &&
        mapped_strobe_connection != nullptr;
    const bool light_output_mutation_locked =
        camera_control->record_video || camera_control->recording_draining;
    const bool can_prepare_calibration_capture =
        !light_output_mutation_locked &&
        (!light_handling_needs_mapped_strobe || mapped_strobe_available);
    ImGui::BeginDisabled(!can_prepare_calibration_capture);
    if (ImGui::Button("Prepare Calibration Capture")) {
        std::string status;
        const bool ok = prepare_calibration_capture_preflight(
            ui_state,
            &ecams[ui_state->selected_camera],
            &selected_camera,
            light_control_ecam,
            light_control_params,
            mapped_strobe_available,
            light_output_mutation_locked,
            selected_light_handling,
            &status);
        set_calibration_preflight_result(ui_state, ok, status);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!can_prepare_calibration_capture);
    if (ImGui::Button("Prepare All Cameras")) {
        std::string status;
        const bool ok = prepare_calibration_capture_preflight_all_cameras(
            ui_state,
            ecams,
            cameras_params,
            num_cameras,
            light_control_ecam,
            light_control_params,
            mapped_strobe_available,
            light_output_mutation_locked,
            selected_light_handling,
            &status);
        set_calibration_preflight_result(ui_state, ok, status);
    }
    ImGui::EndDisabled();
    const bool has_any_capture_restore =
        !ui_state->calibration_capture_restore_states.empty();
    const bool has_selected_capture_restore =
        has_calibration_capture_restore_state(ui_state, selected_camera.camera_serial);
    const bool can_restore_calibration_capture =
        !light_output_mutation_locked &&
        (has_selected_capture_restore || mapped_strobe_available);
    const bool can_restore_all_calibration_capture =
        !light_output_mutation_locked &&
        (has_any_capture_restore || mapped_strobe_available);
    ImGui::BeginDisabled(!can_restore_calibration_capture);
    if (ImGui::Button("Restore Camera Config State")) {
        std::string status;
        const bool ok = restore_calibration_capture_preflight(
            ui_state,
            &ecams[ui_state->selected_camera],
            &selected_camera,
            light_control_ecam,
            light_control_params,
            mapped_strobe_available,
            light_output_mutation_locked,
            &status);
        set_calibration_preflight_result(ui_state, ok, status);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!can_restore_all_calibration_capture);
    if (ImGui::Button("Restore All Camera Config States")) {
        std::string status;
        const bool ok = restore_calibration_capture_preflight_all_cameras(
            ui_state,
            ecams,
            cameras_params,
            num_cameras,
            light_control_ecam,
            light_control_params,
            mapped_strobe_available,
            light_output_mutation_locked,
            &status);
        set_calibration_preflight_result(ui_state, ok, status);
    }
    ImGui::EndDisabled();
    if (ui_state->calibration_capture_profile_active) {
        ImGui::TextDisabled(
            "Active capture profile: %s capture_cam=%s light_cam=%s",
            ui_state->calibration_capture_profile_id.c_str(),
            ui_state->calibration_capture_profile_camera_serial.c_str(),
            ui_state->calibration_capture_profile_light_camera_serial.empty()
                ? "(none)"
                : ui_state->calibration_capture_profile_light_camera_serial.c_str());
    }
    if (light_output_mutation_locked) {
        ImGui::TextDisabled("Calibration prepare/restore actions are disabled while recording/finalizing.");
    } else if (light_handling_needs_mapped_strobe && light_control_camera_indices.empty()) {
        ImGui::TextDisabled("No open camera has an exposed nir_strobe_trigger output mapping.");
    } else if (light_handling_needs_mapped_strobe && !mapped_strobe_available) {
        ImGui::TextDisabled("Choose a light-control camera with an exposed nir_strobe_trigger output mapping.");
    } else {
        ImGui::TextDisabled(
            "Prepare applies selected light handling first, then temporarily sets camera timing to 10 FPS / 10 ms. All-camera prepare uses the same light-first ordering across open cameras.");
    }
    if (!ui_state->calibration_preflight_error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                           "%s",
                           ui_state->calibration_preflight_error.c_str());
    } else if (!ui_state->calibration_preflight_status.empty()) {
        ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.45f, 1.0f),
                           "%s",
                           ui_state->calibration_preflight_status.c_str());
    }

    render_string_preset_combo(
        "Light state",
        &ui_state->calibration_light_state,
        kLightStatePresets,
        IM_ARRAYSIZE(kLightStatePresets));
    if (ui_state->calibration_image_set_purpose == "scale_image" &&
        ui_state->calibration_image_set_target_plane == "projected_surface") {
        ImGui::TextDisabled("Projection-surface scale images usually keep the TTL NIR strobe active so a clear ruler/target is visible to the camera.");
    } else if (ui_state->calibration_image_set_purpose == "scale_image") {
        ImGui::TextDisabled("Fish-plane scale images usually keep the TTL NIR strobe active so a ruler/target is visible to the camera.");
    } else if (ui_state->calibration_image_set_purpose == "arena_projection" ||
               ui_state->calibration_image_set_purpose == "homography_grid" ||
               ui_state->calibration_image_set_purpose == "crosshair_alignment") {
        ImGui::TextDisabled("Projector-pattern captures usually suppress mapped NIR strobe pulses and rely on the visible projection.");
    }
    render_string_preset_combo(
        "Illumination spectrum",
        &ui_state->calibration_illumination_spectrum,
        kIlluminationSpectrumPresets,
        IM_ARRAYSIZE(kIlluminationSpectrumPresets));
    ImGui::Checkbox(
        "Center wavelength nm",
        &ui_state->calibration_has_illumination_center_wavelength_nm);
    ImGui::SameLine();
    ImGui::BeginDisabled(!ui_state->calibration_has_illumination_center_wavelength_nm);
    ImGui::InputDouble(
        "##IlluminationCenterWavelengthNm",
        &ui_state->calibration_illumination_center_wavelength_nm,
        1.0,
        10.0,
        "%.1f");
    ImGui::EndDisabled();
    ImGui::Checkbox(
        "Min wavelength nm",
        &ui_state->calibration_has_illumination_min_wavelength_nm);
    ImGui::SameLine();
    ImGui::BeginDisabled(!ui_state->calibration_has_illumination_min_wavelength_nm);
    ImGui::InputDouble(
        "##IlluminationMinWavelengthNm",
        &ui_state->calibration_illumination_min_wavelength_nm,
        1.0,
        10.0,
        "%.1f");
    ImGui::EndDisabled();
    ImGui::Checkbox(
        "Max wavelength nm",
        &ui_state->calibration_has_illumination_max_wavelength_nm);
    ImGui::SameLine();
    ImGui::BeginDisabled(!ui_state->calibration_has_illumination_max_wavelength_nm);
    ImGui::InputDouble(
        "##IlluminationMaxWavelengthNm",
        &ui_state->calibration_illumination_max_wavelength_nm,
        1.0,
        10.0,
        "%.1f");
    ImGui::EndDisabled();
    ImGui::Checkbox(
        "Bandwidth FWHM nm",
        &ui_state->calibration_has_illumination_bandwidth_fwhm_nm);
    ImGui::SameLine();
    ImGui::BeginDisabled(!ui_state->calibration_has_illumination_bandwidth_fwhm_nm);
    ImGui::InputDouble(
        "##IlluminationBandwidthFwhmNm",
        &ui_state->calibration_illumination_bandwidth_fwhm_nm,
        1.0,
        10.0,
        "%.1f");
    ImGui::EndDisabled();
    render_string_preset_combo(
        "Wavelength confidence",
        &ui_state->calibration_illumination_wavelength_confidence,
        kIlluminationConfidencePresets,
        IM_ARRAYSIZE(kIlluminationConfidencePresets));
    ui_state->calibration_illumination_center_wavelength_nm =
        std::max(0.0, ui_state->calibration_illumination_center_wavelength_nm);
    ui_state->calibration_illumination_min_wavelength_nm =
        std::max(0.0, ui_state->calibration_illumination_min_wavelength_nm);
    ui_state->calibration_illumination_max_wavelength_nm =
        std::max(0.0, ui_state->calibration_illumination_max_wavelength_nm);
    ui_state->calibration_illumination_bandwidth_fwhm_nm =
        std::max(0.0, ui_state->calibration_illumination_bandwidth_fwhm_nm);
    render_string_preset_combo(
        "Projector state",
        &ui_state->calibration_projector_state,
        kProjectorStatePresets,
        IM_ARRAYSIZE(kProjectorStatePresets));
    ImGui::Checkbox(
        "Projector visible to camera",
        &ui_state->calibration_projector_visible_to_camera);
    ImGui::Checkbox(
        "Requires repeatable filter reinstall",
        &ui_state->calibration_requires_filter_reinstalled_repeatably);
    ImGui::InputTextMultiline(
        "Operator notes",
        &ui_state->calibration_operator_notes,
        ImVec2(-1.0f, ImGui::GetTextLineHeight() * 3.0f));

    ImGui::SeparatorText("Generic Calibration Image Set");
    static constexpr const char* kImageSetPurposePresets[] = {
        "arena_projection",
        "homography_grid",
        "scale_image",
        "crosshair_alignment"
    };
    static constexpr const char* kTargetPlanePresets[] = {
        "projected_surface",
        "tank_bottom_outer_surface",
        "tank_bottom_inner_surface",
        "estimated_fish_plane",
        "dish_top_rim",
        "unknown"
    };
    static constexpr const char* kImageRolePresets[] = {
        "projected_arena",
        "grid_on",
        "scale_target",
        "crosshair_on",
        "source"
    };
    if (ImGui::BeginCombo(
            "Image-set purpose",
            ui_state->calibration_image_set_purpose.empty()
                ? "homography_grid"
                : ui_state->calibration_image_set_purpose.c_str())) {
        for (const char* purpose : kImageSetPurposePresets) {
            const bool selected = ui_state->calibration_image_set_purpose == purpose;
            if (ImGui::Selectable(purpose, selected)) {
                apply_calibration_image_set_purpose_defaults(ui_state, purpose);
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    render_string_preset_combo(
        "Target plane",
        &ui_state->calibration_image_set_target_plane,
        kTargetPlanePresets,
        IM_ARRAYSIZE(kTargetPlanePresets));
    render_string_preset_combo(
        "Image role",
        &ui_state->calibration_image_set_image_role,
        kImageRolePresets,
        IM_ARRAYSIZE(kImageRolePresets));
    ImGui::InputText(
        "Projected pattern ID",
        &ui_state->calibration_image_set_projected_pattern_id);
    ImGui::InputText(
        "Projected pattern type",
        &ui_state->calibration_image_set_projected_pattern_type);
    ImGui::InputText(
        "Scale target type",
        &ui_state->calibration_image_set_scale_target_type);
    ImGui::InputTextMultiline(
        "Image-set notes",
        &ui_state->calibration_image_set_notes,
        ImVec2(-1.0f, ImGui::GetTextLineHeight() * 2.0f));
    ImGui::TextDisabled(
        "Use this for piecewise homography/scale/crosshair image artifacts. "
        "It writes only the source image and image_set.json; Citrus fits and accepts later.");

    ImGui::SeparatorText("Persistence");
    ImGui::Text("Calibration session: %s",
                ui_state->calibration_session_id.empty()
                    ? "(not started; first save creates one)"
                    : ui_state->calibration_session_id.c_str());
    if (!ui_state->calibration_session_dir.empty()) {
        ImGui::TextDisabled("%s", ui_state->calibration_session_dir.c_str());
    }
    if (ImGui::Button("Start New Calibration Session")) {
        clear_spatial_calibration_session(ui_state);
        ui_state->persistence_status = "Next save will start a new calibration session.";
        ui_state->persistence_error.clear();
    }
    const bool captured_in_full_resolution =
        !ui_state->has_capture ||
        ui_state->captured_source_array_role.empty() ||
        ui_state->captured_source_array_role == "images_full";
    const bool top_rim_save_busy = top_rim_observation_save_worker().IsBusy();
    const bool generic_image_set_save_busy =
        generic_calibration_image_set_save_worker().IsBusy() ||
        queued_generic_calibration_image_set_save_job_count() > 0;
    const bool spatial_save_busy = top_rim_save_busy || generic_image_set_save_busy;
    const bool can_save_top_rim_observation =
        ui_state->has_capture &&
        ui_state->dish_mask_runtime.has_geometry &&
        captured_in_full_resolution &&
        citrus_template_matches_selected_camera &&
        !spatial_save_busy;
    ImGui::BeginDisabled(!can_save_top_rim_observation);
    if (ImGui::Button("Save Top-Rim Observation")) {
        TopRimObservationSaveJob job;
        std::string error;
        std::string session_artifact_root;
        if (!ensure_spatial_calibration_session(
                ui_state,
                selected_camera,
                artifact_root_dir,
                &session_artifact_root,
                &error) ||
            !prepare_dish_top_rim_observation_save_job_from_spatial_layout(
                ui_state,
                selected_camera,
                session_artifact_root,
                &job,
                &error)) {
            ui_state->persistence_error = error;
            ui_state->persistence_status.clear();
        } else {
            job.session_dir = ui_state->calibration_session_dir;
            if (!top_rim_observation_save_worker().Submit(std::move(job), &error)) {
                ui_state->persistence_error = error;
                ui_state->persistence_status.clear();
            } else {
                ui_state->persistence_status =
                    "Saving top-rim observation artifact in session " +
                    ui_state->calibration_session_id + "...";
                ui_state->persistence_error.clear();
            }
        }
    }
    ImGui::EndDisabled();
    if (top_rim_save_busy) {
        ImGui::TextDisabled("Top-rim observation save is running in the background.");
    }
    const bool can_save_generic_image_set =
        ui_state->has_capture &&
        captured_in_full_resolution &&
        citrus_template_matches_selected_camera &&
        !spatial_save_busy;
    ImGui::SameLine();
    ImGui::BeginDisabled(!can_save_generic_image_set);
    if (ImGui::Button("Save Calibration Image Set")) {
        GenericCalibrationImageSetSaveJob job;
        std::string error;
        std::string session_artifact_root;
        if (!ensure_spatial_calibration_session(
                ui_state,
                selected_camera,
                artifact_root_dir,
                &session_artifact_root,
                &error) ||
            !prepare_generic_calibration_image_set_save_job_from_spatial_layout(
                ui_state,
                selected_camera,
                session_artifact_root,
                &job,
                &error)) {
            ui_state->persistence_error = error;
            ui_state->persistence_status.clear();
        } else {
            job.session_dir = ui_state->calibration_session_dir;
            if (!generic_calibration_image_set_save_worker().Submit(std::move(job), &error)) {
                ui_state->persistence_error = error;
                ui_state->persistence_status.clear();
            } else {
                ui_state->persistence_status =
                    "Saving calibration image-set artifact in session " +
                    ui_state->calibration_session_id + "...";
                ui_state->persistence_error.clear();
            }
        }
    }
    ImGui::EndDisabled();
    if (generic_image_set_save_busy) {
        ImGui::TextDisabled("Calibration image-set save is running in the background.");
    }
    const bool can_save_group_image_sets =
        !ui_state->group_captures.empty() &&
        pending_group_snapshot_count(*ui_state) == 0 &&
        !spatial_save_busy;
    ImGui::SameLine();
    ImGui::BeginDisabled(!can_save_group_image_sets);
    if (ImGui::Button("Save Group Calibration Image Sets")) {
        std::string status;
        std::string error;
        if (!queue_group_calibration_image_set_save_jobs(
                ui_state,
                cameras_params,
                num_cameras,
                selected_camera,
                artifact_root_dir,
                &status,
                &error)) {
            ui_state->persistence_error = error;
            ui_state->persistence_status.clear();
        } else {
            ui_state->persistence_status = status;
            ui_state->persistence_error.clear();
        }
    }
    ImGui::EndDisabled();
    if (!ui_state->group_captures.empty()) {
        ImGui::TextDisabled(
            "Grouped save writes one image_set.json per captured camera and ties them with capture_group_id=%s.",
            ui_state->group_capture_id.c_str());
    }
    if (ui_state->has_capture && !captured_in_full_resolution) {
        ImGui::TextDisabled(
            "Top-rim observations and calibration image sets require full-resolution camera coordinates. "
            "This live snapshot is preview/downsample space only.");
    }
    if (!citrus_template_matches_selected_camera) {
        ImGui::TextDisabled(
            "Spatial calibration saves are blocked until the active Citrus template camera matches the selected Orange camera.");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!citrus_template_matches_selected_camera);
    if (ImGui::Button("Save Arena Layout Artifact")) {
        std::string status;
        std::string error;
        std::string session_artifact_root;
        if (!ensure_spatial_calibration_session(
                ui_state,
                selected_camera,
                artifact_root_dir,
                &session_artifact_root,
                &error) ||
            !save_spatial_layout_artifact(
                ui_state,
                selected_camera,
                session_artifact_root,
                ui_state->calibration_session_dir,
                &status,
                &error)) {
            ui_state->persistence_error = error;
            ui_state->persistence_status.clear();
        } else {
            ui_state->persistence_status = status;
            ui_state->persistence_error.clear();
            rebuild_schema_preview(ui_state, &selected_camera);
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Load Arena Layout Artifact...")) {
        IGFD::FileDialogConfig config;
        config.path = !ui_state->calibration_session_dir.empty()
                          ? ui_state->calibration_session_dir
                          : (artifact_root_dir.empty() ? "." : artifact_root_dir);
        config.countSelectionMax = 1;
        ImGuiFileDialog::Instance()->OpenDialog(
            kLoadSpatialLayoutDialogId,
            "Choose Arena Layout JSON",
            ".json",
            config);
    }
    ImGui::TextDisabled(
        "Spatial calibration saves are grouped under calibrations/sessions/<session_id>/artifacts/<artifact_id>. Arena save writes %s, %s, %s, and %s.",
        kSpatialLayoutMeasurementFilename,
        kSpatialLayoutManifestFilename,
        kSpatialLayoutArenaLayoutRuntimeFilename,
        kSpatialLayoutDishMaskRuntimeFilename);

    ImGui::Separator();
    ImGui::Text("Preview valid: %s", ui_state->preview_valid ? "yes" : "no");

    if (ImGui::TreeNode("Canonical Layout JSON")) {
        if (ImGui::SmallButton("Copy canonical JSON")) {
            ImGui::SetClipboardText(ui_state->canonical_layout_json.c_str());
        }
        ImGui::BeginChild("SpatialCanonicalJson", ImVec2(0.0f, 180.0f), true, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(ui_state->canonical_layout_json.c_str());
        ImGui::EndChild();
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Runtime Calibration JSON")) {
        if (ImGui::SmallButton("Copy runtime JSON")) {
            ImGui::SetClipboardText(ui_state->runtime_preview_json.c_str());
        }
        ImGui::BeginChild("SpatialRuntimeJson", ImVec2(0.0f, 220.0f), true, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(ui_state->runtime_preview_json.c_str());
        ImGui::EndChild();
        ImGui::TreePop();
    }

    ImGui::EndChild();
    ImGui::TableNextColumn();
    ImGui::BeginChild("SpatialLayoutFitPreviewPanel", ImVec2(0.0f, 0.0f), true);

    render_group_capture_panels(ui_state, cameras_params, num_cameras);

    ImGui::SeparatorText("Fit Preview");
    const char* canvas_edit_items[] = {"registration", "selected_zone"};
    ImGui::Combo("Canvas edit mode", &ui_state->canvas_edit_mode, canvas_edit_items, IM_ARRAYSIZE(canvas_edit_items));
    if (!ui_state->has_capture) {
        ImGui::TextDisabled("Capture a frame to render the resolved camera-pixel overlays.");
    } else {
        const bool canvas_changed = draw_runtime_preview(ui_state);
        if (canvas_changed) {
            rebuild_schema_preview(ui_state, &selected_camera);
        }
        if (ui_state->canvas_edit_mode == 0) {
            ImGui::TextDisabled("Drag cyan to move the experimental area. Drag gold to scale it. Drag pink to rotate the layout.");
        } else {
            ImGui::TextDisabled("Drag green to move the selected zone. Drag gold/orange handles to resize it.");
        }
        ImGui::TextDisabled("Blue outline/triangle: current Citrus global-canvas homography projection. Pink outline/cross: detected experimental-area proposal. Green outline/diamond/line: corrected Citrus outline preserving current Citrus radius with the proposed center. Orange outline: resolved experimental boundary. Yellow outline: valid region after edge margin. Green/cyan outlines: resolved zone overlays.");
    }

    ImGui::Separator();
    ImGui::TextWrapped("%s", ui_state->preview_status.c_str());
    if (!ui_state->preview_error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", ui_state->preview_error.c_str());
    }
    if (!ui_state->detection_status.empty()) {
        ImGui::TextWrapped("%s", ui_state->detection_status.c_str());
    }
    if (!ui_state->detection_error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "%s", ui_state->detection_error.c_str());
    }
    if (!ui_state->citrus_import_status.empty()) {
        ImGui::TextWrapped("%s", ui_state->citrus_import_status.c_str());
    }
    if (!ui_state->citrus_import_error.empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.35f, 1.0f), "%s", ui_state->citrus_import_error.c_str());
    }
    if (!ui_state->persistence_status.empty()) {
        ImGui::TextWrapped("%s", ui_state->persistence_status.c_str());
    }
    if (!ui_state->persistence_error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "%s", ui_state->persistence_error.c_str());
    }
    ImGui::EndChild();
    ImGui::EndTable();

    ImGui::End();

    if (ImGuiFileDialog::Instance()->Display(kLoadSpatialLayoutDialogId)) {
        if (ImGuiFileDialog::Instance()->IsOk()) {
            std::string status;
            std::string error;
            if (!load_spatial_layout_artifact(
                    ui_state,
                    ImGuiFileDialog::Instance()->GetFilePathName(),
                    &status,
                    &error)) {
                ui_state->persistence_error = error;
                ui_state->persistence_status.clear();
            } else {
                ui_state->persistence_status = status;
                ui_state->persistence_error.clear();
                rebuild_schema_preview(ui_state, &selected_camera);
            }
        }
        ImGuiFileDialog::Instance()->Close();
    }

    if (ImGuiFileDialog::Instance()->Display(kLoadCitrusArenaConfigDialogId)) {
        if (ImGuiFileDialog::Instance()->IsOk()) {
            std::string status;
            std::string error;
            if (!import_citrus_canvas_templates(
                    ui_state,
                    selected_camera,
                    ImGuiFileDialog::Instance()->GetFilePathName(),
                    &status,
                    &error)) {
                ui_state->citrus_import_error = error;
                ui_state->citrus_import_status.clear();
            } else {
                ui_state->citrus_import_status = status;
                ui_state->citrus_import_error.clear();
                rebuild_schema_preview(ui_state, &selected_camera);
            }
        }
        ImGuiFileDialog::Instance()->Close();
    }
}
