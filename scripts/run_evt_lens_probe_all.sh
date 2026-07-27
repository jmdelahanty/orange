#!/usr/bin/env bash
set -euo pipefail

PROBE_BIN="${PROBE_BIN:-./targets/release/evt_lens_probe}"
LOG_DIR="${LOG_DIR:-/tmp/evt_lens_probe}"
LD_SDK_PATH="${LD_SDK_PATH:-/opt/EVT/eSDK/lib}"

usage() {
  cat <<'EOF'
Usage: run_evt_lens_probe_all.sh [options] [-- <probe args>]

Runs evt_lens_probe across all discovered camera serials.

Options:
  --probe-bin <path>    Path to evt_lens_probe binary (default: ./targets/release/evt_lens_probe)
  --log-dir <path>      Output directory for per-camera logs (default: /tmp/evt_lens_probe)
  --serials <csv>       Comma-separated serial list (skip auto-discovery)
  --sensor-pipeline     Getter-only JSON + exact GenICam XML for every camera
  -h, --help            Show this help

Everything after '--' is passed directly to each probe invocation.

Examples:
  scripts/run_evt_lens_probe_all.sh -- --enable-uart
  scripts/run_evt_lens_probe_all.sh -- --enable-uart --exercise-genicam
  scripts/run_evt_lens_probe_all.sh --serials 02010093,02010094 -- --focus-target 3000
  scripts/run_evt_lens_probe_all.sh --sensor-pipeline --serials 2010093,2010094,2010095,2010096
EOF
}

SERIALS_CSV=""
SENSOR_PIPELINE=0
PROBE_ARGS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --probe-bin)
      shift
      [[ $# -gt 0 ]] || { echo "ERROR: --probe-bin requires a value" >&2; exit 1; }
      PROBE_BIN="$1"
      ;;
    --log-dir)
      shift
      [[ $# -gt 0 ]] || { echo "ERROR: --log-dir requires a value" >&2; exit 1; }
      LOG_DIR="$1"
      ;;
    --serials)
      shift
      [[ $# -gt 0 ]] || { echo "ERROR: --serials requires a value" >&2; exit 1; }
      SERIALS_CSV="$1"
      ;;
    --sensor-pipeline)
      SENSOR_PIPELINE=1
      ;;
    --)
      shift
      PROBE_ARGS=("$@")
      break
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "ERROR: Unknown argument: $1" >&2
      usage
      exit 1
      ;;
  esac
  shift
done

if [[ ! -x "${PROBE_BIN}" ]]; then
  echo "ERROR: probe binary not found or not executable: ${PROBE_BIN}" >&2
  echo "Build it first: cmake --build targets/release --target evt_lens_probe -j8" >&2
  exit 1
fi

mkdir -p "${LOG_DIR}"

export LD_LIBRARY_PATH="${LD_SDK_PATH}:${LD_LIBRARY_PATH:-}"

declare -a SERIALS=()

if [[ -n "${SERIALS_CSV}" ]]; then
  IFS=',' read -r -a SERIALS <<< "${SERIALS_CSV}"
else
  mapfile -t SERIALS < <("${PROBE_BIN}" --list-only | sed -n 's/.*serial=\([^ ]*\).*/\1/p')
fi

if [[ ${#SERIALS[@]} -eq 0 ]]; then
  echo "No camera serials discovered." >&2
  exit 2
fi

timestamp="$(date +%Y%m%d_%H%M%S)"
summary_file="${LOG_DIR}/summary_${timestamp}.txt"
touch "${summary_file}"

echo "Probe binary: ${PROBE_BIN}"
echo "Log dir:      ${LOG_DIR}"
echo "Serials:      ${SERIALS[*]}"
echo "Probe args:   ${PROBE_ARGS[*]:-(none)}"
echo

failures=0
for serial in "${SERIALS[@]}"; do
  serial="${serial// /}"
  [[ -n "${serial}" ]] || continue
  log_path="${LOG_DIR}/evt_lens_probe_${serial}_${timestamp}.log"
  declare -a per_camera_args=(--serial "${serial}")
  if [[ ${SENSOR_PIPELINE} -eq 1 ]]; then
    json_path="${LOG_DIR}/sensor_pipeline_${serial}_${timestamp}.json"
    xml_path="${LOG_DIR}/genicam_${serial}_${timestamp}.xml"
    per_camera_args+=(
      --sensor-pipeline
      --sensor-pipeline-json "${json_path}"
      --genicam-xml-out "${xml_path}"
    )
  fi

  echo "===== SERIAL ${serial} ====="
  echo "Log: ${log_path}"
  if "${PROBE_BIN}" "${per_camera_args[@]}" "${PROBE_ARGS[@]}" | tee "${log_path}"; then
    echo "[PASS] ${serial}" | tee -a "${summary_file}"
    if [[ ${SENSOR_PIPELINE} -eq 1 ]]; then
      echo "       JSON=${json_path}" | tee -a "${summary_file}"
      echo "       XML=${xml_path}" | tee -a "${summary_file}"
    fi
  else
    rc=$?
    echo "[FAIL] ${serial} (exit ${rc})" | tee -a "${summary_file}"
    failures=$((failures + 1))
  fi
  echo
done

echo "Summary written to: ${summary_file}"
if [[ ${failures} -gt 0 ]]; then
  echo "Completed with ${failures} failure(s)." >&2
  exit 3
fi

echo "Completed successfully for all serials."
