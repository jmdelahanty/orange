// Empty-dish background capture in the GUI (docs/empty_background_capture_design.md).
//
// One capture = one 17-frame burst on every streaming camera
// (EmptyBackgroundBurstWorker, reserved at stream start), then the PNGs and
// background.json written off the GUI thread on a housekeeping CPU
// (empty_background::WriteCapture). Started by the GUI panel or by the
// local-control method capture_empty_background (queued by the command drain,
// started by Poll on the GUI thread). Refused while a recording is armed,
// recording or finalizing, while another capture runs, while a camera's
// image-levels check holds its native snapshot, and for a local-control
// request while Orange writes version 1 (the requester needs version 2).
//
// The state of the last capture is published to local-control status as
// local_control.empty_background_capture (StatusJson): request identity, state
// (bursting | writing | captured | failed | refused), capture_id, descriptor
// path and sha256 once written, and per camera the burst outcome with the
// lowest free pool counts seen during the burst.
#pragma once

#include "camera.h"
#include "empty_background.h"
#include "orange_local_control.h"
#include "video_capture.h"

#include <chrono>
#include <functional>
#include <future>
#include <map>
#include <optional>
#include <string>
#include <vector>

class EmptyBackgroundBurstWorker;

namespace orange::gui {

class PerCameraStreamRuntime;

class EmptyBackgroundCapture {
public:
    struct Inputs {
        bool streaming = false;
        bool recording_busy = false;  // armed, recording, draining or finalizing
        int housekeeping_cpu = -1;    // -1: the GUI thread's lowest CPU
        CameraParams* cameras = nullptr;
        const CameraEachSelect* selection = nullptr;
        int num_cameras = 0;
        std::vector<PerCameraStreamRuntime>* runtimes = nullptr;
        std::function<bool(int camera_index)> image_levels_busy;
    };
    struct Request {
        std::string request_id;    // local control; empty for the panel
        std::string operation_id;  // local control
        nlohmann::json requester = nullptr;  // orange.requester.v1
        std::string declared_by;   // version 2
        empty_background::gen::OrangeEmptyBackgroundCaptureV1SceneDeclaration scene_declaration;
    };

    static constexpr std::chrono::seconds kBurstTimeout{5};

    // GUI thread: starts at once (panel). False with the reason.
    bool Start(const Request& request, const Inputs& in, std::string* error);
    // Command drain (GUI thread): a capture_empty_background request, validated
    // and started by the next Poll. A malformed one is recorded as refused.
    void QueueLocalControl(const orange::control::PendingLocalControlCommand& command);
    // GUI thread, every frame.
    void Poll(const Inputs& in);
    bool Busy() const { return phase_ == Phase::kBursting || phase_ == Phase::kWriting; }
    nlohmann::json StatusJson() const { return status_; }
    // The newest capture this process wrote (GUI selection at record start).
    std::optional<empty_background::CaptureReceipt> LastReceipt() const { return last_receipt_; }

private:
    enum class Phase { kIdle, kBursting, kWriting };
    struct CameraBurstState {
        int camera_index = -1;
        std::string serial;
        EmptyBackgroundBurstWorker* worker = nullptr;
        empty_background::CameraConfiguration configuration;
        bool done = false;
        empty_background::CameraBurst burst;
        nlohmann::json evidence = nlohmann::json::object();
    };
    void refuse(const Request& request, const std::string& reason);
    void finish_bursts(const Inputs& in);
    void publish(const std::string& state, const std::string& error = {});

    Phase phase_ = Phase::kIdle;
    Request active_;
    empty_background::CapturePlan plan_;
    std::vector<CameraBurstState> cameras_;
    std::chrono::steady_clock::time_point started_;
    std::string started_at_utc_;
    std::future<empty_background::CaptureReceipt> writer_;
    std::optional<orange::control::PendingLocalControlCommand> queued_;
    std::optional<empty_background::CaptureReceipt> last_receipt_;
    nlohmann::json status_ = nullptr;
};

EmptyBackgroundCapture& GuiEmptyBackgroundCapture();

}  // namespace orange::gui
