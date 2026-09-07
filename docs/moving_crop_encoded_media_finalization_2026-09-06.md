# Moving-crop encoded-media finalization — 2026-09-06

This slice implements independent headless crop supervision and actual encoded
media completion checks shared by headless and GUI. It does **not** yet make
`registered_context_and_moving_crops` available. That mode still needs its own
single/rolling parent output inventory and run-result path, without full-frame
camera artifacts. There is no override or full-frame discard workaround.

Full-frame/split-GOP recording and single-subject moving/lossless crops remain
supported. No fixed-region, multi-fish, atlas, preview-composition, or pose-routing
change is included.

## Selection and supervision

Headless `fixed.crop_recording.mode=external_ipc` owns a crop supervisor even when
the full-frame product uses native encoding. A full-frame external contract is
no longer a prerequisite for starting crop recorders. Optional
`fixed.crop_recording.recorder_tool_path` selects an absolute executable path;
otherwise a supplied full-frame contract remains a compatibility default, then
the existing supervisor executable resolution applies. This does not change the
installed sudo wrapper or its allowlist.

The crop parent recording ID comes from the recording-folder basename, not an
optional full-frame contract's session ID. Existing explicit crop GPU/per-camera
settings and interleaving behavior are retained; omitting a full-frame contract
does not imply a newly discovered GPU pair. Requested crop configuration is
included in `runs.json`; the supervisor plan records the resolved executable,
GPU placement, output paths and encoder configuration.

The strict completion profile applies when an explicit moving-crop media product
and master evidence are selected. Headless uses `fixed.master_frame_journal`;
GUI uses the existing registered-context/master option. GUI prearm requires real
YOLO, a logger, full detector cadence, and external moving crops for every
participating camera. Unconfigured existing recordings retain their prior policy.

Before arm, immutable start evidence includes:

```json
"moving_crop_encoded_media": {
  "schema_version": 1,
  "required": true,
  "profile": "returned_identity_v2_mux_and_full_hevc_decode_v1"
}
```

This supplements `moving_crop_master_coverage`; it never upgrades a metadata-only
receipt into an encoded-media claim.

## Completion evidence

The shared finalizer first requires the independent master journal and the
existing master/crop/detector correspondence receipt. It then checks:

- External recorder returned-identity proof v2: every submitted identity returned,
  no outstanding/mismatched identities, no skipped/dropped/rejected frames, and
  matching accepted packet, write, encoded-frame and metadata counts.
- The completed GOP coordinator and each video's terminal container sidecar:
  packet writes, mux flush, header, trailer, close and exact file size. A merely
  playable/degraded container does not satisfy this strict profile.
- Each recorder CSV against the session crop CSV: recording/local IDs and exact
  integer camera/host timestamps, in encoded order, with no extra or missing rows.
- Every clip, including the final partial clip: MP4/HEVC, exactly one stream,
  authenticated visible width/height, exact packet count and full software-decoded
  frame count, no corrupt/error-marked packets or frames.
- Parent-relative paths, no traversal or symlink aliases, and exact size/SHA-256
  for the video, recorder CSV, container sidecar and master-bound crop CSV.

The decode accepts legitimate blank crops. It does not use scene brightness as a
proxy for success. HEVC's internal 64-pixel CTU allocation envelope is permitted;
the container and decoded visible raster must still match exactly.

The create-once receipt is `Cam<serial>_moving_crop_media_v1.json`, schema
`orange.recording.moving_crop_media_completion` v1. Each video keeps its clip ID,
zero-based clip index, parent recording-frame range and local metadata reference.
Rolling metadata indices reset per clip; parent recording IDs do not. The receipt
binds the metadata-only receipt, which in turn binds the independent master and
detector log. Required parent completion rechecks these references and the media
container raster. Missing or changed evidence prevents `status=completed`.

This is correspondence and encoded-product evidence, not proof of exposure phase,
absolute physical timing, biological detection quality, or mathematical pixel
identity with a separately retained raw source image. The lossless requirement
checks the recorder's configured profile; software decode checks readability,
geometry and counts. No PTP service or per-frame latch is added.

## Threading and stop ordering

All hashing and decoding runs after admission stops, on a housekeeping CPU. GUI
uses its existing background finalizer and context CPU; headless temporarily uses
a configured master-writer CPU after recording workers have stopped. Software
decode uses one CPU thread, bounded raster allocation and a finite per-output
deadline. Finalization can therefore take longer; live throughput is not measured
by these tests.

GUI detector logs can still have buffered rows after the crop encoder drains.
`YoloEventLogger::FlushThrough(folder, last_recording_frame_id, timeout)` requests
a visibility barrier from the existing log writer, including when the last
detector result arrives after the request. It does not flush on every frame or
block the detector/acquisition thread. The finalizer retains the logger, not the
worker or camera buffer. Timeouts fail completion; a flush never certifies gaps.
Headless already stops/joins the detector logger before media validation.

## Remaining implementation and acceptance

- [x] Independent headless crop supervisor and explicit executable configuration.
- [x] Shared returned-identity, mux, decode and exact-file completion receipt.
- [x] Immutable required-media marker and fail-closed common parent gate.
- [x] GUI detector-log drain barrier and strict media finalization integration.
- [x] Camera-free single/rolling, blank, missing-tail and tampering tests.
- [ ] Crop-only single/rolling parent and per-clip inventories on both paths;
  logical camera membership must not be inferred from full-frame files.
- [ ] Headless crop-only run-result validation and full-frame-only scheduling
  preflights must follow the product plan, not just `record_enabled`/sink mode.
- [ ] Required context/master admission for crop-only, then remove the temporary
  implementation refusal and enable the GUI choice.
- [ ] Live consecutive GUI, Citrus-triggered and headless recordings, cancellation,
  failure injection and performance measurements before deployment.

Schemas: `schemas/orange_recording_moving_crop_media_completion_v1.schema.json`
and `schemas/orange_moving_crop_encoded_media_evidence_v1.schema.json`.
The metadata-only schemas retain their original names and meanings.

## Verification

Both `orange` and `orange_client` production targets build. Twenty focused CTest
suites and 81 camera-free CLI admission cases pass (20 product, 42 context, 19
master/crop). The real GUI finalizer test also rejects a required missing crop
media receipt despite a complete source journal.

Seven crop metadata/media groups, media-plan tests, eight master-source groups
and nine context/evidence groups pass address/undefined/leak sanitizers. Tests
include software decoding, a short final clip, missing/extra decoded frames,
returned-identity/packet errors, changed artifacts and late/missing log tails.
Schema JSON parsing and whitespace checks pass; full schema-engine validation
and live camera/GUI/Citrus performance acceptance are not claimed.
