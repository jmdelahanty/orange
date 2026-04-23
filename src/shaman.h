#pragma once

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <grp.h>
#include <cerrno>
#include <cstring>
#include <atomic>
#include <vector>
#include <iostream>
#include <sys/stat.h>
#include <signal.h>
#include <chrono>
#include <string>  // Added for std::string support

inline uint64_t get_steady_time_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

inline uint64_t get_epoch_time_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

// Backward-compatible alias for older callers. This is monotonic/steady time,
// not epoch time, and not any camera/acquisition timestamp.
inline uint64_t get_time_us() {
    return get_steady_time_us();
}

namespace shaman {

// Default shared memory name for backward compatibility
constexpr const char* SHM_NAME = "/shm_box_vector";
constexpr const char* SHM_IPC_GROUP = "ipc";
constexpr mode_t SHM_IPC_MODE = 0660;
constexpr size_t MAX_OBJECTS = 100;
constexpr size_t MAX_KEYPOINTS = 32;
constexpr size_t QUEUE_SIZE = 8;

struct Rect {
    float x, y, width, height;
};

struct Object {
    Rect rect;
    int label;
    float prob;
    float kps[MAX_KEYPOINTS];
    size_t num_kps;
};

// SHM ABI note: keep the existing field names/layout for compatibility.
// `timestamp_us_epoch` and `timestamp_us_monotonic` are Orange producer
// enqueue/publish-time timestamps written when SharedBoxQueue::push(...) runs.
// They are not the original camera/acquisition timestamp for the frame.
struct VectorSlot {
    size_t count;
    Object objects[MAX_OBJECTS];
    uint64_t timestamp_us_epoch;      // Semantically: orange_shm_publish_timestamp_us_epoch
    uint64_t timestamp_us_monotonic;  // Semantically: orange_shm_publish_timestamp_us_monotonic
    uint64_t frame_id;
    uint32_t camera_id;  // FIXED: Changed from uint16_t to uint32_t
    bool yolo_enabled;   // Indicates if YOLO processing was active
};

struct SharedQueue {
    std::atomic<bool> initialized;
    std::atomic<size_t> head;  // writer
    std::atomic<size_t> tail;  // reader
    VectorSlot queue[QUEUE_SIZE];
};

class SharedBoxQueue {
public:
    // NEW: Constructor with custom shared memory name (for per-camera queues)
    SharedBoxQueue(const char* shm_name, bool is_writer) 
        : writer(is_writer), shm_name_(shm_name) {
        
        // Try to create/open shared memory with the specified name
        int flags = is_writer ? (O_RDWR | O_CREAT) : O_RDWR;
        shm_fd = shm_open(shm_name_.c_str(), flags, SHM_IPC_MODE);  // Fixed: use c_str()
        if (shm_fd == -1) {
            throw std::runtime_error(std::string("shm_open failed for ") + 
                                     shm_name_ + ": " + std::strerror(errno));
        }

        if (writer) {
            if (fchmod(shm_fd, SHM_IPC_MODE) == -1) {
                std::cerr << "[shaman] fchmod failed for " << shm_name_
                          << ": " << std::strerror(errno) << std::endl;
            }
            struct group* grp = getgrnam(SHM_IPC_GROUP);
            if (!grp) {
                std::cerr << "[shaman] group '" << SHM_IPC_GROUP
                          << "' not found; shared memory stays with current group for "
                          << shm_name_ << std::endl;
            } else if (fchown(shm_fd, -1, grp->gr_gid) == -1) {
                std::cerr << "[shaman] fchown failed for " << shm_name_
                          << ": " << std::strerror(errno) << std::endl;
            }
        }

        // Check current size
        struct stat shm_stat;
        if (fstat(shm_fd, &shm_stat) == -1) {
            close(shm_fd);
            throw std::runtime_error("fstat failed");
        }

        if (shm_stat.st_size < (off_t)sizeof(SharedQueue)) {
            // First process to run - set up the shared memory
            if (ftruncate(shm_fd, sizeof(SharedQueue)) == -1) {
                close(shm_fd);
                throw std::runtime_error("ftruncate failed");
            }
            creator = true;
        }

        // Map the memory AFTER truncating
        shared = static_cast<SharedQueue*>(mmap(nullptr, sizeof(SharedQueue),
                        PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0));
        if (shared == MAP_FAILED) {
            close(shm_fd);
            throw std::runtime_error("mmap failed");
        }

        if (creator) {
            std::memset(shared, 0, sizeof(SharedQueue));
            shared->head.store(0, std::memory_order_relaxed);
            shared->tail.store(0, std::memory_order_relaxed);
            shared->initialized.store(true, std::memory_order_release);
            if (writer) {
                std::cout << "[SharedBoxQueue] Created new queue: " << shm_name_ << std::endl;
            }
        } else {
            // Wait for initialization
            size_t attempts = 0;
            while (!shared->initialized.load(std::memory_order_acquire)) {
                if (++attempts > 1000) {
                    munmap(shared, sizeof(SharedQueue));
                    close(shm_fd);
                    throw std::runtime_error("Timeout waiting for shared memory init");
                }
                usleep(1000); // wait 1ms
            }
            if (!writer) {
                std::cout << "[SharedBoxQueue] Connected to existing queue: " << shm_name_ << std::endl;
            }
        }
    }
    
    // BACKWARD COMPATIBILITY: Legacy constructor uses default SHM_NAME
    SharedBoxQueue(bool is_writer) : SharedBoxQueue(SHM_NAME, is_writer) {}
    
    ~SharedBoxQueue() {
        if (shared) munmap(shared, sizeof(SharedQueue));
        if (shm_fd >= 0) close(shm_fd);
        // Only unlink if we created it and we're configured to clean up
        // Typically you'd want to keep the queue around for other processes
        // if (writer && creator) shm_unlink(shm_name_.c_str());
    }
    
    // Writer stamps Orange publish-time SHM timestamps here. The current SHM
    // ABI does not carry the original camera/acquisition timestamp.
    bool push(const std::vector<Object>& vec, uint64_t frame_id, uint32_t camera_id, bool yolo_enabled = false) {
        if (!writer) return false;
        if (vec.size() > MAX_OBJECTS) return false;

        size_t h = shared->head.load(std::memory_order_relaxed);
        size_t t = shared->tail.load(std::memory_order_acquire);
        size_t next = (h + 1) % QUEUE_SIZE;

        if (next == t) return false; // queue full

        VectorSlot& slot = shared->queue[h];
        slot.count = vec.size();
        for (size_t i = 0; i < vec.size(); ++i) {
            slot.objects[i] = vec[i];
        }

        slot.timestamp_us_epoch = get_epoch_time_us();
        slot.timestamp_us_monotonic = get_steady_time_us();
        slot.frame_id = frame_id;
        slot.camera_id = camera_id;
        slot.yolo_enabled = yolo_enabled;

        shared->head.store(next, std::memory_order_release);
        return true;
    }
    
    // DEPRECATED: Old push method with uint16_t - provided for transition period
    // Remove this after updating all code
    [[deprecated("Use push with uint32_t camera_id instead")]]
    bool push(const std::vector<Object>& vec, uint64_t frame_id, uint16_t camera_id) {
        return push(vec, frame_id, static_cast<uint32_t>(camera_id), false);
    }
    
    // Reader pops a vector (basic version for backward compatibility)
    bool pop(std::vector<Object>& out, uint64_t& frame_id) {
        if (writer) return false;

        size_t h = shared->head.load(std::memory_order_acquire);
        size_t t = shared->tail.load(std::memory_order_relaxed);

        if (h == t) return false; // empty

        const VectorSlot& slot = shared->queue[t];
        out.resize(slot.count);
        for (size_t i = 0; i < slot.count; ++i) {
            out[i] = slot.objects[i];
        }
        frame_id = slot.frame_id;

        shared->tail.store((t + 1) % QUEUE_SIZE, std::memory_order_release);
        return true;
    }

    // Pop with Orange publish-time monotonic timestamp and uint32_t camera_id.
    bool pop(std::vector<Object>& out, uint64_t& timestamp_us_monotonic, uint64_t& frame_id, uint32_t& camera_id) {
        if (writer) return false;

        size_t h = shared->head.load(std::memory_order_acquire);
        size_t t = shared->tail.load(std::memory_order_relaxed);

        if (h == t) return false; // empty

        const VectorSlot& slot = shared->queue[t];
        out.resize(slot.count);
        for (size_t i = 0; i < slot.count; ++i) {
            out[i] = slot.objects[i];
        }

        timestamp_us_monotonic = slot.timestamp_us_monotonic;
        frame_id = slot.frame_id;
        camera_id = slot.camera_id;

        shared->tail.store((t + 1) % QUEUE_SIZE, std::memory_order_release);
        return true;
    }

    bool pop(std::vector<Object>& out, uint64_t& timestamp_us_epoch, uint64_t& timestamp_us_monotonic,
             uint64_t& frame_id, uint32_t& camera_id) {
        if (writer) return false;

        size_t h = shared->head.load(std::memory_order_acquire);
        size_t t = shared->tail.load(std::memory_order_relaxed);

        if (h == t) return false; // empty

        const VectorSlot& slot = shared->queue[t];
        out.resize(slot.count);
        for (size_t i = 0; i < slot.count; ++i) {
            out[i] = slot.objects[i];
        }

        timestamp_us_epoch = slot.timestamp_us_epoch;
        timestamp_us_monotonic = slot.timestamp_us_monotonic;
        frame_id = slot.frame_id;
        camera_id = slot.camera_id;

        shared->tail.store((t + 1) % QUEUE_SIZE, std::memory_order_release);
        return true;
    }
    
    // DEPRECATED: Old pop method with uint16_t - provided for transition period
    [[deprecated("Use pop with uint32_t camera_id instead")]]
    bool pop(std::vector<Object>& out, uint64_t& timestamp_us_monotonic, uint64_t& frame_id, uint16_t& camera_id) {
        uint32_t camera_id_32;
        bool result = pop(out, timestamp_us_monotonic, frame_id, camera_id_32);
        camera_id = static_cast<uint16_t>(camera_id_32);  // May truncate!
        return result;
    }
    
    // Full pop with Orange publish-time SHM timestamps and YOLO flag.
    bool pop(std::vector<Object>& out, uint64_t& timestamp_us_monotonic, uint64_t& frame_id, 
             uint32_t& camera_id, bool& yolo_enabled) {
        uint64_t timestamp_us_epoch_discarded = 0;
        return pop(out, timestamp_us_epoch_discarded, timestamp_us_monotonic, frame_id, camera_id, yolo_enabled);
    }

    bool pop(std::vector<Object>& out, uint64_t& timestamp_us_epoch, uint64_t& timestamp_us_monotonic,
             uint64_t& frame_id, uint32_t& camera_id, bool& yolo_enabled) {
        if (writer) return false;

        size_t h = shared->head.load(std::memory_order_acquire);
        size_t t = shared->tail.load(std::memory_order_relaxed);

        if (h == t) return false; // empty

        const VectorSlot& slot = shared->queue[t];
        out.resize(slot.count);
        for (size_t i = 0; i < slot.count; ++i) {
            out[i] = slot.objects[i];
        }

        timestamp_us_epoch = slot.timestamp_us_epoch;
        timestamp_us_monotonic = slot.timestamp_us_monotonic;
        frame_id = slot.frame_id;
        camera_id = slot.camera_id;
        yolo_enabled = slot.yolo_enabled;

        shared->tail.store((t + 1) % QUEUE_SIZE, std::memory_order_release);
        return true;
    }
    
    // NEW: Utility method to check if queue is empty (for readers)
    bool isEmpty() const {
        if (writer) return false;
        size_t h = shared->head.load(std::memory_order_acquire);
        size_t t = shared->tail.load(std::memory_order_relaxed);
        return h == t;
    }
    
    // NEW: Utility method to check if queue is full (for writers)
    bool isFull() const {
        if (!writer) return false;
        size_t h = shared->head.load(std::memory_order_relaxed);
        size_t t = shared->tail.load(std::memory_order_acquire);
        size_t next = (h + 1) % QUEUE_SIZE;
        return next == t;
    }
    
    // NEW: Get the queue name
    const std::string& getQueueName() const { return shm_name_; }

private:
    int shm_fd = -1;
    bool writer;
    SharedQueue* shared = nullptr;
    bool creator = false;
    std::string shm_name_;  // NEW: Store the queue name
};

// NEW: Helper function to create per-camera queue name
inline std::string getCameraQueueName(uint32_t camera_id) {
    return "/shm_cam_" + std::to_string(camera_id);
}

// NEW: Helper function to check if a shared memory queue exists
inline bool queueExists(const char* shm_name) {
    int fd = shm_open(shm_name, O_RDONLY, 0666);
    if (fd != -1) {
        close(fd);
        return true;
    }
    return false;
}

// NEW: Helper function to unlink (delete) a shared memory queue
inline void unlinkQueue(const char* shm_name) {
    shm_unlink(shm_name);
    std::cout << "[shaman] Unlinked queue: " << shm_name << std::endl;
}

} // namespace shaman
