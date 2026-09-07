#include "moving_crop_completion_fixture.h"
namespace {
using namespace orange::test_crop_media;
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
