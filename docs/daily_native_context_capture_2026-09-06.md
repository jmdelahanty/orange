# Daily-registration native camera context

Date: 2026-09-06. Status: GUI implementation and isolated builds/tests complete;
live GUI/camera validation is pending. Timed-headless reuse is now implemented
in the follow-up described in `daily_context_recording_reuse_2026-09-06.md`.
Regular GUI recording reuse is now connected as described in
`gui_registered_context_recording_2026-09-06.md`; live acceptance is still pending.
Branch: `agent/acquisition/master-frame-journal-v1-20260906`.
Implementation base: `0504b17144a516339a2cc8053c4f67da727192a5`.

## Operator surface

The guided Daily Registration panel now exposes **Native Experiment Context
(Optional)** after the workflow reaches its completed, accepted-registration
state. This is a separate action from calibration snapshots or recording start.

1. Finish and accept daily registration in the existing guided workflow. Restore
   the experiment's optical path, arrange the dishes, and leave the cameras
   streaming. Detection can be off. Fish may already be present.
2. In the new section, set **Context housekeeping CPU** to a housekeeping CPU
   from the rig isolation plan, not an acquisition or rendering CPU. There is no
   guessed default; a CPU that cannot actually be applied causes capture failure.
3. Set **Subjects in context** to Absent, Present, or Unknown. Confirm dish setup,
   fixed experiment NIR illumination, restored/fixed camera settings, and fixed
   rig/dishes. These are operator declarations, not inferred physical guarantees.
4. Click **Capture Native Context (All Registered Cameras)**. The current
   workflow requires an unambiguous one-target-per-camera registration. All its
   registered cameras must be present and selected/applied in Citrus.
5. Wait for “Native daily context saved; no recording started.” The panel shows
   the saved `context.json` path. A new capture creates a new asset; it does not
   overwrite the previous capture or change the accepted registration.

To save a headless reuse configuration after capture, confirm the scene is
unchanged and use **Copy Headless Context Configuration**. Merge the copied field
into the existing experiment spec's `fixed` object and review its declarations
for the next run. This neither starts recording nor changes global app settings.

The action is disabled while recording/finalizing or another calibration
transaction is active. Its own transaction prevents recording start and camera/
calibration mutations until the source copies and persistence reach a terminal
state. Closing or collapsing the panel does not stop completion polling or leave
the transaction held solely because the controls are hidden.

This surface is in the isolated build, not an installed production executable:
`/tmp/orange-timing-build-20260906/orange`. No camera was opened for validation of
this change. Existing calibration RGBA/averaged snapshots remain unchanged.

## What is captured

One exact full-camera packed Mono8 image per selected camera: no resize, color
conversion, averaging, annotation, crop, or encoding. Each camera may contribute
a different acquisition instant; this is not synchronized multi-camera exposure.
Current limits are 64 MiB per image, 256 MiB total raw image payload, and at most
64 distinct cameras. These are bounded admission limits, not a validated
64-camera performance claim.

The existing snapshot worker reads the analytics-owned device copy when present.
For this explicit daily profile only, it can instead read the retained pool-owned
ring copy. Acquisition ensures that owned copy exists for the one pending native
request even when detection is off; the diagnostic direct-read flag cannot bypass
this requirement. Camera DMA is never accepted merely because an entry has a
reference count. Readiness, pointer/device identity, byte count, and effective
CPU affinity are checked before exact D2H copying.

The snapshot worker temporarily applies the selected housekeeping CPU and restores
its previous affinity afterward. The source is released exactly once after copy;
only owned CPU pixels leave the worker. One temporary housekeeping save task
resolves frozen geometry, hashes, writes, fsyncs and verifies the assets. There
is no new acquisition process, encoder, per-frame file I/O, PTP poll or latch.
After the one-shot request, ordinary acquisition policy is unchanged.

Capture waits up to ten seconds for source results. Failure cancels only unclaimed
requests and drains claimed copies before releasing the calibration transaction.
The timeout is not a promise of interrupting a stalled CUDA call or filesystem
operation safely. Partial failed output directories are retained for diagnosis.

## Artifact custody and identity

The asset lives under the existing daily workflow transaction:

```text
<calibration_session>/guided_registrations/<daily_transaction_id>/
  native_context_<process_id>_<monotonic_ticks>/
    registration.json
    geometry.json
    runtime_before.json
    runtime_after.json
    Cam<SERIAL>_native.raw
    context.json
```

There is one raw file per camera. `registration.json` is an exact-byte copy of
the accepted registration. `geometry.json` freezes the numerical resolver
contract, including each camera's selected daily registration and native raster.
Before/after runtime snapshots establish that the exact accepted registration
was selected, applied and geometry-compatible around the source capture.

The candidate closed descriptor is
`orange.calibration.registered_native_context`, version 1; see
`docs/schemas/orange_daily_registered_native_context_v1.schema.json`.
It binds the files by relative path, byte size and SHA-256 over stored bytes
(`sha256:` prefix). `context.json` is published last. The shared authority store
rejects symlink traversal, root replacement and aliased files, and refuses to
overwrite changed content. Output directory creation is exclusive and relative
to an already validated parent directory descriptor.

Per-camera evidence includes serial, runtime numeric camera ID (zero is valid),
width/height, pixel format, configured frame rate, exposure, gain, focus and iris;
stride, source storage kind, and the original local/hardware IDs and integer
camera/host timestamps. Camera configuration is explicitly attributed to
`orange_runtime_configuration`, not fresh hardware readback. Scene declarations
remain necessary, including for manual optics. Raster and registration/settings
changes detected during capture cause failure.

`scope` is `daily_registration` and `recording_binding` is **unbound**. The source
`recording_frame_id` is zero. No recording/session or producer identity is
invented, and the context is not a row in a future recording's master timeline.
Checksums provide integrity binding, not a signature or a guarantee that the
physical dish remained still. Timestamp semantics are not upgraded to exposure
or physical stimulus timing by saving this image.

## Relationship to recording controls

The separate timed-headless `fixed.registered_scene_context` option still takes
a fresh pre-recording image and binds it to that recording's master owners.
Its analytics-owned-only admission is unchanged. The daily profile's explicit
ring-copy permission does not relax headless admission.

The v2 timed-headless configuration now implements explicit fresh/daily source
selection, verified import and a separate immutable recording-use receipt. It
checks the saved assets, exact camera settings and selected daily geometry before
arm. The original capture keeps its historical identity; rolling clips share
the parent-level imported copy. See `daily_context_recording_reuse_2026-09-06.md`.

The following broader interface remains planned, **not yet implemented**:

- Recording media choice: full frame; full frame plus moving crops; or context
  image plus moving crops only. Established full-frame/split-GOP and supported
  single-subject moving/lossless crops remain first-class modes.
- Fresh context capture during GUI recording arm (saved-daily source selection
  and import are implemented in the separate GUI recording-context slice).
- Finish media selection, crop-only supervision/finalization, and actual
  returned-frame/packet/container/decode/hash reconciliation before enabling
  moving-crops-only completion. A context asset alone does not close those gates.

No visual overview generation is added; that remains downstream work.

## Validation and checklist

- [x] Daily panel, transaction gating, hidden-panel completion and request-scoped
      result consumption; cancellation never steals a claimed source lease.
- [x] Four CPU artifact test groups: exact two-camera custody; stale/changed or
      incomplete selection/geometry; malformed settings/declarations/sources;
      tampering, immutable recapture, symlink parents and file aliases.
- [x] Fourteen CUDA-memory snapshot cases: exact native output, owned-source and
      readiness/device/affinity rejection, detection-off ring opt-in, restored
      affinity, matching request consumption/cancellation, existing RGBA behavior.
      Tests open no cameras or encoder sessions.
- [x] Both Orange executables build. Sixteen focused CTest suites and 42 existing
      camera-free headless context/journal admission cases pass.
- [x] Address/undefined/leak sanitizer checks pass all four CPU groups. Candidate
      schema parses and `git diff --check` passes.
- [ ] Live guided-registration GUI capture with detection on and off, including
      closing the panel while pending and confirming recording remains stopped.
- [ ] Full JSON Schema-engine validation and downstream contract acceptance.
- [x] Timed-headless saved-daily-context selection/import and immutable
      recording-use binding; follow-up validation is documented separately.
- [ ] Moving-crops-only selector, supervision/finalization and encoded-media proof.

See `docs/registered_context_headless_capture_2026-09-06.md` and Citrus
`docs/detection_crop_only_master_frame_record_plan_2026-09-06.md` for the
recording-owned context and overall implementation checklist.
