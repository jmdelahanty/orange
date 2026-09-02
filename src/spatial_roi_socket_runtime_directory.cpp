#include "spatial_roi_socket_runtime_directory.h"

#include "session/spatial_roi_recording_config.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <set>
#include <string_view>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace orange::spatial_roi::ipc {
namespace {

constexpr std::string_view kDirectoryPrefix =
    "/tmp/orange_spatial_roi_";
constexpr std::size_t kDigestCharacters = 24;

class ScopedFd final {
public:
    explicit ScopedFd(const int fd = -1) noexcept : fd_(fd) {}
    ~ScopedFd()
    {
        if (fd_ >= 0) {
            (void)::close(fd_);
        }
    }
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
    int get() const noexcept { return fd_; }
    int release() noexcept { return std::exchange(fd_, -1); }

private:
    int fd_ = -1;
};

bool lowercase_hex(const std::string_view value) noexcept
{
    return value.size() == kDigestCharacters &&
        std::all_of(value.begin(), value.end(), [](const unsigned char ch) {
            return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        });
}

std::string errno_message(const char* operation, const int error_number)
{
    std::string result = operation ? operation : "socket directory operation";
    result += " failed: ";
    result += std::strerror(error_number);
    return result;
}

bool same_identity(const struct stat& status,
                   const std::uint64_t device,
                   const std::uint64_t inode) noexcept
{
    return status.st_dev == static_cast<dev_t>(device) &&
        status.st_ino == static_cast<ino_t>(inode);
}

}  // namespace

SpatialRoiSocketRuntimeDirectory::SpatialRoiSocketRuntimeDirectory(
    const int tmp_fd,
    const int directory_fd,
    std::string directory_path,
    std::string directory_leaf,
    std::vector<std::string> socket_paths,
    const std::uint64_t device,
    const std::uint64_t inode) noexcept
    : tmp_fd_(tmp_fd),
      directory_fd_(directory_fd),
      directory_path_(std::move(directory_path)),
      directory_leaf_(std::move(directory_leaf)),
      socket_paths_(std::move(socket_paths)),
      device_(device),
      inode_(inode),
      owns_entry_(true)
{
}

SpatialRoiSocketRuntimeDirectory::~SpatialRoiSocketRuntimeDirectory()
{
    (void)Close(nullptr);
}

bool SpatialRoiSocketRuntimeDirectory::set_error(
    std::string* error_out,
    const std::string_view message) noexcept
{
    if (!error_out) {
        return false;
    }
    try {
        error_out->assign(message.data(), std::min<std::size_t>(message.size(), 1024));
    } catch (...) {
        error_out->clear();
    }
    return false;
}

bool SpatialRoiSocketRuntimeDirectory::validate_config(
    const SpatialRoiSocketRuntimeDirectoryConfig& config,
    std::string* directory_path_out,
    std::string* leaf_out,
    std::vector<std::string>* socket_paths_out,
    std::string* error_out) noexcept
{
    if (!directory_path_out || !leaf_out || !socket_paths_out) {
        return set_error(error_out,
                         "socket runtime directory validation output is null");
    }
    directory_path_out->clear();
    leaf_out->clear();
    socket_paths_out->clear();
    try {
        const std::string directory_path =
            orange::session::spatial_roi::expected_socket_runtime_directory(
                config.recording_identity_token);
        if (directory_path.empty() ||
            directory_path.size() != kDirectoryPrefix.size() + kDigestCharacters ||
            directory_path.compare(0, kDirectoryPrefix.size(),
                                   kDirectoryPrefix) != 0) {
            return set_error(error_out,
                             "recording identity token does not derive the closed /tmp runtime path");
        }
        const std::string leaf = directory_path.substr(5);
        const std::string_view directory_digest(
            directory_path.data() + kDirectoryPrefix.size(),
            kDigestCharacters);
        if (!lowercase_hex(directory_digest) || leaf.empty() ||
            leaf.find('/') != std::string::npos) {
            return set_error(error_out,
                             "derived socket runtime directory digest or leaf is invalid");
        }
        if (config.logical_stream_ids.size() != 4) {
            return set_error(error_out,
                             "camera socket runtime directory requires exactly four logical streams");
        }
        const std::string socket_prefix = directory_path + "/";
        std::set<std::string> unique_streams;
        std::set<std::string> unique_paths;
        std::vector<std::string> socket_paths;
        socket_paths.reserve(config.logical_stream_ids.size());
        for (const auto& logical_stream_id : config.logical_stream_ids) {
            if (!unique_streams.insert(logical_stream_id).second) {
                return set_error(error_out,
                                 "camera socket runtime directory has a duplicate logical stream");
            }
            const std::string path =
                orange::session::spatial_roi::expected_socket_path(
                    config.recording_identity_token, logical_stream_id);
            if (path.empty() ||
                path.size() != socket_prefix.size() + kDigestCharacters + 5 ||
                path.compare(0, socket_prefix.size(), socket_prefix) != 0 ||
                path.compare(path.size() - 5, 5, ".sock") != 0 ||
                !lowercase_hex(std::string_view(
                    path.data() + socket_prefix.size(), kDigestCharacters)) ||
                path.size() >= sizeof(sockaddr_un{}.sun_path) ||
                !unique_paths.insert(path).second) {
                return set_error(error_out,
                                 "logical stream does not derive a unique closed socket child");
            }
            socket_paths.push_back(path);
        }
        *directory_path_out = directory_path;
        *leaf_out = leaf;
        *socket_paths_out = std::move(socket_paths);
        return true;
    } catch (const std::exception& exception) {
        return set_error(error_out, exception.what());
    } catch (...) {
        return set_error(error_out,
                         "could not derive socket runtime directory configuration");
    }
}

std::unique_ptr<SpatialRoiSocketRuntimeDirectory>
SpatialRoiSocketRuntimeDirectory::Create(
    const SpatialRoiSocketRuntimeDirectoryConfig& config,
    std::string* error_out) noexcept
{
    if (error_out) {
        error_out->clear();
    }
    std::string directory_path;
    std::string leaf;
    std::vector<std::string> socket_paths;
    if (!validate_config(config, &directory_path, &leaf, &socket_paths,
                         error_out)) {
        return nullptr;
    }
    try {
        ScopedFd tmp_fd(::open("/tmp", O_RDONLY | O_DIRECTORY | O_CLOEXEC |
                                          O_NOFOLLOW));
        if (tmp_fd.get() < 0) {
            set_error(error_out, errno_message("open(/tmp)", errno));
            return nullptr;
        }
        struct stat tmp_status {};
        if (::fstat(tmp_fd.get(), &tmp_status) != 0 ||
            !S_ISDIR(tmp_status.st_mode)) {
            set_error(error_out, "retained /tmp authority is not a directory");
            return nullptr;
        }
        if (::mkdirat(tmp_fd.get(), leaf.c_str(), 0700) != 0) {
            const int mkdir_error = errno;
            set_error(error_out,
                      mkdir_error == EEXIST
                          ? "socket runtime directory already exists"
                          : errno_message("mkdirat(socket runtime directory)",
                                          mkdir_error));
            return nullptr;
        }

        ScopedFd directory_fd(::openat(tmp_fd.get(),
                                       leaf.c_str(),
                                       O_RDONLY | O_DIRECTORY | O_CLOEXEC |
                                           O_NOFOLLOW));
        struct stat descriptor_status {};
        struct stat entry_status {};
        const bool authenticated =
            directory_fd.get() >= 0 &&
            ::fstat(directory_fd.get(), &descriptor_status) == 0 &&
            ::fstatat(tmp_fd.get(),
                      leaf.c_str(),
                      &entry_status,
                      AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISDIR(descriptor_status.st_mode) &&
            S_ISDIR(entry_status.st_mode) &&
            descriptor_status.st_dev == entry_status.st_dev &&
            descriptor_status.st_ino == entry_status.st_ino &&
            descriptor_status.st_uid == ::geteuid() &&
            (descriptor_status.st_mode & 07777) == 0700;
        if (!authenticated) {
            if (directory_fd.get() >= 0 &&
                ::fstat(directory_fd.get(), &descriptor_status) == 0 &&
                ::fstatat(tmp_fd.get(), leaf.c_str(), &entry_status,
                          AT_SYMLINK_NOFOLLOW) == 0 &&
                descriptor_status.st_dev == entry_status.st_dev &&
                descriptor_status.st_ino == entry_status.st_ino) {
                (void)::unlinkat(tmp_fd.get(), leaf.c_str(), AT_REMOVEDIR);
            }
            set_error(error_out,
                      "new socket runtime directory failed ownership, mode, or inode authentication");
            return nullptr;
        }

        return std::unique_ptr<SpatialRoiSocketRuntimeDirectory>(
            new SpatialRoiSocketRuntimeDirectory(
                tmp_fd.release(),
                directory_fd.release(),
                std::move(directory_path),
                leaf,
                std::move(socket_paths),
                static_cast<std::uint64_t>(descriptor_status.st_dev),
                static_cast<std::uint64_t>(descriptor_status.st_ino)));
    } catch (const std::exception& exception) {
        set_error(error_out, exception.what());
        return nullptr;
    } catch (...) {
        set_error(error_out, "could not create socket runtime directory");
        return nullptr;
    }
}

void SpatialRoiSocketRuntimeDirectory::close_descriptors() noexcept
{
    const int directory_fd = std::exchange(directory_fd_, -1);
    if (directory_fd >= 0) {
        (void)::close(directory_fd);
    }
    const int tmp_fd = std::exchange(tmp_fd_, -1);
    if (tmp_fd >= 0) {
        (void)::close(tmp_fd);
    }
}

bool SpatialRoiSocketRuntimeDirectory::Close(std::string* error_out) noexcept
{
    if (error_out) {
        error_out->clear();
    }
    try {
        if (!owns_entry_) {
            close_descriptors();
            return true;
        }
        if (tmp_fd_ < 0 || directory_fd_ < 0) {
            owns_entry_ = false;
            close_descriptors();
            return set_error(error_out,
                             "socket runtime directory lost its retained descriptors");
        }

        struct stat descriptor_status {};
        struct stat entry_status {};
        const bool same =
            ::fstat(directory_fd_, &descriptor_status) == 0 &&
            same_identity(descriptor_status, device_, inode_) &&
            ::fstatat(tmp_fd_, directory_leaf_.c_str(), &entry_status,
                      AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISDIR(entry_status.st_mode) &&
            same_identity(entry_status, device_, inode_);
        if (!same) {
            owns_entry_ = false;
            close_descriptors();
            return set_error(error_out,
                             "socket runtime directory binding changed; replacement was preserved");
        }

        // Linux has no inode-conditional unlinkat.  This class and all
        // listeners are one camera-supervisor's single-owner namespace; no
        // same-UID process may mutate it concurrently.  The retained parent
        // fd and immediately preceding identity check prevent ancestor or
        // accidental replacement redirection within that operational trust
        // boundary.
        if (::unlinkat(tmp_fd_, directory_leaf_.c_str(), AT_REMOVEDIR) != 0) {
            const int remove_error = errno;
            owns_entry_ = false;
            close_descriptors();
            return set_error(error_out,
                             errno_message("unlinkat(socket runtime directory)",
                                           remove_error));
        }
        owns_entry_ = false;
        close_descriptors();
        return true;
    } catch (const std::exception& exception) {
        owns_entry_ = false;
        close_descriptors();
        return set_error(error_out, exception.what());
    } catch (...) {
        owns_entry_ = false;
        close_descriptors();
        return set_error(error_out,
                         "socket runtime directory cleanup failed");
    }
}

}  // namespace orange::spatial_roi::ipc
