// Empty-background v1 fixtures (tools/fixtures/empty_background_v1, shared with Palette through
// agent-contracts orange-palette/empty_background_v1) against the generated strict readers.
//
// Everything under valid/ must parse and round-trip (parse -> to_json gives back the same JSON
// value); everything under invalid/ must be refused. capture_* files are
// orange.empty_background_capture v1 descriptors, snapshot_* files are the start-snapshot block
// session.empty_background (orange_recording_empty_background_v1). The schema examples in
// docs/schemas/examples are checked as valid documents too. tools/empty_background_schema_fixture_tests.py
// runs the same files through jsonschema.
//
//   empty_background_schema_fixture_tests <fixture dir> <schema examples dir>
#include "orange_outputs/orange_empty_background_capture_v1.h"
#include "orange_outputs/orange_recording_empty_background_v1.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace capture = orange::outputs::gen::empty_background_capture_v1;
namespace snapshot = orange::outputs::gen::recording_empty_background_v1;
namespace fs = std::filesystem;

namespace {
int failures = 0;
void check(bool ok, const std::string& what) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << what << "\n";
    failures += ok ? 0 : 1;
}

nlohmann::json read_json(const fs::path& path) {
    std::ifstream in(path);
    return nlohmann::json::parse(in);
}

// Parses with the reader the file name selects; on success, *round_trip is true when the
// generated writer gives back the same JSON value.
bool parse_document(const fs::path& path, std::string* error, bool* round_trip) {
    const nlohmann::json doc = read_json(path);
    const std::string name = path.filename().string();
    *round_trip = false;
    if (name.rfind("capture_", 0) == 0 || name.rfind("orange_empty_background_capture_v1", 0) == 0) {
        capture::OrangeEmptyBackgroundCaptureV1 value;
        if (!capture::parse(doc, &value, error)) return false;
        *round_trip = capture::to_json(value) == doc;
        return true;
    }
    if (name.rfind("snapshot_", 0) == 0 || name.rfind("orange_recording_empty_background_v1", 0) == 0) {
        snapshot::OrangeRecordingEmptyBackgroundV1 value;
        if (!snapshot::parse(doc, &value, error)) return false;
        *round_trip = snapshot::to_json(value) == doc;
        return true;
    }
    *error = "unknown fixture kind (name must start with capture_ or snapshot_)";
    return false;
}

std::vector<fs::path> json_files(const fs::path& dir) {
    std::vector<fs::path> out;
    for (const auto& entry : fs::directory_iterator(dir))
        if (entry.is_regular_file() && entry.path().extension() == ".json") out.push_back(entry.path());
    std::sort(out.begin(), out.end());
    return out;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: empty_background_schema_fixture_tests <fixture dir> <schema examples dir>\n";
        return 2;
    }
    const fs::path fixtures(argv[1]);
    const fs::path examples(argv[2]);
    std::vector<fs::path> valid = json_files(fixtures / "valid");
    for (const char* example : {"orange_empty_background_capture_v1_example.json",
                                "orange_recording_empty_background_v1_example.json"})
        valid.push_back(examples / example);
    const std::vector<fs::path> invalid = json_files(fixtures / "invalid");
    check(valid.size() >= 5 && invalid.size() >= 15, "fixture sets present (" + std::to_string(valid.size()) +
                                                          " valid, " + std::to_string(invalid.size()) + " invalid)");
    for (const auto& path : valid) {
        std::string error;
        bool round_trip = false;
        const bool ok = parse_document(path, &error, &round_trip);
        check(ok, "valid " + path.filename().string() + " parses" + (ok ? "" : ": " + error));
        if (ok) check(round_trip, "valid " + path.filename().string() + " round-trips through the generated writer");
    }
    for (const auto& path : invalid) {
        std::string error;
        bool round_trip = false;
        const bool ok = parse_document(path, &error, &round_trip);
        check(!ok, "invalid " + path.filename().string() + " refused" + (ok ? "" : " (" + error + ")"));
    }
    std::cout << (failures == 0 ? "empty_background_schema_fixture_tests: PASS\n"
                                : "empty_background_schema_fixture_tests: FAIL\n");
    return failures == 0 ? 0 : 1;
}
