#pragma once
#include "daily_registered_context.h"
#include "gui_recording_evidence.h"
#include <sched.h>
#include "gui/spatial_layout/sha256.h"
#include <fstream>
#include <functional>
#include <iostream>
#include <unistd.h>

namespace orange::test_daily_context {
namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace orange::calibration;
inline void check(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
inline void refuses(const std::function<void()>& call) {
    bool rejected = false; try { call(); } catch (const std::exception&) { rejected = true; }
    check(rejected, "invalid daily context accepted");
}
inline void put(const fs::path& path, const json& value) { std::ofstream file(path); file << value.dump(); file.close(); check(!file.fail(), "fixture write failed"); }
inline void replace(const fs::path& path, const json& value) {
    // Authority files are deliberately read-only; tamper by replacing a directory
    // entry, not by weakening production permissions for the test.
    fs::rename(path, path.string() + "_before_tamper"); put(path, value);
}
inline json get(const fs::path& path) { std::ifstream file(path); json result; file >> result; return result; }
struct Fixture {
    fs::path root;
    DailyContextPlan plan;
    json registration;
    std::vector<DailyContextFrame> frames;
    Fixture() {
        std::string tmp = (fs::temp_directory_path() / "daily_native_context_test_XXXXXX").string();
        check(mkdtemp(tmp.data()) != nullptr, "mkdtemp failed"); root = tmp;
        fs::create_directory(root / "accepted");
        plan.capture_id = "native_context_fixture"; plan.output_root = root / plan.capture_id;
        plan.registration_path = root / "accepted" / "registration.json";
        plan.requested_at_utc = "2026-09-06T16:00:00Z"; plan.housekeeping_cpu = 0;
        plan.declaration = {{"schema_id", "orange.recording.registered_scene_context.capture_declaration"}, {"schema_version", 1},
            {"registration_authority_status", "accepted_for_experiment"}, {"subject_presence", "present"},
            {"dish_setup_complete", true}, {"nir_illumination_fixed", true}, {"camera_configuration_fixed", true}, {"rig_fixed", true}};
        registration = {{"schema_id", "citrus.calibration.daily_registration"}, {"schema_version", 1}, {"status", "accepted"},
            {"registration_id", "daily-reg-1"}, {"targets", json::array()}};
        plan.geometry = {{"schema_id", "orange.recording.geometry_contract"}, {"schema_version", 1}, {"cameras", json::object()}};
        json targets = json::array();
        for (int i = 0; i < 2; ++i) {
            const auto serial = i == 0 ? "02010093" : "02010094";
            plan.cameras[serial] = {{"camera_id", i}, {"width", 8}, {"height", 4}, {"pixel_format", "Mono8"},
                {"frame_rate_hz", 100}, {"exposure_us", 9000}, {"gain", 0}, {"focus", 10}, {"iris", 20}};
            registration["targets"].push_back({{"camera_id", serial}, {"arena_id", "arena_" + std::to_string(i)}});
            targets.push_back({{"camera_id", serial}, {"state", "selected_valid"}, {"applied", true},
                {"registration_path", plan.registration_path.string()}, {"registration_sha256", "filled below"}});
            plan.geometry["cameras"][serial]["daily_registration_geometry"] = {{"status", "resolved"}, {"mode", "selected_daily_registration"},
                {"registration_id", "daily-reg-1"}, {"registration", {{"source_path", plan.registration_path.string()}, {"sha256", "filled below"}}},
                {"recording_snapshot_entry", {{"camera_serial", serial}, {"camera", {{"width", 8}, {"height", 4}, {"pixel_format", "Mono8"}}}}}};
            DailyContextFrame f; f.source.camera_serial = serial; f.source.width = 8; f.source.height = 4;
            f.source.local_frame_id = 17 + i; f.source.camera_frame_id = 117 + i;
            f.source.camera_timestamp_ns = 1800000000000000001ULL + i; f.source.timestamp_sys_ns = 1800000000000001017ULL + i;
            f.source.mono8.resize(32); for (int j = 0; j < 32; ++j) f.source.mono8[j] = static_cast<unsigned char>(i + j);
            f.source_storage = i == 0 ? "pool_owned_ring_device" : "analytics_owned_device";
            frames.push_back(std::move(f));
        }
        plan.runtime_before = {{"runtime", {{"mode", "selected_daily_registration"}, {"state", "selected_valid"},
            {"all_selected_runtime_safe", true}, {"blocking", false}, {"applied", true},
            {"runtime_geometry_matches_selection", true}, {"targets", targets}}}};
        sync_registration();
    }
    ~Fixture() { std::error_code error; fs::remove_all(root, error); }
    void sync_registration() {
        put(plan.registration_path, registration);
        std::string error;
        check(orange::gui::spatial_layout::checksum::file_sha256(plan.registration_path, &plan.registration_sha256, &error), "fixture hash failed");
        for (auto& target : plan.runtime_before["runtime"]["targets"]) target["registration_sha256"] = plan.registration_sha256;
        plan.runtime_after = plan.runtime_before;
        for (auto& camera : plan.geometry["cameras"].items()) camera.value()["daily_registration_geometry"]["registration"]["sha256"] = plan.registration_sha256;
    }
};
} // namespace orange::test_daily_context
