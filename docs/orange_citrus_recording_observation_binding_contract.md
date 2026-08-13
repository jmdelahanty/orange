# Orange/Citrus Recording Observation Binding Contract

Status: schema-v1 identity and handshake freeze; runtime wiring not yet
implemented

Date: 2026-08-13

Related:

- [Recording observation-context acquisition census](recording_observation_context_acquisition_census_2026_08_13.md)
- [Recording observation identity schema](schemas/orange_recording_observation_identity.schema.json)
- [Orange recording metadata](recording_metadata.md)
- [Citrus runtime geometry H5 contract](../../citrus/docs/runtime_geometry_h5_contract.md)

## Goal

Create a reciprocal, digest-bound association among:

```text
one Orange recording
  x one canonical source-camera frame stream
  x one arena
  x zero or one finalized Citrus arena-session H5
```

The observation edge is stable before Citrus starts. Citrus binding is a
separate lifecycle attached to that edge. A transition from unbound to bound
must never change `observation_context_id`.

## Frozen current producer policy

### Source-camera frame-stream identity

For schema v1, Orange may use the camera serial as
`source_camera_stream_id` under this explicit policy:

```text
camera_serial_unique_source_frame_stream_within_recording_v1
```

This is safe because the complete edge also contains `recording_id`. It means:

- one canonical source-camera frame stream exists for that physical camera in
  one recording;
- camera serial and source-camera stream ID are equal for that stream; and
- camera serial is not treated as a globally unique stream instance across
  recordings.

This field does **not** enumerate every acquisition media stream. Orange's live
crop output is a real, independently addressable media stream with its own
`stream_id` such as `2010095_crop`. Palette correctly inventories both:

```text
full: stream_id = 2010095
crop: stream_id = 2010095_crop
```

The crop is derived only in lineage: its pixels and placement are selected
from the source-camera frame stream. It remains a first-class acquisition-time
media stream. Encoder shards are not first-class media streams after the
authoritative merged output exists; pose inputs and model tensors are processing
products rather than acquisition video streams.

The observation edge binds the arena to the native full-camera frame authority,
where rim geometry, homography, detections, and crop placement are defined. A
later recording-context envelope should separately list all available
acquisition media streams and bind each to this source-camera stream.

The frozen conceptual relationship is:

```text
source-camera frame authority: 2010095
  full media stream: 2010095
    role: ingest_authoritative_full_frame
    pixel space: source-camera/native full-frame pixels
  crop media stream: 2010095_crop
    role: runtime_derived_acquisition_input
    pixel space: crop-frame pixels
    placement geometry: source-camera/native full-frame pixels
    frame clock: recording_frame_id
```

Media-stream membership is not part of `observation_context_id`: enabling or
disabling crop recording must not rename the physical camera/arena observation.
The eventual context envelope should carry an independently digest-bound media
inventory whose rows reference `source_camera_stream_id`.

The Orange/Palette review found two producer vocabulary gaps. They are closed
for newly written Orange artifacts:

- crop output descriptor schema v2 uses
  `role = runtime_derived_acquisition_input`; and
- it exposes `video_pixel_coordinate_space = crop_frame_pixels` separately from
  `source_geometry_coordinate_space = full_frame_pixels`.

The old `coordinate_space = full_frame_pixels` value remains only as a
deprecated source-geometry alias. Historical schema-v1 `role = sidecar`
artifacts remain inspectable and are not rewritten.

If Orange later supports two independent native source-camera frame streams
from one camera in one recording, that producer must use a new identity policy
with explicit stream instance IDs. It must not silently keep using the serial.

### Current topology

The identity schema can represent many-to-many observation edges. Current
producer materialization deliberately enforces:

```text
one source-camera frame stream -> at most one arena
one arena                      -> one or more source-camera streams allowed
```

This matches the current Shadow workflow and camera-keyed geometry envelope.
The one-arena-per-camera restriction is a current producer policy, not a
permanent semantic limitation of an observation edge.

Orange must not claim live support for multiple arenas per camera until
camera-keyed geometry maps and asset paths are migrated to edge-keyed
collections.

## Stable observation identity

`orange.recording.observation_identity` v1 contains only facts known before a
Citrus handshake:

```text
recording_id
camera_id
source_camera_stream_id
source_camera_stream_identity_policy
rig_id
canvas_name
arena_id
```

The arena portion is qualified by rig and canvas because an `arena_1` label is
not globally unique.

Its canonical semantic digest derives `observation_context_id`. Citrus session
UUID, H5 path, and binding state are intentionally excluded. Rolling clips
reuse the same observation identity.

## Binding modes

An Orange recording declares one mode for each observation edge:

| Mode | Meaning | Arm behavior |
| --- | --- | --- |
| `required` | The experiment is Citrus-backed and this edge must be accepted before experiment activation | Reject experiment arm if acceptance is absent or rejected |
| `optional` | Citrus association is desirable but absence must not block this acquisition | Recording may proceed with an explicit unbound state |
| `not_applicable` | This is intentionally an Orange-only recording | Do not create or send a Citrus binding request |

`not_applicable` must never be used merely because Citrus was unavailable.

## Binding lifecycle

The normalized lifecycle is:

```text
not_applicable
requested
accepted_pending_finalization
bound
unbound
historically_unavailable
```

Controlled terminal unbound reasons are:

```text
unavailable_at_recording_start
handshake_not_completed
handshake_rejected
finalization_not_completed
final_receipt_invalid
```

Historical absence uses only:

```text
source_did_not_persist_binding
```

An accepted pre-arm handshake is not yet `bound`. Binding becomes authoritative
only after Citrus closes the H5 and Orange verifies its final receipt.

Binding status and geometry eligibility are independent axes. A valid Citrus
binding does not prove that a dish mask exists or is acceptable for detection,
and an Orange recording may carry a valid recording-bound mask without any
Citrus binding. A downstream operation that needs both must validate both.

## Immutable artifact chain

### 1. Orange binding request

Schema:

```text
orange.citrus.recording_observation_binding_request v1
```

The request binds:

- exact observation identity and digest;
- binding mode (`required` or `optional`);
- Orange recording ID and live recording folder;
- recording-relative immutable recording-start snapshot path and SHA-256;
- rig, canvas, arena, camera, and source-camera frame-stream target;
- optional recording-geometry contract relative path and SHA-256; and
- request creation time.

The target redundantly repeats identity fields so Citrus can compare its
selected runtime target explicitly. Redundant values must agree.

The request must not bind an arm-time digest to the ordinary
`recording_snapshot.json` pathname because Orange patches that file during
finalization. Orange must materialize an immutable recording-start copy (or an
equivalent exact immutable embedded payload) for this reference. Later mutable
snapshot updates cannot change the request evidence.

### 2. Citrus acceptance or rejection

Schema:

```text
citrus.recording_observation_binding_acceptance v1
```

An acceptance echoes the request ID/digest and context ID, then supplies:

- shared Citrus experiment group ID;
- per-arena Citrus `session_uuid`;
- exact selected rig/canvas/arena/camera/stream target;
- planned recording-relative H5 path; and
- acceptance time.

A rejection carries one controlled reason:

```text
identity_mismatch
recording_pointer_mismatch
recording_snapshot_mismatch
target_mismatch
geometry_mismatch
output_path_mismatch
runtime_authority_unavailable
internal_error
```

Citrus must validate an accepted request before experiment activation. Both the
exact request and acceptance, including semantic digests, should be embedded
in the H5.

### 3. Citrus finalized-H5 receipt

Schema:

```text
citrus.recording_observation_finalized_receipt v1
```

After successful H5 flush and close, Citrus supplies:

- request and acceptance IDs/digests;
- observation context ID;
- experiment group and session UUID;
- exact target tuple;
- recording-relative H5 path;
- H5 byte size and SHA-256;
- `session_status = COMPLETE`;
- runtime-geometry contract semantic SHA-256; and
- protocol semantic hash or explicit unsupported status.

The final receipt cannot be embedded inside the H5 it hashes. It is returned to
Orange and stored in the finalized parent recording manifest or a checksummed
recording-local sidecar referenced by that manifest. The H5 contains the exact
request and acceptance, which point back to the Orange recording and stable
observation context.

## Digest rules

All three records use:

```text
canonical_json_utf8_sort_keys_compact_v1
```

The semantic digest is SHA-256 of the strict compact, sorted-key JSON payload,
not pretty-printed file bytes. Derived IDs are the corresponding digest with a
schema-specific prefix.

File SHA-256 values, including the recording snapshot, geometry contract, and
H5 file, are byte-level digests and remain explicitly named as such.

## Path rules

- Recording-relative artifact paths must be relative, normalized, and must not
  contain `.` or `..` path components.
- Citrus must verify that every resolved artifact remains within the frozen
  Orange recording folder.
- The live absolute recording folder participates in the request because it is
  required for the same-machine handshake, but portable downstream references
  use recording-relative paths plus digests.
- A later recording-tree relocation does not alter the observation identity or
  the relative finalized receipt.

## Cross-record validation

An accepted/finalized chain must prove:

```text
request.observation identity digest is valid
request target == observation identity
acceptance request ID/digest == request
acceptance context ID == request context ID
acceptance target == request target
receipt request and acceptance IDs/digests == prior records
receipt context and target == prior records
receipt H5 path == accepted planned path
receipt session UUID and experiment group == acceptance
```

Any mismatch fails the affected association closed. It does not invalidate the
Orange camera recording itself unless the edge's binding mode was `required`
and the mismatch occurred before experiment arm.

## Recording and H5 materialization

Planned producer wiring:

```text
recording_snapshot.json
  observation_identities[]
  observation_binding_requests[]

Citrus H5
  /recording_observation_binding/request_json
  /recording_observation_binding/acceptance_json

recording_session.json
  observation_contexts[]
    identity reference/digest
    request reference/digest
    acceptance reference/digest
    finalized receipt or explicit terminal lifecycle state
```

Rolling clip manifests reference the parent observation contexts. They do not
duplicate or mint new observation IDs.

## Historical policy

Existing historical recordings are never retroactively described as
producer-native bound observations merely because an H5 is nearby or has a
matching timestamp, serial, or arena name.

- Producer-native receipt absent: `historically_unavailable`.
- Exact operator-approved recovery receipt: separate recovery authority.
- Orange-only recording by original design: `not_applicable` only when that
  intent is actually known.

The current external-recorder specs and defaults were checked at this freeze:
every full-frame source `stream_id` equals its camera serial, while crop stream
IDs use `<serial>_crop`. This is evidence for the v1 source-frame policy and for
the separate media-stream inventory—not a promise that recorder transport can
never carry other identifiers.

## Implementation boundary at this freeze

Implemented in Orange now:

- stable observation-edge identity and canonical digest;
- explicit source-stream identity policy;
- general edge-collection validation; and
- separate current-producer topology validation;
- strict schemas for request, acceptance/rejection, and final H5 receipt;
- pure seal/validation functions for all three digest envelopes and their
  reciprocal chain; and
- focused failure tests for mismatched targets and digests, unsafe paths,
  rejected handshakes, and final H5 path disagreement.

These are contract foundations only. They do not yet write recording artifacts
or communicate with Citrus.

Not implemented yet:

- request materialization at Orange recording preparation;
- Orange-to-Citrus request transport;
- Citrus validation or H5 embedding;
- Citrus final H5 hashing and receipt return;
- Orange final manifest integration; or
- Palette consumption of the producer-native binding.
