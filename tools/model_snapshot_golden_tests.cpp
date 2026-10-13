// Golden bytes for the recording snapshot's models.<serial>.detect / .pose blocks
// (build_gui_detect_model_snapshot / build_gui_pose_model_snapshot, used by the GUI, the
// headless client and the synthetic bundle).
//
// Builds the blocks across their branches (detect on/off, selected/default engine, missing
// engine; pose real/noop/off, matching/mismatching/invalid skeleton sidecar, engine without a
// manifest) under a fixed directory and compares every block's bytes with
// tools/fixtures/generated_outputs/golden/model_snapshot_blocks.tsv, produced by the
// hand-written builders before they switched to the generated
// orange.recording_detect_model.v1 / orange.recording_pose_model.v2 code (2026-10-12).
//
//   model_snapshot_golden_tests <golden file> <detect manifest fixture> <pose manifest fixture> [--write]
#include "camera.h"
#include "gui/recording_snapshots.h"
#include "NvEncoder/Logger.h"
#include "json.hpp"
#include "pose_skeleton_sidecar.h"
#include "video_capture.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

// The linked Orange client sources expect the process-wide logger.
simplelogger::Logger* logger = simplelogger::LoggerFactory::CreateConsoleLogger();

namespace {
const fs::path kRoot = "/tmp/orange_model_snapshot_golden";

void put(const fs::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary);
    out << bytes;
}

nlohmann::json read_json(const std::string& path) {
    std::ifstream in(path);
    return nlohmann::json::parse(in);
}

// An engine file and its manifest: the real rig manifest with this engine's path and digest.
void engine_with_manifest(const fs::path& engine, const std::string& bytes, nlohmann::json manifest,
                          const std::string& onnx_sha256) {
    put(engine, bytes);
    manifest["engine"]["path"] = engine.string();
    manifest["engine"]["sha256"] = orange::pose::file_sha256_hex(engine.string());
    manifest["engine"]["bytes"] = bytes.size();
    manifest["source"]["onnx"]["sha256"] = onnx_sha256;
    put(engine.parent_path() / (engine.stem().string() + ".manifest.json"), manifest.dump(2));
}

std::string sidecar(const std::string& onnx_sha256) {
    return R"({"schema_id":"palette.pose_model_skeleton","schema_version":1,"skeleton_id":"pose_schema:traditional_v1",)"
           R"("nodes":[{"id":1,"name":"eye_left"},{"id":0,"name":"swim_bladder"},{"id":2,"name":"eye_right"}],)"
           R"("edges":[[0,1],[0,2],[1,2]],"kpt_shape":[3,3],"source":{"run_id":"run_x","onnx_sha256":")" +
           onnx_sha256 + R"("}})";
}

void pose_env(const char* engine, const char* skeleton_path, const char* mode, const char* crop) {
    auto set = [](const char* name, const char* value) {
        if (value) setenv(name, value, 1); else unsetenv(name);
    };
    set("ORANGE_POSE_ENGINE_PATH", engine);
    set("ORANGE_POSE_SKELETON_ID", nullptr);
    set("ORANGE_POSE_SKELETON_PATH", skeleton_path);
    set("ORANGE_POSE_MODE", mode);
    set("ORANGE_POSE_CROP_SIZE_PX", crop);
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 4 && argc != 5) {
        std::cerr << "usage: model_snapshot_golden_tests <golden> <detect manifest> <pose manifest> [--write]\n";
        return 2;
    }
    fs::remove_all(kRoot);
    fs::create_directories(kRoot);
    const std::string onnx = std::string(64, 'a');
    engine_with_manifest(kRoot / "detect.engine", "detect-engine-bytes", read_json(argv[2]), onnx);
    engine_with_manifest(kRoot / "pose.engine", "pose-engine-bytes", read_json(argv[3]), onnx);
    put(kRoot / "bare_pose.engine", "pose-engine-without-manifest");
    put(kRoot / "sidecar_match.json", sidecar(onnx));
    put(kRoot / "sidecar_other.json", sidecar(std::string(64, 'b')));
    put(kRoot / "sidecar_invalid.json", R"({"schema_id":"other"})");

    CameraParams params{};
    params.camera_serial = "2010093";
    params.camera_id = 0;
    params.gpu_id = 5;
    params.config_path = "/fixture/config/Cam2010093.json";
    const std::string detect_engine = (kRoot / "detect.engine").string();
    const std::string pose_engine = (kRoot / "pose.engine").string();
    const std::string bare_engine = (kRoot / "bare_pose.engine").string();
    const std::string missing_engine = (kRoot / "missing.engine").string();

    std::ostringstream produced;
    auto line = [&](const std::string& name, const nlohmann::json& block) {
        produced << name << '\t' << block.dump() << '\n';
    };
    {
        CameraEachSelect select{};
        select.yolo = true;
        select.yolo_model = detect_engine.c_str();
        line("detect_selected_engine", build_gui_detect_model_snapshot(params, select, "/default.engine"));
        select.yolo_model = "";
        line("detect_default_engine", build_gui_detect_model_snapshot(params, select, detect_engine));
        select.yolo_model = missing_engine.c_str();
        line("detect_missing_engine", build_gui_detect_model_snapshot(params, select, detect_engine));
        select.yolo = false;
        line("detect_disabled", build_gui_detect_model_snapshot(params, select, detect_engine));
    }
    {
        CameraEachSelect select{};
        select.pose = true;
        const std::string match = (kRoot / "sidecar_match.json").string();
        const std::string other = (kRoot / "sidecar_other.json").string();
        const std::string invalid = (kRoot / "sidecar_invalid.json").string();
        pose_env(pose_engine.c_str(), match.c_str(), "real", "192");
        line("pose_real_sidecar_matches", build_gui_pose_model_snapshot(params, select));
        pose_env(pose_engine.c_str(), other.c_str(), nullptr, nullptr);
        line("pose_default_mode_sidecar_differs", build_gui_pose_model_snapshot(params, select));
        pose_env(pose_engine.c_str(), invalid.c_str(), "real", "0");
        line("pose_invalid_sidecar", build_gui_pose_model_snapshot(params, select));
        pose_env(bare_engine.c_str(), match.c_str(), "real", "192");
        line("pose_engine_without_manifest", build_gui_pose_model_snapshot(params, select));
        pose_env(nullptr, nullptr, "noop", nullptr);
        line("pose_noop", build_gui_pose_model_snapshot(params, select));
        select.pose = false;
        pose_env(pose_engine.c_str(), match.c_str(), "real", "192");
        line("pose_disabled", build_gui_pose_model_snapshot(params, select));
    }
    fs::remove_all(kRoot);

    if (argc == 5 && std::string(argv[4]) == "--write") {
        std::ofstream(argv[1], std::ios::binary) << produced.str();
        std::cout << "wrote " << argv[1] << "\n";
        return 0;
    }
    std::ifstream golden_in(argv[1]);
    std::stringstream golden;
    golden << golden_in.rdbuf();
    if (golden.str() != produced.str()) {
        std::cout << "[FAIL] model snapshot blocks differ from the golden file\n--- golden\n" << golden.str()
                  << "--- produced\n" << produced.str();
        return 1;
    }
    std::cout << "[PASS] model snapshot blocks equal the golden file (" << produced.str().size() << " bytes)\n";
    return 0;
}
