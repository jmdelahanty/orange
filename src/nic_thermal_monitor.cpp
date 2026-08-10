#include "nic_thermal_monitor.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace orange::monitoring {
namespace {

constexpr int kDefaultSamplePeriodSeconds = 5;
constexpr int kShutdownWaitMs = 5000;
constexpr int kShutdownPollMs = 25;

volatile std::sig_atomic_t g_helper_stop_requested = 0;

void helper_signal_handler(int)
{
    g_helper_stop_requested = 1;
}

std::string trim(std::string value)
{
    while (!value.empty() &&
           (value.back() == '\n' || value.back() == '\r' ||
            value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    size_t begin = 0;
    while (begin < value.size() &&
           (value[begin] == ' ' || value[begin] == '\t')) {
        ++begin;
    }
    return value.substr(begin);
}

bool read_text_file(const std::filesystem::path& path,
                    std::string* value_out)
{
    if (!value_out) {
        return false;
    }
    std::ifstream input(path);
    if (!input) {
        return false;
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (!input.good() && !input.eof()) {
        return false;
    }
    *value_out = trim(contents.str());
    return true;
}

bool read_i64_file(const std::filesystem::path& path,
                   int64_t* value_out)
{
    std::string text;
    if (!read_text_file(path, &text) || text.empty() || !value_out) {
        return false;
    }
    try {
        size_t consumed = 0;
        const int64_t value = std::stoll(text, &consumed, 10);
        if (consumed != text.size()) {
            return false;
        }
        *value_out = value;
        return true;
    } catch (...) {
        return false;
    }
}

uint64_t realtime_ns()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

std::string utc_now()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_r(&time, &utc);
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

bool env_enabled(const char* name, const bool fallback)
{
    const char* value = std::getenv(name);
    if (!value || value[0] == '\0') {
        return fallback;
    }
    const std::string normalized(value);
    if (normalized == "0" || normalized == "false" ||
        normalized == "FALSE" || normalized == "off") {
        return false;
    }
    if (normalized == "1" || normalized == "true" ||
        normalized == "TRUE" || normalized == "on") {
        return true;
    }
    return fallback;
}

int env_sample_period_seconds()
{
    const char* value = std::getenv("ORANGE_NIC_THERMAL_SAMPLE_SECONDS");
    if (!value || value[0] == '\0') {
        return kDefaultSamplePeriodSeconds;
    }
    try {
        const int parsed = std::stoi(value);
        return std::clamp(parsed, 1, 3600);
    } catch (...) {
        return kDefaultSamplePeriodSeconds;
    }
}

bool parse_env_id(const char* name, unsigned long* value_out)
{
    const char* value = std::getenv(name);
    if (!value || value[0] == '\0' || !value_out) {
        return false;
    }
    try {
        size_t consumed = 0;
        const unsigned long parsed = std::stoul(value, &consumed, 10);
        if (consumed != std::strlen(value)) {
            return false;
        }
        *value_out = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

std::pair<uid_t, gid_t> output_owner()
{
    uid_t uid = getuid();
    gid_t gid = getgid();
    if (geteuid() == 0) {
        unsigned long parsed = 0;
        if (parse_env_id("SUDO_UID", &parsed)) {
            uid = static_cast<uid_t>(parsed);
        }
        if (parse_env_id("SUDO_GID", &parsed)) {
            gid = static_cast<gid_t>(parsed);
        }
    }
    return {uid, gid};
}

bool set_error(std::string* error_out, const std::string& message)
{
    if (error_out) {
        *error_out = message;
    }
    return false;
}

int open_owned_file(const std::filesystem::path& path,
                    const int flags,
                    const uid_t uid,
                    const gid_t gid,
                    std::string* error_out)
{
    const int fd = ::open(path.c_str(), flags | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) {
        set_error(error_out,
                  "failed to open " + path.string() + ": " +
                      std::strerror(errno));
        return -1;
    }
    if (::fchmod(fd, 0644) != 0) {
        const std::string message =
            "failed to set permissions on " + path.string() + ": " +
            std::strerror(errno);
        ::close(fd);
        set_error(error_out, message);
        return -1;
    }
    if (uid != static_cast<uid_t>(-1) &&
        gid != static_cast<gid_t>(-1) &&
        ::fchown(fd, uid, gid) != 0 && errno != EPERM) {
        const std::string message =
            "failed to set ownership on " + path.string() + ": " +
            std::strerror(errno);
        ::close(fd);
        set_error(error_out, message);
        return -1;
    }
    return fd;
}

bool write_all(const int fd, const std::string& contents)
{
    size_t offset = 0;
    while (offset < contents.size()) {
        const ssize_t written =
            ::write(fd, contents.data() + offset, contents.size() - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        offset += static_cast<size_t>(written);
    }
    return true;
}

bool write_json_atomic_owned(const std::filesystem::path& path,
                             const nlohmann::json& value,
                             const uid_t uid,
                             const gid_t gid,
                             std::string* error_out)
{
    const std::filesystem::path temporary =
        path.string() + ".tmp." + std::to_string(getpid());
    std::error_code remove_error;
    std::filesystem::remove(temporary, remove_error);
    const int fd = open_owned_file(
        temporary, O_CREAT | O_EXCL | O_WRONLY, uid, gid, error_out);
    if (fd < 0) {
        return false;
    }
    const std::string contents = value.dump(2) + "\n";
    const bool wrote = write_all(fd, contents);
    const int sync_result = wrote ? ::fsync(fd) : -1;
    const int close_result = ::close(fd);
    if (!wrote || sync_result != 0 || close_result != 0) {
        std::filesystem::remove(temporary, remove_error);
        return set_error(error_out,
                         "failed to write thermal summary " + path.string());
    }
    std::error_code rename_error;
    std::filesystem::rename(temporary, path, rename_error);
    if (rename_error) {
        std::filesystem::remove(temporary, remove_error);
        return set_error(error_out,
                         "failed to publish thermal summary " + path.string() +
                             ": " + rename_error.message());
    }
    return true;
}

std::string csv_escape(const std::string& value)
{
    if (value.find_first_of(",\"\n\r") == std::string::npos) {
        return value;
    }
    std::string escaped = "\"";
    for (const char ch : value) {
        if (ch == '\"') {
            escaped += "\"\"";
        } else {
            escaped += ch;
        }
    }
    escaped += '\"';
    return escaped;
}

std::string join_strings(const std::vector<std::string>& values,
                         const char delimiter)
{
    std::ostringstream out;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            out << delimiter;
        }
        out << values[i];
    }
    return out.str();
}

std::string pci_bdf_for_hwmon(const std::filesystem::path& hwmon)
{
    std::error_code ec;
    const std::filesystem::path device =
        std::filesystem::canonical(hwmon / "device", ec);
    if (ec || device.empty()) {
        return {};
    }
    return device.filename().string();
}

std::vector<std::string> network_interfaces_for_hwmon(
    const std::filesystem::path& hwmon)
{
    std::vector<std::string> interfaces;
    std::error_code ec;
    const std::filesystem::path net_dir = hwmon / "device" / "net";
    if (!std::filesystem::is_directory(net_dir, ec) || ec) {
        return interfaces;
    }
    for (const auto& entry : std::filesystem::directory_iterator(net_dir, ec)) {
        if (ec) {
            break;
        }
        interfaces.push_back(entry.path().filename().string());
    }
    std::sort(interfaces.begin(), interfaces.end());
    return interfaces;
}

struct ThermalSensorReading {
    std::string hwmon;
    std::string pci_bdf;
    std::string interfaces;
    std::string sensor_id;
    std::string label;
    int64_t input_millic = 0;
    int64_t max_millic = 0;
    int64_t crit_millic = 0;
    int64_t highest_millic = 0;
    bool input_read = false;
    bool valid = false;
    bool warning = false;
    bool critical = false;
    std::string status;
};

std::vector<ThermalSensorReading> collect_mlx5_readings(
    const std::filesystem::path& hwmon_root)
{
    std::vector<ThermalSensorReading> readings;
    std::error_code ec;
    if (!std::filesystem::is_directory(hwmon_root, ec) || ec) {
        return readings;
    }
    for (const auto& hwmon_entry :
         std::filesystem::directory_iterator(hwmon_root, ec)) {
        if (ec || !hwmon_entry.is_directory(ec)) {
            continue;
        }
        std::string name;
        if (!read_text_file(hwmon_entry.path() / "name", &name) ||
            name != "mlx5") {
            continue;
        }
        const std::string bdf = pci_bdf_for_hwmon(hwmon_entry.path());
        const std::string interfaces = join_strings(
            network_interfaces_for_hwmon(hwmon_entry.path()), ';');
        for (const auto& sensor_entry :
             std::filesystem::directory_iterator(hwmon_entry.path(), ec)) {
            if (ec || !sensor_entry.is_regular_file(ec)) {
                continue;
            }
            const std::string filename = sensor_entry.path().filename().string();
            constexpr std::string_view suffix = "_input";
            if (filename.rfind("temp", 0) != 0 ||
                filename.size() <= suffix.size() ||
                filename.compare(
                    filename.size() - suffix.size(), suffix.size(), suffix) != 0) {
                continue;
            }
            const std::string sensor_id =
                filename.substr(0, filename.size() - suffix.size());
            ThermalSensorReading reading;
            reading.hwmon = hwmon_entry.path().filename().string();
            reading.pci_bdf = bdf;
            reading.interfaces = interfaces;
            reading.sensor_id = sensor_id;
            if (!read_text_file(
                    hwmon_entry.path() / (sensor_id + "_label"),
                    &reading.label)) {
                reading.label = sensor_id;
            }
            reading.input_read =
                read_i64_file(sensor_entry.path(), &reading.input_millic);
            (void)read_i64_file(
                hwmon_entry.path() / (sensor_id + "_max"),
                &reading.max_millic);
            (void)read_i64_file(
                hwmon_entry.path() / (sensor_id + "_crit"),
                &reading.crit_millic);
            (void)read_i64_file(
                hwmon_entry.path() / (sensor_id + "_highest"),
                &reading.highest_millic);
            if (!reading.input_read) {
                reading.status = "read_error";
            } else if (reading.input_millic <= 0) {
                // mlx5 reports zero when the adapter is not returning valid
                // telemetry. Never convert this to a plausible 0 C sample.
                reading.status = "unavailable_zero";
            } else {
                reading.valid = true;
                reading.warning =
                    reading.max_millic > 0 &&
                    reading.input_millic >= reading.max_millic;
                reading.critical =
                    reading.crit_millic > 0 &&
                    reading.input_millic >= reading.crit_millic;
                reading.status = reading.critical
                    ? "critical"
                    : (reading.warning ? "warning" : "ok");
            }
            readings.push_back(std::move(reading));
        }
    }
    std::sort(
        readings.begin(), readings.end(),
        [](const ThermalSensorReading& lhs, const ThermalSensorReading& rhs) {
            return std::tie(lhs.pci_bdf, lhs.sensor_id, lhs.hwmon) <
                   std::tie(rhs.pci_bdf, rhs.sensor_id, rhs.hwmon);
        });
    return readings;
}

struct ThermalAggregate {
    uint64_t valid_samples = 0;
    uint64_t invalid_samples = 0;
    uint64_t warning_samples = 0;
    uint64_t critical_samples = 0;
    int64_t min_millic = 0;
    int64_t max_millic = 0;
    int64_t last_millic = 0;
    std::string last_status;
    std::string pci_bdf;
    std::string interfaces;
    std::string label;
};

nlohmann::json aggregate_to_json(const std::map<std::string, ThermalAggregate>& aggregates)
{
    nlohmann::json sensors = nlohmann::json::object();
    for (const auto& [key, aggregate] : aggregates) {
        nlohmann::json item = {
            {"pci_bdf", aggregate.pci_bdf},
            {"network_interfaces", aggregate.interfaces},
            {"label", aggregate.label},
            {"valid_samples", aggregate.valid_samples},
            {"invalid_samples", aggregate.invalid_samples},
            {"warning_samples", aggregate.warning_samples},
            {"critical_samples", aggregate.critical_samples},
            {"last_status", aggregate.last_status},
        };
        if (aggregate.valid_samples > 0) {
            item["min_millic"] = aggregate.min_millic;
            item["max_millic"] = aggregate.max_millic;
            item["last_millic"] = aggregate.last_millic;
            item["min_celsius"] =
                static_cast<double>(aggregate.min_millic) / 1000.0;
            item["max_celsius"] =
                static_cast<double>(aggregate.max_millic) / 1000.0;
            item["last_celsius"] =
                static_cast<double>(aggregate.last_millic) / 1000.0;
        }
        sensors[key] = std::move(item);
    }
    return sensors;
}

std::string current_executable_directory()
{
    std::vector<char> buffer(4096, '\0');
    const ssize_t length =
        ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length <= 0) {
        return {};
    }
    buffer[static_cast<size_t>(length)] = '\0';
    return std::filesystem::path(buffer.data()).parent_path().string();
}

std::string resolve_helper_executable()
{
    const char* configured = std::getenv("ORANGE_NIC_THERMAL_MONITOR_BIN");
    if (configured && configured[0] != '\0') {
        return configured;
    }
    const std::string directory = current_executable_directory();
    if (directory.empty()) {
        return {};
    }
    return (std::filesystem::path(directory) / "nic_thermal_monitor").string();
}

}  // namespace

int RunNicThermalMonitorHelper(const NicThermalHelperOptions& options,
                               std::string* error_out)
{
    if (options.csv_path.empty() || options.summary_path.empty()) {
        set_error(error_out, "thermal helper requires CSV and summary paths");
        return 2;
    }
    if (options.sample_period_ms <= 0) {
        set_error(error_out, "thermal helper sample period must be positive");
        return 2;
    }

    g_helper_stop_requested = 0;
    struct sigaction action{};
    action.sa_handler = helper_signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGINT, &action, nullptr);
    if (options.expected_parent_pid > 0) {
        if (::prctl(PR_SET_PDEATHSIG, SIGTERM) != 0) {
            set_error(error_out, "failed to configure parent-death signal");
            return 1;
        }
        if (::getppid() != options.expected_parent_pid) {
            set_error(error_out, "thermal monitor parent exited before helper startup");
            return 1;
        }
    }

    const int csv_fd = open_owned_file(
        options.csv_path,
        O_CREAT | O_WRONLY | O_TRUNC,
        options.output_uid,
        options.output_gid,
        error_out);
    if (csv_fd < 0) {
        return 1;
    }
    FILE* csv = ::fdopen(csv_fd, "w");
    if (!csv) {
        const std::string message =
            "failed to create thermal CSV stream: " +
            std::string(std::strerror(errno));
        ::close(csv_fd);
        set_error(error_out, message);
        return 1;
    }
    std::setvbuf(csv, nullptr, _IOLBF, 0);
    std::fprintf(
        csv,
        "sample_index,monotonic_elapsed_ns,realtime_posix_ns,hwmon,pci_bdf,"
        "network_interfaces,sensor_id,label,temp_millic,max_millic,crit_millic,"
        "highest_millic,valid,warning,critical,status\n");

    const auto started_steady = std::chrono::steady_clock::now();
    const uint64_t started_realtime_ns = realtime_ns();
    const std::string started_at_utc = utc_now();
    uint64_t sample_count = 0;
    uint64_t sample_batches_with_valid_readings = 0;
    uint64_t sample_batches_without_mlx5_sensors = 0;
    uint64_t valid_rows = 0;
    uint64_t invalid_rows = 0;
    uint64_t warning_rows = 0;
    uint64_t critical_rows = 0;
    uint64_t sensor_set_change_events = 0;
    std::map<std::string, uint64_t> invalid_reasons;
    std::map<std::string, ThermalAggregate> aggregates;
    std::set<std::string> previous_sensor_set;
    bool have_previous_sensor_set = false;
    std::string stop_reason = "signal";
    std::string runtime_error;

    auto publish_summary = [&](const std::string& status) -> bool {
        const nlohmann::json summary = {
            {"schema_id", "orange.nic_thermal_monitor_summary"},
            {"schema_version", 1},
            {"status", status},
            {"sensor_backend", "linux_hwmon_sysfs"},
            {"driver_name", "mlx5"},
            {"zero_input_policy", "invalid_unavailable"},
            {"hwmon_root", options.hwmon_root},
            {"csv_path", options.csv_path},
            {"sample_period_ms", options.sample_period_ms},
            {"expected_parent_pid",
             options.expected_parent_pid > 0
                 ? static_cast<int64_t>(options.expected_parent_pid)
                 : static_cast<int64_t>(-1)},
            {"started_at_utc", started_at_utc},
            {"started_realtime_posix_ns", started_realtime_ns},
            {"updated_at_utc", utc_now()},
            {"updated_realtime_posix_ns", realtime_ns()},
            {"sample_count", sample_count},
            {"sample_batches_with_valid_readings",
             sample_batches_with_valid_readings},
            {"sample_batches_without_mlx5_sensors",
             sample_batches_without_mlx5_sensors},
            {"valid_rows", valid_rows},
            {"invalid_rows", invalid_rows},
            {"warning_rows", warning_rows},
            {"critical_rows", critical_rows},
            {"sensor_set_change_events", sensor_set_change_events},
            {"invalid_reasons", invalid_reasons},
            {"sensors", aggregate_to_json(aggregates)},
            {"stop_reason", stop_reason},
            {"error", runtime_error},
        };
        return write_json_atomic_owned(
            options.summary_path,
            summary,
            options.output_uid,
            options.output_gid,
            &runtime_error);
    };

    while (!g_helper_stop_requested) {
        const uint64_t sample_realtime_ns = realtime_ns();
        const uint64_t elapsed_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - started_steady)
                .count());
        const std::vector<ThermalSensorReading> readings =
            collect_mlx5_readings(options.hwmon_root);
        std::set<std::string> current_sensor_set;
        bool batch_has_valid = false;
        if (readings.empty()) {
            ++sample_batches_without_mlx5_sensors;
            ++invalid_rows;
            ++invalid_reasons["no_mlx5_hwmon"];
            std::fprintf(
                csv,
                "%llu,%llu,%llu,,,,,,,,,,,0,0,0,no_mlx5_hwmon\n",
                static_cast<unsigned long long>(sample_count),
                static_cast<unsigned long long>(elapsed_ns),
                static_cast<unsigned long long>(sample_realtime_ns));
        } else {
            for (const ThermalSensorReading& reading : readings) {
                const std::string key =
                    reading.pci_bdf + "/" + reading.sensor_id;
                current_sensor_set.insert(key);
                ThermalAggregate& aggregate = aggregates[key];
                aggregate.pci_bdf = reading.pci_bdf;
                aggregate.interfaces = reading.interfaces;
                aggregate.label = reading.label;
                aggregate.last_status = reading.status;
                if (reading.valid) {
                    batch_has_valid = true;
                    ++valid_rows;
                    ++aggregate.valid_samples;
                    aggregate.last_millic = reading.input_millic;
                    if (aggregate.valid_samples == 1) {
                        aggregate.min_millic = reading.input_millic;
                        aggregate.max_millic = reading.input_millic;
                    } else {
                        aggregate.min_millic = std::min(
                            aggregate.min_millic, reading.input_millic);
                        aggregate.max_millic = std::max(
                            aggregate.max_millic, reading.input_millic);
                    }
                } else {
                    ++invalid_rows;
                    ++aggregate.invalid_samples;
                    ++invalid_reasons[reading.status];
                }
                if (reading.warning) {
                    ++warning_rows;
                    ++aggregate.warning_samples;
                }
                if (reading.critical) {
                    ++critical_rows;
                    ++aggregate.critical_samples;
                }
                std::fprintf(
                    csv,
                    "%llu,%llu,%llu,%s,%s,%s,%s,%s,%lld,%lld,%lld,%lld,%d,%d,%d,%s\n",
                    static_cast<unsigned long long>(sample_count),
                    static_cast<unsigned long long>(elapsed_ns),
                    static_cast<unsigned long long>(sample_realtime_ns),
                    csv_escape(reading.hwmon).c_str(),
                    csv_escape(reading.pci_bdf).c_str(),
                    csv_escape(reading.interfaces).c_str(),
                    csv_escape(reading.sensor_id).c_str(),
                    csv_escape(reading.label).c_str(),
                    static_cast<long long>(reading.input_millic),
                    static_cast<long long>(reading.max_millic),
                    static_cast<long long>(reading.crit_millic),
                    static_cast<long long>(reading.highest_millic),
                    reading.valid ? 1 : 0,
                    reading.warning ? 1 : 0,
                    reading.critical ? 1 : 0,
                    csv_escape(reading.status).c_str());
            }
        }
        if (batch_has_valid) {
            ++sample_batches_with_valid_readings;
        }
        if (have_previous_sensor_set && current_sensor_set != previous_sensor_set) {
            ++sensor_set_change_events;
        }
        previous_sensor_set = std::move(current_sensor_set);
        have_previous_sensor_set = true;
        ++sample_count;
        if (::fflush(csv) != 0) {
            runtime_error = "failed to flush thermal CSV";
            stop_reason = "write_error";
            break;
        }
        if (!publish_summary("running")) {
            stop_reason = "summary_write_error";
            break;
        }
        if (options.max_samples > 0 && sample_count >= options.max_samples) {
            stop_reason = "max_samples_reached";
            break;
        }
        int remaining_ms = options.sample_period_ms;
        while (remaining_ms > 0 &&
               !g_helper_stop_requested) {
            const int slice_ms = std::min(remaining_ms, 100);
            std::this_thread::sleep_for(std::chrono::milliseconds(slice_ms));
            remaining_ms -= slice_ms;
        }
    }

    if (g_helper_stop_requested) {
        stop_reason = "signal_requested";
    }
    if (::fflush(csv) != 0 && runtime_error.empty()) {
        runtime_error = "failed to flush thermal CSV at shutdown";
    }
    if (::fclose(csv) != 0 && runtime_error.empty()) {
        runtime_error = "failed to close thermal CSV";
    }
    const bool success = runtime_error.empty();
    if (!publish_summary(success ? "completed" : "failed") &&
        runtime_error.empty()) {
        runtime_error = "failed to publish final thermal summary";
    }
    if (!runtime_error.empty()) {
        set_error(error_out, runtime_error);
        return 1;
    }
    if (error_out) {
        error_out->clear();
    }
    return 0;
}

bool StartNicThermalMonitorProcess(const std::string& recording_folder,
                                   NicThermalMonitorProcess* monitor,
                                   std::string* error_out)
{
    if (!monitor) {
        return set_error(error_out, "NIC thermal monitor state is null");
    }
    if (monitor->active) {
        std::string stop_error;
        if (!StopNicThermalMonitorProcess(monitor, &stop_error)) {
            return set_error(
                error_out,
                "failed to stop the previous NIC thermal helper: " +
                    stop_error);
        }
    }
    *monitor = NicThermalMonitorProcess{};
    monitor->enabled = env_enabled("ORANGE_NIC_THERMAL_MONITOR_ENABLED", true);
    monitor->recording_folder = recording_folder;
    monitor->sample_period_seconds = env_sample_period_seconds();
    monitor->csv_path =
        (std::filesystem::path(recording_folder) / "nic_thermal_monitor.csv").string();
    monitor->summary_path =
        (std::filesystem::path(recording_folder) /
         "nic_thermal_monitor_summary.json").string();
    monitor->stderr_path =
        (std::filesystem::path(recording_folder) /
         "nic_thermal_monitor.stderr.log").string();
    monitor->started_at_utc = utc_now();
    if (!monitor->enabled) {
        monitor->status = "disabled";
        if (error_out) {
            error_out->clear();
        }
        return true;
    }
    if (recording_folder.empty()) {
        monitor->status = "failed_to_start";
        monitor->error = "recording folder is empty";
        return set_error(error_out, monitor->error);
    }
    monitor->executable_path = resolve_helper_executable();
    if (monitor->executable_path.empty() ||
        ::access(monitor->executable_path.c_str(), X_OK) != 0) {
        monitor->status = "failed_to_start";
        monitor->error =
            "NIC thermal helper is not executable: " + monitor->executable_path;
        return set_error(error_out, monitor->error);
    }

    const auto [uid, gid] = output_owner();
    std::string file_error;
    const int stderr_fd = open_owned_file(
        monitor->stderr_path,
        O_CREAT | O_WRONLY | O_TRUNC,
        uid,
        gid,
        &file_error);
    if (stderr_fd < 0) {
        monitor->status = "failed_to_start";
        monitor->error = file_error;
        return set_error(error_out, monitor->error);
    }
    const int null_fd = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (null_fd < 0) {
        ::close(stderr_fd);
        monitor->status = "failed_to_start";
        monitor->error = "failed to open /dev/null for thermal helper";
        return set_error(error_out, monitor->error);
    }

    const std::string period =
        std::to_string(monitor->sample_period_seconds * 1000);
    const std::string uid_text = std::to_string(static_cast<unsigned long>(uid));
    const std::string gid_text = std::to_string(static_cast<unsigned long>(gid));
    const std::string parent_pid_text = std::to_string(getpid());
    std::vector<std::string> arguments = {
        monitor->executable_path,
        "--csv", monitor->csv_path,
        "--summary", monitor->summary_path,
        "--sample-period-ms", period,
        "--parent-pid", parent_pid_text,
        "--output-uid", uid_text,
        "--output-gid", gid_text,
    };
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (std::string& argument : arguments) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, null_fd, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, stderr_fd, STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, null_fd);
    posix_spawn_file_actions_addclose(&actions, stderr_fd);
    pid_t pid = -1;
    const int spawn_result = posix_spawn(
        &pid,
        monitor->executable_path.c_str(),
        &actions,
        nullptr,
        argv.data(),
        environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(null_fd);
    ::close(stderr_fd);
    if (spawn_result != 0) {
        monitor->status = "failed_to_start";
        monitor->error =
            "failed to spawn NIC thermal helper: " +
            std::string(std::strerror(spawn_result));
        return set_error(error_out, monitor->error);
    }

    monitor->pid = pid;
    monitor->active = true;
    monitor->status = "running";
    if (error_out) {
        error_out->clear();
    }
    return true;
}

bool StopNicThermalMonitorProcess(NicThermalMonitorProcess* monitor,
                                  std::string* error_out)
{
    if (!monitor) {
        return set_error(error_out, "NIC thermal monitor state is null");
    }
    if (!monitor->active || monitor->pid <= 0) {
        if (monitor->status != "not_started" && monitor->stopped_at_utc.empty()) {
            monitor->stopped_at_utc = utc_now();
        }
        if (error_out) {
            *error_out = monitor->error;
        }
        return monitor->error.empty();
    }

    if (::kill(monitor->pid, SIGTERM) != 0 && errno != ESRCH) {
        monitor->error =
            "failed to signal NIC thermal helper: " +
            std::string(std::strerror(errno));
    }
    int wait_status = 0;
    bool reaped = false;
    for (int waited_ms = 0; waited_ms < kShutdownWaitMs;
         waited_ms += kShutdownPollMs) {
        const pid_t result = ::waitpid(monitor->pid, &wait_status, WNOHANG);
        if (result == monitor->pid) {
            reaped = true;
            break;
        }
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == ECHILD) {
                reaped = true;
                break;
            }
            monitor->error =
                "waitpid failed for NIC thermal helper: " +
                std::string(std::strerror(errno));
            break;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(kShutdownPollMs));
    }
    if (!reaped) {
        (void)::kill(monitor->pid, SIGKILL);
        if (::waitpid(monitor->pid, &wait_status, 0) == monitor->pid) {
            reaped = true;
        }
        if (monitor->error.empty()) {
            monitor->error =
                "NIC thermal helper exceeded bounded shutdown and was killed";
        }
    }

    monitor->active = false;
    monitor->stopped_at_utc = utc_now();
    if (reaped && WIFEXITED(wait_status)) {
        monitor->exit_code = WEXITSTATUS(wait_status);
        monitor->status = monitor->exit_code == 0
            ? "completed"
            : "exited_with_error";
        if (monitor->exit_code != 0 && monitor->error.empty()) {
            monitor->error =
                "NIC thermal helper exited with code " +
                std::to_string(monitor->exit_code);
        }
    } else if (reaped && WIFSIGNALED(wait_status)) {
        monitor->term_signal = WTERMSIG(wait_status);
        monitor->status = "stopped_with_signal";
        if (monitor->error.empty()) {
            monitor->error =
                "NIC thermal helper stopped by signal " +
                std::to_string(monitor->term_signal);
        }
    } else if (monitor->status == "running") {
        monitor->status = "stopped";
    }
    if (error_out) {
        *error_out = monitor->error;
    }
    return monitor->error.empty();
}

nlohmann::json NicThermalMonitorProcessToJson(
    const NicThermalMonitorProcess& monitor)
{
    nlohmann::json value = {
        {"schema_id", "orange.nic_thermal_monitor_process"},
        {"schema_version", 1},
        {"enabled", monitor.enabled},
        {"status", monitor.status},
        {"isolation", "helper_process"},
        {"sensor_backend", "linux_hwmon_sysfs"},
        {"driver_name", "mlx5"},
        {"sample_period_seconds", monitor.sample_period_seconds},
        {"recording_folder", monitor.recording_folder},
        {"executable_path", monitor.executable_path},
        {"csv_path", monitor.csv_path},
        {"summary_path", monitor.summary_path},
        {"stderr_path", monitor.stderr_path},
        {"started_at_utc", monitor.started_at_utc},
        {"stopped_at_utc", monitor.stopped_at_utc},
        {"error", monitor.error},
    };
    if (monitor.active && monitor.pid > 0) {
        value["pid"] = static_cast<int>(monitor.pid);
    }
    if (monitor.exit_code >= 0) {
        value["exit_code"] = monitor.exit_code;
    }
    if (monitor.term_signal > 0) {
        value["signal"] = monitor.term_signal;
    }
    return value;
}

}  // namespace orange::monitoring
