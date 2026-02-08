#!/usr/bin/env bash
# rx888_dsp_to_gqrx.sh
#
# Robust dual-FIFO + mbuffer relay for GQRX.
#
# IMPORTANT:
# Some mbuffer builds CANNOT open an existing FIFO with -o <path> and will
# fail with:
#   "unable to open output ... File exists"
#
# The portable solution is:
#   - let the *shell* open FIFO_B
#   - run mbuffer with '-o -' (stdout)
#
# Topology:
#   file -> rx888_dsp -> FIFO_A -> mbuffer -> FIFO_B -> GQRX
#
set -euo pipefail

INFILE="${1:?usage: $0 <real16_135Msps_file>}"

FIFO_A="${FIFO_A:-/tmp/iq_a.fifo}"
FIFO_B="${FIFO_B:-/tmp/iq_b.fifo}"
MBUFFER_MEM="${MBUFFER_MEM:-512M}"
MBUFFER_PREFILL="${MBUFFER_PREFILL:-20}"
DSP_BIN="${DSP_BIN:-./rx888_dsp}"
DSP_ARGS="${DSP_ARGS:--v}"

BLOCK_BYTES=524288

cleanup() {
  [[ -n "${MBUF_PID:-}" ]] && kill "$MBUF_PID" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

command -v mbuffer >/dev/null || { echo "ERROR: mbuffer not found"; exit 1; }
[[ -x "$DSP_BIN" ]] || { echo "ERROR: DSP binary not executable: $DSP_BIN"; exit 1; }
[[ -r "$INFILE" ]] || { echo "ERROR: input file not readable: $INFILE"; exit 1; }

# FIFO_A must exist for rx888_dsp output
[[ -p "$FIFO_A" ]] || { rm -f "$FIFO_A"; mkfifo "$FIFO_A"; }

# FIFO_B must exist for shell redirection
[[ -p "$FIFO_B" ]] || { rm -f "$FIFO_B"; mkfifo "$FIFO_B"; }

sz=$(stat -c%s "$INFILE")
trunc_sz=$(( (sz / BLOCK_BYTES) * BLOCK_BYTES ))
[[ "$trunc_sz" -gt 0 ]] || { echo "ERROR: file < one block"; exit 1; }

echo "=== rx888_dsp_to_gqrx ==="
echo "Input file : $INFILE"
echo "Loop size  : $trunc_sz bytes"
echo
echo "FIFOs:"
echo "  FIFO_A: $FIFO_A (rx888_dsp output)"
echo "  FIFO_B: $FIFO_B (GQRX input)"
echo
echo "Configure GQRX to read:"
echo "  FIFO: $FIFO_B"
echo "  Sample rate: 33.75e6"
echo "  Format: complex float32 IQ (interleaved)"
echo

echo "Starting mbuffer (stdout -> FIFO_B)"
mbuffer -m "$MBUFFER_MEM" -P "$MBUFFER_PREFILL" -i "$FIFO_A" -o - > "$FIFO_B" &
MBUF_PID=$!

echo "Starting rx888_dsp:"
echo "  (loop file) | $DSP_BIN $DSP_ARGS -o $FIFO_A"
echo

while true; do
  head -c "$trunc_sz" "$INFILE"
done | "$DSP_BIN" $DSP_ARGS -o "$FIFO_A"
