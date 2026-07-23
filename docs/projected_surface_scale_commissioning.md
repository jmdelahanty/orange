# Projected-Surface Physical Scale Commissioning

Status: implemented first guided workflow slice.

## Purpose

This commissioning step measures physical scale on the dry projected surface
after the camera-to-canvas homographies have been accepted. It is not a daily
dish-registration step and it does not redefine the canvas or arena rectangle.
When the holder's flattened diffuser gels are the experiment's projection
surfaces, use the holder-installed profile and measure on those installed gels;
an unobstructed shelf measurement is commissioning reference evidence, not a
substitute for the operational holder plane.

The physical target is the 3 mm acrylic disk. Its 5 mm hole pitch is the scale
authority. The 25 mm distance from `C` to `XPLUS` and the measured 77 mm outside
diameter are independent validation measurements; neither may rescale the fit.

## Ownership

Orange owns acquisition and camera-space evidence:

- request full-arena, experimental-area-unmasked uniform gray from Citrus;
- acquire one PTP-grouped image across the selected cameras;
- copy the target definition and its checksums into the calibration session;
- identify `C`, `XPLUS`, `ASYM`, and the regular hole centers;
- fit target millimetres to camera-native pixels;
- with the already-active homography, derive an Orange estimate of canvas
  pixels per millimetre;
- write immutable observations and review overlays.

Citrus owns runtime canvas scale:

- verify the Orange observation identity, checksums, plane contract, and QC;
- independently refit the persisted correspondences;
- combine that fit with Citrus's accepted camera-to-canvas homography;
- write immutable scale candidates;
- expose them for operator review;
- on explicit acceptance, promote active pointers and apply the accepted scale
  as a runtime overlay.

Citrus does not rewrite the canvas JSON or resize arena rectangles during this
operation. It updates the millimetre-to-pixel interpretation used for physical
experimental-area dimensions. Promotion fails if the canvas, active
homography, source observation, or candidate changed after fitting.

## Plane and coordinate contract

- Target plane: `projected_surface`.
- Illuminated hole-center plane: `z = 0 mm`.
- Camera-facing target surface: `z = +3 mm`.
- Target origin: marker `C`.
- Target +X: from `C` toward `XPLUS`.
- Target orientation and handedness must also be resolved using `ASYM`.
- Camera-native pixels and final-display canvas pixels use their established
  top-left-origin, +X-right, +Y-down conventions. This workflow documents and
  validates those conventions; it does not change rendering behavior.

The projected-surface scale is not automatically valid at the dish rim, water
surface, or fish-observation plane. Those remain separate physical products.

## Guided workflow

Prerequisites for the operational holder plane:

1. The holder is installed with each flattened diffuser gel seated exactly as
   it will be during experiments; dishes and water are absent.
2. The canvas and arena locations have been commissioned.
3. A compatible holder-plane projected-surface homography is active for every
   selected camera.
4. One 3 mm disk rests flat on each gel with all special markers visible.
5. Orange is using the PTP calibration timing configuration.

The `uniform_gray` calibration scene illuminates the complete configured arena
rectangle. It must not inherit the current experimental-area/dish mask: doing
so can place a projected edge on the 77 mm disk perimeter and invalidate the
independent outside-diameter check.

Run a dry preview first:

```bash
scripts/run_gui_guided_capture_smoke.sh \
  --profile holder_installed_projected_surface \
  --recipe uniform_gray \
  --save \
  --targets-ready
```

At the machine, add `--execute`. The normal path deliberately stops at the
Orange review panel. Inspect each overlay and the fit, perimeter holdout,
`C`-to-`XPLUS`, outside-diameter, orientation, and Orange/Citrus recomputation
checks. Arm the acceptance checkbox and promote only if every camera passes.
Use **Release candidate without promotion** to reject the transaction while
leaving the current active scale unchanged.

`--accept-projected-surface-scales` exists for explicit automated validation;
it is not the default commissioning path.

## Artifacts and activation

Everything Orange produces belongs to the same calibration session as the
grouped capture. Per-camera files live under the camera artifact's
`scale_observations/<capture-group>/` directory and include:

- `observation.json`
- `overlay.png`

The session also contains an observation-set manifest under
`artifacts/projected_surface_scale_<capture-group>/manifest.json`, and the
session index records that product.

Orange observation schema:

```text
orange.calibration.projected_surface_scale_observation, version 1
```

Citrus stores immutable candidate sets under the selected rig/canvas
calibration-artifact root. Active per-camera pointers identify both the scale
candidate and the homography candidate used to derive it. Experiment readiness
is blocked while a scale candidate awaits review. During migration, a missing
active scale pointer is reported as a fallback; an existing stale or invalid
pointer fails compatibility and blocks start.

### Restart-safe review recovery

Closing Citrus or Orange while a candidate is waiting for review does not
require refitting or a one-off promotion script. In Orange, choose
**Persisted Physical-Scale Candidate Set**, select the Citrus scale-set
`manifest.json`, and then request revalidation. Orange finds the one exact
observation-set manifest whose arena/camera identities, observation paths, and
SHA-256 values match the candidates. Ambiguous, incomplete, or already
finalized sets are rejected.

Citrus authorizes the selected directory under the currently loaded
rig/canvas, verifies the exact requested target set and current canvas
checksum, reloads every checksummed Orange observation, verifies the active
homography identity and matrix, and independently repeats the scale refit and
quality gates. Loading is read-only with respect to active pointers. Its
passed `revalidation` receipt appears in status and is copied into a later
acceptance receipt. Promotion remains a separate operator-armed action.

For recovery without the Orange GUI, use the checked-in dry-run-first tool:

```bash
scripts/review_projected_surface_scale_candidates.py \
  load \
  --candidate-set-manifest /path/to/scale_candidates/<set>/manifest.json \
  --expected-candidate-set-id <set> \
  --expected-canvas-sha256 sha256:<64-hex-digits>
```

Place global `--execute` before the subcommand to send the request. The
`promote` subcommand additionally requires the exact Orange observation-set
manifest, a currently passed Citrus persisted-set revalidation, and both
`--execute` and `--accept-scales`. It does not directly write active files;
all mutation still goes through Citrus's guarded promotion transaction.

## Quality policy

The first implementation fails closed when target identity/orientation is not
resolved, too few holes match, fit or perimeter-holdout residuals exceed their
limits, scale anisotropy is excessive, the 25 mm validation span or 77 mm
outside diameter cannot be validated, or Orange and Citrus recomputations
disagree. Both dimensions are explicitly excluded from rescaling.

This workflow has build and synthetic-image coverage and completed a real
four-camera disk run on 2026-07-20. All four cameras matched 161/161 target
points, validated the 77 mm outside diameter with 360/360 radial support, and
passed Orange and Citrus fit, holdout, anisotropy, orientation, and independent
dimension gates.
