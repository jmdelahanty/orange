# Empty-dish background capture

Date: 2026-11-02. Status: design agreed with Jeremy; file layout, descriptor
and statistic names agreed with Palette (session palette-empty-background,
2026-11-02, checked against Palette main f6f23487). Schemas merged in
agent-contracts `orange-palette/empty_background_v1` and, on 2026-11-15,
`orange-palette/empty_background_v2` (603da1fe: requester, declared_by, the
local_control selection, a schema_version on the block). Orange writes
version 2 since fb0c3ae. Built on 2026-11-15 (branch
`agent/gui/empty-background-20261102`): the core library, the 17-frame burst
on the landing die, the GUI panel, the local-control capture and start
selection, the record-start binding and the headless key. Not yet run on the
rig (see the checklist).

Additions on 2026-11-15 (Jeremy):

- **Burst (option 2/A):** each camera's 17-frame staging area is reserved once
  per stream, last at stream start (after the stream's own allocations), never
  mid-stream. A failed reservation refuses only that camera's capture.
- **Remote starts work unattended:** local-control `capture_empty_background`
  and `start_recording` `params.empty_background` (by capture_id and
  descriptor sha256, nothing implicit). See
  `docs/orange_local_control_contract.md`.
- **A capture that cannot be attached refuses the start** when a remote start
  named it or the operator chose it; without a choice the recording records
  every camera not_selected.
- The panel never offers the newest capture by default: the operator chooses
  "Attach this capture" and confirms the scene per recording.

Bound runner (spec-runner branch, 2026-11-16):
`scripts/run_orange_citrus_bound.sh --empty-background capture_at_setup` (or a
plan session whose `orange.empty_background` is `capture_at_setup`) captures
inside the same GUI process and stream as the recording: launch, Citrus
checks, capture (the launch declares the dishes empty),
`<out-dir>/empty_background_capture.json`, then a wait on
`<out-dir>/scene_confirmation.sock` (mode 0600, the invoking user's) for
`scripts/confirm_bound_run_scene.py --run-dir <out-dir> --confirmed-by <id>`
(fish loaded, dishes not moved; or `--abort`). The runner names the caller by
SO_PEERCRED (pid, uid, gid, user, /proc exe and cmdline), refuses a uid that
is neither the invoking user's nor root's, and itself writes
`<out-dir>/scene_confirmation.json` once with `requester.peer`. A request for
another capture or with another decision is answered, recorded
(`refused_requests`) and the wait goes on. Only an accepted request sets
`scene_unchanged_confirmed`; a timeout or an abort refuses the start.

The display is declared from Citrus, never `unknown`. Before START Citrus
clears its output every frame to rgba8 0,0,0,255 (projector black, not zero
light; Citrus 027d292 code, not measured on the rig) unless an arena preview
or a calibration scene is active. The runner reads Citrus's status
immediately before the capture and again once Orange reports it finished,
and declares `stimulus_display_declaration` {source `citrus_idle_clear`,
fill_rgba_u8 [0,0,0,255], citrus_build, evidence: both reads (time, every
arena's active_preview_mode, calibration_scene, photodiode marker or
`not_reported`, idle_display), evidence_used per read,
display_frame_index_range} in the capture record.
Orange's descriptor gets `{state: on, shown: {kind: solid_color, rgba8:
[0,0,0,255]}}` and a note naming the Citrus build. Evidence: Citrus's
`status.display.idle_display` when reported (written by its render thread
after each swap; citrus-agent branch `agent/citrus/idle-display-status`
17fbc41, not installed): in both reads source `idle_clear`, fill_rgba_u8
[0,0,0,255], no `canvas_fill_overlay`, `preview_arena_count` 0,
`photodiode_marker_patches_rendered` false (lit marker patches are not solid
black, so true refuses), and `display_frame_index` advancing from the first
read to the second (the display is live); every arena that reports
`active_preview_mode` / `protocol_step_preview_active` must say `NONE` /
false. Otherwise every arena must report both (`NONE`, false) and no
calibration scene may be active. Citrus 027d292 reports neither (its `arenas[]` carry no `active_preview_mode`; the
mode appears only in `active_projection_snapshot` for calibration-grid
previews), so with it the capture is refused, naming what Citrus must
advertise (both land with Citrus `agent/citrus/idle-display-status`). There
is no operator override (Jeremy, 2026-11-16): an automated runner never asks
the operator to declare the display. Projector black is `state: on`,
solid_color [0,0,0,255] (confirmed by Jeremy). A display that differs after the capture refuses the start (the
capture stays in the store, never named). A failed
camera or unreserved staging refuses before the fish go in. `--fish-free-test`
lets the runner confirm itself (decision `fish_free_test_scene_unchanged`),
refused when the declaration names subjects. Both files stay in the run's
out dir; the recording gets only Orange's own `empty_background/` copy and
`session.empty_background`.

## Purpose

Give any recording an optional image of the empty dish, taken during setup
under the recording's exact camera settings and illumination. Palette can then
use background subtraction on the recording, for example to generate training
data and to find the fish in frames where the live detector missed it.

## Decisions (Jeremy, 2026-11-02)

1. **Empty dish, captured before the fish go in.** No fish-present median and
   no masking of the fish with YOLO boxes.
2. **Same production state as the recording:** streaming, IR strobe on, the
   same exposure, gain, iris, focus, frame size and offset.
3. **Independent of dish registration.** Any recording can carry a background
   in any media mode. Registration geometry is attached when one exists and is
   never required.
4. **Never required.** A recording without a background starts normally; its
   snapshot says no background was attached.
5. **One capture, at setup.** Capturing again during or between recordings is
   deferred until it can be shown never to disturb a live recording. Most
   recordings are about 30 minutes.
6. **Keep only the median**, not the burst of source frames.
7. **No YOLO check of the burst.** The detector still fires on dust and
   bubbles, so it would give false warnings.
8. **Settings drift is copied and flagged, never refused.** At record start,
   a camera whose checked settings differ from the capture still gets its
   median copied into the recording, with status `settings_changed` naming each
   setting and both values. The recording starts. Palette decides per use
   whether it can model the difference or must refuse the image. (Jeremy,
   2026-11-02; this replaces the first version of this decision, which dropped
   the image.)
9. **Hard-frame mining belongs to Palette**, working from full-frame
   recordings and Orange's existing per-frame outputs. Orange writes no new
   per-frame data for it. Crop-only and held-ROI crops are out of scope for
   now, and there is no separate "training mode".

## Operator workflow

1. Set up cameras, illumination and dishes for the experiment, with water and
   no fish. Start streaming. Detection can be on or off.
2. Press **Capture empty background** and declare that the dishes are empty
   and what the stimulus display shows (off, or on with what canvas or colour).
   Orange cannot observe the display (Citrus/SpecSail drive it, and there is no
   Citrus binding before a recording), so this is an operator declaration.
   Orange captures every streaming camera and saves one capture to the
   background store. No recording starts.
3. Add the fish. Do not change camera settings or illumination.
4. At record start, the newest capture is offered. Confirm "scene unchanged
   since the background, apart from adding the fish", or choose no background.
   The confirmation is per recording and is not saved across restarts.
5. Orange checks the settings per camera, copies the accepted medians into the
   recording and records the result in the start snapshot.

Headless specs name a capture by path (proposed key
`fixed.empty_background_capture`). The same checks apply; a missing key means
no background.

## Capture

- **Burst:** 17 consecutive frames per streaming camera, per-pixel median, 8-bit
  Mono8 at the native recorded raster.
  - Why 17: an odd count makes the median one of the real samples, so no tie
    rule is needed for an even count. The median of N frames cuts random sensor
    noise to about 1.25/sqrt(N) of one frame's: 0.30 for 17, against 0.22 for 33
    and 0.14 for 81. Past about 17 the gain is small and memory grows. The count
    came from the August crop-only design; it stays a configured constant in the
    descriptor (`method: median_17`, version 1), not an operator knob.
  - Worked example and the table behind the choice:
    `docs/background_median_frame_count.md`.
  - At 250 Hz the burst lasts 68 ms, at 100 Hz 170 ms. Nothing in an empty dish
    moves, so consecutive frames are fine.
- **Cameras:** all streaming cameras in one action, each with its own burst.
  Simultaneity across cameras is not needed for an empty background, and none
  is claimed.
- **Refused while a recording is armed, recording or finalizing.** This is the
  rule that keeps the capture away from live recordings (decision 5).
- **Hot-thread rule:** the acquisition thread only hands out frames. Device to
  host copies, the median, hashing and file writes run on the snapshot worker
  and a housekeeping CPU, as the daily native context capture does today.
- **Memory:** 17 frames of 4512 x 4512 is 346 MB per camera (4512 x 1792: 137 MB).
  Frames are copied to host, the median is computed per row band, and the
  source frames are freed. The daily context's admission limits (64 MiB per
  image, 256 MiB total) are for one frame and need their own burst limits.
- **Saturation:** while computing the median, Orange also counts the pixels
  that were 255 in any of the 17 frames (`any_frame_saturated_fraction`). The
  median cannot show these, and they mark where subtraction is unreliable.
- **Failure:** a camera whose burst does not complete within a timeout gets no
  background. Other cameras keep theirs, and the descriptor says which camera
  failed and why.

## What is stored

Background store, outside any recording:

```text
~/orange_data/backgrounds/<capture_id>/
  background.json                   descriptor, written last
  Cam<serial>_empty_background.png  8-bit grayscale median, native raster
```

**Image (agreed with Palette):** PNG, 8-bit greyscale, no alpha, no gamma,
iCCP or sRGB chunks. Palette refuses a PNG whose header disagrees with the
descriptor's width, height or pixel format.

`background.json` (`orange.empty_background_capture`, version 1). Field names
follow the daily native context (`orange.calibration.registered_native_context`
v2) where they overlap; sha256 values use the `sha256:<hex>` form.

- `capture_id`, `captured_at_utc`.
- `scene_declaration`: `dishes_empty: true`, free-text `note`, and
  `stimulus_display` with `source: "operator_declared"` and `state`: `off`
  (a display in the light path, dark), `on`, `unknown` (Palette treats it
  like on with an unknown scene) or `not_displayed` (no display in the light
  path). Only `on` carries `shown`: `{kind: solid_color, rgba8: [r,g,b,a]}`
  (the rigs' colour standard, sRGB rgba8), `{kind: canvas, canvas_name}` or
  `{kind: other, note}` when no exact value is known.
- `method`: `name: "median_17"`, `version: 1`, `frame_count: 17`.
- `pixel_declaration`: raw sensor Mono8, full range, no conversion. Palette
  compares the median with decoded full-frame HEVC and must not apply a
  tv-range shift.
- `registration_id` and geometry, only when a registration was selected; their
  absence is normal.
- `cameras[]`, per camera:
  - `camera_serial`;
  - `camera_configuration`: `width`, `height`, `offset_x`, `offset_y`,
    `pixel_format` (`Mono8`), `frame_rate_hz`, `exposure_us`, `gain`, `focus`,
    `iris`, as configured at capture;
  - `source_frames[17]`: `stream_frame_id`, `camera_frame_id`,
    `camera_timestamp_ns` (PTP/TAI; host clock = minus 37 s),
    `timestamp_sys_ns`;
  - `image`: `relative_path`, `size_bytes`, `sha256`;
  - `statistics` of the median, over the whole image: `saturation_value` (255),
    `saturated_fraction` (median == 255), `any_frame_saturated_fraction`,
    `zero_fraction`, `mean`, `p01`, `p05`, `p50`, `p95`, `p99`, `p999`. The
    image-levels check uses the same names (without the burst-only
    `any_frame_saturated_fraction`). p999 is there because p99 is already 255
    on the 2026-11-01 setup (about 3.4% of pixels clip in a single frame);
  - `failure` with a reason instead of `image` and `statistics` when the burst
    did not complete.

No per-pixel saturation mask and no 17-frame max image (Palette derives
median == 255 itself).

## Recording binding

At record start, after the operator confirmation and before the start
snapshot is sealed:

1. For each recording camera, compare the capture's checked settings with the
   live configuration: exposure, gain, iris, focus, width, height, offset_x,
   offset_y, pixel format. Frame rate is recorded, not checked, because each
   frame's illumination follows exposure, not frame rate.
2. The median is copied into the recording and its SHA-256 verified, both on
   a match (`attached`) and on a difference (`settings_changed`, naming each
   differing setting with both values). The recording starts either way.
3. The start snapshot gets an `empty_background` block on **every** recording,
   with the capture id and descriptor digest when one is attached, and a
   status per recording camera, keyed by `camera_serial`, so every image in
   the recording copy maps to exactly one status. Statuses: `attached`,
   `settings_changed` (with the list), `not_selected`, `missing_in_capture`,
   `copy_failed`. A recording without a capture says `not_selected` for every
   camera: a declared absence, not a missing block. The status lives in the
   snapshot, not in the copied descriptor, which stays byte-exact.
4. The GUI shows the per-camera result before the recording starts, and
   loudly for `settings_changed` and any failure.

Recording layout (at the recording root, outside
`recording_observation_bindings/`; agreed with Palette):

```text
<recording>/empty_background/
  background.json                   exact copy of the capture descriptor
  Cam<serial>_empty_background.png  exact copies, attached and settings_changed cameras
```

The directory is written at record start, so it exists before the transfer
sealer runs and the seal covers it. Palette checks each image's SHA-256
against the descriptor and the seal. Intake does not refuse undeclared files,
so no inventory change is needed first.

Rolling clips share the parent recording's background, as they share its
other context.

## Reuse from the September context code

The daily native context capture (`docs/daily_native_context_capture_2026-09-06.md`,
`src/gui/spatial_layout/daily_native_context_capture.cpp`,
`src/daily_registered_context.*`, `src/spatial_snapshot_worker.*`) already
solves the hard parts:

- exact native frames from the pool-owned ring copy with detection off;
- the housekeeping-CPU save task;
- an atomic descriptor written last;
- the byte-exact copy into a recording;
- per-recording confirmation and the settings comparison
  (`src/recording_daily_context_reuse.*`).

The new feature reuses those pieces without the registration requirement and
adds the burst median. The registered-context and crop-only paths stay as
they are.

## For Palette

1. **The background files:** agreed 2026-11-02 (sections above). Palette
   asked for the schema in agent-contracts, as for `yolo_event` v3, so it can
   vendor it under `shared/contracts`. Palette treats the background as a
   prior, not a truth: it reads the snapshot status before any use, refuses
   `settings_changed` unless the consumer handles the listed differences,
   checks the median against fish-free regions of the decoded video per
   recording (the dish can move when the fish are added), and builds its own
   background when no capture exists. The full-frame video is lossy (about
   4.8 grey levels MAE), so a difference against decoded HEVC is good for
   mining candidate frames, not for pixel-level labels.
2. **Inputs for hard-frame mining**, all existing outputs of a full-frame
   recording:
   - `Cam<serial>_yolo_events.jsonl`: per-frame detections with confidence,
     box, `yolo.status`, spatial-mask counts and the frame identity
     (`recording_frame_id`, `stream_frame_id`, `camera_frame_id`).
   - `Cam<serial>_crop_meta.csv`: `has_detection`, `crop_state`
     (`blank_no_detection` on a miss), detection and crop rectangles.
   - Pose events and per-frame `pose_status`, when the pose stage runs.
   - The full-frame video and its `_meta.csv`; `recording_frame_id` is the
     video key (frame identity v1).
   - Signals seen on the 2026-11-01 250 Hz session (`2026_11_01_14_19_25`):
     13.5% of frames without a detection, rare detection-centre jumps of over
     1000 px between frames, and pose keypoint jumps of 15 to 40 px.
3. Open for Jeremy (raised by Palette): whether mined hard frames get labels
   from the full-frame video or from the lossless crop when one exists.
4. Out of scope for Orange in this round: per-frame hard-frame lists, crop-only
   capture of missed frames and held-ROI crops.

## Deferred

- Capturing a background during or between recordings, with proof that a
  burst cannot affect a live recording.
- Backgrounds for several illumination or projector states.
- Fish-present backgrounds.
- Held-ROI crops and other crop-only training capture.

## Implementation checklist (Orange)

- [x] Palette agrees on the file layout and descriptor (2026-11-02).
- [x] Schema drafts (2026-11-02): `orange_empty_background_capture_v1`,
      `orange_recording_empty_background_v1` (start-snapshot block),
      `orange_image_levels_v1` (shared `statistics`, also used by the
      image-levels check), with examples; codegen and jsonschema agree on them.
- [x] Palette review of the drafts; agent-contracts
      `orange-palette/empty_background_v1` merged (schemas and fixtures
      byte-identical to this branch).
- [x] With the writer: `ORANGE_GENERATED_SCHEMAS` in CMake,
      `published_schemas.EMBEDDED` entry for `/session/empty_background`, and
      `check_output_surface.py --update`; fixtures tested in jsonschema and the
      generated readers (`empty_background_schema_fixture_tests`).
- [x] Burst capture (`src/empty_background_burst_worker.*`, median kernel
      `src/empty_background_median.cu`): owned copies only, never the
      GPUDirect ring; refused while recording; 5 s timeout per camera;
      min free pool entries/events recorded per burst.
- [x] Background store and descriptor, written atomically, descriptor last
      (`orange::empty_background::WriteCapture`).
- [x] GUI: "Empty-dish background" panel (`src/gui/empty_background_capture.*`):
      capture with the declaration (declared_by, dishes empty, display), the
      per-camera result and staging state, and the per-recording selection
      with its confirmation.
- [x] Local control: `capture_empty_background`, `start_recording`
      `params.empty_background`, status `local_control.empty_background_capture`.
- [x] Record-start binding: settings check, copy, digest, start-snapshot block
      (`BindToRecording`).
- [x] Headless spec key `fixed.empty_background_capture` (absolute path of
      background.json) and the same binding.
- [x] Tests: median correctness, statistics, settings-drift flag per setting,
      missing camera, copy failure, a recording that never selects one
      (`empty_background_tests`, `empty_background_schema_tests`).
- [x] Rig check (orange, 2026-11-15, production 60340307), both through
      local control:
      - 4512 x 4512 at 30 Hz: staging 366,446,600 B per camera, burst 536 ms,
        median 28-40 ms, min free entries 62 of 64, max queue depth 1;
        recording `2026_11_15_12_04_35` attached v2 on all 4 cameras,
        539/539 frames each, 0 drops, NIC counters unchanged.
      - 4512 x 1792 at 250 Hz: staging 145,539,080 B, burst 66 ms, median
        8-11 ms; recording `2026_11_15_12_15_15` attached on all 4,
        4725/4725 frames, 0 drops, NIC unchanged. Validator PASS on both.
      - GPU memory while recording (landing / paired die): 5.0 / 0.77 GB at
        full frame, 3.3 / 0.39 GB at 1792 rows, of 15.3 GB.
- [ ] Rig: press the GUI panel once (capture, attach with the confirmation,
      record; attach without it is refused). Needs a person at the rig.