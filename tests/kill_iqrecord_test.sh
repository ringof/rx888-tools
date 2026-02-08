#!/usr/bin/env bash
set -euo pipefail

# Kill iqrecord mid-write and validate what artifacts remain.
#
# Usage:
#   ./tests/kill_iqrecord_test.sh captures/kill_iqrecord_test
#
# Notes:
# - SIGINT/SIGTERM should trigger graceful finalization (meta + run.json).
# - SIGKILL cannot be handled; expect possibly missing meta/run.json for the active file.

OUTDIR="${1:-captures/kill_iqrecord_test}"
MODE="${2:-sigint}"   # sigint|sigterm|sigkill
RUNTIME="${3:-0.5}"   # seconds before killing iqrecord

mkdir -p "$OUTDIR"

# Start generator -> mbuffer -> iqrecord
# Limit is 0 (infinite); we will stop by killing iqrecord.
set +e
./tests/counter_cf32.py --chunk-samples 1000000       | mbuffer -m 1G -q       | ./iqrecord "$OUTDIR" --desc "kill iqrecord mid-write test ($MODE)" &
PIPE_PID=$!
set -e

# Let it write for a moment
sleep "$RUNTIME"

# Find iqrecord PID in the pipeline. Best-effort: children of the pipeline subshell.
IQ_PID="$(pgrep -P "$PIPE_PID" -f './iqrecord' | head -n 1 || true)"
if [[ -z "${IQ_PID}" ]]; then
  # Fallback: search by command line and OUTDIR
  IQ_PID="$(pgrep -f "./iqrecord $OUTDIR" | head -n 1 || true)"
fi

if [[ -z "${IQ_PID}" ]]; then
  echo "Could not find iqrecord PID; pipeline pid was $PIPE_PID" >&2
  exit 2
fi

case "$MODE" in
  sigint)  kill -INT  "$IQ_PID" ;;
  sigterm) kill -TERM "$IQ_PID" ;;
  sigkill) kill -KILL "$IQ_PID" ;;
  *) echo "Unknown MODE: $MODE (use sigint|sigterm|sigkill)" >&2; exit 2 ;;
esac

# Wait for pipeline to unwind
wait "$PIPE_PID" 2>/dev/null || true

echo "=== Post-checks for $OUTDIR (mode=$MODE) ==="
ls -lh "$OUTDIR" || true

# Validate run.json if present
if [[ -f "$OUTDIR/run.json" ]]; then
  if command -v jq >/dev/null 2>&1; then
    jq . "$OUTDIR/run.json" >/dev/null && echo "run.json: valid JSON" || echo "run.json: INVALID JSON"
  else
    python3 -c 'import json,sys; json.load(open(sys.argv[1])); print("run.json: valid JSON")' "$OUTDIR/run.json"           || echo "run.json: INVALID JSON"
  fi
else
  echo "run.json: MISSING"
fi

# Check for any .sigmf-data files lacking .sigmf-meta
missing=0
shopt -s nullglob
for d in "$OUTDIR"/cap_*.sigmf-data; do
  m="${d%.sigmf-data}.sigmf-meta"
  if [[ ! -f "$m" ]]; then
    echo "Missing meta for: $(basename "$d")"
    missing=$((missing+1))
  fi
done
shopt -u nullglob

echo "Missing meta count: $missing"
if [[ "$MODE" == "sigkill" ]]; then
  echo "NOTE: SIGKILL is expected to potentially leave missing meta/run.json for the active file."
  exit 0
fi

# For SIGINT/SIGTERM we expect clean finalization: run.json present and no missing meta.
if [[ ! -f "$OUTDIR/run.json" ]]; then
  echo "FAIL: run.json missing after $MODE" >&2
  exit 3
fi
if [[ "$missing" -ne 0 ]]; then
  echo "FAIL: missing meta after $MODE" >&2
  exit 4
fi

echo "PASS: graceful finalization under $MODE"
