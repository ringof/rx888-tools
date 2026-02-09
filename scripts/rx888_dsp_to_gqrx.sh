#!/usr/bin/env bash
# rx888_dsp_to_gqrx.sh — stand up the full RX888 pipeline feeding GQRX
#
# Topology (live):
#   rx888_stream -> rx888_dsp -> FIFO -> mbuffer -> FIFO -> GQRX
#
# Topology (file playback):
#   (loop file) -> rx888_dsp -> FIFO -> mbuffer -> FIFO -> GQRX
#
# Usage:
#   ./scripts/rx888_dsp_to_gqrx.sh                 # live from RX888
#   ./scripts/rx888_dsp_to_gqrx.sh recording.raw   # loop a capture file
#
# Configure GQRX:
#   I/O device string:  file=/tmp/iq_gqrx.fifo,freq=33750000,rate=33750000
#   Sample rate:         33750000
#   Format:              Complex Float32 (native endian)
#
set -euo pipefail

# ---------- tunables (override via environment) ----------
FIRMWARE="${FIRMWARE:-firmware/SDDC_FX3.img}"
SAMPLERATE="${SAMPLERATE:-135000000}"
GAIN="${GAIN:-0}"
QUEUE_DEPTH="${QUEUE_DEPTH:-32}"
REQ_SIZE="${REQ_SIZE:-1024}"

FIFO_DSP="${FIFO_DSP:-/tmp/iq_dsp.fifo}"    # rx888_dsp writes here
FIFO_GQRX="${FIFO_GQRX:-/tmp/iq_gqrx.fifo}" # GQRX reads here
MBUFFER_MEM="${MBUFFER_MEM:-512M}"
MBUFFER_PREFILL="${MBUFFER_PREFILL:-20}"

STREAM_BIN="${STREAM_BIN:-./rx888_stream}"
DSP_BIN="${DSP_BIN:-./rx888_dsp}"

INFILE="${1:-}"

# ---------- preflight ----------
command -v mbuffer >/dev/null 2>&1 || { echo "ERROR: mbuffer not found (apt install mbuffer)"; exit 1; }
[[ -x "$DSP_BIN" ]]    || { echo "ERROR: $DSP_BIN not found or not executable"; exit 1; }

if [[ -z "$INFILE" ]]; then
    # Live mode
    [[ -x "$STREAM_BIN" ]] || { echo "ERROR: $STREAM_BIN not found or not executable"; exit 1; }
    [[ -r "$FIRMWARE" ]]   || { echo "ERROR: firmware not found: $FIRMWARE"; exit 1; }
else
    # File playback mode
    [[ -r "$INFILE" ]]     || { echo "ERROR: input file not readable: $INFILE"; exit 1; }
fi

# ---------- FIFOs ----------
for f in "$FIFO_DSP" "$FIFO_GQRX"; do
    [[ -p "$f" ]] || { rm -f "$f"; mkfifo "$f"; }
done

# ---------- cleanup ----------
PIDS=()
cleanup() {
    echo ""
    echo "Shutting down..."
    for pid in "${PIDS[@]}"; do
        kill "$pid" 2>/dev/null || true
    done
    wait 2>/dev/null || true
    rm -f "$FIFO_DSP" "$FIFO_GQRX"
    echo "Done."
}
trap cleanup EXIT INT TERM

# ---------- banner ----------
echo "=== rx888_dsp_to_gqrx ==="
if [[ -z "$INFILE" ]]; then
    echo "Mode       : LIVE (rx888_stream at ${SAMPLERATE} S/s)"
else
    echo "Mode       : FILE PLAYBACK (looping $INFILE)"
fi
echo "FIFOs      : $FIFO_DSP -> mbuffer -> $FIFO_GQRX"
echo "mbuffer    : ${MBUFFER_MEM} buffer, ${MBUFFER_PREFILL}% prefill"
echo ""
echo "Configure GQRX:"
echo "  Device string : file=$FIFO_GQRX,freq=33750000,rate=33750000"
echo "  Sample rate   : 33750000"
echo "  Format        : Complex Float32"
echo ""
echo "Start GQRX, then press Enter here to begin streaming..."
read -r

# ---------- start mbuffer ----------
mbuffer -q -m "$MBUFFER_MEM" -P "$MBUFFER_PREFILL" \
    -i "$FIFO_DSP" -o - > "$FIFO_GQRX" &
PIDS+=($!)

# ---------- start source -> DSP ----------
if [[ -z "$INFILE" ]]; then
    # Live: rx888_stream -> rx888_dsp -> FIFO
    "$STREAM_BIN" \
        -f "$FIRMWARE" \
        -s "$SAMPLERATE" \
        -g "$GAIN" \
        -q "$QUEUE_DEPTH" \
        -p "$REQ_SIZE" \
      | "$DSP_BIN" -v -o "$FIFO_DSP" &
    PIDS+=($!)
else
    # File playback: loop file -> rx888_dsp -> FIFO
    BLOCK_BYTES=524288
    sz=$(stat -c%s "$INFILE")
    trunc_sz=$(( (sz / BLOCK_BYTES) * BLOCK_BYTES ))
    [[ "$trunc_sz" -gt 0 ]] || { echo "ERROR: file smaller than one block"; exit 1; }
    (
        while true; do
            head -c "$trunc_sz" "$INFILE"
        done
    ) | "$DSP_BIN" -v -o "$FIFO_DSP" &
    PIDS+=($!)
fi

echo "Pipeline running. Ctrl-C to stop."
wait
