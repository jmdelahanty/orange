#include "spatial_roi_recorder_artifact_root.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace recording = orange::spatial_roi::recording;

using recording::SpatialRoiRecorderArtifactFile;
using recording::SpatialRoiRecorderArtifactFileAccess;
using recording::SpatialRoiRecorderArtifactIdentity;
using recording::SpatialRoiRecorderArtifactRoot;
using recording::kSpatialRoiRecorderArtifactDirectory;

static_assert(!std::is_copy_constructible_v<SpatialRoiRecorderArtifactRoot>);
static_assert(!std::is_copy_assignable_v<SpatialRoiRecorderArtifactRoot>);
static_assert(std::is_move_constructible_v<SpatialRoiRecorderArtifactRoot>);
static_assert(!std::is_copy_constructible_v<SpatialRoiRecorderArtifactFile>);
static_assert(!std::is_copy_assignable_v<SpatialRoiRecorderArtifactFile>);
static_assert(std::is_move_constructible_v<SpatialRoiRecorderArtifactFile>);

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
        std::string pattern =
            "/tmp/orange_spatial_roi_artifact_root_test_XXXXXX";
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

    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

fs::path make_recording_root(const TempTree& tree,
                             const std::string& name = "recording")
{
    const fs::path root = tree.path() / name;
    require(fs::create_directory(root), "failed to create recording root");
    return root;
}

std::unique_ptr<SpatialRoiRecorderArtifactRoot> open_root(
    const fs::path& root,
    const std::vector<std::string>& allowed)
{
    std::unique_ptr<SpatialRoiRecorderArtifactRoot> result;
    std::string error;
    require(SpatialRoiRecorderArtifactRoot::Open(
                root, allowed, &result, &error),
            "artifact root open failed: " + error);
    require(result != nullptr && result->valid(),
            "artifact root open returned no valid authority");
    return result;
}

std::unique_ptr<SpatialRoiRecorderArtifactRoot> open_existing_root(
    const fs::path& root,
    const std::vector<std::string>& allowed)
{
    std::unique_ptr<SpatialRoiRecorderArtifactRoot> result;
    std::string error;
    require(SpatialRoiRecorderArtifactRoot::OpenExisting(
                root, allowed, &result, &error),
            "existing artifact root open failed: " + error);
    require(result != nullptr && result->valid(),
            "existing artifact root open returned no valid authority");
    return result;
}

void require_open_rejected(const fs::path& root,
                           const std::vector<std::string>& allowed,
                           const std::string& context)
{
    std::unique_ptr<SpatialRoiRecorderArtifactRoot> result;
    std::string error;
    require(!SpatialRoiRecorderArtifactRoot::Open(
                root, allowed, &result, &error),
            context + " was unexpectedly accepted");
    require(result == nullptr && !error.empty(),
            context + " rejection did not fail closed with a diagnostic");
}

void require_open_existing_rejected(const fs::path& root,
                                    const std::vector<std::string>& allowed,
                                    const std::string& context)
{
    std::unique_ptr<SpatialRoiRecorderArtifactRoot> result;
    std::string error;
    require(!SpatialRoiRecorderArtifactRoot::OpenExisting(
                root, allowed, &result, &error),
            context + " was unexpectedly opened");
    require(result == nullptr && !error.empty(),
            context + " rejection did not fail closed with a diagnostic");
}

std::unique_ptr<SpatialRoiRecorderArtifactFile> create_file(
    const SpatialRoiRecorderArtifactRoot& root,
    const std::string& relative_path)
{
    std::unique_ptr<SpatialRoiRecorderArtifactFile> result;
    std::string error;
    require(root.CreateFile(relative_path, &result, &error),
            "artifact creation failed: " + error);
    require(result != nullptr && result->valid(),
            "artifact creation returned no valid file");
    require(result->artifact_root_identity() == root.artifact_root_identity(),
            "created artifact did not retain its authorizing root identity");
    return result;
}

std::unique_ptr<SpatialRoiRecorderArtifactFile> open_existing_file(
    const SpatialRoiRecorderArtifactRoot& root,
    const std::string& relative_path,
    const SpatialRoiRecorderArtifactFileAccess access)
{
    std::unique_ptr<SpatialRoiRecorderArtifactFile> result;
    std::string error;
    require(root.OpenExistingFile(relative_path, access, &result, &error),
            "existing artifact open failed: " + error);
    require(result != nullptr && result->valid(),
            "existing artifact open returned no valid file");
    require(result->artifact_root_identity() == root.artifact_root_identity(),
            "adopted artifact did not retain its authorizing root identity");
    return result;
}

void require_existing_file_rejected(
    const SpatialRoiRecorderArtifactRoot& root,
    const std::string& relative_path,
    const SpatialRoiRecorderArtifactFileAccess access,
    const std::string& context)
{
    std::unique_ptr<SpatialRoiRecorderArtifactFile> result;
    std::string error;
    require(!root.OpenExistingFile(relative_path, access, &result, &error),
            context + " was unexpectedly opened");
    require(result == nullptr && !error.empty(),
            context + " rejection did not fail closed with a diagnostic");
}

void require_create_rejected(const SpatialRoiRecorderArtifactRoot& root,
                             const std::string& relative_path,
                             const std::string& context)
{
    std::unique_ptr<SpatialRoiRecorderArtifactFile> result;
    std::string error;
    require(!root.CreateFile(relative_path, &result, &error),
            context + " was unexpectedly created");
    require(result == nullptr && !error.empty(),
            context + " rejection did not fail closed with a diagnostic");
}

void write_all(const int fd, const std::string& bytes)
{
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count =
            ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        require(count > 0,
                std::string("artifact write failed: ") + std::strerror(errno));
        offset += static_cast<std::size_t>(count);
    }
}

SpatialRoiRecorderArtifactIdentity identity_for_fd(const int fd)
{
    struct stat value {};
    require(::fstat(fd, &value) == 0,
            std::string("fstat failed: ") + std::strerror(errno));
    return {static_cast<std::uint64_t>(value.st_dev),
            static_cast<std::uint64_t>(value.st_ino)};
}

int open_directory_fd(const fs::path& path)
{
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    require(fd >= 0,
            "failed to open directory descriptor for adoption: " +
                std::string(std::strerror(errno)));
    return fd;
}

int open_read_only_fd(const fs::path& path)
{
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    require(fd >= 0,
            "failed to open file descriptor for adoption: " +
                std::string(std::strerror(errno)));
    return fd;
}

void require_closed(const int fd, const std::string& context)
{
    errno = 0;
    require(::fcntl(fd, F_GETFD) < 0 && errno == EBADF,
            context + " descriptor was not closed");
}

void require_adoption_rejected(
    const int recording_root_fd,
    const int artifact_root_fd,
    const SpatialRoiRecorderArtifactIdentity recording_identity,
    const SpatialRoiRecorderArtifactIdentity artifact_identity,
    const fs::path& diagnostic_root,
    const std::vector<std::string>& allowed,
    const std::string& context)
{
    std::unique_ptr<SpatialRoiRecorderArtifactRoot> result;
    std::string error;
    require(!SpatialRoiRecorderArtifactRoot::AdoptExistingFds(
                recording_root_fd,
                artifact_root_fd,
                recording_identity,
                artifact_identity,
                diagnostic_root,
                allowed,
                &result,
                &error),
            context + " was unexpectedly accepted");
    require(result == nullptr && !error.empty(),
            context + " rejection did not fail closed with a diagnostic");
    require_closed(recording_root_fd, context + " recording-root");
    if (artifact_root_fd != recording_root_fd) {
        require_closed(artifact_root_fd, context + " artifact-root");
    }
}

void rejects_unsafe_paths_and_unlisted_names()
{
    TempTree tree;
    const fs::path root = make_recording_root(tree);
    const std::vector<std::string> invalid_paths = {
        "", "/absolute.bin", ".", "..", "dir/../escape.bin",
        "dir/./file.bin", "dir//file.bin", "dir/"};
    for (const auto& invalid : invalid_paths) {
        require_open_rejected(root,
                              {invalid},
                              "unsafe artifact path '" + invalid + "'");
    }
    require_open_rejected(root,
                          {},
                          "empty contract artifact allow-list");
    require_open_rejected(root,
                          {"same.bin", "same.bin"},
                          "duplicate contract artifact path");

    auto authority = open_root(root, {"authorized.bin"});
    require(authority->IsAllowed("authorized.bin"),
            "authorized artifact was not retained");
    require(!authority->IsAllowed("invented.bin"),
            "invented artifact appeared authorized");
    require_create_rejected(*authority,
                            "invented.bin",
                            "caller-invented artifact");
}

void rejects_recording_and_artifact_root_symlinks()
{
    TempTree tree;
    const fs::path real_root = make_recording_root(tree, "real_recording");
    const fs::path root_link = tree.path() / "recording_link";
    fs::create_directory_symlink(real_root, root_link);
    require_open_rejected(root_link,
                          {"file.bin"},
                          "symlink authoritative recording root");

    const fs::path real_parent = tree.path() / "real_parent";
    require(fs::create_directory(real_parent),
            "failed to create real recording-root parent");
    const fs::path nested_recording = real_parent / "nested_recording";
    require(fs::create_directory(nested_recording),
            "failed to create nested recording root");
    const fs::path parent_link = tree.path() / "parent_link";
    fs::create_directory_symlink(real_parent, parent_link);
    require_open_rejected(parent_link / "nested_recording",
                          {"file.bin"},
                          "intermediate recording-root symlink");

    require_open_rejected(fs::path("/"),
                          {"file.bin"},
                          "filesystem root recording authority");
    require_open_rejected(fs::path(real_root.string() + "/."),
                          {"file.bin"},
                          "dot component in recording-root authority");
    require_open_rejected(fs::path(real_root.string() + "/../real_recording"),
                          {"file.bin"},
                          "dot-dot component in recording-root authority");

    const fs::path other_root = make_recording_root(tree, "other_recording");
    const fs::path decoy = tree.path() / "decoy_artifact_root";
    require(fs::create_directory(decoy), "failed to create artifact-root decoy");
    fs::create_directory_symlink(
        decoy, other_root / kSpatialRoiRecorderArtifactDirectory);
    require_open_rejected(other_root,
                          {"file.bin"},
                          "symlink external_spatial_roi_recorder directory");
}

void rejects_intermediate_and_leaf_symlinks()
{
    TempTree tree;
    const fs::path root = make_recording_root(tree);
    auto authority =
        open_root(root, {"nested/output.bin", "leaf.bin", "existing.bin"});
    const fs::path artifact_root =
        root / kSpatialRoiRecorderArtifactDirectory;
    const fs::path outside = tree.path() / "outside";
    require(fs::create_directory(outside), "failed to create outside directory");

    fs::create_directory_symlink(outside, artifact_root / "nested");
    require_create_rejected(*authority,
                            "nested/output.bin",
                            "intermediate symlink artifact path");

    {
        std::ofstream target(outside / "target.bin", std::ios::binary);
        require(static_cast<bool>(target), "failed to create symlink target");
        target << "outside";
    }
    fs::create_symlink(outside / "target.bin", artifact_root / "leaf.bin");
    require_create_rejected(*authority,
                            "leaf.bin",
                            "leaf symlink artifact path");

    {
        std::ofstream existing(artifact_root / "existing.bin", std::ios::binary);
        require(static_cast<bool>(existing), "failed to create existing artifact");
        existing << "existing";
    }
    require_create_rejected(*authority,
                            "existing.bin",
                            "existing regular artifact");
}

void exclusive_creation_survives_concurrency()
{
    TempTree tree;
    const fs::path root = make_recording_root(tree);
    auto authority = open_root(root, {"concurrent.bin"});

    std::atomic<int> successes{0};
    std::vector<std::thread> workers;
    for (int index = 0; index < 12; ++index) {
        workers.emplace_back([&] {
            std::unique_ptr<SpatialRoiRecorderArtifactFile> file;
            std::string error;
            if (authority->CreateFile("concurrent.bin", &file, &error)) {
                require(file != nullptr && file->valid(),
                        "concurrent create returned an invalid success");
                ++successes;
            } else {
                require(file == nullptr && !error.empty(),
                        "concurrent rejection lacked a diagnostic");
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    require(successes.load() == 1,
            "exclusive artifact creation did not produce exactly one winner");
    require_create_rejected(*authority,
                            "concurrent.bin",
                            "second open of an existing artifact");
}

void retained_root_descriptor_survives_path_swap()
{
    TempTree tree;
    const fs::path root = make_recording_root(tree);
    auto authority = open_root(root, {"nested/output.bin"});
    const auto initial_root_identity = authority->recording_root_identity();

    const fs::path moved_root = tree.path() / "recording_moved";
    fs::rename(root, moved_root);
    const fs::path decoy = tree.path() / "decoy";
    require(fs::create_directory(decoy), "failed to create swapped-root decoy");
    fs::create_directory_symlink(decoy, root);

    auto file = create_file(*authority, "nested/output.bin");
    write_all(file->borrowed_fd(), "bound-to-original-root");
    std::string error;
    require(file->Seal(&error),
            "file under renamed root did not seal: " + error);
    require(fs::is_regular_file(
                moved_root / kSpatialRoiRecorderArtifactDirectory /
                "nested/output.bin"),
            "retained descriptor did not create under the original root inode");
    require(!fs::exists(decoy / kSpatialRoiRecorderArtifactDirectory /
                        "nested/output.bin"),
            "path swap redirected artifact creation into the decoy root");
    require(identity_for_fd(authority->borrowed_recording_root_fd()) ==
                initial_root_identity,
            "retained recording-root descriptor identity changed after rename");
}

void leaf_unlink_and_replacement_is_detected()
{
    TempTree tree;
    const fs::path root = make_recording_root(tree);
    auto authority = open_root(root, {"replace.bin"});
    auto file = create_file(*authority, "replace.bin");
    write_all(file->borrowed_fd(), "original");
    const auto original_identity = file->identity();

    const fs::path leaf =
        root / kSpatialRoiRecorderArtifactDirectory / "replace.bin";
    require(::unlink(leaf.c_str()) == 0,
            std::string("failed to unlink artifact leaf: ") +
                std::strerror(errno));
    const int replacement_fd =
        ::open(leaf.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    require(replacement_fd >= 0,
            std::string("failed to create replacement leaf: ") +
                std::strerror(errno));
    write_all(replacement_fd, "replacement");
    const auto replacement_identity = identity_for_fd(replacement_fd);
    require(::close(replacement_fd) == 0, "failed to close replacement leaf");
    require(replacement_identity != original_identity,
            "replacement unexpectedly reused the still-open original inode");

    std::string error;
    require(!file->Seal(&error),
            "replaced artifact leaf was incorrectly sealed");
    require(!file->sealed() && !error.empty(),
            "replacement rejection lacked a diagnostic or marked the file sealed");
}

void duplicated_descriptors_and_durable_seal_work()
{
    TempTree tree;
    const fs::path root = make_recording_root(tree);
    auto authority_pointer = open_root(root, {"one/two/artifact.bin"});
    SpatialRoiRecorderArtifactRoot authority(
        std::move(*authority_pointer));
    require(authority.valid() && !authority_pointer->valid(),
            "artifact-root move did not transfer descriptors");

    int duplicated_root_fd = -1;
    int duplicated_artifact_root_fd = -1;
    std::string error;
    require(authority.DuplicateRecordingRootFd(&duplicated_root_fd, &error),
            "recording-root duplicate failed: " + error);
    require(authority.DuplicateArtifactRootFd(
                &duplicated_artifact_root_fd, &error),
            "artifact-root duplicate failed: " + error);
    require((::fcntl(duplicated_root_fd, F_GETFD) & FD_CLOEXEC) != 0 &&
                (::fcntl(duplicated_artifact_root_fd, F_GETFD) & FD_CLOEXEC) != 0,
            "directory duplicates are not close-on-exec");
    require(identity_for_fd(duplicated_root_fd) ==
                authority.recording_root_identity() &&
                identity_for_fd(duplicated_artifact_root_fd) ==
                    authority.artifact_root_identity(),
            "duplicated directory descriptor identity changed");
    require(::close(duplicated_root_fd) == 0 &&
                ::close(duplicated_artifact_root_fd) == 0,
            "failed to close duplicated directory descriptors");

    auto file_pointer = create_file(authority, "one/two/artifact.bin");
    SpatialRoiRecorderArtifactFile file(std::move(*file_pointer));
    require(file.valid() && !file_pointer->valid(),
            "artifact-file move did not transfer descriptors");
    int duplicate = -1;
    require(file.DuplicateFd(&duplicate, &error),
            "artifact duplicate failed: " + error);
    require((::fcntl(duplicate, F_GETFD) & FD_CLOEXEC) != 0,
            "artifact duplicate is not close-on-exec");
    write_all(duplicate, "durable artifact bytes");
    require(::close(duplicate) == 0,
            "failed to close artifact writer duplicate");
    require(file.Seal(&error), "artifact seal failed: " + error);
    require(file.sealed(), "successful artifact seal was not retained");
    require(file.Seal(&error), "idempotent artifact reseal failed: " + error);

    const fs::path output =
        root / kSpatialRoiRecorderArtifactDirectory / "one/two/artifact.bin";
    std::ifstream input(output, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
    require(bytes == "durable artifact bytes",
            "sealed artifact contents changed");
}

void existing_adoption_never_creates_and_honors_access()
{
    TempTree tree;
    const fs::path missing_recording = tree.path() / "missing_recording";
    require_open_existing_rejected(missing_recording,
                                   {"artifact.bin"},
                                   "missing authoritative recording root");
    require(!fs::exists(missing_recording),
            "OpenExisting created a missing recording root");

    const fs::path root = make_recording_root(tree);
    require_open_existing_rejected(root,
                                   {"artifact.bin"},
                                   "missing recorder artifact directory");
    const fs::path artifact_root =
        root / kSpatialRoiRecorderArtifactDirectory;
    require(!fs::exists(artifact_root),
            "OpenExisting created the recorder artifact directory");

    const std::vector<std::string> allowed = {
        "artifact.bin", "missing.bin", "missing_dir/missing.bin",
        "link.bin", "linked_dir/file.bin"};
    {
        auto creating_authority = open_root(root, allowed);
        auto created = create_file(*creating_authority, "artifact.bin");
        write_all(created->borrowed_fd(), "adopted bytes");
        std::string error;
        require(created->Seal(&error),
                "fixture artifact seal failed: " + error);
    }

    auto authority = open_existing_root(root, allowed);
    auto read_only = open_existing_file(
        *authority,
        "artifact.bin",
        SpatialRoiRecorderArtifactFileAccess::kReadOnly);
    require(read_only->access() ==
                SpatialRoiRecorderArtifactFileAccess::kReadOnly,
            "read-only adoption access was not retained");
    const int read_only_flags = ::fcntl(read_only->borrowed_fd(), F_GETFL);
    require(read_only_flags >= 0 &&
                (read_only_flags & O_ACCMODE) == O_RDONLY,
            "read-only adoption descriptor has write access");
    std::string error;
    require(read_only->VerifyCurrentBinding(&error),
            "read-only adoption binding check failed: " + error);

    auto read_write = open_existing_file(
        *authority,
        "artifact.bin",
        SpatialRoiRecorderArtifactFileAccess::kReadWrite);
    require(read_write->access() ==
                SpatialRoiRecorderArtifactFileAccess::kReadWrite,
            "read-write adoption access was not retained");
    const int read_write_flags = ::fcntl(read_write->borrowed_fd(), F_GETFL);
    require(read_write_flags >= 0 &&
                (read_write_flags & O_ACCMODE) == O_RDWR,
            "read-write adoption descriptor is not read-write");

    require_existing_file_rejected(
        *authority,
        "missing.bin",
        SpatialRoiRecorderArtifactFileAccess::kReadOnly,
        "missing authorized leaf");
    require(!fs::exists(artifact_root / "missing.bin"),
            "OpenExistingFile created a missing leaf");
    require_existing_file_rejected(
        *authority,
        "missing_dir/missing.bin",
        SpatialRoiRecorderArtifactFileAccess::kReadWrite,
        "missing authorized intermediate directory");
    require(!fs::exists(artifact_root / "missing_dir"),
            "OpenExistingFile created a missing intermediate directory");

    const fs::path outside = tree.path() / "outside.bin";
    {
        std::ofstream output(outside, std::ios::binary);
        require(static_cast<bool>(output), "failed to create adoption decoy");
        output << "outside";
    }
    fs::create_symlink(outside, artifact_root / "link.bin");
    require_existing_file_rejected(
        *authority,
        "link.bin",
        SpatialRoiRecorderArtifactFileAccess::kReadOnly,
        "existing artifact symlink");

    const fs::path outside_directory = tree.path() / "outside_directory";
    require(fs::create_directory(outside_directory),
            "failed to create adoption directory decoy");
    {
        std::ofstream output(outside_directory / "file.bin", std::ios::binary);
        require(static_cast<bool>(output),
                "failed to create adoption directory decoy leaf");
        output << "outside directory";
    }
    fs::create_directory_symlink(outside_directory,
                                 artifact_root / "linked_dir");
    require_existing_file_rejected(
        *authority,
        "linked_dir/file.bin",
        SpatialRoiRecorderArtifactFileAccess::kReadOnly,
        "existing artifact intermediate symlink");
}

void adopted_leaf_replacement_is_detected()
{
    TempTree tree;
    const fs::path root = make_recording_root(tree);
    {
        auto creating_authority = open_root(root, {"adopt.bin"});
        auto created = create_file(*creating_authority, "adopt.bin");
        write_all(created->borrowed_fd(), "original");
    }
    auto authority = open_existing_root(root, {"adopt.bin"});
    auto adopted = open_existing_file(
        *authority,
        "adopt.bin",
        SpatialRoiRecorderArtifactFileAccess::kReadOnly);
    std::string error;
    require(adopted->VerifyCurrentBinding(&error),
            "initial adopted binding did not verify: " + error);

    const fs::path leaf =
        root / kSpatialRoiRecorderArtifactDirectory / "adopt.bin";
    require(::unlink(leaf.c_str()) == 0,
            std::string("failed to unlink adopted artifact: ") +
                std::strerror(errno));
    const int replacement_fd =
        ::open(leaf.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    require(replacement_fd >= 0,
            std::string("failed to replace adopted artifact: ") +
                std::strerror(errno));
    write_all(replacement_fd, "replacement");
    require(::close(replacement_fd) == 0,
            "failed to close adopted-artifact replacement");

    error.clear();
    require(!adopted->VerifyCurrentBinding(&error),
            "adopted artifact replacement was not detected");
    require(!error.empty(),
            "adopted artifact replacement lacked a diagnostic");
}

void adopts_inherited_descriptors_without_path_reopen()
{
    TempTree tree;
    const fs::path root = make_recording_root(tree);
    auto creating_authority = open_root(root, {"adopted.bin"});
    creating_authority.reset();

    const int recording_fd = open_directory_fd(root);
    const int artifact_fd = open_directory_fd(
        root / kSpatialRoiRecorderArtifactDirectory);
    const auto recording_identity = identity_for_fd(recording_fd);
    const auto artifact_identity = identity_for_fd(artifact_fd);

    std::unique_ptr<SpatialRoiRecorderArtifactRoot> adopted;
    std::string error;
    require(SpatialRoiRecorderArtifactRoot::AdoptExistingFds(
                recording_fd,
                artifact_fd,
                recording_identity,
                artifact_identity,
                root,
                {"adopted.bin"},
                &adopted,
                &error),
            "inherited descriptor adoption failed: " + error);
    require(adopted != nullptr && adopted->valid(),
            "inherited descriptor adoption returned no authority");
    require((::fcntl(adopted->borrowed_recording_root_fd(), F_GETFD) &
             FD_CLOEXEC) != 0 &&
                (::fcntl(adopted->borrowed_artifact_root_fd(), F_GETFD) &
                 FD_CLOEXEC) != 0,
            "adopted descriptors are not close-on-exec");
    require(adopted->recording_root_identity() == recording_identity &&
                adopted->artifact_root_identity() == artifact_identity,
            "adopted descriptors did not retain exact identities");
    auto file = create_file(*adopted, "adopted.bin");
    write_all(file->borrowed_fd(), "inherited authority");
}

void adoption_uses_descriptor_identity_after_path_swap()
{
    TempTree tree;
    const fs::path root = make_recording_root(tree);
    auto creating_authority = open_root(root, {"moved.bin"});
    creating_authority.reset();

    const int recording_fd = open_directory_fd(root);
    const int artifact_fd = open_directory_fd(
        root / kSpatialRoiRecorderArtifactDirectory);
    const auto recording_identity = identity_for_fd(recording_fd);
    const auto artifact_identity = identity_for_fd(artifact_fd);

    const fs::path moved_root = tree.path() / "moved_recording";
    std::error_code rename_error;
    fs::rename(root, moved_root, rename_error);
    require(!rename_error, "failed to move opened recording root: " +
                              rename_error.message());
    const fs::path decoy = tree.path() / "decoy_recording";
    require(fs::create_directory(decoy), "failed to create swapped-root decoy");
    fs::create_directory_symlink(decoy, root);

    std::unique_ptr<SpatialRoiRecorderArtifactRoot> adopted;
    std::string error;
    require(SpatialRoiRecorderArtifactRoot::AdoptExistingFds(
                recording_fd,
                artifact_fd,
                recording_identity,
                artifact_identity,
                root,
                {"moved.bin"},
                &adopted,
                &error),
            "descriptor adoption followed swapped diagnostic path: " + error);
    auto file = create_file(*adopted, "moved.bin");
    write_all(file->borrowed_fd(), "descriptor-bound");
    require(fs::is_regular_file(
                moved_root / kSpatialRoiRecorderArtifactDirectory / "moved.bin"),
            "adopted authority did not remain bound to opened recording root");
    require(!fs::exists(decoy / kSpatialRoiRecorderArtifactDirectory /
                        "moved.bin"),
            "adopted authority followed swapped diagnostic path");
}

void adoption_rejects_wrong_identity_and_descriptor_types()
{
    TempTree tree;
    const fs::path root = make_recording_root(tree);
    auto creating_authority = open_root(root, {"identity.bin"});
    creating_authority.reset();
    const fs::path decoy_root = make_recording_root(tree, "decoy_recording");

    {
        const int recording_fd = open_directory_fd(root);
        const int artifact_fd = open_directory_fd(
            root / kSpatialRoiRecorderArtifactDirectory);
        const int decoy_fd = open_directory_fd(decoy_root);
        const auto decoy_identity = identity_for_fd(decoy_fd);
        const auto artifact_identity = identity_for_fd(artifact_fd);
        require(::close(decoy_fd) == 0, "failed to close decoy identity descriptor");
        require_adoption_rejected(recording_fd,
                                  artifact_fd,
                                  decoy_identity,
                                  artifact_identity,
                                  root,
                                  {"identity.bin"},
                                  "wrong recording identity");
    }

    {
        const int recording_fd = open_directory_fd(root);
        const fs::path regular = tree.path() / "not_a_directory";
        {
            std::ofstream output(regular, std::ios::binary);
            require(static_cast<bool>(output), "failed to create non-directory fixture");
            output << "not a directory";
        }
        const int artifact_fd = open_read_only_fd(regular);
        const auto recording_identity = identity_for_fd(recording_fd);
        const auto artifact_identity = identity_for_fd(artifact_fd);
        require_adoption_rejected(recording_fd,
                                  artifact_fd,
                                  recording_identity,
                                  artifact_identity,
                                  root,
                                  {"identity.bin"},
                                  "non-directory artifact descriptor");
    }
}

void adoption_rejects_unrelated_child_and_bad_allow_list()
{
    TempTree tree;
    const fs::path root = make_recording_root(tree);
    auto creating_authority = open_root(root, {"allow.bin"});
    creating_authority.reset();
    const fs::path unrelated = tree.path() / "unrelated_artifact_root";
    require(fs::create_directory(unrelated), "failed to create unrelated directory");

    {
        const int recording_fd = open_directory_fd(root);
        const int artifact_fd = open_directory_fd(unrelated);
        require_adoption_rejected(recording_fd,
                                  artifact_fd,
                                  identity_for_fd(recording_fd),
                                  identity_for_fd(artifact_fd),
                                  root,
                                  {"allow.bin"},
                                  "unrelated artifact descriptor");
    }

    {
        const fs::path symlink_root = make_recording_root(tree, "symlink_recording");
        const fs::path symlink_target = tree.path() / "symlink_target";
        require(fs::create_directory(symlink_target),
                "failed to create symlink artifact target");
        fs::create_directory_symlink(
            symlink_target,
            symlink_root / kSpatialRoiRecorderArtifactDirectory);
        const int recording_fd = open_directory_fd(symlink_root);
        const int artifact_fd = open_directory_fd(symlink_target);
        const auto recording_identity = identity_for_fd(recording_fd);
        const auto artifact_identity = identity_for_fd(artifact_fd);
        require_adoption_rejected(recording_fd,
                                  artifact_fd,
                                  recording_identity,
                                  artifact_identity,
                                  symlink_root,
                                  {"allow.bin"},
                                  "symlink artifact child");
    }

    {
        const int recording_fd = open_directory_fd(root);
        const int artifact_fd = open_directory_fd(
            root / kSpatialRoiRecorderArtifactDirectory);
        require_adoption_rejected(recording_fd,
                                  artifact_fd,
                                  identity_for_fd(recording_fd),
                                  identity_for_fd(artifact_fd),
                                  root,
                                  {"allow.bin", "allow.bin"},
                                  "duplicate adoption allow-list");
    }

    {
        const int shared_fd = open_directory_fd(root);
        require_adoption_rejected(shared_fd,
                                  shared_fd,
                                  identity_for_fd(shared_fd),
                                  identity_for_fd(shared_fd),
                                  root,
                                  {"allow.bin"},
                                  "shared descriptor arguments");
    }
}

}  // namespace

int main()
{
    try {
        rejects_unsafe_paths_and_unlisted_names();
        rejects_recording_and_artifact_root_symlinks();
        rejects_intermediate_and_leaf_symlinks();
        exclusive_creation_survives_concurrency();
        retained_root_descriptor_survives_path_swap();
        leaf_unlink_and_replacement_is_detected();
        duplicated_descriptors_and_durable_seal_work();
        existing_adoption_never_creates_and_honors_access();
        adopted_leaf_replacement_is_detected();
        adopts_inherited_descriptors_without_path_reopen();
        adoption_uses_descriptor_identity_after_path_swap();
        adoption_rejects_wrong_identity_and_descriptor_types();
        adoption_rejects_unrelated_child_and_bad_allow_list();
        std::cout << "spatial_roi_recorder_artifact_root_tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "[FAIL] " << exception.what() << "\n";
        return 1;
    }
}
