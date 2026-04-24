#include "crop_producer_worker.h"

#include "crop_and_encode_worker.h"
#include "pose_worker.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>

namespace {

uint64_t steady_now_ns()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

double elapsed_ms(uint64_t start_ns, uint64_t end_ns)
{
    if (end_ns < start_ns) {
        return 0.0;
    }
    return static_cast<double>(end_ns - start_ns) / 1000000.0;
}

}  // namespace

int CropProducerWorker::SanitizeCropSize(int requested_size_px)
{
    return sanitize_camera_crop_size_px(requested_size_px);
}

CropProducerWorker::CropProducerWorker(
    const char* name,
    CameraParams* camera_params,
    SafeQueue<WORKER_ENTRY*>& recycle_queue,
    CameraControl* camera_control,
    int crop_size_px)
    : CThreadWorker<WORKER_ENTRY>(name),
      camera_params_(camera_params),
      recycle_queue_(recycle_queue),
      camera_control_(camera_control),
      crop_width_(SanitizeCropSize(crop_size_px)),
      crop_height_(SanitizeCropSize(crop_size_px))
{
    crop_producer_ = std::make_unique<CropProducer>(
        camera_params_,
        recycle_queue_,
        crop_width_,
        crop_height_);
}

CropProducerWorker::~CropProducerWorker()
{
    crop_producer_.reset();

    std::cout << "[CropProducerWorker] Summary for " << threadName
              << " jobs_offered=" << jobs_offered_
              << " jobs_enqueued=" << jobs_enqueued_
              << " queue_full_drops=" << queue_full_drops_
              << " blank_jobs_offered=" << blank_jobs_offered_
              << " blank_jobs=" << blank_jobs_enqueued_
              << " dropped_jobs_offered=" << dropped_jobs_offered_
              << " dropped_jobs=" << dropped_jobs_enqueued_
              << std::endl;
}

void CropProducerWorker::SetCropAndEncodeWorker(CropAndEncodeWorker* crop_worker)
{
    crop_worker_ = crop_worker;
}

void CropProducerWorker::SetPoseWorker(PoseWorker* pose_worker)
{
    pose_enabled_ = (pose_worker != nullptr);
    if (crop_producer_) {
        crop_producer_->SetPoseWorker(pose_worker);
    }
}

void CropProducerWorker::RotateRecordingFolder(const std::string& recording_folder)
{
    if (current_recording_folder_ == recording_folder) {
        return;
    }

    if (!current_recording_folder_.empty()) {
        ResetRecordingCounters();
    }

    current_recording_folder_ = recording_folder;
}

void CropProducerWorker::CloseRecording()
{
    if (current_recording_folder_.empty()) {
        return;
    }

    current_recording_folder_.clear();
    ResetRecordingCounters();
}

void CropProducerWorker::ResetRecordingCounters()
{
    run_jobs_offered_.store(0, std::memory_order_relaxed);
    run_jobs_enqueued_.store(0, std::memory_order_relaxed);
    run_queue_full_drops_.store(0, std::memory_order_relaxed);
    run_blank_jobs_offered_.store(0, std::memory_order_relaxed);
    run_blank_jobs_enqueued_.store(0, std::memory_order_relaxed);
    run_dropped_jobs_offered_.store(0, std::memory_order_relaxed);
    run_dropped_jobs_enqueued_.store(0, std::memory_order_relaxed);
}

CropProducerWorker::RecordingCounters CropProducerWorker::GetRecordingCounters() const
{
    RecordingCounters counters;
    counters.jobs_offered = run_jobs_offered_.load(std::memory_order_relaxed);
    counters.jobs_enqueued = run_jobs_enqueued_.load(std::memory_order_relaxed);
    counters.queue_full_drops = run_queue_full_drops_.load(std::memory_order_relaxed);
    counters.blank_jobs_offered = run_blank_jobs_offered_.load(std::memory_order_relaxed);
    counters.blank_jobs_enqueued = run_blank_jobs_enqueued_.load(std::memory_order_relaxed);
    counters.dropped_jobs_offered = run_dropped_jobs_offered_.load(std::memory_order_relaxed);
    counters.dropped_jobs_enqueued = run_dropped_jobs_enqueued_.load(std::memory_order_relaxed);
    return counters;
}

bool CropProducerWorker::WorkerFunction(WORKER_ENTRY* entry)
{
    if (crop_producer_) {
        crop_producer_->DrainPending(false);
    }

    if (!entry) {
        return false;
    }

    std::unique_ptr<CropEncodeJob> job = std::make_unique<CropEncodeJob>();
    CropFrameSnapshot& frame = job->frame;
    CropEncodePerfSample& perf = job->perf;
    frame.crop_producer_worker_start_host_ns = steady_now_ns();

    frame.recording_frame_id = entry->recording_frame_id;
    frame.local_frame_id = entry->frame_id;
    frame.camera_frame_id = entry->camera_frame_id;
    frame.timestamp = entry->timestamp;
    frame.timestamp_sys = entry->timestamp_sys;
    frame.recording_folder = entry->recording_folder;
    frame.source_width = entry->width;
    frame.source_height = entry->height;
    frame.acquisition_receive_host_ns = entry->acquisition_receive_host_ns;
    frame.yolo_detect_done_host_ns = entry->yolo_detect_done_host_ns;

    perf.worker_start_steady_ns = steady_now_ns();
    perf.queue_depth_start = GetCountQueueInSize();

    const bool frame_should_encode =
        entry->recording_frame_id > 0 && !entry->recording_folder.empty();
    perf.encode_active = frame_should_encode;

    auto enqueue_job = [&](std::unique_ptr<CropEncodeJob> ready_job) {
        if (!crop_worker_ || !ready_job) {
            return false;
        }
        const bool record_active =
            ready_job->frame.recording_frame_id > 0 && !ready_job->frame.recording_folder.empty();
        jobs_offered_++;
        if (record_active) {
            run_jobs_offered_.fetch_add(1, std::memory_order_relaxed);
        }
        if (ready_job->perf.blank_frame) {
            blank_jobs_offered_++;
            if (record_active) {
                run_blank_jobs_offered_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (ready_job->perf.dropped) {
            dropped_jobs_offered_++;
            if (record_active) {
                run_dropped_jobs_offered_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (!crop_worker_->TryEnqueueJob(ready_job.get())) {
            queue_full_drops_++;
            if (record_active) {
                run_queue_full_drops_.fetch_add(1, std::memory_order_relaxed);
            }
            if (ready_job->crop_frame && crop_producer_) {
                crop_producer_->RecycleNow(ready_job->crop_frame);
                ready_job->crop_frame = nullptr;
            }
            return false;
        }
        jobs_enqueued_++;
        if (record_active) {
            run_jobs_enqueued_.fetch_add(1, std::memory_order_relaxed);
        }
        if (ready_job->perf.blank_frame) {
            blank_jobs_enqueued_++;
            if (record_active) {
                run_blank_jobs_enqueued_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (ready_job->perf.dropped) {
            dropped_jobs_enqueued_++;
            if (record_active) {
                run_dropped_jobs_enqueued_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        ready_job.release();
        return true;
    };

    try {
        if (entry->width < crop_width_ || entry->height < crop_height_) {
            std::cerr << "[CropProducerWorker] Dropping crop frame for camera "
                      << camera_params_->camera_serial
                      << ": source frame "
                      << entry->width << "x" << entry->height
                      << " is smaller than crop "
                      << crop_width_ << "x" << crop_height_ << std::endl;
            perf.dropped = true;
            perf.drop_reason = "source_smaller_than_crop";
            perf.total_ms = elapsed_ms(perf.worker_start_steady_ns, steady_now_ns());
            if (frame_should_encode) {
                (void)enqueue_job(std::move(job));
            }
            crop_producer_->ReleaseSourceEntry(entry);
            if (crop_producer_) {
                crop_producer_->DrainPending(false);
            }
            return false;
        }

        const bool has_detection = !entry->detections.empty();
        perf.has_detection = has_detection;
        frame.has_detection = has_detection;

        if (has_detection) {
            const pose::Object best_detection = *std::max_element(
                entry->detections.begin(),
                entry->detections.end(),
                [](const pose::Object& a, const pose::Object& b) { return a.prob < b.prob; });

            const float cx = best_detection.rect.x + best_detection.rect.width * 0.5f;
            const float cy = best_detection.rect.y + best_detection.rect.height * 0.5f;
            const int ix = std::clamp(static_cast<int>(cx) - crop_width_ / 2, 0, entry->width - crop_width_);
            const int iy = std::clamp(static_cast<int>(cy) - crop_height_ / 2, 0, entry->height - crop_height_);
            perf.crop_x = ix;
            perf.crop_y = iy;
            perf.crop_w = crop_width_;
            perf.crop_h = crop_height_;
            frame.detection_confidence = best_detection.prob;
            frame.crop_x = ix;
            frame.crop_y = iy;
            frame.crop_w = crop_width_;
            frame.crop_h = crop_height_;
            frame.detection_x = best_detection.rect.x;
            frame.detection_y = best_detection.rect.y;
            frame.detection_w = best_detection.rect.width;
            frame.detection_h = best_detection.rect.height;

            const bool needs_crop_frame = crop_worker_ || pose_enabled_;
            if (needs_crop_frame) {
                CropProducer::ProduceResult produce_result = crop_producer_->Produce(
                    entry,
                    frame,
                    ix,
                    iy,
                    true,
                    &perf);
                job->crop_frame = produce_result.crop_frame;
                if (produce_result.dropped) {
                    perf.dropped = true;
                    perf.drop_reason = produce_result.drop_reason;
                    perf.total_ms = elapsed_ms(perf.worker_start_steady_ns, steady_now_ns());
                    if (frame_should_encode) {
                        (void)enqueue_job(std::move(job));
                    }
                    if (crop_producer_) {
                        crop_producer_->DrainPending(false);
                    }
                    return false;
                }
            } else {
                crop_producer_->ReleaseSourceEntry(entry);
            }
        } else {
            frame.blank_frame = true;
            perf.blank_frame = true;
            crop_producer_->ReleaseSourceEntry(entry);
        }

        if (job->crop_frame && crop_worker_) {
            (void)enqueue_job(std::move(job));
            if (crop_producer_) {
                crop_producer_->DrainPending(false);
            }
            return false;
        }

        if (!has_detection && crop_worker_) {
            (void)enqueue_job(std::move(job));
            if (crop_producer_) {
                crop_producer_->DrainPending(false);
            }
            return false;
        }

        if (job->crop_frame && crop_producer_) {
            crop_producer_->RecycleNow(job->crop_frame);
            job->crop_frame = nullptr;
        }
    } catch (const std::exception& e) {
        std::cerr << "[CropProducerWorker] Exception processing frame " << frame.local_frame_id
                  << ": " << e.what() << std::endl;
        perf.dropped = true;
        perf.drop_reason = "exception";
        perf.total_ms = elapsed_ms(perf.worker_start_steady_ns, steady_now_ns());
        if (frame_should_encode) {
            (void)enqueue_job(std::move(job));
        }
        if (crop_producer_) {
            crop_producer_->DrainPending(false);
        }
    }

    if (entry && crop_producer_) {
        crop_producer_->ReleaseSourceEntry(entry);
    }
    if (crop_producer_) {
        crop_producer_->DrainPending(false);
    }
    return false;
}
