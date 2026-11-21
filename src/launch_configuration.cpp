#include "launch_configuration.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <stdexcept>
#include <system_error>
#include <vector>

#include "outputs/common/digest.h"

namespace orange::recording {

namespace {

std::mutex& launch_mutex() {
    static std::mutex m;
    return m;
}
std::optional<LaunchCameraPreset>& preset_storage() {
    static std::optional<LaunchCameraPreset> preset;
    return preset;
}
std::optional<LaunchAppConfig>& app_config_storage() {
    static std::optional<LaunchAppConfig> app_config;
    return app_config;
}

bool ends_with_json(const std::string& name) {
    static const std::string suffix = ".json";
    return name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool read_bytes(const std::filesystem::path& path, std::string* bytes, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *error = "open failed: " + path.string();
        return false;
    }
    bytes->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (in.bad()) {
        *error = "read failed: " + path.string();
        return false;
    }
    return true;
}

nlohmann::json nullable(const std::string& value) {
    return value.empty() ? nlohmann::json(nullptr) : nlohmann::json(value);
}

nlohmann::json files_json(const CameraConfigDirDigest& d) {
    if (!d.ok) return nullptr;
    // An array of {name, sha256}: digest_form_v1 wants every digest under a sha256 name.
    nlohmann::json files = nlohmann::json::array();
    for (const auto& [name, token] : d.files) files.push_back({{"name", name}, {"sha256", token}});
    return files;
}

nlohmann::json preset_digest_json(const CameraConfigDirDigest& d) {
    return {{"sha256", d.ok ? nlohmann::json(d.sha256) : nlohmann::json(nullptr)},
            {"files", files_json(d)},
            {"error", d.ok ? nlohmann::json(nullptr) : nlohmann::json(d.error)}};
}

// Names whose entries differ between two digests (added, removed or changed);
// empty when either side has no file list.
std::vector<std::string> changed_files(const CameraConfigDirDigest& a, const CameraConfigDirDigest& b) {
    std::vector<std::string> changed;
    if (!a.ok || !b.ok) return changed;
    std::set<std::string> names;
    for (const auto& [name, _] : a.files) names.insert(name);
    for (const auto& [name, _] : b.files) names.insert(name);
    for (const auto& name : names) {
        const auto ia = a.files.find(name);
        const auto ib = b.files.find(name);
        if (ia == a.files.end() || ib == b.files.end() || ia->second != ib->second) changed.push_back(name);
    }
    return changed;
}

}  // namespace

CameraConfigDirDigest ComputeCameraConfigDirDigest(const std::filesystem::path& folder) {
    CameraConfigDirDigest out;
    std::error_code ec;
    if (!std::filesystem::is_directory(folder, ec) || ec) {
        out.error = "not a folder: " + folder.string();
        return out;
    }
    std::vector<std::filesystem::directory_entry> entries;
    for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        entries.push_back(*it);
    }
    if (ec) {
        out.error = "cannot list " + folder.string() + ": " + ec.message();
        return out;
    }
    std::map<std::string, std::string> files;
    for (const auto& entry : entries) {
        const std::string name = entry.path().filename().string();
        if (!ends_with_json(name)) continue;
        std::error_code type_ec;
        if (entry.is_symlink(type_ec)) {
            out.error = entry.path().string() + " is a symlink; a preset's camera JSONs must be regular files";
            return out;
        }
        if (!entry.is_regular_file(type_ec) || type_ec) {
            out.error = entry.path().string() + " is not a regular file; a preset's camera JSONs must be regular files";
            return out;
        }
        std::string bytes;
        std::string error;
        if (!read_bytes(entry.path(), &bytes, &error)) {
            out.error = error;
            return out;
        }
        files[name] = orange::outputs::sha256_token(bytes);
    }
    if (files.empty()) {
        out.error = "no camera JSON files in " + folder.string();
        return out;
    }
    nlohmann::json canonical_files = nlohmann::json::object();
    for (const auto& [name, token] : files) canonical_files[name] = token;
    try {
        out.sha256 = orange::outputs::canonical_json_sha256_token(nlohmann::json{{"files", canonical_files}});
    } catch (const std::exception& ex) {
        out.error = std::string("file names are not valid UTF-8: ") + ex.what();
        return out;
    }
    out.files = std::move(files);
    out.ok = true;
    return out;
}

FileBytesDigest ComputeFileBytesDigest(const std::string& path) {
    FileBytesDigest out;
    out.path = path;
    if (path.empty()) return out;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        out.status = "missing";
        return out;
    }
    std::string bytes;
    if (!read_bytes(path, &bytes, &out.error)) {
        out.status = "unreadable";
        return out;
    }
    out.status = "loaded";
    out.sha256 = orange::outputs::sha256_token(bytes);
    return out;
}

void RecordLaunchCameraPreset(const std::string& folder, const std::string& loaded_at_utc) {
    LaunchCameraPreset preset;
    preset.path = folder;
    preset.name = std::filesystem::path(folder).lexically_normal().filename().string();
    if (preset.name.empty() || preset.name == ".") {
        preset.name = std::filesystem::path(folder).lexically_normal().parent_path().filename().string();
    }
    preset.loaded_at_utc = loaded_at_utc;
    preset.digest = ComputeCameraConfigDirDigest(folder);
    std::lock_guard<std::mutex> lock(launch_mutex());
    preset_storage() = std::move(preset);
}

void RecordLaunchAppConfig(const std::string& path, const std::string& loaded_at_utc) {
    LaunchAppConfig app_config;
    app_config.loaded_at_utc = loaded_at_utc;
    app_config.digest = ComputeFileBytesDigest(path);
    std::lock_guard<std::mutex> lock(launch_mutex());
    app_config_storage() = std::move(app_config);
}

std::optional<LaunchCameraPreset> LaunchCameraPresetRecord() {
    std::lock_guard<std::mutex> lock(launch_mutex());
    return preset_storage();
}

std::optional<LaunchAppConfig> LaunchAppConfigRecord() {
    std::lock_guard<std::mutex> lock(launch_mutex());
    return app_config_storage();
}

void ResetLaunchConfigurationForTests() {
    std::lock_guard<std::mutex> lock(launch_mutex());
    preset_storage().reset();
    app_config_storage().reset();
}

nlohmann::json LaunchConfigurationStatusJson() {
    const auto preset = LaunchCameraPresetRecord();
    const auto app_config = LaunchAppConfigRecord();
    nlohmann::json out = {{"camera_preset", nullptr}, {"app_config", nullptr}};
    if (preset) {
        out["camera_preset"] = {{"name", preset->name},
                                {"path", preset->path},
                                {"hash_rule", kCameraConfigDirHashRule},
                                {"sha256", preset->digest.ok ? nlohmann::json(preset->digest.sha256) : nlohmann::json(nullptr)},
                                {"loaded_at_utc", preset->loaded_at_utc},
                                {"error", preset->digest.ok ? nlohmann::json(nullptr) : nlohmann::json(preset->digest.error)}};
    }
    if (app_config) {
        out["app_config"] = {{"path", nullable(app_config->digest.path)},
                             {"status", app_config->digest.status},
                             {"loaded_sha256", nullable(app_config->digest.sha256)},
                             {"loaded_at_utc", app_config->loaded_at_utc},
                             {"error", nullable(app_config->digest.error)}};
    }
    return out;
}

nlohmann::json BuildLaunchConfigurationBlockFrom(const std::optional<LaunchCameraPreset>& preset,
                                                 const std::optional<CameraConfigDirDigest>& preset_on_disk,
                                                 const std::optional<LaunchAppConfig>& app_config,
                                                 const std::optional<FileBytesDigest>& app_config_on_disk,
                                                 const std::string& captured_at_utc) {
    nlohmann::json changed_since_launch = nlohmann::json::array();

    nlohmann::json camera_preset = nullptr;
    if (preset) {
        const CameraConfigDirDigest on_disk = preset_on_disk ? *preset_on_disk : CameraConfigDirDigest{};
        const bool changed = preset->digest.ok != on_disk.ok || preset->digest.sha256 != on_disk.sha256 ||
                             (!preset->digest.ok && preset->digest.error != on_disk.error);
        if (changed) changed_since_launch.push_back("camera_preset");
        camera_preset = {{"name", preset->name},
                         {"path", preset->path},
                         {"hash_rule", kCameraConfigDirHashRule},
                         {"loaded_at_utc", preset->loaded_at_utc},
                         {"at_launch", preset_digest_json(preset->digest)},
                         {"on_disk_at_record_start", preset_digest_json(on_disk)},
                         {"changed", changed},
                         {"changed_files", changed_files(preset->digest, on_disk)}};
    }

    nlohmann::json app = nullptr;
    if (app_config) {
        FileBytesDigest on_disk = app_config_on_disk ? *app_config_on_disk : FileBytesDigest{};
        const bool changed =
            app_config->digest.status != on_disk.status || app_config->digest.sha256 != on_disk.sha256;
        if (changed) changed_since_launch.push_back("app_config");
        app = {{"path", nullable(app_config->digest.path)},
               {"loaded_at_utc", app_config->loaded_at_utc},
               {"loaded_status", app_config->digest.status},
               {"loaded_sha256", nullable(app_config->digest.sha256)},
               {"on_disk_status", on_disk.status},
               {"on_disk_sha256", nullable(on_disk.sha256)},
               {"changed", changed}};
    }

    return {{"schema_id", kLaunchConfigurationSchemaId},
            {"schema_version", kLaunchConfigurationSchemaVersion},
            {"captured_at_utc", captured_at_utc},
            {"camera_preset", camera_preset},
            {"app_config", app},
            {"configuration_changed_since_launch", !changed_since_launch.empty()},
            {"changed_since_launch", changed_since_launch}};
}

nlohmann::json BuildLaunchConfigurationBlock(const std::string& captured_at_utc) {
    const auto preset = LaunchCameraPresetRecord();
    const auto app_config = LaunchAppConfigRecord();
    std::optional<CameraConfigDirDigest> preset_on_disk;
    if (preset) preset_on_disk = ComputeCameraConfigDirDigest(preset->path);
    std::optional<FileBytesDigest> app_on_disk;
    if (app_config) app_on_disk = ComputeFileBytesDigest(app_config->digest.path);
    return BuildLaunchConfigurationBlockFrom(preset, preset_on_disk, app_config, app_on_disk, captured_at_utc);
}

void ValidateEmittedLaunchConfiguration(const nlohmann::json& b) {
    auto require = [](bool ok, const std::string& what) {
        if (!ok) throw std::runtime_error("launch_configuration: " + what);
    };
    auto token_or_null = [](const nlohmann::json& v) {
        return v.is_null() || (v.is_string() && orange::outputs::is_sha256_token(v.get<std::string>()));
    };
    auto status_ok = [](const nlohmann::json& v) {
        return v.is_string() && (v == "loaded" || v == "not_configured" || v == "missing" || v == "unreadable");
    };
    require(b.is_object(), "block must be an object");
    for (const char* key : {"schema_id", "schema_version", "captured_at_utc", "camera_preset", "app_config",
                            "configuration_changed_since_launch", "changed_since_launch"}) {
        require(b.contains(key), std::string("missing ") + key);
    }
    require(b.size() == 7, "unexpected keys");
    require(b.at("schema_id") == kLaunchConfigurationSchemaId, "schema_id");
    require(b.at("schema_version") == kLaunchConfigurationSchemaVersion, "schema_version");
    require(b.at("captured_at_utc").is_string() && !b.at("captured_at_utc").get<std::string>().empty(),
            "captured_at_utc");
    std::set<std::string> changed;
    for (const auto& item : b.at("changed_since_launch")) {
        require(item == "camera_preset" || item == "app_config", "changed_since_launch item");
        changed.insert(item.get<std::string>());
    }
    require(b.at("configuration_changed_since_launch").is_boolean() &&
                b.at("configuration_changed_since_launch").get<bool>() == !changed.empty(),
            "configuration_changed_since_launch must say whether changed_since_launch is non-empty");
    const auto& p = b.at("camera_preset");
    if (p.is_null()) {
        require(!changed.count("camera_preset"), "camera_preset null but listed as changed");
    } else {
        require(p.is_object(), "camera_preset must be an object or null");
        require(p.at("hash_rule") == kCameraConfigDirHashRule, "camera_preset.hash_rule");
        require(p.at("name").is_string() && p.at("path").is_string() && p.at("loaded_at_utc").is_string(),
                "camera_preset name/path/loaded_at_utc");
        for (const char* side : {"at_launch", "on_disk_at_record_start"}) {
            const auto& d = p.at(side);
            require(token_or_null(d.at("sha256")), std::string("camera_preset.") + side + ".sha256");
            require(d.at("sha256").is_null() == !d.at("error").is_null(),
                    std::string("camera_preset.") + side + ": exactly one of sha256 and error");
            require(d.at("files").is_null() == d.at("sha256").is_null(), std::string("camera_preset.") + side + ".files");
        }
        require(p.at("changed").get<bool>() == (changed.count("camera_preset") != 0), "camera_preset.changed");
        require(p.at("changed_files").is_array(), "camera_preset.changed_files");
    }
    const auto& a = b.at("app_config");
    if (a.is_null()) {
        require(!changed.count("app_config"), "app_config null but listed as changed");
    } else {
        require(a.is_object(), "app_config must be an object or null");
        require(status_ok(a.at("loaded_status")) && status_ok(a.at("on_disk_status")), "app_config status");
        require(token_or_null(a.at("loaded_sha256")) && token_or_null(a.at("on_disk_sha256")), "app_config sha256");
        require((a.at("loaded_status") == "loaded") == a.at("loaded_sha256").is_string(),
                "app_config.loaded_sha256 set exactly when loaded");
        require((a.at("on_disk_status") == "loaded") == a.at("on_disk_sha256").is_string(),
                "app_config.on_disk_sha256 set exactly when loaded");
        require(a.at("changed").get<bool>() == (changed.count("app_config") != 0), "app_config.changed");
    }
}

}  // namespace orange::recording
