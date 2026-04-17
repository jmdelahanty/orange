#include "gui/host_ptp_panel.h"

#include "imgui.h"

#include <array>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

struct HostPtpStackCommandResult {
    int exit_code = -1;
    std::string output;
    std::string error_message;
};

std::string trim_ascii_whitespace(const std::string& input)
{
    size_t start = 0;
    while (start < input.size() && std::isspace(static_cast<unsigned char>(input[start]))) {
        ++start;
    }
    size_t end = input.size();
    while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1]))) {
        --end;
    }
    return input.substr(start, end - start);
}

std::string shell_quote_single(const std::string& input)
{
    std::string quoted;
    quoted.reserve(input.size() + 2);
    quoted.push_back('\'');
    for (char c : input) {
        if (c == '\'') {
            quoted += "'\\''";
        } else {
            quoted.push_back(c);
        }
    }
    quoted.push_back('\'');
    return quoted;
}

std::filesystem::path resolve_ptp_stack_script_path()
{
    std::vector<std::filesystem::path> candidates;
    candidates.emplace_back(std::filesystem::current_path() / "scripts" / "ptp_stack.sh");

    std::array<char, 4096> exe_path_buffer{};
    const ssize_t exe_path_len = readlink("/proc/self/exe", exe_path_buffer.data(), exe_path_buffer.size() - 1);
    if (exe_path_len > 0) {
        const std::filesystem::path exe_path(std::string(exe_path_buffer.data(), static_cast<size_t>(exe_path_len)));
        candidates.emplace_back(exe_path.parent_path().parent_path().parent_path() / "scripts" / "ptp_stack.sh");
    }

    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec) && !ec) {
            return std::filesystem::absolute(candidate, ec);
        }
    }
    return {};
}

orange::gui::HostPtpStackStatusSummary parse_ptp_stack_status_output(const std::string& output)
{
    orange::gui::HostPtpStackStatusSummary summary;
    summary.parsed = true;

    std::istringstream stream(output);
    std::string line;
    bool in_process_state = false;
    bool in_time_status = false;
    while (std::getline(stream, line)) {
        const std::string trimmed = trim_ascii_whitespace(line);
        if (trimmed == "Process state:") {
            in_process_state = true;
            in_time_status = false;
            continue;
        }
        if (trimmed == "PTP TIME_STATUS_NP:") {
            in_process_state = false;
            in_time_status = true;
            continue;
        }
        if (trimmed.empty()) {
            if (in_process_state) {
                in_process_state = false;
            }
            continue;
        }

        if (in_process_state) {
            if (trimmed == "(no ptp4l/phc2sys process)") {
                summary.ptp4l_running = false;
                summary.phc2sys_running = false;
                continue;
            }
            if (trimmed.find("ptp4l") != std::string::npos) {
                summary.ptp4l_running = true;
            }
            if (trimmed.find("phc2sys") != std::string::npos) {
                summary.phc2sys_running = true;
            }
            continue;
        }

        if (in_time_status) {
            if (trimmed.find("(socket ") != std::string::npos && trimmed.find("not found") != std::string::npos) {
                summary.socket_present = false;
                continue;
            }
            if (trimmed.rfind("sending: GET TIME_STATUS_NP", 0) == 0) {
                summary.socket_present = true;
                continue;
            }
            if (trimmed.rfind("master_offset", 0) == 0) {
                summary.master_offset = trim_ascii_whitespace(trimmed.substr(std::string("master_offset").size()));
                continue;
            }
            if (trimmed.rfind("gmPresent", 0) == 0) {
                const std::string value =
                    trim_ascii_whitespace(trimmed.substr(std::string("gmPresent").size()));
                summary.gm_present_known = true;
                summary.gm_present = (value == "true");
                continue;
            }
            if (trimmed.rfind("gmIdentity", 0) == 0) {
                summary.gm_identity =
                    trim_ascii_whitespace(trimmed.substr(std::string("gmIdentity").size()));
                continue;
            }
        }
    }

    return summary;
}

HostPtpStackCommandResult run_ptp_stack_command(const std::string& script_path,
                                                const std::string& command)
{
    HostPtpStackCommandResult result;
    if (script_path.empty()) {
        result.error_message = "PTP stack script path is empty.";
        return result;
    }

    const std::string full_command =
        shell_quote_single(script_path) + " " + command + " 2>&1";
    FILE* pipe = popen(full_command.c_str(), "r");
    if (!pipe) {
        result.error_message = "Failed to run PTP stack command.";
        return result;
    }

    std::array<char, 512> buffer{};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        result.output += buffer.data();
    }

    const int status = pclose(pipe);
    if (status == -1) {
        result.error_message = "Failed to close PTP stack command pipe.";
        return result;
    }

    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else {
        result.exit_code = status;
    }
    return result;
}

void start_host_ptp_stack_command(orange::gui::HostPtpStackUiState* ui_state,
                                  const std::string& command,
                                  bool refresh_status_after)
{
    if (!ui_state) {
        return;
    }

    orange::gui::reap_host_ptp_stack_worker(ui_state);
    if (ui_state->running.load()) {
        return;
    }

    const std::filesystem::path script_path = resolve_ptp_stack_script_path();
    if (script_path.empty()) {
        std::lock_guard<std::mutex> lock(ui_state->mutex);
        ui_state->error_message = "Could not find scripts/ptp_stack.sh relative to the repo or binary.";
        ui_state->status_message = "Host PTP stack script not found.";
        return;
    }

    {
        std::lock_guard<std::mutex> lock(ui_state->mutex);
        ui_state->script_path = script_path.string();
        ui_state->last_command = command;
        ui_state->status_message = "Running `" + command + "`...";
        ui_state->error_message.clear();
    }

    ui_state->running.store(true);
    ui_state->worker = std::thread([ui_state, command, script = script_path.string(), refresh_status_after]() {
        HostPtpStackCommandResult command_result = run_ptp_stack_command(script, command);
        std::string combined_output = command_result.output;
        int combined_exit_code = command_result.exit_code;
        std::string combined_error = command_result.error_message;

        orange::gui::HostPtpStackStatusSummary parsed_status;
        bool have_parsed_status = false;

        if (command == "status") {
            parsed_status = parse_ptp_stack_status_output(combined_output);
            have_parsed_status = true;
        } else if (refresh_status_after && command_result.exit_code == 0) {
            HostPtpStackCommandResult status_result = run_ptp_stack_command(script, "status");
            if (!combined_output.empty() && !status_result.output.empty()) {
                combined_output += "\n";
            }
            if (!status_result.output.empty()) {
                combined_output += "[status]\n";
                combined_output += status_result.output;
            }
            if (!status_result.error_message.empty()) {
                if (!combined_error.empty()) {
                    combined_error += "\n";
                }
                combined_error += status_result.error_message;
            }
            if (status_result.exit_code != 0) {
                combined_exit_code = status_result.exit_code;
            }
            parsed_status = parse_ptp_stack_status_output(status_result.output);
            have_parsed_status = true;
        }

        {
            std::lock_guard<std::mutex> lock(ui_state->mutex);
            ui_state->last_exit_code = combined_exit_code;
            ui_state->output_text = combined_output.empty() ? "(no output)" : combined_output;
            ui_state->error_message = combined_error;
            if (have_parsed_status) {
                ui_state->parsed_status = parsed_status;
            }

            if (combined_exit_code == 0) {
                ui_state->status_message = "Host PTP stack `" + command + "` completed.";
            } else {
                ui_state->status_message =
                    "Host PTP stack `" + command + "` failed with exit code " + std::to_string(combined_exit_code) + ".";
            }
        }

        ui_state->running.store(false);
    });
}

}  // namespace

namespace orange::gui {

void reap_host_ptp_stack_worker(HostPtpStackUiState* ui_state)
{
    if (!ui_state) {
        return;
    }
    if (ui_state->worker.joinable() && !ui_state->running.load()) {
        ui_state->worker.join();
    }
}

void render_host_ptp_stack_panel(HostPtpStackUiState* ui_state,
                                 const bool streaming_active,
                                 const bool ptp_stream_sync_enabled)
{
    if (!ui_state) {
        return;
    }

    const bool host_ptp_command_running = ui_state->running.load(std::memory_order_acquire);
    const bool host_ptp_controls_available = (geteuid() == 0);
    const bool host_ptp_stop_unsafe = streaming_active && ptp_stream_sync_enabled;

    HostPtpStackStatusSummary host_ptp_status_summary;
    std::string host_ptp_status_message;
    std::string host_ptp_error_message;
    std::string host_ptp_output_text;
    std::string host_ptp_script_path;
    std::string host_ptp_last_command;
    int host_ptp_last_exit_code = 0;
    {
        std::lock_guard<std::mutex> lock(ui_state->mutex);
        host_ptp_status_summary = ui_state->parsed_status;
        host_ptp_status_message = ui_state->status_message;
        host_ptp_error_message = ui_state->error_message;
        host_ptp_output_text = ui_state->output_text;
        host_ptp_script_path = ui_state->script_path;
        host_ptp_last_command = ui_state->last_command;
        host_ptp_last_exit_code = ui_state->last_exit_code;
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Host PTP Stack");
    if (!host_ptp_controls_available) {
        ImGui::TextDisabled("Run Orange with sudo to control host linuxptp from the UI.");
    } else if (host_ptp_stop_unsafe) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
                           "Stop/restart is disabled while streaming with PTP Stream Sync enabled.");
    }
    if (!host_ptp_status_message.empty()) {
        ImGui::TextWrapped("Status: %s", host_ptp_status_message.c_str());
    }
    if (host_ptp_status_summary.parsed) {
        ImGui::Text("ptp4l: %s | phc2sys: %s | socket: %s",
                    host_ptp_status_summary.ptp4l_running ? "running" : "stopped",
                    host_ptp_status_summary.phc2sys_running ? "running" : "stopped",
                    host_ptp_status_summary.socket_present ? "present" : "missing");
        if (host_ptp_status_summary.gm_present_known ||
            !host_ptp_status_summary.gm_identity.empty() ||
            !host_ptp_status_summary.master_offset.empty()) {
            ImGui::Text("gmPresent: %s | gmIdentity: %s | master_offset: %s",
                        host_ptp_status_summary.gm_present_known
                            ? (host_ptp_status_summary.gm_present ? "true" : "false")
                            : "NA",
                        host_ptp_status_summary.gm_identity.empty()
                            ? "NA"
                            : host_ptp_status_summary.gm_identity.c_str(),
                        host_ptp_status_summary.master_offset.empty()
                            ? "NA"
                            : host_ptp_status_summary.master_offset.c_str());
        }
    }

    const bool disable_start_status = !host_ptp_controls_available || host_ptp_command_running;
    const bool disable_stop_restart =
        !host_ptp_controls_available || host_ptp_command_running || host_ptp_stop_unsafe;

    if (disable_start_status) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Start PTP stack")) {
        start_host_ptp_stack_command(ui_state, "start", true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh PTP status")) {
        start_host_ptp_stack_command(ui_state, "status", false);
    }
    if (disable_start_status) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    if (disable_stop_restart) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Stop PTP stack")) {
        start_host_ptp_stack_command(ui_state, "stop", true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Restart PTP stack")) {
        start_host_ptp_stack_command(ui_state, "restart", true);
    }
    if (disable_stop_restart) {
        ImGui::EndDisabled();
    }

    if (!host_ptp_script_path.empty()) {
        ImGui::TextWrapped("Script: %s", host_ptp_script_path.c_str());
    }
    if (!host_ptp_error_message.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", host_ptp_error_message.c_str());
    }
    if (!host_ptp_output_text.empty()) {
        if (ImGui::Button("Copy PTP output")) {
            ImGui::SetClipboardText(host_ptp_output_text.c_str());
        }
        ImGui::SameLine();
        ImGui::Text("Last command: %s (exit %d)",
                    host_ptp_last_command.empty() ? "-" : host_ptp_last_command.c_str(),
                    host_ptp_last_exit_code);
        ImGui::BeginChild("PTPStackOutput", ImVec2(0, 110), true, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(host_ptp_output_text.c_str());
        ImGui::EndChild();
    }
}

}  // namespace orange::gui
