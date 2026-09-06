#include "daily_registered_context.h"
#include "gui/spatial_layout/sha256.h"
#include <fstream>
#include <functional>
#include <iostream>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace orange::calibration;
void check(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
void refuses(const std::function<void()>& call) {
    bool rejected = false; try { call(); } catch (const std::exception&) { rejected = true; }
    check(rejected, "invalid daily context accepted");
}
void put(const fs::path& path, const json& value) { std::ofstream file(path); file << value.dump(); file.close(); check(!file.fail(), "fixture write failed"); }
json get(const fs::path& path) { std::ifstream file(path); json result; file >> result; return result; }
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
void success_and_custody() {
    Fixture f;
    const auto ref = WriteDailyRegisteredContext(f.plan, f.frames);
    check(ref.at("relative_path") == "context.json", "descriptor name changed");
    auto descriptor = get(f.plan.output_root / "context.json");
    check(descriptor.at("recording_binding") == "unbound" && !descriptor.contains("recording_id"), "daily artifact invented a recording identity");
    check(descriptor.at("scene_declaration").at("subject_presence") == "present" && descriptor.at("cameras").size() == 2, "daily subject/camera scope changed");
    check(descriptor.at("cameras")[0].at("camera_configuration").at("camera_id") == 0, "camera index zero rejected");
    check(get(f.plan.output_root / "registration.json") == f.registration && get(f.plan.registration_path) == f.registration, "accepted registration was changed");
    for (const auto& frame : f.frames) {
        std::ifstream in(f.plan.output_root / ("Cam" + frame.source.camera_serial + "_native.raw"), std::ios::binary);
        const std::string bytes{std::istreambuf_iterator<char>(in), {}};
        check(bytes == std::string(frame.source.mono8.begin(), frame.source.mono8.end()), "native bytes changed");
    }
    refuses([&] { WriteDailyRegisteredContext(f.plan, f.frames); });
    check(get(f.plan.output_root / "context.json") == descriptor, "retry overwrote existing capture");
}
void selection_and_geometry_refusal() {
    for (int i = 0; i < 9; ++i) {
        Fixture f;
        if (i == 0) f.plan.runtime_before["runtime"]["blocking"] = true;
        if (i == 1) f.plan.runtime_after["runtime"]["targets"][0]["registration_sha256"] = "sha256:changed";
        if (i == 2) f.plan.runtime_after["runtime"]["runtime_geometry_matches_selection"] = false;
        if (i == 3) f.plan.runtime_after["runtime"]["targets"].erase(0);
        if (i == 4) f.plan.geometry["cameras"]["02010093"]["daily_registration_geometry"]["recording_snapshot_entry"]["camera"]["width"] = 9;
        if (i == 5) f.plan.geometry["cameras"]["02010093"]["daily_registration_geometry"]["registration_id"] = "stale";
        if (i == 6) f.plan.geometry["cameras"]["02010093"]["daily_registration_geometry"]["status"] = "unavailable";
        if (i == 7) { f.registration["status"] = "candidate"; f.sync_registration(); }
        if (i == 8) { f.registration["targets"][1] = f.registration["targets"][0]; f.sync_registration(); }
        refuses([&] { WriteDailyRegisteredContext(f.plan, f.frames); });
        check(!fs::exists(f.plan.output_root), "invalid selection/geometry wrote a capture directory");
    }
}
void input_refusal() {
    for (int i = 0; i < 12; ++i) {
        Fixture f;
        if (i == 0) f.plan.declaration["subject_presence"] = "invented";
        if (i == 1) f.plan.declaration["schema_version"] = 4294967297ULL;
        if (i == 2) f.plan.declaration["rig_fixed"] = false;
        if (i == 3) f.plan.housekeeping_cpu = -1;
        if (i == 4) f.frames[0].source.recording_frame_id = 1;
        if (i == 5) f.frames[0].source_storage = "camera_dma";
        if (i == 6) f.frames[0].source.mono8.pop_back();
        if (i == 7) f.frames[0].source.camera_timestamp_ns = 0;
        if (i == 8) f.plan.cameras["02010093"]["exposure_us"] = 10.5;
        if (i == 9) f.plan.cameras["02010093"]["typo"] = 1;
        if (i == 10) f.frames.pop_back();
        if (i == 11) f.frames[1].source.camera_serial = f.frames[0].source.camera_serial;
        refuses([&] { WriteDailyRegisteredContext(f.plan, f.frames); });
        check(!fs::exists(f.plan.output_root), "invalid inputs wrote a capture directory");
    }
}
void immutable_source_refusal() {
    Fixture changed; { std::ofstream out(changed.plan.registration_path); out << "changed"; }
    refuses([&] { WriteDailyRegisteredContext(changed.plan, changed.frames); });
    Fixture alias; fs::create_hard_link(alias.plan.registration_path, alias.root / "alias.json");
    refuses([&] { WriteDailyRegisteredContext(alias.plan, alias.frames); });
    Fixture link; fs::rename(link.plan.registration_path, link.root / "original.json");
    fs::create_symlink(link.root / "original.json", link.plan.registration_path);
    refuses([&] { WriteDailyRegisteredContext(link.plan, link.frames); });
    Fixture parent; fs::create_directory(parent.root / "real_parent");
    fs::create_directory_symlink(parent.root / "real_parent", parent.root / "aliased_parent");
    parent.plan.output_root = parent.root / "aliased_parent" / parent.plan.capture_id;
    refuses([&] { WriteDailyRegisteredContext(parent.plan, parent.frames); });
    check(!fs::exists(parent.root / "real_parent" / parent.plan.capture_id), "symlink parent created an unauthorized directory");
}
}
int main() {
    try { success_and_custody(); selection_and_geometry_refusal(); input_refusal(); immutable_source_refusal();
        std::cout << "4 daily native context CPU groups passed\n"; return 0;
    } catch (const std::exception& ex) { std::cerr << ex.what() << '\n'; return 1; }
}
