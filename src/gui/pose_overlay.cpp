// src/gui/pose_overlay.cpp — see pose_overlay.h.
#include "gui/pose_overlay.h"

#include <algorithm>
#include <cstring>

namespace orange::gui {

void PoseOverlayMailbox::Publish(const PoseOverlaySnapshot& snapshot) noexcept
{
    const uint64_t start = sequence_.load(std::memory_order_relaxed);
    sequence_.store(start + 1, std::memory_order_release);  // odd: write in progress
    std::atomic_thread_fence(std::memory_order_release);
    slot_ = snapshot;
    slot_.sequence = start + 2;
    std::atomic_thread_fence(std::memory_order_release);
    sequence_.store(start + 2, std::memory_order_release);  // even: consistent
    publishes_.fetch_add(1, std::memory_order_relaxed);
}

bool PoseOverlayMailbox::TryRead(PoseOverlaySnapshot* out) const noexcept
{
    if (!out) {
        return false;
    }
    for (int attempt = 0; attempt < 3; ++attempt) {
        const uint64_t before = sequence_.load(std::memory_order_acquire);
        if (before == 0) {
            return false;  // never published
        }
        if ((before & 1u) != 0) {
            continue;  // write in progress
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        PoseOverlaySnapshot copy = slot_;
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint64_t after = sequence_.load(std::memory_order_acquire);
        if (before == after) {
            *out = copy;
            return true;
        }
    }
    torn_reads_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

namespace {

bool map_normalized(float u, float v, const PoseOverlayRect& rect, PoseOverlayPoint* out)
{
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) {
        return false;
    }
    out->x = rect.min_x + u * rect.width();
    out->y = rect.min_y + v * rect.height();
    return true;
}

}  // namespace

bool map_pose_keypoint_to_screen(const PoseOverlaySnapshot& snapshot,
                                 int keypoint_index,
                                 PoseOverlayImageSpace space,
                                 int source_width,
                                 int source_height,
                                 const PoseOverlayRect& rect,
                                 PoseOverlayPoint* out)
{
    if (!out || keypoint_index < 0 || keypoint_index >= snapshot.keypoint_count ||
        keypoint_index >= kPoseOverlayMaxKeypoints || rect.width() <= 0.0f || rect.height() <= 0.0f) {
        return false;
    }
    const PoseOverlayKeypoint& kp = snapshot.keypoints[keypoint_index];
    if (space == PoseOverlayImageSpace::kFullFrame) {
        if (source_width <= 0 || source_height <= 0) {
            return false;
        }
        return map_normalized(kp.x_px / static_cast<float>(source_width),
                              kp.y_px / static_cast<float>(source_height), rect, out);
    }
    // The crop window shows the video crop, which can be larger than the pose
    // crop the keypoints came from (both are source-pixel rectangles, so no
    // rescaling of the keypoints is needed: only the right rectangle).
    const bool use_preview = snapshot.preview_crop_w > 0 && snapshot.preview_crop_h > 0;
    const int crop_x = use_preview ? snapshot.preview_crop_x : snapshot.crop_x;
    const int crop_y = use_preview ? snapshot.preview_crop_y : snapshot.crop_y;
    const int crop_w = use_preview ? snapshot.preview_crop_w : snapshot.crop_w;
    const int crop_h = use_preview ? snapshot.preview_crop_h : snapshot.crop_h;
    if (crop_w <= 0 || crop_h <= 0) {
        return false;
    }
    return map_normalized((kp.x_px - static_cast<float>(crop_x)) / static_cast<float>(crop_w),
                          (kp.y_px - static_cast<float>(crop_y)) / static_cast<float>(crop_h),
                          rect, out);
}

bool validate_pose_skeleton(const std::vector<PoseOverlaySkeletonEdge>& edges, int keypoint_count)
{
    for (const auto& edge : edges) {
        if (edge.a < 0 || edge.b < 0 || edge.a >= keypoint_count || edge.b >= keypoint_count || edge.a == edge.b) {
            return false;
        }
    }
    return true;
}

size_t build_pose_overlay_commands(const PoseOverlaySnapshot& snapshot,
                                   PoseOverlayImageSpace space,
                                   int source_width,
                                   int source_height,
                                   const PoseOverlayRect& rect,
                                   const std::vector<PoseOverlaySkeletonEdge>& edges,
                                   const PoseOverlayOptions& options,
                                   uint64_t now_host_ns,
                                   std::vector<PoseOverlayDrawCommand>* out,
                                   PoseOverlayStats* stats)
{
    if (!out) {
        return 0;
    }
    PoseOverlayStats local{};
    PoseOverlayStats* st = stats ? stats : &local;
    st->frames_considered++;
    if (snapshot.host_ns != 0 && now_host_ns > snapshot.host_ns &&
        now_host_ns - snapshot.host_ns > options.stale_after_ns) {
        st->stale_frames++;
        return 0;
    }

    const size_t start = out->size();
    const int count = std::min(snapshot.keypoint_count, kPoseOverlayMaxKeypoints);
    PoseOverlayPoint points[kPoseOverlayMaxKeypoints];
    bool mapped[kPoseOverlayMaxKeypoints] = {};
    uint64_t drawn = 0;
    for (int i = 0; i < count; ++i) {
        const PoseOverlayKeypoint& kp = snapshot.keypoints[i];
        if (!kp.visible || kp.confidence < options.min_confidence) {
            continue;
        }
        if (!map_pose_keypoint_to_screen(snapshot, i, space, source_width, source_height, rect, &points[i])) {
            st->keypoints_outside_image++;
            continue;
        }
        mapped[i] = true;
        ++drawn;
    }
    if (drawn == 0) {
        st->empty_frames++;
        return 0;
    }

    if (options.draw_bbox && space == PoseOverlayImageSpace::kFullFrame && snapshot.bbox_w > 0.0f &&
        snapshot.bbox_h > 0.0f && source_width > 0 && source_height > 0) {
        PoseOverlayDrawCommand cmd;
        cmd.kind = PoseOverlayDrawCommand::Kind::kRect;
        cmd.x0 = rect.min_x + snapshot.bbox_x / static_cast<float>(source_width) * rect.width();
        cmd.y0 = rect.min_y + snapshot.bbox_y / static_cast<float>(source_height) * rect.height();
        cmd.x1 = rect.min_x + (snapshot.bbox_x + snapshot.bbox_w) / static_cast<float>(source_width) * rect.width();
        cmd.y1 = rect.min_y + (snapshot.bbox_y + snapshot.bbox_h) / static_cast<float>(source_height) * rect.height();
        cmd.thickness = options.line_thickness_px;
        out->push_back(cmd);
    }
    if (options.draw_edges && validate_pose_skeleton(edges, count)) {
        for (const auto& edge : edges) {
            if (!mapped[edge.a] || !mapped[edge.b]) {
                continue;
            }
            PoseOverlayDrawCommand cmd;
            cmd.kind = PoseOverlayDrawCommand::Kind::kLine;
            cmd.x0 = points[edge.a].x;
            cmd.y0 = points[edge.a].y;
            cmd.x1 = points[edge.b].x;
            cmd.y1 = points[edge.b].y;
            cmd.thickness = options.line_thickness_px;
            cmd.confidence = std::min(snapshot.keypoints[edge.a].confidence, snapshot.keypoints[edge.b].confidence);
            out->push_back(cmd);
            st->edges_drawn++;
        }
    }
    for (int i = 0; i < count; ++i) {
        if (!mapped[i]) {
            continue;
        }
        PoseOverlayDrawCommand cmd;
        cmd.kind = PoseOverlayDrawCommand::Kind::kCircle;
        cmd.x0 = points[i].x;
        cmd.y0 = points[i].y;
        cmd.radius = options.point_radius_px;
        cmd.thickness = options.line_thickness_px;
        cmd.confidence = snapshot.keypoints[i].confidence;
        cmd.label_id = snapshot.keypoints[i].label_id;
        out->push_back(cmd);
    }
    st->frames_drawn++;
    st->keypoints_drawn += drawn;
    st->max_keypoints_per_frame = std::max<uint64_t>(st->max_keypoints_per_frame, drawn);
    return out->size() - start;
}

}  // namespace orange::gui
