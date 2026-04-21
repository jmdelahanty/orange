#pragma once

class DisplayPreviewCadence {
public:
    DisplayPreviewCadence(int max_fps, unsigned int source_frame_rate)
        : max_fps_(max_fps),
          source_frame_rate_(source_frame_rate),
          frames_per_source_frame_(
              (max_fps > 0 && source_frame_rate > 0)
                  ? static_cast<double>(max_fps) / static_cast<double>(source_frame_rate)
                  : 1.0) {}

    bool ShouldDisplayNextFrame() {
        if (IsUnlimited()) {
            return true;
        }

        if (credit_ >= 1.0) {
            credit_ -= 1.0;
            return true;
        }

        credit_ += frames_per_source_frame_;
        if (credit_ < 1.0) {
            return false;
        }

        credit_ -= 1.0;
        if (credit_ > 1.0) {
            credit_ = 0.0;
        }
        return true;
    }

private:
    bool IsUnlimited() const {
        return max_fps_ <= 0 ||
               source_frame_rate_ == 0 ||
               static_cast<unsigned int>(max_fps_) >= source_frame_rate_;
    }

    int max_fps_;
    unsigned int source_frame_rate_;
    double frames_per_source_frame_;
    double credit_ = 1.0;
};
