#include "spatial_roi_unix_socket_transport.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace orange::spatial_roi::ipc {
namespace {

constexpr std::size_t kMaxErrorBytes = kSpatialRoiIpcMaxTextBytes;
constexpr auto kMaxTransportTimeout = std::chrono::minutes(5);

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

private:
    int fd_ = -1;
};

std::string errno_message(const char* operation, int error_number)
{
    std::string message = operation ? operation : "socket operation";
    message += " failed: ";
    message += std::strerror(error_number);
    return message;
}

std::chrono::steady_clock::time_point deadline_after(
    const std::chrono::milliseconds timeout)
{
    return std::chrono::steady_clock::now() + timeout;
}

int poll_timeout_milliseconds(
    const std::chrono::steady_clock::time_point deadline) noexcept
{
    if (deadline == std::chrono::steady_clock::time_point::max()) {
        return -1;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return 0;
    }
    const auto remaining = deadline - now;
    auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
    // poll(2) truncates to milliseconds, so round a positive sub-millisecond
    // remainder upward.  This preserves the caller's bounded deadline while
    // avoiding an accidental busy loop.
    if (remaining > std::chrono::milliseconds(milliseconds)) {
        ++milliseconds;
    }
    if (milliseconds > std::numeric_limits<int>::max()) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(milliseconds);
}

}  // namespace

SpatialRoiUnixSocketLineTransport::SpatialRoiUnixSocketLineTransport(
    const int fd,
    const int cancellation_fd,
    SpatialRoiUnixSocketTransportConfig config,
    const SpatialRoiUnixSocketPeerCredentials peer_credentials)
    : fd_(fd),
      cancellation_fd_(cancellation_fd),
      config_(std::move(config)),
      peer_credentials_(peer_credentials)
{
    // Reserve at least the configured logical bound so normal receives do not
    // reallocate. std::string may reserve a larger physical capacity, but the
    // code never admits more than max_receive_buffer_bytes logical bytes.
    // The factory constructs this object inside an fd-RAII scope, so an
    // allocation failure still closes the adopted descriptor exactly once.
    receive_buffer_.reserve(config_.max_receive_buffer_bytes);
}

std::unique_ptr<SpatialRoiUnixSocketLineTransport>
SpatialRoiUnixSocketLineTransport::AdoptConnectedFd(
    const int fd,
    SpatialRoiUnixSocketTransportConfig config,
    std::string* error_out) noexcept
{
    if (error_out) {
        error_out->clear();
    }

    if (fd < 0) {
        set_error(error_out, "cannot adopt a negative socket descriptor");
        return nullptr;
    }

    ScopedFd owned_fd(fd);
    auto fail = [&](const std::string_view message)
        -> std::unique_ptr<SpatialRoiUnixSocketLineTransport> {
        set_error(error_out, message);
        return nullptr;
    };

    try {
        if (config.max_wire_message_bytes == 0 ||
            config.max_wire_message_bytes > kSpatialRoiIpcMaxWireMessageBytes) {
            return fail("max_wire_message_bytes is outside the protocol bound");
        }
        if (config.max_receive_buffer_bytes == 0 ||
            config.max_receive_buffer_bytes > config.max_wire_message_bytes) {
            return fail(
                "max_receive_buffer_bytes must be positive and no larger than the wire bound");
        }
        if (config.write_timeout.count() <= 0 ||
            config.write_timeout > kMaxTransportTimeout) {
            return fail("write_timeout must be positive and at most five minutes");
        }

        int socket_type = 0;
        socklen_t socket_type_length = sizeof(socket_type);
        if (::getsockopt(owned_fd.get(),
                         SOL_SOCKET,
                         SO_TYPE,
                         &socket_type,
                         &socket_type_length) != 0) {
            return fail(errno_message("getsockopt(SO_TYPE)", errno));
        }
        if (socket_type_length != sizeof(socket_type) ||
            socket_type != SOCK_STREAM) {
            return fail("adopted descriptor is not a SOCK_STREAM socket");
        }

        sockaddr_storage local_address{};
        socklen_t local_address_length = sizeof(local_address);
        if (::getsockname(owned_fd.get(),
                          reinterpret_cast<sockaddr*>(&local_address),
                          &local_address_length) != 0) {
            return fail(errno_message("getsockname", errno));
        }
        if (local_address.ss_family != AF_UNIX) {
            return fail("adopted descriptor is not an AF_UNIX socket");
        }

        sockaddr_storage peer_address{};
        socklen_t peer_address_length = sizeof(peer_address);
        if (::getpeername(owned_fd.get(),
                          reinterpret_cast<sockaddr*>(&peer_address),
                          &peer_address_length) != 0) {
            return fail(errno_message("getpeername", errno));
        }
        if (peer_address.ss_family != AF_UNIX) {
            return fail("adopted descriptor peer is not an AF_UNIX socket");
        }

        SpatialRoiUnixSocketPeerCredentials peer_credentials;
#if defined(SO_PEERCRED)
        struct ucred credentials {
        };
        socklen_t credentials_length = sizeof(credentials);
        if (::getsockopt(owned_fd.get(),
                         SOL_SOCKET,
                         SO_PEERCRED,
                         &credentials,
                         &credentials_length) != 0) {
            return fail(errno_message("getsockopt(SO_PEERCRED)", errno));
        }
        if (credentials_length != sizeof(credentials)) {
            return fail("getsockopt(SO_PEERCRED) returned an invalid length");
        }
        peer_credentials.pid = credentials.pid;
        peer_credentials.uid = credentials.uid;
        peer_credentials.gid = credentials.gid;
        if (config.expected_peer_pid.has_value() &&
            peer_credentials.pid != *config.expected_peer_pid) {
            return fail("SO_PEERCRED pid does not match expected peer pid");
        }
        if (config.expected_peer_uid.has_value() &&
            peer_credentials.uid != *config.expected_peer_uid) {
            return fail("SO_PEERCRED uid does not match expected peer uid");
        }
#else
        if (config.expected_peer_pid.has_value() ||
            config.expected_peer_uid.has_value()) {
            return fail("SO_PEERCRED is unavailable on this platform");
        }
#endif

        const int descriptor_flags = ::fcntl(owned_fd.get(), F_GETFD);
        if (descriptor_flags < 0) {
            return fail(errno_message("fcntl(F_GETFD)", errno));
        }
        if (::fcntl(owned_fd.get(),
                    F_SETFD,
                    descriptor_flags | FD_CLOEXEC) != 0) {
            return fail(errno_message("fcntl(FD_CLOEXEC)", errno));
        }

        const int status_flags = ::fcntl(owned_fd.get(), F_GETFL);
        if (status_flags < 0) {
            return fail(errno_message("fcntl(F_GETFL)", errno));
        }
        if ((status_flags & O_NONBLOCK) == 0 &&
            ::fcntl(owned_fd.get(), F_SETFL, status_flags | O_NONBLOCK) != 0) {
            return fail(errno_message("fcntl(O_NONBLOCK)", errno));
        }

        ScopedFd cancellation_fd(
            ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
        if (cancellation_fd.get() < 0) {
            return fail(errno_message("eventfd", errno));
        }

        auto transport = std::unique_ptr<SpatialRoiUnixSocketLineTransport>(
            new SpatialRoiUnixSocketLineTransport(
                owned_fd.get(), cancellation_fd.get(), std::move(config),
                peer_credentials));
        (void)owned_fd.release();
        (void)cancellation_fd.release();
        return transport;
    } catch (const std::exception& exception) {
        set_error(error_out, exception.what());
        return nullptr;
    } catch (...) {
        set_error(error_out, "failed to initialize socket transport");
        return nullptr;
    }
}

SpatialRoiUnixSocketLineTransport::~SpatialRoiUnixSocketLineTransport()
{
    Close();
}

bool SpatialRoiUnixSocketLineTransport::RequestShutdown(
    std::string* error_out) noexcept
{
    if (error_out) {
        error_out->clear();
    }
    // Record the request only after the wakeup capability has accepted the
    // signal.  If the descriptor is closed or the write fails, a later caller
    // must be able to retry instead of observing a permanently-successful
    // but unsignalled request.
    if (shutdown_requested_.load(std::memory_order_acquire)) {
        return true;
    }
    if (cancellation_fd_ < 0) {
        set_error(error_out,
                  "socket transport cancellation capability is closed");
        return false;
    }
    const std::uint64_t signal = 1;
    while (true) {
        const ssize_t written =
            ::write(cancellation_fd_, &signal, sizeof(signal));
        if (written == static_cast<ssize_t>(sizeof(signal))) {
            shutdown_requested_.store(true, std::memory_order_release);
            return true;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        // A saturated nonblocking eventfd is already signalled.
        if (written < 0 && errno == EAGAIN) {
            shutdown_requested_.store(true, std::memory_order_release);
            return true;
        }
        set_error(error_out, "eventfd cancellation signal failed");
        return false;
    }
}

bool SpatialRoiUnixSocketLineTransport::set_error(
    std::string* error_out,
    const std::string_view message) noexcept
{
    if (!error_out) {
        return false;
    }
    try {
        error_out->assign(message.data(), std::min(message.size(), kMaxErrorBytes));
    } catch (...) {
        error_out->clear();
        return false;
    }
    return true;
}

std::string SpatialRoiUnixSocketLineTransport::bounded_error(
    const char* message) noexcept
{
    try {
        return bounded_error(std::string(message ? message : ""));
    } catch (...) {
        return {};
    }
}

std::string SpatialRoiUnixSocketLineTransport::bounded_error(
    const std::string& message) noexcept
{
    try {
        return message.substr(0, std::min(message.size(), kMaxErrorBytes));
    } catch (...) {
        return {};
    }
}

SpatialRoiUnixSocketLineTransport::PollWaitStatus
SpatialRoiUnixSocketLineTransport::wait_for(
    const short events,
    const std::chrono::steady_clock::time_point deadline,
    bool allow_immediate_probe,
    std::string* error_out) noexcept
{
    if (fd_ < 0) {
        set_error(error_out, "socket transport is closed");
        return PollWaitStatus::kError;
    }
    while (true) {
        const bool this_is_immediate_probe = allow_immediate_probe;
        if (!allow_immediate_probe &&
            deadline != std::chrono::steady_clock::time_point::max() &&
            std::chrono::steady_clock::now() >= deadline) {
            return PollWaitStatus::kTimeout;
        }
        std::array<pollfd, 2> descriptors{};
        descriptors[0].fd = fd_;
        descriptors[0].events = events;
        descriptors[1].fd = cancellation_fd_;
        descriptors[1].events = POLLIN;
        const int timeout = poll_timeout_milliseconds(deadline);
        const nfds_t descriptor_count = cancellation_fd_ >= 0 ? 2U : 1U;
        const int result = ::poll(descriptors.data(), descriptor_count, timeout);
        allow_immediate_probe = false;
        if (result == 0) {
            return PollWaitStatus::kTimeout;
        }
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            set_error(error_out, "poll failed");
            return PollWaitStatus::kError;
        }
        if (!this_is_immediate_probe &&
            deadline != std::chrono::steady_clock::time_point::max() &&
            std::chrono::steady_clock::now() >= deadline) {
            return PollWaitStatus::kTimeout;
        }
        if (descriptor_count == 2U && descriptors[1].revents != 0) {
            set_error(error_out,
                      "socket transport lifecycle cancellation requested");
            return PollWaitStatus::kError;
        }
        if (descriptors[0].revents & POLLNVAL) {
            set_error(error_out, "poll reported an invalid socket descriptor");
            return PollWaitStatus::kError;
        }
        if ((descriptors[0].revents & events) != 0 ||
            (descriptors[0].revents & (POLLERR | POLLHUP)) != 0) {
            return PollWaitStatus::kReady;
        }
        // Ignore unrelated events and keep the same absolute deadline.
    }
}

void SpatialRoiUnixSocketLineTransport::close_with_error(
    const std::string_view message) noexcept
{
    terminal_ = true;
    terminal_status_ = SpatialRoiIpcTransportReadStatus::kError;
    const int descriptor = std::exchange(fd_, -1);
    if (descriptor >= 0) {
        (void)::close(descriptor);
    }
    receive_buffer_.clear();
    try {
        terminal_error_.assign(
            message.data(), std::min(message.size(), kMaxErrorBytes));
    } catch (...) {
        terminal_error_.clear();
    }
}

void SpatialRoiUnixSocketLineTransport::Close() noexcept
{
    const int descriptor = std::exchange(fd_, -1);
    if (descriptor >= 0) {
        (void)::close(descriptor);
    }
    const int cancellation_descriptor =
        std::exchange(cancellation_fd_, -1);
    if (cancellation_descriptor >= 0) {
        (void)::close(cancellation_descriptor);
    }
    receive_buffer_.clear();
    if (!terminal_) {
        terminal_ = true;
        terminal_status_ = SpatialRoiIpcTransportReadStatus::kError;
        try {
            terminal_error_ = "socket transport is closed";
        } catch (...) {
            terminal_error_.clear();
        }
    }
}

SpatialRoiIpcTransportReadResult
SpatialRoiUnixSocketLineTransport::make_read_result(
    const SpatialRoiIpcTransportReadStatus status,
    std::string line,
    const std::string& error) const noexcept
{
    SpatialRoiIpcTransportReadResult result;
    result.status = status;
    try {
        result.line = std::move(line);
        result.error = bounded_error(error);
    } catch (...) {
        result.line.clear();
        result.error.clear();
    }
    return result;
}

SpatialRoiIpcTransportReadResult
SpatialRoiUnixSocketLineTransport::terminal_read_result() const noexcept
{
    return make_read_result(terminal_status_, {}, terminal_error_);
}

bool SpatialRoiUnixSocketLineTransport::WriteLine(
    const std::string& line,
    std::string* error_out)
{
    if (error_out) {
        error_out->clear();
    }
    if (terminal_ || fd_ < 0) {
        set_error(error_out, terminal_error_.empty()
                                ? "socket transport is closed"
                                : terminal_error_);
        return false;
    }
    if (line.empty() || line.back() != '\n' ||
        line.find('\n') != line.size() - 1) {
        set_error(error_out,
                  "socket transport requires exactly one trailing newline");
        return false;
    }
    if (line.size() > config_.max_wire_message_bytes) {
        set_error(error_out, "socket transport line exceeds wire bound");
        return false;
    }

    const auto deadline = deadline_after(config_.write_timeout);
    std::size_t offset = 0;
    bool allow_immediate_probe = false;
    while (offset < line.size()) {
        std::string wait_error;
        const PollWaitStatus wait_status =
            wait_for(POLLOUT, deadline, allow_immediate_probe, &wait_error);
        allow_immediate_probe = false;
        if (wait_status == PollWaitStatus::kTimeout) {
            close_with_error("socket transport write timed out");
            set_error(error_out, terminal_error_);
            return false;
        }
        if (wait_status == PollWaitStatus::kError) {
            close_with_error(
                wait_error.empty()
                    ? std::string_view("socket transport poll failed")
                    : std::string_view(wait_error));
            set_error(error_out, terminal_error_);
            return false;
        }

        const ssize_t written = ::send(fd_,
                                       line.data() + offset,
                                       line.size() - offset,
                                       MSG_NOSIGNAL | MSG_DONTWAIT);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        close_with_error(written < 0 ? "socket send failed"
                                     : "socket send returned zero bytes");
        set_error(error_out, terminal_error_);
        return false;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
        close_with_error("socket transport write timed out");
        set_error(error_out, terminal_error_);
        return false;
    }
    return true;
}

SpatialRoiIpcTransportReadResult SpatialRoiUnixSocketLineTransport::ReadLine(
    const std::chrono::milliseconds timeout,
    const std::size_t max_wire_bytes)
{
    if (terminal_ || fd_ < 0) {
        return terminal_read_result();
    }
    if (timeout.count() < 0 || timeout > kMaxTransportTimeout) {
        return make_read_result(
            SpatialRoiIpcTransportReadStatus::kError,
            {},
            "socket transport read timeout must be between zero and five minutes");
    }
    if (max_wire_bytes == 0) {
        close_with_error("socket transport read wire bound must be positive");
        return terminal_read_result();
    }

    const std::size_t effective_limit = std::min(
        {max_wire_bytes,
         config_.max_wire_message_bytes,
         config_.max_receive_buffer_bytes});
    if (effective_limit == 0) {
        close_with_error("socket transport read wire bound is empty");
        return terminal_read_result();
    }
    const auto deadline = deadline_after(timeout);
    const bool zero_timeout = timeout.count() == 0;
    bool allow_immediate_probe = zero_timeout;
    std::array<char, 4096> chunk{};
    while (true) {
        const auto newline =
            std::find(receive_buffer_.begin(), receive_buffer_.end(), '\n');
        if (newline != receive_buffer_.end()) {
            if (!zero_timeout &&
                std::chrono::steady_clock::now() >= deadline) {
                return make_read_result(
                    SpatialRoiIpcTransportReadStatus::kTimeout,
                    {},
                    "socket transport read timed out");
            }
            const std::size_t line_bytes = static_cast<std::size_t>(
                std::distance(receive_buffer_.begin(), newline)) + 1;
            if (line_bytes > effective_limit) {
                close_with_error("socket transport line exceeds wire bound");
                terminal_status_ = SpatialRoiIpcTransportReadStatus::kTooLarge;
                return terminal_read_result();
            }
            std::string line;
            try {
                line.assign(receive_buffer_.data(), line_bytes);
                receive_buffer_.erase(0, line_bytes);
            } catch (...) {
                close_with_error("socket transport line materialization failed");
                return terminal_read_result();
            }
            return make_read_result(SpatialRoiIpcTransportReadStatus::kLine,
                                    std::move(line),
                                    {});
        }
        if (receive_buffer_.size() >= effective_limit) {
            close_with_error("socket transport line exceeds wire bound");
            terminal_status_ = SpatialRoiIpcTransportReadStatus::kTooLarge;
            return terminal_read_result();
        }

        std::string wait_error;
        const PollWaitStatus wait_status =
            wait_for(POLLIN, deadline, allow_immediate_probe, &wait_error);
        allow_immediate_probe = false;
        if (wait_status == PollWaitStatus::kTimeout) {
            return make_read_result(SpatialRoiIpcTransportReadStatus::kTimeout,
                                    {},
                                    "socket transport read timed out");
        }
        if (wait_status == PollWaitStatus::kError) {
            close_with_error(
                wait_error.empty()
                    ? std::string_view("socket transport poll failed")
                    : std::string_view(wait_error));
            return terminal_read_result();
        }

        const std::size_t available = effective_limit - receive_buffer_.size();
        const std::size_t requested = std::min(available, chunk.size());
        const ssize_t received =
            ::recv(fd_, chunk.data(), requested, MSG_DONTWAIT);
        if (received > 0) {
            try {
                receive_buffer_.append(
                    chunk.data(), static_cast<std::size_t>(received));
            } catch (...) {
                close_with_error("socket transport receive buffer allocation failed");
                return terminal_read_result();
            }
            continue;
        }
        if (received == 0) {
            const bool partial_line = !receive_buffer_.empty();
            close_with_error(partial_line
                                 ? "socket peer reached EOF before line terminator"
                                 : "socket peer reached EOF");
            terminal_status_ = partial_line
                                   ? SpatialRoiIpcTransportReadStatus::kError
                                   : SpatialRoiIpcTransportReadStatus::kEof;
            return terminal_read_result();
        }
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
            continue;
        }
        close_with_error("socket receive failed");
        return terminal_read_result();
    }
}

}  // namespace orange::spatial_roi::ipc
