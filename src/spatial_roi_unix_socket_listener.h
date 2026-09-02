#pragma once

#include "spatial_roi_unix_socket_transport.h"

#include <chrono>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>

namespace orange::spatial_roi::ipc {

// Configuration for the one-shot Gate2 listener.  socket_path is expected to
// be the absolute path authenticated by the recorder contract.  The listener
// does not derive, normalize, repair, or otherwise reinterpret this path.
//
// The final parent directory named by socket_path must already exist, be
// opened without following symlinks, be owned by the effective UID, and have
// exactly mode 0700.  The listener retains that directory descriptor for the
// whole lifetime of the endpoint, so leaf inspection and cleanup remain
// relative to the directory that was authenticated at bind time.
struct SpatialRoiUnixSocketListenerConfig {
    std::string socket_path;
    SpatialRoiUnixSocketTransportConfig transport_config;
};

enum class SpatialRoiUnixSocketListenerAcceptStatus {
    kAccepted,
    kTimeout,
    kClosed,
    kError,
};

struct SpatialRoiUnixSocketListenerAcceptResult {
    SpatialRoiUnixSocketListenerAcceptStatus status =
        SpatialRoiUnixSocketListenerAcceptStatus::kError;
    std::unique_ptr<SpatialRoiUnixSocketLineTransport> transport;
    std::string error;

    bool accepted() const noexcept
    {
        return status == SpatialRoiUnixSocketListenerAcceptStatus::kAccepted &&
               transport != nullptr;
    }

    bool timed_out() const noexcept
    {
        return status == SpatialRoiUnixSocketListenerAcceptStatus::kTimeout;
    }
};

// A strict, single-connection AF_UNIX filesystem listener.  This class is
// intentionally single-owner and not thread-safe.  Its lifecycle is:
//
//   Bind -> zero or more bounded timeout probes -> one AcceptOne -> closed
//
// A successful accept consumes the listener permanently.  A timeout leaves it
// available for a later bounded probe; no second connection can be accepted
// after a successful first accept.  The path is never unlinked before bind,
// and cleanup only unlinks a path whose lstat device/inode still matches the
// socket created by this object.
class SpatialRoiUnixSocketListener final {
public:
    // Bind an exact path and return a listener.  On failure, error_out is
    // populated when supplied and no pre-existing path is removed.
    static std::unique_ptr<SpatialRoiUnixSocketListener> Create(
        const SpatialRoiUnixSocketListenerConfig& config,
        std::string* error_out = nullptr) noexcept;

    static std::unique_ptr<SpatialRoiUnixSocketListener> Create(
        std::string socket_path,
        SpatialRoiUnixSocketTransportConfig transport_config = {},
        std::string* error_out = nullptr) noexcept;

    // Production form.  The caller has already authenticated the private
    // runtime directory and supplies its retained descriptor plus identity.
    // The listener duplicates parent_fd and owns that duplicate.  socket_path
    // remains the exact contract spelling and socket_leaf is the exact leaf
    // to create beneath the supplied directory; no absolute-path walk is
    // performed by this overload.
    static std::unique_ptr<SpatialRoiUnixSocketListener> Create(
        int parent_fd,
        dev_t expected_parent_device,
        ino_t expected_parent_inode,
        std::string socket_path,
        std::string socket_leaf,
        SpatialRoiUnixSocketTransportConfig transport_config = {},
        std::string* error_out = nullptr) noexcept;

    // Bind is the descriptive spelling used by callers that want to make the
    // filesystem side effect obvious.  It is equivalent to Create.
    static std::unique_ptr<SpatialRoiUnixSocketListener> Bind(
        const SpatialRoiUnixSocketListenerConfig& config,
        std::string* error_out = nullptr) noexcept;

    static std::unique_ptr<SpatialRoiUnixSocketListener> Bind(
        std::string socket_path,
        SpatialRoiUnixSocketTransportConfig transport_config = {},
        std::string* error_out = nullptr) noexcept;

    ~SpatialRoiUnixSocketListener();

    SpatialRoiUnixSocketListener(const SpatialRoiUnixSocketListener&) = delete;
    SpatialRoiUnixSocketListener& operator=(
        const SpatialRoiUnixSocketListener&) = delete;
    SpatialRoiUnixSocketListener(SpatialRoiUnixSocketListener&&) = delete;
    SpatialRoiUnixSocketListener& operator=(
        SpatialRoiUnixSocketListener&&) = delete;

    // Accept at most one connected endpoint, waiting no longer than timeout.
    // timeout must be positive and no greater than five minutes.  A timeout
    // is non-terminal; all other failures close the listener fail-closed.
    std::unique_ptr<SpatialRoiUnixSocketLineTransport> AcceptOne(
        std::chrono::milliseconds timeout,
        std::string* error_out = nullptr) noexcept;

    // Absolute-deadline form for callers that already own a bounded
    // supervisor deadline.  The deadline must be in the future and no more
    // than five minutes from invocation.
    std::unique_ptr<SpatialRoiUnixSocketLineTransport> AcceptOne(
        std::chrono::steady_clock::time_point deadline,
        std::string* error_out = nullptr) noexcept;

    // The result form preserves timeout versus terminal-error distinction for
    // supervisors that need it.  AcceptOne above is the convenient transport
    // form and forwards the same operation.
    SpatialRoiUnixSocketListenerAcceptResult AcceptOneResult(
        std::chrono::milliseconds timeout) noexcept;
    SpatialRoiUnixSocketListenerAcceptResult AcceptOneResult(
        std::chrono::steady_clock::time_point deadline) noexcept;

    // Idempotently closes the listening descriptor and conditionally removes
    // this object's socket inode.  The destructor performs the same operation.
    // Cleanup is guaranteed to stay within the retained private parent
    // directory; callers must not mutate that directory concurrently.
    void Close() noexcept;

    bool closed() const noexcept { return listener_fd_ < 0; }
    bool accepted() const noexcept { return accepted_; }
    const std::string& socket_path() const noexcept { return socket_path_; }
    const SpatialRoiUnixSocketTransportConfig& transport_config() const noexcept
    {
        return transport_config_;
    }

    // Exposed for focused descriptor/lifecycle tests and diagnostics.  The
    // descriptor remains owned by the listener and must not be closed by the
    // caller.
    int native_handle() const noexcept { return listener_fd_; }

    // Exposed for focused descriptor/lifecycle tests and diagnostics.  The
    // descriptor remains owned by the listener and must not be closed by the
    // caller.
    int parent_directory_handle() const noexcept { return parent_fd_; }

private:
    struct SocketIdentity {
        dev_t device = 0;
        ino_t inode = 0;
        bool valid = false;
    };

    SpatialRoiUnixSocketListener(
        int listener_fd,
        int parent_fd,
        std::string socket_path,
        std::string socket_leaf,
        SpatialRoiUnixSocketTransportConfig transport_config,
        SocketIdentity identity) noexcept;

    static bool set_error(std::string* error_out,
                          const char* message) noexcept;
    static bool set_error(std::string* error_out,
                          const std::string& message) noexcept;
    static std::string errno_message(const char* operation,
                                     int error_number) noexcept;
    static bool validate_socket_path(const std::string& socket_path,
                                     std::string* error_out) noexcept;
    static bool open_private_parent_directory(
        const std::string& socket_path,
        int* parent_fd_out,
        std::string* socket_leaf_out,
        std::string* error_out) noexcept;
    static std::unique_ptr<SpatialRoiUnixSocketListener>
    create_from_parent_fd(
        int parent_fd,
        dev_t expected_parent_device,
        ino_t expected_parent_inode,
        std::string socket_path,
        std::string socket_leaf,
        SpatialRoiUnixSocketTransportConfig transport_config,
        std::string* error_out) noexcept;

    SpatialRoiUnixSocketListenerAcceptResult accept_one_until(
        std::chrono::steady_clock::time_point deadline) noexcept;
    bool verify_current_binding(std::string* error_out) const noexcept;
    bool capture_current_identity(SocketIdentity* identity,
                                  std::string* error_out) const noexcept;
    void close_listener() noexcept;
    void close_parent_directory() noexcept;
    void remove_own_socket_if_present() noexcept;
    void fail_closed() noexcept;

    int listener_fd_ = -1;
    int parent_fd_ = -1;
    std::string socket_path_;
    std::string socket_leaf_;
    SpatialRoiUnixSocketTransportConfig transport_config_;
    SocketIdentity socket_identity_;
    bool accepted_ = false;
    bool owns_path_ = false;
};

}  // namespace orange::spatial_roi::ipc
