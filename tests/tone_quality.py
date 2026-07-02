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

import sys, argparse, math, struct
try:
    import numpy as np
except ImportError:
    sys.exit("tone_quality.py needs numpy (pip install numpy)")

IQLOG_MAGIC = b'RX888IQ'
IQLOG_HDRLEN = 48


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


# ------------------------- tone_monitor artifacts ------------------------- #
# tone_monitor (src/tone_monitor.c) demodulates the coherent tone inline and
# writes raw telemetry: a decimated complex baseband (--iqlog) and a per-second
# CSV (--statslog). This is the OFFLINE side: it reads those artifacts, runs the
# phase-locked tracking demodulator, and separates real slips/garbles from
# benign GPSDO/PLL/thermal wander (which a tracking loop follows and removes).

def is_iqlog(path):
    try:
        with open(path, 'rb') as f:
            return f.read(len(IQLOG_MAGIC)) == IQLOG_MAGIC
    except OSError:
        return False


def _parse_iqlog_header(hdr, path=""):
    if hdr[:len(IQLOG_MAGIC)] != IQLOG_MAGIC:
        sys.exit(f"tone_quality.py: {path} is not an iqlog (bad magic)")
    ver, decim, N, cyc = struct.unpack('<4I', hdr[8:24])
    fs, ftone, t0 = struct.unpack('<3d', hdr[24:48])
    return dict(version=ver, decim=decim, N=N, cyc=cyc, fs=fs, ftone=ftone,
                t0=t0, bb_rate=fs / decim, deg_per_slip=360.0 * cyc / N)


def read_iqlog_header(path):
    """Header only — for streaming (windowed) analysis of large iqlogs."""
    with open(path, 'rb') as f:
        return _parse_iqlog_header(f.read(IQLOG_HDRLEN), path)


def read_iqlog(path):
    """Return (meta, z) where z is the complex baseband (phasor in LSB).
    Loads the whole file — use the windowed path for multi-GB captures."""
    with open(path, 'rb') as f:
        meta = _parse_iqlog_header(f.read(IQLOG_HDRLEN), path)
        iq = np.fromfile(f, dtype='<f4')
    if iq.size < 4:
        sys.exit(f"tone_quality.py: {path} has too few baseband records")
    z = iq[0::2].astype(np.float64) + 1j * iq[1::2].astype(np.float64)
    return meta, z


def analyze_baseband_windowed(meta, path, window_sec, plotdata=None):
    """Streaming, drift-robust analysis for long / undisciplined captures.
    Reads the iqlog in fixed windows (constant memory) and removes the carrier
    LOCALLY per window, so a free-running oscillator's slow drift isn't mis-read
    as slips. Same slip/amplitude/carrier view as analyze_baseband, but it scales
    to multi-GB / multi-hour files and won't false-count drift as corruption.

    With plotdata set, writes one raw row per window (constant memory, so it
    plots a multi-GB / multi-hour capture end to end that dump_baseband_series
    could not load): the per-window amplitude, residual carrier, phase jitter,
    and slip count vs time. Columns only, no verdict — gnuplot-ready."""
    bb = meta['bb_rate']; dps = meta['deg_per_slip']; decim = meta['decim']
    win = max(int(window_sec * bb), 4096)
    slip_thr = max(dps * 0.5, 20.0)
    total = 0; nwin = 0
    amp_min = np.inf; amp_max = -np.inf; amp_sum = 0.0
    carriers = []; slips = []; jit_sq = 0.0; jit_n = 0
    pf = None
    if plotdata:
        pf = open(plotdata, 'w')
        pf.write(f"# tone_quality windowed series: window={window_sec:g}s "
                 f"({win} records) bb_rate={bb:.3f}Hz decim={decim} "
                 f"fs={meta['fs']:.0f} ftone={meta['ftone']:.0f} "
                 f"deg_per_slip={dps:.4f}\n")
        pf.write("# window  t_s  amp_mean  amp_min  amp_max  resid_carrier_hz  "
                 "jitter_deg  slips\n")
    with open(path, 'rb') as f:
        f.seek(IQLOG_HDRLEN)
        base = 0
        while True:
            raw = np.fromfile(f, dtype='<f4', count=2 * win)
            if raw.size < 4:
                break
            z = raw[0::2].astype(np.float64) + 1j * raw[1::2].astype(np.float64)
            amp = np.abs(z)
            w_amin = float(amp.min()); w_amax = float(amp.max())
            w_asum = float(amp.sum())
            amp_min = min(amp_min, w_amin)
            amp_max = max(amp_max, w_amax)
            amp_sum += w_asum
            w_carr = 0.0; w_jit = 0.0; w_slips = 0
            uph = np.degrees(np.unwrap(np.angle(z)))
            d = np.diff(uph)
            if len(d):
                local = float(np.median(d))             # local carrier this window
                w_carr = local / 360.0 * bb
                carriers.append(w_carr)
                resid = d - local
                m = np.abs(resid) > slip_thr
                w_slips = int(m.sum())
                for k in np.where(m)[0]:
                    slips.append((base + int(k) + 1, float(resid[k])))
                cm = ~m
                if cm.any():
                    w_jit = float(resid[cm].std())
                    jit_sq += float(np.sum(resid[cm] ** 2)); jit_n += int(cm.sum())
            if pf:
                pf.write(f"{nwin} {base / bb:.3f} {w_asum / len(z):.3f} "
                         f"{w_amin:.3f} {w_amax:.3f} {w_carr:+.4f} "
                         f"{w_jit:.4f} {w_slips}\n")
            total += len(z); base += len(z); nwin += 1
    if pf:
        pf.close()
        print(f"  plotdata: wrote {nwin} window rows -> {plotdata}")
    if total == 0:
        sys.exit("tone_quality.py: empty iqlog")
    amp_mean = amp_sum / total
    jitter = (jit_sq / jit_n) ** 0.5 if jit_n else 0.0
    carr = np.array(carriers) if carriers else np.array([0.0])
    print(f"  baseband (windowed {window_sec:g}s): {total} records @ "
          f"{bb/1e3:.3f} kHz ({total/bb:.1f} s)  decim={decim}  {nwin} windows")
    print(f"  tone:     period N={meta['N']} ({meta['cyc']} cyc), {dps:.4f} deg/slip")
    print(f"\n  [1] amplitude")
    print(f"      mean {amp_mean:.1f} LSB  min {amp_min:.1f}  max {amp_max:.1f}")
    print(f"\n  [2] phase (carrier removed per-window -> drift-robust)")
    print(f"      residual carrier {carr.min():+.3f} .. {carr.max():+.3f} Hz "
          f"(drift {carr.max()-carr.min():.3f} Hz over the capture)")
    print(f"      phase jitter {jitter:.3f} deg RMS (slips removed)")
    if not slips:
        print(f"      >>> NO grid slips (no local step > {slip_thr:.0f} deg) across "
              f"{nwin} windows. Acquisition coherent end to end.")
    else:
        net = int(sum(round(s[1] / dps) for s in slips))
        print(f"      >>> {len(slips)} grid SLIP(s), net {net:+d} sample(s):")
        for idx, deg in slips[:20]:
            print(f"        record {idx:>11} (~sample {idx*decim:>14})  "
                  f"{deg:+.1f} deg = {deg/dps:+.2f} sample(s)")
    return 0


def dump_baseband_series(meta, z, path):
    """Write the per-record demodulated time series as plain columns for gnuplot
    (or any plotter): record, time, amplitude, wrapped phase, unwrapped phase,
    per-record phase step. The slip shows as a step in phase; a garble as a dip
    in amplitude."""
    bb_rate = meta['bb_rate']
    amp = np.abs(z)
    ph = np.degrees(np.angle(z))
    uph = np.degrees(np.unwrap(np.angle(z)))
    dph = np.empty_like(uph)
    dph[0] = 0.0
    dph[1:] = np.diff(uph)
    t_ms = np.arange(len(z)) / bb_rate * 1e3
    with open(path, 'w') as f:
        f.write(f"# tone_quality baseband series: decim={meta['decim']} "
                f"bb_rate={bb_rate:.3f}Hz fs={meta['fs']:.0f} ftone={meta['ftone']:.0f} "
                f"deg_per_slip={meta['deg_per_slip']:.4f}\n")
        f.write("# record  t_ms  amp_lsb  phase_deg  uphase_deg  dphase_deg\n")
        for i in range(len(z)):
            f.write(f"{i} {t_ms[i]:.6f} {amp[i]:.3f} {ph[i]:.4f} "
                    f"{uph[i]:.4f} {dph[i]:.4f}\n")
    print(f"  plotdata: wrote {len(z)} rows -> {path}")


def analyze_baseband(meta, z, markers=None):
    """Phase-locked tracking demod on the decimated baseband. A grid slip is a
    persistent phase STEP (~deg_per_slip per slipped sample) the loop cannot
    follow; a garble is an amplitude/phase transient; benign wander is the slow,
    smooth part the loop tracks out. Reports raw measurements AND a slip/garble
    classification (the verdict lives here, offline, never in the inline tool)."""
    bb_rate = meta['bb_rate']
    dps = meta['deg_per_slip']
    amp = np.abs(z)
    uph = np.degrees(np.unwrap(np.angle(z)))     # unwrapped phase, deg
    d = np.diff(uph)                             # per-record phase advance

    # Tracker: the benign residual frequency is the robust (median) phase slope;
    # subtract it so what remains is discontinuities (slips) + jitter.
    base = float(np.median(d)) if len(d) else 0.0
    resid = d - base
    resid_freq_hz = base / 360.0 * bb_rate       # residual carrier offset
    slip_thr = max(dps * 0.5, 20.0)              # half a slip, floored at 20 deg
    slip_at = np.where(np.abs(resid) > slip_thr)[0]
    clean_mask = np.abs(resid) <= slip_thr
    jitter = float(resid[clean_mask].std()) if clean_mask.any() else 0.0

    # Amplitude: robust baseline + MAD, with an ABSOLUTE floor so a noise-free
    # (synthetic) tone whose MAD ~ 0 doesn't turn float-rounding wiggles into
    # "garbles"; a real garble is a sizable amplitude hit. (Slow level drift is
    # better removed with a rolling baseline — a future refinement.)
    amed = float(np.median(amp))
    mad = float(np.median(np.abs(amp - amed)))
    amp_thr = max(6.0 * 1.4826 * mad, 0.01 * amed, 4.0)
    amp_at = np.where(np.abs(amp - amed) > amp_thr)[0]

    # Near-DC SNR of the coherent component (narrowband — the iqlog only spans
    # +/- bb_rate/2 around the carrier, so this is close-in SNR, not Nyquist
    # SINAD; full SINAD needs a raw-capture window). A slip splits the phasor
    # into two regimes, so it is only meaningful with no slips.
    dc = np.mean(z)
    noi = float(np.mean(np.abs(z - dc) ** 2))
    snr_nb = 10 * np.log10(float(np.abs(dc) ** 2) / noi) if noi > 0 else float('inf')

    print(f"  baseband: {len(z)} records @ {bb_rate/1e3:.3f} kHz "
          f"({len(z)/bb_rate:.3f} s)  decim={meta['decim']}  "
          f"fs={meta['fs']/1e6:g} MHz  ftone={meta['ftone']/1e6:g} MHz")
    print(f"  tone:     period N={meta['N']} ({meta['cyc']} cyc), "
          f"{dps:.4f} deg per slipped sample")

    print(f"\n  [1] amplitude (tracked)")
    print(f"      median {amed:.1f} LSB  min {amp.min():.1f}  max {amp.max():.1f}  "
          f"std {amp.std():.2f}  ({len(amp_at)} record(s) beyond +/-{amp_thr:.1f})")

    snr_txt = "n/a (slips present)" if len(slip_at) else f"{snr_nb:.1f} dB"
    print(f"\n  [2] phase tracking (slip discriminator)")
    print(f"      residual carrier {resid_freq_hz:+.4f} Hz  "
          f"(benign GPSDO/PLL wander, tracked out)")
    print(f"      phase jitter {jitter:.3f} deg RMS (slips removed)  "
          f"near-DC SNR {snr_txt}")
    if len(slip_at) == 0:
        print(f"      >>> NO grid slips (no phase step > {slip_thr:.0f} deg). "
              f"Coherence intact.")
    else:
        tot = float(np.sum(np.round(resid[slip_at] / dps)))
        print(f"      >>> {len(slip_at)} grid SLIP(s), net {tot:+.0f} sample(s):")
        for i in slip_at[:20]:
            smp = int((i + 1) * meta['decim'])
            print(f"        record {i:>8} (~sample {smp:>12})  "
                  f"{resid[i]:+.1f} deg = {resid[i]/dps:+.2f} slipped sample(s)")

    print(f"\n  [3] disturbance classification")
    # An amplitude event with NO coincident slip is a garble; a slip with an
    # amplitude event is a slip+garble; a clean phase step is a pure slip.
    def near(idx, pool, tol=2):
        return pool.size and np.min(np.abs(pool - idx)) <= tol
    garbles = [i for i in amp_at if not near(i, slip_at)]
    if len(slip_at) == 0 and len(garbles) == 0:
        print("      >>> CLEAN: no slips, no amplitude garbles.")
    else:
        if len(slip_at):
            print(f"      {len(slip_at)} slip(s) (persistent phase step).")
        if len(garbles):
            print(f"      {len(garbles)} garble(s) (amplitude transient, no phase "
                  f"step) at record(s): "
                  + ", ".join(str(int(i)) for i in garbles[:20]))

    if markers is not None:
        events = np.unique(np.concatenate([slip_at, amp_at])) if (len(slip_at) or len(amp_at)) else np.array([], dtype=int)
        ev_samp = (events + 1) * meta['decim']
        tol = meta['decim']
        if len(ev_samp):
            n = sum(np.min(np.abs(markers - s)) < tol for s in ev_samp)
            print(f"\n  marker correlation: {n}/{len(ev_samp)} events within "
                  f"{tol} samples of a marker splice (of {len(markers)} markers)")
        else:
            print(f"\n  marker correlation: no events to correlate "
                  f"({len(markers)} markers)")
    return 0


def is_statslog(path):
    try:
        with open(path, 'r', errors='ignore') as f:
            for _ in range(4):
                line = f.readline()
                if not line:
                    break
                if line.startswith("time,sec,blocks"):
                    return True
    except OSError:
        return False
    return False


def analyze_statslog(path):
    """Summarize tone_monitor's per-second CSV: amplitude over time, residual
    frequency, and the worst per-second phase step (the inline slip evidence)."""
    rows, hdr = [], None
    with open(path, 'r', errors='ignore') as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            if hdr is None and line.startswith("time,sec,blocks"):
                hdr = line.split(","); continue
            if hdr:
                rows.append(line.split(","))
    if not hdr or not rows:
        sys.exit(f"tone_quality.py: {path} has no statslog rows")
    col = {name: i for i, name in enumerate(hdr)}
    def fcol(r, name):
        try: return float(r[col[name]])
        except (KeyError, IndexError, ValueError): return float('nan')
    amp = np.array([fcol(r, "amp_mean") for r in rows])
    amin = np.array([fcol(r, "amp_min") for r in rows])
    rf = np.array([fcol(r, "resid_freq_hz") for r in rows])
    step = np.array([fcol(r, "max_step_deg") for r in rows])
    secs = len(rows)

    print(f"  statslog: {secs} per-second rows")
    print(f"  amplitude: mean {np.nanmean(amp):.1f} LSB  "
          f"min-of-min {np.nanmin(amin):.1f}  spread {np.nanstd(amp):.2f}")
    print(f"  residual freq: median {np.nanmedian(rf):+.4f} Hz  "
          f"max |{np.nanmax(np.abs(rf)):.4f}| Hz")
    worst = int(np.nanargmax(step))
    print(f"  worst per-second phase step: {step[worst]:.2f} deg "
          f"(row {worst}, sec {rows[worst][col['sec']]}, "
          f"sample {rows[worst][col['worst_step_sample']]})")
    # Rows whose worst step looks like a grid slip (~75 deg at the rig). This is
    # a coarse flag; the iqlog gives the authoritative per-record view.
    suspect = np.where(step > 40.0)[0]
    if len(suspect) == 0:
        print(f"  >>> no second with a phase step > 40 deg — no slip candidates.")
    else:
        print(f"  >>> {len(suspect)} second(s) with a step > 40 deg (slip "
              f"candidates — analyze the iqlog for those windows):")
        for i in suspect[:20]:
            print(f"        sec {rows[i][col['sec']]:>6}  step {step[i]:.1f} deg  "
                  f"sample {rows[i][col['worst_step_sample']]}")
    return 0


def power_spectrum(sig, fs, nperseg, complex_in):
    """Welch-averaged power spectrum (a 'powers'-style view). Returns
    (freqs_hz, power_dBFS, nseg), referenced so a full-scale tone peaks at 0 dBFS.
    The per-bin FRACTION freq/fs is fs-independent, so the peak location proves
    where the energy sits even if the assumed fs is wrong."""
    sig = np.asarray(sig)
    if len(sig) < nperseg:
        nperseg = 1 << int(math.floor(math.log2(max(len(sig), 2))))
    nseg = len(sig) // nperseg
    if nseg < 1:
        sys.exit("tone_quality.py: capture too short for --powers")
    w = np.hanning(nperseg)
    FS_LSB = 32768.0
    if complex_in:
        acc = np.zeros(nperseg)
        for i in range(nseg):
            s = sig[i*nperseg:(i+1)*nperseg]
            acc += np.abs(np.fft.fftshift(np.fft.fft((s - s.mean()) * w)))**2
        freqs = np.fft.fftshift(np.fft.fftfreq(nperseg, 1.0/fs))
        ref = (FS_LSB * np.sum(w))**2
    else:
        acc = np.zeros(nperseg//2 + 1)
        for i in range(nseg):
            s = sig[i*nperseg:(i+1)*nperseg]
            acc += np.abs(np.fft.rfft((s - s.mean()) * w))**2
        freqs = np.fft.rfftfreq(nperseg, 1.0/fs)
        ref = (FS_LSB/2.0 * np.sum(w))**2
    acc /= nseg
    return freqs, 10.0*np.log10(acc/ref + 1e-30), nseg


def dump_powers(freqs, dbfs, fs, nseg, label, top=8):
    """Print the top peaks (to stderr) + freq,dBFS columns (to stdout, gnuplot)."""
    print(f"# powers: {label}", file=sys.stderr)
    print(f"#   fs={fs:.0f} Hz  bins={len(freqs)}  segments={nseg}", file=sys.stderr)
    print(f"#   top peaks (freq, dBFS, frac=freq/fs — frac is fs-independent):",
          file=sys.stderr)
    used = []
    for k in np.argsort(dbfs)[::-1]:
        f = float(freqs[k])
        if any(abs(f - u) < fs/512.0 for u in used):   # dedupe adjacent bins
            continue
        used.append(f)
        print(f"#   {f/1e6:11.4f} MHz  {dbfs[k]:7.2f} dBFS  frac={f/fs:+.5f}",
              file=sys.stderr)
        if len(used) >= top:
            break
    print("# freq_mhz  power_dbfs")
    for f, d in zip(freqs, dbfs):
        print(f"{f/1e6:.6f} {d:.3f}")


def main(argv):
    ap = argparse.ArgumentParser(description="coherent-tone corruption + quality check")
    ap.add_argument("capture", help="raw int16 capture, OR a tone_monitor "
                    ".iq baseband / --statslog CSV (auto-detected)")
    ap.add_argument("--fs", type=float, default=129.6e6)
    ap.add_argument("--ftone", type=float, default=27e6)
    ap.add_argument("--markers")
    ap.add_argument("--max-samples", type=int, default=0)
    ap.add_argument("--block", type=int, default=0)
    ap.add_argument("--plotdata", help="write the demodulated time series as "
                    "gnuplot columns to this file (iqlog only): per-record with "
                    "the whole-file path, per-window with --window)")
    ap.add_argument("--powers", action="store_true", help="dump a Welch power "
                    "spectrum (freq vs dBFS) for gnuplot; works on a raw capture "
                    "(full band) or an iqlog (baseband)")
    ap.add_argument("--powers-nperseg", type=int, default=8192)
    ap.add_argument("--window", type=float, default=0, help="analyze the iqlog in "
                    "streaming windows of N seconds (constant memory + drift-robust "
                    "carrier removal; use for long / undisciplined-oscillator captures)")
    a = ap.parse_args(argv[1:])

    # 'powers'-style spectrum: print freq,dBFS columns + top peaks, then stop.
    if a.powers:
        if is_iqlog(a.capture):
            meta, z = read_iqlog(a.capture)
            bb = meta['fs'] / meta['decim']
            freqs, dbfs, nseg = power_spectrum(z, bb, a.powers_nperseg, True)
            dump_powers(freqs, dbfs, bb, nseg, f"{a.capture} (iqlog baseband)")
        elif is_statslog(a.capture):
            sys.exit("tone_quality.py: --powers needs a raw capture or iqlog, not a statslog")
        else:
            x = load(a.capture, a.max_samples)
            freqs, dbfs, nseg = power_spectrum(x, a.fs, a.powers_nperseg, False)
            dump_powers(freqs, dbfs, a.fs, nseg, a.capture)
        return 0

    # Dispatch on input type: tone_monitor iqlog / statslog / raw capture.
    if is_iqlog(a.capture):
        if a.window and a.window > 0:
            meta = read_iqlog_header(a.capture)
            print(f"===== {a.capture} (tone_monitor iqlog, windowed) =====")
            return analyze_baseband_windowed(meta, a.capture, a.window, a.plotdata)
        meta, z = read_iqlog(a.capture)
        print(f"===== {a.capture} (tone_monitor iqlog) =====")
        if a.plotdata:
            dump_baseband_series(meta, z, a.plotdata)
        markers = np.loadtxt(a.markers, dtype=np.int64, ndmin=1) if a.markers else None
        return analyze_baseband(meta, z, markers)
    if is_statslog(a.capture):
        print(f"===== {a.capture} (tone_monitor statslog) =====")
        return analyze_statslog(a.capture)

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
