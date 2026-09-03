#include "spatial_roi_recorder_cuda_detach.h"

#include <cuda.h>
#include <cuda_runtime.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace orange::spatial_roi::ipc {
namespace {

static_assert(sizeof(cudaIpcMemHandle_t) == kSpatialRoiCudaIpcHandleBytes,
              "CUDA IPC memory handle ABI changed; update the ROI detach contract");
static_assert(sizeof(cudaIpcEventHandle_t) == kSpatialRoiCudaIpcHandleBytes,
              "CUDA IPC event handle ABI changed; update the ROI detach contract");

constexpr std::size_t kMaxDetachSlots = kSpatialRoiIpcMaxQueueFrames;
constexpr std::size_t kMaxDetachBytes =
    static_cast<std::size_t>(kSpatialRoiIpcMaxPackedMono8Bytes);
constexpr std::size_t kMaxErrorBytes = kSpatialRoiIpcMaxTextBytes;

void set_error_noexcept(std::string* error_out, const std::string& message) noexcept
{
    if (!error_out) {
        return;
    }
    try {
        *error_out = message.substr(0, std::min(message.size(), kMaxErrorBytes));
    } catch (...) {
        error_out->clear();
    }
}

void set_error_noexcept(std::string* error_out, const char* message) noexcept
{
    if (!error_out) {
        return;
    }
    try {
        *error_out = message ? message : "";
    } catch (...) {
        error_out->clear();
    }
}

std::string cuda_failure(const char* operation, const cudaError_t status)
{
    std::ostringstream stream;
    stream << (operation ? operation : "CUDA operation")
           << " failed: " << cudaGetErrorString(status);
    return stream.str();
}

std::string cuda_driver_failure(const char* operation, const CUresult status)
{
    const char* description = nullptr;
    (void)cuGetErrorString(status, &description);
    std::ostringstream stream;
    stream << (operation ? operation : "CUDA driver operation")
           << " failed: " << (description ? description : "unknown CUDA error");
    return stream.str();
}

bool same_stream_identity(const SpatialRoiIpcStreamIdentity& lhs,
                          const SpatialRoiIpcStreamIdentity& rhs) noexcept
{
    return lhs.recording_id == rhs.recording_id &&
           lhs.recording_identity_token == rhs.recording_identity_token &&
           lhs.producer_generation == rhs.producer_generation &&
           lhs.camera_id == rhs.camera_id &&
           lhs.camera_serial == rhs.camera_serial &&
           lhs.roi_id == rhs.roi_id && lhs.region_id == rhs.region_id &&
           lhs.arena_group_id == rhs.arena_group_id &&
           lhs.arena_id == rhs.arena_id &&
           lhs.logical_stream_id == rhs.logical_stream_id &&
           lhs.spatial_roi_plan_sha256 == rhs.spatial_roi_plan_sha256;
}

bool same_raster(const SpatialRoiFrameRaster& lhs,
                 const SpatialRoiFrameRaster& rhs) noexcept
{
    return lhs.width == rhs.width && lhs.height == rhs.height;
}

bool same_rect(const SpatialRoiFrameRect& lhs,
               const SpatialRoiFrameRect& rhs) noexcept
{
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.width == rhs.width &&
           lhs.height == rhs.height;
}

bool same_padding(const SpatialRoiFramePadding& lhs,
                  const SpatialRoiFramePadding& rhs) noexcept
{
    return lhs.left == rhs.left && lhs.top == rhs.top &&
           lhs.right == rhs.right && lhs.bottom == rhs.bottom &&
           lhs.value_mono8 == rhs.value_mono8;
}

bool same_geometry(const SpatialRoiFrameDescriptor& descriptor,
                   const SpatialRoiRecorderCudaDetachGeometry& expected) noexcept
{
    return same_raster(descriptor.native_raster, expected.native_raster) &&
           same_rect(descriptor.content_rect, expected.content_rect) &&
           same_raster(descriptor.encoded_raster, expected.encoded_raster) &&
           same_rect(descriptor.encoded_content_rect,
                     expected.encoded_content_rect) &&
           same_padding(descriptor.padding, expected.padding) &&
           descriptor.routing_policy == expected.routing_policy;
}

bool decode_hex_handle(const std::string& encoded, void* output) noexcept
{
    if (!output || encoded.size() != kSpatialRoiCudaIpcHandleHexBytes) {
        return false;
    }
    auto* bytes = static_cast<unsigned char*>(output);
    for (std::size_t index = 0; index < kSpatialRoiCudaIpcHandleBytes; ++index) {
        const unsigned char high = static_cast<unsigned char>(encoded[index * 2]);
        const unsigned char low = static_cast<unsigned char>(encoded[index * 2 + 1]);
        auto decode_nibble = [](const unsigned char value) -> int {
            if (value >= '0' && value <= '9') {
                return static_cast<int>(value - '0');
            }
            if (value >= 'a' && value <= 'f') {
                return static_cast<int>(value - 'a') + 10;
            }
            return -1;
        };
        const int high_value = decode_nibble(high);
        const int low_value = decode_nibble(low);
        if (high_value < 0 || low_value < 0) {
            return false;
        }
        bytes[index] = static_cast<unsigned char>((high_value << 4) | low_value);
    }
    return true;
}

bool checked_raster_bytes(const std::uint32_t width,
                          const std::uint32_t height,
                          std::size_t* mono_bytes_out,
                          std::size_t* nv12_bytes_out,
                          std::string* error_out)
{
    if (!mono_bytes_out || !nv12_bytes_out || width == 0 || height == 0 ||
        (width & 1u) != 0 || (height & 1u) != 0) {
        set_error_noexcept(
            error_out,
            "recorder detach NV12 raster dimensions must be positive and even");
        return false;
    }
    const std::uint64_t mono_bytes_u64 =
        static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    if (mono_bytes_u64 == 0 || mono_bytes_u64 > kMaxDetachBytes ||
        mono_bytes_u64 > std::numeric_limits<std::size_t>::max()) {
        set_error_noexcept(error_out, "recorder detach raster exceeds bounded capacity");
        return false;
    }
    const std::uint64_t nv12_bytes_u64 = mono_bytes_u64 + mono_bytes_u64 / 2ULL;
    if (nv12_bytes_u64 > std::numeric_limits<std::size_t>::max()) {
        set_error_noexcept(error_out, "recorder detach NV12 size overflows size_t");
        return false;
    }
    *mono_bytes_out = static_cast<std::size_t>(mono_bytes_u64);
    *nv12_bytes_out = static_cast<std::size_t>(nv12_bytes_u64);
    if (error_out) {
        error_out->clear();
    }
    return true;
}

SpatialRoiRecorderCudaDetachResult make_result(
    const SpatialRoiRecorderDetachStatus status,
    const std::string& error,
    const bool source_safe = true) noexcept
{
    SpatialRoiRecorderCudaDetachResult result;
    result.status = status;
    result.source_safe = source_safe;
    set_error_noexcept(&result.error, error);
    return result;
}

SpatialRoiRecorderCudaDetachResult make_result(
    const SpatialRoiRecorderDetachStatus status,
    const char* error,
    const bool source_safe = true) noexcept
{
    SpatialRoiRecorderCudaDetachResult result;
    result.status = status;
    result.source_safe = source_safe;
    set_error_noexcept(&result.error, error);
    return result;
}

enum class BoundedCudaQueryStatus {
    kReady,
    kTimedOut,
    kCudaError,
};

template <typename Query>
BoundedCudaQueryStatus wait_for_cuda_query(
    Query&& query,
    const std::chrono::steady_clock::time_point deadline,
    cudaError_t* error_out) noexcept
{
    if (error_out) {
        *error_out = cudaSuccess;
    }
    for (;;) {
        const cudaError_t status = query();
        if (status == cudaSuccess) {
            return BoundedCudaQueryStatus::kReady;
        }
        if (status != cudaErrorNotReady) {
            if (error_out) {
                *error_out = status;
            }
            return BoundedCudaQueryStatus::kCudaError;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return BoundedCudaQueryStatus::kTimedOut;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

class AtomicFlagGuard final {
public:
    explicit AtomicFlagGuard(std::atomic_flag* flag) noexcept : flag_(flag) {}
    ~AtomicFlagGuard()
    {
        if (flag_) {
            flag_->clear(std::memory_order_release);
        }
    }
    AtomicFlagGuard(const AtomicFlagGuard&) = delete;
    AtomicFlagGuard& operator=(const AtomicFlagGuard&) = delete;

private:
    std::atomic_flag* flag_ = nullptr;
};

}  // namespace

namespace detail {

class SpatialRoiRecorderCudaDetachState final {
public:
    struct ClaimedSlot {
        std::size_t index = 0;
        std::uint64_t generation = 0;
        unsigned char* mono8 = nullptr;
        unsigned char* nv12 = nullptr;
    };

    enum class ClaimStatus {
        kClaimed,
        kPoolExhausted,
        kQuarantined,
        kStopped,
        kGenerationExhausted,
    };

    enum class SlotState {
        kFree,
        kInUse,
        kQuarantined,
    };

    enum class ImportCacheLookupStatus {
        kHit,
        kMiss,
        kFull,
        kHandleCollision,
        kStopped,
        kQuarantined,
    };

    struct ImportCacheLookup {
        ImportCacheLookupStatus status = ImportCacheLookupStatus::kStopped;
        void* memory = nullptr;
        cudaEvent_t event = nullptr;
    };

    SpatialRoiRecorderCudaDetachState(
        SpatialRoiRecorderCudaDetachConfig config)
        : config_(std::move(config))
    {
    }

    ~SpatialRoiRecorderCudaDetachState()
    {
        // A quarantined state intentionally leaks its imported CUDA handles,
        // output allocations, and stream.  No destructor can prove that
        // failed or unknown CUDA work is no longer using them.
        if (quarantined_ || !CloseCachedImports(nullptr)) {
            return;
        }
        if (config_.recorder_gpu_id < 0 ||
            cudaSetDevice(config_.recorder_gpu_id) != cudaSuccess) {
            return;
        }
        if (stream_ && cudaStreamSynchronize(stream_) != cudaSuccess) {
            return;
        }
        for (Slot& slot : slots_) {
            if (slot.mono8) {
                (void)cudaFree(slot.mono8);
                slot.mono8 = nullptr;
            }
            if (slot.nv12) {
                (void)cudaFree(slot.nv12);
                slot.nv12 = nullptr;
            }
        }
        if (stream_) {
            (void)cudaStreamDestroy(stream_);
            stream_ = nullptr;
        }
    }

    bool Initialize(std::string* error_out) noexcept
    {
        try {
            int device_count = 0;
            const cudaError_t count_status = cudaGetDeviceCount(&device_count);
            if (count_status != cudaSuccess ||
                config_.recorder_gpu_id >= device_count ||
                config_.expected_source_gpu_id >= device_count) {
                set_error_noexcept(
                    error_out,
                    count_status == cudaSuccess
                        ? "recorder/source GPU id is outside the CUDA device range"
                        : cuda_failure("cudaGetDeviceCount(recorder detach)",
                                       count_status));
                return false;
            }
            const std::array<int, 2> ipc_devices = {
                config_.expected_source_gpu_id,
                config_.recorder_gpu_id};
            for (const int device : ipc_devices) {
                int unified_addressing = 0;
                const cudaError_t unified_status = cudaDeviceGetAttribute(
                    &unified_addressing,
                    cudaDevAttrUnifiedAddressing,
                    device);
                if (unified_status != cudaSuccess || unified_addressing == 0) {
                    set_error_noexcept(
                        error_out,
                        unified_status == cudaSuccess
                            ? "configured CUDA IPC device lacks unified addressing"
                            : cuda_failure(
                                  "cudaDeviceGetAttribute(unified addressing)",
                                  unified_status));
                    return false;
                }
                int ipc_event_support = 0;
                const cudaError_t event_support_status = cudaDeviceGetAttribute(
                    &ipc_event_support,
                    cudaDevAttrIpcEventSupport,
                    device);
                if (event_support_status != cudaSuccess ||
                    ipc_event_support == 0) {
                    set_error_noexcept(
                        error_out,
                        event_support_status == cudaSuccess
                            ? "configured CUDA device lacks IPC event support"
                            : cuda_failure(
                                  "cudaDeviceGetAttribute(IPC event support)",
                                  event_support_status));
                    return false;
                }
            }
            if (config_.recorder_gpu_id != config_.expected_source_gpu_id) {
                int can_access_peer = 0;
                const cudaError_t peer_status = cudaDeviceCanAccessPeer(
                    &can_access_peer,
                    config_.recorder_gpu_id,
                    config_.expected_source_gpu_id);
                if (peer_status != cudaSuccess || can_access_peer == 0) {
                    set_error_noexcept(
                        error_out,
                        peer_status == cudaSuccess
                            ? "recorder GPU cannot access the configured source GPU"
                            : cuda_failure(
                                  "cudaDeviceCanAccessPeer(recorder detach)",
                                  peer_status));
                    return false;
                }
            }
            const cudaError_t set_status = cudaSetDevice(config_.recorder_gpu_id);
            if (set_status != cudaSuccess) {
                set_error_noexcept(error_out,
                                   cuda_failure("cudaSetDevice(recorder detach)",
                                                set_status));
                return false;
            }
            int current_device = -1;
            const cudaError_t get_status = cudaGetDevice(&current_device);
            if (get_status != cudaSuccess ||
                current_device != config_.recorder_gpu_id) {
                set_error_noexcept(
                    error_out,
                    get_status == cudaSuccess
                        ? "cudaGetDevice did not select configured recorder GPU"
                        : cuda_failure("cudaGetDevice(recorder detach)", get_status));
                return false;
            }
            const cudaError_t stream_status =
                cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking);
            if (stream_status != cudaSuccess) {
                set_error_noexcept(
                    error_out,
                    cuda_failure("cudaStreamCreateWithFlags(recorder detach)",
                                 stream_status));
                return false;
            }
            slots_.resize(config_.slot_count);
            // One authenticated producer pool slot yields at most one stable
            // memory/event pair for this logical stream. Reserve the complete
            // bound before admission so cache misses never grow storage on
            // the FRAME path.
            import_cache_.reserve(config_.slot_count);
            for (Slot& slot : slots_) {
                cudaError_t status = cudaMalloc(
                    reinterpret_cast<void**>(&slot.mono8), mono8_bytes_);
                if (status != cudaSuccess) {
                    set_error_noexcept(
                        error_out,
                        cuda_failure("cudaMalloc(recorder Mono8 detach slot)",
                                     status));
                    return false;
                }
                status = cudaMalloc(
                    reinterpret_cast<void**>(&slot.nv12), nv12_bytes_);
                if (status != cudaSuccess) {
                    set_error_noexcept(
                        error_out,
                        cuda_failure("cudaMalloc(recorder NV12 detach slot)",
                                     status));
                    return false;
                }
            }
            accepting_ = true;
            if (error_out) {
                error_out->clear();
            }
            return true;
        } catch (const std::exception& exception) {
            set_error_noexcept(error_out, exception.what());
            return false;
        } catch (...) {
            set_error_noexcept(error_out, "recorder detach pool initialization threw");
            return false;
        }
    }

    ImportCacheLookup LookupCachedImport(
        const cudaIpcMemHandle_t& memory_handle,
        const cudaIpcEventHandle_t& event_handle) noexcept
    {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (quarantined_) {
                return {ImportCacheLookupStatus::kQuarantined, nullptr, nullptr};
            }
            if (!accepting_ || imports_closed_) {
                return {ImportCacheLookupStatus::kStopped, nullptr, nullptr};
            }
            for (const CachedImport& entry : import_cache_) {
                const bool same_memory =
                    std::memcmp(&entry.memory_handle,
                                &memory_handle,
                                sizeof(memory_handle)) == 0;
                const bool same_event =
                    std::memcmp(&entry.event_handle,
                                &event_handle,
                                sizeof(event_handle)) == 0;
                if (same_memory && same_event) {
                    saturating_increment(&counters_.memory_cache_hits);
                    saturating_increment(&counters_.event_cache_hits);
                    return {ImportCacheLookupStatus::kHit,
                            entry.memory,
                            entry.event};
                }
                // A producer pool slot has a stable one-to-one memory/event
                // binding for this generation. Reusing only one half would
                // make restart/slot identity ambiguous and can attempt a
                // second open of an already imported memory handle.
                if (same_memory || same_event) {
                    return {ImportCacheLookupStatus::kHandleCollision,
                            nullptr,
                            nullptr};
                }
            }
            saturating_increment(&counters_.memory_cache_misses);
            saturating_increment(&counters_.event_cache_misses);
            if (import_cache_.size() >= config_.slot_count) {
                saturating_increment(&counters_.import_cache_full);
                return {ImportCacheLookupStatus::kFull, nullptr, nullptr};
            }
            return {ImportCacheLookupStatus::kMiss, nullptr, nullptr};
        } catch (...) {
            return {ImportCacheLookupStatus::kStopped, nullptr, nullptr};
        }
    }

    bool CommitCachedImport(const cudaIpcMemHandle_t& memory_handle,
                            const cudaIpcEventHandle_t& event_handle,
                            void* memory,
                            cudaEvent_t event) noexcept
    {
        if (!memory || !event) {
            return false;
        }
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!accepting_ || imports_closed_ || quarantined_ ||
                import_cache_.size() >= config_.slot_count) {
                return false;
            }
            CachedImport entry;
            entry.memory_handle = memory_handle;
            entry.event_handle = event_handle;
            entry.memory = memory;
            entry.event = event;
            import_cache_.push_back(entry);
            counters_.import_cache_entries = import_cache_.size();
            counters_.peak_import_cache_entries = std::max<std::uint64_t>(
                counters_.peak_import_cache_entries,
                static_cast<std::uint64_t>(import_cache_.size()));
            return true;
        } catch (...) {
            return false;
        }
    }

    bool CloseCachedImports(std::string* error_out) noexcept
    {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (imports_closed_) {
                if (error_out) {
                    error_out->clear();
                }
                return true;
            }
            if (accepting_) {
                set_error_noexcept(
                    error_out,
                    "recorder import cache cannot close before admission stops");
                return false;
            }
            if (quarantined_) {
                set_error_noexcept(
                    error_out,
                    "recorder import cache is source-quarantined");
                return false;
            }
            const cudaError_t set_status =
                cudaSetDevice(config_.recorder_gpu_id);
            if (set_status != cudaSuccess) {
                saturating_increment(&counters_.cache_cleanup_failures);
                saturating_increment(&counters_.cleanup_failures);
                quarantined_ = true;
                set_error_noexcept(
                    error_out,
                    cuda_failure("cudaSetDevice(import cache cleanup)",
                                 set_status));
                return false;
            }
            const cudaError_t stream_status =
                stream_ ? cudaStreamQuery(stream_) : cudaSuccess;
            if (stream_status != cudaSuccess) {
                saturating_increment(&counters_.cache_cleanup_failures);
                saturating_increment(&counters_.cleanup_failures);
                quarantined_ = true;
                set_error_noexcept(
                    error_out,
                    stream_status == cudaErrorNotReady
                        ? "recorder detach stream remained active during import cache cleanup"
                        : cuda_failure("cudaStreamQuery(import cache cleanup)",
                                       stream_status));
                return false;
            }

            // Imported events are retired before their corresponding memory
            // mappings. No FRAME operation can race this loop: the public
            // pool owns a whole-operation gate and requires Stop first.
            for (CachedImport& entry : import_cache_) {
                if (entry.event) {
                    const cudaError_t status = cudaEventDestroy(entry.event);
                    if (status != cudaSuccess) {
                        saturating_increment(&counters_.cache_cleanup_failures);
                        saturating_increment(&counters_.cleanup_failures);
                        quarantined_ = true;
                        set_error_noexcept(
                            error_out,
                            cuda_failure("cudaEventDestroy(import cache cleanup)",
                                         status));
                        return false;
                    }
                    entry.event = nullptr;
                    saturating_increment(&counters_.cached_event_closes);
                }
            }
            for (CachedImport& entry : import_cache_) {
                if (entry.memory) {
                    const cudaError_t status =
                        cudaIpcCloseMemHandle(entry.memory);
                    if (status != cudaSuccess) {
                        saturating_increment(&counters_.cache_cleanup_failures);
                        saturating_increment(&counters_.cleanup_failures);
                        quarantined_ = true;
                        set_error_noexcept(
                            error_out,
                            cuda_failure("cudaIpcCloseMemHandle(import cache cleanup)",
                                         status));
                        return false;
                    }
                    entry.memory = nullptr;
                    saturating_increment(&counters_.cached_memory_closes);
                }
            }
            import_cache_.clear();
            counters_.import_cache_entries = 0;
            imports_closed_ = true;
            if (error_out) {
                error_out->clear();
            }
            return true;
        } catch (const std::exception& exception) {
            quarantined_ = true;
            saturating_increment(&counters_.cache_cleanup_failures);
            saturating_increment(&counters_.cleanup_failures);
            set_error_noexcept(error_out, exception.what());
            return false;
        } catch (...) {
            quarantined_ = true;
            saturating_increment(&counters_.cache_cleanup_failures);
            saturating_increment(&counters_.cleanup_failures);
            set_error_noexcept(error_out,
                               "recorder import cache cleanup threw");
            return false;
        }
    }

    ClaimStatus Claim(ClaimedSlot* claimed_out) noexcept
    {
        if (!claimed_out) {
            return ClaimStatus::kStopped;
        }
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (quarantined_) {
                return ClaimStatus::kQuarantined;
            }
            if (!accepting_) {
                saturating_increment(&counters_.stopped);
                return ClaimStatus::kStopped;
            }
            bool saw_generation_exhaustion = false;
            for (std::size_t index = 0; index < slots_.size(); ++index) {
                Slot& slot = slots_[index];
                if (slot.state != SlotState::kFree) {
                    continue;
                }
                if (slot.generation == std::numeric_limits<std::uint64_t>::max()) {
                    saw_generation_exhaustion = true;
                    continue;
                }
                ++slot.generation;
                slot.state = SlotState::kInUse;
                claimed_out->index = index;
                claimed_out->generation = slot.generation;
                claimed_out->mono8 = slot.mono8;
                claimed_out->nv12 = slot.nv12;
                return ClaimStatus::kClaimed;
            }
            if (saw_generation_exhaustion) {
                saturating_increment(&counters_.generation_exhausted);
                return ClaimStatus::kGenerationExhausted;
            }
            saturating_increment(&counters_.pool_exhausted);
            return ClaimStatus::kPoolExhausted;
        } catch (...) {
            return ClaimStatus::kStopped;
        }
    }

    void Release(const std::size_t index,
                 const std::uint64_t generation) noexcept
    {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (index >= slots_.size()) {
                return;
            }
            Slot& slot = slots_[index];
            if (slot.state == SlotState::kInUse &&
                slot.generation == generation) {
                slot.state = SlotState::kFree;
                saturating_increment(&counters_.slot_releases);
            }
        } catch (...) {
            // Releasing a frame is a destructor path.  If the diagnostic
            // mutex itself fails, retaining the slot is safer than throwing
            // or freeing memory that a stale view may still reference.
        }
    }

    void Quarantine(const ClaimedSlot& claimed,
                    void* imported_memory,
                    cudaEvent_t imported_event) noexcept
    {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (claimed.index < slots_.size()) {
                Slot& slot = slots_[claimed.index];
                if (slot.state == SlotState::kInUse &&
                    slot.generation == claimed.generation) {
                    slot.imported_memory = imported_memory;
                    slot.imported_event = imported_event;
                    slot.state = SlotState::kQuarantined;
                }
            }
            accepting_ = false;
            quarantined_ = true;
            saturating_increment(&counters_.source_quarantines);
        } catch (...) {
            // The caller still treats this endpoint as source-unsafe.  Keep
            // the state stopped and quarantined even if diagnostics failed.
            accepting_ = false;
            quarantined_ = true;
        }
    }

    void Stop() noexcept
    {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            accepting_ = false;
        } catch (...) {
            accepting_ = false;
        }
    }

    bool quarantined() const noexcept
    {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            return quarantined_;
        } catch (...) {
            // Quarantine state is an ownership-safety question. A diagnostic
            // lock failure must never turn an unknown state into RELEASE-safe.
            return true;
        }
    }

    std::size_t slot_capacity() const noexcept
    {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            return slots_.size();
        } catch (...) {
            return 0;
        }
    }

    std::size_t available_slot_count() const noexcept
    {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            std::size_t available = 0;
            for (const Slot& slot : slots_) {
                if (slot.state == SlotState::kFree) {
                    ++available;
                }
            }
            return available;
        } catch (...) {
            return 0;
        }
    }

    SpatialRoiRecorderCudaDetachCounters counters() const noexcept
    {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            return counters_;
        } catch (...) {
            return {};
        }
    }

    void Increment(std::uint64_t SpatialRoiRecorderCudaDetachCounters::*field) noexcept
    {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            saturating_increment(&(counters_.*field));
        } catch (...) {
        }
    }

    void AddBytes(const std::size_t bytes) noexcept
    {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            const std::uint64_t current = counters_.mono8_bytes_copied;
            const std::uint64_t addition = static_cast<std::uint64_t>(bytes);
            if (std::numeric_limits<std::uint64_t>::max() - current < addition) {
                counters_.mono8_bytes_copied = std::numeric_limits<std::uint64_t>::max();
            } else {
                counters_.mono8_bytes_copied = current + addition;
            }
        } catch (...) {
        }
    }

    cudaStream_t stream() const noexcept { return stream_; }
    std::size_t mono8_bytes() const noexcept { return mono8_bytes_; }
    std::size_t nv12_bytes() const noexcept { return nv12_bytes_; }

    void SetRasterBytes(const std::size_t mono_bytes,
                        const std::size_t nv12_bytes) noexcept
    {
        mono8_bytes_ = mono_bytes;
        nv12_bytes_ = nv12_bytes;
    }

    class ClaimGuard final {
    public:
        ClaimGuard(SpatialRoiRecorderCudaDetachState* state,
                   const ClaimedSlot claimed) noexcept
            : state_(state), claimed_(claimed)
        {
        }

        ~ClaimGuard() noexcept
        {
            if (!active_ || !state_) {
                return;
            }
            if (imported_memory_ || imported_event_) {
                state_->Quarantine(
                    claimed_, imported_memory_, imported_event_);
            } else {
                state_->Release(claimed_.index, claimed_.generation);
            }
        }

        void SetImported(void* imported_memory,
                         cudaEvent_t imported_event) noexcept
        {
            imported_memory_ = imported_memory;
            imported_event_ = imported_event;
        }

        void ClearImported() noexcept
        {
            imported_memory_ = nullptr;
            imported_event_ = nullptr;
        }

        void Quarantine() noexcept
        {
            if (active_ && state_) {
                state_->Quarantine(claimed_, imported_memory_, imported_event_);
            }
            active_ = false;
        }

        void Commit() noexcept { active_ = false; }

    private:
        SpatialRoiRecorderCudaDetachState* state_ = nullptr;
        ClaimedSlot claimed_;
        void* imported_memory_ = nullptr;
        cudaEvent_t imported_event_ = nullptr;
        bool active_ = true;
    };

private:
    struct CachedImport {
        cudaIpcMemHandle_t memory_handle{};
        cudaIpcEventHandle_t event_handle{};
        void* memory = nullptr;
        cudaEvent_t event = nullptr;
    };

    struct Slot {
        unsigned char* mono8 = nullptr;
        unsigned char* nv12 = nullptr;
        void* imported_memory = nullptr;
        cudaEvent_t imported_event = nullptr;
        std::uint64_t generation = 0;
        SlotState state = SlotState::kFree;
    };

    static void saturating_increment(std::uint64_t* value) noexcept
    {
        if (value && *value != std::numeric_limits<std::uint64_t>::max()) {
            ++*value;
        }
    }

    SpatialRoiRecorderCudaDetachConfig config_;
    std::size_t mono8_bytes_ = 0;
    std::size_t nv12_bytes_ = 0;
    cudaStream_t stream_ = nullptr;
    std::vector<Slot> slots_;
    std::vector<CachedImport> import_cache_;
    mutable std::mutex mutex_;
    bool accepting_ = false;
    bool quarantined_ = false;
    bool imports_closed_ = false;
    SpatialRoiRecorderCudaDetachCounters counters_;
};

}  // namespace detail

const char* spatial_roi_recorder_detach_status_name(
    const SpatialRoiRecorderDetachStatus status) noexcept
{
    switch (status) {
    case SpatialRoiRecorderDetachStatus::kDetached:
        return "detached";
    case SpatialRoiRecorderDetachStatus::kInvalidArgument:
        return "invalid_argument";
    case SpatialRoiRecorderDetachStatus::kWrongDevice:
        return "wrong_device";
    case SpatialRoiRecorderDetachStatus::kBusy:
        return "busy";
    case SpatialRoiRecorderDetachStatus::kPoolExhausted:
        return "pool_exhausted";
    case SpatialRoiRecorderDetachStatus::kCudaError:
        return "cuda_error";
    case SpatialRoiRecorderDetachStatus::kSourceQuarantined:
        return "source_quarantined";
    case SpatialRoiRecorderDetachStatus::kStopped:
        return "stopped";
    }
    return "unknown";
}

SpatialRoiRecorderDetachedFrame::SpatialRoiRecorderDetachedFrame(
    std::shared_ptr<detail::SpatialRoiRecorderCudaDetachState> state,
    const std::size_t slot_index,
    const std::uint64_t slot_generation,
    SpatialRoiIpcCorrelation correlation,
    unsigned char* device_mono8,
    unsigned char* device_nv12,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::size_t mono8_bytes,
    const std::size_t nv12_bytes) noexcept
    : state_(std::move(state)),
      slot_index_(slot_index),
      slot_generation_(slot_generation),
      correlation_(std::move(correlation)),
      device_mono8_(device_mono8),
      device_nv12_(device_nv12),
      width_(width),
      height_(height),
      mono8_bytes_(mono8_bytes),
      nv12_bytes_(nv12_bytes)
{
}

SpatialRoiRecorderDetachedFrame::~SpatialRoiRecorderDetachedFrame()
{
    Release();
}

SpatialRoiRecorderDetachedFrame::SpatialRoiRecorderDetachedFrame(
    SpatialRoiRecorderDetachedFrame&& other) noexcept
    : state_(std::move(other.state_)),
      slot_index_(other.slot_index_),
      slot_generation_(other.slot_generation_),
      correlation_(std::move(other.correlation_)),
      device_mono8_(other.device_mono8_),
      device_nv12_(other.device_nv12_),
      width_(other.width_),
      height_(other.height_),
      mono8_bytes_(other.mono8_bytes_),
      nv12_bytes_(other.nv12_bytes_)
{
    other.slot_index_ = 0;
    other.slot_generation_ = 0;
    other.device_mono8_ = nullptr;
    other.device_nv12_ = nullptr;
    other.width_ = 0;
    other.height_ = 0;
    other.mono8_bytes_ = 0;
    other.nv12_bytes_ = 0;
}

SpatialRoiRecorderDetachedFrame& SpatialRoiRecorderDetachedFrame::operator=(
    SpatialRoiRecorderDetachedFrame&& other) noexcept
{
    if (this == &other) {
        return *this;
    }
    Release();
    state_ = std::move(other.state_);
    slot_index_ = other.slot_index_;
    slot_generation_ = other.slot_generation_;
    correlation_ = std::move(other.correlation_);
    device_mono8_ = other.device_mono8_;
    device_nv12_ = other.device_nv12_;
    width_ = other.width_;
    height_ = other.height_;
    mono8_bytes_ = other.mono8_bytes_;
    nv12_bytes_ = other.nv12_bytes_;
    other.slot_index_ = 0;
    other.slot_generation_ = 0;
    other.device_mono8_ = nullptr;
    other.device_nv12_ = nullptr;
    other.width_ = 0;
    other.height_ = 0;
    other.mono8_bytes_ = 0;
    other.nv12_bytes_ = 0;
    return *this;
}

void SpatialRoiRecorderDetachedFrame::Release() noexcept
{
    if (state_) {
        state_->Release(slot_index_, slot_generation_);
        state_.reset();
    }
    device_mono8_ = nullptr;
    device_nv12_ = nullptr;
    width_ = 0;
    height_ = 0;
    mono8_bytes_ = 0;
    nv12_bytes_ = 0;
}

SpatialRoiRecorderCudaDetachPool::SpatialRoiRecorderCudaDetachPool(
    SpatialRoiRecorderCudaDetachConfig config) noexcept
    : config_(std::move(config))
{
    try {
        std::string identity_error;
        if (!validate_spatial_roi_ipc_stream_identity(
                config_.expected_stream, &identity_error)) {
            error_ = identity_error.empty()
                         ? "expected recorder detach stream identity is invalid"
                         : identity_error;
            return;
        }
        if (config_.recorder_gpu_id < 0) {
            error_ = "recorder_gpu_id must be non-negative";
            return;
        }
        if (config_.expected_source_gpu_id < 0) {
            error_ = "expected_source_gpu_id must be non-negative";
            return;
        }
        if (config_.expected_assigned_shard_id < 0) {
            error_ = "expected_assigned_shard_id must be non-negative";
            return;
        }
        if (config_.slot_count == 0 || config_.slot_count > kMaxDetachSlots) {
            error_ = "slot_count is outside the bounded recorder detach range";
            return;
        }
        if (config_.max_pool_bytes == 0 ||
            config_.max_pool_bytes >
                kSpatialRoiRecorderCudaDetachMaxPoolBytes) {
            error_ = "max_pool_bytes is outside the bounded recorder detach range";
            return;
        }
        if (config_.operation_timeout_ms == 0 ||
            config_.operation_timeout_ms >
                kSpatialRoiRecorderCudaDetachMaxTimeoutMs) {
            error_ =
                "operation_timeout_ms is outside the bounded recorder detach range";
            return;
        }
        std::string size_error;
        std::size_t mono_bytes = 0;
        std::size_t nv12_bytes = 0;
        if (!checked_raster_bytes(config_.expected_geometry.encoded_raster.width,
                                  config_.expected_geometry.encoded_raster.height,
                                  &mono_bytes,
                                  &nv12_bytes,
                                  &size_error)) {
            error_ = size_error;
            return;
        }
        const std::uint64_t bytes_per_slot =
            static_cast<std::uint64_t>(mono_bytes) +
            static_cast<std::uint64_t>(nv12_bytes);
        if (bytes_per_slot == 0 ||
            config_.slot_count >
                std::numeric_limits<std::uint64_t>::max() / bytes_per_slot ||
            static_cast<std::uint64_t>(config_.slot_count) * bytes_per_slot >
                config_.max_pool_bytes) {
            error_ =
                "recorder-owned Mono8+NV12 slots exceed max_pool_bytes";
            return;
        }

        SpatialRoiFrameDescriptor expected_descriptor;
        expected_descriptor.recording_id = config_.expected_stream.recording_id;
        expected_descriptor.recording_identity_token =
            config_.expected_stream.recording_identity_token;
        expected_descriptor.producer_generation =
            config_.expected_stream.producer_generation;
        expected_descriptor.camera_id = config_.expected_stream.camera_id;
        expected_descriptor.camera_serial = config_.expected_stream.camera_serial;
        expected_descriptor.local_frame_id = 1;
        expected_descriptor.camera_frame_id = 1;
        expected_descriptor.recording_frame_id = 1;
        expected_descriptor.roi_stream_frame_index = 1;
        expected_descriptor.camera_timestamp_ns = 1;
        expected_descriptor.timestamp_sys_ns = 1;
        expected_descriptor.roi_id = config_.expected_stream.roi_id;
        expected_descriptor.region_id = config_.expected_stream.region_id;
        expected_descriptor.arena_group_id =
            config_.expected_stream.arena_group_id;
        expected_descriptor.arena_id = config_.expected_stream.arena_id;
        expected_descriptor.logical_stream_id =
            config_.expected_stream.logical_stream_id;
        expected_descriptor.spatial_roi_plan_sha256 =
            config_.expected_stream.spatial_roi_plan_sha256;
        expected_descriptor.native_raster =
            config_.expected_geometry.native_raster;
        expected_descriptor.content_rect = config_.expected_geometry.content_rect;
        expected_descriptor.encoded_raster =
            config_.expected_geometry.encoded_raster;
        expected_descriptor.encoded_content_rect =
            config_.expected_geometry.encoded_content_rect;
        expected_descriptor.padding = config_.expected_geometry.padding;
        expected_descriptor.source_pixel_format = kSpatialRoiMono8PixelFormat;
        expected_descriptor.bytes = static_cast<std::uint64_t>(mono_bytes);
        expected_descriptor.source_gpu_id = config_.expected_source_gpu_id;
        expected_descriptor.assigned_gpu_id = config_.recorder_gpu_id;
        expected_descriptor.assigned_shard_id =
            config_.expected_assigned_shard_id;
        expected_descriptor.routing_policy =
            config_.expected_geometry.routing_policy;
        std::string geometry_error;
        if (!validate_spatial_roi_frame_descriptor(expected_descriptor,
                                                   &geometry_error)) {
            error_ = geometry_error.empty()
                         ? "expected recorder detach geometry is invalid"
                         : geometry_error;
            return;
        }
        state_ = std::make_shared<detail::SpatialRoiRecorderCudaDetachState>(
            config_);
        auto* raw_state = state_.get();
        // Initialize the one validated size source immediately before the
        // recorder-owned CUDA allocations.
        raw_state->SetRasterBytes(mono_bytes, nv12_bytes);
        if (!raw_state->Initialize(&error_)) {
            state_.reset();
            return;
        }
        valid_ = true;
    } catch (const std::exception& exception) {
        state_.reset();
        set_error_noexcept(&error_, exception.what());
    } catch (...) {
        state_.reset();
        set_error_noexcept(&error_, "recorder detach pool construction threw");
    }
}

SpatialRoiRecorderCudaDetachPool::~SpatialRoiRecorderCudaDetachPool()
{
    Stop();
    state_.reset();
}

std::size_t SpatialRoiRecorderCudaDetachPool::slot_capacity() const noexcept
{
    return state_ ? state_->slot_capacity() : 0;
}

std::size_t SpatialRoiRecorderCudaDetachPool::available_slot_count() const noexcept
{
    return state_ ? state_->available_slot_count() : 0;
}

SpatialRoiRecorderCudaDetachCounters
SpatialRoiRecorderCudaDetachPool::counters() const noexcept
{
    return state_ ? state_->counters() : SpatialRoiRecorderCudaDetachCounters{};
}

SpatialRoiRecorderCudaDetachResult
SpatialRoiRecorderCudaDetachPool::TryDetach(
    const SpatialRoiIpcFrame& frame) noexcept
{
    if (operation_in_progress_.test_and_set(std::memory_order_acquire)) {
        if (state_) {
            state_->Increment(&SpatialRoiRecorderCudaDetachCounters::busy);
        }
        return make_result(SpatialRoiRecorderDetachStatus::kBusy,
                           "recorder detach pool already has an active caller");
    }
    AtomicFlagGuard operation_guard(&operation_in_progress_);
    bool source_import_unresolved = false;
    try {
        return TryDetachImpl(frame, &source_import_unresolved);
    } catch (const std::exception& exception) {
        if (state_) {
            state_->Increment(&SpatialRoiRecorderCudaDetachCounters::cuda_errors);
        }
        return make_result(
            source_import_unresolved
                ? SpatialRoiRecorderDetachStatus::kSourceQuarantined
                : SpatialRoiRecorderDetachStatus::kCudaError,
            exception.what(),
            !source_import_unresolved);
    } catch (...) {
        if (state_) {
            state_->Increment(&SpatialRoiRecorderCudaDetachCounters::cuda_errors);
        }
        return make_result(
            source_import_unresolved
                ? SpatialRoiRecorderDetachStatus::kSourceQuarantined
                : SpatialRoiRecorderDetachStatus::kCudaError,
            "recorder detach operation threw",
            !source_import_unresolved);
    }
}

SpatialRoiRecorderCudaDetachResult
SpatialRoiRecorderCudaDetachPool::TryDetachImpl(
    const SpatialRoiIpcFrame& frame,
    bool* source_import_unresolved)
{
    if (!source_import_unresolved) {
        return make_result(SpatialRoiRecorderDetachStatus::kInvalidArgument,
                           "missing recorder detach source-safety state");
    }
    *source_import_unresolved = false;
    if (state_) {
        state_->Increment(&SpatialRoiRecorderCudaDetachCounters::detach_attempted);
    }
    if (!valid_ || !state_) {
        return make_result(SpatialRoiRecorderDetachStatus::kInvalidArgument,
                           error_.empty() ? "recorder detach pool is invalid"
                                           : error_);
    }
    if (state_->quarantined()) {
        return make_result(
            SpatialRoiRecorderDetachStatus::kSourceQuarantined,
            "recorder detach pool retains an unresolved source import",
            false);
    }

    std::string validation_error;
    try {
        if (!validate_spatial_roi_ipc_frame(frame, &validation_error)) {
            state_->Increment(
                &SpatialRoiRecorderCudaDetachCounters::invalid_arguments);
            return make_result(
                SpatialRoiRecorderDetachStatus::kInvalidArgument,
                validation_error.empty() ? "invalid spatial ROI IPC FRAME"
                                          : validation_error);
        }
        if (!same_stream_identity(
                spatial_roi_ipc_stream_identity_from_descriptor(frame.descriptor),
                config_.expected_stream)) {
            state_->Increment(
                &SpatialRoiRecorderCudaDetachCounters::invalid_arguments);
            return make_result(
                SpatialRoiRecorderDetachStatus::kInvalidArgument,
                "FRAME stream identity does not match recorder detach pool");
        }
    } catch (const std::exception& exception) {
        state_->Increment(
            &SpatialRoiRecorderCudaDetachCounters::invalid_arguments);
        return make_result(SpatialRoiRecorderDetachStatus::kInvalidArgument,
                           exception.what());
    } catch (...) {
        state_->Increment(
            &SpatialRoiRecorderCudaDetachCounters::invalid_arguments);
        return make_result(SpatialRoiRecorderDetachStatus::kInvalidArgument,
                           "spatial ROI IPC FRAME validation threw");
    }

    const SpatialRoiFrameDescriptor& descriptor = frame.descriptor;
    if (descriptor.assigned_gpu_id != config_.recorder_gpu_id) {
        state_->Increment(&SpatialRoiRecorderCudaDetachCounters::wrong_device);
        return make_result(
            SpatialRoiRecorderDetachStatus::kWrongDevice,
            "FRAME assigned_gpu_id does not match recorder detach GPU");
    }
    if (descriptor.source_gpu_id != config_.expected_source_gpu_id) {
        state_->Increment(&SpatialRoiRecorderCudaDetachCounters::wrong_device);
        return make_result(
            SpatialRoiRecorderDetachStatus::kWrongDevice,
            "FRAME source_gpu_id does not match recorder detach binding");
    }
    if (descriptor.assigned_shard_id != config_.expected_assigned_shard_id) {
        state_->Increment(
            &SpatialRoiRecorderCudaDetachCounters::invalid_arguments);
        return make_result(
            SpatialRoiRecorderDetachStatus::kInvalidArgument,
            "FRAME assigned_shard_id does not match recorder detach binding");
    }
    if (!same_geometry(descriptor, config_.expected_geometry)) {
        state_->Increment(
            &SpatialRoiRecorderCudaDetachCounters::invalid_arguments);
        return make_result(
            SpatialRoiRecorderDetachStatus::kInvalidArgument,
            "FRAME geometry/routing does not match recorder detach contract");
    }

    try {
        const cudaError_t set_status = cudaSetDevice(config_.recorder_gpu_id);
        if (set_status != cudaSuccess) {
            state_->Increment(&SpatialRoiRecorderCudaDetachCounters::cuda_errors);
            return make_result(
                SpatialRoiRecorderDetachStatus::kCudaError,
                cuda_failure("cudaSetDevice(recorder detach)", set_status));
        }
        int current_device = -1;
        const cudaError_t get_status = cudaGetDevice(&current_device);
        if (get_status != cudaSuccess) {
            state_->Increment(&SpatialRoiRecorderCudaDetachCounters::cuda_errors);
            return make_result(
                SpatialRoiRecorderDetachStatus::kCudaError,
                cuda_failure("cudaGetDevice(recorder detach)", get_status));
        }
        if (current_device != config_.recorder_gpu_id) {
            state_->Increment(&SpatialRoiRecorderCudaDetachCounters::wrong_device);
            return make_result(
                SpatialRoiRecorderDetachStatus::kWrongDevice,
                "current CUDA device does not match recorder detach GPU");
        }
    } catch (const std::exception& exception) {
        state_->Increment(&SpatialRoiRecorderCudaDetachCounters::cuda_errors);
        return make_result(SpatialRoiRecorderDetachStatus::kCudaError,
                           exception.what());
    } catch (...) {
        state_->Increment(&SpatialRoiRecorderCudaDetachCounters::cuda_errors);
        return make_result(SpatialRoiRecorderDetachStatus::kCudaError,
                           "recorder detach device selection threw");
    }

    detail::SpatialRoiRecorderCudaDetachState::ClaimedSlot claimed;
    const auto claim_status = state_->Claim(&claimed);
    if (claim_status !=
        detail::SpatialRoiRecorderCudaDetachState::ClaimStatus::kClaimed) {
        if (claim_status ==
            detail::SpatialRoiRecorderCudaDetachState::ClaimStatus::kPoolExhausted) {
            return make_result(SpatialRoiRecorderDetachStatus::kPoolExhausted,
                               "recorder detach slot pool is exhausted");
        }
        if (claim_status ==
            detail::SpatialRoiRecorderCudaDetachState::ClaimStatus::kGenerationExhausted) {
            return make_result(SpatialRoiRecorderDetachStatus::kStopped,
                               "recorder detach slot generation exhausted");
        }
        if (claim_status ==
            detail::SpatialRoiRecorderCudaDetachState::ClaimStatus::kQuarantined) {
            return make_result(
                SpatialRoiRecorderDetachStatus::kSourceQuarantined,
                "recorder detach pool retains an unresolved source import",
                false);
        }
        return make_result(SpatialRoiRecorderDetachStatus::kStopped,
                           "recorder detach pool is stopped");
    }

    detail::SpatialRoiRecorderCudaDetachState::ClaimGuard claim_guard(
        state_.get(), claimed);
    void* imported_memory = nullptr;
    cudaEvent_t imported_event = nullptr;

    auto quarantine_claim = [&](const std::string& message) noexcept {
        claim_guard.Quarantine();
        imported_memory = nullptr;
        imported_event = nullptr;
        state_->Increment(&SpatialRoiRecorderCudaDetachCounters::cuda_errors);
        return make_result(SpatialRoiRecorderDetachStatus::kSourceQuarantined,
                           message,
                           false);
    };

    cudaIpcMemHandle_t memory_handle{};
    cudaIpcEventHandle_t event_handle{};
    if (!decode_hex_handle(frame.cuda_buffer.memory_handle_hex,
                           &memory_handle) ||
        !decode_hex_handle(frame.cuda_buffer.ready_event_handle_hex,
                           &event_handle)) {
        state_->Increment(&SpatialRoiRecorderCudaDetachCounters::invalid_arguments);
        return make_result(SpatialRoiRecorderDetachStatus::kInvalidArgument,
                           "CUDA IPC handle hex decoding failed");
    }

    // Start the one absolute CUDA-operation deadline before the first import.
    // Synchronous driver entry points cannot be interrupted in-process; the
    // recorder supervisor must still enforce its own process deadline. If an
    // import call returns late, the remaining query budget is not extended.
    const auto operation_deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(config_.operation_timeout_ms);
    const auto cache_lookup =
        state_->LookupCachedImport(memory_handle, event_handle);
    using CacheStatus = detail::SpatialRoiRecorderCudaDetachState::
        ImportCacheLookupStatus;
    if (cache_lookup.status == CacheStatus::kFull) {
        return make_result(
            SpatialRoiRecorderDetachStatus::kPoolExhausted,
            "recorder CUDA IPC import cache reached its bounded capacity");
    }
    if (cache_lookup.status == CacheStatus::kHandleCollision) {
        state_->Increment(
            &SpatialRoiRecorderCudaDetachCounters::invalid_arguments);
        return make_result(
            SpatialRoiRecorderDetachStatus::kInvalidArgument,
            "producer reused only one half of a cached CUDA memory/event binding");
    }
    if (cache_lookup.status == CacheStatus::kQuarantined) {
        return make_result(
            SpatialRoiRecorderDetachStatus::kSourceQuarantined,
            "recorder import cache is source-quarantined",
            false);
    }
    if (cache_lookup.status == CacheStatus::kStopped) {
        return make_result(SpatialRoiRecorderDetachStatus::kStopped,
                           "recorder import cache is stopped");
    }

    cudaError_t status = cudaSuccess;
    if (cache_lookup.status == CacheStatus::kHit) {
        imported_memory = cache_lookup.memory;
        imported_event = cache_lookup.event;
    } else {
        // Once an import is attempted, every uncertain failure is terminal.
        // The newly opened pair is held by ClaimGuard until the validated,
        // all-or-nothing cache commit transfers ownership to the pool.
        *source_import_unresolved = true;
        status = cudaIpcOpenMemHandle(
            &imported_memory, memory_handle, cudaIpcMemLazyEnablePeerAccess);
        if (status != cudaSuccess) {
            return quarantine_claim(
                cuda_failure("cudaIpcOpenMemHandle(recorder detach)", status));
        }
        claim_guard.SetImported(imported_memory, nullptr);
        state_->Increment(&SpatialRoiRecorderCudaDetachCounters::memory_imports);

        cudaPointerAttributes pointer_attributes{};
        status = cudaPointerGetAttributes(&pointer_attributes, imported_memory);
        if (status != cudaSuccess) {
            return quarantine_claim(
                cuda_failure("cudaPointerGetAttributes(recorder detach)", status));
        }
#if CUDART_VERSION >= 10000
        const cudaMemoryType imported_memory_type = pointer_attributes.type;
#else
        const cudaMemoryType imported_memory_type = pointer_attributes.memoryType;
#endif
        // Legacy cudaMalloc/cudaIpcOpenMemHandle mappings have reported the
        // importing device ordinal here on released NVIDIA drivers; fixed
        // drivers may report the physical source ordinal. The authenticated
        // handoff permits either declared endpoint ordinal.
        const bool reported_expected_ipc_device =
            pointer_attributes.device == config_.expected_source_gpu_id ||
            pointer_attributes.device == config_.recorder_gpu_id;
        if (imported_memory_type != cudaMemoryTypeDevice ||
            pointer_attributes.devicePointer == nullptr ||
            !reported_expected_ipc_device) {
            return quarantine_claim(
                "imported CUDA allocation is not accessible through the declared "
                "source/recorder GPU handoff");
        }
        CUdeviceptr imported_allocation_base = 0;
        std::size_t imported_allocation_bytes = 0;
        const CUresult address_status = cuMemGetAddressRange(
            &imported_allocation_base,
            &imported_allocation_bytes,
            reinterpret_cast<CUdeviceptr>(imported_memory));
        if (address_status != CUDA_SUCCESS) {
            return quarantine_claim(
                cuda_driver_failure("cuMemGetAddressRange(recorder detach)",
                                    address_status));
        }
        const std::uint64_t byte_offset = frame.cuda_buffer.byte_offset;
        const std::uint64_t byte_length = frame.cuda_buffer.byte_length;
        if (imported_allocation_base !=
                reinterpret_cast<CUdeviceptr>(imported_memory) ||
            byte_offset > imported_allocation_bytes ||
            byte_length > imported_allocation_bytes - byte_offset ||
            byte_length != state_->mono8_bytes()) {
            return quarantine_claim(
                "imported CUDA allocation does not contain the declared packed raster span");
        }

        status = cudaIpcOpenEventHandle(&imported_event, event_handle);
        if (status != cudaSuccess) {
            return quarantine_claim(
                cuda_failure("cudaIpcOpenEventHandle(recorder detach)", status));
        }
        claim_guard.SetImported(imported_memory, imported_event);
        state_->Increment(&SpatialRoiRecorderCudaDetachCounters::event_imports);
        if (!state_->CommitCachedImport(memory_handle,
                                        event_handle,
                                        imported_memory,
                                        imported_event)) {
            return quarantine_claim(
                "recorder CUDA IPC import cache commit failed");
        }
        // Cache ownership is now session-scoped. ClaimGuard only protects the
        // recorder output slot from this point forward.
        claim_guard.ClearImported();
    }

    // A cached mapping can remain open safely across FRAMEs, but the current
    // source occurrence is not RELEASE-safe until its raw copy completes.
    *source_import_unresolved = true;

    cudaError_t query_error = cudaSuccess;
    const BoundedCudaQueryStatus source_query = wait_for_cuda_query(
        [&]() noexcept { return cudaEventQuery(imported_event); },
        operation_deadline,
        &query_error);
    if (source_query == BoundedCudaQueryStatus::kTimedOut) {
        return quarantine_claim(
            "producer completion event exceeded recorder detach deadline");
    }
    if (source_query == BoundedCudaQueryStatus::kCudaError) {
        return quarantine_claim(
            cuda_failure("cudaEventQuery(recorder detach)", query_error));
    }

    status = cudaStreamWaitEvent(state_->stream(), imported_event, 0);
    if (status != cudaSuccess) {
        return quarantine_claim(
            cuda_failure("cudaStreamWaitEvent(recorder detach)", status));
    }
    state_->Increment(&SpatialRoiRecorderCudaDetachCounters::source_waits);

    status = cudaMemcpyAsync(claimed.mono8,
                             static_cast<const unsigned char*>(imported_memory) +
                                 frame.cuda_buffer.byte_offset,
                             state_->mono8_bytes(),
                             cudaMemcpyDeviceToDevice,
                             state_->stream());
    if (status != cudaSuccess) {
        return quarantine_claim(
            cuda_failure("cudaMemcpyAsync(Mono8 recorder detach)", status));
    }

    const BoundedCudaQueryStatus copy_query = wait_for_cuda_query(
        [&]() noexcept { return cudaStreamQuery(state_->stream()); },
        operation_deadline,
        &query_error);
    if (copy_query == BoundedCudaQueryStatus::kTimedOut) {
        return quarantine_claim(
            "recorder-owned detach copy exceeded the operation deadline");
    }
    if (copy_query == BoundedCudaQueryStatus::kCudaError) {
        return quarantine_claim(
            cuda_failure("cudaStreamQuery(recorder detach)", query_error));
    }

    // Raw-copy completion is the exact producer source-release boundary.
    // Session-scoped mappings stay cached, but no queued recorder work refers
    // to the producer allocation after this query succeeds.
    *source_import_unresolved = false;

    try {
        SpatialRoiRecorderDetachedFrame detached(
            state_,
            claimed.index,
            claimed.generation,
            spatial_roi_ipc_correlation_from_descriptor(descriptor),
            claimed.mono8,
            claimed.nv12,
            config_.expected_geometry.encoded_raster.width,
            config_.expected_geometry.encoded_raster.height,
            state_->mono8_bytes(),
            state_->nv12_bytes());
        claim_guard.ClearImported();
        claim_guard.Commit();
        state_->AddBytes(state_->mono8_bytes());
        state_->Increment(&SpatialRoiRecorderCudaDetachCounters::detached);

        SpatialRoiRecorderCudaDetachResult result;
        result.status = SpatialRoiRecorderDetachStatus::kDetached;
        result.frame = std::move(detached);
        return result;
    } catch (const std::exception& exception) {
        return make_result(SpatialRoiRecorderDetachStatus::kCudaError,
                           exception.what());
    } catch (...) {
        return make_result(SpatialRoiRecorderDetachStatus::kCudaError,
                           "recorder detach result construction threw");
    }
}

void SpatialRoiRecorderCudaDetachPool::Stop() noexcept
{
    if (state_) {
        state_->Stop();
    }
}

bool SpatialRoiRecorderCudaDetachPool::CloseCachedImports(
    std::string* error_out) noexcept
{
    if (!state_) {
        set_error_noexcept(error_out,
                           "recorder detach pool state is unavailable");
        return false;
    }
    if (operation_in_progress_.test_and_set(std::memory_order_acquire)) {
        set_error_noexcept(
            error_out,
            "recorder detach operation is active during import cache cleanup");
        return false;
    }
    AtomicFlagGuard operation_guard(&operation_in_progress_);
    return state_->CloseCachedImports(error_out);
}

}  // namespace orange::spatial_roi::ipc
