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

**Native vs. the container — when to switch.** Host bring-up and debugging
(firmware load, GPSDO config, mounting the SSD, dialing the attenuator, quick
sanity grabs) is simplest **native** — fewer layers, fast iteration. Move into
the **container** for the actual measurement runs and analysis, and for anything
you share: the image pins the toolchain (tools + numpy + gnuplot) so captures
and `tone_quality.py` results reproduce bit-for-bit elsewhere. Firmware load
stays a host step (like the timebase) — load it natively, then the container
captures the already-running device. Rule of thumb: **native to get first light
dialed in; container once you're collecting data you intend to keep or publish.**

### 3a. Host prerequisites (power, USB permissions, SSD, usbfs)

These are the host-side gotchas that bite first. Do them once per Pi.

**Power — do this first; the RX888 is hungry.** The RX888 mk2 draws ~0.7–1 A,
and the **Pi 5 caps total USB current at 600 mA unless it detects a 5 A PSU**.
Exceed it and over-current protection cuts the port: the FX3 browns out *mid
firmware-boot*, the half-loaded image never runs, and it reverts to DFU
(`04b4:00f3`) — looking exactly like a firmware or driver failure when it's
really power.

- **Best fix: power the RX888 from a self-powered USB 3.0 hub** (its current
  then comes from the hub, not the Pi). Use a USB 3.0 port + cable — USB 2.0
  also can't carry the ~259 MB/s stream. Keep the GPSDO off the RX888's supply.
- If running straight off the Pi: use the official **27 W (5 A) PSU** and add
  `usb_max_current_enable=1` to `/boot/firmware/config.txt`, then reboot.
- Diagnose: `dmesg | grep -i over-current` (any hits = power), and
  `vcgencmd get_throttled` (`0x0` = OK; nonzero = under-voltage/over-current).

**USB permissions (udev)** — so the device is accessible without sudo and from
the container:

```sh
sudo cp udev/99-rx888.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
# then unplug/replug the RX888 so the rule applies to a fresh enumeration
ls -l /dev/bus/usb/*/*           # the FX3 node should be 0666 / group plugdev
```

The rule sets `MODE:="0666"` for the FX3 boot (`00f3`) and app (`00f1`) PIDs
(and a few PMODE variants).

**SSD for captures** — keep captures off the SD card (raw is ~930 GB/hr;
decimated `--iqlog` is ~1.5 GB/hr):

```sh
lsblk -f                                         # find the SSD (nvme0n1p1 / sda1)
sudo mkdir -p /mnt/ssd && sudo mount /dev/nvme0n1p1 /mnt/ssd
sudo mkdir -p /mnt/ssd/rx888 && sudo chown "$USER":"$USER" /mnt/ssd/rx888
df -h /mnt/ssd                                   # confirm space
```

To make the mount permanent, add an `/etc/fstab` line by UUID (from `lsblk -f`):
```
UUID=<your-uuid>  /mnt/ssd  ext4  defaults,nofail  0  2
```

Gate each session with `./scripts/ssd-preflight.sh` (or
`CAPTURE_DIR=… MIN_FREE_GB=… ./scripts/ssd-preflight.sh`) — it verifies the dir
is on the **mounted SSD** (not the SD card), writable, and has room, so a forgot-
to-mount never silently fills the SD card mid-run.

**usbfs buffer size** — high-rate streaming keeps `queue_depth × reqsize` of
transfers in flight (default 32 × 1 MB = 32 MB), which exceeds usbfs's default
16 MB limit → `ENOMEM`/`NO_DEVICE`-style failures under load. Raise it. Note
`usbcore` is **built into the Pi kernel, not a module**, so
`/etc/modprobe.d/options usbcore …` is silently ignored — set it on the **kernel
command line**:

```sh
cat /sys/module/usbcore/parameters/usbfs_memory_mb     # current value
sudo cp /boot/firmware/cmdline.txt /boot/firmware/cmdline.txt.bak
# append to the END of the single line (replace 1000 with your tested value):
sudo sed -i 's/$/ usbcore.usbfs_memory_mb=1000/' /boot/firmware/cmdline.txt
cat /boot/firmware/cmdline.txt && wc -l /boot/firmware/cmdline.txt   # MUST stay one line
sudo reboot
# after reboot, verify it took:
cat /sys/module/usbcore/parameters/usbfs_memory_mb
```

`cmdline.txt` must remain a single line — a stray newline breaks boot (restore
the `.bak` if so).

**GPSDO config + preflight (LBE-142x).** Configure the GPSDO once with the
`lbe-142x` tool ([ringof](https://github.com/ringof/lbe-142x) /
[bvernoux](https://github.com/bvernoux/lbe-142x)):

```sh
lbe-142x --f2 27000000     # OUT2 = 27 MHz coherent tone (saved to flash)
lbe-142x --pps 1           # OUT1 = 1PPS (for pps-gpio)
lbe-142x --nmea 1          # NMEA on (for chrony's coarse second)
```

Run **gpsd read-only** so it never reconfigures the GPSDO's internal GNSS — that
chipset belongs to the disciplining loop, and gpsd's auto-probing both spews
`$GNTXT` and risks disturbing the discipline. In `/etc/default/gpsd`:
```
DEVICES="/dev/ttyACM0"
GPSD_OPTIONS="-n -b"
```
(`-b` = read-only; confirm `gpspipe -r` shows `"readonly":"true"`.)

Then **gate every capture session** with the preflight (PASS/FAIL on GPS+PLL
lock, antenna OK, OUT1=1PPS, OUT2=27 MHz, NMEA enabled, and a live NMEA fix):
```sh
export LBE142X=/path/to/lbe-142x/build/bin/lbe-142x   # or put it on PATH
./scripts/gpsdo-preflight.sh
```

### 3b. Host timebase (once, for the timing test)

```sh
sudo ./scripts/host-timebase-setup.sh            # dry run: prints intended changes
sudo ./scripts/host-timebase-setup.sh --apply    # installs chrony+pps-tools, enables pps-gpio
sudo reboot
ls -l /dev/pps0 && sudo ppstest /dev/pps0        # verify PPS asserts
chronyc sources -v && chronyc tracking           # verify GPSDO lock
```

Captures go to the NVMe, not the SD card (`--iqlog` is ~1.5 GB/hr at the default
decim 2400). See `doc/pps_timing.md` for the two-tier timing method.

### 3c. Capture with the RX888 (USB passthrough)

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
- The pinned-release firmware is baked into the image (`make firmware` at
  build), so `-f firmware/SDDC_FX3.img` resolves inside the container. If the
  device is already in app mode (`04b4:00f1`, EEPROM-flashed), `-f` is harmless.
- **To use your own firmware build**, mount it over the baked one:
  `-v "$PWD/firmware/SDDC_FX3.img:/opt/rx888-tools/firmware/SDDC_FX3.img:ro"`.
- If firmware upload fails with `NO_DEVICE` and the device stays `00f3`, it's
  almost always **power** (over-current), not Docker — see 3a. Fix power first.
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
