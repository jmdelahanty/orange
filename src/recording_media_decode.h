#pragma once
#include <cstdint>
#include <filesystem>

namespace orange::recording {
void RequireCropMediaContainerRaster(const std::filesystem::path&, int width, int height);
// Finalization-only, single-threaded software HEVC decode. Never acquisition work.
// Requires exactly one video stream, one packet and one decoded frame per source
// identity, and the authenticated visible raster. Legitimate black crops pass.
void RequireDecodedCropMedia(const std::filesystem::path&, int width, int height,
                             uint64_t frames);
}
