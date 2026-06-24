#!/usr/bin/env bash
# reproduce — the hardware-free proof of the RX888 PPS/tone data-integrity kit.
#
# Runs the synthetic clean/drop/garble cases through the REAL DSP path
# (tone_monitor --source) and the offline analyzer (tone_quality.py), prints
# the analyzer's conclusions so you can read them, then renders the gnuplot
# figures. No RX888 required. Mount a volume at /out to receive the PNGs:
#   docker run --rm -v "$PWD/out:/out" rx888-ppskit
set -euo pipefail
cd /opt/rx888-tools

TM=./tone_monitor
TQ=tests/tone_quality.py
OUT="${OUT_DIR:-/out}"

echo "=================================================================="
echo " rx888-tools — PPS / coherent-tone data-integrity proof (no HW)"
echo "=================================================================="

echo
echo "## 1. Automated checks (CLI + DSP + analyzer) ##"
tests/tone_monitor_smoke.sh "$TM"
tests/tone_monitor_replay.sh "$TM"

echo
echo "## 2. Watch the discriminator work (synthetic clean/drop/garble) ##"
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
np.concatenate([clean[:k], clean[k+1:]]).astype('<i2').tofile(os.path.join(work, 'drop.s16'))
g = clean.copy()
g[k:k+50] = np.random.default_rng(1).integers(-int(A), int(A), 50).astype('<i2')
g.astype('<i2').tofile(os.path.join(work, 'garble.s16'))
PY
for w in clean drop garble; do
    "$TM" --source "$work/$w.s16" --iqlog "$work/$w.iq" --decim 2400 >/dev/null 2>&1
    echo "------ $w ------"
    python3 "$TQ" "$work/$w.iq" | sed -n '/\[1\]/,/^$/p;/\[2\]/,/^$/p;/\[3\]/,/correlate/p' | sed 's/^/   /'
done

echo
if mkdir -p "$OUT" 2>/dev/null && [ -w "$OUT" ]; then
    echo "## 3. Rendering gnuplot figures to $OUT ##"
    tests/tone_quality_plots.sh "$TM" "$OUT" 2>&1 | grep -v '^Warning' || true
    ls -l "$OUT"/*.png 2>/dev/null || true
else
    echo "## 3. gnuplot figures — mount a volume at /out to receive them:"
    echo "      docker run --rm -v \"\$PWD/out:/out\" rx888-ppskit"
fi

echo
echo "=================================================================="
echo " All checks passed. For live RX888 capture + the timing test, see"
echo " REPRODUCE.md and scripts/host-timebase-setup.sh."
echo "=================================================================="
