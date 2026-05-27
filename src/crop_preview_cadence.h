#ifndef ORANGE_CROP_PREVIEW_CADENCE_H
#define ORANGE_CROP_PREVIEW_CADENCE_H

#include "camera.h"
#include <atomic>
#include <cstdint>

class CropPreviewCadence {
public:
    struct Decision {
        bool offered = false;
        bool update = false;
        bool skipped_by_cadence = false;
    };

    explicit CropPreviewCadence(
        int max_fps = CameraCropPipelineConfig::kDefaultPreviewMaxFps)
    {
        SetMaxFps(max_fps);
    }

    void SetMaxFps(int max_fps)
    {
        max_fps_ = sanitize_camera_crop_preview_max_fps(max_fps);
        min_interval_ns_ = max_fps_ > 0
            ? 1000000000ull / static_cast<uint64_t>(max_fps_)
            : 0;
    }

    int MaxFps() const { return max_fps_; }

    void SetDisplayEnabled(bool enabled)
    {
        const bool was_enabled = display_enabled_.exchange(
            enabled,
            std::memory_order_acq_rel);
        if (enabled && !was_enabled) {
            force_next_update_.store(true, std::memory_order_release);
        }
    }

    bool DisplayEnabled() const { return display_enabled_.load(std::memory_order_acquire); }

    Decision ShouldUpdate(bool preview_available, bool blank_preview, uint64_t now_ns)
    {
        Decision decision;
        if (!preview_available || !DisplayEnabled()) {
            return decision;
        }

        decision.offered = true;

        if (max_fps_ <= 0) {
            decision.update = true;
            return decision;
        }

        if (force_next_update_.exchange(false, std::memory_order_acq_rel)) {
            decision.update = true;
            return decision;
        }

        if (blank_preview && last_preview_was_blank_) {
            decision.skipped_by_cadence = true;
            return decision;
        }

        if (blank_preview != last_preview_was_blank_) {
            decision.update = true;
            return decision;
        }

        if (last_update_ns_ == 0 || now_ns - last_update_ns_ >= min_interval_ns_) {
            decision.update = true;
            return decision;
        }

        decision.skipped_by_cadence = true;
        return decision;
    }

    void MarkUpdated(bool blank_preview, uint64_t now_ns)
    {
        last_update_ns_ = now_ns;
        last_preview_was_blank_ = blank_preview;
    }

private:
    int max_fps_ = CameraCropPipelineConfig::kDefaultPreviewMaxFps;
    uint64_t min_interval_ns_ = 1000000000ull / CameraCropPipelineConfig::kDefaultPreviewMaxFps;
    uint64_t last_update_ns_ = 0;
    bool last_preview_was_blank_ = true;
    std::atomic<bool> display_enabled_{true};
    std::atomic<bool> force_next_update_{false};
};

#endif  // ORANGE_CROP_PREVIEW_CADENCE_H
