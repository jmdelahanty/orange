#pragma once

#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace orange {

// Whether the host PTP stack (ptp4l + phc2sys, systemd units) is up, for a
// PTP-gated stream start. Without it the cameras show no PTP lock and deliver
// no frames (2026-11-04). Process names come from /proc/<pid>/comm; ptp4l's
// management socket must exist. Returns false with a reason naming what is
// missing and how to start it.
inline bool host_ptp_stack_running(std::string* reason_out,
                                   const char* ptp4l_socket = "/var/run/ptp4l") {
    bool ptp4l = false;
    bool phc2sys = false;
    std::error_code ec;
    for (std::filesystem::directory_iterator it("/proc", ec), end; !ec && it != end; it.increment(ec)) {
        const std::string pid = it->path().filename().string();
        if (pid.empty() || !std::isdigit(static_cast<unsigned char>(pid[0]))) {
            continue;
        }
        std::ifstream comm(it->path() / "comm");
        std::string name;
        if (!std::getline(comm, name)) {
            continue;
        }
        ptp4l = ptp4l || name == "ptp4l";
        phc2sys = phc2sys || name == "phc2sys";
        if (ptp4l && phc2sys) {
            break;
        }
    }
    std::error_code socket_ec;
    const bool socket_present = std::filesystem::is_socket(ptp4l_socket, socket_ec);
    if (ptp4l && phc2sys && socket_present) {
        return true;
    }
    if (reason_out) {
        std::string missing;
        if (!ptp4l) missing += "ptp4l not running; ";
        if (!phc2sys) missing += "phc2sys not running; ";
        if (ptp4l && !socket_present) missing += std::string(ptp4l_socket) + " missing; ";
        *reason_out = "Host PTP stack is down (" + missing.substr(0, missing.size() - 2) +
                      "). PTP-gated cameras deliver no frames without it. Start it in a terminal: "
                      "sudo systemctl start ptp4l.service phc2sys.service";
    }
    return false;
}

}  // namespace orange
