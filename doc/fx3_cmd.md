# fx3_cmd

Low-level **vendor-command diagnostics** for the RX888 / RX888mk2. `fx3_cmd`
sends individual FX3 vendor commands to the device, reports `PASS`/`FAIL`, and
returns exit status `0` on PASS / `1` on FAIL. It is a bench/bring-up tool —
probe the device, poke registers, read firmware counters, and recover a wedged
device. It is **not** part of the streaming data path; `rx888_stream` /
`librx888` own that.

```
fx3_cmd [-F firmware.img] <command> [args...]
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
| `stats_shdn` | `GETSTATS` | Verify SHDN is asserted after `STOPFX3` |
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
- Usage error (no command / bad arg count) exits `2`.

This makes `fx3_cmd` usable as a test predicate in shell scripts and CI.

---

## Protocol constants

`fx3_cmd` shares [`include/rx888.h`](../include/rx888.h) with `librx888` —
there is no separate protocol header. Its constants (command IDs, SETARG IDs,
and the GPIO bit map) track the firmware's `protocol.h`, so the `gpio` command
and the `GETSTATS` `gpio_state` readback use the same bit positions the
firmware does — including `LED_BLUE` at bit 11.

---

## Tests

`make check` builds `fx3_cmd` and runs `tests/fx3_cmd_smoke.sh` — a
no-hardware CLI smoke test (usage lists every command, `-h` exits 0, no-arg
exits 2, and each command fails cleanly with exit 1 when no device is present).
Hardware exercise of the actual vendor commands is out of scope for the
in-repo tests; that lives in the `rx888-firmware` harness.
