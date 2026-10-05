// Recording subject references (Palette intake, design "c", 2026-10-05):
// at record start Orange records, per recording camera, the stable MetaZebrobot
// identifiers of the dish the operator declared for that camera (dish_id,
// dish_uuid, revision, updated_at, and the fish subjects registered to the dish
// with their revisions), exactly as the MetaZebrobot API served them. Orange
// never copies biological fields, never fills a default, never blocks a record
// start on the lookup, and never refreshes the block: it is frozen into the
// sealed recording start snapshot and copied verbatim into every
// recording_session.json by the common manifest writer. Palette re-fetches the
// record at intake, pins biology by (dish_uuid, revision) and compares.
//
// Emitted block (recording_snapshot_start.json session.subject_references and
// recording_session.json subject_references), keyed by exact camera serial:
//   {
//     "schema_id": "orange.recording_subject_reference", "schema_version": 1,
//     "status": "collected" | "not_collected" | "lookup_failed",
//     "reason": "" when collected, otherwise non-empty,
//     "zebrobot": null when not_collected, else {
//        "base_url", "endpoint", "fish_endpoint", "api_schema_version": 2,
//        "queried_at_utc", "http_status": int|null,
//        "error": null | {"kind": "transport"|"http", "detail_error": str|null, "message"} },
//     "dish": null unless collected: {"dish_id", "dish_uuid", "revision", "updated_at"},
//     "dish_fish": [ {"fish_id", "revision", "updated_at"} ],   // registered to the dish, not "imaged"
//     "dish_fish_lookup": null when not_collected, else
//        {"status": "complete"|"failed"|"not_attempted", "http_status": int|null, "error": ...}
//   }
// Rules: collected <=> dish non-null and zebrobot.error null; lookup_failed <=>
// zebrobot.error non-null; not_collected <=> zebrobot null and dish null and
// dish_fish empty. Every recording camera has an entry; absence is never
// implied by omission. A failed fish lookup is never read as "no individuals".
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "json.hpp"

namespace orange::recording {

// Minimal blocking HTTP/1.1 GET over a plain socket (http:// only), bounded by
// one deadline for connect + send + receive. No dependencies.
struct HttpGetResult {
    bool transport_ok = false;   // false: connect/send/receive failed or timed out
    int status = 0;              // HTTP status when transport_ok
    std::string body;
    std::string error;           // transport error text when !transport_ok
};
HttpGetResult HttpGet(const std::string& url, int timeout_ms);
using HttpGetFn = std::function<HttpGetResult(const std::string& url, int timeout_ms)>;

// Operator configuration (app config recording.zebrobot / headless fixed.zebrobot).
struct ZebrobotLookupConfig {
    std::string base_url;        // e.g. http://delahantyj-ws1.hhmi.org (no trailing slash)
    int timeout_ms = 2000;
    bool configured() const { return !base_url.empty(); }
    static ZebrobotLookupConfig Parse(const nlohmann::json& config);
    nlohmann::json ToJson() const;
};

// Operator declaration of which dish each camera images (app config
// recording.subject_references / headless fixed.subject_references):
// {"schema_version":1, "default": {"dish_id": "..."}?, "cameras": {"<serial>": {"dish_id": "..."}}?}
// Resolved per recording camera: its own entry, else the default, else no dish
// (which becomes status not_collected / no_dish_declared, never a refusal).
struct SubjectReferencesConfig {
    std::optional<std::string> default_dish_id;
    std::map<std::string, std::string> cameras;
    bool configured() const { return default_dish_id.has_value() || !cameras.empty(); }
    static SubjectReferencesConfig Parse(const nlohmann::json& config);
    nlohmann::json ToJson() const;
    std::map<std::string, std::optional<std::string>> Resolve(const std::vector<std::string>& recording_serials) const;
};

inline constexpr const char* kSubjectReferenceSchemaId = "orange.recording_subject_reference";
inline constexpr int kSubjectReferenceSchemaVersion = 1;
inline constexpr int kZebrobotApiSchemaVersion = 2;

// One camera's reference. `dish_id` empty -> not_collected / no_dish_declared.
// With a dish but no base_url -> lookup_failed (transport, zebrobot_not_configured).
// Performs the dish GET (/dishes/<id>/citrus-snapshot) and, when that
// succeeded, the fish GET (/dishes/<id>/fish); each bounded by timeout_ms.
nlohmann::json BuildSubjectReference(const std::optional<std::string>& dish_id,
                                     const ZebrobotLookupConfig& zebrobot,
                                     const std::string& queried_at_utc,
                                     const HttpGetFn& http_get = HttpGet);

// All recording cameras (one entry each, keyed by serial).
nlohmann::json BuildSubjectReferences(const std::map<std::string, std::optional<std::string>>& dish_by_serial,
                                      const ZebrobotLookupConfig& zebrobot,
                                      const std::string& queried_at_utc,
                                      const HttpGetFn& http_get = HttpGet);

// An entry that declares nothing was collected, with an explicit reason
// (e.g. "synthetic_bundle", "no_dish_declared").
nlohmann::json NotCollectedSubjectReference(const std::string& reason);

// Strict validation of an emitted block (schema, closed keys, the status rules,
// non-empty serial keys). Throws std::runtime_error.
void ValidateEmittedSubjectReferences(const nlohmann::json& block);

// Reads session.subject_references from the recording's sealed start snapshot
// (recording_snapshot_start.json, else recording_snapshot.json); nullopt when
// none was frozen.
std::optional<nlohmann::json> ReadFrozenSubjectReferences(const std::string& recording_folder,
                                                          std::string* error_out);

// Manifest gate (write_recording_session_manifest): copies the frozen block
// verbatim into manifest["subject_references"], requires its membership to
// equal manifest["cameras"], refuses a manifest that already carries a
// different block. No frozen block: the manifest is left without one.
void ApplySubjectReferencesGate(const std::string& recording_folder, nlohmann::json* manifest);

}  // namespace orange::recording
