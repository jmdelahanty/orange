#include "session/recording_observation_binding.h"

#include "gui/spatial_layout/sha256.h"
#include "session/recording_observation_identity.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include <string>

namespace orange::session {
namespace {

using json = nlohmann::json;

bool fail(std::string* error_out, const std::string& message)
{
    if (error_out != nullptr) {
        *error_out = message;
    }
    return false;
}

bool exact_keys(const json& value, const std::set<std::string>& expected)
{
    if (!value.is_object() || value.size() != expected.size()) {
        return false;
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (expected.count(it.key()) == 0) {
            return false;
        }
    }
    return true;
}

bool valid_identifier(const std::string& value)
{
    if (value.empty() || value.size() > 512 ||
        std::isspace(static_cast<unsigned char>(value.front())) != 0 ||
        std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        return false;
    }
    return std::none_of(value.begin(), value.end(), [](const unsigned char ch) {
        return ch < 0x20 || ch == 0x7f;
    });
}

bool valid_sha256(const std::string& value)
{
    if (value.size() != 71 || value.rfind("sha256:", 0) != 0) {
        return false;
    }
    return std::all_of(value.begin() + 7, value.end(), [](const unsigned char ch) {
        return std::isdigit(ch) != 0 || (ch >= 'a' && ch <= 'f');
    });
}

bool valid_derived_id(const std::string& value, const char* prefix)
{
    const std::string expected_prefix(prefix);
    if (value.size() != expected_prefix.size() + 64 ||
        value.rfind(expected_prefix, 0) != 0) {
        return false;
    }
    return std::all_of(
        value.begin() + static_cast<std::ptrdiff_t>(expected_prefix.size()),
        value.end(), [](const unsigned char ch) {
            return std::isdigit(ch) != 0 || (ch >= 'a' && ch <= 'f');
        });
}

bool valid_utc_timestamp(const std::string& value)
{
    // The producer will use the existing UTC formatter.  This bounded lexical
    // gate rejects empty/free-form text without implementing a second clock
    // parser in the contract boundary.
    return value.size() >= 20 && value.size() <= 64 &&
        value.find('T') != std::string::npos && value.back() == 'Z';
}

bool safe_relative_path(const std::string& value)
{
    if (value.empty()) {
        return false;
    }
    const std::filesystem::path path(value);
    if (path.is_absolute()) {
        return false;
    }
    for (const auto& component : path) {
        if (component == "." || component == ".." || component.empty()) {
            return false;
        }
    }
    return true;
}

bool safe_absolute_path(const std::string& value)
{
    if (value.size() < 2) {
        return false;
    }
    const std::filesystem::path path(value);
    if (!path.is_absolute() || path.lexically_normal() != path) {
        return false;
    }
    for (const auto& component : path) {
        if (component == "." || component == "..") {
            return false;
        }
    }
    return true;
}

std::string canonical_sha256(const json& value)
{
    const std::string bytes = value.dump(
        -1, ' ', false, json::error_handler_t::strict);
    return "sha256:" +
        orange::gui::spatial_layout::checksum::sha256_hex(bytes);
}

bool valid_target(const json& target, std::string* error_out)
{
    if (!exact_keys(target, {
            "rig_id", "canvas_name", "arena_id", "camera_id",
            "source_camera_stream_id"})) {
        return fail(error_out, "observation binding target fields are invalid");
    }
    for (const char* field : {
             "rig_id", "canvas_name", "arena_id", "camera_id",
             "source_camera_stream_id"}) {
        if (!valid_identifier(target.value(field, ""))) {
            return fail(error_out,
                        std::string("observation binding target ") + field +
                            " is invalid");
        }
    }
    return true;
}

bool validate_request_contract(const json& contract, std::string* error_out)
{
    if (!exact_keys(contract, {
            "schema_id", "schema_version", "observation_context_id",
            "observation_identity_sha256", "observation_identity",
            "binding_mode", "requested_at_utc", "recording", "target",
            "recording_geometry_contract"}) ||
        contract.value("schema_id", "") != kObservationBindingRequestSchemaId ||
        contract.value("schema_version", 0) != kObservationBindingSchemaVersion) {
        return fail(error_out, "observation binding request contract is invalid");
    }

    RecordingObservationEdgeIdentity identity;
    if (!parse_recording_observation_identity(
            contract.at("observation_identity"), &identity, error_out)) {
        return false;
    }
    const json& identity_record = contract.at("observation_identity");
    if (contract.value("observation_context_id", "") !=
            identity_record.value("observation_context_id", "") ||
        contract.value("observation_identity_sha256", "") !=
            identity_record.value("identity_sha256", "")) {
        return fail(error_out,
                    "request observation identity reference does not match embedded identity");
    }

    const std::string mode = contract.value("binding_mode", "");
    if (mode != "required" && mode != "optional") {
        return fail(error_out, "request binding_mode must be required or optional");
    }
    if (!valid_utc_timestamp(contract.value("requested_at_utc", ""))) {
        return fail(error_out, "request requested_at_utc is invalid");
    }

    const json& recording = contract.at("recording");
    if (!exact_keys(recording, {
            "recording_id", "recording_folder", "recording_snapshot"}) ||
        recording.value("recording_id", "") != identity.recording_id ||
        !valid_identifier(recording.value("recording_id", ""))) {
        return fail(error_out,
                    "request recording identity does not match observation identity");
    }
    if (!safe_absolute_path(recording.value("recording_folder", ""))) {
        return fail(error_out,
                    "request recording_folder must be normalized and absolute");
    }
    const json& snapshot = recording.at("recording_snapshot");
    if (!exact_keys(snapshot, {"role", "relative_path", "sha256"}) ||
        snapshot.value("role", "") != "immutable_recording_start_snapshot" ||
        !safe_relative_path(snapshot.value("relative_path", "")) ||
        !valid_sha256(snapshot.value("sha256", ""))) {
        return fail(error_out, "request immutable recording snapshot is invalid");
    }

    const json& target = contract.at("target");
    if (!valid_target(target, error_out)) {
        return false;
    }
    if (target.value("rig_id", "") != identity.rig_id ||
        target.value("canvas_name", "") != identity.canvas_name ||
        target.value("arena_id", "") != identity.arena_id ||
        target.value("camera_id", "") != identity.camera_id ||
        target.value("source_camera_stream_id", "") !=
            identity.source_camera_stream_id) {
        return fail(error_out,
                    "request target does not match observation identity");
    }

    const json& geometry = contract.at("recording_geometry_contract");
    if (!geometry.is_object()) {
        return fail(error_out, "recording geometry reference must be an object");
    }
    const std::string geometry_status = geometry.value("status", "");
    if (geometry_status == "available") {
        if (!exact_keys(geometry, {"status", "relative_path", "sha256"}) ||
            !safe_relative_path(geometry.value("relative_path", "")) ||
            !valid_sha256(geometry.value("sha256", ""))) {
            return fail(error_out,
                        "available recording geometry reference is invalid");
        }
    } else if (geometry_status == "not_available") {
        if (!exact_keys(geometry, {"status", "reason"})) {
            return fail(error_out,
                        "unavailable recording geometry reference is invalid");
        }
        const std::string reason = geometry.value("reason", "");
        if (reason != "not_configured" && reason != "contract_unavailable" &&
            reason != "contract_invalid") {
            return fail(error_out,
                        "recording geometry absence reason is invalid");
        }
    } else {
        return fail(error_out, "recording geometry status is invalid");
    }
    return true;
}

bool validate_acceptance_contract(const json& contract,
                                  std::string* error_out)
{
    if (!contract.is_object()) {
        return fail(error_out, "binding acceptance contract must be an object");
    }
    const std::string status = contract.value("status", "");
    if (status == "accepted") {
        if (!exact_keys(contract, {
                "schema_id", "schema_version", "status", "request_id",
                "request_contract_sha256",
                "observation_context_id", "decided_at_utc",
                "citrus_experiment_id", "citrus_session_uuid", "target",
                "planned_h5_relative_path"})) {
            return fail(error_out, "accepted binding fields are invalid");
        }
        if (!valid_identifier(contract.value("citrus_experiment_id", "")) ||
            !valid_identifier(contract.value("citrus_session_uuid", "")) ||
            !safe_relative_path(contract.value("planned_h5_relative_path", "")) ||
            !valid_target(contract.at("target"), error_out)) {
            return fail(error_out, "accepted binding content is invalid");
        }
    } else if (status == "rejected") {
        if (!exact_keys(contract, {
                "schema_id", "schema_version", "status", "request_id",
                "request_contract_sha256",
                "observation_context_id", "decided_at_utc", "reason"})) {
            return fail(error_out, "rejected binding fields are invalid");
        }
        const std::set<std::string> reasons = {
            "identity_mismatch", "recording_pointer_mismatch",
            "recording_snapshot_mismatch", "target_mismatch",
            "geometry_mismatch", "output_path_mismatch",
            "runtime_authority_unavailable", "internal_error"};
        if (reasons.count(contract.value("reason", "")) == 0) {
            return fail(error_out, "binding rejection reason is invalid");
        }
    } else {
        return fail(error_out, "binding acceptance status is invalid");
    }

    if (contract.value("schema_id", "") !=
            kObservationBindingAcceptanceSchemaId ||
        contract.value("schema_version", 0) !=
            kObservationBindingSchemaVersion ||
        !valid_derived_id(contract.value("request_id", ""), "obsbindreq_") ||
        !valid_sha256(contract.value("request_contract_sha256", "")) ||
        !valid_derived_id(
            contract.value("observation_context_id", ""), "obsctx_") ||
        !valid_utc_timestamp(contract.value("decided_at_utc", ""))) {
        return fail(error_out, "binding acceptance common content is invalid");
    }
    return true;
}

bool validate_receipt_contract(const json& contract, std::string* error_out)
{
    if (!exact_keys(contract, {
            "schema_id", "schema_version", "request_id",
            "request_contract_sha256", "acceptance_id",
            "acceptance_contract_sha256", "observation_context_id",
            "finalized_at_utc", "citrus_experiment_id",
            "citrus_session_uuid", "target", "h5_artifact", "session_status",
            "runtime_geometry_contract_sha256", "protocol_semantic"}) ||
        contract.value("schema_id", "") !=
            kObservationBindingFinalizedReceiptSchemaId ||
        contract.value("schema_version", 0) != kObservationBindingSchemaVersion) {
        return fail(error_out, "finalized binding receipt contract is invalid");
    }
    if (!valid_derived_id(contract.value("request_id", ""), "obsbindreq_") ||
        !valid_sha256(contract.value("request_contract_sha256", "")) ||
        !valid_derived_id(
            contract.value("acceptance_id", ""), "obsbindacc_") ||
        !valid_sha256(contract.value("acceptance_contract_sha256", "")) ||
        !valid_derived_id(
            contract.value("observation_context_id", ""), "obsctx_") ||
        !valid_utc_timestamp(contract.value("finalized_at_utc", "")) ||
        !valid_identifier(contract.value("citrus_experiment_id", "")) ||
        !valid_identifier(contract.value("citrus_session_uuid", "")) ||
        contract.value("session_status", "") != "COMPLETE" ||
        !valid_sha256(contract.value("runtime_geometry_contract_sha256", "")) ||
        !valid_target(contract.at("target"), error_out)) {
        return fail(error_out, "finalized binding receipt content is invalid");
    }

    const json& artifact = contract.at("h5_artifact");
    if (!exact_keys(artifact, {"relative_path", "size_bytes", "sha256"}) ||
        !safe_relative_path(artifact.value("relative_path", "")) ||
        artifact.value("size_bytes", 0LL) <= 0 ||
        !valid_sha256(artifact.value("sha256", ""))) {
        return fail(error_out, "finalized H5 artifact is invalid");
    }

    const json& semantic = contract.at("protocol_semantic");
    if (!semantic.is_object()) {
        return fail(error_out, "protocol semantic identity must be an object");
    }
    const std::string semantic_status = semantic.value("status", "");
    if (semantic_status == "available") {
        if (!exact_keys(semantic, {"status", "semantic_sha256"}) ||
            !valid_sha256(semantic.value("semantic_sha256", ""))) {
            return fail(error_out, "available protocol semantic identity is invalid");
        }
    } else if (semantic_status == "unsupported") {
        if (!exact_keys(semantic, {"status", "semantic_status_sha256"}) ||
            !valid_sha256(semantic.value("semantic_status_sha256", ""))) {
            return fail(error_out, "unsupported protocol semantic status is invalid");
        }
    } else {
        return fail(error_out, "protocol semantic status is invalid");
    }
    return true;
}

bool seal(const char* schema_id,
          const char* id_prefix,
          const json& contract,
          json* record_out,
          std::string* error_out)
{
    if (record_out == nullptr) {
        return fail(error_out, "binding record output is null");
    }
    const std::string digest = canonical_sha256(contract);
    const std::string id_field =
        std::string(id_prefix) == "obsbindreq_" ? "request_id" :
        std::string(id_prefix) == "obsbindacc_" ? "acceptance_id" :
        "receipt_id";
    *record_out = {
        {"schema_id", schema_id},
        {"schema_version", kObservationBindingSchemaVersion},
        {"canonicalization", kRecordingObservationIdentityCanonicalization},
        {id_field, std::string(id_prefix) + digest.substr(7)},
        {"contract_sha256", digest},
        {"contract", contract},
    };
    return true;
}

bool validate_envelope(const json& record,
                       const char* schema_id,
                       const char* id_field,
                       const char* id_prefix,
                       std::string* error_out)
{
    if (!exact_keys(record, {
            "schema_id", "schema_version", "canonicalization", id_field,
            "contract_sha256", "contract"}) ||
        record.value("schema_id", "") != schema_id ||
        record.value("schema_version", 0) != kObservationBindingSchemaVersion ||
        record.value("canonicalization", "") !=
            kRecordingObservationIdentityCanonicalization) {
        return fail(error_out, "binding envelope schema is invalid");
    }
    const std::string digest = canonical_sha256(record.at("contract"));
    if (record.value("contract_sha256", "") != digest ||
        record.value(id_field, "") != std::string(id_prefix) + digest.substr(7)) {
        return fail(error_out, "binding envelope digest or derived ID mismatch");
    }
    return true;
}

}  // namespace

bool seal_recording_observation_binding_request(
    const nlohmann::json& contract,
    nlohmann::json* request_out,
    std::string* error_out)
{
    if (!validate_request_contract(contract, error_out)) {
        return false;
    }
    return seal(kObservationBindingRequestSchemaId, "obsbindreq_", contract,
                request_out, error_out);
}

bool seal_recording_observation_binding_acceptance(
    const nlohmann::json& contract,
    nlohmann::json* acceptance_out,
    std::string* error_out)
{
    if (!validate_acceptance_contract(contract, error_out)) {
        return false;
    }
    return seal(kObservationBindingAcceptanceSchemaId, "obsbindacc_", contract,
                acceptance_out, error_out);
}

bool seal_recording_observation_finalized_receipt(
    const nlohmann::json& contract,
    nlohmann::json* receipt_out,
    std::string* error_out)
{
    if (!validate_receipt_contract(contract, error_out)) {
        return false;
    }
    return seal(kObservationBindingFinalizedReceiptSchemaId, "obsbindfin_",
                contract, receipt_out, error_out);
}

bool validate_recording_observation_binding_request(
    const nlohmann::json& request,
    std::string* error_out)
{
    return validate_envelope(
               request, kObservationBindingRequestSchemaId, "request_id",
               "obsbindreq_", error_out) &&
        validate_request_contract(request.at("contract"), error_out);
}

bool validate_recording_observation_binding_acceptance(
    const nlohmann::json& acceptance,
    const nlohmann::json& request,
    std::string* error_out)
{
    if (!validate_recording_observation_binding_request(request, error_out) ||
        !validate_envelope(
            acceptance, kObservationBindingAcceptanceSchemaId, "acceptance_id",
            "obsbindacc_", error_out) ||
        !validate_acceptance_contract(acceptance.at("contract"), error_out)) {
        return false;
    }
    const json& request_contract = request.at("contract");
    const json& acceptance_contract = acceptance.at("contract");
    if (acceptance_contract.value("request_id", "") !=
            request.value("request_id", "") ||
        acceptance_contract.value("request_contract_sha256", "") !=
            request.value("contract_sha256", "") ||
        acceptance_contract.value("observation_context_id", "") !=
            request_contract.value("observation_context_id", "")) {
        return fail(error_out,
                    "binding acceptance does not reference the exact request");
    }
    if (acceptance_contract.value("status", "") == "accepted" &&
        acceptance_contract.at("target") != request_contract.at("target")) {
        return fail(error_out,
                    "accepted binding target does not match request target");
    }
    return true;
}

bool validate_recording_observation_finalized_receipt(
    const nlohmann::json& receipt,
    const nlohmann::json& request,
    const nlohmann::json& acceptance,
    std::string* error_out)
{
    if (!validate_recording_observation_binding_acceptance(
            acceptance, request, error_out)) {
        return false;
    }
    if (acceptance.at("contract").value("status", "") != "accepted") {
        return fail(error_out, "rejected binding cannot have a finalized receipt");
    }
    if (!validate_envelope(
            receipt, kObservationBindingFinalizedReceiptSchemaId, "receipt_id",
            "obsbindfin_", error_out) ||
        !validate_receipt_contract(receipt.at("contract"), error_out)) {
        return false;
    }

    const json& request_contract = request.at("contract");
    const json& acceptance_contract = acceptance.at("contract");
    const json& receipt_contract = receipt.at("contract");
    if (receipt_contract.value("request_id", "") !=
            request.value("request_id", "") ||
        receipt_contract.value("request_contract_sha256", "") !=
            request.value("contract_sha256", "") ||
        receipt_contract.value("acceptance_id", "") !=
            acceptance.value("acceptance_id", "") ||
        receipt_contract.value("acceptance_contract_sha256", "") !=
            acceptance.value("contract_sha256", "") ||
        receipt_contract.value("observation_context_id", "") !=
            request_contract.value("observation_context_id", "") ||
        receipt_contract.value("citrus_experiment_id", "") !=
            acceptance_contract.value("citrus_experiment_id", "") ||
        receipt_contract.value("citrus_session_uuid", "") !=
            acceptance_contract.value("citrus_session_uuid", "") ||
        receipt_contract.at("target") != acceptance_contract.at("target") ||
        receipt_contract.at("h5_artifact").value("relative_path", "") !=
            acceptance_contract.value("planned_h5_relative_path", "")) {
        return fail(error_out,
                    "finalized receipt does not match request and acceptance chain");
    }
    return true;
}

}  // namespace orange::session
