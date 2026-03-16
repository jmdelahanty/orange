# Fish Autofocus TODO

Date: February 23, 2026

## Goal

Add reliable autofocus for fish so focus can track real scene changes without constant manual adjustment.

## Scope

- Start with one camera first, then scale to all cameras.
- Keep current manual focus path available as fallback.
- Minimize focus hunting and avoid disrupting frame timing.

## GPU Direct / Throughput Notes

- Preferred architecture: keep autofocus metric computation on GPU.
- Do not copy full frames to CPU for focus scoring.
- Compute focus metric with CUDA on-device and copy back only a scalar score.
- Run autofocus metric at controlled cadence (for example 10-30 Hz), not necessarily every frame.
- Run lens focus writes at even lower cadence (for example 2-10 Hz) due to lens/control latency.
- For very high FPS operation, prioritize ROI/downsampled metric inputs to protect frame budget.

### Async Metric Readback Model

- Use per-camera CUDA stream work ordering: metric kernel -> `cudaMemcpyAsync` of scalar score.
- Copy score into pinned host memory only (no full-frame transfer).
- Use `cudaEventRecord` and non-blocking `cudaEventQuery` to consume score when ready.
- Avoid `cudaStreamSynchronize` in the acquisition loop.
- Keep double-buffered score slots so a new metric launch does not overwrite pending results.

### Current Throughput Constraints (This System)

- At `4512x4512 mono8`, each frame is about `20.36 MB`.
- At `100 FPS` (your practical stream ceiling), one camera payload is roughly `2.04 GB/s` (`~16.3 Gbps`).
- Hardware encoding is currently stable around `60 FPS`; above that, frame drops increase.
- Practical operating model:
- stream/acquire up to `100 FPS` when needed
- record/encode near `60 FPS` for stability
- run autofocus decisions at a slower control rate than frame rate

## What We Already Have

- Working focus set/readback path in `src/camera.cpp` (`update_focus_value` and checked setter logic).
- Per-frame acquisition loop in `src/acquire_frames.cpp` where autofocus decisions can run.
- Optional YOLO pipeline that can provide fish bounding boxes for ROI-based focus metrics.

## Audit Update (2026-03-16)

- Lens-control readiness is stronger than when this TODO was first drafted:
  - focus writes now go through checked setter logic in `src/camera.cpp`,
  - focus-range readiness can optionally use UART bootstrap for affected EF setups,
  - probe tooling exists in `tools/evt_lens_probe.cpp`.
- No autofocus runtime exists yet:
  - no autofocus module/class,
  - no focus metric kernels,
  - no controlled autofocus cadence logic,
  - no autofocus telemetry or runtime config block,
  - no dish-mask parser or sidecar loader in the repo.
- `src/acquire_frames.cpp` still has no autofocus calls wired into the live acquisition loop.
- The tracked sample configs in `config/*.json` still default `focus_uart_bootstrap` to `false`; actual bootstrap enablement remains deployment-specific.

## Phase 0: Prerequisites and Baseline

- [ ] Confirm all cameras have stable focus range at startup (`focus_uart_bootstrap` as needed).
- [ ] Pick one camera/lens pair for initial development.
- [ ] Lock exposure/gain/iris during autofocus experiments.
- [ ] Record baseline clips at fixed manual focus values for comparison.

## Phase 1: Pick and Validate a Focus Metric

- [ ] Implement at least two sharpness metrics on grayscale frames:
- [ ] Variance of Laplacian
- [ ] Tenengrad (Sobel gradient energy)
- [ ] Compute metrics on a fixed ROI first (center crop).
- [ ] Implement metric kernels on CUDA for GPU-resident frames (no full-frame CPU transfer).
- [ ] Return only scalar focus score(s) to CPU-side control logic.
- [ ] Verify metric rises near best focus and drops when defocused.
- [ ] Choose final metric based on stability and noise sensitivity.
- [ ] Validate async readback timing jitter is acceptable at target FPS.

## Phase 2: Define Autofocus Strategy

- [ ] Start with one-shot autofocus (run once before recording/stream start).
- [ ] Add optional periodic refocus mode (for long sessions).
- [ ] Define search policy:
- [ ] coarse sweep (fast)
- [ ] fine sweep around best candidate
- [ ] Add guardrails:
- [ ] deadband/hysteresis to prevent hunting
- [ ] min time between focus moves
- [ ] max step size per update

## Phase 3: Fish-Aware ROI Selection

- [ ] Baseline mode: fixed ROI in expected fish region.
- [ ] Advanced mode: use fish detections to build dynamic ROI.
- [ ] Handle missing detections:
- [ ] keep last good ROI for N frames
- [ ] fall back to fixed ROI
- [ ] Reject ROIs that are too small/low texture for reliable focus scoring.
- [ ] Add static exclusion mask for known dish rim/border regions.
- [ ] Require minimum fish-pixel support in ROI before allowing focus moves.
- [ ] If ROI is dominated by static background edges, hold focus (no move).
- [ ] If fish is near rim, switch to conservative mode (smaller focus steps or temporary hold).

### Dish Mask Precompute Plan

- [ ] Build one static mask per camera from an "empty dish" calibration frame.
- [ ] Recommended first method: manual calibration.
- [ ] Save dish center `(cx, cy)` and radius `r` from one-click/GUI selection.
- [ ] Define inner valid radius `r_valid = r - rim_margin_px` to exclude strong rim edges.
- [ ] Rasterize binary mask `dish_mask(x,y) = 1` if `(x-cx)^2 + (y-cy)^2 <= r_valid^2`, else `0`.
- [ ] Persist mask calibration in per-camera config (or sidecar file), then regenerate mask at startup.
- [ ] Include persistence fields at minimum:
- [ ] `camera_serial`
- [ ] `width,height`
- [ ] `cx,cy,r,r_valid,rim_margin_px`
- [ ] `calibration_timestamp`
- [ ] On startup, validate persisted geometry against active camera resolution before use.
- [ ] If validation fails (missing/mismatch/out-of-range), disable mask use and emit explicit warning.
- [ ] Add a startup log line confirming mask load source and parameters for each camera.
- [ ] Add a calibration command to overwrite saved mask parameters intentionally (no silent overwrite).
- [ ] Add a quick verification mode that reloads the saved mask and writes an overlay preview image.
- [ ] Upload mask once to GPU memory and reuse for all frames.
- [ ] Runtime ROI rule: `af_roi = fish_roi INTERSECT dish_mask`.
- [ ] If `af_roi` is too small, skip autofocus update and hold last focus.

### Dish Mask JSON Example

Use either a sidecar file (recommended) or embed this block in each camera config.

```json
{
  "dish_mask": {
    "schema_version": 1,
    "camera_serial": "2010093",
    "width": 4512,
    "height": 4512,
    "cx": 2254.0,
    "cy": 2256.0,
    "r": 2060.0,
    "rim_margin_px": 140.0,
    "r_valid": 1920.0,
    "calibration_timestamp": "2026-02-23T12:45:00Z",
    "source": "manual"
  }
}
```

### Sidecar File Convention (Recommended)

Keep dish-mask calibration in dedicated per-camera sidecar files:

- Directory: `config/dish_masks/`
- Filename: `dish_mask_<camera_serial>.json` (example: `dish_mask_2010093.json`)
- One file per camera/serial.
- Commit calibration files if they are stable for a rig; otherwise keep in local config storage.
- Prefer loading by serial first; if missing, fall back to embedded `dish_mask` block in camera JSON.

Startup validation rules:

- `camera_serial` must match active camera.
- `width`/`height` must match active stream dimensions.
- `cx` and `cy` must be inside frame bounds.
- `r_valid` must be positive and `< r`.
- `r` must be reasonable for image size (non-zero and not larger than frame diagonal).
- On validation failure, autofocus must run without dish mask and print a clear warning.

### C++ Mapping (Docs-Only)

Proposed config object shape for runtime use:

```cpp
struct DishMaskConfig {
    int schema_version = 1;
    std::string camera_serial;
    int width = 0;
    int height = 0;
    float cx = 0.0f;
    float cy = 0.0f;
    float r = 0.0f;
    float rim_margin_px = 0.0f;
    float r_valid = 0.0f;
    std::string calibration_timestamp;
    std::string source; // "manual" or "auto"
    bool enabled = false; // true only after successful parse + validation
};
```

Parser/validation checklist:

- [ ] Parse `dish_mask` object from per-camera JSON or sidecar.
- [ ] Require `schema_version == 1` (warn and disable otherwise).
- [ ] Require all core geometry fields (`cx, cy, r`, plus `width, height`).
- [ ] If `r_valid` missing, derive as `r - rim_margin_px`.
- [ ] Validate geometry against active camera stream dimensions at startup.
- [ ] Set `enabled=false` on any parse/validation failure.
- [ ] Emit startup log: `serial`, `source`, `cx/cy/r/r_valid`, `enabled`.
- [ ] Keep autofocus functional when mask is disabled (fallback path).

Suggested parser entry points:

- `bool load_dish_mask_config(const nlohmann::json& camera_json, DishMaskConfig* out);`
- `bool validate_dish_mask_config(const DishMaskConfig& cfg, int active_width, int active_height);`

### Optional Auto-Calibration Path

- [ ] Detect dish circle from calibration image using edge + circle fit (for example Hough circle).
- [ ] Validate circle fit score and fall back to manual calibration if confidence is low.
- [ ] Save detected `(cx, cy, r)` and review overlay before accepting.

### Edge-Proximity Guardrails

- [ ] Compute `roi_valid_fraction = area(af_roi) / area(fish_roi)`.
- [ ] Compute `edge_proximity = distance(fish_centroid, dish_center) / r_valid`.
- [ ] If `roi_valid_fraction` is below threshold or `edge_proximity` is high:
- [ ] block focus moves or reduce step size/rate
- [ ] require stronger score improvement before any move

## Phase 4: Integrate with Runtime

- [ ] Add an autofocus module/class (new file) to keep logic isolated.
- [ ] Wire autofocus calls in `src/acquire_frames.cpp` at a controlled cadence.
- [ ] Call focus updates through existing checked setter path in `src/camera.cpp`.
- [ ] Keep control decimation explicit:
- [ ] metric cadence (Hz)
- [ ] focus-write cadence (Hz)
- [ ] Default execution model: run autofocus metric launch in acquire path without a separate worker.
- [ ] Add optional dedicated autofocus worker only if profiling shows contention or cross-pipeline coupling.
- [ ] Add concise logs:
- [ ] focus metric
- [ ] selected focus target
- [ ] move accepted/rejected reason
- [ ] Add runtime flags/config:
- [ ] `autofocus_enabled`
- [ ] `autofocus_mode` (`off`, `oneshot`, `periodic`)
- [ ] `autofocus_interval_frames`

## Phase 4.5: Per-Frame Autofocus Audit Logging

- [ ] Extend recording metadata schema to include autofocus telemetry.
- [ ] Log per-frame fields:
- [ ] `focus_cmd`
- [ ] `af_score`
- [ ] `af_score_valid`
- [ ] `af_state`
- [ ] `af_action`
- [ ] `af_roi_x,af_roi_y,af_roi_w,af_roi_h`
- [ ] Log `focus_readback` at decimated cadence (for example 5-10 Hz) and carry forward latest value.
- [ ] Record `focus_readback_age_frames` so stale readbacks are explicit in analysis.

## Phase 5: Safety and Performance Checks

- [ ] Ensure autofocus does not reduce FPS or increase dropped frames.
- [ ] Ensure focus commands are rate-limited to avoid bus/control saturation.
- [ ] Confirm no regressions in PTP/timestamp behavior while autofocus is active.
- [ ] Add fail-safe: disable autofocus automatically after repeated command failures.

## Phase 6: Validation Protocol

- [ ] Build a repeatable fish test scene (lighting and tank placement fixed).
- [ ] Compare manual-focus vs autofocus clips side-by-side.
- [ ] Validate behavior for:
- [ ] fish moving toward/away from camera
- [ ] fish leaving ROI
- [ ] low-contrast/low-light frames
- [ ] Run at least 3 sessions and keep logs + chosen focus traces.

## Suggested Acceptance Criteria

- [ ] Autofocus converges to near-manual-best focus within 2 seconds in one-shot mode.
- [ ] No sustained oscillation (focus hunting) for at least 60 seconds.
- [ ] Frame rate drop from autofocus overhead is < 2%.
- [ ] Operator can override/disable autofocus instantly from config/UI.

## Nice-to-Have Follow-Ups

- [ ] Per-camera autofocus parameter tuning profiles (different lenses/scenes).
- [ ] Optional ROI visualization overlay for autofocus debugging.
- [ ] Offline autofocus replay tool using recorded frames for fast iteration.
