# ENet Startup Troubleshooting

## Symptom

On Orange startup, the terminal could flood with repeated lines like:

```text
Unable to service network: Network not initialized!
```

This could continue until the GUI was closed.

## Root Cause

The GUI app had two separate bugs in its ENet startup path:

1. It did not call ENet's global `enet_initialize()` before trying to create
   the host.
2. It started the ENet service thread even when host initialization failed.

That meant the service loop could run against an uninitialized `EnetContext`,
which caused repeated `service_network(...)` failure prints.

## Fix

The GUI startup path now:

- calls ENet's global `enet_initialize()` first
- only starts the ENet thread if host initialization succeeds
- releases the host with `enet_release(...)` on shutdown
- calls `enet_deinitialize()` on shutdown when runtime init succeeded

Current expected behavior:

- if ENet initializes successfully, the normal ENet thread starts
- if ENet runtime init fails, Orange prints one clear message and disables ENet
- if ENet host init fails, Orange prints one clear message and does not start
  the ENet thread

Example messages:

```text
[ENet] Global initialization failed; networking disabled.
[ENet] Host initialization failed; ENet thread not started.
```

## Operational Impact

This issue was terminal-noise and startup-path correctness, not a camera/PTP
timing issue.

It affected:

- YOLO ENet output / external ENet consumers
- any future control/state traffic relying on the GUI ENet host

It did not affect:

- local frame IPC over shared memory
- camera streaming itself
- PTP host stack startup

## If The Problem Reappears

Check:

1. whether Orange prints one of the explicit `[ENet] ...` failure messages
2. whether port `3333` is already in use
3. whether the ENet runtime/library is available in the current launch
   environment

If startup is clean, you should no longer see repeated
`Unable to service network: Network not initialized!` spam.
