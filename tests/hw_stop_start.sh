#!/usr/bin/env bash
# hw_stop_start.sh — repeated open/start/stop/close cycles.
#
# Verifies librx888 can re-enter cleanly after each stream session,
# and that the FX3 lands back in idle state ready for the next start.
# Catches resource leaks (transfers, threads, USB handles) and FX3
# state-machine bugs that only show up after a few cycles.
#
# Inspired by the stop_start_cycle scenario in rx888-firmware's
# fw_test.sh.  Implementation here is fresh.
#
# Requires hardware.  Set RX888_HW_TEST=1 to run.

set -euo pipefail

if [[ "${RX888_HW_TEST:-0}" != "1" ]]; then
    echo "skip hw_stop_start (set RX888_HW_TEST=1 to enable)"
    exit 0
fi

STREAM="${RX888_STREAM:-./rx888_stream}"
FIRMWARE="${RX888_FW:-firmware/SDDC_FX3.img}"
RATE="${RX888_RATE:-32000000}"
N="${RX888_CYCLES:-5}"
PER_SECS="${RX888_PER_SECS:-2}"

[[ -x "$STREAM" ]]   || { echo "missing $STREAM"; exit 2; }
[[ -f "$FIRMWARE" ]] || { echo "missing $FIRMWARE (run: make firmware)"; exit 2; }

failures=0

for i in $(seq 1 "$N"); do
    echo "--- cycle $i / $N ---"
    OUT=$(mktemp)

    set +e
    timeout --preserve-status --kill-after=1 $((PER_SECS + 2)) \
        "$STREAM" -f "$FIRMWARE" -s "$RATE" >"$OUT" 2>/dev/null
    rc=$?
    set -e

    bytes=$(stat -c %s "$OUT")
    rm -f "$OUT"

    # Acceptable exit codes: 0 (clean), 124 (timeout fired), 143 (SIGTERM).
    if [[ $rc -ne 0 && $rc -ne 124 && $rc -ne 143 ]]; then
        echo "FAIL cycle $i: rc=$rc"
        failures=$((failures + 1))
        continue
    fi

    # Expect each cycle to produce a meaningful amount of data.
    # Very lax bound: at least 1/8 of a full second's worth, since the
    # firmware-upload + control-transfer dance eats startup time on the
    # first cycle.
    min=$(( RATE * 2 / 8 ))
    if (( bytes < min )); then
        echo "FAIL cycle $i: only $bytes bytes (expected >= $min)"
        failures=$((failures + 1))
        continue
    fi

    echo "ok   cycle $i: $bytes bytes"
done

if (( failures != 0 )); then
    echo "FAILED: $failures of $N cycles"
    exit 1
fi
echo "ALL OK ($N cycles)"
