#pragma once

#include "calibration_image_set.h"
#include "dish_top_rim_observation.h"
#include "gui/spatial_layout/state.h"

#include <opencv2/core.hpp>

#include <string>

namespace orange::gui::spatial_layout {

struct TopRimObservationSaveJob {
    std::string artifact_root_dir;
    std::string session_dir;
    orange::calibration::DishTopRimObservationRequest request;
    orange::calibration::DishTopRimHoughParams hough_params;
    orange::calibration::DishTopRimCircle accepted_circle;
    cv::Mat source_gray;
};

struct GenericCalibrationImageSetSaveJob {
    std::string artifact_root_dir;
    std::string session_dir;
    std::string image_role;
    std::string image_description;
    std::string capture_filename;
    cv::Mat source_gray;
    orange::calibration::CalibrationImageSetRequest request;
};

bool submit_top_rim_observation_save_job(
    TopRimObservationSaveJob job,
    std::string* error_out);

bool top_rim_observation_save_worker_is_busy();

void poll_top_rim_observation_save_worker(SpatialLayoutUiState* ui_state);

bool submit_generic_calibration_image_set_save_job(
    GenericCalibrationImageSetSaveJob job,
    std::string* error_out);

bool generic_calibration_image_set_save_worker_is_busy();

size_t queued_generic_calibration_image_set_save_job_count();

void poll_generic_calibration_image_set_save_worker(SpatialLayoutUiState* ui_state);

} // namespace orange::gui::spatial_layout
