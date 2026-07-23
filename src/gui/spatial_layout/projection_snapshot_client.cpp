#include "gui/spatial_layout/projection_snapshot_client.h"

#include "calibration_image_set.h"
#include "project.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

namespace orange::gui::spatial_layout {
namespace {

std::string sanitize_control_component(std::string value)
{
    for (char& ch : value) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (!std::isalnum(byte) && ch != '-' && ch != '_' && ch != '.') {
            ch = '_';
        }
    }
    return value.empty() ? "unknown" : value;
}

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
    return "/tmp/citrus_local_control.sock";
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

std::string unique_citrus_request_id(
    const std::string& method,
    const std::string& operation_hint)
{
    static std::atomic<uint64_t> sequence{1};
    const uint64_t id = sequence.fetch_add(1, std::memory_order_relaxed);
    return "orange-" + sanitize_control_component(method) + "-" +
           sanitize_control_component(
               operation_hint.empty() ? get_current_utc_timestamp() : operation_hint) +
           "-" + std::to_string(id);
}

bool send_citrus_local_control_request(
    const nlohmann::json& request,
    bool* attempted_out,
    nlohmann::json* response_out,
    std::string* error_out)
{
    if (attempted_out != nullptr) {
        *attempted_out = false;
    }
    if (response_out != nullptr) {
        *response_out = nlohmann::json::object();
    }

    const std::string socket_path = citrus_projection_snapshot_socket_path();
    if (socket_path.empty()) {
        if (error_out) {
            *error_out =
                "Citrus local-control socket is not configured; set "
                "ORANGE_CITRUS_PROJECTION_SNAPSHOT_SOCKET or "
                "CITRUS_GUI_LOCAL_CONTROL_SOCKET.";
        }
        return false;
    }
    if (attempted_out != nullptr) {
        *attempted_out = true;
    }

    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        if (error_out) {
            *error_out = std::string("socket failed: ") + std::strerror(errno);
        }
        return false;
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
        if (error_out) {
            *error_out = "Citrus local-control socket path is too long: " + socket_path;
        }
        close(fd);
        return false;
    }
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (error_out) {
            *error_out =
                "connect failed for Citrus local-control socket " + socket_path +
                ": " + std::strerror(errno);
        }
        close(fd);
        return false;
    }

    std::string socket_error;
    if (!unix_socket_send_all(fd, request.dump() + "\n", &socket_error)) {
        if (error_out) {
            *error_out = socket_error;
        }
        close(fd);
        return false;
    }
    shutdown(fd, SHUT_WR);

    std::string response_text;
    if (!unix_socket_recv_response(fd, &response_text, &socket_error)) {
        if (error_out) {
            *error_out = socket_error;
        }
        close(fd);
        return false;
    }
    close(fd);

    nlohmann::json response = nlohmann::json::parse(response_text, nullptr, false);
    if (response.is_discarded() || !response.is_object()) {
        if (error_out) {
            *error_out = "Citrus local-control response was not valid JSON.";
        }
        return false;
    }
    if (response_out != nullptr) {
        *response_out = std::move(response);
    }
    return true;
}

const nlohmann::json* calibration_scene_from_response(const nlohmann::json& response)
{
    if (response.contains("effect") && response["effect"].is_object() &&
        response["effect"].contains("calibration_scene") &&
        response["effect"]["calibration_scene"].is_object()) {
        return &response["effect"]["calibration_scene"];
    }
    if (response.contains("status") && response["status"].is_object() &&
        response["status"].contains("calibration_scene") &&
        response["status"]["calibration_scene"].is_object()) {
        return &response["status"]["calibration_scene"];
    }
    return nullptr;
}

const nlohmann::json* arena_centering_from_response(const nlohmann::json& response)
{
    if (response.contains("effect") && response["effect"].is_object() &&
        response["effect"].contains("arena_centering") &&
        response["effect"]["arena_centering"].is_object()) {
        return &response["effect"]["arena_centering"];
    }
    if (response.contains("status") && response["status"].is_object() &&
        response["status"].contains("arena_centering") &&
        response["status"]["arena_centering"].is_object()) {
        return &response["status"]["arena_centering"];
    }
    return nullptr;
}

const nlohmann::json* homography_candidate_from_response(
    const nlohmann::json& response)
{
    if (response.contains("effect") && response["effect"].is_object() &&
        response["effect"].contains("homography_candidate") &&
        response["effect"]["homography_candidate"].is_object()) {
        return &response["effect"]["homography_candidate"];
    }
    if (response.contains("status") && response["status"].is_object() &&
        response["status"].contains("homography_candidate") &&
        response["status"]["homography_candidate"].is_object()) {
        return &response["status"]["homography_candidate"];
    }
    return nullptr;
}

const nlohmann::json* projected_surface_scale_candidate_from_response(
    const nlohmann::json& response)
{
    if (response.contains("effect") && response["effect"].is_object() &&
        response["effect"].contains("projected_surface_scale_candidate") &&
        response["effect"]["projected_surface_scale_candidate"].is_object()) {
        return &response["effect"]["projected_surface_scale_candidate"];
    }
    if (response.contains("status") && response["status"].is_object() &&
        response["status"].contains("projected_surface_scale_candidate") &&
        response["status"]["projected_surface_scale_candidate"].is_object()) {
        return &response["status"]["projected_surface_scale_candidate"];
    }
    return nullptr;
}

const nlohmann::json* rig_canvas_commissioning_from_response(
    const nlohmann::json& response)
{
    if (response.contains("effect") && response["effect"].is_object() &&
        response["effect"].contains("rig_canvas_commissioning") &&
        response["effect"]["rig_canvas_commissioning"].is_object()) {
        return &response["effect"]["rig_canvas_commissioning"];
    }
    if (response.contains("status") && response["status"].is_object() &&
        response["status"].contains(
            "runtime_rig_canvas_commissioning_compatibility") &&
        response["status"]["runtime_rig_canvas_commissioning_compatibility"]
            .is_object()) {
        return &response["status"]
            ["runtime_rig_canvas_commissioning_compatibility"];
    }
    return nullptr;
}

const nlohmann::json* daily_registration_from_response(
    const nlohmann::json& response)
{
    if (response.contains("effect") && response["effect"].is_object() &&
        response["effect"].contains("daily_registration") &&
        response["effect"]["daily_registration"].is_object()) {
        return &response["effect"]["daily_registration"];
    }
    if (response.contains("status") && response["status"].is_object() &&
        response["status"].contains("daily_registration") &&
        response["status"]["daily_registration"].is_object()) {
        return &response["status"]["daily_registration"];
    }
    return nullptr;
}

CitrusCalibrationSceneControlResult send_calibration_scene_request(
    const std::string& method,
    const std::string& operation_id,
    const nlohmann::json& params,
    const std::string& request_hint = std::string())
{
    CitrusCalibrationSceneControlResult result;
    nlohmann::json request = {
        {"schema_id", "citrus.local_control.request"},
        {"schema_version", 1},
        {"method", method},
        {"request_id", unique_citrus_request_id(
             method,
             operation_id.empty() ? request_hint : operation_id)},
        {"source", "orange"}
    };
    if (!operation_id.empty()) {
        request["operation_id"] = operation_id;
    }
    if (params.is_object() && !params.empty()) {
        request["params"] = params;
    }

    if (!send_citrus_local_control_request(
            request,
            &result.attempted,
            &result.response,
            &result.reason)) {
        return result;
    }
    result.accepted = result.response.value("accepted", false);
    if (!result.response.value("ok", false)) {
        result.reason = result.response.value("error", method + " returned ok=false.");
        return result;
    }
    if (!result.accepted) {
        result.reason = result.response.value("error", method + " was not accepted.");
        return result;
    }
    if (const nlohmann::json* scene = calibration_scene_from_response(result.response)) {
        result.scene = *scene;
    }
    result.ok = true;
    return result;
}

CitrusArenaCenteringControlResult send_arena_centering_request(
    const std::string& method,
    const std::string& operation_id,
    const nlohmann::json& params,
    const std::string& request_hint = std::string())
{
    CitrusArenaCenteringControlResult result;
    nlohmann::json request = {
        {"schema_id", "citrus.local_control.request"},
        {"schema_version", 1},
        {"method", method},
        {"request_id", unique_citrus_request_id(
             method,
             operation_id.empty() ? request_hint : operation_id)},
        {"source", "orange"}
    };
    if (!operation_id.empty()) {
        request["operation_id"] = operation_id;
    }
    if (params.is_object() && !params.empty()) {
        request["params"] = params;
    }
    if (!send_citrus_local_control_request(
            request,
            &result.attempted,
            &result.response,
            &result.reason)) {
        return result;
    }
    result.accepted = result.response.value("accepted", false);
    if (!result.response.value("ok", false)) {
        result.reason = result.response.value("error", method + " returned ok=false.");
        return result;
    }
    if (!result.accepted) {
        result.reason = result.response.value("error", method + " was not accepted.");
        return result;
    }
    if (const nlohmann::json* centering =
            arena_centering_from_response(result.response)) {
        result.centering = *centering;
    }
    result.ok = true;
    return result;
}

CitrusHomographyCandidateControlResult send_homography_candidate_request(
    const std::string& method,
    const std::string& operation_id,
    const nlohmann::json& params,
    const std::string& request_hint = std::string())
{
    CitrusHomographyCandidateControlResult result;
    nlohmann::json request = {
        {"schema_id", "citrus.local_control.request"},
        {"schema_version", 1},
        {"method", method},
        {"request_id", unique_citrus_request_id(
             method,
             operation_id.empty() ? request_hint : operation_id)},
        {"source", "orange"}
    };
    if (!operation_id.empty()) {
        request["operation_id"] = operation_id;
    }
    if (params.is_object() && !params.empty()) {
        request["params"] = params;
    }
    if (!send_citrus_local_control_request(
            request,
            &result.attempted,
            &result.response,
            &result.reason)) {
        return result;
    }
    result.accepted = result.response.value("accepted", false);
    if (!result.response.value("ok", false)) {
        result.reason = result.response.value("error", method + " returned ok=false.");
        return result;
    }
    if (!result.accepted) {
        result.reason = result.response.value("error", method + " was not accepted.");
        return result;
    }
    if (const nlohmann::json* candidate =
            homography_candidate_from_response(result.response)) {
        result.candidate = *candidate;
    }
    result.ok = true;
    return result;
}

CitrusProjectedSurfaceScaleControlResult send_projected_surface_scale_request(
    const std::string& method,
    const std::string& operation_id,
    const nlohmann::json& params,
    const std::string& request_hint = std::string())
{
    CitrusProjectedSurfaceScaleControlResult result;
    nlohmann::json request = {
        {"schema_id", "citrus.local_control.request"},
        {"schema_version", 1},
        {"method", method},
        {"request_id", unique_citrus_request_id(
             method, operation_id.empty() ? request_hint : operation_id)},
        {"source", "orange"}
    };
    if (!operation_id.empty()) {
        request["operation_id"] = operation_id;
    }
    if (params.is_object() && !params.empty()) {
        request["params"] = params;
    }
    if (!send_citrus_local_control_request(
            request,
            &result.attempted,
            &result.response,
            &result.reason)) {
        return result;
    }
    result.accepted = result.response.value("accepted", false);
    if (!result.response.value("ok", false)) {
        result.reason = result.response.value("error", method + " returned ok=false.");
        return result;
    }
    if (!result.accepted) {
        result.reason = result.response.value("error", method + " was not accepted.");
        return result;
    }
    if (const nlohmann::json* candidate =
            projected_surface_scale_candidate_from_response(result.response)) {
        result.candidate = *candidate;
    }
    result.ok = true;
    return result;
}

CitrusRigCanvasCommissioningControlResult send_rig_canvas_commissioning_request(
    const std::string& method,
    const std::string& operation_id,
    const nlohmann::json& params,
    const std::string& request_hint = std::string())
{
    CitrusRigCanvasCommissioningControlResult result;
    nlohmann::json request = {
        {"schema_id", "citrus.local_control.request"},
        {"schema_version", 1},
        {"method", method},
        {"request_id", unique_citrus_request_id(
             method, operation_id.empty() ? request_hint : operation_id)},
        {"source", "orange"}
    };
    if (!operation_id.empty()) request["operation_id"] = operation_id;
    if (params.is_object() && !params.empty()) request["params"] = params;
    if (!send_citrus_local_control_request(
            request, &result.attempted, &result.response, &result.reason)) {
        return result;
    }
    result.accepted = result.response.value("accepted", false);
    if (!result.response.value("ok", false) || !result.accepted) {
        result.reason = result.response.value(
            "error", method + " was not accepted.");
        return result;
    }
    if (const nlohmann::json* commissioning =
            rig_canvas_commissioning_from_response(result.response)) {
        result.commissioning = *commissioning;
    }
    result.ok = true;
    return result;
}

CitrusDailyRegistrationControlResult send_daily_registration_request(
    const std::string& method,
    const std::string& operation_id,
    const nlohmann::json& params,
    const std::string& request_hint = std::string())
{
    CitrusDailyRegistrationControlResult result;
    nlohmann::json request = {
        {"schema_id", "citrus.local_control.request"},
        {"schema_version", 1},
        {"method", method},
        {"request_id", unique_citrus_request_id(
             method, operation_id.empty() ? request_hint : operation_id)},
        {"source", "orange"}
    };
    if (!operation_id.empty()) request["operation_id"] = operation_id;
    if (params.is_object() && !params.empty()) request["params"] = params;
    if (!send_citrus_local_control_request(
            request, &result.attempted, &result.response, &result.reason)) {
        return result;
    }
    result.accepted = result.response.value("accepted", false);
    if (!result.response.value("ok", false) || !result.accepted) {
        result.reason = result.response.value(
            "error", method + " was not accepted.");
        return result;
    }
    if (const nlohmann::json* daily =
            daily_registration_from_response(result.response)) {
        result.daily_registration = *daily;
    }
    result.ok = true;
    return result;
}

} // namespace

CitrusProjectionSnapshotQueryResult query_citrus_active_projection_snapshot(
    const std::string& phase,
    const std::string& operation_id)
{
    CitrusProjectionSnapshotQueryResult result;
    const std::string safe_operation_id =
        operation_id.empty() ? get_current_utc_timestamp() : operation_id;
    const nlohmann::json request = {
        {"schema_id", "citrus.local_control.request"},
        {"schema_version", 1},
        {"method", "active_projection_snapshot"},
        {"request_id", unique_citrus_request_id(
             "active_projection_snapshot",
             phase + "-" + safe_operation_id)},
        {"source", "orange"}
    };

    nlohmann::json response;
    if (!send_citrus_local_control_request(
            request,
            &result.attempted,
            &response,
            &result.reason)) {
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

CitrusCalibrationSceneControlResult set_citrus_calibration_scene(
    const std::string& transaction_id,
    const std::string& recipe_id,
    const std::vector<std::string>& arena_ids,
    const std::string& operation_id,
    const nlohmann::json& scene_options)
{
    nlohmann::json params = {
        {"transaction_id", transaction_id},
        {"recipe_id", recipe_id},
        {"arena_ids", arena_ids}
    };
    if (scene_options.is_object() && !scene_options.empty()) {
        params["scene_options"] = scene_options;
    }
    return send_calibration_scene_request(
        "set_calibration_scene",
        operation_id,
        params);
}

CitrusCalibrationSceneControlResult query_citrus_calibration_scene_status(
    const std::string& transaction_id,
    const std::string& phase)
{
    return send_calibration_scene_request(
        "calibration_scene_status",
        "",
        nlohmann::json::object(),
        transaction_id + "-" + phase);
}

CitrusCalibrationSceneControlResult restore_citrus_calibration_scene(
    const std::string& transaction_id,
    const std::string& operation_id)
{
    return send_calibration_scene_request(
        "restore_calibration_scene",
        operation_id,
        {{"transaction_id", transaction_id}});
}

CitrusArenaCenteringControlResult begin_citrus_arena_centering(
    const std::string& transaction_id,
    const std::string& canvas_path,
    const nlohmann::json& targets,
    const std::string& operation_id,
    std::uint8_t foreground_gray_u8)
{
    return send_arena_centering_request(
        "begin_arena_centering",
        operation_id,
        {
            {"transaction_id", transaction_id},
            {"canvas_path", canvas_path},
            {"targets", targets},
            {"scene_options", {{"foreground_gray_u8", foreground_gray_u8}}},
        });
}

CitrusArenaCenteringControlResult stage_citrus_arena_centers(
    const std::string& transaction_id,
    const std::string& stage_id,
    const nlohmann::json& centers,
    const std::string& operation_id)
{
    return send_arena_centering_request(
        "stage_arena_centers",
        operation_id,
        {
            {"transaction_id", transaction_id},
            {"stage_id", stage_id},
            {"centers", centers},
        });
}

CitrusArenaCenteringControlResult query_citrus_arena_centering_status(
    const std::string& transaction_id,
    const std::string& phase)
{
    return send_arena_centering_request(
        "arena_centering_status",
        "",
        nlohmann::json::object(),
        transaction_id + "-" + phase);
}

CitrusArenaCenteringControlResult commit_citrus_arena_centering(
    const std::string& transaction_id,
    const std::string& expected_base_checksum,
    const nlohmann::json& verification,
    bool save_verified_centers_armed,
    bool save_verified_layout_armed,
    const std::string& operation_id)
{
    return send_arena_centering_request(
        "commit_arena_centering",
        operation_id,
        {
            {"transaction_id", transaction_id},
            {"expected_base_checksum", expected_base_checksum},
            {"verification", verification},
            {"save_verified_centers_armed", save_verified_centers_armed},
            {"save_verified_layout_armed", save_verified_layout_armed},
        });
}

CitrusArenaCenteringControlResult abort_citrus_arena_centering(
    const std::string& transaction_id,
    const std::string& reason,
    const std::string& operation_id)
{
    return send_arena_centering_request(
        "abort_arena_centering",
        operation_id,
        {{"transaction_id", transaction_id}, {"reason", reason}});
}

CitrusHomographyCandidateControlResult query_citrus_homography_candidate_status(
    const std::string& transaction_id,
    const std::string& phase)
{
    return send_homography_candidate_request(
        "homography_candidate_status",
        "",
        nlohmann::json::object(),
        transaction_id + "-" + phase);
}

CitrusHomographyCandidateControlResult fit_citrus_homography_candidates(
    const std::string& transaction_id,
    const std::string& canvas_path,
    const std::string& expected_canvas_checksum,
    const std::string& orange_session_dir,
    const std::string& capture_group_id,
    const nlohmann::json& targets,
    const nlohmann::json& quality_thresholds,
    const std::string& operation_id)
{
    return send_homography_candidate_request(
        "fit_homography_candidates",
        operation_id,
        {
            {"transaction_id", transaction_id},
            {"canvas_path", canvas_path},
            {"expected_canvas_checksum", expected_canvas_checksum},
            {"orange_session_dir", orange_session_dir},
            {"capture_group_id", capture_group_id},
            {"target_plane", "projected_surface"},
            {"targets", targets},
            {"quality_thresholds", quality_thresholds},
        });
}

CitrusHomographyCandidateControlResult
load_citrus_homography_candidate_set_for_review(
    const std::string& candidate_set_dir,
    const std::string& expected_candidate_set_id,
    const std::string& expected_canvas_checksum,
    const nlohmann::json& targets,
    const std::string& operation_id)
{
    return send_homography_candidate_request(
        "load_homography_candidate_set_for_review",
        operation_id,
        {
            {"candidate_set_dir", candidate_set_dir},
            {"expected_candidate_set_id", expected_candidate_set_id},
            {"expected_canvas_checksum", expected_canvas_checksum},
            {"targets", targets},
        });
}

CitrusHomographyCandidateControlResult promote_citrus_homography_candidates(
    const std::string& transaction_id,
    const std::string& expected_canvas_checksum,
    const nlohmann::json& verification,
    bool accept_homographies_armed,
    const std::string& operation_id)
{
    return send_homography_candidate_request(
        "promote_homography_candidates",
        operation_id,
        {
            {"transaction_id", transaction_id},
            {"expected_canvas_checksum", expected_canvas_checksum},
            {"verification", verification},
            {"accept_homographies_armed", accept_homographies_armed},
        });
}

CitrusHomographyCandidateControlResult reject_citrus_homography_candidates(
    const std::string& transaction_id,
    const std::string& reason,
    const std::string& operation_id)
{
    return send_homography_candidate_request(
        "reject_homography_candidates",
        operation_id,
        {{"transaction_id", transaction_id}, {"reason", reason}});
}

CitrusProjectedSurfaceScaleControlResult
query_citrus_projected_surface_scale_candidate_status(
    const std::string& transaction_id,
    const std::string& phase)
{
    return send_projected_surface_scale_request(
        "projected_surface_scale_candidate_status",
        "",
        nlohmann::json::object(),
        transaction_id + "-" + phase);
}

CitrusProjectedSurfaceScaleControlResult
fit_citrus_projected_surface_scale_candidates(
    const std::string& transaction_id,
    const std::string& canvas_path,
    const std::string& expected_canvas_checksum,
    const nlohmann::json& observations,
    const std::string& operation_id)
{
    return send_projected_surface_scale_request(
        "fit_projected_surface_scale_candidates",
        operation_id,
        {
            {"transaction_id", transaction_id},
            {"canvas_path", canvas_path},
            {"expected_canvas_checksum", expected_canvas_checksum},
            {"observations", observations},
        });
}

CitrusProjectedSurfaceScaleControlResult
load_citrus_projected_surface_scale_candidate_set_for_review(
    const std::string& candidate_set_dir,
    const std::string& expected_candidate_set_id,
    const std::string& expected_canvas_checksum,
    const nlohmann::json& targets,
    const std::string& operation_id)
{
    return send_projected_surface_scale_request(
        "load_projected_surface_scale_candidate_set_for_review",
        operation_id,
        {
            {"candidate_set_dir", candidate_set_dir},
            {"expected_candidate_set_id", expected_candidate_set_id},
            {"expected_canvas_checksum", expected_canvas_checksum},
            {"targets", targets},
        });
}

CitrusProjectedSurfaceScaleControlResult
promote_citrus_projected_surface_scale_candidates(
    const std::string& transaction_id,
    const std::string& expected_canvas_checksum,
    const nlohmann::json& verification,
    bool accept_scales_armed,
    const std::string& operation_id)
{
    return send_projected_surface_scale_request(
        "promote_projected_surface_scale_candidates",
        operation_id,
        {
            {"transaction_id", transaction_id},
            {"expected_canvas_checksum", expected_canvas_checksum},
            {"verification", verification},
            {"accept_scales_armed", accept_scales_armed},
        });
}

CitrusProjectedSurfaceScaleControlResult
reject_citrus_projected_surface_scale_candidates(
    const std::string& transaction_id,
    const std::string& reason,
    const std::string& operation_id)
{
    return send_projected_surface_scale_request(
        "reject_projected_surface_scale_candidates",
        operation_id,
        {{"transaction_id", transaction_id}, {"reason", reason}});
}

CitrusRigCanvasCommissioningControlResult
query_citrus_rig_canvas_commissioning_status(const std::string& phase)
{
    return send_rig_canvas_commissioning_request(
        "rig_canvas_commissioning_status", "", nlohmann::json::object(), phase);
}

CitrusRigCanvasCommissioningControlResult
finalize_citrus_rig_canvas_commissioning(
    const std::string& transaction_id,
    const std::string& canvas_path,
    const std::string& expected_canvas_checksum,
    const nlohmann::json& orange_session_dirs,
    bool accept_commissioning_armed,
    const std::string& operation_id)
{
    return send_rig_canvas_commissioning_request(
        "finalize_rig_canvas_commissioning",
        operation_id,
        {
            {"transaction_id", transaction_id},
            {"canvas_path", canvas_path},
            {"expected_canvas_checksum", expected_canvas_checksum},
            {"orange_session_dirs", orange_session_dirs},
            {"accept_commissioning_armed", accept_commissioning_armed},
        });
}

CitrusDailyRegistrationControlResult query_citrus_daily_registration_status(
    const std::string& phase)
{
    return send_daily_registration_request(
        "daily_registration_status", "", nlohmann::json::object(), phase);
}

CitrusDailyRegistrationControlResult begin_citrus_daily_registration(
    const std::string& transaction_id,
    const nlohmann::json& targets,
    const std::string& operation_id)
{
    return send_daily_registration_request(
        "begin_daily_registration", operation_id,
        {{"transaction_id", transaction_id}, {"targets", targets}});
}

CitrusDailyRegistrationControlResult create_citrus_daily_registration_candidate(
    const std::string& transaction_id,
    const nlohmann::json& observations,
    const std::string& operation_id)
{
    return send_daily_registration_request(
        "create_daily_registration_candidate", operation_id,
        {{"transaction_id", transaction_id}, {"observations", observations}});
}

CitrusDailyRegistrationControlResult preview_citrus_daily_registration_candidate(
    const std::string& transaction_id,
    const std::string& operation_id)
{
    return send_daily_registration_request(
        "preview_daily_registration_candidate", operation_id,
        {{"transaction_id", transaction_id}});
}

CitrusDailyRegistrationControlResult restore_citrus_daily_registration_preview(
    const std::string& transaction_id,
    const std::string& operation_id)
{
    return send_daily_registration_request(
        "restore_daily_registration_preview", operation_id,
        {{"transaction_id", transaction_id}});
}

CitrusDailyRegistrationControlResult abort_citrus_daily_registration(
    const std::string& transaction_id,
    const std::string& reason,
    const std::string& operation_id)
{
    return send_daily_registration_request(
        "abort_daily_registration", operation_id,
        {{"transaction_id", transaction_id}, {"reason", reason}});
}

CitrusDailyRegistrationControlResult accept_citrus_daily_registration(
    const std::string& transaction_id,
    const std::string& expected_candidate_sha256,
    const std::string& valid_until_utc,
    const nlohmann::json& verification,
    bool accept_registration_armed,
    const std::string& operation_id)
{
    return send_daily_registration_request(
        "accept_daily_registration", operation_id,
        {
            {"transaction_id", transaction_id},
            {"expected_candidate_sha256", expected_candidate_sha256},
            {"valid_until_utc", valid_until_utc},
            {"verification", verification},
            {"accept_registration_armed", accept_registration_armed},
        });
}

CitrusDailyRegistrationControlResult reject_citrus_daily_registration(
    const std::string& transaction_id,
    const std::string& reason,
    const std::string& operation_id)
{
    return send_daily_registration_request(
        "reject_daily_registration", operation_id,
        {{"transaction_id", transaction_id}, {"reason", reason}});
}

CitrusDailyRegistrationControlResult select_citrus_daily_registration_runtime_mode(
    const std::string& mode,
    const std::string& registration_path,
    const std::string& expected_registration_sha256,
    bool select_runtime_mode_armed,
    const std::string& operation_id)
{
    nlohmann::json params = {
        {"mode", mode},
        {"select_runtime_mode_armed", select_runtime_mode_armed},
    };
    if (!registration_path.empty()) {
        params["registration_path"] = registration_path;
        params["expected_registration_sha256"] = expected_registration_sha256;
    }
    return send_daily_registration_request(
        "select_daily_registration_runtime_mode", operation_id, params);
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
    ui_state->captured_citrus_calibration_scene_pre_capture =
        nlohmann::json::object();
    ui_state->captured_citrus_calibration_scene_post_capture =
        nlohmann::json::object();
    ui_state->captured_citrus_calibration_scene_consistency =
        nlohmann::json::object();
    ui_state->captured_citrus_calibration_scene_restore_status =
        nlohmann::json::object();
    ui_state->captured_citrus_arena_centering_pre_capture =
        nlohmann::json::object();
    ui_state->captured_citrus_arena_centering_post_capture =
        nlohmann::json::object();
    ui_state->captured_citrus_arena_centering_consistency =
        nlohmann::json::object();
    ui_state->captured_citrus_daily_registration_pre_capture =
        nlohmann::json::object();
    ui_state->captured_citrus_daily_registration_post_capture =
        nlohmann::json::object();
    ui_state->captured_citrus_daily_registration_consistency =
        nlohmann::json::object();
    ui_state->captured_group_membership = nlohmann::json::object();
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
