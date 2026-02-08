# rx888_dsp

## Overview

`rx888_dsp` is a high-performance, real-time DSP decimation pipeline designed for
the RX888 family of SDRs.

It converts a **135 MS/s real int16 input stream** into a
**33.75 MS/s complex float32 IQ stream**, suitable for downstream receivers
such as GQRX.

The program is designed as a **Unix filter**:

    stdin -> DSP pipeline -> stdout or FIFO

It is optimized for modern x86-64 CPUs with **AVX2 + FMA** support and is intended
to run continuously in real time with processing headroom.

---

## Project Status

**Status:** Stable
**Scope:** Single-pipeline, real-time DSP filter
**Target platforms:** Linux x86-64, AVX2-capable CPUs

---

## Build

```sh
gcc -O3 -mavx2 -mfma -std=c11 -Wall -Wextra -Wpedantic \
    -o rx888_dsp rx888_dsp.c \
    -lm -lpthread
```

Requires GCC or Clang with AVX2 + FMA support on x86-64 Linux.

---

## Usage

```sh
rx888_dsp [OPTIONS]
```

### Options

- `-o, --output PATH` -- Write output IQ stream to `PATH` (must be a FIFO).
  If omitted, output is written to stdout.
- `-v, --verbose` -- Enable verbose logging and performance statistics at shutdown.
- `--block-on-full` -- Apply backpressure when downstream is slow instead of
  dropping blocks.
- `-h, --help` -- Show detailed help and exit.
- `-V, --version` -- Show program version and exit.

### Input / Output Format

**Input:** signed 16-bit real samples on stdin at 135 MS/s (native little-endian).

**Output:** complex float32 IQ (interleaved `I0,Q0,I1,Q1,...`) at 33.75 MS/s
on stdout or a FIFO.

Binary I/O to a terminal is refused. Input and output must be connected via
pipes, redirection, or FIFOs.

### Signals

- `SIGINT`, `SIGTERM`, `SIGHUP` -- Clean shutdown. All queued blocks are
  drained through the pipeline before the process exits (no silent data loss
  while output is connected).
- `SIGUSR1` -- Print performance statistics to stderr. Without `-v`, prints a
  compact one-line summary. With `-v`, prints a detailed multi-line block.
- `SIGPIPE` -- Ignored; handled internally to allow downstream reconnects.

### Exit Status

- `0` -- Success
- `1` -- Runtime error (I/O, memory allocation, thread failure)
- `2` -- Usage error (invalid options, TTY detected)

---

## Examples

```sh
# Simple pipeline: file -> decimator -> output file
cat capture.i16 | ./rx888_dsp > output.cf32

# With a FIFO for GQRX:
mkfifo /tmp/iq.fifo
cat capture.i16 | ./rx888_dsp --block-on-full -v -o /tmp/iq.fifo

# Check performance during operation:
kill -USR1 $(pgrep rx888_dsp)
```

For a complete GQRX relay setup with mbuffer, see `rx888_dsp_to_gqrx.sh`.

---

## Notes

- This program is a Unix filter, not an interactive application.
- Output via `-o` is restricted to FIFOs to prevent accidental disk writes.
  For file output, use shell redirection: `rx888_dsp > output.iq`
- The program requires an AVX2 + FMA capable CPU and will not run otherwise.
- Shutdown is clean: on signal or EOF, all blocks already in the pipeline are
  processed and written before the process exits.
- When using `-o PATH` (FIFO output), blocks are dropped if no reader is
  connected. Drop counts are reported via SIGUSR1 and at shutdown. To avoid
  startup drops, start the FIFO consumer before the producer.
- If the reader disconnects mid-stream (EPIPE), rx888_dsp closes the FIFO and
  attempts to reconnect. Blocks are dropped while disconnected.
- On shutdown, queued blocks are drained through the pipeline. If the FIFO has
  no reader during drain, remaining blocks are dropped rather than blocking
  the exit.
- `--block-on-full` controls backpressure between the input reader and the
  processing thread. It does not affect FIFO writer behavior.

---

## Design

See [ARCHITECTURE.md](ARCHITECTURE.md) for details on the DSP pipeline,
stage fusion strategy, SIMD implementation, and threading model.
