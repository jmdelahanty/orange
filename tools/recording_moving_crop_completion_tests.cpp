#include "recording_master_crop_coverage.h"
#include "recording_master_journal.h"
#include "recording_media_decode.h"
#include <fstream>
#include <iostream>
#include <functional>
#include <algorithm>
#include <stdexcept>
#include <unistd.h>
extern "C" {
#include <libavutil/base64.h>
}

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
    Fixture(uint64_t frame_count = 5) {
        std::string name = (fs::temp_directory_path() / "moving_crop_completion_XXXXXX").string();
        check(::mkdtemp(name.data()) != nullptr, "fixture directory failed"); root = name;
        MasterJournalOptions options;
        options.recording_directory = root; options.recording_id = root.filename().string();
        options.camera_serial = serial; options.producer_instance_id = "producer-A";
        MasterFrameJournal journal(options);
        for (uint64_t i = 1; i <= frame_count; ++i) {
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
    void Media(bool rolling) {
        const uint64_t count = rows.size();
        summary["frames_received"] = summary["frames_encoded"] = count;
        summary["codec"] = "hevc"; summary["tuning"] = "lossless";
        summary["merged_output"] = {{"coordinator_enabled", true}, {"pending_gops", 0}, {"failed", false},
            {"mp4", "crop-0.mp4"}, {"metadata", "recorder-0.csv"}};
        json binding = {{"method", "nvenc_input_timestamp_to_output_timestamp_registry"},
            {"metadata_write_event", "completed_gop_after_returned_identity_match"},
            {"verification_rule_id", "orange.external_recorder.frame_identity.v2"},
            {"verified", true}, {"first_packet_write_error_code", nullptr}};
        for (const auto* key : {"submitted_frame_identities", "returned_identity_matches", "encoded_video_frames",
                "metadata_rows", "packet_submissions_accepted", "packet_write_attempts", "packets_written"}) binding[key] = count;
        for (const auto* key : {"identity_mismatches", "outstanding_submitted_identities", "packet_submissions_rejected", "packet_write_failures"}) binding[key] = 0;
        summary["frame_identity_proof"] = {{"schema_id", "orange.external_recorder.frame_identity_proof"}, {"schema_version", 2},
            {"status", "passed"}, {"canonical_field", "recording_frame_id"}, {"scope", "recording_session_and_camera_stream"},
            {"row_granularity", "one_encoded_video_frame"}, {"continuity_policy", "encoded_subset"},
            {"source_frames_skipped_by_policy", 0}, {"source_frames_dropped", 0}, {"video_binding", binding}};
        summary["rolling_output"] = {{"enabled", rolling}, {"clips", json::array()}};
        for (uint64_t clip = 0; clip < (rolling ? 2U : 1U); ++clip) {
            const uint64_t n = std::min<uint64_t>(5, count - clip * 5);
            check(n == 5 || n == 2, "unsupported fixture frame count");
            auto encoded = read(n == 5 ? fs::path(ORANGE_CROP_MEDIA_FIXTURE) :
                fs::path(ORANGE_CROP_MEDIA_FIXTURE).parent_path() / "crop_black_two_256_hevc.mp4.b64");
            encoded.erase(std::remove(encoded.begin(), encoded.end(), '\n'), encoded.end());
            std::string video(encoded.size(), '\0');
            const int size = av_base64_decode(reinterpret_cast<uint8_t*>(video.data()), encoded.c_str(), video.size());
            check(size > 0, "invalid encoded fixture"); video.resize(size);
            const auto mp4 = "crop-" + std::to_string(clip) + ".mp4", csv = "recorder-" + std::to_string(clip) + ".csv";
            put(root / mp4, video);
            std::string metadata = "frame_id,timestamp,timestamp_sys,recording_frame_id,local_frame_id\n";
            for (uint64_t i = clip * 5 + 1; i <= clip * 5 + n; ++i)
                metadata += std::to_string(i) + "," + std::to_string(1700000000000000000ULL + i) + "," +
                    std::to_string(1700000000000000100ULL + i) + "," + std::to_string(i) + "," + std::to_string(i + 7000) + "\n";
            put(root / csv, metadata);
            json packets = {{"submissions_accepted", n}, {"write_attempts", n}, {"packets_written", n},
                {"submissions_rejected", 0}, {"write_failures", 0}, {"complete", true}, {"writer_error_latched", false},
                {"muxer_flush_attempted", true}, {"muxer_flush_succeeded", true}, {"first_write_error_code", nullptr}, {"muxer_flush_error_code", nullptr}};
            json container = {{"file_size_bytes", video.size()}, {"trailer_error_code", nullptr},
                {"output_close_error_code", nullptr}, {"file_size_error", nullptr}};
            for (const auto* key : {"header_written", "trailer_attempted", "trailer_written", "output_close_attempted", "output_closed", "finalized"}) container[key] = true;
            put(root / (mp4 + ".finalization.json"), json{{"schema_id", "orange.video_container_finalization"},
                {"schema_version", 2}, {"status", "complete"}, {"terminal", true}, {"packet_writes", packets}, {"container", container}}.dump());
            if (rolling) summary["rolling_output"]["clips"].push_back({{"clip_index", clip},
                {"clip_id", "clip_" + std::to_string(clip)}, {"first_recording_frame_id", clip * 5 + 1},
                {"last_recording_frame_id", clip * 5 + n}, {"frame_count", n}, {"packets_written", n},
                {"failed", false}, {"mp4", mp4}, {"metadata", csv}});
        }
        Save();
    }
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
void encoded_media() {
    for (uint64_t count : {5, 7, 10}) {
        const bool rolling = count > 5;
        Fixture f(count); f.Media(rolling); f.Finish();
        const auto receipt = FinalizeMovingCropMedia(f.root, f.serial, 256, 256);
        // The codec fixture contains five black frames. Decoder count checks
        // independently reject missing and extra tails, without a bad sidecar
        // short-circuiting the decode path.
        refuses([&] { RequireDecodedCropMedia(f.root / "crop-0.mp4", 256, 256, 6); });
        refuses([&] { RequireDecodedCropMedia(f.root / "crop-0.mp4", 256, 256, 4); });
        check(receipt.at("videos").size() == (rolling ? 2 : 1), "encoded crop clip membership wrong");
        RequireMovingCropMediaReceipt(f.root, f.master);
        put(f.root / "crop-0.mp4", "modified");
        refuses([&] { RequireMovingCropMediaReceipt(f.root, f.master); });
    }
    for (int mutation = 0; mutation < 8; ++mutation) {
        Fixture f; f.Media(false);
        switch (mutation) {
        case 0: f.summary["frame_identity_proof"]["video_binding"]["returned_identity_matches"] = 4; break;
        case 1: f.summary["frame_identity_proof"]["video_binding"]["packet_write_failures"] = 1; break;
        case 2: f.summary["frame_identity_proof"]["schema_version"] = 1; break;
        case 3: put(f.root / "recorder-0.csv", "frame_id,timestamp,timestamp_sys,recording_frame_id,local_frame_id\n"); break;
        case 4: { auto video = read(f.root / "crop-0.mp4"); video.resize(100); put(f.root / "crop-0.mp4", video); break; }
        case 5: std::filesystem::remove(f.root / "crop-0.mp4.finalization.json"); break;
        case 6: f.summary["merged_output"]["mp4"] = "../escape.mp4"; break;
        default: break; // wrong authenticated raster below
        }
        f.Save(); f.Finish();
        refuses([&] { FinalizeMovingCropMedia(f.root, f.serial, mutation == 7 ? 320 : 256, 256); });
        check(!fs::exists(f.root / (f.prefix + "_moving_crop_media_v1.json")), "failed validation published a media receipt");
    }
}
void encoded_media_gate() {
    Fixture f; f.Media(false); f.Marker();
    auto marker = json::parse(read(f.root / "recording_snapshot_start.json"));
    marker["session"]["moving_crop_encoded_media"] = {
        {"schema_version", 1}, {"required", true}, {"profile", "returned_identity_v2_mux_and_full_hevc_decode_v1"}};
    put(f.root / "recording_snapshot_start.json", marker.dump());
    auto parent = [&] { return json{{"status", "completed"}, {"session_id", f.master.at("recording_id")}}; };
    f.Finish();
    auto p = parent(); ApplyRequiredMovingCropMediaGate(f.root, &p);
    check(p.at("status") == "failed", "metadata alone passed required media gate");
    const auto receipt = FinalizeMovingCropMedia(f.root, f.serial, 256, 256);
    p = parent(); ApplyRequiredMovingCropMediaGate(f.root, &p);
    check(p.at("status") == "completed" && p["moving_crop_encoded_media"]["status"] == "complete", "valid media gate failed");
    for (int mutation = 0; mutation < 6; ++mutation) {
        auto changed = receipt;
        if (mutation == 0) changed["extra"] = true;
        if (mutation == 1) changed["width"] = 0;
        if (mutation == 2) changed["width"] = 320;
        if (mutation == 3) changed["mode"] = "rolling_clips";
        if (mutation == 4) changed["videos"][0]["clip_id"] = "changed";
        if (mutation == 5) changed["videos"][0]["crop_metadata"] = changed["videos"][0]["recorder_metadata"];
        put(f.root / (f.prefix + "_moving_crop_media_v1.json"), changed.dump());
        refuses([&] { RequireMovingCropMediaReceipt(f.root, f.master); });
        p = parent(); ApplyRequiredMovingCropMediaGate(f.root, &p);
        check(p.at("status") == "failed", "mutated media receipt passed parent gate");
    }
    Fixture untouched; p = {{"status", "completed"}}; const auto before = p;
    ApplyRequiredMovingCropMediaGate(untouched.root, &p);
    check(p == before, "unselected media gate changed parent");
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
        success(); detector_failures(); metadata_failures(); recorder_failures(); custody_and_gate(); encoded_media(); encoded_media_gate();
        std::cout << "7 moving crop metadata/media completion groups passed\n"; return 0;
    } catch (const std::exception& ex) { std::cerr << ex.what() << '\n'; return 1; }
}
