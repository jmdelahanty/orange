# Upstreaming Assessment & Path — fork → JohnsonLabJanelia/orange

*2026-06-09. Companion to `CODE_REVIEW_2026-06-09.md` and
`COMPARATIVE_REVIEW_2026-06-09.md`. This document answers a harder question than
the earlier reviews: not "is the code good," but "can this work reach other
users," and what has to change for the answer to be yes.*

---

## 1. The hard numbers

| Measure | Value |
|---|---|
| Merge-base with `upstream/main` | `c7f5e48`, **2025-04-17** — 14 months ago |
| Fork commits ahead of upstream | **1,193** |
| Upstream commits never integrated into fork | **226** |
| Fork source size | ~115,100 lines (src/, incl. gui/) |
| Upstream source size | ~36,900 lines |
| Files differing or unique in `src/` | 184 paths |
| Largest fork files | `orange_headless_client.cpp` (10,354), `orange.cpp` (9,420), `spatial_layout_ui.cpp` (7,082) |
| Upstream identity | Published instrument: Zenodo DOI, CITATION.bib, v2.1.0, external docs site |

## 2. The honest mergeability verdict

**This fork cannot merge upstream as a unit, and that will not change with more
polish.** No maintainer of a 37k-line published, citable instrument can review a
+1,193-commit change that triples their codebase, replaces their threading layer,
deletes their multi-machine GUI and 3D detection, and adds ~80k net new lines.
That's not a judgment of the code's quality — much of the fork is *better* than
what it replaced (see the comparative review). It's arithmetic about reviewability
and about what a maintainer can responsibly take ownership of.

Two further structural problems compound it:

1. **The fork is also 226 commits behind.** Upstream fixes (e.g. "drop hardcoded
   waffle-N convention") have not flowed in for over a year. Divergence runs in
   both directions, and both directions are growing.
2. **The fork's biggest contributions are entangled with its biggest files.**
   The worker pipeline, the split-GOP encoder, and the calibration suite — the
   things most worth sharing — are wired through `orange.cpp` (9.4k lines) and
   `orange_headless_client.cpp` (10.4k lines). A maintainer can't take the
   pipeline without taking the monoliths, and the monoliths are unreviewable.

So the realistic futures are:

- **(A) Incremental upstreaming** — extract small, self-contained improvements
  and send them as individually reviewable PRs, while regularly merging
  `upstream/main` into the fork to stop the bleeding. The fork remains the lab's
  advanced variant; upstream absorbs the safe wins over months.
- **(B) The fork becomes its own tool** — a named, citable instrument in its own
  right (its scope already differs: single-node deep pipeline vs multi-machine
  orchestration). Legitimate and common in scientific software; requires the same
  hygiene work as (A) so that *someone other than the author* can build and run it.
- **(C) Upstream adopts the fork's architecture wholesale** — only possible if the
  upstream maintainer actively wants it and co-drives it. Do not assume this; ask.

All three start with the same first step: **a conversation with the upstream
maintainer now**, showing the comparative review, and asking which of their
problems your work could solve. Every week of silence makes (A) and (C) harder.

## 3. Incremental upstreaming plan (path A), ordered by reviewability

Each item is a small PR a maintainer can evaluate in under an hour. Order matters:
early PRs build trust and establish you as a contributor, not an invader.

1. **Bug-fix PRs first (no API changes, obvious wins):**
   - `shaman.h`: `camera_id` u16→u32 truncation fix; epoch vs monotonic
     timestamp separation.
   - `offthreadmachine`: `std::atomic<bool> threadOn`.
   - `camera.h`: `throw(EXIT_FAILURE)` → `throw std::runtime_error(...)`.
   - The `detection2d[]` race fix (their highest-severity bug — bring the fix,
     not just the report).
   - Delete dead `lock_free_queue` / legacy `dim3(1,1)` kernels (exists in both
     trees).
2. **Drop-in robustness (small, isolated):**
   - Proper CUDA `CHECK` macro with file/line (without `exit(1)`).
   - `-Wall -Wextra` in CMake plus the warning fixes that fall out.
3. **Larger but self-contained components (each needs a design conversation
   first):**
   - Async `FFmpegWriter` (queue + writer thread + overflow stats).
   - The typed, bounded, CV-based `CThreadWorker` (breaking change to their
     worker subclasses — offer to do the migration in the PR).
4. **Only after 1–3 land and trust exists:** discuss whether upstream wants any
   of the big architecture (worker pipeline, split-GOP, calibration suite), and
   in what form.

**In parallel, in the fork:** `git merge upstream/main` (or rebase) now and on a
recurring basis — the 226-commit debt only grows. Resolve once while the
divergence is still navigable.

## 4. What must be true before ANY future where others use this code

These hold for paths A, B, and C alike. They are the difference between "my
instrument" and "an instrument":

1. **A second machine must be able to build it.** The build is currently proven
   on one workstation (CUDA arch pinned to sm_86/A6000, local SDK paths). CI is
   the cheapest proof: a GitHub Actions job that configures, builds, and runs
   `ctest` per push. Until this exists, every "can others use it" question is
   theoretical.
2. **A second person must be able to operate it.** The 110+ docs are strong on
   data contracts and thin on "install, configure, run your first recording."
   One end-to-end quickstart written for a stranger.
3. **The monoliths must shrink before anyone else touches them.**
   `orange_headless_client.cpp` (10.4k lines — the largest file in the repo, and
   one the earlier reviews under-weighted) and `orange.cpp` (9.4k) are
   contributor-repellent. The spatial-layout panel extractions show the pattern;
   it needs to continue until the pipeline can be constructed and tested without
   either file.
4. **The instrument-grade gaps close** (from the earlier reviews): the
   `countQueueIn` race, drop logging into session artifacts, preflight/postflight
   validation. A tool you give to others must not fail silently, because others
   won't have your instincts for when it's lying.
5. **Branch and tree hygiene.** 1,817 commits on `exp/`-named branches with ~26
   files of uncommitted work in the tree at review time: define how work
   graduates from experiment branches to a stable main, and keep the working
   tree clean enough that any commit could be the one someone else builds.

## 5. Adopting the epoch/seq idempotency envelope in the fork

(Documented from the deep-dive in `COMPARATIVE_REVIEW_2026-06-09.md` §3.1.)

The fork's external-recorder IPC (`external_recorder_ipc_protocol.h`,
`external_recorder_supervisor.cpp`) coordinates recorder lifecycle over a
transport that can duplicate or lose messages on supervisor restart. Upstream's
envelope pattern applies directly:

- **Adopt the triple** `(job_id, epoch, seq)` on every command, echoed verbatim
  in every reply.
- **Recorder side**: keep `(job, epoch, last_done_seq)`; reject stale epochs;
  `seq <= last_done` → re-ACK without re-executing; advance `last_done` only on
  success (dedup on success, retry on failure).
- **Supervisor side**: bump `epoch` on session reset/restart; discard replies
  not matching the current triple; advance `seq` only when the step is confirmed.

Hardening items to do better than upstream's implementation while adopting it:

1. Persist the epoch (a file under the session dir) so a supervisor restart
   cannot reuse a live epoch — upstream relies on the connect-event reset.
2. Reject a second controller explicitly (a true fencing check), rather than
   assuming a single host.
3. Make each command handler crash-safe at sub-step granularity (the seq gate
   makes *delivery* idempotent; it cannot make a half-executed action safe).
4. Log every dedup hit ("already") and every stale-epoch rejection into the
   session artifacts — these events are exactly the forensic record you want
   when a recording behaved oddly.

## 6. Where the earlier reviews were too soft (corrections to my own grading)

An honest review of the reviews, since the author asked:

1. **"Textbook-quality refcounting" is an unproven claim.** The ownership core —
   the most safety-critical concurrency code in the repo — has host tests for
   semantics but no concurrent stress tests and has never run under TSan. Until
   it survives sanitizers under contention, "looks textbook" is the most that
   can be said. The praise should have been conditional.
2. **`orange_headless_client.cpp` went unmentioned.** The fork review flagged
   `orange.cpp` as a god file while the actual largest file (10,354 lines) never
   appeared in it. Same disease, bigger organ.
3. **The docs got full credit too easily.** 110+ docs is real, but 30+ of them
   are TODO/plan documents — some of that is documentation as deferred work, and
   contract docs without schema-validation tooling drift silently.
4. **"Tests exist" overstated their protective value.** A homegrown assert
   framework, never run in CI, covering contracts but not the pipeline, protects
   against much less than the count suggests. Tests that don't run
   automatically rot into documentation of past intent.
5. **The strategic risk — divergence — was the single biggest threat to the
   author's actual goal and appeared only as a closing takeaway.** It should
   have been finding #1: at the current trajectory, the work becomes
   un-shareable not through any quality defect but through isolation. This
   document exists to correct that omission.

None of this changes the core judgment: the pipeline work is strong, in several
places stronger than what it replaced. But strong code that nobody else can
build, review, or merge does not yet meet the bar the author is aiming for —
"part of a codebase others use." The gap is closable, and §3–4 are the closing
moves.
