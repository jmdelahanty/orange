#pragma once

#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <string>

namespace orange::analytics_mask {

enum class Mode {
    kOff,
    kAudit,
    kGateOnly,
    kGateAndInputMask,
};

inline const char* mode_to_string(const Mode mode)
{
    switch (mode) {
    case Mode::kOff:
        return "off";
    case Mode::kAudit:
        return "audit";
    case Mode::kGateOnly:
        return "gate_only";
    case Mode::kGateAndInputMask:
        return "gate_and_input_mask";
    }
    return "off";
}

inline bool parse_mode(const std::string& value, Mode* mode_out)
{
    if (!mode_out) {
        return false;
    }
    if (value.empty() || value == "off") {
        *mode_out = Mode::kOff;
        return true;
    }
    if (value == "audit") {
        *mode_out = Mode::kAudit;
        return true;
    }
    if (value == "gate_only") {
        *mode_out = Mode::kGateOnly;
        return true;
    }
    if (value == "gate_and_input_mask") {
        *mode_out = Mode::kGateAndInputMask;
        return true;
    }
    return false;
}

inline bool evaluates_centroid(const Mode mode)
{
    return mode != Mode::kOff;
}

inline bool enforces_centroid(const Mode mode)
{
    return mode == Mode::kGateOnly || mode == Mode::kGateAndInputMask;
}

inline bool masks_input(const Mode mode)
{
    return mode == Mode::kGateAndInputMask;
}

struct RuntimeConfig {
    Mode mode = Mode::kOff;
    float input_context_outset_px = 0.0f;
    int apply_timeout_ms = 750;
    std::string source = "default_off";
};

struct RuntimeConfigResolveResult {
    bool ok = false;
    bool mode_was_explicit = false;
    RuntimeConfig config;
    std::string error;
};

inline RuntimeConfigResolveResult resolve_runtime_config_from_environment()
{
    RuntimeConfigResolveResult result;
    const char* mode_env = std::getenv("ORANGE_YOLO_SPATIAL_MASK_MODE");
    result.mode_was_explicit = mode_env && *mode_env;
    std::string mode = result.mode_was_explicit ? mode_env : "off";
    std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char ch) {
        return ch == '-' ? '_' : static_cast<char>(std::tolower(ch));
    });
    if (!parse_mode(mode, &result.config.mode)) {
        result.error =
            "ORANGE_YOLO_SPATIAL_MASK_MODE must be "
            "off|audit|gate_only|gate_and_input_mask";
        return result;
    }
    result.config.source = result.mode_was_explicit
        ? "environment"
        : "default_off";

    const char* outset_env = std::getenv(
        "ORANGE_YOLO_SPATIAL_MASK_INPUT_CONTEXT_OUTSET_PX");
    if (outset_env && *outset_env) {
        char* end = nullptr;
        const double value = std::strtod(outset_env, &end);
        if (end == outset_env || *end != '\0' || !std::isfinite(value) ||
            value < 0.0 || value > 10000.0) {
            result.error =
                "ORANGE_YOLO_SPATIAL_MASK_INPUT_CONTEXT_OUTSET_PX "
                "must be finite and within [0,10000]";
            return result;
        }
        result.config.input_context_outset_px = static_cast<float>(value);
    }

    const char* timeout_env = std::getenv(
        "ORANGE_YOLO_SPATIAL_MASK_APPLY_TIMEOUT_MS");
    if (timeout_env && *timeout_env) {
        char* end = nullptr;
        const long value = std::strtol(timeout_env, &end, 10);
        if (end == timeout_env || *end != '\0' || value < 50 || value > 10000) {
            result.error =
                "ORANGE_YOLO_SPATIAL_MASK_APPLY_TIMEOUT_MS must be "
                "within [50,10000]";
            return result;
        }
        result.config.apply_timeout_ms = static_cast<int>(value);
    }
    result.ok = true;
    return result;
}

struct Circle {
    float cx = 0.0f;
    float cy = 0.0f;
    float radius = 0.0f;
};

inline bool valid_circle(const Circle& circle)
{
    return std::isfinite(circle.cx) && std::isfinite(circle.cy) &&
        std::isfinite(circle.radius) && circle.radius > 0.0f;
}

struct Policy {
    Mode mode = Mode::kOff;
    int source_width = 0;
    int source_height = 0;
    Circle input_circle;
    Circle centroid_gate_circle;
    float input_context_outset_px = 0.0f;
    float outside_tensor_value = 0.0f;
    std::string camera_serial;
    std::string arena_id;
    std::string registration_id;
    std::string artifact_id;
    std::string artifact_schema_id;
    int artifact_schema_version = 0;
    std::string artifact_fingerprint;
    std::string source_path;
    std::string recording_relative_path;
    std::string source_sha256;
};

inline bool validate_policy(const Policy& policy, std::string* error_out = nullptr)
{
    if (error_out) {
        error_out->clear();
    }
    if (policy.mode == Mode::kOff) {
        return true;
    }
    const auto fail = [&](const std::string& error) {
        if (error_out) {
            *error_out = error;
        }
        return false;
    };
    if (policy.camera_serial.empty()) {
        return fail("spatial mask camera serial is missing");
    }
    if (policy.arena_id.empty() || policy.registration_id.empty()) {
        return fail("spatial mask arena or daily-registration identity is missing");
    }
    if (policy.source_width <= 0 || policy.source_height <= 0) {
        return fail("spatial mask source raster is invalid");
    }
    if (!valid_circle(policy.centroid_gate_circle)) {
        return fail("spatial mask centroid-gate circle is invalid");
    }
    if (!valid_circle(policy.input_circle)) {
        return fail("spatial mask input circle is invalid");
    }
    if (!std::isfinite(policy.input_context_outset_px) ||
        policy.input_context_outset_px < 0.0f) {
        return fail("spatial mask input context outset is invalid");
    }
    if (!std::isfinite(policy.outside_tensor_value) ||
        policy.outside_tensor_value < 0.0f ||
        policy.outside_tensor_value > 1.0f) {
        return fail("spatial mask outside tensor value must be within [0,1]");
    }
    if (policy.artifact_id.empty() ||
        policy.artifact_schema_id !=
            "orange.calibration.dish_top_rim_observation" ||
        policy.artifact_schema_version != 2) {
        return fail("spatial mask requires an identified schema-v2 top-rim observation");
    }
    if (policy.source_sha256.rfind("sha256:", 0) != 0 ||
        policy.source_sha256.size() != 71) {
        return fail("spatial mask source SHA-256 identity is missing or invalid");
    }
    for (std::size_t index = 7; index < policy.source_sha256.size(); ++index) {
        if (!std::isxdigit(
                static_cast<unsigned char>(policy.source_sha256[index]))) {
            return fail("spatial mask source SHA-256 contains non-hex characters");
        }
    }
    if (policy.recording_relative_path.empty() ||
        policy.recording_relative_path.front() == '/' ||
        policy.recording_relative_path == ".." ||
        policy.recording_relative_path.rfind("../", 0) == 0 ||
        policy.recording_relative_path.find("/../") != std::string::npos) {
        return fail("spatial mask recording-local source path is missing or invalid");
    }
    if (policy.centroid_gate_circle.cx < 0.0f ||
        policy.centroid_gate_circle.cy < 0.0f ||
        policy.centroid_gate_circle.cx >= static_cast<float>(policy.source_width) ||
        policy.centroid_gate_circle.cy >= static_cast<float>(policy.source_height)) {
        return fail("spatial mask center is outside the camera-native raster");
    }
    const float maximum_reasonable_radius =
        2.0f * std::hypot(static_cast<float>(policy.source_width),
                          static_cast<float>(policy.source_height));
    if (policy.input_circle.radius > maximum_reasonable_radius ||
        policy.centroid_gate_circle.radius > maximum_reasonable_radius) {
        return fail("spatial mask radius is not plausible for the camera-native raster");
    }
    return true;
}

struct GateDecision {
    float centroid_x = 0.0f;
    float centroid_y = 0.0f;
    float signed_boundary_distance_px = 0.0f;
    bool inside = false;
};

inline GateDecision evaluate_box_centroid(const float x,
                                          const float y,
                                          const float width,
                                          const float height,
                                          const Circle& gate)
{
    GateDecision result;
    result.centroid_x = x + width * 0.5f;
    result.centroid_y = y + height * 0.5f;
    const float dx = result.centroid_x - gate.cx;
    const float dy = result.centroid_y - gate.cy;
    const float distance = std::sqrt(dx * dx + dy * dy);
    result.signed_boundary_distance_px = gate.radius - distance;
    result.inside = result.signed_boundary_distance_px >= 0.0f;
    return result;
}

namespace detail {

inline const nlohmann::json* object_member(const nlohmann::json& object,
                                           const char* key)
{
    if (!object.is_object()) {
        return nullptr;
    }
    const auto it = object.find(key);
    return it != object.end() && it->is_object() ? &*it : nullptr;
}

inline bool finite_number(const nlohmann::json& object,
                          const char* key,
                          float* value_out)
{
    if (!value_out || !object.is_object()) {
        return false;
    }
    const auto it = object.find(key);
    if (it == object.end() || !it->is_number()) {
        return false;
    }
    const double value = it->get<double>();
    if (!std::isfinite(value)) {
        return false;
    }
    *value_out = static_cast<float>(value);
    return std::isfinite(*value_out);
}

inline bool circle_from_observation_geometry(const nlohmann::json& geometry,
                                             Circle* circle_out)
{
    if (!circle_out || !geometry.is_object()) {
        return false;
    }
    const auto type_it = geometry.find("type");
    if (type_it != geometry.end() &&
        (!type_it->is_string() || type_it->get<std::string>() != "circle")) {
        return false;
    }
    const nlohmann::json* center = object_member(geometry, "center_px");
    Circle circle;
    if (!center ||
        !finite_number(*center, "x", &circle.cx) ||
        !finite_number(*center, "y", &circle.cy) ||
        !finite_number(geometry, "radius_px", &circle.radius) ||
        !valid_circle(circle)) {
        return false;
    }
    *circle_out = circle;
    return true;
}

inline std::string string_or_empty(const nlohmann::json& object, const char* key)
{
    if (!object.is_object()) {
        return {};
    }
    const auto it = object.find(key);
    return it != object.end() && it->is_string()
        ? it->get<std::string>()
        : std::string();
}

inline int positive_int_or_zero(const nlohmann::json& object, const char* key)
{
    if (!object.is_object()) {
        return 0;
    }
    const auto it = object.find(key);
    if (it == object.end() || !it->is_number_integer()) {
        return 0;
    }
    const int value = it->get<int>();
    return value > 0 ? value : 0;
}

}  // namespace detail

struct ResolveResult {
    bool ok = false;
    Policy policy;
    std::string error_code;
    std::string error;
};

inline ResolveResult resolve_policy_from_recording_geometry_contract(
    const nlohmann::json& contract,
    const std::string& camera_serial,
    const int expected_source_width,
    const int expected_source_height,
    const Mode mode,
    const float input_context_outset_px)
{
    ResolveResult result;
    result.policy.mode = mode;
    result.policy.camera_serial = camera_serial;
    result.policy.input_context_outset_px = input_context_outset_px;
    if (mode == Mode::kOff) {
        result.ok = true;
        return result;
    }
    const auto fail = [&](const std::string& error,
                          const std::string& error_code = "invalid_selected") {
        result.error_code = error_code;
        result.error = error;
        return result;
    };
    if (!contract.is_object()) {
        return fail("recording geometry contract is missing", "invalid_contract");
    }
    const auto contract_version = contract.find("schema_version");
    if (detail::string_or_empty(contract, "schema_id") !=
            "orange.recording.geometry_contract" ||
        contract_version == contract.end() ||
        !contract_version->is_number_integer() ||
        contract_version->get<int>() != 1) {
        return fail("recording geometry contract schema identity is invalid",
                    "invalid_contract");
    }
    const nlohmann::json* cameras = detail::object_member(contract, "cameras");
    if (!cameras) {
        return fail("recording geometry contract has no camera map",
                    "invalid_contract");
    }
    const auto camera_it = cameras->find(camera_serial);
    if (camera_it == cameras->end() || !camera_it->is_object()) {
        return fail("recording geometry contract has no entry for camera " +
                    camera_serial, "missing_required");
    }
    const nlohmann::json* registration = nullptr;
    const nlohmann::json* physical =
        detail::object_member(*camera_it, "physical_registration");
    if (physical) {
        const std::string status = detail::string_or_empty(*physical, "status");
        if (status == "selected_resolved") {
            registration = physical;
        } else if (status == "invalid_selected" ||
                   status == "invalid_pointer") {
            return fail("camera has an invalid selected physical registration",
                        "invalid_selected");
        }
    }
    if (!registration) {
        const nlohmann::json* daily =
            detail::object_member(*camera_it, "daily_registration_geometry");
        if (daily && detail::string_or_empty(*daily, "status") == "resolved" &&
            detail::string_or_empty(*daily, "mode") ==
                "selected_daily_registration") {
            registration = daily;
        }
    }
    if (!registration) {
        return fail("camera does not have an exact selected physical registration",
                    "missing_required");
    }
    const nlohmann::json* entry =
        detail::object_member(*registration, "recording_snapshot_entry");
    if (!entry) {
        return fail("selected physical registration has no embedded rim observation");
    }
    result.policy.registration_id =
        detail::string_or_empty(*registration, "registration_id");
    if (result.policy.registration_id.empty()) {
        result.policy.registration_id =
            detail::string_or_empty(*registration, "artifact_id");
    }
    result.policy.artifact_id = detail::string_or_empty(*entry, "artifact_id");
    result.policy.artifact_schema_id =
        detail::string_or_empty(*entry, "artifact_schema_id");
    const auto schema_version_it = entry->find("artifact_schema_version");
    result.policy.artifact_schema_version =
        schema_version_it != entry->end() && schema_version_it->is_number_integer()
            ? schema_version_it->get<int>()
            : 0;
    result.policy.arena_id = detail::string_or_empty(*entry, "arena_id");
    const std::string entry_camera =
        detail::string_or_empty(*entry, "camera_serial");
    if (!entry_camera.empty() && entry_camera != camera_serial) {
        return fail("physical rim camera identity does not match the active camera");
    }
    if (detail::string_or_empty(*entry, "coordinate_space") !=
        "camera_native_pixels") {
        return fail("physical rim geometry is not in camera-native pixels");
    }
    const auto available_it =
        entry->find("available_for_downstream_detection_gating");
    if (available_it == entry->end() || !available_it->is_boolean() ||
        !available_it->get<bool>()) {
        return fail("physical rim geometry is not approved for detection gating");
    }
    if (detail::string_or_empty(*entry, "gating_semantics") !=
        "bounding_box_centroid_inside_valid_detection_region") {
        return fail("physical rim gating semantics are missing or unsupported");
    }
    const nlohmann::json* operator_review =
        detail::object_member(*entry, "operator_review");
    const auto accepted_it = operator_review
        ? operator_review->find("accepted")
        : nlohmann::json::const_iterator{};
    if (!operator_review || accepted_it == operator_review->end() ||
        !accepted_it->is_boolean() || !accepted_it->get<bool>()) {
        return fail("physical rim observation was not accepted by the operator");
    }

    const nlohmann::json* camera = detail::object_member(*entry, "camera");
    int declared_width = camera ? detail::positive_int_or_zero(*camera, "width") : 0;
    int declared_height = camera ? detail::positive_int_or_zero(*camera, "height") : 0;
    result.policy.source_width = declared_width > 0
        ? declared_width
        : expected_source_width;
    result.policy.source_height = declared_height > 0
        ? declared_height
        : expected_source_height;
    if (expected_source_width <= 0 || expected_source_height <= 0 ||
        result.policy.source_width != expected_source_width ||
        result.policy.source_height != expected_source_height) {
        return fail("physical rim raster does not match the active camera raster");
    }

    const nlohmann::json* valid_region =
        detail::object_member(*entry, "valid_detection_region");
    const nlohmann::json* valid_geometry = valid_region
        ? detail::object_member(*valid_region, "geometry")
        : nullptr;
    if (!valid_region || !valid_geometry ||
        detail::string_or_empty(*valid_region, "coordinate_space") !=
            "camera_native_pixels" ||
        detail::string_or_empty(*valid_region, "purpose") !=
            "bounding_box_centroid_detection_gating" ||
        !detail::circle_from_observation_geometry(
            *valid_geometry, &result.policy.centroid_gate_circle)) {
        return fail("physical rim valid detection circle is missing or invalid");
    }
    const std::string offset_direction =
        detail::string_or_empty(*valid_region, "offset_direction");
    if (offset_direction != "outward" && offset_direction != "none") {
        return fail("physical rim centroid gate does not use outward/none semantics");
    }
    if (!std::isfinite(input_context_outset_px) ||
        input_context_outset_px < 0.0f) {
        return fail("input context outset is invalid");
    }
    result.policy.input_circle = result.policy.centroid_gate_circle;
    result.policy.input_circle.radius += input_context_outset_px;

    const nlohmann::json* calibration_ref =
        detail::object_member(*entry, "calibration_ref");
    result.policy.artifact_fingerprint = calibration_ref
        ? detail::string_or_empty(*calibration_ref, "fingerprint")
        : std::string();
    const nlohmann::json* source = detail::object_member(*entry, "source");
    result.policy.source_path = source
        ? detail::string_or_empty(*source, "path")
        : std::string();
    result.policy.recording_relative_path = source
        ? detail::string_or_empty(
              *source, "intended_recording_relative_path")
        : std::string();
    result.policy.source_sha256 = source
        ? detail::string_or_empty(*source, "sha256")
        : std::string();
    if (result.policy.artifact_fingerprint.empty()) {
        result.policy.artifact_fingerprint = result.policy.source_sha256;
    }

    std::string validation_error;
    if (!validate_policy(result.policy, &validation_error)) {
        return fail(validation_error);
    }
    result.ok = true;
    result.error_code.clear();
    return result;
}

inline nlohmann::json circle_to_json(const Circle& circle)
{
    return {
        {"type", "circle"},
        {"coordinate_space", "camera_native_pixels"},
        {"cx", circle.cx},
        {"cy", circle.cy},
        {"radius", circle.radius},
    };
}

inline nlohmann::json policy_to_json(const Policy& policy)
{
    return {
        {"schema_id", "orange.analytics.spatial_mask_runtime"},
        {"schema_version", 1},
        {"mode", mode_to_string(policy.mode)},
        {"camera_serial", policy.camera_serial},
        {"arena_id", policy.arena_id},
        {"registration_id", policy.registration_id},
        {"source_raster", {
            {"width", policy.source_width},
            {"height", policy.source_height},
        }},
        {"input_mask", {
            {"enabled", masks_input(policy.mode)},
            {"outside_tensor_value", policy.outside_tensor_value},
            {"input_context_outset_px", policy.input_context_outset_px},
            {"geometry", circle_to_json(policy.input_circle)},
        }},
        {"centroid_gate", {
            {"evaluated", evaluates_centroid(policy.mode)},
            {"enforced", enforces_centroid(policy.mode)},
            {"geometry", circle_to_json(policy.centroid_gate_circle)},
        }},
        {"source", {
            {"artifact_id", policy.artifact_id},
            {"artifact_schema_id", policy.artifact_schema_id},
            {"artifact_schema_version", policy.artifact_schema_version},
            {"artifact_fingerprint", policy.artifact_fingerprint},
            {"path", policy.source_path},
            {"recording_relative_path", policy.recording_relative_path},
            {"sha256", policy.source_sha256},
        }},
    };
}

}  // namespace orange::analytics_mask
