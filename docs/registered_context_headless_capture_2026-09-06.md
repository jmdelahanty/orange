# Native registered context capture for moving-crop recording

Date: 2026-09-06. Status: opt-in headless implementation; validation results below.
Branch: `agent/acquisition/master-frame-journal-v1-20260906`.
Implementation base: `d1e737b65d817785cf4e4c0ef26158219800d958`.

This is the registered-context slice of the moving/lossless crop recording plan.
It does **not** enable crop-only media selection yet. Full-frame-only and
full-frame-plus-moving-crop recording remain first-class; full-frame split-GOP,
moving-crop selection/placement, lossless encoding and rolling behavior are
unchanged. No atlas, region routing, visual overview renderer or GUI switch is
introduced here. Nothing is installed, pushed or deployed by this checkpoint.

## Activation

The existing timed headless experiment spec accepts optional
`fixed.registered_scene_context`. It is disabled when absent. Enabled v1 requires
the opt-in master journal, real full-rate YOLO (`decimate = 1`), no synthetic
detection override, a Mono8 source, and accepted selected registration matching
each camera's serial and native raster. The moving-crop profile retains its
separate supervised-external-recorder admission requirements.

Example fragment (CPU 0 is illustrative, **not a production affinity choice**;
select explicit housekeeping CPUs from the rig's isolation plan):

```json
{
  "registered_scene_context": {
    "schema_version": 1,
    "enabled": true,
    "timeout_ms": 10000,
    "worker_cpu_ids": [0],
    "declaration": {
      "schema_id": "orange.recording.registered_scene_context.capture_declaration",
      "schema_version": 1,
      "registration_authority_status": "accepted_for_experiment",
      "subject_presence": "present",
      "dish_setup_complete": true,
      "nir_illumination_fixed": true,
      "camera_configuration_fixed": true,
      "rig_fixed": true
    }
  }
}
```

The object belongs under `fixed`, alongside `master_frame_journal`. The explicit
scene declaration describes the actual installed dish setup after registration.
Fish may be present; `subject_presence` also permits `absent` or `unknown`.
Acceptance and physical stability are operator declarations, not conclusions
inferred from an image or checksum. Diagnostic registration acceptance is refused.
Unknown configuration fields, invalid types, duplicate/missing CPU IDs and invalid
timeouts are refused. The timeout range is 100–60000 ms.

The camera-free `--validate-experiment-spec` path validates configuration and
profile compatibility only. Actual registration, source raster, owned-buffer
availability and applied CPU affinity are verified during prearm/capture.

## Ownership and order

1. Prepare the existing recorder and master-journal owners, resolve geometry,
   and freeze required camera/producer/generation bindings before acquisition.
   Logical recording remains off. The immutable start snapshot declares context
   as required for every selected camera.
2. Start one one-shot snapshot worker per camera. Each worker has one outstanding
   request and an explicitly configured housekeeping CPU (assigned round-robin
   over the configured list). No request is made for every recorded frame.
3. Acquisition uses its existing retained snapshot handoff. Native capture accepts
   **only** the analytics-owned device source. It checks that the ready event was
   issued, waits for it, then copies the exact packed Mono8 pixels to host memory.
   It refuses camera-DMA fallback, format/device mismatch and missing readiness.
4. Release the retained source immediately after the copy. Control-plane code
   persists and verifies the image/descriptor; the camera loop does no new file
   I/O or hashing. Stop the successful snapshot worker before recording arms.
5. Once every required camera is captured and the existing warmup delay is met,
   arm the logical recording. One camera's context can have a different acquisition
   instant from another's; this is **not a synchronized multi-camera exposure**.
6. Keep worker objects alive until acquisition joins, so acquisition never holds
   a dangling worker pointer. After the one-shot request, its pending check is an
   atomic false load, not a per-frame state mutex. On failure, join acquisition
   and stop snapshot workers before recycling/deleting camera resources.

Current owned-copy admission is intentionally narrow: native context requires
the existing analytics-hybrid device copy. Diagnostic `force_ring_copy`, disabled
analytics-owned frames, or an unsupported acquisition source fail capture rather
than silently using another lifetime contract. No new PTP polling/latches or
physical exposure claims are added. Existing frame timestamps are copied exactly.

`timeout_ms` bounds waiting for a completed capture result on the headless control
loop, measured from acquisition startup returning. It is not a hard cancellation
deadline for filesystem I/O, CUDA event synchronization or worker join after a
driver/device failure. No CUDA lease is abandoned merely to meet that deadline.

## Files and identity

All files are in the parent recording directory, not repeated per rolling clip:

| File | Content |
| --- | --- |
| `registered_context_geometry_v1.json` | Frozen numerical resolver geometry contract for the selected cameras. |
| `Cam<SERIAL>_registered_context.raw` | Exact packed camera-native Mono8 image: `width * height` bytes, no header, resize, conversion or averaging. |
| `Cam<SERIAL>_registered_context_v2.json` | Closed v2 descriptor binding the image and geometry by relative path, byte size and SHA-256, plus scene declaration and source identity. |

The new v2 descriptor deliberately has **no invented Arena, region or ROI ID**.
It describes a whole-camera context for the supported single-dish/single-subject
case. The separate fixed-ROI branch's v1 context schema has different required
layout/materialization identities; this implementation does not repurpose it.

The descriptor records camera serial/numeric ID, producer instance and stream
generation from the master owner; native dimensions, stride, Mono8 and
`camera_native_pixels`; and the original local/hardware/recording IDs and integer
camera/host timestamps. `recording_frame_id` must be zero because this image is
pre-recording, while the local frame ID and timestamps must be present. Hardware
frame ID zero is preserved if supplied. The context is **not** a crop obligation
or a fabricated row in the assigned recording-frame sequence. Timestamp epoch and
exposure semantics still come from the recording timing contract, not this image.

The geometry copy is serialized from the resolved contract already in memory;
it is not claimed byte-identical to the separately materialized
`recording_geometry_contract.json`. It freezes the selected registrations'
embedded numerical snapshots and existing provenance. Selected Orange physical
registration is preferred; otherwise a selected resolved Citrus daily registration
is permitted. An invalid selected Orange registration cannot silently fall back.

SHA-256 means exact stored bytes, with `sha256:` prefix. JSON is initially written
using the shared authority store's compact deterministic serialization, but readers
hash the actual files, never a reserialized parsed object. Raw images are bounded
to 64 MiB each. Raster-size arithmetic and cross-file identities are runtime
semantic checks in addition to the JSON schemas.

## Completion and failure

`recording_snapshot_start.json.session.registered_scene_context` freezes the
required camera set, configuration and geometry reference. A refreshed mutable
snapshot cannot remove that requirement. The common parent manifest writer checks
the exact context files, source/raster/registration bindings and master producer
set for both native/external parent writers. Required context state is `pending`,
`captured` or `failed`; a required failure downgrades a `completed` parent. Headless
also requires a captured context receipt in the finalized parent before success.

The reused authority store binds an open directory descriptor, rejects root
replacement, symlink components and hardlink aliases, creates files without
overwriting existing assets, fsyncs and verifies readback. Capture does not reuse a
previous recording's image. Digest binding detects changed evidence; it is not a
signed authenticity proof against an actor who can replace the whole recording.

Candidate closed schemas:

- `docs/schemas/orange_registered_context_config_v1.schema.json`;
- `docs/schemas/orange_registered_context_capture_declaration_v1.schema.json`;
- `docs/schemas/orange_registered_scene_context_v2.schema.json`;
- `docs/schemas/orange_registered_context_evidence_v1.schema.json`.

They are opt-in producer candidates, not silently accepted Palette/Citrus intake
contracts. No existing v6 wire fields or acquisition-index mapping are replaced.

## Deliberate reuse and validation

Read-only reuse base: Orange spatial-ROI
`2f009be1d23671fc5c4f4b19b1bb4aa43e95f260`.
The generic `spatial_roi_session_authority_store.*` and
`session/registered_scene_context_capture_declaration.*` files are copied unchanged.
The native-byte snapshot seam is reused with stricter owned-source/readiness/CPU
checks for this profile. No unrelated ROI branch work is merged.

- [x] Strict configuration/profile admission and candidate closed schemas.
- [x] Prearm owner, native source handoff, context persistence and parent gate.
- [x] Five CPU test groups, including two-camera completion, accepted-registration
      variants, malformed evidence, exact pixels, immutable-start requirements,
      path/root/alias protections and source identity refusal.
- [x] Eight CUDA-memory snapshot cases passed outside the sandbox: exact bytes,
      IDs and single release, plus owned-source/format/size/affinity/readiness/GPU
      identity refusal, and unchanged existing RGBA output. No cameras or NVENC
      sessions were opened.
- [x] 23 camera-free context admission cases and the 19 existing journal/crop
      admission cases passed.
- [x] Both production executables rebuilt. Fifteen focused CTest suites passed,
      including common-manifest context failure propagation, journal and crop
      gates, rolling sidecars, recording drain/phased start, native finalizer,
      status, timing/identity and external-recorder verification.
- [x] ASan/UBSan with leak checking passed all five new CPU groups outside the
      sandbox. The four new schema documents parse; `git diff --check` passes.
- [ ] Live camera/context validation and multi-camera performance measurement.
- [ ] Full schema-engine validation and downstream contract review.

## Remaining moving-crop-only work

- [ ] Explicit media selector that omits the full-frame encoder/supervisor while
      preserving acquisition, logical recording and the master journal.
- [ ] Decouple moving-crop supervision/finalization from full-frame artifacts.
- [ ] Join actual recorder-returned frame identities and packet/mux/container
      evidence to every master/crop output, including missing terminal tails.
- [ ] Validate finalized media decodability, visible dimensions, frame counts and
      exact file hashes; don't treat a source-detach ACK as encoder completion.
- [ ] Finalize crop-only whole/rolling parent inventory without a full-frame MP4
      or full-frame encoder-generated CSV, then coordinate consumer admission.

See `docs/moving_crop_master_metadata_completion_2026-09-06.md` and Citrus
`docs/detection_crop_only_master_frame_record_plan_2026-09-06.md` for the preceding
metadata gate and stable end-to-end checklist.
