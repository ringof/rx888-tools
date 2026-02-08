# iqrecord

## Overview

`iqrecord` is a high-throughput, Unix-style IQ data recorder designed for
continuous capture at very high sample rates (>=135 MS/s), such as those
produced by the RX888 SDR when used with external DSP pipelines.

It reads complex float32 IQ samples from **stdin**, splits them into
fixed-size files, and writes **SigMF-compatible** metadata alongside each
capture. A session-level `run.json` is also written for traceability.

The program is designed as a **Unix filter**:

    producer | iqrecord OUTDIR [OPTIONS]

---

## Project Status

**Status:** Stable
**Scope:** Single-stream IQ recorder with SigMF metadata
**Target platforms:** Linux x86-64 (no architecture-specific requirements)

---

## Build

```sh
gcc -O3 -Wall -Wextra -Wpedantic -std=c11 iqrecord.c -o iqrecord
```

Optional debug build:

```sh
gcc -O0 -g -Wall -Wextra -std=c11 iqrecord.c -o iqrecord
```

No external libraries required.

---

## Usage

```sh
iqrecord OUTDIR [OPTIONS]
```

### Options

- `--freq HZ` -- Center frequency in Hz (written to SigMF metadata).
- `--desc TEXT` -- Free-form description (correctly JSON-escaped in metadata).
- `--fsync` -- Force `fsync()` on each data file before rotation. Ensures
  durability at the cost of potential throughput reduction.

### Compiled-In Defaults

- Sample rate: **33.75 MHz**
- Samples per file: **337,500,000** (~10 s at 33.75 MS/s)
- Block size: **4,194,304 samples** (32 MiB reads)
- Datatype: **cf32_le**

### Input / Output

**Input:** complex float32 IQ (interleaved `I0,Q0,I1,Q1,...`, little-endian)
on stdin. Binary I/O to a terminal is refused.

**Output (per capture segment):**
- `cap_NNNNNN.sigmf-data` -- Raw IQ samples
- `cap_NNNNNN.sigmf-meta` -- SigMF-compliant JSON metadata

**Output (per session):**
- `run.json` -- Session manifest with per-file accounting and timestamps

### Signals

- `SIGINT`, `SIGTERM` -- Clean shutdown. The current file is finalized,
  metadata is written, and `run.json` is closed with final accounting.
- Producer EOF -- Same clean finalization as signal shutdown.
- `SIGKILL` -- Partial files and invalid `run.json` are expected (cannot be
  prevented).

### Exit Status

- `0` -- Success (normal EOF or signal shutdown)
- `1` -- Runtime error (I/O failure, disk full, write error)

---

## Examples

```sh
# Basic capture from a producer:
some_producer | ./iqrecord captures/session1

# With metadata:
some_producer | ./iqrecord captures/session1 --freq 7100000 --desc "40m band"

# Full RX888 pipeline:
rx888_stream | rx888_dsp | mbuffer -m 4G -q \
  | ./iqrecord captures/rx888_run --freq 7100000 --desc "40m band capture"

# Durability mode (fsync each file before rotation):
some_producer | ./iqrecord captures/forensic --fsync
```

---

## Notes

- `iqrecord` assumes continuous input. Use `mbuffer` upstream to absorb
  scheduler jitter at high data rates (~270 MB/s at 33.75 MS/s).
- Output via stdout is not supported; output always goes to files in `OUTDIR`.
- Partial samples at end-of-stream (e.g., from an abrupt producer shutdown)
  are dropped with a warning.
- On write error (e.g., disk full), `iqrecord` prints a diagnostic, finalizes
  `run.json` with correct accounting, and exits with status 1.
- On `SIGKILL`, partial files and missing/invalid metadata are expected and
  documented behavior.

---

## Validation

See [TEST_PLAN.md](TEST_PLAN.md) for the incremental test methodology covering
boundary correctness, throughput, error injection, and full-pipeline stress.
