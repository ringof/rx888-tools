#!/usr/bin/env bash
# tone_quality_hourplot.sh — render a REAL capture (from the rig) into gnuplot
# PNGs. Unlike tone_quality_plots.sh (synthetic clean/drop/garble to prove the
# discriminator), this plots the whole-run telemetry you actually collected, so
# you can eyeball a long acquisition instead of reading the summary numbers.
#
# It plots, in order of what you already have on the SSD:
#   1. statslog CSV (small, no numpy) -> amplitude, residual carrier (drift),
#      and worst phase-step vs time over the whole run. This alone answers the
#      amplitude / drift / slip questions and needs only gnuplot.
#   2. iqlog --powers  -> baseband Welch spectrum. The iqlog is already the
#      demodulated tone, so its energy sits near DC (the residual carrier); this
#      shows the close-in noise floor / sidebands, not the full-band 5/24 line
#      (that frac-5/24 check is for a RAW .s16 capture; see the runbook).
#   3. iqlog --window --plotdata -> per-window amplitude / residual carrier /
#      phase jitter / slip-count vs time (constant memory, so it plots a
#      multi-GB / multi-hour iqlog that the per-record path cannot load).
#
# Every panel is a RAW measurement vs time — no verdict, no pass/fail (that
# lives in tone_quality.py's text analysis). This is just the picture.
#
# Usage: tone_quality_hourplot.sh STATSLOG.csv [IQLOG.iq] [outdir] [window_sec]
#   e.g. tone_quality_hourplot.sh /data/run.csv /data/run.iq ./out 60

set -euo pipefail

STATS="${1:?usage: tone_quality_hourplot.sh STATSLOG.csv [IQLOG.iq] [outdir] [window_sec]}"
IQ="${2:-}"
OUT="${3:-.}"
WIN="${4:-60}"
HERE="$(cd "$(dirname "$0")" && pwd)"
TQ="$HERE/tone_quality.py"

command -v gnuplot >/dev/null 2>&1 || { echo "SKIP: gnuplot not installed"; exit 0; }
[[ -f "$STATS" ]] || { echo "missing statslog: $STATS" >&2; exit 2; }
mkdir -p "$OUT"

base="$(basename "${STATS%.*}")"

# ---- 1. statslog: amplitude / residual carrier / worst phase-step vs time ----
# Columns (1-indexed): 2=sec 4=amp_mean 5=amp_min 6=amp_max 7=resid_freq_hz
# 8=max_step_deg. The "# fs=" and "# end" lines are comments; the "time,sec,..."
# header line is skipped because column 2 ("sec") is non-numeric.
gnuplot <<GP
set terminal pngcairo noenhanced size 1000,860 font ",11"
set output "$OUT/${base}_statslog.png"
set datafile separator comma
set datafile commentschars "#"
set multiplot layout 3,1 title "acquisition telemetry (statslog) — $base"
set grid
set xlabel "time (s)"
set ylabel "amplitude (LSB)"
plot "$STATS" using 2:5:6 with filledcurves lc rgb "#cfe3f5" title "min..max", \
     "$STATS" using 2:4 with lines lw 2 lc rgb "#1f77b4" title "amp_mean"
set ylabel "residual carrier (Hz)"
plot "$STATS" using 2:7 with lines lw 2 lc rgb "#2ca02c" title "resid_freq (drift)"
set ylabel "worst phase step (deg)"
set yrange [0:*]
plot "$STATS" using 2:8 with impulses lw 2 lc rgb "#d62728" title "max_step (75 = 1 slip)", \
     37.5 with lines lc rgb "#888888" dt 2 title "half-slip"
unset multiplot
GP
echo "wrote $OUT/${base}_statslog.png"

# ---- iqlog plots (need numpy for tone_quality.py) ----
if [[ -n "$IQ" ]]; then
    [[ -f "$IQ" ]] || { echo "missing iqlog: $IQ" >&2; exit 2; }
    if ! python3 -c 'import numpy' >/dev/null 2>&1; then
        echo "SKIP iqlog plots: numpy not available"; exit 0
    fi
    iqbase="$(basename "${IQ%.*}")"

    # ---- 2. baseband power spectrum ----
    python3 "$TQ" "$IQ" --powers > "$OUT/${iqbase}_powers.dat" 2> "$OUT/${iqbase}_powers.peaks"
    gnuplot <<GP
set terminal pngcairo noenhanced size 1000,520 font ",11"
set output "$OUT/${iqbase}_powers.png"
set title "baseband power spectrum — $iqbase (iqlog is demodulated: tone sits near DC = the residual carrier)"
set grid
set xlabel "frequency (MHz)"
set ylabel "power (dBFS)"
plot "$OUT/${iqbase}_powers.dat" using 1:2 with lines lw 1.5 lc rgb "#1f77b4" notitle
GP
    echo "wrote $OUT/${iqbase}_powers.png (peaks: $OUT/${iqbase}_powers.peaks)"

    # ---- 3. per-window series over the whole (multi-GB) iqlog ----
    python3 "$TQ" "$IQ" --window "$WIN" --plotdata "$OUT/${iqbase}_win.dat" >/dev/null
    # Columns: 1=window 2=t_s 3=amp_mean 4=amp_min 5=amp_max 6=resid_carrier_hz
    # 7=jitter_deg 8=slips.
    gnuplot <<GP
set terminal pngcairo noenhanced size 1000,1040 font ",11"
set output "$OUT/${iqbase}_window.png"
# Compute every panel's y-range UP FRONT, while y is autoscaled: stats honors
# the active yrange, so a range left set by a prior panel would filter out the
# next column's points ("all points out of range"). A clean disciplined run is
# near-flat (or exactly flat), so give a degenerate range a visible band instead
# of letting gnuplot warn + autoscale to nothing.
df = "$OUT/${iqbase}_win.dat"
stats df using 6 nooutput; r_lo = STATS_min; r_hi = STATS_max
rp = (r_hi > r_lo ? (r_hi-r_lo)*0.1 : (abs(r_hi)>0 ? abs(r_hi)*0.1 : 1))
stats df using 7 nooutput; j_hi = STATS_max
jp = (j_hi > 0 ? j_hi*0.1 : 1)
stats df using 8 nooutput; s_hi = STATS_max
set multiplot layout 4,1 title "iqlog per-window series (${WIN}s windows) — $iqbase"
set grid
set xlabel "time (s)"
set ylabel "amplitude (LSB)"
plot df using 2:4:5 with filledcurves lc rgb "#cfe3f5" title "min..max", \
     df using 2:3 with lines lw 2 lc rgb "#1f77b4" title "amp_mean"
set ylabel "residual carrier (Hz)"
set yrange [r_lo-rp : r_hi+rp]
plot df using 2:6 with lines lw 2 lc rgb "#2ca02c" title "resid_carrier (drift)"
set ylabel "phase jitter (deg RMS)"
set yrange [0 : j_hi+jp]
plot df using 2:7 with lines lw 2 lc rgb "#9467bd" title "jitter"
set ylabel "grid slips / window"
# A clean hour has slips=0 everywhere; floor the top at 1 so it draws a flat
# baseline rather than an empty range.
set yrange [0 : (s_hi > 0 ? s_hi*1.2 : 1)]
plot df using 2:8 with impulses lw 2 lc rgb "#d62728" title "slips"
unset multiplot
GP
    echo "wrote $OUT/${iqbase}_window.png"
fi
