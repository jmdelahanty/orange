#include "session/recording_observation_identity.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using orange::session::RecordingObservationEdgeIdentity;

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

nlohmann::json build(const std::string& recording_id,
                     const std::string& camera_id,
                     const std::string& source_camera_stream_id,
                     const std::string& arena_id)
{
    RecordingObservationEdgeIdentity identity;
    identity.recording_id = recording_id;
    identity.camera_id = camera_id;
    identity.source_camera_stream_id = source_camera_stream_id;
    identity.rig_id = "omnifin0";
    identity.canvas_name = "shadow";
    identity.arena_id = arena_id;

    nlohmann::json record;
    std::string error;
    require(orange::session::build_recording_observation_identity(
                identity, &record, &error),
            "identity should build: " + error);
    return record;
}

void current_shadow_topology_is_valid()
{
    std::vector<nlohmann::json> records;
    for (int index = 0; index < 4; ++index) {
        const std::string camera = "201009" + std::to_string(3 + index);
        records.push_back(build(
            "recording_shadow", camera, camera,
            "arena_" + std::to_string(1 + index)));
    }
    std::string error;
    require(orange::session::validate_recording_observation_identity_set(
                records, &error),
            "current one-to-one Shadow topology should validate: " + error);
}

void many_to_many_topology_is_valid()
{
    const std::vector<nlohmann::json> records = {
        build("recording_many", "cam1", "cam1", "arena_1"),
        build("recording_many", "cam1", "cam1", "arena_2"),
        build("recording_many", "cam2", "cam2", "arena_1"),
    };
    std::string error;
    require(orange::session::validate_recording_observation_identity_set(
                records, &error),
            "many-to-many observation edges should validate: " + error);
}

void arena_label_is_qualified_by_rig_and_canvas()
{
    nlohmann::json shadow = build(
        "recording_qualified", "cam1", "cam1", "arena_1");
    RecordingObservationEdgeIdentity identity;
    identity.recording_id = "recording_qualified";
    identity.camera_id = "cam2";
    identity.source_camera_stream_id = "cam2";
    identity.rig_id = "omnifin0";
    identity.canvas_name = "vmsr2";
    identity.arena_id = "arena_1";
    nlohmann::json vmsr2;
    std::string error;
    require(orange::session::build_recording_observation_identity(
                identity, &vmsr2, &error),
            "qualified arena identity should build: " + error);
    require(shadow.at("observation_context_id") !=
                vmsr2.at("observation_context_id"),
            "same arena label on distinct canvases collided");
}

void duplicate_edge_fails_closed()
{
    const nlohmann::json edge = build(
        "recording_duplicate", "cam1", "cam1", "arena_1");
    std::string error;
    require(!orange::session::validate_recording_observation_identity_set(
                {edge, edge}, &error),
            "duplicate observation edge should fail");
    require(error.find("duplicate") != std::string::npos,
            "duplicate failure should be explicit");
}

void current_producer_rejects_multiple_arenas_per_stream()
{
    const std::vector<nlohmann::json> records = {
        build("recording_topology", "cam1", "cam1", "arena_1"),
        build("recording_topology", "cam1", "cam1", "arena_2"),
    };
    std::string error;
    require(!orange::session::validate_current_recording_observation_topology(
                records, &error),
            "current producer should reject one stream mapped to two arenas");
}

void current_producer_allows_multiple_streams_per_arena()
{
    const std::vector<nlohmann::json> records = {
        build("recording_topology", "cam1", "cam1", "arena_1"),
        build("recording_topology", "cam2", "cam2", "arena_1"),
    };
    std::string error;
    require(orange::session::validate_current_recording_observation_topology(
                records, &error),
            "current producer may map several streams to one arena: " + error);
}

void serial_stream_policy_rejects_an_independent_stream_alias()
{
    RecordingObservationEdgeIdentity identity;
    identity.recording_id = "recording_stream_policy";
    identity.camera_id = "2010095";
    identity.source_camera_stream_id = "camera_3_source_a";
    identity.rig_id = "omnifin0";
    identity.canvas_name = "shadow";
    identity.arena_id = "arena_3";

    nlohmann::json record;
    std::string error;
    require(!orange::session::build_recording_observation_identity(
                identity, &record, &error),
            "serial source-stream policy accepted a distinct stream alias");
}

void tampering_fails_digest_validation()
{
    nlohmann::json record = build(
        "recording_tamper", "cam1", "cam1", "arena_1");
    record["identity"]["arena"]["arena_id"] = "arena_2";

    RecordingObservationEdgeIdentity parsed;
    std::string error;
    require(!orange::session::parse_recording_observation_identity(
                record, &parsed, &error),
            "tampered identity should fail");
    require(error.find("digest") != std::string::npos,
            "tampering should fail at the digest boundary");
}

void clip_identity_is_not_part_of_the_edge()
{
    const nlohmann::json first = build(
        "recording_rolling", "cam1", "cam1", "arena_1");
    const nlohmann::json second = build(
        "recording_rolling", "cam1", "cam1", "arena_1");
    require(first == second,
            "rebuilding an edge for another clip should reuse the same context ID");
}

void mixed_recordings_fail_closed()
{
    const std::vector<nlohmann::json> records = {
        build("recording_1", "cam1", "cam1", "arena_1"),
        build("recording_2", "cam2", "cam2", "arena_2"),
    };
    std::string error;
    require(!orange::session::validate_recording_observation_identity_set(
                records, &error),
            "one context collection must not span recordings");
}

}  // namespace

int main()
{
    try {
        current_shadow_topology_is_valid();
        many_to_many_topology_is_valid();
        arena_label_is_qualified_by_rig_and_canvas();
        duplicate_edge_fails_closed();
        current_producer_rejects_multiple_arenas_per_stream();
        current_producer_allows_multiple_streams_per_arena();
        serial_stream_policy_rejects_an_independent_stream_alias();
        tampering_fails_digest_validation();
        clip_identity_is_not_part_of_the_edge();
        mixed_recordings_fail_closed();
        std::cout << "recording_observation_identity_tests: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "recording_observation_identity_tests: FAIL: "
                  << error.what() << '\n';
        return 1;
    }
}
