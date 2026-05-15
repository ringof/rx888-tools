#!/usr/bin/env python3
"""hw_sample_check.py — sample-content sanity for rx888_stream.

Captures a few seconds of int16 samples without assuming any known
signal at the antenna.  Verifies properties that hold for any
healthy ADC capture:

    1. No long runs of identical samples (would indicate a stale
       buffer being re-presented from the USB layer).
    2. DC offset within a reasonable range for the AD8340 front-end.
    3. Sample standard deviation is non-trivial (input not flatlined).
    4. Less than 1% of samples saturated to the rails (input not
       overdriven).

Inspired by the post-stream sanity checks in rx888-firmware's
fw_test.sh; implementation here is fresh and intentionally avoids
SciPy / NumPy so it runs on a stock Python 3 install.

Requires hardware.  Set RX888_HW_TEST=1 to run.
"""
import os
import statistics
import struct
import subprocess
import sys


def env(name, default):
    return os.environ.get(name, default)


def main() -> int:
    if env("RX888_HW_TEST", "0") != "1":
        print("skip hw_sample_check (set RX888_HW_TEST=1 to enable)")
        return 0

    stream    = env("RX888_STREAM", "./rx888_stream")
    firmware  = env("RX888_FW", "firmware/SDDC_FX3.img")
    rate      = int(env("RX888_RATE", "32000000"))
    secs      = int(env("RX888_SECS", "3"))
    max_run   = int(env("RX888_MAX_RUN", "1024"))
    max_dc    = int(env("RX888_MAX_DC", "4096"))
    # MIN_STDDEV defaults very low: the goal is to verify samples are
    # changing at all (a stuck buffer would have stddev == 0).  Quiet
    # SDR captures with no antenna routinely land at stddev < 10 LSB.
    # The longest_run check is what catches stuck-buffer regressions;
    # this is a belt-and-braces "is anything moving" sanity bound.
    # Override RX888_MIN_STDDEV if you have a known signal source and
    # want a tighter check.
    min_std   = float(env("RX888_MIN_STDDEV", "1"))
    max_sat   = float(env("RX888_MAX_SAT_PCT", "1.0"))

    if not os.path.exists(firmware):
        print(f"missing firmware: {firmware} (run: make firmware)", file=sys.stderr)
        return 2

    n_samples = rate * secs
    n_bytes   = n_samples * 2
    print(f"capturing {n_samples} samples ({secs}s @ {rate} S/s)...")

    p = subprocess.Popen(
        [stream, "-f", firmware, "-s", str(rate)],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    data = bytearray()
    try:
        while len(data) < n_bytes:
            chunk = p.stdout.read(min(1 << 20, n_bytes - len(data)))
            if not chunk:
                break
            data.extend(chunk)
    finally:
        p.terminate()
        try:
            p.wait(timeout=2)
        except subprocess.TimeoutExpired:
            p.kill()

    if len(data) < n_bytes // 2:
        print(f"FAIL: captured only {len(data)} / {n_bytes} bytes")
        return 1

    samples = struct.unpack(f"<{len(data) // 2}h", bytes(data))
    print(f"got {len(samples)} samples")

    failures = 0

    # 1. Longest run of identical values.
    longest = 1
    run = 1
    prev = samples[0]
    for s in samples[1:]:
        if s == prev:
            run += 1
        else:
            if run > longest:
                longest = run
            run = 1
            prev = s
    if run > longest:
        longest = run
    if longest > max_run:
        print(f"FAIL longest constant run = {longest} (limit {max_run})")
        failures += 1
    else:
        print(f"ok   longest constant run = {longest}")

    # 2. DC offset.
    mean = statistics.fmean(samples)
    if abs(mean) > max_dc:
        print(f"FAIL DC offset = {mean:.1f} (limit |x| < {max_dc})")
        failures += 1
    else:
        print(f"ok   DC offset = {mean:.1f}")

    # 3. Standard deviation (use first 1M samples to keep this quick).
    head = samples[: 1 << 20] if len(samples) > (1 << 20) else samples
    stddev = statistics.pstdev(head)
    if stddev < min_std:
        print(f"FAIL stddev = {stddev:.2f} (min {min_std} — input dead?)")
        failures += 1
    else:
        print(f"ok   stddev = {stddev:.2f}")

    # 4. Saturation.
    n_sat = sum(1 for s in samples if abs(s) >= 32700)
    sat_pct = 100.0 * n_sat / len(samples)
    if sat_pct > max_sat:
        print(f"FAIL saturation = {sat_pct:.3f}% (limit {max_sat}%)")
        failures += 1
    else:
        print(f"ok   saturation = {sat_pct:.3f}%")

    if failures:
        print(f"FAILED ({failures} checks)")
        return 1
    print("ALL OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
