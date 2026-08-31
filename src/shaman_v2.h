#pragma once

#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <grp.h>
#include <iomanip>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <type_traits>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>

namespace shaman_v2 {

inline constexpr uint64_t kMagic = 0x4f524e4753484d32ULL; // ORNGSHM2
// Protocol generation remains Shaman-v2. Revision 4 adds grouped-live
// observation provenance and producer restart identity. slot_bytes/queue_bytes
// make older shared-memory objects fail closed instead of being reinterpreted.
inline constexpr uint32_t kSchemaVersion = 4;
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

// A latest-state object vector has no stable cross-frame ordering. The value
// is carried in every slot so consumers cannot accidentally treat payload
// index as a track, region, or scientific fish identity.
enum class ObjectOrder : uint32_t {
    kUnspecified = 0,
    kUnorderedPayloadLocal = 1,
};

inline constexpr uint32_t kObjectOrderUnorderedPayloadLocal =
    static_cast<uint32_t>(ObjectOrder::kUnorderedPayloadLocal);

// Stable, numeric reasons for terminal/exceptional detection outcomes. These
// values are ABI, not display strings; descriptions belong in the contract.
enum class DetectionResultReason : uint32_t {
    kNone = 0,
    kNoSourceDetections = 1,
    kAllDetectionsRejectedByMask = 2,
    kObjectsTruncated = 3,
    kYoloWorkerEnqueueRejected = 4,
    kInferenceTimeout = 5,
    kCpuResultsSkipped = 6,
    kProcessingFailed = 7,
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

    // Empty iff recording_frame_id is zero. Otherwise this is the exact
    // sha256:<hex> token for recording_session.json.session_id.
    char recording_identity_token[72]{};

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

    // Grouped-live observation metadata is append-only after the v3 object
    // vector. source_detection_count is the detector/post-NMS count before
    // the outer spatial mask; retained_detection_count is the post-mask
    // vector count. transmitted_object_count is the number actually present
    // in objects[]; object_count remains its v3 compatibility alias.
    // objects_truncated is the number omitted due to SHAMAN capacity. A
    // nonzero truncation count requires failed status.
    uint32_t source_detection_count = 0;
    uint32_t retained_detection_count = 0;
    uint32_t transmitted_object_count = 0;
    uint32_t objects_truncated = 0;
    uint32_t object_order = kObjectOrderUnorderedPayloadLocal;
    uint32_t detection_reason =
        static_cast<uint32_t>(DetectionResultReason::kNone);

    // Producer generation changes on every authoritative writer restart.
    // producer_instance_id identifies this particular writer process/run.
    uint64_t producer_generation = 0;
    uint64_t producer_instance_id = 0;
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

    // Appended after the v3 queue ring. These atomics let a reader observe a
    // writer reset without interpreting a predecessor generation as current.
    std::atomic<uint64_t> producer_generation{0};
    std::atomic<uint64_t> producer_instance_id{0};
};

static_assert(sizeof(Keypoint) == 16, "Shaman v2 keypoint ABI changed");
static_assert(sizeof(Object) == 548, "Shaman v2 object ABI changed");
static_assert(sizeof(Slot) == 35368, "Shaman v2 slot ABI changed");
static_assert(sizeof(SharedQueue) == 2263640, "Shaman v2 queue ABI changed");
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

// ABI4 uses the standard FNV-1a 64-bit offset basis. Keep this literal in the
// shared header so Orange and Citrus cannot silently derive different model
// identities for the same string.
inline uint64_t fnv1a64(const std::string& value)
{
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char byte : value) {
        hash ^= static_cast<uint64_t>(byte);
        hash *= 1099511628211ULL;
    }
    return hash;
}

inline std::string queue_name_for_camera_serial(const std::string& camera_serial)
{
    if (camera_serial.empty()) {
        throw std::invalid_argument("shaman v2 camera serial is empty");
    }
    if (camera_serial.size() >= sizeof(Slot::camera_serial)) {
        throw std::invalid_argument(
            "shaman v2 camera serial exceeds the fixed ABI field");
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

inline bool detection_status_valid(uint32_t value)
{
    return value <= static_cast<uint32_t>(DetectionStatus::kFailed);
}

inline bool pose_status_valid(uint32_t value)
{
    return value <= static_cast<uint32_t>(PoseStatus::kFailed);
}

inline bool detection_reason_valid(uint32_t value)
{
    return value <= static_cast<uint32_t>(DetectionResultReason::kProcessingFailed);
}

// Validate the complete grouped-live payload before it crosses the SHM
// boundary. This is deliberately independent of slot_header_valid(): the
// writer stamps the header and generation immediately after this check.
inline bool slot_payload_valid(const Slot& slot)
{
    if (slot.payload_kind != static_cast<uint32_t>(PayloadKind::kLatestTrackingState) &&
        slot.payload_kind != static_cast<uint32_t>(PayloadKind::kStreamStatus)) {
        return false;
    }
    if (!detection_status_valid(slot.detection_status) ||
        !pose_status_valid(slot.pose_status) ||
        !detection_reason_valid(slot.detection_reason) ||
        slot.object_order != kObjectOrderUnorderedPayloadLocal ||
        slot.object_count > kMaxObjects ||
        slot.transmitted_object_count != slot.object_count ||
        slot.retained_detection_count < slot.transmitted_object_count ||
        slot.source_detection_count < slot.retained_detection_count ||
        slot.objects_truncated !=
            slot.retained_detection_count - slot.transmitted_object_count) {
        return false;
    }
    for (uint32_t index = 0; index < slot.object_count; ++index) {
        if (slot.objects[index].keypoint_count > kMaxKeypointsPerObject) {
            return false;
        }
    }

    const auto status = static_cast<DetectionStatus>(slot.detection_status);
    const auto reason = static_cast<DetectionResultReason>(slot.detection_reason);
    switch (status) {
        case DetectionStatus::kDisabled:
        case DetectionStatus::kNotScheduled:
        case DetectionStatus::kPending:
            return slot.source_detection_count == 0 &&
                   slot.retained_detection_count == 0 &&
                   slot.transmitted_object_count == 0 &&
                   slot.objects_truncated == 0 &&
                   reason == DetectionResultReason::kNone;
        case DetectionStatus::kDetections:
            return slot.retained_detection_count > 0 &&
                   slot.objects_truncated == 0 &&
                   reason == DetectionResultReason::kNone;
        case DetectionStatus::kZeroDetections:
            return slot.retained_detection_count == 0 &&
                   slot.transmitted_object_count == 0 &&
                   slot.objects_truncated == 0 &&
                   (reason == DetectionResultReason::kNoSourceDetections ||
                    reason == DetectionResultReason::kAllDetectionsRejectedByMask) &&
                   ((slot.source_detection_count == 0 &&
                     reason == DetectionResultReason::kNoSourceDetections) ||
                    (slot.source_detection_count > 0 &&
                     reason == DetectionResultReason::kAllDetectionsRejectedByMask));
        case DetectionStatus::kFailed:
            return reason != DetectionResultReason::kNone &&
                   ((reason == DetectionResultReason::kObjectsTruncated &&
                     slot.objects_truncated > 0) ||
                    (reason != DetectionResultReason::kObjectsTruncated &&
                     slot.objects_truncated == 0));
    }
    return false;
}

inline void copy_camera_serial(char (&dest)[32], const std::string& serial)
{
    std::memset(dest, 0, sizeof(dest));
    std::strncpy(dest, serial.c_str(), sizeof(dest) - 1);
}

inline void copy_recording_identity_token(
    char (&dest)[72],
    const std::string& token)
{
    std::memset(dest, 0, sizeof(dest));
    if (!token.empty()) {
        std::strncpy(dest, token.c_str(), sizeof(dest) - 1);
    }
}

inline void unlink_queue(const std::string& queue_name)
{
    shm_unlink(queue_name.c_str());
}

// The index lock is intentionally outside the shared-memory object so its
// synchronization cannot change the ABI4 layout. Keep this helper public and
// byte-for-byte identical in Citrus: queue-name sanitization is only a human
// readable prefix; the FNV suffix prevents names such as '-' and '_' from
// aliasing the same lock file.
inline std::string index_lock_path(const std::string& queue_name)
{
    std::ostringstream suffix;
    suffix << std::hex << std::nouppercase << std::setfill('0')
           << std::setw(16) << fnv1a64(queue_name);
    std::string path = "/tmp/orange_shaman_v2_index";
    path.reserve(path.size() + queue_name.size() + suffix.str().size() + 2);
    for (const unsigned char byte : queue_name) {
        const bool ascii_alnum =
            (byte >= 'a' && byte <= 'z') ||
            (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9');
        path.push_back(ascii_alnum ? static_cast<char>(byte) : '_');
    }
    path.push_back('_');
    path += suffix.str();
    path += ".lock";
    return path;
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
            if (flock(fd_, LOCK_EX | LOCK_NB) == -1) {
                close_fd();
                throw std::runtime_error(
                    "shaman v2 writer already active for " + queue_name_ +
                    "; refusing a second producer");
            }
            normalize_permissions();
        }

        sync_fd_ = open(index_lock_path(queue_name_).c_str(), O_RDWR | O_CREAT,
                        kIpcMode);
        if (sync_fd_ == -1) {
            close_fd();
            throw std::runtime_error(
                "shaman v2 index lock open failed for " + queue_name_ + ": " +
                std::strerror(errno));
        }
        normalize_sync_permissions();

        struct stat st {};
        if (fstat(fd_, &st) == -1) {
            close_sync_fd();
            close_fd();
            throw std::runtime_error("shaman v2 fstat failed for " + queue_name_ +
                                     ": " + std::strerror(errno));
        }

        const bool needs_initialize = st.st_size < static_cast<off_t>(sizeof(SharedQueue));
        if (needs_initialize) {
            if (!writer_) {
                close_sync_fd();
                close_fd();
                throw std::runtime_error("shaman v2 queue is not initialized: " + queue_name_);
            }
            if (ftruncate(fd_, sizeof(SharedQueue)) == -1) {
                close_sync_fd();
                close_fd();
                throw std::runtime_error("shaman v2 ftruncate failed for " + queue_name_ +
                                         ": " + std::strerror(errno));
            }
        }

        shared_ = static_cast<SharedQueue*>(
            mmap(nullptr, sizeof(SharedQueue), PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));
        if (shared_ == MAP_FAILED) {
            shared_ = nullptr;
            close_sync_fd();
            close_fd();
            throw std::runtime_error("shaman v2 mmap failed for " + queue_name_ +
                                     ": " + std::strerror(errno));
        }

        if (needs_initialize) {
            new (shared_) SharedQueue();
            if (!lock_index()) {
                close_mapping();
                close_sync_fd();
                close_fd();
                throw std::runtime_error(
                    "shaman v2 index lock failed while initializing " + queue_name_);
            }
            reset_for_new_writer();
            shared_->initialized.store(true, std::memory_order_release);
            unlock_index();
        } else {
            (void)wait_initialized();
            if (!queue_header_valid(*shared_)) {
                close_mapping();
                close_sync_fd();
                close_fd();
                throw std::runtime_error(
                    "shaman v2 queue ABI mismatch for " + queue_name_ +
                    "; unlink stale shared memory before recreating it");
            }
            if (writer_) {
                if (!lock_index()) {
                    close_mapping();
                    close_sync_fd();
                    close_fd();
                    throw std::runtime_error(
                        "shaman v2 index lock failed while resetting " + queue_name_);
                }
                reset_for_new_writer();
                shared_->initialized.store(true, std::memory_order_release);
                unlock_index();
            }
        }
    }

    SharedLiveStateQueue(const SharedLiveStateQueue&) = delete;
    SharedLiveStateQueue& operator=(const SharedLiveStateQueue&) = delete;

    ~SharedLiveStateQueue()
    {
        close_mapping();
        close_sync_fd();
        close_fd();
    }

    bool push_latest_state(Slot slot)
    {
        if (!writer_ || !shared_) {
            return false;
        }
        if (!lock_index()) {
            return false;
        }
        ScopedIndexLock index_lock(sync_fd_);
        if (!shared_->initialized.load(std::memory_order_acquire)) {
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

        // ABI4 requires callers to supply the complete grouped metadata. Do
        // not infer counts or rewrite a status/reason here: this is the final
        // SHM boundary and malformed payloads must fail closed.
        if (!slot_payload_valid(slot)) {
            shared_->push_failures.fetch_add(1, std::memory_order_relaxed);
            return false;
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
        slot.producer_generation = shared_->producer_generation.load(
            std::memory_order_acquire);
        slot.producer_instance_id = shared_->producer_instance_id.load(
            std::memory_order_acquire);
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
        if (!lock_index()) {
            return false;
        }
        ScopedIndexLock index_lock(sync_fd_);
        if (!shared_->initialized.load(std::memory_order_acquire)) {
            return false;
        }
        const size_t head = shared_->head.load(std::memory_order_acquire);
        const size_t tail = shared_->tail.load(std::memory_order_relaxed);
        if (head == tail) {
            return false;
        }
        out = shared_->queue[tail];
        shared_->tail.store((tail + 1) % kQueueSize, std::memory_order_release);
        if (!shared_->initialized.load(std::memory_order_acquire)) {
            return false;
        }
        const uint64_t producer_generation = shared_->producer_generation.load(
            std::memory_order_acquire);
        const uint64_t producer_instance_id = shared_->producer_instance_id.load(
            std::memory_order_acquire);
        return slot_header_valid(out) && slot_payload_valid(out) &&
               out.producer_generation == producer_generation &&
               out.producer_instance_id == producer_instance_id;
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

    uint64_t producer_generation() const
    {
        return shared_ ? shared_->producer_generation.load(std::memory_order_acquire) : 0;
    }

    uint64_t producer_instance_id() const
    {
        return shared_ ? shared_->producer_instance_id.load(std::memory_order_acquire) : 0;
    }

    void note_stale_suppressed()
    {
        if (writer_ && shared_) {
            shared_->stale_suppressed.fetch_add(1, std::memory_order_relaxed);
        }
    }

private:
    class ScopedIndexLock {
    public:
        explicit ScopedIndexLock(int fd) : fd_(fd) {}
        ~ScopedIndexLock()
        {
            if (fd_ >= 0) {
                (void)flock(fd_, LOCK_UN);
            }
        }
        ScopedIndexLock(const ScopedIndexLock&) = delete;
        ScopedIndexLock& operator=(const ScopedIndexLock&) = delete;

    private:
        int fd_ = -1;
    };

    bool lock_index() const noexcept
    {
        if (sync_fd_ < 0) {
            return false;
        }
        while (flock(sync_fd_, LOCK_EX) == -1) {
            if (errno != EINTR) {
                return false;
            }
        }
        return true;
    }

    void unlock_index() const noexcept
    {
        if (sync_fd_ >= 0) {
            (void)flock(sync_fd_, LOCK_UN);
        }
    }

    void reset_for_new_writer()
    {
        if (!writer_ || !shared_) {
            return;
        }
        // Readers observe initialized=false while the ring is cleared. This
        // prevents stale slots from a crashed predecessor being interpreted
        // as part of the new producer generation.
        shared_->initialized.store(false, std::memory_order_release);
        uint64_t generation = shared_->producer_generation.load(
            std::memory_order_relaxed);
        generation = generation == UINT64_MAX ? 1 : generation + 1;
        if (generation == 0) {
            generation = 1;
        }
        shared_->producer_generation.store(generation, std::memory_order_release);
        shared_->producer_instance_id.store(
            (static_cast<uint64_t>(getpid()) << 32) ^
                (steady_time_us() & 0xffffffffULL),
            std::memory_order_release);
        shared_->writer_sequence.store(0, std::memory_order_relaxed);
        shared_->head.store(0, std::memory_order_relaxed);
        shared_->tail.store(0, std::memory_order_relaxed);
        shared_->push_failures.store(0, std::memory_order_relaxed);
        shared_->stale_suppressed.store(0, std::memory_order_relaxed);
        for (Slot& queued_slot : shared_->queue) {
            queued_slot = Slot{};
        }
    }

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

    void normalize_sync_permissions()
    {
        const int chmod_rc = fchmod(sync_fd_, kIpcMode);
        (void)chmod_rc;
        struct group* grp = getgrnam(kIpcGroup);
        if (grp) {
            const int chown_rc = fchown(sync_fd_, -1, grp->gr_gid);
            (void)chown_rc;
        }
    }

    bool wait_initialized()
    {
        for (int attempts = 0; attempts < 1000; ++attempts) {
            if (shared_->initialized.load(std::memory_order_acquire)) {
                return true;
            }
            usleep(1000);
        }
        // A crashed predecessor can leave initialized=false after beginning
        // its reset. The exclusive writer lock makes recovery unambiguous;
        // readers still fail closed rather than consuming an uninitialized
        // queue.
        if (writer_) {
            return false;
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

    void close_sync_fd()
    {
        if (sync_fd_ >= 0) {
            close(sync_fd_);
            sync_fd_ = -1;
        }
    }

    bool writer_ = false;
    std::string queue_name_;
    int fd_ = -1;
    int sync_fd_ = -1;
    SharedQueue* shared_ = nullptr;
};

} // namespace shaman_v2
