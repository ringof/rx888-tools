#!/usr/bin/env python3
"""
counter_cf32.py - Generate a synthetic cf32_le IQ stream on stdout.

Format: interleaved float32 little-endian: [I0, Q0, I1, Q1, ...]
Signal: I = counter (wrapped below 2^24 for exact +1 representability), Q = 0.0

Use this to prove continuity across file boundaries: last I in file N should be
followed by first I in file N+1 (with wrap).
"""
import argparse
import struct
import sys

def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Generate cf32_le counter IQ samples to stdout.")
    p.add_argument("--wrap", type=int, default=16_000_000,
                   help="Counter wraps at this value (default: 16,000,000; < 2^24 keeps +1 exactly representable).")
    p.add_argument("--chunk-samples", type=int, default=1_000_000,
                   help="Samples per write() chunk (default: 1,000,000).")
    p.add_argument("--limit-samples", type=int, default=0,
                   help="If >0, stop after this many samples (default: 0 = infinite).")
    p.add_argument("--start", type=int, default=0,
                   help="Initial counter value (default: 0).")
    return p.parse_args()

def main() -> int:
    args = parse_args()
    if args.wrap <= 0:
        print("wrap must be > 0", file=sys.stderr)
        return 2
    if args.chunk_samples <= 0:
        print("chunk-samples must be > 0", file=sys.stderr)
        return 2
    if args.start < 0 or args.start >= args.wrap:
        print("start must be in [0, wrap)", file=sys.stderr)
        return 2

    out = sys.stdout.buffer

    c = args.start
    total_written = 0

    # Allocate once for speed; rebuild each loop for simplicity.
    while True:
        if args.limit_samples and total_written >= args.limit_samples:
            break

        n_samp = args.chunk_samples
        if args.limit_samples:
            remaining = args.limit_samples - total_written
            if remaining < n_samp:
                n_samp = remaining

        # Build chunk
        buf = bytearray()
        # Pre-size roughly (8 bytes/sample) to reduce reallocs
        buf.extend(b"\x00" * (n_samp * 8))
        mv = memoryview(buf)

        # Fill as little-endian float32 pairs
        # struct.pack_into is much faster than repeated concatenation
        off = 0
        for _ in range(n_samp):
            i = float(c)
            struct.pack_into("<ff", mv, off, i, 0.0)
            off += 8
            c += 1
            if c >= args.wrap:
                c = 0

        out.write(buf)
        out.flush()
        total_written += n_samp

    return 0

if __name__ == "__main__":
    raise SystemExit(main())
