# Region quality maps for recordings (design, 2026-11-03)

Status: design only, nothing built. Jeremy asked (2026-11-03) for a full-frame
region quality map designed so that following each fish's box is an option.
The goal is long-term storage: save space in the videos wherever possible
without losing what analysis needs. Background measurements (crop noise
floor, frequency analysis, compression levels) are on the "Photons, Noise and
Bits" page (https://claude.ai/artifact/ETQ3ufsfm7eZMt38oRxieN).

## Where the bytes go

Per camera, from the last two GUI recordings on Data2:

| Recording | Full frame (HEVC, rate-controlled) | Crop (lossless HEVC, all-intra) | Crop share |
| --- | --- | --- | --- |
| 2026_11_02_20_02_54, 100 Hz, 7.4 min | 2.81–2.88 GB | 3.75–4.16 GB | 59 % |
| 2026_11_01_14_19_25, 250 Hz, 26 min | 9.54–9.68 GB | 31.2–34.9 GB | 77 % |

The crops are the larger share, and they are lossless.
- Re-encoding them at QP 26 or 32 is 9× or 70× smaller (2026-11-01 measurements).
- That is the biggest storage lever. It is gated by Palette's evaluation:
  - (a) error against human labels;
  - (b) the keyframe rhythm, plus heading, speed and bouts;
  - (c) training on lossy frames, tested on lossless;
  - (d) the darker 500 Hz frames.
- Palette recommends keeping a lossless reference subset if the crops go lossy.

The full-frame region map in this document is the second lever: smaller, but
independent of Palette's crop decision.

## What exists today

- `src/encoder_qp_map.{h,cpp}`: `QpMapPolicy` with modes `off` and
  `static_dish_prior`. A circle (dish), a halo width, and delta-QP values
  inside / halo / outside (defaults −2 / 0 / +2). `QpDeltaMap` is built on a
  block grid.
- The native recorder (`tools/external_recorder_ipc_probe.cpp`) takes the
  policy as `importance_map` command-line options. It builds the map once per
  session and passes it to NVENC as `qpDeltaMap` with `NV_ENC_QP_MAP_DELTA`:
  one signed value per 32 × 32 block for HEVC, 16 × 16 for H.264. The policy
  reaches the recorder through the external recorder contract
  (`src/external_recorder_contract_utils.cpp`).
- It is off in the app config and the camera configs. It has never been
  measured on the rig.
- NVENC (Video Codec SDK 13.1) has no custom per-frequency quantisation
  matrices (HEVC scaling lists). Region maps are the encoder lever available on
  the A16.

## How a region map saves space

A map does not change how much the encoder works. It changes the rounding
step per 32 × 32 tile, around a base level:
- **Constant-QP mode (fixed base QP):** a coarser step outside the fish saves
  bits directly, and the fish keep the base quality.
- **Rate-controlled mode (the full-frame default, VBR 45/60 Mbit/s):** a map
  moves bits from background to fish at the same file size. To save space, the
  target must also come down. The map is what lets it come down without
  hurting the fish.

On a 4512 × 4512 frame, the fish are a tiny share of the 141 × 141 = 19,881
tiles. Most bits go to the dish floor and to everything outside the dish.

## Modes

| Mode | Map | Needs |
| --- | --- | --- |
| `off` | none | today's default |
| `static_dish_prior` | finer inside the dish, coarser outside, fixed per session | exists; dish circle per camera |
| `fish_following` | base (uniform, or the dish prior) plus a finer step on every tile a fish touches, per frame | detections per frame reaching the recorder (below) |

`fish_following` is designed as a layer on top of either base. With no
detections it degrades to its base.

## fish_following, in detail

**Source of the boxes.**
- The fused per-camera graph finishes detection (and pose) before the frame is
  copied for recording (`copy_after_pose`). The boxes for frame N therefore
  exist before frame N reaches the recorder.
- They come from the same detections that feed SHAMAN v2. There is no second
  detector.

**Getting them to the recorder.**
- Today the recorder receives frames without detections.
- The per-frame frame message (owner push and the frame descriptor) gains a
  small, versioned field: up to K boxes in full-frame pixels, plus a "no
  detection" flag.
- This is a recorder protocol change. It belongs with the session engine
  design (gui-headless-unifier, `docs/session_engine_design_2026_11_02.md`,
  recorder protocol option B), and the dashcam lossless windows want the same
  per-frame channel.

**Building the map** (recorder side, per frame, off the acquisition and
detection threads):
1. Start from the base map: uniform, or the dish prior.
2. For each box:
   - mark every tile it touches;
   - dilate by one tile, because the fish never lines up with the grid and a
     grazed tile must not get the coarse step;
   - optionally extend in the direction of motion by the fish's speed, so a
     tail flick between frames stays inside the fine region.
3. Set those tiles to `fish_delta_qp` (for example −2 to −4 relative to base).
4. Hold the last boxes for H frames after a detection drops out (fish briefly
   lost), then fall back to the base.

Cost: 19,881 bytes per full frame. That is about 2 MB/s at 100 Hz per camera,
built on the recorder's CPU. It's negligible next to the encode, but it must
stay off any thread with a deadline.

**Split-GOP recording.** Each full frame is encoded by whichever of the two
shards (two GPUs) owns its GOP. The map must travel with the frame to that
shard. Both shards build maps the same way from the frame's boxes, so a GOP
boundary does not change quality.

**Rate control.**
- Recommend evaluating with constant base QP plus the map first, because the
  quality is predictable and the saving is directly measurable.
- Then decide whether production stays rate-controlled with a lower target.

**Evidence in the recording.**
- The policy goes into the recorder contract and the start snapshot: mode,
  base, deltas, dilation, hold, and for the dish prior its source artifact and
  digest, as today.
- Per frame:
  - the count of fine tiles and the box source frame id in the encode CSV;
  - no map dump by default; optionally a sampled map image for debugging.
- New output fields go through the schema-first outputs and the output ratchet.

## Evaluation before production

Run on the same footage at several settings (`off`, `static_dish_prior`,
`fish_following` at two deltas), at equal base QP and then at an equal
bitrate:
- **Storage:** bits per frame, overall and split by fish tiles versus
  background.
- **What analysis needs from the full frame:**
  - detection and tracking on the decoded full frame against the lossless or
    `off` reference;
  - the dish rim and registration evidence;
  - anything Palette derives from the master.
- **Visible artefacts:** boundaries between fine and coarse regions at large
  deltas, and the keyframe rhythm (already measured on crops).
- **500 Hz band frames:** separately, because they are darker and noisier.

## Crops

Same machinery, applied to the 384 px crop video. It only helps once the crops
are lossy (a lossless stream has no rounding to vary), and there is less
background in a crop to save. Order: Palette's crop QP evaluation first, then a
crop region map if the numbers justify it.

## Open questions for Jeremy

1. Which full-frame analyses must survive: tracking only, or also registration,
   rim and background measurements? This sets how coarse the background may go.
2. Constant QP, or rate-controlled with a lower target, for production?
3. Whether the detection boxes should come from the fused graph's detections or
   from the crop windows (the crop window already follows the selected fish).
4. Multi-fish arenas: does every detection get a fine region, or only the
   selected one?
