#include "projected_center_preflight.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace orange::control {
namespace {

bool json_bool_or(const nlohmann::json& node, const char* key, const bool fallback)
{
    const auto it = node.find(key);
    if (it == node.end() || !it->is_boolean()) {
        return fallback;
    }
    return it->get<bool>();
}

std::string json_string_or(const nlohmann::json& node,
                           const char* key,
                           const std::string& fallback = {})
{
    const auto it = node.find(key);
    if (it == node.end() || !it->is_string()) {
        return fallback;
    }
    return it->get<std::string>();
}

double json_number_or(const nlohmann::json& node, const char* key, const double fallback)
{
    const auto it = node.find(key);
    if (it == node.end() || !it->is_number()) {
        return fallback;
    }
    return it->get<double>();
}

nlohmann::json finite_number_or_null(const double value)
{
    if (!std::isfinite(value)) {
        return nullptr;
    }
    return value;
}

bool is_visible_capture_mode(const std::string& capture_mode)
{
    return capture_mode == "visible_light_calibration_capture" ||
           capture_mode == "visible_projected_crosshair_verification";
}

bool contains_int(const nlohmann::json& array, const int value)
{
    if (!array.is_array()) {
        return false;
    }
    for (const nlohmann::json& item : array) {
        if (item.is_number_integer() && item.get<int>() == value) {
            return true;
        }
    }
    return false;
}

bool outlets_7_8_confirmed_off(const nlohmann::json& suppression)
{
    if (!json_bool_or(suppression, "outlets_confirmed_off", false)) {
        return false;
    }
    const nlohmann::json outlets =
        suppression.value("outlets", nlohmann::json::array());
    return contains_int(outlets, 7) && contains_int(outlets, 8);
}

bool pinout_disabled_confirmed(const nlohmann::json& suppression)
{
    if (json_bool_or(suppression, "trigger_output_disabled_readback", false)) {
        return true;
    }
    return json_bool_or(suppression, "trigger_output_disabled_confirmed", false);
}

} // namespace

bool IsProjectedCenterPreflightMethod(const std::string& method)
{
    return method == kProjectedCenterPreflightMethod;
}

ProjectedCenterPreflightResult BuildProjectedCenterVerificationPreflight(
    const LocalControlStatusSnapshot& status,
    const nlohmann::json& params,
    const std::string& checked_at_utc)
{
    ProjectedCenterPreflightResult result;
    std::vector<std::string>& reasons = result.blocking_reasons;

    const std::string camera_serial = json_string_or(params, "camera_serial");
    const std::string capture_mode = json_string_or(params, "capture_mode");
    const bool projector_visible_to_camera =
        json_bool_or(params, "projector_visible_to_camera", false);
    const double min_exposure_us = json_number_or(
        params,
        "min_exposure_us",
        kProjectedCenterPreflightMinExposureUsDefault);
    const double max_frame_rate_hz = json_number_or(
        params,
        "max_frame_rate_hz",
        kProjectedCenterPreflightMaxFrameRateHzDefault);
    const double exposure_us_requested =
        json_number_or(params, "exposure_us_requested", std::numeric_limits<double>::quiet_NaN());
    const double exposure_us_readback =
        json_number_or(params, "exposure_us_readback", std::numeric_limits<double>::quiet_NaN());
    const double frame_rate_hz_requested =
        json_number_or(params, "frame_rate_hz_requested", std::numeric_limits<double>::quiet_NaN());
    const double frame_rate_hz_readback =
        json_number_or(params, "frame_rate_hz_readback", std::numeric_limits<double>::quiet_NaN());
    const std::string filter_state = json_string_or(params, "filter_state");

    if (status.recording_active) {
        reasons.push_back("recording_active");
    }
    if (status.recording_finalizing) {
        reasons.push_back("recording_finalizing");
    }
    if (camera_serial.empty()) {
        reasons.push_back("missing_camera_serial");
    }
    if (!is_visible_capture_mode(capture_mode)) {
        reasons.push_back("invalid_capture_mode");
    }
    if (!projector_visible_to_camera) {
        reasons.push_back("projector_not_visible_to_camera");
    }
    if (!std::isfinite(exposure_us_requested)) {
        reasons.push_back("missing_exposure_us_requested");
    } else if (exposure_us_requested < min_exposure_us) {
        reasons.push_back("exposure_us_requested_below_minimum");
    }
    if (!std::isfinite(exposure_us_readback)) {
        reasons.push_back("missing_exposure_us_readback");
    } else if (exposure_us_readback < min_exposure_us) {
        reasons.push_back("exposure_us_readback_below_minimum");
    }
    if (!std::isfinite(frame_rate_hz_requested)) {
        reasons.push_back("missing_frame_rate_hz_requested");
    } else if (frame_rate_hz_requested > max_frame_rate_hz) {
        reasons.push_back("frame_rate_hz_requested_above_maximum");
    }
    if (!std::isfinite(frame_rate_hz_readback)) {
        reasons.push_back("missing_frame_rate_hz_readback");
    } else if (frame_rate_hz_readback > max_frame_rate_hz) {
        reasons.push_back("frame_rate_hz_readback_above_maximum");
    }
    if (filter_state != "operator_confirmed_removed") {
        reasons.push_back("filter_not_operator_confirmed_removed");
    }

    const nlohmann::json suppression =
        params.value("light_trigger_suppression", nlohmann::json::object());
    nlohmann::json suppression_effect = nlohmann::json::object();
    const bool suppression_required = (camera_serial == "2010096") ||
        json_bool_or(suppression, "required", false);
    const std::string suppression_method = json_string_or(suppression, "method");
    std::string suppression_status = "not_required";
    bool suppression_verified = !suppression_required;

    if (suppression_required) {
        suppression_verified = false;
        if (suppression_method == "pinout_disabled") {
            suppression_verified = pinout_disabled_confirmed(suppression);
            suppression_status = suppression_verified ? "verified" : "missing_pinout_disabled_confirmation";
            if (!suppression_verified) {
                reasons.push_back("light_trigger_suppression_pinout_not_disabled");
            }
        } else if (suppression_method == "pancake_batter_outlets_7_8_off") {
            suppression_verified = outlets_7_8_confirmed_off(suppression);
            suppression_status = suppression_verified ? "verified" : "missing_outlets_7_8_off_confirmation";
            if (!suppression_verified) {
                reasons.push_back("light_trigger_suppression_outlets_7_8_not_confirmed_off");
            }
        } else {
            suppression_status = "missing_or_unsupported_method";
            reasons.push_back("light_trigger_suppression_missing_or_unsupported_method");
        }
    }

    suppression_effect = {
        {"required", suppression_required},
        {"camera_serial", camera_serial},
        {"accepted_methods", nlohmann::json::array(
            {"pinout_disabled", "pancake_batter_outlets_7_8_off"})},
        {"method", suppression_method},
        {"status", suppression_status},
        {"verified", suppression_verified},
        {"pinout", {
            {"trigger_output_disabled_readback",
             json_bool_or(suppression, "trigger_output_disabled_readback", false)},
            {"trigger_output_disabled_confirmed",
             json_bool_or(suppression, "trigger_output_disabled_confirmed", false)}
        }},
        {"outlets", {
            {"controller", "pancake-batter"},
            {"required_off", nlohmann::json::array({7, 8})},
            {"reported", suppression.value("outlets", nlohmann::json::array())},
            {"outlets_confirmed_off",
             json_bool_or(suppression, "outlets_confirmed_off", false)}
        }}
    };

    result.passed = reasons.empty();
    result.effect = {
        {"status", result.passed ? "pass" : "fail"},
        {"blocking_reasons", reasons},
        {"camera_serial", camera_serial},
        {"capture_mode", capture_mode},
        {"projector_visible_to_camera", projector_visible_to_camera},
        {"exposure_us_requested", finite_number_or_null(exposure_us_requested)},
        {"exposure_us_readback", finite_number_or_null(exposure_us_readback)},
        {"exposure_us_min_required", min_exposure_us},
        {"frame_rate_hz_requested", finite_number_or_null(frame_rate_hz_requested)},
        {"frame_rate_hz_readback", finite_number_or_null(frame_rate_hz_readback)},
        {"frame_rate_hz_max_allowed", max_frame_rate_hz},
        {"filter_state", filter_state},
        {"light_trigger_suppression", suppression_effect},
        {"recording_state", {
            {"recording_active", status.recording_active},
            {"recording_finalizing", status.recording_finalizing}
        }},
        {"checked_at_utc", checked_at_utc}
    };
    return result;
}

} // namespace orange::control
