#include "session/model_artifacts.h"
#include "session/snapshot_models.h"
#include "orange_outputs/orange_recording_model_artifacts_v2.h"
#include "orange_outputs/orange_tensorrt_engine_manifest_v1.h"
#include "outputs/common/atomic_file.h"

#include "fsuid_guard.h"
#include "gui/spatial_layout/sha256.h"

#include <filesystem>
#include <iostream>
#include <fstream>
#include <optional>
#include <system_error>

namespace orange::session {

namespace fs = std::filesystem;
using orange::gui::spatial_layout::checksum::sha256_hex;

namespace {

namespace em = orange::outputs::gen::tensorrt_engine_manifest_v1;
namespace ma = orange::outputs::gen::recording_model_artifacts_v2;

bool read_bytes(const fs::path& path, std::string* out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return static_cast<bool>(in) || in.eof();
}

// A recorded bare-hex digest as a digest_form_v1 sha256: token; nullopt when none is recorded.
std::optional<std::string> digest_token(const std::string& hex)
{
    return hex.empty() ? std::nullopt : std::optional<std::string>("sha256:" + hex);
}

// Copy `source` to `<folder>/<relative>` (temp + rename) and describe it; a copy whose digest is
// not `expected_sha256` (when one is recorded) is kept and marked digest_mismatch.
ma::OrangeRecordingModelArtifactsV2File copy_declared(const fs::path& folder, const std::string& role,
                                                      const std::string& source, const std::string& relative,
                                                      const std::string& expected_sha256)
{
    ma::OrangeRecordingModelArtifactsV2File file;
    file.role = role;
    file.source_path = source;
    file.path = relative;
    file.expected_sha256 = digest_token(expected_sha256);
    file.status = "absent";
    std::string bytes;
    if (source.empty() || !read_bytes(source, &bytes)) {
        return file;
    }
    const fs::path destination = folder / relative;
    orange::ScopedFsuid fsuid_guard;  // models/ belongs to the sudo invoker
    if (!orange::outputs::write_file_atomic(destination, bytes)) {
        file.status = "copy_failed";
        return file;
    }
    file.size_bytes = bytes.size();
    file.sha256 = digest_token(sha256_hex(bytes));
    file.status = !file.expected_sha256 || file.sha256 == file.expected_sha256 ? "present" : "digest_mismatch";
    return file;
}

// The engine manifest at `path`, through the generated reader of its published schema.
std::optional<em::OrangeTensorrtEngineManifestV1> read_engine_manifest(const std::string& path,
                                                                       std::vector<std::string>* errors)
{
    std::string bytes;
    if (!read_bytes(path, &bytes)) return std::nullopt;
    const nlohmann::json json = nlohmann::json::parse(bytes, nullptr, false);
    em::OrangeTensorrtEngineManifestV1 manifest;
    std::string error;
    if (!em::parse(json, &manifest, &error)) {
        errors->push_back(path + ": does not match orange.tensorrt_engine_manifest v1: " + error);
        return std::nullopt;
    }
    return manifest;
}

}  // namespace

nlohmann::json materialize_model_artifacts(const std::string& recording_folder,
                                           const nlohmann::json& models)
{
    ma::OrangeRecordingModelArtifactsV2 out;
    if (recording_folder.empty() || !models.is_object()) {
        return ma::to_json(out);
    }
    const fs::path folder(recording_folder);
    // The snapshot blocks and the engine manifests through the generated parsers of their
    // published schemas; a block or manifest that does not match is reported and skipped.
    std::vector<std::string> errors;
    const SnapshotModels parsed = parse_snapshot_models(models, &errors);
    auto add = [&](const std::string& serial, const char* kind, const auto& block) {
        if (!block.enabled) return;
        const auto& runtime = block.runtime;
        const std::string& engine_sha = runtime.engine_sha256;
        if (engine_sha.empty()) return;
        const std::string models_key = "models." + serial + "." + kind;
        auto found = out.engines.find(engine_sha);
        if (found != out.engines.end()) {
            found->second.models_keys.push_back(models_key);
            return;
        }
        const auto& manifest_block = runtime.engine_manifest;
        const std::string rel_dir = "models/" + engine_sha + "/";
        ma::OrangeRecordingModelArtifactsV2Engine entry;
        entry.engine_sha256 = engine_sha;
        entry.task = kind;
        entry.model_id = runtime.model_id;
        entry.engine_path = runtime.engine_path;
        entry.engine_bytes = runtime.engine_bytes;
        entry.precision = manifest_block ? manifest_block->precision : std::string();
        entry.build_id = manifest_block ? manifest_block->build_id : std::string();
        entry.run_id = manifest_block ? manifest_block->run_id : std::string();
        entry.models_keys.push_back(models_key);
        const bool manifest_present = manifest_block && manifest_block->present;
        const std::string manifest_path = manifest_present ? manifest_block->path : std::string();
        entry.files.push_back(copy_declared(folder, "engine_manifest", manifest_path,
                                            rel_dir + "engine.manifest.json",
                                            manifest_present ? manifest_block->sha256 : std::string()));
        // The files the engine manifest names: the Palette canonical ONNX manifest, a pose
        // engine's input contract, and an INT8 engine's calibration record and trtexec cache.
        const auto manifest = manifest_present ? read_engine_manifest(manifest_path, &errors) : std::nullopt;
        if (manifest) {
            const auto& source = manifest->source;
            if (source.canonical_manifest && source.canonical_manifest->path) {
                entry.files.push_back(copy_declared(folder, "canonical_onnx_manifest", *source.canonical_manifest->path,
                                                    rel_dir + "canonical.manifest.json",
                                                    source.canonical_manifest->sha256.value_or(std::string())));
            }
            if (source.input_contract && source.input_contract->path) {
                entry.files.push_back(copy_declared(folder, "pose_model_input_contract", *source.input_contract->path,
                                                    rel_dir + "pose_model_input_contract.json",
                                                    source.input_contract->sha256.value_or(std::string())));
            }
            if (manifest->build.int8_calibration && *manifest->build.int8_calibration) {
                const auto& calibration = **manifest->build.int8_calibration;
                entry.files.push_back(copy_declared(folder, "int8_calibration_record",
                                                    calibration.record_path.value_or(std::string()),
                                                    rel_dir + "int8_calibration.record.json",
                                                    calibration.record_sha256.value_or(std::string())));
                entry.files.push_back(copy_declared(folder, "int8_calibration_cache", calibration.cache_path,
                                                    rel_dir + "int8_calibration.cache", calibration.cache_sha256));
            }
        }
        size_t present = 0;
        for (const auto& file : entry.files) present += file.status == "present" ? 1 : 0;
        entry.status = entry.files.front().status == "absent" ? "absent"
                     : (present == entry.files.size() ? "present" : "partial");
        out.engines.emplace(engine_sha, std::move(entry));
    };
    for (const auto& [serial, camera] : parsed) {
        if (camera.detect) add(serial, "detect", *camera.detect);
        if (camera.pose) add(serial, "pose", *camera.pose);
    }
    for (const std::string& error : errors) {
        std::cerr << "[model_artifacts] " << error << std::endl;
    }
    return ma::to_json(out);
}

}  // namespace orange::session
