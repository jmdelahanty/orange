#pragma once

#include <string>

namespace yolo_event_log {

struct SyntheticYoloEventConfig {
    std::string mode = "off";
    int every_n_frames = 10;
    std::string pattern = "alternating";
    bool emit_zero_detections = true;
    int label = 0;
    double confidence = 0.9;

    bool enabled() const {
        return mode == "synthetic";
    }
};

}  // namespace yolo_event_log
