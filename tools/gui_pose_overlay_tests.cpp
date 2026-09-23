// tools/gui_pose_overlay_tests.cpp — unit tests for src/gui/pose_overlay.*
// (mailbox round trip and torn-read handling, keypoint mapping in full-frame
// and crop space, confidence and staleness filtering, skeleton validation,
// draw-command construction). No GL, CUDA or ImGui.
#include "gui/pose_overlay.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace orange::gui;

void require(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        std::exit(1);
    }
}

void require_near(double actual, double expected, double tolerance, const std::string& message)
{
    if (std::fabs(actual - expected) > tolerance) {
        std::cerr << "FAIL: " << message << " actual=" << actual << " expected=" << expected << std::endl;
        std::exit(1);
    }
}

PoseOverlaySnapshot fish_snapshot()
{
    // 4512x4512 source, 256x256 crop at (1000, 2000); three keypoints.
    PoseOverlaySnapshot s;
    s.recording_frame_id = 42;
    s.local_frame_id = 142;
    s.host_ns = 1'000'000'000;
    s.crop_x = 1000;
    s.crop_y = 2000;
    s.crop_w = 256;
    s.crop_h = 256;
    s.bbox_x = 1050.0f;
    s.bbox_y = 2050.0f;
    s.bbox_w = 150.0f;
    s.bbox_h = 120.0f;
    s.keypoint_count = 3;
    s.keypoints[0] = {1128.0f, 2128.0f, 0.9f, 0, true};   // crop centre
    s.keypoints[1] = {1000.0f, 2000.0f, 0.6f, 1, true};   // crop top-left corner
    s.keypoints[2] = {1255.0f, 2255.0f, 0.1f, 2, true};   // below confidence floor
    return s;
}

void test_mailbox_round_trip()
{
    PoseOverlayMailbox mailbox;
    PoseOverlaySnapshot out;
    require(!mailbox.TryRead(&out), "empty mailbox reads false");
    mailbox.Publish(fish_snapshot());
    require(mailbox.TryRead(&out), "published snapshot reads");
    require(out.recording_frame_id == 42 && out.keypoint_count == 3, "snapshot fields survive");
    require(out.sequence == 2, "first publish has sequence 2");
    require(mailbox.publishes() == 1, "publish counter");
    PoseOverlaySnapshot second = fish_snapshot();
    second.recording_frame_id = 43;
    mailbox.Publish(second);
    require(mailbox.TryRead(&out) && out.recording_frame_id == 43 && out.sequence == 4, "latest value wins");
}

void test_mailbox_concurrent_reads_are_consistent()
{
    // A writer publishes snapshots whose keypoints all carry the frame id; a
    // reader must never observe a mix of two publishes.
    PoseOverlayMailbox mailbox;
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        uint64_t frame = 1;
        while (!stop.load()) {
            PoseOverlaySnapshot s;
            s.recording_frame_id = frame;
            s.keypoint_count = kPoseOverlayMaxKeypoints;
            for (int i = 0; i < kPoseOverlayMaxKeypoints; ++i) {
                s.keypoints[i].x_px = static_cast<float>(frame);
            }
            mailbox.Publish(s);
            ++frame;
        }
    });
    uint64_t good = 0;
    for (int i = 0; i < 20000; ++i) {
        PoseOverlaySnapshot out;
        if (!mailbox.TryRead(&out)) {
            continue;
        }
        for (int k = 0; k < kPoseOverlayMaxKeypoints; ++k) {
            require(out.keypoints[k].x_px == static_cast<float>(out.recording_frame_id), "no torn snapshot observed");
        }
        ++good;
    }
    stop.store(true);
    writer.join();
    require(good > 0, "some consistent reads happened");
}

void test_mapping_full_frame()
{
    const PoseOverlaySnapshot s = fish_snapshot();
    const PoseOverlayRect rect{100.0f, 50.0f, 100.0f + 1128.0f, 50.0f + 1128.0f};  // preview at downsample 4
    PoseOverlayPoint p;
    require(map_pose_keypoint_to_screen(s, 0, PoseOverlayImageSpace::kFullFrame, 4512, 4512, rect, &p), "maps centre keypoint");
    require_near(p.x, 100.0 + 1128.0 / 4.0, 1e-3, "full-frame x maps through the downsample");
    require_near(p.y, 50.0 + 2128.0 / 4.0, 1e-3, "full-frame y maps through the downsample");
    // The same keypoint on a differently sized image rectangle scales with it.
    const PoseOverlayRect small{0.0f, 0.0f, 451.2f, 451.2f};
    require(map_pose_keypoint_to_screen(s, 0, PoseOverlayImageSpace::kFullFrame, 4512, 4512, small, &p), "maps on a small rect");
    require_near(p.x, 112.8, 1e-3, "scales with the rectangle");
    require(!map_pose_keypoint_to_screen(s, 3, PoseOverlayImageSpace::kFullFrame, 4512, 4512, rect, &p), "index past count rejected");
    require(!map_pose_keypoint_to_screen(s, 0, PoseOverlayImageSpace::kFullFrame, 0, 4512, rect, &p), "zero source rejected");
}

void test_mapping_crop()
{
    PoseOverlaySnapshot s = fish_snapshot();
    const PoseOverlayRect rect{10.0f, 10.0f, 10.0f + 384.0f, 10.0f + 384.0f};  // crop shown at 384 px
    PoseOverlayPoint p;
    require(map_pose_keypoint_to_screen(s, 0, PoseOverlayImageSpace::kCrop, 4512, 4512, rect, &p), "maps crop centre");
    require_near(p.x, 10.0 + 128.0 / 256.0 * 384.0, 1e-3, "crop-local x scaled to the item");
    require_near(p.y, 10.0 + 128.0 / 256.0 * 384.0, 1e-3, "crop-local y scaled to the item");
    require(map_pose_keypoint_to_screen(s, 1, PoseOverlayImageSpace::kCrop, 4512, 4512, rect, &p), "crop corner maps");
    require_near(p.x, 10.0, 1e-3, "corner lands on the rect min");
    s.keypoints[1].x_px = 999.0f;  // one pixel outside the crop
    require(!map_pose_keypoint_to_screen(s, 1, PoseOverlayImageSpace::kCrop, 4512, 4512, rect, &p), "outside the crop rejected");
    s.crop_w = 0;
    require(!map_pose_keypoint_to_screen(s, 0, PoseOverlayImageSpace::kCrop, 4512, 4512, rect, &p), "degenerate crop rejected");
}

void test_mapping_crop_uses_preview_rect()
{
    // Device ROI path: 192 px pose crop centred inside the 384 px video crop
    // that the preview window shows. A keypoint at the pose crop centre must
    // land at the centre of the drawn image, not at (0.5 * 384/192) of it.
    PoseOverlaySnapshot s = fish_snapshot();
    s.crop_x = 3132; s.crop_y = 1857; s.crop_w = 192; s.crop_h = 192;
    s.preview_crop_x = 3036; s.preview_crop_y = 1761; s.preview_crop_w = 384; s.preview_crop_h = 384;
    s.keypoint_count = 2;
    s.keypoints[0].x_px = 3132.0f + 96.0f; s.keypoints[0].y_px = 1857.0f + 96.0f; s.keypoints[0].confidence = 0.99f; s.keypoints[0].visible = true;
    s.keypoints[1].x_px = 3132.0f; s.keypoints[1].y_px = 1857.0f; s.keypoints[1].confidence = 0.99f; s.keypoints[1].visible = true;
    const PoseOverlayRect rect{0.0f, 0.0f, 400.0f, 400.0f};
    PoseOverlayPoint p;
    require(map_pose_keypoint_to_screen(s, 0, PoseOverlayImageSpace::kCrop, 4512, 4512, rect, &p), "centre maps");
    require_near(p.x, 200.0, 1e-3, "pose-crop centre is the preview centre (x)");
    require_near(p.y, 200.0, 1e-3, "pose-crop centre is the preview centre (y)");
    require(map_pose_keypoint_to_screen(s, 1, PoseOverlayImageSpace::kCrop, 4512, 4512, rect, &p), "pose-crop corner maps");
    require_near(p.x, 100.0, 1e-3, "pose-crop corner sits a quarter in from the preview edge");
    // Without a preview rect the pose crop is the image (single-crop path).
    s.preview_crop_w = 0; s.preview_crop_h = 0;
    require(map_pose_keypoint_to_screen(s, 1, PoseOverlayImageSpace::kCrop, 4512, 4512, rect, &p), "single-crop corner maps");
    require_near(p.x, 0.0, 1e-3, "single crop: corner on the rect min");
}

void test_skeleton_validation()
{
    require(validate_pose_skeleton({{0, 1}, {1, 2}}, 3), "valid skeleton");
    require(!validate_pose_skeleton({{0, 3}}, 3), "edge past count rejected");
    require(!validate_pose_skeleton({{1, 1}}, 3), "self edge rejected");
    require(!validate_pose_skeleton({{-1, 0}}, 3), "negative index rejected");
    require(validate_pose_skeleton({}, 0), "empty skeleton is valid");
}

void test_commands_full_frame()
{
    const PoseOverlaySnapshot s = fish_snapshot();
    const PoseOverlayRect rect{0.0f, 0.0f, 1128.0f, 1128.0f};
    std::vector<PoseOverlayDrawCommand> cmds;
    PoseOverlayStats stats;
    PoseOverlayOptions options;
    const std::vector<PoseOverlaySkeletonEdge> edges{{0, 1}, {1, 2}};
    const size_t n = build_pose_overlay_commands(s, PoseOverlayImageSpace::kFullFrame, 4512, 4512, rect, edges, options,
                                                 s.host_ns + 1'000'000, &cmds, &stats);
    // bbox + one edge (0-1; 1-2 is skipped because keypoint 2 is below the floor) + two circles
    require(n == 4 && cmds.size() == 4, "bbox, one edge, two circles: got " + std::to_string(n));
    require(cmds[0].kind == PoseOverlayDrawCommand::Kind::kRect, "bbox first");
    require_near(cmds[0].x0, 1050.0 / 4.0, 1e-3, "bbox min x");
    require_near(cmds[0].x1, 1200.0 / 4.0, 1e-3, "bbox max x");
    require(cmds[1].kind == PoseOverlayDrawCommand::Kind::kLine, "edge after bbox");
    require(cmds[2].kind == PoseOverlayDrawCommand::Kind::kCircle && cmds[3].kind == PoseOverlayDrawCommand::Kind::kCircle, "circles last");
    require(stats.frames_drawn == 1 && stats.keypoints_drawn == 2 && stats.max_keypoints_per_frame == 2 && stats.edges_drawn == 1,
            "stats count drawn keypoints and edges");
    require(stats.stale_frames == 0 && stats.empty_frames == 0, "not stale or empty");
}

void test_commands_crop_and_filters()
{
    PoseOverlaySnapshot s = fish_snapshot();
    const PoseOverlayRect rect{0.0f, 0.0f, 384.0f, 384.0f};
    std::vector<PoseOverlayDrawCommand> cmds;
    PoseOverlayStats stats;
    PoseOverlayOptions options;
    options.draw_bbox = true;  // bbox is only drawn in full-frame space
    const size_t n = build_pose_overlay_commands(s, PoseOverlayImageSpace::kCrop, 4512, 4512, rect, {}, options,
                                                 s.host_ns, &cmds, &stats);
    require(n == 2, "crop space: two circles, no bbox, no edges: got " + std::to_string(n));

    // Stale snapshot draws nothing and is counted.
    cmds.clear();
    const size_t stale = build_pose_overlay_commands(s, PoseOverlayImageSpace::kCrop, 4512, 4512, rect, {}, options,
                                                     s.host_ns + options.stale_after_ns + 1, &cmds, &stats);
    require(stale == 0 && stats.stale_frames == 1, "stale snapshot skipped");

    // All keypoints below the floor: empty frame.
    options.min_confidence = 0.95f;
    cmds.clear();
    const size_t empty = build_pose_overlay_commands(s, PoseOverlayImageSpace::kCrop, 4512, 4512, rect, {}, options,
                                                     s.host_ns, &cmds, &stats);
    require(empty == 0 && stats.empty_frames == 1, "empty frame counted");

    // A keypoint outside the crop is counted, not drawn.
    options.min_confidence = 0.25f;
    s.keypoints[1].x_px = 5000.0f;
    cmds.clear();
    const size_t partial = build_pose_overlay_commands(s, PoseOverlayImageSpace::kCrop, 4512, 4512, rect, {}, options,
                                                       s.host_ns, &cmds, &stats);
    require(partial == 1 && stats.keypoints_outside_image == 1, "outside keypoint skipped and counted");
    require(build_pose_overlay_commands(s, PoseOverlayImageSpace::kCrop, 4512, 4512, rect, {}, options, s.host_ns, nullptr, &stats) == 0,
            "null output tolerated");
}

}  // namespace

int main()
{
    test_mailbox_round_trip();
    test_mailbox_concurrent_reads_are_consistent();
    test_mapping_full_frame();
    test_mapping_crop();
    test_mapping_crop_uses_preview_rect();
    test_skeleton_validation();
    test_commands_full_frame();
    test_commands_crop_and_filters();
    std::cout << "gui_pose_overlay_tests: all tests passed" << std::endl;
    return 0;
}
