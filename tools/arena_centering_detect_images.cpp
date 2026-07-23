#include "gui/arena_centering_analysis.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::string camera_serial_from_path(const std::string& path)
{
    const std::string marker = "Cam";
    const std::size_t begin = path.find(marker);
    if (begin == std::string::npos) return std::filesystem::path(path).stem().string();
    std::size_t end = begin + marker.size();
    while (end < path.size() && path[end] >= '0' && path[end] <= '9') ++end;
    return path.substr(begin + marker.size(), end - begin - marker.size());
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "Usage: arena_centering_detect_images <image> [image ...]" << std::endl;
        return 2;
    }
    std::vector<std::vector<unsigned char>> owned_rgba;
    std::vector<orange::gui::arena_centering::RgbaFrameView> views;
    owned_rgba.reserve(static_cast<std::size_t>(argc - 1));
    views.reserve(static_cast<std::size_t>(argc - 1));
    for (int index = 1; index < argc; ++index) {
        const std::string path = argv[index];
        cv::Mat source = cv::imread(path, cv::IMREAD_UNCHANGED);
        if (source.empty()) {
            std::cerr << "Could not read " << path << std::endl;
            return 2;
        }
        cv::Mat rgba;
        if (source.channels() == 1) cv::cvtColor(source, rgba, cv::COLOR_GRAY2RGBA);
        else if (source.channels() == 3) cv::cvtColor(source, rgba, cv::COLOR_BGR2RGBA);
        else if (source.channels() == 4) cv::cvtColor(source, rgba, cv::COLOR_BGRA2RGBA);
        else {
            std::cerr << "Unsupported channel count for " << path << std::endl;
            return 2;
        }
        owned_rgba.emplace_back(
            rgba.data, rgba.data + rgba.total() * rgba.elemSize());
        views.push_back({
            camera_serial_from_path(path),
            rgba.cols,
            rgba.rows,
            &owned_rgba.back(),
        });
    }
    const auto batch =
        orange::gui::arena_centering::DetectArenaCenterFiducialsConcurrently(views);
    const auto rectangles =
        orange::gui::arena_centering::DetectArenaRectangleBoundariesConcurrently(views);
    std::cout << nlohmann::json{
        {"center_fiducials", batch.ToJson()},
        {"rectangle_boundaries", rectangles.ToJson()},
    }.dump(2) << std::endl;
    for (const auto& detection : batch.detections) {
        if (!detection.ok) return 1;
    }
    for (const auto& detection : rectangles.detections) {
        if (!detection.ok) return 1;
    }
    return 0;
}
