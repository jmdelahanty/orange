#include "recording_master_crop_coverage.h"
#include "recording_master_journal.h"
#include <fstream>
#include <iostream>
#include <functional>
#include <stdexcept>
#include <unistd.h>

namespace {
using namespace orange::recording;
using json = nlohmann::json;
namespace fs = std::filesystem;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void put(const fs::path& path, const std::string& bytes) {
    std::ofstream out(path); out << bytes; out.close(); check(!out.fail(), "fixture write failed");
}
std::string read(const fs::path& path) {
    std::ifstream in(path); return {std::istreambuf_iterator<char>(in), {}};
}
void refuses(const std::function<void()>& action) {
    bool failed = false;
    try { action(); } catch (const std::exception&) { failed = true; }
    check(failed, "incomplete crop evidence accepted");
}
struct Fixture {
    fs::path root;
    std::string serial = "02010093", prefix = "Cam02010093";
    json master, summary;
    std::vector<json> events;
    std::vector<std::string> rows;
    Fixture() {
        std::string name = (fs::temp_directory_path() / "moving_crop_completion_XXXXXX").string();
        check(::mkdtemp(name.data()) != nullptr, "fixture directory failed"); root = name;
        MasterJournalOptions options;
        options.recording_directory = root; options.recording_id = root.filename().string();
        options.camera_serial = serial; options.producer_instance_id = "producer-A";
        MasterFrameJournal journal(options);
        for (uint64_t i = 1; i <= 5; ++i) {
            MasterFrameFact frame;
            frame.recording_frame_id = i;
            frame.local_frame_id_present = frame.camera_frame_id_present = frame.camera_timestamp_present = frame.host_realtime_present = true;
            frame.local_frame_id = i + 7000; frame.camera_frame_id = i + 14000;
            frame.camera_timestamp_ns = 1700000000000000000ULL + i;
            frame.host_realtime_ns = 1700000000000000100ULL + i;
            check(journal.TrySubmit(frame), "fixture master submit failed");
            const bool blank = i == 3;
            rows.push_back(std::to_string(i) + "," + std::to_string(frame.local_frame_id) + "," +
                std::to_string(frame.camera_frame_id) + "," + std::to_string(frame.camera_timestamp_ns) + "," +
                std::to_string(frame.host_realtime_ns) + "," + (blank ? "0,1,blank_no_detection," : "1,0,detected_crop,") +
                std::to_string(i - 1) + "," + std::to_string(i - 1));
            events.push_back({{"schema_id", "orange.yolo_event"}, {"schema_version", 1}, {"event_sequence", i},
                {"event_kind", "yolo_result"}, {"recording_id", options.recording_id}, {"camera_serial", serial},
                {"frame", {{"recording_frame_id", i}, {"local_frame_id", frame.local_frame_id},
                           {"camera_frame_id", frame.camera_frame_id}, {"record_active", true}}},
                {"timestamps", {{"camera_timestamp", frame.camera_timestamp_ns}, {"timestamp_sys_ns", frame.host_realtime_ns}}},
                {"yolo", {{"status", blank ? "zero_detections" : "detections"}, {"detection_count", blank ? 0 : 1},
                    {"coordinate_space", "source_frame_pixels"}, {"model_id", "fixture-model"}, {"detection_source", "model"},
                    {"synthetic_runtime_detection", false}, {"production_detection_valid", true}}},
                {"detections", blank ? json::array() : json::array({json::object()})}});
        }
        master = journal.Finalize(true);
        summary = {{"schema_id", "orange.external_recorder.summary"}, {"schema_version", 2},
            {"session_id", options.recording_id}, {"stream_id", serial + "_crop"}, {"stream_kind", "crop"}, {"output_kind", "crop"},
            {"frames_received", 5}, {"frames_encoded", 5}, {"worker_failed", false}, {"encode_skipped", 0}, {"encode_dropped", 0},
            {"rolling_output", {{"enabled", true}, {"clips", json::array({
                {{"clip_index", 0}, {"clip_id", "clip_000000"}, {"first_recording_frame_id", 1}, {"last_recording_frame_id", 3},
                    {"frame_count", 3}, {"packets_written", 3}, {"failed", false}},
                {{"clip_index", 1}, {"clip_id", "clip_000001"}, {"first_recording_frame_id", 4}, {"last_recording_frame_id", 5},
                    {"frame_count", 2}, {"packets_written", 2}, {"failed", false}}})}}}};
        Save();
    }
    ~Fixture() { std::error_code ec; fs::remove_all(root, ec); }
    void Save() {
        std::string csv = "recording_frame_id,local_frame_id,camera_frame_id,timestamp,timestamp_sys,has_detection,blank_frame,crop_state,crop_video_frame_index,session_crop_video_frame_index\n";
        for (const auto& row : rows) csv += row + "\n";
        put(root / (prefix + "_crop_meta.csv"), csv);
        std::string jsonl;
        for (const auto& event : events) jsonl += event.dump() + "\n";
        put(root / (prefix + "_yolo_events.jsonl"), jsonl);
        put(root / "summary.json", summary.dump());
    }
    json Finish(bool normal = true) { return FinalizeMovingCropMetadata(root, serial, "summary.json", normal); }
    void Marker() {
        put(root / "recording_snapshot_start.json", json({{"session", {
            {"moving_crop_master_coverage", {{"schema_version", 1}, {"required", true}, {"profile", "external_moving_crop_full_rate_v1"}}},
            {"master_frame_journal", {{"cameras", json::array({{{"camera_serial", serial}, {"recording_id", master["recording_id"]},
                {"producer_instance_id", master["producer_instance_id"]}, {"stream_generation", master["stream_generation"]}}})}}}
        }}}).dump());
    }
};
void success() {
    Fixture f;
    const auto original = read(f.root / (f.prefix + "_crop_meta.csv"));
    const auto result = f.Finish();
    check(result["detection_coverage"]["successful_zero_detection_frames"] == 1, "blank coverage wrong");
    check(result["metadata_correspondence"]["clips"][1]["row_count"] == 2, "partial clip wrong");
    const auto second = read(f.root / (f.prefix + "_crop_clip_1_meta_v1.csv"));
    check(second.find("detected_crop,0,3\n") != std::string::npos && second.find("detected_crop,1,4\n") != std::string::npos,
          "clip local index did not reset independently of session index");
    check(original == read(f.root / (f.prefix + "_crop_meta.csv")), "original metadata changed");
    check(result["media_finalization_evaluated"] == false, "metadata receipt claims video validation");
    RequireMovingCropMetadataReceipt(f.root, f.master);
    refuses([&] { f.Finish(); });
    Fixture whole; whole.summary["rolling_output"] = {{"enabled", false}}; whole.Save();
    check(whole.Finish()["metadata_correspondence"]["clips"].size() == 1, "whole crop mapping wrong");
}
void detector_failures() {
    for (const auto* state : {"failed", "timeout", "pending", "not_scheduled"}) {
        Fixture f; f.events[2]["yolo"]["status"] = state; f.Save(); refuses([&] { f.Finish(); });
    }
    for (int mutation = 0; mutation < 10; ++mutation) {
        Fixture f;
        switch (mutation) {
        case 0: f.events.pop_back(); break;
        case 1: f.events.push_back(f.events.back()); break;
        case 2: f.events[1]["frame"]["local_frame_id"] = 2; break;
        case 3: f.events[1]["timestamps"]["camera_timestamp"] = 1700000000000000000ULL; break;
        case 4: f.events[2]["yolo"]["synthetic_runtime_detection"] = true; break;
        case 5: f.events[2]["yolo"]["detection_count"] = 1; break;
        case 6: f.events[2]["event_sequence"] = 4; break;
        case 7: f.events[2]["recording_id"] = "other"; break;
        case 8: f.events[2]["yolo"]["error"] = "technical_failure"; break;
        case 9: f.events[2]["yolo"]["status"] = "detections"; f.events[2]["yolo"]["detection_count"] = 1;
                f.events[2]["detections"] = json::array({json::object()}); break;
        }
        f.Save(); refuses([&] { f.Finish(); });
    }
}
void metadata_failures() {
    for (int mutation = 0; mutation < 7; ++mutation) {
        Fixture f;
        switch (mutation) {
        case 0: f.rows.pop_back(); break;
        case 1: f.rows.erase(f.rows.begin() + 1); break;
        case 2: f.rows[1] = f.rows[0]; break;
        case 3: f.rows.push_back(f.rows.back()); break;
        case 4: f.rows[0].replace(f.rows[0].find("7001"), 4, "7002"); f.events[0]["frame"]["local_frame_id"] = 7002; break;
        case 5: f.rows[0].replace(f.rows[0].find("14001"), 5, "14002"); f.events[0]["frame"]["camera_frame_id"] = 14002; break;
        case 6: f.rows[0] += ",extra"; break;
        }
        f.Save(); refuses([&] { f.Finish(); });
    }
    Fixture partial; auto csv = read(partial.root / (partial.prefix + "_crop_meta.csv")); csv.pop_back();
    put(partial.root / (partial.prefix + "_crop_meta.csv"), csv); refuses([&] { partial.Finish(); });
}
void recorder_failures() {
    for (int mutation = 0; mutation < 10; ++mutation) {
        Fixture f;
        auto& clip = f.summary["rolling_output"]["clips"][1];
        switch (mutation) {
        case 0: f.summary["frames_encoded"] = 4; break;
        case 1: f.summary["worker_failed"] = true; break;
        case 2: f.summary["encode_dropped"] = 1; break;
        case 3: clip["first_recording_frame_id"] = 5; break;
        case 4: clip["last_recording_frame_id"] = 4; break;
        case 5: clip["packets_written"] = 1; break;
        case 6: clip["clip_id"] = "clip_000000"; break;
        case 7: clip["clip_index"] = 2; break;
        case 8: f.summary["session_id"] = "other"; break;
        case 9: f.summary["stream_kind"] = "full_frame"; break;
        }
        f.Save(); refuses([&] { f.Finish(); });
    }
    Fixture stopped; refuses([&] { stopped.Finish(false); });
}
void custody_and_gate() {
    Fixture f; f.Marker();
    auto parent = [&] { return json{{"status", "completed"}, {"session_id", f.master["recording_id"]}}; };
    auto p = parent(); ApplyRequiredMovingCropMetadataGate(f.root, &p);
    check(p["status"] == "failed", "missing required receipt did not fail parent");
    f.Finish(); p = parent(); ApplyRequiredMovingCropMetadataGate(f.root, &p);
    check(p["status"] == "completed" && p["moving_crop_master_coverage"]["status"] == "matched", "valid receipt blocked");
    put(f.root / "recording_snapshot.json", "{}"); // immutable start still requires coverage
    put(f.root / (f.prefix + "_crop_clip_1_meta_v1.csv"), "mutated");
    p = parent(); ApplyRequiredMovingCropMetadataGate(f.root, &p);
    check(p["status"] == "failed", "tampered clip accepted by parent");
    for (const auto* asset : {"_master_frames_v1.json", "_master_frames_v1.csv", "_crop_meta.csv", "_yolo_events.jsonl"}) {
        Fixture g; g.Finish(); put(g.root / (g.prefix + asset), "changed");
        refuses([&] { RequireMovingCropMetadataReceipt(g.root, g.master); });
    }
    Fixture existing; put(existing.root / (existing.prefix + "_crop_clip_0_meta_v1.csv"), "user file");
    refuses([&] { existing.Finish(); });
    check(read(existing.root / (existing.prefix + "_crop_clip_0_meta_v1.csv")) == "user file", "existing output overwritten");
    Fixture symlink; fs::rename(symlink.root / "summary.json", symlink.root / "actual.json");
    fs::create_symlink("actual.json", symlink.root / "summary.json"); refuses([&] { symlink.Finish(); });
    Fixture unchanged; p = {{"status", "completed"}}; const auto before = p;
    ApplyRequiredMovingCropMetadataGate(unchanged.root, &p); check(p == before, "non-opt-in parent changed");
    for (int mutation = 0; mutation < 4; ++mutation) {
        Fixture g; auto receipt = g.Finish();
        if (mutation == 0) receipt["unexpected"] = true;
        if (mutation == 1) receipt["partitions"][1]["first_recording_frame_id"] = 3;
        if (mutation == 2) receipt["detection_coverage"]["successful_zero_detection_frames"] = 6;
        if (mutation == 3) receipt["partitions"][1]["recorder_clip_index"] = nullptr;
        put(g.root / (g.prefix + "_moving_crop_metadata_v1.json"), receipt.dump());
        refuses([&] { RequireMovingCropMetadataReceipt(g.root, g.master); });
    }
}
}
int main() {
    try {
        success(); detector_failures(); metadata_failures(); recorder_failures(); custody_and_gate();
        std::cout << "5 moving crop completion groups passed\n"; return 0;
    } catch (const std::exception& ex) { std::cerr << ex.what() << '\n'; return 1; }
}
