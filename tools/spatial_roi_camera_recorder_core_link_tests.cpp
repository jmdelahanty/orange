#include "spatial_roi_camera_recorder_stream_core.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>

int main()
{
    using orange::spatial_roi::recording::
        SpatialRoiCameraRecorderStreamCoreConfig;
    using orange::spatial_roi::recording::
        SpatialRoiConcreteCameraRecorderStreamCore;

    SpatialRoiCameraRecorderStreamCoreConfig config;
    std::string error;
    auto core = SpatialRoiConcreteCameraRecorderStreamCore::Create(
        std::move(config), &error);
    if (core || error.empty()) {
        std::cerr
            << "spatial_roi_camera_recorder_core_link_tests: invalid empty "
               "configuration was not rejected\n";
        return EXIT_FAILURE;
    }
    std::cout << "spatial_roi_camera_recorder_core_link_tests: PASS\n";
    return EXIT_SUCCESS;
}
