# rx888_stream -- Test Plan

This test plan targets the RX888 USB3 stream driver (firmware upload,
control-plane configuration, and high-rate streaming to stdout), with
emphasis on robustness (no crashes/segfaults), throughput stability,
and clean recovery from common failure modes.

The tests are organized in two phases:

- **Phase A: No-hardware / build-only** (what you can do immediately)
- **Phase B: Hardware-in-loop** (when you can plug an RX888 in)

---

## 1. Scope

### In Scope
- Compilation correctness across toolchains (gcc/clang)
- Static analysis and sanitizer runs
- Argument parsing and configuration validation
- USB control transaction sequencing (best-effort validation via logs)
- Streaming behavior and stdout backpressure handling
- Clean shutdown and resource cleanup
- Failure-path behavior (device missing, firmware upload failures,
  USB resets/disconnects)

### Out of Scope
- DSP correctness (handled in rx888_dsp test plan)
- RF performance (noise floor, spurs, etc.)
- Firmware reverse-engineering

---

## 2. Test Environments

### Host OS
- Ubuntu 22.04+ (or Debian stable), x86-64
- Kernel 5.15+ recommended for USB3 stability

### Toolchains
- gcc (>= 10)
- clang (>= 14)

### Dependencies
- libusb-1.0 development headers and library
- pthreads (standard on Linux)
- Optional: clang-tidy, cppcheck, valgrind, strace, pv

### Hardware (Phase B)
- RX888 (USB3 streaming path)
- A known-good USB3 port and cable
- Optional: powered USB3 hub (for controlled disconnect tests)

---

## 3. Build-Only Validation (Phase A)

### A1. Clean Builds with Strict Warnings

Build matrix:

```sh
# gcc
gcc -O2 -g -Wall -Wextra -Wshadow -Wformat=2 -Wcast-align \
    -Wstrict-prototypes -Wmissing-prototypes -Wconversion \
    -Wsign-conversion -Werror \
    -o rx888_stream rx888_stream.c ezusb.c \
    $(pkg-config --cflags --libs libusb-1.0) -lpthread

# clang (same flags)
```

Pass criteria: clean build, no warnings, no undefined references.

### A2. Static Analysis

```sh
scan-build make
cppcheck --enable=all --inconclusive --std=c11 --force .
```

Pass criteria: no high-severity issues (null deref, uninit, leaks).
Medium-severity findings triaged (documented or fixed).

### A3. Sanitizers (Without Hardware)

Even without a device, you can validate argument parsing and early error
paths.

Build with `-O0 -g3 -fsanitize=address,undefined -fno-omit-frame-pointer`.

Run:
- `./rx888_stream --help`
- `./rx888_stream -s 135000000` (expect "device not found" but no crash)
- Invalid args: `--samplerate 0`, `--samplerate -1`, `--gain 999`,
  `--att 999`, `--gainmode foo`

Pass criteria: no ASAN/UBSAN reports, clean exits with clear error messages.

### A4. CLI Regression

Verify:
- `--help` output matches current option set and defaults.
- All flags accepted and reflected in verbose logs.
- Invalid combinations rejected with exit status 2.
- Running without stdout redirection prints TTY error and exits 2:
  ```sh
  ./rx888_stream -s 135000000
  # Expected: "rx888_stream: stdout is a TTY; redirect to a file or pipe"
  echo "exit: $?"
  # Expected: exit: 2
  ```

---

## 4. Hardware-in-Loop Streaming (Phase B)

### B1. Device Discovery

Cases:
1. RX888 not connected -- exits cleanly with clear message.
2. RX888 in APP PID state (0x00f1) -- opens and streams.
3. RX888 in BOOT PID state (0x00f3) -- uploads firmware (when `-f`
   provided), transitions to APP PID, then streams.

### B2. Control-Plane Configuration

Run combinations:
- Gain mode: high/low
- Gain: min/mid/max (0, 64, 127)
- Attenuation: 0, 10, 63
- Sample rate: 32000000, 135000000

Pass criteria: control transfers succeed (no libusb errors), device
streams after configuration, no wedges on settings changes.

### B3. Throughput

Run with USB3 settings:

```sh
# Baseline
./rx888_stream -s 135000000 -p 1024 -q 32 | pv -rab -B 4M > /dev/null

# Stress
./rx888_stream -s 135000000 -p 2048 -q 64 | pv -rab -B 4M > /dev/null
```

Pass criteria:
- Sustained throughput near ~257 MiB/s for 135 MS/s int16 real.
- `write()` sizes near transfer size (~1 MiB), not 8 KiB.
- No periodic stalls > 100 ms under normal desktop load.

### B4. Backpressure and Broken Pipe

Tests:
1. Consumer exits immediately: `./rx888_stream ... | head -c 1 > /dev/null`
2. Slow consumer: `./rx888_stream ... | pv -L 10m > /dev/null`

Pass criteria: driver exits cleanly without segfault, writer thread
terminates, transfers cancelled and freed, no runaway CPU loop.

### B5. Controlled Disconnect

While streaming, physically unplug RX888 (or disable hub port).

Pass criteria: driver detects `LIBUSB_TRANSFER_NO_DEVICE` and exits
cleanly. No hang, no segfault.

### B6. CPU Jitter Resilience

While streaming, introduce background load:

```sh
stress-ng --cpu 4 --timeout 30
stress-ng --hdd 2 --timeout 30
```

Pass criteria: no wedges, minimal dropouts. If dropouts occur, they are
logged and the program exits cleanly (fail-fast) rather than crashing.

---

## 5. Functional Correctness

### C1. IQ Framing Sanity

Capture a few seconds and inspect:
- File size matches expected rate (bytes/sec).
- Sample alignment (even number of bytes, consistent framing).
- Optional: apply known signal and confirm spectrum looks plausible.

Pass criteria: no corruption patterns (periodic zeros, repeated blocks).

---

## 6. Exit Criteria

`rx888_stream` is considered ready when:

- No warnings on gcc/clang builds with strict flags.
- No sanitizer findings in build-only runs.
- Can stream continuously for 30 minutes with `-p 1024 -q 32` to a
  consumer without wedges.
- Clean behavior on consumer exit (EPIPE).
- Clean behavior on device disconnect (NO_DEVICE).

---

## Appendix: Quick Test Commands

```sh
# Throughput
./rx888_stream -s 135000000 -p 1024 -q 32 | pv -rab -B 4M > /dev/null

# TTY check (no redirection -- should refuse)
./rx888_stream -s 135000000; echo "exit: $?"

# Syscall size check
sudo strace -f -tt -e trace=write -s 0 -p $(pidof rx888_stream)

# Broken pipe
./rx888_stream -s 135000000 -p 1024 -q 32 | head -c 1 > /dev/null

# CPU jitter stress (run in parallel with streaming)
stress-ng --cpu 4 --timeout 30
```
