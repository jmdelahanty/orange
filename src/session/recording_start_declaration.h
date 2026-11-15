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

// v2 (docs/recording_start_declaration_v2_scope.md, agreed with Palette,
// experiment-spec and pancake-plate 2026-11-15): v1 plus a required `source`
// naming where the declaration came from, and subject_references schema 2
// (camera entries may be {"blank": true}).
inline constexpr int kRecordingStartDeclarationSchemaVersionV2 = 2;
inline constexpr const char* kSourceKindOrchardRunPlan = "orchard_run_plan";
inline constexpr const char* kSourceKindOrangeRunSpec = "orange_run_spec";
inline constexpr const char* kSourceKindOperator = "operator";
// Closed: Palette refuses a rule it has no vectors for (pancake-plate owns them).
inline const std::set<std::string>& KnownPlanHashRules()
{
    static const std::set<std::string> rules = {"orchard_run_plan_v1_plan_sha256"};
    return rules;
}

struct RecordingStartDeclaration {
    int schema_version = kRecordingStartDeclarationSchemaVersion;
    std::string declared_by;
    std::string recording_intent;
    recording::SubjectReferencesConfig subject_references;
    nlohmann::json source = nullptr;  // v2 only, validated as received

    std::string source_kind() const { return source.is_object() ? source.value("kind", std::string()) : ""; }

    nlohmann::json ToJson() const
    {
        nlohmann::json j = {{"schema_id", kRecordingStartDeclarationSchemaId},
                            {"schema_version", schema_version},
                            {"declared_by", declared_by},
                            {"recording_intent", recording_intent},
                            {"subject_references", subject_references.ToJson()}};
        if (schema_version >= kRecordingStartDeclarationSchemaVersionV2) j["source"] = source;
        return j;
    }
};

namespace detail {

[[noreturn]] inline void declaration_fail(const std::string& message)
{
    throw std::runtime_error("recording start declaration: " + message);
}

inline void require_closed_keys(const nlohmann::json& j, const std::set<std::string>& keys, const std::string& what)
{
    if (!j.is_object()) declaration_fail(what + " must be an object");
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (!keys.count(it.key())) declaration_fail("unknown " + what + " field " + it.key());
    }
    for (const auto& key : keys) {
        if (!j.contains(key)) declaration_fail(what + " missing " + key);
    }
}

inline std::string require_match(const nlohmann::json& j, const std::string& key, const std::regex& pattern,
                                 const std::string& what, const std::string& form)
{
    if (!j.at(key).is_string() || !std::regex_match(j.at(key).get<std::string>(), pattern)) {
        declaration_fail(what + "." + key + " must be " + form);
    }
    return j.at(key).get<std::string>();
}

// Repo-relative POSIX path: no leading '/', no '..', no empty segments.
inline std::string require_relative_path(const nlohmann::json& j, const std::string& key, const std::string& what)
{
    if (!j.at(key).is_string()) declaration_fail(what + "." + key + " must be a string");
    const std::string path = j.at(key).get<std::string>();
    bool ok = !path.empty() && path.front() != '/' && path.back() != '/';
    std::size_t start = 0;
    while (ok && start <= path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::string segment = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        ok = !segment.empty() && segment != ".." && segment != "." &&
             segment.find_first_of(std::string("\0\\", 2)) == std::string::npos;
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    if (!ok) declaration_fail(what + "." + key + " must be a repo-relative POSIX path (no leading /, no .., no empty segments)");
    return path;
}

inline void validate_source(const nlohmann::json& source, const std::string& declared_by)
{
    if (!source.is_object() || !source.contains("kind") || !source.at("kind").is_string()) {
        declaration_fail("source must be an object with a string kind");
    }
    const std::string kind = source.at("kind").get<std::string>();
    if (kind == kSourceKindOrangeRunSpec || kind == kSourceKindOperator) {
        require_closed_keys(source, {"kind"}, "source");
        return;
    }
    if (kind != kSourceKindOrchardRunPlan) {
        declaration_fail("source.kind must be orchard_run_plan, orange_run_spec or operator");
    }
    static const std::regex kHex40("^[0-9a-f]{40}$");
    static const std::regex kHex64("^[0-9a-f]{64}$");
    static const std::regex kRepo("^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$");
    static const std::regex kUtc("^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z$");
    static const std::regex kText("^[^\\x00-\\x1f]{1,256}$");
    require_closed_keys(source,
                        {"kind", "repo", "commit", "path", "file_sha256", "tree_dirty", "plan_id", "plan_revision",
                         "plan_session_id", "plan_sha256", "plan_hash_rule", "sign_off", "generator",
                         "plan_copy_relative_path", "plan_json_copy_relative_path", "sign_off_copy_relative_path"},
                        "source");
    require_match(source, "repo", kRepo, "source", "owner/name");
    require_match(source, "commit", kHex40, "source", "40 lowercase hex");
    require_relative_path(source, "path", "source");
    const std::string file_sha256 = require_match(source, "file_sha256", kHex64, "source", "64 lowercase hex");
    if (!source.at("tree_dirty").is_boolean()) declaration_fail("source.tree_dirty must be a boolean");
    if (source.at("tree_dirty").get<bool>()) {
        declaration_fail("source.tree_dirty is true: a signed run from an edited or uncommitted plan is refused");
    }
    require_match(source, "plan_id", kText, "source", "a non-empty string");
    const auto& revision = source.at("plan_revision");
    if (!revision.is_number_integer() || revision.is_boolean() || revision.get<long long>() < 1) {
        declaration_fail("source.plan_revision must be an integer >= 1");
    }
    require_match(source, "plan_session_id", kText, "source", "a non-empty string");
    const std::string plan_sha256 = require_match(source, "plan_sha256", kHex64, "source", "64 lowercase hex");
    if (!source.at("plan_hash_rule").is_string() || !KnownPlanHashRules().count(source.at("plan_hash_rule").get<std::string>())) {
        declaration_fail("source.plan_hash_rule is not a known rule");
    }
    const nlohmann::json& sign_off = source.at("sign_off");
    require_closed_keys(sign_off, {"entry_id", "entry_sha256", "by", "at_utc", "record_path", "record_commit"},
                        "source.sign_off");
    require_match(sign_off, "entry_id", kText, "source.sign_off", "a non-empty string");
    const std::string entry_sha256 = require_match(sign_off, "entry_sha256", kHex64, "source.sign_off", "64 lowercase hex");
    if (!sign_off.at("by").is_string() || sign_off.at("by").get<std::string>() != declared_by) {
        declaration_fail("source.sign_off.by must equal declared_by");
    }
    require_match(sign_off, "at_utc", kUtc, "source.sign_off", "UTC YYYY-MM-DDTHH:MM:SSZ");
    require_relative_path(sign_off, "record_path", "source.sign_off");
    require_match(sign_off, "record_commit", kHex40, "source.sign_off", "40 lowercase hex");
    const nlohmann::json& generator = source.at("generator");
    require_closed_keys(generator, {"repo", "commit", "tool"}, "source.generator");
    require_match(generator, "repo", kRepo, "source.generator", "owner/name");
    require_match(generator, "commit", kHex40, "source.generator", "40 lowercase hex");
    require_relative_path(generator, "tool", "source.generator");
    auto require_exact = [&](const std::string& key, const std::string& expected) {
        if (!source.at(key).is_string() || source.at(key).get<std::string>() != expected) {
            declaration_fail("source." + key + " must be " + expected);
        }
    };
    require_exact("plan_copy_relative_path", "run_plan/" + file_sha256 + ".yaml");
    require_exact("plan_json_copy_relative_path", "run_plan/" + plan_sha256 + ".json");
    require_exact("sign_off_copy_relative_path", "run_plan/sign_off/" + entry_sha256 + ".json");
}

}  // namespace detail

// Throws std::runtime_error naming the first problem.
inline RecordingStartDeclaration ParseRecordingStartDeclaration(const nlohmann::json& j)
{
    using detail::declaration_fail;
    if (!j.is_object()) declaration_fail("must be an object");
    if (!j.contains("schema_version") || !j.at("schema_version").is_number_integer() ||
        j.at("schema_version").is_boolean() ||
        (j.at("schema_version") != kRecordingStartDeclarationSchemaVersion &&
         j.at("schema_version") != kRecordingStartDeclarationSchemaVersionV2)) {
        declaration_fail("schema_version must be 1 or 2");
    }
    RecordingStartDeclaration d;
    d.schema_version = j.at("schema_version").get<int>();
    const bool v2 = d.schema_version == kRecordingStartDeclarationSchemaVersionV2;
    std::set<std::string> keys = {"schema_id", "schema_version", "declared_by", "recording_intent",
                                  "subject_references"};
    if (v2) keys.insert("source");
    detail::require_closed_keys(j, keys, "declaration");
    if (j.at("schema_id") != kRecordingStartDeclarationSchemaId) {
        declaration_fail(std::string("schema_id must be ") + kRecordingStartDeclarationSchemaId);
    }
    // The HHMI identity form pancakebatter's host.yml uses for operators.
    static const std::regex kIdentity("^[a-z][a-z0-9_-]{0,31}$");
    if (!j.at("declared_by").is_string() ||
        !std::regex_match(j.at("declared_by").get<std::string>(), kIdentity)) {
        declaration_fail("declared_by must be an identity matching ^[a-z][a-z0-9_-]{0,31}$");
    }
    d.declared_by = j.at("declared_by").get<std::string>();
    if (!j.at("recording_intent").is_string()) declaration_fail("recording_intent must be a string");
    d.recording_intent = j.at("recording_intent").get<std::string>();
    if (d.recording_intent != "stimulus_experiment" && d.recording_intent != "recording_only") {
        declaration_fail("recording_intent must be stimulus_experiment or recording_only");
    }
    try {
        d.subject_references = recording::SubjectReferencesConfig::Parse(j.at("subject_references"));
    } catch (const std::exception& ex) {
        declaration_fail(ex.what());
    }
    if (d.subject_references.schema_version != d.schema_version) {
        declaration_fail("subject_references.schema_version must equal the declaration's schema_version");
    }
    if (v2) {
        detail::validate_source(j.at("source"), d.declared_by);
        d.source = j.at("source");
    }
    return d;
}

// Which start routes may carry a declaration of each kind (agreed with
// Palette 2026-11-15; Palette refuses a mismatch). v1 has no source and any
// route. In this first build an orchard plan reaches Orange only through a
// run spec: the GUI/orchestrator route needs the run_plan files, not built.
inline void CheckDeclarationRoute(const RecordingStartDeclaration& d, const std::string& confirmed_by)
{
    if (d.schema_version < kRecordingStartDeclarationSchemaVersionV2) return;
    const std::string kind = d.source_kind();
    const bool ok = (kind == kSourceKindOrchardRunPlan && confirmed_by == kConfirmedByRunSpec) ||
                    (kind == kSourceKindOrangeRunSpec && confirmed_by == kConfirmedByRunSpec) ||
                    (kind == kSourceKindOperator &&
                     (confirmed_by == kConfirmedByGuiOperator || confirmed_by == kConfirmedByLocalControl));
    if (!ok) {
        detail::declaration_fail("source.kind " + kind + " cannot be declared through " + confirmed_by);
    }
}

// An orchard plan must name every camera the recording records (declared or
// {"blank": true}); a camera it leaves out is refused, not assumed blank.
inline void CheckDeclarationCoversCameras(const RecordingStartDeclaration& d,
                                          const std::vector<std::string>& recording_serials)
{
    if (d.source_kind() != kSourceKindOrchardRunPlan) return;
    for (const std::string& serial : recording_serials) {
        if (!d.subject_references.cameras.count(serial)) {
            detail::declaration_fail("the plan does not declare recording camera " + serial +
                                     " (declare it, or {\"blank\": true})");
        }
    }
}

// The run_plan/ files an orchard declaration names, each with the sha256 its
// bytes must have (the file name). Empty for every other kind.
struct DeclarationFileCopy {
    std::string relative_path;
    std::string sha256;
};
inline std::vector<DeclarationFileCopy> DeclarationFileCopies(const RecordingStartDeclaration& d)
{
    if (d.source_kind() != kSourceKindOrchardRunPlan) return {};
    return {{d.source.at("plan_copy_relative_path").get<std::string>(), d.source.at("file_sha256").get<std::string>()},
            {d.source.at("plan_json_copy_relative_path").get<std::string>(), d.source.at("plan_sha256").get<std::string>()},
            {d.source.at("sign_off_copy_relative_path").get<std::string>(),
             d.source.at("sign_off").at("entry_sha256").get<std::string>()}};
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
