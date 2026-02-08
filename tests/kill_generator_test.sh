#!/usr/bin/env bash
set -euo pipefail

OUTDIR="${1:-captures/kill_gen_test}"
SLEEP_SECS="${2:-0.5}"

rm -rf "$OUTDIR"
mkdir -p "$OUTDIR"

FIFO="$(mktemp -u /tmp/iqrecord_fifo.XXXXXX)"
mkfifo "$FIFO"

cleanup() {
  rm -f "$FIFO"
}
trap cleanup EXIT

print_procs() {
  local label="$1"
  echo "=== $label ==="
  echo "FIFO: $FIFO"
  echo "OUTDIR: $OUTDIR"
  echo "GEN_PID=${GEN_PID:-<unset>}  PIPE_PID=${PIPE_PID:-<unset>}"

  # Show PID/PPID/PGID and full command for relevant processes.
  # - We query by PID if known, and also by command patterns for extra safety.
  local pids=()
  [[ -n "${GEN_PID:-}" ]] && pids+=("$GEN_PID")
  [[ -n "${PIPE_PID:-}" ]] && pids+=("$PIPE_PID")

  if ((${#pids[@]})); then
    echo "--- ps by known PIDs ---"
    ps -o pid,ppid,pgid,stat,etime,cmd -p "$(IFS=,; echo "${pids[*]}")" || true
  fi

  echo "--- ps by pattern (counter_cf32.py / mbuffer / iqrecord) ---"
  ps -eo pid,ppid,pgid,stat,etime,cmd \
    | grep -E "(counter_cf32\.py|mbuffer|/iqrecord(\s|$))" \
    | grep -v "grep -E" \
    || true

  echo "========================"
}

echo "Starting consumer pipeline: mbuffer < FIFO | iqrecord"

# Start consumer pipeline first; it will block waiting for FIFO data.
mbuffer -m 1G -q < "$FIFO" | ./iqrecord "$OUTDIR" --desc "kill generator mid-stream" &
PIPE_PID=$!

echo "Starting producer: counter_cf32.py > FIFO"
./tests/counter_cf32.py --limit-samples 99999999999 > "$FIFO" &
GEN_PID=$!

# Let data flow a bit.
sleep "$SLEEP_SECS"

print_procs "Before killing generator"

echo "Killing generator PID $GEN_PID (SIGTERM)"
kill -TERM "$GEN_PID" || true

# Wait briefly so process state changes are visible.
sleep 0.2
print_procs "After SIGTERM to generator (0.2s later)"

# If the generator didn't exit, escalate.
if kill -0 "$GEN_PID" 2>/dev/null; then
  echo "Generator still alive; sending SIGKILL"
  kill -KILL "$GEN_PID" || true
  sleep 0.2
  print_procs "After SIGKILL to generator (0.2s later)"
fi

# Reap generator.
wait "$GEN_PID" 2>/dev/null || true
echo "Generator reaped."

# Once writer is gone, FIFO should hit EOF -> mbuffer exits -> iqrecord finalizes.
# Wait for pipeline to drain and exit.
wait "$PIPE_PID" 2>/dev/null || true
echo "Pipeline reaped."

print_procs "Final process snapshot"

echo "=== Post-checks for $OUTDIR (kill producer) ==="
ls -lh "$OUTDIR" || true

if [[ -f "$OUTDIR/run.json" ]]; then
  if jq . "$OUTDIR/run.json" >/dev/null 2>&1; then
    echo "run.json: valid JSON"
  else
    echo "run.json: INVALID JSON"
  fi
else
  echo "run.json: missing"
fi

missing_meta=$(
  ls "$OUTDIR"/cap_*.sigmf-data 2>/dev/null \
    | sed 's/\.sigmf-data$//' \
    | while read -r b; do [[ -f "${b}.sigmf-meta" ]] || echo "${b}.sigmf-meta"; done \
    | wc -l
)

echo "Missing meta count: $missing_meta"

if [[ "$missing_meta" -eq 0 ]]; then
  echo "PASS: graceful finalization after producer death"
else
  echo "FAIL: missing meta files"
fi
