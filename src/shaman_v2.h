#pragma once

#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <grp.h>
#include <new>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <type_traits>
#include <unistd.h>
#include <fcntl.h>

namespace shaman_v2 {

inline constexpr uint64_t kMagic = 0x4f524e4753484d32ULL; // ORNGSHM2
inline constexpr uint32_t kSchemaVersion = 2;
inline constexpr uint32_t kQueueSize = 64;
inline constexpr uint32_t kMaxObjects = 64;
inline constexpr uint32_t kMaxKeypointsPerObject = 32;
inline constexpr const char* kIpcGroup = "ipc";
inline constexpr mode_t kIpcMode = 0660;

enum class PayloadKind : uint32_t {
    kLatestTrackingState = 1,
    kStreamStatus = 2,
};

enum class DetectionStatus : uint32_t {
    kDisabled = 0,
    kNotScheduled = 1,
    kPending = 2,
    kDetections = 3,
    kZeroDetections = 4,
    kFailed = 5,
};

enum class PoseStatus : uint32_t {
    kDisabled = 0,
    kNotRequested = 1,
    kPending = 2,
    kPoses = 3,
    kNoResult = 4,
    kFailed = 5,
};

enum ObjectFlags : uint32_t {
    kObjectHasBbox = 1u << 0,
    kObjectHasPose = 1u << 1,
    kObjectSynthetic = 1u << 2,
};

enum KeypointFlags : uint16_t {
    kKeypointVisible = 1u << 0,
    kKeypointInterpolated = 1u << 1,
};

struct Keypoint {
    float x_px = 0.0f;
    float y_px = 0.0f;
    float confidence = 0.0f;
    uint16_t label_id = 0;
    uint16_t flags = 0;
};

struct Object {
    float x_px = 0.0f;
    float y_px = 0.0f;
    float width_px = 0.0f;
    float height_px = 0.0f;
    float confidence = 0.0f;
    int32_t label_id = 0;
    int32_t track_id = -1;
    uint32_t flags = 0;
    uint32_t keypoint_count = 0;
    Keypoint keypoints[kMaxKeypointsPerObject]{};
};

struct Slot {
    uint64_t magic = kMagic;
    uint32_t schema_version = kSchemaVersion;
    uint32_t slot_bytes = sizeof(Slot);

    uint64_t sequence_id = 0;
    uint32_t payload_kind = static_cast<uint32_t>(PayloadKind::kLatestTrackingState);
    uint32_t flags = 0;

    uint64_t state_frame_id = 0;
    uint64_t source_frame_id = 0;
    uint64_t camera_frame_id = 0;
    uint64_t recording_frame_id = 0;
    uint64_t camera_timestamp_ns = 0;
    uint64_t timestamp_sys_ns = 0;
    uint64_t orange_publish_timestamp_us_epoch = 0;
    uint64_t orange_publish_timestamp_us_monotonic = 0;

    uint32_t camera_id = 0;
    char camera_serial[32]{};
    uint32_t source_width_px = 0;
    uint32_t source_height_px = 0;

    uint32_t detection_status = static_cast<uint32_t>(DetectionStatus::kDisabled);
    uint32_t pose_status = static_cast<uint32_t>(PoseStatus::kDisabled);
    uint64_t detection_model_id_hash = 0;
    uint64_t pose_model_id_hash = 0;
    uint64_t pose_skeleton_id_hash = 0;

    uint32_t object_count = 0;
    Object objects[kMaxObjects]{};
};

struct SharedQueue {
    uint64_t magic = kMagic;
    uint32_t schema_version = kSchemaVersion;
    uint32_t queue_bytes = sizeof(SharedQueue);
    uint32_t slot_bytes = sizeof(Slot);
    uint32_t queue_size = kQueueSize;
    std::atomic<bool> initialized{false};
    std::atomic<uint64_t> writer_sequence{0};
    std::atomic<size_t> head{0};
    std::atomic<size_t> tail{0};
    std::atomic<uint64_t> push_failures{0};
    std::atomic<uint64_t> stale_suppressed{0};
    Slot queue[kQueueSize]{};
};

static_assert(sizeof(Keypoint) == 16, "Shaman v2 keypoint ABI changed");
static_assert(std::is_standard_layout<Keypoint>::value, "Keypoint must be ABI-stable");
static_assert(std::is_standard_layout<Object>::value, "Object must be ABI-stable");
static_assert(std::is_standard_layout<Slot>::value, "Slot must be ABI-stable");

inline uint64_t steady_time_us()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline uint64_t epoch_time_us()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

inline std::string queue_name_for_camera_serial(const std::string& camera_serial)
{
    if (camera_serial.empty()) {
        throw std::invalid_argument("shaman v2 camera serial is empty");
    }
    return "/shm_cam_" + camera_serial + "_v2";
}

inline bool slot_header_valid(const Slot& slot)
{
    return slot.magic == kMagic &&
           slot.schema_version == kSchemaVersion &&
           slot.slot_bytes == sizeof(Slot);
}

inline bool queue_header_valid(const SharedQueue& queue)
{
    return queue.magic == kMagic &&
           queue.schema_version == kSchemaVersion &&
           queue.queue_bytes == sizeof(SharedQueue) &&
           queue.slot_bytes == sizeof(Slot) &&
           queue.queue_size == kQueueSize;
}

inline void copy_camera_serial(char (&dest)[32], const std::string& serial)
{
    std::memset(dest, 0, sizeof(dest));
    std::strncpy(dest, serial.c_str(), sizeof(dest) - 1);
}

inline void unlink_queue(const std::string& queue_name)
{
    shm_unlink(queue_name.c_str());
}

class SharedLiveStateQueue {
public:
    SharedLiveStateQueue(const std::string& queue_name, bool writer)
        : writer_(writer), queue_name_(queue_name)
    {
        if (queue_name_.empty() || queue_name_[0] != '/') {
            throw std::invalid_argument("shaman v2 queue name must start with '/'");
        }

        const int flags = writer_ ? (O_RDWR | O_CREAT) : O_RDWR;
        fd_ = shm_open(queue_name_.c_str(), flags, kIpcMode);
        if (fd_ == -1) {
            throw std::runtime_error("shaman v2 shm_open failed for " + queue_name_ +
                                     ": " + std::strerror(errno));
        }

        if (writer_) {
            normalize_permissions();
        }

        struct stat st {};
        if (fstat(fd_, &st) == -1) {
            close_fd();
            throw std::runtime_error("shaman v2 fstat failed for " + queue_name_ +
                                     ": " + std::strerror(errno));
        }

        const bool needs_initialize = st.st_size < static_cast<off_t>(sizeof(SharedQueue));
        if (needs_initialize) {
            if (!writer_) {
                close_fd();
                throw std::runtime_error("shaman v2 queue is not initialized: " + queue_name_);
            }
            if (ftruncate(fd_, sizeof(SharedQueue)) == -1) {
                close_fd();
                throw std::runtime_error("shaman v2 ftruncate failed for " + queue_name_ +
                                         ": " + std::strerror(errno));
            }
        }

        shared_ = static_cast<SharedQueue*>(
            mmap(nullptr, sizeof(SharedQueue), PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));
        if (shared_ == MAP_FAILED) {
            shared_ = nullptr;
            close_fd();
            throw std::runtime_error("shaman v2 mmap failed for " + queue_name_ +
                                     ": " + std::strerror(errno));
        }

        if (needs_initialize) {
            new (shared_) SharedQueue();
            shared_->initialized.store(true, std::memory_order_release);
        } else {
            wait_initialized();
            if (!queue_header_valid(*shared_)) {
                close_mapping();
                close_fd();
                throw std::runtime_error(
                    "shaman v2 queue ABI mismatch for " + queue_name_ +
                    "; unlink stale shared memory before recreating it");
            }
        }
    }

    SharedLiveStateQueue(const SharedLiveStateQueue&) = delete;
    SharedLiveStateQueue& operator=(const SharedLiveStateQueue&) = delete;

    ~SharedLiveStateQueue()
    {
        close_mapping();
        close_fd();
    }

    bool push_latest_state(Slot slot)
    {
        if (!writer_ || !shared_) {
            return false;
        }
        if (slot.object_count > kMaxObjects) {
            shared_->push_failures.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        for (uint32_t i = 0; i < slot.object_count; ++i) {
            if (slot.objects[i].keypoint_count > kMaxKeypointsPerObject) {
                shared_->push_failures.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        }

        const size_t head = shared_->head.load(std::memory_order_relaxed);
        const size_t tail = shared_->tail.load(std::memory_order_acquire);
        const size_t next = (head + 1) % kQueueSize;
        if (next == tail) {
            shared_->push_failures.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        slot.magic = kMagic;
        slot.schema_version = kSchemaVersion;
        slot.slot_bytes = sizeof(Slot);
        slot.payload_kind = static_cast<uint32_t>(PayloadKind::kLatestTrackingState);
        slot.sequence_id =
            shared_->writer_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
        slot.orange_publish_timestamp_us_epoch = epoch_time_us();
        slot.orange_publish_timestamp_us_monotonic = steady_time_us();

        shared_->queue[head] = slot;
        shared_->head.store(next, std::memory_order_release);
        return true;
    }

    bool pop(Slot& out)
    {
        if (writer_ || !shared_) {
            return false;
        }
        const size_t head = shared_->head.load(std::memory_order_acquire);
        const size_t tail = shared_->tail.load(std::memory_order_relaxed);
        if (head == tail) {
            return false;
        }

        out = shared_->queue[tail];
        shared_->tail.store((tail + 1) % kQueueSize, std::memory_order_release);
        return slot_header_valid(out);
    }

    bool empty() const
    {
        if (!shared_) {
            return true;
        }
        return shared_->head.load(std::memory_order_acquire) ==
               shared_->tail.load(std::memory_order_acquire);
    }

    uint64_t push_failures() const
    {
        return shared_ ? shared_->push_failures.load(std::memory_order_relaxed) : 0;
    }

    uint64_t stale_suppressed() const
    {
        return shared_ ? shared_->stale_suppressed.load(std::memory_order_relaxed) : 0;
    }

    void note_stale_suppressed()
    {
        if (writer_ && shared_) {
            shared_->stale_suppressed.fetch_add(1, std::memory_order_relaxed);
        }
    }

private:
    void normalize_permissions()
    {
        const int chmod_rc = fchmod(fd_, kIpcMode);
        (void)chmod_rc;
        struct group* grp = getgrnam(kIpcGroup);
        if (grp) {
            const int chown_rc = fchown(fd_, -1, grp->gr_gid);
            (void)chown_rc;
        }
    }

    void wait_initialized()
    {
        for (int attempts = 0; attempts < 1000; ++attempts) {
            if (shared_->initialized.load(std::memory_order_acquire)) {
                return;
            }
            usleep(1000);
        }
        throw std::runtime_error("timeout waiting for shaman v2 queue init: " + queue_name_);
    }

    void close_mapping()
    {
        if (shared_) {
            munmap(shared_, sizeof(SharedQueue));
            shared_ = nullptr;
        }
    }

    void close_fd()
    {
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
    }

    bool writer_ = false;
    std::string queue_name_;
    int fd_ = -1;
    SharedQueue* shared_ = nullptr;
};

} // namespace shaman_v2
