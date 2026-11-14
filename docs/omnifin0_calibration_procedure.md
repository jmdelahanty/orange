# omnifin0 calibration procedure (rig-specific)

How the omnifin0 rig (pancake0, four cameras 2010093..96 = arenas 1..4) is
commissioned: what is measured, at which physical height, with which
commands, and where the evidence goes. Written 2026-11-13 during the
recommissioning after the projector move. The generic contracts stay in
their own docs; this note says how they apply to this rig and adds the wet
steps they do not cover.

Geometry authority: rig-designer's record of the imaging stack,
phd-projects `rigs/evidence/rig-odmz/20261113_omnifin0_imaging_stack.md`
(commit f6f1089, with Jeremy's sketch `20261113_stack_sketch_jeremy.png`
beside it; the caliper evidence is `20261113_dish_gel_calipers.md`). If a
number below disagrees with that record, the record wins and this note is
stale.

## 1. The imaging stack and its planes

Bottom to top (Jeremy, 2026-10-20 and 2026-11-13, sketch
`~/Pictures/Screenshots/Screenshot from 2026-11-13 08-07-07.png`):

1. Imaging shelf, clear acrylic.
2. Diffusive gel, pressed flat onto the bottom of the holder cup. This is the
   projection surface: the projected image forms here.
3. The cup: water between the gel and the bottom of the dish.
4. The dish ("tank") base, 3 mm or 5 mm acrylic.
5. The fish water, 3 mm deep at recording fill.

The dish hangs from winglets that rest on top of the holder. The fish water
therefore sits at the same height whatever the base thickness; a thicker
base only displaces cup water. No air anywhere between shelf and fish.

Heights, z upward from the top surface of the gel (z = 0):

| Plane | z (mm) | Source |
|---|---|---|
| Gel top (projection surface) | 0 | definition; gel 0.06 mm thick (calipers), water film under it about as thin (declared) |
| Dish floor top `P_floor` | 8.13 +/- 0.1 | derived: cup depth 8.19 (CAD, no tolerance) minus gel 0.06, winglet underside flush with the floor top (implied by the sketch, unconfirmed). Not measured |
| Fish `P_fish` | about 9.6 (range 8.13 to 11.13) | declared: mid-column of 3 mm water |
| Water surface | about 11.1 | `P_floor` + 3 mm fill |
| Water under the dish | about 5.1 (3 mm base), 3.1 (5 mm base) | derived; irrelevant to the cameras |

Measured 2026-11-13: gel 0.06 mm, bases 3 mm and 5 mm, a "tank rim" of
2.8 mm (dimension not yet assigned). `P_floor` is settled by one depth-gauge
measurement with a dish in place: holder top to dish floor top, and holder top
to gel; the difference is `P_floor`. The GUI's camera-physical dish-base preset
already assumes 8.0 +/- 0.5 mm (`calibration_workflow.cpp`), consistent with
the estimate.

Dish names are unresolved: Citrus's dish files call the 3 mm base `palm1` and
the 5 mm base `palm2` (arenas 1-2 palm1, 3-4 palm2); Jeremy calls the 3 mm one
palm2. Camera scale does not depend on it (one floor height for every dish).

## 2. What each commissioning product is valid for

| Product | Plane | Valid for |
|---|---|---|
| Reference homography, holder operational homography | gel (z = 0) | canvas pixel <-> camera pixel on the projection surface: stimulus placement |
| Projected-surface scale (52.82-52.86 camera px/mm, 2026-11-13) | target in contact with the gel | millimetres on the projection surface |
| Fish-plane scale (section 5) | `P_fish` | fish positions and speeds in millimetres |

The camera sees the fish about 9.6 mm closer than the gel. With an assumed
~50 mm lens about 40 cm away (not verified) the scale grows about 0.3 % per
mm of height, so the gel scale is about 3 % too small at the fish, about
1.2 mm at the edge of an 80 mm dish. The error is a radial magnification
about the camera's nadir point, so it is also a position error between a
fish and the stimulus under it. That is why the fish plane needs its own
measurement.

## 3. Session rules (every commissioning or recommissioning session)

- Back up the app config and set `"image_levels_check": {"enabled": false}`;
  Jeremy unplugs the IR (PDU outlets 7/8). Restore both after the last run.
  Reason and history: `docs/dry_shelf_center_first_commissioning_2026-09-14.md`,
  "Before a recommissioning session".
- NIR filters off, projector Brightness 50.
- Stray light: room lights off, desktop monitor facing away, behavior box
  closed (Jeremy, 2026-11-13). At codes 0-30 stray light is the same order as
  the signal; the record states the light state for every capture.
- HOLD to the other agents before the first capture, CLEAR after the last.
- Claude reviews each fit; the runner accepts, promotes and finalizes when every
  gate passes and the evidence is kept. Any failure stops for Jeremy. Physical
  changes (holder, gels, dishes, disks, water, IR plug) are Jeremy's.
- Before a scripted run, make sure the Spatial Layout window is not collapsed
  (`imgui.ini`, `Collapsed=0`); a collapsed window stalls group capture.

## 4. Dry sequence (done 2026-11-13, release `..._recommission_after_projector_move`)

1. Bootstrap arena centres, dry shelf, no holder
   (`docs/dry_shelf_center_first_commissioning_2026-09-14.md`).
2. Projector intensity sweep (`docs/projector_intensity_commissioning.md`).
   At Brightness 50 grays 32-96 saturate; gray 24 passes.
3. Reference homography fit and accept.
4. Holder installed, gels in, dishes out: operational homography fit and
   promote (`scripts/run_holder_fixture_validation.py --execute
   --confirm-holder-installed-dish-absent`; `docs/holder_fixture_validation.md`).
5. Dry brightness map (section 6), holder and gels, no disks.
6. Projected-surface scale: one 3 mm disk on each gel
   (`docs/projected_surface_scale_commissioning.md`).
7. Finalize the Citrus commissioning release; `orange_session_dirs` must list
   the reference session.

## 5. Fish-plane scale (disk on the dish floor, dish dry)

### Principle

Measure the same physical target at two known heights in every camera:

- on the gel (done in step 6 above): target occupies z 0 to 3;
- on the dish floor: target occupies `P_floor` to `P_floor` + 3.

The target is `acrylic_hole_target_78mm_pitch5_margin3_v002` (3 mm acrylic,
77 mm measured outside diameter, 5 mm hole pitch, markers `C`, `XPLUS`,
`ASYM`), lit from below through its holes by a uniform-gray projection. The
fit does not say which depth inside the 3 mm disk the hole centres represent.
That offset is the same in both captures, so the shift between them is
exactly `P_floor` in every camera, and the disk's mid-plane on the dish floor
(`P_floor` + 1.5) coincides with a mid-column fish.

The dish stays dry (decided 2026-11-13). The winglets set the floor height
wet or dry, and the cameras look down, so water under the dish does not
change what they see of the target. Water over a fish raises its apparent
height by about a quarter of the water above it (about 0.4 mm for a fish
1.5 mm deep), about 0.1 % in scale; that is applied by calculation. Dry also
avoids bubbles under the disk and a floating disk.

### Capture

Physical state: holder, gels and dishes installed, dishes empty and dry, one
disk flat on the floor of each dish, markers visible. Record which dish type
is in each arena.

Settings as the gel capture so the two heights compare like for like
(uniform gray under the target at the commissioned gray, 100 ms, 5 fps). The
launcher saves it as `camera_physical_dish_base_inner_surface` (plane
`dish_base_inner_surface_physical`, target plane `tank_bottom_inner_surface`)
with the projector-backlit lighting and the height from the rig record:

```bash
scripts/run_gui_guided_capture_smoke.sh --profile holder_installed_projected_surface \
  --recipe uniform_gray --save --targets-ready \
  --physical-target-stage camera_physical_dish_base_inner_surface \
  --physical-target-plane-z-mm 8.13 --physical-target-plane-z-uncertainty-mm 0.1 \
  --physical-target-plane-z-source "phd-projects f6f1089 rigs/evidence/rig-odmz/20261113_omnifin0_imaging_stack.md" \
  --record "$ORANGE_COMMISSIONING_RECORD" --execute
```

Use the measured `P_floor` and its rig-record commit once the depth gauge
reading exists. Captured metadata is never edited afterwards.

### Fit and result

`targets/release/physical_target_fit_cli --session-dir <session> --camera
<serial> --out <dir>` (build with `make physical_target_fit_cli`), once per
camera for the gel session and the dish-floor session. It runs the code that
produced the gel scale: Orange's `projected_surface_scale::analyze` (same
target definition and gates: >= 80 matched points, fit RMS <= 0.15 mm,
holdout RMS <= 0.25 mm, `C`-`XPLUS` 25 mm within 0.30 mm, anisotropy <= 3 %,
outside diameter within 1.5 mm) and then Citrus's refit of those
correspondences (RANSAC homography, scale at the origin), which is the
accepted value: on the 2026-11-13 gel capture it reproduces the four accepted
scales exactly, while `analyze` alone differs by 0.02-0.03 px/mm. No canvas
homography. `scripts/fish_plane_scale.py --pair SERIAL,GEL_REPORT,FLOOR_REPORT
... --p-floor-mm 8.13 --p-floor-unc-mm 0.1 --p-floor-source "..."` combines
them. Per camera this gives `s_floor` (camera px/mm) beside the gel value
`s_gel`. Pinhole model: `1/s` is linear in height, so

```
k      = P_floor / (1/s_gel - 1/s_floor)            # per camera
1/s(z) = 1/s_floor - (z - P_floor - e) / k          # e = effective depth inside the disk, 0..3
s_fish = s(P_fish + apparent-height shift of the water over the fish)
```

With `e` = 1.5, `s_fish` is about `s_floor` plus the 0.1 % water term.
Uncertainty: `e` (0 to 3 mm) gives at most +/- 1.5 mm of height at the fish,
about +/- 0.45 % in scale; `P_floor` (+/- 0.1 mm if the CAD holds) moves `k`
proportionally, which shifts `s_fish` very little because `s_floor` is
measured almost at the fish height.

Acceptance: every gate passes in all four cameras, `s_floor > s_gel` in every
camera, and the four `k` agree within their uncertainty (the cameras are at
nominally equal heights). Anything else stops for Jeremy. The fish-plane
scale is not loaded into Citrus; it is an Orange commissioning product until
the camera-only physical-target calibration is implemented.

## 6. Brightness maps

A brightness map is a commissioning artifact: per camera, the
black-subtracted uniform-gray image inside the arena, at several gray levels,
recipe sequence `black_reference,uniform_gray`, 10 frames. The dry map used
24 / 64 / 128 / 255 at 100 / 9 / 2 / 0.4 ms. The wet map adds gray 30 at
60 ms, the experiment's background code, because the dot experiments live at
codes 0-30 where the projector's black floor is a large share of the signal
and the "same shape at every gray" result must be checked there. The
`black_reference` image of each pair is the code-0 map (projector black floor
plus camera dark level):

```bash
for pair in 24:100000 30:60000 64:9000 128:2000 255:400; do g=${pair%%:*}; e=${pair##*:}
  scripts/run_gui_guided_capture_smoke.sh --profile <profile> \
    --recipe-sequence black_reference,uniform_gray --fixture-aperture-shape circle \
    --foreground-gray-u8 $g --calibration-exposure-us $e --frame-count 10 --save --execute
done
```

- Dry (`holder_installed_projected_surface`, holder and gels, no disks), done
  2026-11-13: sessions `calsess_2026_11_13T00_36_37Z`, `T00_37_24Z`,
  `T00_38_10Z`, `T00_38_56Z`. Within-arena max/min 2.0-2.3x, the same shape at
  all four grays (within 2-7 %), hot spot near the top centre of the projected
  image, arenas 2 and 3 about 25 % brighter than 1 and 4, gamma about 2.2.
  Facing arena edges agree within about 6 % across cameras
  (`scripts/brightness_map.py`: 1.06 / 0.99 / 0.95 for pairs 1|2, 2|3, 3|4,
  3-8 mm strips; up to ~5 % more with the strip position; the hand numbers that
  night were 1.04 / 1.03 / 0.96), so the non-uniformity is mostly the
  projector, not the cameras.
- Wet (`wet_tank_projected_surface`, holder, gels, dishes and water at
  recording fill, 3 mm, no disks, NIR filters off): this is what the fish see.
  Compare with the dry map per arena; the difference is the dish and water
  path. Its projection-surface labels are correct with the existing launcher.

Analysis: `scripts/brightness_map.py --session <id> ... --out <dir>` (juicebox
Python): block means (12 x 12 per camera) of `uniform - black` inside the lit
area, normalized to the camera median; median level, saturated fraction, min,
max, max/min, the shape difference against the highest gray, each camera
relative to the others, and the facing-edge ratios. Output
`orange.commissioning.brightness_map` v1 plus PNGs; attach it to the record
(`commissioning_record.py attach --role brightness_map`). Carrying the maps
with each recording through the commissioning release is a follow-up.

## 7. Stimulus dot contrast (wet)

What the cameras see of the experiment's dots through the full wet stack.
Design from experiment-spec (orchard spec `omnifin0-dot-contrast-corners`,
draft; the scene is Jeremy's to approve): background code 30 over each
arena, canvas outside the arenas black; four stationary hard-edged dots per
arena, 10 mm diameter, at the Citrus chaser presets top_left / top_right /
bottom_left / bottom_right, about 21.6 mm from the dish centre; dot codes 0,
20, 27 and a sham 30 (design Weber -0.87, -0.52, -0.18, 0), rotated over the
corners as a Latin square; Brightness 50, 240 Hz.

Sweep, in priority order: (a) the four codes at each corner in every arena
(one Latin square, four static frames); (b) each dot alone against all four
together at one corner (cross-talk, at least code 0); (c) a finer ladder at
one corner per arena (0, 8, 16, 20, 24, 27, 28, 29, 32, 36, 40); (d) the
background alone at 30 and at 0 (covered by the wet brightness map).

Measurement per dot: black-subtracted mean inside the dot, inset from the
edge by a stated margin, and in an annulus of background around it; Weber
contrast `(I_dot - I_bg) / I_bg` from the local annulus, not the arena mean.
Exposure and gain fixed and stated; capture session ids recorded. The
contrast is in the camera's spectral response, not the fish's.

Rendering: the Citrus calibration scene `static_dots` (Citrus abf1da6,
rig-installed 2026-11-13; options and refusals in Citrus
`docs/citrus_gui_automation_control.md`). A protocol run is ruled out: it is
a recording (H5, binding, freeze and transfer rules), its step clock cannot
wait for a capture, and it draws the photodiode marker. Orange's guided
capture has a `static_dots` recipe: `--static-dots-scenes <file>` (a JSON
`{"scenes": [{"label", "scene_options"}]}`) runs `black_reference` then one
`static_dots` capture per scene in one session, each after Citrus's
presentation fence, and saves each with image role `stimulus_dot_contrast`,
the scene options in the capture membership and Citrus's scene status (drawn
centres and radii) before and after. The record step is
`static_dots_<profile>_<file stem>` and keeps a copy of the scenes file.
Jeremy approves the exact scenes file before it is shown. The first file is
`~/orange_data/calibrations/commissioning/static_dots_scenes/omnifin0_dot_contrast_corners_latin_v1.json`
(background only, then the four Latin-square rows of sweep (a)):

```bash
scripts/run_gui_guided_capture_smoke.sh --profile wet_tank_projected_surface \
  --static-dots-scenes ~/orange_data/calibrations/commissioning/static_dots_scenes/omnifin0_dot_contrast_corners_latin_v1.json \
  --calibration-exposure-us 60000 --frame-count 10 --save --record "$ORANGE_COMMISSIONING_RECORD" --execute
```

Black floor: the dots' contrast depends on the projector's black floor (code
0 emits about 0.13 % of white, about 13 % of the code-30 background). A dark
frame with the projector light blocked (card over the output, projector
settings unchanged; same exposures and gain as the scene captures) separates
it from the camera dark level: `code 0 - dark` is the black-floor map,
`code 30 - dark` the background. Capture with the operator's card over the
projector output:

```bash
scripts/run_gui_guided_capture_smoke.sh --profile wet_tank_projected_surface \
  --recipe-sequence black_reference --projector-light-blocked \
  --calibration-exposure-us 60000 --frame-count 10 --save --record "$ORANGE_COMMISSIONING_RECORD" --execute
```

(and again at 100000 us), saved with image role
`dark_frame_projector_light_blocked`.

## 8. Evidence record

Every commissioning session writes one record folder under
`~/orange_data/calibrations/commissioning/` with `scripts/commissioning_record.py`
(layout in `docs/commissioning_record_plan_2026-11-13.md` section 3):

```bash
export ORANGE_COMMISSIONING_RECORD=$(scripts/commissioning_record.py open --rig omnifin0 \
  --reason "wet commissioning" --operator jeremy --agent claude:<session> | jq -r .record_dir)
scripts/commissioning_record.py declare ir_illuminators=unplugged nir_filters=off \
  room_lights=off monitor=facing_away box=closed projector_brightness=50 \
  image_levels_check=disabled water_fill_mm=3 dish_arena_1=<type> ... \
  --source "Jeremy at the rig" --by jeremy
# every launcher run appends a step (passed or failed) through the variable
scripts/commissioning_record.py verdict --step N --reviewer claude --verdict accepted \
  --note "<gates>" --evidence <overlays reviewed>
scripts/commissioning_record.py attach --role brightness_map <brightness_map out>/*
scripts/citrus_commissioning_finalize.py finalize --transaction-id <id> --accept   # when a release is due
scripts/commissioning_record.py close [--release-manifest <commissioning.json>]
scripts/commissioning_record.py verify --record <dir>
```

Declare every physical change as it happens; each step snapshots the declared
state. The 2026-11-13 dry session predates the tool; its folder
`recommission_20261113_omnifin0/` was assembled by hand. Calibration sessions
and closed records are never edited after they are written.

## 9. Open items

- `P_floor` depth-gauge measurement and the winglet underside relative to the
  floor top (no dish CAD exists); meaning of the 2.8 mm "tank rim"; dish names
  (Citrus vs Jeremy); fish eye height. Tracked in rig-designer's record.
- The first rig run of the `static_dots` recipe and the new launcher options
  (the sudo GUI wrapper must allow every launcher env key: the 2026-11-13
  holder check was refused until 436fcfc), PDU switching of the IR outlets:
  plan
  `docs/commissioning_record_plan_2026-11-13.md` section 6.
- Brightness-map artifact type and its place in the commissioning release.
- Citrus flat-field correction design (after the wet map).
- Camera vignetting measurement (separates camera fall-off from projector).
- Lens focal length and working distance per camera (would replace the
  0.3 %/mm assumption with a prediction to check `k` against).
