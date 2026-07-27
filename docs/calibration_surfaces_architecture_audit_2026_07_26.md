# Orange Calibration Surfaces Architecture Audit

Date: 2026-07-26

Status: architecture audit; Phase 1 in-process lease implemented 2026-07-26,
live validation pending

Repository baseline:

- worktree: `/home/jeremy/orange-gop-split-a16`
- branch: `exp/gop-split-a16`
- inspected HEAD: `f84ea96`
- the worktree already contained unrelated uncommitted changes during the
  inspection; those changes were preserved

Related documents:

- [Orange/Citrus calibration stack lifecycle audit](calibration_stack_lifecycle_audit_2026_07_18.md)
- [Orange calibration artifact contract](calibration_artifact_contract.md)
- [Calibration package v2 candidate contract](calibration_package_v2.md)
- [Commissioned setup packaging checklist](commissioned_setup_packaging_implementation_checklist.md)
- [Rig-owned canvas commissioning release](rig_canvas_commissioning_release.md)
- [Guided daily registration contract](orange_citrus_guided_daily_registration_contract.md)
- [Target-derived calibration plan](target_derived_calibration_plan.md)
- [Production-baseline sensor characterization](sensor_baseline_characterization.md)
- [Calibration transaction lease](calibration_transaction_lease.md)

## Executive conclusion

Orange has strong calibration mathematics, useful individual tools, and several
good provenance and lifecycle primitives. The main problem is now architectural:
the operator surfaces, acquisition ownership, evidence storage, and authority
lifecycles have grown as partially overlapping systems.

The repository currently exposes calibration through:

- three separate GUI windows;
- live camera, optics, illumination, and PTP controls elsewhere in the main UI;
- one very large Spatial Layout window that spans capture, analysis, authoring,
  review, promotion, commissioning, and daily runtime selection;
- several persistent commissioning runners still named as smoke tests;
- independent review and recovery CLIs; and
- multiple artifact roots and registry conventions.

At the inspected baseline, the highest-priority issue was not presentation:
Orange did not have one global calibration transaction lease. The initial
process-wide lease and GUI/orchestration guards were implemented on 2026-07-26.
They cover the known Spatial, guided, daily, aperture, FOV, and USAF entry
points. Durable crash recovery, a persisted mutation/restoration journal, and
cross-process/headless coordination remain future work.

The recommended direction is therefore not another standalone target window.
Before adding slanted-edge MTF or depth-of-field acquisition, Orange should
introduce:

1. one exclusive calibration acquisition coordinator;
2. one versioned structured-recipe vocabulary;
3. one common frame-evidence contract;
4. one sealed-product, acceptance-receipt, and scoped-selection lifecycle; and
5. one Calibration & Commissioning home that routes operators by intent.

This does not require rewriting the calibration math or immediately moving
existing artifacts. The first migration can wrap existing workflows and build
a unified read model over their current storage locations.

## Scope

This audit covered the current Orange-side surfaces for:

- camera, lens, filter, illumination, GPIO, and PTP state;
- aperture, field-of-view, USAF, and sensor-baseline characterization;
- single-camera, grouped, averaged, and guided calibration capture;
- arena centering, homography, projected-surface scale, and holder validation;
- top-rim observation and daily dish registration;
- session review, candidate review, acceptance, promotion, and runtime
  selection;
- artifact schemas, registries, recording geometry, and package-v2; and
- current calibration plans and implementation-status documentation.

The audit did not:

- operate cameras, lights, or the projector;
- launch Orange or Citrus;
- modify active calibration state;
- re-fit any calibration product;
- assess the correctness of every calibration algorithm numerically; or
- inspect Citrus internals except where Orange's boundary contract requires it.

The earlier lifecycle audit remains the detailed source for the homography and
projection findings. This document focuses on the organization of the complete
Orange calibration surface.

## Terminology used here

The following terms are deliberately distinct:

- **capture session**: acquisition evidence and its physical/configuration
  context; it does not confer runtime authority;
- **derived product**: a calibration result calculated from evidence;
- **sealed product**: an immutable, checksum-complete derived product;
- **operator-confirmed**: a human confirmed a physical interpretation, such as
  which visible circle is the water-side inner rim;
- **accepted**: the owning calibration authority accepted a sealed product;
- **selected for runtime**: an accepted product was explicitly chosen for the
  current runtime mode;
- **active pointer**: a small mutable, atomic reference to an accepted immutable
  product or release; and
- **recording capsule**: the exact participating calibration subset preserved
  with one recording.

The word `accepted` currently carries several of these meanings in different
surfaces. The distinction above should become the shared vocabulary.

## Current surface inventory

| Surface | Entry and prerequisites | Current state owner | Primary outputs |
| --- | --- | --- | --- |
| Camera Property / Optics / Rig I/O | Main Orange window; cameras open | Live `CameraParams` plus panel-local state | Camera mutations, lens/filter metadata, GPIO and illumination diagnostics |
| Host PTP Stack | Main Orange panel; privileged mutation where required | Separate host-PTP state and worker | Service state and control output |
| Aperture Characterization | Main Orange button; cameras open, normal streaming and recording stopped | Dedicated aperture state, alignment worker, and sweep worker | Aperture manifest, measurement, step/frame CSVs, ruler and representative images |
| USAF Resolution Calibration | Main Orange button; cameras open, normal streaming and recording stopped | Dedicated USAF state, preview worker, and artifact worker | USAF manifest, measurement, positions CSV, reference frames, and ROI overlays |
| Spatial Layout / Experimental Area Registration | Main Orange button; cameras open; many operations also require Citrus | One large `SpatialLayoutUiState` plus several controllers/workers | Image sets, top-rim observations, layouts, masks, candidate reviews, commissioning and daily products |
| Generic guided group capture | `scripts/run_gui_guided_capture_smoke.sh`; dry-run unless armed | Shell orchestration plus Orange/Citrus control transactions | Grouped session captures for multiple workflow profiles and scene recipes |
| Canvas and arena commissioning | `scripts/run_gui_arena_centering_commissioning.sh` | Shell runner plus Orange/Citrus transaction | Center/size proposals, debug overlays, homographies, and canvas commit evidence |
| Projector intensity commissioning | Python runner with explicit physical confirmation | Runner manifest around generic guided capture | Commissioning workspace, per-level captures, and intensity report |
| Holder fixture validation | Python runner with holder-state confirmation | Runner manifest around generic guided capture | Holder evidence, validation overlays, and operational candidates |
| Homography and scale recovery/promotion | Spatial UI plus separate review CLIs | Citrus candidate/review transactions reached through Orange control | Accepted candidates and active pointers |
| Sensor baseline characterization | Headless dark/flat Python runner | Independent run-manifest/session model using bounded pre-encoder capture | Raw NV12 evidence, applied sensor-state readback, metrics, and report |
| Package migration and validation | `scripts/calibration_package.py` | Filesystem package tool | Package-v2 candidate and exact-byte validation; no general activation UI |
| Generic/legacy picture and pose capture | Main Orange picture-save panel and Indigo calibration state | Acquisition flags and a global calibration enum | Pictures and numbered pose images outside the structured product lifecycle |

The main Orange shortcuts are three inline buttons displayed after cameras open,
not a calibration dashboard. See
[`src/orange.cpp`](../src/orange.cpp#L5763). The application window enables
menu-bar flags, but no calibration-oriented menu or status home currently ties
these tools together.

## Current lifecycle topology

The present architecture is closer to the following than to one linear
calibration system:

```text
camera controls ───────────────┐
                              ├─ aperture standalone artifacts
temporary camera streams ─────┼─ USAF standalone artifacts
                              └─ FOV/ruler evidence

live acquisition fanout ──────┐
                              ├─ Spatial Layout session/image sets
Citrus scene control ─────────┼─ homography/scale candidates and review
                              ├─ holder/commissioning workspaces
                              └─ daily rim/registration products

headless recording tap ───────── sensor-baseline characterization session

session-era artifacts ────────── local index.json / latest_by_schema
Citrus accepted products ─────── Citrus active pointers and releases
package-v2 ───────────────────── candidate-only exact-byte package tree
recording geometry ───────────── recording-local assets and H5 mirror
```

Each branch has useful pieces. The problem is that they do not share one
transaction boundary, recipe model, evidence envelope, or authority browser.

## Findings

### P0 baseline finding: no global calibration acquisition lease

Implementation update, 2026-07-26: the first in-process coordinator described
in [Calibration transaction lease](calibration_transaction_lease.md) now covers
the known GUI/orchestration paths below. This subsection preserves the baseline
finding that motivated it. Durable restoration journaling and cross-process
ownership are still open.

At the inspected baseline, the global busy calculation included only aperture job/alignment and
USAF job/preview state:

- [`src/orange.cpp`](../src/orange.cpp#L5285)

That flag disables or guards:

- the camera-property block at
  [`src/orange.cpp`](../src/orange.cpp#L5781);
- the streaming control at
  [`src/orange.cpp`](../src/orange.cpp#L6500); and
- the recording control at
  [`src/orange.cpp`](../src/orange.cpp#L6852).

Spatial Layout grouped capture, guided commissioning, and daily registration
did not participate in that flag. Camera properties otherwise remained mutable,
including width, height, ROI, gain, focus, iris, exposure, and frame rate:

- [`src/gui/camera_properties_panel.cpp`](../src/gui/camera_properties_panel.cpp#L430)

This meant a manual spatial or daily transaction could coexist with camera
mutation, stream shutdown, or recording startup. Individual buttons checked the
state at the instant they were pressed, but no global lease protected the entire
multi-step transaction.

Required correction:

- introduce one `CalibrationAcquisitionCoordinator` or equivalent owner;
- acquire an exclusive transaction lease before any physical preparation or
  Citrus scene mutation;
- declare which camera/projector mutations the recipe is allowed to perform;
- block conflicting UI and local-control actions for the entire lease;
- retain an emergency abort path; and
- persist enough pre-mutation live readback for recovery after abnormal exit.

The lease must not simply mean "streaming off." Some calibration recipes need
the production stream and TTL timing. It should instead mean exclusive
ownership of the relevant camera set, mutable settings, recording lifecycle,
and Citrus calibration scene.

### P0: Spatial Layout mixes incompatible operator authorities

The Spatial Layout window currently combines:

- direct still capture;
- display-preview capture;
- full-resolution stream capture;
- temporal averaging;
- grouped Citrus capture;
- Citrus canvas import;
- manual layout and zone authoring;
- Hough detection and tuning;
- image-set and top-rim persistence;
- session review;
- homography candidate review and promotion;
- projected-surface scale review and promotion;
- immutable canvas commissioning release finalization;
- optional daily runtime-mode selection; and
- the guided daily-registration transaction.

Representative boundaries are visible in:

- capture controls at
  [`src/spatial_layout_ui.cpp`](../src/spatial_layout_ui.cpp#L992);
- grouped capture at
  [`src/spatial_layout_ui.cpp`](../src/spatial_layout_ui.cpp#L1142);
- homography review at
  [`src/spatial_layout_ui.cpp`](../src/spatial_layout_ui.cpp#L1572);
- scale review at
  [`src/spatial_layout_ui.cpp`](../src/spatial_layout_ui.cpp#L1827);
- commissioning authority at
  [`src/spatial_layout_ui.cpp`](../src/spatial_layout_ui.cpp#L2037); and
- daily runtime selection at
  [`src/spatial_layout_ui.cpp`](../src/spatial_layout_ui.cpp#L2164).

The shared state object mirrors this coupling. It carries capture buffers,
session review, schema authoring, Citrus authority, candidate state,
commissioning state, daily state, and render-preview state in one structure:

- [`src/gui/spatial_layout/state.h`](../src/gui/spatial_layout/state.h#L427)

A daily operator should not need to navigate controls that can modify canonical
layout or activate stable rig authority. The UI should route to distinct
workspaces even if those workspaces reuse the same underlying services.

### P0: the current live-mask recording schema contradicts runtime output

At the time of inspection, the checked-in recording-geometry schema required:

```text
active_in_orange_live_detection_pipeline = false
```

See:

- [`docs/schemas/orange_recording_geometry_contract.schema.json`](schemas/orange_recording_geometry_contract.schema.json#L370)

The runtime correctly changes that field when the selected spatial-mask policy
is armed:

- [`src/orange.cpp`](../src/orange.cpp#L2167)

A recording using live centroid gating can therefore fail the published schema
despite accurately reporting runtime behavior. The schema should describe the
allowed boolean state and require the accompanying runtime policy/provenance
when it is true. This correction is independent of the larger reorganization
and should be made first.

Resolution on 2026-07-26: the schema now permits `true` only when the same
recording-local entry declares the enforcing `gate_only` or
`gate_and_input_mask` mode. Persisted-but-unused geometry, `off`, and `audit`
remain constrained to `false`; the neural-input flag is true only for
`gate_and_input_mask`. Existing recording artifacts remain valid.

### P0: evidence and authority are fragmented across roots and browsers

The existing artifact contract deliberately separates:

```text
~/orange_data/calibrations/sessions/<session_id>/artifacts
~/orange_data/calibrations/standalone_artifacts
```

See:

- [`docs/calibration_artifact_contract.md`](calibration_artifact_contract.md#L35)

Additional workflows use:

- `calibrations/commissioning/...`;
- `calibrations/rig_characterization/...`;
- Citrus-owned per-rig/canvas calibration directories;
- package-v2 candidate output roots; and
- recording-local geometry assets.

No single surface answers all of the following:

- What evidence was captured?
- Which analysis result passed?
- Which boundary was operator-confirmed?
- Which product did Citrus accept?
- Which accepted product is selected now?
- Why is a product stale or incompatible?
- Which exact products did a recording use?

The first cleanup should be a unified read-only catalog over the existing
locations. Moving or rewriting old data is not required to deliver that value.

### P1: package-v2 cannot yet complete its authority lifecycle

Package-v2 has the strongest publication and identity model in the repository,
but the common schema deliberately fixes packages as immutable,
non-activatable candidates:

- [`docs/schemas/orange_calibration_package_common.schema.json`](schemas/orange_calibration_package_common.schema.json#L42)

The CLI likewise has no select or activate command, and the deferred work still
includes registry publishing, promotion, rollback, arm-time composition, and
GUI selection:

- [`docs/calibration_package_v2.md`](calibration_package_v2.md#L125)

The immutable bytes should not be mutated from `candidate` to `accepted`.
Instead:

1. seal the content-addressed product;
2. write a separate immutable acceptance or rejection receipt; and
3. atomically update a small scoped active pointer after all gates pass.

Using `sealed` for immutable content and reserving `candidate`, `accepted`, and
`selected_for_runtime` for external lifecycle records would remove ambiguity.

### P1: legacy publication is not artifact-atomic

Several session-era writers publish files sequentially into their final
artifact directories. Individual JSON files may be written atomically in some
paths, but the complete multi-file artifact is not published by one atomic
transition. Examples include:

- generic image-set materialization in
  [`src/gui/spatial_layout/save_jobs.cpp`](../src/gui/spatial_layout/save_jobs.cpp#L801);
- top-rim package writing in
  [`src/dish_top_rim_observation.cpp`](../src/dish_top_rim_observation.cpp#L1353);
- aperture package writing in the standalone tool; and
- session/index updates that occur after payload creation.

Package-v2 already implements the preferred primitive: build a sibling staging
tree, inventory exact bytes, flush it, and publish it with one rename:

- [`scripts/calibration_package_lib.py`](../scripts/calibration_package_lib.py#L396)

That mechanism should become shared publication infrastructure for new and
migrated products.

### P1: registry scope is insufficient

The legacy registry's `latest_by_schema` entry is a last-writer-wins convenience
pointer:

- [`src/project.cpp`](../src/project.cpp#L6649)

It does not encode the dimensions required for a safe calibration lookup:

```text
product kind
+ rig geometry revision
+ camera serial
+ arena ID, when applicable
+ target plane
+ optical operating-point fingerprint
+ accepted/selected lifecycle status
```

There are also two implementations of the legacy registry update logic: the
central helper and a local top-rim implementation:

- [`src/project.cpp`](../src/project.cpp#L6649)
- [`src/dish_top_rim_observation.cpp`](../src/dish_top_rim_observation.cpp#L318)

The implementations have different locking and write behavior. Registry and
publication logic should have one owner.

### P1: mutable aggregate identity can invalidate earlier references

Generic image-set saves append captures into an existing aggregate and then
recompute its fingerprint:

- [`src/gui/spatial_layout/save_jobs.cpp`](../src/gui/spatial_layout/save_jobs.cpp#L848)

As a result, an earlier `<artifact_id, fingerprint>` pair can cease to identify
the aggregate after another capture or linked observation is appended.

The safer split is:

- immutable individual capture products;
- immutable observation and analysis products; and
- a mutable session index containing references to those products.

Session indexes may evolve. Referenced calibration products must not.

### P1: grouped capture is not an atomic PTP-aligned acquisition

The grouped controller first waits for the Citrus presentation fence and then
issues a snapshot request to each camera worker sequentially:

- [`src/gui/spatial_layout/group_capture_controller.cpp`](../src/gui/spatial_layout/group_capture_controller.cpp#L1663)

Each worker independently claims its next available frame. PTP keeps the camera
clock domains aligned, and stricter commissioning runners reject groups whose
captured timestamp span exceeds their threshold, but the workers are not armed
against one shared target frame or PTP epoch.

The generic grouped mode is already documented as appropriate when exact
same-frame timing is not required:

- [`docs/calibration_image_set_schema.md`](calibration_image_set_schema.md#L989)

The UI and metadata should therefore distinguish:

- `grouped_next_frame_with_ptp_timestamps`; and
- a future `ptp_frame_aligned` barrier mode.

The aligned mode should arm all selected workers before release and choose a
shared target timestamp/frame boundary. Post-capture skew validation should
remain mandatory even after coordinated arming.

### P1: presentation settling is runner-dependent rather than recipe-owned

The group-capture engine defaults the post-presentation settle interval to zero
unless an environment variable overrides it:

- [`src/gui/spatial_layout/group_capture_controller.cpp`](../src/gui/spatial_layout/group_capture_controller.cpp#L142)

Some commissioning wrappers intentionally supply a nonzero interval, including
the arena-centering runner. This means the stable-render requirement is encoded
in the invocation rather than in the calibration task itself.

Every structured recipe should declare:

- the display fence it requires;
- minimum post-fence time and/or frames;
- whether a black clearing scene is required;
- the first eligible capture epoch; and
- the validation that proves the pre/post scene identity stayed unchanged.

### P1: native geometry and native sample representation are conflated

`SpatialSnapshotWorker` preserves the full configured camera dimensions and
frame identifiers, but converts supported camera formats into 8-bit RGBA before
the Spatial Layout analyzers and writers consume them:

- [`src/spatial_snapshot_worker.cpp`](../src/spatial_snapshot_worker.cpp#L354)

The resulting artifact is in camera-native **coordinates**, but it is not
necessarily in the camera's native sensor/transport representation. This is
mostly benign for the present Mono8 geometry workflow but becomes important for
Mono10/12, photon-transfer, MTF, and noise work.

Evidence metadata should separately state:

- source raster and coordinate space;
- camera transport pixel format;
- stored sample representation and bit depth;
- conversion steps;
- resized or unresized status; and
- whether the payload is raw evidence, a derived analysis image, or only a UI
  preview.

### P1: USAF and FOV captures have weak source-frame identity

USAF's `Capture position` operation copies whichever full-resolution RGB buffer
was most recently published by its preview thread:

- [`src/usaf_resolution_ui.cpp`](../src/usaf_resolution_ui.cpp#L115)

FOV/ruler capture similarly copies the current preview state:

- [`src/orange.cpp`](../src/orange.cpp#L2554)

These paths do not request a fresh identified source frame or preserve a camera
frame ID and embedded PTP timestamp. They are useful interactive alignment
tools, but they should not become the foundation for authoritative MTF or DOF
evidence.

USAF/FOV is the best first migration target for a common acquisition service:
it needs the new evidence contract, and the migration directly prepares the
planned optical-characterization work.

### P1: temporal averaging is memory-heavy and incompletely evidenced

The current Spatial snapshot average:

- converts every source to RGBA;
- keeps four `uint32` sums per pixel;
- returns a rounded 8-bit mean; and
- preserves only the first and last frame identities.

See:

- [`src/spatial_snapshot_worker.cpp`](../src/spatial_snapshot_worker.cpp#L245)

At `4512 x 4512`, the accumulator alone is approximately 325 MB per camera.
Four simultaneous averages can exceed 1.3 GB before source buffers and
temporary conversions.

A structured averaged acquisition should retain:

- every contributing local and camera frame ID;
- every embedded PTP timestamp;
- missing-frame and cadence checks;
- cross-camera window overlap;
- accumulation representation and rounding policy;
- saturation/clipping checks; and
- optional motion rejection where the target is expected to be stationary.

The implementation can also specialize the accumulator for monochrome data
rather than always summing RGBA.

### P1: setting restoration is workflow-specific and not crash-safe

Guided workflows temporarily change exposure/frame rate and may suppress mapped
strobe output. Restoration occurs in their normal shutdown path, but a crash or
forced termination can leave hardware state changed. Some restore snapshots
are also seeded from cached `CameraParams` rather than guaranteed live node
readback:

- [`src/gui/spatial_layout/preflight.cpp`](../src/gui/spatial_layout/preflight.cpp#L199)

Aperture characterization contains stronger setting restoration and
settling/frame-progression primitives:

- [`src/aperture_characterization.cpp`](../src/aperture_characterization.cpp#L994)

The shared coordinator should reuse the stronger behavior:

- read live values before mutation;
- persist the recovery snapshot before applying changes;
- restore in normal and exception paths;
- verify live readback after restoration; and
- expose an explicit recovery action for an abandoned transaction.

No in-process mechanism can repair a camera after abrupt process death by
itself, so the next startup should detect and offer recovery from the persisted
snapshot.

### P1: recipe and purpose contracts are duplicated

Supported recipe lists and recipe-to-purpose mappings occur independently in:

- [`src/gui/guided_capture_autorun.cpp`](../src/gui/guided_capture_autorun.cpp#L45);
- the Spatial Layout scene selector;
- `scripts/run_gui_guided_capture_smoke.sh`; and
- higher-level Python commissioning runners.

For example, shell defaults and C++ defaults give `uniform_gray` and
`black_reference` different purpose interpretations depending on invocation
context. The wrappers normally pass the explicit context through, but the
duplication makes semantic drift likely.

The right abstraction is not only a scene recipe. It is a versioned calibration
task that binds:

```text
operator intent
+ physical fixture state
+ target and plane
+ camera/lighting mutations
+ Citrus scene
+ acquisition policy
+ expected product
+ QC and authority policy
```

Adding MTF or DOF should require adding one checked-in recipe and one analyzer,
not updating parallel switch statements across the GUI, C++, shell, and Python.

### P1: the session matrix is fixed rather than workflow-specific

The current session capture matrix hardcodes a fixed set of rows, columns, and
expectations:

- [`src/gui/spatial_layout/session_capture_matrix.cpp`](../src/gui/spatial_layout/session_capture_matrix.cpp#L62)

It omits first-class workflows such as holder validation, aperture, USAF,
sensor baselines, and future MTF/DOF. It also cannot clearly distinguish:

- required;
- optional;
- not applicable;
- missing;
- invalid; and
- complete.

Cells select evidence, but launching the missing task and loading its selected
evidence are separate operations. The matrix should be generated from the
selected commissioning/daily/characterization recipe set and make incomplete
cells actionable.

### P2: capture metadata exposes the schema rather than the operator contract

The Spatial metadata panel exposes many raw fields for filters, illumination,
wavelengths, projector state, fixture state, dish state, planes, target method,
patterns, and generic image-set schema content:

- [`src/gui/spatial_layout/metadata_panel.cpp`](../src/gui/spatial_layout/metadata_panel.cpp#L147)
- [`src/gui/spatial_layout/metadata_panel.cpp`](../src/gui/spatial_layout/metadata_panel.cpp#L582)

Most normal workflows already have enough context to resolve these values from
a profile or recipe. The default operator view should show:

- the physical checkpoint requiring confirmation;
- resolved recipe and target;
- applied camera/light/projector readback;
- warnings or contradictions;
- a brief notes field; and
- the next permitted action.

Raw schema fields should remain available under an Advanced/Custom override and
the artifact should record every override explicitly.

### P2: naming hides lifecycle boundaries

Current examples include:

- `Spatial Layout Registration`, which now includes commissioning, promotion,
  daily registration, layout editing, and evidence review;
- `Arena Centering`, which is stable canvas commissioning rather than daily
  removable-dish centering;
- `run_gui_guided_capture_smoke.sh`, which is used to create persistent
  commissioning evidence; and
- `Use inner-rim fit for registration`, which applies a transient view/edit
  seed and can be confused with accepted daily registration.

Recommended operator-facing labels include:

- Canvas & Arena Commissioning;
- Daily Dish Registration;
- Canonical Layout Editor;
- Camera-View Fit Preview;
- Calibration Capture Runner;
- Candidate Review & Activation; and
- Advanced Manual Fit Tools.

The legacy generic picture/Indigo pose surface should be named for that purpose
and placed under legacy or dataset-capture tools instead of appearing to be part
of the rig calibration authority.

### P2: review is path-heavy rather than visual

The current session tree is useful, but evidence selection and loading are
multi-step. Candidate review often displays paths and tables while the relevant
source, detection, accepted overlay, and independent validation are separated.
The Fit Preview also uses many similar outlines explained through a dense
legend.

A shared review component should display together:

- source evidence;
- raw detector proposal;
- operator-adjusted/confirmed geometry;
- independent validation overlay;
- QC values and thresholds;
- target plane and coordinate system;
- provenance and dependency graph; and
- the exact lifecycle transition being requested.

### P2: daily registration retains two conceptual flows

The current ordinary daily contract is rim-only under normal IR optical state.
The UI/state machine still retains the earlier visible projected-center and
post-translation preview branch:

- [`src/gui/spatial_layout/state.h`](../src/gui/spatial_layout/state.h#L298)
- [`src/gui/spatial_layout/daily_registration_workflow.cpp`](../src/gui/spatial_layout/daily_registration_workflow.cpp#L2232)

If the projected-marker path remains valuable, it should be explicitly labeled
as recovery or independent validation. It should not complicate the normal
daily wizard.

### P2: documentation is rich but lacks one authoritative index

The repository has detailed design documents, implementation checklists, audit
records, and TODOs. Their individual quality is a strength, but current status
is difficult to reconstruct because:

- there is no calibration documentation landing page;
- some plans retain unchecked tasks after partial implementation;
- implemented, candidate-only, future, diagnostic, and superseded documents
  live together at the same level; and
- the same product can be described across acquisition, schema, runtime,
  commissioning, and recording documents.

A calibration index should list each product with:

- canonical schema ID and version;
- lifecycle status;
- owning writer and accepting authority;
- target scope and plane;
- GUI/CLI entry point;
- storage root;
- runtime consumer;
- recording/H5 behavior;
- current implementation document; and
- superseded documents.

## Strong foundations to preserve

The reorganization should retain, not replace, the following:

### Explicit plane and coordinate semantics

The current calibration contracts distinguish projected surface, dish/base,
fish-height assumptions, top rim, camera-native coordinates, arena-local
coordinates, and global canvas coordinates. This is essential and should remain
visible in every recipe and product.

### Physical boundary versus centroid-forgiveness policy

The schema-v2 top-rim design separates the physical water-side inner rim from
the outward centroid gate. This is the correct runtime model.

### Full-resolution authority guards

Spatial saves reject downsampled live-preview images for top-rim observations
and generic calibration image sets:

- [`src/gui/spatial_layout/save_job_preparation.cpp`](../src/gui/spatial_layout/save_job_preparation.cpp#L466)
- [`src/gui/spatial_layout/save_job_preparation.cpp`](../src/gui/spatial_layout/save_job_preparation.cpp#L827)

The live preview remains useful for navigation, but it cannot silently become
authoritative geometry.

### Shared live-stream snapshot fanout

`SpatialSnapshotWorker` uses the active acquisition pipeline, frame leases,
source frame identifiers, and embedded timestamps instead of opening another
camera stream. It is a good base for the shared coordinator after its evidence,
alignment, and representation contracts are strengthened.

### Citrus presentation fencing and restoration

The grouped controller verifies the displayed calibration scene before capture
and restores it after the transaction. This is the correct cross-program shape.

### Explicit candidate review and promotion

Projected-surface scale and homography workflows preserve candidates, revalidate
persisted results, and require explicit promotion. Daily acceptance is also
separate from runtime selection.

### Package-v2 publication and inventory

Content-addressed identity, SHA-256 inventories, path containment, exclusive
publication, and source-retention auditing are the strongest artifact practices
currently present.

### Sensor-baseline evidence validation

The baseline runner validates raw layout, frame count, camera state, cadence,
embedded timestamps, PTP span, and neutral chroma. This is a strong reference
for structured camera-characterization evidence.

### Recording-local geometry materialization

Orange can copy the exact compact geometry members participating in a recording
and Citrus mirrors the current recording geometry into H5. The future package
capsule should build on that proven deep-materialization behavior.

### Optionality with fail-closed explicit selection

Missing optional characterization does not block ordinary rig use. Once an
operator explicitly selects a calibration-dependent mode, invalid or stale
selected authority fails closed. That is the correct policy.

## Target information architecture

Orange should expose one **Calibration & Commissioning** home with five routes.
This should be a router and status surface, not another mega-window.

### 1. Rig commissioning

For stable or infrequent work:

- projector intensity selection;
- canvas and camera-visible arena placement;
- unobstructed projected-surface homography;
- projected-surface physical scale;
- holder-installed operational homography;
- holder/aperture validation;
- independent validation patterns; and
- immutable rig/canvas release finalization.

### 2. Daily setup

For removable dish placement:

- normal-runtime optical-state verification;
- grouped inner-rim capture;
- raw fit and accepted boundary review;
- translation-only candidate review;
- explicit acceptance and selection; and
- an equally visible `base_only` alternative.

### 3. Camera and optics characterization

For optional rig/camera characterization:

- sensor dark and uniform-field baseline;
- controlled photon-transfer series;
- aperture/transmission characterization;
- FOV and scale evidence;
- USAF resolution;
- slanted-edge MTF; and
- depth-of-field characterization.

These products normally bind to camera, lens, filter, optical operating point,
target plane, and rig geometry revision. They should not automatically become
projector-geometry gates.

### 4. Evidence and authority

A unified browser should show:

- current rig/canvas/camera scope;
- acquisition sessions;
- sealed products and dependency graphs;
- pending reviews;
- acceptance receipts;
- active commissioning release;
- selected daily registration;
- stale/incompatible explanations;
- rollback choices; and
- recordings that reference each product.

### 5. Advanced and legacy tools

Keep expert capabilities without placing them in the normal path:

- canonical layout authoring;
- arbitrary capture recipes;
- raw Hough tuning;
- manual view registration;
- schema metadata overrides;
- GPIO/light diagnostics;
- recovery validation paths; and
- Indigo/generic dataset capture.

## Proposed service boundaries

### CalibrationAcquisitionCoordinator

One process-level coordinator should own:

- the exclusive calibration transaction lease;
- selected camera scope;
- stream and recording compatibility;
- allowed camera and optics mutations;
- mapped light-output state;
- Citrus scene transaction ownership;
- settling and capture eligibility;
- group arming and PTP policy;
- source-frame evidence collection;
- restoration and recovery snapshots; and
- transaction status for the UI and local-control API.

Existing aperture restoration, Spatial snapshot fanout, grouped Citrus fencing,
and sensor-baseline validation should be reused inside this service.

### CalibrationRecipeRegistry

Recipes should be checked in, versioned, schema-validated, and persisted into
every run. A conceptual recipe contains:

```json
{
  "recipe_id": "optics.usaf1951.production_ir.v1",
  "lifecycle_class": "rig_characterization",
  "scope": {
    "camera_selection": "operator_selected",
    "rig_geometry_revision_required": true
  },
  "physical_state": {
    "holder": "installed",
    "dish": "installed",
    "water": "working_depth",
    "filter": "production_filter",
    "target": "usaf1951_chrome_on_glass",
    "target_plane": "fish_observation_plane"
  },
  "allowed_mutations": {
    "camera": [],
    "illumination": ["restore_production_nir"],
    "citrus_scene": ["black_reference"]
  },
  "acquisition": {
    "source": "live_full_resolution_frame_evidence",
    "synchronization": "grouped_next_frame_with_ptp_timestamps",
    "frame_count": 1,
    "averaging": "none",
    "post_scene_settle_ms": 1000
  },
  "product": {
    "schema_id": "orange.calibration.usaf1951_resolution",
    "authority_policy": "optional_characterization"
  }
}
```

This is illustrative rather than a frozen schema. The important property is
that GUI, CLI, automation, metadata, and validation consume the same recipe
definition.

### FrameEvidenceBundle

Every structured acquisition should emit one common evidence description with:

- recipe ID/version and transaction ID;
- selected camera/arena scope;
- source configuration checksum and live applied readback;
- physical-state confirmations;
- source raster and coordinate convention;
- transport pixel format and stored representation;
- complete conversion history;
- every contributing local/camera/recording frame ID;
- every embedded PTP and system timestamp;
- group and averaging-window membership;
- pre/post Citrus scene snapshots and presentation fence;
- exact source checksums;
- derived preview/mean paths kept distinct from raw evidence; and
- acquisition QC.

Product-specific analyzers should consume this bundle instead of owning camera
streams.

### CalibrationProductPublisher

One publisher should provide:

- canonical identity and checksum vocabulary;
- staging and artifact-atomic publication;
- exact-byte inventory;
- safe ownership handling;
- immutable retry validation;
- path containment;
- schema validation;
- acceptance/rejection receipt publication; and
- scoped active-pointer updates.

It should replace duplicate registry writers and direct final-directory
publication as workflows migrate.

### CalibrationCatalog

The catalog is a read model, not the authority itself. It should index current
session roots, standalone artifacts, commissioning workspaces, Citrus releases,
package-v2 trees, and recording capsules without changing their identities.

The catalog should never turn `latest` into `active`. Authority continues to
come from validated acceptance receipts and scoped active pointers.

## Target product lifecycle

```text
planned recipe
    │
    ▼
captured evidence session ──failed/aborted──► preserved diagnostic evidence
    │
    ▼
derived product
    │
    ▼
sealed immutable product
    │
    ├──rejected──► immutable rejection receipt
    │
    ▼
immutable acceptance receipt
    │
    ▼
atomic scope-specific runtime selection
    │
    ▼
recording-local deep materialization + H5 mirror
```

Operator confirmation can occur between derivation and sealing, depending on
the product. It must not be conflated with authority acceptance or runtime
selection.

## Target logical storage model

The following is a desired logical model. It need not be imposed on existing
paths immediately:

```text
calibrations/
  sessions/<session-id>/
    session.json
    captures/<capture-id>/...
    derived/<product-kind>/<content-id>/...

  rigs/<rig-id>/<rig-geometry-revision>/
    products/<product-kind>/<content-id>/
      package.json
      inventory.json
      members/...
    acceptance/<product-kind>/<receipt-id>.json
    active/<scope-key>.json

recordings/<recording-id>/
  calibration_capsule/
    package.json
    inventory.json
    members/...
```

Sessions preserve evidence. Stable rig products preserve commissioning and
characterization. Daily products preserve rim and translation registration.
Recordings deep-copy only the exact participating subset.

## Migration plan

### Phase 0: correct contracts and freeze vocabulary

- Fix the live-mask recording-schema contradiction. Completed 2026-07-26 with
  mode-dependent recording-local validation.
- Define the shared lifecycle terms in this document as schema vocabulary.
- Define the scope key for every current product.
- Build a canonical calibration product/status index document.
- Mark legacy, diagnostic, superseded, candidate-only, implemented, and active
  documents explicitly.

This phase should not move artifacts or change calibration math.

### Phase 1: introduce the global transaction lease

- [x] Add a process-level calibration transaction state.
- [x] Include Spatial grouped/direct capture, manual preflight, daily
  registration, guided autorun, arena centering, aperture, FOV, and USAF.
- [x] Disable conflicting camera, stream, recording, local-control, and
  calibration-authority mutations at the known GUI/orchestration entry points.
- [x] Reserve the same coordinator during asynchronous recording activation so
  calibration cannot begin before `record_video` becomes active.
- [x] Declare owner-allowed mutations and exact nested-parent identity rather
  than applying one blanket lock.
- [ ] Persist live pre-mutation state and restoration progress in one durable
  transaction journal.
- [x] Add focused tests for conflict rejection, scope/permission enforcement,
  terminal status, and RAII release.
- [ ] Add startup recovery for an abandoned persisted transaction and extend
  the ownership contract to independent headless processes.

This is the first architecture implementation slice.

### Phase 2: centralize recipe definitions

- Define a versioned checked-in recipe schema.
- Represent existing Citrus scenes, workflow profiles, physical checkpoints,
  settling, capture policies, and output products.
- Make GUI, CLI, and automation list recipes from the same registry.
- Preserve an Advanced/Custom path with explicit override provenance.
- Replace the production use of `smoke` naming with `capture` or
  `commissioning`, retaining compatibility aliases if useful.

### Phase 3: introduce common frame evidence

- Extend the active-stream snapshot path to emit complete evidence bundles.
- Add true multi-camera barrier/target-epoch capture where required.
- Separate raw/native payloads, derived gray images, averages, and UI previews.
- Record every averaging member and cadence/skew check.
- Add product-specific source requirements, such as Mono12 for photon-transfer
  characterization.

### Phase 4: migrate USAF/FOV first

- Keep the existing USAF analysis semantics initially.
- Replace latest-preview-buffer capture with identified evidence capture.
- Preserve raw frames, frame IDs, timestamps, optical state, target identity,
  and ROI overlays.
- Move FOV/ruler evidence into the same optical-target acquisition shell.
- Only then add slanted-edge MTF and DOF recipes/analyzers.

This avoids creating a fourth standalone optical-acquisition architecture.

### Phase 5: share artifact sealing and scoped authority

- Extract the package-v2 publisher into shared infrastructure.
- Publish immutable individual captures and products.
- Convert mutable image sets into indexes over immutable members.
- Add immutable acceptance/rejection receipts.
- Add scope-aware atomic pointers and rollback.
- Deep-materialize recording capsules and mirror one composition identity into
  H5.

### Phase 6: reorganize the GUI around operator intent

- Add the Calibration & Commissioning home.
- Split daily setup from stable commissioning and canonical layout editing.
- Add a shared physical-checkpoint and applied-state header.
- Generate workflow checklists/matrices from recipes.
- Add the unified visual evidence/authority browser.
- Move raw schema editing and manual fit tuning under Advanced.
- Relabel legacy dataset/Indigo capture.

The UI can initially call the existing implementations. Internal migration can
continue after the operator model is coherent.

### Phase 7: retire duplicate paths carefully

- Remove duplicate recipe lists only after all callers use the registry.
- Remove duplicate registry writers only after the shared publisher is proven.
- Keep backwards-compatible readers for existing session and Citrus layouts.
- Do not rewrite immutable historical evidence merely to match the new layout.
- Provide explicit migration reports where a stable product is repackaged.

## Immediate implementation checklist

Before implementing new MTF/DOF acquisition:

- [x] Correct `active_in_orange_live_detection_pipeline` schema semantics: the
      recording-local value is now constrained by the explicitly armed mode.
- [x] Define and test the in-process global calibration transaction lease.
- [x] Add Spatial grouped/direct capture, preflight, daily registration, guided
      commissioning, arena centering, aperture/FOV, and USAF to that lease.
- [x] Guard known camera mutations, generic stream changes, recording start,
      and unrelated Citrus authority mutations during the lease.
- [ ] Persist the transaction's pre-mutation/restoration journal and add
      abandoned-transaction startup recovery.
- [ ] Define the first versioned recipe contract.
- [ ] Move current scene/profile/settle mappings behind one registry API.
- [ ] Define the `FrameEvidenceBundle` contract.
- [ ] Choose the authoritative source representation required by USAF, MTF, and
      DOF.
- [ ] Migrate USAF capture from latest-preview copying to identified source
      evidence.
- [ ] Add a calibration documentation/product status index.

The complete artifact/catalog reorganization can proceed afterward; it should
not block the small safety and evidence foundation above.

## Acceptance criteria for the reorganized stack

The architecture should be considered coherent when:

1. only one calibration transaction can own a conflicting camera or Citrus
   scene at a time;
2. recording/stream/camera mutations are rejected with a clear owner and
   reason while a lease is active;
3. every structured capture records one versioned recipe and one complete
   frame-evidence bundle;
4. the UI and CLI enumerate the same recipes and lifecycle states;
5. raw evidence, derived previews, derived calibration products, acceptance,
   and runtime selection are distinct objects;
6. all new products use artifact-atomic staged publication and SHA-256 exact-byte
   inventory;
7. active lookup is scoped by rig revision, camera, arena, target plane, and
   optical operating point as applicable;
8. a unified browser can explain the current active authority and its evidence;
9. each recording deep-copies the exact selected calibration subset and Citrus
   H5 mirrors the same composition identity; and
10. optional characterization remains optional, while explicitly selected
    calibration-dependent modes fail closed when invalid.

## Decision for upcoming structured optical acquisitions

The planned USAF, slanted-edge MTF, and Edmund #54-440 DOF work should share one
structured optical-target acquisition shell. They should remain distinct
analysis products because they answer different questions:

- USAF: limiting-resolution interpretation at selected field positions;
- slanted edge: calculated edge-spread/MTF behavior; and
- #54-440: depth-dependent resolved contrast over a known inclined target.

The common shell should own camera selection, target identity, physical plane,
production optical state, live-frame evidence, positioning labels, capture,
and overlays. Each analyzer then consumes the same trustworthy evidence without
opening or mutating a camera itself.

Therefore the next work should be the acquisition/lifecycle foundation, followed
by USAF migration. Adding DOF directly to the current standalone USAF preview
worker would deepen the fragmentation identified in this audit.

## Final assessment

Orange is not missing calibration capability. It is missing one organizing
layer over capabilities that were built at different times for different
purposes.

The strongest path forward is to compose the best existing primitives:

- aperture's mutation/restoration discipline;
- Spatial Layout's active-stream frame leases and Citrus fencing;
- sensor baseline's raw/cadence/PTP validation;
- scale and homography's explicit review/promotion lifecycle;
- package-v2's exact-byte sealed publication; and
- recording geometry's scoped materialization and H5 handoff.

That approach is safer and smaller than rewriting the calibration stack, and it
creates the right foundation for the next generation of optical
characterization.
