#!/usr/bin/env bash
# hw_fx3_cmd.sh — hardware checks for fx3_cmd diagnostics and the
# --no-claim concurrency path.
#
# Against a real RX888mk2 this validates:
#   1. firmware load + probe          (fx3_cmd -F <img> test)
#   2. read-only diagnostics idle      (stats, stats_pll)
#   3. exclusive-access guard          a normal fx3_cmd command is refused
#                                      with "Resource busy" (exit 1) while
#                                      rx888_stream holds interface 0
#   4. --no-claim concurrency          read-only commands succeed alongside
#                                      the stream, and GETSTATS dma_count
#                                      advances between two snapshots while
#                                      boot_count stays put (stream kept
#                                      running, no device reset)
#   5. --no-claim allowlist            a write command is rejected (exit 2)
#   6. post-stream health              claim works again once the streamer
#                                      detaches
#
# Non-destructive: leaves the device loaded and idle. usbreset/reload are
# destructive and remain manual.
#
# Requires hardware. Set RX888_HW_TEST=1 to run.

set -euo pipefail

if [[ "${RX888_HW_TEST:-0}" != "1" ]]; then
    echo "skip hw_fx3_cmd (set RX888_HW_TEST=1 to enable)"
    exit 0
fi

STREAM="${RX888_STREAM:-./rx888_stream}"
FX3="${RX888_FX3:-./fx3_cmd}"
FIRMWARE="${RX888_FW:-firmware/SDDC_FX3.img}"
RATE="${RX888_RATE:-32000000}"

[[ -x "$STREAM" ]]   || { echo "missing $STREAM"; exit 2; }
[[ -x "$FX3" ]]      || { echo "missing $FX3"; exit 2; }
[[ -f "$FIRMWARE" ]] || { echo "missing $FIRMWARE (run: make firmware)"; exit 2; }

failures=0
ok()   { echo "ok   $*"; }
fail() { echo "FAIL $*"; failures=$((failures+1)); }

run_rc() { set +e; "$@" >/dev/null 2>&1; local rc=$?; set -e; echo "$rc"; }

# Assert a command exits with an expected status.
check() {                # check <want_rc> <label> <cmd...>
    local want="$1" label="$2"; shift 2
    local rc; rc="$(run_rc "$@")"
    if [[ "$rc" -eq "$want" ]]; then
        ok "$label (rc=$rc)"
    else
        fail "$label: expected rc=$want, got rc=$rc"
    fi
}

# Pull a numeric "<field>=<N>" out of a `stats` line (fields are unique).
stat_field() {           # stat_field <field> [fx3 opts...]
    local field="$1"; shift
    "$FX3" "$@" stats 2>/dev/null | sed -n "s/.*${field}=\([0-9]\{1,\}\).*/\1/p"
}

STREAM_PID=""
cleanup() {
    if [[ -n "$STREAM_PID" ]] && kill -0 "$STREAM_PID" 2>/dev/null; then
        kill "$STREAM_PID" 2>/dev/null || true
        wait "$STREAM_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

# --- 1. load firmware + probe -------------------------------------------------
# -F uploads only if the device is in bootloader mode; a no-op if already app.
probe=$(mktemp)
if "$FX3" -F "$FIRMWARE" test >"$probe" 2>&1; then
    ok "load + test: $(tail -1 "$probe")"
else
    fail "load + test failed: $(cat "$probe")"
    rm -f "$probe"
    echo "FAILED ($failures) — no usable device, aborting"
    exit 1
fi
rm -f "$probe"

# --- 2. read-only diagnostics, device idle -----------------------------------
check 0 "stats (idle)"     "$FX3" stats
check 0 "stats_pll (idle)" "$FX3" stats_pll

# --- 3. start a streamer, verify the exclusive-access guard ------------------
"$STREAM" -f "$FIRMWARE" -s "$RATE" >/dev/null 2>&1 &
STREAM_PID=$!
sleep 2   # let the stream come up

if ! kill -0 "$STREAM_PID" 2>/dev/null; then
    fail "streamer exited early; cannot test concurrency"
    echo "FAILED ($failures)"
    exit 1
fi

check 1 "normal 'test' refused while streaming" "$FX3" test

# --- 4. --no-claim reads succeed and the stream keeps running ----------------
check 0 "--no-claim stats during stream"     "$FX3" --no-claim stats
check 0 "--no-claim stats_pll during stream" "$FX3" --no-claim stats_pll

dma1="$(stat_field dma  --no-claim)"
boot1="$(stat_field boot --no-claim)"
sleep 1
dma2="$(stat_field dma  --no-claim)"
boot2="$(stat_field boot --no-claim)"

if [[ -n "$dma1" && -n "$dma2" && "$dma2" -gt "$dma1" ]]; then
    ok "stream uninterrupted: dma advanced $dma1 -> $dma2"
else
    fail "dma_count did not advance during --no-claim reads ($dma1 -> $dma2)"
fi
if [[ -n "$boot1" && "$boot1" == "$boot2" ]]; then
    ok "no device reset during reads (boot=$boot1)"
else
    fail "boot_count changed ($boot1 -> $boot2): device reset under the stream"
fi

# --- 5. --no-claim allowlist rejects a write command -------------------------
check 2 "--no-claim gpio rejected" "$FX3" --no-claim gpio 0x20

# --- 6. device healthy again once the streamer detaches ----------------------
cleanup; STREAM_PID=""
sleep 1
check 0 "post-stream 'test' (claim works again)" "$FX3" test

if (( failures != 0 )); then
    echo "FAILED ($failures)"
    exit 1
fi
echo "ALL OK"
