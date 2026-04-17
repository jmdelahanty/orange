# Recording Preflight Implementation Checklist

## Purpose

This checklist turns the current split-GOP validation plan into an execution
order for the next code slice: using the shared recording validator as a real
preflight gate instead of only a read-only GUI summary.

The intent is to prevent Orange from starting a recording session whose GPU
topology or resource claims are already known to be invalid.

## Current State

Today on `exp/gop-split-a16`:

- split-GOP validation rules are documented in
  [advanced_recording_validation_plan.md](/home/jeremy/orange-gop-split-a16/docs/advanced_recording_validation_plan.md)
- the GUI shows a read-only validation summary in
  [src/gui/recording_panel.cpp](/home/jeremy/orange-gop-split-a16/src/gui/recording_panel.cpp)
- the validation logic itself now lives in the shared helper module:
  - [src/recording_validation.h](/home/jeremy/orange-gop-split-a16/src/recording_validation.h)
  - [src/recording_validation.cpp](/home/jeremy/orange-gop-split-a16/src/recording_validation.cpp)
- unit tests already cover the core policy rules in
  [tools/recording_validation_tests.cpp](/home/jeremy/orange-gop-split-a16/tools/recording_validation_tests.cpp)

What is still missing is the actual session guard:

- GUI can show that a configuration is invalid, but still attempts to start
  streaming / recording unless the operator notices
- headless does not yet fail early on the same preflight rules

## Design Decision

The first hard gate should be at **stream start**, not just at record start.

Why:

- recording pipelines are created when streaming starts
- split-GOP helper routing and encoder allocation are established there
- waiting until the record button is pressed is too late for some invalid
  configurations

The record button should still do a second check as a backup, but stream start
is the primary guard.

## Shared Preflight Helper

### Goal

Introduce one shared helper that returns:

- whether the session is valid
- the per-camera summaries
- flattened error/warning strings suitable for GUI/headless display

### Checklist

- [ ] Add a preflight result struct, for example:
  - `RecordingPreflightResult`
  - fields:
    - `bool ok`
    - `std::vector<CameraRecordingValidationSummary> summaries`
    - `std::vector<std::string> errors`
    - optional `std::vector<std::string> warnings`
- [ ] Add a helper that builds validation inputs from the current session state
- [ ] Add a helper that runs `validate_recording_configuration(...)` and
      flattens errors into user-facing messages
- [ ] Keep this helper outside the GUI layer so GUI and headless can both use it

### Recommended Placement

Short term:

- add it beside the current validator in:
  - [src/recording_validation.h](/home/jeremy/orange-gop-split-a16/src/recording_validation.h)
  - [src/recording_validation.cpp](/home/jeremy/orange-gop-split-a16/src/recording_validation.cpp)

Later, it can move into `src/session/recording_session.*` if that module grows.

## Inputs To The Preflight

### Goal

Use the real current session state rather than inventing a second config model.

### Checklist

- [ ] Build one validation input per camera from:
  - `cameras_params[i].gpu_id`
  - `cameras_params[i].recording.strategy`
  - `cameras_params[i].recording.constraints`
  - `cameras_select[i].record`
- [ ] Do **not** turn `gpu_id` into a list
- [ ] Continue treating the claimed GPU set as:
  - `{source_gpu_id} U {split_gop.encoder_gpu_ids}`

## GUI Gate: Start Streaming

### Goal

Prevent `Start streaming` from creating recording pipelines for an invalid
split-GOP session.

### Main Touch Point

- [src/orange.cpp](/home/jeremy/orange-gop-split-a16/src/orange.cpp)
  around the `Start streaming` button logic and the `camera_control->subscribe`
  transition

### Checklist

- [ ] Run the shared recording preflight immediately before flipping
      `camera_control->subscribe` to `true`
- [ ] Only gate when the session actually includes record-enabled split-GOP
      cameras
- [ ] If preflight fails:
  - [ ] keep `camera_control->subscribe` unchanged
  - [ ] do not allocate recording pipelines
  - [ ] capture the preflight errors into GUI-visible state
- [ ] If preflight succeeds:
  - [ ] continue with the existing stream-start path unchanged

## GUI Gate: Start Recording

### Goal

Recheck the preflight before toggling `record_video` on.

This is a backup guard and keeps the rule close to the record action as well.

### Main Touch Point

- [src/orange.cpp](/home/jeremy/orange-gop-split-a16/src/orange.cpp)
  around the record play/pause button and `camera_control->record_video`

### Checklist

- [ ] Before the `"next_record_state = true"` path proceeds, rerun the shared
      preflight
- [ ] If preflight fails:
  - [ ] do not set `record_video = true`
  - [ ] do not create recording metadata for that run
  - [ ] show/store the error messages
- [ ] If preflight succeeds:
  - [ ] leave the current record-start behavior unchanged

## GUI Error Surface

### Goal

Make failures visible enough that the operator understands what blocked the
session.

### Checklist

- [ ] Add lightweight GUI state for the most recent preflight result
- [ ] Show a compact red summary near the recording controls when the last
      preflight failed
- [ ] Support multiple error lines, for example:
  - topology mismatch
  - missing peer access
  - overlapping GPU claims
- [ ] Keep the read-only advanced validation summary in place
- [ ] Do **not** silently auto-correct invalid GPU choices

### Nice-To-Have Later

- [ ] modal dialog for blocking failures
- [ ] clickable focus/jump to the per-camera advanced summary section

## Headless Reuse

### Goal

Use the same preflight rules in headless so GUI and automated runs do not drift.

### Main Touch Points

- [src/orange_headless_client.cpp](/home/jeremy/orange-gop-split-a16/src/orange_headless_client.cpp)
  in the local-mode and experiment-spec startup paths

### Checklist

- [ ] Reuse the same shared preflight helper before headless recording begins
- [ ] Fail early with a clear log message if preflight fails
- [ ] Include the specific validation errors in stderr/stdout
- [ ] Do not partially start a run that already violates split-GOP topology or
      GPU-claim rules

## Tests

### Existing Coverage

The unit tests already cover the core validator rules:

- valid `PIX` pair
- topology mismatch
- peer-access failure
- overlapping GPU claims
- ignoring non-record cameras
- multiple helper GPUs in the current GUI-conservative mode

### Next Test Checklist

- [ ] Add unit tests for the preflight result flattening helper
- [ ] Add unit tests that verify:
  - no split-GOP record-enabled cameras -> preflight is effectively a no-op
  - mixed session with one valid split-GOP camera and one single-session camera
    still passes
- [ ] If the preflight helper gains warning support, add warning-path tests too

### Manual Validation Checklist

- [ ] Valid `2010096 GPU5+GPU6` GUI session still starts streaming and recording
- [ ] Invalid non-`PIX` helper pair is blocked before stream start
- [ ] Two record-enabled split-GOP cameras with overlapping claimed GPUs are
      blocked
- [ ] Single-session recording remains unaffected

## Suggested Implementation Order

1. Add `RecordingPreflightResult` and the shared preflight helper.
2. Gate `Start streaming`.
3. Gate record start.
4. Add compact GUI error display.
5. Reuse the same helper in headless.
6. Add the extra preflight-specific unit tests.

## Non-Goals

This checklist does not propose:

- automatic GPU conflict resolution
- scheduling overlapping helper use across cameras
- editing advanced split-GOP settings yet
- exposing more queue/backlog tuning controls in the GUI

## Recommendation

Implement the stream-start guard first and keep it strict.

That gives the highest value with the least behavioral ambiguity:

- Orange will stop invalid sessions before pipeline creation
- the same validator remains reusable for record-start backup checks
- headless can adopt the exact same rule set afterward
