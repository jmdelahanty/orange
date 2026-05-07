# Autofocus Runtime Design Plan

Date: May 4, 2026

## Purpose

Define what it would take to add autofocus to Orange without disturbing the
validated acquisition, recording, PTP, YOLO, crop, and pose paths.

The immediate target is not continuous production autofocus. The first useful
target is a measured, auditable one-shot focus workflow that can run before a
recording, choose a focus value, lock it, and leave enough telemetry to decide
whether the result was trustworthy.

## Current Repo State

Orange already has the low-level lens-control pieces needed for an autofocus
prototype:

- Manual Focus and Iris controls exist in the camera properties panel.
- Focus writes go through `update_focus_value(...)` in `src/camera.cpp`.
- Focus writes are range checked, read back, and logged through the checked
  setter path before being treated as successful.
- Per-camera config stores `focus` and `iris`.
- Per-camera config stores `focus_uart_bootstrap`, defaulting to `false`.
- The EF lens focus investigation showed camera `2010096` with
  `EF100mm f/2.8L Macro IS USM` can expose a degenerate `Focus` range until the
  UART lens path is initialized.
- The `evt_lens_probe` diagnostic can validate lens nodes, UART bootstrap, and
  explicit focus/iris target writes outside the GUI.
- Aperture calibration docs already treat focus as part of the calibration
  state: material focus changes require recalibration or at least explicit
  interpretation of aperture artifacts.
- Spatial layout work now defines dish masks and arena layout metadata in
  camera-native pixels. Those artifacts are a natural ROI source for autofocus,
  but autofocus does not consume them yet.

What is not in place yet:

- No autofocus runtime module or worker.
- No focus metric CUDA kernels.
- No scalar metric readback path.
- No autofocus control policy.
- No autofocus config block.
- No autofocus event/audit artifact.
- No GUI workflow for autofocus beyond manual Focus/Iris sliders.
- No headless or GUI validation run for focus movement inside a production-like
  recording session.

## Design Constraints

- Keep manual focus as the always-available fallback.
- Do not run focus hunting during production recording in the first slice.
- Lock exposure, gain, iris, lighting, and camera placement while validating
  focus metrics. Otherwise the metric can optimize brightness or contrast
  changes instead of optical focus.
- Keep autofocus metric work on the GPU. Do not copy full frames to CPU for
  scoring.
- Copy only small scalar metric results back to host memory.
- Use non-blocking CUDA events/readback. Do not add `cudaStreamSynchronize` to
  the acquisition hot path.
- Rate-limit focus writes. EVT focus transactions can be slow or stateful on
  some lens/camera combinations.
- Keep `focus_uart_bootstrap` explicitly config gated. Do not touch UART/GPO
  nodes implicitly for cameras that do not opt in.
- Treat focus as part of the calibration key for aperture, spatial resolution,
  and any downstream measurement that depends on sharpness.

## Proposed Runtime Shape

Add a per-camera `AutofocusManager` that owns focus scoring state, control-loop
state, and audit logging. The manager should be isolated from acquisition as
much as possible:

- Acquisition provides frame references and timing metadata.
- The autofocus manager decides whether this frame should launch a metric pass.
- Metric kernels run on GPU-resident image data or on an explicitly detached
  GPU input.
- Host code consumes completed scalar scores via non-blocking event polling.
- Focus writes are issued through the existing checked focus setter.
- Recording metadata and JSONL audit events capture every score and command.

The first implementation should support a "metric probe" mode that never moves
the lens. This lets us measure overhead and score stability before the control
loop can change the scene.

## Focus Metrics

Implement at least two grayscale sharpness metrics before choosing a default:

- Tenengrad or Sobel gradient energy.
- Variance of Laplacian or another high-frequency energy estimate.

Metric requirements:

- ROI-aware from the first implementation.
- Optional downsample support for lower overhead.
- Robust to dish rim exclusion.
- Stable enough across adjacent frames that a coarse focus sweep forms a clear
  curve.
- Produces scalar outputs with timing telemetry for launch, readback, and score
  availability.

The default metric should be chosen from observed focus sweeps, not preference.
If Tenengrad and Laplacian disagree on real dish/fish content, keep both scores
in telemetry until the failure mode is understood.

## ROI Sources

Autofocus should support increasingly specific ROI sources:

1. `fixed_roi`: simple camera-pixel rectangle for early metric validation.
2. `dish_mask`: valid region from spatial calibration, excluding rim edges.
3. `arena_layout`: selected arena(s) in multi-arena views.
4. `detection_roi`: YOLO/fish ROI clipped to dish/arena validity.

The first control-loop slice should not depend on fish detections. A fixed ROI
or dish-mask-clipped ROI is easier to validate and avoids coupling autofocus to
positive detection behavior before the metric is proven.

Rim and background guardrails:

- Exclude strong dish rim edges from the metric when possible.
- Reject ROIs with too little valid area.
- Reject ROIs with too little texture or insufficient score confidence.
- Hold focus rather than moving when the ROI is invalid.

## Control Policy

Start with one-shot autofocus:

1. Confirm focus range is usable.
2. Confirm exposure/gain/iris are locked.
3. Sweep a coarse set of focus positions across the allowed or configured range.
4. Wait a configured number of settle frames after each focus write.
5. Collect several metric samples at each position.
6. Pick the best coarse candidate.
7. Run a narrower fine sweep around that candidate.
8. Apply the selected focus value.
9. Verify readback.
10. Lock focus before recording starts.

Periodic or continuous autofocus should wait until one-shot behavior is
validated. The periodic mode needs stronger anti-hunting guardrails:

- Minimum score improvement before moving.
- Minimum time between moves.
- Maximum step size.
- Failed-command backoff.
- Manual override.
- Automatic disable after repeated invalid ROI or focus command failures.

## Proposed Config

This is a proposed runtime config shape. It is not implemented yet.

```json
"autofocus": {
  "enabled": false,
  "mode": "off",
  "metric": "tenengrad",
  "roi_policy": "fixed_roi",
  "fixed_roi": {
    "x": 1024,
    "y": 1024,
    "width": 2048,
    "height": 2048
  },
  "use_dish_mask": true,
  "use_arena_layout": false,
  "metric_interval_frames": 5,
  "focus_write_min_interval_ms": 250,
  "settle_frames": 5,
  "sample_frames_per_position": 5,
  "coarse_step": 500,
  "fine_step": 100,
  "min_focus": null,
  "max_focus": null,
  "min_score_improvement_fraction": 0.03,
  "max_failed_commands": 3,
  "write_events_jsonl": true
}
```

Suggested `mode` values:

- `off`: no autofocus work.
- `metric_probe`: compute/log scores but never move focus.
- `oneshot`: run a pre-recording sweep, set focus, then lock.
- `periodic`: periodically evaluate whether a small focus correction is needed.

## Proposed Artifacts

Add one JSONL event stream per camera:

```text
Cam<serial>_autofocus_events.jsonl
```

Each row should be small, parseable, and replayable:

```json
{
  "schema_id": "orange.autofocus_event",
  "schema_version": 1,
  "event_kind": "metric_sample",
  "camera_serial": "2010096",
  "recording_id": "2026_05_04_...",
  "frame_id": 12345,
  "camera_timestamp_ns": 1234567890,
  "mode": "metric_probe",
  "state": "sampling",
  "focus": {
    "target": 3000,
    "readback": 3000,
    "readback_age_frames": 0,
    "range_min": 0,
    "range_max": 6276,
    "inc": 1,
    "command_status": "none"
  },
  "roi": {
    "policy": "fixed_roi",
    "x": 1024,
    "y": 1024,
    "width": 2048,
    "height": 2048,
    "valid_fraction": 1.0
  },
  "metric": {
    "name": "tenengrad",
    "score": 123456.0,
    "valid": true,
    "reject_reason": ""
  },
  "latency_ms": {
    "metric_launch": 0.03,
    "score_ready": 0.12,
    "focus_command": 0.0
  }
}
```

Also add an aggregate summary per camera when autofocus runs:

```text
Cam<serial>_autofocus_summary.json
```

The recording snapshot should eventually advertise these files under a
per-camera autofocus block, for example:

```json
"autofocus": {
  "enabled": true,
  "mode": "oneshot",
  "selected_focus": 3120,
  "metric": "tenengrad",
  "roi_policy": "dish_mask",
  "runtime": {
    "files": {
      "events": "Cam2010096_autofocus_events.jsonl",
      "summary": "Cam2010096_autofocus_summary.json"
    }
  }
}
```

## Implementation Slices

### Slice 1: Capability Snapshot

- Extend lens startup logging or add a small artifact that captures focus/iris
  node availability, min/max/inc, configured focus, readback focus, and
  `focus_uart_bootstrap`.
- Reuse `evt_lens_probe` for hardware validation before runtime changes.
- Confirm target cameras expose non-degenerate focus ranges after startup.

### Slice 2: Metric Probe, No Focus Movement

- Add autofocus config parsing with only `off` and `metric_probe`.
- Add a CUDA focus metric kernel and async scalar readback.
- Support fixed ROI.
- Write `Cam<serial>_autofocus_events.jsonl`.
- Verify no camera gaps, no YOLO regression, and no recorder regression.

### Slice 3: Spatial ROI Integration

- Load the recording snapshot spatial calibration or explicit artifact path for
  the selected camera.
- Clip fixed/fish ROIs to `dish_mask.runtime.valid_geometry`.
- Add ROI rejection telemetry.
- Add preview overlay later; do not block metric validation on it.

### Slice 4: One-Shot Sweep Tool

- Add a headless or diagnostic tool that can sweep focus and write metric
  events without starting a full recording workflow.
- Prefer a tool that can run against a live camera with exposure/gain/iris
  locked.
- Save a focus-curve summary so the best focus decision can be inspected.

### Slice 5: Pre-Recording One-Shot Autofocus

- Add a GUI/headless pre-recording action that runs the validated sweep, applies
  selected focus, verifies readback, and locks focus before recording.
- Advertise autofocus artifacts in the recording snapshot.
- Keep failure behavior conservative: if autofocus cannot prove a good focus,
  leave the existing manual/configured focus in place and mark the event stream
  as failed/held.

### Slice 6: Periodic Autofocus

- Only start after one-shot has repeated clean validation.
- Run at low cadence.
- Require strong score confidence and valid ROI before moving.
- Add operator-visible state and immediate disable/override.

## Validation Plan

For each camera/lens pair:

- Validate focus range and explicit writes with `evt_lens_probe`.
- Run fixed-focus clips across several focus values and confirm the metric curve
  peaks near visually best focus.
- Run metric probe mode at target FPS and confirm overhead is negligible.
- Run one-shot sweep while not recording and inspect the focus curve.
- Run one-shot before recording and verify selected focus, readback, video
  quality, YOLO latency, crop behavior, and pose artifact continuity.
- Repeat after material changes to focus, iris, lens, camera height, dish plane,
  or lighting.

Acceptance criteria for first production-like one-shot mode:

- Converges to a visually plausible focus on a repeatable target.
- Leaves manual focus available and unchanged on failure.
- Adds no camera frame-id gaps.
- Adds no sustained YOLO or recording latency regression.
- Writes parseable autofocus events and summary artifacts.
- Records enough metadata to replay why a focus value was selected.

## Open Questions

- Which camera/lens pairs require `focus_uart_bootstrap=true`?
- Are focus writes always synchronous enough for readback to mean physical
  settle, or do we need a separate settle/verification model?
- What is the best focus target for validation: fish, dish texture, USAF target,
  or a dedicated high-contrast plane at the dish depth?
- Should autofocus be a camera config feature, experiment-spec feature, or both?
- How should aperture calibration artifacts declare their focus dependency once
  autofocus can change focus programmatically?
- Does periodic autofocus have real value for the current rig, or is one-shot
  pre-recording focus enough?

## Related Docs

- `docs/fish_autofocus_todo.md`
- `docs/evt_ef_lens_focus_investigation_todo.md`
- `docs/evt_lens_probe.md`
- `docs/aperture_calibration_usage.md`
- `docs/spatial_layout_contract.md`
- `docs/recording_metadata.md`
