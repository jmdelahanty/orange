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

**Today's dish: the wall top is the reference** (Jeremy 2026-11-15: "the floor is harder to
capture accurately, and in each of the fits so far the top is the only reliable one"). On the
`uniform_gray` frame, 720 rays from a first-pass circle are sampled right up to the frame. Edges
come from a derivative-of-Gaussian operator (sigma 2 px, half-width 6 px, sub-pixel parabola);
a step must have 6 real samples on both sides, so an edge is measurable down to 6 px from the
frame (the only border exclusion). The step threshold is relative to each ray's own floor-to-dark
contrast (max(0.4 DN/px, 1.2 % of the contrast per px)), so a dim, vignetted side still counts.

- THE measurement: a centre-only fit of the wall top with the radius fixed at the calipered
  inner diameter (80.15 +/- 0.3 mm) x the fish-plane scale at the wall top; each ray contributes
  its step nearest that radius (within 15 px); Gauss-Newton with 3 x MAD trimming; bootstrap
  1-sigma centre. It reports arc coverage and, for every ray without a point, why: the rim is
  outside the frame, within the 6 px operator half-width of the frame, no edge was found, or an
  edge was found but not near the fit; and the median step strength and floor-to-dark contrast
  per side (vignetting shows there).
- Cross-check: the free wall-top circle (outermost step per ray, RANSAC), with its own arc
  coverage and per-side inliers; the fixed-vs-free centre difference is reported.
- `--floor-diagnostic` adds the lit floor's edge (free and fixed radius) and the floor/top
  parallax check. Off by default; never in the placement, the tables or the pass/fail.

**Dish placement** (`crosshair.dish_placement`): the fixed-radius wall-top centre against the
observed cross, in mm at the wall-top scale with a film-corrected value and its 1-sigma; the free
wall top is the cross-check. Both measured in the same camera image; no homography involved.

**Compare mode** (staged results, before and after a daily registration):

```bash
~/miniforge3/envs/juicebox/bin/python scripts/registration_accuracy_check.py \
  --before <calsess, base geometry> --after <calsess, daily registration selected> \
  [--daily-registration <citrus .../daily_registration_runtime_selection.json or registration.json>]
```

Runs the check on both sessions (`before/`, `after/`) and writes `registration_compare.json` and
`Cam<serial>_compare.png`. Per camera: (a) BEFORE dish vs cross with its uncertainty; (b) the
APPLIED translation from the registration file (canvas px, mm, and the cross's move in camera
px) and, separately, what the after-session's scene status shows (the experimental-area centre
change; if the status lacks it, the report says so and assumes nothing); the predicted AFTER
offset; (c) AFTER dish vs cross; (d) the residual against `--max-after-mm` (default 0.5 mm: about
2 canvas px) and its noise floor (the fixed wall-top fit's bootstrap centre sigma and the 0.5
canvas px rounding of an integer translation), plus the spread to the cross-checks as the
systematic term; the dot and centre-homography residuals before and after. The figure shows
before | after: the crosshair frame at 1/8 and, below each, a 480 x 480 camera-px inset around
the cross shown at 0.75x with the arrows and a 1 mm bar to that same scale (no exaggeration).

Figures:
- `Cam<serial>_dish_rim.png` (every camera with a uniform-gray frame, 1/4 size, legend below):
  the fixed-radius wall-top fit (white, THE measurement) with its centre, the free wall-top fit
  (magenta, cross-check) with its inlier points, the observed crosshair centre with the arrow to
  the dish centre (cyan, drawn 10x longer, labelled so) and its value +/- 1-sigma in mm, and the
  active registration's circle where one exists (yellow), labelled with its artifact id and
  "EXPIRED <time>" or "valid until <time>". Sides without points are named in the legend. The
  floor edge (green) appears only with `--floor-diagnostic`.
- `Cam<serial>_dish_rim_zoom.png`: 320 x 320 native-pixel crops (shown x2) at the top, right,
  bottom and left (one always falls on the side nearest the frame), with both wall-top fits, their
  points and the registration's fit; a crop says "NO ... EDGE POINTS HERE" and "black = outside
  the frame".
- `Cam<serial>_projection.png` (green: predicted dot, red: measured) and
  `Cam<serial>_crosshair.png` (cyan: fitted arms and outline, red: observed centre, green:
  predicted centre, yellow: the registration's fit).

Caveats:
- The outline is drawn at the experimental-area radius on the film (40.5 mm today). Where it runs
  under the dish wall it is partly hidden, so the outline fit is a weaker number than the cross.
- The gate's edge-support limit (>= 12 DN per px step) was set on the registration workflow's
  own rim frames. On a dimmer frame (uniform gray 64 at 10 ms, 2026-11-15) true rims step only
  2.6-5.6 DN/px and score 0.00-0.22, so a low edge support on this check's frame says nothing
  about a fit; compare the wall-top fits and `registration_vs_today` instead.

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

Dish, wall top (output `20261115T1947Z_grid9_crosshair_v4`):

| Camera | fixed-radius wall top: points, arc, resid median, centre 1-sigma | coverage lost (why) | weakest side (step, x median, contrast) | DISH vs cross | free wall top (cross-check), its arc |
| --- | --- | --- | --- | --- | --- |
| 2010093 | 718, 360 deg, 2.7 px, 0.30 px | 1 deg: 2 single rays not near the fit | left 2.3 DN/px (x0.65, 42 DN) | 0.73 +/- 0.01 mm | 0.74 mm, 251 deg |
| 2010094 | 691, 346.5 deg, 3.1 px, 0.31 px | bottom 83.8-96.2 deg: 5 deg outside the frame, 5 deg within 6 px of it, 3 deg no edge; 1.5 deg single rays | left 2.5 DN/px (x0.55, 78 DN) | 2.36 +/- 0.01 mm | 2.43 mm, 253 deg |
| 2010095 | 695, 360 deg, 5.2 px, 0.54 px | 12.5 deg not near the fit, mostly 144-162 deg | right 2.3 DN/px (x0.50, 78 DN) | 1.59 +/- 0.01 mm | 1.07 mm, 177 deg (no bottom points) |
| 2010096 | 690, 360 deg, 2.3 px, 0.27 px | 15 deg single rays not near the fit, 213-280 deg | right 1.1 DN/px (x0.58, 38 DN) | 1.37 +/- 0.00 mm | 1.14 mm, 239 deg (no left points) |

2010096's right side (dim floor, 43 DN, and a 90 px gradual ramp) was missing before the
contrast-relative threshold; it now has points. 2010094's fitted wall-top circle runs about 2 px
below the frame at the bottom, where only a bright line beyond the wall's dark band reaches the
frame edge: that 13.5 deg is the one remaining gap with no fit points, and the crops show the
fit following the same outer edge on the other three sides. The free wall top misses whole
sides on 2010095 and 2010096, so it is a cross-check only; the fixed-radius fit is the
measurement. The placements agree with v3 within 0.03 mm.

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
