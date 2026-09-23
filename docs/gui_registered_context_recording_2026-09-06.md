# Registered native context in regular GUI recordings

Date: 2026-09-06. Branch: `agent/acquisition/master-frame-journal-v1-20260906`.
Base: `46b7d9f51f262a0d7c88d1655bd1953cadbb838f`.

This slice connects saved Daily Registration context to normal Orange recording,
including the same operator start path invoked by Citrus local control. It does
not select crop-only media, replace full-frame/split-GOP recording, or require a
new executable. Live GUI/camera acceptance remains pending.

## Operator workflow

1. Finish Daily Registration and capture the native context after placing the
   dishes and setting cameras/illumination. Capture remains possible without YOLO.
2. Confirm **Scene unchanged since this capture for the next recording**, then
   choose **Use Context for GUI Recording**. This selects the exact descriptor,
   digest, declarations and explicit housekeeping CPU used for that capture.
3. In **Registered recording context (optional)**, review the selected context
   and CPU list. **Save context recording settings** preserves the selection in
   the Orange app config, under `recording.registered_context_recording`.
   No existing defaults are changed merely by opening the application or capturing
   an image. The save action preserves other app/recording fields.
4. Start recording normally, or let Citrus invoke the normal local-control start.
   Each arm attempt consumes the operator confirmation. Confirm the unchanged
   scene again before the next run. Saved configuration does not restore this
   live confirmation after application restart.

The panel can also paste the existing headless `registered_scene_context` fragment,
or the complete GUI configuration. Pasting does not count as scene confirmation.
Journal writer CPUs initially use the selected capture's explicit housekeeping
CPU; edit the saved configuration or paste a complete configuration to supply
different writer CPUs, queue capacity or context worker CPUs. These must come from
the rig's isolation plan, not a guessed acquisition or render core.

GUI configuration v1 is closed and disabled by default. Enabled configuration
contains `schema_version: 1`, `enabled: true`, `master_frame_journal` (existing
configuration v1), and `registered_scene_context` (existing v2 with
`source.kind: daily_registration`). Both nested products must be enabled.
It deliberately does not implement fresh capture *during* GUI arm: the native
image is captured through the separate Daily Registration action first.

See `schemas/orange_gui_registered_context_recording_config_v1.schema.json`.
Configuration parsing and persistence live in `src/gui_recording_evidence.*`;
GUI controls live in `src/gui/registered_context_recording.*`.

## Start and completion gates

The recording-start worker imports and verifies the saved context before sealing
the immutable start snapshot. It uses the same byte-exact reuse validator as
headless: descriptor size/SHA-256, accepted registration, exact camera set,
native raster, all nine runtime camera settings and per-camera selected geometry.
Camera index zero is valid. Original capture identity is never relabeled as a
frame of the new recording. The separate use receipt binds the original asset to
this recording and its new master journal identity.

Camera mutations and competing calibration/start operations remain excluded by
the recording-start reservation. Live camera objects are copied on the GUI thread;
the background import never reads them. Context checks, import and snapshot
sealing run on the explicitly selected housekeeping CPU. Affinity is restored
before recorder processes start. No extra per-frame camera/PTP polling is added.

Only after successful import, immutable sealing and supervisor startup does the
GUI install the prepared journal sources. The shared session completion function
also requires the ready flag and installed journal owner, and performs the normal
Citrus observation-binding prearm gate before `record_video` becomes true.
Failures and canceled starts stay disarmed; failed journals cannot yield a
successful required-product proof. Local-control success is acknowledged only
after actual arm, as before.

The immutable start stores the resolved GUI configuration, context-use evidence,
and master-journal start evidence. The existing parent artifacts are unchanged:

- `registered_daily_context/`: exact copies of the saved context bundle;
- `registered_context_geometry_v1.json`: current recording geometry;
- `registered_context_use_v1.json`: capture-to-recording use binding;
- `Cam<S>_master_frames_v1.csv` and `.json`: source journal and final descriptor.

GUI journal evidence identifies `profile: gui_acquisition_loop_v1`; headless keeps
`headless_acquisition_loop_v1`. The journal's producer UUID is a **per-recording
journal producer identity**, not evidence of a camera hardware/thread restart.
Its generation starts at zero within that fresh identity. Camera local/hardware
frame IDs retain their actual continuously streaming values.

The background finalizer runs required context/journal hashing on the selected
housekeeping CPU and seals the master before parent manifest validation. Required
context success is reported as `captured`, master success as `complete`. A missing,
failed or corrupt required product prevents the parent from claiming completion.
The original daily directory is not needed after its verified copy is archived.
Rolling clips share this parent context and source domain; they do not pretend to
have new context captures at each clip boundary.

## Continuous GUI streaming and source lifetime

GUI camera threads commonly outlive individual experiments. Each camera therefore
has a stable `MasterSourceSlot`, created before streaming and destroyed only after
camera teardown. The acquisition thread reads a lock-free pointer and publishes
a hazard lease; it does no allocation, shared-pointer increment, mutex acquisition
or file I/O for this handoff. Existing journal fact submission remains bounded.

The control plane retires old admission and waits for the last source lease before
replacing ownership. A retirement timeout leaves storage alive and refuses rearm;
it never frees a still-referenced journal. Storage is bounded to the current/last
run, rather than accumulating every old journal until stream shutdown.

The recording flag is sampled before the slot lease. A frame iteration begun
before arm cannot become an unjournaled recording frame due to a later flag read.
First-active-iteration state resets parent recording numbering even when no idle
camera frame arrived between experiments. Pauses/resumes and rolling clip changes
within the same parent do not trigger that reset. Stream-local and camera hardware
IDs are not reset. Stop closes source admission and accounts for admitted tails
before the normal recording drain/finalization sequence.

## Implementation checklist and acceptance

- [x] Shared closed GUI configuration and explicit persisted source selection.
- [x] Daily capture selection, recording-panel controls, per-arm confirmation.
- [x] Asynchronous reuse import and immutable evidence before normal/Citrus arm.
- [x] Shared session ready gate and failed/canceled-start cleanup.
- [x] Safe per-recording journal handoff for continuously streaming cameras.
- [x] First-run numbering, bounded retirement and existing parent finalization gate.
- [x] CPU regression cases for configuration, required evidence, source tail and rearm.
- [ ] Live GUI run, Citrus-triggered run, consecutive runs without stream restart,
      configuration reload, rejected geometry/settings drift, and stop/cancel cases.
- [ ] Full JSON Schema-engine validation and coordinated downstream acceptance.
- [ ] Separate crop-only media selector and actual encoder-returned media proof.

Validation passed for this checkpoint:

- Both production executables: `orange` and `orange_client`, in
  `/tmp/orange-timing-build-20260906` (not installed).
- Seventeen focused CTest suites, including GUI settings/confirmation, master
  handoff, phased start, finalizer, session status/drain, context, rolling/crop
  metadata, packet telemetry and acquisition/timing authority regressions.
- Finalizer tests exercise both a complete master and an intentionally incomplete
  master through the actual GUI finalizer and common parent manifest writer;
  the latter yields a failed parent. Caller CPU affinity is restored.
- Forty-two camera-free context admission cases and nineteen existing journal/crop
  admission cases through the headless executable.
- Nine daily/context/reuse/GUI evidence groups and eight master-acquisition groups
  under address, undefined-behavior and leak sanitizers, including 100 tail/rearm
  cycles and 100 concurrent streaming replacements.
- Changed schema JSON syntax and `git diff --check`. This is not full
  JSON Schema-engine validation, which remains listed above.

No cameras, GUI session, PTP services, installed wrappers, or production configuration
are changed by the implementation/test work itself.
