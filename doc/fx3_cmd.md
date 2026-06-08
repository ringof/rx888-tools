# fx3_cmd

Low-level **vendor-command diagnostics** for the RX888 / RX888mk2. `fx3_cmd`
sends individual FX3 vendor commands to the device, reports `PASS`/`FAIL`, and
returns exit status `0` on PASS / `1` on FAIL. It is a bench/bring-up tool —
probe the device, poke registers, read firmware counters, and recover a wedged
device. It is **not** part of the streaming data path; `rx888_stream` /
`librx888` own that.

```
fx3_cmd [-F firmware.img] [--no-claim | --force] <command> [args...]
```

Unlike `rx888_stream`, `fx3_cmd` does **not** link `librx888`. It talks to
libusb directly and shares only the wire-protocol constants in
[`include/rx888.h`](../include/rx888.h). The transport/diagnostic core
(`fx3_core` / `fx3_usb` / `fx3_stats`) is imported from the
[`rx888-firmware`](https://github.com/ringof/rx888-firmware) test harness; the
firmware regression/fuzz/soak scenarios stay in that repo.

---

## Build

```sh
make fx3_cmd          # needs libusb-1.0-0-dev
```

Built at the repo root next to `rx888_stream` (and installed into the same
`BINDIR` by `make install`), because firmware upload shells out to
`rx888_stream` — see [Firmware upload](#firmware-upload).

---

## Commands

| Command | Vendor request | Purpose |
|---------|----------------|---------|
| `test` | `TESTFX3` | Read the firmware device-info word (hwconfig, fw version, request count) |
| `gpio <bits>` | `GPIOFX3` | Write the 32-bit GPIO register (hex or decimal) |
| `adc <hz>` | `STARTADC` | Program the ADC sample clock |
| `att <0-63>` | `SETARGFX3`/`DAT31_ATT` | DAT-31 attenuator (half-dB steps) |
| `vga <0-255>` | `SETARGFX3`/`AD8370_VGA` | AD8370 VGA gain code |
| `wdg_max <0-255>` | `SETARGFX3`/`WDG_MAX_RECOV` | Watchdog max recovery count (0 = unlimited) |
| `start` / `stop` | `STARTFX3` / `STOPFX3` | Start / stop the GPIF streaming engine |
| `i2cr <addr> <reg> <len>` | `I2CRFX3` | I2C read (hex) |
| `i2cw <addr> <reg> <byte>...` | `I2CWFX3` | I2C write (hex) |
| `stats` | `GETSTATS` | Dump the firmware diagnostic counters |
| `stats_pll` | `GETSTATS` | Verify the Si5351 PLL is locked |
| `stats_shdn` | `GETSTATS` | Verify SHDN is asserted after `STOPFX3` (needs the 30-byte `GETSTATS`) |
| `stack_check` | `READINFODEBUG` | Query the firmware stack high-water mark |
| `raw <code>` | (arbitrary) | Send a raw vendor request code; expects a STALL for removed commands |
| `reset` | `RESETFX3` | Reboot the FX3 back to bootloader |
| `usbreset` | — | Host-side USB port reset (`USBDEVFS_RESET`); recovers a device too wedged for libusb to claim |
| `debug` | `TESTFX3`/`READINFODEBUG` | Interactive console; `!` switches to local commands, Ctrl-C exits |
| `load <img>` | — | Upload firmware and exit |
| `reload [img]` | `RESETFX3` + `usbreset` | Reset to bootloader, re-upload firmware, verify healthy |

Run `fx3_cmd` with no arguments (or `-h`) for the built-in command list.

### Examples

```sh
fx3_cmd test                         # probe device info
fx3_cmd gpio 0x20                    # write GPIO register (0x20 = SHDWN)
fx3_cmd adc 64000000                 # ADC clock to 64 MHz
fx3_cmd att 15                       # DAT-31 attenuator
fx3_cmd vga 128                      # AD8370 VGA gain
fx3_cmd i2cr 0xC0 0 1                # I2C read (Si5351 status)
fx3_cmd stats                        # diagnostic counters
fx3_cmd stats_pll                    # Si5351 PLL lock check
fx3_cmd reset                        # reboot to bootloader
fx3_cmd usbreset                     # host-side USB port reset
fx3_cmd -F SDDC_FX3.img reload       # reset -> re-upload -> verify
fx3_cmd debug                        # interactive console
```

---

## Running alongside a streamer (exclusive access)

By default `fx3_cmd` **claims USB interface 0** — the same exclusive claim a
streamer (`rx888_stream` / `librx888`, or ka9q-radio's `rx888d`) needs for bulk
transfers. The RX888 has a single bulk interface, so **the two cannot share it**:

- If a streamer is already running, every normal `fx3_cmd` command fails to
  claim the interface. It retries for ~2 s, then prints
  `error: claim interface: Resource busy` and exits `1`.
- Even if it could claim, `fx3_cmd` restarts the EP1-IN endpoint ring and
  issues device-reconfiguring vendor commands — it is meant to have the device
  to itself. Stop the streamer first.

Two commands deliberately bypass the claim and therefore **work while a streamer
is running — but they are destructive**:

- `usbreset` — a raw `USBDEVFS_RESET` on the device node (no claim). It resets
  the USB port, which reboots the FX3 and **kills the active stream** (and drops
  the device to the bootloader, needing a re-flash).
- `reload` — does the same reset, then re-uploads firmware.

These exist to recover a *wedged* device, not to coexist with a healthy stream.

### Why this works — the Linux USB claiming model

A common assumption is that opening a USB device is exclusive. On Linux it
isn't. A device is a usbfs node (`/dev/bus/usb/BBB/DDD`) with three independent
access layers, and **only the middle one is exclusive**:

1. **Open** (`libusb_open`) — *not* exclusive. Multiple processes can hold the
   same device open at once; opening claims nothing.
2. **Claim an interface** (`libusb_claim_interface` → `USBDEVFS_CLAIMINTERFACE`)
   — exclusive, one owner per interface. A second claim returns
   `LIBUSB_ERROR_BUSY` (the `Resource busy` above).
3. **Endpoint I/O** — bulk/interrupt/isochronous transfers require the claim on
   the interface that owns the endpoint. Streaming is bulk-IN on EP1, hence the
   claim.

**Endpoint 0 sits outside all of this and cannot be claimed.** EP0 is the
device's default *control* pipe, defined by the device descriptor — it is not
listed in any interface descriptor, so there is no ioctl to take it. It is
shared by every open handle, always. When the streamer claims interface 0 it
gets interface 0's bulk endpoint exclusively; **EP0 was never — and cannot be —
exclusively held by anyone.**

The RX888's entire command set is vendor control transfers addressed to the
*device* (recipient = device), which usbfs permits without consulting any
interface claim. So `--no-claim` just opens the device (allowed), skips the
claim, and pokes EP0 — concurrently with the streamer's bulk transfers on EP1,
which it never touches. (Everyday proof that EP0 is shared: `lsusb -v` reads
descriptors over EP0 from devices that already have drivers bound and interfaces
claimed.)

This is a property of *every* USB device, not just the RX888 — EP0 is mandatory
and is how enumeration happens before any driver or claim exists. What is
RX888-specific is only that its firmware (a) implements useful vendor commands
on EP0 and (b) services them on a thread separate from the GPIF/DMA stream, so
concurrent EP0 traffic doesn't disturb streaming — see the firmware concurrency
contract, [`rx888-firmware#170`](https://github.com/ringof/rx888-firmware/issues/170).

### `--no-claim` — using stream-safe commands during a stream

`--no-claim` opens the device *without* claiming interface 0 or touching the
bulk endpoint, so its EP0 vendor commands run while another process streams with
interface 0 claimed (see *Why this works* above). Per the firmware contract,
`--no-claim` allows the **stream-safe** EP0 commands:

| Command(s) | Vendor request | Why it's safe |
|---|---|---|
| `test` | `TESTFX3` | read-only device info |
| `stats`, `stats_pll` | `GETSTATS` | reads counters (non-coherent snapshot) |
| `stack_check` | `TESTFX3` + `READINFODEBUG` | debug-log read, negligible interrupt |
| `att`, `vga`, `wdg_max` | `SETARGFX3` | live gain/attenuator tuning; no stream impact |

```sh
# ka9q-radio (or rx888_stream) is streaming; peek and tune live:
fx3_cmd --no-claim stats
fx3_cmd --no-claim stats_pll
fx3_cmd --no-claim att 20        # adjust the attenuator without stopping the stream
```

Reads are a non-coherent snapshot taken while the device is busy — treat them as
monitoring, not ground truth.

### `--force` — stream-unsafe commands and the full debug console

The remaining commands are **stream-unsafe**: per the firmware contract,
`GPIOFX3` (`gpio`), `STARTADC` (`adc`), `STARTFX3`/`STOPFX3` (`start`/`stop`,
and `stats_shdn` which issues them), and `RESETFX3` (`reset`) stop or disrupt
the GPIF/DMA stream. Under `--no-claim` they are rejected (exit `2`) unless you
use **`--force`**, which **implies `--no-claim`** (you don't need both — you
never claim the interface to disrupt a stream; the disruption is via EP0 vendor
commands either way):

```sh
fx3_cmd --no-claim gpio 0x20      # error: not stream-safe; re-run with --force
fx3_cmd --force gpio 0x20         # runs it; warns that it may disrupt the stream
fx3_cmd --force debug             # full interactive console during a stream
```

`--force` enables the **full `debug` console** (including its `!stop` / `!reset`
/ `!gpio` / `!adc` local commands) alongside a running stream. Everything still
goes over EP0 — no interface claim — so the firmware will not crash; the
consequence you are accepting is that these commands **glitch, stop, or reset
the device** out from under the streamer.

`load`, `usbreset`, and `reload` are not part of `--no-claim`/`--force` (they
have their own non-claiming or recovery paths — run them on their own).

---

## Firmware upload

`load`, `reload`, and the `-F <img>` option upload firmware by **spawning the
`rx888_stream` binary** (which links `ezusb`), rather than linking the
GPL-licensed uploader into `fx3_cmd` itself. `fx3_cmd` looks for
`rx888_stream`:

1. in the same directory as the `fx3_cmd` binary,
2. then on `PATH`.

After `make install` both binaries live in the same `BINDIR`, so this resolves
automatically. From a build tree, run `fx3_cmd` from the repo root (where
`rx888_stream` is built) or put it on `PATH`.

`-F` only uploads when the device is found in **bootloader** mode (PID
`0x00F3`); if the application firmware (PID `0x00F1`) is already running it is
left alone.

---

## Exit status & output

- `PASS <command> [details]` on success, exit `0`.
- `FAIL <command> <reason>` on failure, exit `1`.
- Usage error (unknown command, wrong argument count, or a stream-unsafe
  command under `--no-claim` without `--force`) exits `2`.

The command name and argument count are validated **before** the device is
opened, so a typo or wrong arity is always a `2` — independent of whether a
device is attached or a streamer holds it. This makes `fx3_cmd` usable as a
test predicate in shell scripts and CI.

---

## Protocol constants

`fx3_cmd` shares [`include/rx888.h`](../include/rx888.h) with `librx888` —
there is no separate protocol header. Its constants (command IDs, SETARG IDs,
and the GPIO bit map) track the firmware's `protocol.h`, so the `gpio` command
and the `GETSTATS` `gpio_state` readback use the same bit positions the
firmware does — including `LED_BLUE` at bit 11.

### GETSTATS firmware compatibility

The `GETSTATS` payload grew from **26 to 30 bytes** when the firmware added the
`gpio_state` field (#131). `fx3_cmd` reads whichever length the firmware
returns: the first 26 bytes (DMA/GPIF/PIB/I2C/fault counters, Si5351 status,
boot count, CLK0 state) decode on every version, so `stats` and `stats_pll`
work against older firmware too. `stats` prints `gpio=n/a` and `stats_shdn`
reports that it needs the 30-byte payload when running on firmware that predates
`gpio_state`. (A short or truncated reply is reported distinctly from a USB I/O
error.)

---

## Tests

`make check` builds `fx3_cmd` and runs `tests/fx3_cmd_smoke.sh` — a
no-hardware CLI smoke test (usage lists every command, `-h` exits 0, no-arg
exits 2, and each command fails cleanly with exit 1 when no device is present).

`make hw-check` (with `RX888_HW_TEST=1` and a device attached) additionally
runs `tests/hw_fx3_cmd.sh`, which exercises the real vendor commands and the
`--no-claim` concurrency path:

- **load + probe** (`-F … test`).
- **idle diagnostics + config pokes:** `stats`/`stats_pll`, read-only
  `i2cr` (Si5351 status), `adc`/`att`/`vga`, a `start`/`stop` pair, and the
  `stats_shdn` graceful-degradation path on legacy 26-byte `GETSTATS` firmware.
- **exclusive-access guard:** a normal command is refused (`Resource busy`,
  exit 1) while `rx888_stream` holds interface 0.
- **`--no-claim` concurrency:** stream-safe commands succeed alongside the
  stream — including live `att`/`vga` tuning — with `GETSTATS` `dma_count`
  advancing between snapshots while `boot_count` stays put; a stream-unsafe
  command (`gpio`) is refused without `--force`; and `--no-claim stack_check`
  (the debug/EP0 channel) does not stall the stream — early evidence for the
  observe-only-debug
  question in issue #27.

It is non-destructive (leaves the device loaded and idle); `usbreset`/`reload`
are destructive and remain manual. Broader firmware regression/fuzz/soak
coverage lives in the `rx888-firmware` harness.
