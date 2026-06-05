#include "crop_producer_worker.h"

#include "crop_and_encode_worker.h"
#include "crop_preview_worker.h"
#include "pose_worker.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>

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
      crop_height_(SanitizeCropSize(crop_size_px)),
      encode_job_pool_("CropEncodeJob", kDefaultEncodeJobPoolSize),
      preview_job_pool_("CropPreviewJob", kDefaultPreviewJobPoolSize)
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
              << " encode_job_pool_misses=" << encode_job_pool_.GetStats().borrow_misses
              << " preview_job_pool_misses=" << preview_job_pool_.GetStats().borrow_misses
              << std::endl;
}

void CropProducerWorker::SetCropAndEncodeWorker(CropAndEncodeWorker* crop_worker)
{
    crop_worker_ = crop_worker;
    if (crop_worker_) {
        crop_worker_->SetCropProducerWorker(this);
    }
}

void CropProducerWorker::SetCropPreviewWorker(CropPreviewWorker* crop_preview_worker)
{
    crop_preview_worker_ = crop_preview_worker;
    if (crop_preview_worker_) {
        crop_preview_worker_->SetCropProducerWorker(this);
    }
}

void CropProducerWorker::SetPoseWorker(PoseWorker* pose_worker)
{
    pose_worker_ = pose_worker;
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

    ResetRecordingCounters();
    current_recording_folder_ = recording_folder;
    recording_drain_forwarded_ = false;
}

void CropProducerWorker::CloseRecording()
{
    if (current_recording_folder_.empty()) {
        return;
    }

    current_recording_folder_.clear();
    recording_drain_forwarded_ = false;
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
    if (crop_producer_) {
        crop_producer_->ResetRunFanoutCounters();
    }
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
    counters.encode_job_pool = ToCropJobPoolCounters(encode_job_pool_.GetStats());
    counters.preview_job_pool = ToCropJobPoolCounters(preview_job_pool_.GetStats());
    return counters;
}

CropProducerWorker::CropJobPoolCounters CropProducerWorker::ToCropJobPoolCounters(
    const CropEncodeJobPool::Stats& stats)
{
    CropJobPoolCounters counters;
    counters.capacity = stats.capacity;
    counters.available = stats.available;
    counters.active = stats.active;
    counters.high_water = stats.high_water;
    counters.misses_total = stats.borrow_misses;
    counters.invalid_returns_total = stats.invalid_returns;
    counters.double_returns_total = stats.double_returns;
    return counters;
}

CropProducerWorker::CropJobPoolCounters CropProducerWorker::ToCropJobPoolCounters(
    const CropPreviewJobPool::Stats& stats)
{
    CropJobPoolCounters counters;
    counters.capacity = stats.capacity;
    counters.available = stats.available;
    counters.active = stats.active;
    counters.high_water = stats.high_water;
    counters.misses_total = stats.borrow_misses;
    counters.invalid_returns_total = stats.invalid_returns;
    counters.double_returns_total = stats.double_returns;
    return counters;
}

void CropProducerWorker::log_job_pool_miss(const char* pool_name, uint64_t misses_total) const
{
    if (misses_total <= 8 || (misses_total & (misses_total - 1)) == 0) {
        std::cerr << "[CropProducerWorker] " << pool_name
                  << " pool exhausted for camera "
                  << (camera_params_ ? camera_params_->camera_serial : "unknown")
                  << " misses_total=" << misses_total
                  << std::endl;
    }
}

CropEncodeJob* CropProducerWorker::BorrowCropEncodeJob()
{
    CropEncodeJob* job = encode_job_pool_.Borrow();
    if (!job) {
        log_job_pool_miss(
            "CropEncodeJob",
            encode_job_pool_.GetStats().borrow_misses);
    }
    return job;
}

void CropProducerWorker::ReturnCropEncodeJob(CropEncodeJob* job)
{
    if (!job) {
        return;
    }
    if (!encode_job_pool_.Return(job)) {
        const auto stats = encode_job_pool_.GetStats();
        std::cerr << "[CropProducerWorker] Failed to return CropEncodeJob for camera "
                  << (camera_params_ ? camera_params_->camera_serial : "unknown")
                  << " invalid_returns=" << stats.invalid_returns
                  << " double_returns=" << stats.double_returns
                  << std::endl;
    }
}

CropPreviewJob* CropProducerWorker::BorrowCropPreviewJob()
{
    CropPreviewJob* job = preview_job_pool_.Borrow();
    if (!job) {
        log_job_pool_miss(
            "CropPreviewJob",
            preview_job_pool_.GetStats().borrow_misses);
    }
    return job;
}

void CropProducerWorker::ReturnCropPreviewJob(CropPreviewJob* job)
{
    if (!job) {
        return;
    }
    if (!preview_job_pool_.Return(job)) {
        const auto stats = preview_job_pool_.GetStats();
        std::cerr << "[CropProducerWorker] Failed to return CropPreviewJob for camera "
                  << (camera_params_ ? camera_params_->camera_serial : "unknown")
                  << " invalid_returns=" << stats.invalid_returns
                  << " double_returns=" << stats.double_returns
                  << std::endl;
    }
}

bool CropProducerWorker::ProcessEntryInline(WORKER_ENTRY* entry)
{
    std::lock_guard<std::mutex> lock(process_mutex_);
    return ProcessEntryImpl(entry, false);
}

bool CropProducerWorker::WorkerFunction(WORKER_ENTRY* entry)
{
    std::lock_guard<std::mutex> lock(process_mutex_);
    return ProcessEntryImpl(entry, true);
}

bool CropProducerWorker::ForwardRecordingDrainIfReady()
{
    if (!camera_control_ ||
        !camera_control_->recording_draining ||
        current_recording_folder_.empty() ||
        recording_drain_forwarded_) {
        return false;
    }

    if (GetCountQueueInSize() > 0) {
        return false;
    }

    if (crop_producer_) {
        crop_producer_->DrainPending(false);
        if (!crop_producer_->DrainReady()) {
            (void)PutObjectToQueueIn(nullptr);
            return false;
        }
    }

    if (crop_worker_) {
        (void)crop_worker_->PutObjectToQueueIn(nullptr);
    }
    if (crop_preview_worker_) {
        (void)crop_preview_worker_->PutObjectToQueueIn(nullptr);
    }
    if (pose_worker_) {
        (void)pose_worker_->PutObjectToQueueIn(nullptr);
    }
    recording_drain_forwarded_ = true;
    CloseRecording();
    return true;
}

bool CropProducerWorker::ProcessEntryImpl(WORKER_ENTRY*& entry, bool release_source_entry)
{
    if (crop_producer_) {
        crop_producer_->DrainPending(false);
    }

    if (!entry) {
        if (camera_control_ &&
            camera_control_->recording_draining &&
            !current_recording_folder_.empty() &&
            !ForwardRecordingDrainIfReady() &&
            GetCountQueueInSize() == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            (void)PutObjectToQueueIn(nullptr);
        }
        return false;
    }

    CropEncodeJob* job = BorrowCropEncodeJob();
    if (!job) {
        if (release_source_entry && crop_producer_) {
            crop_producer_->ReleaseSourceEntry(entry);
        }
        if (crop_producer_) {
            crop_producer_->DrainPending(false);
        }
        if (camera_control_ && camera_control_->recording_draining) {
            ForwardRecordingDrainIfReady();
        }
        return false;
    }
    struct EncodeJobReturnGuard {
        CropProducerWorker* owner = nullptr;
        CropEncodeJob*& job;
        ~EncodeJobReturnGuard()
        {
            if (owner && job) {
                owner->ReturnCropEncodeJob(job);
                job = nullptr;
            }
        }
    } encode_job_guard{this, job};
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
    CropFrame* producer_handoff_crop_frame = nullptr;
    auto release_producer_handoff_lease = [&]() {
        if (producer_handoff_crop_frame && crop_producer_) {
            crop_producer_->RecycleNow(producer_handoff_crop_frame);
            producer_handoff_crop_frame = nullptr;
        }
    };

    auto return_encode_job = [&](CropEncodeJob*& finished_job) {
        ReturnCropEncodeJob(finished_job);
        finished_job = nullptr;
    };

    auto enqueue_job = [&](CropEncodeJob*& ready_job) {
        if (!crop_worker_ || !ready_job) {
            return_encode_job(ready_job);
            return false;
        }
        const bool has_crop_frame = ready_job->crop_frame != nullptr;
        if (has_crop_frame && crop_producer_) {
            crop_producer_->NoteConsumerOffered(CropProducer::Consumer::kRecording);
            crop_producer_->RetainLease(ready_job->crop_frame);
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
        if (!crop_worker_->TryEnqueueJob(ready_job)) {
            queue_full_drops_++;
            if (has_crop_frame && crop_producer_) {
                crop_producer_->NoteConsumerDropped(CropProducer::Consumer::kRecording);
            }
            if (record_active) {
                run_queue_full_drops_.fetch_add(1, std::memory_order_relaxed);
            }
            if (ready_job->crop_frame && crop_producer_) {
                crop_producer_->RecycleNow(ready_job->crop_frame);
                ready_job->crop_frame = nullptr;
            }
            return_encode_job(ready_job);
            return false;
        }
        if (has_crop_frame && crop_producer_) {
            crop_producer_->NoteConsumerAccepted(CropProducer::Consumer::kRecording);
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
        ready_job = nullptr;
        return true;
    };

    auto enqueue_preview_job = [&](CropPreviewJob* preview_job) {
        if (!crop_preview_worker_ || !preview_job) {
            if (preview_job && preview_job->crop_frame && crop_producer_) {
                crop_producer_->RecycleNow(preview_job->crop_frame);
                preview_job->crop_frame = nullptr;
            }
            ReturnCropPreviewJob(preview_job);
            return false;
        }
        const bool has_crop_frame = preview_job->crop_frame != nullptr;
        if (has_crop_frame && crop_producer_) {
            crop_producer_->NoteConsumerOffered(CropProducer::Consumer::kPreview);
        }
        if (!crop_preview_worker_->TryEnqueuePreview(preview_job)) {
            if (has_crop_frame && crop_producer_) {
                crop_producer_->NoteConsumerDropped(CropProducer::Consumer::kPreview);
            }
            return false;
        }
        if (has_crop_frame && crop_producer_) {
            crop_producer_->NoteConsumerAccepted(CropProducer::Consumer::kPreview);
        }
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
                (void)enqueue_job(job);
            }
            if (release_source_entry) {
                crop_producer_->ReleaseSourceEntry(entry);
            }
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

            CropPreviewCadence::Decision preview_decision;
            if (crop_preview_worker_) {
                preview_decision = crop_preview_worker_->EvaluateOffer(false);
            }
            const bool preview_needs_crop_frame = preview_decision.update;
            const bool recording_needs_crop_frame = crop_worker_ && frame_should_encode;
            const bool needs_crop_frame =
                recording_needs_crop_frame || pose_enabled_ || preview_needs_crop_frame;
            if (needs_crop_frame) {
                CropProducer::ProduceResult produce_result = crop_producer_->Produce(
                    entry,
                    frame,
                    ix,
                    iy,
                    true,
                    &perf,
                    release_source_entry);
                job->crop_frame = produce_result.crop_frame;
                producer_handoff_crop_frame = produce_result.crop_frame;
                if (produce_result.dropped) {
                    perf.dropped = true;
                    perf.drop_reason = produce_result.drop_reason;
                    perf.total_ms = elapsed_ms(perf.worker_start_steady_ns, steady_now_ns());
                    if (frame_should_encode) {
                        (void)enqueue_job(job);
                    }
                    if (crop_producer_) {
                        crop_producer_->DrainPending(false);
                    }
                    release_producer_handoff_lease();
                    return false;
                }
            } else {
                if (release_source_entry) {
                    crop_producer_->ReleaseSourceEntry(entry);
                }
            }

            if (preview_needs_crop_frame && job->crop_frame && crop_producer_) {
                crop_producer_->RetainLease(job->crop_frame);
                CropPreviewJob* preview_job = BorrowCropPreviewJob();
                if (preview_job) {
                    preview_job->frame = frame;
                    preview_job->crop_frame = job->crop_frame;
                    preview_job->blank_preview = false;
                    (void)enqueue_preview_job(preview_job);
                } else {
                    crop_producer_->RecycleNow(job->crop_frame);
                    crop_producer_->NoteConsumerDropped(CropProducer::Consumer::kPreview);
                }
            }
        } else {
            frame.blank_frame = true;
            perf.blank_frame = true;
            if (crop_preview_worker_) {
                const CropPreviewCadence::Decision preview_decision =
                    crop_preview_worker_->EvaluateOffer(true);
                if (preview_decision.update) {
                    CropPreviewJob* preview_job = BorrowCropPreviewJob();
                    if (preview_job) {
                        preview_job->frame = frame;
                        preview_job->blank_preview = true;
                        (void)enqueue_preview_job(preview_job);
                    }
                }
            }
            if (release_source_entry) {
                crop_producer_->ReleaseSourceEntry(entry);
            }
        }

        if (job->crop_frame && crop_worker_ && frame_should_encode) {
            (void)enqueue_job(job);
            release_producer_handoff_lease();
            if (crop_producer_) {
                crop_producer_->DrainPending(false);
            }
            return false;
        }

        if (!has_detection && crop_worker_ && frame_should_encode) {
            (void)enqueue_job(job);
            if (crop_producer_) {
                crop_producer_->DrainPending(false);
            }
            return false;
        }

        if (job->crop_frame) {
            release_producer_handoff_lease();
            job->crop_frame = nullptr;
        }
    } catch (const std::exception& e) {
        std::cerr << "[CropProducerWorker] Exception processing frame " << frame.local_frame_id
                  << ": " << e.what() << std::endl;
        perf.dropped = true;
        perf.drop_reason = "exception";
        perf.total_ms = elapsed_ms(perf.worker_start_steady_ns, steady_now_ns());
        if (frame_should_encode) {
            (void)enqueue_job(job);
        }
        if (crop_producer_) {
            crop_producer_->DrainPending(false);
        }
        release_producer_handoff_lease();
    }

    if (entry && crop_producer_ && release_source_entry) {
        crop_producer_->ReleaseSourceEntry(entry);
    }
    if (crop_producer_) {
        crop_producer_->DrainPending(false);
    }
    if (!frame_should_encode && camera_control_ && camera_control_->recording_draining) {
        ForwardRecordingDrainIfReady();
    }
    return false;
}
