# Guided Orange/Citrus GUI Capture Smoke

The guided capture smoke exercises the real Orange GUI render loop, camera
acquisition workers, Citrus stimulus display, and the named-scene
local-control transaction without simulating mouse clicks.

Orange uses its normal GUI autorun to open cameras and begin streaming. A
calibration-specific autorun then:

1. imports the configured Citrus canvas;
2. selects an explicit camera scope and named scene recipe;
3. transactionally suppresses the mapped NIR strobe and applies the requested
   calibration timing to only those cameras;
4. calls the same grouped-capture controller as the GUI button;
5. waits for Citrus' shared presentation fence;
6. captures fresh full-resolution camera frames;
7. verifies the same scene revision and content fingerprint after capture;
8. waits for Citrus to restore the prior scene;
9. restores the original camera timing and mapped strobe state;
10. optionally saves schema-valid grouped calibration image sets;
11. stops streaming, writes a result JSON, and closes Orange.

The default command is a dry run:

```bash
scripts/run_gui_guided_capture_smoke.sh
```

Run the safe, non-persistent four-camera black-reference smoke with:

```bash
scripts/run_gui_guided_capture_smoke.sh --execute
```

`black_reference` intentionally uses the diagnostic-only purpose
`diagnostic_black_reference` and does not save a calibration image set. This
prevents a transport/lifecycle smoke from being mistaken for physical
calibration evidence.

Named profiles choose the fixture-aware Citrus scene and Orange metadata as a
unit. For holder-removed rectangular commissioning evidence:

```bash
scripts/run_gui_guided_capture_smoke.sh \
  --execute \
  --profile unobstructed_canvas_commissioning \
  --cameras 2010093 \
  --save
```

For a dry holder-installed, dish-absent circular-support capture:

```bash
scripts/run_gui_guided_capture_smoke.sh \
  --execute \
  --profile holder_installed_projected_surface \
  --cameras 2010093 \
  --save
```

The unobstructed profile requests Citrus `homography_grid`; the holder and wet
tank profiles request `homography_rings`; daily installed-tank registration
requests `experimental_area_center_and_outline`. Explicit `--recipe` and
`--purpose` options remain available for diagnostic overrides.

Projected grid/ring/dot scenes also accept `--foreground-gray-u8`. This sets
opaque RGB (`R=G=B=value`, `A=255`); it does not attenuate alpha. Guided
capture defaults to 100 ms exposure at 5 FPS and records applied readback plus
restore status in the result JSON. By default the launcher derives a temporary
camera config at that timing before camera startup, so all cameras establish
their shared PTP gate cadence together. The live preflight does not rewrite a
camera value when its hardware readback already matches. It waits one second so
queued startup frames cannot become calibration evidence. The timing can be changed with
`--calibration-exposure-us` and `--calibration-frame-rate-hz`. Disabling the
preflight requires the explicit diagnostic option `--no-calibration-preflight`.
`--no-start-at-calibration-timing` is an additional diagnostic escape hatch; it
does not provide the same simultaneous exposure-phase guarantee.

`--sweep-foreground-grays-u8 80,96,112 --sweep-repeats 5` performs all grouped
captures in one Orange/Citrus invocation. When saving is enabled, all samples
append to one calibration session and the result JSON records each
sample's workflow, captures, and persistence identity.

`--recipe-sequence` generalizes the same one-process/one-session mechanism for
an ordered commissioning procedure. Every recipe keeps its own truthful purpose
and scene metadata. `--fixture-aperture-shape` records the holder visibility
shape without relabeling it as an experimental area or dish rim. Sequence result
schema v4 records and validates the exact ordered recipes, aperture shape, PTP
groups, and common session identity. The permanent holder workflow is described
in [holder_fixture_validation.md](holder_fixture_validation.md).

For a visible Cam2010093 center-and-outline capture after the optical path is
physically prepared:

```bash
scripts/run_gui_guided_capture_smoke.sh \
  --execute \
  --recipe experimental_area_center_and_outline \
  --purpose crosshair_alignment \
  --cameras 2010093 \
  --save
```

The live runner validates the result with
`scripts/validate_gui_guided_capture_result.py`. A passing result requires:

- the exact expected camera set;
- the exact requested workflow profile, when one was supplied;
- valid nonempty full-resolution captures with local and camera frame IDs;
- `capture_group_membership.status=complete`;
- `citrus_scene_consistency.status=same_scene`;
- a display-fenced Citrus restore with `state=restored`, `presented=true`, and
  `active=false`;
- transactional calibration preflight applied the expected timing and restored
  the prior camera/light state;
- Orange streaming stopped before exit;
- a real calibration session directory when `--save` is requested.

For sweeps, validation additionally requires the exact level/repeat sequence
and one shared calibration session. The privileged wrapper returns the result
JSON and its narrowly validated calibration session tree to `SUDO_UID:SUDO_GID`,
matching the ownership handoff used for recording artifacts.

For a repeated saturation/stability sweep, use the permanent commissioning
utility described in [projector_intensity_commissioning.md](projector_intensity_commissioning.md).

The live launcher uses the installed narrow GUI privilege wrapper. Reinstall
that wrapper after adding or changing guided-capture environment fields:

```bash
scripts/install_orange_gui_validation_wrapper.sh --install-sudoers
```
