#!/usr/bin/env bash
# Stamp, run, and analyse one detect-latency experiment spec.
#
#   scripts/run_detect_latency_spec.sh <spec-name-or-path> [baseline.json]
#
# - stamps a copy of the spec into /tmp with a unique experiment_id and
#   external_recorder_contract.artifact_root (the client refuses to reuse a
#   non-empty run folder),
# - runs it through the NOPASSWD benchmark wrapper with YOLO perf logging,
# - runs scripts/analyze_yolo_latency_phases.py on the result, writing
#   <run folder>/latency_phases.json, and prints the summary (with deltas
#   against the optional baseline JSON).
#
# Pre-flight the cameras first (a stale control session refuses to open):
#   sudo -n /usr/local/bin/orange-evt-stream-smoke \
#     --config-dir /home/jeremy/orange_data/config/local/100_cam4_ptp_fourcam --all --frames 5
# and if a camera answers "GVCP ACK error", reboot it over GVCP:
#   targets/release/evt_force_reboot <serial> <camera-ip>
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SPEC_ARG="${1:?spec name or path required}"
BASELINE_JSON="${2:-}"
if [[ -f "$SPEC_ARG" ]]; then SPEC_SRC="$(realpath "$SPEC_ARG")"; else SPEC_SRC="$REPO_ROOT/experiment_specs/${SPEC_ARG%.json}.json"; fi
[[ -f "$SPEC_SRC" ]] || { echo "spec not found: $SPEC_SRC" >&2; exit 2; }
CLIENT="$REPO_ROOT/targets/release/orange_client"
[[ -x "$CLIENT" ]] || { echo "build first: cmake --build targets/release --target orange_client external_recorder_ipc_probe" >&2; exit 2; }
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
    old = c['artifact_root']; new = old + '_' + stamp
    c['artifact_root'] = new; c['session_id'] = name
    for st in c['streams'].values():
        for k in ('summary_json', 'video_sanity_json', 'mp4', 'gop_routing_csv'):
            st[k] = st[k].replace(old, new)
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
RUN_DIR="$(ls -d "$OUTPUT_ROOT/$EXPERIMENT_ID"/run_0001* 2>/dev/null | head -1 || true)"
if [[ -z "$RUN_DIR" ]]; then
  echo "[run] no run folder produced; last log lines:" >&2
  grep -v "PIPELINE\|YOLO_PERF" "$LOG" | tail -8 >&2
  exit "$RC"
fi
python3 - "$OUTPUT_ROOT/$EXPERIMENT_ID/runs.json" <<'PY'
import json, sys
r = json.load(open(sys.argv[1]))['runs'][0]
print(f"[run] status={r.get('status')} pass_fail={r.get('pass_fail')} reason={str(r.get('reason'))[:200]}")
for row in r.get('camera_results', []):
    print(f"[run]   camera {row.get('camera_serial')}: pass_fail={row.get('pass_fail')} reason={str(row.get('reason'))[:160]}"
          f" cap_skips={row.get('deferred_release_cap_skips_final')} copy_fallbacks={row.get('deferred_release_copy_fallbacks_final')}"
          f" submitted={row.get('submitted_frames_final')}"
          f" acked={row.get('external_ipc_frames_acked_final')}")
PY
ARGS=("$RUN_DIR" --steady-after 200 --json "$RUN_DIR/latency_phases.json")
[[ -n "$ARTIFACT_ROOT" ]] && ARGS+=(--external-recorder-dir "$ARTIFACT_ROOT")
[[ -n "$BASELINE_JSON" ]] && ARGS+=(--baseline-json "$BASELINE_JSON")
python3 "$REPO_ROOT/scripts/analyze_yolo_latency_phases.py" "${ARGS[@]}"
echo "[run] analysis json: $RUN_DIR/latency_phases.json"
