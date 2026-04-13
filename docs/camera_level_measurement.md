# Camera Level Measurement Notes

Purpose: explain what "camera level" can mean in Orange, what a single
ChArUco image can and cannot tell us, how board-reference error propagates, and
how intrinsics would be estimated before using pose for quantitative level
measurements.

Date anchored: 2026-04-06.

## Terms

In practice, "level" usually means one of these:

- `image roll`: the image is rotated relative to a reference horizontal or
  vertical direction.
- `camera-to-plane tilt`: the optical axis is not normal to the dish, shelf, or
  calibration-board plane.
- `absolute gravity level`: the camera orientation relative to gravity.
- `cross-camera consistency`: multiple cameras disagree with each other even if
  none of them is gravity-leveled.

These are related, but they are not the same measurement.

## What A Single ChArUco Image Can Tell Us

A single image of a ChArUco board can recover the camera pose relative to the
board plane if camera intrinsics are already known.

Why this works:

- the board geometry is known in board coordinates
- the detected image corners are known in pixel coordinates
- perspective distortion depends on the board orientation relative to the
  camera

The solve is the standard camera model:

```text
u ~ K [R | t] X
```

where:

- `X` is a 3D board point
- `u` is the corresponding image point
- `K` is the intrinsic matrix
- `R, t` are the board-to-camera pose

Once `R` is estimated, the board normal in camera coordinates is known, and
that gives the camera tilt relative to the board plane.

In practice this pose would be solved with `solvePnP` or an equivalent planar
pose routine after corner detection.

Important limit:

- a single image does not tell us tilt relative to gravity by itself
- it tells us tilt relative to the board
- the board only becomes a gravity reference if it was physically leveled
  against gravity

Related limit:

- a single planar image is not the right way to estimate a robust intrinsic
  calibration from scratch
- intrinsics should be estimated first from a multi-image calibration dataset

So the statement should be:

- single ChArUco image + known intrinsics -> pose relative to the board plane
- single ChArUco image + known intrinsics + leveled board -> pose relative to
  gravity

## Why Tilt Is Observable In One Image

If the board is tilted relative to the camera, the corner pattern is not just
translated or rotated in the image. It is foreshortened.

Examples of the signal:

- squares on the farther side project smaller than squares on the nearer side
- equal board spacings become unequal image spacings
- the outer edges of the board are not related by a pure in-plane image
  rotation

Those perspective effects are enough to recover plane orientation when the
board layout is known and intrinsics are fixed.

This is a relative pose estimate. It is not a gravity sensor.

## What Intrinsics Need To Be Known First

Before using a single ChArUco image for level, Orange should estimate
per-camera intrinsics:

- focal lengths `fx`, `fy`
- principal point `cx`, `cy`
- lens distortion coefficients
- optionally skew, though most pipelines keep skew fixed at zero

Typical distortion terms:

- radial: `k1`, `k2`, `k3`, optionally higher order
- tangential: `p1`, `p2`

These parameters are required because distortion and focal length directly
affect the recovered pose. If they are wrong, the tilt estimate can be biased.

## How Intrinsics Would Be Calculated

Intrinsic calibration should use many ChArUco images from different views of
the same board, not a single image.

Recommended capture pattern:

- 15 to 40 frames per camera
- vary distance, in-plane rotation, and out-of-plane tilt
- move the board through the center and edges of the field
- include views where the board occupies different fractions of the frame
- avoid using only nearly fronto-parallel views

For each frame:

1. Detect ArUco markers.
2. Interpolate or refine ChArUco chessboard corners.
3. Match each detected corner to its known board coordinate.

Then solve all frames together by minimizing reprojection error:

```text
min over K, D, {Ri, ti} of
sum over images i and corners j
|| u_ij - project(K, D, Ri, ti, X_j) ||^2
```

where:

- `K` is the intrinsic matrix
- `D` is the distortion parameter set
- `Ri, ti` are the board poses for each calibration image
- `X_j` are the known board points
- `u_ij` are the observed image points

The result is a per-camera intrinsic calibration plus a reprojection error
summary.

Practical validation:

- report RMS reprojection error in pixels
- inspect undistorted corner overlays
- keep a small holdout set of images not used in the fit
- reject or rerun calibrations with obviously biased edge residuals

## How Level Would Be Calculated After Intrinsics

Once intrinsics are known, Orange can estimate camera level relative to a board
plane from one or more new images.

Workflow:

1. Place a ChArUco board on the reference plane.
2. Detect board corners in the image.
3. Solve pose for `R, t`.
4. Convert the board normal into camera coordinates.
5. Derive angle metrics from that pose.

Useful outputs:

- `tilt_deg`: angle between the camera optical axis and the board normal
- `roll_deg`: image rotation relative to the board axes
- `reprojection_error_px`
- confidence or stability over repeated frames

If the board is intended to represent the dish plane, then `tilt_deg` answers
"how square is the camera to the dish plane?"

If the board is independently leveled to gravity, then the same estimate can be
reported as absolute camera level.

## What A Non-Level Shelf Or Holder Means

If the shelf, dish holder, or board mount is tilted by less than 1 degree:

- relative camera-to-board measurements can still be accurate
- absolute gravity-referenced measurements inherit that reference-plane error

The shelf tilt usually hurts absolute accuracy more than relative precision.

A useful error-budget rule is:

```text
absolute_level_error ~= reference_plane_error + pose_error + calibration_error
```

or more conservatively, if treated as independent terms:

```text
total_error ~= sqrt(reference_plane_error^2 +
                    pose_error^2 +
                    calibration_error^2)
```

Example:

- board or shelf known only to `+-1.0 deg`
- image-pose estimate `+-0.2 deg`

Then the gravity-referenced answer is still dominated by the `+-1.0 deg`
reference uncertainty.

So:

- if we only care about camera relative to the dish plane, shelf level does not
  matter much as long as the board is on that same plane
- if we care about camera relative to gravity or water surface, shelf level
  must be measured separately

Scale intuition:

- `1 degree` over `100 mm` corresponds to about `1.75 mm` of height difference

## Recommended Orange Interpretation

For Orange, the cleanest semantics are:

- `camera_to_board_tilt_deg`: image-derived pose relative to the calibration
  board
- `camera_to_dish_tilt_deg`: same quantity when the board sits on the dish or
  dish holder reference plane
- `camera_to_gravity_tilt_deg`: only valid when the board plane has been
  independently leveled or measured against gravity

This avoids overstating what the image alone proves.

## Relationship To Current Repo State

Current `orange-jeremy` already has a ruler-based alignment path that extracts
line angle and center offset from reference images. That is useful for a quick
roll-style check, but it is not a full plane-pose estimate.

The older `orange` repo also contains an ArUco-based pose path:

- marker detection and pose estimation in `src/aruco_nano.h`
- calibration loading and projection math in `src/realtime_tool.cpp`

That older path is closer to the kind of board-based procedure needed for
quantitative level measurement.

## Recommended Next Step

If Orange adds a dedicated camera-level calibration mode, the recommended order
is:

1. Intrinsic calibration per camera from a ChArUco dataset.
2. Pose solve from one or more ChArUco images on the intended reference plane.
3. Repeated-frame summary:
   - median tilt
   - frame-to-frame spread
   - reprojection error
4. Optional external level measurement of the board plane if gravity-referenced
   reporting is needed.
