#pragma once

#include "json.hpp"

#include <string>

namespace orange::session::spatial_roi {

inline constexpr const char* kRegisteredSceneContextCaptureDeclarationSchemaId =
    "orange.recording.registered_scene_context.capture_declaration";
inline constexpr int kRegisteredSceneContextCaptureDeclarationSchemaVersion = 1;

inline constexpr const char* kRegistrationAcceptedForExperiment =
    "accepted_for_experiment";
inline constexpr const char* kRegistrationDiagnosticNotPhysicalAcceptance =
    "diagnostic_not_physical_acceptance";

// Explicit operator/scenario declaration for facts that cannot be inferred
// from a geometry ID or digest alone. There is deliberately no implicit
// accepted default.
struct RegisteredSceneContextCaptureDeclaration final {
    int schema_version =
        kRegisteredSceneContextCaptureDeclarationSchemaVersion;
    std::string registration_authority_status;
    std::string subject_presence = "unknown";
    bool dish_setup_complete = false;
    bool nir_illumination_fixed = false;
    bool camera_configuration_fixed = false;
    bool rig_fixed = false;
};

bool validate_registered_scene_context_capture_declaration(
    const RegisteredSceneContextCaptureDeclaration& declaration,
    std::string* error_out = nullptr);

bool parse_registered_scene_context_capture_declaration(
    const nlohmann::json& value,
    RegisteredSceneContextCaptureDeclaration* declaration_out,
    std::string* error_out = nullptr);

nlohmann::json registered_scene_context_capture_declaration_to_json(
    const RegisteredSceneContextCaptureDeclaration& declaration);

bool registered_scene_context_daily_registration_accepted(
    const RegisteredSceneContextCaptureDeclaration& declaration) noexcept;

}  // namespace orange::session::spatial_roi
