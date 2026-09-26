#include "recording_context.h"

#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>

namespace orange::recording {
namespace {

using json = nlohmann::json;

void require(bool ok, const std::string& message)
{
    if (!ok) throw std::runtime_error(message);
}

// Decode one UTF-8 code point at `i` (advancing it); false when malformed
// (truncated, overlong, surrogate, above U+10FFFF, or a stray continuation).
bool next_code_point(const std::string& s, std::size_t& i, char32_t& cp)
{
    const unsigned char c0 = static_cast<unsigned char>(s[i]);
    std::size_t n = 0;
    if (c0 < 0x80) { cp = c0; i += 1; return true; }
    if ((c0 & 0xE0) == 0xC0) { n = 1; cp = c0 & 0x1F; }
    else if ((c0 & 0xF0) == 0xE0) { n = 2; cp = c0 & 0x0F; }
    else if ((c0 & 0xF8) == 0xF0) { n = 3; cp = c0 & 0x07; }
    else return false;
    if (s.size() - i <= n) return false;  // needs n continuation bytes
    for (std::size_t k = 1; k <= n; ++k) {
        const unsigned char c = static_cast<unsigned char>(s[i + k]);
        if ((c & 0xC0) != 0x80) return false;
        cp = (cp << 6) | (c & 0x3F);
    }
    static const char32_t min_for[4] = {0, 0x80, 0x800, 0x10000};
    if (cp < min_for[n] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    i += n + 1;
    return true;
}

// Unicode control characters (general category Cc): C0, DEL, C1.
bool is_control(char32_t cp) { return cp < 0x20 || (cp >= 0x7F && cp <= 0x9F); }

// The ECMA-262 `\s` set, which the JSON Schema label pattern uses at the
// label boundaries; a superset of Python str.strip()'s non-control set, so a
// label Orange accepts is never trimmed by the Citrus transfer validator.
bool is_boundary_whitespace(char32_t cp)
{
    return cp == 0x09 || cp == 0x0A || cp == 0x0B || cp == 0x0C || cp == 0x0D ||
           cp == 0x20 || cp == 0xA0 || cp == 0x1680 ||
           (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 ||
           cp == 0x202F || cp == 0x205F || cp == 0x3000 || cp == 0xFEFF;
}

// Producer label contract (recording_type, recording_subtype), shared with
// the Citrus acceptance parser and the transfer-v2 schema: non-empty, at most
// 1024 UTF-8 bytes (the JSON Schema maxLength 1024 counts characters and is
// the looser bound), well-formed UTF-8, no Unicode control character (C0,
// DEL, C1) anywhere, and no leading or trailing whitespace. Never trimmed or
// normalized: the declaration is preserved byte for byte or rejected.
std::string require_label(const json& j, const char* key, const char* what)
{
    require(j.contains(key) && j.at(key).is_string(), std::string("recording context ") + key + " must be a string (" + what + ")");
    const std::string value = j.at(key).get<std::string>();
    require(!value.empty(), std::string("recording context ") + key + " must not be empty");
    require(value.size() <= 1024, std::string("recording context ") + key + " exceeds 1024 UTF-8 bytes");
    char32_t first = 0, last = 0;
    std::size_t i = 0;
    bool any = false;
    while (i < value.size()) {
        char32_t cp = 0;
        require(next_code_point(value, i, cp), std::string("recording context ") + key + " is not valid UTF-8");
        require(!is_control(cp), std::string("recording context ") + key + " contains a control character");
        if (!any) { first = cp; any = true; }
        last = cp;
    }
    require(!is_boundary_whitespace(first) && !is_boundary_whitespace(last),
            std::string("recording context ") + key + " has surrounding whitespace");
    return value;
}

std::string require_enum(const json& j, const char* key, std::initializer_list<const char*> allowed)
{
    require(j.contains(key) && j.at(key).is_string(), std::string("recording context ") + key + " must be a string");
    const std::string value = j.at(key).get<std::string>();
    for (const char* option : allowed) {
        if (value == option) return value;
    }
    std::string choices;
    for (const char* option : allowed) { if (!choices.empty()) choices += "|"; choices += option; }
    throw std::runtime_error(std::string("recording context ") + key + " must be one of " + choices + ", got '" + value + "'");
}

// Closed object: no key outside `keys`; every key in `keys` present except
// those listed in `optional`.
void exact_keys(const json& j, const std::set<std::string>& keys, const char* what,
                const std::set<std::string>& optional = {})
{
    require(j.is_object(), std::string(what) + " must be an object");
    for (auto it = j.begin(); it != j.end(); ++it) {
        require(keys.count(it.key()) != 0, std::string("unknown ") + what + " field: " + it.key());
    }
    for (const auto& key : keys) {
        require(j.contains(key) || optional.count(key) != 0, std::string(what) + " is missing required field " + key);
    }
}

const std::set<std::string> kConfigKeys = {
    "recording_type", "recording_subtype", "behavior_mode", "recording_intent", "data_origin"};
const std::set<std::string> kEmittedKeys = {
    "schema_id", "schema_version",
    "recording_type", "recording_subtype", "behavior_mode", "recording_intent", "data_origin"};

RecordingContext parse_fields(const json& j, bool subtype_required)
{
    RecordingContext c;
    c.recording_type = require_label(j, "recording_type", "producer label");
    if (j.contains("recording_subtype")) {
        require(!j.at("recording_subtype").is_null(),
                "recording context recording_subtype must not be null; omit the key to leave the subtype unspecified");
        require(!(j.at("recording_subtype").is_string() && j.at("recording_subtype").get<std::string>().empty()),
                "recording context recording_subtype must not be empty; omit the key to leave the subtype unspecified");
        c.recording_subtype = require_label(j, "recording_subtype", "producer label");
    } else {
        require(!subtype_required, "recording context is missing required field recording_subtype (schema_version 1)");
    }
    c.behavior_mode = require_enum(j, "behavior_mode", {"free", "embedded", "none"});
    c.recording_intent = require_enum(j, "recording_intent", {"stimulus_experiment", "recording_only"});
    c.data_origin = require_enum(j, "data_origin", {"acquired", "synthetic"});
    return c;
}

json read_json_file(const std::filesystem::path& path)
{
    std::ifstream in(path);
    require(static_cast<bool>(in), "cannot read " + path.string());
    return json::parse(in);
}

}  // namespace

RecordingContext RecordingContext::Parse(const json& entry)
{
    exact_keys(entry, kConfigKeys, "recording context", {"recording_subtype"});
    return parse_fields(entry, /*subtype_required=*/false);
}

RecordingContext RecordingContext::ParseEmitted(const json& emitted)
{
    exact_keys(emitted, kEmittedKeys, "emitted recording context", {"recording_subtype"});
    require(emitted.at("schema_id").is_string() && emitted.at("schema_id") == kSchemaId,
            std::string("emitted recording context schema_id must be ") + kSchemaId);
    const json& version = emitted.at("schema_version");
    require(version.is_number_integer() && !version.is_boolean() &&
            (version == kSchemaVersion || version == kSchemaVersionOptionalSubtype),
            "emitted recording context schema_version must be 1 or 2");
    return parse_fields(emitted, /*subtype_required=*/version == kSchemaVersion);
}

json RecordingContext::ToJson() const
{
    json j = {{"recording_type", recording_type},
              {"behavior_mode", behavior_mode}, {"recording_intent", recording_intent}, {"data_origin", data_origin}};
    if (recording_subtype) j["recording_subtype"] = *recording_subtype;
    return j;
}

json RecordingContext::ToEmittedJson() const
{
    json j = ToJson();
    j["schema_id"] = kSchemaId;
    j["schema_version"] = emitted_schema_version();
    return j;
}

bool RecordingContext::operator==(const RecordingContext& o) const
{
    return recording_type == o.recording_type && recording_subtype == o.recording_subtype &&
           behavior_mode == o.behavior_mode && recording_intent == o.recording_intent && data_origin == o.data_origin;
}

RecordingContextsConfig RecordingContextsConfig::Parse(const json& config)
{
    require(config.is_object(), "recording contexts config must be an object");
    for (auto it = config.begin(); it != config.end(); ++it) {
        require(it.key() == "schema_version" || it.key() == "default" || it.key() == "cameras",
                "unknown recording contexts config field: " + it.key());
    }
    require(config.contains("schema_version") && config.at("schema_version").is_number_integer() &&
            !config.at("schema_version").is_boolean() && config.at("schema_version") == 1,
            "recording contexts config requires schema_version 1");
    RecordingContextsConfig c;
    if (config.contains("default")) {
        c.default_context = RecordingContext::Parse(config.at("default"));
    }
    if (config.contains("cameras")) {
        require(config.at("cameras").is_object(), "recording contexts config cameras must be an object keyed by serial");
        for (auto it = config.at("cameras").begin(); it != config.at("cameras").end(); ++it) {
            require(!it.key().empty(), "recording contexts config camera serial must not be empty");
            c.cameras[it.key()] = RecordingContext::Parse(it.value());
        }
    }
    require(c.configured(), "recording contexts config declares neither a default nor any camera");
    return c;
}

json RecordingContextsConfig::ToJson() const
{
    json j = {{"schema_version", 1}};
    if (default_context) j["default"] = default_context->ToJson();
    if (!cameras.empty()) {
        json cams = json::object();
        for (const auto& [serial, ctx] : cameras) cams[serial] = ctx.ToJson();
        j["cameras"] = cams;
    }
    return j;
}

std::map<std::string, RecordingContext> RecordingContextsConfig::Resolve(const std::vector<std::string>& recording_serials) const
{
    require(!recording_serials.empty(), "recording contexts: no recording cameras to resolve");
    std::map<std::string, RecordingContext> resolved;
    for (const auto& serial : recording_serials) {
        require(!serial.empty(), "recording contexts: empty camera serial");
        require(resolved.count(serial) == 0, "recording contexts: duplicate camera serial " + serial);
        const auto it = cameras.find(serial);
        if (it != cameras.end()) {
            resolved[serial] = it->second;
        } else if (default_context) {
            resolved[serial] = *default_context;
        } else {
            throw std::runtime_error("recording contexts: camera " + serial + " has no context entry and no default is configured");
        }
    }
    return resolved;
}

std::map<std::string, RecordingContext> ParseEmittedRecordingContexts(const json& block)
{
    require(block.is_object() && !block.empty(), "recording_contexts must be a non-empty object keyed by camera serial");
    std::map<std::string, RecordingContext> out;
    for (auto it = block.begin(); it != block.end(); ++it) {
        require(!it.key().empty(), "recording_contexts has an empty camera serial key");
        out[it.key()] = RecordingContext::ParseEmitted(it.value());
    }
    return out;
}

json EmittedRecordingContextsJson(const std::map<std::string, RecordingContext>& contexts)
{
    json j = json::object();
    for (const auto& [serial, ctx] : contexts) j[serial] = ctx.ToEmittedJson();
    return j;
}

std::optional<std::map<std::string, RecordingContext>> ReadFrozenRecordingContexts(
    const std::string& recording_folder, std::string* error_out)
{
    try {
        const std::filesystem::path root(recording_folder);
        const auto sealed = root / "recording_snapshot_start.json";
        const auto mutable_path = root / "recording_snapshot.json";
        const auto path = std::filesystem::exists(sealed) ? sealed : mutable_path;
        if (!std::filesystem::exists(path)) return std::nullopt;
        const json snapshot = read_json_file(path);
        if (!snapshot.is_object() || !snapshot.contains("session") || !snapshot.at("session").is_object() ||
            !snapshot.at("session").contains("recording_contexts")) {
            return std::nullopt;
        }
        return ParseEmittedRecordingContexts(snapshot.at("session").at("recording_contexts"));
    } catch (const std::exception& ex) {
        if (error_out) *error_out = std::string("frozen recording_contexts: ") + ex.what();
        return std::nullopt;
    }
}

void ApplyRecordingContextsGate(const std::string& recording_folder, json* manifest)
{
    require(manifest != nullptr && manifest->is_object(), "recording contexts gate needs a manifest object");
    // Parent manifests only; clip manifests (orange.recording_clip) carry no cameras block.
    if (!manifest->contains("cameras") || !manifest->at("cameras").is_array()) return;
    std::string error;
    const auto frozen = ReadFrozenRecordingContexts(recording_folder, &error);
    require(error.empty(), error);
    if (!frozen) {
        require(!manifest->contains("recording_contexts"),
                "manifest carries recording_contexts but the recording start snapshot froze none");
        return;
    }
    std::set<std::string> cameras;
    for (const auto& serial : manifest->at("cameras")) {
        require(serial.is_string(), "manifest cameras must be serial strings");
        cameras.insert(serial.get<std::string>());
    }
    std::set<std::string> frozen_serials;
    for (const auto& [serial, ctx] : *frozen) frozen_serials.insert(serial);
    require(cameras == frozen_serials,
            "recording_contexts membership does not equal the manifest camera set");
    const json emitted = EmittedRecordingContextsJson(*frozen);
    if (manifest->contains("recording_contexts")) {
        require(manifest->at("recording_contexts") == emitted,
                "manifest recording_contexts differ from the frozen start snapshot");
    }
    (*manifest)["recording_contexts"] = emitted;
}

std::string ApplyRecordingIntentToBindingMode(const std::string& recording_folder,
                                              const std::string& binding_mode,
                                              std::string* error_out)
{
    std::string error;
    const auto frozen = ReadFrozenRecordingContexts(recording_folder, &error);
    if (!error.empty()) {
        if (error_out) *error_out = error;
        return {};
    }
    if (!frozen) return binding_mode;
    bool any_recording_only = false;
    for (const auto& [serial, ctx] : *frozen) {
        if (ctx.recording_intent == "recording_only") any_recording_only = true;
    }
    if (!any_recording_only) return binding_mode;
    if (binding_mode == "required") {
        if (error_out) {
            *error_out = "recording_intent is recording_only but ORANGE_CITRUS_OBSERVATION_BINDING_MODE=required; "
                         "a recording-only session must not bind Citrus observations";
        }
        return {};
    }
    return "not_applicable";
}

}  // namespace orange::recording
