# rx888_stream

## Overview

`rx888_stream` streams **int16 real** samples from the RX888 / RX888mk2
directly to stdout over USB3. It is designed to sustain **135 MS/s
(~257 MiB/s)** when properly configured.

The program is designed as the **source end of a Unix pipeline**:

    rx888_stream [OPTIONS] | rx888_dsp | mbuffer | iqrecord ...

It handles firmware upload, device configuration, and high-rate bulk
streaming with clean shutdown on signal, broken pipe, or device disconnect.

---

## Project Status

**Status:** Stable
**Scope:** Single-device USB3 streaming driver
**Target platforms:** Linux x86-64, USB3-capable host

---

## Requirements

- Linux (kernel 5.15+ recommended for USB3 stability)
- USB3-capable host and cable
- RX888 / RX888mk2
- libusb-1.0 development headers and library
- Sufficient USBFS buffer memory (see below)

---

## Build

```sh
make
```

Or manually:

```sh
gcc -O2 -Wall -Wextra -std=c11 -o rx888_stream rx888_stream.c ezusb.c \
    $(pkg-config --cflags --libs libusb-1.0) -lpthread
```

### udev Rule (Recommended)

Install so the device is accessible without root:

```sh
sudo cp 99-rx888.rules /etc/udev/rules.d/
sudo udevadm control --reload
```

Unplug and replug the RX888.

---

## IMPORTANT: USB Buffer Memory

At 135 MS/s, the RX888 produces ~257 MiB/s. Linux must allow enough USBFS
buffer memory to sustain this without allocation failures.

Check current value:

```sh
cat /sys/module/usbcore/parameters/usbfs_memory_mb
```

If it is low (often 16), increase it:

```sh
sudo sh -c 'echo 256 > /sys/module/usbcore/parameters/usbfs_memory_mb'
```

To make this persistent across reboots:

```sh
echo 'options usbcore usbfs_memory_mb=256' | sudo tee /etc/modprobe.d/usbcore.conf
```

---

## Usage

```sh
rx888_stream [OPTIONS] > iq.raw
```

### Options

- `-f, --firmware <file>` -- Upload firmware if device is in boot mode.
- `-v, --verbose[=N]` -- Verbosity level (default 0; `-v` without N sets 1).
  Level 1: periodic throughput stats. Level 2+: libusb diagnostics.
- `-s, --samplerate <Hz>` -- Sample rate. Supported: 32000000, 135000000.
  Default: 32000000.
- `-m, --gainmode <low|high>` -- Gain mode. Default: high.
- `-g, --gain <0..127>` -- VGA gain value. Default: 0.
- `-a, --att <0..63>` -- Attenuation value. Default: 0.
- `-d, --dither` -- Enable dither GPIO.
- `-r, --rand` -- Enable randomizer GPIO.
- `-q, --queuedepth <N>` -- Concurrent in-flight USB transfers.
  Default: 32.
- `-p, --reqsize <N>` -- Transfer size in packets. Default: 1024
  (~1 MiB per transfer on USB3).
- `--fixup` -- Enable legacy sample fixup (bit tweak in callback).
- `--ctrl-timeout <ms>` -- Control transfer timeout. Default: 5000.
- `--stream-timeout <ms>` -- Stream transfer timeout. Default: 0 (infinite).
- `--watchdog-timeout <ms>` -- No-data watchdog. If no USB callbacks arrive
  for this long while transfers are in-flight, assume the link is wedged
  and exit. Default: 3000. Set to 0 to disable.
- `-h, --help` -- Show help and exit.

### Output Format

**little-endian signed int16, real samples** on stdout.

At 135 MS/s, each sample is 2 bytes, producing ~257 MiB/s.

### Signals

- `SIGINT`, `SIGTERM` -- Clean shutdown. In-flight transfers are cancelled,
  USB interface released, device closed.
- `SIGPIPE` -- Ignored. Broken pipe is detected via `write()` and triggers
  clean shutdown.

### Exit Status

- `0` -- Success (normal shutdown or signal)
- `1` -- Runtime error (device not found, USB failure, I/O error)
- `2` -- Usage error (invalid arguments)

---

## Firmware Upload

If the RX888 is in boot mode (PID 0x00f3), firmware must be uploaded:

```sh
./rx888_stream -f SDDC_FX3.img > iq.raw
```

After firmware upload and re-enumeration, subsequent runs do not require
`-f` unless the device was power-cycled.

---

## Examples

### Recommended Settings for 135 MS/s

```sh
./rx888_stream -s 135000000 -q 32 -p 1024 > iq.raw
```

- `-s 135000000` -- 135 MS/s sample rate
- `-p 1024` -- ~1 MiB per USB transfer (1024 packets x 1024 bytes)
- `-q 32` -- 32 concurrent in-flight transfers (~32 MiB total buffering)

### Limiting Output Size (Testing / Capture)

```sh
./rx888_stream -s 135000000 -q 32 -p 1024 | pv -Ss 2G > capture.raw
```

### Verbose Throughput Monitoring

```sh
./rx888_stream -v -s 135000000 -q 32 -p 1024 > /dev/null
```

Example output:

```
t=5s ok=1290 bad=0 in_flight=31 out=1290.00 MiB (257.49 MiB/s)
```

### Quick Sample Sanity Check

```sh
./rx888_stream -s 135000000 -q 32 -p 1024 \
  | head -c $((64*1024*1024)) \
  | python3 -c "
import sys, numpy as np
x = np.frombuffer(sys.stdin.buffer.read(), dtype='<i2')
print('min/max:', x.min(), x.max())
print('std:', x.std())
print('rms:', np.sqrt((x.astype(np.float64)**2).mean()))
"
```

With no antenna (or a 50-ohm terminator), values should cluster near zero.

### Full Pipeline

```sh
./rx888_stream -s 135000000 -q 32 -p 1024 \
  | rx888_dsp --block-on-full -v \
  | mbuffer -m 4G -q \
  | iqrecord captures/session --freq 7100000 --desc "40m band"
```

---

## Notes

- Output is binary int16 samples on stdout. If stdout is a terminal, the
  program prints an error and exits (status 2). Redirect to a file or pipe.
- No buffering is performed outside the internal USB queue and writer thread.
  Use `mbuffer` downstream to absorb scheduler jitter.
- The design intentionally avoids blocking inside libusb callbacks; a
  separate writer thread handles stdout I/O and transfer resubmission.
- If the RX888 firmware wedges, a power-cycle may be required.
- For USB3 performance, tune `-p` and `-q` to stay within `usbfs_memory_mb`.
- If downstream stalls long enough to fill the internal transfer queue
  (queue depth x transfer size), `rx888_stream` exits rather than blocking
  the USB event loop or silently dropping samples. Increase `-q` or fix
  the downstream bottleneck.

---

## Validation

See [TEST_PLAN.md](TEST_PLAN.md) for build-only and hardware-in-loop test
procedures covering argument parsing, sanitizer runs, throughput, backpressure,
disconnect recovery, and CPU jitter resilience.

---

## Origin

Rewrite of [rx888_test](https://github.com/cozycactus/rx888_test) with a
focus on sustained 135 MS/s operation, robust USB streaming, and clean
pipe-based integration with external DSP.

---

## License

`rx888_stream.c` is MIT-licensed. However, `ezusb.c` and `ezusb.h` are
licensed under the GNU General Public License v2 (or later). Because the
binary links both together, the combined work is distributed under the
terms of the GPL-2.0-or-later. See the license headers in each source
file for details.
