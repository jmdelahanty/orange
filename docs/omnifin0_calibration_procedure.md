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

## 5. Wet fish-plane scale

### Principle

Measure the same physical target at two known heights in every camera:

- dry: disk on the gel (done in step 6 above), target occupies z 0 to 3;
- wet: disk on the dish floor under water, target occupies `P_floor` to
  `P_floor` + 3.

The target is `acrylic_hole_target_78mm_pitch5_margin3_v002` (3 mm acrylic,
77 mm measured outside diameter, 5 mm hole pitch, markers `C`, `XPLUS`,
`ASYM`), lit from below through its holes by a uniform-gray projection. The
fit does not say which depth inside the 3 mm disk the hole centres represent.
That offset is the same in both captures, so the dry-to-wet shift is exactly
`P_floor` in every camera, and the disk's mid-plane on the dish floor
(`P_floor` + 1.5) coincides with a mid-column fish.

### Capture

Physical state: holder, gels, dishes installed; one disk flat on the floor of
each dish, markers visible; water about 4 mm deep (covers the disk); no air
bubbles under the disk; water settled. Record which dish type is in each
arena and the water depth.

Use the dry scale capture's settings so the two heights are compared like for
like (gray 76, 100 ms, 5 fps). Purpose `validation_pattern`, so nothing is
fitted, loaded or promoted in Citrus:

```bash
# dry preview (prints the plan, touches nothing)
scripts/run_gui_guided_capture_smoke.sh --profile wet_tank_projected_surface \
  --recipe-sequence black_reference,uniform_gray --purpose validation_pattern \
  --foreground-gray-u8 76 --calibration-exposure-us 100000 --frame-count 10 --save
# then the same with --execute
```

Labelling limit (2026-11-13): this saves the images as wet projection-surface
validation captures (`projected_surface_wet_runtime_stack`). Orange's schema
has the correct label, `camera_only_physical_target_calibration` at
`camera_physical_dish_base_inner_surface` (plane
`dish_base_inner_surface_physical`), but its GUI preset assumes NIR strobe
light, R72 filter, projector off and a dry dish, and the launcher cannot
select it. The physical truth (disk on dish floor, heights, water depth)
therefore goes into the evidence record below, which points at the sessions
by id and image checksum. Follow-up: a launcher option for the capture stage
and a wet, projector-backlit physical-target preset.

### Fit and result

Offline, with the code that produced the dry scale
(`orange::gui::projected_surface_scale::analyze`, same target definition, same
quality thresholds: >= 80 matched points, fit RMS <= 0.15 mm, holdout RMS
<= 0.25 mm, `C`-`XPLUS` 25 mm within 0.30 mm, anisotropy <= 3 %, outside
diameter within 1.5 mm), no canvas homography. Per camera this gives
`s_wet` (camera px/mm) beside the dry `s_dry`. Pinhole model: `1/s` is
linear in height, so

```
k      = P_floor / (1/s_dry - 1/s_wet)              # per camera
1/s(z) = 1/s_wet - (z - P_floor - e) / k            # e = effective depth inside the disk, 0..3
s_fish = s(P_fish)                                  # e = 1.5 -> s_fish ~= s_wet
```

Uncertainty: `e` (0 to 3 mm) gives at most +/- 1.5 mm of height at the fish,
about +/- 0.45 % in scale; `P_floor` (+/- 0.1 mm if the CAD holds, more if the
winglet assumption is wrong) moves `k` proportionally, which shifts `s_fish` very little because `s_wet` is measured at
the fish height. Refraction through about 1 mm of water over the disk shifts
the apparent height by about 0.25 mm and is ignored.

Acceptance: every gate above passes in all four cameras, `s_wet > s_dry` in
every camera, and the four `k` agree with each other within their
uncertainty (the cameras are at nominally equal heights). Anything else stops
for Jeremy. The fish-plane scale is not loaded into Citrus; it is an Orange
commissioning product until the camera-only physical-target calibration is
implemented.

## 6. Brightness maps

A brightness map is a commissioning artifact: per camera, the
black-subtracted uniform-gray image inside the arena, at four gray levels
(24 / 64 / 128 / 255 with exposures 100 / 9 / 2 / 0.4 ms), recipe sequence
`black_reference,uniform_gray`, 10 frames:

```bash
for pair in 24:100000 64:9000 128:2000 255:400; do g=${pair%%:*}; e=${pair##*:}
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
  Facing arena edges agree within about 4 % across cameras (ratios 1.04, 1.03,
  0.96), so the non-uniformity is the projector, not the cameras.
- Wet (`wet_tank_projected_surface`, holder, gels, dishes and water at
  recording fill, no disks): this is what the fish see. Compare with the dry map
  per arena; the difference is the dish and water path.

Analysis: block means (12 x 12 per camera) of `uniform - black` inside the lit
area, normalized to the camera median; report median level, saturated
fraction, min, max, max/min and the shape difference against gray 255. The
analysis scripts used on 2026-11-13 move into the repo with the brightness-map
artifact type (follow-up), and the maps then travel with each recording
through the commissioning release.

## 7. Evidence record

Every wet product gets a folder
`~/orange_data/calibrations/commissioning/<product>_<UTC timestamp>/`
(as the intensity reports do), holding `commissioning_report.json` with:

- the session ids and image sha256 values it was computed from;
- the physical state: dish type per arena, water depth, the plane heights
  with their sources (section 1) and the rig record commit;
- the fit outputs per camera, the gates and their values, overlays;
- the reviewer verdict (Claude or Jeremy) and the time.

Calibration sessions and the evidence folders are never edited after they
are written.

## 8. Open items

- `P_floor` depth-gauge measurement and the winglet underside relative to the
  floor top (no dish CAD exists); meaning of the 2.8 mm "tank rim"; dish names
  (Citrus vs Jeremy); fish eye height. Tracked in rig-designer's record.
- Launcher capture-stage option and wet physical-target preset (section 5).
- Brightness-map artifact type and its place in the commissioning release.
- Citrus flat-field correction design (after the wet map).
- Camera vignetting measurement (separates camera fall-off from projector).
- Lens focal length and working distance per camera (would replace the
  0.3 %/mm assumption with a prediction to check `k` against).
