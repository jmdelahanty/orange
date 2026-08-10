#include "nic_thermal_monitor.h"

#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace {

bool parse_u64(const std::string& text, uint64_t* value_out)
{
    if (!value_out || text.empty() || text.front() == '-') {
        return false;
    }
    try {
        size_t consumed = 0;
        const unsigned long long parsed = std::stoull(text, &consumed, 10);
        if (consumed != text.size()) {
            return false;
        }
        *value_out = static_cast<uint64_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_id(const std::string& text, unsigned long* value_out)
{
    uint64_t value = 0;
    if (!parse_u64(text, &value) ||
        value > std::numeric_limits<unsigned long>::max() || !value_out) {
        return false;
    }
    *value_out = static_cast<unsigned long>(value);
    return true;
}

void print_usage(const char* argv0)
{
    std::cerr
        << "Usage: " << argv0
        << " --csv PATH --summary PATH [--hwmon-root PATH]"
           " [--sample-period-ms N] [--max-samples N]"
           " [--parent-pid N]"
           " [--output-uid N] [--output-gid N]\n";
}

}  // namespace

int main(int argc, char** argv)
{
    orange::monitoring::NicThermalHelperOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        auto require_value = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << name << "\n";
                return nullptr;
            }
            return argv[++i];
        };
        if (argument == "--csv") {
            const char* value = require_value("--csv");
            if (!value) return 2;
            options.csv_path = value;
        } else if (argument == "--summary") {
            const char* value = require_value("--summary");
            if (!value) return 2;
            options.summary_path = value;
        } else if (argument == "--hwmon-root") {
            const char* value = require_value("--hwmon-root");
            if (!value) return 2;
            options.hwmon_root = value;
        } else if (argument == "--sample-period-ms") {
            const char* value = require_value("--sample-period-ms");
            uint64_t parsed = 0;
            if (!value || !parse_u64(value, &parsed) || parsed == 0 ||
                parsed > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
                std::cerr << "Invalid --sample-period-ms\n";
                return 2;
            }
            options.sample_period_ms = static_cast<int>(parsed);
        } else if (argument == "--max-samples") {
            const char* value = require_value("--max-samples");
            if (!value || !parse_u64(value, &options.max_samples)) {
                std::cerr << "Invalid --max-samples\n";
                return 2;
            }
        } else if (argument == "--parent-pid") {
            const char* value = require_value("--parent-pid");
            uint64_t parsed = 0;
            if (!value || !parse_u64(value, &parsed) || parsed == 0 ||
                parsed > static_cast<uint64_t>(
                    std::numeric_limits<pid_t>::max())) {
                std::cerr << "Invalid --parent-pid\n";
                return 2;
            }
            options.expected_parent_pid = static_cast<pid_t>(parsed);
        } else if (argument == "--output-uid") {
            const char* value = require_value("--output-uid");
            unsigned long parsed = 0;
            if (!value || !parse_id(value, &parsed)) {
                std::cerr << "Invalid --output-uid\n";
                return 2;
            }
            options.output_uid = static_cast<uid_t>(parsed);
        } else if (argument == "--output-gid") {
            const char* value = require_value("--output-gid");
            unsigned long parsed = 0;
            if (!value || !parse_id(value, &parsed)) {
                std::cerr << "Invalid --output-gid\n";
                return 2;
            }
            options.output_gid = static_cast<gid_t>(parsed);
        } else if (argument == "--help" || argument == "-h") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown argument: " << argument << "\n";
            print_usage(argv[0]);
            return 2;
        }
    }

    std::string error;
    const int result = orange::monitoring::RunNicThermalMonitorHelper(
        options, &error);
    if (result != 0 && !error.empty()) {
        std::cerr << error << "\n";
    }
    return result;
}
