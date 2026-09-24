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

std::string require_label(const json& j, const char* key, const char* what)
{
    require(j.contains(key) && j.at(key).is_string(), std::string("recording context ") + key + " must be a string (" + what + ")");
    const std::string value = j.at(key).get<std::string>();
    require(!value.empty(), std::string("recording context ") + key + " must not be empty");
    require(value.size() <= 1024, std::string("recording context ") + key + " exceeds 1024 characters");
    for (unsigned char c : value) {
        require(c >= 0x20 && c != 0x7f, std::string("recording context ") + key + " contains a control character");
    }
    require(value.front() != ' ' && value.back() != ' ' && value.front() != '\t' && value.back() != '\t',
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

void exact_keys(const json& j, const std::set<std::string>& keys, const char* what)
{
    require(j.is_object(), std::string(what) + " must be an object");
    for (auto it = j.begin(); it != j.end(); ++it) {
        require(keys.count(it.key()) != 0, std::string("unknown ") + what + " field: " + it.key());
    }
    for (const auto& key : keys) {
        require(j.contains(key), std::string(what) + " is missing required field " + key);
    }
}

const std::set<std::string> kConfigKeys = {
    "recording_type", "recording_subtype", "behavior_mode", "recording_intent", "data_origin"};
const std::set<std::string> kEmittedKeys = {
    "schema_id", "schema_version",
    "recording_type", "recording_subtype", "behavior_mode", "recording_intent", "data_origin"};

RecordingContext parse_fields(const json& j)
{
    RecordingContext c;
    c.recording_type = require_label(j, "recording_type", "producer label");
    c.recording_subtype = require_label(j, "recording_subtype", "producer label");
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
    exact_keys(entry, kConfigKeys, "recording context");
    return parse_fields(entry);
}

RecordingContext RecordingContext::ParseEmitted(const json& emitted)
{
    exact_keys(emitted, kEmittedKeys, "emitted recording context");
    require(emitted.at("schema_id").is_string() && emitted.at("schema_id") == kSchemaId,
            std::string("emitted recording context schema_id must be ") + kSchemaId);
    require(emitted.at("schema_version").is_number_integer() && !emitted.at("schema_version").is_boolean() &&
            emitted.at("schema_version") == kSchemaVersion,
            "emitted recording context schema_version must be 1");
    return parse_fields(emitted);
}

json RecordingContext::ToJson() const
{
    return {{"recording_type", recording_type}, {"recording_subtype", recording_subtype},
            {"behavior_mode", behavior_mode}, {"recording_intent", recording_intent}, {"data_origin", data_origin}};
}

json RecordingContext::ToEmittedJson() const
{
    json j = ToJson();
    j["schema_id"] = kSchemaId;
    j["schema_version"] = kSchemaVersion;
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
