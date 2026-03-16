# Calibration + 3D Parity TODO (orange -> orange-jeremy)

Date: 2026-02-24
Scope: bring `orange-jeremy` to parity with `orange` for calibration persistence, calibration loading, and runtime 3D triangulation/projection.

## Context

`orange` currently has end-to-end runtime support for:
- loading per-camera calibration files (`camera_matrix`, `distortion_coefficients`, `tc_ext`, `rc_ext`),
- triangulating 3D points from multi-camera 2D detections,
- reprojecting 3D results back to camera views.

Refs:
- `/home/jeremy/orange/src/realtime_tool.h:15`
- `/home/jeremy/orange/src/realtime_tool.cpp:80`
- `/home/jeremy/orange/src/realtime_tool.cpp:189`
- `/home/jeremy/orange/src/detect3d.cpp:16`
- `/home/jeremy/orange/src/gui.h:88`

`orange-jeremy` currently has:
- calibration pose state machine + image capture flow,
- no intrinsic/extrinsic calibration data model,
- no runtime calibration loader,
- no 3D triangulation module.

Refs:
- `/home/jeremy/orange-jeremy/src/global.h:16`
- `/home/jeremy/orange-jeremy/src/orange.cpp:570`
- `/home/jeremy/orange-jeremy/src/acquire_frames.cpp:466`
- `/home/jeremy/orange-jeremy/src/project.cpp:64`

## Audit Update (2026-03-16)

- Re-checked current repo state:
  - calibration image-capture flow is still present in the GUI,
  - calibration directory creation still exists in project startup,
  - async image writing exists via `ImageWriterWorker`,
  - CMake already links OpenCV SFM.
- The core parity gap remains unchanged:
  - no calibration artifact schema/loader in `src/`,
  - no runtime 3D join/triangulation module,
  - no reprojection overlay path driven by persisted calibration data.
- The related `docs/3d_ptp_coupling_contract.md` remains a contract-only document for now and still depends on unresolved PTP hardening work.

## Goal

Support these capabilities in `orange-jeremy`:
1. Persist and load camera intrinsics/extrinsics.
2. Validate calibration health at startup and stream start.
3. Run real-time 3D triangulation from multi-camera detections.
4. Reproject 3D detections onto per-camera views.
5. Keep current image-capture flow for calibration data acquisition.

## Phase 0: Baseline and Design Decisions

- [ ] Adopt and lock the 3D/PTP runtime coupling contract:
  - `docs/3d_ptp_coupling_contract.md`
  - confirm join key policy, skew budget, timeout policy, and drop reasons.
- [ ] Decide canonical calibration artifact format:
  - Option A: keep `Cam<serial>.yaml` compatibility with `orange`.
  - Option B: define JSON schema and provide one-way importer from YAML.
- [ ] Define calibration root in `orange-jeremy` data tree (for example `orange_data/calib_yaml`).
- [ ] Define camera-selection policy for 3D (which cameras participate, minimum 2).
- [ ] Define versioned schema for calibration artifacts.

## Phase 1: Calibration Data Model and Storage

- [ ] Add calibration structs/classes in `src/` for:
  - intrinsics (`K`, distortion),
  - extrinsics (`R`, `rvec`, `tvec`),
  - projection matrix (`P`),
  - metadata (serial, width/height, timestamp, schema version).
- [ ] Add load/save APIs with strict validation:
  - matrix shape/type checks,
  - serial and resolution consistency checks,
  - clear failure reason strings.
- [ ] Add startup folder creation for calibration artifacts if missing.
- [ ] Add config pointer/reference from per-camera settings to calibration artifact path.

## Phase 2: Capture Workflow Hardening for Calibration Datasets

- [ ] Keep existing pose-driven capture flow, but add run metadata:
  - session id,
  - pose index,
  - selected cameras,
  - timestamp.
- [ ] Persist a manifest in each calibration capture folder listing files by camera and pose.
- [ ] Add capture completeness checks:
  - reject "next pose" if selected cameras have not all written frames.
- [ ] Add explicit UI status for calibration capture progress and missing cameras.
- [ ] Make calibration image save path non-blocking for acquisition threads:
  - move all disk I/O off camera/acquisition threads,
  - use non-blocking enqueue (`try_enqueue`) from acquisition path,
  - define overflow policy (drop oldest/newest + explicit warning counter) instead of spin-waiting.
- [ ] Ensure save jobs contain fully populated immutable payload (path, image buffer, dimensions, format) before enqueue.
- [ ] Track per-camera save completion and only advance to next pose when all required saves finish (or fail with explicit reason).
- [ ] Add save pipeline telemetry:
  - queue depth,
  - enqueue drops/timeouts,
  - write latency (P50/P95/P99),
  - end-to-end pose capture completion time.

Current capture refs:
- `/home/jeremy/orange-jeremy/src/orange.cpp:542`
- `/home/jeremy/orange-jeremy/src/orange.cpp:576`
- `/home/jeremy/orange-jeremy/src/image_writer_worker.cpp:28`

## Phase 3: Offline Calibration Tooling

- [ ] Add scripts/tools to solve calibration from captured images:
  - intrinsic calibration per camera,
  - extrinsic/multi-camera solve,
  - reprojection error report.
- [ ] Output directly in runtime-consumable artifact format.
- [ ] Add CLI commands and docs for:
  - input folder conventions,
  - board/marker definitions,
  - expected outputs.
- [ ] Add compatibility importer to reuse existing `orange` calibration YAML files.

## Phase 4: Runtime Calibration Loading in orange-jeremy

- [ ] On camera-open or stream-start, load calibration for each active camera.
- [ ] Track `has_calibration_results` per camera and expose in UI.
- [ ] Fail policy:
  - stream allowed without calibration for 2D-only modes,
  - 3D mode requires valid calibration on participating cameras.
- [ ] Add diagnostics panel/log lines showing calibration status and source path.

## Phase 5: 3D Runtime Module

- [ ] Add a dedicated runtime module (for example `detect3d.*`) to:
  - collect synchronized 2D detections from participating cameras,
  - triangulate 3D points,
  - publish reprojections per camera.
- [ ] Implement join behavior exactly per `docs/3d_ptp_coupling_contract.md`:
  - enforce session-id consistency,
  - enforce join timeout and skew budget,
  - emit typed drop reasons when joins fail.
- [ ] Use OpenCV triangulation/projection path equivalent to `orange`:
  - undistort points,
  - triangulate from projection matrices,
  - `projectPoints` for overlays.
- [ ] Integrate with current worker pipeline:
  - source detections from YOLO stage outputs,
  - avoid blocking acquisition path,
  - preserve clean shutdown ordering.
- [ ] Gate 3D processing with explicit readiness synchronization and timeouts.
- [ ] Include join provenance metadata in every 3D output:
  - source camera list/frame ids,
  - join key used,
  - observed skew summary.

## Phase 6: UI + Network Integration

- [ ] Add UI controls for:
  - enable/disable 3D mode,
  - choose participating cameras,
  - show calibration/triangulation health.
- [ ] Add optional network payloads for 3D outputs if needed by downstream consumers.
- [ ] Keep existing INDIGO calibration signal flow backward-compatible.

Relevant signal refs:
- `/home/jeremy/orange-jeremy/schema/fetch.fbs:12`
- `/home/jeremy/orange-jeremy/src/enet_thread.cpp:99`
- `/home/jeremy/orange-jeremy/src/network_base.cpp:128`

## Phase 7: Validation and Tests

- [ ] Unit tests:
  - calibration file parse/validate,
  - matrix conversion correctness,
  - projection/triangulation math sanity checks.
- [ ] Integration tests:
  - missing calibration file,
  - wrong resolution calibration,
  - one camera dropped in 3D mode,
  - stale/invalid extrinsics.
- [ ] Dataset regression checks:
  - compare 3D outputs vs known reference captures,
  - enforce reprojection-error thresholds.
- [ ] Performance checks:
  - measure added latency from 3D module,
  - verify no startup/shutdown regressions.

## Definition of Done

- [ ] `orange-jeremy` can load persisted intrinsics/extrinsics for active cameras.
- [ ] 3D mode triangulates from at least two cameras and projects back into views.
- [ ] Calibration capture sessions produce manifest + solver-ready image sets.
- [ ] Missing/invalid calibration is surfaced clearly and handled by policy.
- [ ] End-to-end calibration + 3D workflow is documented and reproducible.
