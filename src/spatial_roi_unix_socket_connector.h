#pragma once

#include "spatial_roi_unix_socket_transport.h"

#include <chrono>
#include <memory>
#include <string>

namespace orange::spatial_roi::ipc {

struct SpatialRoiUnixSocketConnectorConfig {
    // Exact endpoint emitted by the authenticated spatial-ROI recorder
    // contract.  The connector never derives, normalizes, retries, or
    // substitutes this pathname.
    std::string socket_path;

    // A production connector requires both the already-spawned recorder PID
    // and UID.  Adoption rechecks them through SO_PEERCRED after connect.
    SpatialRoiUnixSocketTransportConfig transport_config;

    // One bounded connection attempt.  Readiness polling and process launch
    // policy belong to the camera supervisor; this primitive never reconnects.
    std::chrono::milliseconds connect_timeout{1000};
};

// Establish one nonblocking filesystem AF_UNIX connection and immediately
// adopt it into the strict line transport.  Every failure closes the socket.
// The function does not unlink, create, or chmod any filesystem entry.
class SpatialRoiUnixSocketConnector final {
public:
    static std::unique_ptr<SpatialRoiUnixSocketLineTransport> Connect(
        const SpatialRoiUnixSocketConnectorConfig& config,
        std::string* error_out = nullptr) noexcept;

    SpatialRoiUnixSocketConnector() = delete;
};

}  // namespace orange::spatial_roi::ipc
