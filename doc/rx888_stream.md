# rx888_stream

CLI front-end that streams **int16 real** samples from the RX888 /
RX888mk2 to stdout.  All the libusb / threading / descriptor logic
lives in [librx888](librx888.md); this binary is a thin wrapper
(~200 lines) that parses options, opens the device, and writes the
sample callback to stdout.

```
rx888_stream [OPTIONS] | rx888_dsp | mbuffer | iqrecord ...
```

---

## Build

```sh
make rx888_stream    # depends on librx888.so
```

The binary uses `$ORIGIN` rpath so it finds the in-tree `librx888.so`
without `LD_LIBRARY_PATH`.  After `make install`, `librx888.so` lives
in `$LIBDIR` (default `/usr/local/lib`).

### udev rule (recommended)

```sh
sudo cp udev/99-rx888.rules /etc/udev/rules.d/
sudo udevadm control --reload && sudo udevadm trigger
```

Unplug and replug the RX888.

---

## USB buffer memory

At 135 MS/s the device produces ~257 MiB/s.  Bump
`usbfs_memory_mb` accordingly:

```sh
sudo sh -c 'echo 1000 > /sys/module/usbcore/parameters/usbfs_memory_mb'
```

For 32 MS/s, 256 MiB is plenty.  See the README for the persistent
`tmpfiles.d` snippet.

---

## Usage

```sh
rx888_stream [OPTIONS] > iq.raw
```

| Flag | Meaning | Default |
|------|---------|---------|
| `-f, --firmware <file>`     | Upload firmware if device in boot mode | (none) |
| `-v, --verbose`             | Print stats once per second to stderr  | off |
| `-s, --samplerate <Hz>`     | Sample rate (32M or 135M)              | 32000000 |
| `-d, --dither`              | Enable ADC dither GPIO                 | off |
| `-r, --rand`                | Enable randomizer GPIO + sample fixup  | off |
| `--fixup`                   | Enable sample fixup independently of -r| off |
| `-m, --gainmode <low\|high>`| AD8340 gain range                      | high |
| `-g, --gain <0..127>`       | AD8340 VGA code                        | 0 |
| `-a, --att <0..63>`         | DAT-31 attenuator (half-dB steps)      | 0 |
| `-q, --queuedepth <N>`      | Concurrent USB transfers               | 32 |
| `-p, --reqsize <N>`         | Transfer size in packets               | 1024 |
| `--ctrl-timeout <ms>`       | Vendor control-transfer timeout        | 5000 |
| `--stream-timeout <ms>`     | Bulk transfer timeout (0 = infinite)   | 0 |
| `--watchdog-timeout <ms>`   | No-data watchdog (0 disables)          | 3000 |
| `-h, --help`                | Show help and exit                     | — |

### Output format

Little-endian signed `int16`, real samples on stdout.  At 135 MS/s
that's ~257 MiB/s.

If stdout is a TTY, the program prints an error and exits with status 2.
Redirect to a file or pipe.

### Signals

- `SIGINT`, `SIGTERM` — clean shutdown (cancel transfers, release USB,
  exit).
- `SIGPIPE` — ignored; broken pipe is detected via `write()` and
  triggers the same shutdown path.

### Exit status

| Code | Meaning |
|------|---------|
| 0    | Clean exit (signal, EOF, or broken pipe) |
| 1    | Runtime error (device not found, USB failure) |
| 2    | Usage error (bad flags, TTY stdout) |

### Verbose stats

`-v` prints one line per second to stderr:

```
t=5s ok=1290 bad=0 in_flight=31 out=1290.00 MiB (257.49 MiB/s)
```

The numbers come from `rx888_get_stats()`.

---

## Examples

### 32 MS/s smoke capture

```sh
./rx888_stream -f firmware/SDDC_FX3.img -s 32000000 \
  | head -c $((128*1024*1024)) > /tmp/cap.s16
```

### Recommended 135 MS/s pipeline

```sh
./rx888_stream -f firmware/SDDC_FX3.img -v -s 135000000 -q 32 -p 1024 \
  | rx888_dsp --block-on-full \
  | mbuffer -m 4G -q \
  | iqrecord captures/session --freq 7100000 --desc "40m band"
```

### Quick sample sanity (no antenna / 50-Ω terminator)

```sh
./rx888_stream -f firmware/SDDC_FX3.img -s 32000000 \
  | head -c $((64*1024*1024)) \
  | python3 -c "
import sys, numpy as np
x = np.frombuffer(sys.stdin.buffer.read(), dtype='<i2')
print(f'samples={len(x)} min={x.min()} max={x.max()} std={x.std():.1f}')
"
```

The bundled `tests/hw_sample_check.py` does this automatically with
hard-coded thresholds.

---

## Validation

| Phase                 | How                       | Hardware? |
|-----------------------|---------------------------|-----------|
| ABI / argparse / errors | `make check`            | no  |
| Throughput / stop-start / sample sanity | `make hw-check` | yes |

See [`doc/rx888_stream_testplan.md`](rx888_stream_testplan.md) for the
full plan.

---

## Notes

- No buffering happens outside librx888's transfer queue and the
  writer thread.  Use `mbuffer` downstream to absorb scheduler jitter
  at high sample rates.
- librx888 will not recover automatically from
  `LIBUSB_TRANSFER_NO_DEVICE` or watchdog timeout — both terminate
  the stream.  See
  [`doc/gr-rx888/stream-division-of-recovery.md`](gr-rx888/stream-division-of-recovery.md)
  for the planned recovery layer.

---

## License

GPL-3.0-or-later, inherited from `librx888.so` (which links the
GPL-2.0-or-later `ezusb.c`).  See `LICENSE` and the headers in each
source file.
