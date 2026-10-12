// Generated output code (tools/schema_codegen) against real Orange output.
//
// Golden round trip: every fixture line (real lines from the 2026-10-11 gate recording, frame
// fields under their v3 names) is parsed by the generated strict reader and written back by the
// generated writer; the bytes must be identical, so a writer switched to the generated code
// produces exactly today's output. Also checks that the reader refuses malformed lines.
//
//   generated_outputs_tests <fixture dir> [<extra yolo.jsonl> <extra pose.jsonl>]
#include "orange_outputs/orange_pose_event_v3.h"
#include "orange_outputs/orange_yolo_event_v3.h"

#include <fstream>
#include <iostream>
#include <string>

namespace yolo = orange::outputs::gen::yolo_event_v3;
namespace pose = orange::outputs::gen::pose_event_v3;

namespace {
int failures = 0;
void check(bool ok, const std::string& what) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << what << "\n";
    failures += ok ? 0 : 1;
}

template <class Variant, class Parse, class Write>
void round_trip(const std::string& path, Parse parse, Write write, const std::string& label) {
    std::ifstream in(path);
    std::string line;
    std::size_t lines = 0, bad = 0;
    while (std::getline(in, line)) {
        ++lines;
        Variant value;
        std::string error;
        if (!parse(nlohmann::json::parse(line), &value, &error)) {
            if (bad++ < 3) std::cout << label << " parse failed: " << error << "\n";
            continue;
        }
        if (write(value).dump() != line && bad++ < 3) {
            std::cout << label << " bytes differ: " << line.substr(0, 200) << "\n";
        }
    }
    check(lines > 0 && bad == 0, label + ": " + std::to_string(lines) + " lines round-trip byte-exact");
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2 && argc != 4) {
        std::cerr << "usage: generated_outputs_tests <fixture dir> [<yolo.jsonl> <pose.jsonl>]\n";
        return 2;
    }
    const std::string dir = argv[1];
    auto yolo_parse = [](const nlohmann::json& j, yolo::OrangeYoloEventV3* v, std::string* e) { return yolo::parse(j, v, e); };
    auto yolo_write = [](const yolo::OrangeYoloEventV3& v) { return yolo::to_json(v); };
    auto pose_parse = [](const nlohmann::json& j, pose::OrangePoseEventV3* v, std::string* e) { return pose::parse(j, v, e); };
    auto pose_write = [](const pose::OrangePoseEventV3& v) { return pose::to_json(v); };
    round_trip<yolo::OrangeYoloEventV3>(dir + "/yolo_event_v3_lines.jsonl", yolo_parse, yolo_write, "yolo fixture");
    round_trip<pose::OrangePoseEventV3>(dir + "/pose_event_v3_lines.jsonl", pose_parse, pose_write, "pose fixture");
    if (argc == 4) {
        round_trip<yolo::OrangeYoloEventV3>(argv[2], yolo_parse, yolo_write, "yolo extra");
        round_trip<pose::OrangePoseEventV3>(argv[3], pose_parse, pose_write, "pose extra");
    }

    // The strict reader refuses what the schema refuses.
    std::ifstream in(dir + "/yolo_event_v3_lines.jsonl");
    std::string header_line, result_line, line;
    while (std::getline(in, line)) {
        const auto j = nlohmann::json::parse(line);
        if (j["event_kind"] == "session_header" && header_line.empty()) header_line = line;
        if (j["event_kind"] == "yolo_result" && result_line.empty()) result_line = line;
    }
    auto refused = [&](nlohmann::json j, const std::string& what, const std::string& expect) {
        yolo::OrangeYoloEventV3 v;
        std::string error;
        const bool ok = yolo::parse(j, &v, &error);
        check(!ok && error.find(expect) != std::string::npos, "refuses " + what + " (" + error + ")");
    };
    auto result = nlohmann::json::parse(result_line);
    { auto j = result; j["schema_version"] = 2; refused(j, "an older schema_version", "must be"); }
    { auto j = result; j["frame"]["local_frame_id"] = 1; refused(j, "an undeclared field", "unexpected field"); }
    { auto j = result; j["frame"].erase("recording_frame_id"); refused(j, "a missing required field", "missing"); }
    { auto j = result; j["frame"]["recording_frame_id"] = -1; refused(j, "a negative frame id", "non-negative"); }
    { auto j = result; j["event_kind"] = "bogus"; refused(j, "an unknown event_kind", "unknown value"); }
    { auto j = nlohmann::json::parse(header_line); j["spatial_mask"]["geometry_extra"] = 1;
      yolo::OrangeYoloEventV3 v; std::string e;
      check(yolo::parse(j, &v, &e) && yolo::to_json(v)["spatial_mask"]["geometry_extra"] == 1,
            "open object keeps undeclared keys verbatim"); }

    std::cout << (failures ? "generated_outputs_tests FAILED\n" : "generated_outputs_tests passed\n");
    return failures ? 1 : 0;
}
