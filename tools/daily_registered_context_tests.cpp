#include "daily_registered_context.h"
#include "gui_recording_evidence.h"
#include <sched.h>
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
void replace(const fs::path& path, const json& value) {
    // Authority files are deliberately read-only; tamper by replacing a directory
    // entry, not by weakening production permissions for the test.
    fs::rename(path, path.string() + "_before_tamper"); put(path, value);
}
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
struct ReuseFixture : Fixture {
    json ref, config, start;
    fs::path recording_root;
    orange::recording::RegisteredContextSet contexts;
    std::vector<orange::recording::RegisteredContextCamera> cameras;
    ReuseFixture() {
        ref = WriteDailyRegisteredContext(plan, frames);
        recording_root = root / "new_recording"; fs::create_directory(recording_root);
        config = {{"schema_version", 2}, {"enabled", true}, {"worker_cpu_ids", {0}}, {"declaration", plan.declaration},
            {"source", {{"kind", "daily_registration"}, {"descriptor_path", (plan.output_root / "context.json").string()},
                {"size_bytes", ref.at("size_bytes")}, {"sha256", ref.at("sha256")}, {"scene_unchanged_since_capture", true}}}};
        for (const auto& item : plan.cameras.items()) cameras.push_back({item.key(), "new-producer-" + item.key(),
            item.value().at("camera_id").get<uint64_t>(), 9, 8, 4, item.value()});
    }
    void prepare() {
        const auto parsed = orange::recording::RegisteredContextConfig::Parse(config);
        check(orange::recording::RegisteredContextConfig::Parse(parsed.ToJson()).ToJson() == parsed.ToJson(), "reuse config roundtrip changed");
        contexts.Prepare(parsed, recording_root, cameras, plan.geometry); start = contexts.StartEvidence(); save_start();
    }
    void save_start() {
        json masters = json::array();
        for (const auto& camera : cameras) masters.push_back({{"camera_serial", camera.serial}, {"recording_id", recording_root.filename().string()},
            {"producer_instance_id", camera.producer_instance_id}, {"stream_generation", camera.stream_generation}});
        put(recording_root / "recording_snapshot_start.json", {{"session", {{"registered_scene_context", start}, {"master_frame_journal", {{"cameras", masters}}}}}});
    }
    json parent(const std::string& state = "completed") {
        json p = {{"session_id", recording_root.filename().string()}, {"status", state}};
        orange::recording::ApplyRequiredRegisteredContextGate(recording_root, &p); return p;
    }
};
void reuse_lifecycle() {
    ReuseFixture f; f.prepare();
    check(f.contexts.Complete(), "verified reuse requires a new source frame");
    check(f.parent("running").at("registered_scene_context").at("status") == "pending", "reuse finalized before recording completion");
    check(f.parent().at("status") == "completed", "valid reuse failed completion");
    check(f.start.at("config").at("source") == f.config.at("source"), "resolved selection not persisted");
    const auto archive = f.recording_root / "registered_daily_context";
    check(get(archive / "context.json") == get(f.plan.output_root / "context.json"), "import relabeled old image identity");
    check(!get(archive / "context.json").at("cameras")[0].contains("producer_instance_id"), "capture assigned to new producer");
    check(get(f.recording_root / "registered_context_use_v1.json").at("recording_cameras")[0].at("producer_instance_id") == f.cameras[0].producer_instance_id,
        "use receipt lost new recording binding");
    for (const auto& frame : f.frames) {
        std::ifstream in(archive / ("Cam" + frame.source.camera_serial + "_native.raw"), std::ios::binary);
        check(std::string(std::istreambuf_iterator<char>(in), {}) == std::string(frame.source.mono8.begin(), frame.source.mono8.end()), "imported pixels changed");
    }
    fs::rename(f.plan.output_root, f.root / "source_unavailable");
    fs::rename(f.plan.registration_path, f.root / "registration_unavailable.json");
    check(f.parent().at("status") == "completed", "finalization requires original calibration paths");
    refuses([&] { f.contexts.Accept(f.frames[0].source); });
}
void reuse_admission_refusal() {
    for (int i = 0; i < 14; ++i) {
        ReuseFixture f;
        if (i == 0) f.config["source"]["scene_unchanged_since_capture"] = false;
        if (i == 1) f.config["source"]["sha256"] = "sha256:" + std::string(64, '0');
        if (i == 2) f.config["source"]["size_bytes"] = 1;
        if (i == 3) f.cameras[0].camera_configuration["exposure_us"] = 3;
        if (i == 4) f.cameras[0].width = 9;
        if (i == 5) f.cameras.pop_back();
        if (i == 6) f.cameras[1] = f.cameras[0];
        if (i == 7) f.plan.geometry["cameras"][f.cameras[0].serial]["daily_registration_geometry"]["registration_id"] = "new-reg";
        if (i == 8) f.plan.geometry["cameras"][f.cameras[0].serial]["daily_registration_geometry"]["recording_snapshot_entry"]["changed_geometry"] = true;
        if (i == 9) f.plan.geometry["cameras"][f.cameras[0].serial]["physical_registration"]["mode"] = "selected_physical_registration";
        if (i == 10) replace(f.plan.output_root / "Cam02010093_native.raw", "changed");
        if (i == 11) fs::create_hard_link(f.plan.output_root / "context.json", f.root / "context_alias.json");
        if (i == 12) f.cameras[0].producer_instance_id.clear();
        if (i == 13) f.config["declaration"]["nir_illumination_fixed"] = false;
        refuses([&] { f.prepare(); });
        check(!fs::exists(f.recording_root / "registered_daily_context"), "bad reuse created an archive");
    }
}
void reuse_finalization_refusal() {
    for (int i = 0; i < 12; ++i) {
        ReuseFixture f; f.prepare();
        const auto archive = f.recording_root / "registered_daily_context";
        if (i == 0) replace(archive / "Cam02010093_native.raw", "changed");
        if (i == 1) replace(archive / "context.json", json::object());
        if (i == 2) replace(archive / "geometry.json", json::object());
        if (i == 3) fs::create_hard_link(archive / "registration.json", f.root / "hardlink.json");
        if (i == 4) replace(f.recording_root / "registered_context_use_v1.json", json::object());
        if (i == 5) { f.start["cameras"][0]["producer_instance_id"] = "wrong"; f.save_start(); }
        if (i == 6) { f.start["daily_context"]["directory"] = "../other"; f.save_start(); }
        if (i == 7) { f.start["config"]["source"]["scene_unchanged_since_capture"] = false; f.save_start(); }
        if (i == 8) { f.cameras[0].stream_generation = 10; f.save_start(); }
        if (i == 9) { fs::rename(archive, f.root / "aliased_archive"); fs::create_directory_symlink(f.root / "aliased_archive", archive); }
        if (i == 10) replace(f.recording_root / "registered_context_geometry_v1.json", json::object());
        if (i == 11) { f.start["profile"] = 12; f.save_start(); }
        put(f.recording_root / "recording_snapshot.json", json::object());
        check(f.parent().at("status") == "failed" && f.parent().at("registered_scene_context").at("status") == "failed", "altered reuse evidence completed parent");
    }
}
void daily_reader_closed_schema() {
    for (int i = 0; i < 10; ++i) {
        ReuseFixture f;
        const auto path = f.plan.output_root / "context.json";
        auto d = get(path);
        if (i == 0) d["extra"] = true;
        if (i == 1) d["schema_version"] = 4294967297ULL;
        if (i == 2) d["cameras"][0]["source_frame"]["recording_frame_id"] = 4;
        if (i == 3) d["cameras"][0]["source_frame"]["local_frame_id"] = 1.5;
        if (i == 4) d["cameras"][0]["image"]["relative_path"] = "../outside.raw";
        if (i == 5) d["cameras"][0]["camera_configuration"]["typo"] = true;
        if (i == 6) d["cameras"][0]["stride_bytes"] = 9;
        if (i == 7) d["housekeeping_cpu"] = 4294967297ULL;
        if (i == 8) d["recording_binding"] = "current_recording";
        if (i == 9) d["cameras"].push_back(d["cameras"][0]);
        replace(path, d);
        std::string sha, error; check(orange::gui::spatial_layout::checksum::file_sha256(path, &sha, &error), "rehash failed");
        refuses([&] { ReadDailyRegisteredContext(f.plan.output_root, {"context.json", fs::file_size(path), sha}); });
    }
}
int available_cpu() {
    cpu_set_t cpus; CPU_ZERO(&cpus);
    check(sched_getaffinity(0, sizeof(cpus), &cpus) == 0, "read affinity failed");
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) if (CPU_ISSET(cpu, &cpus)) return cpu;
    throw std::runtime_error("no allowed CPU");
}
void gui_config_and_required_evidence() {
    using namespace orange::recording;
    ReuseFixture f;
    f.config["worker_cpu_ids"] = {available_cpu()};
    MasterAcquisitionConfig master; master.enabled = true; master.writer_cpu_ids = {available_cpu()};
    json config = {{"schema_version", 1}, {"enabled", true}, {"master_frame_journal", master.ToJson()},
                   {"registered_scene_context", f.config}};
    const auto parsed = GuiRecordingEvidenceConfig::Parse(config);
    check(GuiRecordingEvidenceConfig::Parse(parsed.ToJson()).ToJson() == parsed.ToJson(), "GUI config roundtrip changed");
    check(!GuiRecordingEvidenceConfig::Parse({{"schema_version", 1}, {"enabled", false}}).enabled, "GUI default enabled");
    const auto settings = f.root / "app.json";
    check(!ReadGuiRecordingEvidenceConfig(settings).enabled, "absent app config enabled GUI evidence");
    const json unrelated = {{"storage", {{"recording_root", "/untouched"}}},
        {"recording", {{"sink_mode", "external_ipc"}, {"clip_seconds", 5}}}};
    put(settings, unrelated);
    SaveGuiRecordingEvidenceConfig(settings, parsed);
    check(ReadGuiRecordingEvidenceConfig(settings).ToJson() == parsed.ToJson(), "saved GUI settings did not reload");
    const auto stored = get(settings);
    check(stored.at("storage") == unrelated.at("storage") && stored.at("recording").at("sink_mode") == "external_ipc" &&
        stored.at("recording").at("clip_seconds") == 5, "context save changed other recording settings");
    auto disabled = parsed; disabled.enabled = false;
    SaveGuiRecordingEvidenceConfig(settings, disabled);
    check(!ReadGuiRecordingEvidenceConfig(settings).enabled &&
        ReadGuiRecordingEvidenceConfig(settings).context.ToJson() == parsed.context.ToJson(), "disable lost saved context selection");
    auto invalid = stored; invalid["recording"]["registered_context_recording"]["enabled"] = "true";
    put(settings, invalid);
    refuses([&] { ReadGuiRecordingEvidenceConfig(settings); });
    SaveGuiRecordingEvidenceConfig(settings, parsed); // explicit operator repair of this field
    for (int i = 0; i < 9; ++i) {
        auto bad = config;
        if (i == 0) bad["typo"] = true;
        if (i == 1) bad["schema_version"] = true;
        if (i == 2) bad["schema_version"] = 4294967297ULL;
        if (i == 3) bad["enabled"] = 1;
        if (i == 4) bad.erase("master_frame_journal");
        if (i == 5) bad["master_frame_journal"]["enabled"] = false;
        if (i == 6) bad["registered_scene_context"]["source"] = {{"kind", "fresh_capture"}};
        if (i == 7) bad["registered_scene_context"]["source"]["scene_unchanged_since_capture"] = false;
        if (i == 8) bad["master_frame_journal"]["writer_cpu_ids"] = json::array();
        refuses([&] { GuiRecordingEvidenceConfig::Parse(bad); });
    }
    GuiRecordingEvidence evidence;
    evidence.Prepare(parsed, f.recording_root, f.cameras, f.plan.geometry);
    check(evidence.artifacts.at("gui_registered_context_recording") == parsed.ToJson(), "resolved GUI config missing");
    put(f.recording_root / "recording_snapshot_start.json", {{"session", evidence.artifacts}});
    json parent = {{"session_id", f.recording_root.filename().string()}, {"status", "completed"}};
    auto unsealed = parent;
    ApplyRequiredMasterJournalGate(f.recording_root, &unsealed);
    check(unsealed.at("status") == "failed", "unsealed GUI journal accepted");
    for (const auto& camera : f.cameras) {
        auto* journal = evidence.journals->Find(camera.serial);
        check(journal->ProducerInstanceId() != camera.producer_instance_id, "GUI retained old producer identity");
        MasterFrameFact frame; frame.recording_frame_id = 1;
        frame.local_frame_id = 912; frame.local_frame_id_present = true;
        frame.camera_frame_id = 1704; frame.camera_frame_id_present = true;
        frame.camera_timestamp_ns = 1800000000000001234ULL; frame.camera_timestamp_present = true;
        MasterAcquisitionJournal::Iteration iteration(journal, true); iteration.Submit(frame);
    }
    std::string error;
    check(evidence.journals->Finalize(true, &error), "GUI journal did not seal");
    ApplyRequiredMasterJournalGate(f.recording_root, &parent);
    ApplyRequiredRegisteredContextGate(f.recording_root, &parent);
    if (parent.at("status") != "completed") std::cerr << parent.dump(2) << '\n';
    check(parent.at("status") == "completed" && parent.at("registered_scene_context").at("status") == "captured",
        "GUI context and master evidence did not complete together");
    // Finalization no longer depends on the original daily directory.
    fs::rename(f.plan.output_root, f.root / "daily_moved");
    ApplyRequiredRegisteredContextGate(f.recording_root, &parent);
    check(parent.at("status") == "completed", "GUI finalization depended on external daily asset");
}
}
int main() {
    try { success_and_custody(); selection_and_geometry_refusal(); input_refusal(); immutable_source_refusal();
        reuse_lifecycle(); reuse_admission_refusal(); reuse_finalization_refusal(); daily_reader_closed_schema();
        gui_config_and_required_evidence();
        std::cout << "9 daily native context/cross-recording reuse/GUI CPU groups passed\n"; return 0;
    } catch (const std::exception& ex) { std::cerr << ex.what() << '\n'; return 1; }
}
