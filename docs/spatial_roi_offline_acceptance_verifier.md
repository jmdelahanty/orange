# Spatial ROI offline acceptance verifier

`tools/validate_spatial_roi_recording.py` checks one completed recording
folder without writing to it. It requires the schema-v3
`spatial_roi_recording` session snapshot, the four plan-ordered fixed-region
descriptors, the finalized receipt, the complete immutable encoder profile,
and all 48 receipt-authorized ROI artifacts. The ordinary combined policy also
requires the first-class full-frame descriptor; the ROI-only exception is
defined below. It authenticates the exact normalized
configuration, verified plan, and recorder contract bytes declared by the
session; recomputes the canonical plan digest; and binds the plan's resolved
camera/ROI order, rectangles, padding, socket names, artifacts, GPU mapping,
and per-stream limits to the terminal session and descriptors. Artifact paths
are opened below the recording root with `O_NOFOLLOW`; receipt paths are
independently opened below
`external_spatial_roi_recorder`.  Every artifact must be a unique regular file
with exactly the receipt size and SHA-256. Authority, ROI, and full-frame
video/metadata/keyframe artifacts share one global path/inode uniqueness set.
The verifier retains or revalidates the accepted video identity across the
decode phase, re-hashes every ROI video after probing, and confirms that each
recording-root path still resolves to the same inode/size/timestamps before
success. Concurrent rename or in-place substitution therefore cannot join a
receipt for one file to decode evidence for another.
The finalized full descriptor must also be byte-identical across the v3
authority, schema-2 manifest and snapshot compatibility views, the snapshot
encoder view, `camera_artifacts`, and the single-clip artifact/output
projections. This prevents an older consumer from observing a stale pending or
substituted full-frame product while the v3 consumer sees finalized media.
The verifier also recomputes the complete nine-field admission-usage record
from the four resolved rectangles, padded rasters, source cadence, Mono8 pool
depth, queue depth, and recording limits, then checks every configured
admission ceiling. All session regions must use the camera's one
`arena_group_id`, and overlapping camera-native rectangles are rejected when
`allow_roi_overlap` is false.

The opt-in `fixed_rois_with_registered_context` media policy is also accepted.
Under that policy, omission of the full-frame product is explicit and required:
the backend and every compatibility projection must carry the closed
`omitted_by_policy` record, while fixed-ROI products remain subject to the
same finalized identity, receipt, artifact, and camera/recording binding
checks. The verifier then requires the registered-context runtime record and
the exact closed `registered_scene_context.json` descriptor. It binds the
descriptor's recording, camera, authority, source-frame, geometry, invariant,
and registration-status fields to the session and runtime record; opens the
declared paths relative to the recording root with no symlink traversal; and
checks the Mono8 file's exact size, SHA-256, and bytes. A missing full-frame
descriptor is therefore not a failure in this policy, but an unexpected
full-frame product or an incomplete context record is.

Use text output for an operator check:

```text
python3 tools/validate_spatial_roi_recording.py --require-ffprobe \
  /home/jeremy/orange_data/exp/unsorted/2010096_spatial_roi_diagnostic_plumbing_100fps_gpu5_v1/<run-id>
```

Use `--json` for automation.  The verifier exits zero only for `status=pass`.
It runs `ffprobe -count_frames` when available; `--require-ffprobe` makes an
unavailable probe or any raster/frame-count decode failure fatal.  Without the
flag, unavailable ffprobe is reported as a warning and the metadata/artifact
checks still run.

The verifier does not accept an arbitrary self-digesting evidence JSON. Each
ROI evidence manifest must have the exact production schema-v2 13-field shape,
the authenticated contract/plan/recording/camera/stream/geometry/GPU/profile/
root/limit binding, terminal `complete` state, clean closed encoder snapshot,
exact receipt counts and dense ranges, exact ten finalized artifact references,
the evidence-JSONL reference, and a recomputable finalization-request and
finalized-receipt digest. All four lanes must cover the same source recording
frame range. The first-class full-frame descriptor and its metadata CSV must
cover that exact range; the full video, metadata, and keyframe paths must all
exist as unique nonempty regular files.

The diagnostic runner deliberately does not guess a run folder after
`--execute`: an old completed run can coexist under the durable output root.
After a successful run, copy the exact `recording_folder`/run-id from the
orange-client experiment output and invoke the command above.  This avoids
silently certifying an older run.

A completed session must carry the closed spatial-ROI storage-preflight schema
in both its ready and terminal lifecycle payloads. The verifier requires both
copies to be identical, checked, and passed; binds their artifact-root identity
to the finalized receipt; recomputes the media + evidence + nonzero-reserve
budget; and requires that total not exceed the observed filesystem available
bytes. Result and policy schema identifiers are checked independently.
Each stream's retained media and combined evidence artifacts must also fit the
authenticated per-stream limits, and the contract aggregate limits must equal
those per-stream limits multiplied by the exact four-stream count. A complete
session additionally requires a clean stopped producer and a successful,
reaped recorder process whose ready and terminal preflight evidence is
byte-identical.
Every retained child-event wrapper has the same closed shape, and any lifecycle
field repeated in its raw payload must agree with the wrapper. A complete child
must have emitted nonzero stdout bytes, its latest event must equal its
successful terminal event, and every receipt frame count must fit the
authenticated `max_frames_per_stream` as well as its media/evidence budgets.

For the first supported combined topology, `recording_sink_mode=external_ipc`,
the verifier also requires the recording-root `combined_storage_preflight.json`
referenced identically by the manifest and snapshot. It checks the closed
passed/hard-guarantee record, the single full-frame stream, the synthetic
aggregate ROI stream, exact authenticated ROI media/evidence bounds, one shared
filesystem identity, and the retained/peak/headroom/reserve/available-byte
arithmetic. A complete aggregate cannot be certified from two independent
child preflights that each observed the same free space.

The schema-v5 recorder contract is accepted as a closed authority, not a bag
of selected fields (the legacy schema-v4 contract remains accepted with its
schema-v2 plan). The verifier checks the exact top-level and per-stream key
sets; supervision and required-evidence flags; source cadence/pixel format;
non-rolling control; ACK/RELEASE ownership, inactive drain/finalize
negotiation, queue bounds and backpressure policy; the selected immutable HEVC
profile; direct/structured artifact aliases; and recomputed detach,
encode-queue, writer, media, and evidence aggregates. The direct
`rate_control_mode`, `quality_value`, and `gop` fields must equal the selected
profile rather than a fixed CQP/Q0/GOP-1 fallback. The active
P1/low-latency/VBR-Q20/GOP-25 profile therefore remains authenticated end to
end.

For each non-empty ROI receipt with `N` frames, every accepted lifecycle,
encoded-frame, and packet count equals `N`; only `keyframes` follows the
selected GOP cadence: `1 + (N - 1) // gop_length`. A no-frame receipt has zero
expected keyframes before the complete-session non-empty gate is applied.

The ordinary full-frame product remains first class. For a complete aggregate
recording its descriptor must carry backend-matched packet provenance, the
MP4/full-frame coordinate contract, dense frame coverage, and three unique
artifacts. Its packet count must equal the dense metadata/frame count. Its
canonical metadata identity is `recording_frame_id` when that
column exists and otherwise the established in-process `frame_id`; if both
exist, they must agree row-for-row. The manifest session/root/mode and
single-clip root are coupled to the opened folder, while the snapshot's backend
mode and full-frame lifecycle must exactly project the manifest backend.
