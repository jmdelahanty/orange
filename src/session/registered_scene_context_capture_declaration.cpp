#include "session/registered_scene_context_capture_declaration.h"

#include <array>
#include <string_view>
#include <utility>

namespace orange::session::spatial_roi {
namespace {

using json = nlohmann::json;

constexpr std::array<std::string_view, 8> kKeys = {
    "schema_id",
    "schema_version",
    "registration_authority_status",
    "subject_presence",
    "dish_setup_complete",
    "nir_illumination_fixed",
    "camera_configuration_fixed",
    "rig_fixed",
};

bool fail(std::string* error_out, const std::string& message)
{
    if (error_out) {
        *error_out = message;
    }
    return false;
}

bool exact_keys(const json& value, std::string* error_out)
{
    if (!value.is_object()) {
        return fail(error_out,
                    "registered scene context capture declaration must be an object");
    }
    for (const auto key : kKeys) {
        if (!value.contains(std::string(key))) {
            return fail(error_out,
                        "registered scene context capture declaration." +
                            std::string(key) + " is required");
        }
    }
    if (value.size() != kKeys.size()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            bool known = false;
            for (const auto key : kKeys) {
                if (it.key() == key) {
                    known = true;
                    break;
                }
            }
            if (!known) {
                return fail(error_out,
                            "registered scene context capture declaration." +
                                it.key() + " is not allowed");
            }
        }
        return fail(error_out,
                    "registered scene context capture declaration has an unexpected key count");
    }
    return true;
}

}  // namespace

bool validate_registered_scene_context_capture_declaration(
    const RegisteredSceneContextCaptureDeclaration& declaration,
    std::string* error_out)
{
    if (error_out) {
        error_out->clear();
    }
    if (declaration.schema_version !=
        kRegisteredSceneContextCaptureDeclarationSchemaVersion) {
        return fail(error_out, "capture declaration schema_version must be 1");
    }
    if (declaration.registration_authority_status !=
            kRegistrationAcceptedForExperiment &&
        declaration.registration_authority_status !=
            kRegistrationDiagnosticNotPhysicalAcceptance) {
        return fail(
            error_out,
            "registration_authority_status must be accepted_for_experiment or diagnostic_not_physical_acceptance");
    }
    if (declaration.subject_presence != "absent" &&
        declaration.subject_presence != "present" &&
        declaration.subject_presence != "unknown") {
        return fail(error_out,
                    "subject_presence must be absent, present, or unknown");
    }
    if (!declaration.dish_setup_complete ||
        !declaration.nir_illumination_fixed ||
        !declaration.camera_configuration_fixed || !declaration.rig_fixed) {
        return fail(error_out,
                    "dish setup and NIR/camera/rig fixed-state declarations must all be true");
    }
    return true;
}

bool parse_registered_scene_context_capture_declaration(
    const nlohmann::json& value,
    RegisteredSceneContextCaptureDeclaration* declaration_out,
    std::string* error_out)
{
    if (error_out) {
        error_out->clear();
    }
    if (!declaration_out) {
        return fail(error_out, "capture declaration output is null");
    }
    if (!exact_keys(value, error_out)) {
        return false;
    }
    if (!value.at("schema_id").is_string() ||
        value.at("schema_id").get<std::string>() !=
            kRegisteredSceneContextCaptureDeclarationSchemaId) {
        return fail(error_out, "capture declaration schema_id is invalid");
    }
    if (!value.at("schema_version").is_number_integer() ||
        value.at("schema_version").get<int>() !=
            kRegisteredSceneContextCaptureDeclarationSchemaVersion) {
        return fail(error_out, "capture declaration schema_version must be 1");
    }
    for (const char* key : {"registration_authority_status",
                            "subject_presence"}) {
        if (!value.at(key).is_string()) {
            return fail(error_out,
                        std::string("capture declaration ") + key +
                            " must be a string");
        }
    }
    for (const char* key : {"dish_setup_complete",
                            "nir_illumination_fixed",
                            "camera_configuration_fixed",
                            "rig_fixed"}) {
        if (!value.at(key).is_boolean()) {
            return fail(error_out,
                        std::string("capture declaration ") + key +
                            " must be a boolean");
        }
    }

    RegisteredSceneContextCaptureDeclaration parsed;
    parsed.registration_authority_status =
        value.at("registration_authority_status").get<std::string>();
    parsed.subject_presence = value.at("subject_presence").get<std::string>();
    parsed.dish_setup_complete = value.at("dish_setup_complete").get<bool>();
    parsed.nir_illumination_fixed =
        value.at("nir_illumination_fixed").get<bool>();
    parsed.camera_configuration_fixed =
        value.at("camera_configuration_fixed").get<bool>();
    parsed.rig_fixed = value.at("rig_fixed").get<bool>();
    if (!validate_registered_scene_context_capture_declaration(
            parsed, error_out)) {
        return false;
    }
    *declaration_out = std::move(parsed);
    return true;
}

nlohmann::json registered_scene_context_capture_declaration_to_json(
    const RegisteredSceneContextCaptureDeclaration& declaration)
{
    return {
        {"schema_id", kRegisteredSceneContextCaptureDeclarationSchemaId},
        {"schema_version", declaration.schema_version},
        {"registration_authority_status",
         declaration.registration_authority_status},
        {"subject_presence", declaration.subject_presence},
        {"dish_setup_complete", declaration.dish_setup_complete},
        {"nir_illumination_fixed", declaration.nir_illumination_fixed},
        {"camera_configuration_fixed",
         declaration.camera_configuration_fixed},
        {"rig_fixed", declaration.rig_fixed},
    };
}

bool registered_scene_context_daily_registration_accepted(
    const RegisteredSceneContextCaptureDeclaration& declaration) noexcept
{
    return declaration.registration_authority_status ==
        kRegistrationAcceptedForExperiment;
}

}  // namespace orange::session::spatial_roi
