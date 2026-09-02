#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace orange::spatial_roi::recording {

inline constexpr const char* kSpatialRoiRecorderArtifactDirectory =
    "external_spatial_roi_recorder";
inline constexpr std::size_t kSpatialRoiRecorderArtifactMaxPathBytes = 1024;
inline constexpr std::size_t kSpatialRoiRecorderArtifactMaxComponentBytes = 255;
inline constexpr std::size_t kSpatialRoiRecorderArtifactMaxComponents = 32;
inline constexpr std::size_t kSpatialRoiRecorderArtifactMaxCount = 512;

struct SpatialRoiRecorderArtifactIdentity {
    std::uint64_t device = 0;
    std::uint64_t inode = 0;

    bool operator==(const SpatialRoiRecorderArtifactIdentity& other) const noexcept
    {
        return device == other.device && inode == other.inode;
    }

    bool operator!=(const SpatialRoiRecorderArtifactIdentity& other) const noexcept
    {
        return !(*this == other);
    }
};

enum class SpatialRoiRecorderArtifactFileAccess {
    kReadOnly,
    kReadWrite,
};

// One exclusively created regular artifact beneath the recorder artifact
// directory. The descriptor and the leaf's original (device, inode) identity
// remain owned for the lifetime of this object. Callers may borrow or duplicate
// the descriptor for libraries that can write an already-authorized file.
class SpatialRoiRecorderArtifactFile final {
public:
    ~SpatialRoiRecorderArtifactFile();

    SpatialRoiRecorderArtifactFile(
        const SpatialRoiRecorderArtifactFile&) = delete;
    SpatialRoiRecorderArtifactFile& operator=(
        const SpatialRoiRecorderArtifactFile&) = delete;
    SpatialRoiRecorderArtifactFile(SpatialRoiRecorderArtifactFile&& other) noexcept;
    SpatialRoiRecorderArtifactFile& operator=(
        SpatialRoiRecorderArtifactFile&& other) noexcept;

    bool valid() const noexcept { return file_fd_ >= 0; }
    int borrowed_fd() const noexcept { return file_fd_; }
    const std::string& relative_path() const noexcept { return relative_path_; }
    const SpatialRoiRecorderArtifactIdentity& identity() const noexcept
    {
        return identity_;
    }
    const SpatialRoiRecorderArtifactIdentity& artifact_root_identity() const noexcept
    {
        return artifact_root_identity_;
    }
    SpatialRoiRecorderArtifactFileAccess access() const noexcept
    {
        return access_;
    }
    bool sealed() const noexcept { return sealed_; }

    // Returns a new close-on-exec descriptor for the same open file
    // description. Ownership of the returned descriptor transfers to caller.
    bool DuplicateFd(int* fd_out, std::string* error_out = nullptr) const;

    // Verify that the retained parent directory still maps the authorized
    // leaf name to the exact regular-file inode opened by this object.
    bool VerifyCurrentBinding(std::string* error_out = nullptr) const;

    // After all writer duplicates have been quiesced, verify that the current
    // directory entry still names this regular file, fsync the file, recheck
    // the binding, and fsync its parent directory. This establishes durable
    // namespace binding; it does not make a regular file kernel-immutable.
    bool Seal(std::string* error_out = nullptr);

private:
    friend class SpatialRoiRecorderArtifactRoot;

    SpatialRoiRecorderArtifactFile(int file_fd,
                                   int parent_fd,
                                   std::string relative_path,
                                   std::string leaf_name,
                                   SpatialRoiRecorderArtifactIdentity identity,
                                   SpatialRoiRecorderArtifactIdentity artifact_root_identity,
                                   SpatialRoiRecorderArtifactFileAccess access);

    void Reset() noexcept;

    int file_fd_ = -1;
    int parent_fd_ = -1;
    std::string relative_path_;
    std::string leaf_name_;
    SpatialRoiRecorderArtifactIdentity identity_;
    SpatialRoiRecorderArtifactIdentity artifact_root_identity_;
    SpatialRoiRecorderArtifactFileAccess access_ =
        SpatialRoiRecorderArtifactFileAccess::kReadOnly;
    bool sealed_ = false;
};

// Descriptor-relative authority for exactly one recording root and its exact
// external_spatial_roi_recorder child. `allowed_relative_paths` must come from
// an already verified recorder contract. Paths are retained as an exact
// allow-list; CreateFile never accepts a caller-invented output name.
class SpatialRoiRecorderArtifactRoot final {
public:
    static bool Open(
        const std::filesystem::path& authoritative_recording_root,
        const std::vector<std::string>& allowed_relative_paths,
        std::unique_ptr<SpatialRoiRecorderArtifactRoot>* root_out,
        std::string* error_out = nullptr);

    // Recovery/adoption entry point. Both the recording root and exact
    // external_spatial_roi_recorder child must already exist; this call never
    // creates a directory.
    static bool OpenExisting(
        const std::filesystem::path& authoritative_recording_root,
        const std::vector<std::string>& allowed_relative_paths,
        std::unique_ptr<SpatialRoiRecorderArtifactRoot>* root_out,
        std::string* error_out = nullptr);

    // Adopt two descriptors inherited from a trusted recorder/supervisor. The
    // factory takes ownership of both descriptors as soon as it is called:
    // every rejected descriptor is closed exactly once, and successful
    // descriptors are closed by the returned root. The descriptors are never
    // reopened through diagnostic_recording_root; that path is retained only
    // for diagnostics and metadata. On success FD_CLOEXEC is set on both
    // adopted descriptors (and they remain owned by the returned root).
    //
    // The expected identities must be the (device, inode) pairs obtained from
    // the descriptors by the trusted hand-off authority. The artifact
    // descriptor must also be the exact non-symlink
    // external_spatial_roi_recorder child of recording_root_fd.
    static bool AdoptExistingFds(
        int recording_root_fd,
        int artifact_root_fd,
        SpatialRoiRecorderArtifactIdentity expected_recording_root_identity,
        SpatialRoiRecorderArtifactIdentity expected_artifact_root_identity,
        const std::filesystem::path& diagnostic_recording_root,
        const std::vector<std::string>& allowed_relative_paths,
        std::unique_ptr<SpatialRoiRecorderArtifactRoot>* root_out,
        std::string* error_out = nullptr);

    ~SpatialRoiRecorderArtifactRoot();

    SpatialRoiRecorderArtifactRoot(
        const SpatialRoiRecorderArtifactRoot&) = delete;
    SpatialRoiRecorderArtifactRoot& operator=(
        const SpatialRoiRecorderArtifactRoot&) = delete;
    SpatialRoiRecorderArtifactRoot(SpatialRoiRecorderArtifactRoot&& other) noexcept;
    SpatialRoiRecorderArtifactRoot& operator=(
        SpatialRoiRecorderArtifactRoot&& other) noexcept;

    bool valid() const noexcept
    {
        return recording_root_fd_ >= 0 && artifact_root_fd_ >= 0;
    }
    int borrowed_recording_root_fd() const noexcept { return recording_root_fd_; }
    int borrowed_artifact_root_fd() const noexcept { return artifact_root_fd_; }
    const SpatialRoiRecorderArtifactIdentity& recording_root_identity() const noexcept
    {
        return recording_root_identity_;
    }
    const SpatialRoiRecorderArtifactIdentity& artifact_root_identity() const noexcept
    {
        return artifact_root_identity_;
    }
    const std::filesystem::path& opened_recording_root() const noexcept
    {
        return opened_recording_root_;
    }

    bool DuplicateRecordingRootFd(int* fd_out,
                                  std::string* error_out = nullptr) const;
    bool DuplicateArtifactRootFd(int* fd_out,
                                 std::string* error_out = nullptr) const;

    // Creates a contract-authorized leaf exactly once. Intermediate
    // directories are created/opened from retained directory descriptors and
    // every existing component is required to be a non-symlink directory.
    bool CreateFile(
        const std::string& allowed_relative_path,
        std::unique_ptr<SpatialRoiRecorderArtifactFile>* file_out,
        std::string* error_out = nullptr) const;

    // Opens one existing allow-listed regular artifact without creating any
    // intermediate directory or leaf. Access is explicit because adoption
    // readers must not accidentally gain write authority.
    bool OpenExistingFile(
        const std::string& allowed_relative_path,
        SpatialRoiRecorderArtifactFileAccess access,
        std::unique_ptr<SpatialRoiRecorderArtifactFile>* file_out,
        std::string* error_out = nullptr) const;

    bool IsAllowed(const std::string& relative_path) const noexcept;

private:
    SpatialRoiRecorderArtifactRoot(
        int recording_root_fd,
        int artifact_root_fd,
        std::filesystem::path opened_recording_root,
        SpatialRoiRecorderArtifactIdentity recording_root_identity,
        SpatialRoiRecorderArtifactIdentity artifact_root_identity,
        std::set<std::string> allowed_relative_paths);

    static bool OpenImpl(
        const std::filesystem::path& authoritative_recording_root,
        const std::vector<std::string>& allowed_relative_paths,
        bool create_artifact_directory,
        std::unique_ptr<SpatialRoiRecorderArtifactRoot>* root_out,
        std::string* error_out);

    static bool AdoptExistingFdsImpl(
        int recording_root_fd,
        int artifact_root_fd,
        SpatialRoiRecorderArtifactIdentity expected_recording_root_identity,
        SpatialRoiRecorderArtifactIdentity expected_artifact_root_identity,
        const std::filesystem::path& diagnostic_recording_root,
        const std::vector<std::string>& allowed_relative_paths,
        std::unique_ptr<SpatialRoiRecorderArtifactRoot>* root_out,
        std::string* error_out);

    void Reset() noexcept;

    int recording_root_fd_ = -1;
    int artifact_root_fd_ = -1;
    std::filesystem::path opened_recording_root_;
    SpatialRoiRecorderArtifactIdentity recording_root_identity_;
    SpatialRoiRecorderArtifactIdentity artifact_root_identity_;
    std::set<std::string> allowed_relative_paths_;
};

}  // namespace orange::spatial_roi::recording
