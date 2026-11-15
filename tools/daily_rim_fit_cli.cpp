// Run the known-radius daily rim fit on a saved 8-bit grey PNG (evidence, dry checks).
//
//   daily_rim_fit_cli <png> <camera_serial> <fish_plane_scale.json> <start_cx> <start_cy> [--points]
//
// Prints the fit JSON (src/daily_rim_fit.h). Read-only: writes nothing.
#include "daily_rim_fit.h"

#include <opencv2/imgcodecs.hpp>

#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    if (argc < 6) {
        std::cerr << "usage: daily_rim_fit_cli <png> <camera_serial> <fish_plane_scale.json> <start_cx> <start_cy> [--points]\n";
        return 2;
    }
    const cv::Mat image = cv::imread(argv[1], cv::IMREAD_GRAYSCALE);
    if (image.empty() || image.type() != CV_8UC1 || !image.isContinuous()) {
        std::cerr << "cannot read an 8-bit grey image: " << argv[1] << "\n";
        return 2;
    }
    namespace fit = orange::daily_rim_fit;
    const fit::KnownRadius known = fit::known_radius_from_scale_file(argv[3], argv[2]);
    if (!known.ok) {
        std::cerr << "known radius: " << known.error << "\n";
        return 2;
    }
    const fit::Config config;
    const fit::Result result = fit::fit_known_radius(image.data, image.cols, image.rows,
                                                     std::atof(argv[4]), std::atof(argv[5]),
                                                     known.radius_px, config);
    nlohmann::json out = fit::to_json(result, config, 1.0 / known.px_per_mm,
                                      argc > 6 && std::string(argv[6]) == "--points");
    out["image"] = argv[1];
    out["camera_serial"] = argv[2];
    out["known_radius"] = {{"inner_diameter_mm", known.inner_diameter_mm},
                           {"wall_height_mm", known.wall_height_mm},
                           {"z_rim_top_mm", known.z_rim_top_mm},
                           {"px_per_mm", known.px_per_mm},
                           {"radius_px", known.radius_px},
                           {"fish_plane_scale_sha256", known.scale_file_sha256}};
    std::cout << out.dump(1) << "\n";
    return result.ok ? 0 : 1;
}
