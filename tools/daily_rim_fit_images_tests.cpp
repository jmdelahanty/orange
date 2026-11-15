// The daily rim fit's review images (src/daily_rim_fit_images.h): written for an
// accepted and for a refused fit, the frame lossless at native resolution.
#include "daily_rim_fit.h"
#include "daily_rim_fit_images.h"

#include <opencv2/imgcodecs.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const std::string& what)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << what << "\n";
    } else {
        std::cout << "ok: " << what << "\n";
    }
}

// IR-like rim: bright inside, a 10 px dark ring at `radius`, bright outside.
std::vector<std::uint8_t> render(int width, int height, double cx, double cy, double radius)
{
    std::mt19937 rng(11);
    std::normal_distribution<double> noise(0.0, 2.0);
    std::vector<std::uint8_t> img(static_cast<std::size_t>(width) * height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const double d = std::hypot(x - cx, y - cy);
            double v = d < radius ? 200.0 : (d < radius + 10.0 ? 70.0 : 190.0);
            v += noise(rng);
            img[static_cast<std::size_t>(y) * width + x] = static_cast<std::uint8_t>(std::clamp(v, 0.0, 255.0));
        }
    }
    return img;
}

namespace fs = std::filesystem;
namespace fit = orange::daily_rim_fit;

}  // namespace

int main()
{
    const fs::path root = fs::temp_directory_path() / ("daily_rim_fit_images_tests_" + std::to_string(::getpid()));
    fs::remove_all(root);
    const int width = 1400;
    const int height = 1400;
    {
        const auto img = render(width, height, 700.0, 700.0, 600.0);
        const auto r = fit::fit_known_radius(img.data(), width, height, 690.0, 712.0, 600.0);
        expect(r.ok, "accepted fit: ok");
        const fs::path dir = root / "accepted" / "2010093_arena_1";
        const auto images = fit::write_fit_images(img, width, height, r, dir, "Cam2010093 arena_1");
        expect(images.ok && images.error.empty(), "accepted fit: images written");
        expect(images.frame_png == (dir / fit::kRimFrameFilename).string() &&
                   images.overlay_png == (dir / fit::kRimOverlayFilename).string() &&
                   images.zoom_png == (dir / fit::kRimZoomFilename).string(),
               "accepted fit: frame, overlay and zoom beside the fit JSON");
        const cv::Mat frame = cv::imread(images.frame_png, cv::IMREAD_UNCHANGED);
        bool identical = frame.type() == CV_8UC1 && frame.cols == width && frame.rows == height && frame.isContinuous();
        if (identical) identical = std::equal(img.begin(), img.end(), frame.ptr<std::uint8_t>(0));
        expect(identical, "rim frame: native resolution, 8-bit grey, lossless (byte-identical)");
        const cv::Mat overlay = cv::imread(images.overlay_png, cv::IMREAD_COLOR);
        expect(!overlay.empty() && overlay.cols == width / 4 && overlay.rows > height / 4,
               "overlay: the view at 1/4 with the legend strip below");
        const cv::Mat zoom = cv::imread(images.zoom_png, cv::IMREAD_COLOR);
        expect(!zoom.empty() && zoom.cols == 1280 && zoom.rows > zoom.cols,
               "zoom: 2 x 2 native-resolution tiles (320 px shown x2) with the key below");
        const auto j = fit::to_json(images);
        expect(j.at("ok") == true && j.at("overlay_png") == images.overlay_png && j.at("error").is_null(),
               "images json: ok, paths, no error");
        bool leftovers = false;
        for (const auto& entry : fs::directory_iterator(dir)) {
            leftovers = leftovers || entry.path().filename().string().find(".tmp") != std::string::npos;
        }
        expect(!leftovers, "atomic writes leave no temporaries");
    }
    {
        // A refused fit (a short arc: the rim mostly outside the frame) still gets its images.
        const int narrow = 900;
        std::vector<std::uint8_t> img = render(narrow, height, 1100.0, 700.0, 600.0);
        const auto r = fit::fit_known_radius(img.data(), narrow, height, 1100.0, 700.0, 600.0);
        expect(!r.ok, "short arc: refused");
        const fs::path dir = root / "refused" / "2010094_arena_2";
        const auto images = fit::write_fit_images(img, narrow, height, r, dir, "Cam2010094 arena_2");
        expect(images.ok && fs::exists(images.frame_png) && fs::exists(images.overlay_png),
               "refused fit: frame and overlay written");
        expect(r.fixed.ok == !images.zoom_png.empty(), "refused fit: zoom written when there is a fixed fit");
    }
    {
        // Mismatched buffer: nothing written, an error named.
        std::vector<std::uint8_t> img(10);
        const fit::Result r;
        const auto images = fit::write_fit_images(img, width, height, r, root / "bad", "bad");
        expect(!images.ok && images.error == "rim_frame_size_mismatch" && !fs::exists(root / "bad"),
               "size mismatch: refused before writing");
    }
    fs::remove_all(root);
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "daily_rim_fit_images_tests passed\n";
    return 0;
}
