# Crop Recording Lifecycle Fix Plan

Date: 2026-05-27

Scope: Implement fixes for invalid cropped MP4 outputs and crop frame pool
misses observed in the long GUI/external-recorder recording at
`/home/jeremy/orange_data/exp/unsorted/2026_05_22_13_37_22`.

Update, 2026-05-27: follow-up GUI runs showed crop preview itself is not the
remaining frame-pacing bottleneck. With crop preview hidden, main-display
downsample set to `4`, and display preview capped at `30 fps`, the GUI still
fell to roughly `15-25 fps` while full-frame external IPC and YOLO stayed
healthy. Crop encode service/submit times remained high enough to suspect
same-process crop NVENC/video-writing pressure. The first implementation slice
keeps crop ROI generation in process but adds an opt-in external crop recording
sink so crop video encode/write can be moved out of the GUI process for
diagnosis.

## Bottom Line

The full-frame external-recorder path is healthy in the affected recording.
Each external MP4 decodes, all four cameras sustained about 100 fps, and the
external recorder ACKed/encoded every submitted full-frame descriptor with no
drops.

The crop path has two separate problems:

- The crop MP4 files are not valid HEVC-in-MP4 files. `ffprobe` reports
  missing stream info, the MP4 `hvcC` codec configuration is effectively empty,
  and the first indexed keyframe is not sample 0.
- The crop frame pool is too small for the observed crop encode/preview
  service time on several cameras. This caused `crop_frame_pool_empty` drops,
  especially on cameras `2010095` and `2010096`.

Do not treat these as camera acquisition drops. The full-frame acquisition and
external recording lifecycle was intact.

## Evidence From The Recording

Artifact:

```text
/home/jeremy/orange_data/exp/unsorted/2026_05_22_13_37_22
```

Full-frame external recording:

- `external_recorder/Cam*_external.mp4` decodes as HEVC, `4512x4512`, 100 fps.
- `scripts/verify_external_recorder_session.py --allow-missing-video-sanity`
  passed for the external recorder folder.
- Each camera had full-frame external frames ACKed and encoded with no external
  drops.
- Pipeline telemetry showed zero camera dropped frames, zero GetFrame errors,
  and zero external IPC failures.

Crop recording:

- All four `Cam*_crop.mp4` files fail `ffprobe` with invalid HEVC stream data.
- Crop keyframe sidecars begin at frame 9 for three cameras and frame 9/10 for
  the fourth, while the external full-frame keyframe sidecars begin at frame 0.
- MP4 trace inspection showed an `hvc1` track with an unusably small `hvcC`
  box and length-prefixed packet data that the demuxer could not interpret
  without valid codec configuration.

Crop pool misses:

```text
2010093: pool_empty=0,   dropped=0,   zero_packets=3
2010094: pool_empty=1,   dropped=1,   zero_packets=4
2010095: pool_empty=48,  dropped=48,  zero_packets=51
2010096: pool_empty=143, dropped=143, zero_packets=146
```

Pose was disabled for this recording, so the pool misses are from crop
recording/preview pressure, not pose holding crop frames.

## Current Lifecycle Shape

Full-frame lifecycle:

1. Acquisition obtains a `WORKER_ENTRY` from the camera acquire pool.
2. Acquisition sets consumer refcounts for display, YOLO, and recording.
3. The external recorder path detaches from the source frame, exports a CUDA IPC
   descriptor, waits for ACK, then releases the original `WORKER_ENTRY`.
4. The recorder process owns its encode slots and writes valid HEVC MP4 outputs
   using the external split-GOP lifecycle.

Crop lifecycle:

1. YOLO chooses a detection and `CropProducerWorker` asks `CropProducer` for a
   `CropFrame`.
2. `CropProducer` takes a frame from its fixed pool, copies the ROI into
   `CropFrame::d_crop_mono`, records a ready event, and releases the original
   acquisition `WORKER_ENTRY` after the crop copy is safe.
3. `CropAndEncodeWorker` consumes the `CropFrame`, copies it into the crop
   NVENC input image and optional preview image, then calls
   `RecycleAfterConsumerStream`.
4. `RecycleAfterConsumerStream` records a recycle event on the crop worker
   stream; the crop frame returns to the pool once that event has completed.
5. The crop worker writes packets through the bespoke `FFmpegWriter` path.

The crop frame is not intentionally held until NVENC encode completion. That is
good and should be preserved. However, slow crop worker service still makes
jobs wait in the crop worker queue while holding `CropFrame` leases, so the
small pool can empty.

## Fix 1: Make Crop MP4 Files Valid

The crop writer must start each MP4 segment with a usable keyframe and valid
HEVC codec configuration.

Implementation requirements:

- Force the first encoded crop frame in every crop MP4 to be an IDR frame.
- Request VPS/SPS/PPS output with that first IDR frame.
- Repeat the same rule on any future crop file rollover or clip boundary.
- Ensure the first keyframe sidecar entry is frame 0 for each crop MP4.
- Populate MP4 HEVC codec configuration (`hvcC`) correctly before or while
  writing the container header.
- Preserve monotonic PTS/DTS and packet duration semantics at 100 fps.

Preferred implementation direction:

- Reuse the full-frame/external recorder MP4 packet/container path where
  possible instead of continuing to expand the bespoke crop `FFmpegWriter`.
- Keep crop-specific ROI generation separate, but share HEVC packet handling,
  parameter-set extraction, keyframe accounting, and MP4 validation behavior.

Minimum viable implementation:

- Add crop-worker state equivalent to the full-frame encoder's `force_next_idr`
  behavior.
- Set `NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS` for the first
  crop frame written after opening a crop MP4.
- Update or replace `FFmpegWriter` so HEVC extradata is valid for `hvc1` MP4
  output. If the writer cannot know VPS/SPS/PPS until the first encoded packet,
  delay `avformat_write_header` until those parameter sets are available or
  move crop output to a writer that already supports this.
- Add a guard that fails the recording validation if a crop MP4's first
  keyframe is not frame 0.

Code touchpoints:

- `src/crop_and_encode_worker.cpp`
- `src/FFmpegWriter.cpp`
- Any shared encode profile/container helpers already used by the full-frame
  or external recorder path
- Recording validators under `scripts/`

## Fix 2: Add Crop Pool Headroom And Backpressure

The current crop frame pool default is 8. In the affected recording, crop worker
service time on some cameras was high enough to exhaust that pool. Raising the
pool size alone is likely necessary but not sufficient; the queueing policy and
metrics should make pool pressure explicit.

Implementation requirements:

- Make crop frame pool size a normal recording/runtime setting, not only an
  environment override.
- Raise the production default for 100 fps cropped recording. A starting value
  of 32 is consistent with recent pose/crop diagnostic settings and gives one
  GOP-length burst of headroom.
- Record the effective crop pool size in `recording_snapshot.json`,
  crop sidecars, and relevant perf output.
- Add runtime metrics for free crop frames, pending recycle frames, pool wait
  time, pool miss count, crop worker queue high water, and crop queue residence
  time.
- Bound the crop worker queue relative to the crop pool, or explicitly document
  and implement a best-effort crop-drop policy when the consumer cannot sustain
  the offered rate.
- Preserve the current early recycle shape: after crop-to-NVENC-input and
  preview copies are safely enqueued, recycle the `CropFrame`; do not hold it
  through NVENC encode completion unless correctness requires it.

Backpressure policy options:

- Strict mode: no intentional crop drops. Use a larger pool, bounded queue, and
  fail validation if any `crop_frame_pool_empty` row appears.
- Best-effort mode: allow crop drops under overload, but make them explicit in
  the manifest, perf CSV, and UI so a user cannot mistake a partial crop video
  for a complete per-frame crop record.

For production recording, strict mode is the safer default until the UI and
manifest clearly communicate best-effort crop completeness.

Code touchpoints:

- `src/crop_producer.h`
- `src/crop_producer.cpp`
- `src/crop_producer_worker.cpp`
- `src/crop_and_encode_worker.cpp`
- Snapshot/session manifest writing code
- Crop and GUI validators under `scripts/`

## Fix 3: Separate Preview Cost From Recording Correctness

Crop preview should not be able to make the recorded crop artifact invalid or
silently incomplete.

Implementation requirements:

- Measure crop preview copy/display time separately from crop encode time.
- Add a validation mode that can disable crop preview while keeping crop
  recording enabled. Use it to distinguish recording throughput from GUI
  presentation overhead.
- If preview remains enabled during production recording, ensure preview uses a
  copy-owned buffer and does not extend `CropFrame` lifetime beyond the copy
  event.
- Expose preview-related drops or skips separately from crop recording drops.

## Fix 4: Integrate Crop Artifacts Into Session Validation

Today the external full-frame path has strong summary and verifier coverage.
The crop path needs comparable artifact checks.

Implementation requirements:

- Add crop MP4 validation to the GUI/external recording validator.
- Check that each enabled crop artifact has:
  - a decodable video stream,
  - expected dimensions,
  - expected frame rate,
  - valid packet count/frame count accounting,
  - first keyframe at frame 0,
  - nonempty keyframe sidecar,
  - crop metadata rows matching the intended blank/detection/drop policy.
- Include crop artifacts in `recording_session.json` or a linked crop artifact
  manifest with paths, packet counts, drop counts, and validity status.
- Make validation failure distinguish between invalid container, crop pool
  overload, no detections, and intentional best-effort drops.

## Fix 5: Isolate Crop Video Output From The GUI Process

The crop images are small, but crop video output is not free. Per-frame fixed
costs still include CUDA/NVENC submission, synchronization, packet handoff,
writer thread pressure, and MP4/keyframe bookkeeping. Those costs can perturb
the GUI process even when the pixel count is only `256x256`.

Implementation requirements:

- Keep crop ROI selection, crop metadata, and crop perf telemetry in Orange.
- Add an opt-in crop recording sink mode, defaulting to the existing
  in-process writer.
- In external crop sink mode, export the crop-owned Mono8 CUDA buffer through
  CUDA IPC and wait for the external recorder to ACK a safe detach/copy.
- Preserve crop metadata/perf rows in the Orange recording folder so validation
  can still compare YOLO rows, crop rows, and external summary counts.
- Do not treat external crop output as production-ready until recorder
  supervision, summary ingestion, session metadata, and validators cover it.

First slice status:

- `ORANGE_CROP_RECORDING_SINK_MODE=external_ipc` is the intended opt-in switch.
- In-process crop recording remains the default.
- This is a crop video-output isolation path, not an out-of-process YOLO/crop
  ROI-selection path.
- GUI/session supervision now starts a second external-recorder lifecycle for
  crop streams when the opt-in switch is set. Crop streams use crop-suffixed
  sockets and artifacts so they do not collide with the full-frame external
  recorder.
- The normal in-process crop path revalidated on
  `/home/jeremy/orange_data/exp/unsorted/2026_05_27_16_17_49`: all four cameras
  wrote aligned crop artifacts with `1335` rows and no crop drops.

## Suggested Implementation Order

1. Add validators first for the known failure mode:
   `ffprobe` crop MP4, first keyframe equals 0, and crop pool miss counts are
   reported clearly.
2. Fix crop MP4 keyframe/extradata handling.
3. Add crop pool sizing to config/snapshot and raise the production default for
   100 fps crop recording.
4. Add crop pool/queue residence metrics and backpressure policy.
5. Fix stop/drain/finalization ordering so crop workers close only after all
   recording crop jobs already in flight have drained.
6. Add the external crop video-output diagnostic sink and compare GUI FPS with
   crop encode in process versus out of process.
7. Run a short one-camera crop smoke with detections or synthetic ROI only to
   validate file/container correctness.
8. Run the four-camera GUI/external-recorder shape with crop output enabled and
   require no camera drops, no external recorder drops, valid external MP4s,
   valid crop MP4s, and zero crop pool misses in strict mode.
9. Re-run with pose enabled after the crop-only path is stable, because pose
   adds another `CropFrame` lease and can change pool pressure.

## Acceptance Criteria

A fix should not be considered complete until all of these hold for the target
recording shape:

- RedGUI and `ffprobe` can open every enabled `Cam*_crop.mp4`.
- The crop keyframe sidecar starts at frame 0 for every crop MP4.
- The crop MP4 has valid HEVC-in-MP4 codec configuration.
- `crop_frame_pool_empty = 0` in strict production crop mode.
- Crop metadata gaps are either zero or explicitly explained by an intentional
  best-effort drop policy in the manifest.
- Full-frame external recorder outputs remain valid and complete.
- Camera acquisition counters remain clean: zero camera frame-id gaps, zero
  GetFrame errors, and zero preprocess drops.
- YOLO latency does not regress from the current external IPC baseline beyond
  the agreed tolerance.

## Open Design Questions

- Should production crop recording be strict by default, or is best-effort crop
  recording acceptable if the manifest and UI make incompleteness obvious?
- Should crop recording continue to use a local `FFmpegWriter`, or should it
  move fully onto the shared recorder/output stack?
- Should crop videos include blank frames for no-detection frames, or should
  they represent only accepted detection crops with a separate sparse index?
- What crop keyframe cadence is desired after the first frame: every frame,
  every GOP, or only at clip boundaries?
- When pose is enabled, should pose consume the same crop frame lease or should
  pose get a separate copy-owned input slot to isolate pose latency from crop
  recording completeness?
