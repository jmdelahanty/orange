#pragma once

// Schema identity of orange.calibration.dish_top_rim_observation, without the OpenCV-dependent
// observation API, so readers (e.g. the recording geometry resolver) compare against the same
// constant the writer uses.
namespace orange::calibration {

inline constexpr const char* kDishTopRimObservationSchemaId =
    "orange.calibration.dish_top_rim_observation";
inline constexpr int kDishTopRimObservationSchemaVersionV1 = 1;
inline constexpr int kDishTopRimObservationSchemaVersion = 3;  // v3: *_stream_frame_id, source_image_index

}  // namespace orange::calibration
