# Pancake0 PCIe, NIC, GPU, And NUMA Topology Notes

Date: 2026-04-19

This note records what we know about the current `pancake0` host topology while
debugging two-camera 100 FPS split-GOP recording failures.

## Host Summary

- System vendor: Puget Systems
- System product: AMD Threadripper PRO WRX80 4U V2
- Motherboard: ASUSTeK Pro WS WRX80E-SAGE SE WIFI Rev 1.xx
- CPU: AMD Ryzen Threadripper PRO 5975WX, 32 cores / 64 threads
- OS-visible NUMA nodes: 1

Linux currently exposes this machine as a single NUMA node:

```text
numactl --hardware:
available: 1 nodes (0)
node 0 cpus: 0-63

/sys/devices/system/node/online:
0

lscpu:
NUMA node(s): 1
NUMA node0 CPU(s): 0-63
```

The PCI devices we checked report `numa=-1` and `local_cpus=0-63`, so the OS is
not publishing useful per-device CPU locality hints. `numactl` CPU or memory
binding may still reduce scheduling jitter, but it is not expected to fix
NIC-to-GPU PCIe locality.

If we need deeper visualization later, `hwloc` / `lstopo-no-graphics` can show
the topology Linux sees. It cannot reveal extra NUMA nodes unless firmware
exposes them to the kernel.

## GPU Layout

`nvidia-smi --query-gpu=index,pci.bus_id,name --format=csv,noheader` reported:

```text
0, 00000000:01:00.0, NVIDIA RTX A6000
1, 00000000:2F:00.0, NVIDIA A16
2, 00000000:30:00.0, NVIDIA A16
3, 00000000:31:00.0, NVIDIA A16
4, 00000000:32:00.0, NVIDIA A16
5, 00000000:45:00.0, NVIDIA A16
6, 00000000:46:00.0, NVIDIA A16
7, 00000000:47:00.0, NVIDIA A16
8, 00000000:48:00.0, NVIDIA A16
```

The useful A16 topology groups are:

- `GPU1..GPU4`: mutually `PIX`
- `GPU5..GPU8`: mutually `PIX`
- `GPU1..GPU4` to `GPU5..GPU8`: `SYS`
- no NVLink path is present

For split-GOP, source and helper GPUs should stay inside one `PIX` group.

## NIC Layout

The active camera-facing ports before recabling were:

```text
mlx5_0 port 1 ==> mlnx1_p1_25g (Up)    192.168.110.1/24
mlx5_1 port 1 ==> mlnx1_p2_25g (Up)    192.168.120.1/24
mlx5_2 port 1 ==> mlnx1_p3_25g (Up)    192.168.130.1/24
mlx5_3 port 1 ==> mlnx1_p4_25g (Up)    192.168.140.1/24
```

The second ConnectX-7 card was present but down:

```text
mlx5_4 port 1 ==> mlnx2_p1_25g (Down)
mlx5_5 port 1 ==> mlnx2_p2_25g (Down)
mlx5_6 port 1 ==> mlnx2_p3_25g (Down)
mlx5_7 port 1 ==> mlnx2_p4_25g (Down)
```

From the observed PCIe tree:

- `mlnx1_*` ports are on bus `61:00.*`
- `mlnx2_*` ports are on bus `49:00.*`
- `GPU1..GPU4` are on buses `2F:00.0` through `32:00.0`
- `GPU5..GPU8` are on buses `45:00.0` through `48:00.0`

From `nvidia-smi topo -m`:

- active `mlnx1_*` / `NIC0..NIC3` ports are `SYS` to all A16 GPUs
- inactive `mlnx2_*` / `NIC4..NIC7` ports are `PHB` to `GPU5..GPU8`
- inactive `mlnx2_*` / `NIC4..NIC7` ports are `SYS` to `GPU1..GPU4`

## Practical Interpretation

The current split-GOP validation that checks source-to-helper GPU topology is
necessary but incomplete.

For example, the recent two-camera configuration had good source/helper GPU
pairs:

- camera `2010095`: source `GPU1`, helper `GPU2`, source/helper path `PIX`
- camera `2010096`: source `GPU5`, helper `GPU6`, source/helper path `PIX`

But both cameras were coming in through `mlnx1_*`, which is `SYS` to all A16
GPUs. That means the camera ingress path may be remote even when the helper pair
itself is local.

This matters because the failure appears to scale with helper copy payload size:

- routing and helper scheduling alone did not reproduce the collapse
- very small helper copies stayed healthy
- larger helper copies caused frame-ID jumps and throughput collapse near the
  helper-owned GOP transition

The strongest current hypothesis is that the system is sensitive to combined
camera ingress DMA plus source-to-helper copy pressure when the camera NIC is not
local enough to the source GPU.

## Best Cable-Only Topology Experiment

The best next hardware experiment is to move cameras onto `mlnx2_*` ports and
use source/helper GPUs from `GPU5..GPU8`.

Target shape:

- camera A -> `mlnx2_p1_25g` / `NIC4` -> source `GPU5`, helper `GPU6`
- camera B -> `mlnx2_p2_25g` / `NIC5` -> source `GPU7`, helper `GPU8`

Expected topology:

- camera NIC to source GPU: `PHB`
- source GPU to helper GPU: `PIX`

This is not perfect, but it is materially better than `SYS` camera ingress plus
`PIX` helper copy. If this improves the 100 FPS two-camera run, we should add
explicit NIC-to-source-GPU locality to our preflight model instead of validating
only source-to-helper GPU pairs.

## Checks After Recabling

After moving cables and assigning IPs, capture:

```bash
ibdev2netdev
ip -br addr
nvidia-smi topo -m
nvidia-smi --query-gpu=index,pci.bus_id,name --format=csv,noheader
```

For each camera IP, also check which interface Linux routes through:

```bash
ip route get <camera-ip>
```

We should only rerun the 100 FPS two-camera split-GOP experiment after those
routes confirm the cameras are using `mlnx2_*`.

## Recabling Progress

### 2026-04-20: Camera `2010096` moved to `mlnx2_p4_25g`

Camera `2010096` was moved from the old `mlnx1_*` side onto `mlnx2_p4_25g`.

Observed host state:

```text
mlx5_3 port 1 ==> mlnx1_p4_25g (Down)
mlx5_7 port 1 ==> mlnx2_p4_25g (Up)

mlnx1_p4_25g DOWN 192.168.140.1/24
mlnx2_p4_25g UP   192.168.180.1/24
```

The moved camera was visible on the new subnet:

```text
ip neigh show dev mlnx2_p4_25g:
192.168.180.2 lladdr e0:55:97:1e:ab:f0 STALE

ip route get 192.168.180.2:
192.168.180.2 dev mlnx2_p4_25g src 192.168.180.1
```

A short ping from `192.168.180.1` through `mlnx2_p4_25g` to `192.168.180.2`
succeeded. Orange camera discovery was then run manually and confirmed the
camera is up through the tool.

This validated the first half of the recabling strategy.

### 2026-04-20: Camera `2010095` moved to `mlnx2_p3_25g`

Camera `2010095` was moved onto `mlnx2_p3_25g`.

Observed host state:

```text
mlx5_2 port 1 ==> mlnx1_p3_25g (Down)
mlx5_6 port 1 ==> mlnx2_p3_25g (Up)

mlnx1_p3_25g DOWN
mlnx2_p3_25g UP   192.168.170.1/24
```

The moved camera was visible on the new subnet:

```text
ip neigh show dev mlnx2_p3_25g:
192.168.170.2 lladdr e0:55:97:1e:ab:ef STALE

ip route get 192.168.170.2:
192.168.170.2 dev mlnx2_p3_25g src 192.168.170.1
```

A short ping from `192.168.170.1` through `mlnx2_p3_25g` to `192.168.170.2`
succeeded.

Current recabled camera layout:

- `2010095` -> `mlnx2_p3_25g`, host `192.168.170.1`, camera `192.168.170.2`
- `2010096` -> `mlnx2_p4_25g`, host `192.168.180.1`, camera `192.168.180.2`

Orange camera discovery confirmed both cameras are found and `2010095` /
`2010096` are bound to the expected `mlnx2_*` NICs.

The next two-camera split-GOP stress test should use source/helper GPUs entirely
from the `GPU5..GPU8` `PIX` group.

### 2026-04-20: Recabled two-camera preprocess-only test passed

After rebooting to clear stale CUDA contexts, the two-camera recabled topology
probe passed:

```text
experiment_id:
2010095_2010096_split_gop_hevc_100fps_preprocessonly_mlnx2_gpu5_8_freerun2

artifact:
/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_preprocessonly_mlnx2_gpu5_8_freerun2
```

Test shape:

- `recording_sink_mode = preprocess_only`
- `sync_mode = free_run`
- `2010095`: `mlnx2_p3_25g`, source `GPU7`, helper `GPU8`
- `2010096`: `mlnx2_p4_25g`, source `GPU5`, helper `GPU6`
- full-frame helper copy path active
- no mux / final video output in this diagnostic run

Run summary:

- `pass_runs = 1`
- `fail_runs = 0`
- both cameras completed at about `100 fps`
- `dropped_frames_camera = 0`
- `pre_drops_final = 0`
- `enc_fail_final = 0`
- `helper_fallback_frames_final = 0`

Pipeline CSV final counters showed the real helper path was exercised:

```text
Cam2010095: submitted=1402 primary=702 helper_requested=700 helper_dispatched=700
Cam2010096: submitted=1402 primary=702 helper_requested=700 helper_dispatched=700
```

The acquisition cadence probes showed no frame-ID jumps in the `80..160` probe
window:

```text
Cam2010095: jumps=0, receive_delta_ns range 9.73 ms .. 10.32 ms
Cam2010096: jumps=0, receive_delta_ns range 9.55 ms .. 10.38 ms
```

The first helper-owned GOP started at recording frame `101` for both cameras and
remained healthy:

```text
Cam2010095 frame 101 -> target GPU8, helper_requested=1, receive_delta ~= 10.04 ms
Cam2010096 frame 101 -> target GPU6, helper_requested=1, receive_delta ~= 10.07 ms
```

This is the strongest evidence so far that the previous two-camera 100 FPS
failure was topology-sensitive. Moving both camera ingress paths from the
`mlnx1_*` `SYS` side to the `mlnx2_*` side near `GPU5..GPU8`, while keeping
source/helper pairs `PIX`, removed the stale-frame onset in the same
preprocess-only helper-copy diagnostic.

### 2026-04-20: Recabled two-camera real-recording test produced videos but was not fully clean

The same recabled topology was then tested with real encode / mux output:

```text
experiment_id:
2010095_2010096_split_gop_hevc_100fps_real_mlnx2_gpu5_8_freerun1

artifact:
/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_real_mlnx2_gpu5_8_freerun1
```

Test shape:

- `recording_sink_mode = real`
- `sync_mode = free_run`
- `2010095`: `mlnx2_p3_25g`, source `GPU7`, helper `GPU8`
- `2010096`: `mlnx2_p4_25g`, source `GPU5`, helper `GPU6`
- full-frame helper copy path active
- MP4 output enabled

The run completed and produced both videos:

```text
Cam2010095.mp4: 264,168,821 bytes, 14.02 s, ~150.7 Mbps
Cam2010096.mp4: 264,568,646 bytes, 14.02 s, ~151.0 Mbps
```

The helper path stayed active and did not fall back:

```text
Cam2010095: submitted=1402 primary=702 helper_requested=700 helper_dispatched=700 helper_fallback=0
Cam2010096: submitted=1402 primary=702 helper_requested=700 helper_dispatched=700 helper_fallback=0
```

Pipeline health was mostly good:

- `pre_drops_final = 0`
- `enc_fail_final = 0`
- `acq_starve_final = 0`
- A16 GPU memory released cleanly after the run

But this run was not fully clean:

```text
Cam2010095: dropped_frames_camera = 0
Cam2010096: dropped_frames_camera = 17
```

The `2010096` camera drop counter first appeared around the middle of the run
after repeated `EVT_CameraGetFrame Error, 12` messages. The acquisition cadence
probe window `80..160` still showed zero frame-ID jumps for both cameras, so
the observed drop onset was outside that narrow probe window.

Interpretation:

- the recabled topology is strong enough for dual-camera full-frame helper-copy
  routing at 100 FPS,
- real encode / mux output can complete and produce videos,
- but the full real-recording path needs repeat testing and stricter policy
  handling before being called validated.

Follow-up:

- repeat the real-recording run once to check whether the `2010096` camera drop
  is reproducible,
- headless experiment policy now fails by default when
  `dropped_frames_camera > 0` through `require_zero_camera_drops=true`,
- acquisition now emits `Cam<serial>_acquisition_drop_events.csv` so late-run
  frame-ID gaps and `EVT_CameraGetFrame` errors are visible even when the
  low-rate cadence probe misses the onset.

## Follow-Up Engineering

If the recabled test helps, add a config/preflight concept for camera ingress
locality. The likely shape is:

- camera config records the expected ingress interface or NIC identity
- preflight resolves camera IP to an egress interface with `ip route get`
- preflight maps that interface to a PCI bus through `/sys/class/net/.../device`
- preflight compares NIC-to-source-GPU topology from `nvidia-smi topo -m`
- source/helper `PIX` validation remains required for split-GOP

This keeps the validation honest: split-GOP is not just a GPU pair problem. It
is a camera NIC -> source GPU -> helper GPU topology problem.
