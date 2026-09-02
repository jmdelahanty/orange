#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace orange::spatial_roi::ipc {

// Exact camera-level authority input for the four filesystem AF_UNIX
// endpoints.  Paths are deliberately not caller-supplied: they are derived
// from the authenticated recording token and plan-ordered logical stream IDs
// by the same helpers used by the recorder contract.
struct SpatialRoiSocketRuntimeDirectoryConfig {
    std::string recording_identity_token;
    std::vector<std::string> logical_stream_ids;
};

// Creates one fresh, euid-owned mode-0700 directory directly beneath /tmp.
// Existing entries are always refused.  Cleanup is scoped through a retained
// /tmp descriptor and removes the directory only when its device/inode still
// match and it is empty.  Socket leaves remain owned by their listeners.  The
// camera supervisor is the sole namespace owner: Linux has no
// inode-conditional unlinkat, so same-UID concurrent mutation is explicitly
// outside this cleanup contract.
class SpatialRoiSocketRuntimeDirectory final {
public:
    static std::unique_ptr<SpatialRoiSocketRuntimeDirectory> Create(
        const SpatialRoiSocketRuntimeDirectoryConfig& config,
        std::string* error_out = nullptr) noexcept;

    ~SpatialRoiSocketRuntimeDirectory();

    SpatialRoiSocketRuntimeDirectory(
        const SpatialRoiSocketRuntimeDirectory&) = delete;
    SpatialRoiSocketRuntimeDirectory& operator=(
        const SpatialRoiSocketRuntimeDirectory&) = delete;
    SpatialRoiSocketRuntimeDirectory(
        SpatialRoiSocketRuntimeDirectory&&) = delete;
    SpatialRoiSocketRuntimeDirectory& operator=(
        SpatialRoiSocketRuntimeDirectory&&) = delete;

    // Idempotent after success.  A false return deliberately leaves a
    // nonempty or substituted directory in place for diagnosis.
    bool Close(std::string* error_out = nullptr) noexcept;

    bool valid() const noexcept { return directory_fd_ >= 0 && owns_entry_; }
    const std::string& directory_path() const noexcept
    {
        return directory_path_;
    }
    const std::vector<std::string>& socket_paths() const noexcept
    {
        return socket_paths_;
    }
    std::uint64_t device() const noexcept { return device_; }
    std::uint64_t inode() const noexcept { return inode_; }

    // Borrowed descriptor for constructing listener endpoints without
    // re-walking the mutable absolute namespace.  It remains owned by this
    // object; listener construction must duplicate it before returning.
    int borrowed_directory_fd() const noexcept { return directory_fd_; }

private:
    SpatialRoiSocketRuntimeDirectory(
        int tmp_fd,
        int directory_fd,
        std::string directory_path,
        std::string directory_leaf,
        std::vector<std::string> socket_paths,
        std::uint64_t device,
        std::uint64_t inode) noexcept;

    static bool validate_config(
        const SpatialRoiSocketRuntimeDirectoryConfig& config,
        std::string* directory_path_out,
        std::string* leaf_out,
        std::vector<std::string>* socket_paths_out,
        std::string* error_out) noexcept;
    static bool set_error(std::string* error_out,
                          std::string_view message) noexcept;
    void close_descriptors() noexcept;

    int tmp_fd_ = -1;
    int directory_fd_ = -1;
    std::string directory_path_;
    std::string directory_leaf_;
    std::vector<std::string> socket_paths_;
    std::uint64_t device_ = 0;
    std::uint64_t inode_ = 0;
    bool owns_entry_ = false;
};

}  // namespace orange::spatial_roi::ipc
