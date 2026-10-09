// Unit tests for src/session/model_artifacts: copies the engine manifest and,
// for INT8 engines, the calibration record and cache into models/<sha>/ and
// declares them with digests; one entry per distinct engine.
#include "session/model_artifacts.h"
#include "gui/spatial_layout/sha256.h"

#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
namespace { int failures = 0; void require(bool ok, const std::string& w) { if (!ok) { ++failures; std::cerr << "FAIL: " << w << std::endl; } }
void put(const fs::path& p, const std::string& s) { fs::create_directories(p.parent_path()); std::ofstream o(p, std::ios::binary); o << s; } }

int main()
{
    const fs::path root = fs::temp_directory_path() / "orange_model_artifacts_tests";
    fs::remove_all(root); fs::create_directories(root / "run");
    const fs::path cache = root / "calib" / "int8.cache"; put(cache, "CACHEBYTES");
    const fs::path record = root / "calib" / "int8.cache.json"; put(record, "{\"frames_used\":[\"a\"]}");
    const fs::path manifest = root / "engines" / "det.manifest.json";
    put(manifest, nlohmann::json{{"schema_id", "orange.tensorrt_engine_manifest"}, {"schema_version", 1},
        {"build", {{"int8_calibration", {{"cache_path", cache.string()}, {"record_path", record.string()}}}}}}.dump());
    const fs::path pose_manifest = root / "engines" / "pose.manifest.json";
    put(pose_manifest, nlohmann::json{{"schema_id", "orange.tensorrt_engine_manifest"}, {"schema_version", 1}, {"build", {{"tool", "trtexec"}}}}.dump());
    const std::string det_sha(64, 'a'), pose_sha(64, 'b');
    nlohmann::json models;
    for (const char* serial : {"2010093", "2010094"}) {
        models[serial] = {
            {"detect", {{"enabled", true}, {"runtime", {{"engine_sha256", det_sha}, {"model_id", "det"}, {"engine_path", "/e/det.engine"}, {"engine_bytes", 10},
                {"engine_manifest", {{"present", true}, {"path", manifest.string()}, {"precision", "int8"}, {"build_id", "b"}, {"run_id", "r"}}}}}}},
            {"pose", {{"enabled", true}, {"runtime", {{"engine_sha256", pose_sha}, {"model_id", "pose"}, {"engine_path", "/e/pose.engine"},
                {"engine_manifest", {{"present", true}, {"path", pose_manifest.string()}, {"precision", "fp16"}}}}}}}};
    }
    models["2010095"] = {{"detect", {{"enabled", false}, {"runtime", {{"engine_sha256", det_sha}}}}},
                         {"pose", {{"enabled", true}, {"runtime", {{"engine_sha256", std::string(64, 'c')}, {"engine_manifest", {{"present", false}, {"path", "/missing.manifest.json"}}}}}}}};
    const nlohmann::json out = orange::session::materialize_model_artifacts((root / "run").string(), models);
    require(out.at("schema_id") == "orange.recording_model_artifacts" && out.at("schema_version") == 1, "header");
    const auto& engines = out.at("engines");
    require(engines.size() == 3, "three distinct engines");
    const auto& det = engines.at(det_sha);
    require(det.at("models_keys").size() == 2 && det.at("task") == "detect" && det.at("precision") == "int8", "detect entry shared by two cameras");
    require(det.at("files").size() == 3 && det.at("status") == "present", "manifest + record + cache copied");
    for (const auto& f : det.at("files")) {
        const fs::path p = root / "run" / f.at("path").get<std::string>();
        require(fs::exists(p), "copied file exists: " + p.string());
        std::string chk, err; orange::gui::spatial_layout::checksum::file_sha256(p, &chk, &err);
        require(chk == "sha256:" + f.at("sha256").get<std::string>() && f.at("size_bytes") == fs::file_size(p), "digest and size declared");
        require(f.at("path").get<std::string>().rfind("models/" + det_sha + "/", 0) == 0, "path under models/<sha>/");
    }
    const auto& pose = engines.at(pose_sha);
    require(pose.at("files").size() == 1 && pose.at("status") == "present", "fp16 pose: manifest only");
    const auto& missing = engines.at(std::string(64, 'c'));
    require(missing.at("status") == "absent" && missing.at("files").at(0).at("status") == "absent", "missing manifest declared absent");
    require(!fs::exists(root / "run" / "models" / std::string(64, 'c') / "engine.manifest.json"), "nothing copied for a missing manifest");
    fs::remove_all(root);
    if (failures) { std::cerr << failures << " model artifacts test(s) failed" << std::endl; return 1; }
    std::cout << "All model artifacts tests passed." << std::endl; return 0;
}
