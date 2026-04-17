# Advanced Recording Validation Plan

## Purpose

This note defines how Orange should expose advanced per-camera recording
controls in the GUI and what validation rules should apply before a split-GOP
recording session is allowed to start.

The immediate goal is not to expose every internal knob. It is to make the
validated split-GOP workflow editable and safe enough that the GUI does not let
an operator assemble obviously unsupported GPU combinations.

## Current Reality

On `exp/gop-split-a16`, the split-GOP path that is actually validated today is:

- `recording.strategy.mode = split_gop`
- `recording.strategy.split_gop.placement = multi_gpu`
- `recording.strategy.split_gop.source_encoder_policy = hybrid_split`
- `recording.strategy.split_gop.transfer_mode = raw`

And the validated host-level patterns so far are:

- one recording camera at a time
- one source GPU plus one helper GPU
- helper pair with `PIX` topology
- peer-access-capable pair

Examples already validated on this branch:

- camera `2010095` on `GPU1 + GPU2`
- camera `2010096` on `GPU5 + GPU6`

That matters because the GUI should reflect the validated operating envelope,
not the broader space that the config schema can theoretically express.

## UI Model

### Shared Session Recording Panel

The shared recording panel should continue to own session-wide controls such as:

- save root
- codec
- preset
- tuning
- rate control
- quality
- GOP length
- output shape

The existing per-camera `record` checkbox still means:

- this camera participates in the current recording session

### Per-Camera Advanced Recording Panel

Advanced split-GOP controls should be shown per camera, not as one shared
session-wide block.

That panel is the correct place for:

- `source_gpu_id`
- `recording.strategy.mode`
- `recording.strategy.split_gop.encoder_gpu_ids`
- `recording.strategy.split_gop.transfer_mode`
- `recording.strategy.split_gop.source_encoder_policy`
- `recording.constraints.require_peer_access`
- `recording.constraints.preferred_topology_class`
- `recording.resources.acquire_work_entries`
- `recording.resources.encoder_entry_pool_size`

Read-only diagnostics for that same camera should also live there:

- runtime topology class
- peer-access capability / observation
- helper routing counters
- overflow/backlog state
- latency summaries

## Why Validation Is Needed

The schema can describe more combinations than the current branch has validated
or scheduled safely.

Without validation, the GUI could let an operator select:

- a helper GPU pair that is not `PIX`
- a pair without peer access
- the same helper GPU for multiple cameras
- overlapping source/helper GPU claims across cameras

Those combinations may still parse cleanly as config, but they are not the same
thing as a supported recording session.

## Validation Goals

The GUI should answer two questions before recording begins:

1. Is each selected camera's split-GOP topology locally valid?
2. Are the selected cameras jointly valid as one recording session?

The first is per-camera validation.
The second is cross-camera resource-conflict validation.

## Per-Camera Validation Rules

For the first GUI implementation, split-GOP validation should be conservative.

### Rule 1: Only Expose Validated Modes

For now, the GUI should only allow editing toward the currently validated mode:

- `mode = split_gop`
- `placement = multi_gpu`
- `source_encoder_policy = hybrid_split`

Other modeled-but-not-fully-validated combinations should stay config-only
until runtime support is broader.

### Rule 2: Helper GPU Set Must Be Well-Formed

For now, the GUI should require exactly two GPU ids in the effective encode set:

- the source GPU
- one helper GPU

In practice, that means:

- `source_gpu_id` must be set
- `split_gop.encoder_gpu_ids` must be non-empty
- the helper set should resolve to one non-source helper GPU for the validated
  path

If the operator picks more than one helper GPU, the GUI should reject it as
"not yet supported in GUI validation".

### Rule 3: Topology Must Match Preferred Class

If `recording.constraints.preferred_topology_class` is set, the GUI should
check the actual pair topology using:

- `lookup_nvidia_smi_topology_class(source_gpu_id, helper_gpu_id, ...)`

For the current validated workflow, the expected class is:

- `PIX`

Recommended behavior:

- if preferred topology is `PIX` and actual topology is not `PIX`, treat it as
  invalid for now
- show the detected topology in the error message

Example:

- `Cam2010096 split-GOP helper GPU 4 is PHB relative to source GPU 5; expected PIX`

### Rule 4: Peer Access Must Match Constraint

If `recording.constraints.require_peer_access = true`, the GUI should validate
that the pair is peer-access-capable before allowing recording.

This should be a hard failure for the first implementation, not just a warning.

### Rule 5: Present A Clear Resolved Summary

Before recording starts, each camera's advanced UI should show a short resolved
summary such as:

- `source GPU 5 -> helper GPU 6`
- `topology PIX`
- `peer access required: yes`
- `transfer raw`
- `policy hybrid_split`

That reduces ambiguity and makes validation failures easier to understand.

## Cross-Camera Validation Rules

This is the user concern that matters most for multi-camera sessions:

- if two cameras want the same split-GOP GPUs, that should not silently proceed

For the current branch, the safe answer is:

- yes, overlapping split-GOP GPU claims should be treated as invalid

### Why Overlap Is Unsafe Today

The branch does not yet have a multi-camera split-GOP GPU scheduler or
admission-control layer.

That means there is currently no validated mechanism to:

- reserve encode/helper GPUs across cameras
- apportion helper capacity across multiple recording cameras
- reason about fairness or throughput when pairs overlap

So if two cameras both request the same helper GPU or same source/helper pair,
the GUI should not assume this is supported.

### Rule 6: No Overlapping GPU Claims Across Record-Enabled Split-GOP Cameras

For now, the GUI should treat the following as a hard conflict:

- any overlap in the per-camera claimed split-GOP GPU set among cameras with
  `record = true`

The claimed set should include:

- the camera's `source_gpu_id`
- all effective `split_gop.encoder_gpu_ids`

So if:

- camera A claims `{5, 6}`
- camera B claims `{5, 6}`

that is invalid.

If:

- camera A claims `{1, 2}`
- camera B claims `{2, 3}`

that is also invalid.

### Rule 7: Validate Across The Recording Session, Not Just The Edited Camera

A camera can be locally valid and still be invalid in the current session if
another record-enabled camera overlaps its GPU claims.

So the GUI must run this validation against the whole current recording set:

- all cameras with `record = true`
- and split-GOP enabled

This should happen:

- while editing advanced recording settings
- and again before `Record` or stream start is allowed to proceed

## Recommended UX

### In The Advanced Per-Camera Panel

Show:

- editable fields
- detected topology
- validation status line

Examples:

- `Valid: source GPU 5 -> helper GPU 6 (PIX, peer access available)`
- `Invalid: helper GPU 4 is PHB relative to source GPU 5`

### In The Shared Recording Panel

Show a compact session-level validation summary when any record-enabled camera
uses split GOP.

Examples:

- `2 split-GOP cameras configured; 1 GPU conflict detected`
- `Split-GOP session valid for cameras: Cam2010095, Cam2010096`

### Start/Record Guard

If there are hard validation failures:

- disable the recording action
- explain why

Do not silently downgrade or reroute GPU selections in the GUI.

## Recommended Implementation Order

### Phase 1: Read-Only Validation Summary

Add a per-camera advanced summary that shows:

- source GPU
- helper GPU ids
- detected topology
- peer-access-capable yes/no
- session conflict yes/no

No editing yet.

### Phase 2: Editable Strategy Fields With Validation

Allow editing:

- mode
- helper GPU ids
- transfer mode
- source encoder policy

Enforce:

- `PIX` requirement when configured
- peer-access requirement
- no overlapping GPU claims across record-enabled cameras

### Phase 3: Constraints And Resources

Expose:

- preferred topology class
- require peer access
- resource pool sizes

Keep session-level conflict validation in place.

## Non-Goals

This note does not propose:

- supporting overlapping helper GPU allocation across cameras
- building a multi-camera encode scheduler in the GUI layer
- exposing every split-GOP queue/buffer limit as a GUI control
- relaxing topology constraints beyond what is already validated

## Recommendation

For now, the GUI should be intentionally conservative:

- require `PIX`-matched helper pairs for the validated workflow
- require peer access when configured
- reject overlapping split-GOP GPU claims across record-enabled cameras

That will keep the first advanced-recording UI aligned with the actual
validated runtime envelope, instead of letting the GUI assemble combinations
that the branch does not yet schedule safely.
