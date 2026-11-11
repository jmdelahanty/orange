#!/usr/bin/env bash
# Stamp, run, and analyse one detect-latency experiment spec.
#
#   scripts/run_detect_latency_spec.sh [--orange-client <binary>] <spec-name-or-path> [baseline.json]
#
# --orange-client selects the client binary (default: this worktree's
# targets/release/orange_client). The sudo wrapper accepts only its allowed
# paths; a new worktree's binary must be added there first.
#
# - stamps a copy of the spec into /tmp with a unique experiment_id and
#   external_recorder_contract.artifact_root (the client refuses to reuse a
#   non-empty run folder),
# - runs it through the NOPASSWD benchmark wrapper with YOLO perf logging,
# - runs scripts/analyze_yolo_latency_phases.py on the first run, writing
#   <output_root>/<experiment_id>/latency_phases.json next to runs.json (never
#   into the run folder, which orange_client has sealed), and prints the
#   summary (with deltas against the optional baseline JSON).
#
# Pre-flight the cameras first (a stale control session refuses to open):
#   sudo -n /usr/local/bin/orange-evt-stream-smoke \
#     --config-dir /home/jeremy/orange_data/config/local/100_cam4_ptp_fourcam --all --frames 5
# and if a camera answers "GVCP ACK error", reboot it over GVCP:
#   targets/release/evt_force_reboot <serial> <camera-ip>
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CLIENT_OVERRIDE=""
while [[ $# -gt 0 && "$1" == --* ]]; do
  case "$1" in
    --orange-client)
      [[ $# -ge 2 ]] || { echo "--orange-client requires a path" >&2; exit 2; }
      CLIENT_OVERRIDE="$(realpath -e "$2" 2>/dev/null || true)"
      [[ -x "$CLIENT_OVERRIDE" ]] || { echo "orange_client not executable: $2" >&2; exit 2; }
      shift 2 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done
SPEC_ARG="${1:?spec name or path required}"
BASELINE_JSON="${2:-}"
if [[ -f "$SPEC_ARG" ]]; then SPEC_SRC="$(realpath "$SPEC_ARG")"; else SPEC_SRC="$REPO_ROOT/experiment_specs/${SPEC_ARG%.json}.json"; fi
[[ -f "$SPEC_SRC" ]] || { echo "spec not found: $SPEC_SRC" >&2; exit 2; }
CLIENT="${CLIENT_OVERRIDE:-$REPO_ROOT/targets/release/orange_client}"
[[ -x "$CLIENT" ]] || { echo "build first: cmake --build targets/release --target orange_client external_recorder_ipc_probe" >&2; exit 2; }
echo "[run] orange_client=$CLIENT"
STAMP="$(date +%Y%m%d_%H%M%S)"
STAMPED="/tmp/$(basename "${SPEC_SRC%.json}")_${STAMP}.json"
python3 - "$SPEC_SRC" "$STAMP" "$STAMPED" <<'PY'
import json, sys
src, stamp, out = sys.argv[1:4]
s = json.load(open(src))
name = s['experiment_id'] + '_' + stamp
s['experiment_id'] = name
c = s['fixed'].get('external_recorder_contract')
new = ''
if c:  # stream-only specs have no recorder contract
    old = c['artifact_root']
    c['session_id'] = name
    if old.startswith('/'):
        # Absolute root outside the recording folder: stamp it so a rerun never
        # reuses it (the client refuses a reused artifact_root).
        new = old + '_' + stamp
        c['artifact_root'] = new
        for st in c['streams'].values():
            for k in ('summary_json', 'video_sanity_json', 'mp4', 'gop_routing_csv'):
                st[k] = st[k].replace(old, new)
    else:
        # Relative root (e.g. "external_recorder"): resolved by the client under
        # the per-run recording folder, which is unique already; stream paths
        # stay relative to it (Palette-transferable layout).
        new = old
json.dump(s, open(out, 'w'), indent=2)
print(f"experiment_id={name}")
print(f"artifact_root={new}")
PY
EXPERIMENT_ID="$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['experiment_id'])" "$STAMPED")"
ARTIFACT_ROOT="$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['fixed'].get('external_recorder_contract', {}).get('artifact_root', ''))" "$STAMPED")"
OUTPUT_ROOT="$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['fixed']['output_root'])" "$STAMPED")"
LOG="/tmp/${EXPERIMENT_ID}.log"
echo "[run] spec=$STAMPED log=$LOG"
set +e
sudo -n /usr/local/bin/orange-local-benchmark --orange-client "$CLIENT" --yolo-perf-log --yolo-perf-sample 1 "$STAMPED" > "$LOG" 2>&1
RC=$?
set -e
echo "[run] exit=$RC"
# The run folder from runs.json: run_0001__..., or <experiment_id>__run_0001__...
# with unique_run_id.
EXPERIMENT_DIR="$OUTPUT_ROOT/$EXPERIMENT_ID"
RUN_DIR="$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['runs'][0].get('recording_folder', ''))" "$EXPERIMENT_DIR/runs.json" 2>/dev/null || true)"
[[ -n "$RUN_DIR" && -d "$RUN_DIR" ]] || RUN_DIR=""
if [[ -z "$RUN_DIR" ]]; then
  echo "[run] no run folder produced; last log lines:" >&2
  grep -v "PIPELINE\|YOLO_PERF" "$LOG" | tail -8 >&2
  exit "$RC"
fi
python3 - "$EXPERIMENT_DIR/runs.json" <<'PY'
import json, sys
r = json.load(open(sys.argv[1]))['runs'][0]
print(f"[run] status={r.get('status')} pass_fail={r.get('pass_fail')} reason={str(r.get('reason'))[:200]}")
for row in r.get('camera_results', []):
    print(f"[run]   camera {row.get('camera_serial')}: pass_fail={row.get('pass_fail')} reason={str(row.get('reason'))[:160]}"
          f" cap_skips={row.get('deferred_release_cap_skips_final')} copy_fallbacks={row.get('deferred_release_copy_fallbacks_final')}"
          f" submitted={row.get('submitted_frames_final')}"
          f" acked={row.get('external_ipc_frames_acked_final')}")
PY
ANALYSIS_JSON="$EXPERIMENT_DIR/latency_phases.json"
ARGS=("$RUN_DIR" --steady-after 200 --json "$ANALYSIS_JSON")
# A relative artifact_root lives under the run folder.
if [[ -n "$ARTIFACT_ROOT" ]]; then
  [[ "$ARTIFACT_ROOT" == /* ]] || ARTIFACT_ROOT="$RUN_DIR/$ARTIFACT_ROOT"
  ARGS+=(--external-recorder-dir "$ARTIFACT_ROOT")
fi
[[ -n "$BASELINE_JSON" ]] && ARGS+=(--baseline-json "$BASELINE_JSON")
# The analysis needs numpy, which /usr/bin/python3 lacks: ORANGE_ANALYSIS_PYTHON,
# else the juicebox env, else python3. Its result never changes this script's
# exit code, which is orange_client's.
ANALYSIS_PYTHON="${ORANGE_ANALYSIS_PYTHON:-}"
if [[ -z "$ANALYSIS_PYTHON" ]]; then
  ANALYSIS_PYTHON=python3
  [[ -x "$HOME/miniforge3/envs/juicebox/bin/python" ]] && ANALYSIS_PYTHON="$HOME/miniforge3/envs/juicebox/bin/python"
fi
if "$ANALYSIS_PYTHON" "$REPO_ROOT/scripts/analyze_yolo_latency_phases.py" "${ARGS[@]}"; then
  echo "[run] analysis json: $ANALYSIS_JSON"
else
  echo "[run] latency analysis FAILED ($ANALYSIS_PYTHON); rerun: $ANALYSIS_PYTHON scripts/analyze_yolo_latency_phases.py ${ARGS[*]}" >&2
fi
exit "$RC"
