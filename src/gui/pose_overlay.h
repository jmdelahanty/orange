// src/gui/pose_overlay.h
//
// Pose keypoint overlay for the GUI previews: pure data and geometry, no
// ImGui, CUDA or GL dependency, so it is unit-testable and can be built
// before it is wired in.
//
// Design (2026-09-22 read-only inspection): the pose worker decodes keypoints
// on its own thread and today publishes them only to the Shaman v2 IPC queue,
// which Citrus drains. For an on-screen overlay the pose worker will write a
// per-camera latest-pose snapshot into a PoseOverlayMailbox (single writer,
// lock-free for the writer), and the GUI main thread will read it and draw
// ImGui draw-list primitives over the preview image. Nothing here touches
// acquisition, the display worker or the preview staging lock.
//
// Coordinates: snapshot keypoints and the bbox are in SOURCE (full-frame)
// pixels; the crop rectangle says where the pose crop sits in the source.
// Mapping to the screen uses normalized coordinates of the image being drawn
// (full frame or crop), so the preview downsample factor cancels out.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace orange::gui {

inline constexpr int kPoseOverlayMaxKeypoints = 32;

struct PoseOverlayKeypoint {
    float x_px = 0.0f;         // source pixels
    float y_px = 0.0f;         // source pixels
    float confidence = 0.0f;
    uint16_t label_id = 0;
    bool visible = false;
};

struct PoseOverlaySnapshot {
    uint64_t sequence = 0;            // set by the mailbox on publish
    uint64_t recording_frame_id = 0;  // 0 when not recording
    uint64_t local_frame_id = 0;
    uint64_t camera_frame_id = 0;
    uint64_t host_ns = 0;             // steady clock at publish (staleness)
    int crop_x = 0;                   // pose crop origin/size in source pixels
    int crop_y = 0;
    int crop_w = 0;
    int crop_h = 0;
    float bbox_x = 0.0f;              // detection box in source pixels
    float bbox_y = 0.0f;
    float bbox_w = 0.0f;
    float bbox_h = 0.0f;
    int keypoint_count = 0;
    PoseOverlayKeypoint keypoints[kPoseOverlayMaxKeypoints];
};

// Single-writer, multi-reader latest-value slot (seqlock). Publish() never
// blocks and never allocates, so the pose thread can call it in its result
// path. TryRead() returns false on a torn read (writer in progress); callers
// keep the previous snapshot in that case.
class PoseOverlayMailbox {
public:
    void Publish(const PoseOverlaySnapshot& snapshot) noexcept;
    bool TryRead(PoseOverlaySnapshot* out) const noexcept;
    uint64_t publishes() const noexcept { return publishes_.load(std::memory_order_relaxed); }
    uint64_t torn_reads() const noexcept { return torn_reads_.load(std::memory_order_relaxed); }

private:
    std::atomic<uint64_t> sequence_{0};  // odd while a write is in progress
    PoseOverlaySnapshot slot_{};
    std::atomic<uint64_t> publishes_{0};
    mutable std::atomic<uint64_t> torn_reads_{0};
};

// Screen rectangle of the drawn image item (ImGui GetItemRectMin/Max or the
// canvas plot rectangle), in screen pixels.
struct PoseOverlayRect {
    float min_x = 0.0f;
    float min_y = 0.0f;
    float max_x = 0.0f;
    float max_y = 0.0f;
    float width() const { return max_x - min_x; }
    float height() const { return max_y - min_y; }
};

enum class PoseOverlayImageSpace {
    kFullFrame,  // the image shows the whole source frame (any downsample)
    kCrop,       // the image shows the pose crop (crop_x/y/w/h of the snapshot)
};

struct PoseOverlayPoint {
    float x = 0.0f;
    float y = 0.0f;
};

// Maps one keypoint to screen pixels. Returns false when the keypoint lies
// outside the drawn image (for kCrop, outside the crop rectangle) or the
// geometry is degenerate.
bool map_pose_keypoint_to_screen(const PoseOverlaySnapshot& snapshot,
                                 int keypoint_index,
                                 PoseOverlayImageSpace space,
                                 int source_width,
                                 int source_height,
                                 const PoseOverlayRect& rect,
                                 PoseOverlayPoint* out);

struct PoseOverlaySkeletonEdge {
    int a = 0;
    int b = 0;
};

// True when every edge references keypoint indexes below keypoint_count and
// no edge joins a keypoint to itself.
bool validate_pose_skeleton(const std::vector<PoseOverlaySkeletonEdge>& edges, int keypoint_count);

struct PoseOverlayOptions {
    float min_confidence = 0.25f;        // keypoints below this are not drawn
    float point_radius_px = 4.0f;
    float line_thickness_px = 2.0f;
    bool draw_bbox = true;
    bool draw_edges = true;
    uint64_t stale_after_ns = 500'000'000;  // snapshot older than this is not drawn
};

struct PoseOverlayDrawCommand {
    enum class Kind { kCircle, kLine, kRect };
    Kind kind = Kind::kCircle;
    float x0 = 0.0f;   // circle centre / line start / rect min
    float y0 = 0.0f;
    float x1 = 0.0f;   // line end / rect max
    float y1 = 0.0f;
    float radius = 0.0f;
    float thickness = 0.0f;
    float confidence = 0.0f;
    uint16_t label_id = 0;
};

struct PoseOverlayStats {
    uint64_t frames_considered = 0;
    uint64_t frames_drawn = 0;
    uint64_t stale_frames = 0;
    uint64_t empty_frames = 0;      // no keypoints above the confidence floor
    uint64_t keypoints_drawn = 0;
    uint64_t max_keypoints_per_frame = 0;
    uint64_t edges_drawn = 0;
    uint64_t keypoints_outside_image = 0;
};

// Builds the draw commands for one snapshot. Pure: the ImGui glue turns the
// commands into AddCircleFilled/AddLine/AddRect calls. Returns the number of
// commands appended to `out`.
size_t build_pose_overlay_commands(const PoseOverlaySnapshot& snapshot,
                                   PoseOverlayImageSpace space,
                                   int source_width,
                                   int source_height,
                                   const PoseOverlayRect& rect,
                                   const std::vector<PoseOverlaySkeletonEdge>& edges,
                                   const PoseOverlayOptions& options,
                                   uint64_t now_host_ns,
                                   std::vector<PoseOverlayDrawCommand>* out,
                                   PoseOverlayStats* stats);

}  // namespace orange::gui
