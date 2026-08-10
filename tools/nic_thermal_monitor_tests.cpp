#include "nic_thermal_monitor.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include <unistd.h>

namespace {

int failures = 0;
std::filesystem::path binary_directory;

void expect(bool condition, const std::string& message)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << "\n";
    }
}

void write_text(const std::filesystem::path& path, const std::string& value)
{
    std::ofstream output(path);
    output << value;
}

std::string read_text(const std::filesystem::path& path)
{
    std::ifstream input(path);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

nlohmann::json read_json(const std::filesystem::path& path)
{
    std::ifstream input(path);
    nlohmann::json value;
    input >> value;
    return value;
}

std::filesystem::path make_fixture_root(const std::string& suffix)
{
    const std::filesystem::path root =
        std::filesystem::path("/tmp") /
        ("orange_nic_thermal_test_" + std::to_string(getpid()) + "_" + suffix);
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root);
    return root;
}

void test_valid_and_unavailable_samples()
{
    const std::filesystem::path root = make_fixture_root("readings");
    const std::filesystem::path hwmon_root = root / "hwmon";
    const std::filesystem::path hwmon = hwmon_root / "hwmon0";
    const std::filesystem::path device = root / "pci" / "0000:49:00.0";
    std::filesystem::create_directories(hwmon);
    std::filesystem::create_directories(device / "net" / "mlnx1_p1_25g");
    std::filesystem::create_directory_symlink(device, hwmon / "device");
    write_text(hwmon / "name", "mlx5\n");
    write_text(hwmon / "temp1_label", "asic\n");
    write_text(hwmon / "temp1_input", "65000\n");
    write_text(hwmon / "temp1_max", "80000\n");
    write_text(hwmon / "temp1_crit", "90000\n");
    write_text(hwmon / "temp1_highest", "70000\n");
    write_text(hwmon / "temp2_label", "module0\n");
    write_text(hwmon / "temp2_input", "0\n");
    write_text(hwmon / "temp2_crit", "100000\n");
    write_text(hwmon / "temp3_label", "hotspot\n");
    write_text(hwmon / "temp3_input", "95000\n");
    write_text(hwmon / "temp3_max", "80000\n");
    write_text(hwmon / "temp3_crit", "90000\n");

    orange::monitoring::NicThermalHelperOptions options;
    options.hwmon_root = hwmon_root.string();
    options.csv_path = (root / "thermal.csv").string();
    options.summary_path = (root / "summary.json").string();
    options.sample_period_ms = 1;
    options.max_samples = 2;
    options.output_uid = getuid();
    options.output_gid = getgid();
    std::string error;
    expect(
        orange::monitoring::RunNicThermalMonitorHelper(options, &error) == 0,
        "fixture helper succeeds: " + error);

    const std::string csv = read_text(options.csv_path);
    expect(csv.find("sample_index,monotonic_elapsed_ns,realtime_posix_ns") == 0,
           "CSV begins with the versioned column contract");
    expect(csv.find("0000:49:00.0,mlnx1_p1_25g,temp1,asic,65000") !=
               std::string::npos,
           "valid mlx5 reading includes BDF, interface, and raw millidegrees");
    expect(csv.find("temp2,module0,0,0,100000,0,0,0,0,unavailable_zero") !=
               std::string::npos,
           "zero mlx5 input is explicitly invalid rather than 0 C");
    expect(csv.find("temp3,hotspot,95000,80000,90000,0,1,1,1,critical") !=
               std::string::npos,
           "threshold crossings are preserved as warning and critical evidence");

    const nlohmann::json summary = read_json(options.summary_path);
    expect(summary.value("schema_id", "") ==
               "orange.nic_thermal_monitor_summary",
           "summary schema is identified");
    expect(summary.value("status", "") == "completed",
           "bounded fixture finishes cleanly");
    expect(summary.value("sample_count", 0ULL) == 2,
           "summary records two sample batches");
    expect(summary.value("valid_rows", 0ULL) == 4,
           "summary counts valid rows");
    expect(summary.value("invalid_rows", 0ULL) == 2,
           "summary counts unavailable rows");
    expect(summary["invalid_reasons"].value("unavailable_zero", 0ULL) == 2,
           "summary preserves why the readings were invalid");
    expect(summary.value("warning_rows", 0ULL) == 2 &&
               summary.value("critical_rows", 0ULL) == 2,
           "summary aggregates threshold crossings");
    const nlohmann::json& sensor = summary["sensors"]["0000:49:00.0/temp1"];
    expect(sensor.value("min_millic", 0LL) == 65000,
           "valid sensor minimum is aggregated");
    expect(sensor.value("max_millic", 0LL) == 65000,
           "valid sensor maximum is aggregated");

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

void test_missing_mlx5_is_observable()
{
    const std::filesystem::path root = make_fixture_root("missing");
    const std::filesystem::path hwmon_root = root / "hwmon";
    std::filesystem::create_directories(hwmon_root / "hwmon0");
    write_text(hwmon_root / "hwmon0" / "name", "nvme\n");

    orange::monitoring::NicThermalHelperOptions options;
    options.hwmon_root = hwmon_root.string();
    options.csv_path = (root / "thermal.csv").string();
    options.summary_path = (root / "summary.json").string();
    options.sample_period_ms = 1;
    options.max_samples = 1;
    options.output_uid = getuid();
    options.output_gid = getgid();
    std::string error;
    expect(
        orange::monitoring::RunNicThermalMonitorHelper(options, &error) == 0,
        "missing-mlx5 fixture remains an observational success: " + error);
    const std::string csv = read_text(options.csv_path);
    expect(csv.find("no_mlx5_hwmon") != std::string::npos,
           "missing mlx5 sensor set is written as an invalid sentinel row");
    const nlohmann::json summary = read_json(options.summary_path);
    expect(summary.value("sample_batches_without_mlx5_sensors", 0ULL) == 1,
           "summary counts missing mlx5 batches");
    expect(summary["invalid_reasons"].value("no_mlx5_hwmon", 0ULL) == 1,
           "summary distinguishes missing hwmon from zero sensor values");

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

void test_parent_process_lifecycle()
{
    const std::filesystem::path root = make_fixture_root("process");
    const std::filesystem::path helper = binary_directory / "nic_thermal_monitor";
    setenv("ORANGE_NIC_THERMAL_MONITOR_BIN", helper.c_str(), 1);
    setenv("ORANGE_NIC_THERMAL_MONITOR_ENABLED", "1", 1);
    setenv("ORANGE_NIC_THERMAL_SAMPLE_SECONDS", "1", 1);

    orange::monitoring::NicThermalMonitorProcess process;
    std::string error;
    expect(
        orange::monitoring::StartNicThermalMonitorProcess(
            root.string(), &process, &error),
        "parent starts the sibling helper: " + error);
    expect(process.active && process.pid > 0,
           "started helper has an active child identity");

    for (int attempt = 0; attempt < 100 &&
         !std::filesystem::exists(process.summary_path); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    expect(std::filesystem::exists(process.csv_path),
           "spawned helper creates its CSV");
    expect(std::filesystem::exists(process.summary_path),
           "spawned helper publishes a running summary");
    expect(
        orange::monitoring::StopNicThermalMonitorProcess(&process, &error),
        "parent stops and reaps the helper: " + error);
    expect(!process.active && process.status == "completed",
           "normal SIGTERM produces a clean terminal process state");
    expect(read_json(process.summary_path).value("status", "") == "completed",
           "helper publishes a completed terminal summary");

    unsetenv("ORANGE_NIC_THERMAL_MONITOR_BIN");
    unsetenv("ORANGE_NIC_THERMAL_MONITOR_ENABLED");
    unsetenv("ORANGE_NIC_THERMAL_SAMPLE_SECONDS");
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

}  // namespace

int main(int argc, char** argv)
{
    (void)argc;
    if (argv && argv[0]) {
        binary_directory = std::filesystem::absolute(argv[0]).parent_path();
    }
    test_valid_and_unavailable_samples();
    test_missing_mlx5_is_observable();
    test_parent_process_lifecycle();
    if (failures != 0) {
        std::cerr << failures << " NIC thermal monitor test(s) failed\n";
        return 1;
    }
    std::cout << "NIC thermal monitor tests passed\n";
    return 0;
}
