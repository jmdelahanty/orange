#include "gui/texture_resources.h"

#include <cuda_runtime.h>

namespace {

void create_texture(GLuint* texture, const int width, const int height)
{
    glGenTextures(1, texture);
    glBindTexture(GL_TEXTURE_2D, *texture);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RGBA,
        width,
        height,
        0,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);
    glEnable(GL_TEXTURE_2D);
}

void create_pbo(GLuint* pbo, const int width, const int height)
{
    glGenBuffers(1, pbo);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, *pbo);
    glBufferData(
        GL_PIXEL_UNPACK_BUFFER,
        static_cast<GLsizeiptr>(width) * height * 4,
        nullptr,
        GL_DYNAMIC_COPY);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
}

}  // namespace

void setup_texture(GL_Texture& texture, const int width, const int height)
{
    cudaGetDevice(&texture.cuda_device_id);
    cudaStreamCreate(&texture.streams);
    create_pbo(&texture.pbo, width, height);
    cudaGraphicsGLRegisterBuffer(
        &texture.cuda_resource,
        texture.pbo,
        cudaGraphicsRegisterFlagsNone);
    cudaGraphicsMapResources(1, &texture.cuda_resource, texture.streams);
    cudaGraphicsResourceGetMappedPointer(
        reinterpret_cast<void**>(&texture.cuda_buffer),
        &texture.cuda_pbo_storage_buffer_size,
        texture.cuda_resource);
    if (texture.cuda_buffer) {
        cudaMemsetAsync(
            texture.cuda_buffer,
            0,
            static_cast<std::size_t>(width) * height * 4,
            texture.streams);
        cudaStreamSynchronize(texture.streams);
    }
    create_texture(&texture.texture, width, height);
}

void upload_texture_from_pbo(
    GL_Texture& texture,
    const int width,
    const int height)
{
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, texture.pbo);
    glBindTexture(GL_TEXTURE_2D, texture.texture);
    glTexSubImage2D(
        GL_TEXTURE_2D,
        0,
        0,
        0,
        width,
        height,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        nullptr);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void clear_upload_and_cleanup(
    GL_Texture& texture,
    const int width,
    const int height)
{
    if (texture.cuda_device_id >= 0) {
        cudaSetDevice(texture.cuda_device_id);
    }
    if (texture.cuda_buffer) {
        const std::size_t bytes =
            static_cast<std::size_t>(width) * height * 4;
        cudaMemset(texture.cuda_buffer, 0, bytes);
    }

    if (texture.streams) {
        cudaStreamSynchronize(texture.streams);
    }
    if (texture.cuda_resource) {
        cudaGraphicsUnmapResources(1, &texture.cuda_resource);
        texture.cuda_buffer = nullptr;
    }

    upload_texture_from_pbo(texture, width, height);

    if (texture.cuda_resource) {
        cudaGraphicsUnregisterResource(texture.cuda_resource);
        texture.cuda_resource = nullptr;
    }
    if (texture.pbo) {
        glDeleteBuffers(1, &texture.pbo);
        texture.pbo = 0;
    }
    if (texture.streams) {
        cudaStreamDestroy(texture.streams);
        texture.streams = nullptr;
    }
    if (texture.texture) {
        glDeleteTextures(1, &texture.texture);
        texture.texture = 0;
    }
    texture.cuda_buffer = nullptr;
    texture.cuda_pbo_storage_buffer_size = 0;
    texture.cuda_device_id = -1;
}
