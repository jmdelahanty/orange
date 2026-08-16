#include "session/recording_observation_binding.h"
#include "session/recording_observation_finalization.h"
#include "session/recording_observation_identity.h"
#include "session/recording_observation_prearm.h"
#include "session/recording_observation_request_artifacts.h"
#include "gui/spatial_layout/sha256.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <unistd.h>

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

void write_file(const std::filesystem::path& path, const std::string& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "could not write " + path.string());
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string read_file(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "could not read " + path.string());
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

std::string file_sha256(const std::filesystem::path& path)
{
    std::string value;
    std::string error;
    require(orange::gui::spatial_layout::checksum::file_sha256(
                path, &value, &error),
            "could not checksum " + path.string() + ": " + error);
    return value;
}

json resolved_geometry()
{
    return {
        {"schema_id", "orange.recording.geometry_contract"},
        {"schema_version", 1},
        {"status", "resolved"},
        {"selection", {
            {"configured", true},
            {"rig_id", "omnifin0"},
            {"selected_canvas_name", "shadow"},
        }},
        {"cameras", {
            {"2010095", {
                {"camera_serial", "2010095"},
                {"arena_id", "arena_3"},
                {"status", "resolved"},
                {"selected_canvas", {{"canvas_name", "shadow"}}},
            }},
            {"2010096", {
                {"camera_serial", "2010096"},
                {"arena_id", "arena_4"},
                {"status", "resolved"},
                {"selected_canvas", {{"canvas_name", "shadow"}}},
            }},
        }},
    };
}

std::filesystem::path make_materialization_fixture(const json& geometry)
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orange_observation_request_artifacts_" +
         std::to_string(static_cast<long long>(getpid())));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const std::filesystem::path geometry_path =
        root / "recording_geometry_contract.json";
    write_file(geometry_path, geometry.dump(2) + "\n");
    const json start = {
        {"schema_version", 2},
        {"recording_id", "2026_08_13_12_00_00"},
        {"camera_runtime", {
            {"2010095", {{"width", 4512}, {"height", 4512}}},
            {"2010096", {{"width", 4512}, {"height", 4512}}},
        }},
        {"source_camera_streams", {
            {"2010095", {
                {"schema_id", "orange.recording.source_camera_stream"},
                {"schema_version", 1},
                {"camera_id", "2010095"},
                {"source_camera_stream_id", "2010095"},
                {"source_camera_stream_identity_policy",
                 orange::session::
                     kCameraSerialSourceFrameStreamIdentityPolicy},
                {"role", "canonical_acquisition_source"},
            }},
            {"2010096", {
                {"schema_id", "orange.recording.source_camera_stream"},
                {"schema_version", 1},
                {"camera_id", "2010096"},
                {"source_camera_stream_id", "2010096"},
                {"source_camera_stream_identity_policy",
                 orange::session::
                     kCameraSerialSourceFrameStreamIdentityPolicy},
                {"role", "canonical_acquisition_source"},
            }},
        }},
        {"recording_geometry_contract", {
            {"relative_path", "recording_geometry_contract.json"},
            {"sha256", file_sha256(geometry_path)},
        }},
    };
    const std::filesystem::path start_path =
        root / "recording_snapshot_start.json";
    write_file(start_path, start.dump(2) + "\n");
    std::error_code error;
    std::filesystem::permissions(
        start_path,
        std::filesystem::perms::owner_read |
            std::filesystem::perms::group_read |
            std::filesystem::perms::others_read,
        std::filesystem::perm_options::replace,
        error);
    require(!error, "could not make start fixture read-only");
    return root;
}

void test_request_artifact_materialization_and_tamper_gates()
{
    const std::filesystem::path root =
        make_materialization_fixture(resolved_geometry());
    orange::session::RecordingObservationBindingRequestMaterialization result;
    std::string error;
    require(orange::session::materialize_recording_observation_binding_requests(
                root.string(), "optional", "2026-08-13T16:00:00Z",
                &result, &error),
            "request artifact materialization failed: " + error);
    require(result.status == "materialized" && result.artifacts.size() == 2,
            "resolved two-camera geometry must produce two edge requests");
    require(result.collection_relative_path ==
                "recording_observation_bindings/request_collection.json" &&
                !result.collection_sha256.empty(),
            "request materialization must produce a digest-bound collection");

    const json collection = json::parse(read_file(
        root / result.collection_relative_path));
    require(collection.at("request_count") == 2 &&
                collection.at("requests").size() == 2,
            "collection must inventory every request exactly once");
    for (const auto& artifact : result.artifacts) {
        const std::filesystem::path request_path = root / artifact.relative_path;
        require(std::filesystem::exists(request_path),
                "materialized request path must exist");
        require(file_sha256(request_path) == artifact.sha256,
                "request artifact checksum must cover exact file bytes");
        require(orange::session::validate_recording_observation_binding_request(
                    artifact.request, &error),
                "materialized request must satisfy the frozen schema: " + error);
        require(artifact.request.at("contract").at("recording")
                    .at("recording_snapshot").at("sha256") ==
                    file_sha256(root / "recording_snapshot_start.json"),
                "request must bind the exact immutable start snapshot");
    }

    orange::session::RecordingObservationBindingRequestMaterialization repeated;
    require(orange::session::materialize_recording_observation_binding_requests(
                root.string(), "optional", "2099-01-01T00:00:00Z",
                &repeated, &error),
            "idempotent request verification should pass: " + error);
    require(repeated.collection_sha256 == result.collection_sha256,
            "idempotent retry must preserve the original request time and bytes");

    const std::filesystem::path first_request =
        root / result.artifacts.front().relative_path;
    std::error_code permission_error;
    std::filesystem::permissions(
        first_request,
        std::filesystem::perms::owner_write,
        std::filesystem::perm_options::add,
        permission_error);
    require(!permission_error, "could not make request artifact writable for test");
    require(!orange::session::materialize_recording_observation_binding_requests(
                root.string(), "optional", "2026-08-13T16:00:00Z",
                &repeated, &error),
            "writable request artifact must fail closed");
    require(error.find("writable") != std::string::npos,
            "writable request artifact should report the immutability failure");
    std::filesystem::remove_all(root);
}

void test_request_artifact_unavailable_and_contradictory_geometry()
{
    json unconfigured = resolved_geometry();
    unconfigured["status"] = "not_configured";
    unconfigured["selection"] = {{"configured", false}};
    auto root = make_materialization_fixture(unconfigured);
    orange::session::RecordingObservationBindingRequestMaterialization result;
    std::string error;
    require(orange::session::materialize_recording_observation_binding_requests(
                root.string(), "optional", "2026-08-13T16:00:00Z",
                &result, &error),
            "Orange-only optional materialization should remain available: " + error);
    require(result.status == "unavailable" && result.artifacts.empty() &&
                result.reason == "citrus_canvas_not_selected",
            "unconfigured Citrus geometry must not fabricate an edge request");
    std::filesystem::remove_all(root);

    json contradictory = resolved_geometry();
    contradictory["cameras"]["2010095"]["camera_serial"] = "2010096";
    root = make_materialization_fixture(contradictory);
    require(!orange::session::materialize_recording_observation_binding_requests(
                root.string(), "optional", "2026-08-13T16:00:00Z",
                &result, &error),
            "contradictory recording-bound camera identity must fail closed");
    require(error.find("mismatched camera") != std::string::npos,
            "contradictory camera identity should produce a useful failure");
    std::filesystem::remove_all(root);
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

json accepted_batch_response(const json& local_request)
{
    json acceptances = json::array();
    for (const auto& request :
         local_request.at("params").at("binding_requests")) {
        const std::string context =
            request.at("contract").at("observation_context_id");
        json contract = {
            {"schema_id",
             orange::session::kObservationBindingAcceptanceSchemaId},
            {"schema_version",
             orange::session::kObservationBindingSchemaVersion},
            {"status", "accepted"},
            {"request_id", request.at("request_id")},
            {"request_contract_sha256", request.at("contract_sha256")},
            {"observation_context_id", context},
            {"decided_at_utc", "2026-08-13T16:00:01Z"},
            {"citrus_experiment_id", "citexp_transaction_test"},
            {"citrus_session_uuid", "citsess_" + context.substr(7)},
            {"target", request.at("contract").at("target")},
            {"planned_h5_relative_path",
             "citrus/citsess_" + context.substr(7) + "_protocol.h5"},
        };
        json acceptance;
        std::string error;
        require(orange::session::seal_recording_observation_binding_acceptance(
                    contract, &acceptance, &error),
                "test acceptance seal failed: " + error);
        acceptances.push_back(std::move(acceptance));
    }
    return {
        {"ok", true},
        {"accepted", true},
        {"effect", {{"recording_observation_binding", {
            {"schema_id", "citrus.recording_observation_binding_batch_result"},
            {"schema_version", 1},
            {"status", "accepted"},
            {"citrus_experiment_id", "citexp_transaction_test"},
            {"acceptance_count", acceptances.size()},
            {"acceptances", std::move(acceptances)},
        }}}},
    };
}

void test_prearm_required_acceptance_is_persisted_and_idempotent()
{
    const auto root = make_materialization_fixture(resolved_geometry());
    orange::session::RecordingObservationBindingRequestMaterialization requests;
    orange::session::RecordingObservationPreArmResult result;
    std::string error;
    int transport_calls = 0;
    const auto transport = [&transport_calls](
        const json& request, json* response, std::string*) {
        ++transport_calls;
        *response = accepted_batch_response(request);
        return true;
    };
    require(orange::session::prepare_recording_observation_pre_arm(
                root.string(), "required", "2026-08-13T16:00:00Z",
                &requests, &result, &error, transport),
            "required pre-arm acceptance failed: " + error);
    require(result.arm_allowed &&
                result.lifecycle_status == "accepted_pending_finalization" &&
                result.acceptances.size() == 2 && transport_calls == 1,
            "required accepted batch did not produce a complete arm decision");
    require(!result.decision_sha256.empty() &&
                std::filesystem::exists(root / result.decision_relative_path),
            "accepted pre-arm decision was not persisted");
    for (const auto& acceptance : result.acceptances) {
        require(std::filesystem::exists(root / acceptance.relative_path) &&
                    file_sha256(root / acceptance.relative_path) ==
                        acceptance.sha256,
                "exact Citrus acceptance was not persisted immutably");
    }

    orange::session::RecordingObservationPreArmResult repeated;
    require(orange::session::execute_recording_observation_pre_arm(
                root.string(), requests, "required",
                "2099-01-01T00:00:00Z", &repeated, &error, transport),
            "idempotent pre-arm decision reload failed: " + error);
    require(transport_calls == 1 &&
                repeated.decision_sha256 == result.decision_sha256 &&
                repeated.arm_allowed,
            "idempotent pre-arm retry resent or changed the accepted decision");

    const std::filesystem::path first_acceptance =
        root / result.acceptances.front().relative_path;
    std::error_code permission_error;
    std::filesystem::permissions(
        first_acceptance,
        std::filesystem::perms::owner_write,
        std::filesystem::perm_options::add,
        permission_error);
    require(!permission_error,
            "could not make acceptance artifact writable for tamper test");
    require(!orange::session::execute_recording_observation_pre_arm(
                root.string(), requests, "required",
                "2099-01-01T00:00:00Z", &repeated, &error, transport),
            "replayed pre-arm decision accepted a writable acceptance artifact");
    require(transport_calls == 1,
            "invalid replayed evidence should fail before contacting Citrus");
    std::filesystem::remove_all(root);
}

void test_prearm_rejects_non_atomic_or_inconsistent_batch()
{
    const auto root = make_materialization_fixture(resolved_geometry());
    orange::session::RecordingObservationBindingRequestMaterialization requests;
    orange::session::RecordingObservationPreArmResult result;
    std::string error;
    const auto inconsistent = [](const json& request,
                                 json* response,
                                 std::string*) {
        *response = accepted_batch_response(request);
        (*response)["effect"]["recording_observation_binding"]
                   ["acceptances"][1]["contract"]["citrus_experiment_id"] =
            "citexp_wrong";
        json& acceptance = (*response)["effect"]
            ["recording_observation_binding"]["acceptances"][1];
        const json inconsistent_contract = acceptance.at("contract");
        json resealed;
        std::string seal_error;
        require(orange::session::seal_recording_observation_binding_acceptance(
                    inconsistent_contract, &resealed, &seal_error),
                "could not reseal inconsistent acceptance fixture: " +
                    seal_error);
        acceptance = std::move(resealed);
        return true;
    };
    require(!orange::session::prepare_recording_observation_pre_arm(
                root.string(), "required", "2026-08-13T16:00:00Z",
                &requests, &result, &error, inconsistent),
            "inconsistent Citrus experiment IDs should fail closed");
    require(!std::filesystem::exists(
                root / "recording_observation_bindings/acceptances"),
            "invalid batch must not publish a partial acceptance set");
    std::filesystem::remove_all(root);
}

void test_prearm_required_and_optional_transport_failure_policy()
{
    const auto unavailable = [](const json&, json*, std::string* error) {
        if (error) *error = "fixture_transport_unavailable";
        return false;
    };
    for (const std::string mode : {"required", "optional"}) {
        const auto root = make_materialization_fixture(resolved_geometry());
        orange::session::RecordingObservationBindingRequestMaterialization requests;
        orange::session::RecordingObservationPreArmResult result;
        std::string error;
        require(orange::session::prepare_recording_observation_pre_arm(
                    root.string(), mode, "2026-08-13T16:00:00Z",
                    &requests, &result, &error, unavailable),
                mode + " unavailable transport should produce an explicit decision: " +
                    error);
        require(result.lifecycle_status == "unbound" &&
                    result.reason == "handshake_not_completed" &&
                    result.transport_attempted &&
                    result.arm_allowed == (mode == "optional"),
                mode + " transport failure policy is incorrect");
        std::filesystem::remove_all(root);
    }
}

void test_prearm_not_applicable_never_contacts_citrus()
{
    const auto root = make_materialization_fixture(resolved_geometry());
    orange::session::RecordingObservationBindingRequestMaterialization requests;
    orange::session::RecordingObservationPreArmResult result;
    std::string error;
    int transport_calls = 0;
    require(orange::session::prepare_recording_observation_pre_arm(
                root.string(), "not_applicable", "2026-08-13T16:00:00Z",
                &requests, &result, &error,
                [&transport_calls](const json&, json*, std::string*) {
                    ++transport_calls;
                    return false;
                }),
            "not-applicable pre-arm decision failed: " + error);
    require(result.arm_allowed && !result.transport_attempted &&
                result.lifecycle_status == "not_applicable" &&
                transport_calls == 0 && requests.artifacts.empty(),
            "not-applicable mode contacted Citrus or blocked recording");
    const json decision = json::parse(read_file(
        root / result.decision_relative_path));
    require(decision.at("recording_id") == "2026_08_13_12_00_00",
            "not-applicable evidence must retain the recording identity");
    std::filesystem::remove_all(root);
}

void test_post_close_finalization_is_complete_idempotent_and_manifest_bound()
{
    const auto root = make_materialization_fixture(resolved_geometry());
    orange::session::RecordingObservationBindingRequestMaterialization requests;
    orange::session::RecordingObservationPreArmResult pre_arm;
    std::string error;
    require(orange::session::prepare_recording_observation_pre_arm(
                root.string(), "required", "2026-08-13T16:00:00Z",
                &requests, &pre_arm, &error,
                [](const json& request, json* response, std::string*) {
                    *response = accepted_batch_response(request);
                    return true;
                }),
            "post-close fixture pre-arm failed: " + error);

    json receipts = json::array();
    for (std::size_t index = 0; index < requests.artifacts.size(); ++index) {
        const json& request = requests.artifacts[index].request;
        const json& acceptance = pre_arm.acceptances[index].acceptance;
        const std::string h5_relative =
            acceptance.at("contract").at("planned_h5_relative_path");
        const std::string h5_bytes =
            "closed-citrus-h5-fixture-" + std::to_string(index);
        write_file(root / h5_relative, h5_bytes);

        json contract = receipt_contract(request, acceptance);
        contract["citrus_experiment_id"] = "citexp_transaction_test";
        contract["citrus_session_uuid"] =
            acceptance.at("contract").at("citrus_session_uuid");
        contract["target"] = acceptance.at("contract").at("target");
        contract["h5_artifact"] = {
            {"relative_path", h5_relative},
            {"size_bytes", h5_bytes.size()},
            {"sha256", file_sha256(root / h5_relative)},
        };
        json receipt;
        require(orange::session::seal_recording_observation_finalized_receipt(
                    contract, &receipt, &error),
                "post-close receipt seal failed: " + error);
        receipts.push_back(std::move(receipt));
    }

    const json params = {
        {"experiment_id", "citexp_transaction_test"},
        {"receipts", receipts},
    };
    const auto conflicting_receipt_path =
        root / "recording_observation_bindings/receipts" /
        (receipts.front().at("contract")
             .at("observation_context_id").get<std::string>() + ".json");
    write_file(conflicting_receipt_path, "partial-write-fixture");
    const auto partial_write =
        orange::session::finalize_recording_observation_bindings(
            root.string(), params);
    require(!partial_write.ok,
            "conflicting partial receipt artifact was overwritten");
    std::filesystem::remove(conflicting_receipt_path);
    const auto incomplete =
        orange::session::finalize_recording_observation_bindings(
            root.string(),
            {{"experiment_id", "citexp_transaction_test"},
             {"receipts", json::array({receipts.front()})}});
    require(!incomplete.ok,
            "partial multi-H5 receipt set was accepted as bound");
    const auto finalized =
        orange::session::finalize_recording_observation_bindings(
            root.string(), params);
    require(finalized.ok &&
                finalized.collection.at("binding_status") == "bound" &&
                finalized.collection.at("context_count") == 2,
            "complete post-close receipt set did not bind: " +
                finalized.error);
    const auto collection_path = root /
        orange::session::kObservationBindingFinalizationRelativePath;
    require(std::filesystem::exists(collection_path) &&
                file_sha256(collection_path) ==
                    finalized.collection_reference.at("sha256"),
            "finalized collection reference does not bind exact bytes");
    const std::string first_collection_bytes = read_file(collection_path);

    require(orange::session::refresh_recording_session_observation_bindings(
                root.string(), &error),
            "receipt ACK should not require recording_session.json to exist: " +
                error);

    const auto repeated =
        orange::session::finalize_recording_observation_bindings(
            root.string(), params);
    require(repeated.ok &&
                read_file(collection_path) == first_collection_bytes &&
                repeated.collection_reference == finalized.collection_reference,
            "byte-identical finalization retry changed immutable evidence");

    json manifest = {
        {"schema_id", "orange.recording_session"},
        {"mode", "rolling_clips"},
        {"clips", json::array({{{"clip_id", "clip_000000"}}})},
    };
    require(orange::session::apply_recording_observation_finalization_to_manifest(
                root.string(), &manifest, &error),
            "could not add finalized collection to manifest: " + error);
    require(manifest.at("recording_observation_bindings")
                    .at("binding_status") == "bound" &&
                manifest.at("observation_contexts").size() == 2,
            "recording manifest did not expose the bound observation contexts");
    require(
        manifest.at("clips").at(0).at("observation_contexts")
                .at("authority") == "parent_recording_session" &&
            manifest.at("clips").at(0).at("observation_contexts")
                .at("contexts").size() == 2,
        "rolling clip did not reference the parent observation contexts");

    write_file(root / "recording_session.json",
               json{{"schema_id", "orange.recording_session"}}.dump(2) +
                   "\n");
    require(orange::session::refresh_recording_session_observation_bindings(
                root.string(), &error),
            "could not refresh already-finalized recording manifest: " +
                error);
    const json refreshed_manifest = json::parse(
        read_file(root / "recording_session.json"));
    require(refreshed_manifest.at("recording_observation_bindings")
                    .at("binding_status") == "bound" &&
                refreshed_manifest.at("observation_contexts").size() == 2,
            "post-finalization manifest refresh omitted bound contexts");

    const std::string first_h5 =
        receipts.front().at("contract").at("h5_artifact")
            .at("relative_path");
    write_file(root / first_h5, "tampered-after-close");
    const auto tampered =
        orange::session::finalize_recording_observation_bindings(
            root.string(), params);
    require(!tampered.ok,
            "finalization retry accepted H5 bytes that no longer match receipt");
    std::filesystem::remove_all(root);
}

void test_manifest_never_infers_bound_without_final_receipts()
{
    const auto root = make_materialization_fixture(resolved_geometry());
    orange::session::RecordingObservationBindingRequestMaterialization requests;
    orange::session::RecordingObservationPreArmResult pre_arm;
    std::string error;
    require(orange::session::prepare_recording_observation_pre_arm(
                root.string(), "required", "2026-08-13T16:00:00Z",
                &requests, &pre_arm, &error,
                [](const json& request, json* response, std::string*) {
                    *response = accepted_batch_response(request);
                    return true;
                }),
            "unbound manifest fixture pre-arm failed: " + error);
    json manifest = {{"schema_id", "orange.recording_session"}};
    require(orange::session::apply_recording_observation_finalization_to_manifest(
                root.string(), &manifest, &error),
            "unbound manifest materialization failed: " + error);
    require(manifest.at("recording_observation_bindings").at("status") ==
                "unbound" &&
                manifest.at("observation_contexts").size() == 2,
            "missing final receipts were incorrectly inferred as bound");
    for (const auto& context : manifest.at("observation_contexts")) {
        require(context.at("status") == "unbound",
                "unfinalized context was upgraded to bound");
    }
    std::filesystem::remove_all(root);
}

void test_streaming_sha256_matches_known_vector()
{
    const auto root = make_materialization_fixture(resolved_geometry());
    const auto path = root / "sha256_known_vector.bin";
    write_file(path, "abc");
    std::string digest;
    std::string error;
    require(
        orange::gui::spatial_layout::checksum::file_sha256(
            path, &digest, &error),
        "streaming SHA-256 could not hash the known vector: " + error);
    require(
        digest ==
            "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9c"
            "b410ff61f20015ad",
        "streaming SHA-256 did not match the NIST abc vector");
    std::filesystem::remove_all(root);
}

}  // namespace

int main()
{
    try {
        test_complete_chain();
        test_request_artifact_materialization_and_tamper_gates();
        test_request_artifact_unavailable_and_contradictory_geometry();
        test_geometry_is_independent_from_binding();
        test_target_mismatch_fails();
        test_unsafe_path_fails();
        test_malformed_nested_record_fails_without_throwing();
        test_acceptance_must_reference_exact_request();
        test_rejection_is_terminal();
        test_final_receipt_path_must_match_acceptance();
        test_prearm_required_acceptance_is_persisted_and_idempotent();
        test_prearm_rejects_non_atomic_or_inconsistent_batch();
        test_prearm_required_and_optional_transport_failure_policy();
        test_prearm_not_applicable_never_contacts_citrus();
        test_post_close_finalization_is_complete_idempotent_and_manifest_bound();
        test_manifest_never_infers_bound_without_final_receipts();
        test_streaming_sha256_matches_known_vector();
        std::cout << "recording_observation_binding_tests: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "recording_observation_binding_tests: FAIL: "
                  << error.what() << '\n';
        return 1;
    }
}
