#include "gui/gui_timing_sidecar_writer.h"
#include "gui/spatial_layout/sha256.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>

namespace {

int failures = 0;

void expect(const bool condition, const std::string& message)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

std::vector<std::string> lines(const std::filesystem::path& path)
{
    std::ifstream input(path);
    std::vector<std::string> result;
    std::string line;
    while (std::getline(input, line)) {
        result.push_back(line);
    }
    return result;
}

std::vector<std::string> split_csv(const std::string& line)
{
    std::vector<std::string> fields;
    std::istringstream input(line);
    std::string field;
    while (std::getline(input, field, ',')) {
        fields.push_back(field);
    }
    return fields;
}

orange::gui::GuiFrameTimingSample timing_sample(const double base)
{
    orange::gui::GuiFrameTimingSample sample;
    sample.frame_total_ms = base + 1.0;
    sample.pre_frame_maintenance_ms = base + 2.0;
    sample.imgui_new_frame_ms = base + 3.0;
    sample.orange_window_draw_ms = base + 4.0;
    sample.recording_panel_draw_ms = base + 5.0;
    sample.camera_properties_draw_ms = base + 6.0;
    sample.main_texture_upload_ms = base + 7.0;
    sample.crop_texture_upload_ms = base + 8.0;
    sample.camera_window_draw_ms = base + 9.0;
    sample.crop_window_draw_ms = base + 10.0;
    sample.speed_graph_draw_ms = base + 11.0;
    sample.render_present_ms = base + 12.0;
    sample.main_texture_upload_count = 2;
    sample.crop_texture_upload_count = 1;
    return sample;
}

void test_two_chronological_windows(const std::filesystem::path& root)
{
    const auto started = std::chrono::steady_clock::time_point(
        std::chrono::seconds(100));
    orange::gui::GuiTimingSidecarWriter writer;
    expect(writer.Start(root, started), "writer starts");
    writer.Observe(0.02, timing_sample(0.0), true, false,
                   started + std::chrono::milliseconds(10));
    writer.Observe(0.025, timing_sample(1.0), true, true,
                   started + std::chrono::milliseconds(500));
    writer.Observe(0.05, timing_sample(2.0), false, false,
                   started + std::chrono::milliseconds(1010));
    writer.Observe(0.04, timing_sample(3.0), false, false,
                   started + std::chrono::milliseconds(1490));
    writer.StopAndDrain(started + std::chrono::milliseconds(1600));

    const nlohmann::json artifact = writer.ArtifactJson();
    expect(artifact.value("status", "") == "complete", "artifact is complete");
    expect(artifact.value("windows_offered", 0ULL) == 2, "two windows offered");
    expect(artifact.value("windows_written", 0ULL) == 2, "two windows written");
    expect(artifact.value("windows_dropped", 1ULL) == 0, "no windows dropped");
    expect(artifact.value("samples_offered", 0ULL) == 4, "four samples offered");
    expect(artifact.value("samples_written", 0ULL) == 4, "four samples written");
    expect(artifact.value("samples_dropped", 1ULL) == 0, "no samples dropped");
    expect(artifact.value("window_parity_complete", false),
           "window accounting has terminal parity");
    expect(artifact.value("sample_parity_complete", false),
           "sample accounting has terminal parity");
    expect(artifact.value("queue_high_water", 0U) <=
               artifact.value("queue_capacity", 0U),
           "queue remains bounded");
    expect(artifact.value("size_bytes", 0ULL) > 0, "file size recorded");
    expect(artifact.value("sha256", "").rfind("sha256:", 0) == 0,
           "sha256 recorded");

    const std::filesystem::path csv = root / "gui_timing_windows.csv";
    std::string observed_sha256;
    std::string checksum_error;
    expect(orange::gui::spatial_layout::checksum::file_sha256(
               csv, &observed_sha256, &checksum_error),
           "written sidecar can be checksummed");
    expect(observed_sha256 == artifact.value("sha256", ""),
           "incremental digest matches the finalized file");
    std::error_code size_error;
    expect(std::filesystem::file_size(csv, size_error) ==
               artifact.value("size_bytes", 0ULL) && !size_error,
           "incremental byte count matches the finalized file");
    const std::vector<std::string> rows = lines(csv);
    expect(rows.size() == 3, "CSV has header and two data rows");
    if (rows.size() == 3) {
        const std::vector<std::string> header = split_csv(rows[0]);
        const std::vector<std::string> first = split_csv(rows[1]);
        const std::vector<std::string> second = split_csv(rows[2]);
        expect(header.size() == first.size() && first.size() == second.size(),
               "every CSV row matches the schema width");
        expect(first.size() > 12, "CSV includes timing aggregates");
        if (first.size() > 8 && second.size() > 8) {
            expect(first[0] == "orange.gui_timing_windows", "schema ID is explicit");
            expect(first[1] == "1", "schema version is explicit");
            expect(first[2] == "0" && second[2] == "1",
                   "window indices preserve chronology");
            expect(first[7] == "2" && second[7] == "2",
                   "per-window GUI frame counts are exact");
        }
    }
}

void test_open_failure_is_fail_open(const std::filesystem::path& root)
{
    std::error_code directory_error;
    std::filesystem::create_directories(root, directory_error);
    expect(!directory_error, "failure-test directory created");
    const std::filesystem::path not_a_directory = root / "ordinary_file";
    {
        std::ofstream output(not_a_directory);
        output << "not a directory\n";
    }
    orange::gui::GuiTimingSidecarWriter writer;
    const auto started = std::chrono::steady_clock::now();
    (void)writer.Start(not_a_directory, started);
    writer.Observe(0.02, timing_sample(0.0), false, false, started);
    writer.StopAndDrain(started + std::chrono::milliseconds(100));
    const nlohmann::json artifact = writer.ArtifactJson();
    expect(artifact.value("status", "") == "failed",
           "open failure is preserved as diagnostic evidence");
    expect(!artifact.value("writer_error", "").empty(),
           "open failure includes an error");
}

}  // namespace

int main()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orange_gui_timing_sidecar_tests_" + std::to_string(getpid()));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    expect(!error, "temporary test root created");

    test_two_chronological_windows(root / "success");
    test_open_failure_is_fail_open(root / "failure");

    std::filesystem::remove_all(root, error);
    if (failures != 0) {
        std::cerr << failures << " GUI timing sidecar test(s) failed\n";
        return 1;
    }
    std::cout << "GUI timing sidecar tests passed\n";
    return 0;
}
