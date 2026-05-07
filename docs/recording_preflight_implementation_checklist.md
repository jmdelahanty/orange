# Recording Preflight Implementation Checklist

## Purpose

This checklist records the implementation state for the split-GOP recording
preflight gate. It was originally written as the execution order for turning
the shared recording validator into a real preflight gate instead of only a
read-only GUI summary.

The intent is to prevent Orange from starting a recording session whose GPU
topology or resource claims are already known to be invalid.

## Current State

Updated 2026-05-04 on `exp/gop-split-a16`:

- split-GOP validation rules are documented in
  [advanced_recording_validation_plan.md](/home/jeremy/orange-gop-split-a16/docs/advanced_recording_validation_plan.md)
- the GUI shows a read-only validation summary in
  [src/gui/recording_panel.cpp](/home/jeremy/orange-gop-split-a16/src/gui/recording_panel.cpp)
- the validation logic and preflight helper now live in the shared helper
  module:
  - [src/recording_validation.h](/home/jeremy/orange-gop-split-a16/src/recording_validation.h)
  - [src/recording_validation.cpp](/home/jeremy/orange-gop-split-a16/src/recording_validation.cpp)
- unit tests cover the core policy rules and preflight flattening in
  [tools/recording_validation_tests.cpp](/home/jeremy/orange-gop-split-a16/tools/recording_validation_tests.cpp)
- GUI stream start and record start both run the shared preflight before
  proceeding.
- Headless local/spec startup reuses the same shared preflight before
  recording begins.
- GUI preflight errors are stored and shown near the recording controls.

This checklist is retained as an implementation audit. The core preflight gate
has landed; remaining items are validation coverage or future UX polish.

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

- [x] Add a preflight result struct, for example:
  - `RecordingPreflightResult`
  - fields:
    - `bool ok`
    - `std::vector<CameraRecordingValidationSummary> summaries`
    - `std::vector<std::string> errors`
    - optional `std::vector<std::string> warnings`
- [x] Add a helper that builds validation inputs from the current session state
- [x] Add a helper that runs `validate_recording_configuration(...)` and
      flattens errors into user-facing messages
- [x] Keep this helper outside the GUI layer so GUI and headless can both use it

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

- [x] Build one validation input per camera from:
  - `cameras_params[i].gpu_id`
  - `cameras_params[i].recording.strategy`
  - `cameras_params[i].recording.constraints`
  - `cameras_select[i].record`
- [x] Do **not** turn `gpu_id` into a list
- [x] Continue treating the claimed GPU set as:
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

- [x] Run the shared recording preflight immediately before flipping
      `camera_control->subscribe` to `true`
- [x] Only gate when the session actually includes record-enabled split-GOP
      cameras
- [x] If preflight fails:
  - [x] keep `camera_control->subscribe` unchanged
  - [x] do not allocate recording pipelines
  - [x] capture the preflight errors into GUI-visible state
- [x] If preflight succeeds:
  - [x] continue with the existing stream-start path unchanged

## GUI Gate: Start Recording

### Goal

Recheck the preflight before toggling `record_video` on.

This is a backup guard and keeps the rule close to the record action as well.

### Main Touch Point

- [src/orange.cpp](/home/jeremy/orange-gop-split-a16/src/orange.cpp)
  around the record play/pause button and `camera_control->record_video`

### Checklist

- [x] Before the `"next_record_state = true"` path proceeds, rerun the shared
      preflight
- [x] If preflight fails:
  - [x] do not set `record_video = true`
  - [x] do not create recording metadata for that run
  - [x] show/store the error messages
- [x] If preflight succeeds:
  - [x] leave the current record-start behavior unchanged

## GUI Error Surface

### Goal

Make failures visible enough that the operator understands what blocked the
session.

### Checklist

- [x] Add lightweight GUI state for the most recent preflight result
- [x] Show a compact red summary near the recording controls when the last
      preflight failed
- [x] Support multiple error lines, for example:
  - topology mismatch
  - missing peer access
  - overlapping GPU claims
- [x] Keep the read-only advanced validation summary in place
- [x] Do **not** silently auto-correct invalid GPU choices

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

- [x] Reuse the same shared preflight helper before headless recording begins
- [x] Fail early with a clear log message if preflight fails
- [x] Include the specific validation errors in stderr/stdout
- [x] Do not partially start a run that already violates split-GOP topology or
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

- [x] Add unit tests for the preflight result flattening helper
- [ ] Add unit tests that verify:
  - [x] no split-GOP record-enabled cameras -> preflight is effectively a no-op
  - [ ] mixed session with one valid split-GOP camera and one single-session camera
    still passes
- [ ] If the preflight helper gains warning support, add warning-path tests too

### Manual Validation Checklist

- [x] Valid `2010096 GPU5+GPU6` GUI session still starts streaming and recording
- [x] Invalid non-`PIX` helper pair is blocked before stream start
- [ ] Two record-enabled split-GOP cameras with overlapping claimed GPUs are
      blocked
- [x] Single-session recording remains unaffected

## Suggested Implementation Order

1. [x] Add `RecordingPreflightResult` and the shared preflight helper.
2. [x] Gate `Start streaming`.
3. [x] Gate record start.
4. [x] Add compact GUI error display.
5. [x] Reuse the same helper in headless.
6. [ ] Add the remaining preflight-specific unit/manual tests noted above.

## Non-Goals

This checklist does not propose:

- automatic GPU conflict resolution
- scheduling overlapping helper use across cameras
- editing advanced split-GOP settings yet
- exposing more queue/backlog tuning controls in the GUI

## Recommendation

The stream-start guard is implemented and should stay strict.

That gives the highest value with the least behavioral ambiguity:

- Orange will stop invalid sessions before pipeline creation
- the same validator remains reusable for record-start backup checks
- headless can adopt the exact same rule set afterward
