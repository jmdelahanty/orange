#pragma once

namespace orange {

// Orange's transform between the raster a camera delivers and every frame Orange
// saves, records or calibrates in: none. No Orange code flips, mirrors, rotates or
// transposes a camera frame; recordings encode the delivered raster and calibration
// image sets read the preview texture in camera row order (a downsampled image set
// is scaled, not reoriented; see its image_shape). Written into the camera's
// sensor_pipeline raster_geometry and into the calibration image set's camera block
// so consumers (Citrus's homography mirror-parity check) read it instead of
// assuming it. Change it only together with code that reorients frames.
inline constexpr const char* kRasterTransform = "none";

}  // namespace orange
