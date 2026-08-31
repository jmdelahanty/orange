#include "spatial_roi_batch_producer.h"

#include "session/spatial_roi_recording_config.h"
#include "shaman_v2_recording_identity.h"

#include <cuda_runtime.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace orange::spatial_roi {

namespace detail {
struct SpatialRoiVerifiedPlanCapability {
    // An injective, length-delimited snapshot of every public limit field.
    // The capability is const-owned by SpatialRoiBatchLimits, so a caller can
    // copy or mutate the public limits but cannot update this binding.
    std::string limits_snapshot;
};
}

namespace {

void set_error(std::string* error_out, std::string message)
{
    if (error_out) {
        *error_out = std::move(message);
    }
}

std::string cuda_failure(const char* operation, cudaError_t status)
{
    std::ostringstream out;
    out << operation << " failed: " << cudaGetErrorString(status);
    return out.str();
}

bool same_source_identity(const SpatialRoiFrameIdentity& lhs,
                          const SpatialRoiFrameIdentity& rhs)
{
    return lhs.recording_id == rhs.recording_id &&
           lhs.recording_identity_token == rhs.recording_identity_token &&
           lhs.producer_generation == rhs.producer_generation &&
           lhs.camera_id == rhs.camera_id &&
           lhs.camera_serial == rhs.camera_serial &&
           lhs.local_frame_id == rhs.local_frame_id &&
           lhs.camera_frame_id == rhs.camera_frame_id &&
           lhs.recording_frame_id == rhs.recording_frame_id &&
           lhs.camera_timestamp_ns == rhs.camera_timestamp_ns &&
           lhs.timestamp_sys_ns == rhs.timestamp_sys_ns;
}

bool is_canonical_sha256(const std::string& value)
{
    if (value.size() != 71 || value.compare(0, 7, "sha256:") != 0) {
        return false;
    }
    for (std::size_t index = 7; index < value.size(); ++index) {
        const unsigned char byte =
            static_cast<unsigned char>(value[index]);
        if (!((byte >= '0' && byte <= '9') ||
              (byte >= 'a' && byte <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool checked_add(std::size_t left, std::size_t right, std::size_t* result)
{
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        return false;
    }
    *result = left + right;
    return true;
}

bool checked_multiply(std::size_t left, std::size_t right, std::size_t* result)
{
    if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
        return false;
    }
    *result = left * right;
    return true;
}

bool checked_pointer_offset(const void* base,
                           std::size_t offset,
                           const char* description,
                           std::string* error_out,
                           void** result = nullptr)
{
    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(base);
    if (offset > std::numeric_limits<std::uintptr_t>::max() - address) {
        set_error(error_out,
                  std::string(description) + " pointer arithmetic overflows");
        return false;
    }
    if (result) {
        *result = reinterpret_cast<void*>(address + offset);
    }
    return true;
}

bool checked_const_pointer_offset(const void* base,
                                  std::size_t offset,
                                  const char* description,
                                  std::string* error_out,
                                  const unsigned char** result = nullptr)
{
    void* adjusted = nullptr;
    if (!checked_pointer_offset(
            base, offset, description, error_out, &adjusted)) {
        return false;
    }
    if (result) {
        *result = static_cast<const unsigned char*>(adjusted);
    }
    return true;
}

bool checked_rect_end(const SpatialRoiRect& rect,
                      std::uint64_t* right,
                      std::uint64_t* bottom)
{
    const std::uint64_t rect_right =
        static_cast<std::uint64_t>(rect.x) + rect.width;
    const std::uint64_t rect_bottom =
        static_cast<std::uint64_t>(rect.y) + rect.height;
    // This is currently unreachable for uint32_t fields, but keep the check
    // explicit so a future wire-width change cannot silently reintroduce an
    // overflow in rectangle validation.
    if (rect_right < rect.x || rect_bottom < rect.y) {
        return false;
    }
    if (right) {
        *right = rect_right;
    }
    if (bottom) {
        *bottom = rect_bottom;
    }
    return true;
}

bool checked_source_row_span(const SpatialRoiSourceView& source,
                             const SpatialRoiRect& rect,
                             std::string* error_out)
{
    std::size_t last_row = 0;
    std::size_t row_offset = 0;
    std::size_t last_byte_exclusive = 0;
    if (!checked_add(static_cast<std::size_t>(rect.y),
                     static_cast<std::size_t>(rect.height - 1),
                     &last_row) ||
        !checked_multiply(last_row,
                          source.pitch_bytes,
                          &row_offset) ||
        !checked_add(row_offset,
                     static_cast<std::size_t>(rect.x),
                     &row_offset) ||
        !checked_add(row_offset,
                     static_cast<std::size_t>(rect.width),
                     &last_byte_exclusive) ||
        !checked_const_pointer_offset(source.device_data,
                                      last_byte_exclusive,
                                      "camera-native source",
                                      error_out)) {
        set_error(error_out, "camera-native source pointer arithmetic overflows");
        return false;
    }
    return true;
}

bool checked_output_row_span(const SpatialRoiRaster& raster,
                             const SpatialRoiRect& rect,
                             std::string* error_out)
{
    std::size_t last_row = 0;
    std::size_t row_offset = 0;
    std::size_t last_byte_exclusive = 0;
    if (!checked_add(static_cast<std::size_t>(rect.y),
                     static_cast<std::size_t>(rect.height - 1),
                     &last_row) ||
        !checked_multiply(last_row,
                          static_cast<std::size_t>(raster.width),
                          &row_offset) ||
        !checked_add(row_offset,
                     static_cast<std::size_t>(rect.x),
                     &row_offset) ||
        !checked_add(row_offset,
                     static_cast<std::size_t>(rect.width),
                     &last_byte_exclusive)) {
        set_error(error_out, "encoded ROI pointer arithmetic overflows");
        return false;
    }
    return true;
}

bool is_safe_identifier(const std::string& value,
                        const std::size_t max_length = 160)
{
    if (value.empty() || value.size() > max_length) {
        return false;
    }
    const auto is_ascii_alnum = [](const unsigned char byte) {
        return (byte >= 'a' && byte <= 'z') ||
               (byte >= 'A' && byte <= 'Z') ||
               (byte >= '0' && byte <= '9');
    };
    if (!is_ascii_alnum(static_cast<unsigned char>(value.front()))) {
        return false;
    }
    return std::all_of(
        value.begin() + 1,
        value.end(),
        [&](const unsigned char byte) {
            return is_ascii_alnum(byte) || byte == '_' || byte == '-' ||
                   byte == '.';
        });
}

bool is_valid_recording_id(const std::string& value)
{
    if (value.empty() || value.size() > 512 ||
        std::isspace(static_cast<unsigned char>(value.front())) != 0 ||
        std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        return false;
    }
    return std::none_of(value.begin(), value.end(), [](const unsigned char byte) {
        return byte < 0x20 || byte == 0x7f;
    });
}

bool same_rect(const SpatialRoiRect& lhs, const SpatialRoiRect& rhs)
{
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.width == rhs.width &&
           lhs.height == rhs.height;
}

bool same_raster(const SpatialRoiRaster& lhs, const SpatialRoiRaster& rhs)
{
    return lhs.width == rhs.width && lhs.height == rhs.height;
}

bool rect_fits(std::uint32_t outer_width,
               std::uint32_t outer_height,
               const SpatialRoiRect& rect)
{
    if (rect.width == 0 || rect.height == 0) {
        return false;
    }
    std::uint64_t right = 0;
    std::uint64_t bottom = 0;
    if (!checked_rect_end(rect, &right, &bottom)) {
        return false;
    }
    return right <= outer_width && bottom <= outer_height;
}

bool validate_limits(const SpatialRoiBatchLimits& limits,
                     std::string* error_out)
{
    if (limits.gpu_id < 0) {
        set_error(error_out, "gpu_id must be non-negative");
        return false;
    }
    if (limits.batch_slot_count == 0) {
        set_error(error_out, "batch_slot_count must be positive");
        return false;
    }
    if (limits.pool_frames_per_stream == 0) {
        set_error(error_out, "pool_frames_per_stream must be positive");
        return false;
    }
    if (limits.batch_slot_count != limits.pool_frames_per_stream) {
        set_error(error_out,
                  "batch_slot_count must equal pool_frames_per_stream");
        return false;
    }
    if (limits.max_rois_per_batch == 0) {
        set_error(error_out, "max_rois_per_batch must be positive");
        return false;
    }
    if (limits.expected_camera_id < 0) {
        set_error(error_out, "expected_camera_id must be non-negative");
        return false;
    }
    if (!is_valid_recording_id(limits.expected_recording_id)) {
        set_error(error_out,
                  "expected_recording_id must be printable text of at most 512 bytes");
        return false;
    }
    if (!is_canonical_sha256(limits.expected_recording_identity_token)) {
        set_error(error_out,
                  "expected_recording_identity_token must be canonical sha256:<lowercase hex>");
        return false;
    }
    if (limits.expected_recording_identity_token !=
        shaman_v2_recording_identity::token_for_recording_id(
            limits.expected_recording_id)) {
        set_error(error_out,
                  "expected_recording_identity_token does not match expected_recording_id");
        return false;
    }
    if (!is_safe_identifier(limits.expected_producer_generation, 64)) {
        set_error(error_out,
                  "expected_producer_generation must be a safe identifier");
        return false;
    }
    if (!is_safe_identifier(limits.expected_camera_serial, 64)) {
        set_error(error_out, "expected_camera_serial must be a safe identifier");
        return false;
    }
    if (limits.expected_native_raster.width == 0 ||
        limits.expected_native_raster.height == 0) {
        set_error(error_out, "expected native raster dimensions must be positive");
        return false;
    }
    if (!is_canonical_sha256(limits.expected_spatial_roi_plan_sha256)) {
        set_error(
            error_out,
            "expected_spatial_roi_plan_sha256 must be canonical sha256:<lowercase hex>");
        return false;
    }
    if (limits.output_capacities.empty()) {
        set_error(error_out, "output_capacities must contain at least one ROI");
        return false;
    }
    if (limits.expected_roi_descriptors.size() !=
        limits.output_capacities.size()) {
        set_error(error_out,
                  "expected_roi_descriptors must exactly match output_capacities");
        return false;
    }
    if (limits.output_capacities.size() > limits.max_rois_per_batch) {
        set_error(error_out, "output_capacities exceeds max_rois_per_batch");
        return false;
    }

    std::size_t bytes_per_slot = 0;
    for (std::size_t index = 0; index < limits.output_capacities.size(); ++index) {
        const SpatialRoiOutputCapacity& capacity = limits.output_capacities[index];
        if (capacity.width == 0 || capacity.height == 0) {
            set_error(error_out,
                      "output_capacities[" + std::to_string(index) +
                          "] dimensions must be positive");
            return false;
        }
        std::size_t capacity_bytes = 0;
        if (!checked_multiply(static_cast<std::size_t>(capacity.width),
                              static_cast<std::size_t>(capacity.height),
                              &capacity_bytes) ||
            !checked_add(bytes_per_slot, capacity_bytes, &bytes_per_slot)) {
            set_error(error_out,
                      "output capacity byte count overflows size_t");
            return false;
        }

        const SpatialRoiPlanRoiBinding& descriptor =
            limits.expected_roi_descriptors[index];
        if (!is_safe_identifier(descriptor.roi_id, 64) ||
            !is_safe_identifier(descriptor.region_id, 64) ||
            !is_safe_identifier(descriptor.arena_group_id, 64) ||
            (!descriptor.arena_id.empty() &&
             !is_safe_identifier(descriptor.arena_id, 64)) ||
            !is_safe_identifier(descriptor.logical_stream_id, 64)) {
            set_error(error_out,
                      "expected ROI descriptor contains an unsafe identifier");
            return false;
        }
        if (!checked_multiply(static_cast<std::size_t>(capacity.width),
                              static_cast<std::size_t>(capacity.height),
                              &capacity_bytes) ||
            descriptor.output_bytes != capacity_bytes) {
            set_error(error_out,
                      "expected ROI descriptor byte count does not match output capacity");
            return false;
        }
        if (!same_raster(descriptor.encoded_raster, capacity) ||
            descriptor.source_rect.width == 0 ||
            descriptor.source_rect.height == 0 ||
            descriptor.encoded_content_rect.width == 0 ||
            descriptor.encoded_content_rect.height == 0) {
            set_error(error_out,
                      "expected ROI descriptor geometry is inconsistent with output capacity");
            return false;
        }
    }
    if (!checked_multiply(bytes_per_slot,
                          limits.batch_slot_count,
                          &bytes_per_slot)) {
        set_error(error_out,
                  "preallocated output pool byte count overflows size_t");
        return false;
    }

    if (limits.admission_pool_bytes == 0 && limits.expected_pool_bytes == 0) {
        set_error(error_out,
                  "admission_pool_bytes must bind the exact producer pool size");
        return false;
    }
    if (limits.admission_pool_bytes != 0 &&
        limits.admission_pool_bytes != bytes_per_slot) {
        set_error(error_out,
                  "admission_pool_bytes does not match output capacities and pool depth");
        return false;
    }
    if (limits.expected_pool_bytes != 0 &&
        limits.expected_pool_bytes != bytes_per_slot) {
        set_error(error_out,
                  "expected_pool_bytes does not match output capacities and pool depth");
        return false;
    }

    return true;
}

void append_snapshot_u64(std::string* snapshot, std::uint64_t value)
{
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        snapshot->push_back(static_cast<char>((value >> shift) & 0xffU));
    }
}

void append_snapshot_string(std::string* snapshot, const std::string& value)
{
    append_snapshot_u64(snapshot, static_cast<std::uint64_t>(value.size()));
    snapshot->append(value);
}

void append_snapshot_rect(std::string* snapshot, const SpatialRoiRect& rect)
{
    append_snapshot_u64(snapshot, rect.x);
    append_snapshot_u64(snapshot, rect.y);
    append_snapshot_u64(snapshot, rect.width);
    append_snapshot_u64(snapshot, rect.height);
}

void append_snapshot_raster(std::string* snapshot,
                            const SpatialRoiRaster& raster)
{
    append_snapshot_u64(snapshot, raster.width);
    append_snapshot_u64(snapshot, raster.height);
}

// This snapshot is intentionally injective rather than a short checksum: it
// is the immutable capability binding for construction, so a caller changing
// any public limit after verified-plan materialization must be rejected even
// if a digest collision could otherwise be manufactured.
std::string limits_snapshot(const SpatialRoiBatchLimits& limits)
{
    std::string snapshot;
    append_snapshot_u64(
        &snapshot, static_cast<std::uint64_t>(static_cast<std::int64_t>(limits.gpu_id)));
    append_snapshot_u64(&snapshot, limits.batch_slot_count);
    append_snapshot_u64(&snapshot, limits.max_rois_per_batch);
    append_snapshot_u64(&snapshot, limits.pool_frames_per_stream);
    append_snapshot_u64(&snapshot, limits.admission_pool_bytes);
    append_snapshot_u64(&snapshot, limits.expected_pool_bytes);
    append_snapshot_string(&snapshot, limits.expected_recording_id);
    append_snapshot_string(
        &snapshot, limits.expected_recording_identity_token);
    append_snapshot_string(&snapshot, limits.expected_producer_generation);
    append_snapshot_u64(&snapshot,
                        static_cast<std::uint64_t>(static_cast<std::int64_t>(
                            limits.expected_camera_id)));
    append_snapshot_string(&snapshot, limits.expected_camera_serial);
    append_snapshot_raster(&snapshot, limits.expected_native_raster);
    append_snapshot_string(&snapshot, limits.expected_spatial_roi_plan_sha256);

    append_snapshot_u64(
        &snapshot, static_cast<std::uint64_t>(limits.expected_roi_descriptors.size()));
    for (const SpatialRoiPlanRoiBinding& descriptor :
         limits.expected_roi_descriptors) {
        append_snapshot_string(&snapshot, descriptor.roi_id);
        append_snapshot_string(&snapshot, descriptor.region_id);
        append_snapshot_string(&snapshot, descriptor.arena_group_id);
        append_snapshot_string(&snapshot, descriptor.arena_id);
        append_snapshot_string(&snapshot, descriptor.logical_stream_id);
        append_snapshot_rect(&snapshot, descriptor.source_rect);
        append_snapshot_raster(&snapshot, descriptor.encoded_raster);
        append_snapshot_rect(&snapshot, descriptor.encoded_content_rect);
        append_snapshot_u64(&snapshot,
                            static_cast<std::uint64_t>(descriptor.output_bytes));
    }

    append_snapshot_u64(
        &snapshot, static_cast<std::uint64_t>(limits.output_capacities.size()));
    for (const SpatialRoiOutputCapacity& capacity : limits.output_capacities) {
        append_snapshot_raster(&snapshot, capacity);
    }
    return snapshot;
}

}  // namespace

const char* spatial_roi_batch_status_name(SpatialRoiBatchStatus status) noexcept
{
    switch (status) {
        case SpatialRoiBatchStatus::kAccepted:
            return "accepted";
        case SpatialRoiBatchStatus::kInvalidArgument:
            return "invalid_argument";
        case SpatialRoiBatchStatus::kPoolExhausted:
            return "pool_exhausted";
        case SpatialRoiBatchStatus::kCudaError:
            return "cuda_error";
        case SpatialRoiBatchStatus::kSourceQuarantined:
            return "source_quarantined";
        case SpatialRoiBatchStatus::kStopped:
            return "stopped";
    }
    return "unknown";
}

bool validate_spatial_roi_batch(
    const SpatialRoiSourceView& source,
    const std::vector<SpatialRoiWorkItem>& work_items,
    const SpatialRoiBatchLimits& limits,
    std::string* error_out)
{
    if (!validate_limits(limits, error_out)) {
        return false;
    }
    if (!source.device_data) {
        set_error(error_out, "source device_data is null");
        return false;
    }
    if (!source.source_lease) {
        set_error(error_out,
                  "source source_lease is required for asynchronous input");
        return false;
    }
    if (source.pixel_format != SpatialRoiSourcePixelFormat::kMono8) {
        set_error(error_out,
                  "source pixel_format must be Mono8 for schema v1");
        return false;
    }
    if (!source.ready_event) {
        set_error(error_out,
                  "source ready_event is required for asynchronous input");
        return false;
    }
    if (source.width == 0 || source.height == 0) {
        set_error(error_out, "source raster dimensions must be positive");
        return false;
    }
    if (source.pitch_bytes < source.width) {
        set_error(error_out, "source pitch is smaller than its Mono8 row width");
        return false;
    }
    std::size_t source_raster_bytes = 0;
    if (!checked_multiply(source.pitch_bytes,
                          static_cast<std::size_t>(source.height),
                          &source_raster_bytes)) {
        set_error(error_out,
                  "source allocation byte calculation overflows size_t");
        return false;
    }
    if (source.allocation_bytes < source_raster_bytes) {
        set_error(error_out,
                  "source allocation_bytes is smaller than pitch * height");
        return false;
    }
    if (source.gpu_id != limits.gpu_id) {
        set_error(error_out, "source GPU does not match the batch producer GPU");
        return false;
    }
    if (source.width != limits.expected_native_raster.width ||
        source.height != limits.expected_native_raster.height) {
        set_error(error_out, "source native raster does not match the configured expectation");
        return false;
    }
    if (work_items.empty()) {
        set_error(error_out, "spatial ROI batch must contain at least one work item");
        return false;
    }
    if (work_items.size() > limits.max_rois_per_batch) {
        set_error(error_out, "spatial ROI batch exceeds max_rois_per_batch");
        return false;
    }
    if (work_items.size() != limits.output_capacities.size()) {
        set_error(error_out,
                  "spatial ROI work item count must exactly match output_capacities");
        return false;
    }

    std::unordered_set<std::string> roi_ids;
    std::unordered_set<std::string> logical_stream_ids;
    const SpatialRoiFrameIdentity& source_identity = source.identity;
    if (!is_safe_identifier(source_identity.camera_serial, 64)) {
        set_error(error_out, "source camera_serial must be a safe identifier");
        return false;
    }
    if (!is_valid_recording_id(source_identity.recording_id)) {
        set_error(error_out,
                  "source recording_id must be printable text of at most 512 bytes");
        return false;
    }
    if (!is_canonical_sha256(source_identity.recording_identity_token)) {
        set_error(
            error_out,
            "source recording_identity_token must be canonical sha256:<lowercase hex>");
        return false;
    }
    if (source_identity.recording_identity_token !=
        shaman_v2_recording_identity::token_for_recording_id(
            source_identity.recording_id)) {
        set_error(error_out,
                  "source recording_identity_token does not match recording_id");
        return false;
    }
    if (source_identity.recording_id != limits.expected_recording_id ||
        source_identity.recording_identity_token !=
            limits.expected_recording_identity_token) {
        set_error(error_out,
                  "source recording identity does not match the verified plan");
        return false;
    }
    if (!is_safe_identifier(source_identity.producer_generation, 64)) {
        set_error(error_out, "source producer_generation must be a safe identifier");
        return false;
    }
    if (source_identity.producer_generation !=
        limits.expected_producer_generation) {
        set_error(error_out,
                  "source producer_generation does not match the verified plan");
        return false;
    }
    if (source_identity.camera_id < 0) {
        set_error(error_out, "source camera_id must be non-negative");
        return false;
    }
    if (source_identity.camera_id != limits.expected_camera_id ||
        source_identity.camera_serial != limits.expected_camera_serial) {
        set_error(error_out,
                  "source camera identity does not match the configured expectation");
        return false;
    }
    if (source_identity.recording_frame_id == 0) {
        set_error(error_out, "source recording_frame_id must be positive");
        return false;
    }

    for (std::size_t index = 0; index < work_items.size(); ++index) {
        const SpatialRoiWorkItem& item = work_items[index];
        const std::string prefix = "work item " + std::to_string(index) + ": ";
        if (!same_source_identity(source_identity, item.source)) {
            set_error(error_out, prefix + "source identity differs from the source view");
            return false;
        }
        if (!is_safe_identifier(item.roi_id, 64)) {
            set_error(error_out, prefix + "roi_id must be a safe identifier");
            return false;
        }
        const SpatialRoiPlanRoiBinding& descriptor =
            limits.expected_roi_descriptors[index];
        if (item.roi_id != descriptor.roi_id ||
            item.region_id != descriptor.region_id ||
            item.arena_group_id != descriptor.arena_group_id ||
            item.arena_id != descriptor.arena_id ||
            item.logical_stream_id != descriptor.logical_stream_id) {
            set_error(error_out,
                      prefix + "ROI identity/stream labels do not match the verified plan order");
            return false;
        }
        if (!roi_ids.insert(item.roi_id).second) {
            set_error(error_out, prefix + "roi_id is duplicated within the batch");
            return false;
        }
        if (!is_safe_identifier(item.region_id, 64)) {
            set_error(error_out, prefix + "region_id must be a safe identifier");
            return false;
        }
        if (!is_safe_identifier(item.arena_group_id, 64)) {
            set_error(error_out, prefix + "arena_group_id must be a safe identifier");
            return false;
        }
        if (!item.arena_id.empty() && !is_safe_identifier(item.arena_id, 64)) {
            set_error(error_out, prefix + "arena_id must be empty or a safe identifier");
            return false;
        }
        if (!is_safe_identifier(item.logical_stream_id, 64)) {
            set_error(error_out, prefix + "logical_stream_id must be a safe identifier");
            return false;
        }
        if (!logical_stream_ids.insert(item.logical_stream_id).second) {
            set_error(
                error_out,
                prefix + "logical_stream_id is duplicated within the batch");
            return false;
        }
        if (!is_canonical_sha256(item.spatial_roi_plan_sha256)) {
            set_error(
                error_out,
                prefix +
                    "spatial_roi_plan_sha256 must be canonical sha256:<lowercase hex>");
            return false;
        }
        if (item.spatial_roi_plan_sha256 !=
            limits.expected_spatial_roi_plan_sha256) {
            set_error(error_out,
                      prefix +
                          "spatial_roi_plan_sha256 does not match the configured plan");
            return false;
        }

        const SpatialRoiOutputGeometry& geometry = item.geometry;
        if (!rect_fits(source.width, source.height, geometry.content_rect)) {
            set_error(error_out, prefix + "camera-native content_rect is out of bounds");
            return false;
        }
        if (geometry.encoded_raster.width == 0 ||
            geometry.encoded_raster.height == 0) {
            set_error(error_out, prefix + "encoded_raster dimensions must be positive");
            return false;
        }
        const SpatialRoiOutputCapacity& capacity = limits.output_capacities[index];
        if (geometry.encoded_raster.width != capacity.width ||
            geometry.encoded_raster.height != capacity.height) {
            set_error(error_out,
                      prefix + "encoded_raster does not match its ordered output capacity");
            return false;
        }
        if (!rect_fits(
                geometry.encoded_raster.width,
                geometry.encoded_raster.height,
                geometry.encoded_content_rect)) {
            set_error(error_out, prefix + "encoded_content_rect is out of bounds");
            return false;
        }
        if (geometry.content_rect.width != geometry.encoded_content_rect.width ||
            geometry.content_rect.height != geometry.encoded_content_rect.height) {
            set_error(
                error_out,
                prefix +
                    "content dimensions differ between source and encoded raster; resizing is forbidden");
            return false;
        }
        if (!same_rect(geometry.content_rect, descriptor.source_rect) ||
            !same_raster(geometry.encoded_raster, descriptor.encoded_raster) ||
            !same_rect(geometry.encoded_content_rect,
                       descriptor.encoded_content_rect)) {
            set_error(error_out,
                      prefix + "source or encoded geometry does not match the verified plan order");
            return false;
        }
        std::string arithmetic_error;
        if (!checked_source_row_span(source,
                                     geometry.content_rect,
                                     &arithmetic_error) ||
            !checked_output_row_span(geometry.encoded_raster,
                                     geometry.encoded_content_rect,
                                     &arithmetic_error)) {
            set_error(error_out, prefix + arithmetic_error);
            return false;
        }
    }

    if (error_out) {
        error_out->clear();
    }
    return true;
}

namespace {

bool materialize_batch_limits_from_verified_plan(
    const orange::session::spatial_roi::SpatialRoiRecordingPlan& plan,
    const std::string& camera_serial,
    int gpu_id,
    SpatialRoiBatchLimits* limits_out,
    std::string* error_out)
{
    if (!limits_out) {
        set_error(error_out, "spatial ROI batch limits destination is null");
        return false;
    }
    if (gpu_id < 0) {
        set_error(error_out, "gpu_id must be non-negative");
        return false;
    }

    const auto camera_it = plan.cameras.find(camera_serial);
    if (camera_it == plan.cameras.end()) {
        set_error(error_out,
                  "verified spatial ROI plan has no camera " + camera_serial);
        return false;
    }
    const auto& camera = camera_it->second;
    if (camera.camera_serial != camera_serial || camera.rois.empty()) {
        set_error(error_out,
                  "verified spatial ROI plan camera has no admitted ROI descriptors");
        return false;
    }
    SpatialRoiBatchLimits limits;
    limits.gpu_id = gpu_id;
    limits.batch_slot_count = plan.pool_frames_per_stream;
    limits.pool_frames_per_stream = plan.pool_frames_per_stream;
    limits.max_rois_per_batch = camera.rois.size();
    limits.admission_pool_bytes = camera.pool_bytes;
    limits.expected_pool_bytes = camera.pool_bytes;
    limits.expected_recording_id = plan.recording_id;
    limits.expected_recording_identity_token = plan.recording_identity_token;
    limits.expected_producer_generation = plan.producer_generation;
    limits.expected_camera_id = camera.camera_id;
    limits.expected_camera_serial = camera.camera_serial;
    limits.expected_native_raster = {
        camera.native_raster.width, camera.native_raster.height};
    limits.expected_spatial_roi_plan_sha256 = plan.plan_sha256;
    limits.output_capacities.reserve(camera.rois.size());
    limits.expected_roi_descriptors.reserve(camera.rois.size());
    for (const auto& roi : camera.rois) {
        SpatialRoiPlanRoiBinding descriptor;
        descriptor.roi_id = roi.roi_id;
        descriptor.region_id = roi.region_id;
        descriptor.arena_group_id = roi.arena_group_id;
        descriptor.arena_id = roi.has_arena_id ? roi.arena_id : std::string();
        descriptor.logical_stream_id = roi.logical_stream_id;
        descriptor.source_rect = {
            roi.source_rect.x,
            roi.source_rect.y,
            roi.source_rect.width,
            roi.source_rect.height,
        };
        descriptor.encoded_raster = {
            roi.encoded_raster.width, roi.encoded_raster.height};
        descriptor.encoded_content_rect = {
            roi.encoded_content_rect.x,
            roi.encoded_content_rect.y,
            roi.encoded_content_rect.width,
            roi.encoded_content_rect.height,
        };
        if (!checked_multiply(static_cast<std::size_t>(
                                  roi.encoded_raster.width),
                              static_cast<std::size_t>(
                                  roi.encoded_raster.height),
                              &descriptor.output_bytes)) {
            set_error(error_out,
                      "verified spatial ROI producer pool exceeds size_t capacity");
            return false;
        }
        limits.output_capacities.push_back(descriptor.encoded_raster);
        limits.expected_roi_descriptors.push_back(std::move(descriptor));
    }

    std::string limits_error;
    if (!validate_limits(limits, &limits_error)) {
        set_error(error_out,
                  "verified spatial ROI plan produced invalid batch limits: " +
                      limits_error);
        return false;
    }
    *limits_out = std::move(limits);
    if (error_out) {
        error_out->clear();
    }
    return true;
}

}  // namespace

bool spatial_roi_batch_limits_from_verified_plan(
    const nlohmann::json& verified_plan,
    const std::string& camera_serial,
    int gpu_id,
    SpatialRoiBatchLimits* limits_out,
    std::string* error_out)
{
    try {
        orange::session::spatial_roi::SpatialRoiRecordingPlan plan;
        if (!orange::session::spatial_roi::parse_verified_plan(
                verified_plan, &plan, error_out)) {
            return false;
        }
        if (!materialize_batch_limits_from_verified_plan(
                plan, camera_serial, gpu_id, limits_out, error_out)) {
            return false;
        }
        auto capability =
            std::make_shared<detail::SpatialRoiVerifiedPlanCapability>();
        capability->limits_snapshot = limits_snapshot(*limits_out);
        limits_out->verified_plan_capability_ = std::move(capability);
        return true;
    } catch (const std::exception& ex) {
        set_error(error_out,
                  std::string("verified spatial ROI plan serialization failed: ") +
                      ex.what());
        return false;
    } catch (...) {
        set_error(error_out,
                  "verified spatial ROI plan serialization failed: unknown exception");
        return false;
    }
}

namespace detail {

class SpatialRoiBatchPoolState
    : public std::enable_shared_from_this<SpatialRoiBatchPoolState> {
public:
    struct OutputStorage {
        unsigned char* device_data = nullptr;
    };

    struct AcquiredSlot {
        std::size_t index = 0;
        std::uint64_t generation = 0;
        const std::vector<OutputStorage>* outputs = nullptr;
        cudaEvent_t completion_event = nullptr;
    };

    explicit SpatialRoiBatchPoolState(SpatialRoiBatchLimits limits)
        : limits_(limits)
    {
    }

    ~SpatialRoiBatchPoolState()
    {
        accepting_.store(false, std::memory_order_release);
        if (limits_.gpu_id >= 0) {
            (void)cudaSetDevice(limits_.gpu_id);
        }
        if (stream_) {
            (void)cudaStreamSynchronize(stream_);
        }
        for (Slot& slot : slots_) {
            if (slot.completion_event) {
                (void)cudaEventDestroy(slot.completion_event);
                slot.completion_event = nullptr;
            }
            for (OutputStorage& output : slot.outputs) {
                if (output.device_data) {
                    (void)cudaFree(output.device_data);
                    output.device_data = nullptr;
                }
            }
        }
        if (stream_) {
            (void)cudaStreamDestroy(stream_);
            stream_ = nullptr;
        }
    }

    bool Initialize(std::string* error_out)
    {
        cudaError_t status = cudaSetDevice(limits_.gpu_id);
        if (status != cudaSuccess) {
            set_error(error_out, cuda_failure("cudaSetDevice", status));
            return false;
        }
        status = cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking);
        if (status != cudaSuccess) {
            set_error(
                error_out,
                cuda_failure("cudaStreamCreateWithFlags(spatial ROI batch)", status));
            return false;
        }

        slots_.resize(limits_.batch_slot_count);
        for (Slot& slot : slots_) {
            slot.outputs.resize(limits_.output_capacities.size());
            for (std::size_t index = 0; index < slot.outputs.size(); ++index) {
                OutputStorage& output = slot.outputs[index];
                const SpatialRoiOutputCapacity& capacity =
                    limits_.output_capacities[index];
                const std::size_t output_capacity_bytes =
                    static_cast<std::size_t>(capacity.width) * capacity.height;
                // Keep every ROI at the base of its own allocation so a later
                // recorder lane can export it directly through CUDA IPC. The
                // active raster is packed at its own encoded width because the
                // current IPC descriptor does not carry a row-pitch field.
                status = cudaMalloc(
                    reinterpret_cast<void**>(&output.device_data),
                    output_capacity_bytes);
                if (status != cudaSuccess) {
                    set_error(
                        error_out,
                        cuda_failure(
                            "cudaMalloc(spatial ROI output slot)",
                            status));
                    return false;
                }
            }
            status = cudaEventCreateWithFlags(
                &slot.completion_event,
                cudaEventDisableTiming);
            if (status != cudaSuccess) {
                set_error(
                    error_out,
                    cuda_failure("cudaEventCreateWithFlags(spatial ROI batch)", status));
                return false;
            }
        }
        accepting_.store(true, std::memory_order_release);
        return true;
    }

    bool accepting() const noexcept
    {
        return accepting_.load(std::memory_order_acquire);
    }

    void StopAccepting() noexcept
    {
        // Admission and enqueue are one linearizable critical section. A
        // caller racing TryProduce must either acquire this mutex first (and
        // observe stopped) or finish its complete enqueue sequence before this
        // call returns.
        std::lock_guard<std::mutex> enqueue_lock(enqueue_mutex_);
        std::lock_guard<std::mutex> lock(pool_mutex_);
        accepting_.store(false, std::memory_order_release);
    }

    void Quarantine() noexcept
    {
        std::lock_guard<std::mutex> enqueue_lock(enqueue_mutex_);
        std::lock_guard<std::mutex> lock(pool_mutex_);
        accepting_.store(false, std::memory_order_release);
        quarantine_for_process_lifetime();
    }

    // Internal error path used while TryProduce already owns enqueue_mutex_.
    // It must not call StopAccepting(), which would recursively lock the same
    // non-recursive mutex.
    void DisableAccepting() noexcept
    {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        accepting_.store(false, std::memory_order_release);
    }

    // TryAcquire is called while SpatialRoiBatchProducer owns enqueue_mutex_.
    // A device-selection failure cannot prove that source reads admitted by
    // earlier batches have completed, so retain the complete pool rather than
    // merely stopping future admission.
    void QuarantineWithEnqueueLockHeld() noexcept
    {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        accepting_.store(false, std::memory_order_release);
        quarantine_for_process_lifetime();
    }

    bool TryAcquire(AcquiredSlot* acquired,
                    SpatialRoiBatchStatus* failure_status,
                    const std::shared_ptr<void>& source_lease,
                    std::string* error_out)
    {
        if (failure_status) {
            *failure_status = SpatialRoiBatchStatus::kCudaError;
        }
        if (!acquired) {
            set_error(error_out, "internal error: null acquired-slot destination");
            return false;
        }
        if (!accepting()) {
            if (failure_status) {
                *failure_status = SpatialRoiBatchStatus::kStopped;
            }
            set_error(error_out, "spatial ROI batch producer is stopped");
            return false;
        }

        cudaError_t status = cudaSetDevice(limits_.gpu_id);
        if (status != cudaSuccess) {
            QuarantineWithEnqueueLockHeld();
            if (failure_status) {
                *failure_status =
                    SpatialRoiBatchStatus::kSourceQuarantined;
            }
            set_error(error_out, cuda_failure("cudaSetDevice", status));
            return false;
        }

        std::lock_guard<std::mutex> lock(pool_mutex_);
        // The first check is only a fast path. TryProduce holds
        // enqueue_mutex_ while calling this method, and StopAccepting takes
        // that same admission lock before changing accepting_, so a stop
        // racing with admission cannot let a batch claim a slot after the
        // stop has returned.
        if (!accepting()) {
            if (failure_status) {
                *failure_status = SpatialRoiBatchStatus::kStopped;
            }
            set_error(error_out, "spatial ROI batch producer is stopped");
            return false;
        }
        if (!reclaim_completed_locked(error_out)) {
            accepting_.store(false, std::memory_order_release);
            if (failure_status) {
                *failure_status =
                    SpatialRoiBatchStatus::kSourceQuarantined;
            }
            return false;
        }
        for (std::size_t index = 0; index < slots_.size(); ++index) {
            Slot& slot = slots_[index];
            if (slot.state != SlotState::kFree) {
                continue;
            }
            slot.state = SlotState::kInUse;
            ++slot.generation;
            // shared_ptr assignment is noexcept. The slot owns the source
            // lease independently of the returned result so Reset() cannot
            // release it before the completion event is observed.
            slot.source_lease = source_lease;
            acquired->index = index;
            acquired->generation = slot.generation;
            acquired->outputs = &slot.outputs;
            acquired->completion_event = slot.completion_event;
            return true;
        }
        if (failure_status) {
            *failure_status = SpatialRoiBatchStatus::kPoolExhausted;
        }
        set_error(error_out, "all preallocated spatial ROI batch slots are in use");
        return false;
    }

    void Release(std::size_t index, std::uint64_t generation) noexcept
    {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        if (index >= slots_.size()) {
            return;
        }
        Slot& slot = slots_[index];
        if (slot.state != SlotState::kInUse || slot.generation != generation) {
            return;
        }
        slot.state = SlotState::kPendingRecycle;
    }

    cudaError_t FailAcquiredSlot(const AcquiredSlot& acquired) noexcept
    {
        DisableAccepting();

        // A failed enqueue may follow one or more successful async operations
        // (for example, the full-raster memset before the native-pixel copy).
        // Do not return the source lease to acquisition merely because the
        // submission failed. The synchronous drain is deliberately confined
        // to this rare failure path; its status is returned to TryProduce and
        // included in the failure result.
        cudaError_t cleanup_status = cudaErrorUnknown;
        if (limits_.gpu_id >= 0) {
            cleanup_status = cudaSetDevice(limits_.gpu_id);
            if (cleanup_status == cudaSuccess && stream_) {
                cleanup_status = cudaStreamSynchronize(stream_);
            }
        }
        std::lock_guard<std::mutex> lock(pool_mutex_);
        if (acquired.index >= slots_.size()) {
            return cleanup_status;
        }
        Slot& slot = slots_[acquired.index];
        if (slot.state == SlotState::kInUse &&
            slot.generation == acquired.generation) {
            // A successful stream synchronization makes every queued source
            // read and output write complete, so the allocation can be reused.
            // If synchronization itself fails, the CUDA runtime has not
            // proved that any queued work is finished. Retain the entire pool
            // state in a process-lifetime self-cycle below: destroying only
            // the source lease would still free events, outputs, or the stream
            // while the failed work might be using them.
            if (cleanup_status == cudaSuccess) {
                slot.source_lease.reset();
                slot.state = SlotState::kFree;
            } else {
                slot.state = SlotState::kInUse;
                quarantine_for_process_lifetime();
            }
        }
        return cleanup_status;
    }

    std::size_t available_slot_count() noexcept
    {
        if (cudaSetDevice(limits_.gpu_id) != cudaSuccess) {
            return 0;
        }
        std::lock_guard<std::mutex> lock(pool_mutex_);
        std::string ignored;
        (void)reclaim_completed_locked(&ignored);
        std::size_t count = 0;
        for (const Slot& slot : slots_) {
            if (slot.state == SlotState::kFree) {
                ++count;
            }
        }
        return count;
    }

    std::size_t pending_slot_count() noexcept
    {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        std::size_t count = 0;
        for (const Slot& slot : slots_) {
            if (slot.state == SlotState::kPendingRecycle) {
                ++count;
            }
        }
        return count;
    }

    std::size_t slot_capacity() const noexcept { return slots_.size(); }
    cudaStream_t stream() const noexcept { return stream_; }
    std::mutex& enqueue_mutex() noexcept { return enqueue_mutex_; }

    std::uint64_t next_batch_sequence() noexcept
    {
        return next_batch_sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

private:
    void quarantine_for_process_lifetime() noexcept
    {
        if (quarantined_self_) {
            return;
        }
        try {
            // Keep the stream, events, output allocations, slot source lease,
            // and synchronization state alive forever after an unproven
            // cleanup. This is deliberately fail-stop for this producer, but
            // avoids releasing resources that a CUDA operation may still use.
            quarantined_self_ = shared_from_this();
        } catch (...) {
            // A missing control block or an impossible allocation failure
            // cannot be handled safely: releasing the pool could race queued
            // device work. Fail closed instead of risking use-after-free.
            std::abort();
        }
    }

    enum class SlotState {
        kFree,
        kInUse,
        kPendingRecycle,
    };

    struct Slot {
        std::vector<OutputStorage> outputs;
        cudaEvent_t completion_event = nullptr;
        std::shared_ptr<void> source_lease;
        SlotState state = SlotState::kFree;
        std::uint64_t generation = 0;
    };

    bool reclaim_completed_locked(std::string* error_out)
    {
        for (Slot& slot : slots_) {
            if (slot.state != SlotState::kPendingRecycle) {
                continue;
            }
            const cudaError_t status = cudaEventQuery(slot.completion_event);
            if (status == cudaSuccess) {
                slot.source_lease.reset();
                slot.state = SlotState::kFree;
                continue;
            }
            if (status == cudaErrorNotReady) {
                continue;
            }
            set_error(
                error_out,
                cuda_failure("cudaEventQuery(spatial ROI batch)", status));
            accepting_.store(false, std::memory_order_release);
            quarantine_for_process_lifetime();
            return false;
        }
        return true;
    }

    SpatialRoiBatchLimits limits_;
    cudaStream_t stream_ = nullptr;
    std::vector<Slot> slots_;
    std::atomic<bool> accepting_{false};
    std::atomic<std::uint64_t> next_batch_sequence_{0};
    std::mutex enqueue_mutex_;
    std::mutex pool_mutex_;
    // Set only after cleanup synchronization fails. This intentional cycle
    // prevents the pool destructor from freeing CUDA resources or source
    // leases whose in-flight use was not disproven.
    std::shared_ptr<SpatialRoiBatchPoolState> quarantined_self_;
};

// Scope guard for the interval after pool admission and before the result has
// been committed to its caller. Every return or exception in that interval
// drains/reclaims the acquired slot, and a failed drain quarantines the entire
// pool state for the process lifetime.
class AcquiredSlotCleanup {
public:
    AcquiredSlotCleanup(SpatialRoiBatchPoolState* state,
                        SpatialRoiBatchPoolState::AcquiredSlot slot) noexcept
        : state_(state), slot_(slot)
    {
    }

    ~AcquiredSlotCleanup() noexcept
    {
        if (!committed_ && state_) {
            (void)state_->FailAcquiredSlot(slot_);
        }
    }

    cudaError_t cleanup_now() noexcept
    {
        if (committed_ || !state_) {
            return cudaSuccess;
        }
        committed_ = true;
        return state_->FailAcquiredSlot(slot_);
    }

    void commit() noexcept { committed_ = true; }

private:
    SpatialRoiBatchPoolState* state_ = nullptr;
    SpatialRoiBatchPoolState::AcquiredSlot slot_;
    bool committed_ = false;
};

}  // namespace detail

SpatialRoiBatchResult::~SpatialRoiBatchResult()
{
    Reset();
}

SpatialRoiBatchResult::SpatialRoiBatchResult(
    SpatialRoiBatchResult&& other) noexcept
    : status_(other.status_),
      source_release_safe_(other.source_release_safe_),
      error_(std::move(other.error_)),
      batch_sequence_(other.batch_sequence_),
      completion_event_(other.completion_event_),
      outputs_(std::move(other.outputs_)),
      source_lease_(std::move(other.source_lease_)),
      state_(std::move(other.state_)),
      slot_index_(other.slot_index_),
      slot_generation_(other.slot_generation_)
{
    other.status_ = SpatialRoiBatchStatus::kStopped;
    other.source_release_safe_ = true;
    other.batch_sequence_ = 0;
    other.completion_event_ = nullptr;
    other.source_lease_.reset();
    other.slot_index_ = 0;
    other.slot_generation_ = 0;
}

SpatialRoiBatchResult& SpatialRoiBatchResult::operator=(
    SpatialRoiBatchResult&& other) noexcept
{
    if (this == &other) {
        return *this;
    }
    Reset();
    status_ = other.status_;
    source_release_safe_ = other.source_release_safe_;
    error_ = std::move(other.error_);
    batch_sequence_ = other.batch_sequence_;
    completion_event_ = other.completion_event_;
    outputs_ = std::move(other.outputs_);
    source_lease_ = std::move(other.source_lease_);
    state_ = std::move(other.state_);
    slot_index_ = other.slot_index_;
    slot_generation_ = other.slot_generation_;

    other.status_ = SpatialRoiBatchStatus::kStopped;
    other.source_release_safe_ = true;
    other.batch_sequence_ = 0;
    other.completion_event_ = nullptr;
    other.source_lease_.reset();
    other.slot_index_ = 0;
    other.slot_generation_ = 0;
    return *this;
}

void SpatialRoiBatchResult::Reset() noexcept
{
    if (state_ && status_ == SpatialRoiBatchStatus::kAccepted) {
        state_->Release(slot_index_, slot_generation_);
    }
    state_.reset();
    outputs_.clear();
    source_lease_.reset();
    completion_event_ = nullptr;
    batch_sequence_ = 0;
    slot_index_ = 0;
    slot_generation_ = 0;
    status_ = SpatialRoiBatchStatus::kStopped;
    source_release_safe_ = true;
    error_.clear();
}

SpatialRoiBatchProducer::SpatialRoiBatchProducer(
    const SpatialRoiBatchLimits& limits)
    : limits_(limits)
{
    if (!limits_.verified_plan_capability_) {
        throw std::invalid_argument(
            "SpatialRoiBatchProducer requires limits minted from a verified plan");
    }
    if (limits_.verified_plan_capability_->limits_snapshot !=
        limits_snapshot(limits_)) {
        throw std::invalid_argument(
            "SpatialRoiBatchProducer limits were modified after verified-plan materialization");
    }
    std::string error;
    if (!validate_limits(limits_, &error)) {
        throw std::invalid_argument("Invalid spatial ROI batch limits: " + error);
    }
    std::shared_ptr<detail::SpatialRoiBatchPoolState> state =
        std::make_shared<detail::SpatialRoiBatchPoolState>(limits_);
    if (!state->Initialize(&error)) {
        throw std::runtime_error(
            "Could not initialize spatial ROI batch producer: " + error);
    }
    state_ = std::move(state);
}

SpatialRoiBatchProducer::~SpatialRoiBatchProducer()
{
    StopAccepting();
}

SpatialRoiBatchResult SpatialRoiBatchProducer::TryProduce(
    const SpatialRoiSourceView& source,
    const std::vector<SpatialRoiWorkItem>& work_items)
{
    auto failure = [](SpatialRoiBatchStatus status, std::string error) {
        SpatialRoiBatchResult result;
        result.status_ = status;
        result.source_release_safe_ =
            status != SpatialRoiBatchStatus::kSourceQuarantined;
        result.error_ = std::move(error);
        return result;
    };

    if (!state_ || !state_->accepting()) {
        return failure(
            SpatialRoiBatchStatus::kStopped,
            "spatial ROI batch producer is stopped");
    }

    std::string error;
    if (!validate_spatial_roi_batch(source, work_items, limits_, &error)) {
        return failure(SpatialRoiBatchStatus::kInvalidArgument, std::move(error));
    }

    // Copy identity/geometry before claiming a bounded device slot. Any host
    // allocation failure therefore cannot strand a pool entry.
    std::vector<SpatialRoiOutputView> outputs;
    outputs.reserve(work_items.size());
    for (const SpatialRoiWorkItem& item : work_items) {
        SpatialRoiOutputView output;
        output.work_item = item;
        outputs.push_back(std::move(output));
    }

    // Hold admission through slot acquisition and every CUDA enqueue. This
    // pairs with SpatialRoiBatchPoolState::StopAccepting so shutdown cannot
    // return between TryAcquire and the first enqueue.
    std::unique_lock<std::mutex> enqueue_lock(state_->enqueue_mutex());

    detail::SpatialRoiBatchPoolState::AcquiredSlot slot;
    SpatialRoiBatchStatus acquire_failure_status =
        SpatialRoiBatchStatus::kCudaError;
    if (!state_->TryAcquire(
            &slot, &acquire_failure_status, source.source_lease, &error)) {
        return failure(acquire_failure_status, std::move(error));
    }
    detail::AcquiredSlotCleanup acquired_slot_cleanup(state_.get(), slot);

    auto failure_after_acquire =
        [&](SpatialRoiBatchStatus status, std::string message) {
            const cudaError_t cleanup_status = acquired_slot_cleanup.cleanup_now();
            if (cleanup_status != cudaSuccess) {
                if (!message.empty()) {
                    message += "; ";
                }
                message += cuda_failure(
                    "cudaStreamSynchronize(spatial ROI failure cleanup)",
                    cleanup_status);
                message += "; source is quarantined and must not be released";
                status = SpatialRoiBatchStatus::kSourceQuarantined;
            }
            return failure(status, std::move(message));
        };

    cudaError_t status = cudaSetDevice(limits_.gpu_id);
    if (status != cudaSuccess) {
        error = cuda_failure("cudaSetDevice", status);
        return failure_after_acquire(
            SpatialRoiBatchStatus::kCudaError, std::move(error));
    }

    status = cudaStreamWaitEvent(state_->stream(), source.ready_event, 0);
    if (status != cudaSuccess) {
        error = cuda_failure("cudaStreamWaitEvent(spatial ROI source)", status);
        return failure_after_acquire(
            SpatialRoiBatchStatus::kCudaError, std::move(error));
    }

    for (std::size_t index = 0; index < work_items.size(); ++index) {
        const SpatialRoiOutputGeometry& geometry = work_items[index].geometry;
        const detail::SpatialRoiBatchPoolState::OutputStorage& output_storage =
            (*slot.outputs)[index];
        unsigned char* output_base = output_storage.device_data;
        const std::size_t output_pitch = geometry.encoded_raster.width;
        outputs[index].device_data = output_base;
        outputs[index].pitch_bytes = output_pitch;

        std::size_t output_capacity_bytes = 0;
        if (!checked_multiply(static_cast<std::size_t>(
                                  geometry.encoded_raster.width),
                              static_cast<std::size_t>(
                                  geometry.encoded_raster.height),
                              &output_capacity_bytes) ||
            !checked_pointer_offset(output_base,
                                    output_capacity_bytes,
                                    "encoded ROI allocation",
                                    &error)) {
            if (error.empty()) {
                error = "encoded ROI allocation pointer arithmetic overflows";
            }
            return failure_after_acquire(
                SpatialRoiBatchStatus::kInvalidArgument, std::move(error));
        }

        std::size_t encoded_content_offset = 0;
        if (!output_base ||
            !checked_output_row_span(geometry.encoded_raster,
                                     geometry.encoded_content_rect,
                                     &error)) {
            if (error.empty()) {
                error = "encoded ROI output arithmetic overflows";
            }
            return failure_after_acquire(
                SpatialRoiBatchStatus::kInvalidArgument, std::move(error));
        }
        void* encoded_content_pointer = nullptr;
        if (!checked_multiply(
                static_cast<std::size_t>(geometry.encoded_content_rect.y),
                output_pitch,
                &encoded_content_offset) ||
            !checked_add(encoded_content_offset,
                         static_cast<std::size_t>(
                             geometry.encoded_content_rect.x),
                         &encoded_content_offset) ||
            !checked_pointer_offset(output_base,
                                    encoded_content_offset,
                                    "encoded ROI destination",
                                    &error,
                                    &encoded_content_pointer)) {
            if (error.empty()) {
                error = "encoded ROI destination pointer arithmetic overflows";
            }
            return failure_after_acquire(
                SpatialRoiBatchStatus::kInvalidArgument, std::move(error));
        }
        unsigned char* encoded_destination =
            static_cast<unsigned char*>(encoded_content_pointer);

        // Zero the complete encoded raster every frame. This makes every byte
        // outside encoded_content_rect explicit padding, including optional
        // left/top placement as well as the contract's normal right/bottom
        // alignment padding.
        status = cudaMemset2DAsync(
            output_base,
            output_pitch,
            0,
            geometry.encoded_raster.width,
            geometry.encoded_raster.height,
            state_->stream());
        if (status != cudaSuccess) {
            error = cuda_failure("cudaMemset2DAsync(spatial ROI padding)", status);
            return failure_after_acquire(
                SpatialRoiBatchStatus::kCudaError, std::move(error));
        }

        std::size_t source_content_offset = 0;
        if (!checked_multiply(static_cast<std::size_t>(geometry.content_rect.y),
                              source.pitch_bytes,
                              &source_content_offset) ||
            !checked_add(source_content_offset,
                         static_cast<std::size_t>(geometry.content_rect.x),
                         &source_content_offset)) {
            error = "camera-native source pointer arithmetic overflows";
            return failure_after_acquire(
                SpatialRoiBatchStatus::kInvalidArgument, std::move(error));
        }
        const unsigned char* source_content = nullptr;
        if (!checked_const_pointer_offset(source.device_data,
                                          source_content_offset,
                                          "camera-native source",
                                          &error,
                                          &source_content)) {
            return failure_after_acquire(
                SpatialRoiBatchStatus::kInvalidArgument, std::move(error));
        }
        status = cudaMemcpy2DAsync(
            encoded_destination,
            output_pitch,
            source_content,
            source.pitch_bytes,
            geometry.content_rect.width,
            geometry.content_rect.height,
            cudaMemcpyDeviceToDevice,
            state_->stream());
        if (status != cudaSuccess) {
            error = cuda_failure("cudaMemcpy2DAsync(spatial ROI native copy)", status);
            return failure_after_acquire(
                SpatialRoiBatchStatus::kCudaError, std::move(error));
        }
    }

    status = cudaEventRecord(slot.completion_event, state_->stream());
    if (status != cudaSuccess) {
        error = cuda_failure("cudaEventRecord(spatial ROI batch complete)", status);
        return failure_after_acquire(
            SpatialRoiBatchStatus::kCudaError, std::move(error));
    }

    SpatialRoiBatchResult result;
    result.status_ = SpatialRoiBatchStatus::kAccepted;
    result.source_release_safe_ = false;
    result.batch_sequence_ = state_->next_batch_sequence();
    result.completion_event_ = slot.completion_event;
    result.outputs_ = std::move(outputs);
    result.source_lease_ = source.source_lease;
    result.state_ = state_;
    result.slot_index_ = slot.index;
    result.slot_generation_ = slot.generation;
    acquired_slot_cleanup.commit();
    return result;
}

void SpatialRoiBatchProducer::StopAccepting() noexcept
{
    if (state_) {
        state_->StopAccepting();
    }
}

void SpatialRoiBatchProducer::Quarantine() noexcept
{
    if (state_) {
        state_->Quarantine();
    }
}

std::size_t SpatialRoiBatchProducer::slot_capacity() const noexcept
{
    return state_ ? state_->slot_capacity() : 0;
}

std::size_t SpatialRoiBatchProducer::available_slot_count() const noexcept
{
    return state_ ? state_->available_slot_count() : 0;
}

std::size_t SpatialRoiBatchProducer::pending_slot_count() const noexcept
{
    return state_ ? state_->pending_slot_count() : 0;
}

}  // namespace orange::spatial_roi
