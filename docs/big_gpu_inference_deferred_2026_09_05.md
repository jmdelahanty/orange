# Deferred: Detection On A Big GPU (A6000 Prototype, L40S / RTX 6000 Ada Target)

Date: 2026-09-05. Status: **deferred**. Decision on 2026-09-05: keep the
four-camera pipeline on the A16s as it stands (see
`detect_latency_review_2026_09_03.md` for where that stands: 2.20 ms mean,
2.29 p95, 2.32 p99 acquisition-to-detect with full-frame recording, crops
and crop video on) and revisit this when hardware or a latency need
justifies it. Everything below is measured on the rig's RTX A6000 on
2026-09-04 or is a scaled estimate marked as such.

## What was measured

Same detect ONNX (omnifin0 cedar-shadow v007) built for the A6000 with
`trtexec --fp16`; cameras acquired by GPUDirect straight into GPU 0;
engine-only true direct read; no recorder (the A6000 cannot host one, see
below). Specs: `onecam_engine_only_direct_read_omnifin_a16`,
`onecam_engine_only_direct_read_omnifin_a6000`,
`threecam_engine_only_direct_read_omnifin_a6000`. Engine:
`orange_data/detect/omnifin0_cedar_shadow_v007_detect_20260206-235656_25f3fbcb_a6000_gpu0_trt100_fp16.engine`.

| | A16 die (GA107, 10 SMs) | RTX A6000 (GA102, 84 SMs) |
|---|---|---|
| trtexec GPU compute, median, fp16 with EfficientNMS in the graph | 2.06 ms | 0.68 ms |
| one camera: graph mean / p95 | 2.061 / 2.067 | 0.708 / 0.737 |
| one camera: acquisition to detect mean / p95 / p99 | 2.197 / 2.211 / 2.226 | 0.886 / 0.937 / 1.014 |
| three synchronized cameras on the one GPU: graph mean / p95 | 2.00 / 2.03 each, on its own die | 1.25 / 1.57 |
| three synchronized cameras: acquisition to detect mean / p95 / p99 | 2.20 / 2.21 / 2.23 | 1.43 / 1.78 / 2.40 |

Readings: 8x the SMs and 3.8x the bandwidth buy 3x on this batch-one
640-input graph, which is launch-bound; the CUDA graph already hides most
launch cost. Three batch-one graphs landing together (PTP synchronizes the
cameras to microseconds) overlap only partly on 84 SMs, so the mean gain
drops to 1.5x and the p99 advantage is gone at three cameras; at four it
would be worse. GPUDirect RDMA into the A6000 works (three cameras, 100
fps each, zero gaps).

## Why the A6000 in this rig cannot take the job alone

- No peer access with any A16 die (`nvidia-smi topo -p2p r`: NS against
  all of them; the A16 dies have it with each other across both cards).
  CUDA IPC handles from the analytics process cannot be opened on an A16
  from A6000 memory, so no A16 recorder can read frames the A6000 holds
  (the external crop recorder placed on the A6000 died on its first
  frame with `cudaIpcOpenMemHandle: peer access is not supported`).
- One NVENC engine, which cannot encode even one camera's 20 MP at 100
  fps.

Detection on the A6000 with recording on the A16s therefore means two
host hops per frame (20 MB device-to-host, 20 MB host-to-device, about
1.7 ms each at PCIe 4 rates, off the detect path), 8 GB/s in and 8 GB/s
out of the A6000's link for four cameras, and the copies contending with
the graph on the A6000 the way copies did on the A16 dies. Feasible on
paper; the registered-source recorder would still work because the
host-to-device copy can land in the pool buffer. Not worth building
unless the batched inference below is built too.

## The two designs, if revisited

### A. Batched inference on one big GPU (A6000 prototype)

Because PTP delivers all four frames within microseconds, run one
batch-four inference per period instead of four batch-one graphs. Batch
four on this graph should cost about 1.0 to 1.2 ms for all four (estimate
from the three-camera overlap measurement), a floor near 1.3 ms per camera
against 2.2 today. Work: a batch-N engine build; a scheduler that gathers
the four frames (bounded wait, degrade to a smaller batch if a camera is
late); per-camera output demux back into each camera's YOLO worker path;
and, for recording, the host bounce above or a big card with its own
encoders. The A6000 is enough to prototype the inference half with three
cameras and no recorder.

### Why one Ada card needs none of the split-GOP machinery

Why the A16 needs it. An A16 is not one GPU: it is four GA107 dies on one
board, each with its own 16 GB of memory, its own ten SMs and its own
single seventh-generation NVENC engine, behind one PCIe switch. There is
no shared memory across dies. A frame that the camera RDMAs into die 3
exists only in die 3's memory. Die 3's one NVENC cannot sustain the 20 MP stream at 100 fps: the
split-GOP design record (`docs/multi_gpu_gop_splitting_design.md`) states
that a single NVENC path cannot be relied on to save 4512x4512 Mono8 at
100 fps, the encoding-master measurements of 2026-06-05 show one encoder
failing even P5 HQ GOP1 at 60 fps (327 of 421 frames, 94 drops), and the
only four-camera known-good point, P1 LL VBR150 GOP25 at 100 fps, is a
two-shard result. (A previous draft of this section claimed about 100 fps
per engine at p1; that figure was an inference from the copy path's
per-frame time, not a capacity measurement, and is withdrawn.) So to
record one camera at 100 fps at all, the GOPs alternate between two
dies' encoders, and for every other GOP the other die must first pull the 20 MB
frame out of the detect die's memory across the card switch before it
can encode. That copy, the switch it crosses, and the second die's
encoder reading memory that the graph on the first die is also using are
the origin of everything in the review document called "same-die
residual", "card-level contention", "other-die shard", "crop recorder
placement" and "GOP-parity interleave". The scheme exists because each
die has one encoder too slow for the stream and its own memory; on the
A16, split-GOP is required, not an optimization.

Why an L40S or RTX 6000 Ada does not. It is one GPU with one memory
space and three eighth-generation NVENC engines on it. Every engine reads
the same memory the graph writes and the camera RDMAs into, so an encoder
never needs the frame copied anywhere: the registered-source path we run
today on the detect die, encode straight from the pool buffer, is the
only path, for both cameras and for the crop encoder. There is no second
die, so nothing crosses a switch and no die is "the other one". The three
engines are load-balanced by the driver across encode sessions on their
own: one session per camera stream plus one per crop stream, and the
driver spreads them over the engines without any GOP routing in our code.
Whether one Ada engine sustains 100 fps of 20 MP is not known: a GA107
engine does not, its actual capacity at P1 LL was never measured on its
own, and NVIDIA's generational figure of roughly 2x per engine for Ada is
not a measurement of this format at this size. Two cases, both fine:

- One Ada engine does sustain 100 fps: one session per camera, the
  driver spreads the sessions over the three engines, no GOP routing in
  our code.
- It does not: the GOPs still alternate between two encode sessions, but
  both sessions are on the same GPU reading the same buffer, so the merge
  logic that exists today keeps working and the copy, the switch and the
  other-die residual are gone. Split-GOP survives as a session
  alternation, not as a data movement.

Either way three engines are comfortably more than two cameras need,
because the alternation only has to buy the same 2x headroom it buys on
the A16, now with no transfer cost.

What is left over is the one effect that does not depend on topology:
the encoder reading a frame from the same memory the graph is using,
measured at 0.1 ms on a GA107 die with 200 GB/s of bandwidth, and
expected to shrink on a card with 864 GB/s. Everything else in the list
above becomes a no-op: the split-GOP strategy collapses to single-GPU
placement (one session per camera if an Ada engine sustains the rate,
two alternating sessions on the same GPU if not), the external recorder becomes one process per camera on the
same GPU, the crop recorder goes on the same GPU with no placement
rule, and the merged-GOP writer has one shard to merge.

### B. Two L40S or RTX 6000 Ada cards, two cameras each (preferred)

Each card owns its two cameras end to end: RDMA in, detection, recording
through the card's own NVENC engines from the same memory (the
registered-source path we already run on the A16 detect die), crops and
pose in the idle window on the same card. Nothing crosses a PCIe link
after the frame arrives, so the card switch, the other-die shard, the
crop placement rules and the host bounce all disappear.

- Encode fits: three eighth-generation NVENC engines per card against
  two cameras. A GA107 engine cannot sustain 20 MP at 100 fps (see the
  section above), and whether one Ada engine can is unmeasured; if it
  cannot, two sessions alternate GOPs on the same GPU with no copy, which
  is the existing merge logic minus the transfer. Measure per-engine
  capacity first when a card is present.
- Latency, estimated from the A6000 measurement: graph 0.4 to 0.5 ms on
  142 Ada SMs at higher clocks; two synchronized cameras per card either
  overlap partly or take one batch-two inference of about 0.6 ms; frame
  floor roughly 0.8 to 1.0 ms with recording on.
- Engineering remaining: a recording profile placing both cameras'
  encoders on the same GPU (the strategy config already expresses
  single-GPU placement); the optional batch-two engine. The copy after
  detection, the registered source, the crop interleave and the verifier
  checks are placement-independent and carry over unchanged.
- Fit: the L40S is a passive 350 W datacenter card that needs chassis
  airflow; the RTX 6000 Ada is the same silicon with active cooling and
  the same three NVENC engines, the right fit for the pancake0
  workstation. GPUDirect RDMA is supported on both.
- Verification plan when a card is present, in order. About half a day
  of rig time; every estimate above becomes a measurement.
  1. **Per-engine NVENC capacity** (the unknown that decides whether GOPs
     alternate at all). Spec
     `onecam_nvenc_capacity_single_shard_p1_ll_a16`: one camera at 100
     fps, one external recorder shard on one GPU, P1 LL VBR150 GOP25, no
     crops, single-shard routing. Point its GPU ids at the new card. Read
     three things: the recorder summary's `frames_encoded` divided by the
     run duration (the sustained encode rate; 100 fps means the engine
     keeps up, less means the engine's capacity), the recorder's encode
     CSV `enqueue_age_ms` (flat means keeping up, climbing means falling
     behind), and the run's `pass_fail` (a single engine that cannot keep
     up backs the pool up to the hard cap and the cap verifier fails the
     run by design; that failure is the measurement, not a bug). Then the
     same with a second camera on the same GPU as a second single-shard
     stream to see whether two sessions land on two engines and both
     hold 100 fps, and with three. This is also runnable today on an A16
     die, where it would replace the never-measured GA107 P1 LL figure
     with a number; expect it to fail at 100 fps there. Do not treat any
     run of this spec as data: it exists to be pushed past its limit.

     **Measured on the A16, 2026-09-05 00:51** (camera 2010093, die 3, 60
     s): the engine received 5,900 frames and encoded 5,639, dropping 261
     on the recorder side, a sustained **95.6 fps** against the 100
     required. Its queue of 32 filled within the first six seconds and
     stayed full (enqueue age flat at 332 ms, which is 32 frames times
     about 10.4 ms per frame, so one 20 MP P1 LL frame costs the engine
     about 10.4 ms). Upstream, the ingress pending count sat at the soft
     cap of 32 for the whole run with 5,261 copy fallbacks and zero
     camera drops, zero starvation and zero cap skips (pending high-water
     37 of the hard cap 48), so the fallback did exactly what it was built
     for and acquisition never noticed. The run failed the verifier with
     "external recorder single-clip outputs are incomplete", which is the
     designed outcome. So one GA107 engine is at 96 percent of the stream:
     split-GOP on the A16 is required, and its two-shard form leaves each
     engine at 48 percent duty, which is the headroom every clean run in
     the review document has been running on.
  2. The engine-only specs on the card (one camera, then two
     synchronized) for the graph and the true direct-read floor.
  3. A two-camera registered spec with the recorder on the same GPU: one
     session per camera if step 1 said an engine holds 100 fps, two
     alternating sessions on the same GPU (single-GPU split-GOP placement)
     if not.
  4. The crop interleave spec with the crop recorder on the same GPU.
  5. The 30-minute endurance with everything on.

### C. Network multicast: feed two GPUs without a host bounce (to evaluate)

Asked 2026-09-05: could the camera, NIC or SDK broadcast a frame to two
places at once, so the A6000 detects while an A16 records, with no copy
between them? Inside a host there is no free fan-out: every copy of a
20 MB frame is a read and a write against a finite memory or PCIe
bandwidth, and neither the SDK nor CUDA exposes PCIe-level multicast. The
pipeline already broadcasts the *reference* to a frame at zero cost (the
refcounted worker entry every consumer retains); bytes only ever need
copying when a memory domain is crossed, which is the whole story of this
document.

The one real broadcast is on the network. GigE Vision streams over UDP,
and the protocol supports multicast: the camera sends each packet **once**,
addressed to a multicast group rather than to one NIC, and an Ethernet
switch replicates the stream to every port that has joined the group.
The camera does not send twice, and it does not know how many receivers
there are; one application holds the control channel and the others open
the camera as receivers. So the topology this needs, per camera:

- a switch between the camera and the host (today each camera is cabled
  point-to-point to its own NIC on its own subnet, so there is no switch
  to replicate anything);
- one host NIC port per destination GPU that has joined the group, each
  with GPUDirect into its GPU: one port landing frames in an A16 die for
  recording, a second port landing the same frames in the A6000 for
  detection. Two destinations means two ports per camera, so yes, the
  port count doubles;
- aggregate bandwidth: about 16 Gbit/s per camera (20 MB at 100 fps),
  twice per camera with two receivers, 128 Gbit/s across the switch and
  NICs for four cameras;
- two SDK receivers per camera, each reassembling packets on the CPU, so
  roughly twice the receive-side CPU work, and PTP timestamps that both
  receivers see identically (they come from the camera, so they should).

What it buys: detection on the A6000 at the measured 0.9 ms floor for one
camera (or the batched-inference floor for four) with the recording path
on the A16s untouched, no host bounce, and no P2P requirement between the
two GPUs. What it does not change: the A6000 still has one NVENC, so
recording stays on the A16s with everything in the review document as
it is; and detections must reach the recorder's metadata and the crop
producer, which today happens in one process; with two receivers the
detection results would cross processes (a few kilobytes per frame, the
existing IPC handles it) and the crop would be cut on the A16 from its
own copy of the frame using boxes produced on the A6000.

Open questions, in the order to check them: whether the Emergent SDK
supports GigE Vision multicast / receiver mode on these cameras at all
(the camera manual and the SDK headers decide this; `EVT_CameraOpen`
today takes control of the device); whether the rig's NICs and a
100 GbE switch can carry 128 Gbit/s with GPUDirect on every receiving
port; and whether the receive-side CPU cost of two reassemblies per
camera fits next to everything else. None of it needs building to be
answered; it is a day of reading and a switch on the bench. If the
answer is yes, it is the only design that gives the A6000's inference
floor without the two-card purchase.

## Why not route frames from the A16s to a big GPU for inference

Decided with Jeremy on 2026-09-05 and recorded so the option is not
re-argued; the reasons were checked against the measurements before
being written down.

1. **Topology.** No peer access between the A16 dies and the A6000, and
   one NVENC on the A6000: every frame needs two host hops of about
   1.7 ms each, and recording cannot leave the A16s.
2. **The copy sits on the detect path in the obvious variant.** With the
   cameras cabled as they are, frames land on the A16 dies first, so the
   A16-to-host-to-A6000 transfer happens *before* detection: 3.4 ms of
   copying, more than the entire 2.2 ms the pipeline delivers today. The
   only escape is RDMA straight into the A6000, which moves the copies to
   the recording side, off the detect path, but then pushes 8 GB/s in and
   8 GB/s out of the A6000's single PCIe link for four cameras, in
   contention with the graph on that GPU.
3. **Orchestration.** Staging and ordering four streams' transfers
   against a recorder that does not own them is real engineering.

Not a reason, and an earlier draft stated it wrongly: the cost of a
batch-four engine. Batch four is cheaper per frame than four separate
graphs (about 1.0 to 1.2 ms for all four, roughly a tenth of the A6000,
against 1.43 ms measured for three unbatched cameras); what is expensive
about batching is the scheduler and the tail risk when frames do not
arrive together, not the compute. If the big-GPU direction is ever taken
up, it is design B (each card owns its cameras end to end) or design C
(multicast, if the SDK allows it), not the host-bounce path.

## What stays on the A16s meanwhile

The tree's defaults (event sync, deferred PTP latch, detect-priority
gate, registered source with copy fallback, copy after detection, GOP-parity
crop interleaving, encoder prewarm at hello, the cap and crop-count
verifier checks) and the roadmap in the review document: four-camera runs
when 2010096 is back, a real animal, the GUI smoke, and the engine (INT8
or input size) as the only A16-side lever with more than 0.1 ms in it.
