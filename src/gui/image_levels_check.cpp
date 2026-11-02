#include "gui/image_levels_check.h"

#include "image_levels_camera.h"
#include "project.h"
#include "spatial_snapshot_worker.h"

#include <pthread.h>
#include <sched.h>

#include <algorithm>
#include <iostream>

namespace orange::gui {
namespace {

constexpr double kSettleSeconds = 2.0;  // lens moves at stream start finish well within this
constexpr double kRequestTimeoutSeconds = 10.0;

int lowest_cpu_of_this_thread()
{
    cpu_set_t set;
    CPU_ZERO(&set);
    if (pthread_getaffinity_np(pthread_self(), sizeof(set), &set) != 0) return -1;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &set)) return cpu;
    }
    return -1;
}

double seconds_since(std::chrono::steady_clock::time_point t)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

}  // namespace

ImageLevelsCheck& GuiImageLevelsCheck()
{
    static ImageLevelsCheck check;
    return check;
}

ImageLevelsCheck::~ImageLevelsCheck()
{
    for (auto& camera : cameras_) {
        if (camera.rehome_thread.joinable()) camera.rehome_thread.join();
        if (camera.computing.valid()) camera.computing.wait();
    }
}

const char* ImageLevelsCheck::phase_name(Phase phase)
{
    switch (phase) {
    case Phase::kIdle: return "waiting_for_stream";
    case Phase::kSettling: return "settling";
    case Phase::kRequested: return "waiting_for_frame";
    case Phase::kComputing: return "computing";
    case Phase::kRehoming: return "rehoming_iris";
    case Phase::kDone: return "done";
    }
    return "unknown";
}

void ImageLevelsCheck::reset(Camera* camera, SpatialSnapshotWorker* worker)
{
    if (camera->phase == Phase::kRequested && worker && camera->request_id != 0) {
        worker->CancelUnclaimedRequest(camera->request_id);
    }
    if (camera->rehome_thread.joinable()) camera->rehome_thread.join();
    if (camera->computing.valid()) camera->computing.wait();
    const std::string serial = camera->serial;
    *camera = Camera{};
    camera->serial = serial;
}

void ImageLevelsCheck::publish(Camera* camera, const CameraParams& params)
{
    camera->result = image_levels::EvaluateCamera(
        params.image_levels, image_levels::LevelSettingsFromCamera(params), camera->measurement, camera->first,
        camera->rehomed,
        camera->phase == Phase::kDone ? camera->not_measured_reason
                                      : std::string("measurement_in_progress_") + phase_name(camera->phase));
    image_levels::GuiResultStore().Set(params.camera_serial, camera->result);
}

bool ImageLevelsCheck::TryConsume(SpatialSnapshotResult* result)
{
    if (!result) return false;
    for (auto& camera : cameras_) {
        if (camera.phase == Phase::kRequested && camera.request_id == result->request_id &&
            camera.operation_id == result->operation_id && !camera.consumed) {
            camera.consumed = std::make_unique<SpatialSnapshotResult>(std::move(*result));
            return true;
        }
    }
    return result->operation_id.rfind("image_levels_check_", 0) == 0;  // stale: drop it
}

void ImageLevelsCheck::RequestMeasureNow(int camera_index)
{
    measure_now_.push_back(camera_index);
}

bool ImageLevelsCheck::Busy(int camera_index) const
{
    if (camera_index < 0 || camera_index >= static_cast<int>(cameras_.size())) return false;
    for (int requested : measure_now_) {
        if (requested == camera_index) return true;
    }
    const Camera& c = cameras_[static_cast<size_t>(camera_index)];
    switch (c.phase) {
    case Phase::kSettling:
    case Phase::kRequested:
    case Phase::kComputing:
    case Phase::kRehoming:
        return true;
    case Phase::kIdle:
        return c.armed;
    case Phase::kDone:
        return false;
    }
    return false;
}

ImageLevelsCheck::View ImageLevelsCheck::CameraView(int camera_index) const
{
    if (camera_index < 0 || camera_index >= static_cast<int>(cameras_.size())) return {"waiting_for_stream", nullptr};
    const Camera& c = cameras_[static_cast<size_t>(camera_index)];
    return {phase_name(c.phase), c.result};
}

std::optional<image_levels::Measurement> ImageLevelsCheck::LastMeasurement(int camera_index) const
{
    if (camera_index < 0 || camera_index >= static_cast<int>(cameras_.size())) return std::nullopt;
    return cameras_[static_cast<size_t>(camera_index)].measurement;
}

void ImageLevelsCheck::Poll(const Inputs& in, CameraParams* cameras, int num_cameras, SpatialSnapshotWorker* const* workers)
{
    if (static_cast<int>(cameras_.size()) != num_cameras) {
        for (auto& c : cameras_) reset(&c, nullptr);
        cameras_.clear();
        cameras_.resize(static_cast<size_t>(std::max(0, num_cameras)));
        image_levels::GuiResultStore().Clear();
    }
    std::vector<int> measure_now;
    measure_now.swap(measure_now_);
    for (int i = 0; i < num_cameras; ++i) {
        Camera& c = cameras_[static_cast<size_t>(i)];
        CameraParams& params = cameras[i];
        SpatialSnapshotWorker* worker = workers ? workers[i] : nullptr;
        c.serial = params.camera_serial;
        c.armed = in.enabled && in.streaming && worker;
        if (!in.enabled || !in.streaming || !worker) {
            if (c.phase != Phase::kIdle || !c.result.is_null()) {
                reset(&c, worker);
                image_levels::GuiResultStore().Set(params.camera_serial,
                    image_levels::EvaluateCamera(params.image_levels, image_levels::LevelSettingsFromCamera(params),
                                                 std::nullopt, std::nullopt, false,
                                                 in.enabled ? "not_streaming" : "disabled_in_app_config"));
            }
            continue;
        }
        const Phase before = c.phase;
        for (int requested : measure_now) {
            if (requested == i && !in.recording_busy &&
                (c.phase == Phase::kDone || c.phase == Phase::kIdle || c.phase == Phase::kSettling)) {
                reset(&c, worker);
                c.phase = Phase::kSettling;
                c.settle_from = std::chrono::steady_clock::now();
                c.settle_seconds = 0.0;
            }
        }
        if (in.recording_busy && c.phase == Phase::kRequested && !c.consumed &&
            worker->CancelUnclaimedRequest(c.request_id)) {
            c.phase = Phase::kSettling;  // not claimed yet: no copy happened
        }
        if (in.recording_busy && (c.phase == Phase::kIdle || c.phase == Phase::kSettling)) {
            // Never start a copy once a recording is on its way.
            c.not_measured_reason = "recording_started_before_measurement";
            c.phase = Phase::kDone;
        }
        switch (c.phase) {
        case Phase::kIdle:
            if (worker->LatestObservedCameraTimestampNs() != 0) {
                c.phase = Phase::kSettling;
                c.settle_from = std::chrono::steady_clock::now();
                c.settle_seconds = kSettleSeconds;
            }
            break;
        case Phase::kSettling: {
            if (seconds_since(c.settle_from) < c.settle_seconds) break;
            const int cpu = in.housekeeping_cpu >= 0 ? in.housekeeping_cpu : lowest_cpu_of_this_thread();
            c.operation_id = "image_levels_check_" + params.camera_serial;
            std::string error;
            if (cpu < 0 || !worker->RequestNativeSnapshot(c.operation_id, &c.request_id, &error,
                                                         NativeSnapshotOptions{cpu, true})) {
                image_levels::Measurement failed;
                failed.measured_at_utc = get_current_utc_timestamp();
                failed.error = cpu < 0 ? "no housekeeping CPU" : error;
                c.measurement = failed;
                c.phase = Phase::kDone;
                break;
            }
            c.requested_at = std::chrono::steady_clock::now();
            c.phase = Phase::kRequested;
            break;
        }
        case Phase::kRequested: {
            if (!c.consumed) {
                SpatialSnapshotResult popped;
                if (worker->PopCompletedSnapshotForRequest(c.request_id, c.operation_id, &popped)) {
                    c.consumed = std::make_unique<SpatialSnapshotResult>(std::move(popped));
                }
            }
            if (!c.consumed) {
                if (seconds_since(c.requested_at) > kRequestTimeoutSeconds) {
                    worker->CancelUnclaimedRequest(c.request_id);
                    image_levels::Measurement failed;
                    failed.measured_at_utc = get_current_utc_timestamp();
                    failed.error = "no frame within 10 s";
                    c.measurement = failed;
                    c.phase = Phase::kDone;
                }
                break;
            }
            auto frame = std::make_shared<SpatialSnapshotResult>(std::move(*c.consumed));
            c.consumed.reset();
            const image_levels::LevelSettings settings = image_levels::LevelSettingsFromCamera(params);
            const std::string at = get_current_utc_timestamp();
            c.computing = std::async(std::launch::async, [frame, settings, at]() {
                image_levels::Measurement m;
                m.measured_at_utc = at;
                m.settings = settings;
                if (!frame->ok || frame->capture_representation != "native_bytes" || frame->native_bytes.empty()) {
                    m.error = frame->error.empty() ? "native snapshot failed" : frame->error;
                    return m;
                }
                m.ok = true;
                m.source = frame->capture_mode;
                m.native_source_storage = frame->native_source_storage;
                m.camera_frame_id = frame->camera_frame_id;
                m.camera_timestamp_ns = frame->camera_timestamp_ns;
                m.timestamp_sys_ns = frame->timestamp_sys_ns;
                m.statistics = image_levels::ComputeStatistics(frame->native_bytes.data(), frame->native_bytes.size());
                return m;
            });
            c.phase = Phase::kComputing;
            break;
        }
        case Phase::kComputing: {
            if (c.computing.wait_for(std::chrono::seconds(0)) != std::future_status::ready) break;
            image_levels::Measurement m = c.computing.get();
            const bool out_of_band = m.ok && !image_levels::MeanInBand(params.image_levels, m.statistics.mean);
            if (out_of_band && !c.second_pass && in.rehome_iris && params.lens_control_enabled &&
                in.rehome_iris_blocking && !in.recording_busy) {
                std::cout << "[IMAGE_LEVELS] Cam" << params.camera_serial << " mean " << m.statistics.mean
                          << " out of band; re-homing iris (0 then " << params.iris << ")" << std::endl;
                c.first = m;
                c.rehomed = true;
                c.rehome_done = std::make_shared<std::atomic<bool>>(false);
                auto done = c.rehome_done;
                auto rehome = in.rehome_iris_blocking;
                c.rehome_thread = std::thread([rehome, i, done]() {
                    rehome(i);
                    done->store(true, std::memory_order_release);
                });
                c.phase = Phase::kRehoming;
                break;
            }
            c.measurement = m;
            c.phase = Phase::kDone;
            break;
        }
        case Phase::kRehoming:
            if (!c.rehome_done || !c.rehome_done->load(std::memory_order_acquire)) break;
            c.rehome_thread.join();
            c.second_pass = true;
            c.settle_from = std::chrono::steady_clock::now();
            c.settle_seconds = kSettleSeconds;
            c.phase = Phase::kSettling;
            break;
        case Phase::kDone:
            break;
        }
        if (c.phase != before || c.result.is_null()) {
            publish(&c, params);
            if (c.phase == Phase::kDone && before != Phase::kDone) {
                const auto& r = c.result;
                std::cout << "[IMAGE_LEVELS] Cam" << params.camera_serial << " " << r.at("status").get<std::string>();
                if (c.measurement && c.measurement->ok) {
                    const auto& s = c.measurement->statistics;
                    std::cout << " mean " << s.mean << " saturated_fraction " << s.saturated_fraction << " p99 " << s.p99
                              << " p999 " << s.p999;
                }
                if (!r.at("warnings").empty()) std::cout << " warnings " << r.at("warnings").dump();
                if (c.measurement && !c.measurement->ok) std::cout << " error " << c.measurement->error;
                std::cout << std::endl;
            }
        }
    }
}

}  // namespace orange::gui
