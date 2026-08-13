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

// Seal a caller-supplied schema-v1 contract payload in the corresponding
// canonical semantic-digest envelope.  The payload is validated before it is
// sealed.  These functions perform no filesystem access or producer mutation.
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
