# PTP Recording Sink Experiment Plan

Date: 2026-04-17
Branch: `exp/gop-split-a16`

Related notes:

- `docs/multi_camera_failure_modes.md`
- `docs/ptp_sync_hardening_todo.md`
- `docs/helper_queue_wait_explainer.md`

## Recabled Validation Update (2026-04-21)

The original experiment plan below is now historical context for the older
pre-recable/nonzero-stagger stale-frame investigations. After the recabled A16
topology and the stable GPUDirect receive/requeue fix in commit `951f910`,
no-stagger headless `ptp_gate` recording is validated in short dual-camera
`100 fps` runs.

Current validated artifacts:

- stream-only sanity:
  `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_ptp_stream_only_recabled_stable_frame_patch`
- real recording, best `12 s` run:
  `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_ptp_real_recabled_stable_frame_patch_12s`

Checked-in specs:

- `experiment_specs/2010095_2010096_split_gop_hevc_100fps_ptp_stream_only_recabled_stable_frame_patch.json`
- `experiment_specs/2010095_2010096_split_gop_hevc_100fps_ptp_real_recabled_stable_frame_patch_12s.json`

Best real-recording result:

- `2010095`: `1001` submitted frames, `0` frame-ID gaps, `0` GetFrame errors,
  `0` preprocess drops, `0` encode failures
- `2010096`: `1000` submitted frames, `0` frame-ID gaps, `0` GetFrame errors,
  `0` preprocess drops, `0` encode failures
- split-GOP output: `overflow_events = 0`, `peak_backlog_gops = 2`
- `ptp_sync_summary.json`: sub-microsecond mean PTP offset and
  latch-minus-frame around `9.2 ms`

Remaining caveat:

- early startup `PTP_STALE_DUMP` logs still appear while encoders are coming up,
  but the steady-state summaries and artifacts show no frame loss, receive
  errors, preprocess drops, encode failures, or GOP overflow

Current read:

- current no-stagger recabled headless `ptp_gate` is a validated synchronized
  recording point for cameras `2010095` and `2010096`
- the old nonzero-stagger stale-frame artifacts remain useful historical failure
  evidence, but they should not be read as contradicting the current no-stagger
  recabled validation
- GUI PTP recording, longer soaks, and more than two cameras remain unvalidated

## Purpose

This note captures the next diagnostic step for the unstable:

- dual-camera `100 fps`
- `ptp_gate`
- recording-enabled
- nonzero-stagger

failure mode.

The immediate goal is to separate:

- "recording enabled changes buffer ownership / frame requeue timing"

from:

- "real preprocess / encode / output work is required to trigger the failure"

## Current Read

The current evidence is already fairly specific:

- dual-camera `100 fps` `ptp_gate` `stream_only` is healthy
- in the original pre-recable/nonzero-stagger investigation, dual-camera
  `100 fps` `ptp_gate` recording was not healthy
- stale-onset receive logging shows the bad camera already receiving stale
  frames before visible app-side starvation
- stale-onset recording-submit logging shows the bad camera is still:
  - primary-only
  - shallow at the handoff
  - not yet showing preprocess waits/drops

That means:

- recording must be enabled to trigger the bad interaction
- but the first visible onset is still upstream of the normal recording hot
  spots

One plausible mechanism is direct-pointer buffer lifetime:

- without recording, acquisition can often recycle/requeue camera buffers
  quickly
- with recording enabled, the `WORKER_ENTRY` lifetime extends downstream
- in direct-pointer mode, camera requeue is deferred until the last consumer
  releases the entry
- that can perturb camera / SDK / transport buffering without requiring
  obvious queue growth first

This is still a hypothesis. The sink experiments are meant to test it.

## Proposed Experiment

Add a headless-only experimental recording sink mode that replaces the real
recording path with simpler staged sinks.

This should be controlled by experiment spec and local headless CLI only.

Suggested controls:

- `fixed.recording_sink_mode = real|preprocess_only|immediate_recycle|threaded_handoff_only`
- `--recording-sink-mode real|preprocess_only|immediate_recycle|threaded_handoff_only`

Default remains:

- `real`

## Stage 1: Immediate Recycle Sink

Behavior:

- recording is still logically enabled
- acquisition still assigns `recording_frame_id`
- acquisition still takes the "recording active" branch
- but instead of calling the real recording pipeline, the entry is released
  and recycled immediately

Important intent:

- preserve "recording on" bookkeeping
- avoid preprocess / encode / output work
- avoid a cross-thread handoff to the recording pipeline

What this tests:

- whether bookkeeping alone is enough to trigger the failure

Expected interpretation:

- if this is healthy:
  - bookkeeping alone is not the trigger
- if this is unhealthy:
  - the trigger is very early and may be tied to frame-id mode,
    reference-counting, or direct-pointer ownership transitions

## Stage 2: Threaded Handoff-Only Sink

Behavior:

- recording is still logically enabled
- acquisition still hands the frame to a dedicated recording-side worker or
  queue
- that worker immediately releases / recycles the entry
- no preprocess, encode, or output work is performed

Important intent:

- preserve the cross-thread handoff and delayed release semantics
- still avoid real recording work

What this tests:

- whether ownership transfer and delayed camera-buffer requeue timing are
  sufficient to trigger the failure

Expected interpretation:

- if `immediate_recycle` passes but `threaded_handoff_only` fails:
  - the trigger is likely handoff / ownership / requeue timing
- if both pass:
  - the trigger is further downstream in real recording work
- if both fail:
  - the trigger is earlier than expected and not dependent on real recording
    work

## Recommended Initial Matrix

Start with the known-problem case:

- dual-camera `100 fps`
- `ptp_gate`
- `2 ms` stagger
- validated PIX pairs

Run:

1. `real`
2. `immediate_recycle`
3. `threaded_handoff_only`

Track:

- `acq_fps_mean`
- `dropped_frames_camera`
- `[PTP_STALE_DUMP]` presence
- `latch_minus_frame_ns` behavior
- whether the bad behavior still follows the offset camera

## Why This Is Better Than More Handoff Logging Right Now

The current handoff logs already show:

- the bad camera is still primary-only
- helper dispatch has barely started
- preprocess queues are shallow
- preprocess pools are near full

So more counters at the same boundary are unlikely to discriminate the root
cause as effectively as replacing the downstream behavior with a simpler sink.

## Non-Goals

This experiment is not meant to:

- validate a permanent production mode
- replace the real recording pipeline
- explain the entire `2 x 100 fps free_run` backlog-overflow mode

It is specifically aimed at:

- the historical `100 fps` `ptp_gate` recording-enabled stale-frame onset
  problem

## Exit Criteria

This experiment is successful if it tells us which of these is true:

1. recording bookkeeping alone is enough
2. cross-thread handoff / delayed release is enough
3. real recording work is required

Once one of those is established, the next instrumentation step can be much
more targeted.

## First Results (2026-04-17)

The first two sink probes were run against the known-bad case:

- dual-camera `100 fps`
- `ptp_gate`
- `2 ms` stagger
- validated PIX pairs

Artifacts:

- `immediate_recycle`:
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger2ms_immediaterecycle_rerun2`
- `threaded_handoff_only`:
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger2ms_threadedhandoff_rerun2`

Results:

- both sink modes sustained about `100 fps` on both cameras
- both sink modes had:
  - `0` camera drops
  - `0` acquisition starvation
  - no stale-frame onset

Current interpretation:

- bookkeeping alone is not enough to trigger the failure
- a simple cross-thread handoff / delayed release is also not enough

## Follow-Up: Acquisition Buffer Ownership Probe (2026-04-18)

Because the sink modes passed cleanly, the next probe forced acquisition to
copy GPUDirect camera buffers into Orange-owned ring-buffer memory before
recording work:

- control:
  - `fixed.acquisition_buffer_mode = force_ring_copy`
- artifact:
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger2ms_forceringcopy_rerun1`

Result:

- `2010095` improved substantially relative to the failing direct-pointer run
  but still did not sustain the target:
  - `enc_fps_mean = 80.2815`
- `2010096` still collapsed badly:
  - `enc_fps_mean = 5.35862`
- stale-frame onset still occurred
- the run used `direct=0 ring=1` at acquisition, so the direct camera-buffer
  pass-through path was genuinely disabled

Interpretation:

- direct camera-buffer lifetime / requeue behavior is not the whole problem
- forcing ring-copy changes the shape of the failure, but does not eliminate
  the offset-camera collapse
- the remaining trigger still requires real downstream recording work

## Follow-Up: Preprocess-Only Probe (2026-04-18)

The next discriminator ran the same known-bad case with:

- `fixed.recording_sink_mode = preprocess_only`

Artifact:

- `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger2ms_preprocessonly_rerun1`

Result:

- the run still failed
- `2010095` degraded to `acq_fps_mean = 90.6936`
- `2010096` degraded to `acq_fps_mean = 82.2947`
- stale-frame onset still occurred on the offset camera
- there was still no encoder/output work

Most important clue:

- the stale-onset dump occurred just after helper routing began on the bad
  camera
- the bad camera was still primary-only through recording frame `100`
- helper routing started at recording frame `101`
- the stale threshold fired at local frame `106`

Interpretation:

- real preprocess work is enough to trigger the bad interaction
- encode/shared output are not required
- the strongest remaining suspect is now cross-GPU helper preprocess under
  PTP-gated stagger, not the later encode/output path

## Follow-Up: Primary-Only Preprocess Probe (2026-04-18)

To isolate helper routing, the same `preprocess_only` probe was rerun with a
runtime recording override:

- `fixed.recording.mode = "single_session"`

That keeps the run in:

- real acquisition
- real primary preprocess
- no helper routing
- no HW encoder / output work

Artifact:

- `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_hevc_100fps_gop25_dual_pix_ptp_stagger2ms_preprocessonly_primaryonly_rerun1`

Result:

- both cameras stayed healthy
- `2010095`: `99.839172 fps`, `0` drops
- `2010096`: `99.840248 fps`, `0` drops
- no stale-frame onset

Interpretation:

- primary-only preprocess is healthy at `100 fps` under `ptp_gate + 2 ms`
  stagger
- helper cross-GPU preprocess is now the narrowest confirmed trigger for the
  bad multi-camera `100 fps` PTP-stagger failure
- the pathological stale-frame onset requires real downstream recording work,
  not just "recording enabled" state or a lightweight recording handoff

## Follow-Up: No-Stagger Primary-Only Control (2026-04-18)

The same primary-only preprocess control was rerun without any PTP stagger:

- `fixed.recording.mode = "single_session"`
- `fixed.recording_sink_mode = "preprocess_only"`
- `fixed.ptp_gate_stagger_ns = 0`

Artifact:

- `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_hevc_100fps_gop25_dual_pix_ptp_nostagger_preprocessonly_primaryonly_rerun1`

Result:

- both cameras stayed healthy
- `2010095`: `99.835815 fps`, `0` drops
- `2010096`: `99.835266 fps`, `0` drops
- no stale-frame onset

Interpretation:

- the recovery is not specific to the `2 ms` offset
- disabling helper routing is what fixes the run
- the offset only matters once the helper cross-GPU preprocess path is active

That does not yet prove whether the trigger lives in:

- preprocess
- helper routing / split-GOP topology
- encoder-side work
- shared output / ordered GOP release

But it rules out the earlier "ownership timing alone" hypothesis as the primary
cause.

## Follow-Up: Lightweight Helper Host Sampling Baseline (2026-04-18)

The intrusive CUDA-event helper probe was replaced with a lighter host-side
sampler that records, for the first `32` helper-routed frames:

- helper enqueue time in `RecordingIngress`
- helper worker start time
- helper worker completion time
- queue depth and free buffer/event counts at enqueue/start

Validated artifacts:

- `free_run`:
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_preprocessonly_dual_pix_freerun_helperprobe5`
- `ptp_gate`:
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_preprocessonly_dual_pix_ptp_helperprobe5`

Result:

- both modes still degraded to roughly `69-70 fps`
- `free_run`:
  - `2010095`: `69.8883 fps`
  - `2010096`: `69.9954 fps`
- `ptp_gate`:
  - `2010095`: `69.3989 fps`
  - `2010096`: `69.2939 fps`

Most important helper-host timing result:

- the helper worker itself is not slow
- the dominant startup cost is helper queue wait, not worker service time
- first helper-routed frames showed:
  - `free_run`: queue wait about `28.5-28.8 ms`, worker service about `0.05-0.07 ms`
  - `ptp_gate`: queue wait about `33.3-33.6 ms`, worker service about `0.04-0.05 ms`
- queue wait then decayed quickly:
  - frame `102`: about `19-24 ms`
  - frame `103`: about `9-13 ms`
  - frame `104`: about `3.5-3.8 ms`
  - later sampled frames: mostly `0.01-0.10 ms`

Interpretation:

- helper preprocessing work itself is not the dominant cost
- the failure is currently better described as a helper-path startup backlog
  right when helper routing begins at recording frame `101`
- that startup backlog exists in both `free_run` and `ptp_gate`
- `ptp_gate` is slightly worse at onset, but the basic helper startup problem
  is not PTP-specific in this probe

This shifts the next question from:

- "is the helper worker slow?"

to:

- "why does helper routing begin with a burst of queued work before the helper
  worker settles into steady-state service?"

## Follow-Up: Helper Cross-GPU Prewarm (2026-04-19)

The helper-host timing probe showed that the first helper-routed frame paid a
large startup queue-wait cost even though helper worker service time was tiny.
To test whether that was CUDA setup rather than steady-state helper speed, the
helper preprocess worker now prewarms cross-GPU input setup during pipeline
construction.

The prewarm does this before recording starts:

- enables peer access from acquisition GPU to helper preprocess GPU
- allocates the helper-side input staging buffer
- records and synchronizes a lightweight CUDA event on the helper stream

Validation probes:

- `free_run`:
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_preprocessonly_dual_pix_freerun_helperprobe6`
- `ptp_gate`:
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_preprocessonly_dual_pix_ptp_helperprobe6`

Result:

- first helper queue wait improved from about `28-34 ms` to about `4 ms`
- helper worker service remained tiny, usually below `0.1 ms`
- both runs still remained marginal because acquisition rate stayed around
  `69-70 fps`
- camera drops remained high:
  - `free_run`: `400-401`
  - `ptp_gate`: `351`

Interpretation:

- helper cross-GPU setup cold-start was real and is now mostly removed
- the remaining two-camera `100 fps` failure is not explained by helper
  activation latency alone
- next diagnostics should focus on why acquisition timing drops into the
  `50/100 fps` alternating pattern once helper routing is active

## Follow-Up: Deferred Source Release (2026-04-19)

The next source-lifetime hypothesis was that preprocess might recycle a raw
camera/ring source entry before queued CUDA work had finished reading it. The
preprocess worker now defers raw source release until a CUDA event proves the
source is no longer needed:

- helper cross-GPU route: release after the peer copy into helper staging has
  completed
- same-GPU route: release after preprocess work has been queued through the
  source-safe point

The probe also records source-lifetime samples around recording frames
`90-140`, which straddles helper route activation at frame `101`.

Validated artifacts:

- `free_run`:
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_preprocessonly_dual_pix_freerun_helperprobe10`
- `ptp_gate`:
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_preprocessonly_dual_pix_ptp_helperprobe11`

Result:

- both completed as marginal, not failed
- both had `0` source-release event misses after the release-event pool was
  sized to at least the normal preprocess entry pool
- `free_run`:
  - `2010095`: `70.2575 fps`, `400` camera drops
  - `2010096`: `70.2966 fps`, `401` camera drops
- `ptp_gate`:
  - `2010095`: `69.4057 fps`, `351` camera drops
  - `2010096`: `69.5988 fps`, `351` camera drops
- sampled normal source-release delay was small:
  - same-GPU primary frames: about `0.4-0.5 ms`
  - helper cross-GPU frames: about `3.2 ms`

Interpretation:

- raw source buffer reuse is now CUDA-safe by construction
- the remaining `100 fps` failure is not explained by premature raw-source
  recycling
- the next useful probe should move upstream to acquisition timing:
  `EVT_CameraGetFrame` wait duration, camera frame timestamps, and host receive
  cadence around helper route activation

One abnormal PTP run before the event-pool sizing fix showed a single-camera
stall and source-release event exhaustion:

- `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_preprocessonly_dual_pix_ptp_helperprobe10`

Treat that as a stress signal for event-pool sizing, not as the primary
performance baseline.

## Follow-Up: Acquisition Cadence Probe (2026-04-19)

The next probe moves upstream from preprocess/source-release timing to the
acquisition handoff itself. Orange now writes a compact per-camera CSV sidecar:

- `<recording_folder>/Cam<serial>_acquisition_cadence_probe.csv`

The probe records frames `80-160`, using `recording_frame_id` when recording is
active and `local_frame_id` otherwise. That window surrounds helper activation
at recording frame `101`.

The sidecar includes:

- `EVT_CameraGetFrame` wait time
- host receive-to-receive frame delta
- camera timestamp delta
- PTP latch/frame deltas when PTP is active
- receive-to-recording-submit latency
- selected recording target GPU and primary/helper route flags
- acquisition free-entry/free-event counts
- preprocess/encode queue and resource counters from `RecordingIngress`

Intended interpretation:

- if camera timestamp or host receive deltas are already alternating before
  submit, the problem is acquisition/camera cadence
- if acquisition cadence is clean but receive-to-submit or route-transition
  fields jump, the problem is the acquisition-to-recording handoff
- if acquisition and submit stay clean while downstream queues grow, the
  problem has moved back into preprocess/encode/output

Initial `free_run` validation artifact:

- `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_preprocessonly_dual_pix_freerun_cadenceprobe1`

Important result from the captured probe window:

- recording frame `101` is the first helper-routed frame on both cameras
- recording frame `101` still has a normal `~10 ms` camera timestamp delta
- starting at recording frame `102`, both cameras jump to `~20 ms` camera
  timestamp deltas
- camera frame IDs then advance by `2` per received frame:
  - `2010095`: frame `101 -> 103 -> 105 -> 107`
  - `2010096`: frame `101 -> 103 -> 105 -> 107`
- `receive_to_submit_ns` stays tiny, usually a few microseconds
- acquisition free entries/events remain healthy

Interpretation:

- the `100 fps` free-run collapse is already visible at camera receive time
- the acquisition thread is not spending meaningful time between receive and
  recording submit
- this points away from `RecordingIngress::SubmitFrame` cost and toward an
  upstream GPUDirect/EVT/acquisition-buffer interaction that is triggered by
  the helper route becoming active

This run crashed during cleanup on `AcquisitionStop` for one camera with the
known EVT socket-error/segfault pattern, but the full `80-160` probe window was
written for both cameras and is usable for this localization result.

## Follow-Up: Helper Source-Read No-Op (2026-04-19)

The acquisition cadence probe localized the free-run collapse to camera receive
time immediately after helper routing begins. The next control kept split-GOP
routing active but made helper-routed preprocess workers release the source
entry without reading/copying from the source GPU.

Experiment switch:

- `fixed.helper_noop_source_read = true` in the headless experiment spec
- internally maps to `ORANGE_PREPROCESS_HELPER_NOOP_SOURCE_READ=1`
- only cross-GPU/helper-routed preprocess entries are no-oped
- primary-routed entries still run through normal preprocess

Validation artifact:

- `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_preprocessonly_dual_pix_freerun_helpernoop2`

Result:

- both cameras passed the current diagnostic policy
- `2010095`: `99.9989 fps`, `0` camera drops
- `2010096`: `100.001 fps`, `0` camera drops
- submitted/routed frame counts remained split-GOP shaped:
  - `1200` submitted
  - `600` primary routed
  - `600` helper requested
  - `600` helper dispatched
- acquisition cadence sidecars show helper route activation at frame `101`
- frame IDs remain sequential through the probe window:
  - `2010095`: `99, 100, 101, 102, 103, ...`
  - `2010096`: `99, 100, 101, 102, 103, ...`
- receive-to-submit latency remains only a few microseconds
- helper worker queue wait/service time is tiny after the first no-op helper
  frame

Interpretation:

- split-GOP routing itself is not sufficient to cause the `50 fps` collapse
- `RecordingIngress::SubmitFrame` is not the bottleneck
- helper worker scheduling is not the bottleneck
- the trigger is specifically in the helper GPU source-read path:
  peer access, `cudaMemcpyPeerAsync`, or the lifetime/requeue interaction around
  a helper GPU reading a GPUDirect camera-owned source buffer

The next useful control is to force acquisition into Orange-owned ring-copy
buffers and rerun the real helper path. If ring-copy stays healthy, the problem
is specific to helper cross-GPU reads from EVT/GPUDirect camera buffers. If
ring-copy still fails, the cross-GPU copy path itself is the stronger suspect.

## Follow-Up: Force Ring-Copy With Real Helper Path (2026-04-19)

The helper no-op control showed that split-GOP routing itself is safe when the
helper worker does not read the source buffer. The next control forced
acquisition into Orange-owned ring-copy buffers while keeping the real helper
source-read path active.

Experiment setting:

- `fixed.acquisition_buffer_mode = "force_ring_copy"`
- `fixed.helper_noop_source_read` omitted/disabled
- `recording_sink_mode = "preprocess_only"`
- `sync_mode = "free_run"`

Validation artifact:

- `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_preprocessonly_dual_pix_freerun_ringcopy1`

Result:

- the run completed but remained marginal
- `2010095`: `70.1342 fps`, `400` post-warmup camera drops
- `2010096`: `70.1779 fps`, `401` post-warmup camera drops
- acquisition mode counters confirm ring-copy path:
  - `direct=0`
  - `ring>0`
- cadence sidecars show the same transition:
  - frame `101` is first helper route
  - frame `102` starts skipping every other camera frame
  - frame IDs jump `101 -> 103 -> 105 -> 107`
  - host receive deltas jump to about `20 ms`
  - `pending_requeues` stays around `2`
- helper source-release samples show cross-GPU source-safe release around
  `3.4 ms` after event record, without event misses

Interpretation:

- forcing Orange-owned ring-copy buffers does not fix the collapse
- the failure is not limited to helper reads from EVT/GPUDirect camera-owned
  buffers
- the stronger current suspect is the helper cross-GPU source-read/copy itself:
  reading from the acquisition GPU into the helper GPU appears to interfere
  with sustained `100 fps` acquisition from that source GPU
- this points toward source-GPU/PCIe/RDMA contention or a CUDA peer-copy
  interaction, not `RecordingIngress` routing or source-buffer ownership alone

## Follow-Up: Helper Peer-Copy Byte Sweep (2026-04-19)

The ring-copy result kept the real helper source-read path active and still
failed, so the next control limited how much data the helper worker copies from
the acquisition GPU into helper-GPU staging.

Experiment setting:

- `recording_sink_mode = "preprocess_only"`
- `sync_mode = "free_run"`
- `fixed.helper_copy_bytes = -1` keeps the normal full-frame helper copy
- `fixed.helper_copy_bytes = 0` skips the peer-copy payload while still routing
  to the helper preprocess worker
- positive `fixed.helper_copy_bytes` values copy only that many bytes

This is a diagnostic-only control. Any value other than the full frame produces
invalid helper-side image content, but it keeps the route, worker scheduling,
and source-release mechanics active enough to test the acquisition-cadence
impact of the copy payload.

Validation artifacts:

- `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_preprocessonly_dual_pix_freerun_copy0probe1`
- `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_preprocessonly_dual_pix_freerun_copy4kprobe1`
- `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_preprocessonly_dual_pix_freerun_copy1mprobe1`
- `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_preprocessonly_dual_pix_freerun_copy4mprobe1`
- `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_preprocessonly_dual_pix_freerun_copy8mprobe1`

Result summary:

| Helper copy cap | Result | Cadence probe |
| --- | --- | --- |
| `0` bytes | passes at about `100 fps`, `0` drops | no camera-frame-id jumps |
| `4 KiB` | passes at about `100 fps`, `0` drops | no camera-frame-id jumps |
| `1 MiB` | passes at about `100 fps`, `0` drops | no camera-frame-id jumps |
| `4 MiB` | run policy still passes, but cadence shows first disruption | first frame-ID jump at recording frame `102` on both cameras |
| `8 MiB` | marginal, about `96.7 fps`, `40-41` post-warmup drops | frame-ID jumps begin at recording frame `102` and recur |

Important cadence detail:

- helper routing starts at recording frame `101`
- the first bad receive cadence appears at recording frame `102`
- for `4 MiB` and `8 MiB`, both cameras jump from camera frame `101` to `103`
  and host receive deltas grow to about `20 ms`
- for `0`, `4 KiB`, and `1 MiB`, camera frame IDs remain sequential through
  the same probe window

Current interpretation:

- helper routing and helper worker scheduling are not sufficient to cause the
  collapse
- a tiny peer copy is also not sufficient
- the acquisition-cadence disruption appears once the helper peer-copy payload
  reaches a few MiB per helper-routed frame
- the current root-cause target is now the sustained source-GPU to helper-GPU
  peer-copy payload contending with or perturbing camera receive on the source
  GPU, rather than helper thread startup, submit cost, or source-buffer
  ownership alone

Route-shape clarification:

- split-GOP routing is GOP-level, not frame-level interleaving
- with `gop_length = 25` and two encoder GPUs, routing is shaped like:
  - frames `1-25`: first route GPU
  - frames `26-50`: second route GPU
  - frames `51-75`: first route GPU
  - frames `76-100`: second route GPU
- across a full run this still averages to about half primary-routed and half
  helper-routed frames
- but during a helper-owned GOP, every incoming frame in that GOP needs the
  full source-to-helper copy
- at `100 fps`, the active helper-copy window is therefore closer to:
  - `100 copies/sec * ~20 MiB/frame = ~2 GiB/sec` per active camera
  - not a smooth `50 copies/sec` spread evenly over the whole second

Implication for the next experiment:

- the byte sweep is consistent with burst contention, not just average PCIe
  bandwidth exhaustion
- delaying helper copy start by a small configurable amount may help if the
  large peer copy is colliding with the camera receive/requeue phase at the
  start of helper-owned GOPs
- useful first sweep:
  - `0 ns` baseline
  - `250000 ns`
  - `500000 ns`
  - `1000000 ns`
  - `2000000 ns`
- if small delays improve cadence, the problem is timing/burst contention
- if delays do not help, the remaining suspect is steadier source-GPU /
  copy-engine / PCIe contention during the helper-owned GOP itself

## Follow-Up: Helper Peer-Copy Delay Probe (2026-04-19)

The first timing probe added:

- `fixed.helper_copy_delay_ns`
- internally mapped to `ORANGE_PREPROCESS_HELPER_COPY_DELAY_NS`
- applied only in cross-GPU helper preprocess workers, immediately before the
  source-GPU to helper-GPU peer copy

First validation artifact:

- `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_preprocessonly_dual_pix_freerun_copydelay250us_probe1`

Setting:

- full-frame helper copy
- `recording_sink_mode = "preprocess_only"`
- `sync_mode = "free_run"`
- `fixed.helper_copy_delay_ns = 250000`

Result:

- the run did not complete cleanly; it hit the known EVT socket-error/segfault
  cleanup path after one camera stopped
- no `runs.csv` was written
- sidecar artifacts were written and are useful:
  - `Cam2010095_acquisition_cadence_probe.csv`
  - `Cam2010096_acquisition_cadence_probe.csv`
  - `Cam2010095_pipeline_perf.csv`
  - `Cam2010096_pipeline_perf.csv`
  - `recording_snapshot.json`
- both cameras began helper routing at recording frame `101`
- both cameras started skipping every other camera frame at recording frame
  `102`
- cadence sidecars show `59` frame-ID jumps in the `80-160` probe window for
  each camera
- pipeline CSVs show acquisition repeatedly alternating around `50 fps` and
  `90-98 fps`
- terminal output before the crash showed:
  - `2010096`: `451` camera drops, about `67.8 fps`
  - `2010095`: socket operation failed during shutdown/checkCameraErrors

Interpretation:

- a simple per-frame `250 us` host delay before the helper peer copy does not
  move the copy into a safe window
- this delay worsened the symptom relative to the no-delay full-copy case
- the result points away from "the copy starts a few hundred microseconds too
  early" as the primary explanation
- the remaining suspect is sustained copy pressure during helper-owned GOPs, or
  a more specific copy scheduling/topology interaction that a naive sleep does
  not fix
