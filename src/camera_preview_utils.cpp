#include "camera_preview_utils.h"

#include <cuda_runtime.h>
#include <opencv2/opencv.hpp>

#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>

namespace {

class ScopedPreviewCudaDevice {
public:
    explicit ScopedPreviewCudaDevice(int target_device)
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

    ~ScopedPreviewCudaDevice()
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

size_t preview_frame_byte_count(int pixel_type, unsigned int width, unsigned int height)
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

bool extract_frame_host_bytes(const Emergent::CEmergentFrame& frame,
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
        const size_t byte_count = preview_frame_byte_count(frame.pixel_type, frame.size_x, frame.size_y);
        if (byte_count == 0) {
            if (error_out) {
                *error_out = "Unsupported pixel format for preview extraction.";
            }
            return false;
        }
        try {
            ScopedPreviewCudaDevice guard(attrs.device);
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
    return true;
}

bool convert_frame_bytes_to_bgr(int pixel_type,
                                unsigned int width,
                                unsigned int height,
                                const unsigned char* frame_bytes,
                                cv::Mat* bgr_out,
                                std::string* error_out)
{
    if (bgr_out == nullptr || frame_bytes == nullptr || width == 0 || height == 0) {
        if (error_out) {
            *error_out = "Invalid frame buffer for BGR conversion.";
        }
        return false;
    }

    switch (pixel_type) {
        case GVSP_PIX_MONO8: {
            cv::Mat gray(static_cast<int>(height), static_cast<int>(width), CV_8UC1,
                         const_cast<unsigned char*>(frame_bytes));
            cv::cvtColor(gray, *bgr_out, cv::COLOR_GRAY2BGR);
            return true;
        }
        case GVSP_PIX_BAYRG8: {
            cv::Mat raw(static_cast<int>(height), static_cast<int>(width), CV_8UC1,
                        const_cast<unsigned char*>(frame_bytes));
            cv::cvtColor(raw, *bgr_out, cv::COLOR_BayerRG2BGR);
            return true;
        }
        case GVSP_PIX_BAYGB8: {
            cv::Mat raw(static_cast<int>(height), static_cast<int>(width), CV_8UC1,
                        const_cast<unsigned char*>(frame_bytes));
            cv::cvtColor(raw, *bgr_out, cv::COLOR_BayerGB2BGR);
            return true;
        }
        case GVSP_PIX_RGB8: {
            cv::Mat rgb(static_cast<int>(height), static_cast<int>(width), CV_8UC3,
                        const_cast<unsigned char*>(frame_bytes));
            cv::cvtColor(rgb, *bgr_out, cv::COLOR_RGB2BGR);
            return true;
        }
        case GVSP_PIX_BGR8: {
            cv::Mat bgr(static_cast<int>(height), static_cast<int>(width), CV_8UC3,
                        const_cast<unsigned char*>(frame_bytes));
            *bgr_out = bgr.clone();
            return true;
        }
        default:
            if (error_out) {
                *error_out = "Preview conversion does not support this pixel format.";
            }
            return false;
    }
}

} // namespace

namespace orange::preview {

bool grab_latest_frame(Emergent::CEmergentCamera* camera,
                       CameraParams* camera_params,
                       unsigned int timeout_ms,
                       Emergent::CEmergentFrame* frame_out,
                       int* dropped_frame_count_out)
{
    if (frame_out == nullptr) {
        return false;
    }

    Emergent::CEmergentFrame frame{};
    EVT_ERROR err = EVT_CameraGetFrame(camera, &frame, timeout_ms);
    if (err != EVT_SUCCESS) {
        return false;
    }

    int dropped_frames = 0;
    while (true) {
        Emergent::CEmergentFrame newer_frame{};
        err = EVT_CameraGetFrame(camera, &newer_frame, 0);
        if (err != EVT_SUCCESS) {
            break;
        }
        check_camera_errors(EVT_CameraQueueFrame(camera, &frame), camera_params->camera_serial.c_str());
        frame = newer_frame;
        ++dropped_frames;
    }

    *frame_out = frame;
    if (dropped_frame_count_out != nullptr) {
        *dropped_frame_count_out = dropped_frames;
    }
    return true;
}

bool frame_to_bgr(const Emergent::CEmergentFrame& frame, cv::Mat* bgr_out, std::string* error_out)
{
    std::vector<unsigned char> host_bytes;
    const unsigned char* frame_bytes = nullptr;
    if (!extract_frame_host_bytes(frame, &host_bytes, &frame_bytes, error_out)) {
        return false;
    }
    return convert_frame_bytes_to_bgr(frame.pixel_type, frame.size_x, frame.size_y, frame_bytes, bgr_out, error_out);
}

bool rgb_to_rgba(const std::vector<unsigned char>& rgb,
                 int width,
                 int height,
                 std::vector<unsigned char>* rgba_out,
                 std::string* error_out)
{
    if (rgba_out == nullptr) {
        if (error_out) {
            *error_out = "rgb_to_rgba requires a non-null output buffer.";
        }
        return false;
    }
    if (width <= 0 || height <= 0) {
        if (error_out) {
            *error_out = "rgb_to_rgba requires positive width and height.";
        }
        return false;
    }
    const size_t expected_rgb_bytes =
        static_cast<size_t>(width) * static_cast<size_t>(height) * 3U;
    if (rgb.size() != expected_rgb_bytes) {
        if (error_out) {
            *error_out = "rgb_to_rgba input size does not match width*height*3.";
        }
        return false;
    }

    rgba_out->resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4U);
    for (int i = 0; i < width * height; ++i) {
        const size_t src = static_cast<size_t>(i) * 3U;
        const size_t dst = static_cast<size_t>(i) * 4U;
        (*rgba_out)[dst + 0] = rgb[src + 0];
        (*rgba_out)[dst + 1] = rgb[src + 1];
        (*rgba_out)[dst + 2] = rgb[src + 2];
        (*rgba_out)[dst + 3] = 255;
    }
    return true;
}

bool update_rgba_texture(GLuint* texture,
                         int* texture_width,
                         int* texture_height,
                         const unsigned char* rgba,
                         int width,
                         int height,
                         std::string* error_out)
{
    if (texture == nullptr || texture_width == nullptr || texture_height == nullptr || rgba == nullptr) {
        if (error_out) {
            *error_out = "update_rgba_texture requires non-null texture pointers and pixel data.";
        }
        return false;
    }
    if (width <= 0 || height <= 0) {
        if (error_out) {
            *error_out = "update_rgba_texture requires positive width and height.";
        }
        return false;
    }

    if (*texture == 0 || *texture_width != width || *texture_height != height) {
        if (*texture != 0) {
            glDeleteTextures(1, texture);
        }
        glGenTextures(1, texture);
        glBindTexture(GL_TEXTURE_2D, *texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        *texture_width = width;
        *texture_height = height;
    } else {
        glBindTexture(GL_TEXTURE_2D, *texture);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}

bool update_rgba_texture(GLuint* texture,
                         int* texture_width,
                         int* texture_height,
                         const std::vector<unsigned char>& rgba,
                         int width,
                         int height,
                         std::string* error_out)
{
    if (rgba.empty()) {
        if (error_out) {
            *error_out = "update_rgba_texture requires non-empty RGBA pixel data.";
        }
        return false;
    }
    return update_rgba_texture(texture, texture_width, texture_height, rgba.data(), width, height, error_out);
}

void clear_texture(GLuint* texture, int* texture_width, int* texture_height)
{
    if (texture != nullptr && *texture != 0) {
        glDeleteTextures(1, texture);
        *texture = 0;
    }
    if (texture_width != nullptr) {
        *texture_width = 0;
    }
    if (texture_height != nullptr) {
        *texture_height = 0;
    }
}

bool capture_single_frame_rgba(Emergent::CEmergentCamera* camera,
                               CameraParams* camera_params,
                               int buffer_count,
                               unsigned int timeout_ms,
                               std::vector<unsigned char>* rgba_out,
                               int* width_out,
                               int* height_out,
                               int* dropped_frame_count_out,
                               std::string* error_out)
{
    if (camera == nullptr || camera_params == nullptr || rgba_out == nullptr ||
        width_out == nullptr || height_out == nullptr) {
        if (error_out) {
            *error_out = "capture_single_frame_rgba received invalid pointers.";
        }
        return false;
    }
    if (buffer_count <= 0) {
        if (error_out) {
            *error_out = "capture_single_frame_rgba requires buffer_count > 0.";
        }
        return false;
    }

    Emergent::CEmergentFrame* frames = nullptr;
    bool stream_opened = false;
    bool buffers_allocated = false;
    bool acquisition_started = false;

    try {
        camera_open_stream(camera, camera_params);
        stream_opened = true;

        frames = new Emergent::CEmergentFrame[buffer_count]();
        allocate_frame_buffer(camera, frames, camera_params, buffer_count);
        buffers_allocated = true;

        check_camera_errors(
            EVT_CameraExecuteCommand(camera, "AcquisitionStart"),
            camera_params->camera_serial.c_str());
        acquisition_started = true;

        Emergent::CEmergentFrame frame{};
        int dropped_frames = 0;
        if (!grab_latest_frame(camera, camera_params, timeout_ms, &frame, &dropped_frames)) {
            throw std::runtime_error("Timed out waiting for a camera frame.");
        }

        cv::Mat bgr;
        std::string convert_error;
        if (!frame_to_bgr(frame, &bgr, &convert_error)) {
            EVT_CameraQueueFrame(camera, &frame);
            throw std::runtime_error(convert_error);
        }

        cv::Mat rgba;
        cv::cvtColor(bgr, rgba, cv::COLOR_BGR2RGBA);
        rgba_out->assign(rgba.data, rgba.data + rgba.total() * rgba.elemSize());
        *width_out = rgba.cols;
        *height_out = rgba.rows;
        if (dropped_frame_count_out != nullptr) {
            *dropped_frame_count_out = dropped_frames;
        }

        check_camera_errors(EVT_CameraQueueFrame(camera, &frame), camera_params->camera_serial.c_str());
    } catch (const std::exception& ex) {
        if (error_out) {
            *error_out = ex.what();
        }
    }

    if (acquisition_started) {
        EVT_CameraExecuteCommand(camera, "AcquisitionStop");
    }
    if (buffers_allocated && frames != nullptr) {
        try {
            destroy_frame_buffer(camera, frames, buffer_count, camera_params);
        } catch (...) {
        }
    }
    delete[] frames;
    if (stream_opened) {
        EVT_CameraCloseStream(camera);
    }

    return error_out == nullptr || error_out->empty();
}

} // namespace orange::preview
