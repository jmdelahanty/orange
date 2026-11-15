#pragma once

// What a person declares for one recording, and the per-recording subject
// confirmation built from it (subject_confirmation.json at the recording root).
//
// A GUI start takes the declaration from the Subject panel's Confirm button. A
// remote start (local-control `start_recording`, later orange_client and an
// MHS adapter) carries it in the request as params.declaration:
//
//   {"schema_id": "orange.recording_start_declaration", "schema_version": 1,
//    "declared_by": "<HHMI identity>",            // the person who declares; a claim
//    "recording_intent": "stimulus_experiment" | "recording_only",
//    "subject_references": {"schema_version": 1, "default"?: {...}, "cameras"?: {...}}}
//
// subject_references has the app config's form (recording_subject_reference.h);
// {"schema_version": 1} with no default and no cameras declares every camera
// blank (not_collected), the same as the panel's "Blank all". Every field is
// required: a remote start never falls back to the saved table or intent.
//
// The confirmation (orange.recording_subject_confirmation v2) keeps the two
// identities apart: `declaration.declared_by` is what the request claims, and
// `requester` is who actually sent it as the transport saw it (the socket's
// peer uid, the GUI's invoking user, or the experiment spec file an
// orange_client run read, with its sha256). v2 added `declaration` (null for a
// panel confirmation) and `requester` to v1's keys. confirmed_by is
// gui_operator, local_control or run_spec.

#include <map>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "json.hpp"
#include "recording_subject_reference.h"

namespace orange::session {

inline constexpr const char* kRecordingStartDeclarationSchemaId = "orange.recording_start_declaration";
inline constexpr int kRecordingStartDeclarationSchemaVersion = 1;
inline constexpr const char* kSubjectConfirmationSchemaId = "orange.recording_subject_confirmation";
inline constexpr int kSubjectConfirmationSchemaVersion = 2;

inline constexpr const char* kConfirmedByGuiOperator = "gui_operator";
inline constexpr const char* kConfirmedByLocalControl = "local_control";
inline constexpr const char* kConfirmedByRunSpec = "run_spec";

struct RecordingStartDeclaration {
    std::string declared_by;
    std::string recording_intent;
    recording::SubjectReferencesConfig subject_references;

    nlohmann::json ToJson() const
    {
        return {{"schema_id", kRecordingStartDeclarationSchemaId},
                {"schema_version", kRecordingStartDeclarationSchemaVersion},
                {"declared_by", declared_by},
                {"recording_intent", recording_intent},
                {"subject_references", subject_references.ToJson()}};
    }
};

// Throws std::runtime_error naming the first problem.
inline RecordingStartDeclaration ParseRecordingStartDeclaration(const nlohmann::json& j)
{
    auto fail = [](const std::string& message) {
        throw std::runtime_error("recording start declaration: " + message);
    };
    if (!j.is_object()) fail("must be an object");
    static const std::set<std::string> kKeys = {"schema_id", "schema_version", "declared_by",
                                                "recording_intent", "subject_references"};
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (!kKeys.count(it.key())) fail("unknown field " + it.key());
    }
    for (const auto& key : kKeys) {
        if (!j.contains(key)) fail("missing " + key);
    }
    if (j.at("schema_id") != kRecordingStartDeclarationSchemaId) {
        fail(std::string("schema_id must be ") + kRecordingStartDeclarationSchemaId);
    }
    if (!j.at("schema_version").is_number_integer() ||
        j.at("schema_version") != kRecordingStartDeclarationSchemaVersion) {
        fail("schema_version must be 1");
    }
    RecordingStartDeclaration d;
    // The HHMI identity form pancakebatter's host.yml uses for operators.
    static const std::regex kIdentity("^[a-z][a-z0-9_-]{0,31}$");
    if (!j.at("declared_by").is_string() ||
        !std::regex_match(j.at("declared_by").get<std::string>(), kIdentity)) {
        fail("declared_by must be an identity matching ^[a-z][a-z0-9_-]{0,31}$");
    }
    d.declared_by = j.at("declared_by").get<std::string>();
    if (!j.at("recording_intent").is_string()) fail("recording_intent must be a string");
    d.recording_intent = j.at("recording_intent").get<std::string>();
    if (d.recording_intent != "stimulus_experiment" && d.recording_intent != "recording_only") {
        fail("recording_intent must be stimulus_experiment or recording_only");
    }
    try {
        d.subject_references = recording::SubjectReferencesConfig::Parse(j.at("subject_references"));
    } catch (const std::exception& ex) {
        fail(ex.what());
    }
    return d;
}

// The one writer of orange.recording_subject_confirmation, for every way a
// recording is declared. `declaration` is null for a panel confirmation.
inline nlohmann::json BuildSubjectConfirmation(const recording::SubjectReferencesConfig& config,
                                               const std::vector<std::string>& camera_serials,
                                               const std::string& confirmed_by,
                                               const nlohmann::json& declaration,
                                               const nlohmann::json& requester,
                                               const std::string& confirmed_at_utc)
{
    nlohmann::json cameras = nlohmann::json::object();
    for (const auto& [serial, d] : config.Resolve(camera_serials)) {
        cameras[serial] = {{"dish_id", d.dish_id ? nlohmann::json(*d.dish_id) : nlohmann::json(nullptr)},
                           {"subject_count", d.subject_count ? nlohmann::json(*d.subject_count)
                                                             : nlohmann::json(nullptr)}};
    }
    return {{"schema_id", kSubjectConfirmationSchemaId},
            {"schema_version", kSubjectConfirmationSchemaVersion},
            {"confirmed_by", confirmed_by},
            {"confirmed_at_utc", confirmed_at_utc},
            {"cameras", cameras},
            {"config", config.ToJson()},
            {"declaration", declaration},
            {"requester", requester}};
}

}  // namespace orange::session
