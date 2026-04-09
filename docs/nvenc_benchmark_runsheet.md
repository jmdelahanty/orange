# NVENC Benchmark Run Sheet

Date: 2026-04-01
Scope: concrete benchmark matrix for the current shipping recording path using
the available `orange` encoder controls.

See also:

- `docs/nvenc_direct_input_plan.md`
- `docs/nvenc_throughput_todo.md`
- `docs/codec_quality_evaluation_protocol.md`
- `docs/pre_encoder_reference_capture_plan.md`
- `docs/output_artifacts_contract.md`
- `scripts/plot_pipeline_perf.py`

## Goal

Measure the practical single-session recording envelope cleanly before changing
code, and separate:

- hard NVENC throughput limits,
- preset / tuning / codec cost,
- rate-control cost,
- and non-encoder contention from display / YOLO / fan-out.

This run sheet is intentionally "full" for the settings the UI exposes today,
but it still avoids meaningless combinations and uncontrolled confounders.

## Current Exposed Knobs

The current UI exposes these recording controls:

- codec: `h264`, `hevc`
- preset: `p1`, `p3`, `p5`, `p7`
- tuning: `hq`, `ll`, `ull`, `lossless`
- rate control: `vbr`, `vbr_cq`, `cqp`
- quality value: integer `1-51` for `vbr_cq` and `cqp`

Current code behavior that matters for interpretation:

- `vbr` enables `AQ` and `TemporalAQ`
- `hq` also enables lookahead
- `ll` and `ull` disable lookahead
- `cqp` disables `AQ`, `TemporalAQ`, and lookahead
- `lossless` overrides the normal RC path and forces all-intra / GOP `1`

For now, use `quality_value = 20` whenever the matrix calls for `vbr_cq` or
`cqp`, because that is the current default and keeps the first sweep comparable.

## Fixed Conditions

Use the same fixed conditions for every run unless the block explicitly says
otherwise:

- camera: `2.8 MP` color camera
- camera FPS: `400`
- output resolution: native / full resolution
- scene: same tank, same framing, same lighting, same exposure settings
- session count: one recording session at a time
- display: off
- YOLO: off
- other nonessential fan-out: off
- duration: `30 s` triage runs
- warmup: ignore the first `10 s` before judging steady state

Use these GPU targets:

1. `RTX A6000`
2. one logical GPU on the `A16`

## Artifacts To Keep For Every Run

For each run, keep:

- the output video file
- `Cam*_pipeline_perf.csv`
- `nvidia_smi_dmon.csv` and `nvidia_smi_dmon.stderr.log` when available
- `recording_snapshot.json`
- plot + summary output from `scripts/plot_pipeline_perf.py`

Recommended plotting command:

```bash
source /home/jeremy/miniforge3/etc/profile.d/conda.sh
conda activate juicebox
python scripts/plot_pipeline_perf.py /path/to/recording_dir --stats --stats-out /path/to/plots/summary.csv --out-dir /path/to/plots
```

Recommended per-run notes:

- GPU id
- driver version
- approximate GPU clocks or whether clocks were locked
- anything unusual in the scene or capture path

## Measurement Policy

Use the current pool sizes and backpressure behavior as part of the measurement.
Do not increase pool sizes first just to suppress failures.

Interpret the runtime counters like this:

- `acq_starve`: acquisition could not reserve the required work entry or events
  before attempting to fetch another camera frame. This is acquisition-stage
  backpressure.
- `pre_waits`: preprocess had to retry before it could reserve encoder-side
  resources. This is an early warning, not an automatic failure.
- `pre_drops`: preprocess explicitly dropped a frame because encoder-side
  resources were still unavailable after retries.
- `enc_fail`: the HW encode stage failed to obtain or submit encoder work.
- `enc_slow`: the HW encode stage exceeded the current slow-frame threshold.

Benchmark policy:

- Allow drops, starvation, and failures in exploratory runs. Do not hide them.
- For boundary-finding runs, record the first appearance of `acq_starve`,
  `pre_drops`, or `enc_fail`, and whether the counters keep climbing.
- Treat larger pools as a later experiment, not as the first fix. Larger pools
  mostly delay exhaustion; they do not change steady-state throughput.

Candidate pass criteria after the `10 s` warmup:

- `enc_fps` stays within roughly `1%` of target FPS.
- `acq_starve` stays flat at `0`.
- `pre_drops` stays flat at `0`.
- `enc_fail` stays flat at `0`.
- `pre_q` and `enc_q` do not grow steadily.
- `pre_buffers` and `pre_events` do not pin near zero.

Interpretation notes:

- Rising `pre_waits` without drops means the run is near the edge but not yet
  disqualified.
- A run that passes for `30 s` but fails over `3-5 min` is not production-safe.
- Use pipeline CSV counters for encode-path stability, and use camera
  `dropped_frames` as a separate source-side health signal for frame-ID gaps and
  `EVT_CameraGetFrame` errors.

## Run Naming

Use a directory or note format that makes later comparison easy:

`<date>_<gpu>_<codec>_<preset>_<tuning>_<rc>[_q20][_display][_yolo]`

Examples:

- `2026_04_01_a6000_hevc_p3_hq_vbr`
- `2026_04_01_a16g0_h264_p1_ll_cqp_q20`
- `2026_04_01_a6000_hevc_p1_ll_vbr_display_yolo`

## Block A: Core Encoder Sweep

Purpose:

- map the preset / tuning / codec envelope on the cleanest current path
- identify which combinations are truly sustainable at `400 FPS`
- locate the failing boundary before adding more dimensions

Run all combinations of:

- codec: `h264`, `hevc`
- preset: `p1`, `p3`, `p5`, `p7`
- tuning: `ull`, `ll`, `hq`
- rate control: `vbr`

Do not include `lossless` in this block.

Count:

- `24` runs per GPU
- `48` total across `RTX A6000` + one `A16` GPU

Why this is the right first full sweep:

- it covers the full exposed preset ladder
- it covers both low-latency tunings plus the quality-oriented tuning
- it keeps RC fixed so preset / tuning effects are not confounded

## Block B: Rate-Control Sweep On Anchor Points

Purpose:

- measure how much throughput cost is coming from RC mode rather than preset
- compare AQ / TemporalAQ / lookahead-heavy paths against the cheaper `cqp`
  path

Run these anchor operating points:

- `h264 p1 ll`
- `h264 p3 hq`
- `hevc p1 ll`
- `hevc p3 hq`

For each anchor point, run:

- `vbr`
- `vbr_cq` with `quality_value = 20`
- `cqp` with `quality_value = 20`

Count:

- `12` runs per GPU
- `24` total across `RTX A6000` + one `A16` GPU

Why these anchors:

- `p1 ll` is the throughput-first edge
- `p3 hq` is a likely quality-oriented stress point
- both codecs are represented

## Block C: Lossless Reference Clips

Purpose:

- create short high-fidelity references for later codec comparisons
- avoid treating long uncompressed capture as a default workflow

Run these short clips:

- `h264 p1 lossless`
- `hevc p1 lossless`

Settings:

- duration: `10 s`
- display: off
- YOLO: off

Count:

- `2` runs per GPU
- `4` total across `RTX A6000` + one `A16` GPU

Why this is bounded:

- lossless is useful as a reference artifact
- it is not the main throughput study
- sweeping all presets under lossless is lower value than the other blocks

## Block D: Fan-Out / Contention Subset

Purpose:

- separate "true encoder ceiling" from system contention
- quantify whether display / YOLO materially reduce sustainable encode rate

Use these representative settings:

- one stable high-throughput point from Block A
- one near-boundary or failing point from Block A

Recommended default pair if you want consistency before looking at results:

- `hevc p1 ll vbr`
- `hevc p3 hq vbr`

For each chosen setting, run:

- baseline: display off, YOLO off
- display on, YOLO off
- display off, YOLO on
- display on, YOLO on

Count:

- `8` runs per GPU if you use two representative settings
- `16` total across `RTX A6000` + one `A16` GPU

## Block E: Long Confirmation Runs

Purpose:

- confirm that "passes for 30 seconds" also means "sustained"
- expose slower pool drain, queue growth, or intermittent encode lag

Run `3-5 min` confirmations for:

- the fastest clearly stable point
- the best quality point that still appears stable
- the first point that looks marginal
- the first point that clearly fails

Recommended minimum:

- `4` long runs per GPU

## Suggested Order

Best-practice order:

1. Run Block A on `RTX A6000`.
2. Run Block A on one `A16` GPU.
3. Plot and inspect results before expanding.
4. Run Block B on both GPUs.
5. Run Block C on the better single-stream GPU first, then the other GPU.
6. Run Block D only after a stable point and a marginal point are known.
7. Run Block E last.

This keeps the early analysis clean and prevents the later blocks from being
driven by guesses.

## Stop Conditions

You can stop expanding a branch of the matrix early if:

- the encoder is obviously far below target FPS and queue / pool exhaustion is
  immediate
- a slower preset on the same codec and tuning is guaranteed to be worse for
  the current question
- a second GPU reproduces the same ranking clearly enough that the remaining
  branch adds little value

Even with time available, best practice is to avoid collecting redundant runs
that do not change the decision.

## What To Look For

Primary comparisons:

- `h264` vs `hevc` at matched preset / tuning
- `ull` vs `ll` vs `hq` at matched codec / preset
- `p1` vs `p3` vs `p5` vs `p7` at matched codec / tuning
- `vbr` vs `vbr_cq` vs `cqp` on the anchor points
- `RTX A6000` vs `A16` for the same single-session stream

Primary failure signals:

- `enc_fps` below target FPS
- monotonic growth in `enc_q` or `pre_q`
- falling `pre_buffers` / `pre_events`
- nonzero `pre_drops`
- sustained increase in `enc_slow`
- final summary showing the encoder never reached steady-state target

## Expected Total Run Count

If you execute the full run sheet as written:

- Block A: `24`
- Block B: `12`
- Block C: `2`
- Block D: `8`
- Block E: `4`

Total:

- `50` runs per GPU
- `100` runs across `RTX A6000` + one `A16` GPU

That is a real full matrix, but it is still structured enough to interpret.

## Follow-On Decisions This Matrix Should Enable

After this run sheet, the team should be able to answer:

- Is the current limit mostly hard NVENC capacity or current-path overhead?
- Is `HEVC` worth its throughput cost for the target workloads?
- How large is the preset / tuning tax in practice at `2.8 MP @ 400 FPS`?
- Does `cqp` buy meaningful headroom over the current `vbr` path?
- Does the `RTX A6000` materially outperform a single `A16` GPU for one hard
  stream?
- Which operating points deserve long-run validation and later comparison
  against direct registered-input NV12 and pre-encoder reference clips?
