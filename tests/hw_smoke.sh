#!/usr/bin/env bash
# hw_smoke.sh — sustained-stream smoke test.
#
# Captures rx888_stream output for N seconds and checks that the byte
# rate is within tolerance of (samplerate * 2).  Catches gross
# regressions in the streaming path: stalls, dropped buffers, USB
# misconfiguration, queue/packet sizing wrong.
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
TOL_PCT="${RX888_TOL_PCT:-10}"

[[ -x "$STREAM" ]]    || { echo "missing $STREAM"; exit 2; }
[[ -f "$FIRMWARE" ]]  || { echo "missing $FIRMWARE (run: make firmware)"; exit 2; }

OUT=$(mktemp)
ERR=$(mktemp)
trap 'rm -f "$OUT" "$ERR"' EXIT

echo "capturing $SECS s @ $RATE S/s..."

# Use timeout to enforce a hard upper bound; rx888_stream exits cleanly
# on SIGTERM via its existing signal handler.
set +e
timeout --preserve-status $((SECS + 3)) \
    "$STREAM" -f "$FIRMWARE" -s "$RATE" >"$OUT" 2>"$ERR"
rc=$?
set -e

# 124 = timeout fired before stream stopped on its own; that's normal here.
if [[ $rc -ne 0 && $rc -ne 124 && $rc -ne 143 ]]; then
    echo "FAIL: rx888_stream exited rc=$rc"
    sed -n '1,20p' "$ERR" >&2
    exit 1
fi

bytes=$(stat -c %s "$OUT")
expected=$((RATE * 2 * SECS))
lo=$((expected * (100 - TOL_PCT) / 100))
hi=$((expected * (100 + TOL_PCT) / 100))

printf "captured %d bytes (expected ~%d, tolerance +/- %d%%, range [%d, %d])\n" \
    "$bytes" "$expected" "$TOL_PCT" "$lo" "$hi"

if (( bytes < lo )); then
    echo "FAIL: byte count below lower bound"
    exit 1
fi
if (( bytes > hi )); then
    echo "FAIL: byte count above upper bound (impossible? something is wrong)"
    exit 1
fi

echo "ok hw_smoke ($bytes bytes in ${SECS}s)"
