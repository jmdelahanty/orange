#pragma once

#include <GL/glew.h>
#include <cuda_gl_interop.h>
#include <cuda_runtime_api.h>

#include <cstddef>

// OpenGL/CUDA display resources must be created and destroyed on the GUI
// thread that owns the OpenGL context.  Keeping this small interface separate
// from gui.h lets background lifecycle modules describe texture ownership
// without pulling the full GUI implementation into another translation unit.
struct GL_Texture {
    GLuint texture = 0;
    GLuint pbo = 0;
    cudaGraphicsResource_t cuda_resource = nullptr;
    unsigned char* cuda_buffer = nullptr;
    std::size_t cuda_pbo_storage_buffer_size = 0;
    cudaStream_t streams = nullptr;
    int cuda_device_id = -1;
    int num_channels = 0;
};

void setup_texture(GL_Texture& texture, int width, int height);
void upload_texture_from_pbo(GL_Texture& texture, int width, int height);
void clear_upload_and_cleanup(GL_Texture& texture, int width, int height);
