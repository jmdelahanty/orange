#pragma once

#include "spatial_roi_ipc_handoff.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>

namespace orange::spatial_roi::ipc {

// The transport only adopts an already-connected AF_UNIX/SOCK_STREAM file
// descriptor.  Listening, path management, and reconnect policy belong to a
// future supervisor and are intentionally outside this class. The class is
// single-owner and not thread-safe: ReadLine, WriteLine, and Close must never
// run concurrently, and callers must not retain or duplicate the adopted fd.
// RequestShutdown is the one deliberate exception: a lifecycle owner may call
// it from another thread to interrupt a bounded ReadLine/WriteLine before it
// joins the transport owner. Close must still wait until that owner has joined.
struct SpatialRoiUnixSocketTransportConfig {
    // This is also the upper bound accepted by WriteLine.  ReadLine applies
    // the smaller of this value and its per-call max_wire_bytes argument.
    std::size_t max_wire_message_bytes = kSpatialRoiIpcMaxWireMessageBytes;

    // The receive buffer is never allowed to grow beyond this value.  A line
    // that cannot fit, including its trailing newline, is rejected before it
    // is returned as a std::string.  Keeping this no larger than the wire
    // bound makes the transport's memory use independently auditable.
    std::size_t max_receive_buffer_bytes = kSpatialRoiIpcMaxWireMessageBytes;

    // WriteLine has no timeout parameter in the shared interface, so writes
    // use this bounded poll deadline.  A timeout is a transport failure and
    // closes the adopted descriptor; the handoff must not retransmit.
    std::chrono::milliseconds write_timeout{1000};

    // Empty optionals disable the corresponding credential check.  When set,
    // the value must match SO_PEERCRED obtained at adoption time. A production
    // supervisor that checks PID must accept a connection created by the
    // already-spawned recorder; a socketpair created before fork identifies
    // its creator and is not proof of the eventual exec child.
    std::optional<pid_t> expected_peer_pid;
    std::optional<uid_t> expected_peer_uid;
};

struct SpatialRoiUnixSocketPeerCredentials {
    pid_t pid = -1;
    uid_t uid = static_cast<uid_t>(-1);
    gid_t gid = static_cast<gid_t>(-1);
};

class SpatialRoiUnixSocketLineTransport final
    : public SpatialRoiIpcLineTransport {
public:
    // On success, ownership of fd is transferred to the returned transport.
    // On failure, fd is closed before returning nullptr.  The descriptor
    // must already be a connected AF_UNIX/SOCK_STREAM endpoint.
    static std::unique_ptr<SpatialRoiUnixSocketLineTransport>
    AdoptConnectedFd(
        int fd,
        SpatialRoiUnixSocketTransportConfig config = {},
        std::string* error_out = nullptr) noexcept;

    ~SpatialRoiUnixSocketLineTransport() override;

    SpatialRoiUnixSocketLineTransport(
        const SpatialRoiUnixSocketLineTransport&) = delete;
    SpatialRoiUnixSocketLineTransport& operator=(
        const SpatialRoiUnixSocketLineTransport&) = delete;
    SpatialRoiUnixSocketLineTransport(
        SpatialRoiUnixSocketLineTransport&&) = delete;
    SpatialRoiUnixSocketLineTransport& operator=(
        SpatialRoiUnixSocketLineTransport&&) = delete;

    bool WriteLine(const std::string& line,
                   std::string* error_out) override;

    SpatialRoiIpcTransportReadResult ReadLine(
        std::chrono::milliseconds timeout,
        std::size_t max_wire_bytes) override;

    // Interrupt an active owner through a private pollable event capability;
    // this method neither reads nor mutates socket-owner state. The caller must
    // join the owner before calling Close. This operation is idempotent and is
    // the only method permitted concurrently with ReadLine/WriteLine.
    bool RequestShutdown(std::string* error_out = nullptr) noexcept;

    // Closing is idempotent.  close(2) is deliberately not retried after
    // EINTR because Linux may already have released the descriptor number.
    void Close() noexcept;

    bool closed() const noexcept { return fd_ < 0; }
    const SpatialRoiUnixSocketTransportConfig& config() const noexcept
    {
        return config_;
    }
    const SpatialRoiUnixSocketPeerCredentials& peer_credentials() const noexcept
    {
        return peer_credentials_;
    }

private:
    enum class PollWaitStatus {
        kReady,
        kTimeout,
        kError,
    };

    SpatialRoiUnixSocketLineTransport(
        int fd,
        int cancellation_fd,
        SpatialRoiUnixSocketTransportConfig config,
        SpatialRoiUnixSocketPeerCredentials peer_credentials);

    static bool set_error(std::string* error_out,
                          std::string_view message) noexcept;
    static std::string bounded_error(const char* message) noexcept;
    static std::string bounded_error(const std::string& message) noexcept;

    PollWaitStatus wait_for(short events,
                            std::chrono::steady_clock::time_point deadline,
                            bool allow_immediate_probe,
                            std::string* error_out) noexcept;
    void close_with_error(std::string_view message) noexcept;
    SpatialRoiIpcTransportReadResult terminal_read_result() const noexcept;
    SpatialRoiIpcTransportReadResult make_read_result(
        SpatialRoiIpcTransportReadStatus status,
        std::string line,
        const std::string& error) const noexcept;

    int fd_ = -1;
    int cancellation_fd_ = -1;
    std::atomic<bool> shutdown_requested_{false};
    SpatialRoiUnixSocketTransportConfig config_;
    SpatialRoiUnixSocketPeerCredentials peer_credentials_;
    std::string receive_buffer_;
    bool terminal_ = false;
    SpatialRoiIpcTransportReadStatus terminal_status_ =
        SpatialRoiIpcTransportReadStatus::kError;
    std::string terminal_error_;
};

}  // namespace orange::spatial_roi::ipc
