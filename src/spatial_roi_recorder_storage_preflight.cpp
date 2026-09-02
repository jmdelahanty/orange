#include "spatial_roi_recorder_storage_preflight.h"

#include <cerrno>
#include <cstring>
#include <exception>
#include <limits>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <type_traits>
#include <unistd.h>
#include <utility>

namespace orange::spatial_roi::recording {
namespace {

using json = nlohmann::json;
namespace contract = session::spatial_roi;

bool checked_multiply(const std::uint64_t left,
                      const std::uint64_t right,
                      std::uint64_t* out)
{
    if (!out || (left != 0 &&
                 right > std::numeric_limits<std::uint64_t>::max() / left)) {
        return false;
    }
    *out = left * right;
    return true;
}

bool checked_add(const std::uint64_t left,
                 const std::uint64_t right,
                 std::uint64_t* out)
{
    if (!out || right > std::numeric_limits<std::uint64_t>::max() - left) {
        return false;
    }
    *out = left + right;
    return true;
}

std::string errno_message(const char* operation, const int error_number)
{
    std::string result = operation ? operation : "filesystem query";
    result += " failed: ";
    const char* text = std::strerror(error_number);
    result += text ? text : "unknown error";
    return result;
}

bool fail(std::string* error_out, std::string message)
{
    if (error_out) {
        *error_out = std::move(message);
    }
    return false;
}

bool default_filesystem_query(
    const int artifact_root_fd,
    SpatialRoiRecorderFilesystemStats* stats_out,
    std::string* error_out)
{
    if (!stats_out) {
        return fail(error_out, "filesystem stats destination is null");
    }
    if (artifact_root_fd < 0) {
        return fail(error_out, "artifact root descriptor is invalid");
    }

    struct statvfs value {};
    if (::fstatvfs(artifact_root_fd, &value) != 0) {
        return fail(error_out,
                    errno_message("fstatvfs artifact root descriptor", errno));
    }
    // f_frsize is the fundamental allocation unit used for f_blocks and
    // f_bavail. A filesystem reporting zero cannot be converted safely.
    if (value.f_frsize == 0) {
        return fail(error_out,
                    "fstatvfs artifact root descriptor returned zero block size");
    }
    const auto copy_counter = [](const auto source,
                                 std::uint64_t* destination,
                                 const char* field,
                                 std::string* failure) {
        using Counter = decltype(source);
        if constexpr (std::is_signed_v<Counter>) {
            if (source < 0) {
                return fail(failure,
                            std::string("fstatvfs returned a negative ") +
                                field);
            }
        }
        if constexpr (sizeof(Counter) > sizeof(std::uint64_t)) {
            if (source > static_cast<Counter>(
                             std::numeric_limits<std::uint64_t>::max())) {
                return fail(failure,
                            std::string("fstatvfs ") + field +
                                " exceeds uint64 byte-accounting range");
            }
        }
        *destination = static_cast<std::uint64_t>(source);
        return true;
    };
    if (!copy_counter(value.f_frsize,
                      &stats_out->block_size_bytes,
                      "fundamental block size",
                      error_out) ||
        !copy_counter(value.f_blocks,
                      &stats_out->total_blocks,
                      "total block count",
                      error_out) ||
        !copy_counter(value.f_bavail,
                      &stats_out->available_blocks,
                      "available block count",
                      error_out)) {
        return false;
    }
    return true;
}

void initialize_result(
    const SpatialRoiRecorderStoragePreflightPolicy& policy,
    const std::uint64_t max_media_bytes_total,
    const std::uint64_t max_evidence_bytes_total,
    const SpatialRoiRecorderArtifactRoot& artifact_root,
    SpatialRoiRecorderStoragePreflightResult* result)
{
    *result = SpatialRoiRecorderStoragePreflightResult{};
    result->schema_id = contract::kSpatialRoiRecorderStoragePreflightSchemaId;
    result->schema_version =
        contract::kSpatialRoiRecorderStoragePreflightSchemaVersion;
    result->checked = true;
    result->status = "failed";
    result->policy = policy;
    result->artifact_root_identity = artifact_root.artifact_root_identity();
    result->max_media_bytes_total = max_media_bytes_total;
    result->max_evidence_bytes_total = max_evidence_bytes_total;
}

bool reject_result(SpatialRoiRecorderStoragePreflightResult* result,
                   std::string* error_out,
                   std::string reason)
{
    result->passed = false;
    result->status = "failed";
    result->error = std::move(reason);
    return fail(error_out, result->error);
}

}  // namespace

bool run_spatial_roi_recorder_storage_preflight(
    const SpatialRoiRecorderArtifactRoot& artifact_root,
    const std::uint64_t max_media_bytes_total,
    const std::uint64_t max_evidence_bytes_total,
    const SpatialRoiRecorderStoragePreflightPolicy& policy,
    SpatialRoiRecorderStoragePreflightResult* result_out,
    const SpatialRoiRecorderFilesystemQuery& filesystem_query,
    std::string* error_out)
{
    if (error_out) {
        error_out->clear();
    }
    if (!result_out) {
        return fail(error_out, "storage preflight result destination is null");
    }
    initialize_result(policy,
                      max_media_bytes_total,
                      max_evidence_bytes_total,
                      artifact_root,
                      result_out);

    if (policy.schema_id !=
            contract::kSpatialRoiRecorderStoragePreflightPolicySchemaId ||
        policy.schema_version !=
            contract::kSpatialRoiRecorderStoragePreflightPolicySchemaVersion ||
        !policy.required || policy.reserved_free_bytes == 0) {
        return reject_result(result_out,
                             error_out,
                             "storage preflight policy is invalid or has zero reserved free space");
    }
    if (!artifact_root.valid()) {
        return reject_result(result_out,
                             error_out,
                             "artifact root authority is invalid");
    }
    if (max_media_bytes_total == 0 || max_evidence_bytes_total == 0) {
        return reject_result(result_out,
                             error_out,
                             "authenticated media and evidence budgets must be nonzero");
    }

    std::uint64_t budget_bytes = 0;
    if (!checked_add(max_media_bytes_total,
                     max_evidence_bytes_total,
                     &budget_bytes) ||
        !checked_add(budget_bytes,
                     policy.reserved_free_bytes,
                     &result_out->required_bytes)) {
        return reject_result(result_out,
                             error_out,
                             "storage preflight required byte total overflowed");
    }

    struct stat root_status {};
    if (::fstat(artifact_root.borrowed_artifact_root_fd(), &root_status) != 0) {
        return reject_result(result_out,
                             error_out,
                             errno_message("fstat artifact root descriptor", errno));
    }
    if (!S_ISDIR(root_status.st_mode) || root_status.st_ino == 0 ||
        static_cast<std::uint64_t>(root_status.st_dev) !=
            result_out->artifact_root_identity.device ||
        static_cast<std::uint64_t>(root_status.st_ino) !=
            result_out->artifact_root_identity.inode) {
        return reject_result(result_out,
                             error_out,
                             "artifact root descriptor identity no longer matches its retained authority");
    }

    SpatialRoiRecorderFilesystemStats stats;
    std::string query_error;
    bool queried = false;
    try {
        queried = filesystem_query
                      ? filesystem_query(
                            artifact_root.borrowed_artifact_root_fd(),
                            &stats,
                            &query_error)
                      : default_filesystem_query(
                            artifact_root.borrowed_artifact_root_fd(),
                            &stats,
                            &query_error);
    } catch (const std::exception& exception) {
        query_error = std::string("filesystem capacity query threw: ") +
                      exception.what();
    } catch (...) {
        query_error = "filesystem capacity query threw";
    }
    result_out->filesystem = stats;
    if (!queried) {
        return reject_result(result_out,
                             error_out,
                             query_error.empty()
                                 ? "filesystem capacity query failed"
                                 : query_error);
    }
    if (stats.block_size_bytes == 0) {
        return reject_result(result_out,
                             error_out,
                             "filesystem capacity query returned zero block size");
    }
    if (!checked_multiply(stats.total_blocks,
                          stats.block_size_bytes,
                          &result_out->capacity_bytes) ||
        !checked_multiply(stats.available_blocks,
                          stats.block_size_bytes,
                          &result_out->available_bytes)) {
        return reject_result(result_out,
                             error_out,
                             "filesystem block-derived byte totals overflowed");
    }
    if (stats.available_blocks > stats.total_blocks ||
        result_out->available_bytes > result_out->capacity_bytes) {
        return reject_result(result_out,
                             error_out,
                             "filesystem available blocks exceed total capacity");
    }

    result_out->passed = result_out->available_bytes >= result_out->required_bytes;
    result_out->status = result_out->passed ? "passed" : "failed";
    if (!result_out->passed) {
        result_out->error =
            "filesystem available bytes are below the authenticated required total";
        return fail(error_out, result_out->error);
    }
    result_out->error.clear();
    return true;
}

nlohmann::json spatial_roi_recorder_storage_preflight_to_json(
    const SpatialRoiRecorderStoragePreflightResult& result)
{
    return {
        {"schema_id", result.schema_id},
        {"schema_version", result.schema_version},
        {"checked", result.checked},
        {"passed", result.passed},
        {"status", result.status},
        {"error", result.error},
        {"policy", {
            {"schema_id", result.policy.schema_id},
            {"schema_version", result.policy.schema_version},
            {"required", result.policy.required},
            {"reserved_free_bytes", result.policy.reserved_free_bytes},
        }},
        {"artifact_root", {
            {"device", result.artifact_root_identity.device},
            {"inode", result.artifact_root_identity.inode},
        }},
        {"filesystem", {
            {"block_size_bytes", result.filesystem.block_size_bytes},
            {"total_blocks", result.filesystem.total_blocks},
            {"available_blocks", result.filesystem.available_blocks},
            {"capacity_bytes", result.capacity_bytes},
            {"available_bytes", result.available_bytes},
        }},
        {"budgets", {
            {"max_media_bytes_total", result.max_media_bytes_total},
            {"max_evidence_bytes_total", result.max_evidence_bytes_total},
            {"reserved_free_bytes", result.policy.reserved_free_bytes},
            {"required_bytes", result.required_bytes},
        }},
    };
}

}  // namespace orange::spatial_roi::recording
