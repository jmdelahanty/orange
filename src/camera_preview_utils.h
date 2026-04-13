#ifndef ORANGE_CAMERA_PREVIEW_UTILS_H
#define ORANGE_CAMERA_PREVIEW_UTILS_H

#include "camera.h"

#include <GL/glew.h>
#include <opencv2/core/mat.hpp>

#include <string>
#include <vector>

namespace orange::preview {

bool grab_latest_frame(Emergent::CEmergentCamera* camera,
                       CameraParams* camera_params,
                       unsigned int timeout_ms,
                       Emergent::CEmergentFrame* frame_out,
                       int* dropped_frame_count_out);

bool frame_to_bgr(const Emergent::CEmergentFrame& frame, cv::Mat* bgr_out, std::string* error_out);

bool rgb_to_rgba(const std::vector<unsigned char>& rgb,
                 int width,
                 int height,
                 std::vector<unsigned char>* rgba_out,
                 std::string* error_out = nullptr);

bool update_rgba_texture(GLuint* texture,
                         int* texture_width,
                         int* texture_height,
                         const unsigned char* rgba,
                         int width,
                         int height,
                         std::string* error_out = nullptr);

bool update_rgba_texture(GLuint* texture,
                         int* texture_width,
                         int* texture_height,
                         const std::vector<unsigned char>& rgba,
                         int width,
                         int height,
                         std::string* error_out = nullptr);

void clear_texture(GLuint* texture, int* texture_width = nullptr, int* texture_height = nullptr);

bool capture_single_frame_rgba(Emergent::CEmergentCamera* camera,
                               CameraParams* camera_params,
                               int buffer_count,
                               unsigned int timeout_ms,
                               std::vector<unsigned char>* rgba_out,
                               int* width_out,
                               int* height_out,
                               int* dropped_frame_count_out,
                               std::string* error_out);

} // namespace orange::preview

#endif
