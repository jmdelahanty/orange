#pragma once
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

namespace orange::test_crop_media {
using namespace orange::recording;
using json = nlohmann::json;
namespace fs = std::filesystem;
inline void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
inline void put(const fs::path& path, const std::string& bytes) {
    std::ofstream out(path); out << bytes; out.close(); check(!out.fail(), "fixture write failed");
}
inline std::string read(const fs::path& path) {
    std::ifstream in(path); return {std::istreambuf_iterator<char>(in), {}};
}
inline void refuses(const std::function<void()>& action) {
    bool failed = false;
    try { action(); } catch (const std::exception&) { failed = true; }
    check(failed, "incomplete crop evidence accepted");
}
struct Fixture {
    fs::path root;
    std::string serial = "02010093", prefix = "Cam02010093";
    std::string tag;
    bool owns_root = true;
    json master, summary;
    std::vector<json> events;
    std::vector<std::string> rows;
    Fixture(uint64_t frame_count = 5, std::string camera_serial = "02010093", fs::path shared_root = {}, std::string file_tag = {})
        : serial(std::move(camera_serial)), prefix("Cam" + serial), tag(std::move(file_tag)) {
        if (shared_root.empty()) {
            std::string name = (fs::temp_directory_path() / "moving_crop_completion_XXXXXX").string();
            check(::mkdtemp(name.data()) != nullptr, "fixture directory failed"); root = name;
        } else { root = std::move(shared_root); owns_root = false; }
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
    ~Fixture() { if (owns_root) { std::error_code ec; fs::remove_all(root, ec); } }
    void Save() {
        std::string csv = "recording_frame_id,local_frame_id,camera_frame_id,timestamp,timestamp_sys,has_detection,blank_frame,crop_state,crop_video_frame_index,session_crop_video_frame_index\n";
        for (const auto& row : rows) csv += row + "\n";
        put(root / (prefix + "_crop_meta.csv"), csv);
        std::string jsonl;
        for (const auto& event : events) jsonl += event.dump() + "\n";
        put(root / (prefix + "_yolo_events.jsonl"), jsonl);
        put(root / (tag + "summary.json"), summary.dump());
    }
    json Finish(bool normal = true) { return FinalizeMovingCropMetadata(root, serial, tag + "summary.json", normal); }
    void Media(bool rolling) {
        const uint64_t count = rows.size();
        summary["frames_received"] = summary["frames_encoded"] = count;
        summary["codec"] = "hevc"; summary["tuning"] = "lossless";
        summary["merged_output"] = {{"coordinator_enabled", true}, {"pending_gops", 0}, {"failed", false},
            {"mp4", tag + "crop-0.mp4"}, {"metadata", tag + "recorder-0.csv"}};
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
            const auto mp4 = tag + "crop-" + std::to_string(clip) + ".mp4", csv = tag + "recorder-" + std::to_string(clip) + ".csv";
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
} // namespace orange::test_crop_media
