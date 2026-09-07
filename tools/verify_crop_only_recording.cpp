// Read-only adapter for runner validation. No camera, recording or repair path.
#include "recording_crop_only_manifest.h"
#include "scoped_housekeeping_cpu.h"
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    nlohmann::json report = {{"schema_id", "orange.recording.crop_only_validation"},
        {"schema_version", 1}, {"status", "fail"}};
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") {
            std::cout << "Usage: verify_crop_only_recording <exact-recording-folder> --worker-cpu <cpu>\n"
                "Read-only master/context/encoded-media/clip-index verification; JSON to stdout.\n";
            return 0;
        }
        if (argc != 4 || std::string(argv[2]) != "--worker-cpu")
            throw std::runtime_error("expected recording folder and --worker-cpu <cpu>");
        const std::string value(argv[3]);
        if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
            throw std::runtime_error("worker CPU must be a nonnegative integer");
        orange::ScopedHousekeepingCpu affinity(std::stoi(value));
        const auto root = std::filesystem::canonical(argv[1]);
        report["recording_folder"] = root.string();
        report["recording_session"] = orange::recording::ReadVerifiedCropOnlyRecordingManifest(root);
        report["status"] = "pass";
    } catch (const std::exception& ex) { report["error"] = ex.what(); }
    std::cout << report.dump() << '\n';
    return report.at("status") == "pass" ? 0 : 1;
}
