# Dry-shelf center-first commissioning — 2026-09-14

Status: implemented and exercised successfully on the four-camera rig on
2026-09-15. A fresh, uninterrupted end-to-end commissioning run remains the
final repeatability check before treating the workflow as production-accepted.

After a camera has moved, the old projected arena can extend beyond the new
camera raster. A full 10-by-10 homography grid, and even the arena rectangle,
may then be clipped. Requiring either before finding the projected center
creates a circular dependency. The 2026-09-14 post-move sweep visibly showed
the rightmost grid column clipped on all four cameras; none of its tested gray
levels qualified, despite strong visible dots. The unarmed standard centering
run failed at baseline rectangle detection on camera 2010094 and changed no
centers or homographies.

## Ordered authority

1. **Physical field of view:** the operator fixes the cameras and verifies the
   complete intended dish fits in each sensor image. Software translation
   cannot recover a physically clipped dish.
2. **Bootstrap projected centers:** on the unobstructed dry shelf, analyze only
   the center fiducial and symmetric local probes. The existing scene may also
   show an arena outline, but its edges are not required or accepted at this
   stage. Solve the projector-to-camera
   local Jacobian, move each projected arena center to its camera sensor center,
   and verify with an independent capture. PTP grouping, projection-stability
   and Citrus transaction/receipt gates remain mandatory. Rectangle detection
   is intentionally absent. An explicitly selected gray is provisional for
   finding fiducials, not a passing projector-intensity measurement.
3. **Qualified field and photometry:** with the committed new centers, repeat
   the full-grid projector-intensity sweep. Do not reuse a pre-move intensity
   report as current acceptance authority. A failed sweep does not authorize
   homography fitting.
4. **Edges then homographies:** the standard centering runner still detects
   rectangle edges at baseline/candidate and verifies the centered geometry.
   Its optional resize has a separate persistence arm. Only with a fresh
   all-camera passing intensity report may it capture the complete dot grid,
   fit homographies, and—if separately armed—accept them.
5. **Later physical geometry:** the dish-holder center/rim and projected-surface
   scale are separate observations. Sensor centering does not assert that the
   holder is concentric with the camera or that mm/px is unchanged.

## Bootstrap invocation

The launcher is a dry run unless `--execute` is present. Center persistence is
independently armed; an unarmed passing probe rolls back the proposed centers.

```bash
CITRUS_BIN=/path/to/verified/citrus \
CITRUS_PROJECT_ROOT_OVERRIDE=/home/jeremy/citrus \
scripts/run_gui_arena_centering_commissioning.sh \
  --bootstrap-centers-only --foreground-gray-u8 84 \
  --execute --save-verified-centers
```

`--bootstrap-centers-only` cannot be combined with
`--projector-intensity-report`, `--resize-arenas`,
`--save-verified-layout`, `--fit-homographies`, or
`--accept-homographies`. The result is schema v4 and explicitly identifies
`bootstrap_center_fiducial_v1`; its gray qualification is
`provisional_for_center_fiducial_only`. Its validator requires center/stability
evidence and rejects rectangle or homography claims. This result is **not** a
homography or photometry acceptance record.

After bootstrap, run the existing intensity-sweep launcher against the newly
centered canvas. Then pass that fresh report to the standard centering launcher
for edge checks and optional homography fitting/acceptance. Keep each report,
the final Citrus canvas checksum, and the subsequent scale/holder evidence
linked to the current rig-geometry revision.
