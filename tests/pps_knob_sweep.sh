#!/usr/bin/env bash
# pps_knob_sweep.sh — sweep librx888's -q/-p against the throughput ceiling.
#
# At high sample rates the FX3 can drop DMA buffers it cannot drain to USB fast
# enough (see doc/pps_integrity.md, "Throughput ceiling"). This drives
# pps_integrity across queue-depth (-q) and transfer-size (-p) settings to find
# whether the loss is tunable (host/scheduling-bound) or a hard drain limit.
#
# The figure of merit per run is the clock-INDEPENDENT produced-vs-delivered
# loss: the "undelivered ... MB" field of the DMA-buffers line (0 / negative =
# lossless). The script runs controls at a low rate, sweeps -q at the high rate,
# picks the depth with the least loss, sweeps -p at that depth, then prints a
# summary table.
#
# HARDWARE TEST — needs an RX888 (and loaded firmware). Not part of `make check`.
#
# Env overrides:
#   PPS=./pps_integrity   RX888_FW=SDDC_FX3.img   OUTDIR=<dir>
#   LORATE=64  HIRATE=129.6                 (MSPS; fractional ok)
#   DUR_CTRL=0.25  DUR_SWEEP=0.33           (hours per control / sweep cell)
#   QSWEEP="16 32 64 128"                   (depths to try at HIRATE, p=1024)
#   PSWEEP="512 2048 4096"                  (transfer sizes at the winning depth)
#
# Note: 20-min cells rank settings; re-run the winner for 1-3 h to confirm.

# NOT set -e: pps_integrity exits non-zero on loss/displaced, which is data, not
# a script failure.
set -uo pipefail

PPS="${PPS:-./pps_integrity}"
FW="${RX888_FW:-SDDC_FX3.img}"
OUTDIR="${OUTDIR:-pps_sweep_$(date +%Y%m%d_%H%M%S)}"
LORATE="${LORATE:-64}"
HIRATE="${HIRATE:-129.6}"
DUR_CTRL="${DUR_CTRL:-0.25}"
DUR_SWEEP="${DUR_SWEEP:-0.33}"
QSWEEP="${QSWEEP:-16 32 64 128}"
PSWEEP="${PSWEEP:-512 2048 4096}"

[[ -x "$PPS" ]] || { echo "missing or non-executable: $PPS (run 'make pps_integrity')" >&2; exit 2; }
[[ -f "$FW"  ]] || { echo "firmware not found: $FW (set RX888_FW=...)" >&2; exit 2; }

mkdir -p "$OUTDIR"
echo "sweep output -> $OUTDIR"

# Sanitize a rate like 129.6 into a filename-safe token (129p6).
tok() { echo "$1" | tr '.' 'p'; }

# run_cell <label> <rate> <q> <p> <hours>
run_cell() {
    local label="$1" rate="$2" q="$3" p="$4" dur="$5"
    local log="$OUTDIR/${label}.log"
    echo ">> $label : rate=$rate q=$q p=$p dur=${dur}h"
    "$PPS" "$dur" --rate "$rate" -q "$q" -p "$p" -f "$FW" >"$log" 2>&1
    if ! grep -q "PPS INTEGRITY RESULT" "$log"; then
        echo "   !! no result in $log (device/firmware problem?):" >&2
        tail -3 "$log" | sed 's/^/   /' >&2
        return 1
    fi
    local u; u=$(undeliv "$log")
    echo "   result=$(field "$log" 'Result:') undelivered=${u} MB"
}

# undeliv <log> -> signed MB the produced/delivered check reported (or NA).
undeliv() {
    grep -oE 'undelivered [+-][0-9.]+' "$1" 2>/dev/null | head -1 | awk '{print $2}' \
        || true
}
# field <log> <prefix> -> the rest of the first line starting with <prefix>.
field() { grep -m1 "$2" "$1" 2>/dev/null | sed "s/.*$2[[:space:]]*//" | cut -c1-40; }

# --- controls at the low (expected-lossless) rate ---
run_cell "$(tok $LORATE)_q32_p1024"  "$LORATE" 32 1024 "$DUR_CTRL" \
    || { echo "first run produced no result; aborting" >&2; exit 1; }
run_cell "$(tok $LORATE)_q16_p1024"  "$LORATE" 16 1024 "$DUR_CTRL"

# --- depth sweep at the high rate, p=1024 ---
for q in $QSWEEP; do
    run_cell "$(tok $HIRATE)_q${q}_p1024" "$HIRATE" "$q" 1024 "$DUR_SWEEP"
done

# --- pick the depth with the least loss (smallest undelivered) ---
best_q=""; best_val=""
for q in $QSWEEP; do
    v=$(undeliv "$OUTDIR/$(tok $HIRATE)_q${q}_p1024.log")
    [[ -z "$v" ]] && continue
    if [[ -z "$best_val" ]] || awk "BEGIN{exit !($v < $best_val)}"; then
        best_val="$v"; best_q="$q"
    fi
done
[[ -z "$best_q" ]] && best_q=32
echo "best depth: -q $best_q (undelivered ${best_val:-NA} MB) -> sweeping -p"

# --- transfer-size sweep at the winning depth ---
for p in $PSWEEP; do
    run_cell "$(tok $HIRATE)_q${best_q}_p${p}" "$HIRATE" "$best_q" "$p" "$DUR_SWEEP"
done

# --- summary table ---
echo
echo "=== KNOB SWEEP SUMMARY ($OUTDIR) ==="
printf "%-22s %-7s %12s %9s %9s\n" "cell(rate_q_p)" "result" "undeliv_MB" "spurious" "displ"
for log in "$OUTDIR"/*.log; do
    name=$(basename "$log" .log)
    res=$(field "$log" 'Result:')
    u=$(undeliv "$log"); u=${u:-NA}
    spur=$(grep -m1 'Spurious shorts:' "$log" | awk '{print $3}')
    disp=$(grep -m1 'Missed markers:' "$log" | sed -n 's/.*displaced: \([0-9]*\).*/\1/p')
    printf "%-22s %-7s %12s %9s %9s\n" "$name" "${res:-?}" "$u" "${spur:-?}" "${disp:-?}"
done
echo
echo "lossless = undelivered <= ~0 and result PASS. Re-run the best cell for"
echo "1-3 h to confirm before trusting it. Logs: $OUTDIR/"
