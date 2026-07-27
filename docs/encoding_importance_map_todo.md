# Recording Importance Map TODO

Date: 2026-03-18
Updated: 2026-07-24
Status: static daily-dish prior implemented for the external recorder; dynamic
YOLO ROI remains planned

Scope: plan a full-frame recording quality-prioritization system that can steer
NVENC bit allocation toward the fish without requiring TensorRT.

Current runtime status:
- headless recording now supports an opt-in `importance_map_mode=static_roi`
  smoke path that emits a synthetic centered square delta-QP map
- the square size is controlled by `importance_map_roi_size_px` and currently
  defaults to `512`
- this v1 path is for plumbing validation only and is not yet dish-geometry
  driven
- the production external full-frame recorder now supports an opt-in
  `static_dish_prior` delta-QP policy
- the GUI option is disabled by default and resolves the exact selected daily
  registration's accepted camera-pixel circle at recording arm
- Orange binds the recording-local rim artifact path, checksum, and fingerprint
  into each recorder stream before the recorder process starts
- the external recorder builds one immutable codec-native map per shard and
  attaches it to every NVENC submission path
- the external recorder accepts `vbr_cq` and uses the shared Orange video
  profile to configure NVENC VBR `targetQuality`, the requested average
  bitrate, maximum bitrate ceiling, and VBV while keeping AQ, temporal AQ, and
  lookahead disabled
- recorder summaries preserve the requested mode, resolved strategy,
  target-quality activation, bitrate/VBV limits, and QP-map provenance
- motion ROI, arena priors, and detector-informed maps remain planned work

Note:
- For the review of Palette's July 24 noise-floor, fish-edge, range, and encoder
  characterization claims, including arithmetic corrections and the required
  paired validation, see
  `docs/palette_rig_characterization_review_2026_07_24.md`.
- For the newer draft on canonical `layout_id` / `zone_id`, per-recording
  registration, and resolved camera-pixel overlays for Citrus, see
  `docs/spatial_layout_contract.md`.
- For the field-level schema definition, see `docs/spatial_layout_schema.md`.
- For the concrete codec-comparison workflow, see
  `docs/codec_quality_evaluation_protocol.md`.
- For the planned pre-compression reference-capture slice, see
  `docs/pre_encoder_reference_capture_plan.md`.

## Goals

- Keep full-frame recording usable without forcing crop-only or lossless-crop
  workflows.
- Make TensorRT an optional quality-enhancement signal, not a prerequisite.
- Standardize dish-mask discovery and metadata so Orange, Citrus, and future
  consumers use the same geometry identity.
- Support both single-dish and multi-arena camera views under one calibration
  model.
- Reuse one dish-mask schema across autofocus, recording-quality priors, and
  downstream experiment metadata.

## Non-Goals

- Exact NVENC implementation details in this document.
- Replacing crop streams or lossless crops as optional outputs.
- Final consumer-side UI behavior in Citrus.

## Design Position

- First shared contracts to define are:
  - `dish_mask` for the outer valid region
  - `arena_layout` for zero or more sub-arenas inside that region
- Dish mask remains the stable outer spatial prior for all non-TRT fallback
  behavior.
- Arena layout is the stable inner spatial prior for multi-arena views.
- Consumer-visible identity should be a calibration artifact reference, not a
  guessed config path.
- Runtime encoding should accept multiple importance sources, ordered:
  1. detector/tracker bbox when available
  2. motion/background-subtraction ROI constrained by dish mask and arena layout
  3. static arena prior
  4. static dish prior
  5. neutral full-frame map

## Latency Position

Spatial encoding guidance should not be treated as a look-ahead feature by
default.

Working assumption:

- a per-frame importance map or delta-QP map should not inherently add fixed
  encoder latency the way look-ahead does,
- the bigger latency risks are upstream map generation and downstream throughput
  loss,
- therefore the feature should be designed so that it can run from current or
  past-frame signals only, without waiting on future frames.

Practical implications:

- safe-ish latency profile:
  - static dish prior
  - static arena prior
  - cheap motion ROI
  - detector/tracker bbox when already available in time
  - reuse last-good map when the newest signal is missing
- risky latency profile:
  - blocking encode on same-frame TRT if TRT is not already on budget
  - temporal smoothing that requires future frames
  - enabling quality modes that already queue frames internally, such as
    look-ahead
  - any importance-map path that makes encode throughput fall below input rate

Design rule:

- importance-map generation should degrade to stale or simpler maps rather than
  blocking the encode path,
- future-frame dependence should be treated as a separate feature and justified
  explicitly,
- validation should distinguish:
  - direct added frame latency,
  - throughput loss that causes queue growth,
  - and subjective/objective quality gain.

This is consistent with the current NVENC reading:

- NVIDIA explicitly documents look-ahead as buffering future frames and queuing
  input,
- the planned importance-map path should be treated instead as per-picture
  guidance unless benchmarking shows a real throughput penalty large enough to
  create backlog.

## Recommended First Slice

The first runtime prototype should be intentionally boring:

- one static dish-prior map per camera
- built once from validated dish-mask geometry
- cached and reused for every frame in the recording session
- mild deltas only
- opt-in only
- no same-frame TRT dependency
- no encode-path blocking when newer signals are unavailable

This is a coarse prior, not the final fish-aware solution.

It should be treated as:

- the lowest-risk way to validate map plumbing end to end,
- a way to bias quality toward the biologically relevant arena region before
  adding motion or detector inputs,
- and a baseline against which later motion-aware or bbox-aware maps can be
  judged.

For single-dish views, this will not isolate the zebrafish itself.
It will only distinguish:

- "inside the valid arena"
- versus "background / out-of-dish region"

That is still useful because it can stop spending bits outside the dish, while
keeping latency and implementation risk low.

## Block Size Selection Notes

How people choose an importance-map block size is usually constrained in this
order:

1. encoder/API-supported map granularity
2. ROI scale
3. stability vs precision tradeoff
4. map-generation cost and implementation complexity

Documented facts relevant to the current NVENC path:

- NVIDIA's NVENC API defines `qpDeltaMap` at codec block granularity, not
  per-pixel:
  - H.264: one value per macroblock
  - HEVC: one value per CTB
- In the SDK header currently used by Orange, `NV_ENC_CONFIG_HEVC::maxCUSize`
  is documented as only supporting `NV_ENC_HEVC_CUSIZE_32x32` in the current
  NVENC SDK path.
- NVIDIA's Jetson encoder docs also describe H.265 motion/block data on a
  `32x32` coded-tree-block grid, which is consistent with treating a coarse
  `32x32` grid as the encoder-native control surface for this first slice.

Practical inference for Orange:

- For a static dish prior, start on the encoder-native block grid rather than
  inventing a finer synthetic grid.
- A whole-dish prior is spatially coarse, so `32x32` blocks are usually an
  acceptable first validation surface.
- Smaller regions, such as individual fish bodies, may later require a finer
  or more adaptive strategy, but that should only be pursued if:
  - the encoder path actually supports it,
  - and benchmarking shows the coarser grid is visibly insufficient.

Why not chase smaller blocks immediately:

- Finer maps increase spatial precision, but also increase the risk of hard
  ROI boundaries, temporal pumping, and noisy per-frame map changes.
- A coarse static prior is intentionally "boring" and is less likely to create
  artifacts while the plumbing is still being validated.
- For the current goal, proving that NVENC accepts and applies a stable
  block-level delta-QP map is more important than maximizing spatial precision.

Current Orange position:

- `static_roi` uses the current HEVC/NVENC block grid (`32x32` CTBs).
- That choice should be treated as "encoder-native and low-risk", not as a
  final claim that `32x32` is always best for fish-aware ROI encoding.
- Future dish-geometry and fish-aware ROI work should benchmark quality and
  stability before changing map granularity.

Sources:

- NVIDIA NVENC Video Encoder API Programming Guide:
  https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvenc-video-encoder-api-prog-guide/index.html
  - tuning/preset guidance (`P1` highest performance to `P7` lowest
    performance) and rate-control behavior
- NVIDIA Jetson encoder API reference (`MVInfo_`):
  https://docs.nvidia.com/jetson/archives/r38.2/ApiReference/structMVInfo__.html
  - H.264 motion vectors per `16x16` block and H.265 per `32x32` CTB
- Local SDK header used by Orange:
  - `/usr/local/include/ffnvcodec/nvEncodeAPI.h`
  - `qpDeltaMap` is per MB for H.264 and per CTB for HEVC
  - current HEVC `maxCUSize` note says the SDK path only supports
    `NV_ENC_HEVC_CUSIZE_32x32`

## Encoder-Speed Position

Importance maps should not be assumed to make NVENC materially "easier" in the
motion-estimation sense.

Working assumption:

- motion estimation, mode decisions, and reference selection still happen,
- a delta-QP or emphasis map mainly biases rate-distortion / quantization
  choices and bit allocation,
- therefore the map itself is more likely to be throughput-neutral or slightly
  slower than to produce an intrinsic encoder-speed win.

So any net speed or latency gain is more likely to come from system design
choices around the map than from the map primitive itself.

Most plausible win paths are:

- the map is static or cheaply reusable, so map generation cost is near zero,
- the map lets us compare against or replace generic `AQ` / `TemporalAQ`
  behavior that may be spending bits in the wrong places,
- the map allows simpler upstream logic than same-frame TRT or expensive motion
  analysis,
- the map works without enabling look-ahead.

Design implication:

- do not sell this feature to ourselves as "fewer motion-vector decisions,"
- instead treat it as "better bit-allocation guidance for a small important ROI
  inside a mostly static frame."

Benchmarking should separate:

- encoder throughput / utilization,
- queue growth and end-to-end latency,
- bitrate / file size,
- and fish-detail retention or downstream-task quality.

## Why Dish Mask First

The dish mask is the lowest-risk signal in this system:

- it does not require TRT,
- it is stable across frames,
- it constrains motion false positives to the arena,
- it is useful to autofocus and encoding alike,
- Citrus can attach it to experiment metadata once and reuse it downstream.

Without a stable dish mask, every later fallback becomes noisier and harder to
reason about.

Multi-arena views do not remove the need for a dish mask. They add a second
calibration layer on top of it.

## Single-Dish vs Multi-Arena

Keep these concepts separate:

- `dish_mask`: what part of the frame is valid arena-bearing space at all
- `arena_layout`: one or more named sub-arenas inside that valid region

Rules:

- Single-dish/single-fish views may use only `dish_mask`.
- Multi-arena views should use both `dish_mask` and `arena_layout`.
- Do not overload `dish_mask` to mean both outer region and sub-arena list.
- Runtime importance maps may contain multiple disconnected protected regions.

## Reuse Existing Dish-Mask Geometry

Do not invent a second dish-mask geometry schema.

Use the same core fields already proposed in
`docs/fish_autofocus_todo.md`:

- `camera_serial`
- `width`
- `height`
- `cx`
- `cy`
- `r`
- `rim_margin_px`
- `r_valid`
- `calibration_timestamp`
- `source`

For v1, standardize on circular dishes only.

Future extension may add:

- `geometry.type = polygon`
- arbitrary raster masks
- multiple valid regions per frame

But v1 should stay circle-only so Orange and Citrus can implement it cheaply and
reliably.

## Arena-Layout Artifact (Planned)

Use a second calibration artifact when a camera view contains multiple stable
sub-arenas.

Proposed artifact schema id:

```text
orange.calibration.arena_layout
```

Planned payload shape:

```json
{
  "schema_id": "orange.calibration.arena_layout",
  "schema_version": 1,
  "artifact_id": "arenalayout_2026_03_30_...",
  "created_utc": "2026-03-30T12:00:00Z",
  "calibration_ref": {
    "artifact_id": "arenalayout_2026_03_30_...",
    "artifact_schema_id": "orange.calibration.arena_layout",
    "artifact_schema_version": 1,
    "fingerprint": "fnv1a64:..."
  },
  "camera": {
    "serial": "2010093",
    "width": 4512,
    "height": 4512,
    "pixel_format": "Mono8"
  },
  "layout": {
    "coordinate_space": "camera_native_pixels",
    "arenas": [
      {
        "arena_id": "a0",
        "geometry": {"type": "circle", "cx": 1120.0, "cy": 1120.0, "r": 420.0},
        "context": {"dish_design_id": "dish4_v1", "arena_index": 0}
      },
      {
        "arena_id": "a1",
        "geometry": {"type": "circle", "cx": 3392.0, "cy": 1120.0, "r": 420.0},
        "context": {"dish_design_id": "dish4_v1", "arena_index": 1}
      },
      {
        "arena_id": "a2",
        "geometry": {"type": "circle", "cx": 1120.0, "cy": 3392.0, "r": 420.0},
        "context": {"dish_design_id": "dish4_v1", "arena_index": 2}
      },
      {
        "arena_id": "a3",
        "geometry": {"type": "circle", "cx": 3392.0, "cy": 3392.0, "r": 420.0},
        "context": {"dish_design_id": "dish4_v1", "arena_index": 3}
      }
    ]
  },
  "provenance": {
    "source": "manual",
    "source_image_kind": "empty_dish_frame"
  }
}
```

Notes:

- `arena_id` must be stable within the layout artifact.
- `arena_index` is convenience metadata, not identity.
- v1 should allow disconnected arenas and should not assume only one fish per
  full camera frame.
- A single camera may therefore map to several independent “important regions”
  in the final encoding map.

## Canonical Consumer-Facing Identity

Use a calibration artifact reference as the canonical identity, following
`docs/calibration_artifact_contract.md`.

Proposed artifact schema id:

```text
orange.calibration.dish_mask
```

Proposed calibration-ref shape:

```json
{
  "artifact_id": "dishmask_...",
  "artifact_schema_id": "orange.calibration.dish_mask",
  "artifact_schema_version": 1,
  "fingerprint": "fnv1a64:..."
}
```

Rationale:

- Orange can still support local sidecar convenience files.
- Citrus should not guess `config/dish_masks/...` or parse camera config files
  directly.
- Citrus can resolve a stable `artifact_id` through the artifact registry and
  copy the same ref into its own metadata.

## Dish-Mask Artifact Payload (Planned)

Planned payload shape for the dish-mask artifact:

```json
{
  "schema_id": "orange.calibration.dish_mask",
  "schema_version": 1,
  "artifact_id": "dishmask_2026_03_18_...",
  "created_utc": "2026-03-18T12:00:00Z",
  "calibration_ref": {
    "artifact_id": "dishmask_2026_03_18_...",
    "artifact_schema_id": "orange.calibration.dish_mask",
    "artifact_schema_version": 1,
    "fingerprint": "fnv1a64:..."
  },
  "camera": {
    "serial": "2010093",
    "width": 4512,
    "height": 4512,
    "pixel_format": "Mono8"
  },
  "geometry": {
    "type": "circle",
    "coordinate_space": "camera_native_pixels",
    "cx": 2254.0,
    "cy": 2256.0,
    "r": 2060.0,
    "rim_margin_px": 140.0,
    "r_valid": 1920.0
  },
  "provenance": {
    "source": "manual",
    "source_image_kind": "empty_dish_frame"
  },
  "context": {
    "dish_design_id": "dish_v1",
    "canvas_id": "canvas_a",
    "shelf_id": "shelf_2"
  }
}
```

Notes:

- `coordinate_space` is critical. Citrus and Orange must know the geometry is
  expressed in the native camera pixel frame.
- `context` fields are optional but useful for downstream grouping and training
  metadata.
- `r_valid` is the actual safe region for runtime use. `r` is retained for
  provenance and visualization.

## Recording Snapshot Integration (Planned)

Add a new top-level `calibrations` block to `recording_snapshot.json`.

Planned shape:

```json
{
  "calibrations": {
    "2010093": {
      "dish_mask": {
        "calibration_ref": {
          "artifact_id": "dishmask_...",
          "artifact_schema_id": "orange.calibration.dish_mask",
          "artifact_schema_version": 1,
          "fingerprint": "fnv1a64:..."
        },
        "runtime": {
          "enabled": true,
          "width": 4512,
          "height": 4512,
          "geometry": {
            "type": "circle",
            "coordinate_space": "camera_native_pixels",
            "cx": 2254.0,
            "cy": 2256.0,
            "r": 2060.0,
            "rim_margin_px": 140.0,
            "r_valid": 1920.0
          },
          "source": "manual"
        }
      },
      "arena_layout": {
        "calibration_ref": {
          "artifact_id": "arenalayout_...",
          "artifact_schema_id": "orange.calibration.arena_layout",
          "artifact_schema_version": 1,
          "fingerprint": "fnv1a64:..."
        },
        "runtime": {
          "enabled": true,
          "coordinate_space": "camera_native_pixels",
          "arenas": [
            {"arena_id": "a0", "geometry": {"type": "circle", "cx": 1120.0, "cy": 1120.0, "r": 420.0}},
            {"arena_id": "a1", "geometry": {"type": "circle", "cx": 3392.0, "cy": 1120.0, "r": 420.0}},
            {"arena_id": "a2", "geometry": {"type": "circle", "cx": 1120.0, "cy": 3392.0, "r": 420.0}},
            {"arena_id": "a3", "geometry": {"type": "circle", "cx": 3392.0, "cy": 3392.0, "r": 420.0}}
          ],
          "source": "manual"
        }
      }
    }
  }
}
```

Rules:

- `calibration_ref` is the identity.
- `runtime` is the resolved geometry Orange actually used for the run.
- Citrus should copy the same `calibration_ref` into its own metadata rather
  than inventing a second identifier.
- Citrus may also copy the resolved runtime geometry for convenience, but the
  reference remains authoritative.
- `dish_mask` and `arena_layout` are independent calibration entries. A camera
  may emit one, both, or neither.

## Citrus Consumer Rule

Citrus should resolve spatial-calibration identity in this order:

1. Read `recording_snapshot.json`.
2. Read `calibrations[serial].dish_mask.calibration_ref` if present.
3. Read `calibrations[serial].arena_layout.calibration_ref` if present.
4. Resolve each artifact through the Orange calibration artifact registry.
5. Copy the same `calibration_ref` values into Citrus experiment metadata.
6. Optionally also copy the resolved runtime geometry blocks used by Orange.

Citrus should not:

- guess local sidecar file paths,
- parse Orange camera config files for dish mask,
- create a new dish-mask identifier when a calibration ref already exists.

## How Orange Should Find the Dish Mask

### Phase 1 Recommendation

Use manual calibration from an empty-dish frame.

Store:

- `cx`
- `cy`
- `r`
- `rim_margin_px`
- derived `r_valid`

This is the cheapest path to a stable cross-tool mask.

### Phase 2 Recommendation

Add optional semi-automatic or automatic dish-circle fitting:

- edge detect on empty-dish frame,
- circle fit / Hough circle,
- overlay preview and operator accept/reject,
- save the accepted result as the same artifact schema.

### Validation Rules

Before Orange uses the mask at runtime:

- `camera_serial` must match active camera
- `width` and `height` must match active stream dimensions
- `cx` and `cy` must be in-bounds
- `r_valid` must be positive and less than `r`
- circle must plausibly fit inside the frame

If validation fails:

- disable dish-mask use,
- emit a clear warning,
- fall back to neutral map or static-default encoding behavior.

## How Orange Should Find the Arena Layout

### Phase 1 Recommendation

Use manual calibration for stable sub-arena geometry.

Store, per arena:

- `arena_id`
- geometry
- optional design/context fields

For v1, use stable geometry only. Do not try to infer arena occupancy or fish
presence from the layout artifact itself.

### Validation Rules

Before Orange uses the layout at runtime:

- `camera_serial` must match active camera
- `width` and `height` must match active stream dimensions
- all `arena_id` values must be unique
- all arena geometries must be in-bounds
- arenas should either lie inside `dish_mask` or explicitly declare that no
  outer dish mask applies

If validation fails:

- disable arena-layout use,
- emit a clear warning,
- continue using `dish_mask` or neutral behavior if available.

## Runtime Importance Source Plan

Planned priority order for the future `ImportanceMapBuilder`:

1. `semantic_bbox_inside_arena`
2. `motion_roi_inside_arena`
3. `static_arena_prior`
4. `static_dish_prior`
5. `neutral`

Expected behavior:

- bbox expands and smooths over time
- motion ROI is clipped to `dish_mask` and, when present, to the active
  `arena_layout`
- static arena prior may create several disconnected protected regions
- static dish prior gives mild protection even when no motion is detected
- neutral mode remains available for users who want no special behavior
- final map may contain multiple simultaneous important regions in one frame

Implementation preference for the first slice:

- start with `static_dish_prior` only,
- prove map wiring and benchmarking on that baseline,
- add motion or semantic ROI only after the static-prior path is understood.

## Dynamic YOLO ROI Plan After GUI YOLO Validation

Date: 2026-04-22

The current static ROI path is a successful NVENC plumbing test, but it is not
fish-aware. The newer GUI path has now validated one-camera `100 fps` recording
with YOLO enabled and clean one-to-one video/meta/YOLO log alignment. That makes
YOLO-derived importance maps technically plausible, but the current priority is
lossless high-resolution crop production that can feed pose TensorRT before crop
video encoding. Dynamic QP maps should remain a later codec-quality experiment.

Current code-path finding:

- Acquisition dispatches the same `WORKER_ENTRY` to display, YOLO, and
  recording according to the selected subscriptions.
- Recording ingress sends the frame toward encoder preprocess immediately.
- `YoloWorker` writes detections back onto `WORKER_ENTRY`, but encoder
  preprocess and encoder HW do not currently wait for YOLO completion.
- `ENCODER_WORKER_ENTRY` carries prepared-frame identity and timing, but no
  detection or ROI payload.
- Production full-frame encoding now runs in a separate recorder process.
- Therefore dynamic YOLO guidance needs a compact asynchronous ROI update on
  the existing bidirectional Orange/recorder control connection; it must not
  rely on sharing an in-process worker object.
- A strict same-frame YOLO ROI would still require blocking or intentional
  buffering and is not the intended first implementation.

Design rule for the first dynamic implementation:

- Do not block encode on same-frame YOLO.
- Let `YoloWorker` publish a per-camera latest-good ROI state with source frame
  identity and completion time.
- Forward that state to the external recorder as a non-blocking control update.
- Let each external encoder shard select the newest ROI whose source frame is
  not newer than the frame being encoded and compose a per-frame delta-QP map.
- If the latest ROI is stale or missing, fall back to a configured safe source:
  static ROI, static dish/arena prior, or neutral map.

This makes `yolo_roi` a "latest available semantic prior", not a strict
same-frame semantic annotation. The distinction should be explicit in metadata.

Recommended first runtime mode:

```text
importance_map_mode=yolo_roi
```

Accepted aliases can include:

```text
dynamic_roi
yolo-roi
semantic_roi
```

Initial behavior:

- Convert the best YOLO/tracker box into source-frame pixel ROI.
- Expand the ROI by a configurable margin/halo before mapping to CTBs.
- Clamp ROI to the active source frame.
- Transform source-frame ROI into encoder-output coordinates when output
  downsampling/resizing is active.
- Fill the HEVC `32x32` CTB delta-QP map:
  - inside expanded ROI: protected negative QP delta
  - optional halo: milder negative or neutral delta
  - outside ROI: positive or neutral delta, depending on rate-control test
- Reuse the last good ROI for a bounded number of frames.
- Prefer a slightly-too-large ROI over clipping the fish.

Suggested v1 policy defaults:

- `inside_delta_qp = -3`
- `outside_delta_qp = +3` for direct comparison with `static_roi`
- `margin_px = 256` in source-frame pixels
- `hold_frames = 30`
- `smoothing_alpha = 0.25`
- fallback source: `static_roi` when configured, otherwise neutral

For the daily-dish production path, the fallback should instead be the exact
armed `static_dish_prior`. The dynamic map composes regions by taking the most
protective applicable delta:

- expanded YOLO box core: strong protection
- YOLO box halo: mild protection
- accepted daily dish: static protection
- outside dish: positive QP penalty

At 100 fps, non-blocking publication will commonly make the most recent useful
box one frame old. A generous box margin, bounded hold time, and optional
tracker prediction are preferable to delaying video submission for same-frame
YOLO.

Dynamic maps cannot safely overwrite one shared byte array while NVENC may
still reference it. Use a bounded map-slot ring tied to encoder in-flight
submissions, or otherwise retain each submitted map until that submission is
retired. Static dish maps avoid this issue because their storage is immutable
for the full encoder lifetime.

Safety rules from the July 2026 consumer review:

- A missing, stale, or late detection must never remove the static dish prior.
  Dynamic YOLO boxes are an additional quality boost, not the only protection.
- The dynamic policy must never block frame submission. If no eligible ROI
  update is immediately available, encode with the immutable dish map and log
  the fallback.
- The full-frame master must remain complete even when YOLO or crop production
  fails. Dynamic guidance cannot become a recording availability dependency.
- Persist the source detection frame id, encoded frame id, ROI rectangle,
  margins, deltas, and fresh/held/fallback state for every map change (or every
  encoded frame in a compact sidecar). Spatially varying quantization is part
  of recording provenance.
- Score quality on escape/fast-motion epochs explicitly. Average-frame metrics
  can hide the exact events where a lagged ROI is least reliable.
- Run a bitrate/CQ ladder against the same pre-encoder reference before
  promoting dynamic guidance. If uniform settings preserve downstream fish
  metrics at a useful bitrate, dynamic coupling may not be justified.

The generated chaser position can be another zero-latency importance source
because Citrus knows it before rendering. It can preserve the stimulus and may
provide an anticipatory region during chase behavior, but it does not replace
the fish ROI: retreat, freezing, spontaneous motion, and detection failures can
put the animal elsewhere.

Metadata that should be emitted in `recording_snapshot.json`:

- requested mode and active mode
- block size and grid dimensions
- inside/outside/halo deltas
- source coordinate space
- ROI source: `latest_yolo_detection`
- whether same-frame ROI is guaranteed: `false`
- hold-frame and smoothing parameters
- fallback policy

Per-run diagnostics that would make validation interpretable:

- number of frames encoded with a fresh YOLO ROI
- number of frames encoded with a held ROI
- number of frames encoded with fallback/neutral map
- latest ROI age in frames and milliseconds
- ROI source frame id versus encoded recording frame id
- optional sampled ROI rectangles in a compact CSV or JSONL sidecar

Implementation checklist:

- [ ] Keep `yolo_roi` behind the high-resolution crop/pose reactivation work
      unless codec-quality evaluation becomes the immediate priority again.
- [x] Extract codec-native QP-map generation into a reusable
      `ImportanceMapBuilder` helper.
- [ ] Add unit tests for ROI-to-CTB-grid conversion, clamping, margin expansion,
      and source-to-output scaling.
- [ ] Add a per-camera `LatestYoloRoiState` published by `YoloWorker` and sent
      to the external recorder over the control protocol.
- [ ] Update `YoloWorker` to publish best detection ROI with frame ids,
      confidence, and timestamps.
- [ ] Extend `ImportanceMapConfig` with `yolo_roi` mode and v1 policy fields.
- [ ] Update headless CLI/spec parsing, run config, runs JSON/CSV, and analyzer
      fields for the new mode.
- [ ] Update the external recorder shards to compose a per-frame QP map from
      the latest ROI when `importance_map_mode=yolo_roi`.
- [ ] Preserve the existing `static_roi` path as the baseline/off-vs-on proof.
- [ ] Emit snapshot diagnostics explaining whether dynamic maps used fresh,
      held, or fallback ROI data.
- [ ] Validate with a short GUI YOLO recording and bounded pre-encoder
      reference capture before any longer run.

## TODO Plan

## Phase 1: Schema and Contracts

- [ ] Define `orange.calibration.dish_mask` artifact schema.
- [ ] Define `orange.calibration.arena_layout` artifact schema.
- [ ] Reuse the existing circular dish-mask geometry fields from
  `docs/fish_autofocus_todo.md`.
- [ ] Decide required versus optional payload fields.
- [ ] Define `recording_snapshot.json` extension:
  - [ ] top-level `calibrations`
  - [ ] per-camera `dish_mask`
  - [ ] per-camera `arena_layout`
  - [ ] `calibration_ref`
  - [ ] resolved runtime geometry block
- [ ] Define Citrus copy-through rule for spatial calibration metadata.
- [ ] Add a short compatibility note to `docs/recording_metadata.md` once the
  planned shape is accepted.

## Phase 2: Orange Dish-Mask Calibration Workflow

- [ ] Implement manual dish-circle calibration from an empty-dish frame.
- [ ] Save calibration as a dish-mask artifact package, not just an ad-hoc path.
- [ ] Write overlay preview for operator verification.
- [ ] Add explicit overwrite/update flow for recalibration.
- [ ] Add optional auto-fit path later, but keep manual calibration as the first
  supported path.
- [ ] Implement manual arena-layout calibration for multi-arena views.
- [ ] Save arena layout as a separate calibration artifact package.

## Phase 3: Orange Runtime Loading and Emission

- [ ] Load dish-mask artifact reference for each active camera.
- [ ] Load arena-layout artifact reference for each active camera when present.
- [ ] Validate artifact geometry against active stream dimensions at startup.
- [ ] Emit resolved dish-mask ref + runtime geometry into recording snapshot.
- [ ] Emit resolved arena-layout ref + runtime geometry into recording snapshot.
- [ ] Add concise startup log line summarizing mask status.
- [ ] Keep recording functional when no dish mask is available.

## Phase 4: Citrus Integration

- [ ] Parse dish-mask and arena-layout calibration refs from Orange recording snapshot.
- [ ] Resolve artifact refs through the Orange calibration artifact registry.
- [ ] Copy spatial `calibration_ref` values into Citrus experiment metadata.
- [ ] Optionally copy Orange-resolved runtime geometry into Citrus metadata for
  convenience.
- [ ] Avoid any Citrus path-guessing dependency on Orange config layout.

## Phase 5: Importance Signals Without TRT

- [x] Implement a dish-prior-only external-recorder prototype before motion or
      TRT-driven signals.
- [x] Cache one static dish-prior map per camera / recording session and reuse
  it for every frame.
- [x] Build `static_dish_prior` from the selected daily-registration accepted
      mask.
- [ ] Build `static_arena_prior` from zero or more arena geometries.
- [ ] Add low-resolution motion/background-subtraction ROI constrained by the
  dish mask and, when present, by the arena layout.
- [ ] Add temporal smoothing and stale-ROI hold.
- [ ] Treat motion fallback as a soft prior, not as hard truth.
- [ ] Prefer recall over precision for fallback ROI generation.
- [ ] Define how multiple simultaneous active arenas combine into one final map.

## Phase 6: Optional TRT Integration

- [ ] Add bbox-driven importance source when detector/tracker is available.
- [ ] Expand bbox before use.
- [ ] Fuse bbox with dish mask, arena layout, and temporal smoothing.
- [ ] Keep TRT optional and degradable to non-TRT fallback modes.

## Phase 7: NVENC Integration

- [x] Start NVENC map integration with static dish-prior input only.
- [x] Convert the daily circle into a codec-ready map primitive.
- [x] Use the HEVC delta-QP map path for external full-frame recording.
- [ ] Start with mild default deltas and validate visually plus downstream-task
  quality.
- [x] Keep the feature opt-in and disabled by default.
- [ ] Benchmark future map-guided encoding against generic `AQ/TemporalAQ`, not
  just against neutral encoding.
- [x] Support the same `vbr_cq` quality-target strategy in the external recorder
      and the in-process encoder.
- [ ] Run an identical-source VBR/VBR-CQ/CQP quality and file-size ladder before
      promoting map-guided encoding.
- [ ] Screen P3 against P1 with matched VBR 150 Mbps and VBR-CQ 20 rows at
      4512x4512 @ 100 fps. Treat P3 as a candidate only if it preserves zero
      drops and bounded recorder queues while improving fixed-bitrate quality
      and/or reducing bitrate at the fixed CQ target.
- [ ] Measure real throughput impact; do not assume map guidance reduces motion
  estimation work.

## Phase 8: Validation

- [ ] Define a short reference-capture mode for codec comparisons.
- [ ] Keep reference capture bounded by duration or frame count; do not treat
  uncompressed capture as the default long-run recording mode.
- [ ] Decide which ground-truth layer is being measured:
  - [ ] sensor-native raw / mono / Bayer frames
  - [ ] pre-encoder frames after resize / color conversion
  - [ ] decoded frames from candidate codecs
- [ ] Compare codec outputs against the matching reference layer rather than
  against an unrelated source representation.
- [ ] Use the same short clips to compare:
  - [ ] live-looking / pre-encoder detail
  - [ ] current compressed output
  - [ ] lossless codec output
  - [ ] generic `AQ/TemporalAQ`
  - [ ] static dish-prior-only map output
  - [ ] future mask-informed delta-QP / importance-map output
- [ ] Validate dish-mask artifact resolution by Orange runtime.
- [ ] Validate arena-layout artifact resolution by Orange runtime.
- [ ] Validate Citrus can resolve and copy the same calibration ref.
- [ ] Validate snapshot round-trip for one full recording session.
- [ ] Validate non-TRT mode with:
  - [ ] dish prior only
  - [ ] dish prior + static multi-arena layout
  - [ ] dish prior + motion fallback
- [ ] Validate TRT mode with bbox + fallbacks.
- [ ] Compare file size, NVENC utilization, and fish-detail quality against
  neutral encoding.
- [ ] Compare static dish-prior-only output against current `AQ/TemporalAQ`
  settings with the same clips.
- [ ] Compare downstream segmentation / keypoint / labeling quality against the
  same pre-encoder reference clips, not only against human visual judgment.

## Reference Baseline Position

For downstream segmentation / labeling / keypoint evaluation, a short
uncompressed reference clip is valuable.

Rationale:

- lossless and high-quality codec modes are still codec decisions,
- they may preserve detail very well, but they do not give a perfect "no codec
  damage" baseline,
- a bounded uncompressed reference makes it possible to measure what the codec
  itself is removing.

Important distinction:

- sensor-native raw / mono / Bayer reference answers:
  - "what information came from the camera before our local preprocessing?"
- pre-encoder reference answers:
  - "what information did the codec see before compression?"

For codec-strategy benchmarking, the pre-encoder reference is usually the most
direct comparator.

That is because it isolates codec damage from:

- debayer choices,
- resize choices,
- color-space conversion choices,
- and any future importance-map / delta-QP decisions.

Terminology clarification:

- "pre-encoder reference" here means the prepared recording-path frame
  immediately before compression,
- it does not mean every live consumer in the system sees the exact same frame
  representation.

In particular:

- the recording path sees the prepared encoder input,
- display and other live branches may tap earlier or different
  representations,
- so it is expected that a live view can preserve more visible detail than the
  saved compressed video.

That is not, by itself, evidence that acquisition lost detail.
It is often evidence that the codec path introduced the visible loss.

This makes the benchmark ladder clearer:

1. compare live-looking / pre-encoder detail against the compressed output,
2. confirm that the loss first appears at the codec boundary,
3. then compare codec strategies against the same pre-encoder baseline.

Recommended position:

- support a short explicit reference-capture mode for experiments,
- keep it bounded to small windows such as a few seconds or a fixed frame
  count,
- store enough metadata to reconstruct exact source format and preprocessing
  state,
- do not assume the right answer is a general-purpose "raw video recording
  mode" for all sessions.

Why bounded only:

- the data rate at Orange resolutions is extremely high,
- uncompressed capture can become an I/O benchmark instead of a codec
  benchmark,
- a long-run raw mode can distort the rest of the pipeline and make results
  harder to interpret.

So the intended ladder is:

1. short uncompressed reference clip,
2. same clip encoded with candidate strategies,
3. visual + downstream-task comparison against the reference,
4. only then decide which codec strategy is worth using at scale.

## Open Questions

- [ ] Should Orange support both a local sidecar convenience file and an
  artifact package, or should it normalize immediately to artifacts only?
- [ ] Should Citrus copy only `calibration_ref`, or also cache the resolved
  runtime geometry block in every experiment record?
- [ ] Do we want `dish_design_id` to be required in the dish-mask artifact
  context block, or optional for v1?
- [ ] Should a single-arena view emit only `dish_mask`, or also emit a trivial
  one-entry `arena_layout` for consistency?
- [ ] Should the first motion fallback operate on native-size mono frames or a
  dedicated low-resolution scout pass?
