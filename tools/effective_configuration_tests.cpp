// Tests for the effective-configuration provenance block.
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "effective_configuration.h"

using orange::recording::BuildEffectiveConfiguration;
using orange::recording::BuildEffectiveConfigurationFrom;
using orange::recording::ValidateEmittedEffectiveConfiguration;

namespace {
int failures = 0;
void check(bool ok, const std::string& what) {
    std::cout << (ok ? "ok   " : "FAIL ") << what << std::endl;
    if (!ok) ++failures;
}
bool throws(const nlohmann::json& b) {
    try { ValidateEmittedEffectiveConfiguration(b); return false; } catch (const std::exception&) { return true; }
}
std::filesystem::path temp_file(const std::string& name, const std::string& bytes) {
    const auto p = std::filesystem::temp_directory_path() / ("effective_configuration_tests_" + name);
    std::ofstream(p, std::ios::binary) << bytes;
    return p;
}
}  // namespace

int main() {
    const std::vector<std::pair<std::string, std::string>> env = {
        {"ORANGE_EXTERNAL_RECORDER_OWNER_PUSH", "1"},
        {"ORANGE_CITRUS_BINDING_REQUEST_VERSION", "2"},
        {"ORANGE_FAKE_API_TOKEN", "hunter2"},
        {"CUDA_VISIBLE_DEVICES", "0,1"},
        {"HOME", "/home/x"},
        {"PATH", "/usr/bin"},
    };
    const std::vector<std::string> exported = {"ORANGE_EXTERNAL_RECORDER_OWNER_PUSH"};

    // 1. loaded app config
    const std::string cfg_bytes = "{\"recording\": {\"external_ipc\": {\"owner_push\": true}}}";
    const auto cfg = temp_file("app.json", cfg_bytes);
    auto b = BuildEffectiveConfigurationFrom(env, exported, cfg.string(), "2026-10-05T22:00:00Z");
    check(!throws(b), "loaded block validates");
    check(b["app_config"]["status"] == "loaded", "app_config loaded");
    check(b["app_config"]["sha256"].get<std::string>().size() == 64, "sha256 present");
    check(b["app_config"]["contents"]["recording"]["external_ipc"]["owner_push"] == true, "contents copied");
    check(b["app_config"]["size_bytes"] == static_cast<std::int64_t>(cfg_bytes.size()), "size_bytes");
    check(b["environment"].size() == 4, "ORANGE_* and CUDA captured, HOME/PATH not");
    check(b["environment"]["ORANGE_EXTERNAL_RECORDER_OWNER_PUSH"]["source"] == "app_config", "bridge export marked app_config");
    check(b["environment"]["ORANGE_CITRUS_BINDING_REQUEST_VERSION"]["source"] == "environment", "operator variable marked environment");
    check(b["environment"]["ORANGE_CITRUS_BINDING_REQUEST_VERSION"]["value"] == "2", "value kept");
    check(b["environment"]["ORANGE_FAKE_API_TOKEN"]["value"] == "<redacted>" &&
              b["environment"]["ORANGE_FAKE_API_TOKEN"]["redacted"] == true, "secret-looking name redacted");
    check(b["environment"]["CUDA_VISIBLE_DEVICES"]["value"] == "0,1", "CUDA_VISIBLE_DEVICES captured");
    check(b["exported_from_app_config"] == nlohmann::json({"ORANGE_EXTERNAL_RECORDER_OWNER_PUSH"}), "exported list");
    check(b["process"]["pid"].is_number_integer(), "pid");

    // 2. missing / invalid / not configured
    auto m = BuildEffectiveConfigurationFrom(env, exported, "/nonexistent/app.json", "2026-10-05T22:00:00Z");
    check(!throws(m) && m["app_config"]["status"] == "missing" && m["app_config"]["sha256"].is_null(), "missing file declared");
    const auto bad = temp_file("bad.json", "{not json");
    auto i = BuildEffectiveConfigurationFrom(env, exported, bad.string(), "2026-10-05T22:00:00Z");
    check(!throws(i) && i["app_config"]["status"] == "invalid_json" && i["app_config"]["sha256"].is_string() &&
              i["app_config"]["contents"].is_null() && i["app_config"]["error"].is_string(), "invalid json declared with its digest");
    auto n = BuildEffectiveConfigurationFrom(env, exported, "", "2026-10-05T22:00:00Z");
    check(!throws(n) && n["app_config"]["status"] == "not_configured" && n["app_config"]["path"].is_null(), "not configured");

    // 3. validator refusals
    auto t = b; t["environment"]["HOME"] = {{"value", "x"}, {"source", "environment"}};
    check(throws(t), "uncaptured name refused");
    t = b; t["environment"]["ORANGE_CITRUS_BINDING_REQUEST_VERSION"]["source"] = "app_config";
    check(throws(t), "source disagreeing with exported list refused");
    t = b; t["environment"]["ORANGE_FAKE_API_TOKEN"]["value"] = "hunter2";
    check(throws(t), "unredacted secret refused");
    t = b; t["schema_version"] = 2;
    check(throws(t), "wrong schema_version refused");
    t = b; t["app_config"]["status"] = "loaded"; t["app_config"]["contents"] = nullptr;
    check(throws(t), "loaded without contents refused");

    // 4. live capture with the registry
    setenv("ORANGE_EFFECTIVE_CONFIGURATION_TEST_VAR", "live", 1);
    setenv("ORANGE_EFFECTIVE_CONFIGURATION_BRIDGED", "1", 1);
    orange::recording::NoteEnvExportedFromAppConfig("ORANGE_EFFECTIVE_CONFIGURATION_BRIDGED");
    orange::recording::SetLoadedAppConfigPath(cfg.string());
    auto live = BuildEffectiveConfiguration("2026-10-05T22:00:00Z");
    check(!throws(live), "live block validates");
    check(live["environment"]["ORANGE_EFFECTIVE_CONFIGURATION_TEST_VAR"]["source"] == "environment", "live env var captured");
    check(live["environment"]["ORANGE_EFFECTIVE_CONFIGURATION_BRIDGED"]["source"] == "app_config", "live bridged var marked");
    check(live["app_config"]["status"] == "loaded" && live["app_config"]["path"] == cfg.string(), "live app config path");
    check(live["process"]["executable"].is_string(), "executable resolved");

    std::filesystem::remove(cfg); std::filesystem::remove(bad);
    std::cout << (failures ? "FAILED" : "ALL PASS") << " (" << failures << " failures)" << std::endl;
    return failures ? 1 : 0;
}
