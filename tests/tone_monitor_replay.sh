#!/usr/bin/env bash
# tone_monitor_replay.sh — validate tone_monitor's DSP with no hardware.
#
# Synthesizes a coherent 27 MHz tone at 129.6 MSPS (the rig's exact 5/24 ratio),
# plus a one-sample DROP and a 50-sample GARBLE, feeds each through the REAL
# production path via --source, and checks the decimated baseband (iqlog):
#   clean  -> flat phase, flat amplitude
#   drop   -> a persistent ~75 deg phase step (one grid slip), amplitude flat
#   garble -> an amplitude dip, NO persistent phase step
# This exercises the same sample_cb/Goertzel/iqlog code the device path uses.
#
# Requires python3 + numpy; skips cleanly (exit 0) if unavailable.

set -euo pipefail

TM="${1:-./tone_monitor}"
[[ -x "$TM" ]] || { echo "missing or non-executable: $TM" >&2; exit 2; }

if ! python3 -c 'import numpy' >/dev/null 2>&1; then
    echo "SKIP tone_monitor_replay: python3+numpy not available"
    exit 0
fi

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
    "$TM" --source "$work/$w.s16" --iqlog "$work/$w.iq" --decim 2400 \
        >/dev/null 2>&1 || { echo "FAIL: tone_monitor failed on $w"; exit 1; }
done

python3 - "$work" <<'PY'
import numpy as np, struct, sys, os
work = sys.argv[1]
def load(p):
    with open(p, 'rb') as f:
        hdr = f.read(48)
        assert hdr[:7] == b'RX888IQ', "bad iqlog magic"
        iq = np.fromfile(f, dtype='<f4')
    return iq[0::2] + 1j*iq[1::2]
def stepstats(p):
    z = load(p); ph = np.degrees(np.angle(z))
    dph = (np.diff(ph) + 180) % 360 - 180
    amp = np.abs(z)
    return amp, ph, (np.abs(dph).max() if len(dph) else 0.0)

fail = 0
def check(cond, msg):
    global fail
    print(("ok   " if cond else "FAIL ") + msg)
    if not cond: fail += 1

amp, ph, mx = stepstats(os.path.join(work, 'clean.iq'))
check(mx < 1.0,            f"clean: no phase step (max {mx:.2f} deg)")
check(amp.std() < 1.0,     f"clean: flat amplitude (std {amp.std():.2f})")

amp, ph, mx = stepstats(os.path.join(work, 'drop.iq'))
check(70.0 < mx < 80.0,    f"drop: ~75 deg grid slip (max {mx:.2f} deg)")
check(amp.std() < 1.0,     f"drop: amplitude flat through slip (std {amp.std():.2f})")

amp, ph, mx = stepstats(os.path.join(work, 'garble.iq'))
check(mx < 5.0,            f"garble: no persistent phase step (max {mx:.2f} deg)")
check(amp.min() < 0.99*amp.max(), f"garble: amplitude dip (min {amp.min():.0f} < max {amp.max():.0f})")

sys.exit(1 if fail else 0)
PY
rc=$?
if [[ $rc -eq 0 ]]; then echo "ALL OK"; else echo "FAILED"; fi
exit $rc
