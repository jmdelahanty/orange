// Generated output code (tools/schema_codegen) against real Orange output.
//
// Golden round trip: every fixture line (real lines from the 2026-10-11 gate recording, frame
// fields under their v3 names) is parsed by the generated strict reader and written back by the
// generated writer; the bytes must be identical, so a writer switched to the generated code
// produces exactly today's output. Also checks that the reader refuses malformed lines.
//
// Engine manifests (orange.tensorrt_engine_manifest v1, written by the Python build scripts) are
// compared as JSON values, not bytes: Python formats them, not the generated writer.
//
//   generated_outputs_tests <fixture dir> [<extra yolo.jsonl> <extra pose.jsonl>]
//   generated_outputs_tests --engine-manifests <manifest.json>...
#include "orange_outputs/orange_pose_event_v3.h"
#include "orange_outputs/orange_recording_detect_model_v1.h"
#include "orange_outputs/orange_recording_pose_model_v2.h"
#include "orange_outputs/orange_tensorrt_engine_manifest_v1.h"
#include "orange_outputs/orange_yolo_event_v3.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace yolo = orange::outputs::gen::yolo_event_v3;
namespace pose = orange::outputs::gen::pose_event_v3;
namespace manifest = orange::outputs::gen::tensorrt_engine_manifest_v1;
namespace detect_model = orange::outputs::gen::recording_detect_model_v1;
namespace pose_model = orange::outputs::gen::recording_pose_model_v2;

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

// parse -> to_json gives back the same JSON value (every key, every value, open objects kept).
bool manifest_round_trip(const std::string& path) {
    std::ifstream in(path);
    const nlohmann::json original = nlohmann::json::parse(in, nullptr, false);
    manifest::OrangeTensorrtEngineManifestV1 value;
    std::string error;
    if (original.is_discarded() || !manifest::parse(original, &value, &error)) {
        std::cout << path << ": parse failed: " << error << "\n";
        return false;
    }
    const nlohmann::json written = manifest::to_json(value);
    if (written != original) {
        std::cout << path << ": round trip differs: " << nlohmann::json::diff(original, written).dump().substr(0, 400) << "\n";
        return false;
    }
    return true;
}

void engine_manifests(const std::string& dir) {
    std::size_t files = 0, bad = 0;
    std::string sample;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() != ".json") continue;
        ++files;
        bad += manifest_round_trip(entry.path().string()) ? 0 : 1;
        if (sample.empty() || entry.path().string().find("int8") != std::string::npos) sample = entry.path().string();
    }
    check(files >= 3 && bad == 0, "engine manifests: " + std::to_string(files) + " rig manifests round-trip as JSON values");

    std::ifstream in(sample);
    const nlohmann::json good = nlohmann::json::parse(in);
    auto refused = [&](nlohmann::json j, const std::string& what, const std::string& expect) {
        manifest::OrangeTensorrtEngineManifestV1 v;
        std::string error;
        const bool ok = manifest::parse(j, &v, &error);
        check(!ok && error.find(expect) != std::string::npos, "manifest refuses " + what + " (" + error + ")");
    };
    { auto j = good; j["schema_id"] = "orange.other"; refused(j, "another schema_id", "must be"); }
    { auto j = good; j.erase("engine"); refused(j, "a missing engine block", "missing"); }
    { auto j = good; j["engine"]["sha256"] = 7; refused(j, "a non-string digest", "expected a string"); }
    { auto j = good; j["build"]["command"] = 3; refused(j, "a union value of another type", "expected"); }
    { auto j = good; j["keypoints"] = nullptr;
      manifest::OrangeTensorrtEngineManifestV1 v; std::string e;
      const bool ok = manifest::parse(j, &v, &e);
      check(ok && v.keypoints && !*v.keypoints && manifest::to_json(v).at("keypoints").is_null(),
            "an optional nullable field keeps an explicit null apart from absence"); }
    { auto j = good; j.erase("keypoints");
      manifest::OrangeTensorrtEngineManifestV1 v; std::string e;
      const bool ok = manifest::parse(j, &v, &e);
      check(ok && !v.keypoints && !manifest::to_json(v).contains("keypoints"),
            "an absent optional nullable field stays absent"); }
}
// A real snapshot models.<serial> block (detect + pose; an excerpt of a 2026-10-11 gate
// recording's recording_snapshot_start.json) through the generated readers and writers.
void snapshot_models(const std::string& path) {
    std::ifstream in(path);
    const nlohmann::json fixture = nlohmann::json::parse(in);
    const nlohmann::json& camera = fixture.at("models").begin().value();
    const nlohmann::json& detect = camera.at("detect");
    const nlohmann::json& pose = camera.at("pose");
    detect_model::OrangeRecordingDetectModelV1 d;
    pose_model::OrangeRecordingPoseModelV2 p;
    std::string error;
    check(detect_model::parse(detect, &d, &error) && detect_model::to_json(d) == detect,
          "snapshot detect block round-trips " + error);
    check(pose_model::parse(pose, &p, &error) && pose_model::to_json(p) == pose,
          "snapshot pose block round-trips " + error);
    // allOf[0]: an enabled detector is TensorRT with an engine path ("none" is in the backend enum).
    { auto j = detect; j["runtime"]["backend"] = "none";
      detect_model::OrangeRecordingDetectModelV1 v; std::string e;
      const bool ok = detect_model::parse(j, &v, &e);
      check(!ok && e.find("/runtime/backend: must be \"tensorrt\" (allOf[0])") != std::string::npos,
            "an enabled detector must be TensorRT (" + e + ")"); }
    { auto j = detect; j["runtime"]["engine_path"] = "";
      detect_model::OrangeRecordingDetectModelV1 v; std::string e;
      const bool ok = detect_model::parse(j, &v, &e);
      check(!ok && e.find("/runtime/engine_path: must be at least 1 characters (allOf[0])") != std::string::npos,
            "an enabled detector names its engine (" + e + ")"); }
    { auto j = detect; j["enabled"] = false; j["runtime"]["backend"] = "none";
      detect_model::OrangeRecordingDetectModelV1 v; std::string e;
      const bool ok = detect_model::parse(j, &v, &e);
      check(ok, "the TensorRT rule applies only when enabled (" + e + ")"); }
}
}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "--engine-manifests") {
        std::size_t bad = 0;
        for (int i = 2; i < argc; ++i) bad += manifest_round_trip(argv[i]) ? 0 : 1;
        std::cout << (argc - 2) << " engine manifests, " << bad << " failed\n";
        return bad ? 1 : 0;
    }
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

    engine_manifests(dir + "/engine_manifests");
    snapshot_models(dir + "/recording_snapshot_models_excerpt.json");

    std::cout << (failures ? "generated_outputs_tests FAILED\n" : "generated_outputs_tests passed\n");
    return failures ? 1 : 0;
}
