#include "spatial_roi_socket_runtime_directory.h"

#include "session/spatial_roi_recording_config.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;
namespace ipc = orange::spatial_roi::ipc;

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string unique_digest()
{
    static std::atomic<std::uint32_t> sequence{0};
    const auto ticks = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const std::uint32_t tail =
        static_cast<std::uint32_t>(::getpid()) ^ sequence.fetch_add(1);
    std::array<char, 25> bytes{};
    std::snprintf(bytes.data(), bytes.size(), "%016llx%08x",
                  static_cast<unsigned long long>(ticks), tail);
    return bytes.data();
}

ipc::SpatialRoiSocketRuntimeDirectoryConfig config_for(
    const std::string& directory_digest)
{
    ipc::SpatialRoiSocketRuntimeDirectoryConfig config;
    config.recording_identity_token =
        "sha256:" + directory_digest + std::string(40, '0');
    for (std::uint32_t index = 0; index < 4; ++index) {
        config.logical_stream_ids.push_back(
            "CAM001_spatial_roi_roi" + std::to_string(index));
    }
    return config;
}

std::string directory_path_for(
    const ipc::SpatialRoiSocketRuntimeDirectoryConfig& config)
{
    return orange::session::spatial_roi::expected_socket_runtime_directory(
        config.recording_identity_token);
}

void cleanup_exact(const fs::path& path)
{
    std::error_code ignored;
    fs::remove_all(path, ignored);
}

void test_create_and_exact_cleanup()
{
    const auto config = config_for(unique_digest());
    const std::string directory_path = directory_path_for(config);
    std::string error;
    auto directory = ipc::SpatialRoiSocketRuntimeDirectory::Create(config, &error);
    require(directory && directory->valid(), "create failed: " + error);
    struct stat status {};
    require(directory->directory_path() == directory_path &&
                directory->socket_paths().size() == 4 &&
                ::lstat(directory_path.c_str(), &status) == 0 &&
                S_ISDIR(status.st_mode) && status.st_uid == ::geteuid() &&
                (status.st_mode & 07777) == 0700,
            "created directory did not retain exact mode/owner/type");
    require(directory->device() == static_cast<std::uint64_t>(status.st_dev) &&
                directory->inode() == static_cast<std::uint64_t>(status.st_ino),
            "created directory identity was not retained");
    require(directory->Close(&error), "exact empty cleanup failed: " + error);
    require(::lstat(directory_path.c_str(), &status) != 0 &&
                errno == ENOENT,
            "exact runtime directory remained after cleanup");
    require(directory->Close(&error), "idempotent close failed");
}

void test_existing_entry_is_refused_and_preserved()
{
    const auto config = config_for(unique_digest());
    const std::string directory_path = directory_path_for(config);
    require(::mkdir(directory_path.c_str(), 0700) == 0,
            "could not create existing directory fixture");
    std::string error;
    auto directory = ipc::SpatialRoiSocketRuntimeDirectory::Create(config, &error);
    require(!directory && error.find("already exists") != std::string::npos &&
                fs::is_directory(directory_path),
            "pre-existing runtime directory was accepted or removed");
    cleanup_exact(directory_path);
}

void test_identity_must_derive_exact_four_children()
{
    auto config = config_for(unique_digest());
    const std::string directory_path = directory_path_for(config);
    config.recording_identity_token[0] = 'x';
    std::string error;
    require(!ipc::SpatialRoiSocketRuntimeDirectory::Create(config, &error) &&
                !fs::exists(directory_path),
            "invalid recording identity token was accepted or left residue");

    config = config_for(unique_digest());
    const std::string short_path = directory_path_for(config);
    config.logical_stream_ids.pop_back();
    require(!ipc::SpatialRoiSocketRuntimeDirectory::Create(config, &error) &&
                !fs::exists(short_path),
            "non-four-stream path set was accepted");

    config = config_for(unique_digest());
    const std::string duplicate_path = directory_path_for(config);
    config.logical_stream_ids.back() = config.logical_stream_ids.front();
    require(!ipc::SpatialRoiSocketRuntimeDirectory::Create(config, &error) &&
                !fs::exists(duplicate_path),
            "duplicate logical stream derived duplicate socket authority");
}

void test_nonempty_directory_is_preserved()
{
    const auto config = config_for(unique_digest());
    const std::string directory_path = directory_path_for(config);
    std::string error;
    auto directory = ipc::SpatialRoiSocketRuntimeDirectory::Create(config, &error);
    require(directory != nullptr, "create failed: " + error);
    const fs::path marker = fs::path(directory_path) / "unexpected";
    {
        std::ofstream output(marker);
        output << "preserve me\n";
    }
    require(!directory->Close(&error) && fs::exists(marker),
            "nonempty runtime directory was removed");
    cleanup_exact(directory_path);
}

void test_substituted_entry_is_preserved()
{
    const auto config = config_for(unique_digest());
    const std::string directory_path = directory_path_for(config);
    const fs::path moved = directory_path + ".moved";
    std::string error;
    auto directory = ipc::SpatialRoiSocketRuntimeDirectory::Create(config, &error);
    require(directory != nullptr, "create failed: " + error);
    require(::rename(directory_path.c_str(), moved.c_str()) == 0,
            "could not move owned directory fixture");
    require(::mkdir(directory_path.c_str(), 0700) == 0,
            "could not create replacement directory fixture");
    require(!directory->Close(&error) && fs::exists(directory_path) &&
                fs::exists(moved),
            "substituted runtime directory was removed");
    cleanup_exact(directory_path);
    cleanup_exact(moved);
}

}  // namespace

int main()
{
    try {
        test_create_and_exact_cleanup();
        test_existing_entry_is_refused_and_preserved();
        test_identity_must_derive_exact_four_children();
        test_nonempty_directory_is_preserved();
        test_substituted_entry_is_preserved();
        std::cout << "spatial_roi_socket_runtime_directory_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "spatial_roi_socket_runtime_directory_tests: FAIL: "
                  << exception.what() << '\n';
        return 1;
    }
}
