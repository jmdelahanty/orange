#include "gui/frame_ipc_panel.h"

#include "imgui.h"

namespace orange::gui {

void render_frame_ipc_status_panel(
    const bool streaming_active,
    CameraEachSelect* cameras_select,
    CameraParams* cameras_params,
    const int num_cameras,
    const std::vector<FrameIPCManager*>& frame_ipc_managers,
    const std::vector<std::string>& frame_ipc_init_errors)
{
    if (!streaming_active || !cameras_select || !cameras_params || num_cameras <= 0) {
        return;
    }

    bool any_frame_ipc_active = false;
    for (int i = 0; i < num_cameras; ++i) {
        if (cameras_select[i].send_frame_ipc) {
            any_frame_ipc_active = true;
            break;
        }
    }

    if (!any_frame_ipc_active) {
        return;
    }

    ImGui::Separator();
    ImGui::Text("Frame IPC Status:");
    for (int i = 0; i < num_cameras; ++i) {
        if (!cameras_select[i].send_frame_ipc) {
            continue;
        }

        FrameIPCManager* ipc_manager =
            (i < static_cast<int>(frame_ipc_managers.size()))
                ? frame_ipc_managers[i]
                : nullptr;
        const std::string expected_queue_name =
            "/shm_cam_" + cameras_params[i].camera_serial;
        if (!ipc_manager) {
            const char* init_error =
                (i < static_cast<int>(frame_ipc_init_errors.size()) &&
                 !frame_ipc_init_errors[i].empty())
                    ? frame_ipc_init_errors[i].c_str()
                    : "init did not complete";
            ImGui::Text(
                "  %s: %s [manager unavailable]",
                cameras_params[i].camera_serial.c_str(),
                expected_queue_name.c_str());
            ImGui::TextDisabled("    init_error=%s", init_error);
            continue;
        }

        ImGui::Text(
            "  %s: %s",
            cameras_params[i].camera_serial.c_str(),
            ipc_manager->getQueueName().c_str());
        ImGui::TextDisabled(
            "    base=%llu updates=%llu push_fail=%llu base_drop=%llu update_drop=%llu stale_live_suppress=%llu",
            static_cast<unsigned long long>(ipc_manager->getFramesSent()),
            static_cast<unsigned long long>(ipc_manager->getUpdatesSent()),
            static_cast<unsigned long long>(ipc_manager->getIpcPushFailures()),
            static_cast<unsigned long long>(ipc_manager->getBaseQueueDrops()),
            static_cast<unsigned long long>(ipc_manager->getUpdateQueueDrops()),
            static_cast<unsigned long long>(ipc_manager->getUpdateStaleDrops()));
    }
    ImGui::TextDisabled(
        "  SHM slot timestamps are Orange publish-time us, not camera_timestamp_ns");
    ImGui::TextDisabled("  Run './dummy_reader' to monitor");
}

}  // namespace orange::gui
