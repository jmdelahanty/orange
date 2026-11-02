// GUI side of the image-levels check (src/image_levels.h): one native
// full-resolution frame per camera, 2 s after the stream delivers frames,
// statistics off the GUI thread, an iris re-home and a second measurement when
// the mean is out of band. Never measures while a recording is starting, running
// or finalizing (the copy reads the landing die). Results go to
// image_levels::GuiResultStore() for the recording start snapshot.
#pragma once

#include "camera.h"
#include "image_levels.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

class SpatialSnapshotWorker;
struct SpatialSnapshotResult;

namespace orange::gui {

class ImageLevelsCheck {
public:
    struct Inputs {
        bool enabled = true;
        bool rehome_iris = true;
        int housekeeping_cpu = -1;
        bool streaming = false;
        bool recording_busy = false;
        // Blocking iris re-home for one camera (iris 0, then the configured
        // value); run on the checker's own thread.
        std::function<void(int camera_index)> rehome_iris_blocking;
    };

    ~ImageLevelsCheck();

    // GUI thread, every frame.
    void Poll(const Inputs& inputs, CameraParams* cameras, int num_cameras, SpatialSnapshotWorker* const* workers);
    // For other consumers that drain a worker's completed snapshots: true when
    // the result belongs to this check (it is then kept here).
    bool TryConsume(SpatialSnapshotResult* result);
    // Operator: measure this camera again now (ignored while recording).
    void RequestMeasureNow(int camera_index);

    // Latest per-camera view for the GUI panel.
    struct View {
        std::string phase;
        nlohmann::json result;  // EvaluateCamera output, or null
    };
    View CameraView(int camera_index) const;
    std::optional<image_levels::Measurement> LastMeasurement(int camera_index) const;
    // GUI thread. True while this camera's check holds, or is about to take, its
    // snapshot worker's single native request: settling, requested, computing,
    // re-homing, a pending Measure now, or idle on a stream that has not yet
    // delivered frames. Another native-snapshot user (the empty-background
    // capture) refuses the camera while this is true instead of queuing.
    bool Busy(int camera_index) const;

private:
    enum class Phase { kIdle, kSettling, kRequested, kComputing, kRehoming, kDone };
    struct Camera {
        Phase phase = Phase::kIdle;
        std::string serial;
        std::chrono::steady_clock::time_point settle_from{};
        std::chrono::steady_clock::time_point requested_at{};
        double settle_seconds = 2.0;
        uint64_t request_id = 0;
        std::string operation_id;
        std::unique_ptr<SpatialSnapshotResult> consumed;
        std::future<image_levels::Measurement> computing;
        std::optional<image_levels::Measurement> first;
        std::optional<image_levels::Measurement> measurement;
        bool rehomed = false;
        bool second_pass = false;
        bool armed = false;  // enabled, streaming and a worker: idle means about to settle
        std::string not_measured_reason;
        std::shared_ptr<std::atomic<bool>> rehome_done;
        std::thread rehome_thread;
        nlohmann::json result;
    };

    void reset(Camera* camera, SpatialSnapshotWorker* worker);
    void publish(Camera* camera, const CameraParams& params);
    static const char* phase_name(Phase phase);

    std::vector<Camera> cameras_;
    std::vector<int> measure_now_;
};

ImageLevelsCheck& GuiImageLevelsCheck();

}  // namespace orange::gui
