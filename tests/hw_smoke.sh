#!/usr/bin/env bash
# hw_smoke.sh — sustained-stream smoke test.
#
# Bounds the capture by BYTE COUNT (via `head -c`), not by wall time.
# That way "rx888_stream produced more samples than expected in N
# seconds" stops being a failure mode (it isn't one — it just means
# the streamer kept up).  The real things this catches are:
#
#   - rx888_stream produces less than the requested byte count
#     within a generous deadline (stalls, dropped buffers, USB
#     misconfiguration, queue/packet sizing wrong)
#   - rx888_stream crashes / exits non-zero before reaching the
#     target
#
# Inspired by the sustained_stream / throughput scenarios in
# rx888-firmware's fw_test.sh.  Implementation here is fresh.
#
# Requires hardware.  Set RX888_HW_TEST=1 to run.

set -euo pipefail

if [[ "${RX888_HW_TEST:-0}" != "1" ]]; then
    echo "skip hw_smoke (set RX888_HW_TEST=1 to enable)"
    exit 0
fi

STREAM="${RX888_STREAM:-./rx888_stream}"
FIRMWARE="${RX888_FW:-firmware/SDDC_FX3.img}"
RATE="${RX888_RATE:-32000000}"
SECS="${RX888_SECS:-10}"
# Hard upper bound on wall-clock time; if it takes longer than this,
# throughput is broken.  Generous so firmware upload + startup latency
# don't trip it.
DEADLINE="${RX888_DEADLINE:-$((SECS * 3 + 5))}"

[[ -x "$STREAM" ]]    || { echo "missing $STREAM"; exit 2; }
[[ -f "$FIRMWARE" ]]  || { echo "missing $FIRMWARE (run: make firmware)"; exit 2; }

OUT=$(mktemp)
ERR=$(mktemp)
trap 'rm -f "$OUT" "$ERR"' EXIT

target_bytes=$((RATE * 2 * SECS))
echo "capturing $target_bytes bytes ($SECS s @ $RATE S/s, deadline ${DEADLINE}s)..."

start_ns=$(date +%s%N)
set +e
# `head -c $target_bytes` closes the pipe once it has the target;
# rx888_stream gets EPIPE and exits cleanly.  The outer `timeout` is
# only a safety net if the streamer never produces enough bytes.
timeout --preserve-status --kill-after=2 "$DEADLINE" \
    "$STREAM" -f "$FIRMWARE" -s "$RATE" 2>"$ERR" \
  | head -c "$target_bytes" >"$OUT"
stream_rc=${PIPESTATUS[0]}
head_rc=${PIPESTATUS[1]}
set -e
end_ns=$(date +%s%N)
elapsed_ms=$(( (end_ns - start_ns) / 1000000 ))

bytes=$(stat -c %s "$OUT")
printf "got %d bytes in %d ms (stream_rc=%d head_rc=%d)\n" \
    "$bytes" "$elapsed_ms" "$stream_rc" "$head_rc"

if (( bytes != target_bytes )); then
    echo "FAIL: got $bytes / $target_bytes bytes"
    echo "--- rx888_stream stderr ---"
    sed -n '1,20p' "$ERR" >&2
    exit 1
fi

# Sanity-check the elapsed time.  We expect roughly SECS seconds of
# wall-clock plus startup latency.  Anything below SECS would mean the
# system clock is broken; anything above DEADLINE was already caught
# by the `timeout` above.  Just informational.
if (( elapsed_ms < SECS * 1000 / 2 )); then
    echo "warn: elapsed=$elapsed_ms ms is suspiciously short for $SECS s of data"
fi

# head closing the pipe causes rx888_stream to see EPIPE.  Its handler
# treats that as a clean shutdown, so rc=0.  Tolerate 141 (SIGPIPE)
# in case the signal slipped through.
case "$stream_rc" in
    0|141) ;;
    *) echo "FAIL: rx888_stream exited rc=$stream_rc"; exit 1 ;;
esac

echo "ok hw_smoke ($bytes bytes, ~$(( bytes / (elapsed_ms > 0 ? elapsed_ms : 1) * 1000 / 1024 / 1024 )) MiB/s avg)"
