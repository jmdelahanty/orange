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
