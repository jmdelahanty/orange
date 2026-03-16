# Lighting Uniformity TODO

Date: February 23, 2026

## Goal

Reduce per-camera brightness mismatch so all cameras are visually consistent under the same scene and fixed camera settings.

## Current Context

- PTP timing and frame cadence look stable in recent runs.
- Camera startup logs showed one config mismatch:
  - `2010093` iris target was `3`
  - `2010094/2010095/2010096` iris target were `5`
- This must be normalized before attributing differences to lighting.

## Audit Update (2026-03-16)

- The tracked repo currently contains only a small sample of committed camera configs, not the full live four-camera rig configuration referenced above.
- Treat the iris-mismatch note in this TODO as a live-rig observation, not a statement about the committed sample configs in this repo.
- No lighting-analysis automation or per-camera brightness telemetry has landed in the codebase yet.

## Phase 1: Lock Software Settings

- [ ] Set identical `iris`, `exposure`, and `gain` in all active camera JSON configs.
- [ ] Confirm `AutoGain`/auto-exposure behavior is disabled during acquisition.
- [ ] Run startup and verify each camera prints matching readback values for:
  - [ ] iris
  - [ ] exposure
  - [ ] gain

## Phase 2: Controlled Baseline Capture

- [ ] Place a uniform target (matte white/gray card) at normal working distance.
- [ ] Warm up lights to steady state before measuring.
- [ ] Capture a short clip with all 4 cameras using fixed settings.
- [ ] Record per-camera mean brightness and spread (same ROI for all cameras).
- [ ] Save this as baseline reference for later comparisons.

## Phase 3: Separate Position vs Camera Effects

- [ ] Physically swap camera positions (example: swap dimmest with brightest).
- [ ] Repeat the same baseline capture.
- [ ] Determine outcome:
  - [ ] If dimness follows position -> lighting/diffuser geometry issue.
  - [ ] If dimness follows camera/lens -> unit-to-unit optical/sensor variation.

## Phase 4: Lighting/Diffuser Optimization

- [ ] Adjust diffuser placement and angle to reduce directional falloff.
- [ ] Check for partial occlusions, shadows, or lens hood reflections.
- [ ] Match light distance and angle across all camera viewing paths.
- [ ] Re-measure after each change using the same capture protocol.

## Phase 5: Finalize and Lock

- [ ] Pick final settings and document:
  - [ ] light positions
  - [ ] diffuser setup
  - [ ] camera iris/exposure/gain
- [ ] Save final reference capture.
- [ ] Add final values to camera config files used in production.

## Suggested Acceptance Criteria

- [ ] Per-camera mean brightness variation <= 5% on the uniform target.
- [ ] No single camera deviates by > 8% from the group median.
- [ ] Stable results across at least 3 repeat runs.

## Follow-Up Automation (Optional)

- [ ] Add a live per-camera brightness metric line during streaming.
- [ ] Add a script to compare per-camera brightness stats from recorded metadata.
