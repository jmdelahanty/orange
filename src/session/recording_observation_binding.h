#pragma once

#include "json.hpp"

#include <string>

namespace orange::session {

inline constexpr const char* kObservationBindingRequestSchemaId =
    "orange.citrus.recording_observation_binding_request";
inline constexpr const char* kObservationBindingAcceptanceSchemaId =
    "citrus.recording_observation_binding_acceptance";
inline constexpr const char* kObservationBindingFinalizedReceiptSchemaId =
    "citrus.recording_observation_finalized_receipt";
inline constexpr int kObservationBindingSchemaVersion = 1;
// Binding request v2 (2026-09-24, joint with Citrus): the request contract
// carries the frozen parent recording context (contract.recording_context,
// seven fields, inside the request digest) and Citrus answers with acceptance
// v2 whose rejection reasons add recording_context_{missing,invalid,
// mismatch,unavailable} and batch_rejected. Envelope and contract carry the
// same version; a v2 request needs a v2 acceptance; receipts stay v1 (Citrus
// 9aa6d57). Opt-in until the joint test passes:
// ORANGE_CITRUS_BINDING_REQUEST_VERSION=2 (default 1).
inline constexpr int kObservationBindingRequestSchemaVersionV2 = 2;
int resolve_recording_observation_binding_request_version(std::string* error_out = nullptr);
inline bool accepted_observation_binding_schema_version(int version)
{
    return version == kObservationBindingSchemaVersion ||
           version == kObservationBindingRequestSchemaVersionV2;
}
// The schema_version of a sealed envelope or of a contract payload as a
// JSON integer (1 or 2); 0 for anything else (missing, float, out of range).
inline int binding_record_schema_version(const nlohmann::json& record)
{
    if (!record.is_object() || !record.contains("schema_version") ||
        !record.at("schema_version").is_number_integer()) return 0;
    const auto& v = record.at("schema_version");
    if (v == kObservationBindingSchemaVersion) return kObservationBindingSchemaVersion;
    if (v == kObservationBindingRequestSchemaVersionV2) return kObservationBindingRequestSchemaVersionV2;
    return 0;
}

// Normalized lifecycle vocabulary for the future recording-context writer.
// A binding becomes authoritative only at `bound`; an acceptance alone is
// `accepted_pending_finalization`.
inline constexpr const char* kObservationBindingStatusNotApplicable =
    "not_applicable";
inline constexpr const char* kObservationBindingStatusRequested = "requested";
inline constexpr const char* kObservationBindingStatusAcceptedPendingFinalization =
    "accepted_pending_finalization";
inline constexpr const char* kObservationBindingStatusBound = "bound";
inline constexpr const char* kObservationBindingStatusUnbound = "unbound";
inline constexpr const char* kObservationBindingStatusHistoricallyUnavailable =
    "historically_unavailable";

// Seal a caller-supplied contract payload in the corresponding canonical
// semantic-digest envelope; the envelope takes the contract's schema_version
// (request/acceptance 1 or 2, receipt 1).  The payload is validated before it
// is sealed.  These functions perform no filesystem access or producer
// mutation.
bool seal_recording_observation_binding_request(
    const nlohmann::json& contract,
    nlohmann::json* request_out,
    std::string* error_out = nullptr);

bool seal_recording_observation_binding_acceptance(
    const nlohmann::json& contract,
    nlohmann::json* acceptance_out,
    std::string* error_out = nullptr);

bool seal_recording_observation_finalized_receipt(
    const nlohmann::json& contract,
    nlohmann::json* receipt_out,
    std::string* error_out = nullptr);

// Verify individual envelopes and the reciprocal chain.  The acceptance may
// be accepted or rejected.  A finalized receipt is valid only for an accepted
// acceptance and must exactly match request/acceptance identities, target,
// session IDs, and planned H5 path.
bool validate_recording_observation_binding_request(
    const nlohmann::json& request,
    std::string* error_out = nullptr);

bool validate_recording_observation_binding_acceptance(
    const nlohmann::json& acceptance,
    const nlohmann::json& request,
    std::string* error_out = nullptr);

bool validate_recording_observation_finalized_receipt(
    const nlohmann::json& receipt,
    const nlohmann::json& request,
    const nlohmann::json& acceptance,
    std::string* error_out = nullptr);

}  // namespace orange::session
