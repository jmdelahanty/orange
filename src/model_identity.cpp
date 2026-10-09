#include "model_identity.h"

#include "pose_skeleton_sidecar.h"

#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <system_error>
#include <filesystem>

namespace orange::model_identity {

namespace {

std::string read_file(const std::string& path, bool* ok)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *ok = false;
        return {};
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    *ok = static_cast<bool>(in) || in.eof();
    return buffer.str();
}

std::string string_at(const nlohmann::json& object, const char* key)
{
    if (!object.is_object()) return {};
    auto it = object.find(key);
    if (it == object.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

const nlohmann::json& object_at(const nlohmann::json& object, const char* key)
{
    static const nlohmann::json empty = nlohmann::json::object();
    if (!object.is_object()) return empty;
    auto it = object.find(key);
    if (it == object.end() || !it->is_object()) return empty;
    return *it;
}

void read_manifest(ModelIdentity* identity)
{
    // Palette writes the manifest beside the engine as `<stem>.manifest.json`
    // (`foo.engine` -> `foo.manifest.json`); `<engine>.manifest.json` is
    // accepted as a fallback.
    std::error_code ec;
    const std::filesystem::path engine_file(identity->engine_path);
    const std::filesystem::path stem_manifest =
        engine_file.parent_path() / (engine_file.stem().string() + ".manifest.json");
    const std::filesystem::path suffix_manifest(identity->engine_path + ".manifest.json");
    if (std::filesystem::is_regular_file(stem_manifest, ec)) {
        identity->manifest_path = stem_manifest.string();
    } else if (std::filesystem::is_regular_file(suffix_manifest, ec)) {
        identity->manifest_path = suffix_manifest.string();
    } else {
        identity->manifest_path = stem_manifest.string();
        identity->manifest_present = false;
        return;
    }
    identity->manifest_present = true;
    bool ok = false;
    const std::string bytes = read_file(identity->manifest_path, &ok);
    if (!ok) {
        identity->manifest_error = "unreadable";
        return;
    }
    identity->manifest_sha256 = orange::pose::file_sha256_hex(identity->manifest_path);
    nlohmann::json manifest;
    try {
        manifest = nlohmann::json::parse(bytes);
    } catch (const std::exception& e) {
        identity->manifest_error = std::string("parse: ") + e.what();
        return;
    }
    if (!manifest.is_object()) {
        identity->manifest_error = "not an object";
        return;
    }
    identity->manifest_schema_id = string_at(manifest, "schema_id");
    if (auto it = manifest.find("schema_version"); it != manifest.end() && it->is_number_integer()) {
        identity->manifest_schema_version = it->get<int>();
    }
    if (identity->manifest_schema_id != "orange.tensorrt_engine_manifest") {
        identity->manifest_error = "unexpected schema_id '" + identity->manifest_schema_id + "'";
        return;
    }
    identity->manifest_status = string_at(manifest, "status");
    identity->task = string_at(manifest, "task");
    identity->run_id = string_at(manifest, "palette_run_id");
    identity->set_id = string_at(manifest, "set_id");
    identity->build_id = string_at(manifest, "build_id");
    identity->precision = string_at(manifest, "precision");
    identity->target_hardware_class = string_at(manifest, "target_hardware_class");
    identity->created_at_utc = string_at(manifest, "created_at_utc");

    const nlohmann::json& engine = object_at(manifest, "engine");
    identity->declared_engine_sha256 = string_at(engine, "sha256");
    identity->declared_engine_sha256_matches =
        !identity->declared_engine_sha256.empty() &&
        identity->declared_engine_sha256 == identity->engine_sha256;

    const nlohmann::json& source = object_at(manifest, "source");
    const nlohmann::json& onnx = object_at(source, "onnx");
    identity->onnx_sha256 = string_at(onnx, "sha256");
    identity->onnx_path = string_at(onnx, "path");
    const nlohmann::json& weights = object_at(source, "weights");
    identity->weights_sha256 = string_at(weights, "sha256");
    identity->weights_path = string_at(weights, "source_path");
    if (identity->weights_path.empty()) identity->weights_path = string_at(weights, "relative_path");
    const nlohmann::json& canonical = object_at(source, "canonical_manifest");
    identity->canonical_manifest_path = string_at(canonical, "path");
    identity->canonical_manifest_sha256 = string_at(canonical, "sha256");
    const nlohmann::json& onnx_manifest = object_at(source, "onnx_manifest");
    identity->onnx_manifest_path = string_at(onnx_manifest, "path");
    identity->onnx_manifest_sha256 = string_at(onnx_manifest, "sha256");
}

}  // namespace

ModelIdentity resolve_model_identity_uncached(const std::string& engine_path)
{
    ModelIdentity identity;
    identity.engine_path = engine_path;
    if (engine_path.empty()) {
        return identity;
    }
    std::error_code ec;
    if (std::filesystem::is_regular_file(engine_path, ec)) {
        identity.engine_present = true;
        identity.engine_bytes = static_cast<std::uint64_t>(std::filesystem::file_size(engine_path, ec));
        if (ec) identity.engine_bytes = 0;
        identity.engine_sha256 = orange::pose::file_sha256_hex(engine_path);
    }
    read_manifest(&identity);
    return identity;
}

ModelIdentity resolve_model_identity(const std::string& engine_path)
{
    static std::mutex mutex;
    static std::map<std::string, ModelIdentity> cache;
    if (engine_path.empty()) {
        return resolve_model_identity_uncached(engine_path);
    }
    std::lock_guard<std::mutex> lock(mutex);
    auto it = cache.find(engine_path);
    if (it == cache.end()) {
        it = cache.emplace(engine_path, resolve_model_identity_uncached(engine_path)).first;
    }
    return it->second;
}

nlohmann::json engine_manifest_json(const ModelIdentity& identity)
{
    if (identity.engine_path.empty()) {
        return nullptr;
    }
    nlohmann::json out = {
        {"path", identity.manifest_path},
        {"present", identity.manifest_present},
        {"sha256", identity.manifest_sha256},
        {"error", identity.manifest_error},
        {"schema_id", identity.manifest_schema_id},
        {"schema_version", identity.manifest_schema_version},
        {"status", identity.manifest_status},
        {"task", identity.task},
        {"run_id", identity.run_id},
        {"set_id", identity.set_id},
        {"build_id", identity.build_id},
        {"precision", identity.precision},
        {"target_hardware_class", identity.target_hardware_class},
        {"created_at_utc", identity.created_at_utc},
        {"engine_sha256_declared", identity.declared_engine_sha256},
        {"engine_sha256_matches", identity.declared_engine_sha256_matches},
        {"onnx", {{"path", identity.onnx_path}, {"sha256", identity.onnx_sha256}}},
        {"weights", {{"path", identity.weights_path}, {"sha256", identity.weights_sha256}}},
        {"canonical_manifest", {{"path", identity.canonical_manifest_path}, {"sha256", identity.canonical_manifest_sha256}}},
        {"onnx_manifest", {{"path", identity.onnx_manifest_path}, {"sha256", identity.onnx_manifest_sha256}}}
    };
    return out;
}

}  // namespace orange::model_identity
