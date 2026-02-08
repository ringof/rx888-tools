#!/usr/bin/env python3
"""
verify_rx888_dsp.py - Known-answer test for rx888_dsp decimation pipeline.

Generates a synthetic int16 sine wave at a chosen frequency (default 25 MHz)
sampled at 135 MS/s, pipes it through rx888_dsp, reads the cf32 output, and
verifies by FFT that the expected tone appears at the correct frequency in
the output spectrum.

Signal chain (from rx888_dsp ARCHITECTURE.md):
  Stage 1: -Fs/4 shift (-33.75 MHz)
  Stage 2: Halfband decimate-by-2  (135 -> 67.5 MS/s)
  Stage 3: +Fs'/4 shift (+16.875 MHz)
  Stage 4: Halfband decimate-by-2  (67.5 -> 33.75 MS/s)

The net frequency shift is -Fs/4 + Fs'/4 = -33.75 + 16.875 = -16.875 MHz.
The passband covers input frequencies roughly 0 - 33.75 MHz.
An input tone at f_in maps to output frequency:
  f_out = f_in - 16.875 MHz

Examples:
  10.00 MHz in  ->  -6.875 MHz out  (well inside passband)
  16.875 MHz in ->   0.000 MHz out  (DC)
  25.00 MHz in  ->  +8.125 MHz out

Usage:
  ./verify_rx888_dsp.py                       # default: 25 MHz tone
  ./verify_rx888_dsp.py --freq-mhz 43.75      # 10 MHz in output
  ./verify_rx888_dsp.py --rx888-dsp ./rx888_dsp  # explicit path

Exit codes:
  0: tone detected at expected frequency within tolerance
  1: argument / runtime error
  2: verification failed (tone not found or wrong frequency)
"""
import argparse
import math
import os
import struct
import subprocess
import sys
import tempfile

FS_IN = 135_000_000       # Input sample rate (Hz)
FS_OUT = 33_750_000        # Output sample rate (Hz)
NET_SHIFT = 16_875_000     # Net frequency shift: Fs/4 - Fs'/4 = 16.875 MHz

# rx888_dsp processes INPUT_SAMPLES = 4194304 int16 samples per block.
# Generate enough blocks for clean FFT analysis.
INPUT_BLOCK = 4_194_304
DEFAULT_BLOCKS = 4


def parse_args():
    p = argparse.ArgumentParser(
        description="Known-answer FFT test for rx888_dsp.")
    p.add_argument("--freq-mhz", type=float, default=10.0,
                   help="Input tone frequency in MHz (default: 10.0). "
                        "Must be within passband: ~0 - 33.75 MHz.")
    p.add_argument("--amplitude", type=float, default=0.02,
                   help="Tone amplitude as fraction of full-scale int16 "
                        "(default: 0.02 = 2%%).")
    p.add_argument("--blocks", type=int, default=DEFAULT_BLOCKS,
                   help=f"Number of input blocks to generate "
                        f"(default: {DEFAULT_BLOCKS}).")
    p.add_argument("--rx888-dsp", type=str, default="./rx888_dsp",
                   help="Path to rx888_dsp binary (default: ./rx888_dsp).")
    p.add_argument("--freq-tolerance-hz", type=float, default=200_000,
                   help="Max acceptable error between expected and detected "
                        "peak frequency in Hz (default: 200000).")
    p.add_argument("--min-snr-db", type=float, default=20.0,
                   help="Minimum SNR of detected peak above noise floor "
                        "(default: 20 dB).")
    p.add_argument("--keep-files", action="store_true",
                   help="Do not delete temporary input/output files.")
    p.add_argument("-v", "--verbose", action="store_true",
                   help="Print detailed diagnostics.")
    return p.parse_args()


def generate_int16_tone(freq_hz, amplitude, n_samples):
    """Generate int16 samples of a real sine wave."""
    max_val = 32767.0
    amp = amplitude * max_val
    omega = 2.0 * math.pi * freq_hz / FS_IN

    # Build in chunks to limit memory
    chunk = 1_000_000
    buf = bytearray()
    for start in range(0, n_samples, chunk):
        end = min(start + chunk, n_samples)
        samples = []
        for i in range(start, end):
            val = amp * math.sin(omega * i)
            val = max(-32768, min(32767, int(round(val))))
            samples.append(val)
        buf.extend(struct.pack(f"<{len(samples)}h", *samples))
    return bytes(buf)


def read_cf32(path):
    """Read cf32_le file, return (I_array, Q_array) as lists of float."""
    size = os.path.getsize(path)
    n_samples = size // 8
    I = []
    Q = []
    with open(path, "rb") as f:
        for _ in range(n_samples):
            raw = f.read(8)
            if len(raw) < 8:
                break
            i, q = struct.unpack("<ff", raw)
            I.append(i)
            Q.append(q)
    return I, Q


def fft_peak(I_samples, Q_samples, fs):
    """Find the peak frequency and its SNR using a simple DFT via FFT.

    Returns (peak_freq_hz, snr_db, magnitude_spectrum).
    Uses only the standard library (no numpy dependency).
    """
    N = len(I_samples)
    # Build complex array
    x = [complex(I_samples[i], Q_samples[i]) for i in range(N)]

    # Apply Hann window to reduce spectral leakage
    for i in range(N):
        w = 0.5 * (1.0 - math.cos(2.0 * math.pi * i / N))
        x[i] *= w

    # Python's built-in doesn't have FFT; use a simple Bluestein or
    # just call out to numpy if available, otherwise use DFT on a
    # decimated subset for speed.
    try:
        import numpy as np
        X = np.fft.fft(np.array(x))
        mag = np.abs(X)
        peak_bin = int(np.argmax(mag))
        peak_mag = mag[peak_bin]

        # SNR via signal power vs noise power.
        # Signal bins: peak +/- a small window (accounts for spectral leakage
        # from the Hann window, which spreads energy ~4 bins either side).
        power = mag ** 2
        total_power = float(np.sum(power))

        signal_half_width = max(8, N // 2048)  # generous window around peak
        sig_lo = max(0, peak_bin - signal_half_width)
        sig_hi = min(N, peak_bin + signal_half_width + 1)
        signal_power = float(np.sum(power[sig_lo:sig_hi]))
        noise_power = total_power - signal_power

        if noise_power > 0:
            snr_db = 10.0 * math.log10(signal_power / noise_power)
        else:
            snr_db = 999.0

        # Convert bin to frequency (complex FFT: negative freqs in upper half)
        if peak_bin > N // 2:
            peak_freq = (peak_bin - N) * fs / N
        else:
            peak_freq = peak_bin * fs / N

        return peak_freq, snr_db, mag.tolist()

    except ImportError:
        # Fallback: brute-force DFT on a subset of frequency bins around
        # the expected peak.  Much slower but works without numpy.
        print("Warning: numpy not available, using slow DFT fallback",
              file=sys.stderr)
        # Check +/- 2 MHz around expected frequency, in 10 kHz steps
        best_mag = 0
        best_freq = 0
        total_mag = 0
        n_bins = 0
        # Also sample some noise bins for SNR estimate
        noise_mags = []

        for freq_hz in range(-FS_OUT // 2, FS_OUT // 2, 10_000):
            omega = -2.0 * math.pi * freq_hz / fs
            re = 0.0
            im = 0.0
            for i in range(N):
                angle = omega * i
                re += x[i].real * math.cos(angle) - x[i].imag * math.sin(angle)
                im += x[i].real * math.sin(angle) + x[i].imag * math.cos(angle)
            mag = math.sqrt(re * re + im * im)
            if mag > best_mag:
                best_mag = mag
                best_freq = freq_hz
            noise_mags.append(mag)
            n_bins += 1

        # Power-based SNR: signal power vs everything else
        total_power = sum(m * m for m in noise_mags)
        signal_power = best_mag * best_mag
        noise_power = total_power - signal_power
        snr_db = 10.0 * math.log10(signal_power / noise_power) if noise_power > 0 else 999.0
        return float(best_freq), snr_db, []


def main():
    args = parse_args()

    freq_hz = args.freq_mhz * 1e6
    expected_output_freq = freq_hz - NET_SHIFT
    n_input_samples = INPUT_BLOCK * args.blocks

    # Validate frequency is in passband (0 to Fs/4 = 33.75 MHz)
    low = 0
    high = FS_IN / 4  # 33.75 MHz
    if freq_hz < low or freq_hz > high:
        print(f"ERROR: {args.freq_mhz} MHz is outside the DSP passband "
              f"({low/1e6:.3f} - {high/1e6:.3f} MHz)", file=sys.stderr)
        return 1

    if not os.path.isfile(args.rx888_dsp):
        print(f"ERROR: rx888_dsp binary not found at {args.rx888_dsp}",
              file=sys.stderr)
        return 1

    if args.verbose:
        print(f"Input:  {args.freq_mhz} MHz tone, {args.amplitude*100:.1f}% "
              f"full-scale, {n_input_samples} samples @ {FS_IN/1e6} MS/s")
        print(f"Expect: {expected_output_freq/1e6:.3f} MHz in output "
              f"@ {FS_OUT/1e6} MS/s")
        print(f"Blocks: {args.blocks} x {INPUT_BLOCK} samples")

    # Generate input
    if args.verbose:
        print("Generating input tone...", end=" ", flush=True)
    input_data = generate_int16_tone(freq_hz, args.amplitude, n_input_samples)
    if args.verbose:
        print(f"done ({len(input_data)} bytes)")

    # Run rx888_dsp
    if args.verbose:
        print("Running rx888_dsp...", end=" ", flush=True)

    try:
        result = subprocess.run(
            [args.rx888_dsp],
            input=input_data,
            capture_output=True,
            timeout=60)
    except subprocess.TimeoutExpired:
        print("ERROR: rx888_dsp timed out after 60 seconds", file=sys.stderr)
        return 1
    except FileNotFoundError:
        print(f"ERROR: could not execute {args.rx888_dsp}", file=sys.stderr)
        return 1

    if result.returncode != 0:
        print(f"ERROR: rx888_dsp exited with code {result.returncode}",
              file=sys.stderr)
        if result.stderr:
            print(result.stderr.decode(errors="replace"), file=sys.stderr)
        return 1

    output_data = result.stdout
    n_output_samples = len(output_data) // 8

    if args.verbose:
        print(f"done ({n_output_samples} output samples, "
              f"{len(output_data)} bytes)")

    if n_output_samples == 0:
        print("ERROR: rx888_dsp produced no output", file=sys.stderr)
        return 1

    # Parse cf32 output
    I_out = []
    Q_out = []
    for i in range(n_output_samples):
        off = i * 8
        iv, qv = struct.unpack_from("<ff", output_data, off)
        I_out.append(iv)
        Q_out.append(qv)

    # Skip the first block of output (filter transient / startup artifact)
    skip = n_output_samples // args.blocks
    I_analysis = I_out[skip:]
    Q_analysis = Q_out[skip:]

    if args.verbose:
        print(f"Analyzing {len(I_analysis)} samples (skipped first {skip} "
              f"for filter settling)")

    # FFT analysis
    peak_freq, snr_db, _ = fft_peak(I_analysis, Q_analysis, FS_OUT)

    freq_error = abs(peak_freq - expected_output_freq)

    if args.verbose:
        print(f"Expected peak: {expected_output_freq/1e6:+.3f} MHz")
        print(f"Detected peak: {peak_freq/1e6:+.3f} MHz")
        print(f"Frequency error: {freq_error:.0f} Hz")
        print(f"SNR: {snr_db:.1f} dB")

    # Verdict
    freq_ok = freq_error <= args.freq_tolerance_hz
    snr_ok = snr_db >= args.min_snr_db

    if freq_ok and snr_ok:
        print(f"PASS: tone at {peak_freq/1e6:+.3f} MHz "
              f"(expected {expected_output_freq/1e6:+.3f} MHz, "
              f"error {freq_error:.0f} Hz), "
              f"SNR {snr_db:.1f} dB")
        return 0
    else:
        parts = []
        if not freq_ok:
            parts.append(f"frequency error {freq_error:.0f} Hz > "
                         f"{args.freq_tolerance_hz:.0f} Hz tolerance")
        if not snr_ok:
            parts.append(f"SNR {snr_db:.1f} dB < {args.min_snr_db:.1f} dB "
                         f"minimum")
        print(f"FAIL: {'; '.join(parts)}")
        print(f"  Expected: {expected_output_freq/1e6:+.3f} MHz")
        print(f"  Detected: {peak_freq/1e6:+.3f} MHz, SNR {snr_db:.1f} dB")
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
