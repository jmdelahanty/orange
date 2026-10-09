#include "session/model_artifacts.h"

#include "gui/spatial_layout/sha256.h"

#include <filesystem>
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
    std::error_code ec;
    fs::create_directories(destination.parent_path(), ec);
    const fs::path temp = destination.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) { file["status"] = "copy_failed"; return file; }
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out) { file["status"] = "copy_failed"; fs::remove(temp, ec); return file; }
    }
    fs::rename(temp, destination, ec);
    if (ec) { file["status"] = "copy_failed"; fs::remove(temp, ec); return file; }
    file["size_bytes"] = bytes.size();
    file["sha256"] = sha256_hex(bytes);
    file["status"] = "present";
    return file;
}

std::string str(const nlohmann::json& o, const char* key)
{
    const auto it = o.find(key);
    return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
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
    std::map<std::string, nlohmann::json> engines;
    for (auto cam = models.begin(); cam != models.end(); ++cam) {
        if (!cam.value().is_object()) continue;
        for (const char* kind : {"detect", "pose"}) {
            const auto block_it = cam.value().find(kind);
            if (block_it == cam.value().end() || !block_it->is_object()) continue;
            const nlohmann::json& block = *block_it;
            if (!block.value("enabled", false)) continue;
            const nlohmann::json runtime = block.value("runtime", nlohmann::json::object());
            const std::string engine_sha = str(runtime, "engine_sha256");
            if (engine_sha.empty()) continue;
            const std::string models_key = "models." + cam.key() + "." + kind;
            auto found = engines.find(engine_sha);
            if (found != engines.end()) {
                found->second["models_keys"].push_back(models_key);
                continue;
            }
            const nlohmann::json manifest_block = runtime.value("engine_manifest", nlohmann::json::object());
            const std::string manifest_path = manifest_block.is_object() ? str(manifest_block, "path") : std::string();
            const std::string rel_dir = "models/" + engine_sha;
            nlohmann::json entry = {
                {"engine_sha256", engine_sha},
                {"task", kind},
                {"model_id", str(runtime, "model_id")},
                {"engine_path", str(runtime, "engine_path")},
                {"engine_bytes", runtime.value("engine_bytes", 0ULL)},
                {"precision", manifest_block.is_object() ? str(manifest_block, "precision") : std::string()},
                {"build_id", manifest_block.is_object() ? str(manifest_block, "build_id") : std::string()},
                {"run_id", manifest_block.is_object() ? str(manifest_block, "run_id") : std::string()},
                {"models_keys", nlohmann::json::array({models_key})},
                {"files", nlohmann::json::array()}
            };
            const bool manifest_present = manifest_block.is_object() && manifest_block.value("present", false);
            entry["files"].push_back(copy_declared(folder, "engine_manifest",
                                                  manifest_present ? manifest_path : std::string(),
                                                  rel_dir + "/engine.manifest.json"));
            // INT8 engines: the calibration record and the trtexec cache named
            // by the manifest's build.int8_calibration block.
            if (manifest_present) {
                std::string bytes;
                nlohmann::json manifest;
                if (read_bytes(manifest_path, &bytes)) {
                    manifest = nlohmann::json::parse(bytes, nullptr, false);
                }
                const nlohmann::json build = manifest.is_object() ? manifest.value("build", nlohmann::json::object()) : nlohmann::json::object();
                const nlohmann::json calib = build.is_object() ? build.value("int8_calibration", nlohmann::json()) : nlohmann::json();
                if (calib.is_object()) {
                    entry["files"].push_back(copy_declared(folder, "int8_calibration_record",
                                                          str(calib, "record_path"), rel_dir + "/int8_calibration.record.json"));
                    entry["files"].push_back(copy_declared(folder, "int8_calibration_cache",
                                                          str(calib, "cache_path"), rel_dir + "/int8_calibration.cache"));
                }
            }
            size_t present = 0;
            for (const auto& f : entry["files"]) present += f.value("status", "") == "present" ? 1 : 0;
            entry["status"] = present == 0 ? "absent"
                            : (present == entry["files"].size() ? "present" : "partial");
            engines.emplace(engine_sha, std::move(entry));
        }
    }
    for (auto& [sha, entry] : engines) out["engines"][sha] = entry;
    return out;
}

}  // namespace orange::session
