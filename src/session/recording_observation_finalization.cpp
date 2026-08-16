#include "session/recording_observation_finalization.h"

#include "gui/spatial_layout/sha256.h"
#include "fsuid_guard.h"
#include "session/recording_observation_binding.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace orange::session {
namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

constexpr const char* kRequestCollectionRelativePath =
    "recording_observation_bindings/request_collection.json";
constexpr const char* kPreArmRelativePath =
    "recording_observation_bindings/pre_arm_decision.json";
constexpr const char* kReceiptDirectory =
    "recording_observation_bindings/receipts";

bool fail(std::string* error_out, const std::string& message)
{
    if (error_out != nullptr) {
        *error_out = message;
    }
    return false;
}

bool safe_relative_path(const std::string& value)
{
    if (value.empty()) {
        return false;
    }
    const fs::path path(value);
    if (path.is_absolute() || path.lexically_normal() != path) {
        return false;
    }
    for (const auto& component : path) {
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
    }
    return true;
}

bool path_inside(const fs::path& candidate, const fs::path& root)
{
    const fs::path normalized_candidate = candidate.lexically_normal();
    const fs::path normalized_root = root.lexically_normal();
    auto candidate_it = normalized_candidate.begin();
    for (auto root_it = normalized_root.begin(); root_it != normalized_root.end();
         ++root_it, ++candidate_it) {
        if (candidate_it == normalized_candidate.end() ||
            *candidate_it != *root_it) {
            return false;
        }
    }
    return true;
}

bool read_bytes(const fs::path& path,
                std::string* bytes_out,
                std::string* error_out)
{
    if (bytes_out == nullptr) {
        return fail(error_out, "read output is null");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return fail(error_out, "could not open " + path.string());
    }
    bytes_out->assign(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
    if (input.bad()) {
        return fail(error_out, "could not read " + path.string());
    }
    return true;
}

bool read_json(const fs::path& path, json* value_out, std::string* error_out)
{
    if (value_out == nullptr) {
        return fail(error_out, "JSON output is null");
    }
    std::string bytes;
    if (!read_bytes(path, &bytes, error_out)) {
        return false;
    }
    *value_out = json::parse(bytes, nullptr, false);
    return !value_out->is_discarded() ||
        fail(error_out, "invalid JSON in " + path.string());
}

std::string byte_sha256(const std::string& bytes)
{
    return "sha256:" +
        orange::gui::spatial_layout::checksum::sha256_hex(bytes);
}

bool file_sha256(const fs::path& path,
                 std::string* value_out,
                 std::string* error_out)
{
    // The shared helper hashes in bounded chunks. This finalization call also
    // stays outside acquisition and GUI rendering.
    return orange::gui::spatial_layout::checksum::file_sha256(
        path, value_out, error_out);
}

bool write_all(const int fd, const std::string& bytes, std::string* error_out)
{
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t written = ::write(
            fd, bytes.data() + offset, bytes.size() - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return fail(error_out,
                        "write failed: " + std::string(std::strerror(errno)));
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

bool write_create_once_exact(const fs::path& path,
                             const std::string& bytes,
                             std::string* error_out)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        return fail(error_out,
                    "could not create directory for " + path.string() +
                        ": " + ec.message());
    }
    const int fd = ::open(
        path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
        0444);
    if (fd >= 0) {
        bool ok = write_all(fd, bytes, error_out);
        if (ok && ::fsync(fd) != 0) {
            ok = fail(error_out,
                      "fsync failed for " + path.string() + ": " +
                          std::strerror(errno));
        }
        if (::close(fd) != 0 && ok) {
            ok = fail(error_out,
                      "close failed for " + path.string() + ": " +
                          std::strerror(errno));
        }
        if (!ok) {
            fs::remove(path, ec);
        }
        return ok;
    }
    if (errno != EEXIST) {
        return fail(error_out,
                    "could not create " + path.string() + ": " +
                        std::strerror(errno));
    }
    std::string existing;
    return read_bytes(path, &existing, error_out) &&
        (existing == bytes ||
         fail(error_out,
              "immutable artifact already exists with different bytes: " +
                  path.string()));
}

bool write_atomic_replace(const fs::path& path,
                          const std::string& bytes,
                          std::string* error_out)
{
    const fs::path temporary = path.string() + ".observation-finalization.tmp." +
        std::to_string(static_cast<long long>(::getpid()));
    const int fd = ::open(
        temporary.c_str(),
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
        0644);
    if (fd < 0) {
        return fail(error_out,
                    "could not create manifest staging file: " +
                        std::string(std::strerror(errno)));
    }
    bool ok = write_all(fd, bytes, error_out);
    if (ok && ::fsync(fd) != 0) {
        ok = fail(error_out,
                  "manifest staging fsync failed: " +
                      std::string(std::strerror(errno)));
    }
    if (::close(fd) != 0 && ok) {
        ok = fail(error_out,
                  "manifest staging close failed: " +
                      std::string(std::strerror(errno)));
    }
    if (ok && ::rename(temporary.c_str(), path.c_str()) != 0) {
        ok = fail(error_out,
                  "manifest atomic replace failed: " +
                      std::string(std::strerror(errno)));
    }
    if (!ok) {
        std::error_code remove_error;
        fs::remove(temporary, remove_error);
        return false;
    }
    const int directory_fd = ::open(
        path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_fd < 0) {
        return fail(error_out, "could not open recording directory for fsync");
    }
    const bool directory_ok = ::fsync(directory_fd) == 0;
    const int saved_errno = errno;
    ::close(directory_fd);
    return directory_ok ||
        fail(error_out,
             "recording directory fsync failed: " +
                 std::string(std::strerror(saved_errno)));
}

json artifact_reference(const std::string& relative_path,
                        const std::string& bytes)
{
    return {
        {"relative_path", relative_path},
        {"size_bytes", bytes.size()},
        {"sha256", byte_sha256(bytes)},
    };
}

bool load_referenced_json(const fs::path& root,
                          const json& reference,
                          json* value_out,
                          std::string* error_out)
{
    const std::string relative_path = reference.value("relative_path", "");
    if (!safe_relative_path(relative_path)) {
        return fail(error_out, "artifact reference path is invalid");
    }
    const fs::path path = root / relative_path;
    if (!path_inside(path, root)) {
        return fail(error_out, "artifact reference escapes recording folder");
    }
    std::error_code status_error;
    const auto status = fs::symlink_status(path, status_error);
    if (status_error || status.type() != fs::file_type::regular) {
        return fail(error_out,
                    "referenced artifact is missing, a symlink, or not a "
                    "regular file: " + relative_path);
    }
    std::string bytes;
    if (!read_bytes(path, &bytes, error_out) ||
        byte_sha256(bytes) != reference.value("sha256", "") ||
        (reference.contains("byte_size") &&
         reference.value("byte_size", 0ULL) != bytes.size())) {
        return fail(error_out,
                    "artifact reference digest or byte size mismatch: " +
                        relative_path);
    }
    *value_out = json::parse(bytes, nullptr, false);
    return !value_out->is_discarded() ||
        fail(error_out, "referenced artifact is not valid JSON");
}

json unbound_summary(const json& request_collection,
                     const std::string& reason)
{
    json contexts = json::array();
    for (const auto& request : request_collection.value("requests", json::array())) {
        contexts.push_back({
            {"observation_context_id",
             request.value("observation_context_id", "")},
            {"observation_identity_sha256",
             request.value("observation_identity_sha256", "")},
            {"observation_identity",
             request.value("observation_identity", json::object())},
            {"status", kObservationBindingStatusUnbound},
            {"reason", reason},
        });
    }
    return {
        {"schema_id", kObservationBindingFinalizationSchemaId},
        {"schema_version", 1},
        {"status", kObservationBindingStatusUnbound},
        {"binding_mode", request_collection.value("binding_mode", "")},
        {"recording_id", request_collection.value("recording_id", "")},
        {"reason", reason},
        {"context_count", contexts.size()},
        {"observation_contexts", std::move(contexts)},
    };
}

}  // namespace

RecordingObservationFinalizationResult
finalize_recording_observation_bindings(
    const std::string& recording_folder,
    const nlohmann::json& params)
{
    RecordingObservationFinalizationResult result;
    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    const fs::path root = fs::path(recording_folder).lexically_normal();
    std::error_code root_error;
    if (!root.is_absolute() || !fs::is_directory(root, root_error) ||
        root_error) {
        result.error = "recording folder is missing or not absolute";
        return result;
    }
    try {
    const std::string experiment_id = params.value("experiment_id", "");
    const json receipts = params.value("receipts", json::array());
    if (experiment_id.empty() || !receipts.is_array() || receipts.empty()) {
        result.error = "experiment_id and a nonempty receipts array are required";
        return result;
    }

    json request_collection;
    json pre_arm;
    if (!read_json(root / kRequestCollectionRelativePath,
                   &request_collection, &result.error) ||
        !read_json(root / kPreArmRelativePath, &pre_arm, &result.error)) {
        return result;
    }
    if (request_collection.value("status", "") != "materialized" ||
        pre_arm.value("lifecycle_status", "") !=
            kObservationBindingStatusAcceptedPendingFinalization ||
        request_collection.value("request_count", 0ULL) != receipts.size() ||
        pre_arm.value("acceptance_count", 0ULL) != receipts.size()) {
        result.error = "receipt count or pre-arm lifecycle does not match";
        return result;
    }

    std::map<std::string, json> requests_by_context;
    for (const auto& reference : request_collection.at("requests")) {
        json request;
        if (!load_referenced_json(root, reference, &request, &result.error) ||
            !validate_recording_observation_binding_request(
                request, &result.error)) {
            return result;
        }
        requests_by_context.emplace(
            request.at("contract").value("observation_context_id", ""),
            std::move(request));
    }
    std::map<std::string, json> acceptances_by_context;
    for (const auto& reference : pre_arm.at("acceptances")) {
        json acceptance;
        if (!load_referenced_json(root, reference, &acceptance, &result.error)) {
            return result;
        }
        const std::string context =
            acceptance.at("contract").value("observation_context_id", "");
        const auto request = requests_by_context.find(context);
        if (request == requests_by_context.end() ||
            !validate_recording_observation_binding_acceptance(
                acceptance, request->second, &result.error) ||
            acceptance.at("contract").value("status", "") != "accepted" ||
            acceptance.at("contract").value("citrus_experiment_id", "") !=
                experiment_id) {
            result.error = result.error.empty()
                ? "acceptance does not match finalization experiment"
                : result.error;
            return result;
        }
        acceptances_by_context.emplace(context, std::move(acceptance));
    }

    std::set<std::string> seen_contexts;
    std::vector<std::pair<fs::path, std::string>> receipt_writes;
    json contexts = json::array();
    std::string collection_finalized_at;
    for (const auto& receipt : receipts) {
        const std::string context =
            receipt.value("contract", json::object())
                .value("observation_context_id", "");
        const auto request = requests_by_context.find(context);
        const auto acceptance = acceptances_by_context.find(context);
        if (request == requests_by_context.end() ||
            acceptance == acceptances_by_context.end() ||
            !seen_contexts.insert(context).second ||
            !validate_recording_observation_finalized_receipt(
                receipt, request->second, acceptance->second, &result.error)) {
            result.error = result.error.empty()
                ? "receipt set contains an unknown or duplicate context"
                : result.error;
            return result;
        }
        const json& receipt_contract = receipt.at("contract");
        if (receipt_contract.value("citrus_experiment_id", "") !=
            experiment_id) {
            result.error = "receipt experiment ID mismatch";
            return result;
        }
        const json& h5 = receipt_contract.at("h5_artifact");
        const std::string h5_relative = h5.value("relative_path", "");
        if (!safe_relative_path(h5_relative)) {
            result.error = "receipt H5 path is invalid";
            return result;
        }
        const fs::path h5_path = root / h5_relative;
        std::error_code ec;
        const auto h5_status = fs::symlink_status(h5_path, ec);
        if (ec || h5_status.type() != fs::file_type::regular) {
            result.error =
                "receipt H5 is missing, a symlink, or not a regular file";
            return result;
        }
        const auto h5_size = fs::file_size(h5_path, ec);
        std::string h5_sha;
        if (!path_inside(h5_path, root) || ec || h5_size == 0 ||
            h5_size != h5.value("size_bytes", 0ULL) ||
            !file_sha256(h5_path, &h5_sha, &result.error) ||
            h5_sha != h5.value("sha256", "")) {
            result.error = result.error.empty()
                ? "closed H5 size or SHA-256 does not match receipt"
                : result.error;
            return result;
        }

        const std::string receipt_relative =
            std::string(kReceiptDirectory) + "/" + context + ".json";
        const std::string receipt_bytes = receipt.dump(2) + "\n";
        receipt_writes.emplace_back(root / receipt_relative, receipt_bytes);
        const std::string finalized_at =
            receipt_contract.value("finalized_at_utc", "");
        collection_finalized_at =
            std::max(collection_finalized_at, finalized_at);
        contexts.push_back({
            {"observation_context_id", context},
            {"observation_identity_sha256",
             request->second.at("contract")
                 .value("observation_identity_sha256", "")},
            {"observation_identity",
             request->second.at("contract").at("observation_identity")},
            {"status", kObservationBindingStatusBound},
            {"request", {
                {"request_id", request->second.value("request_id", "")},
                {"contract_sha256",
                 request->second.value("contract_sha256", "")},
                {"relative_path",
                 std::string("recording_observation_bindings/requests/") +
                     context + ".json"},
            }},
            {"acceptance", {
                {"acceptance_id",
                 acceptance->second.value("acceptance_id", "")},
                {"contract_sha256",
                 acceptance->second.value("contract_sha256", "")},
                {"relative_path",
                 std::string("recording_observation_bindings/acceptances/") +
                     context + ".json"},
            }},
            {"finalized_receipt", {
                {"receipt_id", receipt.value("receipt_id", "")},
                {"contract_sha256", receipt.value("contract_sha256", "")},
                {"relative_path", receipt_relative},
                {"sha256", byte_sha256(receipt_bytes)},
            }},
            {"citrus_h5", h5},
        });
    }
    if (seen_contexts.size() != requests_by_context.size()) {
        result.error = "receipt set does not cover every requested context";
        return result;
    }
    std::sort(contexts.begin(), contexts.end(), [](const json& left,
                                                   const json& right) {
        return left.value("observation_context_id", "") <
            right.value("observation_context_id", "");
    });

    result.collection = {
        {"schema_id", kObservationBindingFinalizationSchemaId},
        {"schema_version", 1},
        {"status", "finalized"},
        {"binding_status", kObservationBindingStatusBound},
        {"binding_mode", request_collection.value("binding_mode", "")},
        {"recording_id", request_collection.value("recording_id", "")},
        {"citrus_experiment_id", experiment_id},
        {"finalized_at_utc", collection_finalized_at},
        {"context_count", contexts.size()},
        {"observation_contexts", std::move(contexts)},
    };
    const std::string collection_bytes = result.collection.dump(2) + "\n";

    for (const auto& write : receipt_writes) {
        if (!write_create_once_exact(write.first, write.second, &result.error)) {
            return result;
        }
    }
    const fs::path collection_path =
        root / kObservationBindingFinalizationRelativePath;
    if (!write_create_once_exact(
            collection_path, collection_bytes, &result.error)) {
        return result;
    }
    result.collection_reference = artifact_reference(
        kObservationBindingFinalizationRelativePath, collection_bytes);
    result.collection_reference["schema_id"] =
        "orange.recording.observation_binding_finalization_reference";
    result.collection_reference["schema_version"] = 1;
    result.collection_reference["status"] = "finalized";
    result.collection_reference["binding_status"] =
        kObservationBindingStatusBound;
    result.collection_reference["context_count"] =
        result.collection.value("context_count", 0ULL);
    result.ok = true;
    return result;
    } catch (const json::exception& error) {
        result.error =
            "recording-observation finalization evidence is malformed: " +
            std::string(error.what());
        return result;
    } catch (const fs::filesystem_error& error) {
        result.error =
            "recording-observation finalization filesystem failure: " +
            std::string(error.what());
        return result;
    }
}

bool apply_recording_observation_finalization_to_manifest(
    const std::string& recording_folder,
    nlohmann::json* manifest,
    std::string* error_out)
{
    if (manifest == nullptr || !manifest->is_object()) {
        return fail(error_out, "recording session manifest is invalid");
    }
    const fs::path root = fs::path(recording_folder).lexically_normal();
    const fs::path requests_path = root / kRequestCollectionRelativePath;
    if (!fs::exists(requests_path)) {
        return true;
    }
    json request_collection;
    if (!read_json(requests_path, &request_collection, error_out)) {
        return false;
    }
    const fs::path collection_path =
        root / kObservationBindingFinalizationRelativePath;
    json collection;
    if (!fs::exists(collection_path)) {
        collection = unbound_summary(
            request_collection, "finalized_receipt_unavailable");
    } else if (!read_json(collection_path, &collection, error_out) ||
               collection.value("schema_id", "") !=
                   kObservationBindingFinalizationSchemaId ||
               collection.value("schema_version", 0) != 1 ||
               collection.value("status", "") != "finalized" ||
               collection.value("binding_status", "") !=
                   kObservationBindingStatusBound ||
               collection.value("recording_id", "") !=
                   request_collection.value("recording_id", "") ||
               !collection.contains("observation_contexts") ||
               !collection.at("observation_contexts").is_array() ||
               collection.value("context_count", 0ULL) !=
                   collection.at("observation_contexts").size()) {
        if (error_out != nullptr) {
            error_out->clear();
        }
        collection = unbound_summary(
            request_collection, "finalized_receipt_invalid");
    }

    (*manifest)["recording_observation_bindings"] = collection;
    (*manifest)["observation_contexts"] =
        collection.value("observation_contexts", json::array());
    if (manifest->value("mode", "") == "rolling_clips" &&
        manifest->contains("clips") && manifest->at("clips").is_array()) {
        json parent_context_references = json::array();
        for (const auto& context : manifest->at("observation_contexts")) {
            parent_context_references.push_back({
                {"observation_context_id",
                 context.value("observation_context_id", "")},
                {"observation_identity_sha256",
                 context.value("observation_identity_sha256", "")},
            });
        }
        for (auto& clip : (*manifest)["clips"]) {
            if (!clip.is_object()) {
                continue;
            }
            clip["observation_contexts"] = {
                {"schema_id",
                 "orange.recording_clip.parent_observation_context_reference"},
                {"schema_version", 1},
                {"authority", "parent_recording_session"},
                {"parent_field", "observation_contexts"},
                {"identity_policy", "inherit_without_rekeying"},
                {"contexts", parent_context_references},
            };
        }
    }
    return true;
}

bool refresh_recording_session_observation_bindings(
    const std::string& recording_folder,
    std::string* error_out)
{
    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    const fs::path root = fs::path(recording_folder).lexically_normal();
    const fs::path manifest_path = root / "recording_session.json";
    std::error_code status_error;
    const auto status = fs::symlink_status(manifest_path, status_error);
    if (status_error) {
        return fail(error_out,
                    "could not inspect recording_session.json: " +
                        status_error.message());
    }
    if (status.type() == fs::file_type::not_found) {
        return true;
    }
    if (status.type() != fs::file_type::regular) {
        return fail(error_out,
                    "recording_session.json is not a regular file");
    }
    json manifest;
    if (!read_json(manifest_path, &manifest, error_out) ||
        !manifest.is_object() ||
        !apply_recording_observation_finalization_to_manifest(
            recording_folder, &manifest, error_out)) {
        return false;
    }
    return write_atomic_replace(
        manifest_path, manifest.dump(2) + "\n", error_out);
}

}  // namespace orange::session
