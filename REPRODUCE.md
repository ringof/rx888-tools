# Reproducing the RX888 PPS / data-integrity proof

This kit is split so the part that needs *no hardware* is trivially
reproducible by anyone, and the part that needs the rig is one host script plus
a USB passthrough.

- **Container** — the stream + analysis tools (librx888, `pps_integrity`,
  `stream_soak`, `tone_monitor`) and the offline analyzers (`tone_quality.py`,
  numpy, gnuplot). Deps come from apt, so there is no Python install to manage.
- **Host** — chrony + pps-gpio, which discipline the kernel clock and load a
  kernel module. These are inherently host-level and are set up by one script.

## 1. The hardware-free proof (anyone, any machine)

Validates the whole DSP and the slip/garble discriminator from synthetic data —
runs on an arm64 Pi or an amd64 laptop, no RX888 needed.

```sh
docker build -t rx888-ppskit .
docker run --rm -v "$PWD/out:/out" rx888-ppskit
```

This runs the CLI/DSP/analyzer checks, prints the analyzer's conclusions for the
clean/drop/garble cases (clean → CLEAN, drop → one 75° grid slip, garble →
amplitude dip with no phase step), and writes the gnuplot figures to `./out/`.

Multi-arch build (so reviewers on x86 can run it too):

```sh
docker buildx build --platform linux/arm64,linux/amd64 -t rx888-ppskit .
```

## 2. Replaying real captures (bit-for-bit reproducible analysis)

The hardware is the only thing you can't ship — but the *bytes* are. A recorded
`tone_monitor --iqlog` (or a raw `.s16` capture) replays through the exact same
analyzer to the exact same numbers:

```sh
docker run --rm -v "$PWD/data:/data" rx888-ppskit \
    python3 tests/tone_quality.py /data/run.iq
```

Publish a real `run.iq` / `run.statslog` from your rig alongside the image and
others reproduce your analysis exactly, then run `make check` for the
independent synthetic cases.

## 3. Live capture on the Pi

### 3a. Host timebase (once, for the timing test)

```sh
sudo ./scripts/host-timebase-setup.sh            # dry run: prints intended changes
sudo ./scripts/host-timebase-setup.sh --apply    # installs chrony+pps-tools, enables pps-gpio
sudo reboot
ls -l /dev/pps0 && sudo ppstest /dev/pps0        # verify PPS asserts
chronyc sources -v && chronyc tracking           # verify GPSDO lock
```

Captures go to the NVMe, not the SD card (`--iqlog` is ~1.5 GB/hr at the default
decim 2400). See `doc/pps_timing.md` for the two-tier timing method.

### 3b. Capture with the RX888 (USB passthrough)

The FX3 **re-enumerates** when firmware is uploaded (boot PID `04b4:00f3` →
app `04b4:00f1`), so a fixed `--device=/dev/bus/usb/BBB/DDD` mapping breaks.
Bind-mount the whole USB tree and allow the USB major (189) so the new node is
visible inside the container:

```sh
# short test first (~7 s): confirms USB passthrough + firmware + SSD write
docker run --rm \
    -v /dev/bus/usb:/dev/bus/usb --device-cgroup-rule='c 189:* rmw' \
    -v /mnt/ssd/rx888:/data \
    rx888-ppskit \
    ./tone_monitor 0.002 -f firmware/SDDC_FX3.img \
        --iqlog /data/test.iq --statslog /data/test.csv

# real run: 0 hours = until you docker stop / SIGINT
docker run --rm \
    -v /dev/bus/usb:/dev/bus/usb --device-cgroup-rule='c 189:* rmw' \
    -v /mnt/ssd/rx888:/data \
    rx888-ppskit \
    ./tone_monitor 0 -f firmware/SDDC_FX3.img \
        --iqlog /data/run.iq --statslog /data/run.csv
```

Notes:
- The firmware blob is baked into the image (`make firmware` at build), so
  `-f firmware/SDDC_FX3.img` resolves inside the container. If the device is
  already in app mode (`04b4:00f1`, EEPROM-flashed), `-f` is harmless.
- `--device-cgroup-rule='c 189:* rmw'` grants the USB char-device major; use
  `--privileged` instead for a quick POC if the rule is fussy.
- `/mnt/ssd/rx888` is the host SSD mount (make it writable: `chown` it to you);
  `/data` is where the tools write. USB bulk runs at full rate in the container
  — the kernel does the DMA.
- Analyze from the same image: `docker run --rm -v /mnt/ssd/rx888:/data
  rx888-ppskit python3 tests/tone_quality.py /data/test.iq`.

## What is NOT in the container, and why

`chrony` and `pps-gpio` discipline the host **kernel** clock and load a kernel
module; running chrony in a container is an anti-pattern (it needs
`CAP_SYS_TIME` and fights the shared host clock). They stay on the host via
`scripts/host-timebase-setup.sh`. `rx888_dsp` is also excluded — its AVX2 path
is x86-only and not part of this kit.

## Layout

| piece | where |
|---|---|
| stream + analysis tools, numpy, gnuplot | container (`Dockerfile`) |
| no-hardware proof entrypoint | `docker/reproduce.sh` |
| host clock (chrony + pps-gpio) | `scripts/host-timebase-setup.sh` |
| method + results | `doc/tone_quality.md`, `doc/pps_timing.md`, `doc/pps_integrity.md` |
