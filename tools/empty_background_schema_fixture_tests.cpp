// Empty-background fixtures against the generated strict readers: version 1
// (tools/fixtures/empty_background_v1, shared with Palette through agent-contracts
// orange-palette/empty_background_v1) and the version 2 draft (tools/fixtures/empty_background_v2).
//
// Everything under valid/ must parse and round-trip (parse -> to_json gives back the same JSON
// value); everything under invalid/ must be refused. capture_* files are
// orange.empty_background_capture vN descriptors, snapshot_* files are the start-snapshot block
// session.empty_background (orange_recording_empty_background_vN). The v1 schema examples in
// docs/schemas/examples are checked as valid documents too. tools/empty_background_schema_fixture_tests.py
// runs the same files through jsonschema.
//
//   empty_background_schema_fixture_tests <fixture dir> <version 1|2> [<schema examples dir>]
#include "orange_outputs/orange_empty_background_capture_v1.h"
#include "orange_outputs/orange_empty_background_capture_v2.h"
#include "orange_outputs/orange_recording_empty_background_v1.h"
#include "orange_outputs/orange_recording_empty_background_v2.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace capture = orange::outputs::gen::empty_background_capture_v1;
namespace snapshot = orange::outputs::gen::recording_empty_background_v1;
namespace capture2 = orange::outputs::gen::empty_background_capture_v2;
namespace snapshot2 = orange::outputs::gen::recording_empty_background_v2;
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
template <typename T, typename Parse, typename Write>
bool parse_with(const nlohmann::json& doc, Parse parse, Write write, std::string* error, bool* round_trip) {
    T value;
    if (!parse(doc, &value, error)) return false;
    *round_trip = write(value) == doc;
    return true;
}

bool parse_document(const fs::path& path, int version, std::string* error, bool* round_trip) {
    const nlohmann::json doc = read_json(path);
    const std::string name = path.filename().string();
    *round_trip = false;
    if (version == 2) {
        if (name.rfind("capture_", 0) == 0)
            return parse_with<capture2::OrangeEmptyBackgroundCaptureV2>(
                doc, [](const nlohmann::json& j, auto* v, std::string* e) { return capture2::parse(j, v, e); },
                [](const auto& v) { return capture2::to_json(v); }, error, round_trip);
        if (name.rfind("snapshot_", 0) == 0)
            return parse_with<snapshot2::OrangeRecordingEmptyBackgroundV2>(
                doc, [](const nlohmann::json& j, auto* v, std::string* e) { return snapshot2::parse(j, v, e); },
                [](const auto& v) { return snapshot2::to_json(v); }, error, round_trip);
        *error = "unknown fixture kind (name must start with capture_ or snapshot_)";
        return false;
    }
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
    const int version = argc >= 3 ? std::atoi(argv[2]) : 0;
    if ((version != 1 || argc != 4) && (version != 2 || argc != 3)) {
        std::cerr << "usage: empty_background_schema_fixture_tests <fixture dir> 1 <schema examples dir>\n"
                     "       empty_background_schema_fixture_tests <fixture dir> 2\n";
        return 2;
    }
    const fs::path fixtures(argv[1]);
    std::vector<fs::path> valid = json_files(fixtures / "valid");
    if (version == 1) {
        const fs::path examples(argv[3]);
        for (const char* example : {"orange_empty_background_capture_v1_example.json",
                                    "orange_recording_empty_background_v1_example.json"})
            valid.push_back(examples / example);
    }
    const std::vector<fs::path> invalid = json_files(fixtures / "invalid");
    const std::size_t min_valid = version == 1 ? 5 : 6, min_invalid = version == 1 ? 15 : 21;
    check(valid.size() >= min_valid && invalid.size() >= min_invalid, "fixture sets present (" + std::to_string(valid.size()) +
                                                          " valid, " + std::to_string(invalid.size()) + " invalid)");
    for (const auto& path : valid) {
        std::string error;
        bool round_trip = false;
        const bool ok = parse_document(path, version, &error, &round_trip);
        check(ok, "valid " + path.filename().string() + " parses" + (ok ? "" : ": " + error));
        if (ok) check(round_trip, "valid " + path.filename().string() + " round-trips through the generated writer");
    }
    for (const auto& path : invalid) {
        std::string error;
        bool round_trip = false;
        const bool ok = parse_document(path, version, &error, &round_trip);
        check(!ok, "invalid " + path.filename().string() + " refused" + (ok ? "" : " (" + error + ")"));
    }
    std::cout << (failures == 0 ? "empty_background_schema_fixture_tests: PASS\n"
                                : "empty_background_schema_fixture_tests: FAIL\n");
    return failures == 0 ? 0 : 1;
}
