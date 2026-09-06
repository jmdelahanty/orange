# Saved daily context: configuration and recording reuse

Date: 2026-09-06. Branch: `agent/acquisition/master-frame-journal-v1-20260906`.
Base: daily GUI capture checkpoint `21bbfb7f081dc9774f6d5eff2170046081ddead0`.
Status: opt-in timed-headless implementation; not crop-only media selection or
GUI recording-time reuse. Live validation remains pending.

## Configuration ownership

The context choice is an explicit part of the saved experiment specification,
not inferred from the most recent image or retained only in GUI state.

| Surface | What it keeps |
| --- | --- |
| Orange experiment spec, `fixed.registered_scene_context` | Versioned source selection, exact descriptor path/size/SHA-256, housekeeping CPUs, timeout and operator declarations. |
| Daily calibration context directory | Immutable original native images, registration/geometry/settings, capture IDs/timestamps and capture declarations. |
| Run configuration and immutable recording-start snapshot | Resolved configuration and the selected recording-use bindings. |
| Parent recording directory and completion manifest | Verified copies of daily assets, current recording geometry, a recording-use receipt and terminal integrity result shared by rolling clips. |

This is Orange recording configuration, not a new Citrus stimulus-protocol field.
Camera/rig settings remain in their existing configuration; their resolved values
are compared and recorded here. No workstation app default, canonical canvas or
existing experiment spec is silently edited. GUI capture controls themselves are
not new auto-saved global preferences. The persisted configuration is the explicit
field copied/saved into an experiment spec; per-capture settings are also recorded
in the daily artifact.

## Using a daily image

After a successful Daily Registration native capture, the panel now offers
**Copy Headless Context Configuration**. First confirm **Scene unchanged since
this capture for the next recording**. Merge the copied `registered_scene_context`
field into the existing experiment spec's `fixed` object, preserving its other
settings. It contains the actual descriptor path, byte size and SHA-256, selected
housekeeping CPU and capture declarations. Review those declarations for the
recording, including subject presence. Copying the field does not start recording,
enable the master journal or change which media products are recorded.

The new closed configuration v2 adds an explicit source. Shape under `fixed`:

```json
{
  "registered_scene_context": {
    "schema_version": 2,
    "enabled": true,
    "worker_cpu_ids": [0],
    "timeout_ms": 10000,
    "source": {
      "kind": "daily_registration",
      "descriptor_path": "/absolute/daily/capture/context.json",
      "size_bytes": 1234,
      "sha256": "sha256:REPLACE_WITH_THE_ACTUAL_64_HEX_DIGEST",
      "scene_unchanged_since_capture": true
    },
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

This is an illustrative fragment, **not a runnable spec**: replace the example
path, size, hash and CPU. CPU 0 is not a recommended affinity choice. The GUI
copy action avoids transcribing the descriptor reference. The existing master
journal and real full-rate YOLO admission requirements remain in force; the
moving-crop profile separately requires supervised external crop recording.

For a new image on each recording, v2 uses `source: {"kind": "fresh_capture"}`.
Existing v1 remains fresh-capture-only with its exact previous wire shape; adding
`source` to v1 is rejected. An absent configuration remains disabled. V2 requires
an explicit source, even when disabled, and never silently picks a new or old
image. Schema and configuration roundtrips preserve the source selection.

`scene_unchanged_since_capture` is an explicit operator assertion supplied for
the recording, not a physical sensor or proof of a recent human action. Review
it before each run; a reused spec cannot independently detect a moved dish,
changed NIR illumination or manually altered optics. The source capture's own
declarations remain unchanged; the new recording-use declaration is separate.

## Prearm checks and timing

Before acquisition threads start, the headless control path temporarily applies
the selected housekeeping CPU and verifies/imports the daily bundle. It then
restores its prior CPU affinity. Reuse creates no snapshot worker and does not
capture a new frame or call a camera/PTP register. Existing warmup and logical
recording arm still run normally. The fresh profile retains its existing native
snapshot workers and analytics-owned-only policy.

Reuse requires the exact camera set and all nine recorded runtime configuration
fields: camera numeric ID, dimensions, Mono8, configured frame rate, exposure,
gain, focus and iris. This first profile conservatively rejects even a numeric
runtime-index reassignment, despite stable camera serial being the source key.
These are runtime settings, not newly polled hardware readbacks.

The current resolver's per-camera selected daily-registration geometry must
equal the captured per-camera contract, including accepted registration ID,
original path, digest and numerical snapshot. A new registration, changed raster,
settings change, missing camera or selected Orange physical-registration override
refuses reuse. A source image alone cannot authorize a geometry fallback. The
recording captures a new current geometry contract; it does not substitute the
historical top-level resolver bookkeeping as current state.

The loader and writer share the daily payload validation. The loader checks the
closed descriptor, unique camera set, exact integer identities, pre-recording
frame ID zero, source storage, raster/stride/byte count, scene declarations and
before/after selection evidence. Every referenced file is verified through the
directory-bound authority store. Symlinks, aliases, root replacement, incorrect
hashes and unexpected paths are rejected. Limits remain 64 MiB per raw image and
256 MiB raw payload; import additionally caps the referenced bundle at 320 MiB,
with a 16 MiB descriptor cap. Hashing/persistence is off the recording hot path.
The fresh-capture timeout does not claim to interrupt synchronous import I/O.

## Recording artifacts

```text
<parent_recording>/
  registered_daily_context/
    context.json
    registration.json
    geometry.json
    runtime_before.json
    runtime_after.json
    Cam<SERIAL>_native.raw
  registered_context_geometry_v1.json
  registered_context_use_v1.json
  recording_snapshot_start.json
  recording_session.json
```

The archived daily files are exact-byte copies. `context.json` still says
`recording_binding: unbound`, and its original frame IDs and timestamps are not
relabeled as belonging to the new recording's producer. `context.json` is copied
last; the destination is reopened and validated before use is accepted. Existing
archive directories are never adopted or overwritten.

The new `orange.recording.registered_context_use` v1 receipt relates the old
capture ID/digest to the new recording ID, current recording camera/master
bindings, current geometry and renewed declaration. A camera's current producer
identity belongs to this use binding, not to the old context source frame.
The immutable start requirement uses schema version 2 and profile
`daily_registered_context_reuse_v1`; the old fresh-capture evidence is unchanged.

At completion, the shared native/external parent-manifest gate verifies the
copied bundle, use receipt, current geometry, immutable selected configuration
and master-owner camera set. Missing/changed required evidence prevents a
`completed` parent, and the headless final check reports failure. Finalization
does not open the original daily directory or registration source path. Moving
the original calibration files after import therefore does not break the saved
recording. This is integrity binding, not a signature against wholesale evidence
replacement.

Rolling clips share this parent-level evidence; no new image is captured per
clip, and the context creates no master-frame or crop-output obligation.
Source-detach ACKs and context verification do not prove encoded-video success.

## Candidate schemas and validation

- `docs/schemas/orange_registered_context_config_v2.schema.json`
- `docs/schemas/orange_registered_context_reuse_evidence_v2.schema.json`
- `docs/schemas/orange_registered_context_use_v1.schema.json`

These remain opt-in producer candidates; existing Citrus v6 names and Palette
admission are unchanged. Full schema-engine and downstream review are pending.
CPU tests cover configuration roundtrips; intact two-camera import; exact bytes
and preserved old/new identity separation; independence from original paths;
closed-schema/type/alias rejection; changed registration/settings; and required
completion failures after evidence alteration or mutable-snapshot refresh.
Camera-free CLI checks exercise both v1 and v2 (fresh, reuse and rolling), without
reading the selected asset or opening a camera. Actual source custody and geometry
checks happen during prearm, not `--validate-experiment-spec`.

Validation at this checkpoint: both isolated Orange executables rebuilt; sixteen
focused CTest suites passed; 42 context and 19 existing journal/crop camera-free
configuration cases passed; all eight daily artifact/reuse CPU groups passed
under address/undefined/leak sanitizers outside the sandbox. The three new
schemas and updated fresh-evidence schema parse, and `git diff --check` passes.
No live cameras, GUI session or encoder workloads were launched. The fresh v1
evidence schema additionally permits explicit fresh v2 configuration; it does
not admit a daily-reuse configuration without the separate reuse proof.

## Still outstanding

- [ ] Live GUI clipboard and headless reuse validation against real registration.
- [ ] Full JSON Schema-engine validation and coordinated consumer acceptance.
- [ ] GUI recording-time context selection/import (the GUI currently captures
      and exports configuration for headless; it does not reuse during GUI arm).
- [ ] Explicit recording media selector and crop-only supervision/finalization.
- [ ] Actual recorder-returned identities, packet/mux/container/decode/hash
      reconciliation before claiming moving-crops-only recording is usable.

Full-frame/split-GOP and single-subject moving/lossless crops remain supported;
this slice does not disable any full-frame encoder or introduce fixed-region,
atlas or visualization workloads.
