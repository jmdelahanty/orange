# Shadow live bound fixture: Orange runbook (2026-09-29)

The live gate after the synthetic Orange → Citrus → Palette chain passed
(agent-contracts PR 52). Only after this passes does Orange's default binding
request version move from 1 to 2. Synthetic success does not replace it.

Sequence agreed with Citrus: (1) verify both GUI builds and the installed
wrapper; (2) match Orange's per-camera context with each Shadow Arena context;
(3) run with binding request v2 and binding mode `required`; (4) check
finalization, dry-run the transfer, confirm Palette admission.

## 0. Prechecks (done 2026-09-29, repeat if anything changes)

| Check | State |
| --- | --- |
| Integration tree `/home/jeremy/orange-integration-20260921` | `a481250`, clean, `orange` up to date |
| Production tree `/home/jeremy/orange-gop-split-a16` | `a481250`, clean, `orange` up to date (GUI sources unchanged since 34830c6) |
| Installed wrapper `/usr/local/bin/orange-gui-validation` | identical to `scripts/orange_gui_validation_wrapper.sh`; forwards `ORANGE_CITRUS_BINDING_REQUEST_VERSION` |
| Host PTP stack | `ptp4l` + `phc2sys` running (verify with `sudo -n scripts/ptp_stack.sh status` before the run) |
| Shadow canvas `~/citrus/targets/rigs/omnifin0/shadow/shadow.json` | no `recording_context` on any Arena yet (Citrus sets these) |
| App config `~/orange_data/config/app/default.json` | no `recording.contexts`, no `recording.citrus_binding_request_version` (set in the panel / env below) |
| Rig | idle; no builds by anyone during the run |
| Transfer sealer pre-check | after any candidate recording: `scripts/check_recording_transfer_source.sh <folder>` runs Citrus's `recording_transfer_v2.py --validate-source-only` from the pinned checkout (digests verified) and must print no `Transfer refused`; the operator's real transfer (`transfer_as_domain_user.sh` → `rsync_move.sh`, profile parent-recording-v2) then seals the envelope Palette requires. A stimulus-intent session recorded without Citrus is refused by design (`observation binding exists without finalized manifest projection`); use intent `recording_only` for Orange-only sessions |
| Citrus main carries the unified-H5 correspondence v2 / chaser v2 / capacity work | Palette, 2026-10-06: the 27 commits on `agent/unified-h5-streamed-correspondence-20260923` were never merged to citrus main; a bound recording made from main is refused at Palette admission for non-subject reasons. Resolved: merged on citrus main (288f14d; Palette admitted Citrus's five v3 fixtures from it). Palette #265 merged on Palette main as 13e2c6c4 (no-dish arenas admitted; the Orange/Citrus dish cross-check falls back to dish_id when Citrus's lookup failed), so the Palette side is ready. Run configuration (artifact_profile `unified_experimental_h5_v1`, capacity_configuration_path, the controlled live-start prerequisites) must be installed on the Citrus side. A recording-only session (no Citrus) is unaffected |
| MetaZebrobot consumer expectations | `scripts/check_metazebrobot_consumer_pin.sh --live http://delahantyj-ws1.hhmi.org` (fetches the verifier from agent-contracts main at the PR 54 merge 5fc735fe) prints `PASS`; PASS 2026-10-05 21:30Z against the live service (digest f5280e43… matches the pin; the service happened to run 509a3eb8 then). The check is the digest only: `service_commit` and `service_commit_dirty` are provenance and move with every MetaZebrobot restart, never something to assert |

## 1. Context values (operator decision, both sides identical)

Per camera parent, Orange emits one `citrus.parent_recording_context`; Citrus
compares it with the Arena's expected context and rejects a mismatch
(`recording_context_mismatch`) or a missing Arena context
(`recording_context_unavailable`). Agreed shape for Shadow:

| Field | Value |
| --- | --- |
| `recording_type` | operator label, e.g. `behavior` (same string on both sides, byte for byte) |
| `recording_subtype` | **omitted** (context v2; blank in the panel) |
| `behavior_mode` | the Shadow mode, e.g. `embedded` (same on both sides) |
| `recording_intent` | `stimulus_experiment` |
| `data_origin` | `acquired` |

Orange side: launch the GUI, open "Recording context (what this recording
is)" under the media products selection, fill the fields, leave the subtype
blank, press "Save recording context" (writes `recording.contexts.default`
to the app config; it then applies to every camera). Citrus side: the same
values in each Shadow Arena's `recording_context` (Citrus's canvas config;
4 Arenas, cameras 2010093..2010096).

## 2. Launch

```bash
cd /home/jeremy/orange-integration-20260921
sudo -n scripts/ptp_stack.sh status            # must be running
scripts/check_gui_citrus_completion_ready.py --check-socket

ORANGE_CITRUS_BINDING_REQUEST_VERSION=2 \
ORANGE_CITRUS_OBSERVATION_BINDING_MODE=required \
ORANGE_CITRUS_RECORDING_CANVAS_CONFIG_PATH=/home/jeremy/citrus/targets/rigs/omnifin0/shadow/shadow.json \
scripts/run_gui_fourcam_external_ipc_validation.sh \
  --hidden-crop-preview --citrus-display-safe --manual-citrus-completion-control
```

The launcher forwards those three variables through the wrapper. With
`required`, a record start is refused (not armed) unless Citrus accepts all
four bindings; the GUI refusal names each rejected context and reason.

Then, before starting Citrus:

```bash
export ORANGE_CITRUS_HANDOFF=/tmp/orange_manual_citrus_completion_handoff.json
ORANGE_RECORDING_FOLDER=$(scripts/check_gui_citrus_completion_ready.py \
  --require-manual-citrus-ready --wait-seconds 120 \
  --write-handoff "${ORANGE_CITRUS_HANDOFF}" --print-recording-folder)
```

Start Citrus on Shadow with its Orange completion notifier enabled
(`docs/manual_orange_citrus_completion_runbook.md`). Record; finish with
STOP ALL in Citrus (or let the protocol complete).

## 3. Validate (Orange side)

```bash
# GUI recording + Citrus completion path (choose the finish you used)
scripts/validate_gui_citrus_completion_recording.py --handoff "${ORANGE_CITRUS_HANDOFF}" --stop-all
#   or: --natural-completion

# Binding chain: request v2 with the frozen context, acceptance v2, receipts v1,
# unified H5 layout, finalized collection, manifest projection
scripts/validate_recording_observation_bindings.py "${ORANGE_RECORDING_FOLDER}" \
  --expected-cameras 2010093,2010094,2010095,2010096 --expected-count 4

# Manifest/context block and the frozen snapshot agreement are covered by
# validate_gui_ptp_recording.py (check_recording_contexts), invoked by the above.
```

Expected in `recording_snapshot_start.json`: `session.recording_contexts`
with four v2 entries (no subtype, `data_origin = acquired`), the geometry
contract `resolved` for Shadow (commissioned pointers, daily registration as
selected), `models[<serial>]` from the live GUI. Expected in
`recording_observation_bindings/`: four v2 requests, four v2 acceptances,
`pre_arm_decision.json` with `lifecycle_status = accepted_pending_finalization`,
then four receipts and `finalized_collection.json` with `binding_status =
bound` after Citrus finalizes.

## 4. Transfer dry run and Palette

Citrus's domain-user helper (`scripts/transfer_as_domain_user.sh`, dry run
first) delivers the recording folder to the agreed `agent_review_evidence`
path; Citrus's `recording_transfer_v2.py --validate-source-only` must admit
it first. Palette then runs intake and reports on PR 52.

## 5. After a pass

- Flip the Orange default: `kObservationBindingSchemaVersion` stays 1 for
  receipts; change `resolve_recording_observation_binding_request_version` to
  default to 2 (or set app config `recording.citrus_binding_request_version = 2`
  on this host as the interim), update the contract doc, rerun the binding
  tests, and record the live artifact folder in the journal.
- Keep `100_cam4_ptp_fourcam` and the Citrus-safe display profile as the
  validation defaults.

## Do not

- Build anything on the rig during the run (IPI storms drop card-A frames).
- Edit a sealed recording folder; rerun instead.
- Substitute a subtype, an empty string or `null`; the omission is the
  declaration.
