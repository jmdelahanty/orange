#pragma once

#include "json.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace orange::session::spatial_roi {

inline constexpr const char* kSpatialRoiSessionAuthorityCanonicalization =
    "canonical_json_utf8_sort_keys_compact_v1";

struct SpatialRoiSessionAuthorityIdentity final {
    std::uint64_t device = 0;
    std::uint64_t inode = 0;

    bool operator==(const SpatialRoiSessionAuthorityIdentity& other) const noexcept
    {
        return device == other.device && inode == other.inode;
    }
    bool operator!=(const SpatialRoiSessionAuthorityIdentity& other) const noexcept
    {
        return !(*this == other);
    }
};

// The path is relative to the recording root. Its digest is always computed
// from the exact bytes that were read back from the authorized descriptor.
struct SpatialRoiSessionAuthorityReceipt final {
    std::string relative_path;
    std::uint64_t size_bytes = 0;
    std::string sha256;

    bool operator==(const SpatialRoiSessionAuthorityReceipt& other) const noexcept
    {
        return relative_path == other.relative_path &&
            size_bytes == other.size_bytes && sha256 == other.sha256;
    }
    bool operator!=(const SpatialRoiSessionAuthorityReceipt& other) const noexcept
    {
        return !(*this == other);
    }

    nlohmann::json ToJson() const;
};

class SpatialRoiSessionAuthorityStore final {
public:
    static bool OpenExisting(
        const std::filesystem::path& recording_root,
        const std::vector<std::string>& allowed_relative_paths,
        std::unique_ptr<SpatialRoiSessionAuthorityStore>* store_out,
        std::string* error_out = nullptr);

    // Creates missing root components with mkdirat(0700), but opens every
    // existing component with O_NOFOLLOW and never changes existing modes.
    static bool OpenOrCreate(
        const std::filesystem::path& recording_root,
        const std::vector<std::string>& allowed_relative_paths,
        std::unique_ptr<SpatialRoiSessionAuthorityStore>* store_out,
        std::string* error_out = nullptr);

    ~SpatialRoiSessionAuthorityStore();

    SpatialRoiSessionAuthorityStore(
        const SpatialRoiSessionAuthorityStore&) = delete;
    SpatialRoiSessionAuthorityStore& operator=(
        const SpatialRoiSessionAuthorityStore&) = delete;
    SpatialRoiSessionAuthorityStore(
        SpatialRoiSessionAuthorityStore&& other) noexcept;
    SpatialRoiSessionAuthorityStore& operator=(
        SpatialRoiSessionAuthorityStore&& other) noexcept;

    bool valid() const noexcept { return recording_root_fd_ >= 0; }
    int borrowed_recording_root_fd() const noexcept
    {
        return recording_root_fd_;
    }
    const std::filesystem::path& recording_root() const noexcept
    {
        return recording_root_path_;
    }
    const SpatialRoiSessionAuthorityIdentity& recording_root_identity() const noexcept
    {
        return recording_root_identity_;
    }
    bool IsAllowed(const std::string& relative_path) const noexcept;

    // Writes one exact flat authority file without replacing any existing
    // directory entry. An identical retry returns its verified receipt;
    // different bytes, aliases, symlinks, or races fail closed.
    bool PublishBytes(
        const std::string& relative_path,
        const std::string& bytes,
        SpatialRoiSessionAuthorityReceipt* receipt_out,
        std::string* error_out = nullptr) const;

    // Canonicalizes the JSON using dump()'s compact UTF-8 representation and
    // publishes those exact bytes through PublishBytes.
    bool PublishJson(
        const std::string& relative_path,
        const nlohmann::json& value,
        SpatialRoiSessionAuthorityReceipt* receipt_out,
        std::string* error_out = nullptr) const;

    // Reopens and verifies one allow-listed authority file against a receipt.
    // The returned bytes are the exact bytes read from the verified fd.
    bool ReadAndVerify(
        const SpatialRoiSessionAuthorityReceipt& expected,
        std::string* bytes_out,
        SpatialRoiSessionAuthorityReceipt* verified_out = nullptr,
        std::string* error_out = nullptr) const;

    bool VerifyRootBinding(std::string* error_out = nullptr) const;

private:
    SpatialRoiSessionAuthorityStore(
        int recording_root_fd,
        std::filesystem::path recording_root_path,
        SpatialRoiSessionAuthorityIdentity recording_root_identity,
        std::set<std::string> allowed_relative_paths);

    static bool OpenImpl(
        const std::filesystem::path& recording_root,
        const std::vector<std::string>& allowed_relative_paths,
        bool create_missing,
        std::unique_ptr<SpatialRoiSessionAuthorityStore>* store_out,
        std::string* error_out);

    void Reset() noexcept;

    int recording_root_fd_ = -1;
    std::filesystem::path recording_root_path_;
    SpatialRoiSessionAuthorityIdentity recording_root_identity_;
    std::set<std::string> allowed_relative_paths_;
};

}  // namespace orange::session::spatial_roi
