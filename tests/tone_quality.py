#!/usr/bin/env python3
# tone_quality.py — corruption + analog-quality check on a coherent-tone capture.
#
# RIG PRECONDITION (what makes this work): a single GPSDO 27 MHz output is
# split to BOTH the RX888 Si5351 reference AND the RF input, and the Si5351 is
# configured with INTEGER multipliers only (PLL x24 -> 648 MHz VCO -> /5 ->
# 129.6 MHz; NO fractional MultiSynth, so no delta-sigma phase dithering). The
# tone is therefore exactly phase-coherent with the sample clock:
#
#     f_tone / fs = 27 / 129.6 = 5 / 24  (exact)
#
# so the digitized tone is a DETERMINISTIC period-24 sequence — a built-in test
# pattern, which the LTC2208 does not provide on its own. That coherence is the
# whole basis of detector (1); the tool self-checks it (flat period-N residual
# + flat DDC phase => coherence held; structure => fractional divider / not
# split / non-coherent).
#
# Detectors:
#   1. Period-N self-consistency (N = fs / gcd(fs, ftone)) — x[n] vs x[n-N]
#      should match within ADC noise. A dropped/duplicated/garbled sample
#      breaks it: a garble is a lone spike; a drop/dup is a *persistent* step
#      (the grid slips one sample = a 360*ftone/fs = 75 deg phase jump). This is
#      the corruption verdict; it needs no signal model, only coherence.
#   2. DDC to baseband + unwrapped phase — confirms coherence (a flat line) and
#      shows slips as phase steps; a second, independent view.
#   3. FFT — SINAD / SFDR / ENOB / noise floor (analog quality + spurs).
#
# Input: raw interleaved int16 little-endian REAL samples (e.g. an rx888_stream
# capture). Optional --markers FILE (one cumulative sample index per line, e.g.
# from pps_integrity) to correlate corruption with the PPS marker splices.
#
# Usage: tone_quality.py CAPTURE.s16 [--fs HZ] [--ftone HZ] [--markers FILE]
#                        [--max-samples N]
#
# Requires numpy.

import sys, argparse, math
try:
    import numpy as np
except ImportError:
    sys.exit("tone_quality.py needs numpy (pip install numpy)")


def period_samples(fs, ftone):
    g = math.gcd(int(round(fs)), int(round(ftone)))
    return int(round(fs)) // g, int(round(ftone)) // g  # (N samples, cycles)


def load(path, max_samples):
    try:
        x = np.fromfile(path, dtype='<i2', count=(max_samples if max_samples else -1))
    except OSError as e:
        sys.exit(f"tone_quality.py: cannot read {path}: {e}")
    if x.size == 0:
        sys.exit(f"tone_quality.py: {path} is empty or not int16 data")
    return x.astype(np.float64)


def detect_corruption(x, N, block):
    """Period-N self-consistency. x[n] vs x[n-N] should match within ADC noise.
    A corruption (drop/dup/garble) breaks it in the ~N-sample window at the
    event (a drop heals after N samples once both terms are shifted equally, so
    this detector LOCALIZES events; the DDC phase step below says whether a drop
    actually slipped the grid). Returns (clean?, events, floor, thr, rms,
    nblocks) with each event = (sample_index, magnitude_x_floor), localized to
    the exact peak sample."""
    res = x[N:] - x[:-N]
    nb = len(res) // block
    if nb < 2:
        return None, [], 0.0, 0.0, np.array([]), 0
    rb = res[:nb*block].reshape(nb, block)
    rms = np.sqrt(np.mean(rb**2, axis=1))
    floor = float(np.median(rms))
    thr = max(8.0 * floor, 1.0)
    bad = rms > thr
    events, prev = [], False
    for b in range(nb):
        if bad[b] and not prev:                       # onset of a disturbance
            j = int(np.argmax(np.abs(rb[b])))         # exact peak within block
            idx = b * block + j + N                   # -> x[] sample index
            events.append((idx, rms[b] / max(floor, 1e-9)))
        prev = bad[b]
    return (len(events) == 0), events, floor, thr, rms, nb


def analog_metrics(x, fs, ftone):
    """SINAD / SFDR / ENOB over a windowed chunk, fundamental at ftone."""
    n = 1 << int(math.floor(math.log2(min(len(x), 1 << 22))))
    if n < 4096:
        return None
    seg = x[:n] - np.mean(x[:n])
    w = np.blackman(n)
    X = np.abs(np.fft.rfft(seg * w))**2
    f = np.fft.rfftfreq(n, 1.0 / fs)
    k0 = int(np.argmin(np.abs(f - ftone)))
    def bin_band(k):  # fundamental leaks a few bins under the window
        lo, hi = max(1, k-4), min(len(X), k+5)
        return lo, hi
    lo, hi = bin_band(k0)
    sig = X[lo:hi].sum()
    noise = X.copy(); noise[0] = 0.0; noise[lo:hi] = 0.0          # drop DC + fundamental
    nd = noise.sum()
    sinad = 10*np.log10(sig / nd) if nd > 0 else float('inf')
    spur = noise.max()
    sfdr = 10*np.log10(sig / spur) if spur > 0 else float('inf')
    enob = (sinad - 1.76) / 6.02
    return dict(sinad=sinad, sfdr=sfdr, enob=enob, fund_bin=k0,
                fund_hz=f[k0], spur_hz=f[int(np.argmax(noise))])


def ddc_phase(x, fs, ftone, decim=64):
    """Mix to baseband, decimate, unwrapped phase. Coherent => ~flat."""
    n = (len(x) // decim) * decim
    nn = np.arange(n)
    bb = x[:n] * np.exp(-2j*np.pi*ftone/fs * nn)
    bb = bb[:n].reshape(-1, decim).mean(axis=1)       # boxcar LPF + decimate
    ph = np.unwrap(np.angle(bb))
    t = np.arange(len(ph))
    a, b = np.polyfit(t, ph, 1)                        # detrend residual freq
    resid = ph - (a*t + b)
    return np.degrees(a) * (fs/decim) / 360.0, np.degrees(resid)  # (resid Hz, deg)


def main(argv):
    ap = argparse.ArgumentParser(description="coherent-tone corruption + quality check")
    ap.add_argument("capture")
    ap.add_argument("--fs", type=float, default=129.6e6)
    ap.add_argument("--ftone", type=float, default=27e6)
    ap.add_argument("--markers")
    ap.add_argument("--max-samples", type=int, default=0)
    ap.add_argument("--block", type=int, default=0)
    a = ap.parse_args(argv[1:])

    N, cyc = period_samples(a.fs, a.ftone)
    block = a.block or (N * 256)
    x = load(a.capture, a.max_samples)
    print(f"===== {a.capture} =====")
    print(f"  fs={a.fs/1e6:g} MSPS  ftone={a.ftone/1e6:g} MHz  "
          f"period N={N} samples ({cyc} cycles)  loaded {len(x)} samples "
          f"({len(x)/a.fs*1e3:.1f} ms)")
    if len(x) < N * 512:
        print("  (capture too short for a meaningful check)"); return 2

    clean, events, floor, thr, rms, nb = detect_corruption(x, N, block)
    print(f"\n  [1] period-{N} self-consistency  (block={block} samples)")
    print(f"      residual floor={floor:.1f} LSB  threshold={thr:.1f} LSB  "
          f"corrupt blocks={int((rms>thr).sum())}/{nb}")
    if clean:
        print("      >>> CLEAN: every block consistent to ADC noise — no "
              "dropped/duplicated/garbled samples.")
    else:
        print(f"      >>> {len(events)} disturbance(s) localized (drop/dup or garble):")
        for idx, mag in events[:20]:
            print(f"        sample {idx:>12}  ({mag:.0f}x floor)")

    # Phase step => a sample slipped the grid (drop/dup); a per-step slip count
    # distinguishes slips from garbles, which leave the phase flat.
    dfreq, dphase = ddc_phase(x, a.fs, a.ftone)
    dstep = np.abs(np.diff(dphase))
    slips = int((dstep > 40.0).sum())            # ~75 deg per single-sample slip
    p2p, std = float(np.ptp(dphase)), float(np.std(dphase))
    print(f"\n  [2] DDC phase (coherence + slip check)")
    print(f"      residual std {std:.2f} deg, p2p {p2p:.1f} deg; "
          f"phase steps >40 deg: {slips}")
    if slips == 0 and p2p < 30.0:
        print("      >>> coherence intact, NO grid slips. Any [1] disturbances "
              "are garbles / bit-errors, not dropped samples.")
    else:
        print(f"      >>> {slips} grid SLIP(s) (dropped/duplicated samples) — the "
              "phase steps ~75 deg/slip. (Residual-freq fit is corrupted by the "
              "step, so ignore it here.)")

    m = analog_metrics(x, a.fs, a.ftone)
    if m:
        print(f"\n  [3] analog quality (FFT)")
        print(f"      fundamental {m['fund_hz']/1e6:.4f} MHz  SINAD {m['sinad']:.1f} dB"
              f"  SFDR {m['sfdr']:.1f} dB  ENOB {m['enob']:.1f} bits"
              f"  worst spur {m['spur_hz']/1e6:.3f} MHz")

    if a.markers:
        mk = np.loadtxt(a.markers, dtype=np.int64, ndmin=1)
        ev = np.array([e[0] for e in events], dtype=np.int64)
        if len(ev):
            near = sum(np.min(np.abs(mk - i)) < N for i in ev)
            print(f"\n  marker correlation: {near}/{len(ev)} corruption events "
                  f"within {N} samples of a marker splice (of {len(mk)} markers)")
        else:
            print(f"\n  marker correlation: no corruption events to correlate "
                  f"({len(mk)} markers)")
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
