// Effective configuration provenance, sealed into the recording start snapshot
// (recording_snapshot_start.json session.effective_configuration) so a run can
// be reproduced from its folder no matter which path set a knob.
//
// Orange takes settings from the app configuration file, from ORANGE_*
// environment variables (which win over the file), and from a bridge that
// exports file values into the environment when the variable is absent. The
// block records all three: the app config file as loaded (path, sha256, size
// and contents), every ORANGE_* variable present at record start with its
// value and whether it came from the real environment or from the bridge, and
// a short list of non-ORANGE variables that shape the process (CUDA, display,
// library path). Values of variables whose names look like secrets are
// redacted. Nothing here is read back by Orange.
//
//   {
//     "schema_id": "orange.effective_configuration", "schema_version": 1,
//     "captured_at_utc": "...Z",
//     "process": {"pid", "executable", "cwd"},
//     "app_config": {"path": str|null, "status": "loaded"|"not_configured"|"missing"|"unreadable"|"invalid_json",
//                    "sha256": hex|null, "size_bytes": int|null, "contents": object|null, "error": str|null},
//     "environment": {"<NAME>": {"value": str, "source": "environment"|"app_config", "redacted"?: true}},
//     "exported_from_app_config": [names the bridge exported, sorted]
//   }
#pragma once

#include <string>
#include <vector>

#include "json.hpp"

namespace orange::recording {

inline constexpr const char* kEffectiveConfigurationSchemaId = "orange.effective_configuration";
inline constexpr int kEffectiveConfigurationSchemaVersion = 1;

// Called by the app-config-to-environment bridge for every variable it sets,
// so the capture can tell bridge exports from operator-set variables.
void NoteEnvExportedFromAppConfig(const std::string& name);
std::vector<std::string> EnvExportedFromAppConfig();

// The app configuration file the process loaded (empty: none / not configured).
void SetLoadedAppConfigPath(const std::string& path);
std::string LoadedAppConfigPath();

// True for names the capture includes: every ORANGE_* variable plus a fixed
// list of process-shaping variables (CUDA_*, DISPLAY, LD_LIBRARY_PATH, ...).
bool IsCapturedEnvName(const std::string& name);
// True when the value must be redacted (name contains SECRET, TOKEN, PASSWORD, PASSWD, API_KEY).
bool IsSecretEnvName(const std::string& name);

// Builds the block from the live process (environ, /proc/self/exe, the loaded
// app config path). Never throws; file problems are declared in app_config.status.
nlohmann::json BuildEffectiveConfiguration(const std::string& captured_at_utc);

// Same, from explicit inputs (tests): env as name->value pairs, exported names,
// app config path ("" = not configured).
nlohmann::json BuildEffectiveConfigurationFrom(const std::vector<std::pair<std::string, std::string>>& env,
                                               const std::vector<std::string>& exported_from_app_config,
                                               const std::string& app_config_path,
                                               const std::string& captured_at_utc);

// Strict shape check of an emitted block. Throws std::runtime_error.
void ValidateEmittedEffectiveConfiguration(const nlohmann::json& block);

}  // namespace orange::recording
