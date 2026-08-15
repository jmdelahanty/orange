#include "session/recording_observation_request_artifacts.h"

#include "fsuid_guard.h"
#include "gui/spatial_layout/sha256.h"
#include "session/recording_observation_binding.h"
#include "session/recording_observation_identity.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace orange::session {
namespace {

using json = nlohmann::json;
namespace checksum = orange::gui::spatial_layout::checksum;

constexpr const char* kStartSnapshotFileName = "recording_snapshot_start.json";
constexpr const char* kRequestDirectory =
    "recording_observation_bindings/requests";
constexpr const char* kRequestCollectionRelativePath =
    "recording_observation_bindings/request_collection.json";
constexpr const char* kRequestCollectionSchemaId =
    "orange.recording.observation_binding_request_collection";

bool fail(std::string* error_out, const std::string& message)
{
    if (error_out) {
        *error_out = message;
    }
    return false;
}

std::filesystem::path normalized_absolute(const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::path value = std::filesystem::weakly_canonical(path, error);
    if (!error) {
        return value;
    }
    value = std::filesystem::absolute(path, error);
    return error ? path.lexically_normal() : value.lexically_normal();
}

bool path_is_inside(const std::filesystem::path& child,
                    const std::filesystem::path& parent)
{
    const auto normalized_child = normalized_absolute(child);
    const auto normalized_parent = normalized_absolute(parent);
    auto child_it = normalized_child.begin();
    for (auto parent_it = normalized_parent.begin();
         parent_it != normalized_parent.end(); ++parent_it, ++child_it) {
        if (child_it == normalized_child.end() || *child_it != *parent_it) {
            return false;
        }
    }
    return true;
}

bool safe_relative_path(const std::string& value)
{
    if (value.empty()) {
        return false;
    }
    const std::filesystem::path path(value);
    if (path.is_absolute()) {
        return false;
    }
    for (const auto& component : path) {
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
    }
    return true;
}

bool read_regular_file(const std::filesystem::path& path,
                       std::string* bytes_out,
                       std::string* error_out)
{
    if (!bytes_out) {
        return fail(error_out, "file byte destination is null");
    }
    std::error_code status_error;
    if (!std::filesystem::is_regular_file(path, status_error) || status_error) {
        return fail(error_out, "required regular file is unavailable: " + path.string());
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return fail(error_out, "failed to open required file: " + path.string());
    }
    *bytes_out = std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
    if (!input.good() && !input.eof()) {
        return fail(error_out, "failed to read required file: " + path.string());
    }
    if (bytes_out->empty()) {
        return fail(error_out, "required file is empty: " + path.string());
    }
    return true;
}

bool parse_json_object(const std::string& bytes,
                       const std::string& role,
                       json* value_out,
                       std::string* error_out)
{
    if (!value_out) {
        return fail(error_out, role + " destination is null");
    }
    *value_out = json::parse(bytes, nullptr, false);
    if (value_out->is_discarded() || !value_out->is_object()) {
        return fail(error_out, role + " is not a valid JSON object");
    }
    return true;
}

std::string sha256_bytes(const std::string& bytes)
{
    return "sha256:" + checksum::sha256_hex(bytes);
}

bool valid_sha256(const std::string& value)
{
    return value.size() == 71 && value.rfind("sha256:", 0) == 0 &&
        std::all_of(value.begin() + 7, value.end(), [](const unsigned char ch) {
            return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        });
}

bool valid_read_only_permissions(const std::filesystem::path& path)
{
    std::error_code error;
    const auto permissions = std::filesystem::status(path, error).permissions();
    return !error &&
        (permissions & (std::filesystem::perms::owner_write |
                        std::filesystem::perms::group_write |
                        std::filesystem::perms::others_write)) ==
            std::filesystem::perms::none;
}

bool write_all(const int descriptor,
               const std::string& bytes,
               std::string* error_out)
{
    std::size_t written = 0;
    while (written < bytes.size()) {
        const ssize_t result = ::write(
            descriptor, bytes.data() + written, bytes.size() - written);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return fail(error_out,
                        std::string("request artifact write failed: ") +
                            std::strerror(errno));
        }
        if (result == 0) {
            return fail(error_out, "request artifact write was unexpectedly short");
        }
        written += static_cast<std::size_t>(result);
    }
    return true;
}

bool create_read_only_file_once(const std::filesystem::path& destination,
                                const std::string& bytes,
                                std::string* error_out)
{
    const std::filesystem::path temporary =
        destination.string() + ".tmp." +
        std::to_string(static_cast<long long>(::getpid())) + "." +
        std::to_string(static_cast<long long>(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    const int descriptor = ::open(
        temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
        S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (descriptor < 0) {
        return fail(error_out,
                    std::string("failed to create request artifact temporary file: ") +
                        std::strerror(errno));
    }

    bool ok = write_all(descriptor, bytes, error_out);
    if (ok && ::fsync(descriptor) != 0) {
        ok = fail(error_out,
                  std::string("failed to fsync request artifact: ") +
                      std::strerror(errno));
    }
    if (ok && ::fchmod(descriptor, S_IRUSR | S_IRGRP | S_IROTH) != 0) {
        ok = fail(error_out,
                  std::string("failed to make request artifact read-only: ") +
                      std::strerror(errno));
    }
    if (::close(descriptor) != 0 && ok) {
        ok = fail(error_out,
                  std::string("failed to close request artifact: ") +
                      std::strerror(errno));
    }
    if (!ok) {
        (void)::unlink(temporary.c_str());
        return false;
    }

    if (::link(temporary.c_str(), destination.c_str()) != 0) {
        const int link_error = errno;
        (void)::unlink(temporary.c_str());
        if (link_error != EEXIST) {
            return fail(error_out,
                        std::string("failed to publish request artifact: ") +
                            std::strerror(link_error));
        }
        std::string existing;
        if (!read_regular_file(destination, &existing, error_out)) {
            return false;
        }
        if (existing != bytes) {
            return fail(error_out,
                        "create-once request artifact already has different bytes");
        }
        return valid_read_only_permissions(destination) ||
            fail(error_out, "existing request artifact is writable");
    }
    (void)::unlink(temporary.c_str());
    return true;
}

bool request_matches_edge(const json& request,
                          const json& identity,
                          const json& target,
                          const std::string& binding_mode,
                          const std::string& recording_folder,
                          const std::string& start_sha256,
                          const std::string& geometry_relative_path,
                          const std::string& geometry_sha256,
                          std::string* error_out)
{
    if (!validate_recording_observation_binding_request(request, error_out)) {
        return false;
    }
    const json& contract = request.at("contract");
    const json& recording = contract.at("recording");
    const json& snapshot = recording.at("recording_snapshot");
    const json& geometry = contract.at("recording_geometry_contract");
    if (contract.at("observation_identity") != identity ||
        contract.at("target") != target ||
        contract.value("binding_mode", "") != binding_mode ||
        recording.value("recording_folder", "") != recording_folder ||
        snapshot.value("relative_path", "") != kStartSnapshotFileName ||
        snapshot.value("sha256", "") != start_sha256 ||
        geometry.value("status", "") != "available" ||
        geometry.value("relative_path", "") != geometry_relative_path ||
        geometry.value("sha256", "") != geometry_sha256) {
        return fail(error_out,
                    "existing request artifact does not match sealed recording evidence");
    }
    return true;
}

}  // namespace

nlohmann::json recording_observation_binding_request_collection_reference(
    const RecordingObservationBindingRequestMaterialization& result)
{
    json reference = {
        {"schema_id",
         "orange.recording.observation_binding_request_collection_reference"},
        {"schema_version", 1},
        {"status", result.status},
        {"binding_mode", result.binding_mode},
        {"request_count", result.artifacts.size()},
    };
    if (!result.reason.empty()) {
        reference["reason"] = result.reason;
    }
    if (!result.collection_relative_path.empty()) {
        reference.update({
            {"relative_path", result.collection_relative_path},
            {"sha256", result.collection_sha256},
            {"byte_size", result.collection_byte_size},
            {"immutability_policy", "create_once_exact_bytes_v1"},
        });
    }
    return reference;
}

bool materialize_recording_observation_binding_requests(
    const std::string& recording_folder,
    const std::string& binding_mode,
    const std::string& requested_at_utc,
    RecordingObservationBindingRequestMaterialization* result_out,
    std::string* error_out)
{
    if (result_out) {
        *result_out = RecordingObservationBindingRequestMaterialization{};
        result_out->binding_mode = binding_mode;
    }
    if (error_out) {
        error_out->clear();
    }
    if (!result_out || recording_folder.empty()) {
        return fail(error_out, "recording folder and materialization result are required");
    }
    if (binding_mode != "required" && binding_mode != "optional") {
        return fail(error_out, "binding mode must be required or optional");
    }

    const std::filesystem::path folder = normalized_absolute(recording_folder);
    std::string start_bytes;
    const std::filesystem::path start_path = folder / kStartSnapshotFileName;
    if (!read_regular_file(start_path, &start_bytes, error_out)) {
        return false;
    }
    if (!valid_read_only_permissions(start_path)) {
        return fail(error_out, "immutable recording-start snapshot is writable");
    }
    json start_snapshot;
    if (!parse_json_object(
            start_bytes, "immutable recording-start snapshot", &start_snapshot,
            error_out)) {
        return false;
    }
    const std::string recording_id = start_snapshot.value("recording_id", "");
    if (recording_id.empty()) {
        return fail(error_out, "immutable recording-start snapshot has no recording_id");
    }
    const std::string start_sha256 = sha256_bytes(start_bytes);

    const json geometry_reference = start_snapshot.value(
        "recording_geometry_contract", json::object());
    const std::string geometry_relative_path =
        geometry_reference.value("relative_path", "");
    const std::string geometry_sha256 = geometry_reference.value("sha256", "");
    if (!safe_relative_path(geometry_relative_path) ||
        !valid_sha256(geometry_sha256)) {
        return fail(error_out,
                    "sealed start snapshot has no valid recording geometry reference");
    }
    const std::filesystem::path geometry_path =
        folder / std::filesystem::path(geometry_relative_path);
    if (!path_is_inside(geometry_path, folder)) {
        return fail(error_out,
                    "recording geometry reference resolves outside the recording folder");
    }
    std::string geometry_bytes;
    if (!read_regular_file(geometry_path, &geometry_bytes, error_out)) {
        return false;
    }
    if (sha256_bytes(geometry_bytes) != geometry_sha256) {
        return fail(error_out,
                    "recording geometry bytes do not match sealed start evidence");
    }
    json geometry;
    if (!parse_json_object(
            geometry_bytes, "recording geometry contract", &geometry,
            error_out)) {
        return false;
    }
    if (geometry.value("schema_id", "") !=
            "orange.recording.geometry_contract" ||
        geometry.value("schema_version", 0) != 1) {
        return fail(error_out, "recording geometry contract schema is invalid");
    }

    const json selection = geometry.value("selection", json::object());
    if (!selection.value("configured", false)) {
        result_out->status = "unavailable";
        result_out->reason = "citrus_canvas_not_selected";
        return true;
    }
    const std::string rig_id = selection.value("rig_id", "");
    const std::string canvas_name =
        selection.value("selected_canvas_name", "");
    if (rig_id.empty() || canvas_name.empty()) {
        return fail(error_out,
                    "resolved recording geometry lacks rig or selected canvas identity");
    }
    const json global_geometry_errors = geometry.value("errors", json::array());
    if (global_geometry_errors.is_array() && !global_geometry_errors.empty()) {
        return fail(error_out,
                    "recording-bound Citrus geometry has global validation errors");
    }

    const json cameras = geometry.value("cameras", json::object());
    const json camera_runtime = start_snapshot.value("camera_runtime", json::object());
    const json source_streams = start_snapshot.value(
        "source_camera_streams", json::object());
    if (!cameras.is_object() || cameras.empty() || !camera_runtime.is_object() ||
        !source_streams.is_object() || source_streams.empty()) {
        return fail(error_out,
                    "recording evidence lacks recording-bound source-camera streams");
    }

    struct Edge {
        json identity;
        json target;
    };
    std::vector<Edge> edges;
    std::vector<json> identities;
    bool source_geometry_unavailable = false;
    for (auto stream_it = source_streams.begin();
         stream_it != source_streams.end(); ++stream_it) {
        const std::string camera_id = stream_it.key();
        const json& source_stream = stream_it.value();
        if (!source_stream.is_object() ||
            source_stream.value("schema_id", "") !=
                "orange.recording.source_camera_stream" ||
            source_stream.value("schema_version", 0) != 1 ||
            source_stream.value("camera_id", "") != camera_id ||
            source_stream.value("source_camera_stream_id", "") != camera_id ||
            source_stream.value("source_camera_stream_identity_policy", "") !=
                kCameraSerialSourceFrameStreamIdentityPolicy ||
            source_stream.value("role", "") !=
                "canonical_acquisition_source") {
            return fail(error_out,
                        "sealed source-camera stream identity is invalid");
        }
        if (!cameras.contains(camera_id) ||
            !cameras.at(camera_id).is_object()) {
            source_geometry_unavailable = true;
            continue;
        }
        const json& camera = cameras.at(camera_id);
        if (camera.value("camera_serial", "") != camera_id) {
            return fail(error_out,
                        "resolved geometry contains a mismatched camera entry");
        }
        const json camera_errors = camera.value("errors", json::array());
        if (camera_errors.is_array() && !camera_errors.empty()) {
            return fail(error_out,
                        "recording-bound camera geometry has validation errors");
        }
        if (camera.value("status", "") != "resolved") {
            source_geometry_unavailable = true;
            continue;
        }
        if (!camera_runtime.contains(camera_id) ||
            !camera_runtime.at(camera_id).is_object()) {
            return fail(error_out,
                        "geometry camera is absent from sealed camera runtime evidence");
        }
        const std::string arena_id = camera.value("arena_id", "");
        const json selected_canvas = camera.value("selected_canvas", json::object());
        if (arena_id.empty() ||
            selected_canvas.value("canvas_name", "") != canvas_name) {
            return fail(error_out,
                        "geometry camera arena/canvas identity is contradictory");
        }

        RecordingObservationEdgeIdentity edge_identity;
        edge_identity.recording_id = recording_id;
        edge_identity.camera_id = camera_id;
        edge_identity.source_camera_stream_id = camera_id;
        edge_identity.rig_id = rig_id;
        edge_identity.canvas_name = canvas_name;
        edge_identity.arena_id = arena_id;
        json identity;
        if (!build_recording_observation_identity(
                edge_identity, &identity, error_out)) {
            return false;
        }
        identities.push_back(identity);
        edges.push_back({
            identity,
            {
                {"rig_id", rig_id},
                {"canvas_name", canvas_name},
                {"arena_id", arena_id},
                {"camera_id", camera_id},
                {"source_camera_stream_id", camera_id},
            },
        });
    }
    if (source_geometry_unavailable) {
        result_out->status = "unavailable";
        result_out->reason = "recording_bound_geometry_not_fully_resolved";
        return true;
    }
    if (edges.empty()) {
        result_out->status = "unavailable";
        result_out->reason = "no_recording_bound_camera_arena_edges";
        return true;
    }
    if (!validate_current_recording_observation_topology(identities, error_out)) {
        return false;
    }
    std::sort(edges.begin(), edges.end(), [](const Edge& left, const Edge& right) {
        return left.identity.value("observation_context_id", "") <
            right.identity.value("observation_context_id", "");
    });

    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    const std::filesystem::path request_dir = folder / kRequestDirectory;
    std::error_code create_error;
    std::filesystem::create_directories(request_dir, create_error);
    if (create_error) {
        return fail(error_out,
                    "failed to create observation request directory: " +
                        create_error.message());
    }

    for (const Edge& edge : edges) {
        const std::string context_id =
            edge.identity.value("observation_context_id", "");
        const std::filesystem::path request_path =
            request_dir / (context_id + ".json");
        json request;
        std::string request_bytes;
        if (std::filesystem::exists(request_path)) {
            if (!read_regular_file(request_path, &request_bytes, error_out) ||
                !parse_json_object(
                    request_bytes, "existing observation binding request",
                    &request, error_out) ||
                !valid_read_only_permissions(request_path) ||
                !request_matches_edge(
                    request, edge.identity, edge.target, binding_mode,
                    folder.string(), start_sha256, geometry_relative_path,
                    geometry_sha256, error_out)) {
                if (error_out && error_out->empty()) {
                    *error_out = "existing observation binding request is writable";
                }
                return false;
            }
            if (request_bytes != request.dump(2) + "\n") {
                return fail(
                    error_out,
                    "existing observation binding request is not canonical JSON");
            }
        } else {
            const json contract = {
                {"schema_id", kObservationBindingRequestSchemaId},
                {"schema_version", kObservationBindingSchemaVersion},
                {"observation_context_id", context_id},
                {"observation_identity_sha256",
                 edge.identity.at("identity_sha256")},
                {"observation_identity", edge.identity},
                {"binding_mode", binding_mode},
                {"requested_at_utc", requested_at_utc},
                {"recording", {
                    {"recording_id", recording_id},
                    {"recording_folder", folder.string()},
                    {"recording_snapshot", {
                        {"role", "immutable_recording_start_snapshot"},
                        {"relative_path", kStartSnapshotFileName},
                        {"sha256", start_sha256},
                    }},
                }},
                {"target", edge.target},
                {"recording_geometry_contract", {
                    {"status", "available"},
                    {"relative_path", geometry_relative_path},
                    {"sha256", geometry_sha256},
                }},
            };
            if (!seal_recording_observation_binding_request(
                    contract, &request, error_out)) {
                return false;
            }
            request_bytes = request.dump(2) + "\n";
            if (!create_read_only_file_once(
                    request_path, request_bytes, error_out)) {
                return false;
            }
        }

        RecordingObservationBindingRequestArtifact artifact;
        artifact.observation_context_id = context_id;
        artifact.request_id = request.value("request_id", "");
        artifact.relative_path =
            std::filesystem::relative(request_path, folder).generic_string();
        artifact.sha256 = sha256_bytes(request_bytes);
        artifact.byte_size = static_cast<std::uint64_t>(request_bytes.size());
        artifact.observation_identity = edge.identity;
        artifact.request = std::move(request);
        result_out->artifacts.push_back(std::move(artifact));
    }

    json collection_requests = json::array();
    for (const auto& artifact : result_out->artifacts) {
        collection_requests.push_back({
            {"observation_context_id", artifact.observation_context_id},
            {"observation_identity_sha256",
             artifact.observation_identity.at("identity_sha256")},
            {"observation_identity", artifact.observation_identity},
            {"request_id", artifact.request_id},
            {"request_contract_sha256",
             artifact.request.at("contract_sha256")},
            {"relative_path", artifact.relative_path},
            {"sha256", artifact.sha256},
            {"byte_size", artifact.byte_size},
        });
    }
    const json collection = {
        {"schema_id", kRequestCollectionSchemaId},
        {"schema_version", 1},
        {"status", "materialized"},
        {"binding_mode", binding_mode},
        {"recording_id", recording_id},
        {"recording_snapshot", {
            {"role", "immutable_recording_start_snapshot"},
            {"relative_path", kStartSnapshotFileName},
            {"sha256", start_sha256},
        }},
        {"recording_geometry_contract", {
            {"relative_path", geometry_relative_path},
            {"sha256", geometry_sha256},
        }},
        {"request_count", collection_requests.size()},
        {"requests", std::move(collection_requests)},
    };
    const std::string collection_bytes = collection.dump(2) + "\n";
    const std::filesystem::path collection_path =
        folder / kRequestCollectionRelativePath;
    if (!create_read_only_file_once(
            collection_path, collection_bytes, error_out)) {
        return false;
    }

    result_out->status = "materialized";
    result_out->reason.clear();
    result_out->collection_relative_path = kRequestCollectionRelativePath;
    result_out->collection_sha256 = sha256_bytes(collection_bytes);
    result_out->collection_byte_size =
        static_cast<std::uint64_t>(collection_bytes.size());
    return true;
}

}  // namespace orange::session
