#include "crop_only_manifest_fixture.h"
#include "recording_crop_only_result.h"
namespace {
using namespace orange::test_crop_media;
void lifecycle() {
    for (bool rolling : {false, true}) for (bool two : {false, true}) {
        CropOnlyFixture f(rolling, two);
        auto parent = BuildCropOnlyRecordingManifest(f.first.root, f.lifecycle);
        if (parent.at("status") != "completed") throw std::runtime_error(parent.dump(2));
        check(parent.at("camera_artifacts").empty() && !parent.contains("frame_identity_contract"), "phantom full-frame authority");
        check(parent.at("cameras").size() == (two ? 2U : 1U), "logical camera membership changed");
        check(parent.at("clips").size() == (two ? 2U : 1U) * (rolling ? 2U : 1U), "clip collection membership changed");
        const auto& crop = parent.at("recording_outputs").at(f.first.serial).at("crop");
        check(!crop.contains("video") && crop.at("representation") == "clip_collection", "aggregate count paired with a video");
        const auto& last = crop.at("clips").back();
        check(last.at("first_video_frame_index") == 0 && last.at("last_video_frame_index") == (rolling ? 1 : 4), "clip-local indices wrong");
        check(last.at("first_recording_frame_id") == (rolling ? 6 : 1), "clip reset parent frame identity");
        PublishCropOnlyRecordingIndex(f.first.root, &parent);
        put(f.first.root / "recording_session.json", parent.dump());
        check(ReadVerifiedCropOnlyRecordingManifest(f.first.root) == parent, "read-only collection verification failed");
        json row = {{"acq_fps_mean", 100}};
        EvaluateCropOnlyCameraResult(f.first.root, f.first.serial, {100, 5}, &row);
        check(row.at("pass_fail") == "pass" && row.at("video_present") == false && row.at("crop_frame_count") == (rolling ? 7 : 5),
            "crop result demanded full-frame video or lost count");
        row["camera_frame_id_gaps"] = 1;
        EvaluateCropOnlyCameraResult(f.first.root, f.first.serial, {100, 5}, &row);
        check(row.at("pass_fail") == "fail", "crop result ignored acquisition drops");
        row["camera_frame_id_gaps"] = 0; row["acq_fps_mean"] = 70;
        EvaluateCropOnlyCameraResult(f.first.root, f.first.serial, {100, 5}, &row);
        check(row.at("pass_fail") == "marginal", "crop result ignored acquisition throughput");
        const auto bytes = read(f.first.root / "recording_crop_clip_index_v1.json");
        PublishCropOnlyRecordingIndex(f.first.root, &parent);
        check(bytes == read(f.first.root / "recording_crop_clip_index_v1.json"), "retry changed index");
        const auto index = json::parse(bytes);
        check(index.at("clips").size() == parent.at("clips").size(), "index lost clip membership");
        check(!fs::exists(f.first.root / (f.first.prefix + ".mp4")) && !fs::exists(f.first.root / "recording_clip_index.json"), "invented full-frame outputs");
        const auto path = f.first.root / index.at("clips")[0].at("manifest").at("relative_path").get<std::string>();
        fs::rename(path, path.string() + ".saved"); put(path, "user-owned");
        refuses([&] { PublishCropOnlyRecordingIndex(f.first.root, &parent); });
        refuses([&] { ReadVerifiedCropOnlyRecordingManifest(f.first.root); });
        check(read(path) == "user-owned", "publication overwrote conflicting evidence");
    }
}
void failures() {
    for (int failure = 0; failure < 8; ++failure) {
        CropOnlyFixture f(true);
        switch (failure) {
        case 0: fs::remove(f.first.root / "recording_snapshot_start.json"); break;
        case 1: f.start["session"].erase("registered_scene_context"); f.SaveStart(); break;
        case 2: f.start["session"]["recording_media_plan"]["selection"]["mode"] = "full_frame"; f.SaveStart(); break;
        case 3: f.lifecycle["cameras"].push_back("unknown-camera"); break;
        case 4: fs::rename(f.first.root / (f.first.prefix + "_registered_context.raw"), f.first.root / "context.saved");
                put(f.first.root / (f.first.prefix + "_registered_context.raw"), "changed"); break;
        case 5: put(f.first.root / "crop-1.mp4", "changed"); break;
        case 6: f.lifecycle["mode"] = "single_clip"; break;
        case 7: fs::remove(f.first.root / (f.first.prefix + "_moving_crop_media_v1.json")); break;
        }
        auto parent = BuildCropOnlyRecordingManifest(f.first.root, f.lifecycle);
        check(parent.at("status") == "failed" && parent.at("crop_only_inventory").at("status") == "failed", "incomplete inventory accepted");
        check(parent.at("clips").empty() && parent.at("recording_outputs").empty(), "invented output after evidence failure");
        PublishCropOnlyRecordingIndex(f.first.root, &parent);
        check(!parent.contains("crop_clip_index") && !fs::exists(f.first.root / "recording_crop_clip_index_v1.json"), "failed parent published index");
    }
    CropOnlyFixture f(false); f.lifecycle["status"] = "interrupted";
    auto parent = BuildCropOnlyRecordingManifest(f.first.root, f.lifecycle);
    check(parent.at("status") == "interrupted", "valid media promoted interrupted parent");
    PublishCropOnlyRecordingIndex(f.first.root, &parent); check(!parent.contains("crop_clip_index"), "interrupted parent indexed");
    f.lifecycle["camera_artifacts"] = {{f.first.serial, {{"video", "phantom.mp4"}}}};
    refuses([&] { BuildCropOnlyRecordingManifest(f.first.root, f.lifecycle); });
}
void daily_context_and_publication_recheck() {
    CropOnlyFixture f(true, true, true);
    auto parent = BuildCropOnlyRecordingManifest(f.first.root, f.lifecycle);
    if (parent.at("status") != "completed") throw std::runtime_error(parent.dump(2));
    const auto& context = parent.at("clips")[0].at("registered_context");
    check(context.at("profile") == "daily_registered_context_reuse_v1" && context.contains("use_receipt"), "daily asset binding lost");
    auto changed = parent; changed["clips"][0]["camera_serial"] = "../../escape";
    refuses([&] { PublishCropOnlyRecordingIndex(f.first.root, &changed); });
    check(!fs::exists(f.first.root / "recording_crop_clip_index_v1.json"), "changed in-memory index published");
    PublishCropOnlyRecordingIndex(f.first.root, &parent);
    put(f.first.root / "second-crop-1.mp4", "changed after construction");
    refuses([&] { PublishCropOnlyRecordingIndex(f.first.root, &parent); });
    f.lifecycle["crop_clip_index"] = parent.at("crop_clip_index");
    refuses([&] { BuildCropOnlyRecordingManifest(f.first.root, f.lifecycle); });
}
}
void arm_and_result_failures() {
    using namespace orange::test_crop_media;
    for (int failure = -1; failure < 6; ++failure) {
        CropOnlyFixture f(true, true, true);
        const auto plan = f.start.at("session").at("recording_media_plan");
        if (failure == 0) f.start["session"]["moving_crop_encoded_media"]["required"] = false;
        if (failure == 1) f.start["session"].erase("master_frame_journal");
        if (failure == 2) f.start["session"].erase("moving_crop_master_coverage");
        if (failure == 3) f.start["session"]["recording_media_plan"]["cameras"].erase(0);
        if (failure >= 0 && failure < 4) f.SaveStart();
        if (failure == 4) fs::remove(f.first.root / "registered_context_use_v1.json");
        if (failure == 5) fs::remove(f.first.root / "recording_snapshot_start.json");
        if (failure < 0) RequireCropOnlyArmEvidence(f.first.root, plan);
        else refuses([&] { RequireCropOnlyArmEvidence(f.first.root, plan); });
    }
    for (int failure = 0; failure < 5; ++failure) {
        CropOnlyFixture f(false);
        auto parent = BuildCropOnlyRecordingManifest(f.first.root, f.lifecycle);
        PublishCropOnlyRecordingIndex(f.first.root, &parent);
        put(f.first.root / "recording_session.json", parent.dump());
        if (failure == 0) fs::remove(f.first.root / "recording_crop_clip_index_v1.json");
        if (failure == 1) { fs::rename(f.first.root / "recording_crop_clip_index_v1.csv", f.first.root / "saved.csv");
            put(f.first.root / "recording_crop_clip_index_v1.csv", "changed"); }
        if (failure == 2) { parent["status"] = "interrupted"; put(f.first.root / "recording_session.json", parent.dump()); }
        if (failure == 3) put(f.first.root / "crop-0.mp4", "changed");
        json row = {{"acq_fps_mean", 100}};
        EvaluateCropOnlyCameraResult(f.first.root, failure == 4 ? "missing" : f.first.serial, {100, 5}, &row);
        check(row.at("pass_fail") == "fail" && row.at("status") == "failed", "crop result accepted incomplete collection");
    }
}
int main() {
    try { lifecycle(); failures(); daily_context_and_publication_recheck(); arm_and_result_failures(); std::cout << "Crop-only single/rolling inventory, index, custody and failure tests passed.\n"; }
    catch (const std::exception& ex) { std::cerr << ex.what() << '\n'; return 1; }
}
