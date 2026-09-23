#include "recording_registered_context.h"
#include <fstream>
#include <functional>
#include <iostream>
#include <unistd.h>

namespace {
using namespace orange::recording;
namespace fs = std::filesystem;
using json = nlohmann::json;
void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
void refuses(const std::function<void()>& work) {
    bool threw = false; try { work(); } catch (const std::exception&) { threw = true; }
    check(threw, "unsafe registered context accepted");
}
void put(const fs::path& path, const json& value) {
    std::ofstream out(path); out << value.dump(); out.close(); check(!out.fail(), "fixture write failed");
}
json declaration() {
    return {{"schema_id", "orange.recording.registered_scene_context.capture_declaration"}, {"schema_version", 1},
        {"registration_authority_status", "accepted_for_experiment"}, {"subject_presence", "present"},
        {"dish_setup_complete", true}, {"nir_illumination_fixed", true}, {"camera_configuration_fixed", true}, {"rig_fixed", true}};
}
RegisteredContextConfig config() {
    return RegisteredContextConfig::Parse({{"schema_version", 1}, {"enabled", true}, {"worker_cpu_ids", {0}}, {"declaration", declaration()}});
}
struct Fixture {
    fs::path root;
    json geometry, start;
    RegisteredContextSet contexts;
    std::vector<RegisteredContextCamera> cameras = {{"02010093", "epoch-A", 0, 3, 8, 4}, {"02010094", "epoch-B", 1, 0, 8, 4}};
    Fixture() {
        std::string name = (fs::temp_directory_path() / "registered_context_test_XXXXXX").string();
        check(mkdtemp(name.data()) != nullptr, "fixture mkdtemp failed"); root = name;
        geometry = {{"schema_id", "orange.recording.geometry_contract"}, {"schema_version", 1}, {"cameras", json::object()}};
        for (const auto& c : cameras) geometry["cameras"][c.serial] = {
            {"physical_registration", {{"status", "selected_resolved"}, {"mode", "selected_physical_registration"},
                {"recording_snapshot_entry", {{"artifact_id", "accepted-rim"}, {"camera_serial", c.serial},
                    {"camera", {{"width", 8}, {"height", 4}, {"pixel_format", "Mono8"}}}}}}}};
    }
    ~Fixture() { std::error_code ec; fs::remove_all(root, ec); }
    void Prepare(const RegisteredContextConfig& context_config = config()) {
        contexts.Prepare(context_config, root, cameras, geometry); start = contexts.StartEvidence();
        json masters = json::array();
        for (const auto& c : cameras) masters.push_back({{"camera_serial", c.serial}, {"recording_id", root.filename().string()},
            {"producer_instance_id", c.producer_instance_id}, {"stream_generation", c.stream_generation}});
        put(root / "recording_snapshot_start.json", {{"session", {{"registered_scene_context", start}, {"master_frame_journal", {{"cameras", masters}}}}}});
    }
    RegisteredContextFrame frame(std::size_t index) {
        RegisteredContextFrame f; f.camera_serial = cameras.at(index).serial; f.width = 8; f.height = 4;
        f.local_frame_id = 5 + index; f.camera_frame_id = 250 + index;
        f.camera_timestamp_ns = 1700000000000000001ULL + index; f.timestamp_sys_ns = 1700000000000000101ULL + index;
        f.mono8.resize(32); for (std::size_t i = 0; i < 32; ++i) f.mono8[i] = static_cast<unsigned char>(i + index);
        return f;
    }
    json Parent(const std::string& status = "completed") {
        json p = {{"session_id", root.filename().string()}, {"status", status}};
        ApplyRequiredRegisteredContextGate(root, &p); return p;
    }
};
void strict_config() {
    check(!RegisteredContextConfig::Parse(RegisteredContextConfig{}.ToJson()).enabled, "default must be disabled");
    const auto valid = config().ToJson();
    check(RegisteredContextConfig::Parse(valid).ToJson() == valid, "config roundtrip mismatch");
    for (auto bad : {json(true), json(2), json(4294967297ULL), json(-1), json(1.5), json("1")}) {
        auto v = valid; v["schema_version"] = bad; refuses([&] { RegisteredContextConfig::Parse(v); });
        v = valid; v["declaration"]["schema_version"] = bad; refuses([&] { RegisteredContextConfig::Parse(v); });
    }
    for (int test = 0; test < 7; ++test) {
        auto v = valid;
        if (test == 0) v["typo"] = true;
        if (test == 1) v["worker_cpu_ids"] = json::array();
        if (test == 2) v["worker_cpu_ids"] = {0, 0};
        if (test == 3) v["timeout_ms"] = 0;
        if (test == 4) v["declaration"]["registration_authority_status"] = "diagnostic_not_physical_acceptance";
        if (test == 5) v["declaration"]["rig_fixed"] = false;
        if (test == 6) v.erase("declaration");
        refuses([&] { RegisteredContextConfig::Parse(v); });
    }
    auto absent = valid; absent["declaration"]["subject_presence"] = "absent"; RegisteredContextConfig::Parse(absent);
    absent["declaration"]["subject_presence"] = "unknown"; RegisteredContextConfig::Parse(absent);
}
void lifecycle() {
    Fixture f; f.Prepare(); check(!f.contexts.Complete(), "prearm treated as captured");
    check(f.Parent("running")["registered_scene_context"]["status"] == "pending", "running context not pending");
    check(f.Parent()["status"] == "failed", "missing snapshots completed parent");
    f.contexts.Accept(f.frame(0)); check(!f.contexts.Complete(), "partial camera set completed");
    check(f.Parent()["status"] == "failed", "partial snapshots completed parent");
    f.contexts.Accept(f.frame(1)); check(f.contexts.Complete(), "all snapshots not complete");
    check(f.Parent()["status"] == "completed" && f.Parent()["registered_scene_context"]["status"] == "captured", "captured context gate failed");
    refuses([&] { f.contexts.Accept(f.frame(0)); });
    std::ifstream in(f.root / "Cam02010093_registered_context.raw", std::ios::binary);
    const std::string actual{std::istreambuf_iterator<char>(in), {}};
    const auto expected = f.frame(0).mono8;
    check(actual == std::string(expected.begin(), expected.end()), "native pixels changed");
    put(f.root / "recording_snapshot.json", json::object());
    fs::rename(f.root / "Cam02010093_registered_context.raw", f.root / "original.raw");
    std::ofstream altered(f.root / "Cam02010093_registered_context.raw"); altered << "changed"; altered.close();
    check(f.Parent()["status"] == "failed", "mutable refresh bypassed changed required image");
    Fixture v2; auto v2_config = config(); v2_config.schema_version = 2;
    v2.Prepare(v2_config); v2.contexts.Accept(v2.frame(0)); v2.contexts.Accept(v2.frame(1));
    json snapshot; { std::ifstream in(v2.root / "recording_snapshot_start.json"); in >> snapshot; }
    auto& cfg = snapshot["session"]["registered_scene_context"]["config"];
    check(cfg.at("schema_version") == 2 && cfg.at("source").at("kind") == "fresh_capture", "v2 fresh source not persisted");
    check(v2.Parent()["status"] == "completed", "v2 explicit fresh configuration refused");
    cfg["source"] = {{"kind", "daily_registration"}, {"descriptor_path", "/a/context.json"},
        {"size_bytes", 1}, {"sha256", "sha256:" + std::string(64, 'a')}, {"scene_unchanged_since_capture", true}};
    put(v2.root / "recording_snapshot_start.json", snapshot);
    check(v2.Parent()["status"] == "failed", "fresh evidence accepted a reuse configuration without reuse proof");
}
void geometry_and_source_refusal() {
    for (int test = 0; test < 5; ++test) {
        Fixture f; auto& p = f.geometry["cameras"][f.cameras[0].serial]["physical_registration"];
        if (test == 0) p["status"] = "not_performed";
        if (test == 1) p["recording_snapshot_entry"]["camera"]["width"] = 9;
        if (test == 2) p["recording_snapshot_entry"]["camera_serial"] = "different";
        if (test == 3) p["recording_snapshot_entry"]["camera"]["pixel_format"] = "BayerRG8";
        if (test == 4) p["recording_snapshot_entry"]["artifact_id"] = "";
        refuses([&] { f.Prepare(); });
    }
    for (int test = 0; test < 5; ++test) {
        Fixture f; f.Prepare(); auto frame = f.frame(0);
        if (test == 0) frame.recording_frame_id = 1;
        if (test == 1) frame.camera_timestamp_ns = 0;
        if (test == 2) frame.width = 9;
        if (test == 3) frame.mono8.pop_back();
        if (test == 4) frame.camera_serial = "unknown";
        refuses([&] { f.contexts.Accept(frame); }); check(!f.contexts.Complete(), "rejected source became complete");
    }
    Fixture daily;
    for (auto& item : daily.geometry["cameras"].items()) {
        auto selected = item.value()["physical_registration"]; selected["status"] = "resolved"; selected["mode"] = "selected_daily_registration";
        item.value() = {{"daily_registration_geometry", selected}};
    }
    daily.Prepare(); daily.contexts.Accept(daily.frame(0)); daily.contexts.Accept(daily.frame(1));
    check(daily.Parent()["status"] == "completed", "Citrus daily registration refused");
}
void custody() {
    Fixture f; f.Prepare();
    fs::create_symlink("missing", f.root / "Cam02010093_registered_context.raw");
    refuses([&] { f.contexts.Accept(f.frame(0)); });
    Fixture renamed; renamed.Prepare(); const auto saved = renamed.root.string() + "_saved";
    fs::rename(renamed.root, saved); fs::create_directory(renamed.root);
    refuses([&] { renamed.contexts.Accept(renamed.frame(0)); });
    fs::remove(renamed.root); fs::rename(saved, renamed.root);
    Fixture existing; { std::ofstream out(existing.root / "Cam02010093_registered_context.raw"); out << "user image"; }
    refuses([&] { existing.Prepare(); });
    Fixture normal; auto p = normal.Parent(); check(!p.contains("registered_scene_context") && p["status"] == "completed", "default recording changed");
}
void finalized_evidence_refusal() {
    for (int test = 0; test < 9; ++test) {
        Fixture f; f.Prepare(); f.contexts.Accept(f.frame(0)); f.contexts.Accept(f.frame(1));
        const auto descriptor_path = f.root / "Cam02010093_registered_context_v2.json";
        json descriptor; { std::ifstream in(descriptor_path); in >> descriptor; }
        if (test < 6) {
            if (test == 0) descriptor["source_frame"]["recording_frame_id"] = 1;
            if (test == 1) descriptor["source_frame"]["camera_timestamp_ns"] = 1.5;
            if (test == 2) descriptor["camera_binding"]["camera_id"] = 99;
            if (test == 3) descriptor["image"]["relative_path"] = "../other.raw";
            if (test == 4) descriptor["native_raster"]["stride_bytes"] = 9;
            if (test == 5) descriptor["invented_field"] = true;
            fs::rename(descriptor_path, f.root / "original_descriptor.json"); put(descriptor_path, descriptor);
        } else if (test == 6) {
            // Hardlink aliases violate the create-once authority custody rule.
            fs::create_hard_link(f.root / "Cam02010093_registered_context.raw", f.root / "aliased.raw");
        } else {
            auto path = f.root / "recording_snapshot_start.json";
            json start; { std::ifstream in(path); in >> start; }
            if (test == 7) start["session"]["registered_scene_context"]["cameras"][1] =
                start["session"]["registered_scene_context"]["cameras"][0];
            if (test == 8) start["session"]["master_frame_journal"]["cameras"][0]["producer_instance_id"] = "different-producer";
            put(path, start);
        }
        const auto parent = f.Parent();
        check(parent["status"] == "failed" && parent["registered_scene_context"]["status"] == "failed",
              "malformed final context passed parent gate");
    }
}
}
int main() {
    try { strict_config(); lifecycle(); geometry_and_source_refusal(); custody(); finalized_evidence_refusal();
        std::cout << "5 registered context CPU groups passed\n"; return 0;
    } catch (const std::exception& ex) { std::cerr << ex.what() << '\n'; return 1; }
}
