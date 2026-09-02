#include "session/registered_scene_context_capture_declaration.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

orange::session::spatial_roi::RegisteredSceneContextCaptureDeclaration
accepted_declaration()
{
    return {
        1,
        orange::session::spatial_roi::kRegistrationAcceptedForExperiment,
        "unknown",
        true,
        true,
        true,
        true,
    };
}

void test_closed_round_trip_and_acceptance_mapping()
{
    using namespace orange::session::spatial_roi;
    const auto accepted = accepted_declaration();
    const nlohmann::json value =
        registered_scene_context_capture_declaration_to_json(accepted);
    RegisteredSceneContextCaptureDeclaration parsed;
    std::string error;
    require(parse_registered_scene_context_capture_declaration(
                value, &parsed, &error),
            "accepted declaration should parse: " + error);
    require(registered_scene_context_daily_registration_accepted(parsed),
            "accepted authority must map to daily-registration accepted");

    auto diagnostic = value;
    diagnostic["registration_authority_status"] =
        kRegistrationDiagnosticNotPhysicalAcceptance;
    require(parse_registered_scene_context_capture_declaration(
                diagnostic, &parsed, &error),
            "diagnostic declaration should parse: " + error);
    require(!registered_scene_context_daily_registration_accepted(parsed),
            "diagnostic authority must not claim physical acceptance");

    diagnostic["unknown"] = true;
    require(!parse_registered_scene_context_capture_declaration(
                diagnostic, &parsed, &error),
            "unknown declaration keys must fail closed");
}

void test_invalid_claims_rejected()
{
    using namespace orange::session::spatial_roi;
    std::string error;
    auto declaration = accepted_declaration();
    declaration.dish_setup_complete = false;
    require(!validate_registered_scene_context_capture_declaration(
                declaration, &error),
            "incomplete dish setup must be rejected");
    declaration = accepted_declaration();
    declaration.subject_presence = "not_declared";
    require(!validate_registered_scene_context_capture_declaration(
                declaration, &error),
            "open subject-presence text must be rejected");
    declaration = accepted_declaration();
    declaration.registration_authority_status = "assumed";
    require(!validate_registered_scene_context_capture_declaration(
                declaration, &error),
            "assumed registration acceptance must be rejected");
}

}  // namespace

int main()
{
    try {
        test_closed_round_trip_and_acceptance_mapping();
        test_invalid_claims_rejected();
        std::cout
            << "All registered scene context capture declaration tests passed.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        std::cerr << "[FAIL] " << exception.what() << "\n";
        return EXIT_FAILURE;
    }
}
