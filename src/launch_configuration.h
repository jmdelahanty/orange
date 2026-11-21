// The configuration this process actually runs on, kept from the moment it was
// loaded, so a recording made hours later in the same process cannot silently
// describe files that were edited after they were read.
//
// Two inputs are kept:
//   - the camera preset folder the cameras were opened from (ORANGE_GUI_CONFIG_DIR /
//     the selected local config folder, or the headless spec's folder), with its digest
//     under rule orange_camera_config_dir_sha256_v1 (the same rule as
//     scripts/camera_config_dir_digest.py; see ComputeCameraConfigDirDigest);
//   - the app config file (~/orange_data/config/app/default.json) as read at launch:
//     SHA-256 of its exact bytes.
//
// Local-control status reports both (status.configuration, LaunchConfigurationStatusJson).
// At every record start the start snapshot seals session.launch_configuration
// (orange.recording.launch_configuration v1, docs/schemas/
// orange_recording_launch_configuration_v1.schema.json): the launch-time record, the
// on-disk digests recomputed at that moment, and configuration_changed_since_launch with
// the list of what differs. A difference is recorded, never refused (2026-11-21).
// session.effective_configuration still reads the app config file at record start; this
// block says whether those bytes are the ones the process loaded.
#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>

#include "json.hpp"

namespace orange::recording {

inline constexpr const char* kCameraConfigDirHashRule = "orange_camera_config_dir_sha256_v1";
inline constexpr const char* kLaunchConfigurationSchemaId = "orange.recording.launch_configuration";
inline constexpr int kLaunchConfigurationSchemaVersion = 1;

// Rule orange_camera_config_dir_sha256_v1 (scripts/camera_config_dir_digest.py):
//   files  = every directory entry directly in the folder whose name ends in ".json";
//            a *.json entry that is a symlink or not a regular file is an error naming it.
//   entry  = file name -> "sha256:<hex>" of the file's raw bytes.
//   digest = "sha256:<hex>" of the canonical JSON {"files": {name: entry}} (keys sorted,
//            compact, UTF-8 unescaped: src/outputs/common/digest.h canonical_json_text).
// A folder that is missing, unreadable, or holds no *.json file has no digest (error set).
struct CameraConfigDirDigest {
    bool ok = false;
    std::string error;
    std::map<std::string, std::string> files;  // name -> sha256:<hex>
    std::string sha256;                        // sha256:<hex> when ok
};
CameraConfigDirDigest ComputeCameraConfigDirDigest(const std::filesystem::path& folder);

// The app config file's bytes: status loaded | not_configured (empty path) | missing | unreadable.
struct FileBytesDigest {
    std::string path;
    std::string status = "not_configured";
    std::string sha256;  // sha256:<hex> when loaded
    std::string error;
};
FileBytesDigest ComputeFileBytesDigest(const std::string& path);

struct LaunchCameraPreset {
    std::string name;  // folder basename
    std::string path;  // folder as opened
    std::string loaded_at_utc;
    CameraConfigDirDigest digest;
};
struct LaunchAppConfig {
    std::string loaded_at_utc;
    FileBytesDigest digest;
};

// Camera open (GUI thread) or headless camera configuration: reads the folder's JSON
// files once (small files, never on a hot thread). Replaces the previous record: the
// newest camera open is the one the cameras run on.
void RecordLaunchCameraPreset(const std::string& folder, const std::string& loaded_at_utc);
// Right after the app config was loaded at launch.
void RecordLaunchAppConfig(const std::string& path, const std::string& loaded_at_utc);
std::optional<LaunchCameraPreset> LaunchCameraPresetRecord();
std::optional<LaunchAppConfig> LaunchAppConfigRecord();
// Tests only.
void ResetLaunchConfigurationForTests();

// status.configuration: {"camera_preset": null | {name, path, sha256, hash_rule, loaded_at_utc, error},
//                        "app_config": null | {path, status, loaded_sha256, loaded_at_utc, error}}.
// No file I/O.
nlohmann::json LaunchConfigurationStatusJson();

// session.launch_configuration for a record start: the launch records above, the on-disk
// state recomputed now, and what changed. Reads the preset folder and the app config file.
nlohmann::json BuildLaunchConfigurationBlock(const std::string& captured_at_utc);
// Same, from explicit inputs (tests).
nlohmann::json BuildLaunchConfigurationBlockFrom(const std::optional<LaunchCameraPreset>& preset,
                                                 const std::optional<CameraConfigDirDigest>& preset_on_disk,
                                                 const std::optional<LaunchAppConfig>& app_config,
                                                 const std::optional<FileBytesDigest>& app_config_on_disk,
                                                 const std::string& captured_at_utc);

// Strict shape check of an emitted block. Throws std::runtime_error.
void ValidateEmittedLaunchConfiguration(const nlohmann::json& block);

}  // namespace orange::recording
