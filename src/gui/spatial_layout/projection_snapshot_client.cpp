#include "gui/spatial_layout/projection_snapshot_client.h"

#include "calibration_image_set.h"
#include "gui/spatial_layout/session_io.h"
#include "project.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

namespace orange::gui::spatial_layout {
namespace {

std::string citrus_projection_snapshot_socket_path()
{
    const char* explicit_path = std::getenv("ORANGE_CITRUS_PROJECTION_SNAPSHOT_SOCKET");
    if (explicit_path != nullptr && explicit_path[0] != '\0') {
        return explicit_path;
    }
    const char* citrus_control = std::getenv("CITRUS_GUI_LOCAL_CONTROL_SOCKET");
    if (citrus_control != nullptr && citrus_control[0] != '\0') {
        return citrus_control;
    }
    return "";
}

int citrus_projection_snapshot_timeout_ms()
{
    const char* value = std::getenv("ORANGE_CITRUS_PROJECTION_SNAPSHOT_TIMEOUT_MS");
    if (value == nullptr || value[0] == '\0') {
        return 500;
    }
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || parsed <= 0) {
        return 500;
    }
    return static_cast<int>(std::clamp<long>(parsed, 50, 5000));
}

bool unix_socket_send_all(int fd, const std::string& payload, std::string* error_out)
{
    size_t offset = 0;
    while (offset < payload.size()) {
        const ssize_t sent =
            send(fd, payload.data() + offset, payload.size() - offset, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (error_out) {
                *error_out = std::string("send failed: ") + std::strerror(errno);
            }
            return false;
        }
        if (sent == 0) {
            if (error_out) {
                *error_out = "send returned zero bytes";
            }
            return false;
        }
        offset += static_cast<size_t>(sent);
    }
    return true;
}

bool unix_socket_recv_response(int fd, std::string* response_out, std::string* error_out)
{
    if (response_out == nullptr) {
        if (error_out) {
            *error_out = "response destination is null";
        }
        return false;
    }
    response_out->clear();
    std::array<char, 4096> buffer{};
    while (response_out->size() < 4u * 1024u * 1024u) {
        const ssize_t received = recv(fd, buffer.data(), buffer.size(), 0);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (error_out) {
                *error_out = std::string("recv failed: ") + std::strerror(errno);
            }
            return false;
        }
        if (received == 0) {
            break;
        }
        response_out->append(buffer.data(), static_cast<size_t>(received));
        if (response_out->find('\n') != std::string::npos) {
            break;
        }
    }
    if (response_out->empty()) {
        if (error_out) {
            *error_out = "no response from Citrus local-control socket";
        }
        return false;
    }
    return true;
}

} // namespace

CitrusProjectionSnapshotQueryResult query_citrus_active_projection_snapshot(
    const std::string& phase,
    const std::string& operation_id)
{
    CitrusProjectionSnapshotQueryResult result;
    const std::string socket_path = citrus_projection_snapshot_socket_path();
    if (socket_path.empty()) {
        result.reason =
            "Citrus projection snapshot socket is not configured; set "
            "ORANGE_CITRUS_PROJECTION_SNAPSHOT_SOCKET or CITRUS_GUI_LOCAL_CONTROL_SOCKET.";
        return result;
    }
    result.attempted = true;

    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        result.reason = std::string("socket failed: ") + std::strerror(errno);
        return result;
    }

    const int timeout_ms = citrus_projection_snapshot_timeout_ms();
    timeval timeout{};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(addr.sun_path)) {
        result.reason = "Citrus local-control socket path is too long: " + socket_path;
        close(fd);
        return result;
    }
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        result.reason =
            "connect failed for Citrus local-control socket " + socket_path +
            ": " + std::strerror(errno);
        close(fd);
        return result;
    }

    const std::string safe_operation_id =
        operation_id.empty() ? get_current_utc_timestamp() : operation_id;
    const std::string request_id =
        "projection-snapshot-" + sanitize_artifact_component(phase) + "-" +
        sanitize_artifact_component(safe_operation_id);
    const nlohmann::json request = {
        {"schema_id", "citrus.local_control.request"},
        {"schema_version", 1},
        {"method", "active_projection_snapshot"},
        {"request_id", request_id},
        {"source", "orange"}
    };

    std::string socket_error;
    const std::string payload = request.dump() + "\n";
    if (!unix_socket_send_all(fd, payload, &socket_error)) {
        result.reason = socket_error;
        close(fd);
        return result;
    }
    shutdown(fd, SHUT_WR);

    std::string response_text;
    if (!unix_socket_recv_response(fd, &response_text, &socket_error)) {
        result.reason = socket_error;
        close(fd);
        return result;
    }
    close(fd);

    const nlohmann::json response =
        nlohmann::json::parse(response_text, nullptr, false);
    if (response.is_discarded() || !response.is_object()) {
        result.reason = "Citrus active_projection_snapshot response was not valid JSON.";
        return result;
    }
    if (!response.value("ok", false)) {
        result.reason =
            response.value("error", "Citrus active_projection_snapshot returned ok=false.");
        return result;
    }

    const nlohmann::json* snapshot = nullptr;
    if (response.contains("effect") &&
        response["effect"].is_object() &&
        response["effect"].contains("active_projection_snapshot") &&
        response["effect"]["active_projection_snapshot"].is_object()) {
        snapshot = &response["effect"]["active_projection_snapshot"];
    } else if (response.contains("result") &&
               response["result"].is_object() &&
               response["result"].contains("active_projection_snapshot") &&
               response["result"]["active_projection_snapshot"].is_object()) {
        snapshot = &response["result"]["active_projection_snapshot"];
    }

    if (snapshot == nullptr) {
        result.reason =
            "Citrus active_projection_snapshot response did not contain "
            "effect.active_projection_snapshot.";
        return result;
    }
    result.snapshot = *snapshot;
    result.ok = true;
    return result;
}

void clear_captured_citrus_projection_snapshot_metadata(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr) {
        return;
    }
    ui_state->captured_citrus_projection_snapshot_pre_capture =
        nlohmann::json::object();
    ui_state->captured_citrus_projection_snapshot_post_capture =
        nlohmann::json::object();
    ui_state->captured_citrus_projection_epoch_consistency =
        nlohmann::json::object();
}

void set_captured_citrus_projection_snapshots(
    SpatialLayoutUiState* ui_state,
    const nlohmann::json& pre_capture,
    const nlohmann::json& post_capture)
{
    if (ui_state == nullptr) {
        return;
    }
    ui_state->captured_citrus_projection_snapshot_pre_capture =
        pre_capture.is_object() ? pre_capture : nlohmann::json::object();
    ui_state->captured_citrus_projection_snapshot_post_capture =
        post_capture.is_object() ? post_capture : nlohmann::json::object();
    ui_state->captured_citrus_projection_epoch_consistency =
        nlohmann::json::object();
}

bool snapshot_projection_matches_context(
    const nlohmann::json& snapshot,
    const orange::calibration::CalibrationImageSetRequest& request)
{
    if (!snapshot.is_object() ||
        !snapshot.contains("projections") ||
        !snapshot["projections"].is_array()) {
        return true;
    }
    const std::string camera_serial = request.camera.serial;
    const std::string rig_id =
        request.rig_context.value("rig_id", std::string());
    const std::string canvas_id =
        request.rig_context.value("canvas_id", std::string());
    const std::string arena_id =
        request.rig_context.value("arena_id", std::string());
    if (camera_serial.empty() && rig_id.empty() && canvas_id.empty() && arena_id.empty()) {
        return true;
    }

    for (const nlohmann::json& projection : snapshot["projections"]) {
        if (!projection.is_object()) {
            continue;
        }
        if (!rig_id.empty() &&
            projection.value("rig_id", std::string()) != rig_id) {
            continue;
        }
        if (!canvas_id.empty() &&
            projection.value("canvas_name", std::string()) != canvas_id &&
            projection.value("canvas_id", std::string()) != canvas_id) {
            continue;
        }
        if (!arena_id.empty() &&
            projection.value("arena_id", std::string()) != arena_id) {
            continue;
        }
        if (!camera_serial.empty() &&
            projection.contains("associated_camera_ids") &&
            projection["associated_camera_ids"].is_array()) {
            bool camera_found = false;
            for (const nlohmann::json& id : projection["associated_camera_ids"]) {
                if (id.is_string() && id.get<std::string>() == camera_serial) {
                    camera_found = true;
                    break;
                }
            }
            if (!camera_found) {
                continue;
            }
        }
        return true;
    }
    return false;
}

nlohmann::json make_citrus_projection_epoch_consistency(
    const orange::calibration::CalibrationImageSetRequest& request)
{
    const nlohmann::json& pre = request.citrus_projection_snapshot_pre_capture;
    const nlohmann::json& post = request.citrus_projection_snapshot_post_capture;
    nlohmann::json out = {
        {"status", "unavailable"},
        {"blocking_or_warning_reason",
         "Citrus active projection snapshot was unavailable; capture remains warn-only."},
        {"policy", "warn_only_v0"}
    };
    if (!pre.is_object() || pre.empty() || !post.is_object() || post.empty()) {
        return out;
    }

    if (pre.value("status", std::string()) != "active" ||
        post.value("status", std::string()) != "active") {
        out["blocking_or_warning_reason"] =
            "Citrus active projection snapshot was not active before and after capture.";
        out["pre_status"] = pre.value("status", std::string("unknown"));
        out["post_status"] = post.value("status", std::string("unknown"));
        return out;
    }

    const std::string pre_epoch = pre.value("projection_epoch_id", std::string());
    const std::string post_epoch = post.value("projection_epoch_id", std::string());
    if (pre_epoch.empty() || post_epoch.empty()) {
        out["blocking_or_warning_reason"] =
            "Citrus active projection snapshot lacked projection_epoch_id.";
        return out;
    }
    if (pre_epoch != post_epoch) {
        out["status"] = "changed_epoch";
        out["blocking_or_warning_reason"] =
            "Citrus projection_epoch_id changed between pre-capture and post-capture.";
        out["pre_projection_epoch_id"] = pre_epoch;
        out["post_projection_epoch_id"] = post_epoch;
        return out;
    }
    if (!snapshot_projection_matches_context(pre, request) ||
        !snapshot_projection_matches_context(post, request)) {
        out["status"] = "metadata_mismatch";
        out["blocking_or_warning_reason"] =
            "Citrus projection snapshot did not contain a projection matching this Orange camera/arena context.";
        out["projection_epoch_id"] = pre_epoch;
        return out;
    }

    out["status"] = "same_epoch";
    out["blocking_or_warning_reason"] = "";
    out["projection_epoch_id"] = pre_epoch;
    return out;
}

} // namespace orange::gui::spatial_layout
