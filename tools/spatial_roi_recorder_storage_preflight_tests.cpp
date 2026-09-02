#include "spatial_roi_recorder_storage_preflight.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace contract = orange::session::spatial_roi;
namespace recording = orange::spatial_roi::recording;

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class TempTree final {
public:
    TempTree()
    {
        std::string pattern = "/tmp/orange_spatial_roi_storage_test_XXXXXX";
        std::vector<char> mutable_pattern(pattern.begin(), pattern.end());
        mutable_pattern.push_back('\0');
        const char* created = ::mkdtemp(mutable_pattern.data());
        require(created != nullptr,
                std::string("mkdtemp failed: ") + std::strerror(errno));
        path_ = created;
    }

    ~TempTree()
    {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

std::unique_ptr<recording::SpatialRoiRecorderArtifactRoot> make_root(
    const TempTree& tree)
{
    const fs::path recording_root = tree.path() / "recording";
    require(fs::create_directory(recording_root),
            "failed to create recording root");
    std::unique_ptr<recording::SpatialRoiRecorderArtifactRoot> root;
    std::string error;
    require(recording::SpatialRoiRecorderArtifactRoot::Open(
                recording_root, {"stream.mp4"}, &root, &error),
            "failed to open artifact root: " + error);
    require(root && root->valid(), "artifact root authority is invalid");
    return root;
}

recording::SpatialRoiRecorderStoragePreflightPolicy policy()
{
    recording::SpatialRoiRecorderStoragePreflightPolicy result;
    result.reserved_free_bytes = 100;
    return result;
}

recording::SpatialRoiRecorderFilesystemQuery query_with(
    const recording::SpatialRoiRecorderFilesystemStats stats)
{
    return [stats](const int fd,
                   recording::SpatialRoiRecorderFilesystemStats* output,
                   std::string*) {
        require(fd >= 0, "filesystem query did not receive the root descriptor");
        *output = stats;
        return true;
    };
}

void test_pass_and_closed_json()
{
    TempTree tree;
    auto root = make_root(tree);
    recording::SpatialRoiRecorderStoragePreflightResult result;
    std::string error;
    require(recording::run_spatial_roi_recorder_storage_preflight(
                *root,
                100,
                200,
                policy(),
                &result,
                query_with({10, 1000, 80}),
                &error),
            error);
    require(result.checked && result.passed && result.status == "passed" &&
                result.required_bytes == 400 && result.capacity_bytes == 10000 &&
                result.available_bytes == 800,
            "passing preflight did not retain exact byte accounting");
    require(result.artifact_root_identity == root->artifact_root_identity(),
            "preflight did not retain the exact artifact-root identity");
    const nlohmann::json value =
        recording::spatial_roi_recorder_storage_preflight_to_json(result);
    require(value.size() == 10 &&
                value.at("schema_id") ==
                    contract::kSpatialRoiRecorderStoragePreflightSchemaId &&
                value.at("schema_version") ==
                    contract::kSpatialRoiRecorderStoragePreflightSchemaVersion &&
                value.at("passed") == true &&
                value.at("policy").at("schema_id") ==
                    contract::kSpatialRoiRecorderStoragePreflightPolicySchemaId &&
                value.at("policy").at("schema_version") ==
                    contract::kSpatialRoiRecorderStoragePreflightPolicySchemaVersion &&
                value.at("policy").at("reserved_free_bytes") == 100 &&
                value.at("budgets").at("max_media_bytes_total") == 100 &&
                value.at("budgets").at("max_evidence_bytes_total") == 200 &&
                value.at("budgets").at("required_bytes") == 400 &&
                value.at("filesystem").at("available_bytes") == 800,
            "preflight JSON was not the closed versioned observation");
}

void test_insufficient_space_fails_closed()
{
    TempTree tree;
    auto root = make_root(tree);
    recording::SpatialRoiRecorderStoragePreflightResult result;
    std::string error;
    require(!recording::run_spatial_roi_recorder_storage_preflight(
                *root,
                100,
                200,
                policy(),
                &result,
                query_with({10, 1000, 39}),
                &error),
            "insufficient space unexpectedly passed");
    require(result.checked && !result.passed && result.status == "failed" &&
                result.required_bytes == 400 && !result.error.empty() &&
                error == result.error,
            "insufficient space did not retain a failed preflight result");
}

void test_block_conversion_overflow_fails_closed()
{
    TempTree tree;
    auto root = make_root(tree);
    recording::SpatialRoiRecorderStoragePreflightResult result;
    std::string error;
    require(!recording::run_spatial_roi_recorder_storage_preflight(
                *root,
                100,
                200,
                policy(),
                &result,
                query_with({std::numeric_limits<std::uint64_t>::max(), 2, 1}),
                &error),
            "block multiplication overflow unexpectedly passed");
    require(result.checked && !result.passed &&
                result.status == "failed" &&
                result.error.find("overflow") != std::string::npos,
            "block multiplication overflow was not fail-closed");
}

void test_query_failure_fails_closed()
{
    TempTree tree;
    auto root = make_root(tree);
    recording::SpatialRoiRecorderStoragePreflightResult result;
    std::string error;
    const recording::SpatialRoiRecorderFilesystemQuery query =
        [](const int, recording::SpatialRoiRecorderFilesystemStats*,
           std::string* error_out) {
            if (error_out) *error_out = "injected filesystem query failure";
            return false;
        };
    require(!recording::run_spatial_roi_recorder_storage_preflight(
                *root, 100, 200, policy(), &result, query, &error),
            "query failure unexpectedly passed");
    require(result.checked && !result.passed &&
                result.error == "injected filesystem query failure" &&
                error == result.error,
            "query failure was not retained in the closed result");
}

void test_invalid_descriptor_fails_closed()
{
    TempTree tree;
    auto root = make_root(tree);
    const int descriptor = root->borrowed_artifact_root_fd();
    require(::close(descriptor) == 0, "failed to invalidate test descriptor");
    recording::SpatialRoiRecorderStoragePreflightResult result;
    std::string error;
    require(!recording::run_spatial_roi_recorder_storage_preflight(
                *root, 100, 200, policy(), &result, {}, &error),
            "invalid descriptor unexpectedly passed");
    require(result.checked && !result.passed &&
                result.error.find("fstat") != std::string::npos,
            "invalid descriptor did not fail at descriptor identity check");
}

void test_path_replacement_does_not_redirect_descriptor_query()
{
    TempTree tree;
    auto root = make_root(tree);
    const fs::path artifact_path =
        tree.path() / "recording" / recording::kSpatialRoiRecorderArtifactDirectory;
    const fs::path moved_path = tree.path() / "moved_artifacts";
    std::error_code rename_error;
    fs::rename(artifact_path, moved_path, rename_error);
    require(!rename_error, "failed to move the original artifact directory");
    require(fs::create_directory(artifact_path),
            "failed to create replacement artifact directory");

    recording::SpatialRoiRecorderStoragePreflightResult result;
    std::string error;
    require(recording::run_spatial_roi_recorder_storage_preflight(
                *root, 100, 200, policy(), &result,
                query_with({10, 1000, 80}), &error),
            error);
    require(result.passed &&
                result.artifact_root_identity == root->artifact_root_identity(),
            "descriptor-bound query was redirected by pathname replacement");
}

}  // namespace

int main()
{
    try {
        test_pass_and_closed_json();
        test_insufficient_space_fails_closed();
        test_block_conversion_overflow_fails_closed();
        test_query_failure_fails_closed();
        test_invalid_descriptor_fails_closed();
        test_path_replacement_does_not_redirect_descriptor_query();
        std::cout << "spatial_roi_recorder_storage_preflight_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "spatial_roi_recorder_storage_preflight_tests: FAIL: "
                  << exception.what() << '\n';
        return 1;
    }
}
