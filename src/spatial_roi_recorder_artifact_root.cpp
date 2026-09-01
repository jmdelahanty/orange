#include "spatial_roi_recorder_artifact_root.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>

namespace orange::spatial_roi::recording {
namespace {

bool fail(std::string* error_out, const std::string& message)
{
    if (error_out != nullptr) {
        *error_out = message;
    }
    return false;
}

std::string errno_message(const std::string& operation, const int error_number)
{
    std::ostringstream stream;
    stream << operation << ": " << std::strerror(error_number)
           << " (errno=" << error_number << ")";
    return stream.str();
}

void clear_error(std::string* error_out)
{
    if (error_out != nullptr) {
        error_out->clear();
    }
}

void close_fd(int* fd) noexcept
{
    if (fd != nullptr && *fd >= 0) {
        const int saved_errno = errno;
        // On Linux an EINTR return still consumes the descriptor. Retrying can
        // accidentally close a descriptor that another thread has since
        // acquired with the same number.
        (void)::close(*fd);
        *fd = -1;
        errno = saved_errno;
    }
}

bool duplicate_fd(const int fd,
                  const std::string& description,
                  int* fd_out,
                  std::string* error_out)
{
    clear_error(error_out);
    if (fd_out == nullptr) {
        return fail(error_out, description + " duplicate output is null");
    }
    *fd_out = -1;
    if (fd < 0) {
        return fail(error_out, description + " is not open");
    }
    int duplicate = -1;
    do {
        duplicate = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
    } while (duplicate < 0 && errno == EINTR);
    if (duplicate < 0) {
        return fail(error_out,
                    errno_message("failed to duplicate " + description, errno));
    }
    *fd_out = duplicate;
    return true;
}

bool identity_from_stat(const struct stat& value,
                        SpatialRoiRecorderArtifactIdentity* identity_out,
                        std::string* error_out)
{
    if (identity_out == nullptr) {
        return fail(error_out, "artifact identity output is null");
    }
    if (value.st_ino == 0) {
        return fail(error_out, "artifact has an invalid filesystem identity");
    }
    identity_out->device = static_cast<std::uint64_t>(value.st_dev);
    identity_out->inode = static_cast<std::uint64_t>(value.st_ino);
    return true;
}

bool regular_identity_for_fd(const int fd,
                             SpatialRoiRecorderArtifactIdentity* identity_out,
                             std::string* error_out)
{
    struct stat value {};
    if (::fstat(fd, &value) != 0) {
        return fail(error_out,
                    errno_message("failed to stat artifact file descriptor", errno));
    }
    if (!S_ISREG(value.st_mode)) {
        return fail(error_out, "artifact file descriptor is not a regular file");
    }
    return identity_from_stat(value, identity_out, error_out);
}

bool directory_identity_for_fd(const int fd,
                               const std::string& description,
                               SpatialRoiRecorderArtifactIdentity* identity_out,
                               std::string* error_out)
{
    struct stat value {};
    if (::fstat(fd, &value) != 0) {
        return fail(error_out,
                    errno_message("failed to stat " + description, errno));
    }
    if (!S_ISDIR(value.st_mode)) {
        return fail(error_out, description + " is not a directory");
    }
    return identity_from_stat(value, identity_out, error_out);
}

bool split_safe_relative_path(const std::string& path,
                              std::vector<std::string>* components_out,
                              std::string* error_out)
{
    if (components_out == nullptr) {
        return fail(error_out, "artifact path component output is null");
    }
    components_out->clear();
    if (path.empty()) {
        return fail(error_out, "artifact relative path is empty");
    }
    if (path.size() > kSpatialRoiRecorderArtifactMaxPathBytes) {
        return fail(error_out, "artifact relative path exceeds the byte bound");
    }
    if (path.front() == '/') {
        return fail(error_out, "artifact path must be relative");
    }
    if (path.find('\0') != std::string::npos) {
        return fail(error_out, "artifact path contains a NUL byte");
    }

    std::size_t begin = 0;
    while (begin <= path.size()) {
        const std::size_t separator = path.find('/', begin);
        const std::size_t end = separator == std::string::npos
                                    ? path.size()
                                    : separator;
        if (end == begin) {
            return fail(error_out,
                        "artifact path contains an empty component");
        }
        const std::string component = path.substr(begin, end - begin);
        if (component == "." || component == "..") {
            return fail(error_out,
                        "artifact path contains a dot traversal component");
        }
        if (component.size() > kSpatialRoiRecorderArtifactMaxComponentBytes) {
            return fail(error_out,
                        "artifact path component exceeds the byte bound");
        }
        components_out->push_back(component);
        if (components_out->size() > kSpatialRoiRecorderArtifactMaxComponents) {
            return fail(error_out,
                        "artifact path exceeds the component-count bound");
        }
        if (separator == std::string::npos) {
            break;
        }
        begin = separator + 1;
    }
    return true;
}

bool fsync_fd(const int fd,
              const std::string& description,
              std::string* error_out)
{
    int result = -1;
    do {
        result = ::fsync(fd);
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
        return fail(error_out,
                    errno_message("failed to fsync " + description, errno));
    }
    return true;
}

bool open_or_create_directory_at(const int parent_fd,
                                 const std::string& component,
                                 int* directory_fd_out,
                                 std::string* error_out)
{
    if (directory_fd_out == nullptr) {
        return fail(error_out, "directory descriptor output is null");
    }
    *directory_fd_out = -1;

    bool created = false;
    if (::mkdirat(parent_fd, component.c_str(), 0700) == 0) {
        created = true;
    } else if (errno != EEXIST) {
        return fail(error_out,
                    errno_message("failed to create artifact directory component '" +
                                      component + "'",
                                  errno));
    }

    int directory_fd = -1;
    do {
        directory_fd = ::openat(parent_fd,
                                component.c_str(),
                                O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    } while (directory_fd < 0 && errno == EINTR);
    if (directory_fd < 0) {
        return fail(error_out,
                    errno_message("failed to open non-symlink artifact directory component '" +
                                      component + "'",
                                  errno));
    }

    SpatialRoiRecorderArtifactIdentity unused_identity;
    if (!directory_identity_for_fd(directory_fd,
                                   "artifact directory component '" + component + "'",
                                   &unused_identity,
                                   error_out)) {
        close_fd(&directory_fd);
        return false;
    }
    if (created && !fsync_fd(parent_fd,
                             "parent of artifact directory component '" +
                                 component + "'",
                             error_out)) {
        close_fd(&directory_fd);
        return false;
    }
    *directory_fd_out = directory_fd;
    return true;
}

bool open_existing_directory_at(const int parent_fd,
                                const std::string& component,
                                int* directory_fd_out,
                                std::string* error_out)
{
    if (directory_fd_out == nullptr) {
        return fail(error_out, "directory descriptor output is null");
    }
    *directory_fd_out = -1;
    int directory_fd = -1;
    do {
        directory_fd = ::openat(parent_fd,
                                component.c_str(),
                                O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    } while (directory_fd < 0 && errno == EINTR);
    if (directory_fd < 0) {
        return fail(error_out,
                    errno_message("failed to open existing non-symlink artifact directory component '" +
                                      component + "'",
                                  errno));
    }
    SpatialRoiRecorderArtifactIdentity unused_identity;
    if (!directory_identity_for_fd(directory_fd,
                                   "existing artifact directory component '" +
                                       component + "'",
                                   &unused_identity,
                                   error_out)) {
        close_fd(&directory_fd);
        return false;
    }
    *directory_fd_out = directory_fd;
    return true;
}

bool open_absolute_directory_without_symlinks(
    const std::string& absolute_path,
    int* directory_fd_out,
    std::string* error_out)
{
    if (directory_fd_out == nullptr) {
        return fail(error_out, "absolute directory descriptor output is null");
    }
    *directory_fd_out = -1;
    if (absolute_path.empty() || absolute_path.front() != '/') {
        return fail(error_out, "authoritative recording root must be absolute");
    }
    if (absolute_path == "/") {
        return fail(error_out,
                    "filesystem root cannot be an authoritative recording root");
    }

    std::vector<std::string> components;
    if (!split_safe_relative_path(
            absolute_path.substr(1), &components, error_out)) {
        return false;
    }

    int current_fd = -1;
    do {
        current_fd =
            ::open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    } while (current_fd < 0 && errno == EINTR);
    if (current_fd < 0) {
        return fail(error_out,
                    errno_message("failed to open filesystem root", errno));
    }

    std::string traversed;
    for (const auto& component : components) {
        int next_fd = -1;
        do {
            next_fd = ::openat(current_fd,
                               component.c_str(),
                               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        } while (next_fd < 0 && errno == EINTR);
        if (next_fd < 0) {
            const int open_error = errno;
            close_fd(&current_fd);
            return fail(error_out,
                        errno_message(
                            "failed to open non-symlink recording-root component '/" +
                                traversed + component + "'",
                            open_error));
        }
        close_fd(&current_fd);
        current_fd = next_fd;
        traversed += component + "/";
    }

    *directory_fd_out = current_fd;
    return true;
}

}  // namespace

SpatialRoiRecorderArtifactFile::SpatialRoiRecorderArtifactFile(
    const int file_fd,
    const int parent_fd,
    std::string relative_path,
    std::string leaf_name,
    const SpatialRoiRecorderArtifactIdentity identity,
    const SpatialRoiRecorderArtifactIdentity artifact_root_identity,
    const SpatialRoiRecorderArtifactFileAccess access)
    : file_fd_(file_fd),
      parent_fd_(parent_fd),
      relative_path_(std::move(relative_path)),
      leaf_name_(std::move(leaf_name)),
      identity_(identity),
      artifact_root_identity_(artifact_root_identity),
      access_(access)
{
}

SpatialRoiRecorderArtifactFile::~SpatialRoiRecorderArtifactFile()
{
    Reset();
}

SpatialRoiRecorderArtifactFile::SpatialRoiRecorderArtifactFile(
    SpatialRoiRecorderArtifactFile&& other) noexcept
{
    *this = std::move(other);
}

SpatialRoiRecorderArtifactFile& SpatialRoiRecorderArtifactFile::operator=(
    SpatialRoiRecorderArtifactFile&& other) noexcept
{
    if (this != &other) {
        Reset();
        file_fd_ = std::exchange(other.file_fd_, -1);
        parent_fd_ = std::exchange(other.parent_fd_, -1);
        relative_path_ = std::move(other.relative_path_);
        leaf_name_ = std::move(other.leaf_name_);
        identity_ = other.identity_;
        artifact_root_identity_ = other.artifact_root_identity_;
        access_ = other.access_;
        sealed_ = other.sealed_;
        other.identity_ = {};
        other.artifact_root_identity_ = {};
        other.access_ = SpatialRoiRecorderArtifactFileAccess::kReadOnly;
        other.sealed_ = false;
    }
    return *this;
}

void SpatialRoiRecorderArtifactFile::Reset() noexcept
{
    close_fd(&file_fd_);
    close_fd(&parent_fd_);
    relative_path_.clear();
    leaf_name_.clear();
    identity_ = {};
    artifact_root_identity_ = {};
    access_ = SpatialRoiRecorderArtifactFileAccess::kReadOnly;
    sealed_ = false;
}

bool SpatialRoiRecorderArtifactFile::DuplicateFd(int* fd_out,
                                                 std::string* error_out) const
{
    return duplicate_fd(file_fd_, "artifact file", fd_out, error_out);
}

bool SpatialRoiRecorderArtifactFile::VerifyCurrentBinding(
    std::string* error_out) const
{
    if (!valid() || parent_fd_ < 0 || leaf_name_.empty()) {
        return fail(error_out, "artifact file binding is not open");
    }
    struct stat current {};
    if (::fstatat(parent_fd_,
                  leaf_name_.c_str(),
                  &current,
                  AT_SYMLINK_NOFOLLOW) != 0) {
        return fail(error_out,
                    errno_message("failed to verify current artifact leaf '" +
                                      relative_path_ + "'",
                                  errno));
    }
    if (!S_ISREG(current.st_mode)) {
        return fail(error_out,
                    "current artifact leaf is no longer a regular file: " +
                        relative_path_);
    }
    SpatialRoiRecorderArtifactIdentity current_identity;
    if (!identity_from_stat(current, &current_identity, error_out)) {
        return false;
    }
    if (current_identity != identity_) {
        return fail(error_out,
                    "current artifact leaf no longer names the authorized opened inode: " +
                        relative_path_);
    }
    return true;
}

bool SpatialRoiRecorderArtifactFile::Seal(std::string* error_out)
{
    clear_error(error_out);
    if (!VerifyCurrentBinding(error_out)) {
        return false;
    }
    if (!fsync_fd(file_fd_, "artifact file '" + relative_path_ + "'", error_out)) {
        return false;
    }
    // Recheck after flushing so an unlink/replacement racing the file fsync is
    // not certified as the artifact that was actually flushed.
    if (!VerifyCurrentBinding(error_out)) {
        return false;
    }
    if (!fsync_fd(parent_fd_,
                  "artifact parent directory for '" + relative_path_ + "'",
                  error_out)) {
        return false;
    }
    // A replacement racing the parent fsync must also fail closed.
    if (!VerifyCurrentBinding(error_out)) {
        return false;
    }
    sealed_ = true;
    return true;
}

SpatialRoiRecorderArtifactRoot::SpatialRoiRecorderArtifactRoot(
    const int recording_root_fd,
    const int artifact_root_fd,
    std::filesystem::path opened_recording_root,
    const SpatialRoiRecorderArtifactIdentity recording_root_identity,
    const SpatialRoiRecorderArtifactIdentity artifact_root_identity,
    std::set<std::string> allowed_relative_paths)
    : recording_root_fd_(recording_root_fd),
      artifact_root_fd_(artifact_root_fd),
      opened_recording_root_(std::move(opened_recording_root)),
      recording_root_identity_(recording_root_identity),
      artifact_root_identity_(artifact_root_identity),
      allowed_relative_paths_(std::move(allowed_relative_paths))
{
}

SpatialRoiRecorderArtifactRoot::~SpatialRoiRecorderArtifactRoot()
{
    Reset();
}

SpatialRoiRecorderArtifactRoot::SpatialRoiRecorderArtifactRoot(
    SpatialRoiRecorderArtifactRoot&& other) noexcept
{
    *this = std::move(other);
}

SpatialRoiRecorderArtifactRoot& SpatialRoiRecorderArtifactRoot::operator=(
    SpatialRoiRecorderArtifactRoot&& other) noexcept
{
    if (this != &other) {
        Reset();
        recording_root_fd_ = std::exchange(other.recording_root_fd_, -1);
        artifact_root_fd_ = std::exchange(other.artifact_root_fd_, -1);
        opened_recording_root_ = std::move(other.opened_recording_root_);
        recording_root_identity_ = other.recording_root_identity_;
        artifact_root_identity_ = other.artifact_root_identity_;
        allowed_relative_paths_ = std::move(other.allowed_relative_paths_);
        other.recording_root_identity_ = {};
        other.artifact_root_identity_ = {};
    }
    return *this;
}

void SpatialRoiRecorderArtifactRoot::Reset() noexcept
{
    close_fd(&artifact_root_fd_);
    close_fd(&recording_root_fd_);
    opened_recording_root_.clear();
    recording_root_identity_ = {};
    artifact_root_identity_ = {};
    allowed_relative_paths_.clear();
}

bool SpatialRoiRecorderArtifactRoot::Open(
    const std::filesystem::path& authoritative_recording_root,
    const std::vector<std::string>& allowed_relative_paths,
    std::unique_ptr<SpatialRoiRecorderArtifactRoot>* root_out,
    std::string* error_out)
{
    return OpenImpl(authoritative_recording_root,
                    allowed_relative_paths,
                    true,
                    root_out,
                    error_out);
}

bool SpatialRoiRecorderArtifactRoot::OpenExisting(
    const std::filesystem::path& authoritative_recording_root,
    const std::vector<std::string>& allowed_relative_paths,
    std::unique_ptr<SpatialRoiRecorderArtifactRoot>* root_out,
    std::string* error_out)
{
    return OpenImpl(authoritative_recording_root,
                    allowed_relative_paths,
                    false,
                    root_out,
                    error_out);
}

bool SpatialRoiRecorderArtifactRoot::OpenImpl(
    const std::filesystem::path& authoritative_recording_root,
    const std::vector<std::string>& allowed_relative_paths,
    const bool create_artifact_directory,
    std::unique_ptr<SpatialRoiRecorderArtifactRoot>* root_out,
    std::string* error_out)
{
    clear_error(error_out);
    if (root_out == nullptr) {
        return fail(error_out, "artifact root output is null");
    }
    root_out->reset();
    const std::string recording_root_string =
        authoritative_recording_root.generic_string();
    if (recording_root_string.empty() ||
        !authoritative_recording_root.is_absolute()) {
        return fail(error_out,
                    "authoritative recording root must be a non-empty absolute path");
    }
    if (recording_root_string.size() >
            kSpatialRoiRecorderArtifactMaxPathBytes ||
        recording_root_string.find('\0') != std::string::npos) {
        return fail(error_out,
                    "authoritative recording root exceeds the path bound or contains NUL");
    }
    if (allowed_relative_paths.empty()) {
        return fail(error_out, "artifact allow-list must not be empty");
    }
    if (allowed_relative_paths.size() > kSpatialRoiRecorderArtifactMaxCount) {
        return fail(error_out, "artifact allow-list exceeds the count bound");
    }

    std::set<std::string> allowed;
    for (const auto& path : allowed_relative_paths) {
        std::vector<std::string> components;
        if (!split_safe_relative_path(path, &components, error_out)) {
            return false;
        }
        if (!allowed.insert(path).second) {
            return fail(error_out, "artifact allow-list contains a duplicate path");
        }
    }

    int recording_root_fd = -1;
    if (!open_absolute_directory_without_symlinks(
            recording_root_string, &recording_root_fd, error_out)) {
        return false;
    }

    SpatialRoiRecorderArtifactIdentity recording_root_identity;
    if (!directory_identity_for_fd(recording_root_fd,
                                   "authoritative recording root",
                                   &recording_root_identity,
                                   error_out)) {
        close_fd(&recording_root_fd);
        return false;
    }

    int artifact_root_fd = -1;
    const bool artifact_root_opened = create_artifact_directory
                                          ? open_or_create_directory_at(
                                                recording_root_fd,
                                                kSpatialRoiRecorderArtifactDirectory,
                                                &artifact_root_fd,
                                                error_out)
                                          : open_existing_directory_at(
                                                recording_root_fd,
                                                kSpatialRoiRecorderArtifactDirectory,
                                                &artifact_root_fd,
                                                error_out);
    if (!artifact_root_opened) {
        close_fd(&recording_root_fd);
        return false;
    }
    SpatialRoiRecorderArtifactIdentity artifact_root_identity;
    if (!directory_identity_for_fd(artifact_root_fd,
                                   "spatial ROI recorder artifact root",
                                   &artifact_root_identity,
                                   error_out)) {
        close_fd(&artifact_root_fd);
        close_fd(&recording_root_fd);
        return false;
    }

    root_out->reset(new SpatialRoiRecorderArtifactRoot(
        recording_root_fd,
        artifact_root_fd,
        authoritative_recording_root,
        recording_root_identity,
        artifact_root_identity,
        std::move(allowed)));
    return true;
}

bool SpatialRoiRecorderArtifactRoot::DuplicateRecordingRootFd(
    int* fd_out,
    std::string* error_out) const
{
    return duplicate_fd(recording_root_fd_,
                        "authoritative recording root",
                        fd_out,
                        error_out);
}

bool SpatialRoiRecorderArtifactRoot::DuplicateArtifactRootFd(
    int* fd_out,
    std::string* error_out) const
{
    return duplicate_fd(artifact_root_fd_,
                        "spatial ROI recorder artifact root",
                        fd_out,
                        error_out);
}

bool SpatialRoiRecorderArtifactRoot::IsAllowed(
    const std::string& relative_path) const noexcept
{
    return allowed_relative_paths_.find(relative_path) !=
           allowed_relative_paths_.end();
}

bool SpatialRoiRecorderArtifactRoot::CreateFile(
    const std::string& allowed_relative_path,
    std::unique_ptr<SpatialRoiRecorderArtifactFile>* file_out,
    std::string* error_out) const
{
    clear_error(error_out);
    if (file_out == nullptr) {
        return fail(error_out, "artifact file output is null");
    }
    file_out->reset();
    if (!valid()) {
        return fail(error_out, "spatial ROI recorder artifact root is not open");
    }
    if (!IsAllowed(allowed_relative_path)) {
        return fail(error_out,
                    "artifact path is not present in the verified contract allow-list: " +
                        allowed_relative_path);
    }

    std::vector<std::string> components;
    if (!split_safe_relative_path(
            allowed_relative_path, &components, error_out)) {
        return false;
    }

    int current_directory_fd = -1;
    if (!duplicate_fd(artifact_root_fd_,
                      "spatial ROI recorder artifact root",
                      &current_directory_fd,
                      error_out)) {
        return false;
    }
    for (std::size_t index = 0; index + 1 < components.size(); ++index) {
        int next_directory_fd = -1;
        if (!open_or_create_directory_at(current_directory_fd,
                                         components[index],
                                         &next_directory_fd,
                                         error_out)) {
            close_fd(&current_directory_fd);
            return false;
        }
        close_fd(&current_directory_fd);
        current_directory_fd = next_directory_fd;
    }

    const std::string& leaf = components.back();
    int file_fd = -1;
    do {
        file_fd = ::openat(current_directory_fd,
                           leaf.c_str(),
                           O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                           0600);
    } while (file_fd < 0 && errno == EINTR);
    if (file_fd < 0) {
        const int open_error = errno;
        close_fd(&current_directory_fd);
        return fail(error_out,
                    errno_message("failed to exclusively create authorized artifact '" +
                                      allowed_relative_path + "'",
                                  open_error));
    }

    SpatialRoiRecorderArtifactIdentity identity;
    if (!regular_identity_for_fd(file_fd, &identity, error_out)) {
        close_fd(&file_fd);
        close_fd(&current_directory_fd);
        return false;
    }

    auto candidate = std::unique_ptr<SpatialRoiRecorderArtifactFile>(
        new SpatialRoiRecorderArtifactFile(
            file_fd,
            current_directory_fd,
            allowed_relative_path,
            leaf,
            identity,
            artifact_root_identity_,
            SpatialRoiRecorderArtifactFileAccess::kReadWrite));
    if (!candidate->VerifyCurrentBinding(error_out)) {
        return false;
    }
    *file_out = std::move(candidate);
    return true;
}

bool SpatialRoiRecorderArtifactRoot::OpenExistingFile(
    const std::string& allowed_relative_path,
    const SpatialRoiRecorderArtifactFileAccess access,
    std::unique_ptr<SpatialRoiRecorderArtifactFile>* file_out,
    std::string* error_out) const
{
    clear_error(error_out);
    if (file_out == nullptr) {
        return fail(error_out, "artifact file output is null");
    }
    file_out->reset();
    if (!valid()) {
        return fail(error_out, "spatial ROI recorder artifact root is not open");
    }
    if (!IsAllowed(allowed_relative_path)) {
        return fail(error_out,
                    "artifact path is not present in the verified contract allow-list: " +
                        allowed_relative_path);
    }

    std::vector<std::string> components;
    if (!split_safe_relative_path(
            allowed_relative_path, &components, error_out)) {
        return false;
    }

    int current_directory_fd = -1;
    if (!duplicate_fd(artifact_root_fd_,
                      "spatial ROI recorder artifact root",
                      &current_directory_fd,
                      error_out)) {
        return false;
    }
    for (std::size_t index = 0; index + 1 < components.size(); ++index) {
        int next_directory_fd = -1;
        if (!open_existing_directory_at(current_directory_fd,
                                        components[index],
                                        &next_directory_fd,
                                        error_out)) {
            close_fd(&current_directory_fd);
            return false;
        }
        close_fd(&current_directory_fd);
        current_directory_fd = next_directory_fd;
    }

    int access_flags = 0;
    switch (access) {
        case SpatialRoiRecorderArtifactFileAccess::kReadOnly:
            access_flags = O_RDONLY;
            break;
        case SpatialRoiRecorderArtifactFileAccess::kReadWrite:
            access_flags = O_RDWR;
            break;
        default:
            close_fd(&current_directory_fd);
            return fail(error_out, "existing artifact access mode is invalid");
    }

    const std::string& leaf = components.back();
    int file_fd = -1;
    do {
        file_fd = ::openat(current_directory_fd,
                           leaf.c_str(),
                           access_flags | O_NOFOLLOW | O_CLOEXEC);
    } while (file_fd < 0 && errno == EINTR);
    if (file_fd < 0) {
        const int open_error = errno;
        close_fd(&current_directory_fd);
        return fail(error_out,
                    errno_message("failed to open existing authorized artifact '" +
                                      allowed_relative_path + "'",
                                  open_error));
    }

    SpatialRoiRecorderArtifactIdentity identity;
    if (!regular_identity_for_fd(file_fd, &identity, error_out)) {
        close_fd(&file_fd);
        close_fd(&current_directory_fd);
        return false;
    }
    auto candidate = std::unique_ptr<SpatialRoiRecorderArtifactFile>(
        new SpatialRoiRecorderArtifactFile(
            file_fd,
            current_directory_fd,
            allowed_relative_path,
            leaf,
            identity,
            artifact_root_identity_,
            access));
    if (!candidate->VerifyCurrentBinding(error_out)) {
        return false;
    }
    *file_out = std::move(candidate);
    return true;
}

}  // namespace orange::spatial_roi::recording
