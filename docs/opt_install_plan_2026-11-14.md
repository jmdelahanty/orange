# Installing Orange under /opt: plan (2026-11-14)

Status: **plan only.** Nothing described here has been built or installed.
Jeremy's decisions of 2026-11-14 are in section 7. Items still marked
**Decision** need Jeremy.

## 1. Goal and what is already decided

Goal: anyone allowed to run the rig starts Orange from one versioned,
root-owned install. Nobody runs a binary from someone's home checkout, every
run records exactly which release it used, and a bad release is undone in
one command.

Decided by Jeremy (2026-10-28, rig-designer session; recorded in
`~/phd-projects/rigs/ledger.md`):

- One operator at a time.
- Jeremy is the only full-sudo admin. Other users get passwordless sudo only
  to run Orange.
- Configs are per user. Calibrations are Jeremy's, shared read-only.
- Nobody writes to `/groups` directly.

Proposed before, never confirmed:

- `/opt/<program>/<version>`, installed from tagged versions, with the sudo
  rule pinned to the installed copy (pancake-batter, 2026-11-03).
- File capabilities on the binary instead of sudo (Orange session,
  2026-10-28). Unproven; it needs a non-root camera-open test.

Sibling patterns on this host:

- `/opt/recording-transfer/<version>/` with an `INSTALLED.json` manifest
  (package, version, source tag and commit, artifact sha256, installer,
  time). The Orange pre-check pins a version and keeps older ones for
  rollback.
- Citrus installs in-tree (`targets/citrus`) from released main and keeps
  `targets/citrus.prev-<sha>` copies for rollback.

## 2. Today (checked 2026-11-14)

- **Binaries.** Built in the main checkout
  `/home/jeremy/orange/targets/release/`. Four sudo wrappers in
  `/usr/local/bin` (gui-validation, local-benchmark, evt-stream-smoke,
  gdb-headless-bt) accept only binaries there, and launcher scripts pass
  `--orange-bin`/`--orange-client`.
- **What `/opt/orange` already holds:**
  - `bin/external_recorder_ipc_probe_native`, plus its `.ptx` and a `.bak`
    of the previous recorder;
  - `lib/ffmpeg-nvidia`, `lib/opencv` and `lib/tensorrt-oss`. The last is
    owned by jeremy and should become root-owned.
- **Linking is already install-ready.** The RUNPATH holds only absolute
  system paths: `/opt/EVT/eSDK/lib`, `/usr/local/cuda/lib64`,
  `/opt/orange/lib/{ffmpeg-nvidia,opencv}/lib` and
  `/usr/local/TensorRT-10.0.1.6/lib`. `ldd` finds every library for
  `orange` and `orange_client`. Copying the binaries elsewhere works
  unchanged.
- **Files read from the source tree at run time.** These are what must
  move:
  - `fonts/Roboto-Regular.ttf` and `fonts/forkawesome-webfont.ttf`, relative
    to the current folder (`src/gx_helper.h`). The GUI only works when
    started from the repo root.
  - `scripts/validate_gui_ptp_recording.py`, the post-finalize validator. It
    is found from the current folder, or from the binary path as
    `<tree>/targets/release/orange` -> `<tree>`
    (`src/gui/post_finalize_validation.cpp`, `repo_root()`).
  - `scripts/verify_external_recorder_session.py`, relative to the current
    folder (`src/external_recorder_contract_utils.h`).
  - `scripts/external_video_sanity.py` for the headless client
    (`resolve_headless_repo_relative_path`).
  - `scripts/ptp_stack.sh` and `scripts/host_stall_monitor.py`, run by the
    GUI wrapper from the tree root.
- **Source version.** Read at run time by running `git` on the worktree found
  from the current folder or the binary (`build_source_version_snapshot()`,
  `src/project.cpp`). An installed binary has no `.git` next to it, so every
  run would record "git worktree not found". The launcher's
  `ORANGE_GUI_REQUIRE_SOURCE_VERSION=1` would then refuse.
- **Data root, resolved two ways:**
  - `build_default_orange_root_dir()` uses the home of `SUDO_USER`, then
    `HOME`, then the passwd entry.
  - `src/recording_physical_registration.cpp` and the post-finalize
    validator use `HOME` only. Under sudo that can be `/root`.
  - Python tools use `Path.home()`.
  - Launchers hard-code `/home/jeremy/orange_data/...`.
- **IPC permissions:**
  - SHAMAN shared memory `/dev/shm/shm_cam_*_v2` is `root:ipc 0660`;
    jeremy is in `ipc`.
  - `/run/ptp4l` is `root:root 0660`.
  - Orange's local-control sockets use fixed `/tmp` paths.
  - Citrus's control socket is jeremy's, mode 0600 (Citrus side).

## 3. Proposed layout

```
/opt/orange/                      root:root 0755, nothing user-writable
  releases/
    2026.11.14-43eef19/           <date>-<short commit>; one release per install
      bin/        orange orange_client evt_stream_smoke evt_camera_ready
                  evt_force_reboot physical_target_fit_cli
                  external_recorder_ipc_probe_native native_nv12_write.ptx
      share/orange/
        fonts/    scripts/  (the run-time subset, see 4b)  docs/schemas/  contracts/
      INSTALLED.json              same shape as /opt/recording-transfer: source
                                  repo/branch/commit/describe, clean-tree flag,
                                  CMake cache settings, toolchain versions (CUDA,
                                  TensorRT, EVT SDK, driver), ctest summary,
                                  installer, installed_at, installed_by
      SHA256SUMS                  every file in the release
  current  -> releases/<active>   flipped atomically (rename of a new symlink)
  previous -> releases/<prior>
  lib/                            third-party, unchanged (tensorrt-oss -> root)
  bin/external_recorder_ipc_probe_native -> ../current/bin/...   compatibility
                                  link so the app config and specs keep working
```

- Releases are immutable once installed. A fix is a new release.
- Keep every release; nothing is removed automatically (decided 2026-11-14).
  At about 100-200 MB per release, this is cheap for a long time.
- The source tree stays the development home. Agents build and test there as
  today, and only an install puts a build into `/opt`.

## 4. Changes needed, in order

Each change is a normal dish-branch change with tests, promoted on Jeremy's
word. None needs the rig until step 5.

**a. Build-time version stamp.**
- CMake writes `build_info.h`: commit, describe, branch and dirty flag at
  configure and build time.
- `build_source_version_snapshot()` falls back to it, plus the release's
  `INSTALLED.json`, when no worktree is found. It records `install_root` and
  `release_id` beside the commit.
- The launcher's source-version requirement accepts an installed stamp, and
  refuses a dirty one.

**b. Install-relative support files.**
- One `orange_share_dir()` tries, in order:
  - `<exe>/../share/orange` (installed);
  - `<exe>/../../..` (`targets/release` in a tree);
  - the current folder (last, for compatibility).
- The fonts, the post-finalize validator, the recorder verifier and
  `external_video_sanity.py` all use it, so the GUI no longer depends on the
  folder it starts in.
- The run-time script subset is listed in one file,
  `scripts/runtime_files.txt`, which the installer copies and a test checks
  against what the code references.

**c. One data-root resolver.** One function in C++, mirrored by one Python
helper, used everywhere. It replaces:
- the `HOME`-only paths in `recording_physical_registration.cpp` and
  `post_finalize_validation.cpp`;
- `Path.home() / "orange_data"` in the scripts;
- the hard-coded `/home/jeremy/orange_data` in the launchers.

The rule:
- **User root**, for the person who started Orange: camera configs, models,
  specs and personal settings.
  - Under sudo, that is `SUDO_USER`'s home from the passwd database;
    otherwise the real uid's home from passwd, not `HOME`.
  - Root's own home is never used. Root without `SUDO_USER` refuses to start
    rather than guess.
  - Path: `<home>/orange_data`.
- **Per-person data on the data drive** (Jeremy, 2026-11-14):
  `/mnt/Data2/<username>/<rig>/orange/`, for that person's recordings and run
  outputs.
  - `<username>` is the account that started Orange (`SUDO_USER`).
    Orange already writes files as that user (6af7c8b).
  - Open: one person can have two accounts, for example the local `jeremy`
    and the HHMI `delahantyj`. Which identity names the folder?
- **Rig-wide shared data:** `/mnt/Data2/rigs/<rig>/calibrations`, written by
  whoever commissions the rig and readable by every operator. Every operator
  must use the rig's commissioned calibration, so it is not per person.
- **Where these paths come from:** the root-owned machine description that
  pancakebatter installs, `/etc/pancakebatter/host.yml` `paths:`. It already
  has `data_root: /home/jeremy/orange_data`, and its comment says the username
  stays embedded until the shared-machine migration. Add the drive root and
  the per-person and rig patterns there, via pancake-batter's installer.
  - Orange already pins `/etc/pancakebatter` documents into every recording
    (`session.rig_config`), so a run records the paths it used.
  - A user cannot redirect them.
- Today recordings go to `/mnt/Data2/orange_data/exp/unsorted` and
  calibrations to `~/orange_data/calibrations`. Moving the existing data is a
  separate, explicit step, not part of the resolver change.
- **`ORANGE_DATA_ROOT`** override for tests only. Release binaries started
  through the wrappers ignore it.

**d. Installer `scripts/install_orange_release.sh`.** Jeremy runs it with
sudo.
- Refuses unless:
  - the checkout is clean, at a commit that is on the fork;
  - no `orange`, `orange_client` or recorder process is running (as Citrus
    installs do);
  - the HOLD/CLEAR protocol has cleared the host.
- Builds in a fresh build folder with the production cache settings
  (Release, sm_86, the `/opt/orange` libs, TensorRT 10.0.1.6). Builds every
  shipped target, runs ctest, and refuses on any failure.
- Stages into `releases/<id>.new`, then writes `INSTALLED.json` and
  `SHA256SUMS`, then `chown -R root:root` and `chmod -R a-w`.
- Renames the staged folder to `releases/<id>`, moves `previous` to the old
  `current`, then flips `current`.
- Prints the summary to send to the other sessions: release, commit, sha256
  of `orange` and `orange_client`, and time.
- `--rollback` swaps `current` and `previous` with no build. `--list` shows
  the releases.
- Folds in the native recorder:
  `build_orange_native_recorder.sh --install` installs into the release
  being staged, not into `/opt/orange/bin`.

**e. Wrappers pinned to the install.**
- Each wrapper accepts `realpath` targets under `/opt/orange/releases/*/bin/`
  only after checking the release folder is root-owned and not writable by
  others. It defaults to `/opt/orange/current/bin/<binary>`.
- The wrapper takes `ptp_stack.sh` and `host_stall_monitor.py` from the
  release's `share/orange/scripts`, not from a home folder.
- Development builds stay runnable (decided 2026-11-14) through separate
  `orange-dev-*` wrappers. They accept only the dev-tree path
  `/home/jeremy/orange/targets/release`, and only jeremy's sudoers entry may
  run them. The release wrappers never accept a dev-tree binary.

**f. Operator entry points.**
- `/usr/local/bin/orange-gui` runs the release's GUI launcher, and
  `/usr/local/bin/orange-headless` its headless launcher. Both use the
  release's binaries and scripts, and the user's data root.
- Specs and camera configs stay data: they move with the data root, not with
  the release.

## 5. Rollout

1. **P0 decisions** (section 7).
2. **P1: code a-c on dish.**
   - Tests: the version stamp with no `.git`; the share-dir lookup from a fake
     install layout; the data-root order.
   - Rig check with the dev tree: one GUI validation run and one 60 s headless
     gate show nothing changed.
3. **P2: installer and the first release.**
   - Install release 1 next to the dev tree, under HOLD.
   - Run, all from `/opt/orange/current` through the new wrappers:
     - stream smoke;
     - the 60 s headless gate;
     - a GUI validation recording;
     - one guided calibration capture.
   - Compare each with the same run from the dev tree: identical
     `source_version` commit, frame counts, NIC counters and latencies.
   - Practise rollback once.
4. **P3: switch the defaults.**
   - Launchers, aliases (`r-orange`) and docs default to `/opt/orange/current`.
   - From here, every promotion that should reach the rig ends with an
     install.
   - Tell Citrus, pancake-batter and Palette the new binary path. Process
     matches on `targets/release/orange` in pancakebatter
     (`reset_camera_nics.py`, rig health) need `/opt/orange/` added.
5. **P4: multi-user** (section 6). Only after P3 has run for a while.

Every install follows the recording rules: HOLD to the other agents, no
recording in progress, nothing heavy during runs, and the summary sent
afterwards.

## 6. Multi-user (phase 4; mostly open)

- **Running Orange** (decided 2026-11-14): the cameras need root, as now,
  so Orange runs through the passwordless sudo wrappers, pinned to `/opt`,
  with `env_reset`, no `ORANGE_*` env passthrough and the wrapper's env
  allow-list. File capabilities are dropped.
  - Note: a sudo rule for Orange is effectively root for its holder unless
    every path Orange opens or runs is pinned. `ORANGE_PTP_PMC_PATH` is run
    as root by `popen`, and `ptp_stack.sh` starts ptp4l with `sudo -b`.
    These must take no user-controlled path.
- **Data** (decided 2026-11-14):
  - Camera configs, models, specs and settings are per user, in each user's
    `~/orange_data`.
  - Shared data lives on the data drive(s) in the rig root (section 4c):
    calibrations, readable by all and written by the admin, and recordings.
- **Accounts** (decided 2026-11-14):
  - Jeremy is the only full-sudo admin. Operators do not get their own broad
    sudo.
  - One sudoers rule for a group lets its members run the release wrappers as
    root with no password, and nothing else:
    `%orange-operators ALL=(root) NOPASSWD: /usr/local/bin/orange-gui, ...`
  - Users are HHMI domain accounts (for example `delahantyj`, uid 64406,
    home `/home/delahantyj@hhmi.org`). Permissions on this machine come from
    local groups, which domain accounts can join.
  - Adding a user:
    1. the domain account exists;
    2. add it to the local groups `orange-operators` and `ipc`;
    3. its `~/orange_data` is created on first run from the release's
       defaults;
    4. create its folder on the data drive.
  - Testing: `delahantyj` is already in the local `sudo` group (full sudo).
    It can test domain-account homes and per-person folders, but not the
    narrow operator rule. That needs an account without sudo.
- **IPC.**
  - Operators join group `ipc` for SHAMAN shared memory.
  - A per-boot `/run/orange/` (tmpfiles.d, group `orange`, 0770) for the
    local-control sockets and a single-operator lock (`flock`), replacing
    the fixed `/tmp` paths.
  - Citrus's socket mode is Citrus's change.
  - `/run/ptp4l` stays root-only; Orange reads PTP state through the
    wrapper.
- **One operator at a time.** The `/run/orange` lock refuses a second
  Orange, and the GUI shows who holds it.

## 7. Decisions

Decided by Jeremy, 2026-11-14:
- A separate jeremy-only dev wrapper; dev builds stay runnable for
  development.
- The rollout order as in section 5: code first, then an install alongside
  the dev tree with a rig comparison, then the defaults switch, then
  multi-user last.
- Every release is kept.
- The native recorder is part of the release; the installer builds both
  CMake projects.
- Camera configs and models are per user. Shared data roots live on the data
  drive(s). Operators get the group's narrow passwordless rule, not their own
  sudo.

- Releases are named by a git tag per install (`orange-v2026.11.14`, plus
  `.N` for a second install that day). The installer creates the tag, and it
  names the release folder. Pushing each tag to the fork follows the usual
  push-needs-Jeremy's-word rule.
- Sudo wrappers, not file capabilities: the cameras need root.
- Per-person data on the data drive at `/mnt/Data2/<username>/<rig>/orange/`.
  Paths come from the installed `/etc/pancakebatter/host.yml`.

Still open:
1. Which account names a person's folder when they have both a local and an
   HHMI account (`jeremy` and `delahantyj`).
2. The exact `paths:` keys in `host.yml`. pancake-batter owns the file and
   its installer.
3. A test account without sudo, for the operator rule.

## 8. Effort

- P1: about a day of agent work plus one short rig check.
- P2: about half a day plus a 1-2 hour rig session.
- P3: small.
- P4: the host.yml paths with pancake-batter, the operator group and sudoers rule (Jeremy), the per-person folders, and one test as a non-sudo operator.
