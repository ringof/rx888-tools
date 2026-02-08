#!/usr/bin/env python3
"""
verify_cf32.py - Verify continuity across iqrecord-produced SigMF .sigmf-data files.

Assumptions:
- Data type is cf32_le: interleaved float32 little-endian (I,Q) pairs.
- The producer is counter_cf32.py (or equivalent) where I increments by +1
  each sample, wrapping at WRAP.

Checks:
- Each file size is a multiple of 8 bytes (one complex sample).
- Prints per-file: sample_count, first_I, last_I.
- Boundary continuity: first_I(next) == last_I(prev) + 1 (with wrap).

Usage:
  ./verify_cf32.py "captures/*/cap_*.sigmf-data" --wrap 16000000

Exit codes:
  0: all checks passed
  2: continuity or format failure
  1: no files matched / argument problems
"""
import argparse
import glob
import os
import struct
import sys
from typing import Tuple

BYTES_PER_SAMPLE = 8

def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Verify cf32_le counter-stream continuity across files.")
    p.add_argument("glob", help="Glob for .sigmf-data files (quote it).")
    p.add_argument("--wrap", type=int, default=16_000_000,
                   help="Counter wrap value used by generator (default: 16,000,000).")
    p.add_argument("--tolerance", type=float, default=1e-6,
                   help="Float comparison tolerance (default: 1e-6).")
    p.add_argument("--show-all", action="store_true",
                   help="Print every file line (default prints every file anyway; kept for future).")
    return p.parse_args()

def fmt_counter(x: float, tol: float) -> str:
    rx = round(x)
    if abs(x - rx) < tol:
        return str(int(rx))
    return f"{x:.6f}"

def read_first_last_I(path: str) -> Tuple[float, float, int]:
    size = os.path.getsize(path)
    if size < BYTES_PER_SAMPLE:
        raise ValueError(f"{path}: too small ({size} bytes)")
    if size % BYTES_PER_SAMPLE != 0:
        raise ValueError(f"{path}: size {size} not multiple of {BYTES_PER_SAMPLE}")
    n_samples = size // BYTES_PER_SAMPLE

    with open(path, "rb") as f:
        head = f.read(BYTES_PER_SAMPLE)
        i0, q0 = struct.unpack("<ff", head)

        f.seek(-BYTES_PER_SAMPLE, os.SEEK_END)
        tail = f.read(BYTES_PER_SAMPLE)
        i1, q1 = struct.unpack("<ff", tail)
    return i0, i1, n_samples

def expected_next(prev_last: float, wrap: int) -> float:
    nxt = prev_last + 1.0
    if nxt >= float(wrap):
        nxt = 0.0
    return nxt

def main() -> int:
    args = parse_args()
    files = sorted(glob.glob(args.glob))
    if not files:
        print(f"No files matched: {args.glob}", file=sys.stderr)
        return 1
    if args.wrap <= 0:
        print("wrap must be > 0", file=sys.stderr)
        return 1

    ok = True
    prev_last = None

    for idx, p in enumerate(files):
        try:
            first_i, last_i, n_samples = read_first_last_I(p)
        except Exception as e:
            print(f"FAIL {p}: {e}", file=sys.stderr)
            ok = False
            continue

        print(f"{idx:06d} {os.path.basename(p)} samples={n_samples} firstI={fmt_counter(first_i, args.tolerance)} lastI={fmt_counter(last_i, args.tolerance)}")

        if prev_last is not None:
            exp = expected_next(prev_last, args.wrap)
            if abs(first_i - exp) > args.tolerance:
                print(f"  FAIL continuity: expected firstI={fmt_counter(exp, args.tolerance)} got {fmt_counter(first_i, args.tolerance)}", file=sys.stderr)
                ok = False

        prev_last = last_i

    return 0 if ok else 2

if __name__ == "__main__":
    raise SystemExit(main())
