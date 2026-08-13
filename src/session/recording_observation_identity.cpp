#include "session/recording_observation_identity.h"

#include "gui/spatial_layout/sha256.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <tuple>

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

bool validate_identity(const RecordingObservationEdgeIdentity& identity,
                       std::string* error_out)
{
    if (!valid_identifier(identity.recording_id)) {
        return fail(error_out, "recording_id must be a valid non-empty identifier");
    }
    if (!valid_identifier(identity.camera_id)) {
        return fail(error_out, "camera_id must be a valid non-empty identifier");
    }
    if (!valid_identifier(identity.source_camera_stream_id)) {
        return fail(error_out,
                    "source_camera_stream_id must be a valid non-empty identifier");
    }
    if (identity.source_camera_stream_identity_policy !=
        kCameraSerialSourceFrameStreamIdentityPolicy) {
        return fail(error_out,
                    "source_camera_stream_identity_policy is unsupported");
    }
    if (identity.source_camera_stream_id != identity.camera_id) {
        return fail(error_out,
                    "camera-serial source-frame policy requires "
                    "source_camera_stream_id "
                    "to equal camera_id within this recording");
    }
    if (!valid_identifier(identity.rig_id)) {
        return fail(error_out, "rig_id must be a valid non-empty identifier");
    }
    if (!valid_identifier(identity.canvas_name)) {
        return fail(error_out,
                    "canvas_name must be a valid non-empty identifier");
    }
    if (!valid_identifier(identity.arena_id)) {
        return fail(error_out, "arena_id must be a valid non-empty identifier");
    }
    return true;
}

json identity_payload(const RecordingObservationEdgeIdentity& identity)
{
    return {
        {"schema_id", kRecordingObservationIdentitySchemaId},
        {"schema_version", kRecordingObservationIdentitySchemaVersion},
        {"identity_scope", kRecordingObservationIdentityScope},
        {"recording_id", identity.recording_id},
        {"camera", {
            {"camera_id", identity.camera_id},
            {"source_camera_stream_id", identity.source_camera_stream_id},
            {"source_camera_stream_identity_policy",
             identity.source_camera_stream_identity_policy},
        }},
        {"arena", {
            {"rig_id", identity.rig_id},
            {"canvas_name", identity.canvas_name},
            {"arena_id", identity.arena_id},
        }},
    };
}

std::string canonical_sha256(const json& value)
{
    const std::string bytes = value.dump(
        -1, ' ', false, json::error_handler_t::strict);
    return "sha256:" +
        orange::gui::spatial_layout::checksum::sha256_hex(bytes);
}

bool exact_keys(const json& value,
                const std::set<std::string>& expected)
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

}  // namespace

bool build_recording_observation_identity(
    const RecordingObservationEdgeIdentity& identity,
    nlohmann::json* record_out,
    std::string* error_out)
{
    if (record_out == nullptr) {
        return fail(error_out, "recording observation identity output is null");
    }
    if (!validate_identity(identity, error_out)) {
        return false;
    }

    const json payload = identity_payload(identity);
    const std::string digest = canonical_sha256(payload);
    *record_out = {
        {"schema_id", kRecordingObservationIdentitySchemaId},
        {"schema_version", kRecordingObservationIdentitySchemaVersion},
        {"identity_scope", kRecordingObservationIdentityScope},
        {"canonicalization", kRecordingObservationIdentityCanonicalization},
        {"observation_context_id", "obsctx_" + digest.substr(7)},
        {"identity_sha256", digest},
        {"identity", payload},
    };
    return true;
}

bool parse_recording_observation_identity(
    const nlohmann::json& record,
    RecordingObservationEdgeIdentity* identity_out,
    std::string* error_out)
{
    if (identity_out == nullptr) {
        return fail(error_out, "recording observation identity destination is null");
    }
    if (!exact_keys(record, {
            "schema_id", "schema_version", "identity_scope",
            "canonicalization", "observation_context_id", "identity_sha256",
            "identity"})) {
        return fail(error_out,
                    "recording observation identity has unexpected or missing fields");
    }
    if (record.value("schema_id", "") != kRecordingObservationIdentitySchemaId ||
        record.value("schema_version", 0) !=
            kRecordingObservationIdentitySchemaVersion ||
        record.value("identity_scope", "") !=
            kRecordingObservationIdentityScope ||
        record.value("canonicalization", "") !=
            kRecordingObservationIdentityCanonicalization) {
        return fail(error_out,
                    "recording observation identity schema or scope mismatch");
    }

    const json& payload = record.at("identity");
    if (!exact_keys(payload, {
            "schema_id", "schema_version", "identity_scope", "recording_id",
            "camera", "arena"}) ||
        payload.value("schema_id", "") != kRecordingObservationIdentitySchemaId ||
        payload.value("schema_version", 0) !=
            kRecordingObservationIdentitySchemaVersion ||
        payload.value("identity_scope", "") !=
            kRecordingObservationIdentityScope) {
        return fail(error_out, "recording observation identity payload mismatch");
    }

    const json& camera = payload.at("camera");
    if (!exact_keys(camera, {
            "camera_id", "source_camera_stream_id",
            "source_camera_stream_identity_policy"})) {
        return fail(error_out,
                    "recording observation identity camera is invalid");
    }
    const json& arena = payload.at("arena");
    if (!exact_keys(arena, {"rig_id", "canvas_name", "arena_id"})) {
        return fail(error_out,
                    "recording observation identity arena is invalid");
    }

    RecordingObservationEdgeIdentity parsed;
    parsed.recording_id = payload.value("recording_id", "");
    parsed.camera_id = camera.value("camera_id", "");
    parsed.source_camera_stream_id = camera.value(
        "source_camera_stream_id", "");
    parsed.source_camera_stream_identity_policy = camera.value(
        "source_camera_stream_identity_policy", "");
    parsed.rig_id = arena.value("rig_id", "");
    parsed.canvas_name = arena.value("canvas_name", "");
    parsed.arena_id = arena.value("arena_id", "");
    if (!validate_identity(parsed, error_out)) {
        return false;
    }

    const std::string expected_digest = canonical_sha256(identity_payload(parsed));
    if (record.value("identity_sha256", "") != expected_digest) {
        return fail(error_out, "recording observation identity digest mismatch");
    }
    if (record.value("observation_context_id", "") !=
        "obsctx_" + expected_digest.substr(7)) {
        return fail(error_out,
                    "recording observation context ID does not match its identity digest");
    }

    *identity_out = std::move(parsed);
    return true;
}

bool validate_recording_observation_identity_set(
    const std::vector<nlohmann::json>& records,
    std::string* error_out)
{
    if (records.empty()) {
        return fail(error_out, "recording observation identity set is empty");
    }

    std::string recording_id;
    std::set<std::tuple<
        std::string, std::string, std::string, std::string>> edges;
    for (const json& record : records) {
        RecordingObservationEdgeIdentity identity;
        if (!parse_recording_observation_identity(
                record, &identity, error_out)) {
            return false;
        }
        if (recording_id.empty()) {
            recording_id = identity.recording_id;
        } else if (identity.recording_id != recording_id) {
            return fail(error_out,
                        "recording observation identity set spans multiple recordings");
        }

        const auto edge = std::make_tuple(
            identity.source_camera_stream_id, identity.rig_id,
            identity.canvas_name,
            identity.arena_id);
        if (!edges.insert(edge).second) {
            return fail(error_out,
                        "duplicate recording camera-stream/arena observation edge");
        }
    }
    return true;
}

bool validate_current_recording_observation_topology(
    const std::vector<nlohmann::json>& records,
    std::string* error_out)
{
    if (!validate_recording_observation_identity_set(records, error_out)) {
        return false;
    }

    std::map<std::string, std::tuple<std::string, std::string, std::string>>
        arena_by_stream;
    for (const json& record : records) {
        RecordingObservationEdgeIdentity identity;
        if (!parse_recording_observation_identity(
                record, &identity, error_out)) {
            return false;
        }
        const auto arena = std::make_tuple(
            identity.rig_id, identity.canvas_name, identity.arena_id);
        const auto [it, inserted] = arena_by_stream.emplace(
            identity.source_camera_stream_id, arena);
        if (!inserted && it->second != arena) {
            return fail(error_out,
                        "current producer topology maps one source-camera "
                        "frame stream to "
                        "more than one arena");
        }
    }
    return true;
}

}  // namespace orange::session
