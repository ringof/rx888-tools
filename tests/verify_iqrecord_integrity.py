#!/usr/bin/env python3
"""
verify_iqrecord_integrity.py - Spot-check that iqrecord output matches input.

Verifies that cf32_le data written by iqrecord is byte-identical to what
counter_cf32.py generated.  Rather than comparing every byte (which would
require keeping the full input in memory), this script:

  1. Regenerates the expected counter stream on-the-fly.
  2. Reads each .sigmf-data file and compares samples at configurable
     intervals (default: every 1000th sample), plus the first and last
     sample of every file.
  3. Reports any mismatches with file offset, expected, and actual values.

Usage:
  # After: counter_cf32.py --limit-samples N | iqrecord OUTDIR
  ./verify_iqrecord_integrity.py "OUTDIR/cap_*.sigmf-data"

  # Check every 500th sample:
  ./verify_iqrecord_integrity.py "OUTDIR/cap_*.sigmf-data" --stride 500

Exit codes:
  0: all checked samples match
  1: argument / file error
  2: mismatch detected
"""
import argparse
import glob
import os
import struct
import sys

BYTES_PER_SAMPLE = 8  # cf32_le: two float32 (I, Q)


def parse_args():
    p = argparse.ArgumentParser(
        description="Spot-check iqrecord output against expected counter stream.")
    p.add_argument("glob", help="Glob for .sigmf-data files (quote it).")
    p.add_argument("--wrap", type=int, default=16_000_000,
                   help="Counter wrap value (default: 16,000,000).")
    p.add_argument("--stride", type=int, default=1000,
                   help="Check every Nth sample (default: 1000).  "
                        "First and last sample of each file are always checked.")
    p.add_argument("--tolerance", type=float, default=1e-6,
                   help="Float comparison tolerance (default: 1e-6).")
    p.add_argument("--start", type=int, default=0,
                   help="Initial counter value (default: 0).")
    p.add_argument("--max-errors", type=int, default=20,
                   help="Stop after this many mismatches (default: 20).")
    return p.parse_args()


def main():
    args = parse_args()
    files = sorted(glob.glob(args.glob))
    if not files:
        print(f"No files matched: {args.glob}", file=sys.stderr)
        return 1

    counter = args.start
    total_samples = 0
    checked = 0
    errors = 0

    for fpath in files:
        size = os.path.getsize(fpath)
        if size % BYTES_PER_SAMPLE != 0:
            print(f"FAIL {fpath}: size {size} not a multiple of {BYTES_PER_SAMPLE}",
                  file=sys.stderr)
            return 1
        n_samples = size // BYTES_PER_SAMPLE
        if n_samples == 0:
            continue

        with open(fpath, "rb") as f:
            for s in range(n_samples):
                raw = f.read(BYTES_PER_SAMPLE)
                if len(raw) < BYTES_PER_SAMPLE:
                    print(f"FAIL {fpath}: unexpected short read at sample {s}",
                          file=sys.stderr)
                    return 1

                expected_i = float(counter)
                expected_q = 0.0

                # Check this sample?  Always check first, last, and every stride.
                is_first = (s == 0)
                is_last = (s == n_samples - 1)
                is_stride = (s % args.stride == 0)

                if is_first or is_last or is_stride:
                    actual_i, actual_q = struct.unpack("<ff", raw)
                    ok = (abs(actual_i - expected_i) < args.tolerance and
                          abs(actual_q - expected_q) < args.tolerance)
                    checked += 1
                    if not ok:
                        errors += 1
                        global_sample = total_samples + s
                        print(f"MISMATCH file={os.path.basename(fpath)} "
                              f"sample={s} (global {global_sample}): "
                              f"expected I={expected_i} Q={expected_q}, "
                              f"got I={actual_i} Q={actual_q}")
                        if errors >= args.max_errors:
                            print(f"Stopping after {args.max_errors} errors.",
                                  file=sys.stderr)
                            return 2

                counter += 1
                if counter >= args.wrap:
                    counter = 0

        total_samples += n_samples

    if errors > 0:
        print(f"\nFAIL: {errors} mismatches in {checked} checked samples "
              f"({total_samples} total)", file=sys.stderr)
        return 2

    print(f"PASS: {checked} samples checked, {total_samples} total across "
          f"{len(files)} file(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
