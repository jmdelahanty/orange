// Tests src/timing/diagnostics_index.h: catalog matching (longest suffix),
// per-file log writer statistics, clock anchors, citrus/ and models/ skipped.
#include "timing/diagnostics_index.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {
int failures = 0;
void expect(bool ok, const std::string& what)
{
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << std::endl;
    }
}
struct Row {
    uint64_t a = 0;
};
void touch(const std::filesystem::path& p)
{
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p) << "x\n";
}
}  // namespace

int main()
{
    using namespace orange::timing;
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("orange_diag_index_tests_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);

    expect(std::string(find_declaration("Cam1_crop_meta.csv")->suffix) == "_crop_meta.csv",
           "longest suffix: crop meta");
    expect(std::string(find_declaration("Cam1_external_meta.csv")->suffix) == "_external_meta.csv",
           "longest suffix: external meta");
    expect(std::string(find_declaration("Cam1_meta.csv")->suffix) == "_meta.csv", "plain meta");
    expect(find_declaration("recording_session.json") == nullptr, "unlisted file not matched");

    auto channel = LogChannel<Row>::Create({"ring_release", 64},
                                           [](const Row& r, std::string& out) { out += std::to_string(r.a) + "\n"; });
    channel->Open((dir / "Cam7_ring_release.csv").string(), "stream_frame_id,receive_host_ns\n");
    for (uint64_t i = 0; i < 5; ++i) channel->Emit({i});
    expect(channel->Retire(std::chrono::milliseconds(5000)), "retire");
    touch(dir / "Cam7_yolo_events.jsonl");
    touch(dir / "external_recorder" / "Cam7_external_encode.csv");
    touch(dir / "citrus" / "Cam7_meta.csv");  // skipped
    touch(dir / "models" / "x_meta.csv");     // skipped

    std::string error;
    expect(write_diagnostics_index(dir.string(), &error), "write index: " + error);
    std::ifstream in(dir / "diagnostics_index.json");
    const nlohmann::json index = nlohmann::json::parse(in);
    expect(index.value("schema_id", "") == "orange.recording.diagnostics_index", "schema id");
    const auto& files = index.at("files");
    expect(files.size() == 3, "three files indexed, citrus/ and models/ skipped (got " +
                                  std::to_string(files.size()) + ")");
    bool saw_ring = false, saw_encode = false;
    for (const auto& f : files) {
        if (f.value("path", "") == "Cam7_ring_release.csv") {
            saw_ring = true;
            expect(f.at("log_writer").value("rows_written", 0) == 5, "ring rows_written 5");
            expect(f.at("log_writer").value("channel", "") == "ring_release", "ring channel name");
            expect(f.at("time_columns").at("receive_host_ns").value("time_base", "") == "host_monotonic",
                   "ring receive_host_ns is host monotonic");
        }
        if (f.value("path", "") == "external_recorder/Cam7_external_encode.csv") {
            saw_encode = true;
            expect(f.value("other_ms_columns_time_base", "") == "host_monotonic", "encode default ms base");
        }
    }
    expect(saw_ring && saw_encode, "ring and recorder encode entries present");
    const auto& anchors = index.at("clock_anchors");
    expect(anchors.size() >= 2, "first-log and index anchors present");
    expect(anchors.front().value("label", "") == "first_log_file_opened", "first anchor label");
    for (const auto& a : anchors) {
        expect(a.value("host_realtime_ns", 0ull) > 0 && a.value("host_monotonic_ns", 0ull) > 0,
               "anchor carries both clocks");
    }

    fs::remove_all(dir);
    if (failures) {
        std::cerr << failures << " diagnostics index test(s) failed" << std::endl;
        return 1;
    }
    std::cout << "timing diagnostics index tests passed" << std::endl;
    return 0;
}
