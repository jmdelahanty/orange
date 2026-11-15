#pragma once

#include "json.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace orange::gui::spatial_layout {

inline constexpr const char* kActivePhysicalRegistrationPointerSchemaId =
    "orange.calibration.active_physical_registration_pointer";
// Version 2 (2026-11-15) adds the selection's freshness window, the group
// acceptance that made it, and explicit reuse of an older fit. Version 1
// pointers stay readable (their window starts at the fit's time).
inline constexpr int kActivePhysicalRegistrationPointerSchemaVersion = 2;
inline constexpr int kActivePhysicalRegistrationPointerReadableV1 = 1;

// Jeremy 2026-11-15: a physical registration counts for one day. The window
// starts at the fit's own recorded time (observation created_utc), or, for an
// explicit reuse of an older fit, at the re-selection; never at file times.
inline constexpr const char* kPhysicalRegistrationFreshnessRule =
    "orange_physical_registration_freshness_24h_v1";
inline constexpr long long kPhysicalRegistrationFreshnessSeconds = 24LL * 3600LL;

// How a pointer came to select its artifact.
inline constexpr const char* kSelectionPolicyAcceptedWithGroupFit =
    "accepted_with_group_fit";             // the operator accepted the group's fits
inline constexpr const char* kSelectionPolicyExplicitOperatorSelection =
    "explicit_operator_selection";         // a fit still inside its window
inline constexpr const char* kSelectionPolicyExplicitOperatorReuse =
    "explicit_operator_reuse";             // an older fit, chosen on purpose
inline constexpr const char* kClearPolicyExplicitOperatorClear =
    "explicit_operator_clear";
inline constexpr const char* kClearPolicyFitNotAcceptedInGroup =
    "cleared_fit_not_accepted_in_group";

struct PhysicalRegistrationArtifactCandidate {
    std::string artifact_id;
    std::string camera_serial;
    std::string created_utc;
    std::filesystem::path observation_path;
    std::string observation_sha256;
    std::filesystem::path manifest_path;
    std::string manifest_sha256;
    std::string dish_fill_state;
    std::string physical_state_summary;
    std::string pixel_format;
    int width_px = 0;
    int height_px = 0;
    double accepted_center_x_px = 0.0;
    double accepted_center_y_px = 0.0;
    double accepted_radius_px = 0.0;
    double centroid_gate_outset_px = 0.0;
    bool operator_confirmed = false;
    bool compatible = false;
    std::string compatibility_reason;
    nlohmann::json observation = nlohmann::json::object();
};

struct PhysicalRegistrationSelectionResolution {
    bool pointer_exists = false;
    bool selected = false;
    bool valid = false;
    std::string status = "not_selected";
    std::string error;
    std::filesystem::path pointer_path;
    std::string pointer_sha256;
    nlohmann::json pointer = nlohmann::json::object();
    PhysicalRegistrationArtifactCandidate candidate;
    // For a selected pointer whose artifact revalidated: the freshness
    // evaluation (rule, policy, window, evaluated_at_utc, state fresh|expired).
    // An expired selection is status invalid_selected, error selection_expired.
    nlohmann::json freshness = nlohmann::json();
};

// How a selection is being made.
struct PhysicalRegistrationSelectionRequest {
    std::string selected_at_utc;
    // kSelectionPolicyAcceptedWithGroupFit, kSelectionPolicyExplicitOperatorSelection
    // or kSelectionPolicyExplicitOperatorReuse. An operator selection of a fit
    // already past its window is refused unless the policy is reuse; a reuse of
    // a fit still inside its window is refused too (it is a plain selection).
    std::string policy = kSelectionPolicyExplicitOperatorSelection;
    std::string group_transaction_id;  // required for accepted_with_group_fit
};

// Seconds since the epoch of a "YYYY-MM-DDTHH:MM:SS[.fff]Z" timestamp; false
// when it does not parse.
bool parse_utc_timestamp_seconds(const std::string& utc, long long* seconds_out);
std::string format_utc_timestamp_seconds(long long seconds);

std::filesystem::path active_physical_registration_pointer_path(
    const std::filesystem::path& calibration_base_dir,
    const std::string& camera_serial);

std::vector<PhysicalRegistrationArtifactCandidate>
discover_physical_registration_artifacts(
    const std::filesystem::path& calibration_base_dir,
    const std::string& camera_serial,
    int expected_width_px,
    int expected_height_px,
    const std::string& expected_pixel_format,
    std::vector<std::string>* warnings_out = nullptr);

PhysicalRegistrationArtifactCandidate validate_physical_registration_artifact(
    const std::filesystem::path& calibration_base_dir,
    const std::filesystem::path& observation_path,
    const std::string& camera_serial,
    int expected_width_px,
    int expected_height_px,
    const std::string& expected_pixel_format);

// Resolves and revalidates the camera's active pointer, including its
// freshness at evaluated_at_utc (a recording passes its captured_at_utc; the
// overload without it uses the current time).
PhysicalRegistrationSelectionResolution resolve_active_physical_registration(
    const std::filesystem::path& calibration_base_dir,
    const std::string& camera_serial,
    int expected_width_px,
    int expected_height_px,
    const std::string& expected_pixel_format,
    const std::string& evaluated_at_utc);
PhysicalRegistrationSelectionResolution resolve_active_physical_registration(
    const std::filesystem::path& calibration_base_dir,
    const std::string& camera_serial,
    int expected_width_px,
    int expected_height_px,
    const std::string& expected_pixel_format);

// The freshness window of a fit selected under a request: valid_from is the
// fit's created_utc, or the re-selection time for a reuse; valid_until is 24 h
// later. False (with a reason) when a time does not parse.
bool physical_registration_freshness_window(
    const std::string& fit_created_utc,
    const std::string& policy,
    const std::string& selected_at_utc,
    std::string* valid_from_utc_out,
    std::string* valid_until_utc_out,
    std::string* error_out);

bool select_physical_registration_artifact(
    const std::filesystem::path& calibration_base_dir,
    const PhysicalRegistrationArtifactCandidate& candidate,
    const PhysicalRegistrationSelectionRequest& request,
    std::string* error_out);

// Clears the camera's pointer. policy is kClearPolicyExplicitOperatorClear, or
// kClearPolicyFitNotAcceptedInGroup with the group's transaction id.
bool clear_active_physical_registration(
    const std::filesystem::path& calibration_base_dir,
    const std::string& camera_serial,
    const std::string& cleared_at_utc,
    std::string* error_out,
    const std::string& policy = kClearPolicyExplicitOperatorClear,
    const std::string& group_transaction_id = std::string());

struct PhysicalRegistrationGroupAcceptanceResult {
    std::string camera_serial;
    std::string action;  // "selected" | "cleared"
    std::string artifact_id;        // selected only
    std::string observation_sha256; // selected only
    std::string pointer_sha256;     // the pointer as written
    std::string error;              // non-empty when this camera's write failed
};

// Accepting a group's rim fits selects them (Jeremy 2026-11-15): every
// accepted candidate becomes its camera's active selection
// (accepted_with_group_fit, with the group's transaction id), and every camera
// of the group whose fit was not accepted is cleared
// (cleared_fit_not_accepted_in_group) so no older selection survives for it.
// Each camera's pointer is written atomically; one camera's failure does not
// undo the others, and every camera gets a result. Returns true when all
// writes succeeded.
bool apply_physical_registration_group_acceptance(
    const std::filesystem::path& calibration_base_dir,
    const std::string& group_transaction_id,
    const std::vector<PhysicalRegistrationArtifactCandidate>& accepted,
    const std::vector<std::string>& not_accepted_camera_serials,
    const std::string& accepted_at_utc,
    std::vector<PhysicalRegistrationGroupAcceptanceResult>* results_out,
    std::string* error_out);

}  // namespace orange::gui::spatial_layout
