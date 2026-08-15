#pragma once

#include "json.hpp"
#include "session/recording_observation_request_artifacts.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace orange::session {

inline constexpr const char* kRecordingObservationPreArmDecisionSchemaId =
    "orange.recording.observation_binding_pre_arm_decision";
inline constexpr int kRecordingObservationPreArmDecisionSchemaVersion = 1;

struct RecordingObservationAcceptanceArtifact {
    std::string observation_context_id;
    std::string acceptance_id;
    std::string relative_path;
    std::string sha256;
    std::uint64_t byte_size = 0;
    nlohmann::json acceptance = nlohmann::json::object();
};

struct RecordingObservationPreArmResult {
    std::string binding_mode;
    std::string lifecycle_status;
    std::string reason;
    bool arm_allowed = false;
    bool transport_attempted = false;
    std::string decision_relative_path;
    std::string decision_sha256;
    std::uint64_t decision_byte_size = 0;
    std::vector<RecordingObservationAcceptanceArtifact> acceptances;
};

using RecordingObservationBindingTransport = std::function<bool(
    const nlohmann::json& request,
    nlohmann::json* response_out,
    std::string* error_out)>;

// Resolve required|optional|not_applicable from
// ORANGE_CITRUS_OBSERVATION_BINDING_MODE. The default remains optional so an
// ordinary Orange recording is never made Citrus-dependent implicitly.
std::string resolve_recording_observation_binding_mode(
    std::string* error_out = nullptr);

// Execute or verify the create-once pre-arm decision. A materialized request
// collection is sent as one bounded batch so Citrus can assign one shared
// experiment-group ID atomically. Semantic rejections are persisted as
// evidence and reported through arm_allowed rather than being confused with a
// transport failure.
bool execute_recording_observation_pre_arm(
    const std::string& recording_folder,
    const RecordingObservationBindingRequestMaterialization& requests,
    const std::string& binding_mode,
    const std::string& decided_at_utc,
    RecordingObservationPreArmResult* result_out,
    std::string* error_out = nullptr,
    RecordingObservationBindingTransport transport = {});

// Full idempotent producer gate used by GUI, headless, and the phased-start
// lifecycle backstop. not_applicable intentionally emits no Citrus request.
bool prepare_recording_observation_pre_arm(
    const std::string& recording_folder,
    const std::string& binding_mode,
    const std::string& decided_at_utc,
    RecordingObservationBindingRequestMaterialization* requests_out,
    RecordingObservationPreArmResult* result_out,
    std::string* error_out = nullptr,
    RecordingObservationBindingTransport transport = {});

nlohmann::json recording_observation_pre_arm_decision_reference(
    const RecordingObservationPreArmResult& result);

}  // namespace orange::session
