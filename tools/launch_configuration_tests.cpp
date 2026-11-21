// Tests for the launch configuration record (src/launch_configuration.h).
//
//   launch_configuration_tests                 unit tests
//   launch_configuration_tests <dir>           write sample documents (writer_samples_schema_tests.py)
//   launch_configuration_tests --digest <dir>  print the folder digest, or "error: ..." (exit 2);
//                                              tools/launch_configuration_digest_tests.py compares
//                                              it with scripts/camera_config_dir_digest.py
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "launch_configuration.h"
#include "outputs/common/digest.h"

namespace lc = orange::recording;
namespace fs = std::filesystem;

namespace {
int failures = 0;
void check(bool ok, const std::string& what) {
    std::cout << (ok ? "ok   " : "FAIL ") << what << std::endl;
    if (!ok) ++failures;
}
bool throws(const nlohmann::json& b) {
    try {
        lc::ValidateEmittedLaunchConfiguration(b);
        return false;
    } catch (const std::exception&) {
        return true;
    }
}
void write(const fs::path& p, const std::string& bytes) {
    std::ofstream(p, std::ios::binary) << bytes;
}
fs::path fresh_dir(const std::string& name) {
    const fs::path dir = fs::temp_directory_path() / ("launch_configuration_tests_" + std::to_string(::getpid()) + "_" + name);
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

// A preset folder like ~/orange_data/config/local/100_cam4_ptp_fourcam.
fs::path make_preset(const std::string& name) {
    const fs::path dir = fresh_dir("") / name;
    fs::create_directories(dir);
    write(dir / "Cam2010093.json", "{\"exposure\": 100}\n");
    write(dir / "Cam2010094.json", "{\"exposure\": 100}\n");
    write(dir / "README.md", "not part of the digest");
    fs::create_directories(dir / "sub");
    write(dir / "sub" / "Cam9.json", "{}");
    return dir;
}

void test_digest_rule() {
    const fs::path dir = make_preset("100_cam4_ptp_fourcam");
    const auto d = lc::ComputeCameraConfigDirDigest(dir);
    check(d.ok && d.error.empty(), "preset digest computed");
    check(d.files.size() == 2, "only *.json directly in the folder (README and sub/ skipped)");
    const std::string entry = orange::outputs::sha256_token("{\"exposure\": 100}\n");
    check(d.files.at("Cam2010093.json") == entry, "entry is the token of the raw bytes");
    const std::string canonical =
        "{\"files\":{\"Cam2010093.json\":\"" + entry + "\",\"Cam2010094.json\":\"" + entry + "\"}}";
    check(d.sha256 == orange::outputs::sha256_token(canonical), "digest is the token of the canonical files object");

    write(dir / "Cam2010094.json", "{\"exposure\": 200}\n");
    const auto d2 = lc::ComputeCameraConfigDirDigest(dir);
    check(d2.ok && d2.sha256 != d.sha256, "an edited camera JSON changes the digest");

    fs::create_symlink(dir / "Cam2010093.json", dir / "Cam_link.json");
    const auto d3 = lc::ComputeCameraConfigDirDigest(dir);
    check(!d3.ok && d3.error.find("symlink") != std::string::npos, "a symlinked *.json is an error");
    fs::remove(dir / "Cam_link.json");

    fs::create_directories(dir / "weird.json");
    const auto d4 = lc::ComputeCameraConfigDirDigest(dir);
    check(!d4.ok && d4.error.find("not a regular file") != std::string::npos, "a *.json folder is an error");
    fs::remove(dir / "weird.json");

    const fs::path empty = fresh_dir("empty");
    write(empty / "notes.txt", "x");
    check(!lc::ComputeCameraConfigDirDigest(empty).ok, "no *.json: no digest");
    check(!lc::ComputeCameraConfigDirDigest(empty / "missing").ok, "missing folder: no digest");
    fs::remove_all(dir.parent_path());
    fs::remove_all(empty);
}

void test_block_and_status() {
    lc::ResetLaunchConfigurationForTests();
    check(lc::LaunchConfigurationStatusJson() ==
              nlohmann::json({{"camera_preset", nullptr}, {"app_config", nullptr}}),
          "status before any load: both null");
    const auto empty_block = lc::BuildLaunchConfigurationBlock("2026-11-21T00:00:00Z");
    check(!throws(empty_block) && empty_block.at("camera_preset").is_null() &&
              empty_block.at("configuration_changed_since_launch") == false,
          "block without records validates, nothing changed");

    const fs::path preset = make_preset("100_cam4_ptp_fourcam");
    const fs::path app_dir = fresh_dir("app");
    const fs::path app = app_dir / "default.json";
    write(app, "{\"recording\": {\"clip_seconds\": 900}}");
    lc::RecordLaunchCameraPreset(preset.string(), "2026-11-21T08:00:00Z");
    lc::RecordLaunchAppConfig(app.string(), "2026-11-21T07:59:00Z");

    const auto status = lc::LaunchConfigurationStatusJson();
    const auto launch_digest = lc::ComputeCameraConfigDirDigest(preset);
    check(status.at("camera_preset").at("name") == "100_cam4_ptp_fourcam" &&
              status.at("camera_preset").at("path") == preset.string() &&
              status.at("camera_preset").at("hash_rule") == "orange_camera_config_dir_sha256_v1" &&
              status.at("camera_preset").at("sha256") == launch_digest.sha256 &&
              status.at("camera_preset").at("loaded_at_utc") == "2026-11-21T08:00:00Z",
          "status.configuration.camera_preset");
    check(status.at("app_config").at("loaded_sha256") ==
                  orange::outputs::sha256_token("{\"recording\": {\"clip_seconds\": 900}}") &&
              status.at("app_config").at("status") == "loaded" && status.at("app_config").at("path") == app.string(),
          "status.configuration.app_config");

    auto b = lc::BuildLaunchConfigurationBlock("2026-11-21T09:00:00Z");
    check(!throws(b), "unchanged block validates");
    check(b.at("configuration_changed_since_launch") == false && b.at("changed_since_launch").empty(),
          "nothing changed since launch");
    check(b.at("camera_preset").at("at_launch").at("sha256") == launch_digest.sha256 &&
              b.at("camera_preset").at("on_disk_at_record_start").at("sha256") == launch_digest.sha256 &&
              b.at("camera_preset").at("at_launch").at("files").size() == 2 &&
              b.at("camera_preset").at("at_launch").at("files")[0].at("name") == "Cam2010093.json",
          "camera preset digests and file list");

    // Edits after launch: the process still runs on the launch bytes.
    write(preset / "Cam2010094.json", "{\"exposure\": 150}\n");
    write(preset / "Cam2010095.json", "{}");
    write(app, "{\"recording\": {\"clip_seconds\": 60}}");
    b = lc::BuildLaunchConfigurationBlock("2026-11-21T10:00:00Z");
    check(!throws(b), "changed block validates");
    check(b.at("configuration_changed_since_launch") == true &&
              b.at("changed_since_launch") == nlohmann::json({"camera_preset", "app_config"}),
          "both changes listed");
    check(b.at("camera_preset").at("changed_files") == nlohmann::json({"Cam2010094.json", "Cam2010095.json"}),
          "changed and added camera JSONs named");
    check(b.at("camera_preset").at("at_launch").at("sha256") == launch_digest.sha256,
          "launch digest kept after the edit");
    check(b.at("app_config").at("changed") == true &&
              b.at("app_config").at("loaded_sha256") != b.at("app_config").at("on_disk_sha256"),
          "app config change recorded with both digests");
    check(lc::LaunchConfigurationStatusJson().at("camera_preset").at("sha256") == launch_digest.sha256,
          "status keeps reporting the launch digest");

    // Removal: the folder is gone at record start.
    fs::remove_all(preset);
    fs::remove(app);
    b = lc::BuildLaunchConfigurationBlock("2026-11-21T11:00:00Z");
    check(!throws(b) && b.at("camera_preset").at("on_disk_at_record_start").at("sha256").is_null() &&
              b.at("camera_preset").at("on_disk_at_record_start").at("error").is_string() &&
              b.at("app_config").at("on_disk_status") == "missing" &&
              b.at("configuration_changed_since_launch") == true,
          "removed preset and app config recorded as changed");

    // A camera reopen replaces the record.
    const fs::path preset2 = make_preset("250_cam4_ptp_fourcam");
    lc::RecordLaunchCameraPreset(preset2.string() + "/", "2026-11-21T12:00:00Z");
    check(lc::LaunchConfigurationStatusJson().at("camera_preset").at("name") == "250_cam4_ptp_fourcam",
          "reopen replaces the preset record (trailing slash tolerated)");

    // Validator refusals.
    auto t = lc::BuildLaunchConfigurationBlock("2026-11-21T12:00:00Z");
    check(!throws(t), "reopened block validates");
    t["configuration_changed_since_launch"] = !t["configuration_changed_since_launch"].get<bool>();
    check(throws(t), "flag disagreeing with changed_since_launch refused");
    t = lc::BuildLaunchConfigurationBlock("2026-11-21T12:00:00Z");
    t["camera_preset"]["at_launch"]["sha256"] = "abc";
    check(throws(t), "bare digest refused");
    t = lc::BuildLaunchConfigurationBlock("2026-11-21T12:00:00Z");
    t["extra"] = 1;
    check(throws(t), "unknown key refused");

    fs::remove_all(preset2.parent_path());
    fs::remove_all(preset.parent_path());
    fs::remove_all(app_dir);
    lc::ResetLaunchConfigurationForTests();
}

void write_samples(const fs::path& out) {
    const fs::path preset = make_preset("100_cam4_ptp_fourcam");
    const fs::path app_dir = fresh_dir("sample_app");
    const fs::path app = app_dir / "default.json";
    write(app, "{}");
    auto emit = [&](const std::string& name) {
        const auto block = lc::BuildLaunchConfigurationBlock("2026-11-21T09:00:00Z");
        lc::ValidateEmittedLaunchConfiguration(block);
        std::ofstream(out / ("launch_configuration_" + name + ".json")) << block.dump(2) << "\n";
        std::ofstream(out / ("recording_snapshot_start.launch_configuration_" + name + ".json"))
            << nlohmann::json({{"session", {{"launch_configuration", block}}}}).dump(2) << "\n";
    };
    lc::ResetLaunchConfigurationForTests();
    emit("headless_none");
    lc::RecordLaunchCameraPreset(preset.string(), "2026-11-21T08:00:00Z");
    lc::RecordLaunchAppConfig(app.string(), "2026-11-21T07:59:00Z");
    emit("unchanged");
    write(preset / "Cam2010094.json", "{\"exposure\": 150}\n");
    write(app, "{\"x\": 1}");
    emit("changed");
    fs::remove_all(preset);
    fs::remove(app);
    emit("removed");
    lc::RecordLaunchAppConfig("", "2026-11-21T07:59:00Z");
    lc::RecordLaunchCameraPreset((app_dir / "nothing").string(), "2026-11-21T08:00:00Z");
    emit("not_configured_and_unreadable_preset");
    fs::remove_all(preset.parent_path());
    fs::remove_all(app_dir);
    lc::ResetLaunchConfigurationForTests();
}
}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--digest") {
        const auto d = lc::ComputeCameraConfigDirDigest(argv[2]);
        if (!d.ok) {
            std::cout << "error: " << d.error << std::endl;
            return 2;
        }
        std::cout << d.sha256 << std::endl;
        return 0;
    }
    if (argc == 2) {
        write_samples(argv[1]);
        return 0;
    }
    test_digest_rule();
    test_block_and_status();
    std::cout << (failures ? "FAILED" : "ALL PASS") << " (" << failures << " failures)" << std::endl;
    return failures ? 1 : 0;
}
