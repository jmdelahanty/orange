#include "gui/spatial_layout/preflight.h"

#include "project.h"

#include <algorithm>
#include <sstream>
#include <vector>

namespace orange::gui::spatial_layout {
namespace {

constexpr const char* kCalibrationCaptureProfileId =
    "spatial_layout_visible_long_exposure_v1";
constexpr unsigned int kCalibrationCaptureFrameRateHz = 10;
constexpr unsigned int kCalibrationCaptureExposureUs = 10000;

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

std::string make_spatial_rig_io_connection_key(const CameraParams& camera_params,
                                               const CameraRigIoConnection& connection)
{
    return camera_params.camera_serial + ":" + connection.camera_line + ":" + connection.purpose;
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

bool has_calibration_capture_restore_state_internal(
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

}  // namespace

const CameraRigIoConnection* find_mapped_nir_strobe_output_connection(
    const CameraParams& camera_params)
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

bool has_calibration_capture_restore_state(
    const SpatialLayoutUiState* ui_state,
    const std::string& camera_serial)
{
    return has_calibration_capture_restore_state_internal(ui_state, camera_serial);
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
        has_calibration_capture_restore_state_internal(ui_state, capture_params->camera_serial)) {
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
    const int num_cameras,
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
        if (!has_calibration_capture_restore_state_internal(ui_state, camera_params->camera_serial)) {
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
    const int num_cameras,
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

}  // namespace orange::gui::spatial_layout
