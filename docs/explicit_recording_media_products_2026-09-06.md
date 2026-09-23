# Explicit recording products — 2026-09-06

Status (updated 2026-09-07): all three product selections are implemented in the
isolated GUI and timed headless build. Crop-only now requires verified registered
context, an independent master journal, real full-rate detection and supervised
moving crops at startup, and validates its own clip collection at completion.
See [activation and acceptance](crop_only_activation_2026-09-07.md).
Live acceptance and production installation remain pending.

## Operator contract

Choose a product, not an enable-then-suppress combination:

| Mode | Full-frame encoder | Moving-crop encoder | Required context |
| --- | --- | --- | --- |
| `full_frame` | Yes | No | No |
| `full_frame_and_moving_crops` | Yes | Yes | No |
| `registered_context_and_moving_crops` | No | Yes | Yes, verified before arm |

These choices do not change native/split-GOP transport, codec, GOP, GPU placement,
crop size, detection selection, moving-window placement, or lossless settings.
Single-subject moving crops and full-frame split-GOP remain supported products.
There is no `disable_full_frame` field, discard sink, or admission override.
The product plan directly determines which recorder owners are constructed.

The `Record` camera selection means participation in a logical recording session.
Acquisition and its recording-frame identities remain independent of selected
encoders. The product plan owns which output pipelines should exist. GUI workers
are constructed when streaming starts, so product selection is frozen there;
stop and restart streaming to change it. Explicit-mode camera membership or crop
changes after startup are rejected before a recording folder is claimed.

## Configuration and metadata

GUI app configuration: `recording.media_products`. Headless experiment spec:
`fixed.media_products`. Both use the same closed selection:

```json
{"schema_version": 1, "mode": "full_frame_and_moving_crops"}
```

Omitting the field retains existing per-camera choices. This is compatibility
behavior, not a fourth media product. Explicit null, misspellings, extra switches,
and unsupported versions are rejected. The GUI's “Existing per-camera choices”
removes the explicit selection only when the user saves it.

In the GUI, use **Recording media products** before starting streaming. An
explicit choice sets crop-recording participation for selected recording cameras
before workers are created. `Record`, YOLO, pose, and encoder settings retain
their separate meanings. Save is deliberate and preserves context/encoder and
unrelated app settings. Merely making a selection does not edit the app file.

Headless also requires an explicit matching `fixed.crop_recording` backend when
moving crops are selected. A full-frame-only choice with a configured active crop
backend is rejected, not silently ignored. Incomplete crop-only requirements are
rejected during spec validation; actual context and recorder readiness are checked
again before recording is armed.

For explicit selections, `session.recording_media_plan` in the recording snapshot
captures each participating camera, effective mode, and required media products
before immutable start sealing. Headless `runs.json` also retains the selection.
This is planned output evidence, **not** proof that those media were encoded.
Existing final output descriptors remain the output inventory for full-frame
products. The crop-only finalizer now writes an explicit clip collection; see
[crop-only parent and clip inventory](crop_only_parent_and_clip_inventory_2026-09-07.md).
Optional context
and master settings for full-frame products remain separately recorded and are not
made mandatory by this slice. Universal master journaling is still a separate
rollout; a false `master_frame_record_required` does not mean no journal exists.

Schemas:

- `schemas/orange_recording_media_products_v1.schema.json`
- `schemas/orange_recording_media_plan_v1.schema.json`

## Implementation checklist

- [x] One CPU-only mode parser and per-camera plan shared by GUI and headless.
- [x] Closed mode configuration; preserve unconfigured behavior.
- [x] Deliberate GUI persistence, separate from encoder settings and context choice.
- [x] Freeze explicit GUI choices at stream construction; reject later drift.
- [x] Full-frame pipeline construction uses the planned full-frame product.
- [x] Record explicit plans before immutable start sealing.
- [x] Replace the temporary implementation refusal with required startup evidence.
- [x] Separate headless moving-crop supervisor options from full-frame contract.
- [x] Route GUI single/rolling parent finalization without full-frame camera artifacts.
- [x] Route headless timed-run single/rolling parent finalization without full-frame files.
- [x] Complete headless experiment run-result/post-run handling for crop collections.
- [x] Integrate actual encoded-frame identities, packet/mux evidence, media hashes
      and decoded-raster/count checks with master-to-crop coverage on both paths.
- [x] Require registered context and an independent master journal at crop-only arm.
- [x] Remove the implementation refusal and enable the GUI choice without an
      override switch; preserve required-evidence admission checks.
- [x] Camera-free missing/tampered/tail-loss/zero-detection and rolling validation.
- [ ] Live GUI/Citrus-triggered and headless validation, then throughput checks.

## Verification

Follow-up implementation and remaining crop-only activation work:
[moving-crop encoded-media finalization](moving_crop_encoded_media_finalization_2026-09-06.md).
The production-build/regression totals below describe the initial product-selector
slice; follow-up verification is recorded in that document and the Citrus checklist.

`recording_media_plan_tests` covers product ownership, recording membership,
schema admission, planned context/master requirements, compatibility, app-field
preservation, explicit removal, and symlink-save refusal. GUI control and phased
start tests cover restored selections, frozen membership, prearm rejection and
snapshot persistence. `tools/test_headless_media_products_spec.py` exercises
camera-free CLI admission, including valid crop-only, missing requirements and contradictory
product/backend combinations. Production builds and focused regression results
are recorded below. JSON schema documents supplement the C++
closed parser; full schema-engine validation is not claimed.

Passed in `/tmp/orange-timing-build-20260906`: both `orange` and `orange_client`
production targets; 18 focused CTest suites; 15 media-product, 42 registered-context
and 19 master/crop camera-free admission cases. The CPU-only media-plan tests and
nine daily-context/reuse/GUI-evidence groups also pass address, undefined-behavior
and leak sanitizers. Schema JSON parsing and `git diff --check` pass. No hardware
run, production installation, default-config edit, or remote publication occurred.
