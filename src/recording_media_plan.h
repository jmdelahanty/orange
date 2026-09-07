#pragma once
#include "json.hpp"
#include <optional>
#include <string>
#include <vector>

namespace orange::recording {
// Product identity is independent of encoder transport (native/split-GOP/IPC).
enum class RecordingMediaMode { FullFrame, FullFrameAndMovingCrops, RegisteredContextAndMovingCrops };
const char* RecordingMediaModeName(RecordingMediaMode);

struct RecordingMediaSelection {
    // Absent preserves the existing per-camera recording/crop choices. It is
    // not a fourth product mode and cannot silently opt a camera into crop-only.
    std::optional<RecordingMediaMode> mode;
    static RecordingMediaSelection Parse(const nlohmann::json&);
    nlohmann::json ToJson() const;
    bool FullFrame() const;
    bool MovingCrops(bool existing_crop_choice) const;
    bool RequiresContext() const;
};

struct RecordingMediaCameraInput {
    std::string serial;
    bool participates = false; // logical recording membership, not an encoder switch
    bool moving_crops = false;
};
struct RecordingMediaCameraPlan {
    std::string serial;
    RecordingMediaMode mode = RecordingMediaMode::FullFrame;
    bool FullFrame() const;
    bool MovingCrops() const;
};
struct RecordingMediaPlan {
    RecordingMediaSelection selection;
    std::vector<RecordingMediaCameraPlan> cameras;
    static RecordingMediaPlan Resolve(const RecordingMediaSelection&,
                                      const std::vector<RecordingMediaCameraInput>&);
    bool HasFullFrame() const;
    bool HasMovingCrops() const;
    bool RequiresContext() const;
    const RecordingMediaCameraPlan* Find(const std::string& serial) const;
    nlohmann::json ToJson() const;
};

// Product requirements, independent of transport. Called before claiming a run.
void RequireCropOnlyRecordingInputs(const RecordingMediaSelection&, bool master_enabled,
    bool context_enabled, bool real_full_rate_detector, bool external_moving_crops);
} // namespace orange::recording
