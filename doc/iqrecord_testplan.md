# iqrecord -- Test Plan

This document describes an incremental test plan to validate correctness,
robustness, and performance of `iqrecord`.

Tests are ordered from lowest risk / fastest feedback to full end-to-end
SDR operation.

---

## Phase 0: Build and Static Checks

### Goals
- Ensure code builds cleanly with no warnings.

### Tests

```sh
gcc -O3 -Wall -Wextra -Wpedantic -std=c11 iqrecord.c -o iqrecord
```

- Verify zero warnings.
- Run under `valgrind --tool=none` as a basic sanity check.

### 0.1 JSON Escaping

Ensure user-provided strings (e.g., `--desc`) produce valid JSON:

```sh
rm -rf captures/json_escape_test
./tests/counter_cf32.py --limit-samples 1000000 \
  | ./iqrecord captures/json_escape_test \
    --desc 'quotes: "double" and backslash: \\ and newline:\n and tab:\t'

jq . captures/json_escape_test/run.json >/dev/null
jq . captures/json_escape_test/cap_000000.sigmf-meta >/dev/null
```

Expected: `jq` succeeds for both files.

### 0.2 `--fsync` Smoke Test

```sh
rm -rf captures/fsync_smoke
./tests/counter_cf32.py --limit-samples 1000000 \
  | ./iqrecord captures/fsync_smoke --fsync --desc "fsync smoke"
jq . captures/fsync_smoke/run.json >/dev/null
```

Expected: no crash, capture written, `run.json` parses.

---

## Phase 1: No-Data / Error Handling

### Goals
- Validate graceful behavior with no input.

### Tests

```sh
./iqrecord captures/empty_test < /dev/null
```

Expected: no crash, no data files, warning about no data read.

---

## Phase 2: Synthetic Counter Source (Continuity Truth Test)

### Goals
- Prove boundary correctness.
- Prove no dropped or duplicated samples at file boundaries.

### Test Harness

The following scripts live in `tests/`:

- `counter_cf32.py` -- Generates cf32_le IQ where I increments by +1 per
  sample (wraps below 2^24 for exact float32 representability), Q = 0.
- `verify_cf32.py` -- Verifies file sizes, per-file first/last counters,
  and boundary continuity.

### Run

Record ~25 seconds (2 full 10 s files + 1 partial):

```sh
./tests/counter_cf32.py --limit-samples 900000000 \
  | mbuffer -m 1G -q \
  | ./iqrecord captures/counter_test --desc "cf32 counter continuity test"
```

### Verify

```sh
./tests/verify_cf32.py "captures/counter_test/cap_*.sigmf-data" --wrap 16000000
```

Expected: each file reports `firstI` and `lastI`, all boundary checks pass.

**This test must pass before any real SDR testing.**

---

## Phase 3: Block Boundary Stress

### Goals
- Exercise spillover logic and catch off-by-one errors.

### Tests
- Temporarily modify `block_samples` to odd values that force frequent
  file-boundary crossings within a single read.
- Run for many file rotations (minutes).

Expected: perfect continuity, no partial samples, no malformed metadata.

---

## Phase 4: Throughput

### Goals
- Validate sustained throughput at target data rates.
- Detect disk or buffering bottlenecks.

At cf32 with Fs=33.75 MHz, the payload rate is ~270 MB/s.

### Tests

```sh
./tests/counter_cf32.py --limit-samples 2000000000 \
  | pv -br \
  | mbuffer -m 2G -q \
  | ./iqrecord captures/perf_test
```

Monitor: MB/s stability, CPU usage (`pidstat -t 1`), disk write behavior
(`iostat -x 1`).

Expected: stable throughput >=270 MB/s, no stalls, no memory growth.
NVMe utilization below ~80%.

---

## Phase 5: Error Injection

### Goals
- Validate robustness under failure.
- Ensure artifacts are usable after interruption.

### 5.1 Disk Full

Fill disk or use a quota-limited filesystem. Confirm `iqrecord` prints a
diagnostic, finalizes `run.json`, and exits with status 1. No silent
corruption of existing files.

### 5.2 Kill Producer Mid-Stream

Kill the upstream producer abruptly. Expected: `iqrecord` finalizes the
last file and drops any trailing partial-sample bytes with a warning.

### 5.3 SIGINT / SIGTERM

```sh
./tests/kill_iqrecord_test.sh captures/kill_test sigint 0.5
./tests/kill_iqrecord_test.sh captures/kill_test sigterm 0.5
```

Expected:
- `run.json` exists and is valid JSON.
- No `.sigmf-data` files are missing their `.sigmf-meta`.
- The last file may be shorter than 10 seconds but is sample-aligned.

### 5.4 SIGKILL (Worst Case)

```sh
./tests/kill_iqrecord_test.sh captures/kill_test sigkill 0.5
```

Expected: missing `.sigmf-meta` and/or invalid `run.json` for the active
file is possible (cannot be prevented). Earlier completed files remain valid.

---

## Phase 6: End-to-End Pipeline

### Goals
- Validate the full recording chain under realistic conditions.
- Detect scheduling jitter, USB burstiness, and I/O backpressure issues.

### 6.1 Replay from File (Deterministic Source)

Replay a previously-recorded RX888 raw capture through the DSP chain:

```sh
cat rx888_raw_i16.bin \
  | rx888_dsp \
  | mbuffer -m 2G -q \
  | ./iqrecord captures/replay_test --freq <Hz>
```

This isolates DSP + recorder interaction without USB timing variability.
Expected: identical behavior run-to-run, no dropped samples.

### 6.2 Replay with CPU Contention

While running 6.1, introduce background load:

```sh
stress-ng --cpu 6 --vm 2 --vm-bytes 4G
```

Or competing I/O:

```sh
fio --name=nvme_noise --rw=write --bs=1M --size=50G --numjobs=2
```

Expected: temporary buffering absorption, no permanent sample loss.
Failures here indicate system-level scheduling limits, not recorder bugs.

### 6.3 Live RX888 Capture

```sh
rx888_stream | rx888_dsp | mbuffer -m 4G -q \
  | ./iqrecord captures/rx888_live --freq <Hz>
```

Recommendations:
- Pin processes to cores (`taskset`)
- Isolate one core for USB IRQ handling
- Use `ionice -c1` for recorder

Monitor: any overrun/drop counters, `dmesg -w` for USB/xHCI errors,
`iostat -x 1`, CPU frequency and thermals.

Expected: stable continuous recording, no USB errors, clean SigMF output.

---

## Success Criteria

`iqrecord` is considered correct when:

- Boundary continuity tests pass repeatedly (Phase 2).
- No sample loss attributable to `iqrecord`.
- Metadata timestamps are consistent and monotonic.
- Output is consumable by downstream SigMF-aware tools.
- `run.json` is always finalized on clean shutdown (signal or EOF).

Only after these criteria are met should refactoring, modularization, or
integration into larger frameworks be attempted.
