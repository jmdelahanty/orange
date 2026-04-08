# Headless Sudo Wrapper

Use this when the Emergent/NIC stack requires root for GPU-direct streaming and
you want to run local headless Orange experiments without typing a sudo
password each time.

## Wrapper

The repo includes a narrow wrapper script:

- `scripts/orange_local_benchmark_wrapper.sh`

It only allows:

- `orange_client --mode local --experiment-spec <spec>`

It only accepts spec files under:

- `/home/jeremy/orange-jeremy/experiment_specs`
- `/tmp`

After the run, it chowns the experiment output folder back to the invoking
user so artifacts are not left root-owned.

## Install

Install the wrapper as a root-owned executable:

```bash
sudo install -o root -g root -m 0755 \
  /home/jeremy/orange-jeremy/scripts/orange_local_benchmark_wrapper.sh \
  /usr/local/bin/orange-local-benchmark
```

Add a narrow sudoers entry for user `jeremy`:

```bash
printf 'jeremy ALL=(root) NOPASSWD: /usr/local/bin/orange-local-benchmark\n' | \
  sudo tee /etc/sudoers.d/orange-local-benchmark >/dev/null
sudo chmod 0440 /etc/sudoers.d/orange-local-benchmark
sudo visudo -cf /etc/sudoers.d/orange-local-benchmark
```

## Run

Smoke test:

```bash
sudo /usr/local/bin/orange-local-benchmark \
  /home/jeremy/orange-jeremy/experiment_specs/2010096_smoke_a6000.json
```

Full Block A:

```bash
sudo /usr/local/bin/orange-local-benchmark \
  /home/jeremy/orange-jeremy/experiment_specs/2010096_block_a_a6000.json
```

Temporary retry spec:

```bash
sudo /usr/local/bin/orange-local-benchmark /tmp/2010096_smoke_a6000_retry.json
```

## Notes

- This is intended for benchmark/experiment runs, not as a general root wrapper
  for arbitrary Orange commands.
- If the SDK ever stops requiring root, prefer running `orange_client` directly
  as the normal user again.
