#include "headless_registered_context.h"
#include <stdexcept>

namespace orange::recording {
void HeadlessRegisteredContext::AddCamera(int index, CameraParams* params,
                                         SafeQueue<WORKER_ENTRY*>& recycle, int cpu) {
    if (!params || params->pixel_format != "Mono8" || cameras_.count(index))
        throw std::runtime_error("registered context requires unique Mono8 cameras");
    auto& camera = cameras_[index];
    const auto name = "RegisteredContext_" + params->camera_serial;
    camera.worker = std::make_unique<SpatialSnapshotWorker>(name.c_str(), params, recycle);
    camera.worker->SetNativeSnapshotCpu(cpu);
    camera.worker->SetMaxQueueSize(1);
    std::string error;
    if (!camera.worker->RequestNativeSnapshot("registered_context_v2", &camera.request_id, &error))
        throw std::runtime_error(error);
    if (camera.worker->StartThread() != 0) throw std::runtime_error("registered context worker could not start");
}
SpatialSnapshotWorker* HeadlessRegisteredContext::Worker(int index) const {
    const auto it = cameras_.find(index);
    return it == cameras_.end() ? nullptr : it->second.worker.get();
}
bool HeadlessRegisteredContext::Poll() {
    for (auto& item : cameras_) {
        auto& camera = item.second;
        if (camera.stopped) continue;
        if (camera.worker->HasFatalError()) throw std::runtime_error(camera.worker->GetFatalErrorMessage());
        SpatialSnapshotResult frame;
        if (!camera.worker->PopCompletedSnapshot(&frame)) continue;
        if (!frame.ok || frame.request_id != camera.request_id || frame.operation_id != "registered_context_v2" ||
            frame.capture_representation != "native_bytes" || frame.pixel_format != GVSP_PIX_MONO8 ||
            frame.requested_frame_count != 1 || frame.completed_frame_count != 1)
            throw std::runtime_error("registered context snapshot rejected: " + frame.error);
        RegisteredContextFrame native;
        native.camera_serial = frame.camera_serial; native.width = frame.width; native.height = frame.height;
        native.local_frame_id = frame.local_frame_id; native.camera_frame_id = frame.camera_frame_id;
        native.recording_frame_id = frame.recording_frame_id;
        native.camera_timestamp_ns = frame.camera_timestamp_ns; native.timestamp_sys_ns = frame.timestamp_sys_ns;
        native.mono8 = std::move(frame.native_bytes);
        evidence.Accept(native);
        camera.worker->StopThread(); camera.stopped = true;
    }
    return evidence.Complete();
}
void HeadlessRegisteredContext::Stop() {
    for (auto& item : cameras_) if (item.second.worker && !item.second.stopped) {
        item.second.worker->StopThread(); item.second.stopped = true;
    }
}
}
