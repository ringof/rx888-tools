#!/usr/bin/env bash
# hw_fx3_cmd.sh — hardware checks for fx3_cmd diagnostics and the
# --no-claim concurrency path.
#
# Against a real RX888mk2 this validates:
#   1. firmware load + probe          (fx3_cmd -F <img> test)
#   2. idle diagnostics + config pokes stats/stats_pll, i2cr (Si5351 status),
#                                      adc/att/vga, start/stop, and the
#                                      stats_shdn graceful-degradation path on
#                                      legacy (26-byte GETSTATS) firmware
#   3. exclusive-access guard          a normal fx3_cmd command is refused
#                                      with "Resource busy" (exit 1) while
#                                      rx888_stream holds interface 0
#   4. --no-claim concurrency          stream-safe commands succeed alongside
#                                      the stream (incl. live att/vga tuning),
#                                      GETSTATS dma_count advances while
#                                      boot_count stays put, and --no-claim
#                                      stack_check (the debug/EP0 channel) does
#                                      not stall the stream (probes issue #27)
#   5. --no-claim gating               a stream-unsafe command (gpio) is
#                                      rejected without --force (exit 2)
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
I2C_ADDR="${RX888_SI5351_ADDR:-0xC0}"   # Si5351 I2C write address (0x60 << 1)

[[ -x "$STREAM" ]]   || { echo "missing $STREAM"; exit 2; }
[[ -x "$FX3" ]]      || { echo "missing $FX3"; exit 2; }
[[ -f "$FIRMWARE" ]] || { echo "missing $FIRMWARE (run: make firmware)"; exit 2; }

failures=0
ok()   { echo "ok   $*"; }
fail() { echo "FAIL $*"; failures=$((failures+1)); }
note() { echo "note $*"; }   # informational; does not affect pass/fail

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

# --- 2. idle diagnostics + config pokes --------------------------------------
check 0 "stats (idle)"     "$FX3" stats
check 0 "stats_pll (idle)" "$FX3" stats_pll

# Detect the GETSTATS payload flavor (30-byte firmware exposes gpio_state).
STATS_OUT="$("$FX3" stats 2>/dev/null || true)"
if grep -q "gpio=0x" <<<"$STATS_OUT"; then HAS_GPIO=1; else HAS_GPIO=0; fi
if [[ $HAS_GPIO -eq 1 ]]; then
    note "GETSTATS: 30-byte payload (gpio_state present)"
else
    note "GETSTATS: 26-byte legacy payload (no gpio_state)"
fi

# Read-only I2C: Si5351 status register (addr reg=0, 1 byte).
check 0 "i2cr Si5351 status" "$FX3" i2cr "$I2C_ADDR" 0 1

# Config pokes on the idle device: ADC clock, attenuator, VGA.
check 0 "adc $RATE" "$FX3" adc "$RATE"
check 0 "att 0"     "$FX3" att 0
check 0 "vga 0"     "$FX3" vga 0

# GPIF start/stop pair (no consumer; stop immediately to return to idle).
check 0 "start" "$FX3" start
check 0 "stop"  "$FX3" stop

# stats_shdn needs the 30-byte GETSTATS (gpio_state).  On legacy firmware it
# must degrade *gracefully* (clear message, not an opaque error); on 30-byte
# firmware it is a real functional check, reported informationally so a firmware
# result does not fail this harness.
set +e
shdn_out="$("$FX3" stats_shdn 2>&1)"; shdn_rc=$?
set -e
if [[ $HAS_GPIO -eq 0 ]]; then
    if [[ $shdn_rc -ne 0 ]] && grep -qi "gpio_state\|30-byte" <<<"$shdn_out"; then
        ok "stats_shdn degrades gracefully on legacy firmware (rc=$shdn_rc)"
    else
        fail "stats_shdn legacy path: expected a graceful gpio_state message, got rc=$shdn_rc: $shdn_out"
    fi
else
    note "stats_shdn (30-byte firmware) rc=$shdn_rc: $(tail -1 <<<"$shdn_out")"
fi

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

# --no-claim stack_check is the one stream-safe no-claim command that drives the
# READINFODEBUG debug channel (EP0).  Running it mid-stream probes issue #27:
# does the debug channel disturb an active stream?  We do not assert
# stack_check's own result (its console parse may legitimately fail on some
# firmware) — only that the stream keeps advancing across it.
dma_sc1="$(stat_field dma --no-claim)"
run_rc "$FX3" --no-claim stack_check >/dev/null
sleep 1
dma_sc2="$(stat_field dma --no-claim)"
if [[ -n "$dma_sc1" && -n "$dma_sc2" && "$dma_sc2" -gt "$dma_sc1" ]]; then
    ok "--no-claim stack_check non-disruptive: dma $dma_sc1 -> $dma_sc2 (re #27)"
else
    fail "stream stalled across --no-claim stack_check ($dma_sc1 -> $dma_sc2) (re #27)"
fi

# att/vga are stream-SAFE per rx888-firmware#170 (SETARGFX3 live tuning): they
# must succeed under --no-claim during a stream and not disrupt it.
dma_t1="$(stat_field dma --no-claim)"
check 0 "--no-claim att during stream" "$FX3" --no-claim att 0
check 0 "--no-claim vga during stream" "$FX3" --no-claim vga 0
sleep 1
dma_t2="$(stat_field dma --no-claim)"
if [[ -n "$dma_t1" && -n "$dma_t2" && "$dma_t2" -gt "$dma_t1" ]]; then
    ok "--no-claim att/vga non-disruptive: dma $dma_t1 -> $dma_t2"
else
    fail "stream stalled across --no-claim att/vga ($dma_t1 -> $dma_t2)"
fi

# --- 5. --no-claim gating: stream-unsafe rejected without --force ------------
# (We don't run --force write commands here — they'd intentionally disrupt the
# stream; the smoke test covers that --force passes the gate.)
check 2 "--no-claim gpio rejected (needs --force)" "$FX3" --no-claim gpio 0x20

# --- 6. device healthy again once the streamer detaches ----------------------
cleanup; STREAM_PID=""
sleep 1
check 0 "post-stream 'test' (claim works again)" "$FX3" test

if (( failures != 0 )); then
    echo "FAILED ($failures)"
    exit 1
fi
echo "ALL OK"
