// Unit tests for src/model_identity: engine hashing, manifest reading and the
// snapshot JSON block (realtime-products item 2).
#include "model_identity.h"
#include "pose_skeleton_sidecar.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using orange::model_identity::ModelIdentity;

namespace {

int failures = 0;

void require(bool ok, const std::string& what)
{
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << std::endl;
    }
}

fs::path make_temp_dir()
{
    const fs::path base = fs::temp_directory_path() / "orange_model_identity_tests";
    fs::remove_all(base);
    fs::create_directories(base);
    return base;
}

void write_file(const fs::path& path, const std::string& bytes)
{
    std::ofstream out(path, std::ios::binary);
    out << bytes;
}

void test_empty_path()
{
    const ModelIdentity id = orange::model_identity::resolve_model_identity("");
    require(!id.engine_present && id.engine_sha256.empty() && !id.manifest_present, "empty path -> empty identity");
    require(orange::model_identity::engine_manifest_json(id).is_null(), "empty path -> null manifest json");
}

void test_missing_engine_and_manifest(const fs::path& dir)
{
    const std::string engine = (dir / "missing.engine").string();
    const ModelIdentity id = orange::model_identity::resolve_model_identity_uncached(engine);
    require(!id.engine_present && id.engine_sha256.empty(), "missing engine -> no hash");
    require(!id.manifest_present && id.manifest_error.empty(), "missing manifest -> present false, no error");
    const nlohmann::json j = orange::model_identity::engine_manifest_json(id);
    require(j.is_object() && j.at("present") == false && j.at("path") == (dir / "missing.manifest.json").string(),
            "missing manifest json block");
}

void test_engine_with_manifest(const fs::path& dir)
{
    const fs::path engine = dir / "detect.engine";
    const std::string bytes = "tensorrt-engine-bytes";
    write_file(engine, bytes);
    const std::string expected_sha = orange::pose::file_sha256_hex(engine.string());
    nlohmann::json manifest = {
        {"schema_id", "orange.tensorrt_engine_manifest"},
        {"schema_version", 1},
        {"status", "candidate"},
        {"task", "pose"},
        {"palette_run_id", "run-1"},
        {"set_id", "set-1"},
        {"build_id", "a16_gpu5_trt100_fp16"},
        {"precision", "fp16"},
        {"target_hardware_class", "A16"},
        {"created_at_utc", "2026-09-16T22:01:27Z"},
        {"engine", {{"path", engine.string()}, {"sha256", expected_sha}, {"bytes", bytes.size()}}},
        {"source", {
            {"onnx", {{"path", "/m/source.onnx"}, {"sha256", std::string(64, 'a')}}},
            {"weights", {{"source_path", "/m/best.pt"}, {"sha256", std::string(64, 'b')}}},
            {"canonical_manifest", {{"path", "/m/c.canonical.manifest.json"}, {"sha256", std::string(64, 'c')}}},
            {"onnx_manifest", {{"path", "/m/source.onnx.manifest.json"}, {"sha256", std::string(64, 'd')}}}
        }}
    };
    // Palette's layout: foo.engine -> foo.manifest.json
    write_file(dir / "detect.manifest.json", manifest.dump(2));

    const ModelIdentity id = orange::model_identity::resolve_model_identity_uncached(engine.string());
    require(id.manifest_path == (dir / "detect.manifest.json").string(), "stem manifest path chosen");
    require(id.engine_present && id.engine_bytes == bytes.size(), "engine present with size");
    require(id.engine_sha256 == expected_sha && expected_sha.size() == 64, "engine sha computed");
    require(id.manifest_present && id.manifest_error.empty(), "manifest read");
    require(id.manifest_sha256.size() == 64, "manifest sha computed");
    require(id.manifest_schema_id == "orange.tensorrt_engine_manifest" && id.manifest_schema_version == 1, "schema fields");
    require(id.declared_engine_sha256 == expected_sha && id.declared_engine_sha256_matches, "declared engine sha matches");
    require(id.onnx_sha256 == std::string(64, 'a') && id.onnx_path == "/m/source.onnx", "onnx fields");
    require(id.weights_sha256 == std::string(64, 'b') && id.weights_path == "/m/best.pt", "weights fields");
    require(id.canonical_manifest_sha256 == std::string(64, 'c'), "canonical manifest sha");
    require(id.onnx_manifest_sha256 == std::string(64, 'd'), "onnx manifest sha");
    require(id.run_id == "run-1" && id.set_id == "set-1" && id.build_id == "a16_gpu5_trt100_fp16" &&
            id.precision == "fp16" && id.task == "pose" && id.manifest_status == "candidate",
            "manifest identity strings");

    const nlohmann::json j = orange::model_identity::engine_manifest_json(id);
    require(j.at("present") == true && j.at("engine_sha256_matches") == true &&
            j.at("onnx").at("sha256") == std::string(64, 'a') &&
            j.at("weights").at("sha256") == std::string(64, 'b') &&
            j.at("build_id") == "a16_gpu5_trt100_fp16" && j.at("error") == "",
            "manifest json block");

    // Cached resolution returns the same answer and survives a later edit of
    // the engine (the snapshot hashes what ran at first resolution).
    const ModelIdentity cached1 = orange::model_identity::resolve_model_identity(engine.string());
    write_file(engine, bytes + "-changed");
    const ModelIdentity cached2 = orange::model_identity::resolve_model_identity(engine.string());
    require(cached1.engine_sha256 == expected_sha && cached2.engine_sha256 == expected_sha, "cache by path");
    const ModelIdentity fresh = orange::model_identity::resolve_model_identity_uncached(engine.string());
    require(fresh.engine_sha256 != expected_sha && !fresh.declared_engine_sha256_matches,
            "edited engine no longer matches the manifest");
}

void test_suffix_manifest_fallback(const fs::path& dir)
{
    const fs::path engine = dir / "legacy.engine";
    write_file(engine, "y");
    write_file(fs::path(engine.string() + ".manifest.json"),
               R"({"schema_id":"orange.tensorrt_engine_manifest","schema_version":1,"build_id":"b"})");
    const ModelIdentity id = orange::model_identity::resolve_model_identity_uncached(engine.string());
    require(id.manifest_present && id.manifest_path == engine.string() + ".manifest.json" && id.build_id == "b",
            "<engine>.manifest.json accepted when no stem manifest exists");
}

void test_bad_manifests(const fs::path& dir)
{
    const fs::path engine = dir / "bad.engine";
    write_file(engine, "x");
    write_file(dir / "bad.manifest.json", "{not json");
    ModelIdentity id = orange::model_identity::resolve_model_identity_uncached(engine.string());
    require(id.manifest_present && id.manifest_error.rfind("parse:", 0) == 0, "parse error reported");
    require(id.engine_sha256.size() == 64, "engine still hashed with a bad manifest");

    write_file(dir / "bad.manifest.json", R"({"schema_id":"other","schema_version":1})");
    id = orange::model_identity::resolve_model_identity_uncached(engine.string());
    require(id.manifest_present && id.manifest_error.find("unexpected schema_id") != std::string::npos,
            "wrong schema id reported");
    require(id.onnx_sha256.empty() && id.weights_sha256.empty(), "wrong schema -> no identity fields");
}

}  // namespace

int main()
{
    const fs::path dir = make_temp_dir();
    test_empty_path();
    test_missing_engine_and_manifest(dir);
    test_engine_with_manifest(dir);
    test_suffix_manifest_fallback(dir);
    test_bad_manifests(dir);
    fs::remove_all(dir);
    if (failures) {
        std::cerr << failures << " model identity test(s) failed" << std::endl;
        return 1;
    }
    std::cout << "All model identity tests passed." << std::endl;
    return 0;
}
