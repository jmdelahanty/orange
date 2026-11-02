# Empty-dish background capture

Date: 2026-11-02. Status: design agreed with Jeremy; not implemented. Nothing
here is declared to Palette yet: the file layout and schema ids below are
proposals until Palette agrees (standing rule: tell Palette before declaring
new recording files).

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
8. **Settings drift refuses the background, not the recording.** At record
   start, any difference in the checked settings drops the background for that
   camera and names the setting.
9. **Hard-frame mining belongs to Palette**, working from full-frame
   recordings and Orange's existing per-frame outputs. Orange writes no new
   per-frame data for it. Crop-only and held-ROI crops are out of scope for
   now, and there is no separate "training mode".

## Operator workflow

1. Set up cameras, illumination and dishes for the experiment, with water and
   no fish. Start streaming. Detection can be on or off.
2. Press **Capture empty background** and declare that the dishes are empty.
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
- **Failure:** a camera whose burst does not complete within a timeout gets no
  background. Other cameras keep theirs, and the descriptor says which camera
  failed and why.

## What is stored

Background store, outside any recording (proposed):

```text
~/orange_data/backgrounds/<capture_id>/
  background.json                   descriptor, written last
  Cam<serial>_empty_background.png  8-bit grayscale median, native raster
```

PNG is proposed because it is lossless, carries its own dimensions and opens in
any tool. Palette may prefer raw Mono8 plus dimensions; that is their call.

`background.json` (proposed `orange.empty_background_capture`, version 1)
carries, per camera:

- serial, width, height, offset_x, offset_y, pixel format;
- exposure, gain, iris, focus and frame rate as configured at capture;
- the stream and camera frame IDs and camera and host timestamps of the 17
  source frames;
- the method (`median_17`, version 1);
- the image file's size and SHA-256.

It also carries the capture time (UTC) and the operator declaration
(`dishes_empty: true`, free-text note). The registration id and geometry are
included only when a registration was selected; their absence is normal.

## Recording binding

At record start, after the operator confirmation and before the start
snapshot is sealed:

1. For each recording camera, compare the capture's checked settings with the
   live configuration: exposure, gain, iris, focus, width, height, offset_x,
   offset_y, pixel format. Frame rate is recorded, not checked, because each
   frame's illumination follows exposure, not frame rate.
2. A match copies the median into the recording and verifies its SHA-256. A
   difference refuses that camera's background and names each differing
   setting with both values. The recording still starts.
3. The start snapshot gets an `empty_background` block with the capture id and
   descriptor digest, and a per-camera status. Proposed statuses:
   `attached`, `not_selected`, `settings_changed` (with the list),
   `missing_in_capture`, `copy_failed`.
4. The GUI shows the per-camera result before the recording starts, and
   loudly for any refusal.

Proposed recording layout (at the recording root, outside
`recording_observation_bindings/`):

```text
<recording>/empty_background/
  background.json                   exact copy of the capture descriptor
  Cam<serial>_empty_background.png  exact copies, attached cameras only
```

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

1. **The background files:** layout, PNG or raw, descriptor fields. Tell
   Orange what intake needs before Orange declares anything.
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
3. Out of scope for Orange in this round: per-frame hard-frame lists, crop-only
   capture of missed frames and held-ROI crops.

## Deferred

- Capturing a background during or between recordings, with proof that a
  burst cannot affect a live recording.
- Backgrounds for several illumination or projector states.
- Fish-present backgrounds.
- Held-ROI crops and other crop-only training capture.

## Implementation checklist (Orange)

- [ ] Palette agrees on the file layout and descriptor; schemas in new files
      under `docs/schemas`, through the output ratchet.
- [ ] Burst capture and median on the snapshot worker; refused while
      recording; per-camera timeout and failure reporting.
- [ ] Background store and descriptor, written atomically, descriptor last.
- [ ] GUI: Capture empty background (with the empty-dish declaration),
      selection at record start, the per-recording confirmation and the
      per-camera result display.
- [ ] Record-start binding: settings check, copy, digest, start-snapshot block.
- [ ] Headless spec key and the same binding.
- [ ] Tests: median correctness, settings-drift refusal per setting,
      missing camera, copy failure, a recording that never selects one.
- [ ] Rig check: capture at 4512 x 4512 and 4512 x 1792, then a recording with
      the background attached, with 0 camera frame-id gaps and unchanged NIC
      counters.
