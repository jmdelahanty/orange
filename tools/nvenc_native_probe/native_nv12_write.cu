#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>

// Stable Driver API entry point shared by the standalone probe and a future
// recorder integration. Each warp covers 512 adjacent luma bytes. Aligned
// rows use one 16-byte source load and one surface write per thread; unusual
// row pitches fall back to aligned 4-byte loads without changing output.
extern "C" __global__ void orange_native_nv12_write_luma(
    cudaSurfaceObject_t output_y,
    const std::uint8_t* input_y,
    std::size_t input_pitch,
    std::uint32_t width,
    std::uint32_t height)
{
    constexpr std::uint32_t kBytesPerThread = 16;
    const std::uint32_t x =
        (blockIdx.x * blockDim.x + threadIdx.x) * kBytesPerThread;
    const std::uint32_t y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) {
        return;
    }

    const std::uint8_t* source = input_y + static_cast<std::size_t>(y) * input_pitch + x;
    if (x + kBytesPerThread <= width) {
        uint4 pixels;
        if ((reinterpret_cast<std::uintptr_t>(source) & 0xfU) == 0) {
            pixels = *reinterpret_cast<const uint4*>(source);
        } else {
            const auto* words = reinterpret_cast<const std::uint32_t*>(source);
            pixels = make_uint4(words[0], words[1], words[2], words[3]);
        }
        surf2Dwrite(pixels, output_y, x, y, cudaBoundaryModeTrap);
        return;
    }

    // Width is normally divisible by 16 (4512 and its pitched variants are),
    // but retain a safe 4-byte tail for other valid NV12 widths.
    for (std::uint32_t offset = 0; offset < kBytesPerThread && x + offset < width; offset += 4) {
        const std::uint32_t pixels =
            *reinterpret_cast<const std::uint32_t*>(source + offset);
        surf2Dwrite(pixels, output_y, x + offset, y, cudaBoundaryModeTrap);
    }
}
