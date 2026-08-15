#include "session/recording_observation_prearm.h"

#include "fsuid_guard.h"
#include "gui/spatial_layout/sha256.h"
#include "session/recording_observation_binding.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

namespace orange::session {
namespace {

using json = nlohmann::json;
namespace checksum = orange::gui::spatial_layout::checksum;

constexpr const char* kAcceptanceDirectory =
    "recording_observation_bindings/acceptances";
constexpr const char* kDecisionRelativePath =
    "recording_observation_bindings/pre_arm_decision.json";
constexpr const char* kBatchResultSchemaId =
    "citrus.recording_observation_binding_batch_result";
constexpr std::size_t kMaximumPayloadBytes = 4u * 1024u * 1024u;

bool fail(std::string* error_out, const std::string& message)
{
    if (error_out) {
        *error_out = message;
    }
    return false;
}

std::string sha256_bytes(const std::string& bytes)
{
    return "sha256:" + checksum::sha256_hex(bytes);
}

bool read_regular_file(const std::filesystem::path& path,
                       std::string* bytes_out,
                       std::string* error_out)
{
    std::error_code status_error;
    if (!bytes_out || !std::filesystem::is_regular_file(path, status_error) ||
        status_error) {
        return fail(error_out, "required pre-arm artifact is unavailable: " +
                                   path.string());
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return fail(error_out, "failed to open pre-arm artifact: " + path.string());
    }
    *bytes_out = std::string(std::istreambuf_iterator<char>(input),
                             std::istreambuf_iterator<char>());
    return (!bytes_out->empty() && (input.good() || input.eof())) ||
        fail(error_out, "failed to read pre-arm artifact: " + path.string());
}

bool read_only(const std::filesystem::path& path)
{
    std::error_code error;
    const auto permissions = std::filesystem::status(path, error).permissions();
    return !error &&
        (permissions & (std::filesystem::perms::owner_write |
                        std::filesystem::perms::group_write |
                        std::filesystem::perms::others_write)) ==
            std::filesystem::perms::none;
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
        if (component == "..") {
            return false;
        }
    }
    return path.lexically_normal().generic_string() == value;
}

bool read_recording_id(const std::filesystem::path& folder,
                       std::string* recording_id_out,
                       std::string* error_out)
{
    if (!recording_id_out) {
        return fail(error_out, "recording ID output is required");
    }
    const std::filesystem::path snapshot_path =
        folder / "recording_snapshot_start.json";
    std::string bytes;
    if (!read_regular_file(snapshot_path, &bytes, error_out) ||
        !read_only(snapshot_path)) {
        return fail(error_out,
                    "immutable recording-start snapshot is invalid or writable");
    }
    const json snapshot = json::parse(bytes, nullptr, false);
    if (snapshot.is_discarded() || !snapshot.is_object()) {
        return fail(error_out,
                    "immutable recording-start snapshot is not a JSON object");
    }
    *recording_id_out = snapshot.value("recording_id", "");
    return !recording_id_out->empty() ||
        fail(error_out,
             "immutable recording-start snapshot has no recording_id");
}

bool write_all(const int descriptor,
               const std::string& bytes,
               std::string* error_out)
{
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = ::write(
            descriptor, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return fail(error_out, std::string("pre-arm artifact write failed: ") +
                                       std::strerror(errno));
        }
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

bool create_read_only_file_once(const std::filesystem::path& destination,
                                const std::string& bytes,
                                std::string* error_out)
{
    const std::filesystem::path temporary =
        destination.string() + ".tmp." + std::to_string(::getpid()) + "." +
        std::to_string(static_cast<long long>(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    const int descriptor = ::open(
        temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
        S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (descriptor < 0) {
        return fail(error_out, std::string("failed to create pre-arm artifact: ") +
                                   std::strerror(errno));
    }
    bool ok = write_all(descriptor, bytes, error_out);
    if (ok && ::fsync(descriptor) != 0) {
        ok = fail(error_out, std::string("failed to fsync pre-arm artifact: ") +
                                 std::strerror(errno));
    }
    if (ok && ::fchmod(descriptor, S_IRUSR | S_IRGRP | S_IROTH) != 0) {
        ok = fail(error_out, "failed to make pre-arm artifact read-only");
    }
    if (::close(descriptor) != 0 && ok) {
        ok = fail(error_out, "failed to close pre-arm artifact");
    }
    if (!ok) {
        (void)::unlink(temporary.c_str());
        return false;
    }
    if (::link(temporary.c_str(), destination.c_str()) != 0) {
        const int link_error = errno;
        (void)::unlink(temporary.c_str());
        if (link_error != EEXIST) {
            return fail(error_out, std::string("failed to publish pre-arm artifact: ") +
                                       std::strerror(link_error));
        }
        std::string existing;
        if (!read_regular_file(destination, &existing, error_out) ||
            existing != bytes || !read_only(destination)) {
            return fail(error_out,
                        "create-once pre-arm artifact differs or is writable");
        }
        return true;
    }
    (void)::unlink(temporary.c_str());
    return true;
}

std::string socket_path()
{
    for (const char* key : {
             "ORANGE_CITRUS_OBSERVATION_BINDING_SOCKET",
             "CITRUS_GUI_LOCAL_CONTROL_SOCKET",
             "ORANGE_CITRUS_PROJECTION_SNAPSHOT_SOCKET"}) {
        const char* value = std::getenv(key);
        if (value && value[0] != '\0') {
            return value;
        }
    }
    return "/tmp/citrus_local_control.sock";
}

int timeout_ms()
{
    const char* value =
        std::getenv("ORANGE_CITRUS_OBSERVATION_BINDING_TIMEOUT_MS");
    if (!value || value[0] == '\0') {
        return 1500;
    }
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    return end == value
        ? 1500
        : static_cast<int>(std::clamp<long>(parsed, 50, 5000));
}

bool send_all(const int fd, const std::string& payload, std::string* error_out)
{
    std::size_t offset = 0;
    while (offset < payload.size()) {
        const ssize_t count = ::send(
            fd, payload.data() + offset, payload.size() - offset, MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return fail(error_out, std::string("Citrus binding send failed: ") +
                                       std::strerror(errno));
        }
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

bool default_transport(const json& request,
                       json* response_out,
                       std::string* error_out)
{
    const std::string payload = request.dump() + "\n";
    if (payload.size() > kMaximumPayloadBytes) {
        return fail(error_out, "Citrus binding request exceeds 4 MiB");
    }
    const std::string endpoint = socket_path();
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return fail(error_out, std::string("Citrus binding socket failed: ") +
                                   std::strerror(errno));
    }
    timeval timeout{};
    const int milliseconds = timeout_ms();
    timeout.tv_sec = milliseconds / 1000;
    timeout.tv_usec = (milliseconds % 1000) * 1000;
    (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (endpoint.size() >= sizeof(address.sun_path)) {
        ::close(fd);
        return fail(error_out, "Citrus binding socket path is too long");
    }
    std::strncpy(address.sun_path, endpoint.c_str(), sizeof(address.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        const std::string message = std::string("Citrus binding connect failed: ") +
            std::strerror(errno);
        ::close(fd);
        return fail(error_out, message);
    }
    if (!send_all(fd, payload, error_out)) {
        ::close(fd);
        return false;
    }
    (void)::shutdown(fd, SHUT_WR);
    std::string response_bytes;
    std::array<char, 4096> buffer{};
    while (response_bytes.size() < kMaximumPayloadBytes) {
        const ssize_t count = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            const std::string message =
                std::string("Citrus binding receive failed: ") +
                std::strerror(errno);
            ::close(fd);
            return fail(error_out, message);
        }
        if (count == 0) {
            break;
        }
        response_bytes.append(buffer.data(), static_cast<std::size_t>(count));
        if (response_bytes.find('\n') != std::string::npos) {
            break;
        }
    }
    ::close(fd);
    if (response_bytes.empty() || response_bytes.size() >= kMaximumPayloadBytes) {
        return fail(error_out, "Citrus binding response is empty or exceeds 4 MiB");
    }
    *response_out = json::parse(response_bytes, nullptr, false);
    return (!response_out->is_discarded() && response_out->is_object()) ||
        fail(error_out, "Citrus binding response is not a JSON object");
}

bool parse_existing_decision(
    const std::filesystem::path& folder,
    const std::string& expected_recording_id,
    const std::string& expected_binding_mode,
    const RecordingObservationBindingRequestMaterialization& requests,
    RecordingObservationPreArmResult* result_out,
    std::string* error_out)
{
    const std::filesystem::path path = folder / kDecisionRelativePath;
    if (!std::filesystem::exists(path)) {
        return false;
    }
    std::string bytes;
    if (!read_regular_file(path, &bytes, error_out) || !read_only(path)) {
        return fail(error_out, "existing pre-arm decision is invalid or writable");
    }
    const json decision = json::parse(bytes, nullptr, false);
    if (decision.is_discarded() || !decision.is_object() ||
        decision.value("schema_id", "") !=
            kRecordingObservationPreArmDecisionSchemaId ||
        decision.value("schema_version", 0) !=
            kRecordingObservationPreArmDecisionSchemaVersion ||
        decision.value("recording_id", "") != expected_recording_id ||
        decision.value("binding_mode", "") != expected_binding_mode ||
        !decision.contains("arm_allowed") ||
        !decision.at("arm_allowed").is_boolean() ||
        !decision.contains("transport_attempted") ||
        !decision.at("transport_attempted").is_boolean() ||
        !decision.contains("acceptances") ||
        !decision.at("acceptances").is_array() ||
        !decision.contains("acceptance_count") ||
        !decision.at("acceptance_count").is_number_unsigned() ||
        decision.at("acceptance_count").get<std::size_t>() !=
            decision.at("acceptances").size() ||
        !decision.contains("request_collection") ||
        decision.at("request_collection") !=
            recording_observation_binding_request_collection_reference(requests)) {
        return fail(error_out, "existing pre-arm decision schema is invalid");
    }
    result_out->binding_mode = decision.value("binding_mode", "");
    result_out->lifecycle_status = decision.value("lifecycle_status", "");
    result_out->reason = decision.value("reason", "");
    result_out->arm_allowed = decision.value("arm_allowed", false);
    result_out->transport_attempted = decision.value("transport_attempted", false);
    result_out->decision_relative_path = kDecisionRelativePath;
    result_out->decision_sha256 = sha256_bytes(bytes);
    result_out->decision_byte_size = bytes.size();
    std::map<std::string,
             const RecordingObservationBindingRequestArtifact*> by_context;
    for (const auto& request : requests.artifacts) {
        by_context.emplace(request.observation_context_id, &request);
    }
    std::set<std::string> seen_contexts;
    bool all_accepted = !decision.at("acceptances").empty();
    bool any_accepted = false;
    for (const auto& row : decision.at("acceptances")) {
        if (!row.is_object()) {
            return fail(error_out,
                        "existing pre-arm acceptance reference is malformed");
        }
        RecordingObservationAcceptanceArtifact artifact;
        artifact.observation_context_id = row.value("observation_context_id", "");
        artifact.acceptance_id = row.value("acceptance_id", "");
        artifact.relative_path = row.value("relative_path", "");
        artifact.sha256 = row.value("sha256", "");
        artifact.byte_size = row.value("byte_size", 0ULL);
        const auto request_it = by_context.find(artifact.observation_context_id);
        if (request_it == by_context.end() ||
            !seen_contexts.insert(artifact.observation_context_id).second ||
            !safe_relative_path(artifact.relative_path) ||
            std::filesystem::path(artifact.relative_path).parent_path()
                    .generic_string() != kAcceptanceDirectory) {
            return fail(error_out,
                        "existing pre-arm acceptance reference is invalid");
        }
        const std::filesystem::path acceptance_path =
            folder / artifact.relative_path;
        std::string acceptance_bytes;
        if (!read_regular_file(
                acceptance_path, &acceptance_bytes, error_out) ||
            !read_only(acceptance_path) ||
            acceptance_bytes.size() != artifact.byte_size ||
            sha256_bytes(acceptance_bytes) != artifact.sha256) {
            return fail(error_out,
                        "existing pre-arm acceptance artifact is invalid");
        }
        artifact.acceptance = json::parse(acceptance_bytes, nullptr, false);
        if (artifact.acceptance.is_discarded() ||
            artifact.acceptance.value("acceptance_id", "") !=
                artifact.acceptance_id ||
            artifact.acceptance.value("contract_sha256", "") !=
                row.value("acceptance_contract_sha256", "") ||
            !validate_recording_observation_binding_acceptance(
                artifact.acceptance, request_it->second->request, error_out)) {
            return fail(error_out,
                        "existing pre-arm acceptance does not match its exact request");
        }
        const bool accepted =
            artifact.acceptance.at("contract").value("status", "") ==
                "accepted";
        all_accepted = all_accepted && accepted;
        any_accepted = any_accepted || accepted;
        result_out->acceptances.push_back(std::move(artifact));
    }
    const bool lifecycle_consistent =
        (result_out->lifecycle_status == "accepted_pending_finalization" &&
         all_accepted && seen_contexts.size() == requests.artifacts.size() &&
         result_out->arm_allowed &&
         result_out->transport_attempted) ||
        (result_out->lifecycle_status == "not_applicable" &&
         expected_binding_mode == "not_applicable" &&
         result_out->acceptances.empty() && result_out->arm_allowed &&
         !result_out->transport_attempted) ||
        (result_out->lifecycle_status == "unbound" &&
         expected_binding_mode != "not_applicable" &&
         !any_accepted && !result_out->reason.empty() &&
         result_out->arm_allowed == (expected_binding_mode == "optional"));
    if (!lifecycle_consistent) {
        return fail(error_out,
                    "existing pre-arm decision lifecycle is inconsistent");
    }
    return true;
}

}  // namespace

std::string resolve_recording_observation_binding_mode(std::string* error_out)
{
    const char* raw = std::getenv("ORANGE_CITRUS_OBSERVATION_BINDING_MODE");
    const std::string mode = raw && raw[0] != '\0' ? raw : "optional";
    if (mode != "required" && mode != "optional" &&
        mode != "not_applicable") {
        fail(error_out,
             "ORANGE_CITRUS_OBSERVATION_BINDING_MODE must be required, optional, or not_applicable");
        return {};
    }
    return mode;
}

nlohmann::json recording_observation_pre_arm_decision_reference(
    const RecordingObservationPreArmResult& result)
{
    return {
        {"schema_id", "orange.recording.observation_binding_pre_arm_decision_reference"},
        {"schema_version", 1},
        {"binding_mode", result.binding_mode},
        {"lifecycle_status", result.lifecycle_status},
        {"reason", result.reason},
        {"arm_allowed", result.arm_allowed},
        {"relative_path", result.decision_relative_path},
        {"sha256", result.decision_sha256},
        {"byte_size", result.decision_byte_size},
        {"immutability_policy", "create_once_exact_bytes_v1"},
    };
}

bool execute_recording_observation_pre_arm(
    const std::string& recording_folder,
    const RecordingObservationBindingRequestMaterialization& requests,
    const std::string& binding_mode,
    const std::string& decided_at_utc,
    RecordingObservationPreArmResult* result_out,
    std::string* error_out,
    RecordingObservationBindingTransport transport)
{
    if (!result_out || recording_folder.empty()) {
        return fail(error_out, "recording folder and pre-arm result are required");
    }
    if (error_out) {
        error_out->clear();
    }
    *result_out = RecordingObservationPreArmResult{};
    result_out->binding_mode = binding_mode;
    const std::filesystem::path folder =
        std::filesystem::weakly_canonical(recording_folder);
    std::string recording_id;
    if (!read_recording_id(folder, &recording_id, error_out)) {
        return false;
    }
    if (parse_existing_decision(
            folder, recording_id, binding_mode, requests, result_out,
            error_out)) {
        return true;
    }
    if (error_out && !error_out->empty()) {
        return false;
    }

    json acceptance_references = json::array();
    result_out->arm_allowed = true;
    if (binding_mode == "not_applicable") {
        result_out->lifecycle_status = "not_applicable";
        result_out->reason = "orange_recording_only";
    } else if (requests.status != "materialized") {
        result_out->lifecycle_status = "unbound";
        result_out->reason = "unavailable_at_recording_start";
        result_out->arm_allowed = binding_mode == "optional";
    } else {
        json exact_requests = json::array();
        for (const auto& artifact : requests.artifacts) {
            exact_requests.push_back(artifact.request);
        }
        const std::string operation_digest = requests.collection_sha256.size() > 7
            ? requests.collection_sha256.substr(7)
            : checksum::sha256_hex(exact_requests.dump());
        const json local_request = {
            {"schema_id", "citrus.local_control.request"},
            {"schema_version", 1},
            {"method", "accept_recording_observation_bindings"},
            {"request_id", "orange-obsbind-" + operation_digest},
            {"operation_id", "orange-obsbind-" + operation_digest},
            {"source", "orange"},
            {"params", {
                {"binding_requests", std::move(exact_requests)},
                {"request_collection", {
                    {"relative_path", requests.collection_relative_path},
                    {"sha256", requests.collection_sha256},
                }},
            }},
        };
        result_out->transport_attempted = true;
        json response;
        std::string transport_error;
        if (!(transport ? transport(local_request, &response, &transport_error)
                        : default_transport(local_request, &response, &transport_error))) {
            result_out->lifecycle_status = "unbound";
            result_out->reason = "handshake_not_completed";
            result_out->arm_allowed = binding_mode == "optional";
        } else if (!response.value("ok", false) ||
                   !response.value("accepted", false)) {
            result_out->lifecycle_status = "unbound";
            result_out->reason = "handshake_rejected";
            result_out->arm_allowed = binding_mode == "optional";
        } else {
            const json effect = response.value("effect", json::object());
            if (!effect.is_object()) {
                return fail(error_out,
                            "Citrus binding response effect is not an object");
            }
            const json batch = effect.value(
                "recording_observation_binding", json::object());
            if (!batch.is_object()) {
                return fail(error_out,
                            "Citrus binding batch response is not an object");
            }
            const json acceptances = batch.value("acceptances", json::array());
            if (batch.value("schema_id", "") != kBatchResultSchemaId ||
                batch.value("schema_version", 0) != 1 ||
                !acceptances.is_array() ||
                acceptances.size() != requests.artifacts.size() ||
                !batch.contains("acceptance_count") ||
                !batch.at("acceptance_count").is_number_unsigned() ||
                batch.at("acceptance_count").get<std::size_t>() !=
                    acceptances.size() ||
                (batch.value("status", "") != "accepted" &&
                 batch.value("status", "") != "rejected")) {
                return fail(error_out,
                            "Citrus binding batch response shape is invalid");
            }
            std::map<std::string,
                     const RecordingObservationBindingRequestArtifact*> by_context;
            for (const auto& artifact : requests.artifacts) {
                by_context.emplace(artifact.observation_context_id, &artifact);
            }
            bool all_accepted = true;
            std::set<std::string> seen;
            std::set<std::string> acceptance_ids;
            std::set<std::string> session_uuids;
            std::set<std::string> planned_h5_paths;
            const std::string batch_experiment_id =
                batch.value("citrus_experiment_id", "");
            for (const auto& acceptance : acceptances) {
                if (!acceptance.is_object() ||
                    !acceptance.contains("contract") ||
                    !acceptance.at("contract").is_object()) {
                    return fail(error_out, "Citrus returned a malformed acceptance");
                }
                const std::string context_id = acceptance.at("contract").value(
                    "observation_context_id", "");
                const auto request_it = by_context.find(context_id);
                if (request_it == by_context.end() || !seen.insert(context_id).second) {
                    return fail(error_out,
                                "Citrus returned an unknown or duplicate acceptance context");
                }
                if (!validate_recording_observation_binding_acceptance(
                        acceptance, request_it->second->request, error_out)) {
                    return false;
                }
                const json& contract = acceptance.at("contract");
                const bool row_accepted =
                    contract.value("status", "") == "accepted";
                all_accepted = all_accepted && row_accepted;
                if (!acceptance_ids.insert(
                        acceptance.value("acceptance_id", "")).second) {
                    return fail(error_out,
                                "Citrus returned duplicate acceptance identity");
                }
                if (row_accepted &&
                    (batch_experiment_id.empty() ||
                     contract.value("citrus_experiment_id", "") !=
                         batch_experiment_id ||
                     !session_uuids.insert(
                         contract.value("citrus_session_uuid", "")).second ||
                     !planned_h5_paths.insert(
                         contract.value("planned_h5_relative_path", "")).second)) {
                    return fail(error_out,
                                "Citrus accepted batch identities are inconsistent");
                }
            }
            if ((batch.value("status", "") == "accepted") != all_accepted ||
                (!all_accepted &&
                 std::any_of(
                     acceptances.begin(), acceptances.end(),
                     [](const json& acceptance) {
                         return acceptance.at("contract").value("status", "") ==
                             "accepted";
                     }))) {
                return fail(error_out,
                            "Citrus batch status is not atomic or self-consistent");
            }
            // Only publish immutable acceptance files after the complete batch
            // has passed identity, uniqueness, and all-or-none validation. A
            // malformed later row must never strand a valid-looking partial
            // acceptance set in the recording folder.
            orange::ScopedFsuid fsuid_guard;
            (void)fsuid_guard;
            const std::filesystem::path acceptance_dir =
                folder / kAcceptanceDirectory;
            std::error_code create_error;
            std::filesystem::create_directories(acceptance_dir, create_error);
            if (create_error) {
                return fail(error_out, "failed to create acceptance directory: " +
                                           create_error.message());
            }
            for (const auto& acceptance : acceptances) {
                const std::string context_id = acceptance.at("contract").value(
                    "observation_context_id", "");
                const std::string bytes = acceptance.dump(2) + "\n";
                const std::filesystem::path path =
                    acceptance_dir / (context_id + ".json");
                if (!create_read_only_file_once(path, bytes, error_out)) {
                    return false;
                }
                RecordingObservationAcceptanceArtifact artifact;
                artifact.observation_context_id = context_id;
                artifact.acceptance_id = acceptance.value("acceptance_id", "");
                artifact.relative_path = std::filesystem::relative(path, folder)
                    .generic_string();
                artifact.sha256 = sha256_bytes(bytes);
                artifact.byte_size = bytes.size();
                artifact.acceptance = acceptance;
                acceptance_references.push_back({
                    {"observation_context_id", context_id},
                    {"acceptance_id", artifact.acceptance_id},
                    {"acceptance_contract_sha256",
                     acceptance.value("contract_sha256", "")},
                    {"relative_path", artifact.relative_path},
                    {"sha256", artifact.sha256},
                    {"byte_size", artifact.byte_size},
                });
                result_out->acceptances.push_back(std::move(artifact));
            }
            result_out->lifecycle_status = all_accepted
                ? "accepted_pending_finalization"
                : "unbound";
            result_out->reason = all_accepted ? "" : "handshake_rejected";
            result_out->arm_allowed = all_accepted || binding_mode == "optional";
        }
    }

    const json decision = {
        {"schema_id", kRecordingObservationPreArmDecisionSchemaId},
        {"schema_version", kRecordingObservationPreArmDecisionSchemaVersion},
        {"recording_id", recording_id},
        {"decided_at_utc", decided_at_utc},
        {"binding_mode", binding_mode},
        {"lifecycle_status", result_out->lifecycle_status},
        {"reason", result_out->reason},
        {"arm_allowed", result_out->arm_allowed},
        {"transport_attempted", result_out->transport_attempted},
        {"request_collection", recording_observation_binding_request_collection_reference(requests)},
        {"acceptance_count", acceptance_references.size()},
        {"acceptances", std::move(acceptance_references)},
    };
    const std::string decision_bytes = decision.dump(2) + "\n";
    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    std::error_code create_error;
    std::filesystem::create_directories(
        (folder / kDecisionRelativePath).parent_path(), create_error);
    if (create_error || !create_read_only_file_once(
            folder / kDecisionRelativePath, decision_bytes, error_out)) {
        return create_error
            ? fail(error_out, "failed to create pre-arm decision directory: " +
                                  create_error.message())
            : false;
    }
    result_out->decision_relative_path = kDecisionRelativePath;
    result_out->decision_sha256 = sha256_bytes(decision_bytes);
    result_out->decision_byte_size = decision_bytes.size();
    return true;
}

bool prepare_recording_observation_pre_arm(
    const std::string& recording_folder,
    const std::string& binding_mode,
    const std::string& decided_at_utc,
    RecordingObservationBindingRequestMaterialization* requests_out,
    RecordingObservationPreArmResult* result_out,
    std::string* error_out,
    RecordingObservationBindingTransport transport)
{
    if (!requests_out) {
        return fail(error_out, "request materialization result is required");
    }
    *requests_out = RecordingObservationBindingRequestMaterialization{};
    requests_out->binding_mode = binding_mode;
    if (binding_mode == "not_applicable") {
        requests_out->status = "not_applicable";
        requests_out->reason = "orange_recording_only";
    } else if (!materialize_recording_observation_binding_requests(
                   recording_folder, binding_mode, decided_at_utc,
                   requests_out, error_out)) {
        return false;
    }
    return execute_recording_observation_pre_arm(
        recording_folder, *requests_out, binding_mode, decided_at_utc,
        result_out, error_out, std::move(transport));
}

}  // namespace orange::session
