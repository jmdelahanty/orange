#pragma once

// The wire format (structs, constants, layout asserts and the helpers both sides compute) is the
// shared jmdelahanty/shaman-ipc release pinned at third_party/shaman-ipc. This file keeps only
// Orange's queue writer.
#include <shaman_ipc/shaman_v2_wire.h>

#include <cerrno>
#include <grp.h>
#include <new>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>

namespace shaman_v2 {

inline void unlink_queue(const std::string& queue_name)
{
    shm_unlink(queue_name.c_str());
}

class SharedLiveStateQueue {
public:
    const std::string& name() const { return queue_name_; }

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

        // Only the writer creates the index lock. The lock lives in /tmp, a
        // sticky directory, and fs.protected_regular forbids even root from
        // O_CREAT-opening a regular file another user created there: a reader
        // that created the lock first (2026-09-23) blocked the producer with
        // EACCES. A reader therefore opens an existing lock only and retries
        // until the producer has created it; a writer that finds a foreign
        // stale lock unlinks it once and recreates it.
        const std::string lock_path = index_lock_path(queue_name_);
        sync_fd_ = open(lock_path.c_str(), writer_ ? (O_RDWR | O_CREAT) : O_RDWR, kIpcMode);
        if (sync_fd_ == -1 && writer_ && (errno == EACCES || errno == EPERM)) {
            unlink(lock_path.c_str());
            sync_fd_ = open(lock_path.c_str(), O_RDWR | O_CREAT, kIpcMode);
        }
        if (sync_fd_ == -1) {
            close_fd();
            throw std::runtime_error(
                "shaman v2 index lock open failed for " + queue_name_ + ": " +
                std::strerror(errno) + (writer_ ? "" : " (no producer has created it yet)"));
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
