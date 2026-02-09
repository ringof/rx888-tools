# rx888_tools

A Linux toolset for high-throughput SDR streaming with the RX888 / RX888mk2.
Three command-line programs form a Unix pipeline that captures raw USB3
samples, decimates them to complex IQ, and records to disk in SigMF format:

```
rx888_stream  →  rx888_dsp  →  iqrecord
  (USB3→stdout)   (stdin→stdout)   (stdin→files)
  int16 real       int16→cf32       cf32_le
  135 MS/s         4:1 decimation   SigMF + run.json
                   33.75 MS/s out
```

Each program does one thing and communicates via pipes, so any stage can be
replaced, teed, buffered (`mbuffer`, `pv`), or omitted as needed.

---

## Requirements

- **Linux** (x86_64) — kernel USB3 support, sufficient USBFS buffer memory
- **libusb-1.0** development headers (`libusb-1.0-0-dev` on Debian/Ubuntu)
- **AVX2 + FMA** capable CPU (required by `rx888_dsp`)
- **RX888 / RX888mk2** with USB3 cable and host port

---

## Build

```bash
make
```

This builds all three binaries in the project root. To build a single
program:

```bash
make rx888_stream
make rx888_dsp
make iqrecord
```

---

## Install / Uninstall

```bash
sudo make install                      # installs to /usr/local/bin
sudo make install PREFIX=/opt/rx888    # or a custom prefix
sudo make uninstall
```

This installs the binaries, the firmware image, and the udev rule. After
installing, reload udev rules:

```bash
sudo udevadm control --reload-rules && sudo udevadm trigger
```

---

## Quick Start

**1. Set up USB buffer memory** (required once per boot for 135 MS/s):

```bash
sudo sh -c 'echo 1000 > /sys/module/usbcore/parameters/usbfs_memory_mb'
```

**2. Stream, decimate, and record:**

```bash
rx888_stream -f firmware/SDDC_FX3.img -s 135000000 \
  | rx888_dsp --block-on-full \
  | iqrecord /data/capture --freq 7100000 --desc "7.1 MHz capture"
```

This uploads firmware (if the device is in DFU mode), captures at
135 MS/s, decimates 4:1 to 33.75 MS/s cf32, and writes 10-second SigMF
files to `/data/capture/`. Use `--block-on-full` to backpressure
instead of dropping blocks when the recorder falls behind.

**3. Stream to GQRX via FIFO:**

```bash
mkfifo /tmp/iq.fifo
rx888_stream -f firmware/SDDC_FX3.img -s 135000000 \
  | rx888_dsp -o /tmp/iq.fifo
```

Configure GQRX to read from `/tmp/iq.fifo` (complex float32, 33.75 MHz).

**4. Buffer with mbuffer for sustained writes:**

```bash
rx888_stream -f firmware/SDDC_FX3.img -s 135000000 \
  | rx888_dsp --block-on-full -v \
  | mbuffer -m 2G -q \
  | iqrecord /data/capture --freq 7100000
```

The `mbuffer` stage absorbs disk I/O stalls, preventing backpressure
from reaching the DSP pipeline.

---

## Programs

| Program | Purpose | Input | Output |
|---------|---------|-------|--------|
| `rx888_stream` | USB3 bulk capture | RX888 device | int16 real samples on stdout |
| `rx888_dsp` | DSP decimation (4:1) | int16 real on stdin | cf32 IQ on stdout or FIFO |
| `iqrecord` | SigMF file recorder | cf32 IQ on stdin | `.sigmf-data` + `.sigmf-meta` files |

### rx888_stream

```
rx888_stream -f firmware/SDDC_FX3.img [options] > iq.raw
```

Key options: `-f <firmware>` (required on first run after power-cycle),
`-s <Hz>` (sample rate: 32000000 or 135000000, default 32000000),
`-v` (verbose), `-g <0..127>` (gain), `-q <N>` (queue depth),
`-p <N>` (request size in packets). Run `rx888_stream -h` for full usage.

### rx888_dsp

```
rx888_dsp [OPTIONS]
```

Key options: `-o <path>` (output to FIFO instead of stdout),
`-v` (verbose + stats), `--block-on-full` (backpressure instead of
dropping blocks). Send `SIGUSR1` to print live statistics.
Run `rx888_dsp -h` for full usage.

### iqrecord

```
iqrecord OUTDIR [--freq HZ] [--desc TEXT] [--fsync]
```

Reads cf32 IQ from stdin and writes 10-second SigMF file pairs plus a
session-level `run.json`. Use `--freq` to set the center frequency in
metadata and `--desc` for a capture description.

See `doc/` for detailed per-program documentation, architecture notes, and
test plans.

---

## Project Layout

```
rx888_tools/
├── Makefile              Build system with install/uninstall
├── src/                  Source code (C11)
│   ├── rx888_stream.c    USB3 streamer + ezusb firmware loader
│   ├── ezusb.c           EZ-USB/FX3 firmware upload (vendor code)
│   ├── rx888_dsp.c       AVX2/FMA DSP pipeline (3 threads)
│   └── iqrecord.c        SigMF recorder with file rotation
├── include/              Shared headers
│   ├── rx888.h           RX888 protocol constants
│   └── ezusb.h           EZ-USB API
├── doc/                  Documentation
├── tests/                Test scripts and generators
├── firmware/             RX888 FX3 firmware image
└── udev/                 udev rule for non-root device access
```

---

## License

MIT — see individual source files for copyright notices.
