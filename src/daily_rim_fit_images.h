#pragma once

// Review images of one daily rim fit (src/daily_rim_fit.h), written beside the fit JSON so
// the person who confirms the daily registration sees what the fit saw, and so a refused
// fit can be diagnosed:
//   daily_rim_frame.png        the grey rim frame the fit ran on, native resolution, lossless
//   daily_rim_fit_overlay.png  the whole view at 1/4: fixed (known-radius) and free fits,
//                              inlier and rejected edge points, every reported gap labelled
//   daily_rim_fit_zoom.png     native-resolution crops at the top, right, bottom and left
//                              (the layout of scripts/registration_accuracy_check.py)
// Pure CPU work (OpenCV encode + atomic file writes): callers run it off the GUI thread.

#include "daily_rim_fit.h"

#include "json.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace orange::daily_rim_fit {

inline constexpr const char* kRimFrameFilename = "daily_rim_frame.png";
inline constexpr const char* kRimOverlayFilename = "daily_rim_fit_overlay.png";
inline constexpr const char* kRimZoomFilename = "daily_rim_fit_zoom.png";

struct FitImages {
    bool ok = false;
    std::string error;
    std::string frame_png;
    std::string overlay_png;
    std::string zoom_png;
};

// Writes the three images into `directory` (created if needed). `label` heads the
// figures (e.g. "Cam2010093 arena_1").
FitImages write_fit_images(const std::vector<std::uint8_t>& gray, int width, int height, const Result& result,
                           const std::filesystem::path& directory, const std::string& label);

nlohmann::json to_json(const FitImages& images);

}  // namespace orange::daily_rim_fit
