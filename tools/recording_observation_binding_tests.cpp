#include "session/recording_observation_binding.h"
#include "session/recording_observation_identity.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using json = nlohmann::json;

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string digest(char value)
{
    return "sha256:" + std::string(64, value);
}

json make_identity()
{
    orange::session::RecordingObservationEdgeIdentity identity;
    identity.recording_id = "2026_08_13_12_00_00";
    identity.camera_id = "2010095";
    identity.source_camera_stream_id = "2010095";
    identity.rig_id = "omnifin0";
    identity.canvas_name = "shadow";
    identity.arena_id = "arena_3";

    json record;
    std::string error;
    require(orange::session::build_recording_observation_identity(
                identity, &record, &error),
            "identity build failed: " + error);
    return record;
}

json target()
{
    return {
        {"rig_id", "omnifin0"},
        {"canvas_name", "shadow"},
        {"arena_id", "arena_3"},
        {"camera_id", "2010095"},
        {"source_camera_stream_id", "2010095"},
    };
}

json request_contract(const json& identity, bool geometry_available = true)
{
    return {
        {"schema_id",
         orange::session::kObservationBindingRequestSchemaId},
        {"schema_version", orange::session::kObservationBindingSchemaVersion},
        {"observation_context_id", identity.at("observation_context_id")},
        {"observation_identity_sha256", identity.at("identity_sha256")},
        {"observation_identity", identity},
        {"binding_mode", "required"},
        {"requested_at_utc", "2026-08-13T16:00:00Z"},
        {"recording",
         {
             {"recording_id", "2026_08_13_12_00_00"},
             {"recording_folder",
              "/home/jeremy/orange_data/exp/unsorted/2026_08_13_12_00_00"},
             {"recording_snapshot",
              {
                  {"role", "immutable_recording_start_snapshot"},
                  {"relative_path", "recording_snapshot_start.json"},
                  {"sha256", digest('a')},
              }},
         }},
        {"target", target()},
        {"recording_geometry_contract",
         geometry_available
             ? json{{"status", "available"},
                    {"relative_path", "calibration/recording_geometry.json"},
                    {"sha256", digest('b')}}
             : json{{"status", "not_available"},
                    {"reason", "not_configured"}}},
    };
}

json accepted_contract(const json& request)
{
    return {
        {"schema_id",
         orange::session::kObservationBindingAcceptanceSchemaId},
        {"schema_version", orange::session::kObservationBindingSchemaVersion},
        {"status", "accepted"},
        {"request_id", request.at("request_id")},
        {"request_contract_sha256", request.at("contract_sha256")},
        {"observation_context_id",
         request.at("contract").at("observation_context_id")},
        {"decided_at_utc", "2026-08-13T16:00:01Z"},
        {"citrus_experiment_id", "citexp_20260813_160001"},
        {"citrus_session_uuid", "session_arena_3_20260813"},
        {"target", target()},
        {"planned_h5_relative_path",
         "citrus/session_arena_3_20260813_protocol.h5"},
    };
}

json rejected_contract(const json& request)
{
    return {
        {"schema_id",
         orange::session::kObservationBindingAcceptanceSchemaId},
        {"schema_version", orange::session::kObservationBindingSchemaVersion},
        {"status", "rejected"},
        {"request_id", request.at("request_id")},
        {"request_contract_sha256", request.at("contract_sha256")},
        {"observation_context_id",
         request.at("contract").at("observation_context_id")},
        {"decided_at_utc", "2026-08-13T16:00:01Z"},
        {"reason", "target_mismatch"},
    };
}

json receipt_contract(const json& request, const json& acceptance)
{
    return {
        {"schema_id",
         orange::session::kObservationBindingFinalizedReceiptSchemaId},
        {"schema_version", orange::session::kObservationBindingSchemaVersion},
        {"request_id", request.at("request_id")},
        {"request_contract_sha256", request.at("contract_sha256")},
        {"acceptance_id", acceptance.at("acceptance_id")},
        {"acceptance_contract_sha256", acceptance.at("contract_sha256")},
        {"observation_context_id",
         request.at("contract").at("observation_context_id")},
        {"finalized_at_utc", "2026-08-13T16:20:00Z"},
        {"citrus_experiment_id", "citexp_20260813_160001"},
        {"citrus_session_uuid", "session_arena_3_20260813"},
        {"target", target()},
        {"h5_artifact",
         {
             {"relative_path",
              "citrus/session_arena_3_20260813_protocol.h5"},
             {"size_bytes", 1234567},
             {"sha256", digest('c')},
         }},
        {"session_status", "COMPLETE"},
        {"runtime_geometry_contract_sha256", digest('d')},
        {"protocol_semantic",
         {{"status", "available"}, {"semantic_sha256", digest('e')}}},
    };
}

void test_complete_chain()
{
    const json identity = make_identity();
    json request;
    json acceptance;
    json receipt;
    std::string error;
    require(orange::session::seal_recording_observation_binding_request(
                request_contract(identity), &request, &error),
            "request seal failed: " + error);
    require(orange::session::seal_recording_observation_binding_acceptance(
                accepted_contract(request), &acceptance, &error),
            "acceptance seal failed: " + error);
    require(orange::session::seal_recording_observation_finalized_receipt(
                receipt_contract(request, acceptance), &receipt, &error),
            "receipt seal failed: " + error);
    require(orange::session::validate_recording_observation_finalized_receipt(
                receipt, request, acceptance, &error),
            "complete chain validation failed: " + error);
    require(request.at("contract").at("observation_context_id") ==
                receipt.at("contract").at("observation_context_id"),
            "binding lifecycle renamed the stable observation context");
}

void test_geometry_is_independent_from_binding()
{
    json request;
    std::string error;
    require(orange::session::seal_recording_observation_binding_request(
                request_contract(make_identity(), false), &request, &error),
            "binding should allow an explicit unavailable geometry product: " +
                error);
}

void test_target_mismatch_fails()
{
    json contract = request_contract(make_identity());
    contract["target"]["arena_id"] = "arena_4";
    json request;
    std::string error;
    require(!orange::session::seal_recording_observation_binding_request(
                contract, &request, &error),
            "request accepted a target that disagrees with its identity");
}

void test_unsafe_path_fails()
{
    json contract = request_contract(make_identity());
    contract["recording"]["recording_snapshot"]["relative_path"] =
        "../recording_snapshot.json";
    json request;
    std::string error;
    require(!orange::session::seal_recording_observation_binding_request(
                contract, &request, &error),
            "request accepted an escaping recording-relative path");
}

void test_malformed_nested_record_fails_without_throwing()
{
    json contract = request_contract(make_identity());
    contract["recording_geometry_contract"] = nullptr;
    json request;
    std::string error;
    require(!orange::session::seal_recording_observation_binding_request(
                contract, &request, &error),
            "request accepted a null geometry reference");
}

void test_acceptance_must_reference_exact_request()
{
    json request;
    json acceptance;
    std::string error;
    require(orange::session::seal_recording_observation_binding_request(
                request_contract(make_identity()), &request, &error),
            "request seal failed: " + error);
    json contract = accepted_contract(request);
    contract["request_contract_sha256"] = digest('f');
    require(orange::session::seal_recording_observation_binding_acceptance(
                contract, &acceptance, &error),
            "independently valid acceptance failed to seal: " + error);
    require(!orange::session::validate_recording_observation_binding_acceptance(
                acceptance, request, &error),
            "acceptance referenced a different request digest");
}

void test_rejection_is_terminal()
{
    json request;
    json rejection;
    json receipt;
    std::string error;
    require(orange::session::seal_recording_observation_binding_request(
                request_contract(make_identity()), &request, &error),
            "request seal failed: " + error);
    require(orange::session::seal_recording_observation_binding_acceptance(
                rejected_contract(request), &rejection, &error),
            "rejection seal failed: " + error);
    require(orange::session::validate_recording_observation_binding_acceptance(
                rejection, request, &error),
            "rejection chain should validate: " + error);

    json accepted;
    require(orange::session::seal_recording_observation_binding_acceptance(
                accepted_contract(request), &accepted, &error),
            "accepted fixture seal failed: " + error);
    require(orange::session::seal_recording_observation_finalized_receipt(
                receipt_contract(request, accepted), &receipt, &error),
            "receipt fixture seal failed: " + error);
    require(!orange::session::validate_recording_observation_finalized_receipt(
                receipt, request, rejection, &error),
            "a rejected binding incorrectly accepted a finalized receipt");
}

void test_final_receipt_path_must_match_acceptance()
{
    json request;
    json acceptance;
    json receipt;
    std::string error;
    require(orange::session::seal_recording_observation_binding_request(
                request_contract(make_identity()), &request, &error),
            "request seal failed: " + error);
    require(orange::session::seal_recording_observation_binding_acceptance(
                accepted_contract(request), &acceptance, &error),
            "acceptance seal failed: " + error);
    json contract = receipt_contract(request, acceptance);
    contract["h5_artifact"]["relative_path"] = "citrus/other.h5";
    require(orange::session::seal_recording_observation_finalized_receipt(
                contract, &receipt, &error),
            "independently valid receipt failed to seal: " + error);
    require(!orange::session::validate_recording_observation_finalized_receipt(
                receipt, request, acceptance, &error),
            "final receipt path disagreed with accepted planned path");
}

}  // namespace

int main()
{
    try {
        test_complete_chain();
        test_geometry_is_independent_from_binding();
        test_target_mismatch_fails();
        test_unsafe_path_fails();
        test_malformed_nested_record_fails_without_throwing();
        test_acceptance_must_reference_exact_request();
        test_rejection_is_terminal();
        test_final_receipt_path_must_match_acceptance();
        std::cout << "recording_observation_binding_tests: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "recording_observation_binding_tests: FAIL: "
                  << error.what() << '\n';
        return 1;
    }
}
