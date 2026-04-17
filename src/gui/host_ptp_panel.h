#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace orange::gui {

struct HostPtpStackStatusSummary {
    bool parsed = false;
    bool ptp4l_running = false;
    bool phc2sys_running = false;
    bool socket_present = false;
    bool gm_present_known = false;
    bool gm_present = false;
    std::string gm_identity;
    std::string master_offset;
};

struct HostPtpStackUiState {
    std::thread worker;
    std::atomic<bool> running{false};
    std::mutex mutex;
    std::string script_path;
    std::string last_command;
    std::string status_message = "Host PTP stack status not queried yet.";
    std::string error_message;
    std::string output_text;
    int last_exit_code = 0;
    HostPtpStackStatusSummary parsed_status;
};

void reap_host_ptp_stack_worker(HostPtpStackUiState* ui_state);

void render_host_ptp_stack_panel(HostPtpStackUiState* ui_state,
                                 bool streaming_active,
                                 bool ptp_stream_sync_enabled);

}  // namespace orange::gui
