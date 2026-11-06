#pragma once

#include "json.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

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
    std::string port_counters_path;
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

// One mlx5 port's ethtool statistics (`ethtool -S`) at one sample.
struct NicPortCounterReading {
    std::string interface;
    std::string pci_bdf;
    std::string operstate;
    bool read_ok = false;
    std::string error;
    std::map<std::string, uint64_t> counters;  // the NicPortCounterNames() the port reports
};

using NicPortCounterSource = std::function<std::vector<NicPortCounterReading>()>;

// The counters nic_port_counters.json keeps, and the subset that means frames
// were lost at the port (rx_discards_phy: the NIC had no room for a camera
// packet; rx_out_of_buffer: no receive buffer posted).
const std::vector<std::string>& NicPortCounterNames();
const std::vector<std::string>& NicPortLossCounterNames();

// Reads the counters of every mlx5_core interface under net_root through the
// SIOCETHTOOL ioctl (ETHTOOL_GSTRINGS + ETHTOOL_GSTATS); no privileges needed.
std::vector<NicPortCounterReading> ReadMlx5PortCounters(
    const std::string& net_root = "/sys/class/net");

// First, last and per-interval changes of the port counters over a recording.
class NicPortCounterTracker {
public:
    void AddSample(uint64_t sample_index,
                   uint64_t elapsed_ns,
                   uint64_t realtime_ns,
                   const std::vector<NicPortCounterReading>& readings);
    // The "ports" object and "ports_with_loss" of nic_port_counters.json.
    nlohmann::json PortsJson() const;
    nlohmann::json PortsWithLossJson() const;
    uint64_t sample_count() const { return sample_count_; }

private:
    struct Interval {
        uint64_t sample_index = 0;
        uint64_t elapsed_ns = 0;
        uint64_t realtime_ns = 0;
        std::map<std::string, int64_t> delta;
    };
    struct Port {
        std::string pci_bdf;
        std::string operstate;
        uint64_t valid_samples = 0;
        uint64_t failed_samples = 0;
        std::string last_error;
        std::map<std::string, uint64_t> first;
        std::map<std::string, uint64_t> previous;
        std::map<std::string, uint64_t> last;
        std::vector<Interval> changed_intervals;  // a counter other than rx_packets_phy moved
        bool changed_intervals_truncated = false;
    };
    std::map<std::string, Port> ports_;
    uint64_t sample_count_ = 0;
};

struct NicThermalHelperOptions {
    std::string hwmon_root = "/sys/class/hwmon";
    std::string csv_path;
    std::string summary_path;
    // nic_port_counters.json; empty skips the port counters.
    std::string port_counters_path;
    std::string net_root = "/sys/class/net";
    // Empty reads ReadMlx5PortCounters(net_root); tests pass a fake.
    NicPortCounterSource port_counter_source;
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
