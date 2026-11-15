# Registration accuracy check

`scripts/registration_accuracy_check.py` answers one question with numbers an agent can read:
do the dish registration and the projection mapping match what the cameras actually see?
It only reads (a guided-capture calibration session, the Citrus canvas and its active
homography pointers, the physical-registration pointers or a rim observation, the fish-plane
scale file) and writes `registration_accuracy.json` plus downsampled overlay PNGs to
`~/orange_data/registration_accuracy/<UTC stamp>/` (or `--out`).

It needs the cameras to see the projector, so the NIR filters must be off. With the filters in
place (production) the projected patterns are invisible; run it at setup or commissioning.

## The capture

One guided capture, filters off, dishes in place with water
(`scripts/run_gui_guided_capture_smoke.sh`):

```bash
scripts/run_gui_guided_capture_smoke.sh --profile wet_tank_projected_surface \
  --recipe-sequence black_reference,uniform_gray,experimental_area_center_and_outline,static_dots \
  --static-dots-scenes <scenes.json> --foreground-gray-u8 64 \
  --calibration-exposure-us 10000 --frame-count 10 --save --record <commissioning record> --execute
```

- `black_reference`: subtracted from every pattern frame.
- `uniform_gray`: lights the dish floor so the rim edge is visible (edge support, overlay).
- `experimental_area_center_and_outline`: Citrus draws each arena's experimental-area circle
  and a full-diameter centre cross (2 canvas px, red).
- `static_dots`: white dots on black at known canvas positions. A grid file:
  `scripts/registration_accuracy_check.py --emit-grid-scenes <scenes.json>` (two scenes,
  centre + 8 at 18 mm + 6 at 32 mm, 3 mm dots), or any static_dots scenes file.
- Exposure: calibration timing is 5 fps and camera 2010096's output drives the IR strobe, so
  keep exposure at or below 16 ms (8 % duty); 10 ms is enough with the filters off.

Then:

```bash
~/miniforge3/envs/juicebox/bin/python scripts/registration_accuracy_check.py --session <calsess id>
```

Frames are found by the Citrus scene `recipe_id` saved with each image, not by image role.

## What it reports, per camera

**A. Projection.** For every drawn dot: its camera position predicted with the inverse of the
active Citrus homography (as `scripts/dot_contrast.py` does), the measured centroid near that
prediction (black-subtracted, intensity-weighted; saturated disks are fine), and the residual in
camera px, canvas px and mm at the projection plane. Pass: every dot found and every residual
<= `--max-residual-mm` (default 1.0 mm: the 3 mm stimulus dots' radius is 1.5 mm, so a 1 mm
error keeps a dot's centre inside its intended disk, and it is about twice the error the
homography fit itself reports). A similarity fit, commanded to measured, about the
experimental-area centre (shift in mm, rotation, scale; an affine fit too with 3 or more spread
points) says what kind of error it is: a pure shift is a translation-only re-registration, a
rotation or scale points at a homography refit.

**B. Dish registration.** For the active physical-registration pointer (or `--rim-observation`):
whether the fit is still fresh (24 h), the edge support of its accepted circle on the session's
`uniform_gray` frame computed exactly as the C++ gate (`orange_dish_rim_auto_accept_v1`: 720
points, strongest 1 px radial step within +/- 6 px, >= 12 DN, pass >= 0.40), the radius against
the physical band (80 mm inner diameter x the fish-plane scale at the rim, +/- 2 %), and the dish
centre mapped through the homography into the canvas against the experimental-area centre.
Without a `uniform_gray` frame the rim is checked on the observation's own source frame, which
says nothing about today's placement (a warning says so).

**C. Crosshair.** The cross centre is found in the image: each arm is a weighted
total-least-squares line fit (black-subtracted), and the centre is their intersection. The
outline circle is an algebraic fit. Reported:

- `dish_placement` (the primary dish-placement number): the fitted rim centre minus the observed
  cross, in camera px and in mm with the fish-plane scale at the rim (also at the film for
  comparison). Both come from the same camera image; no homography is involved. Pass: <=
  `--max-dish-offset-mm` (default 2.0 mm).
- `homography_at_centre`: the observed cross against its homography prediction, in mm, next to
  the dot residuals.
- `outline_vs_rim`: the projected outline against the rim, each in its own plane's mm (the
  outline is on the film, the rim about 9-11 mm higher).

**Today's dish** (`dish.today`): on the `uniform_gray` frame the rim shows two edges, fitted
separately. On 720 rays from a first-pass circle, each radial profile (stopping 8 px inside the
frame, so the frame edge is never taken for a rim) is smoothed over 5 px and its bright-to-dark
steps (>= max(1.5 DN/px, 30 % of the ray's strongest)) are found. The innermost is the floor-edge
candidate, the outermost the wall-top (inner rim at the top) candidate; each set gets its own
RANSAC circle (3 px tolerance, iterative refits). Per fit: inliers, median/max residual (px and
mm), centre, radius, edge support (gate rule) and per-side inliers and median residual (right,
bottom, left, top; a side with none says "NO EDGE POINTS"). `dish.today.parallax` compares the
two centres and radii with the fish-plane model (both circles share one physical centre at
different heights; principal point assumed at the image centre, not measured);
`frame_border_clearance_px/mm` says how close the rim is to each frame edge.
`crosshair.dish_placement` is the floor-edge centre against the observed cross (near the film,
no homography), plus both centres corrected to the film plane with the model as a cross-check.
`dish.registration_vs_today` compares the active registration with the wall-top fit.

Figures:
- `Cam<serial>_dish_rim.png` (every camera with a uniform-gray frame, 1/4 size, legend below):
  the floor-edge fit (green) and the wall-top fit (magenta) with their centres and inlier points,
  the observed crosshair centre with the arrow to the floor-edge centre (cyan, drawn 10x longer)
  and its value in mm, and the active registration's circle where one exists (yellow), labelled
  with its artifact id and "EXPIRED <time>" or "valid until <time>". Sides without edge points
  are named in the legend.
- `Cam<serial>_dish_rim_zoom.png`: 320 x 320 native-pixel crops (shown x2) at the top, right,
  bottom and left, centred between the two edges, with both fits, their inlier points and the
  registration's fit; a crop says "NO ... EDGE POINTS HERE" and "black = outside the frame".
- `Cam<serial>_projection.png` (green: predicted dot, red: measured) and
  `Cam<serial>_crosshair.png` (cyan: fitted arms and outline, red: observed centre, green:
  predicted centre, yellow: the registration's fit, magenta: today's rim).

Caveats:
- The outline is drawn at the experimental-area radius on the film (40.5 mm today). Where it runs
  under the dish wall it is partly hidden, so the outline fit is a weaker number than the cross.
- The gate's edge-support limit (>= 12 DN per px step) was set on the registration workflow's
  own rim frames. On a dimmer frame (uniform gray 64 at 10 ms, 2026-11-15) true rims step only
  2.6-5.6 DN/px and score 0.00-0.22, so a low edge support on this check's frame says nothing
  about a fit; compare `observed_rim` and `registration_vs_today` instead.

## Results, 2026-11-15

Rig capture `calsess_2026_11_15T19_47_21Z_shadow` (filters off, dishes and water in place, IR
lights on, 10 ms; black, uniform gray 64, centre and outline, static_dots
`omnifin0_registration_accuracy_grid9_3mm_v1`: per arena the centre, +/-25 mm on each axis and
4 diagonals at 17.68 mm, 3 mm white dots):

| Camera | dots | residual median / max (mm) | similarity at centre (shift mm, rot deg, scale) | homography at cross (mm) | today's rim vs cross (mm, at rim scale) | today's rim radius |
| --- | --- | --- | --- | --- | --- | --- |
| 2010093 | 9/9 | 0.37 / 0.60 | (-0.17, -0.23), -0.062, 1.0010 | 0.38 | 0.69 | 40.08 mm |
| 2010094 | 9/9 | 0.17 / 0.24 | (0.09, 0.12), 0.062, 1.0031 | 0.16 | 2.35 (0.67, 2.26) | 40.08 mm |
| 2010095 | 9/9 | 0.17 / 0.25 | (0.07, 0.14), 0.045, 1.0031 | 0.18 | 1.51 | 40.11 mm |
| 2010096 | 9/9 | 0.29 / 0.35 | (0.05, 0.27), -0.048, 1.0033 | 0.23 | 1.36 | 40.21 mm |

Today's dish, two edges (output `20261115T1947Z_grid9_crosshair_v3`):

| Camera | floor edge r (inliers, sides without points) | wall top r (inliers, sides without points) | centre diff top - floor, measured / predicted (px) | radius diff, measured / predicted (px) | floor centre vs cross | film-corrected: floor / top vs cross | frame clearance |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 2010093 | 2124.1 px, 39.81 mm (277/717) | 2144.7 px, 40.09 mm (396/717) | (6.8, -21.6) / (-0.2, 0.1) | 20.6 / 5.9 | 0.90 mm | 0.89 / 0.67 mm | left 55 px |
| 2010094 | 2118.0 px, 39.69 mm (177/703; top 6) | 2144.4 px, 40.07 mm (408/703) | (2.0, -31.7) / (0.1, 0.4) | 26.3 / 5.8 | 2.87 mm | 2.88 / 2.31 mm | bottom 1 px |
| 2010095 | 2102.7 px, 39.44 mm (181/720; top 4) | 2132.7 px, 39.89 mm (285/720; left 10, top 5) | (-9.6, -26.1) / (0.1, 0.4) | 30.0 / 5.7 | 2.44 mm | 2.44 / 1.92 mm | bottom 20 px |
| 2010096 | 2131.3 px, 39.98 mm (184/557) | 2154.8 px, 40.31 mm (273/557; right none) | (-0.1, -16.5) / (0.1, 0.0) | 23.4 / 5.8 | 1.52 mm | 1.53 / 1.55 mm | right 47 px |

Inlier residuals are 1.0-1.5 px median, 3 px max, for both edges on every camera. The edges do
not behave like the floor/wall corner and the top of a 2.8 mm wall seen with parallax: the
radius difference is 4-5x the model's, and the floor-edge centre sits 16-32 px toward -y of the
wall-top centre on all four cameras (the model predicts under 0.5 px). A shift in the same
direction in every arena points at the projector's light angle: the floor's lit edge looks like
an illumination edge, not a geometric one. Where the dish is far off-axis (2010094 bottom, near
the frame) the wall shows as a ~90 px dark band, also far wider than a 2.8 mm wall would give.
Neither edge alone is a settled reference yet; the two give placements within 0.6 mm of each
other, and 2010094 and 2010095 are over 2 mm from the projected centre by either.

The October homographies hold to 0.6 mm everywhere; the remaining error is mostly a +0.1 to
+0.3 % scale (the expected refraction term with dishes and water in the light path), with
2010093 also showing about 0.8 deg of affine shear (affine rms 0.16 mm against 0.01-0.03 on the
others). Today's rims measure 40.08-40.21 mm in radius, in line with the calipered 80.15 mm
inner diameter. Dish placement against the projected centre is 0.7-2.4 mm; 2010094's dish sits
2.3 mm toward +y. The only registration on file (2010093, 2026-11-04) is 1.65 mm from today's
rim, so it no longer describes the dish (and it has expired).

Orientation session `calsess_2026_11_15T17_29_47Z_shadow` (2 dots per arena, no uniform gray or
crosshair frame):

`calsess_2026_11_15T17_29_47Z_shadow` (2 dots per arena, no uniform gray or crosshair frame):
dot residuals 0.10-0.46 mm on the October homographies, similarity at the centre within 0.3 mm,
rotation under 0.13 deg, scale +0.2 to +0.5 % (two points: an exact fit, nothing left to judge). The crosshair detector was checked on the
2026-07-21 crosshair frames (`calsess_2026_07_21T16_36_06Z_shadow`): arms at 89.4-89.8 deg,
centre 0.4-1.9 mm from the current homography's prediction (those frames predate the November
projector move).

Tests: `tools/registration_accuracy_check_tests.py` (ctest
`registration_accuracy_check_tests`) on synthetic data: the similarity and affine fits, the dot
centroid, the gate's edge-support rule, the radius band and the crosshair detector.
