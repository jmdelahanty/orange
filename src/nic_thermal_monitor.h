#pragma once

#include "json.hpp"

#include <cstdint>
#include <string>

#include <sys/types.h>

namespace orange::monitoring {

struct NicThermalMonitorProcess {
    bool enabled = true;
    bool active = false;
    pid_t pid = -1;
    int sample_period_seconds = 5;
    std::string recording_folder;
    std::string executable_path;
    std::string csv_path;
    std::string summary_path;
    std::string stderr_path;
    std::string started_at_utc;
    std::string stopped_at_utc;
    std::string status = "not_started";
    int exit_code = -1;
    int term_signal = 0;
    std::string error;
};

// Starts a recording-scoped helper process. Sensor discovery and every hwmon
// read happen in the child; the caller only performs a bounded posix_spawn.
// Monitoring is observational: a missing helper is recorded but does not
// reject recording startup.
bool StartNicThermalMonitorProcess(const std::string& recording_folder,
                                   NicThermalMonitorProcess* monitor,
                                   std::string* error_out = nullptr);

// Stops the helper with SIGTERM, waits a bounded interval, and escalates to
// SIGKILL only if the child does not exit. Safe to call repeatedly.
bool StopNicThermalMonitorProcess(NicThermalMonitorProcess* monitor,
                                  std::string* error_out = nullptr);

nlohmann::json NicThermalMonitorProcessToJson(
    const NicThermalMonitorProcess& monitor);

struct NicThermalHelperOptions {
    std::string hwmon_root = "/sys/class/hwmon";
    std::string csv_path;
    std::string summary_path;
    int sample_period_ms = 5000;
    uint64_t max_samples = 0;  // zero means run until SIGTERM/SIGINT
    pid_t expected_parent_pid = -1;
    uid_t output_uid = static_cast<uid_t>(-1);
    gid_t output_gid = static_cast<gid_t>(-1);
};

// Entry point shared by the tiny executable and fixture-backed tests.
int RunNicThermalMonitorHelper(const NicThermalHelperOptions& options,
                               std::string* error_out = nullptr);

}  // namespace orange::monitoring
