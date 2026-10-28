#include "rig_config_pin.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

#include "outputs/common/digest.h"

namespace orange::recording {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

// rig_events.CORRECTABLE: what a correction may replace.
const char* const kCorrectable[] = {
    "at", "at_precision", "not_before", "not_after", "old", "new", "method", "baseline", "arenas", "layer",
    "condition", "extent", "locations", "effect", "remedy", "evidence_files"};

json plain_scalar(const std::string& text)
{
    // PyYAML (YAML 1.1) safe_load resolution for the plain scalars rig documents use.
    static const std::regex kNull("^(~|null|Null|NULL|)$");
    static const std::regex kTrue("^(true|True|TRUE|yes|Yes|YES|on|On|ON)$");
    static const std::regex kFalse("^(false|False|FALSE|no|No|NO|off|Off|OFF)$");
    static const std::regex kInt("^[-+]?(0|[1-9][0-9_]*)$");
    static const std::regex kFloat("^[-+]?([0-9][0-9_]*)?\\.[0-9_]*([eE][-+][0-9]+)?$");
    if (std::regex_match(text, kNull)) return nullptr;
    if (std::regex_match(text, kTrue)) return true;
    if (std::regex_match(text, kFalse)) return false;
    std::string digits = text;
    digits.erase(std::remove(digits.begin(), digits.end(), '_'), digits.end());
    try {
        if (std::regex_match(text, kInt)) return std::stoll(digits);
        if (std::regex_match(text, kFloat) && digits != "." && digits != "+." && digits != "-.") {
            return std::stod(digits);
        }
    } catch (const std::exception&) {
        // Out of range: keep the text.
    }
    return text;
}

json node_to_json(const YAML::Node& node)
{
    switch (node.Type()) {
    case YAML::NodeType::Null:
    case YAML::NodeType::Undefined:
        return nullptr;
    case YAML::NodeType::Scalar:
        // "?" is a plain scalar; quoted and block scalars ("!") stay strings.
        return node.Tag() == "?" ? plain_scalar(node.Scalar()) : json(node.Scalar());
    case YAML::NodeType::Sequence: {
        json out = json::array();
        for (const auto& item : node) out.push_back(node_to_json(item));
        return out;
    }
    case YAML::NodeType::Map: {
        json out = json::object();
        for (const auto& item : node) out[item.first.as<std::string>()] = node_to_json(item.second);
        return out;
    }
    }
    return nullptr;
}

// Python's str() of a ledger value, for rig_events.starts().
std::string python_str(const json& value)
{
    if (value.is_string()) return value.get<std::string>();
    if (value.is_null()) return "None";
    if (value.is_boolean()) return value.get<bool>() ? "True" : "False";
    return value.dump();
}

// Python dict key for an event id (event ids are strings; anything else keeps its JSON text).
std::string key_of(const json& value)
{
    return value.is_string() ? "s:" + value.get<std::string>() : "j:" + value.dump();
}

struct PositionedEvent {
    std::string key;
    std::size_t position = 0;
    json event;
};

// rig_events.effective: every non-correction event, corrections applied, voided ones dropped.
// Insertion order follows Python dict semantics (reassignment keeps the slot).
std::vector<PositionedEvent> effective(const json& events)
{
    std::vector<PositionedEvent> out;
    auto find = [&out](const std::string& key) {
        return std::find_if(out.begin(), out.end(), [&key](const PositionedEvent& e) { return e.key == key; });
    };
    if (!events.is_array()) return out;
    std::size_t position = 0;
    for (const json& ev : events) {
        if (!ev.is_object()) continue;  // equipment_events._dicts
        const std::size_t i = position++;
        if (ev.value("kind", json()) != "correction") {
            const std::string key = key_of(ev.value("event_id", json()));
            auto it = find(key);
            if (it == out.end()) {
                out.push_back({key, i, ev});
            } else {
                it->position = i;
                it->event = ev;
            }
            continue;
        }
        const std::string key = key_of(ev.value("corrects", json()));
        auto it = find(key);
        if (it == out.end()) continue;
        if (ev.contains("void") && ev["void"].is_boolean() && ev["void"].get<bool>()) {
            out.erase(it);
            continue;
        }
        json fixed = it->event;
        for (const char* field : kCorrectable) {
            if (ev.contains(field)) fixed[field] = ev[field];
        }
        if (fixed.value("at_precision", json()) == "window") {
            fixed.erase("at");
        } else {
            fixed.erase("not_before");
            fixed.erase("not_after");
        }
        it->event = std::move(fixed);
    }
    return out;
}

std::string starts(const json& ev)
{
    if (ev.value("at_precision", json()) == "window") return python_str(ev.value("not_after", json()));
    return python_str(ev.value("at", json()));
}

std::vector<json> timeline(const json& events)
{
    std::vector<PositionedEvent> list = effective(events);
    std::stable_sort(list.begin(), list.end(), [](const PositionedEvent& a, const PositionedEvent& b) {
        const std::string sa = starts(a.event);
        const std::string sb = starts(b.event);
        return sa != sb ? sa < sb : a.position < b.position;
    });
    std::vector<json> out;
    out.reserve(list.size());
    for (auto& item : list) out.push_back(std::move(item.event));
    return out;
}

bool read_bytes(const fs::path& path, std::string* bytes)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (in.bad()) return false;
    *bytes = buffer.str();
    return true;
}

// The installer's sidecar: one "key: value" per line.
json read_sidecar(const fs::path& path)
{
    std::string bytes;
    if (!fs::exists(path)) return {{"path", path.string()}, {"status", "missing"}};
    if (!read_bytes(path, &bytes)) return {{"path", path.string()}, {"status", "unreadable"}};
    json fields = json::object();
    std::istringstream lines(bytes);
    std::string line;
    while (std::getline(lines, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        auto trim = [](std::string text) {
            const auto first = text.find_first_not_of(" \t\r");
            const auto last = text.find_last_not_of(" \t\r");
            return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
        };
        fields[trim(line.substr(0, colon))] = trim(line.substr(colon + 1));
    }
    return {{"path", path.string()}, {"status", "read"}, {"fields", fields}};
}

fs::path events_path_for(const fs::path& rig_file)
{
    // Repository layout rigs/<rig_id>/rig.yml + events.yml; installed layout <rig_id>.yml + <rig_id>.events.yml.
    if (rig_file.filename() == "rig.yml") return rig_file.parent_path() / "events.yml";
    fs::path events = rig_file;
    events.replace_extension(".events.yml");
    return events;
}

json file_entry(const fs::path& path, const std::string& bytes)
{
    return {{"path", path.string()},
            {"sha256", orange::outputs::sha256_hex(bytes)},
            {"size_bytes", static_cast<std::uint64_t>(bytes.size())}};
}

}  // namespace

json RigYamlToJson(const std::string& text)
{
    try {
        return node_to_json(YAML::Load(text));
    } catch (const YAML::Exception& error) {
        throw std::runtime_error(std::string("YAML parse error: ") + error.what());
    }
}

json RigEventsActiveConditions(const json& events)
{
    // rig_events.active_conditions (no arena filter): open conditions in first-observed order.
    std::vector<std::pair<std::string, json>> open;
    for (const json& ev : timeline(events)) {
        const json kind = ev.value("kind", json());
        const std::string key = key_of(ev.value("condition_id", json()));
        auto it = std::find_if(open.begin(), open.end(), [&key](const auto& entry) { return entry.first == key; });
        if (kind == "condition_observed") {
            if (it == open.end()) open.emplace_back(key, ev);
            else it->second = ev;
        } else if (kind == "condition_cleared" && it != open.end()) {
            open.erase(it);
        }
    }
    json out = json::array();
    for (auto& entry : open) out.push_back(std::move(entry.second));
    return out;
}

json RigEventsAttestations(const json& events)
{
    json out = json::array();
    for (const json& ev : timeline(events)) {
        if (ev.value("kind", json()) != "attested") continue;
        out.push_back({
            {"event_id", ev.value("event_id", json())},
            {"fields", ev.value("fields", json())},
            {"rig_geometry_revision", ev.value("rig_geometry_revision", json())},
            {"orientation_sha256", ev.value("orientation_sha256", json())},
            {"scene_id", ev.value("scene_id", json())},
        });
    }
    return out;
}

std::string RecordingRigIdFromGeometryContract(const json& geometry_contract)
{
    if (!geometry_contract.is_object()) return {};
    const json selection = geometry_contract.value("selection", json::object());
    if (!selection.is_object()) return {};
    const json rig_id = selection.value("rig_id", json());
    return rig_id.is_string() ? rig_id.get<std::string>() : std::string();
}

json BuildRigConfigPin(const std::string& recording_rig_id,
                       const std::string& env_rig_config,
                       const std::string& installed_dir,
                       const std::string& captured_at_utc)
{
    json pin = {
        {"schema_id", kRigConfigPinSchemaId},
        {"schema_version", kRigConfigPinSchemaVersion},
        {"captured_at_utc", captured_at_utc},
        {"status", "pinned"},
        {"resolution", {
            {"source", nullptr},
            {"environment_variable", kRigConfigEnvironmentVariable},
            {"recording_rig_id", recording_rig_id.empty() ? json() : json(recording_rig_id)},
        }},
        {"rig_id", nullptr},
        {"rig_geometry_revision", nullptr},
        {"rig_file", nullptr},
        {"events_file", nullptr},
        {"source_sidecar", nullptr},
        {"active_conditions", json::array()},
        {"attestations", json::array()},
        {"warnings", json::array()},
    };
    auto fail = [&pin](const char* status, const std::string& warning) {
        pin["status"] = status;
        pin["warnings"].push_back(warning);
        return pin;
    };

    fs::path rig_path;
    if (!env_rig_config.empty()) {
        rig_path = env_rig_config;
        pin["resolution"]["source"] = "environment";
    } else if (!recording_rig_id.empty()) {
        rig_path = fs::path(installed_dir) / (recording_rig_id + ".yml");
        pin["resolution"]["source"] = "installed";
    } else {
        return fail("rig_id_unknown",
                    "no $PANCAKEBATTER_RIG_CONFIG and no Citrus rig id for this recording; rig documents not pinned");
    }

    if (!fs::exists(rig_path)) {
        return fail("not_installed", "rig file " + rig_path.string() + " does not exist; rig documents not pinned");
    }
    std::string rig_bytes;
    if (!read_bytes(rig_path, &rig_bytes)) {
        return fail("unreadable", "rig file " + rig_path.string() + " cannot be read");
    }
    pin["rig_file"] = file_entry(rig_path, rig_bytes);
    json rig_doc;
    try {
        rig_doc = RigYamlToJson(rig_bytes);
    } catch (const std::exception& error) {
        return fail("invalid", "rig file " + rig_path.string() + ": " + error.what());
    }
    if (!rig_doc.is_object() || !rig_doc.value("rig_id", json()).is_string() ||
        !rig_doc.value("rig_geometry_revision", json()).is_number_integer()) {
        return fail("invalid", "rig file " + rig_path.string() + " lacks rig_id or an integer rig_geometry_revision");
    }
    const std::string rig_id = rig_doc["rig_id"].get<std::string>();
    pin["rig_id"] = rig_id;
    pin["rig_geometry_revision"] = rig_doc["rig_geometry_revision"];
    const bool rig_id_mismatch = !recording_rig_id.empty() && rig_id != recording_rig_id;

    const fs::path sidecar_path = fs::path(rig_path.string() + ".source");
    const json sidecar = read_sidecar(sidecar_path);
    const fs::path events_path = events_path_for(rig_path);

    std::string events_bytes;
    json events_entry = {{"path", events_path.string()}, {"status", "pinned"}, {"sha256", nullptr},
                         {"size_bytes", nullptr}};
    const char* events_problem = nullptr;
    json events_doc;
    if (!fs::exists(events_path)) {
        events_entry["status"] = "missing";
        events_problem = "events_missing";
    } else if (!read_bytes(events_path, &events_bytes)) {
        events_entry["status"] = "unreadable";
        events_problem = "unreadable";
    } else {
        const json entry = file_entry(events_path, events_bytes);
        events_entry["sha256"] = entry["sha256"];
        events_entry["size_bytes"] = entry["size_bytes"];
        try {
            events_doc = RigYamlToJson(events_bytes);
        } catch (const std::exception& error) {
            events_doc = nullptr;
            pin["warnings"].push_back("events ledger " + events_path.string() + ": " + error.what());
        }
        if (!events_doc.is_object() || !events_doc.value("events", json()).is_array() ||
            events_doc.value("rig", json()) != rig_id) {
            events_entry["status"] = "invalid";
            events_problem = "invalid";
            if (events_doc.is_object()) {
                pin["warnings"].push_back("events ledger " + events_path.string() +
                                          " has no events list or names another rig");
            }
        }
    }
    pin["events_file"] = events_entry;

    json sidecar_out = {{"path", sidecar.value("path", "")}, {"status", sidecar.value("status", "missing")},
                        {"commit", nullptr}, {"installed", nullptr}, {"source", nullptr},
                        {"events_source", nullptr}, {"rig_sha256_matches", nullptr},
                        {"events_sha256_matches", nullptr}};
    bool sidecar_mismatch = false;
    if (sidecar.value("status", "") == "read") {
        const json fields = sidecar.value("fields", json::object());
        for (const char* key : {"commit", "installed", "source", "events_source"}) {
            if (fields.contains(key)) sidecar_out[key] = fields[key];
        }
        if (fields.contains("sha256")) {
            sidecar_out["rig_sha256_matches"] = fields["sha256"] == pin["rig_file"]["sha256"];
            sidecar_mismatch = sidecar_mismatch || !sidecar_out["rig_sha256_matches"].get<bool>();
        }
        if (fields.contains("events_sha256") && events_entry["sha256"].is_string()) {
            sidecar_out["events_sha256_matches"] = fields["events_sha256"] == events_entry["sha256"];
            sidecar_mismatch = sidecar_mismatch || !sidecar_out["events_sha256_matches"].get<bool>();
        }
    } else if (pin["resolution"]["source"] == "installed") {
        pin["warnings"].push_back("installed rig file has no readable .source sidecar: commit unknown");
    }
    pin["source_sidecar"] = sidecar_out;

    if (events_problem == nullptr) {
        const json& events = events_doc["events"];
        pin["active_conditions"] = RigEventsActiveConditions(events);
        json attestations = RigEventsAttestations(events);
        for (json& attestation : attestations) {
            attestation["attestation_revision_matches"] =
                attestation["rig_geometry_revision"].is_number_integer()
                    ? json(attestation["rig_geometry_revision"] == pin["rig_geometry_revision"])
                    : json();
        }
        pin["attestations"] = attestations;
    }

    if (rig_id_mismatch) {
        return fail("rig_id_mismatch", "rig file names rig " + rig_id + " but this recording's Citrus rig is " +
                                           recording_rig_id);
    }
    if (events_problem != nullptr) {
        return fail(events_problem, "events ledger " + events_path.string() + " not pinned (" +
                                        events_entry["status"].get<std::string>() + ")");
    }
    if (sidecar_mismatch) {
        return fail("sidecar_mismatch", "a pinned file's sha256 differs from " + sidecar_path.string() +
                                            "; the installed copy changed after install");
    }
    return pin;
}

json BuildRigConfigPinForRecording(const std::string& recording_rig_id, const std::string& captured_at_utc)
{
    const char* env = std::getenv(kRigConfigEnvironmentVariable);
    return BuildRigConfigPin(recording_rig_id, env != nullptr ? env : "", kInstalledRigConfigDir, captured_at_utc);
}

}  // namespace orange::recording
