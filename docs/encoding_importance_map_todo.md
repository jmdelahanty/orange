# Recording Importance Map TODO

Date: 2026-03-18
Status: target design, not implemented

Scope: plan a full-frame recording quality-prioritization system that can steer
NVENC bit allocation toward the fish without requiring TensorRT.

Note:
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

- [ ] Implement a dish-prior-only prototype before motion or TRT-driven signals.
- [ ] Cache one static dish-prior map per camera / recording session and reuse
  it for every frame.
- [ ] Build `static_dish_prior` from the dish mask.
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

- [ ] Start NVENC map integration with static dish-prior input only.
- [ ] Convert importance sources into a codec-ready map primitive.
- [ ] Prefer HEVC delta-QP map path for full-frame recording.
- [ ] Start with mild default deltas and validate visually plus downstream-task
  quality.
- [ ] Keep the feature opt-in.
- [ ] Benchmark future map-guided encoding against generic `AQ/TemporalAQ`, not
  just against neutral encoding.
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
