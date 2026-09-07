# Crop-only parent and clip inventory — 2026-09-07

Status: implemented and camera-free tested on
`agent/acquisition/master-frame-journal-v1-20260906`, isolated worktree
`/tmp/orange-timing-evidence-20260906`, based on `6a8866b`.
This is the tenth moving-crop-only implementation slice. Runtime admission still
refuses `registered_context_and_moving_crops`: required startup and headless
experiment run-result integration remain unfinished. No override flag exists.

## Recording and video identities

A crop-only recording retains logical camera membership even though it has no
continuous full-frame video. Membership must match the immutable
`session.recording_media_plan` and the independent master journal, not files
discovered on disk. Every selected camera needs complete master, detector/crop
correspondence, encoded-media and registered-context evidence.

Each camera has a recording-wide master. Each moving crop video has its own
zero-based local video index, crop metadata, encoded raster/count, and original
parent recording-frame range. Rolling resets only the local index. The short
final clip has its actual count, not the nominal clip length.

`clip_index_scope=camera_stream` is explicit. The tuple
`(recording_id, camera_serial, clip_index)` identifies a clip; `clip_id` alone is
not globally unique. Equal clip indices from different cameras do not assert
synchronized physical start/end times. Camera-specific clip lengths are allowed.
The index orders cameras by serial and retains validated clip order within each.

## Files and schemas

All references are relative to the parent recording directory, with exact size
and SHA-256. Actual video locations come from the validated media receipt; files
are not moved, renamed, concatenated or re-encoded by this finalizer.

| Artifact | Purpose |
| --- | --- |
| `recording_session.json` | Existing session envelope, explicit `media_product_mode`, logical cameras and crop-only inventory |
| `Cam<S>_master_frames_v1.json` / `.csv` | Independent camera recording-frame history |
| `Cam<S>_moving_crop_media_v1.json` | Previously implemented encoded-media completion receipt |
| `Cam<S>_crop_clip_<N>_manifest_v1.json` | One exact video, its local crop metadata and parent source/context references |
| `recording_crop_clip_index_v1.json` / `.csv` | Per-camera clip collection and manifest references |

The parent keeps `orange.recording_session` v1 with an explicit product
discriminator. New nested records have closed schemas:

- `orange.recording.crop_only_inventory` v1: immutable-start reference and a
  per-camera master/context/media/clip inventory, or a failed result with reason.
- `orange.recording.moving_crop_clip` v1: exact video/CSV/container references,
  parent frame range, local frame range, master descriptor and context binding.
- `orange.recording.moving_crop_clip_index` v1: inventory, clip-manifest references
  and CSV reference.
- Crop output descriptor v3: `representation=clip_collection`, aggregate count,
  and individual clips. It has **no aggregate video filename**, including for a
  single clip. Each clip has its actual per-video v2 output descriptor.

Schema files are under `docs/schemas/orange_recording_*crop*.schema.json`.
`crop_clip_index` in the parent and mutable snapshot binds the JSON/CSV index.
`camera_artifacts` stays empty: that field describes actual full-frame media.
No `Cam<S>.mp4`, full-frame CSV, or full-frame rolling index is fabricated.

The new path preserves Shaman camera and recording-token contracts. It does not
create a full-frame `frame_identity_contract`, v1 acquisition-index projection or
full-frame timestamp seal from crop rows. Master and per-clip correspondence stay
explicit; no new exposure/PTP/physical-timing claim or Palette admission is implied.

## Registered context and custody

Fresh native context binds each camera's descriptor/image. Saved daily context
binds `registered_daily_context/context.json` and
`registered_context_use_v1.json`; consumers resolve the camera by serial in the
archived bundle. The original daily-capture identity is not replaced by the new
recording identity. Finalization does not need the original calibration directory.

Index and clip manifests are published create-once with exact-byte retry support.
Changed files, missing tail clips, conflicting existing manifests, changed logical
membership or edited in-memory clip references cannot publish a successful index.
Source references are revalidated before publication. A failed/interrupted
parent is never promoted by the existence of valid media; it has no completion
index. Interrupted sessions may retain a verified inventory as evidence only.
Partially published files after an I/O failure are not completion authority;
consumers require the completed parent and its exact index reference.

GUI and headless use the same builder through the common parent writer. It
returns the actual finalized parent to callers so GUI snapshot status cannot
report success after a required-evidence failure. Hashing/decoding and index
writes remain on post-recording housekeeping/finalizer execution, not acquisition
or detector threads. No new thread, encoder, camera-buffer lease or PTP poll is
introduced.

## Checklist and next activation slice

- [x] Shared logical-camera inventory and single/rolling crop-only parent builder.
- [x] GUI finalizer does not require a full-frame supervisor or media artifact.
- [x] Headless timed-run parent path does not fabricate full-frame artifacts.
- [x] Clip-local indexing, parent recording identity and partial-clip handling.
- [x] Native and archived daily context references, with fail-closed custody.
- [x] Immutable per-video manifests and parent-local JSON/CSV index.
- [x] Actual finalized parent status returned to GUI/headless snapshot writers.
- [x] Full-frame encoder preflight/native rolling scheduling follows media plan.
- [ ] Finish headless experiment camera-result and post-run supervisor handling;
  these must consume the crop collection without requiring a full-frame video.
- [ ] Require valid context, independent master, real full-rate detector/logger
  and external moving-crop configuration at crop-only arm on both GUI/headless.
- [ ] Enable the product only after those admission/failure tests pass. Remove
  the temporary implementation refusal, not replace it with an override switch.
- [ ] Consecutive GUI, Citrus-triggered and headless live validation, cancellation,
  failures and performance checks before deployment or default changes.

Full-frame split-GOP and one-camera/single-subject moving lossless crops remain
first-class supported modes. Fixed-region/multi-fish/atlas and downstream visual
composition are not part of this slice.

## Verification

The CPU fixture uses synthetic 256×256 HEVC videos, including a two-frame final
clip; this is not a live fish or encoder-performance test. Coverage includes one
and two cameras with unequal per-camera frame counts, native/daily context custody,
immutable-start mismatch, missing/altered media, failed/interrupted parent status,
conflicting publications, repeat publication and edited in-memory references.
The actual GUI finalizer and headless-style common parent writer are exercised.
Both production executables (`orange`, `orange_client`) build. All 21 focused
CTest suites pass, as do 81 camera-free CLI admission cases (20 media products,
42 registered context, 19 master/crop). The new inventory suite, seven shared
crop metadata/media groups and nine daily-context/reuse groups pass address,
undefined-behavior and leak sanitizers. `git diff --check` passes.
Schema JSON parsing is checked; a full JSON Schema engine is not installed in
this environment and is not claimed. No installation is needed for these tests.
