#include "gui/texture_resources.h"
#include "gui/preview_staging_lock.h"

#include <cuda_runtime.h>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>

namespace orange::gui {

namespace {
std::mutex& staging_mutex_for(const void* staging)
{
    static std::mutex registry_mutex;
    static std::map<const void*, std::unique_ptr<std::mutex>> registry;
    std::lock_guard<std::mutex> guard(registry_mutex);
    auto& slot = registry[staging];
    if (!slot) {
        slot = std::make_unique<std::mutex>();
    }
    return *slot;
}
}  // namespace

std::unique_lock<std::mutex> lock_preview_staging(const void* staging)
{
    if (!staging) {
        return {};
    }
    return std::unique_lock<std::mutex>(staging_mutex_for(staging));
}

std::unique_lock<std::mutex> try_lock_preview_staging(const void* staging)
{
    if (!staging) {
        return {};
    }
    return std::unique_lock<std::mutex>(staging_mutex_for(staging), std::try_to_lock);
}

}  // namespace orange::gui

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
    // Ownership contract (CUDA graphics interop, fixed 2026-09-21): the PBO is
    // registered with CUDA but never left mapped. Background preview workers
    // write into a plain device staging buffer (`cuda_buffer`, same GPU as the
    // GL context); the GUI thread maps the PBO, copies staging -> PBO on
    // `streams`, unmaps, and only then lets OpenGL read the PBO. Before this,
    // the PBO stayed CUDA-mapped for the whole session while glTexSubImage2D
    // read it, which the CUDA docs define as undefined behaviour.
    cudaGetDevice(&texture.cuda_device_id);
    cudaStreamCreate(&texture.streams);
    create_pbo(&texture.pbo, width, height);
    cudaGraphicsGLRegisterBuffer(
        &texture.cuda_resource,
        texture.pbo,
        cudaGraphicsRegisterFlagsWriteDiscard);
    const std::size_t bytes = static_cast<std::size_t>(width) * height * 4;
    texture.cuda_pbo_storage_buffer_size = bytes;
    texture.cuda_buffer = nullptr;
    if (cudaMalloc(reinterpret_cast<void**>(&texture.cuda_buffer), bytes) != cudaSuccess) {
        texture.cuda_buffer = nullptr;
    } else {
        cudaMemset(texture.cuda_buffer, 0, bytes);
    }
    create_texture(&texture.texture, width, height);
}

void upload_texture_from_pbo(
    GL_Texture& texture,
    const int width,
    const int height)
{
    // Diagnostic (2026-09-21): ORANGE_GUI_SKIP_PBO_UPLOAD=1 keeps every CUDA
    // preview copy but never hands the PBO to OpenGL, so the displayed
    // textures freeze. Used in the card-A frame-loss bisect.
    static const bool skip_upload = [] {
        const char* value = std::getenv("ORANGE_GUI_SKIP_PBO_UPLOAD");
        return value && *value && std::strcmp(value, "0") != 0;
    }();
    if (skip_upload) {
        return;
    }
    const std::size_t bytes = static_cast<std::size_t>(width) * height * 4;
    if (texture.cuda_resource && texture.cuda_buffer && texture.streams) {
        // The worker may be writing the staging buffer right now; skip this
        // upload rather than block the GUI thread (the next serial follows).
        auto staging_lock = orange::gui::try_lock_preview_staging(texture.cuda_buffer);
        if (!staging_lock.owns_lock()) {
            return;
        }
        if (texture.cuda_device_id >= 0 && cudaSetDevice(texture.cuda_device_id) != cudaSuccess) {
            return;
        }
        // Map -> copy staging into the PBO -> unmap, all stream-ordered, then
        // wait so OpenGL below reads a complete frame. The map/unmap pair is
        // what transfers ownership between CUDA and OpenGL. Any failure
        // leaves the PBO unmapped and skips the GL upload.
        static bool logged_failure = false;
        auto fail = [&](const char* what, cudaError_t status) {
            if (!logged_failure) {
                logged_failure = true;
                std::cerr << "[GUI][texture] preview upload skipped: " << what
                          << " (" << cudaGetErrorString(status) << ")" << std::endl;
            }
        };
        cudaError_t status = cudaGraphicsMapResources(1, &texture.cuda_resource, texture.streams);
        if (status != cudaSuccess) {
            fail("cudaGraphicsMapResources", status);
            return;
        }
        void* mapped = nullptr;
        std::size_t mapped_bytes = 0;
        status = cudaGraphicsResourceGetMappedPointer(&mapped, &mapped_bytes, texture.cuda_resource);
        bool copied = false;
        if (status != cudaSuccess || !mapped) {
            fail("cudaGraphicsResourceGetMappedPointer", status);
        } else if (mapped_bytes < bytes) {
            fail("mapped PBO smaller than the texture", cudaErrorInvalidValue);
        } else {
            status = cudaMemcpyAsync(mapped, texture.cuda_buffer, bytes, cudaMemcpyDeviceToDevice, texture.streams);
            if (status != cudaSuccess) {
                fail("cudaMemcpyAsync(staging -> PBO)", status);
            } else {
                copied = true;
            }
        }
        status = cudaGraphicsUnmapResources(1, &texture.cuda_resource, texture.streams);
        if (status != cudaSuccess) {
            fail("cudaGraphicsUnmapResources", status);
            return;
        }
        status = cudaStreamSynchronize(texture.streams);
        if (status != cudaSuccess) {
            fail("cudaStreamSynchronize(texture stream)", status);
            return;
        }
        if (!copied) {
            return;
        }
    }
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

    // Push the cleared staging buffer through the PBO so the texture goes
    // black, then release everything. The PBO is not mapped at this point.
    upload_texture_from_pbo(texture, width, height);

    if (texture.cuda_resource) {
        cudaGraphicsUnregisterResource(texture.cuda_resource);
        texture.cuda_resource = nullptr;
    }
    if (texture.cuda_buffer) {
        cudaFree(texture.cuda_buffer);
        texture.cuda_buffer = nullptr;
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
