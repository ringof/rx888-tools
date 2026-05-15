# rx888_stream / librx888 — Test Plan

The streaming engine is `librx888.so`; `rx888_stream` is a CLI wrapper.
The two layers are tested together: the library through its public API,
the binary through its argparse and signal handling.

Tests are split into two phases:

- **Phase A — no hardware.**  Runs in CI on every PR.  Single command:
  `make check`.
- **Phase B — hardware in loop.**  Bench validation against a real
  RX888.  Single command: `make hw-check` (with `RX888_HW_TEST=1`).

---

## 1. Scope

**In scope**

- librx888 public API: lifecycle, validation, NULL safety, default
  config, no-device path.
- `rx888_stream` argparse: every documented flag accepted, unknown
  flags rejected, exit codes correct.
- USB streaming: throughput within tolerance, stop/start re-entrancy,
  sample-content sanity (no constant runs, DC offset, stddev,
  saturation).
- Compile cleanliness across gcc/clang.

**Out of scope**

- DSP correctness (covered by `doc/rx888_dsp.md` test scripts).
- RF performance (noise figure, IIP3, etc.).
- Firmware behaviour beyond "samples come out and STOPFX3 idles the
  device" (covered by `ringof/rx888-firmware`'s own test suite).

---

## 2. Phase A — no hardware

### A1. Build

```sh
make all
```

Library and all three binaries compile under
`-Werror -Wall -Wextra -Wpedantic`.  CI confirms this on Ubuntu 24.04.

Optional sanitizer build:

```sh
make clean
make CFLAGS_LIB="-O0 -g3 -fsanitize=address,undefined -fno-omit-frame-pointer \
                 -std=c11 -Wall -Wextra -fPIC -pthread \
                 $(pkg-config --cflags libusb-1.0)"
make check
```

ASAN/UBSAN should be silent on the no-hardware test runs.

### A2. ABI surface

The CI workflow (`.github/workflows/ci.yml`) asserts the exact set of
`T rx888_*` symbols exported by `librx888.so`.  Any added or renamed
public function must be reflected in the workflow's `expected` list,
or the build fails.

### A3. `make check`

Runs two scripts:

#### `tests/librx888_api`

C program built against `librx888.so` that exercises the public API
without a device:

- NULL inputs to every public function do not crash.
- `rx888_open(NULL, NULL)` and `rx888_open(&r, NULL)` return
  `LIBUSB_ERROR_INVALID_PARAM` and clear `*out`.
- `rx888_open` rejects a `calloc`'d (zero) config with
  `LIBUSB_ERROR_INVALID_PARAM`.
- `rx888_config_init_default()` populates every documented default.
- With a valid config and no device present, `rx888_open()` returns
  `LIBUSB_ERROR_NO_DEVICE` (skipped if `RX888_HW_PRESENT=1`).
- `rx888_version()` and `rx888_strerror()` return non-empty strings.

#### `tests/cli_smoke.sh`

Shell harness for `rx888_stream`:

- `-h` exits 0 and the help text mentions every documented long flag
  (`--firmware --verbose --dither --rand --fixup --samplerate
  --gainmode --gain --att --queuedepth --reqsize --ctrl-timeout
  --stream-timeout --watchdog-timeout --help`).  Catches accidental
  flag drops.
- `--does-not-exist` exits non-zero.
- No-device run with stdout redirected exits 1 (not 0, not segfault).

**Pass criteria:** all subtests print `ok`, the script prints
`ALL OK`, exit 0.

---

## 3. Phase B — hardware in loop

Run with `RX888_HW_TEST=1` once an RX888 is plugged in, firmware has
been fetched (`make firmware`), and `usbfs_memory_mb` is set
appropriately.

```sh
RX888_HW_TEST=1 make hw-check
```

`hw-check` runs three scripts in sequence; each fails fast.

### B1. `tests/hw_smoke.sh` — sustained throughput

Captures `RX888_SECS` (default 10) seconds of samples at
`RX888_RATE` (default 32 MS/s) and checks the byte count is within
`RX888_TOL_PCT` (default ±10 %) of the expected `rate × 2 × secs`.

Catches: stalls, dropped buffers, USB misconfiguration, queue/packet
sizing wrong.  Inspired by the `sustained_stream` scenario in
rx888-firmware's `fw_test.sh`; written fresh.

### B2. `tests/hw_stop_start.sh` — re-entrancy

Runs `RX888_CYCLES` (default 5) consecutive open/start/stop/close
cycles, verifying each produces non-trivial data and exits cleanly.

Catches: resource leaks (transfers, threads, USB handles), FX3
state-machine bugs that only show up after a few cycles, librx888
teardown issues.

### B3. `tests/hw_sample_check.py` — content sanity

Captures `RX888_SECS` (default 3) seconds of `int16` samples and
verifies properties that hold for any healthy ADC capture, with no
assumptions about a known signal at the antenna:

| Check                          | Default threshold | Override env var       |
|--------------------------------|-------------------|------------------------|
| Longest run of identical samples | ≤ 1024          | `RX888_MAX_RUN`        |
| DC offset                       | \|x\| ≤ 4096    | `RX888_MAX_DC`         |
| Sample stddev (first 1M)        | ≥ 100           | `RX888_MIN_STDDEV`     |
| Saturation (\|x\| ≥ 32700)      | ≤ 1.0 %         | `RX888_MAX_SAT_PCT`    |

Catches: stale-buffer reuse (constant runs), dead input (low stddev),
overdriven input (saturation).  Pure-Python so it runs without
NumPy/SciPy.

### B4. Manual checks worth keeping in your head

These aren't scripted yet:

- **Broken pipe behaviour:**
  `rx888_stream ... | head -c 1 > /dev/null` — should exit cleanly,
  no hang, no leftover process.
- **Controlled disconnect:** unplug the device mid-stream — should
  exit on `LIBUSB_TRANSFER_NO_DEVICE`, no segfault.
- **CPU jitter resilience:** run `stress-ng --cpu 4 --timeout 30` in
  parallel with a 30-minute capture; should sustain throughput.
- **30-minute soak:** any of the above commands run for 30 minutes
  to a `pv -rab > /dev/null` consumer.  Watch for runaway memory or
  drifting MiB/s.

If any of these become recurring concerns, fold them into
`tests/hw_*` scripts.

---

## 4. Exit criteria

`librx888` and `rx888_stream` are considered ready when:

- `make all` passes under `-Werror` on gcc and clang.
- `make check` passes with no failures.
- `make hw-check` passes with the default thresholds at 32 MS/s.
- A 30-minute 135 MS/s capture to `pv -rab -B 4M > /dev/null`
  sustains ≥ 90 % of theoretical throughput.
- Manual broken-pipe and disconnect checks exit cleanly.

---

## Appendix — quick commands

```sh
# Non-hardware
make check

# Hardware (with device + firmware fetched)
make firmware
RX888_HW_TEST=1 make hw-check

# Throughput watch
./rx888_stream -v -s 135000000 > /dev/null

# Broken pipe
./rx888_stream -s 32000000 | head -c 1 > /dev/null; echo "exit=$?"

# Sanitizer + non-hw tests
make clean
make CFLAGS_LIB="-O0 -g3 -fsanitize=address,undefined \
                 -std=c11 -Wall -Wextra -fPIC -pthread \
                 $(pkg-config --cflags libusb-1.0)"
make check
```
