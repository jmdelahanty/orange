#include "spatial_roi_unix_socket_connector.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace orange::spatial_roi::ipc {
namespace {

constexpr auto kMaximumConnectWait = std::chrono::minutes(5);
constexpr std::size_t kMaximumErrorBytes = kSpatialRoiIpcMaxTextBytes;

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

bool set_error(std::string* error_out, const std::string_view message) noexcept
{
    if (!error_out) {
        return false;
    }
    try {
        error_out->assign(message.data(),
                          std::min(message.size(), kMaximumErrorBytes));
    } catch (...) {
        error_out->clear();
    }
    return false;
}

std::string errno_message(const char* operation, const int error_number)
{
    std::string result = operation ? operation : "Unix socket connect";
    result += " failed: ";
    const char* detail = std::strerror(error_number);
    if (detail) {
        result += detail;
    }
    return result;
}

bool valid_exact_path(const std::string& path,
                      std::string* error_out) noexcept
{
    try {
        if (path.empty() || path.front() != '/' ||
            path.find('\0') != std::string::npos ||
            path.size() >= sizeof(sockaddr_un{}.sun_path)) {
            return set_error(error_out,
                             "connector socket path is not a bounded absolute filesystem path");
        }
        std::size_t begin = 1;
        std::size_t components = 0;
        while (begin < path.size()) {
            const std::size_t end = path.find('/', begin);
            const std::size_t length =
                end == std::string::npos ? path.size() - begin : end - begin;
            if (length == 0) {
                return set_error(error_out,
                                 "connector socket path contains an empty component");
            }
            const std::string_view component(path.data() + begin, length);
            if (component == "." || component == "..") {
                return set_error(error_out,
                                 "connector socket path contains an unsafe component");
            }
            ++components;
            if (end == std::string::npos) {
                break;
            }
            begin = end + 1;
        }
        if (components < 2 || path.back() == '/') {
            return set_error(error_out,
                             "connector socket path lacks a private parent and leaf");
        }
        return true;
    } catch (const std::exception& exception) {
        return set_error(error_out, exception.what());
    } catch (...) {
        return set_error(error_out, "connector socket path validation failed");
    }
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

bool wait_for_connect(const int fd,
                      const std::chrono::steady_clock::time_point deadline,
                      std::string* error_out) noexcept
{
    for (;;) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return set_error(error_out, "Unix socket connect timed out");
        }
        pollfd descriptor{};
        descriptor.fd = fd;
        descriptor.events = POLLOUT;
        const int result =
            ::poll(&descriptor, 1, poll_timeout_milliseconds(deadline));
        if (result == 0) {
            return set_error(error_out, "Unix socket connect timed out");
        }
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            try {
                return set_error(error_out, errno_message("poll(connect)", errno));
            } catch (...) {
                return set_error(error_out, "poll(connect) failed");
            }
        }
        if ((descriptor.revents & POLLNVAL) != 0) {
            return set_error(error_out,
                             "poll(connect) reported an invalid descriptor");
        }
        int socket_error = 0;
        socklen_t socket_error_size = sizeof(socket_error);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error,
                         &socket_error_size) != 0 ||
            socket_error_size != sizeof(socket_error)) {
            try {
                return set_error(error_out,
                                 errno_message("getsockopt(SO_ERROR)", errno));
            } catch (...) {
                return set_error(error_out, "getsockopt(SO_ERROR) failed");
            }
        }
        if (socket_error != 0) {
            try {
                return set_error(error_out,
                                 errno_message("connect", socket_error));
            } catch (...) {
                return set_error(error_out, "Unix socket connect failed");
            }
        }
        if ((descriptor.revents & POLLOUT) != 0) {
            return true;
        }
        return set_error(error_out,
                         "poll(connect) returned without a connected endpoint");
    }
}

}  // namespace

std::unique_ptr<SpatialRoiUnixSocketLineTransport>
SpatialRoiUnixSocketConnector::Connect(
    const SpatialRoiUnixSocketConnectorConfig& config,
    std::string* error_out) noexcept
{
    if (error_out) {
        error_out->clear();
    }
    try {
        if (!valid_exact_path(config.socket_path, error_out)) {
            return nullptr;
        }
        if (config.connect_timeout.count() <= 0 ||
            config.connect_timeout > kMaximumConnectWait) {
            set_error(error_out,
                      "connect timeout must be positive and at most five minutes");
            return nullptr;
        }
        if (!config.transport_config.expected_peer_pid.has_value() ||
            !config.transport_config.expected_peer_uid.has_value() ||
            *config.transport_config.expected_peer_pid <= 0 ||
            *config.transport_config.expected_peer_uid ==
                static_cast<uid_t>(-1)) {
            set_error(error_out,
                      "connector requires the expected recorder pid and uid");
            return nullptr;
        }

        // The exact endpoint is derived inside the authenticated, private
        // runtime directory.  SO_PEERCRED below is the server-identity
        // authority; this pathname check supplies the filesystem policy and
        // rejects accidental substitution before connect.
        struct stat endpoint_status {};
        if (::lstat(config.socket_path.c_str(), &endpoint_status) != 0) {
            set_error(error_out, errno_message("lstat(socket endpoint)", errno));
            return nullptr;
        }
        if (!S_ISSOCK(endpoint_status.st_mode) ||
            endpoint_status.st_uid != ::geteuid() ||
            (endpoint_status.st_mode & 0777) != 0600) {
            set_error(error_out,
                      "connector endpoint must be an euid-owned mode-0600 socket");
            return nullptr;
        }

        ScopedFd fd(::socket(AF_UNIX,
                             SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
                             0));
        if (fd.get() < 0) {
            set_error(error_out, errno_message("socket", errno));
            return nullptr;
        }
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, config.socket_path.data(),
                    config.socket_path.size());
        address.sun_path[config.socket_path.size()] = '\0';
        const socklen_t address_length = static_cast<socklen_t>(
            offsetof(sockaddr_un, sun_path) + config.socket_path.size() + 1);
        const auto deadline =
            std::chrono::steady_clock::now() + config.connect_timeout;
        int result = -1;
        do {
            result = ::connect(fd.get(),
                               reinterpret_cast<const sockaddr*>(&address),
                               address_length);
        } while (result != 0 && errno == EINTR &&
                 std::chrono::steady_clock::now() < deadline);
        if (result != 0 && errno != EINPROGRESS && errno != EALREADY) {
            set_error(error_out, errno_message("connect", errno));
            return nullptr;
        }
        if (result != 0 && !wait_for_connect(fd.get(), deadline, error_out)) {
            return nullptr;
        }

        // Adoption repeats descriptor type, family, connected-peer, flags,
        // and SO_PEERCRED validation.  It owns and closes fd even on failure.
        const int adopted_fd = fd.release();
        return SpatialRoiUnixSocketLineTransport::AdoptConnectedFd(
            adopted_fd, config.transport_config, error_out);
    } catch (const std::exception& exception) {
        set_error(error_out, exception.what());
        return nullptr;
    } catch (...) {
        set_error(error_out, "Unix socket connector failed");
        return nullptr;
    }
}

}  // namespace orange::spatial_roi::ipc
