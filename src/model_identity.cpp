#include "model_identity.h"

#include "orange_outputs/orange_tensorrt_engine_manifest_v1.h"
#include "orange_outputs/palette_canonical_onnx_model_manifest_v2.h"
#include "outputs/common/digest.h"
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

std::string string_or_empty(const std::optional<std::string>& value)
{
    return value ? *value : std::string();
}

// Palette's canonical ONNX manifest: the bytes the engine manifest recorded, valid against the
// pinned Palette schema. Only version 2 is accepted; version 1 (bare-hex payload_digest) was
// retired on 2026-10-13, when the last production engine was rebound to a v2 manifest.
std::string check_canonical_manifest(const std::string& path, const std::string& declared_sha256)
{
    bool ok = false;
    const std::string bytes = read_file(path, &ok);
    if (!ok) {
        return "unreadable";
    }
    const std::string actual = orange::pose::file_sha256_hex(path);
    if (declared_sha256.empty() || actual != declared_sha256) {
        return "sha256 " + actual + " differs from the engine manifest's " +
            (declared_sha256.empty() ? std::string("(none recorded)") : declared_sha256);
    }
    const nlohmann::json manifest = nlohmann::json::parse(bytes, nullptr, false);
    if (!manifest.is_object()) {
        return "not a JSON object";
    }
    if (manifest.value("schema_id", std::string()) != "palette.canonical_onnx_model_manifest") {
        return "schema_id is not palette.canonical_onnx_model_manifest";
    }
    std::string error;
    const nlohmann::json version = manifest.value("schema_version", nlohmann::json());
    // The payload digest is the SHA-256 of the payload's canonical JSON
    // (canonical_json_utf8_sort_keys_compact_v1, outputs/common/digest.h).
    std::string payload_hex;
    try {
        if (manifest.contains("payload")) {
            payload_hex = orange::outputs::sha256_hex(orange::outputs::canonical_json_text(manifest.at("payload")));
        }
    } catch (const std::exception&) {
        return "payload is not valid UTF-8 JSON";
    }
    if (version != 2) {
        return "schema_version " + version.dump() +
            " is not accepted: Orange accepts palette.canonical_onnx_model_manifest v2 only";
    }
    orange::outputs::gen::palette_canonical_onnx_manifest_v2::PaletteCanonicalOnnxModelManifestV2 parsed;
    if (!orange::outputs::gen::palette_canonical_onnx_manifest_v2::parse(manifest, &parsed, &error)) {
        return "not valid against palette.canonical_onnx_model_manifest v2: " + error;
    }
    if (parsed.payload_sha256 != "sha256:" + payload_hex) {
        return "payload_sha256 " + parsed.payload_sha256 + " is not the payload's digest sha256:" + payload_hex;
    }
    return {};
}

void read_manifest(ModelIdentity* identity)
{
    // Orange's engine build scripts (scripts/write_tensorrt_engine_manifest.py,
    // scripts/write_pose_tensorrt_engine_manifest.py) write the manifest beside
    // the engine as `<stem>.manifest.json` (`foo.engine` -> `foo.manifest.json`);
    // `<engine>.manifest.json` is accepted as a fallback.
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
    const nlohmann::json manifest = nlohmann::json::parse(bytes, nullptr, false);
    if (manifest.is_discarded()) {
        identity->manifest_error = "parse: not valid JSON";
        return;
    }
    if (!manifest.is_object()) {
        identity->manifest_error = "not an object";
        return;
    }
    // What the file says it is, reported even when it is not a v1 engine manifest.
    if (auto it = manifest.find("schema_id"); it != manifest.end() && it->is_string()) {
        identity->manifest_schema_id = it->get<std::string>();
    }
    if (auto it = manifest.find("schema_version"); it != manifest.end() && it->is_number_integer()) {
        identity->manifest_schema_version = it->get<int>();
    }
    if (identity->manifest_schema_id != "orange.tensorrt_engine_manifest") {
        identity->manifest_error = "unexpected schema_id '" + identity->manifest_schema_id + "'";
        return;
    }
    // Generated strict reader of the published schema
    // (docs/schemas/orange_tensorrt_engine_manifest_v1.schema.json): a manifest
    // that does not match is reported, never read field by field into blanks.
    namespace gen = orange::outputs::gen::tensorrt_engine_manifest_v1;
    gen::OrangeTensorrtEngineManifestV1 parsed;
    std::string error;
    if (!gen::parse(manifest, &parsed, &error)) {
        identity->manifest_error = "does not match orange.tensorrt_engine_manifest v1: " + error;
        return;
    }
    identity->manifest_status = parsed.status;
    identity->task = string_or_empty(parsed.task);
    identity->run_id = parsed.palette_run_id;
    identity->set_id = string_or_empty(parsed.set_id);
    identity->build_id = string_or_empty(parsed.build_id);
    identity->precision = parsed.precision;
    identity->target_hardware_class = parsed.target_hardware_class;
    identity->created_at_utc = parsed.created_at_utc;

    identity->declared_engine_sha256 = parsed.engine.sha256;
    identity->declared_engine_sha256_matches =
        !identity->declared_engine_sha256.empty() &&
        identity->declared_engine_sha256 == identity->engine_sha256;

    const auto& source = parsed.source;
    identity->onnx_sha256 = string_or_empty(source.onnx.sha256);
    identity->onnx_path = string_or_empty(source.onnx.path);
    identity->weights_sha256 = string_or_empty(source.weights.sha256);
    identity->weights_path = string_or_empty(source.weights.source_path);
    if (identity->weights_path.empty()) identity->weights_path = string_or_empty(source.weights.relative_path);
    if (source.canonical_manifest) {
        identity->canonical_manifest_path = string_or_empty(source.canonical_manifest->path);
        identity->canonical_manifest_sha256 = string_or_empty(source.canonical_manifest->sha256);
        if (!identity->canonical_manifest_path.empty()) {
            identity->canonical_manifest_error = check_canonical_manifest(
                identity->canonical_manifest_path, identity->canonical_manifest_sha256);
        }
    }
    if (source.onnx_manifest) {
        identity->onnx_manifest_path = string_or_empty(source.onnx_manifest->path);
        identity->onnx_manifest_sha256 = string_or_empty(source.onnx_manifest->sha256);
    }
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

std::string canonical_manifest_refusal(const ModelIdentity& identity)
{
    if (identity.canonical_manifest_error.empty()) {
        return {};
    }
    return "engine " + identity.engine_path + ": Palette canonical manifest " +
        identity.canonical_manifest_path + " " + identity.canonical_manifest_error;
}

}  // namespace orange::model_identity
