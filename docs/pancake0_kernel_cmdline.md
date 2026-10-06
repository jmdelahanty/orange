# pancake0 kernel command line and clocksource

Recorded 2026-09-17, before adding `tsc=reliable`. This is the state of the
rig host as booted since 2026-08-09 (`uptime -s`), kernel
`6.5.0-44-generic` (Ubuntu 22.04 HWE), NVIDIA driver 535.183.06 (runfile,
DKMS), CUDA 12.2, Mellanox OFED 24.01 (DKMS), Secure Boot off.
`docs/install_linux_cuda_eSDK.md` still shows the original
`GRUB_CMDLINE_LINUX_DEFAULT="quiet splash pci=realloc=off"`; the live
configuration has since gained the CPU isolation options below.

## /etc/default/grub as of 2026-09-17

```
GRUB_CMDLINE_LINUX_DEFAULT="quiet splash pci=realloc=off ipv6.disable=1 isolcpus=1,2,6,8,10,12,38,40,42,44 nohz_full=1,2,6,8,10,12,38,40,42,44 rcu_nocbs=1,2,6,8,10,12,38,40,42,44"
GRUB_CMDLINE_LINUX="amd-iommu=on pci=nommconf"
```

Running command line (`/proc/cmdline`):

```
BOOT_IMAGE=/boot/vmlinuz-6.5.0-44-generic root=UUID=c60c38ed-2574-4698-be88-7c06aeef8582 ro iommu=pt pci=nommconf quiet splash pci=realloc=off ipv6.disable=1 isolcpus=1,2,6,8,10,12,33,34,38,40,42,44 nohz_full=1,2,6,8,10,12,33,34,38,40,42,44 rcu_nocbs=1,2,6,8,10,12,33,34,38,40,42,44 tsc=reliable vt.handoff=7
```

(as of the 2026-10-06 17:44 reboot; the earlier line had `amd-iommu=on` and
the 10-CPU isolated set.)

| Option | Why it is there |
|---|---|
| `amd-iommu=on` | IOMMU for the EVT GPUDirect / Rivermax path on a Threadripper PRO host |
| `pci=nommconf` | PCI config access through the legacy mechanism; part of the original EVT eSDK install recipe |
| `pci=realloc=off` | keeps the firmware's BAR assignment (large BARs for the A16 dies and the Mellanox NICs) |
| `ipv6.disable=1` | camera links are IPv4-only |
| `isolcpus=…`, `nohz_full=…`, `rcu_nocbs=…` (CPUs 1,2,6,8,10,12,33,34,38,40,42,44 since 2026-10-06; 1,2,6,8,10,12,38,40,42,44 before) | Core roles (record of intent: pancakebatter `hosts/pancake0/config.yml` `kernel_tuning.core_roles`, verified by its `check_kernel_tuning.py`): **6, 8, 10, 12** = Orange's per-camera YOLO worker threads (`ORANGE_YOLO_AFFINITY_CAM_<serial>`: 2010093 6, 2010094 8, 2010095 10, 2010096 12; the thread that also launches the fused graph carrying the pose network), **38, 40, 42, 44** = their SMT siblings, kept idle; **1, 2** = Citrus (stimulus render thread on 1, arena update loop on 2, pinned via `citrus_runtime.threading` in `citrus/system_config.yml`), **33, 34** = their siblings, added 2026-10-06 so Citrus owns whole cores like the YOLO threads. Orange pins nothing else: the acquisition, pose-decode, crop and recorder threads float on the housekeeping CPUs (the earlier claim here that acquisition threads were pinned was wrong). The GUI validation requires the options (`ORANGE_GUI_REQUIRE_KERNEL_CMDLINE_OPTIONS=isolcpus,nohz_full,rcu_nocbs`) and that `6,8,10,12,38,40,42,44` are within the isolated set. Headless runs do not pin. |
| `vt.handoff=7` | added by GRUB for the graphical console |

CPU: AMD Ryzen Threadripper PRO 5975WX, 32 cores / 64 threads, governor
`schedutil`, `acpi_idle` with C1 and C2 enabled, power-profiles-daemon
`balanced`.

## The clocksource incident of 2026-09-11

`journalctl -k`:

```
Sep 11 07:03:06 kernel: clocksource: Clocksource 'tsc' skewed -485515617 ns (-485 ms) over watchdog 'acpi_pm' interval of 485515617 ns (485 ms)
Sep 11 07:03:06 kernel: tsc: Marking TSC unstable due to clocksource watchdog
Sep 11 07:03:06 kernel: TSC found unstable after boot, most likely due to broken BIOS. Use 'tsc=unstable'.
Sep 11 07:03:06 kernel: clocksource: Switched to clocksource acpi_pm
```

The CPU advertises `constant_tsc nonstop_tsc rdtscp`; the skew equals one
watchdog interval, i.e. a single glitchy comparison, not a drifting
counter. Since then every clock read costs about 1.95 us (measured with
`time.perf_counter_ns` in a loop) instead of tens of nanoseconds, and the
reads serialise on one I/O port across all CPUs, so every host-side
latency segment of the analytics pipeline is inflated by microseconds and
the inflation grows with thread count. The effect on the detect latency
figures is quantified in `docs/handoff_pose_analytics_2026_09_16.md`
(addenda of 2026-09-17: two-camera engine-only acquisition-to-detect 2.15
on 2026-09-04 vs 2.26 on 2026-09-12 and after, same binary; four cameras
2.47). While `acpi_pm` is active the sysfs node
`/sys/devices/system/cpu/clocksource/clocksource0/current_clocksource`
is absent on this kernel, so check the kernel log instead.

## Planned change: `tsc=reliable`

Append `tsc=reliable` to `GRUB_CMDLINE_LINUX_DEFAULT` (everything else
unchanged), then `sudo update-grub && sudo reboot`. `tsc=reliable` tells
the kernel to trust the TSC and stop the clocksource watchdog from
demoting it; the TSC itself is re-validated at boot. Verify after boot:

```
grep -o 'tsc=reliable' /proc/cmdline
cat /sys/devices/system/cpu/clocksource/clocksource0/current_clocksource   # expect: tsc
journalctl -k | grep -i clocksource                                          # no "unstable" line
```

Then run the four-camera preflight (`orange-evt-stream-smoke --all
--frames 5`) before the first analytics run: the GPUDirect path needs
`nvidia_peermem` rebuilt by DKMS against the OFED modules for the same
kernel (it has on every boot so far). Every latency baseline taken between
2026-09-12 and this reboot must be re-measured afterwards.

## How to check before trusting any host-side timing

1. `journalctl -k | grep -iE 'clocksource|tsc'` shows the TSC as current
   and no "Marking TSC unstable" line since boot.
2. A clock read costs under 100 ns:
   `python3 -c "import time;n=200000;t=time.perf_counter_ns();[time.perf_counter_ns() for _ in range(n)];print((time.perf_counter_ns()-t)/n,'ns')"`.
3. In a two-camera engine-only run, `acquisition_to_worker_start_ms` is
   about 0.02 and `cpu_post_sync_ms` about 0.06 (the 2026-09-04 values).
