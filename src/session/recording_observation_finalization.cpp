#include "session/recording_observation_finalization.h"

#include "gui/spatial_layout/sha256.h"
#include "fsuid_guard.h"
#include "session/recording_observation_binding.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
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

// Where a finalization writes and what it must satisfy before writing. The
// live finalize writes revision 1 (receipts/, finalized_collection.json); an
// upgrade writes revision N (receipts/r<N>/, finalized_collection.r<N>.json).
struct FinalizeTarget {
    std::string receipt_directory = kReceiptDirectory;
    std::string collection_relative = kObservationBindingFinalizationRelativePath;
    bool allow_legacy_roles = false;
    json extra_collection_fields = json::object();
    std::function<bool(const json& collection, std::string* error_out)> check;
};

// The same proof as for the H5: a safe path inside the recording, a regular
// non-symlink file, exact size and SHA-256; for the stimulus-video
// finalization also the declared container facts against the file.
bool verify_citrus_artifact_files(const fs::path& root,
                                  const json& artifacts,
                                  bool allow_legacy_roles,
                                  std::string* error_out)
{
    for (const auto& artifact : artifacts) {
        const std::string role = artifact.value("role", "");
        const std::string relative = artifact.value("relative_path", "");
        if (role == kCitrusArtifactRoleLegacyRecordingDiagnostic && !allow_legacy_roles) {
            return fail(error_out,
                        "legacy_recording_diagnostic is valid only in an upgraded collection");
        }
        const fs::path path = root / relative;
        std::error_code ec;
        const auto status = fs::symlink_status(path, ec);
        if (ec || status.type() != fs::file_type::regular || !path_inside(path, root)) {
            return fail(error_out,
                        "declared Citrus artifact is missing, a symlink, or not a regular file: " +
                            relative);
        }
        const auto size = fs::file_size(path, ec);
        std::string sha;
        if (ec || size != artifact.value("size_bytes", 0ULL) ||
            !file_sha256(path, &sha, error_out) || sha != artifact.value("sha256", "")) {
            return fail(error_out,
                        "declared Citrus artifact size or SHA-256 does not match receipt: " +
                            relative);
        }
        if (role == kCitrusArtifactRoleStimulusVideoFinalization) {
            json finalization;
            if (!read_json(path, &finalization, error_out)) {
                return false;
            }
            const json& declared = artifact.at("finalization");
            const json container = finalization.value("container", json::object());
            if (finalization.value("schema_id", "") != declared.value("schema_id", "") ||
                finalization.value("schema_version", -1) != declared.value("schema_version", -2) ||
                finalization.value("status", "") != declared.value("status", "") ||
                finalization.value("terminal", false) != declared.value("terminal", true) ||
                container.value("trailer_written", false) != declared.value("trailer_written", true) ||
                container.value("output_closed", false) != declared.value("output_closed", true) ||
                container.value("file_size_bytes", 0ULL) !=
                    declared.value("container_file_size_bytes", 1ULL)) {
                return fail(error_out,
                            "stimulus video finalization file disagrees with the receipt: " +
                                relative);
            }
        }
    }
    return true;
}

RecordingObservationFinalizationResult
finalize_into(const std::string& recording_folder,
              const nlohmann::json& params,
              const FinalizeTarget& target)
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
    std::set<std::string> declared_paths;  // H5s and Citrus artifacts, collection-wide
    std::vector<std::string> legacy_contexts;
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

        if (!declared_paths.insert(h5_relative).second) {
            result.error = "receipt H5 path is declared twice in the collection";
            return result;
        }
        if (receipt_contract.contains("citrus_artifacts")) {
            const json& artifacts = receipt_contract.at("citrus_artifacts");
            if (!verify_citrus_artifact_files(root, artifacts, target.allow_legacy_roles,
                                              &result.error)) {
                return result;
            }
            for (const auto& artifact : artifacts) {
                if (!declared_paths.insert(artifact.value("relative_path", "")).second) {
                    result.error = "a Citrus artifact path is declared twice in the collection: " +
                        artifact.value("relative_path", "");
                    return result;
                }
                if (artifact.value("role", "") == kCitrusArtifactRoleLegacyRecordingDiagnostic) {
                    legacy_contexts.push_back(context);
                }
            }
        }

        const std::string receipt_relative =
            target.receipt_directory + "/" + context + ".json";
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
    if (legacy_contexts.size() > 1 ||
        (legacy_contexts.size() == 1 && legacy_contexts.front() != *seen_contexts.begin())) {
        result.error = "the legacy recording diagnostic must appear once, in the receipt of "
                       "the lowest observation_context_id";
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
    for (const auto& field : target.extra_collection_fields.items()) {
        result.collection[field.key()] = field.value();
    }
    if (target.check && !target.check(result.collection, &result.error)) {
        return result;
    }
    const std::string collection_bytes = result.collection.dump(2) + "\n";

    for (const auto& write : receipt_writes) {
        if (!write_create_once_exact(write.first, write.second, &result.error)) {
            return result;
        }
    }
    const fs::path collection_path = root / target.collection_relative;
    if (!write_create_once_exact(
            collection_path, collection_bytes, &result.error)) {
        return result;
    }
    result.collection_reference = artifact_reference(
        target.collection_relative, collection_bytes);
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

std::string revision_collection_relative(int revision)
{
    return revision == 1
        ? std::string(kObservationBindingFinalizationRelativePath)
        : std::string(kObservationBindingRevisionPrefix) + std::to_string(revision) + ".json";
}

std::string revision_receipt_directory(int revision)
{
    return revision == 1 ? std::string(kReceiptDirectory)
                         : std::string(kReceiptDirectory) + "/r" + std::to_string(revision);
}

struct CollectionRevision {
    int revision = 0;
    std::string relative_path;
    std::string sha256;
    json collection;
};

// The finalized-collection chain: revision 1 is finalized_collection.json;
// revision N >= 2 is finalized_collection.r<N>.json, schema_version 2, whose
// `supersedes` names revision N-1 by path and digest. Linear, gap-free, from
// r1; any r<N> outside the chain makes it invalid. An empty chain is valid
// (nothing finalized yet).
bool load_collection_chain(const fs::path& root,
                           std::vector<CollectionRevision>* chain,
                           std::string* error_out)
{
    chain->clear();
    const fs::path directory = root / "recording_observation_bindings";
    std::set<int> present;
    std::error_code ec;
    if (fs::is_directory(directory, ec)) {
        const std::string prefix = "finalized_collection.r";
        for (const auto& entry : fs::directory_iterator(directory, ec)) {
            const std::string name = entry.path().filename().string();
            if (name.rfind(prefix, 0) != 0 || name.size() <= prefix.size() + 5 ||
                name.compare(name.size() - 5, 5, ".json") != 0) {
                continue;
            }
            const std::string digits = name.substr(prefix.size(), name.size() - prefix.size() - 5);
            if (digits.empty() || digits.size() > 6 ||
                !std::all_of(digits.begin(), digits.end(), ::isdigit) || digits.front() == '0') {
                return fail(error_out, "unexpected collection revision file: " + name);
            }
            present.insert(std::stoi(digits));
        }
    }
    if (!fs::exists(root / kObservationBindingFinalizationRelativePath)) {
        return present.empty() ||
            fail(error_out, "collection revisions exist without revision 1");
    }
    const int last = present.empty() ? 1 : *present.rbegin();
    for (int revision = 1; revision <= last; ++revision) {
        CollectionRevision item;
        item.revision = revision;
        item.relative_path = revision_collection_relative(revision);
        if (revision > 1 && !present.count(revision)) {
            return fail(error_out, "collection revision chain has a gap at r" +
                                       std::to_string(revision));
        }
        std::string bytes;
        if (!read_bytes(root / item.relative_path, &bytes, error_out)) {
            return false;
        }
        item.sha256 = byte_sha256(bytes);
        item.collection = json::parse(bytes, nullptr, false);
        if (item.collection.is_discarded() || !item.collection.is_object()) {
            return fail(error_out, "collection revision is not valid JSON: " + item.relative_path);
        }
        if (revision == 1) {
            if (item.collection.value("schema_version", 0) != 1 ||
                item.collection.contains("revision")) {
                return fail(error_out, "revision 1 must be the schema_version 1 collection");
            }
        } else {
            const json supersedes = item.collection.value("supersedes", json::object());
            const CollectionRevision& previous = chain->back();
            if (item.collection.value("schema_version", 0) != 2 ||
                item.collection.value("revision", 0) != revision ||
                supersedes.value("relative_path", "") != previous.relative_path ||
                supersedes.value("sha256", "") != previous.sha256 ||
                item.collection.value("revision_reason", "") !=
                    kObservationBindingRevisionReasonReceiptV2Upgrade) {
                return fail(error_out, "collection revision r" + std::to_string(revision) +
                                           " does not supersede r" +
                                           std::to_string(revision - 1) + " exactly");
            }
        }
        chain->push_back(std::move(item));
    }
    return true;
}

}  // namespace

RecordingObservationFinalizationResult
upgrade_recording_observation_receipts(
    const std::string& recording_folder,
    const nlohmann::json& params)
{
    RecordingObservationFinalizationResult result;
    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    const fs::path root = fs::path(recording_folder).lexically_normal();
    try {
    if (params.value("reason", "") != kObservationBindingRevisionReasonReceiptV2Upgrade) {
        result.error = std::string("upgrade reason must be ") +
            kObservationBindingRevisionReasonReceiptV2Upgrade;
        return result;
    }
    const json receipts = params.value("receipts", json::array());
    for (const auto& receipt : receipts) {
        if (binding_record_schema_version(receipt) !=
            kObservationBindingFinalizedReceiptSchemaVersionV2) {
            result.error = "a receipt v2 upgrade accepts only v2 receipts";
            return result;
        }
    }
    std::vector<CollectionRevision> chain;
    if (!load_collection_chain(root, &chain, &result.error)) {
        return result;
    }
    if (chain.empty()) {
        result.error = "nothing to upgrade: the recording has no finalized collection";
        return result;
    }
    const CollectionRevision& head = chain.back();
    if (head.collection.value("status", "") != "finalized" ||
        head.collection.value("binding_status", "") != kObservationBindingStatusBound) {
        result.error = "the head collection is not finalized and bound";
        return result;
    }

    // Idempotent retry: the head already carries exactly these receipts.
    std::map<std::string, std::string> head_receipt_sha;
    for (const auto& context : head.collection.at("observation_contexts")) {
        head_receipt_sha[context.value("observation_context_id", "")] =
            context.at("finalized_receipt").value("sha256", "");
    }
    bool same_as_head = head.revision > 1 && receipts.size() == head_receipt_sha.size();
    for (const auto& receipt : receipts) {
        const std::string context =
            receipt.value("contract", json::object()).value("observation_context_id", "");
        const auto found = head_receipt_sha.find(context);
        same_as_head = same_as_head && found != head_receipt_sha.end() &&
            found->second == byte_sha256(receipt.dump(2) + "\n");
    }
    if (same_as_head) {
        result.collection = head.collection;
        result.collection_reference = {
            {"relative_path", head.relative_path},
            {"sha256", head.sha256},
            {"revision", head.revision},
            {"status", "finalized"},
            {"binding_status", kObservationBindingStatusBound},
        };
        result.ok = true;
        return result;
    }

    const int revision = head.revision + 1;
    FinalizeTarget target;
    target.receipt_directory = revision_receipt_directory(revision);
    target.collection_relative = revision_collection_relative(revision);
    target.allow_legacy_roles = true;
    target.extra_collection_fields = {
        {"schema_version", 2},
        {"revision", revision},
        {"supersedes", {{"relative_path", head.relative_path}, {"sha256", head.sha256}}},
        {"revision_reason", kObservationBindingRevisionReasonReceiptV2Upgrade},
        {"revised_at_utc", params.value("revised_at_utc", "")},
    };
    const json& head_collection = head.collection;
    // A receipt upgrade changes receipts only: same experiment, contexts,
    // identities, requests, acceptances and byte-identical H5s.
    target.check = [&head_collection](const json& collection, std::string* error_out) {
        if (collection.value("citrus_experiment_id", "") !=
                head_collection.value("citrus_experiment_id", "") ||
            collection.value("recording_id", "") != head_collection.value("recording_id", "") ||
            collection.value("binding_mode", "") != head_collection.value("binding_mode", "") ||
            collection.value("context_count", 0ULL) !=
                head_collection.value("context_count", 1ULL)) {
            return fail(error_out, "upgrade does not match the head collection's recording");
        }
        const json& before = head_collection.at("observation_contexts");
        const json& after = collection.at("observation_contexts");
        for (std::size_t i = 0; i < after.size(); ++i) {
            for (const char* field : {"observation_context_id", "observation_identity_sha256",
                                      "observation_identity", "request", "acceptance",
                                      "citrus_h5", "status"}) {
                if (before.at(i).at(field) != after.at(i).at(field)) {
                    return fail(error_out, std::string("upgrade changes ") + field +
                                               " of " +
                                               after.at(i).value("observation_context_id", ""));
                }
            }
        }
        return true;
    };
    const std::string revised_at = params.value("revised_at_utc", "");
    if (revised_at.empty()) {
        result.error = "revised_at_utc is required";
        return result;
    }
    result = finalize_into(recording_folder, params, target);
    if (result.ok) {
        result.collection_reference["revision"] = revision;
    }
    return result;
    } catch (const json::exception& error) {
        result.error = "receipt upgrade evidence is malformed: " + std::string(error.what());
        return result;
    } catch (const fs::filesystem_error& error) {
        result.error = "receipt upgrade filesystem failure: " + std::string(error.what());
        return result;
    }
}

RecordingObservationFinalizationResult
finalize_recording_observation_bindings(
    const std::string& recording_folder,
    const nlohmann::json& params)
{
    return finalize_into(recording_folder, params, FinalizeTarget{});
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
    std::vector<CollectionRevision> chain;
    std::string chain_error;
    json collection;
    const bool chain_ok = load_collection_chain(root, &chain, &chain_error);
    if (chain_ok && chain.empty()) {
        collection = unbound_summary(
            request_collection, "finalized_receipt_unavailable");
    } else if (!chain_ok ||
               (collection = chain.back().collection,
                collection.value("schema_id", "") !=
                    kObservationBindingFinalizationSchemaId) ||
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
            request_collection,
            chain_ok ? "finalized_receipt_invalid" : "finalized_collection_chain_invalid");
    } else if (chain.size() > 1) {
        // Project the head and the whole chain (oldest first) so a reader can
        // verify every superseded revision by digest.
        json revision_chain = json::array();
        for (const auto& item : chain) {
            revision_chain.push_back({{"revision", item.revision},
                                      {"relative_path", item.relative_path},
                                      {"sha256", item.sha256}});
        }
        collection["revision_chain"] = std::move(revision_chain);
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
        if (status_error == std::errc::no_such_file_or_directory) {
            return true;
        }
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
