# Registered context and moving crops: activation — 2026-09-07

Status: implementation enabled in the isolated GUI and timed headless build;
live acceptance and production installation remain pending. Orange base `7031b17`,
branch `agent/acquisition/master-frame-journal-v1-20260906`, worktree
`/tmp/orange-timing-evidence-20260906`, build `/tmp/orange-timing-build-20260906`.
This is the eleventh slice of the Citrus detection-crop-only checklist.

Runner follow-up: [crop-only runner acceptance](crop_only_runner_acceptance_2026-09-07.md)
adds dedicated headless templates, GUI/orchestrator profiles and product-aware
validation. Wrapper source allowlists are updated; installing them is an operator
step. Live acceptance remains pending.

## What is selected

`registered_context_and_moving_crops` acquires full camera frames for real YOLO
and existing single-subject moving/lossless crop extraction. It creates a master
frame record, registered static context and moving crop videos, **not** a continuous
full-frame encoder or a discard sink. Full-frame only and full-frame-plus-crops
remain first-class choices. No codec, crop-window, GOP, GPU placement, selection,
blank-frame or pose behavior is changed by this slice.

No implicit default changes: existing per-camera choices are preserved when
`media_products` is absent. This is not fixed-region/multi-subject recording or
an atlas. It does not create a downstream visual overview.

## GUI configuration and startup

1. After daily registration and scene setup, capture/select the native daily
   context through the existing registration interface. See
   `daily_native_context_capture_2026-09-06.md` and
   `gui_registered_context_recording_2026-09-06.md`.
2. Before streaming, choose **Registered context + moving crops** in Recording
   media products. Select the recording cameras and real YOLO. Keep the existing
   external moving-crop backend and explicit crop GPU/encoder settings.
3. Configure Registered recording context with the saved daily context, master
   writer and context housekeeping CPUs. Save explicitly if desired. The product
   selection and evidence settings are persistent configuration, not a transient
   full-frame-disable flag.
4. Start streaming. Each selected camera needs a native Mono8 raster and its
   master source slot. The recorded product/membership is frozen at stream setup;
   a later change requires restarting streaming.
5. Confirm that the scene is unchanged before **each** recording. Start uses the
   existing asynchronous operator path, also used by Citrus-triggered starts.

Before arm, verify a real per-camera YOLO logger at cadence 1, external moving
crops, master ownership, context import/acceptance and the immutable media plan.
On the housekeeping worker, verify required markers and exact context custody;
then require successful crop supervisor startup. Missing context, a missing
recorder or stale plan cannot set `record_video=true`. A daily image retains its
original capture identity; importing it does not claim a new exposure.

Crop rolling control is independent of the unselected full-frame transport:
the crop supervisor receives the configured recording and clip duration whether
the saved full-frame backend is `real` or `external_ipc`.

## Timed headless configuration

Use a versioned explicit selection in the experiment spec:

```json
"media_products": {
  "schema_version": 1,
  "mode": "registered_context_and_moving_crops"
}
```

This is a `fixed` field, together with:

- `master_frame_journal`: enabled v1, explicit housekeeping writer CPUs.
- `registered_scene_context`: enabled with native pre-recording capture or saved
  daily-context reuse, its existing acceptance/declaration and worker CPU fields.
- `yolo_worker`: a real detector, `decimate=1`, event logger initialized.
- `crop_recording.mode=external_ipc`: explicit crop GPU placement; optional
  absolute `recorder_tool_path` resolves the crop executable independently.
- `recording_control`: a finite recording duration; optional `clip_seconds`.

Use the existing context and master configuration runbooks for complete records;
do not copy fake acceptance IDs/digests from test fixtures into a real experiment.
`tools/test_headless_media_products_spec.py` demonstrates camera-free parsing,
not registration or hardware readiness. Do not supply a full-frame
`external_recorder_contract` or `pre_encoder_reference_capture` for crop-only;
they would request an unselected product. The saved full-frame sink choice can
remain `real` or `external_ipc` without starting its recorder.

Actual initialized detector/crop workers, independent master, completed context
and started crop supervisors are checked again before arm. Hashing is on configured
housekeeping CPUs; no per-frame PTP polling, camera latches or new hot-path work
are introduced. The installed wrapper/allowlist is unchanged.

## Completion and experiment results

The common finalizer already produces the parent, per-video manifests and clip
index described in `crop_only_parent_and_clip_inventory_2026-09-07.md`.
The new **read-only** result validator requires an existing completed parent and
revalidates its master/context/media bindings, exact index JSON/CSV and every
per-video manifest. It never repairs or creates missing completion artifacts.
An interrupted or failed parent cannot pass because some clips happen to decode.

Headless `runs.json` reports:

- `recording_result_profile=master_bound_moving_crop_collection_v1`, product mode
  and `full_frame_encoder_settings_applicable=false`.
- `crop_media_status`, clip/frame counts, verified crop collection, clip-index
  reference, master frame record and registered-context binding.
- Full-frame `video_present=false` and `video_content_status=not_selected`.
  No aggregate video path or invented full-frame CSV is reported.

`runs.csv` also includes product, crop status/counts and full-frame encoder-setting
applicability. Existing encoder columns describe full-frame settings, not a new
crop profile; use the bound crop collection and supervisor plan for crop evidence.
Detector/pose result joins use the explicitly selected, master-validated crop
metadata rather than requiring `Cam<S>_meta.csv` from a full-frame encoder.

Completion always requires strict crop returned-identity, mux, exact-file and
decoded-raster/count evidence. Legitimate zero-detection blank crops remain valid.
Camera/acquisition/preprocess counter policies, GetFrame errors and measured
acquisition FPS checks remain active. Missing telemetry cannot produce an FPS
pass. A nonzero recording-process result forces the run to fail even if some
media artifacts completed. No exposure/PTP or raw-pixel-equality proof is implied.

## Acceptance checklist

- [x] Shared required-input and verified-context arm checks.
- [x] GUI product availability, per-run scene confirmation and frozen plan.
- [x] Independent crop supervision and single/rolling GUI preparation.
- [x] Read-only crop collection validation and headless result integration.
- [x] Explicit crop-metadata joins for detector/pose validators.
- [x] Missing/tampered evidence, unready owners, process-failure policy and
  valid blank/partial-tail fixtures covered in implementation or regression tests.
- [ ] Consecutive single and rolling GUI/operator/Citrus-triggered runs.
- [ ] Timed headless native-context and daily-context crop-only runs.
- [ ] Live cancellation, detector/recorder failure and missing-tail injection.
- [ ] Full-frame-only and full-frame-plus-crops live compatibility runs.
- [ ] Compare acquisition drops, pool pressure, encoder failure and throughput;
  verify housekeeping placement and finalization time under real workloads.
- [ ] Coordinate candidate master/crop-only records with downstream consumers
  before claiming Palette/Citrus timing admission.

Camera-free fixtures are not biological detector or throughput evidence. No live
camera run, deployment, installed-wrapper change, main-branch change, push or
installation is part of this slice. Local `/tmp` checkout/build paths are not a
durable deployment; commits retain the implementation in Orange's shared Git store.

## Verification

Both production targets (`orange`, `orange_client`) build. All 22 focused suites
pass: 21 in the sandbox and the CUDA-only context snapshot suite outside it
(14 synthetic GPU-memory cases, no camera). All 93 camera-free CLI admission
cases pass: 32 media products, 42 context and 19 master/crop.

Address/undefined/leak sanitizers pass for the crop inventory/result suite, seven
crop metadata/media groups, nine daily-context/reuse/GUI-evidence groups, eight
master-acquisition groups and the media-plan tests. New regressions cover GUI
crop-only preparation with native/external full-frame backend preferences and
single/rolling timing, explicit crop-metadata detector/pose joins, required arm
markers, missing/tampered index/clip/parent evidence and acquisition/FPS policies.
Schema JSON parsing and whitespace checks pass; full schema-engine validation
is not claimed.
