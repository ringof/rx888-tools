# rx888-tools

A Linux toolset for high-throughput SDR streaming with the RX888 / RX888mk2.
The streaming engine is a shared library (`librx888`); on top of it, three
command-line programs form a Unix pipeline that captures raw USB3 samples,
decimates them to complex IQ, and records to disk in SigMF format:

```
rx888_stream  →  rx888_dsp  →  iqrecord
  (USB3→stdout)   (stdin→stdout)   (stdin→files)
  int16 real       int16→cf32       cf32_le
  135 MS/s         4:1 decimation   SigMF + run.json
                   33.75 MS/s out
```

`rx888_stream` is a thin CLI wrapper around `librx888.so`. The library is
also what `gr-rx888` (a GNU Radio out-of-tree source block, planning docs
under `doc/gr-rx888/`) will consume — same code path for both.

---

## Requirements

- **Linux** (x86_64) — kernel USB3 support, sufficient USBFS buffer memory
- **libusb-1.0** development headers (`libusb-1.0-0-dev` on Debian/Ubuntu)
- **AVX2 + FMA** capable CPU (required by `rx888_dsp` only)
- **RX888 / RX888mk2** with USB3 cable and host port

Build deps on Ubuntu 24.04:

```bash
sudo apt install build-essential pkg-config libusb-1.0-0-dev
```

---

## Build

```bash
make            # librx888.so + librx888.a + pkg-config + 3 binaries
make check      # non-hardware ABI / CLI tests (also runs in CI)
```

To build a single program:

```bash
make rx888_stream    # links librx888.so
make rx888_dsp
make iqrecord
```

---

## Firmware

The FX3 firmware blob is **not vendored**. It is fetched from the
[`ringof/rx888-firmware`](https://github.com/ringof/rx888-firmware) release
matching `firmware/VERSION` and checksum-verified against `firmware/SHA256SUMS`:

```bash
make firmware                # fetch the pinned version
make firmware-latest         # bump to the most recent rx888-firmware release
                             #   (used by the auto-bump CI workflow)
```

`firmware/SDDC_FX3.img` is gitignored. `make install` refuses to run until
the blob has been fetched.

---

## Install / Uninstall

```bash
sudo make install                      # installs to /usr/local
sudo make install PREFIX=/opt/rx888    # custom prefix
sudo make uninstall
```

This installs `librx888.so`, `librx888.a`, `librx888.pc`, `librx888.h`,
the three binaries, the firmware blob, and the udev rule. After installing:

```bash
sudo udevadm control --reload-rules && sudo udevadm trigger
```

---

## Quick Start

**1. Set up USB buffer memory** (once per boot for 135 MS/s):

```bash
sudo sh -c 'echo 1000 > /sys/module/usbcore/parameters/usbfs_memory_mb'
```

For 32 MS/s, 256 MiB is plenty. To make this persistent, drop a
`tmpfiles.d` snippet:

```bash
echo 'w /sys/module/usbcore/parameters/usbfs_memory_mb - - - - 256' \
  | sudo tee /usr/lib/tmpfiles.d/rx888.conf
sudo systemd-tmpfiles --create /usr/lib/tmpfiles.d/rx888.conf
```

(`/etc/modprobe.d/usbcore.conf` does not work on Ubuntu — `usbcore` is
built into the kernel, not a module.)

**2. Fetch firmware:**

```bash
make firmware
```

**3. Stream, decimate, record:**

```bash
rx888_stream -f firmware/SDDC_FX3.img -s 135000000 \
  | rx888_dsp --block-on-full \
  | iqrecord /data/capture --freq 7100000 --desc "7.1 MHz capture"
```

**4. Stream to GQRX via FIFO:**

```bash
mkfifo /tmp/iq.fifo
rx888_stream -f firmware/SDDC_FX3.img -s 135000000 \
  | rx888_dsp -o /tmp/iq.fifo
```

Configure GQRX to read from `/tmp/iq.fifo` (complex float32, 33.75 MHz).

---

## Tests

```bash
make check         # non-hardware: librx888 ABI + CLI smoke. Run on every PR.
make hw-check      # hardware: throughput, stop/start cycles, sample sanity.
                   # Requires RX888 + RX888_HW_TEST=1.
```

Test scripts live under `tests/`. See `doc/rx888_stream_testplan.md`.

---

## Programs

| Program | Purpose | Input | Output |
|---------|---------|-------|--------|
| `rx888_stream` | USB3 bulk capture (CLI over librx888) | RX888 device | int16 real samples on stdout |
| `rx888_dsp`    | DSP decimation (4:1) | int16 real on stdin | cf32 IQ on stdout or FIFO |
| `iqrecord`     | SigMF file recorder | cf32 IQ on stdin | `.sigmf-data` + `.sigmf-meta` files |
| `fx3_cmd`      | Vendor-command diagnostics CLI | RX888 device | `PASS`/`FAIL` + details |

Per-program docs in `doc/`:

- `doc/librx888.md` — library API and threading model
- `doc/rx888_stream.md` — CLI options and pipeline examples
- `doc/rx888_dsp.md` / `doc/rx888_dsp_arch.md`
- `doc/iqrecord.md`
- `doc/fx3_cmd.md` — vendor-command diagnostics CLI
- `doc/gr-rx888/` — design plan for the GNU Radio source block

### fx3_cmd — diagnostics CLI

A bench/operator tool that sends individual SDDC_FX3 vendor commands and
reports `PASS`/`FAIL`. Useful for bringing up hardware, poking registers, and
recovering a wedged device. Built on a shared FX3 host core
(`fx3_core`/`fx3_usb`/`fx3_stats`) imported from the
[rx888-firmware](https://github.com/ringof/rx888-firmware) test harness; the
firmware regression/fuzz/soak scenarios stay in that repo.

```sh
make fx3_cmd                                   # build (needs libusb-1.0-0-dev)

./fx3_cmd test                                 # probe device info (TESTFX3)
./fx3_cmd gpio 0x20                            # write GPIO register (0x20 = SHDWN; see include/rx888.h)
./fx3_cmd adc 64000000                         # set ADC clock to 64 MHz
./fx3_cmd att 15                               # DAT-31 attenuator (0-63)
./fx3_cmd vga 128                              # AD8370 VGA gain (0-255)
./fx3_cmd start / stop                         # GPIF streaming on/off
./fx3_cmd i2cr 0xC0 0 1                         # I2C read (Si5351 status)
./fx3_cmd stats                                # GETSTATS diagnostic counters
./fx3_cmd stats_pll                            # verify Si5351 PLL lock
./fx3_cmd reset                                # reboot FX3 to bootloader
./fx3_cmd usbreset                             # host-side USB port reset
./fx3_cmd -F SDDC_FX3.img reload               # reset → re-upload → verify
./fx3_cmd debug                                # interactive console (! for local cmds)
```

`load`/`reload`/`-F` upload firmware via `rx888_stream`, found alongside the
binary or on `PATH`. Run `./fx3_cmd` with no arguments for the full command
list.

`fx3_cmd` shares the wire-protocol constants in `include/rx888.h` with
`librx888` — there is no separate protocol header. The GPIO bit map in
`rx888.h` tracks the firmware's `protocol.h` (the authority for the device
this toolset loads): the firmware exposes only `LED_BLUE`, at bit 11 — unlike
the KA9Q-radio map, which places `LED_YELLOW`/`LED_RED`/`LED_BLUE` at bits
10/11/12. See [`doc/fx3_cmd.md`](doc/fx3_cmd.md) for the full command reference
and the firmware-upload model.

---

## Project Layout

```
rx888_tools/
├── Makefile                      Build, install, firmware, tests
├── src/
│   ├── librx888.c                Streaming engine (libusb + threading)
│   ├── ezusb.c                   FX3 firmware upload (vendored, GPL-2.0+)
│   ├── rx888_stream.c            Thin CLI over librx888 (~200 lines)
│   ├── rx888_dsp.c               AVX2/FMA DSP pipeline
│   ├── iqrecord.c                SigMF recorder
│   └── fx3_cmd/                  Vendor-command diagnostics CLI
│       ├── fx3_cmd.c             Diagnostics CLI (dispatch + debug console)
│       ├── fx3_core.{c,h}        Shared diagnostic primitives
│       ├── fx3_usb.{c,h}         USB transport / device lifecycle
│       └── fx3_stats.{c,h}       GETSTATS decoder
├── include/
│   ├── librx888.h                Public library API
│   ├── rx888.h                   FX3 protocol constants
│   └── ezusb.h                   EZ-USB API
├── firmware/
│   ├── VERSION                   Pinned rx888-firmware tag
│   ├── SHA256SUMS                Expected checksum
│   └── (SDDC_FX3.img)            Fetched, never committed
├── tests/                        DSP/iqrecord scripts + librx888 + hw tests
├── doc/                          Documentation
├── udev/                         udev rule for non-root device access
└── .github/workflows/            CI + auto-firmware-bump
```

---

## License

MIT for the original tools — see [LICENSE](LICENSE).

`src/ezusb.c` and `include/ezusb.h` are GPL-2.0-or-later (derived from the
Linux kernel `fxload` utility). Because `librx888.so` links them, the
library and `rx888_stream` binary are distributed under GPL-3.0-or-later
(compatible with GPL-2.0-or-later upstream and with GNU Radio downstream).
`rx888_dsp` and `iqrecord` are MIT only.

`fx3_cmd` (`src/fx3_cmd/`) is MIT. It links only libusb and does **not**
link `ezusb`/`librx888`; firmware upload is done by spawning the installed
`rx888_stream` binary, so no GPL code is linked into it.
