#include "aperture_characterization.h"
#include "fsuid_guard.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

#include <cuda_runtime.h>

namespace {

struct BrightnessAccumulator {
    std::array<unsigned long long, 256> bins{};
    unsigned long long pixel_count = 0;
    double sum = 0.0;
    double sum_sq = 0.0;
    unsigned int min_value = 255;
    unsigned int max_value = 0;
};

struct GridMeanAccumulator {
    unsigned int rows = 0;
    unsigned int cols = 0;
    std::vector<double> sum;
    std::vector<unsigned long long> pixel_count;
};

struct RepresentativeFrameWriteJob {
    std::string path;
    int pixel_type = 0;
    unsigned int width = 0;
    unsigned int height = 0;
    std::vector<unsigned char> bytes;
};

bool write_representative_frame_image_buffer(
    int pixel_type,
    unsigned int width,
    unsigned int height,
    const std::vector<unsigned char>& bytes,
    const std::string& path,
    std::string* error_out);

struct IrisVerificationResult {
    unsigned int requested = 0;
    EVT_ERROR set_error = EVT_SUCCESS;
    bool command_succeeded = false;
    EVT_ERROR read_error = EVT_SUCCESS;
    bool has_readback = false;
    unsigned int readback = 0;
    bool matches_requested = false;
};

struct FrameSequenceTracker {
    bool has_last_frame = false;
    unsigned int last_frame_id = 0;
    unsigned long long last_timestamp = 0;
};

class RepresentativeFrameWriter {
public:
    RepresentativeFrameWriter()
        : worker_(&RepresentativeFrameWriter::worker_loop, this)
    {
    }

    ~RepresentativeFrameWriter()
    {
        try {
            finish();
        } catch (...) {
        }
    }

    void enqueue(RepresentativeFrameWriteJob job)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_.push_back(std::move(job));
        cv_.notify_one();
    }

    void finish()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            done_ = true;
        }
        cv_.notify_one();
        if (worker_.joinable()) {
            worker_.join();
        }
        if (!error_message_.empty()) {
            throw std::runtime_error(error_message_);
        }
    }

private:
    void worker_loop()
    {
        while (true) {
            RepresentativeFrameWriteJob job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this]() { return done_ || !jobs_.empty(); });
                if (jobs_.empty()) {
                    if (done_) {
                        return;
                    }
                    continue;
                }
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }

            std::string error;
            if (!write_representative_frame_image_buffer(
                    job.pixel_type,
                    job.width,
                    job.height,
                    job.bytes,
                    job.path,
                    &error)) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (error_message_.empty()) {
                    error_message_ = error;
                }
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<RepresentativeFrameWriteJob> jobs_;
    std::thread worker_;
    bool done_ = false;
    std::string error_message_;
};

void log_aperture_message(const CameraParams* camera_params, const std::string& message)
{
    const char* camera_serial = camera_params != nullptr ? camera_params->camera_serial.c_str() : "unknown";
    std::cout << camera_serial << " [aperture_characterization] " << message << std::endl;
}

template <typename Func>
void run_aperture_phase(const CameraParams* camera_params, const char* phase, Func&& func)
{
    log_aperture_message(camera_params, std::string("Begin: ") + phase);
    try {
        func();
        log_aperture_message(camera_params, std::string("Done: ") + phase);
    } catch (const std::exception& ex) {
        std::ostringstream oss;
        oss << "Aperture characterization failed during " << phase
            << " for camera " << (camera_params != nullptr ? camera_params->camera_serial : "unknown")
            << ": " << ex.what();
        throw std::runtime_error(oss.str());
    } catch (...) {
        std::ostringstream oss;
        oss << "Aperture characterization failed during " << phase
            << " for camera " << (camera_params != nullptr ? camera_params->camera_serial : "unknown")
            << ": unknown error";
        throw std::runtime_error(oss.str());
    }
}

class ScopedCudaDevice {
public:
    explicit ScopedCudaDevice(int target_device)
    {
        cudaError_t get_err = cudaGetDevice(&previous_device_);
        had_previous_device_ = (get_err == cudaSuccess);
        if (target_device >= 0 && (!had_previous_device_ || previous_device_ != target_device)) {
            const cudaError_t set_err = cudaSetDevice(target_device);
            if (set_err != cudaSuccess) {
                std::ostringstream oss;
                oss << "cudaSetDevice(" << target_device << ") failed: " << cudaGetErrorString(set_err);
                throw std::runtime_error(oss.str());
            }
            switched_ = true;
        }
    }

    ~ScopedCudaDevice()
    {
        if (switched_ && had_previous_device_) {
            cudaSetDevice(previous_device_);
        }
    }

private:
    int previous_device_ = -1;
    bool had_previous_device_ = false;
    bool switched_ = false;
};

size_t representative_frame_byte_count(int pixel_type, unsigned int width, unsigned int height);

unsigned int percentile_from_bins(const std::array<unsigned long long, 256>& bins, unsigned long long pixel_count, double q)
{
    if (pixel_count == 0) {
        return 0;
    }
    unsigned long long target = static_cast<unsigned long long>(std::ceil(q * static_cast<double>(pixel_count)));
    if (target == 0) {
        target = 1;
    }
    unsigned long long running = 0;
    for (size_t i = 0; i < bins.size(); ++i) {
        running += bins[i];
        if (running >= target) {
            return static_cast<unsigned int>(i);
        }
    }
    return 255;
}

FrameBrightnessStats finalize_stats(const BrightnessAccumulator& acc)
{
    FrameBrightnessStats stats;
    stats.pixel_count = acc.pixel_count;
    if (acc.pixel_count == 0) {
        return stats;
    }

    const double inv_count = 1.0 / static_cast<double>(acc.pixel_count);
    stats.mean = acc.sum * inv_count;
    const double variance = std::max(0.0, (acc.sum_sq * inv_count) - (stats.mean * stats.mean));
    stats.stddev = std::sqrt(variance);
    stats.min_value = acc.min_value;
    stats.max_value = acc.max_value;
    stats.black_fraction = static_cast<double>(acc.bins[0]) * inv_count;
    stats.white_fraction = static_cast<double>(acc.bins[255]) * inv_count;
    stats.p05 = percentile_from_bins(acc.bins, acc.pixel_count, 0.05);
    stats.median = percentile_from_bins(acc.bins, acc.pixel_count, 0.50);
    stats.p95 = percentile_from_bins(acc.bins, acc.pixel_count, 0.95);
    stats.p99 = percentile_from_bins(acc.bins, acc.pixel_count, 0.99);
    return stats;
}

void init_grid_mean_accumulator(unsigned int rows, unsigned int cols, GridMeanAccumulator* acc)
{
    acc->rows = rows;
    acc->cols = cols;
    acc->sum.assign(static_cast<size_t>(rows) * static_cast<size_t>(cols), 0.0);
    acc->pixel_count.assign(static_cast<size_t>(rows) * static_cast<size_t>(cols), 0);
}

unsigned int read_pixel_brightness(int pixel_type, const unsigned char* data, unsigned long long pixel_index)
{
    switch (pixel_type) {
        case GVSP_PIX_MONO8:
        case GVSP_PIX_BAYRG8:
        case GVSP_PIX_BAYGB8:
            return data[pixel_index];
        case GVSP_PIX_RGB8:
        case GVSP_PIX_BGR8: {
            const unsigned long long base = pixel_index * 3ULL;
            return static_cast<unsigned int>(data[base] + data[base + 1] + data[base + 2]) / 3U;
        }
        default:
            return 0;
    }
}

bool pixel_type_supported_for_brightness(int pixel_type)
{
    switch (pixel_type) {
        case GVSP_PIX_MONO8:
        case GVSP_PIX_BAYRG8:
        case GVSP_PIX_BAYGB8:
        case GVSP_PIX_RGB8:
        case GVSP_PIX_BGR8:
            return true;
        default:
            return false;
    }
}

BrightnessGridStats finalize_grid_stats(const GridMeanAccumulator& acc, double global_mean)
{
    BrightnessGridStats stats;
    stats.rows = acc.rows;
    stats.cols = acc.cols;
    const size_t cell_count = static_cast<size_t>(acc.rows) * static_cast<size_t>(acc.cols);
    stats.tile_mean.resize(cell_count, 0.0);
    stats.tile_relative_mean.resize(cell_count, 0.0);
    if (cell_count == 0) {
        return stats;
    }

    double rel_sum = 0.0;
    double rel_sum_sq = 0.0;
    bool has_relative = false;
    for (size_t i = 0; i < cell_count; ++i) {
        if (acc.pixel_count[i] > 0) {
            stats.tile_mean[i] = acc.sum[i] / static_cast<double>(acc.pixel_count[i]);
        }
        if (global_mean > 0.0) {
            stats.tile_relative_mean[i] = stats.tile_mean[i] / global_mean;
            rel_sum += stats.tile_relative_mean[i];
            rel_sum_sq += stats.tile_relative_mean[i] * stats.tile_relative_mean[i];
            if (!has_relative) {
                stats.min_relative_mean = stats.tile_relative_mean[i];
                stats.max_relative_mean = stats.tile_relative_mean[i];
                has_relative = true;
            } else {
                stats.min_relative_mean = std::min(stats.min_relative_mean, stats.tile_relative_mean[i]);
                stats.max_relative_mean = std::max(stats.max_relative_mean, stats.tile_relative_mean[i]);
            }
        }
    }

    if (has_relative) {
        const double inv_count = 1.0 / static_cast<double>(cell_count);
        const double rel_mean = rel_sum * inv_count;
        const double rel_var = std::max(0.0, (rel_sum_sq * inv_count) - (rel_mean * rel_mean));
        stats.cv_relative_mean = rel_mean > 0.0 ? std::sqrt(rel_var) / rel_mean : 0.0;
    }

    return stats;
}

void accumulate_value(BrightnessAccumulator* acc, unsigned int value)
{
    acc->bins[value] += 1;
    acc->pixel_count += 1;
    acc->sum += static_cast<double>(value);
    acc->sum_sq += static_cast<double>(value) * static_cast<double>(value);
    acc->min_value = std::min(acc->min_value, value);
    acc->max_value = std::max(acc->max_value, value);
}

bool accumulate_frame_pixels(
    int pixel_type,
    unsigned int width,
    unsigned int height,
    const unsigned char* data,
    BrightnessAccumulator* acc)
{
    if (data == nullptr || width == 0 || height == 0) {
        return false;
    }

    const unsigned long long pixel_count =
        static_cast<unsigned long long>(width) * static_cast<unsigned long long>(height);

    if (!pixel_type_supported_for_brightness(pixel_type)) {
        return false;
    }

    for (unsigned long long i = 0; i < pixel_count; ++i) {
        accumulate_value(acc, read_pixel_brightness(pixel_type, data, i));
    }
    return true;
}

bool accumulate_frame_grid_means(
    int pixel_type,
    unsigned int width,
    unsigned int height,
    const unsigned char* data,
    GridMeanAccumulator* acc)
{
    if (data == nullptr || width == 0 || height == 0 || acc == nullptr || acc->rows == 0 || acc->cols == 0) {
        return false;
    }
    if (!pixel_type_supported_for_brightness(pixel_type)) {
        return false;
    }

    for (unsigned int row = 0; row < height; ++row) {
        const unsigned int tile_row =
            std::min(acc->rows - 1U, static_cast<unsigned int>((static_cast<unsigned long long>(row) * acc->rows) / height));
        for (unsigned int col = 0; col < width; ++col) {
            const unsigned int tile_col =
                std::min(acc->cols - 1U, static_cast<unsigned int>((static_cast<unsigned long long>(col) * acc->cols) / width));
            const size_t tile_index = static_cast<size_t>(tile_row) * acc->cols + tile_col;
            const unsigned long long pixel_index =
                static_cast<unsigned long long>(row) * static_cast<unsigned long long>(width) + col;
            acc->sum[tile_index] += static_cast<double>(read_pixel_brightness(pixel_type, data, pixel_index));
            acc->pixel_count[tile_index] += 1ULL;
        }
    }

    return true;
}

bool extract_frame_host_bytes(
    const Emergent::CEmergentFrame& frame,
    const CameraParams* camera_params,
    std::vector<unsigned char>* host_bytes,
    const unsigned char** data_out,
    std::string* error_out)
{
    if (data_out == nullptr) {
        if (error_out) {
            *error_out = "extract_frame_host_bytes requires a non-null data_out pointer.";
        }
        return false;
    }
    if (frame.imagePtr == nullptr || frame.size_x == 0 || frame.size_y == 0) {
        if (error_out) {
            *error_out = "Frame image pointer is null or dimensions are zero.";
        }
        return false;
    }

    cudaPointerAttributes attrs{};
    const cudaError_t attr_status = cudaPointerGetAttributes(&attrs, frame.imagePtr);
    if (attr_status == cudaSuccess && attrs.type == cudaMemoryTypeDevice) {
        const size_t byte_count = representative_frame_byte_count(frame.pixel_type, frame.size_x, frame.size_y);
        if (byte_count == 0) {
            if (error_out) {
                *error_out = "Unsupported pixel format for host extraction.";
            }
            return false;
        }
        try {
            ScopedCudaDevice guard(attrs.device);
            host_bytes->resize(byte_count);
            const cudaError_t copy_err =
                cudaMemcpy(host_bytes->data(), frame.imagePtr, byte_count, cudaMemcpyDeviceToHost);
            if (copy_err != cudaSuccess) {
                if (error_out) {
                    std::ostringstream oss;
                    oss << "cudaMemcpy(DeviceToHost) failed: " << cudaGetErrorString(copy_err);
                    *error_out = oss.str();
                }
                return false;
            }
        } catch (const std::exception& ex) {
            if (error_out) {
                *error_out = ex.what();
            }
            return false;
        }
        *data_out = host_bytes->data();
        return true;
    }

    if (attr_status != cudaSuccess) {
        cudaGetLastError();
    }

    host_bytes->clear();
    *data_out = static_cast<const unsigned char*>(frame.imagePtr);
    (void)camera_params;
    return true;
}

BrightnessAccumulator add_accumulators(const BrightnessAccumulator& lhs, const BrightnessAccumulator& rhs)
{
    BrightnessAccumulator out;
    out.pixel_count = lhs.pixel_count + rhs.pixel_count;
    out.sum = lhs.sum + rhs.sum;
    out.sum_sq = lhs.sum_sq + rhs.sum_sq;
    out.min_value = std::min(lhs.min_value, rhs.min_value);
    out.max_value = std::max(lhs.max_value, rhs.max_value);
    for (size_t i = 0; i < out.bins.size(); ++i) {
        out.bins[i] = lhs.bins[i] + rhs.bins[i];
    }
    if (out.pixel_count == 0) {
        out.min_value = 0;
        out.max_value = 0;
    }
    return out;
}

const char* representative_frame_extension(int pixel_type)
{
    switch (pixel_type) {
        case GVSP_PIX_MONO8:
        case GVSP_PIX_BAYRG8:
        case GVSP_PIX_BAYGB8:
            return ".pgm";
        case GVSP_PIX_RGB8:
        case GVSP_PIX_BGR8:
            return ".ppm";
        default:
            return nullptr;
    }
}

size_t representative_frame_byte_count(int pixel_type, unsigned int width, unsigned int height)
{
    const size_t pixel_count = static_cast<size_t>(width) * static_cast<size_t>(height);
    switch (pixel_type) {
        case GVSP_PIX_MONO8:
        case GVSP_PIX_BAYRG8:
        case GVSP_PIX_BAYGB8:
            return pixel_count;
        case GVSP_PIX_RGB8:
        case GVSP_PIX_BGR8:
            return pixel_count * 3U;
        default:
            return 0;
    }
}

bool write_representative_frame_image_buffer(
    int pixel_type,
    unsigned int width,
    unsigned int height,
    const std::vector<unsigned char>& bytes,
    const std::string& path,
    std::string* error_out)
{
    if (width == 0 || height == 0) {
        if (error_out) {
            *error_out = "Representative frame is empty.";
        }
        return false;
    }

    const size_t expected_size = representative_frame_byte_count(pixel_type, width, height);
    if (expected_size == 0) {
        if (error_out) {
            *error_out = "Representative frame save does not support this pixel format.";
        }
        return false;
    }
    if (bytes.size() != expected_size) {
        if (error_out) {
            *error_out = "Representative frame buffer size does not match the requested format.";
        }
        return false;
    }

    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        if (error_out) {
            *error_out = "Failed to open representative frame path: " + path;
        }
        return false;
    }

    const unsigned char* data = bytes.data();
    const unsigned long long pixel_count =
        static_cast<unsigned long long>(width) * static_cast<unsigned long long>(height);

    switch (pixel_type) {
        case GVSP_PIX_MONO8:
        case GVSP_PIX_BAYRG8:
        case GVSP_PIX_BAYGB8: {
            out << "P5\n" << width << " " << height << "\n255\n";
            out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(pixel_count));
            return static_cast<bool>(out);
        }
        case GVSP_PIX_RGB8: {
            out << "P6\n" << width << " " << height << "\n255\n";
            out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(pixel_count * 3ULL));
            return static_cast<bool>(out);
        }
        case GVSP_PIX_BGR8: {
            out << "P6\n" << width << " " << height << "\n255\n";
            std::vector<unsigned char> rgb(pixel_count * 3ULL);
            for (unsigned long long i = 0; i < pixel_count; ++i) {
                const unsigned long long base = i * 3ULL;
                rgb[base] = data[base + 2];
                rgb[base + 1] = data[base + 1];
                rgb[base + 2] = data[base];
            }
            out.write(reinterpret_cast<const char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
            return static_cast<bool>(out);
        }
        default:
            if (error_out) {
                *error_out = "Representative frame save does not support this pixel format.";
            }
            return false;
    }
}

IrisVerificationResult set_iris_and_readback(
    Emergent::CEmergentCamera* camera,
    CameraParams* camera_params,
    unsigned int iris_value)
{
    IrisVerificationResult result;
    result.requested = iris_value;
    result.set_error = EVT_CameraSetUInt32Param(camera, "Iris", iris_value);
    result.command_succeeded = (result.set_error == EVT_SUCCESS);
    if (!result.command_succeeded) {
        return result;
    }

    result.read_error = EVT_CameraGetUInt32Param(camera, "Iris", &result.readback);
    result.has_readback = (result.read_error == EVT_SUCCESS);
    result.matches_requested = result.has_readback && result.readback == iris_value;

    if (result.matches_requested) {
        camera_params->iris = iris_value;
    }
    return result;
}

IrisVerificationResult read_iris_value(
    Emergent::CEmergentCamera* camera,
    unsigned int expected_iris)
{
    IrisVerificationResult result;
    result.requested = expected_iris;
    result.command_succeeded = true;
    result.read_error = EVT_CameraGetUInt32Param(camera, "Iris", &result.readback);
    result.has_readback = (result.read_error == EVT_SUCCESS);
    result.matches_requested = result.has_readback && result.readback == expected_iris;
    return result;
}

std::string describe_iris_verification_result(
    const IrisVerificationResult& result,
    bool include_set_error)
{
    std::ostringstream oss;
    oss << "target=" << result.requested;
    if (include_set_error) {
        oss << " set=" << (result.command_succeeded ? "ok" : get_evt_error_string(result.set_error));
    }
    if (result.has_readback) {
        oss << " readback=" << result.readback;
    } else {
        oss << " readback_error=" << get_evt_error_string(result.read_error);
    }
    oss << " match=" << (result.matches_requested ? "yes" : "no");
    return oss.str();
}

void validate_frame_progression(
    const Emergent::CEmergentFrame& frame,
    FrameSequenceTracker* tracker,
    const CameraParams* camera_params,
    unsigned int iris_value,
    const char* phase,
    size_t step_index,
    size_t total_steps)
{
    if (tracker == nullptr) {
        return;
    }
    if (!tracker->has_last_frame) {
        tracker->has_last_frame = true;
        tracker->last_frame_id = frame.frame_id;
        tracker->last_timestamp = frame.timestamp;
        return;
    }

    if (frame.frame_id <= tracker->last_frame_id) {
        std::ostringstream oss;
        oss << "Non-monotonic frame_id during " << phase
            << " at iris " << iris_value
            << " step " << step_index << "/" << total_steps
            << ": current=" << frame.frame_id
            << " previous=" << tracker->last_frame_id;
        throw std::runtime_error(oss.str());
    }

    if (frame.timestamp <= tracker->last_timestamp) {
        std::ostringstream oss;
        oss << "Non-monotonic timestamp during " << phase
            << " at iris " << iris_value
            << " step " << step_index << "/" << total_steps
            << ": current=" << frame.timestamp
            << " previous=" << tracker->last_timestamp;
        throw std::runtime_error(oss.str());
    }

    tracker->last_frame_id = frame.frame_id;
    tracker->last_timestamp = frame.timestamp;
    (void)camera_params;
}

bool grab_one_frame(
    Emergent::CEmergentCamera* camera,
    Emergent::CEmergentFrame* frame,
    CameraParams* camera_params,
    unsigned int timeout_ms)
{
    EVT_ERROR err = EVT_CameraGetFrame(camera, frame, timeout_ms);
    (void)camera_params;
    return err == EVT_SUCCESS;
}

void requeue_frame_or_throw(
    Emergent::CEmergentCamera* camera,
    Emergent::CEmergentFrame* frame,
    CameraParams* camera_params)
{
    EVT_ERROR requeue_err = EVT_CameraQueueFrame(camera, frame);
    if (requeue_err != EVT_SUCCESS) {
        std::ostringstream oss;
        oss << camera_params->camera_serial << " failed to requeue frame: " << get_evt_error_string(requeue_err);
        throw std::runtime_error(oss.str());
    }
}

}  // namespace

FrameBrightnessStats compute_frame_brightness_stats(const Emergent::CEmergentFrame& frame)
{
    std::vector<unsigned char> host_frame_bytes;
    const unsigned char* frame_bytes = nullptr;
    std::string host_extract_error;
    if (!extract_frame_host_bytes(frame, nullptr, &host_frame_bytes, &frame_bytes, &host_extract_error)) {
        return FrameBrightnessStats{};
    }
    BrightnessAccumulator acc;
    if (!accumulate_frame_pixels(frame.pixel_type, frame.size_x, frame.size_y, frame_bytes, &acc)) {
        return FrameBrightnessStats{};
    }
    return finalize_stats(acc);
}

ApertureClassification classify_aperture_step(
    const FrameBrightnessStats& stats,
    const ApertureCharacterizationThresholds& thresholds)
{
    if (stats.pixel_count == 0) {
        return ApertureClassification::kUnsupportedPixelFormat;
    }
    if (stats.white_fraction >= thresholds.saturated_white_fraction || stats.p99 >= thresholds.saturated_p99_min) {
        return ApertureClassification::kSaturated;
    }
    if (stats.mean <= thresholds.dim_mean_max ||
        stats.p95 <= thresholds.dim_p95_max ||
        stats.black_fraction >= thresholds.dim_black_fraction_min) {
        return ApertureClassification::kTooDim;
    }
    return ApertureClassification::kUsable;
}

const char* aperture_classification_to_string(ApertureClassification classification)
{
    switch (classification) {
        case ApertureClassification::kUsable:
            return "usable";
        case ApertureClassification::kSaturated:
            return "saturated";
        case ApertureClassification::kTooDim:
            return "too_dim";
        case ApertureClassification::kUnsupportedPixelFormat:
        default:
            return "unsupported_pixel_format";
    }
}

std::vector<unsigned int> build_iris_sweep(
    unsigned int iris_min,
    unsigned int iris_max,
    unsigned int iris_inc,
    unsigned int step_multiplier)
{
    std::vector<unsigned int> iris_values;
    if (iris_max < iris_min) {
        return iris_values;
    }

    const unsigned int effective_inc = std::max(1U, iris_inc == 0 ? 1U : iris_inc) * std::max(1U, step_multiplier);
    for (unsigned int iris = iris_min; iris <= iris_max; iris += effective_inc) {
        iris_values.push_back(iris);
        if (iris_max - iris < effective_inc) {
            break;
        }
    }
    if (iris_values.empty() || iris_values.back() != iris_max) {
        iris_values.push_back(iris_max);
    }
    return iris_values;
}

namespace {

std::string sanitize_artifact_token(const std::string& value)
{
    std::string sanitized;
    sanitized.reserve(value.size());
    for (unsigned char c : value) {
        if (std::isalnum(c) != 0 || c == '_' || c == '-') {
            sanitized.push_back(static_cast<char>(c));
        } else {
            sanitized.push_back('_');
        }
    }

    while (!sanitized.empty() && sanitized.front() == '_') {
        sanitized.erase(sanitized.begin());
    }
    while (!sanitized.empty() && sanitized.back() == '_') {
        sanitized.pop_back();
    }

    if (sanitized.empty()) {
        sanitized = "artifact";
    }
    return sanitized;
}

std::string artifact_relative_path(const std::string& artifact_dir, const std::string& path)
{
    if (artifact_dir.empty() || path.empty()) {
        return path;
    }

    const std::filesystem::path relative =
        std::filesystem::path(path).lexically_relative(std::filesystem::path(artifact_dir));
    if (!relative.empty() && relative.native().rfind("..", 0) != 0) {
        return relative.generic_string();
    }
    return path;
}

constexpr uint64_t kFnv1a64OffsetBasis = 14695981039346656037ULL;
constexpr uint64_t kFnv1a64Prime = 1099511628211ULL;

void fnv1a64_update_bytes(uint64_t* hash, const void* data, size_t size)
{
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) {
        *hash ^= static_cast<uint64_t>(bytes[i]);
        *hash *= kFnv1a64Prime;
    }
}

void fnv1a64_update_string(uint64_t* hash, const std::string& value)
{
    const uint64_t length = static_cast<uint64_t>(value.size());
    fnv1a64_update_bytes(hash, &length, sizeof(length));
    if (!value.empty()) {
        fnv1a64_update_bytes(hash, value.data(), value.size());
    }
}

bool fnv1a64_update_file(uint64_t* hash, const std::filesystem::path& path, std::string* error_out)
{
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        if (error_out) {
            *error_out = "Failed to open representative frame for fingerprint: " + path.string();
        }
        return false;
    }

    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0) {
            fnv1a64_update_bytes(hash, buffer.data(), static_cast<size_t>(count));
        }
    }

    if (!input.eof()) {
        if (error_out) {
            *error_out = "Failed to read representative frame for fingerprint: " + path.string();
        }
        return false;
    }

    return true;
}

std::string format_fnv1a64(uint64_t hash)
{
    std::ostringstream oss;
    oss << kCalibrationFingerprintAlgorithm << ":"
        << std::hex << std::setfill('0') << std::setw(16) << std::nouppercase << hash;
    return oss.str();
}

} // namespace

std::string compute_aperture_characterization_fingerprint(
    const nlohmann::json& measurement_json,
    const ApertureCharacterizationArtifactPaths& paths,
    std::string* error_out)
{
    nlohmann::json fingerprint_payload = measurement_json;
    fingerprint_payload.erase("artifact_id");
    fingerprint_payload.erase("created_utc");
    fingerprint_payload.erase("calibration_ref");

    if (fingerprint_payload.contains("request") && fingerprint_payload["request"].is_object()) {
        fingerprint_payload["request"].erase("representative_frame_prefix");
        if (fingerprint_payload["request"].contains("representative_frame_dir")) {
            fingerprint_payload["request"]["representative_frame_dir"] =
                std::filesystem::path(paths.representative_frames_dir).filename().generic_string();
        }
        if (fingerprint_payload["request"].contains("camera_config_snapshot") &&
            fingerprint_payload["request"]["camera_config_snapshot"].is_object()) {
            fingerprint_payload["request"]["camera_config_snapshot"]["source_path"] = "";
            if (fingerprint_payload["request"]["camera_config_snapshot"].contains("snapshot_path")) {
                fingerprint_payload["request"]["camera_config_snapshot"]["snapshot_path"] =
                    std::filesystem::path(paths.camera_config_snapshot_path).filename().generic_string();
            }
        }
    }

    uint64_t hash = kFnv1a64OffsetBasis;
    const std::string measurement_dump = fingerprint_payload.dump();
    fnv1a64_update_string(&hash, measurement_dump);

    const auto hash_directory = [&](const std::filesystem::path& directory) -> bool {
        if (!std::filesystem::exists(directory)) {
            return true;
        }

        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            if (entry.is_regular_file()) {
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());

        for (const std::filesystem::path& file : files) {
            const std::string relative_path = artifact_relative_path(paths.artifact_dir, file.string());
            fnv1a64_update_string(&hash, relative_path);
            if (!fnv1a64_update_file(&hash, file, error_out)) {
                return false;
            }
        }
        return true;
    };

    if (!hash_directory(std::filesystem::path(paths.representative_frames_dir)) ||
        !hash_directory(std::filesystem::path(paths.fov_reference_frames_dir))) {
        return {};
    }

    if (std::filesystem::exists(paths.camera_config_snapshot_path)) {
        const std::string relative_path =
            artifact_relative_path(paths.artifact_dir, paths.camera_config_snapshot_path);
        fnv1a64_update_string(&hash, relative_path);
        if (!fnv1a64_update_file(&hash, std::filesystem::path(paths.camera_config_snapshot_path), error_out)) {
            return {};
        }
    }

    return format_fnv1a64(hash);
}

std::string build_aperture_characterization_artifact_id(
    const std::string& prefix_base,
    const CameraParams& camera_params,
    const std::string& timestamp_label)
{
    std::ostringstream oss;
    oss << "aperturecal_"
        << sanitize_artifact_token(prefix_base)
        << "_"
        << sanitize_artifact_token(timestamp_label)
        << "_Cam" << sanitize_artifact_token(camera_params.camera_serial)
        << "_exp" << camera_params.exposure
        << "_focus" << camera_params.focus;
    return oss.str();
}

ApertureCharacterizationArtifactPaths make_aperture_characterization_artifact_paths(
    const std::string& artifact_root_dir,
    const std::string& artifact_id)
{
    const std::filesystem::path artifact_dir = std::filesystem::path(artifact_root_dir) / artifact_id;

    ApertureCharacterizationArtifactPaths paths;
    paths.artifact_id = artifact_id;
    paths.artifact_dir = artifact_dir.string();
    paths.manifest_path = (artifact_dir / "manifest.json").string();
    paths.measurement_json_path = (artifact_dir / "measurement.json").string();
    paths.steps_csv_path = (artifact_dir / "steps.csv").string();
    paths.frames_csv_path = (artifact_dir / "frames.csv").string();
    paths.camera_config_snapshot_path = (artifact_dir / "camera_config_snapshot.json").string();
    paths.representative_frames_dir = (artifact_dir / "representative_frames").string();
    paths.fov_reference_frames_dir = (artifact_dir / "fov_reference_frames").string();
    paths.fov_horizontal_capture_path = (artifact_dir / "fov_reference_frames" / "horizontal_ruler.ppm").string();
    paths.fov_vertical_capture_path = (artifact_dir / "fov_reference_frames" / "vertical_ruler.ppm").string();
    return paths;
}

ApertureCharacterizationResult characterize_aperture(
    Emergent::CEmergentCamera* camera,
    CameraParams* camera_params,
    const ApertureCharacterizationRequest& request)
{
    if (request.iris_values.empty()) {
        throw std::invalid_argument("Aperture characterization requires at least one iris value.");
    }
    if (request.frames_per_step == 0) {
        throw std::invalid_argument("frames_per_step must be greater than zero.");
    }
    if ((request.grid_rows == 0) != (request.grid_cols == 0)) {
        throw std::invalid_argument("grid_rows and grid_cols must both be zero or both be greater than zero.");
    }
    const bool grid_enabled = request.grid_rows > 0 && request.grid_cols > 0;

    {
        std::ostringstream oss;
        oss << "Starting run: iris_steps=" << request.iris_values.size()
            << " frames_per_step=" << request.frames_per_step
            << " settle_frames=" << request.settle_frames
            << " grab_timeout_ms=" << request.grab_timeout_ms
            << " grid=" << (grid_enabled ? (std::to_string(request.grid_rows) + "x" + std::to_string(request.grid_cols)) : "off")
            << " manage_acquisition=" << (request.manage_acquisition ? "true" : "false")
            << " save_representative_frames=" << (request.save_representative_frames ? "true" : "false");
        log_aperture_message(camera_params, oss.str());
    }

    ApertureCharacterizationResult result;
    run_aperture_phase(camera_params, "read original iris", [&]() {
        check_camera_errors(EVT_CameraGetUInt32Param(camera, "Iris", &result.original_iris), camera_params->camera_serial.c_str());
    });

    struct Cleanup {
        Emergent::CEmergentCamera* camera = nullptr;
        CameraParams* camera_params = nullptr;
        bool manage_acquisition = false;
        bool restore_original_iris = false;
        unsigned int original_iris = 0;
        ApertureCharacterizationResult* result = nullptr;

        ~Cleanup()
        {
            if (manage_acquisition) {
                log_aperture_message(camera_params, "Begin cleanup: stop temporary acquisition");
                EVT_ERROR stop_err = EVT_CameraExecuteCommand(camera, "AcquisitionStop");
                if (stop_err == EVT_SUCCESS) {
                    result->acquisition_stopped = true;
                    log_aperture_message(camera_params, "Done cleanup: stop temporary acquisition");
                } else {
                    std::ostringstream oss;
                    oss << "Failed to stop acquisition cleanly: " << get_evt_error_string(stop_err);
                    result->warnings.push_back(oss.str());
                    log_aperture_message(camera_params, oss.str());
                }
            }
            if (restore_original_iris) {
                std::ostringstream oss;
                oss << "Begin cleanup: restore original iris=" << original_iris;
                log_aperture_message(camera_params, oss.str());
                const IrisVerificationResult restore_result =
                    set_iris_and_readback(camera, camera_params, original_iris);
                if (restore_result.matches_requested) {
                    result->restored_original_iris = true;
                    log_aperture_message(camera_params, "Done cleanup: restore original iris");
                } else {
                    std::ostringstream warn;
                    warn << "Failed to restore original iris value: "
                         << describe_iris_verification_result(restore_result, true);
                    result->warnings.push_back(warn.str());
                    log_aperture_message(camera_params, warn.str());
                }
            }
        }
    } cleanup{camera, camera_params, false, request.restore_original_iris, result.original_iris, &result};

    if (request.manage_acquisition) {
        run_aperture_phase(camera_params, "start temporary acquisition", [&]() {
            check_camera_errors(
                EVT_CameraExecuteCommand(camera, "AcquisitionStart"),
                camera_params->camera_serial.c_str());
        });
        result.acquisition_started = true;
        cleanup.manage_acquisition = true;
    }

    if (request.save_representative_frames && !request.representative_frame_dir.empty()) {
        orange::ScopedFsuid fsuid_guard;
        (void)fsuid_guard;
        std::filesystem::create_directories(request.representative_frame_dir);
        log_aperture_message(
            camera_params,
            std::string("Representative frames will be written under ") + request.representative_frame_dir);
    }
    std::unique_ptr<RepresentativeFrameWriter> representative_frame_writer;
    if (request.save_representative_frames) {
        representative_frame_writer = std::make_unique<RepresentativeFrameWriter>();
    }

    FrameSequenceTracker frame_sequence_tracker;
    for (size_t step_index = 0; step_index < request.iris_values.size(); ++step_index) {
        const unsigned int iris_value = request.iris_values[step_index];
        {
            std::ostringstream oss;
            oss << "Capture step " << (step_index + 1) << "/" << request.iris_values.size()
                << ": iris=" << iris_value;
            log_aperture_message(camera_params, oss.str());
        }
        const IrisVerificationResult set_result = set_iris_and_readback(camera, camera_params, iris_value);
        if (!set_result.command_succeeded || !set_result.matches_requested) {
            std::ostringstream oss;
            oss << "Failed to set iris for capture step "
                << (step_index + 1) << "/" << request.iris_values.size()
                << ": " << describe_iris_verification_result(set_result, true);
            throw std::runtime_error(oss.str());
        }

        Emergent::CEmergentFrame frame{};
        for (unsigned int settle_index = 0; settle_index < request.settle_frames; ++settle_index) {
            if (!grab_one_frame(camera, &frame, camera_params, request.grab_timeout_ms)) {
                std::ostringstream oss;
                oss << "Failed to grab settle frame at iris " << iris_value;
                throw std::runtime_error(oss.str());
            }
            validate_frame_progression(
                frame,
                &frame_sequence_tracker,
                camera_params,
                iris_value,
                "settle capture",
                step_index + 1,
                request.iris_values.size());
            requeue_frame_or_throw(camera, &frame, camera_params);
        }

        ApertureStepResult step;
        step.iris = iris_value;
        step.iris_command_succeeded = set_result.command_succeeded;
        step.has_iris_readback_after_set = set_result.has_readback;
        step.iris_readback_after_set = set_result.readback;
        step.iris_verified_after_set = set_result.matches_requested;
        BrightnessAccumulator step_acc;
        GridMeanAccumulator step_grid_acc;
        if (grid_enabled) {
            init_grid_mean_accumulator(request.grid_rows, request.grid_cols, &step_grid_acc);
        }
        for (unsigned int capture_index = 0; capture_index < request.frames_per_step; ++capture_index) {
            if (!grab_one_frame(camera, &frame, camera_params, request.grab_timeout_ms)) {
                std::ostringstream oss;
                oss << "Failed to grab measurement frame at iris " << iris_value;
                throw std::runtime_error(oss.str());
            }
            validate_frame_progression(
                frame,
                &frame_sequence_tracker,
                camera_params,
                iris_value,
                "measurement capture",
                step_index + 1,
                request.iris_values.size());

            std::vector<unsigned char> host_frame_bytes;
            const unsigned char* frame_bytes = nullptr;
            std::string host_extract_error;
            if (!extract_frame_host_bytes(frame, camera_params, &host_frame_bytes, &frame_bytes, &host_extract_error)) {
                requeue_frame_or_throw(camera, &frame, camera_params);
                std::ostringstream oss;
                oss << "Failed to access frame bytes at iris " << iris_value << ": " << host_extract_error;
                throw std::runtime_error(oss.str());
            }

            BrightnessAccumulator frame_acc;
            if (!accumulate_frame_pixels(frame.pixel_type, frame.size_x, frame.size_y, frame_bytes, &frame_acc)) {
                requeue_frame_or_throw(camera, &frame, camera_params);
                std::ostringstream oss;
                oss << "Unsupported pixel format while measuring iris " << iris_value;
                throw std::runtime_error(oss.str());
            }
            if (grid_enabled &&
                !accumulate_frame_grid_means(frame.pixel_type, frame.size_x, frame.size_y, frame_bytes, &step_grid_acc)) {
                requeue_frame_or_throw(camera, &frame, camera_params);
                std::ostringstream oss;
                oss << "Failed to compute brightness grid while measuring iris " << iris_value;
                throw std::runtime_error(oss.str());
            }

            ApertureFrameSample sample;
            sample.capture_index = capture_index;
            sample.frame_id = frame.frame_id;
            sample.timestamp = frame.timestamp;
            sample.stats = finalize_stats(frame_acc);
            step.samples.push_back(sample);
            step_acc = add_accumulators(step_acc, frame_acc);

            const bool should_save_representative_frame =
                request.save_representative_frames &&
                !step.has_representative_frame &&
                capture_index + 1 == request.frames_per_step;
            if (should_save_representative_frame) {
                const char* extension = representative_frame_extension(frame.pixel_type);
                if (extension == nullptr) {
                    requeue_frame_or_throw(camera, &frame, camera_params);
                    std::ostringstream oss;
                    oss << "Representative frame save does not support pixel type " << frame.pixel_type;
                    throw std::runtime_error(oss.str());
                }

                std::ostringstream file_name;
                file_name << request.representative_frame_prefix
                          << "_iris-" << iris_value
                          << "_frame-" << frame.frame_id
                          << extension;
                const std::filesystem::path frame_path =
                    std::filesystem::path(request.representative_frame_dir) / file_name.str();

                const size_t byte_count =
                    representative_frame_byte_count(frame.pixel_type, frame.size_x, frame.size_y);
                if (byte_count == 0) {
                    requeue_frame_or_throw(camera, &frame, camera_params);
                    throw std::runtime_error("Representative frame save does not support this pixel format.");
                }
                RepresentativeFrameWriteJob job;
                job.path = frame_path.string();
                job.pixel_type = frame.pixel_type;
                job.width = frame.size_x;
                job.height = frame.size_y;
                job.bytes.resize(byte_count);
                std::memcpy(job.bytes.data(), frame_bytes, byte_count);
                representative_frame_writer->enqueue(std::move(job));
                step.has_representative_frame = true;
                step.representative_frame_path = frame_path.string();
            }
            requeue_frame_or_throw(camera, &frame, camera_params);
        }

        const IrisVerificationResult verify_result = read_iris_value(camera, iris_value);
        step.has_iris_readback_after_capture = verify_result.has_readback;
        step.iris_readback_after_capture = verify_result.readback;
        step.iris_verified_after_capture = verify_result.matches_requested;
        if (!verify_result.matches_requested) {
            std::ostringstream oss;
            oss << "Iris drifted or failed verification after capture step "
                << (step_index + 1) << "/" << request.iris_values.size()
                << ": " << describe_iris_verification_result(verify_result, false);
            throw std::runtime_error(oss.str());
        }

        step.summary = finalize_stats(step_acc);
        if (grid_enabled) {
            step.has_grid = true;
            step.grid = finalize_grid_stats(step_grid_acc, step.summary.mean);
        }
        step.classification = classify_aperture_step(step.summary, request.thresholds);
        {
            std::ostringstream oss;
            const unsigned int first_frame_id = step.samples.empty() ? 0U : step.samples.front().frame_id;
            const unsigned int last_frame_id = step.samples.empty() ? 0U : step.samples.back().frame_id;
            oss << "Completed iris=" << iris_value
                << " readback_after_set=" << step.iris_readback_after_set
                << " readback_after_capture=" << step.iris_readback_after_capture
                << " frame_ids=[" << first_frame_id << "," << last_frame_id << "]"
                << " mean=" << step.summary.mean
                << " p99=" << step.summary.p99
                << " classification=" << aperture_classification_to_string(step.classification);
            log_aperture_message(camera_params, oss.str());
        }
        result.steps.push_back(step);
        if (request.progress_callback) {
            request.progress_callback(step_index + 1, request.iris_values.size(), iris_value);
        }
    }

    size_t reference_index = 0;
    bool found_reference = false;
    if (request.has_reference_iris) {
        for (size_t i = 0; i < result.steps.size(); ++i) {
            if (result.steps[i].iris == request.reference_iris) {
                reference_index = i;
                found_reference = true;
                break;
            }
        }
        if (!found_reference) {
            std::ostringstream oss;
            oss << "Requested reference iris " << request.reference_iris << " was not captured.";
            result.warnings.push_back(oss.str());
        }
    }

    if (!found_reference) {
        for (size_t i = 0; i < result.steps.size(); ++i) {
            if (result.steps[i].classification == ApertureClassification::kUsable) {
                reference_index = i;
                found_reference = true;
                break;
            }
        }
    }
    if (!found_reference && !result.steps.empty()) {
        reference_index = 0;
        found_reference = true;
    }

    if (found_reference) {
        result.has_reference_iris = true;
        result.reference_iris = result.steps[reference_index].iris;
        result.reference_mean = result.steps[reference_index].summary.mean;
    }

    double effective_reference_f_number = 0.0;
    bool has_effective_reference_f_number = false;
    if (request.has_reference_f_number &&
        request.fov_calibration.enabled &&
        request.fov_calibration.has_effective_reference_f_number &&
        request.fov_calibration.effective_reference_f_number > 0.0) {
        effective_reference_f_number = request.fov_calibration.effective_reference_f_number;
        has_effective_reference_f_number = true;
    }

    for (ApertureStepResult& step : result.steps) {
        if (result.reference_mean > 0.0 && step.summary.mean > 0.0) {
            step.relative_mean = step.summary.mean / result.reference_mean;
            step.delta_ev = std::log2(result.reference_mean / step.summary.mean);
            if (request.has_reference_f_number) {
                step.has_estimated_f_number = true;
                step.estimated_f_number =
                    request.reference_f_number * std::sqrt(result.reference_mean / step.summary.mean);
            }
            if (has_effective_reference_f_number) {
                step.has_estimated_effective_f_number = true;
                step.estimated_effective_f_number =
                    effective_reference_f_number * std::sqrt(result.reference_mean / step.summary.mean);
            }
        }
    }

    for (const ApertureStepResult& step : result.steps) {
        if (step.classification == ApertureClassification::kUsable) {
            if (!result.has_usable_window) {
                result.has_usable_window = true;
                result.usable_iris_min = step.iris;
                result.usable_iris_max = step.iris;
            } else {
                result.usable_iris_max = step.iris;
            }
        } else if (step.classification == ApertureClassification::kSaturated) {
            if (!result.has_saturation_boundary || step.iris > result.saturation_limited_through_iris) {
                result.has_saturation_boundary = true;
                result.saturation_limited_through_iris = step.iris;
            }
        } else if (step.classification == ApertureClassification::kTooDim) {
            if (!result.has_dim_boundary || step.iris < result.dim_limited_from_iris) {
                result.has_dim_boundary = true;
                result.dim_limited_from_iris = step.iris;
            }
        }
    }

    if (result.has_reference_iris &&
        result.steps[reference_index].classification != ApertureClassification::kUsable) {
        std::ostringstream oss;
        oss << "Reference iris " << result.reference_iris
            << " is classified as "
            << aperture_classification_to_string(result.steps[reference_index].classification)
            << ".";
        result.warnings.push_back(oss.str());
    }

    if (representative_frame_writer) {
        run_aperture_phase(camera_params, "flush representative frame writer", [&]() {
            representative_frame_writer->finish();
        });
    }

    log_aperture_message(camera_params, "Run complete.");
    return result;
}

ApertureCharacterizationResult characterize_aperture_with_stream(
    Emergent::CEmergentCamera* camera,
    CameraParams* camera_params,
    const ApertureCharacterizationRequest& request,
    unsigned int frame_buffer_count)
{
    if (frame_buffer_count == 0) {
        throw std::invalid_argument("frame_buffer_count must be greater than zero.");
    }

    struct StreamCleanup {
        Emergent::CEmergentCamera* camera = nullptr;
        CameraParams* camera_params = nullptr;
        Emergent::CEmergentFrame* frames = nullptr;
        unsigned int frame_buffer_count = 0;
        bool buffers_allocated = false;
        bool stream_opened = false;

        ~StreamCleanup()
        {
            if (buffers_allocated) {
                log_aperture_message(camera_params, "Begin cleanup: release temporary frame buffers");
                try {
                    destroy_frame_buffer(camera, frames, static_cast<int>(frame_buffer_count), camera_params);
                } catch (...) {
                    log_aperture_message(camera_params, "Cleanup warning: failed to release temporary frame buffers cleanly.");
                }
                log_aperture_message(camera_params, "Done cleanup: release temporary frame buffers");
            }
            delete[] frames;
            if (stream_opened) {
                log_aperture_message(camera_params, "Begin cleanup: close temporary stream");
                EVT_CameraCloseStream(camera);
                log_aperture_message(camera_params, "Done cleanup: close temporary stream");
            }
        }
    } cleanup;

    cleanup.camera = camera;
    cleanup.camera_params = camera_params;

    run_aperture_phase(camera_params, "open temporary stream", [&]() {
        camera_open_stream(camera, camera_params);
    });
    cleanup.stream_opened = true;

    cleanup.frames = new Emergent::CEmergentFrame[frame_buffer_count]();
    log_aperture_message(
        camera_params,
        "Temporary characterization buffers use the standard zero-copy stream path; device-backed frames are copied to host before CPU measurement.");
    run_aperture_phase(camera_params, "allocate temporary frame buffers", [&]() {
        allocate_frame_buffer(camera, cleanup.frames, camera_params, static_cast<int>(frame_buffer_count));
    });
    cleanup.buffers_allocated = true;
    cleanup.frame_buffer_count = frame_buffer_count;

    return characterize_aperture(camera, camera_params, request);
}

nlohmann::json aperture_characterization_to_json(
    const ApertureCharacterizationResult& result,
    const ApertureCharacterizationRequest& request,
    const CameraParams& camera_params,
    const std::string& lens_name,
    const std::string& artifact_id,
    const std::string& created_utc,
    const std::string& fingerprint,
    const ApertureCharacterizationArtifactPaths& paths)
{
    const std::string representative_frames_dir_name =
        request.save_representative_frames
            ? std::filesystem::path(paths.representative_frames_dir).filename().generic_string()
            : "";
    auto make_fov_capture_json = [&](const FovAlignmentCapture& capture) {
        const std::string relative_capture_path =
            capture.has_capture && !capture.capture_path.empty()
                ? artifact_relative_path(paths.artifact_dir, capture.capture_path)
                : "";
        return nlohmann::json{
            {"has_capture", capture.has_capture},
            {"capture_path", relative_capture_path},
            {"has_detected_line", capture.has_detected_line},
            {"line_angle_deg", capture.line_angle_deg},
            {"angle_error_deg", capture.angle_error_deg},
            {"center_offset_px", capture.center_offset_px},
            {"center_offset_fraction", capture.center_offset_fraction}
        };
    };
    nlohmann::json fov_json = {
        {"enabled", request.fov_calibration.enabled},
        {"working_distance_mm", request.fov_calibration.working_distance_mm},
        {"pixel_pitch_um", request.fov_calibration.pixel_pitch_um},
        {"has_field_width_mm", request.fov_calibration.has_field_width_mm},
        {"field_width_mm", request.fov_calibration.field_width_mm},
        {"has_field_height_mm", request.fov_calibration.has_field_height_mm},
        {"field_height_mm", request.fov_calibration.field_height_mm},
        {"sensor_width_mm", request.fov_calibration.sensor_width_mm},
        {"sensor_height_mm", request.fov_calibration.sensor_height_mm},
        {"has_magnification_x", request.fov_calibration.has_magnification_x},
        {"magnification_x", request.fov_calibration.magnification_x},
        {"has_magnification_y", request.fov_calibration.has_magnification_y},
        {"magnification_y", request.fov_calibration.magnification_y},
        {"has_mean_magnification", request.fov_calibration.has_mean_magnification},
        {"mean_magnification", request.fov_calibration.mean_magnification},
        {"has_effective_reference_f_number", request.fov_calibration.has_effective_reference_f_number},
        {"effective_reference_f_number", request.fov_calibration.effective_reference_f_number},
        {"horizontal_capture", make_fov_capture_json(request.fov_calibration.horizontal_capture)},
        {"vertical_capture", make_fov_capture_json(request.fov_calibration.vertical_capture)}
    };
    const std::string relative_camera_config_snapshot_path =
        request.camera_config_snapshot.has_snapshot && !request.camera_config_snapshot.snapshot_path.empty()
            ? artifact_relative_path(paths.artifact_dir, request.camera_config_snapshot.snapshot_path)
            : "";
    nlohmann::json camera_config_snapshot_json = {
        {"has_source_path", request.camera_config_snapshot.has_source_path},
        {"source_path", request.camera_config_snapshot.source_path},
        {"has_snapshot", request.camera_config_snapshot.has_snapshot},
        {"snapshot_path", relative_camera_config_snapshot_path},
        {"error", request.camera_config_snapshot.error}
    };

    nlohmann::json root;
    root["schema_id"] = kApertureCalibrationArtifactSchemaId;
    root["schema_version"] = kApertureCalibrationArtifactSchemaVersion;
    root["artifact_id"] = artifact_id;
    root["created_utc"] = created_utc;
    root["calibration_ref"] = {
        {"artifact_id", artifact_id},
        {"artifact_schema_id", kApertureCalibrationArtifactSchemaId},
        {"artifact_schema_version", kApertureCalibrationArtifactSchemaVersion},
        {"fingerprint", fingerprint}
    };
    root["camera"] = {
        {"serial", camera_params.camera_serial},
        {"width", camera_params.width},
        {"height", camera_params.height},
        {"frame_rate", camera_params.frame_rate},
        {"gain", camera_params.gain},
        {"exposure", camera_params.exposure},
        {"focus", camera_params.focus},
        {"pixel_format", camera_params.pixel_format},
        {"lens_name", lens_name},
        {"iris_min", camera_params.iris_min},
        {"iris_max", camera_params.iris_max},
        {"iris_inc", camera_params.iris_inc}
    };
    root["request"] = {
        {"iris_values", request.iris_values},
        {"frames_per_step", request.frames_per_step},
        {"settle_frames", request.settle_frames},
        {"grab_timeout_ms", request.grab_timeout_ms},
        {"manage_acquisition", request.manage_acquisition},
        {"restore_original_iris", request.restore_original_iris},
        {"has_reference_iris", request.has_reference_iris},
        {"reference_iris", request.reference_iris},
        {"has_reference_f_number", request.has_reference_f_number},
        {"reference_f_number", request.reference_f_number},
        {"camera_config_snapshot", camera_config_snapshot_json},
        {"fov_calibration", fov_json},
        {"grid_rows", request.grid_rows},
        {"grid_cols", request.grid_cols},
        {"save_representative_frames", request.save_representative_frames},
        {"representative_frame_dir", representative_frames_dir_name},
        {"representative_frame_prefix", request.representative_frame_prefix},
        {"thresholds",
            {
                {"saturated_white_fraction", request.thresholds.saturated_white_fraction},
                {"saturated_p99_min", request.thresholds.saturated_p99_min},
                {"dim_mean_max", request.thresholds.dim_mean_max},
                {"dim_p95_max", request.thresholds.dim_p95_max},
                {"dim_black_fraction_min", request.thresholds.dim_black_fraction_min}
            }}
    };
    root["summary"] = {
        {"original_iris", result.original_iris},
        {"restored_original_iris", result.restored_original_iris},
        {"acquisition_started", result.acquisition_started},
        {"acquisition_stopped", result.acquisition_stopped},
        {"has_reference_iris", result.has_reference_iris},
        {"reference_iris", result.reference_iris},
        {"reference_mean", result.reference_mean},
        {"has_usable_window", result.has_usable_window},
        {"usable_iris_min", result.usable_iris_min},
        {"usable_iris_max", result.usable_iris_max},
        {"has_saturation_boundary", result.has_saturation_boundary},
        {"saturation_limited_through_iris", result.saturation_limited_through_iris},
        {"has_dim_boundary", result.has_dim_boundary},
        {"dim_limited_from_iris", result.dim_limited_from_iris},
        {"warnings", result.warnings}
    };

    nlohmann::json steps = nlohmann::json::array();
    for (const ApertureStepResult& step : result.steps) {
        nlohmann::json samples = nlohmann::json::array();
        for (const ApertureFrameSample& sample : step.samples) {
            samples.push_back({
                {"capture_index", sample.capture_index},
                {"frame_id", sample.frame_id},
                {"timestamp", sample.timestamp},
                {"mean", sample.stats.mean},
                {"median", sample.stats.median},
                {"p05", sample.stats.p05},
                {"p95", sample.stats.p95},
                {"p99", sample.stats.p99},
                {"stddev", sample.stats.stddev},
                {"min", sample.stats.min_value},
                {"max", sample.stats.max_value},
                {"black_fraction", sample.stats.black_fraction},
                {"white_fraction", sample.stats.white_fraction}
            });
        }

        const std::string representative_frame_path =
            step.has_representative_frame
                ? artifact_relative_path(paths.artifact_dir, step.representative_frame_path)
                : "";
        nlohmann::json grid_json;
        if (step.has_grid) {
            grid_json = {
                {"rows", step.grid.rows},
                {"cols", step.grid.cols},
                {"tile_mean", step.grid.tile_mean},
                {"tile_relative_mean", step.grid.tile_relative_mean},
                {"min_relative_mean", step.grid.min_relative_mean},
                {"max_relative_mean", step.grid.max_relative_mean},
                {"cv_relative_mean", step.grid.cv_relative_mean}
            };
        }
        steps.push_back({
            {"iris", step.iris},
            {"iris_command_succeeded", step.iris_command_succeeded},
            {"has_iris_readback_after_set", step.has_iris_readback_after_set},
            {"iris_readback_after_set", step.iris_readback_after_set},
            {"iris_verified_after_set", step.iris_verified_after_set},
            {"has_iris_readback_after_capture", step.has_iris_readback_after_capture},
            {"iris_readback_after_capture", step.iris_readback_after_capture},
            {"iris_verified_after_capture", step.iris_verified_after_capture},
            {"classification", aperture_classification_to_string(step.classification)},
            {"has_grid", step.has_grid},
            {"grid", grid_json},
            {"has_representative_frame", step.has_representative_frame},
            {"representative_frame_path", representative_frame_path},
            {"summary",
                {
                    {"mean", step.summary.mean},
                    {"median", step.summary.median},
                    {"p05", step.summary.p05},
                    {"p95", step.summary.p95},
                    {"p99", step.summary.p99},
                    {"stddev", step.summary.stddev},
                    {"min", step.summary.min_value},
                    {"max", step.summary.max_value},
                    {"black_fraction", step.summary.black_fraction},
                    {"white_fraction", step.summary.white_fraction}
                }},
            {"relative_mean", step.relative_mean},
            {"delta_ev", step.delta_ev},
            {"has_estimated_f_number", step.has_estimated_f_number},
            {"estimated_f_number", step.estimated_f_number},
            {"has_estimated_effective_f_number", step.has_estimated_effective_f_number},
            {"estimated_effective_f_number", step.estimated_effective_f_number},
            {"samples", samples}
        });
    }
    root["steps"] = steps;
    return root;
}

nlohmann::json aperture_characterization_manifest_to_json(
    const ApertureCharacterizationResult& result,
    const ApertureCharacterizationRequest& request,
    const CameraParams& camera_params,
    const std::string& lens_name,
    const std::string& artifact_id,
    const std::string& created_utc,
    const std::string& fingerprint,
    const ApertureCharacterizationArtifactPaths& paths)
{
    auto make_manifest_fov_capture = [&](const FovAlignmentCapture& capture) {
        const std::string relative_capture_path =
            capture.has_capture && !capture.capture_path.empty()
                ? artifact_relative_path(paths.artifact_dir, capture.capture_path)
                : "";
        return nlohmann::json{
            {"has_capture", capture.has_capture},
            {"capture_path", relative_capture_path},
            {"has_detected_line", capture.has_detected_line},
            {"angle_error_deg", capture.angle_error_deg},
            {"center_offset_fraction", capture.center_offset_fraction}
        };
    };
    const std::string relative_camera_config_snapshot_path =
        request.camera_config_snapshot.has_snapshot && !request.camera_config_snapshot.snapshot_path.empty()
            ? artifact_relative_path(paths.artifact_dir, request.camera_config_snapshot.snapshot_path)
            : "";
    nlohmann::json camera_config_snapshot_json = {
        {"has_source_path", request.camera_config_snapshot.has_source_path},
        {"source_path", request.camera_config_snapshot.source_path},
        {"has_snapshot", request.camera_config_snapshot.has_snapshot},
        {"snapshot_path", relative_camera_config_snapshot_path},
        {"error", request.camera_config_snapshot.error}
    };
    nlohmann::json manifest;
    manifest["schema_id"] = kCalibrationManifestSchemaId;
    manifest["schema_version"] = kCalibrationManifestSchemaVersion;
    manifest["artifact_id"] = artifact_id;
    manifest["artifact_schema_id"] = kApertureCalibrationArtifactSchemaId;
    manifest["artifact_schema_version"] = kApertureCalibrationArtifactSchemaVersion;
    manifest["created_utc"] = created_utc;
    manifest["producer"] = {
        {"application", "orange"},
        {"artifact_type", "aperture_characterization"}
    };
    manifest["calibration_ref"] = {
        {"artifact_id", artifact_id},
        {"artifact_schema_id", kApertureCalibrationArtifactSchemaId},
        {"artifact_schema_version", kApertureCalibrationArtifactSchemaVersion},
        {"fingerprint", fingerprint}
    };
    manifest["compatibility"] = {
        {"camera_serial", camera_params.camera_serial},
        {"lens_name", lens_name},
        {"focus", camera_params.focus},
        {"exposure", camera_params.exposure},
        {"gain", camera_params.gain},
        {"pixel_format", camera_params.pixel_format},
        {"width", camera_params.width},
        {"height", camera_params.height},
        {"iris_min", camera_params.iris_min},
        {"iris_max", camera_params.iris_max},
        {"iris_inc", camera_params.iris_inc}
    };
    manifest["request"] = {
        {"frames_per_step", request.frames_per_step},
        {"settle_frames", request.settle_frames},
        {"grab_timeout_ms", request.grab_timeout_ms},
        {"has_reference_iris", request.has_reference_iris},
        {"reference_iris", request.reference_iris},
        {"has_reference_f_number", request.has_reference_f_number},
        {"reference_f_number", request.reference_f_number},
        {"camera_config_snapshot", camera_config_snapshot_json},
        {"fov_calibration",
            {
                {"enabled", request.fov_calibration.enabled},
                {"working_distance_mm", request.fov_calibration.working_distance_mm},
                {"pixel_pitch_um", request.fov_calibration.pixel_pitch_um},
                {"has_field_width_mm", request.fov_calibration.has_field_width_mm},
                {"field_width_mm", request.fov_calibration.field_width_mm},
                {"has_field_height_mm", request.fov_calibration.has_field_height_mm},
                {"field_height_mm", request.fov_calibration.field_height_mm},
                {"sensor_width_mm", request.fov_calibration.sensor_width_mm},
                {"sensor_height_mm", request.fov_calibration.sensor_height_mm},
                {"has_mean_magnification", request.fov_calibration.has_mean_magnification},
                {"mean_magnification", request.fov_calibration.mean_magnification},
                {"has_effective_reference_f_number", request.fov_calibration.has_effective_reference_f_number},
                {"effective_reference_f_number", request.fov_calibration.effective_reference_f_number},
                {"horizontal_capture", make_manifest_fov_capture(request.fov_calibration.horizontal_capture)},
                {"vertical_capture", make_manifest_fov_capture(request.fov_calibration.vertical_capture)}
            }},
        {"grid_rows", request.grid_rows},
        {"grid_cols", request.grid_cols}
    };
    manifest["summary"] = {
        {"has_usable_window", result.has_usable_window},
        {"usable_iris_min", result.usable_iris_min},
        {"usable_iris_max", result.usable_iris_max},
        {"has_saturation_boundary", result.has_saturation_boundary},
        {"saturation_limited_through_iris", result.saturation_limited_through_iris},
        {"has_dim_boundary", result.has_dim_boundary},
        {"dim_limited_from_iris", result.dim_limited_from_iris},
        {"warnings", result.warnings}
    };
    manifest["files"] = {
        {"manifest", "manifest.json"},
        {"measurement_json", std::filesystem::path(paths.measurement_json_path).filename().string()},
        {"steps_csv", std::filesystem::path(paths.steps_csv_path).filename().string()},
        {"frames_csv", std::filesystem::path(paths.frames_csv_path).filename().string()},
        {"camera_config_snapshot",
         request.camera_config_snapshot.has_snapshot
             ? std::filesystem::path(paths.camera_config_snapshot_path).filename().string()
             : ""},
        {"fov_reference_frames_dir",
         request.fov_calibration.horizontal_capture.has_capture || request.fov_calibration.vertical_capture.has_capture
             ? std::filesystem::path(paths.fov_reference_frames_dir).filename().string()
             : ""},
        {"representative_frames_dir",
         request.save_representative_frames
             ? std::filesystem::path(paths.representative_frames_dir).filename().string()
             : ""}
    };
    return manifest;
}

bool write_aperture_characterization_json(
    const std::string& path,
    const nlohmann::json& data,
    std::string* error_out)
{
    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    std::ofstream out(path);
    if (!out) {
        if (error_out) {
            *error_out = "Failed to open JSON output path: " + path;
        }
        return false;
    }
    out << std::setw(2) << data << "\n";
    return true;
}

bool write_aperture_characterization_step_csv(
    const std::string& path,
    const ApertureCharacterizationResult& result,
    const ApertureCharacterizationArtifactPaths& paths,
    std::string* error_out)
{
    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    std::ofstream out(path);
    if (!out) {
        if (error_out) {
            *error_out = "Failed to open step CSV output path: " + path;
        }
        return false;
    }

    out << "iris,iris_command_succeeded,iris_readback_after_set,iris_verified_after_set,iris_readback_after_capture,iris_verified_after_capture,classification,representative_frame,frames_used,mean,median,p05,p95,p99,stddev,min,max,black_fraction,white_fraction,grid_min_relative_mean,grid_max_relative_mean,grid_cv_relative_mean,relative_mean,delta_ev,estimated_f_number,estimated_effective_f_number\n";
    for (const ApertureStepResult& step : result.steps) {
        const std::string representative_frame_path =
            step.has_representative_frame
                ? artifact_relative_path(paths.artifact_dir, step.representative_frame_path)
                : "";
        out << step.iris << ","
            << (step.iris_command_succeeded ? 1 : 0) << ",";
        if (step.has_iris_readback_after_set) {
            out << step.iris_readback_after_set;
        }
        out << ","
            << (step.iris_verified_after_set ? 1 : 0) << ",";
        if (step.has_iris_readback_after_capture) {
            out << step.iris_readback_after_capture;
        }
        out << ","
            << (step.iris_verified_after_capture ? 1 : 0) << ","
            << aperture_classification_to_string(step.classification) << ","
            << representative_frame_path << ","
            << step.samples.size() << ","
            << step.summary.mean << ","
            << step.summary.median << ","
            << step.summary.p05 << ","
            << step.summary.p95 << ","
            << step.summary.p99 << ","
            << step.summary.stddev << ","
            << step.summary.min_value << ","
            << step.summary.max_value << ","
            << step.summary.black_fraction << ","
            << step.summary.white_fraction << ","
            << (step.has_grid ? step.grid.min_relative_mean : 0.0) << ","
            << (step.has_grid ? step.grid.max_relative_mean : 0.0) << ","
            << (step.has_grid ? step.grid.cv_relative_mean : 0.0) << ","
            << step.relative_mean << ","
            << step.delta_ev << ",";
        if (step.has_estimated_f_number) {
            out << step.estimated_f_number;
        }
        out << ",";
        if (step.has_estimated_effective_f_number) {
            out << step.estimated_effective_f_number;
        }
        out << "\n";
    }
    return true;
}

bool write_aperture_characterization_frame_csv(
    const std::string& path,
    const ApertureCharacterizationResult& result,
    std::string* error_out)
{
    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    std::ofstream out(path);
    if (!out) {
        if (error_out) {
            *error_out = "Failed to open frame CSV output path: " + path;
        }
        return false;
    }

    out << "iris,capture_index,frame_id,timestamp,mean,median,p05,p95,p99,stddev,min,max,black_fraction,white_fraction\n";
    for (const ApertureStepResult& step : result.steps) {
        for (const ApertureFrameSample& sample : step.samples) {
            out << step.iris << ","
                << sample.capture_index << ","
                << sample.frame_id << ","
                << sample.timestamp << ","
                << sample.stats.mean << ","
                << sample.stats.median << ","
                << sample.stats.p05 << ","
                << sample.stats.p95 << ","
                << sample.stats.p99 << ","
                << sample.stats.stddev << ","
                << sample.stats.min_value << ","
                << sample.stats.max_value << ","
                << sample.stats.black_fraction << ","
                << sample.stats.white_fraction << "\n";
        }
    }
    return true;
}
