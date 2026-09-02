#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "spatial_roi_unix_socket_listener.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace orange::spatial_roi::ipc {
namespace {

constexpr auto kMaximumAcceptWait = std::chrono::minutes(5);
constexpr std::size_t kMaximumErrorBytes = 4096;

class ScopedFd final {
public:
    explicit ScopedFd(const int fd) noexcept : fd_(fd) {}
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

    void reset(const int fd = -1) noexcept
    {
        if (fd_ >= 0) {
            (void)::close(fd_);
        }
        fd_ = fd;
    }

private:
    int fd_ = -1;
};

struct PathIdentity final {
    dev_t device = 0;
    ino_t inode = 0;
    bool valid = false;
};

bool same_identity(const struct stat& stat_value,
                  const PathIdentity& identity) noexcept
{
    return identity.valid && S_ISSOCK(stat_value.st_mode) &&
           stat_value.st_dev == identity.device &&
           stat_value.st_ino == identity.inode;
}

bool capture_entry_identity(const int parent_fd,
                            const std::string& leaf,
                            PathIdentity* identity,
                            std::string* error_out) noexcept
{
    if (parent_fd < 0 || !identity) {
        return false;
    }
    struct stat stat_value {
    };
    if (::fstatat(parent_fd,
                  leaf.c_str(),
                  &stat_value,
                  AT_SYMLINK_NOFOLLOW) != 0) {
        if (error_out) {
            try {
                *error_out = "fstatat socket path failed: ";
                const char* text = std::strerror(errno);
                if (text) {
                    error_out->append(text);
                }
            } catch (...) {
                error_out->clear();
            }
        }
        return false;
    }
    if (!S_ISSOCK(stat_value.st_mode)) {
        if (error_out) {
            try {
                *error_out = "bound socket path is not a filesystem socket";
            } catch (...) {
                error_out->clear();
            }
        }
        return false;
    }
    identity->device = stat_value.st_dev;
    identity->inode = stat_value.st_ino;
    identity->valid = true;
    return true;
}

void unlink_exact_entry_if_owned(const int parent_fd,
                                 const std::string& leaf,
                                 const PathIdentity& identity) noexcept
{
    if (parent_fd < 0 || !identity.valid) {
        return;
    }
    struct stat stat_value {
    };
    if (::fstatat(parent_fd,
                  leaf.c_str(),
                  &stat_value,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
        !same_identity(stat_value, identity)) {
        return;
    }
    // The parent descriptor is retained from the authenticated private
    // directory walk.  unlinkat therefore cannot follow a replacement of an
    // ancestor pathname or remove an entry in a different directory.  The
    // final identity check and unlinkat are still not an atomic inode compare;
    // callers must not mutate this owner-only directory concurrently.
    (void)::unlinkat(parent_fd, leaf.c_str(), 0);
}

int poll_timeout_milliseconds(
    const std::chrono::steady_clock::time_point deadline) noexcept
{
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return 0;
    }
    const auto remaining = deadline - now;
    auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
    if (remaining > std::chrono::milliseconds(milliseconds)) {
        ++milliseconds;
    }
    if (milliseconds <= 0) {
        return 1;
    }
    if (milliseconds > std::numeric_limits<int>::max()) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(milliseconds);
}

SpatialRoiUnixSocketListenerAcceptResult make_result(
    const SpatialRoiUnixSocketListenerAcceptStatus status,
    const char* error = nullptr) noexcept
{
    SpatialRoiUnixSocketListenerAcceptResult result;
    result.status = status;
    if (error) {
        try {
            result.error.assign(error, std::min(std::strlen(error),
                                                kMaximumErrorBytes));
        } catch (...) {
            result.error.clear();
        }
    }
    return result;
}

}  // namespace

SpatialRoiUnixSocketListener::SpatialRoiUnixSocketListener(
    const int listener_fd,
    const int parent_fd,
    std::string socket_path,
    std::string socket_leaf,
    SpatialRoiUnixSocketTransportConfig transport_config,
    const SocketIdentity identity) noexcept
    : listener_fd_(listener_fd),
      parent_fd_(parent_fd),
      socket_path_(std::move(socket_path)),
      socket_leaf_(std::move(socket_leaf)),
      transport_config_(std::move(transport_config)),
      socket_identity_{identity.device, identity.inode, identity.valid},
      owns_path_(identity.valid)
{
}

bool SpatialRoiUnixSocketListener::set_error(
    std::string* error_out,
    const char* message) noexcept
{
    if (!error_out) {
        return false;
    }
    try {
        const char* safe_message = message ? message : "";
        error_out->assign(
            safe_message, std::min(std::strlen(safe_message), kMaximumErrorBytes));
    } catch (...) {
        error_out->clear();
        return false;
    }
    return true;
}

bool SpatialRoiUnixSocketListener::set_error(
    std::string* error_out,
    const std::string& message) noexcept
{
    if (!error_out) {
        return false;
    }
    try {
        error_out->assign(message.data(),
                          std::min(message.size(), kMaximumErrorBytes));
    } catch (...) {
        error_out->clear();
        return false;
    }
    return true;
}

std::string SpatialRoiUnixSocketListener::errno_message(
    const char* operation,
    const int error_number) noexcept
{
    try {
        std::string result = operation ? operation : "socket operation";
        result += " failed: ";
        const char* text = std::strerror(error_number);
        if (text) {
            result += text;
        }
        if (result.size() > kMaximumErrorBytes) {
            result.resize(kMaximumErrorBytes);
        }
        return result;
    } catch (...) {
        return {};
    }
}

bool SpatialRoiUnixSocketListener::validate_socket_path(
    const std::string& socket_path,
    std::string* error_out) noexcept
{
    if (socket_path.empty()) {
        set_error(error_out, "socket path must not be empty");
        return false;
    }
    if (socket_path.find('\0') != std::string::npos) {
        set_error(error_out, "socket path must not contain NUL");
        return false;
    }
    if (socket_path.front() != '/') {
        set_error(error_out, "socket path must be absolute");
        return false;
    }
    // A filesystem AF_UNIX path has room for at most 107 bytes plus the
    // terminating NUL on Linux.  Abstract namespace addresses are not
    // accepted by this filesystem listener.
    if (socket_path.size() >= sizeof(sockaddr_un{}.sun_path)) {
        set_error(error_out, "socket path exceeds sockaddr_un filesystem bound");
        return false;
    }

    // Keep one exact spelling for the contract boundary.  Empty components
    // would otherwise make paths such as //tmp/socket or /tmp//socket
    // aliases for the same endpoint while retaining a different authenticated
    // string.  The final component is the socket leaf; a root directory is
    // deliberately not accepted as its parent because it cannot satisfy the
    // private-directory authority below.
    std::vector<std::string> components;
    std::size_t component_start = 1;
    while (component_start < socket_path.size()) {
        const std::size_t separator = socket_path.find('/', component_start);
        const std::size_t component_end =
            separator == std::string::npos ? socket_path.size() : separator;
        if (component_end == component_start) {
            set_error(error_out, "socket path contains an empty component");
            return false;
        }
        const std::string component =
            socket_path.substr(component_start, component_end - component_start);
        if (component == "." || component == "..") {
            set_error(error_out,
                      "socket path contains unsafe . or .. component");
            return false;
        }
        components.push_back(component);
        if (separator == std::string::npos) {
            break;
        }
        component_start = separator + 1;
    }
    if (components.size() < 2) {
        set_error(error_out,
                  "socket path must include a private parent directory");
        return false;
    }
    return true;
}

bool SpatialRoiUnixSocketListener::open_private_parent_directory(
    const std::string& socket_path,
    int* parent_fd_out,
    std::string* socket_leaf_out,
    std::string* error_out) noexcept
{
    if (!parent_fd_out || !socket_leaf_out) {
        set_error(error_out, "private parent directory output is null");
        return false;
    }
    *parent_fd_out = -1;
    socket_leaf_out->clear();

    try {
        std::vector<std::string> components;
        std::size_t component_start = 1;
        while (component_start < socket_path.size()) {
            const std::size_t separator = socket_path.find('/', component_start);
            const std::size_t component_end =
                separator == std::string::npos ? socket_path.size() : separator;
            if (component_end == component_start) {
                set_error(error_out, "socket path contains an empty component");
                return false;
            }
            components.push_back(socket_path.substr(
                component_start, component_end - component_start));
            if (separator == std::string::npos) {
                break;
            }
            component_start = separator + 1;
        }
        if (components.size() < 2) {
            set_error(error_out,
                      "socket path must include a private parent directory");
            return false;
        }

        int current_fd = -1;
        do {
            current_fd = ::open("/",
                                O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        } while (current_fd < 0 && errno == EINTR);
        if (current_fd < 0) {
            set_error(error_out,
                      errno_message("open filesystem root", errno));
            return false;
        }
        ScopedFd current(current_fd);

        // Open every parent component relative to the descriptor already
        // authenticated.  O_NOFOLLOW prevents a symlink from redirecting the
        // walk, and retaining the final descriptor makes later leaf work
        // independent of changes to the original absolute pathname.
        for (std::size_t index = 0; index + 1 < components.size(); ++index) {
            int next_fd = -1;
            do {
                next_fd = ::openat(current.get(),
                                   components[index].c_str(),
                                   O_RDONLY | O_DIRECTORY | O_NOFOLLOW |
                                       O_CLOEXEC);
            } while (next_fd < 0 && errno == EINTR);
            if (next_fd < 0) {
                set_error(error_out,
                          errno_message("open socket parent directory component",
                                        errno));
                return false;
            }
            current.reset(next_fd);
        }

        struct stat parent_stat {
        };
        if (::fstat(current.get(), &parent_stat) != 0) {
            set_error(error_out,
                      errno_message("fstat socket parent directory", errno));
            return false;
        }
        const uid_t effective_uid = ::geteuid();
        if (!S_ISDIR(parent_stat.st_mode) ||
            parent_stat.st_uid != effective_uid ||
            (parent_stat.st_mode & 0777) != 0700) {
            set_error(error_out,
                      "socket parent directory must be euid-owned with mode 0700");
            return false;
        }

        *socket_leaf_out = components.back();
        *parent_fd_out = current.release();
        return true;
    } catch (const std::exception& exception) {
        set_error(error_out, exception.what());
        return false;
    } catch (...) {
        set_error(error_out, "failed to open private socket parent directory");
        return false;
    }
}

std::unique_ptr<SpatialRoiUnixSocketListener>
SpatialRoiUnixSocketListener::Create(
    const SpatialRoiUnixSocketListenerConfig& config,
    std::string* error_out) noexcept
{
    if (error_out) {
        error_out->clear();
    }

    try {
        std::string validation_error;
        if (!validate_socket_path(config.socket_path, &validation_error)) {
            set_error(error_out, validation_error);
            return nullptr;
        }

        int parent_fd = -1;
        std::string socket_leaf;
        if (!open_private_parent_directory(config.socket_path,
                                           &parent_fd,
                                           &socket_leaf,
                                           error_out)) {
            return nullptr;
        }
        struct stat parent_stat {
        };
        ScopedFd owned_parent_fd(parent_fd);
        if (::fstat(parent_fd, &parent_stat) != 0) {
            set_error(error_out,
                      errno_message("fstat socket parent directory", errno));
            return nullptr;
        }
        const int transferred_parent_fd = owned_parent_fd.release();
        return create_from_parent_fd(transferred_parent_fd,
                                     parent_stat.st_dev,
                                     parent_stat.st_ino,
                                     config.socket_path,
                                     std::move(socket_leaf),
                                     config.transport_config,
                                     error_out);
    } catch (const std::exception& exception) {
        set_error(error_out, exception.what());
    } catch (...) {
        set_error(error_out, "failed to initialize Unix socket listener");
    }
    return nullptr;
}

std::unique_ptr<SpatialRoiUnixSocketListener>
SpatialRoiUnixSocketListener::Create(
    const int parent_fd,
    const dev_t expected_parent_device,
    const ino_t expected_parent_inode,
    std::string socket_path,
    std::string socket_leaf,
    SpatialRoiUnixSocketTransportConfig transport_config,
    std::string* error_out) noexcept
{
    if (error_out) {
        error_out->clear();
    }
    if (parent_fd < 0) {
        set_error(error_out, "socket parent descriptor is invalid");
        return nullptr;
    }

    try {
        std::string validation_error;
        if (!validate_socket_path(socket_path, &validation_error)) {
            set_error(error_out, validation_error);
            return nullptr;
        }
        const std::size_t separator = socket_path.rfind('/');
        if (separator == std::string::npos ||
            socket_leaf.empty() || socket_leaf.find('/') != std::string::npos ||
            socket_leaf.find('\0') != std::string::npos ||
            socket_path.compare(separator + 1,
                                std::string::npos,
                                socket_leaf) != 0) {
            set_error(error_out,
                      "socket leaf does not match the exact socket path");
            return nullptr;
        }

        const int duplicate_fd =
            ::fcntl(parent_fd, F_DUPFD_CLOEXEC, 0);
        if (duplicate_fd < 0) {
            set_error(error_out,
                      errno_message("duplicate socket parent descriptor", errno));
            return nullptr;
        }
        return create_from_parent_fd(duplicate_fd,
                                     expected_parent_device,
                                     expected_parent_inode,
                                     std::move(socket_path),
                                     std::move(socket_leaf),
                                     std::move(transport_config),
                                     error_out);
    } catch (const std::exception& exception) {
        set_error(error_out, exception.what());
    } catch (...) {
        set_error(error_out,
                  "failed to initialize Unix socket listener from parent descriptor");
    }
    return nullptr;
}

std::unique_ptr<SpatialRoiUnixSocketListener>
SpatialRoiUnixSocketListener::create_from_parent_fd(
    const int parent_fd,
    const dev_t expected_parent_device,
    const ino_t expected_parent_inode,
    std::string socket_path,
    std::string socket_leaf,
    SpatialRoiUnixSocketTransportConfig transport_config,
    std::string* error_out) noexcept
{
    ScopedFd owned_parent_fd(parent_fd);
    PathIdentity identity;
    bool path_was_bound = false;
    try {
        if (parent_fd < 0) {
            set_error(error_out, "socket parent descriptor is invalid");
            return nullptr;
        }
        struct stat parent_stat {
        };
        if (::fstat(parent_fd, &parent_stat) != 0) {
            set_error(error_out,
                      errno_message("fstat socket parent directory", errno));
            return nullptr;
        }
        if (!S_ISDIR(parent_stat.st_mode) ||
            parent_stat.st_uid != ::geteuid() ||
            (parent_stat.st_mode & 0777) != 0700 ||
            parent_stat.st_dev != expected_parent_device ||
            parent_stat.st_ino != expected_parent_inode) {
            set_error(error_out,
                      "socket parent descriptor is not the expected euid-owned 0700 directory");
            return nullptr;
        }

        if (!transport_config.expected_peer_pid.has_value() ||
            !transport_config.expected_peer_uid.has_value()) {
            set_error(error_out,
                      "listener requires expected peer pid and uid credentials");
            return nullptr;
        }
        if (*transport_config.expected_peer_pid <= 0 ||
            *transport_config.expected_peer_uid == static_cast<uid_t>(-1)) {
            set_error(error_out,
                      "listener expected peer credentials are invalid");
            return nullptr;
        }

        struct stat existing {
        };
        if (::fstatat(parent_fd,
                      socket_leaf.c_str(),
                      &existing,
                      AT_SYMLINK_NOFOLLOW) == 0) {
            set_error(error_out, "socket path already exists; refusing to replace it");
            return nullptr;
        }
        if (errno != ENOENT) {
            const int error_number = errno;
            set_error(error_out,
                      errno_message("fstatat socket path", error_number));
            return nullptr;
        }

        const int bound_fd = ::socket(AF_UNIX,
                                      SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
                                      0);
        if (bound_fd < 0) {
            set_error(error_out, errno_message("socket", errno));
            return nullptr;
        }
        ScopedFd owned_fd(bound_fd);

        const int descriptor_flags = ::fcntl(bound_fd, F_GETFD);
        if (descriptor_flags < 0 || (descriptor_flags & FD_CLOEXEC) == 0) {
            set_error(error_out,
                      descriptor_flags < 0
                          ? errno_message("fcntl(F_GETFD)", errno)
                          : "listener socket is not close-on-exec");
            return nullptr;
        }
        const int status_flags = ::fcntl(bound_fd, F_GETFL);
        if (status_flags < 0 || (status_flags & O_NONBLOCK) == 0) {
            set_error(error_out,
                      status_flags < 0
                          ? errno_message("fcntl(F_GETFL)", errno)
                          : "listener socket is not nonblocking");
            return nullptr;
        }

        // Linux bind(2) has no bindat(2).  Resolve the retained parent fd
        // through procfs so a mutable absolute-path ancestor cannot redirect
        // creation.  Reject paths that cannot fit instead of falling back to
        // an unauthenticated pathname bind.
        sockaddr_un address{};
        const std::string descriptor_relative_path =
            "/proc/self/fd/" + std::to_string(parent_fd) + "/" + socket_leaf;
        if (descriptor_relative_path.size() >= sizeof(address.sun_path)) {
            set_error(error_out,
                      "descriptor-relative socket path exceeds filesystem bound");
            return nullptr;
        }
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path,
                    descriptor_relative_path.data(),
                    descriptor_relative_path.size());
        address.sun_path[descriptor_relative_path.size()] = '\0';
        const socklen_t address_length = static_cast<socklen_t>(
            offsetof(sockaddr_un, sun_path) + descriptor_relative_path.size() + 1);
        if (::bind(bound_fd,
                   reinterpret_cast<const sockaddr*>(&address),
                   address_length) != 0) {
            set_error(error_out, errno_message("bind", errno));
            return nullptr;
        }
        path_was_bound = true;

        auto fail_after_bind = [&](const std::string& message)
            -> std::unique_ptr<SpatialRoiUnixSocketListener> {
            set_error(error_out, message);
            unlink_exact_entry_if_owned(parent_fd, socket_leaf, identity);
            path_was_bound = false;
            return nullptr;
        };

        struct stat bound_stat {
        };
        if (::fstat(bound_fd, &bound_stat) != 0) {
            return fail_after_bind(errno_message("fstat bound socket", errno));
        }
        if (!S_ISSOCK(bound_stat.st_mode)) {
            return fail_after_bind("bound descriptor is not a filesystem socket");
        }

        // A Linux AF_UNIX descriptor lives in sockfs, while the pathname made
        // by bind has a separate filesystem identity.  Use fstat only to
        // validate the descriptor type; cleanup and replacement checks must be
        // anchored to the path entry beneath the retained private parent fd.
        std::string identity_error;
        PathIdentity entry_identity;
        if (!capture_entry_identity(parent_fd,
                                    socket_leaf,
                                    &entry_identity,
                                    &identity_error)) {
            return fail_after_bind(
                identity_error.empty()
                    ? "bound socket leaf is not a filesystem socket"
                    : identity_error);
        }
        identity = entry_identity;

        // There is no portable fchmodat(..., AT_SYMLINK_NOFOLLOW) and Linux's
        // fchmod on a socket FD changes only the socket file object, not the
        // filesystem dentry mode.  The retained owner-only parent scopes this
        // relative fchmodat to the authenticated directory; recapture the
        // identity and mode immediately afterward.
        if (::fchmodat(parent_fd,
                       socket_leaf.c_str(),
                       static_cast<mode_t>(0600),
                       0) != 0) {
            return fail_after_bind(errno_message("fchmodat socket path", errno));
        }

        if (!capture_entry_identity(parent_fd,
                                    socket_leaf,
                                    &entry_identity,
                                    &identity_error) ||
            entry_identity.device != identity.device ||
            entry_identity.inode != identity.inode) {
            return fail_after_bind(
                "socket leaf changed while establishing listener ownership");
        }
        struct stat mode_stat {
        };
        if (::fstatat(parent_fd,
                      socket_leaf.c_str(),
                      &mode_stat,
                      AT_SYMLINK_NOFOLLOW) != 0 ||
            (mode_stat.st_mode & 0777) != 0600) {
            return fail_after_bind("listener socket mode is not exactly 0600");
        }

        struct stat absolute_stat {
        };
        if (::lstat(socket_path.c_str(), &absolute_stat) != 0 ||
            !same_identity(absolute_stat, identity)) {
            return fail_after_bind(
                "socket path does not resolve to the retained private parent");
        }

        // Gate2 deliberately uses backlog one: the endpoint admits one
        // recorder connection and does not queue reconnect attempts.
        if (::listen(bound_fd, 1) != 0) {
            return fail_after_bind(errno_message("listen", errno));
        }

        auto listener = std::unique_ptr<SpatialRoiUnixSocketListener>(
            new SpatialRoiUnixSocketListener(bound_fd,
                                             parent_fd,
                                             std::move(socket_path),
                                             std::move(socket_leaf),
                                             std::move(transport_config),
                                             {identity.device,
                                              identity.inode,
                                              identity.valid}));
        (void)owned_fd.release();
        (void)owned_parent_fd.release();
        return listener;
    } catch (const std::exception& exception) {
        set_error(error_out, exception.what());
    } catch (...) {
        set_error(error_out, "failed to initialize Unix socket listener");
    }
    if (path_was_bound) {
        unlink_exact_entry_if_owned(parent_fd, socket_leaf, identity);
    }
    return nullptr;
}

std::unique_ptr<SpatialRoiUnixSocketListener>
SpatialRoiUnixSocketListener::Create(
    std::string socket_path,
    SpatialRoiUnixSocketTransportConfig transport_config,
    std::string* error_out) noexcept
{
    SpatialRoiUnixSocketListenerConfig config;
    try {
        config.socket_path = std::move(socket_path);
        config.transport_config = std::move(transport_config);
    } catch (const std::exception& exception) {
        set_error(error_out, exception.what());
        return nullptr;
    } catch (...) {
        set_error(error_out, "failed to initialize Unix socket listener config");
        return nullptr;
    }
    return Create(config, error_out);
}

std::unique_ptr<SpatialRoiUnixSocketListener>
SpatialRoiUnixSocketListener::Bind(
    const SpatialRoiUnixSocketListenerConfig& config,
    std::string* error_out) noexcept
{
    return Create(config, error_out);
}

std::unique_ptr<SpatialRoiUnixSocketListener>
SpatialRoiUnixSocketListener::Bind(
    std::string socket_path,
    SpatialRoiUnixSocketTransportConfig transport_config,
    std::string* error_out) noexcept
{
    return Create(std::move(socket_path),
                  std::move(transport_config),
                  error_out);
}

SpatialRoiUnixSocketListener::~SpatialRoiUnixSocketListener()
{
    Close();
}

void SpatialRoiUnixSocketListener::close_listener() noexcept
{
    const int descriptor = std::exchange(listener_fd_, -1);
    if (descriptor >= 0) {
        (void)::close(descriptor);
    }
}

void SpatialRoiUnixSocketListener::close_parent_directory() noexcept
{
    const int descriptor = std::exchange(parent_fd_, -1);
    if (descriptor >= 0) {
        (void)::close(descriptor);
    }
}

bool SpatialRoiUnixSocketListener::capture_current_identity(
    SocketIdentity* identity,
    std::string* error_out) const noexcept
{
    PathIdentity captured;
    if (!capture_entry_identity(parent_fd_,
                                socket_leaf_,
                                &captured,
                                error_out)) {
        return false;
    }
    if (identity) {
        identity->device = captured.device;
        identity->inode = captured.inode;
        identity->valid = captured.valid;
    }
    return true;
}

bool SpatialRoiUnixSocketListener::verify_current_binding(
    std::string* error_out) const noexcept
{
    if (parent_fd_ < 0 || socket_leaf_.empty()) {
        set_error(error_out, "listener private parent directory is closed");
        return false;
    }
    SocketIdentity current;
    if (!capture_current_identity(&current, error_out)) {
        return false;
    }
    if (!socket_identity_.valid || current.device != socket_identity_.device ||
        current.inode != socket_identity_.inode) {
        set_error(error_out,
                  "listener socket path was replaced; refusing cleanup or accept");
        return false;
    }

    // The retained parent descriptor is the cleanup authority, but the
    // authenticated contract still names an exact absolute spelling.  Check
    // that spelling as well so replacing an ancestor directory cannot leave
    // us accepting on an orphaned endpoint that clients can no longer reach.
    struct stat absolute {
    };
    const PathIdentity expected_identity{socket_identity_.device,
                                        socket_identity_.inode,
                                        socket_identity_.valid};
    if (::lstat(socket_path_.c_str(), &absolute) != 0 ||
        !same_identity(absolute, expected_identity)) {
        set_error(error_out,
                  "listener socket path was replaced; refusing cleanup or accept");
        return false;
    }
    return true;
}

void SpatialRoiUnixSocketListener::remove_own_socket_if_present() noexcept
{
    if (!owns_path_ || parent_fd_ < 0) {
        return;
    }
    PathIdentity identity{socket_identity_.device,
                          socket_identity_.inode,
                          socket_identity_.valid};
    struct stat current {
    };
    if (::fstatat(parent_fd_,
                  socket_leaf_.c_str(),
                  &current,
                  AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) {
            owns_path_ = false;
        }
        return;
    }
    if (!same_identity(current, identity)) {
        // The current entry belongs to somebody else.  Do not retry against
        // it on a later destructor/Close call.
        owns_path_ = false;
        return;
    }
    (void)::unlinkat(parent_fd_, socket_leaf_.c_str(), 0);
    struct stat after_unlink {
    };
    if (::fstatat(parent_fd_,
                  socket_leaf_.c_str(),
                  &after_unlink,
                  AT_SYMLINK_NOFOLLOW) != 0 &&
        errno == ENOENT) {
        owns_path_ = false;
    }
}

void SpatialRoiUnixSocketListener::fail_closed() noexcept
{
    close_listener();
    remove_own_socket_if_present();
    close_parent_directory();
}

void SpatialRoiUnixSocketListener::Close() noexcept
{
    close_listener();
    remove_own_socket_if_present();
    close_parent_directory();
}

SpatialRoiUnixSocketListenerAcceptResult
SpatialRoiUnixSocketListener::accept_one_until(
    const std::chrono::steady_clock::time_point deadline) noexcept
{
    try {
        if (listener_fd_ < 0) {
            return make_result(
                SpatialRoiUnixSocketListenerAcceptStatus::kClosed,
                "Unix socket listener is closed");
        }
        if (accepted_) {
            return make_result(
                SpatialRoiUnixSocketListenerAcceptStatus::kClosed,
                "Unix socket listener already consumed its one connection");
        }

        std::string binding_error;
        if (!verify_current_binding(&binding_error)) {
            fail_closed();
            SpatialRoiUnixSocketListenerAcceptResult result = make_result(
                SpatialRoiUnixSocketListenerAcceptStatus::kError);
            set_error(&result.error, binding_error);
            return result;
        }

        while (true) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return make_result(
                    SpatialRoiUnixSocketListenerAcceptStatus::kTimeout,
                    "Unix socket accept timed out");
            }
            pollfd descriptor{};
            descriptor.fd = listener_fd_;
            descriptor.events = POLLIN;
            const int poll_result =
                ::poll(&descriptor, 1, poll_timeout_milliseconds(deadline));
            if (poll_result == 0) {
                return make_result(
                    SpatialRoiUnixSocketListenerAcceptStatus::kTimeout,
                    "Unix socket accept timed out");
            }
            if (poll_result < 0) {
                if (errno == EINTR) {
                    continue;
                }
                const std::string error = errno_message("poll", errno);
                fail_closed();
                SpatialRoiUnixSocketListenerAcceptResult result = make_result(
                    SpatialRoiUnixSocketListenerAcceptStatus::kError);
                set_error(&result.error, error);
                return result;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return make_result(
                    SpatialRoiUnixSocketListenerAcceptStatus::kTimeout,
                    "Unix socket accept timed out");
            }
            if ((descriptor.revents & POLLNVAL) != 0) {
                fail_closed();
                return make_result(
                    SpatialRoiUnixSocketListenerAcceptStatus::kError,
                    "poll reported an invalid listener descriptor");
            }
            if ((descriptor.revents & POLLIN) == 0) {
                if ((descriptor.revents & (POLLERR | POLLHUP)) != 0) {
                    fail_closed();
                    return make_result(
                        SpatialRoiUnixSocketListenerAcceptStatus::kError,
                        "poll reported a listener error");
                }
                continue;
            }

            // The pathname is mutable independently of the listening fd.  A
            // replacement may have happened while poll was sleeping, so the
            // identity check is repeated immediately before every accept4.
            binding_error.clear();
            if (!verify_current_binding(&binding_error)) {
                fail_closed();
                SpatialRoiUnixSocketListenerAcceptResult result = make_result(
                    SpatialRoiUnixSocketListenerAcceptStatus::kError);
                set_error(&result.error, binding_error);
                return result;
            }

            sockaddr_storage peer_address{};
            socklen_t peer_address_length = sizeof(peer_address);
            const int accepted_fd = ::accept4(
                listener_fd_,
                reinterpret_cast<sockaddr*>(&peer_address),
                &peer_address_length,
                SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (accepted_fd >= 0) {
                ScopedFd owned_fd(accepted_fd);
                accepted_ = true;
                // Close and unlink before adoption.  A credential mismatch
                // still consumes the one connection and cannot trigger a
                // reconnect against a new process.
                close_listener();
                remove_own_socket_if_present();

                std::string adoption_error;
                auto transport = SpatialRoiUnixSocketLineTransport::AdoptConnectedFd(
                    owned_fd.get(), transport_config_, &adoption_error);
                if (!transport) {
                    SpatialRoiUnixSocketListenerAcceptResult result = make_result(
                        SpatialRoiUnixSocketListenerAcceptStatus::kError);
                    set_error(&result.error, adoption_error);
                    return result;
                }
                (void)owned_fd.release();
                SpatialRoiUnixSocketListenerAcceptResult result = make_result(
                    SpatialRoiUnixSocketListenerAcceptStatus::kAccepted);
                result.transport = std::move(transport);
                return result;
            }
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ||
                errno == ECONNABORTED) {
                continue;
            }
            const std::string error = errno_message("accept4", errno);
            fail_closed();
            SpatialRoiUnixSocketListenerAcceptResult result = make_result(
                SpatialRoiUnixSocketListenerAcceptStatus::kError);
            set_error(&result.error, error);
            return result;
        }
    } catch (const std::exception& exception) {
        fail_closed();
        return make_result(SpatialRoiUnixSocketListenerAcceptStatus::kError,
                           exception.what());
    } catch (...) {
        fail_closed();
        return make_result(SpatialRoiUnixSocketListenerAcceptStatus::kError,
                           "Unix socket accept failed");
    }
}

SpatialRoiUnixSocketListenerAcceptResult
SpatialRoiUnixSocketListener::AcceptOneResult(
    const std::chrono::milliseconds timeout) noexcept
{
    if (timeout.count() <= 0 || timeout > kMaximumAcceptWait) {
        fail_closed();
        return make_result(
            SpatialRoiUnixSocketListenerAcceptStatus::kError,
            "accept timeout must be positive and at most five minutes");
    }
    try {
        return AcceptOneResult(std::chrono::steady_clock::now() + timeout);
    } catch (const std::exception& exception) {
        fail_closed();
        return make_result(SpatialRoiUnixSocketListenerAcceptStatus::kError,
                           exception.what());
    } catch (...) {
        fail_closed();
        return make_result(SpatialRoiUnixSocketListenerAcceptStatus::kError,
                           "failed to establish accept deadline");
    }
}

SpatialRoiUnixSocketListenerAcceptResult
SpatialRoiUnixSocketListener::AcceptOneResult(
    const std::chrono::steady_clock::time_point deadline) noexcept
{
    const auto now = std::chrono::steady_clock::now();
    if (deadline <= now || deadline - now > kMaximumAcceptWait) {
        fail_closed();
        return make_result(
            SpatialRoiUnixSocketListenerAcceptStatus::kError,
            "accept deadline must be in the future and at most five minutes away");
    }
    return accept_one_until(deadline);
}

std::unique_ptr<SpatialRoiUnixSocketLineTransport>
SpatialRoiUnixSocketListener::AcceptOne(
    const std::chrono::milliseconds timeout,
    std::string* error_out) noexcept
{
    if (error_out) {
        error_out->clear();
    }
    auto result = AcceptOneResult(timeout);
    if (result.transport) {
        return std::move(result.transport);
    }
    set_error(error_out, result.error);
    return nullptr;
}

std::unique_ptr<SpatialRoiUnixSocketLineTransport>
SpatialRoiUnixSocketListener::AcceptOne(
    const std::chrono::steady_clock::time_point deadline,
    std::string* error_out) noexcept
{
    if (error_out) {
        error_out->clear();
    }
    auto result = AcceptOneResult(deadline);
    if (result.transport) {
        return std::move(result.transport);
    }
    set_error(error_out, result.error);
    return nullptr;
}

}  // namespace orange::spatial_roi::ipc
