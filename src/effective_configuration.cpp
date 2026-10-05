#include "effective_configuration.h"

#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <stdexcept>

#include "gui/spatial_layout/sha256.h"

extern char** environ;

namespace orange::recording {

namespace {

std::mutex& registry_mutex() {
    static std::mutex m;
    return m;
}
std::set<std::string>& exported_names() {
    static std::set<std::string> names;
    return names;
}
std::string& app_config_path_storage() {
    static std::string path;
    return path;
}

const std::set<std::string>& captured_exact_names() {
    static const std::set<std::string> names = {
        "CUDA_VISIBLE_DEVICES", "CUDA_DEVICE_ORDER", "CUDA_MODULE_LOADING", "CUDA_LAUNCH_BLOCKING",
        "CUDA_MPS_PIPE_DIRECTORY", "CUDA_MPS_ACTIVE_THREAD_PERCENTAGE", "LD_LIBRARY_PATH",
        "DISPLAY", "WAYLAND_DISPLAY", "XDG_SESSION_TYPE", "SUDO_USER", "OMP_NUM_THREADS",
    };
    return names;
}

std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

nlohmann::json describe_app_config(const std::string& path) {
    nlohmann::json out = {{"path", nullptr}, {"status", "not_configured"}, {"sha256", nullptr},
                          {"size_bytes", nullptr}, {"contents", nullptr}, {"error", nullptr}};
    if (path.empty()) return out;
    out["path"] = path;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        out["status"] = "missing";
        return out;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        out["status"] = "unreadable";
        out["error"] = "open failed";
        return out;
    }
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
        out["status"] = "unreadable";
        out["error"] = "read failed";
        return out;
    }
    out["sha256"] = orange::gui::spatial_layout::checksum::sha256_hex(bytes);
    out["size_bytes"] = static_cast<std::int64_t>(bytes.size());
    try {
        out["contents"] = nlohmann::json::parse(bytes);
        out["status"] = "loaded";
    } catch (const std::exception& ex) {
        out["status"] = "invalid_json";
        out["error"] = ex.what();
    }
    return out;
}

}  // namespace

void NoteEnvExportedFromAppConfig(const std::string& name) {
    if (name.empty()) return;
    std::lock_guard<std::mutex> lock(registry_mutex());
    exported_names().insert(name);
}

std::vector<std::string> EnvExportedFromAppConfig() {
    std::lock_guard<std::mutex> lock(registry_mutex());
    return std::vector<std::string>(exported_names().begin(), exported_names().end());
}

void SetLoadedAppConfigPath(const std::string& path) {
    std::lock_guard<std::mutex> lock(registry_mutex());
    app_config_path_storage() = path;
}

std::string LoadedAppConfigPath() {
    std::lock_guard<std::mutex> lock(registry_mutex());
    return app_config_path_storage();
}

bool IsCapturedEnvName(const std::string& name) {
    if (name.rfind("ORANGE_", 0) == 0) return true;
    return captured_exact_names().count(name) != 0;
}

bool IsSecretEnvName(const std::string& name) {
    const std::string u = upper(name);
    for (const char* needle : {"SECRET", "TOKEN", "PASSWORD", "PASSWD", "API_KEY"}) {
        if (u.find(needle) != std::string::npos) return true;
    }
    return false;
}

nlohmann::json BuildEffectiveConfigurationFrom(const std::vector<std::pair<std::string, std::string>>& env,
                                               const std::vector<std::string>& exported_from_app_config,
                                               const std::string& app_config_path,
                                               const std::string& captured_at_utc) {
    std::set<std::string> exported(exported_from_app_config.begin(), exported_from_app_config.end());
    nlohmann::json environment = nlohmann::json::object();
    for (const auto& [name, value] : env) {
        if (name.empty() || !IsCapturedEnvName(name)) continue;
        nlohmann::json entry = {{"value", IsSecretEnvName(name) ? "<redacted>" : value},
                                {"source", exported.count(name) ? "app_config" : "environment"}};
        if (IsSecretEnvName(name)) entry["redacted"] = true;
        environment[name] = std::move(entry);
    }
    nlohmann::json process = {{"pid", static_cast<std::int64_t>(::getpid())}, {"executable", nullptr}, {"cwd", nullptr}};
    std::error_code ec;
    const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) process["executable"] = exe.string();
    const auto cwd = std::filesystem::current_path(ec);
    if (!ec) process["cwd"] = cwd.string();
    return {{"schema_id", kEffectiveConfigurationSchemaId},
            {"schema_version", kEffectiveConfigurationSchemaVersion},
            {"captured_at_utc", captured_at_utc},
            {"process", process},
            {"app_config", describe_app_config(app_config_path)},
            {"environment", environment},
            {"exported_from_app_config", nlohmann::json(std::vector<std::string>(exported.begin(), exported.end()))}};
}

nlohmann::json BuildEffectiveConfiguration(const std::string& captured_at_utc) {
    std::vector<std::pair<std::string, std::string>> env;
    for (char** p = environ; p && *p; ++p) {
        const std::string kv(*p);
        const auto eq = kv.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        env.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
    }
    return BuildEffectiveConfigurationFrom(env, EnvExportedFromAppConfig(), LoadedAppConfigPath(), captured_at_utc);
}

void ValidateEmittedEffectiveConfiguration(const nlohmann::json& b) {
    auto require = [](bool ok, const std::string& what) {
        if (!ok) throw std::runtime_error("effective_configuration: " + what);
    };
    require(b.is_object(), "block must be an object");
    for (const char* key : {"schema_id", "schema_version", "captured_at_utc", "process", "app_config", "environment",
                            "exported_from_app_config"}) {
        require(b.contains(key), std::string("missing ") + key);
    }
    require(b.at("schema_id") == kEffectiveConfigurationSchemaId, "schema_id");
    require(b.at("schema_version") == kEffectiveConfigurationSchemaVersion, "schema_version");
    require(b.at("captured_at_utc").is_string() && !b.at("captured_at_utc").get<std::string>().empty(), "captured_at_utc");
    const auto& a = b.at("app_config");
    require(a.is_object(), "app_config must be an object");
    for (const char* key : {"path", "status", "sha256", "size_bytes", "contents", "error"}) {
        require(a.contains(key), std::string("app_config missing ") + key);
    }
    const std::string status = a.at("status").get<std::string>();
    require(status == "loaded" || status == "not_configured" || status == "missing" || status == "unreadable" ||
                status == "invalid_json",
            "app_config.status");
    if (status == "loaded") {
        require(a.at("sha256").is_string() && a.at("sha256").get<std::string>().size() == 64, "loaded needs sha256");
        require(a.at("contents").is_object(), "loaded needs contents");
    }
    if (status == "not_configured") require(a.at("path").is_null(), "not_configured needs null path");
    const auto& env = b.at("environment");
    require(env.is_object(), "environment must be an object");
    std::set<std::string> exported;
    for (const auto& name : b.at("exported_from_app_config")) exported.insert(name.get<std::string>());
    for (auto it = env.begin(); it != env.end(); ++it) {
        require(IsCapturedEnvName(it.key()), "environment holds uncaptured name " + it.key());
        require(it.value().is_object() && it.value().contains("value") && it.value().contains("source"),
                "environment entry " + it.key());
        const std::string source = it.value().at("source").get<std::string>();
        require(source == "environment" || source == "app_config", "environment source " + it.key());
        require((source == "app_config") == (exported.count(it.key()) != 0),
                "environment source disagrees with exported_from_app_config for " + it.key());
        if (IsSecretEnvName(it.key())) require(it.value().at("value") == "<redacted>", "unredacted secret " + it.key());
    }
}

}  // namespace orange::recording
