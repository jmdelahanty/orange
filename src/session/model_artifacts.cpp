#include "session/model_artifacts.h"
#include "session/snapshot_models.h"
#include "orange_outputs/orange_tensorrt_engine_manifest_v1.h"
#include "outputs/common/atomic_file.h"

#include "fsuid_guard.h"
#include "gui/spatial_layout/sha256.h"

#include <filesystem>
#include <iostream>
#include <fstream>
#include <map>
#include <system_error>

namespace orange::session {

namespace fs = std::filesystem;
using orange::gui::spatial_layout::checksum::sha256_hex;

namespace {

bool read_bytes(const fs::path& path, std::string* out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return static_cast<bool>(in) || in.eof();
}

// Copy `source` to `<folder>/<relative>` (temp + rename) and describe it.
nlohmann::json copy_declared(const fs::path& folder, const std::string& role,
                             const std::string& source, const std::string& relative)
{
    nlohmann::json file = {{"role", role}, {"source_path", source}, {"path", relative},
                           {"size_bytes", 0}, {"sha256", ""}, {"status", "absent"}};
    std::string bytes;
    if (source.empty() || !read_bytes(source, &bytes)) {
        return file;
    }
    const fs::path destination = folder / relative;
    orange::ScopedFsuid fsuid_guard;  // models/ belongs to the sudo invoker
    if (!orange::outputs::write_file_atomic(destination, bytes)) {
        file["status"] = "copy_failed";
        return file;
    }
    file["size_bytes"] = bytes.size();
    file["sha256"] = sha256_hex(bytes);
    file["status"] = "present";
    return file;
}

}  // namespace

nlohmann::json materialize_model_artifacts(const std::string& recording_folder,
                                           const nlohmann::json& models)
{
    nlohmann::json out = {
        {"schema_id", "orange.recording_model_artifacts"},
        {"schema_version", 1},
        {"root", "models"},
        {"engines", nlohmann::json::object()}
    };
    if (recording_folder.empty() || !models.is_object()) {
        return out;
    }
    const fs::path folder(recording_folder);
    // The snapshot blocks and the engine manifests through the generated parsers of their
    // published schemas; a block or manifest that does not match is reported and skipped.
    std::vector<std::string> errors;
    const SnapshotModels parsed = parse_snapshot_models(models, &errors);
    std::map<std::string, nlohmann::json> engines;
    auto add = [&](const std::string& serial, const char* kind, const auto& block) {
        if (!block.enabled) return;
        const auto& runtime = block.runtime;
        const std::string& engine_sha = runtime.engine_sha256;
        if (engine_sha.empty()) return;
        const std::string models_key = "models." + serial + "." + kind;
        auto found = engines.find(engine_sha);
        if (found != engines.end()) {
            found->second["models_keys"].push_back(models_key);
            return;
        }
        const auto& manifest_block = runtime.engine_manifest;
        const std::string rel_dir = "models/" + engine_sha;
        nlohmann::json entry = {
            {"engine_sha256", engine_sha},
            {"task", kind},
            {"model_id", runtime.model_id},
            {"engine_path", runtime.engine_path},
            {"engine_bytes", runtime.engine_bytes},
            {"precision", manifest_block ? manifest_block->precision : std::string()},
            {"build_id", manifest_block ? manifest_block->build_id : std::string()},
            {"run_id", manifest_block ? manifest_block->run_id : std::string()},
            {"models_keys", nlohmann::json::array({models_key})},
            {"files", nlohmann::json::array()}
        };
        const bool manifest_present = manifest_block && manifest_block->present;
        const std::string manifest_path = manifest_present ? manifest_block->path : std::string();
        entry["files"].push_back(copy_declared(folder, "engine_manifest", manifest_path,
                                              rel_dir + "/engine.manifest.json"));
        // INT8 engines: the calibration record and the trtexec cache named
        // by the manifest's build.int8_calibration block.
        if (manifest_present) {
            namespace em = orange::outputs::gen::tensorrt_engine_manifest_v1;
            std::string bytes;
            const nlohmann::json manifest_json = read_bytes(manifest_path, &bytes)
                ? nlohmann::json::parse(bytes, nullptr, false) : nlohmann::json();
            em::OrangeTensorrtEngineManifestV1 manifest;
            std::string error;
            if (!em::parse(manifest_json, &manifest, &error)) {
                errors.push_back(manifest_path + ": does not match orange.tensorrt_engine_manifest v1: " + error);
            } else if (manifest.build.int8_calibration && *manifest.build.int8_calibration) {
                const auto& calibration = **manifest.build.int8_calibration;
                entry["files"].push_back(copy_declared(folder, "int8_calibration_record",
                                                      calibration.record_path.value_or(std::string()),
                                                      rel_dir + "/int8_calibration.record.json"));
                entry["files"].push_back(copy_declared(folder, "int8_calibration_cache",
                                                      calibration.cache_path, rel_dir + "/int8_calibration.cache"));
            }
        }
        size_t present = 0;
        for (const auto& f : entry["files"]) present += f.value("status", "") == "present" ? 1 : 0;
        entry["status"] = present == 0 ? "absent"
                        : (present == entry["files"].size() ? "present" : "partial");
        engines.emplace(engine_sha, std::move(entry));
    };
    for (const auto& [serial, camera] : parsed) {
        if (camera.detect) add(serial, "detect", *camera.detect);
        if (camera.pose) add(serial, "pose", *camera.pose);
    }
    for (const std::string& error : errors) {
        std::cerr << "[model_artifacts] " << error << std::endl;
    }
    for (auto& [sha, entry] : engines) out["engines"][sha] = entry;
    return out;
}

}  // namespace orange::session
