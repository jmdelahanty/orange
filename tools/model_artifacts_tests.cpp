// Unit tests for src/session/model_artifacts: copies the engine manifest, the Palette canonical
// manifest and (pose) the input contract it binds, and for INT8 engines the calibration record
// and cache into models/<sha>/ and declares them with digests and the digests they should have;
// one entry per distinct engine; the declaration matches orange.recording_model_artifacts v2. The snapshot blocks and engine
// manifests start from real rig fixtures (tools/fixtures/generated_outputs), since the reader
// parses both with the generated strict readers of their published schemas.
//
//   model_artifacts_tests <recording_snapshot_models_excerpt.json> <INT8 detect manifest> <pose manifest>
#include "session/model_artifacts.h"
#include "gui/spatial_layout/sha256.h"
#include "orange_outputs/orange_recording_model_artifacts_v2.h"

#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
namespace { int failures = 0; void require(bool ok, const std::string& w) { if (!ok) { ++failures; std::cerr << "FAIL: " << w << std::endl; } }
void put(const fs::path& p, const std::string& s) { fs::create_directories(p.parent_path()); std::ofstream o(p, std::ios::binary); o << s; }
nlohmann::json read_json(const char* path) { std::ifstream in(path); return nlohmann::json::parse(in); }
nlohmann::json patched(nlohmann::json base, const nlohmann::json& patch) { base.merge_patch(patch); return base; }
std::string sha_of(const std::string& s) { return orange::gui::spatial_layout::checksum::sha256_hex(s); } }

int main(int argc, char** argv)
{
    if (argc != 4) {
        std::cerr << "usage: model_artifacts_tests <models excerpt> <detect manifest> <pose manifest>" << std::endl;
        return 2;
    }
    const nlohmann::json excerpt = read_json(argv[1]).at("models").begin().value();
    const fs::path root = fs::temp_directory_path() / "orange_model_artifacts_tests";
    fs::remove_all(root); fs::create_directories(root / "run");
    const fs::path cache = root / "calib" / "int8.cache"; put(cache, "CACHEBYTES");
    const fs::path record = root / "calib" / "int8.cache.json"; put(record, "{\"frames_used\":[\"a\"]}");
    const fs::path manifest = root / "engines" / "det.manifest.json";
    nlohmann::json det_manifest = read_json(argv[2]);
    det_manifest["build"]["int8_calibration"]["cache_path"] = cache.string();
    det_manifest["build"]["int8_calibration"]["record_path"] = record.string();
    det_manifest["build"]["int8_calibration"]["cache_sha256"] = sha_of("CACHEBYTES");
    det_manifest["build"]["int8_calibration"].erase("record_sha256");
    const fs::path det_canonical = root / "sources" / "det.canonical.manifest.json"; put(det_canonical, "{\"det\":1}");
    det_manifest["source"]["canonical_manifest"] = {{"path", det_canonical.string()}, {"sha256", sha_of("{\"det\":1}")}};
    put(manifest, det_manifest.dump());
    // The pose engine binds a canonical manifest and an input contract; the contract on disk no
    // longer has the bytes the engine manifest recorded.
    const fs::path pose_manifest = root / "engines" / "pose.manifest.json";
    nlohmann::json pose_json = read_json(argv[3]);
    const fs::path pose_canonical = root / "sources" / "pose.canonical.manifest.json"; put(pose_canonical, "{\"pose\":1}");
    const fs::path pose_contract = root / "sources" / "pose_model_input_contract.json"; put(pose_contract, "{\"changed\":1}");
    pose_json["source"]["canonical_manifest"] = {{"path", pose_canonical.string()}, {"sha256", sha_of("{\"pose\":1}")}};
    pose_json["source"]["input_contract"]["path"] = pose_contract.string();
    pose_json["source"]["input_contract"]["sha256"] = sha_of("{\"original\":1}");
    put(pose_manifest, pose_json.dump());
    std::string det_manifest_sha, pose_manifest_sha, err0;
    orange::gui::spatial_layout::checksum::file_sha256(manifest, &det_manifest_sha, &err0);
    orange::gui::spatial_layout::checksum::file_sha256(pose_manifest, &pose_manifest_sha, &err0);
    det_manifest_sha = det_manifest_sha.substr(7); pose_manifest_sha = pose_manifest_sha.substr(7);
    const std::string det_sha(64, 'a'), pose_sha(64, 'b');
    nlohmann::json models;
    for (const char* serial : {"2010093", "2010094"}) {
        models[serial] = {
            {"detect", patched(excerpt.at("detect"), {{"enabled", true}, {"runtime", {{"engine_sha256", det_sha}, {"model_id", "det"}, {"engine_path", "/e/det.engine"}, {"engine_bytes", 10},
                {"engine_manifest", {{"present", true}, {"path", manifest.string()}, {"sha256", det_manifest_sha}, {"precision", "int8"}, {"build_id", "b"}, {"run_id", "r"}}}}}})},
            {"pose", patched(excerpt.at("pose"), {{"enabled", true}, {"runtime", {{"engine_sha256", pose_sha}, {"model_id", "pose"}, {"engine_path", "/e/pose.engine"},
                {"engine_manifest", {{"present", true}, {"path", pose_manifest.string()}, {"sha256", pose_manifest_sha}, {"precision", "fp16"}}}}}})}};
    }
    models["2010095"] = {{"detect", patched(excerpt.at("detect"), {{"enabled", false}, {"runtime", {{"engine_sha256", det_sha}}}})},
                         {"pose", patched(excerpt.at("pose"), {{"enabled", true}, {"runtime", {{"engine_sha256", std::string(64, 'c')}, {"engine_manifest", {{"present", false}, {"path", "/missing.manifest.json"}}}}}})}};
    // A block that does not match its published schema is reported and skipped.
    models["2010096"] = {{"detect", patched(excerpt.at("detect"), {{"runtime", {{"engine_sha256", std::string(64, 'd')}, {"backend", "onnxruntime"}}}})}};
    const nlohmann::json out = orange::session::materialize_model_artifacts((root / "run").string(), models);
    require(out.at("schema_id") == "orange.recording_model_artifacts" && out.at("schema_version") == 2, "header");
    {
        namespace ma = orange::outputs::gen::recording_model_artifacts_v2;
        ma::OrangeRecordingModelArtifactsV2 parsed; std::string error;
        const bool ok = ma::parse(out, &parsed, &error);
        require(ok, "matches orange.recording_model_artifacts v2: " + error);
        nlohmann::json bad = out; bad["engines"]["not-a-sha"] = bad["engines"].begin().value();
        require(!ma::parse(bad, &parsed, &error), "an engine key that is not a sha256 is refused");
    }
    const auto& engines = out.at("engines");
    require(engines.size() == 3, "three distinct engines");
    require(!engines.contains(std::string(64, 'd')), "a block that does not match its schema declares nothing");
    const auto& det = engines.at(det_sha);
    require(det.at("models_keys").size() == 2 && det.at("task") == "detect" && det.at("precision") == "int8", "detect entry shared by two cameras");
    require(det.at("files").size() == 4 && det.at("status") == "present", "manifest + canonical manifest + record + cache copied");
    std::vector<std::string> roles; for (const auto& f : det.at("files")) roles.push_back(f.at("role"));
    require(roles == std::vector<std::string>{"engine_manifest", "canonical_onnx_manifest", "int8_calibration_record", "int8_calibration_cache"}, "detect roles");
    require(det.at("files").at(0).at("expected_sha256") == "sha256:" + det_manifest_sha, "engine manifest expected digest from the snapshot");
    require(det.at("files").at(2).at("expected_sha256").is_null(), "no recorded digest: expected null");
    for (const auto& f : det.at("files")) {
        const fs::path p = root / "run" / f.at("path").get<std::string>();
        require(fs::exists(p), "copied file exists: " + p.string());
        std::string chk, err; orange::gui::spatial_layout::checksum::file_sha256(p, &chk, &err);
        require(chk == f.at("sha256").get<std::string>() && f.at("size_bytes") == fs::file_size(p), "digest token and size declared");
        require(f.at("path").get<std::string>().rfind("models/" + det_sha + "/", 0) == 0, "path under models/<sha>/");
    }
    const auto& pose = engines.at(pose_sha);
    require(pose.at("files").size() == 3 && pose.at("status") == "partial", "fp16 pose: manifest, canonical manifest, input contract");
    require(pose.at("files").at(1).at("role") == "canonical_onnx_manifest" && pose.at("files").at(1).at("status") == "present",
            "pose canonical manifest copied");
    const auto& contract = pose.at("files").at(2);
    require(contract.at("role") == "pose_model_input_contract" && contract.at("status") == "digest_mismatch"
            && contract.at("sha256") == "sha256:" + sha_of("{\"changed\":1}")
            && contract.at("expected_sha256") == "sha256:" + sha_of("{\"original\":1}")
            && fs::exists(root / "run" / contract.at("path").get<std::string>()),
            "a changed input contract is copied and declared digest_mismatch");
    const auto& missing = engines.at(std::string(64, 'c'));
    require(missing.at("status") == "absent" && missing.at("files").at(0).at("status") == "absent"
            && missing.at("files").at(0).at("sha256").is_null(), "missing manifest declared absent, no digest");
    require(!fs::exists(root / "run" / "models" / std::string(64, 'c') / "engine.manifest.json"), "nothing copied for a missing manifest");
    fs::remove_all(root);
    if (failures) { std::cerr << failures << " model artifacts test(s) failed" << std::endl; return 1; }
    std::cout << "All model artifacts tests passed." << std::endl; return 0;
}
