#!/usr/bin/env bash
# tone_quality_plots.sh — generate gnuplot figures of the tone_quality
# discriminator for the three synthetic cases (clean / drop / garble).
#
# Pipeline per case:
#   numpy synth  ->  tone_monitor --source --iqlog  ->  tone_quality.py
#   --plotdata (per-record demodulated time series)  ->  gnuplot PNG.
#
# Each figure has two panels vs time: unwrapped phase (a SLIP shows as a ~75 deg
# step) and amplitude (a GARBLE shows as a dip). No hardware; needs python3 +
# numpy + gnuplot.
#
# Usage: tone_quality_plots.sh [tone_monitor] [outdir]

set -euo pipefail

TM="${1:-./tone_monitor}"
OUT="${2:-.}"
HERE="$(cd "$(dirname "$0")" && pwd)"
TQ="$HERE/tone_quality.py"

[[ -x "$TM" ]] || { echo "missing tone_monitor: $TM" >&2; exit 2; }
command -v gnuplot >/dev/null 2>&1 || { echo "SKIP: gnuplot not installed"; exit 0; }
python3 -c 'import numpy' >/dev/null 2>&1 || { echo "SKIP: numpy not available"; exit 0; }
mkdir -p "$OUT"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

python3 - "$work" <<'PY'
import numpy as np, sys, os
work = sys.argv[1]
fs, ft, A, N = 129.6e6, 27e6, 10000.0, 600_000
n = np.arange(N)
clean = np.round(A*np.cos(2*np.pi*ft/fs*n)).astype('<i2')
clean.tofile(os.path.join(work, 'clean.s16'))
k = N//2
np.concatenate([clean[:k], clean[k+1:]]).astype('<i2').tofile(os.path.join(work,'drop.s16'))
g = clean.copy()
g[k:k+50] = np.random.default_rng(1).integers(-int(A), int(A), 50).astype('<i2')
g.astype('<i2').tofile(os.path.join(work, 'garble.s16'))
PY

for w in clean drop garble; do
    "$TM" --source "$work/$w.s16" --iqlog "$work/$w.iq" --decim 2400 >/dev/null 2>&1
    python3 "$TQ" "$work/$w.iq" --plotdata "$work/$w.dat" >/dev/null
done

for w in clean drop garble; do
    gnuplot <<GP
set terminal pngcairo noenhanced size 900,620 font ",11"
set output "$OUT/tone_quality_$w.png"
stats "$work/$w.dat" using 3 nooutput
amed = STATS_median
set multiplot layout 2,1 title "tone_quality - '$w' (coherent 27 MHz tone, decim 2400 -> 54 kHz baseband)"
set grid
set xlabel "time (ms)"
set ylabel "unwrapped phase (deg)"
set yrange [-100:25]
plot "$work/$w.dat" using 2:5 with lines lw 2 lc rgb "#1f77b4" title "phase (slip -> step)"
set ylabel "amplitude (LSB)"
set yrange [amed*0.90 : amed*1.05]
plot "$work/$w.dat" using 2:3 with lines lw 2 lc rgb "#d62728" title "amplitude (garble -> dip)"
unset multiplot
GP
    echo "wrote $OUT/tone_quality_$w.png"
done
