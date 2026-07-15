# rx888_ppm

**Online ppm trim of the RX888 sample clock.** A small Python tool that bends
the ADC sample clock by a few parts-per-million *while the receiver is
streaming*, so you can walk a known carrier onto its mark by hand — no GPSDO,
no restart, no rebuild.

```
scripts/rx888_ppm.py [global opts] <command> [args...]
```

It is a control-plane companion to [`fx3_cmd`](fx3_cmd.md): it does not link
`librx888` and is not in the data path. It shells out to `fx3_cmd` for the
actual USB I2C transfers.

---

## How it works (and why it's glitch-free)

The RX888 synthesizes its ADC sample clock in an Si5351A — or the MS5351M
clone — from the board reference. That part is a two-stage synthesizer:

```
reference → fractional-N PLL (VCO 600–900 MHz) → output Multisynth divider → CLK
```

`rx888_ppm` perturbs **only the PLL-A feedback fractional divider** (MSNA,
registers 26–33) and **never issues a PLL soft reset** (register 177). The PLL
is an analog loop, so a small change to the fractional term `b/c` slews the VCO
*continuously* to the new frequency. The output Multisynth divider is left
untouched, so its counter never reloads and no output edge is added or dropped.
A tiny phase perturbation while the VCO settles is harmless to a sampling clock;
a lost or extra edge would not be — and this path cannot produce one. This is
the same technique QRP Labs radios use to change frequency (and key clean FSK)
without stopping their Si5351 clock.

Because the correction moves the *actual* clock rather than relabeling the
sample rate in software, nulling **one** reference calibrates the **whole**
span: every tuned frequency shifts with it, at the true nominal sample rate.

### Coexisting with a running stream

The tool invokes `fx3_cmd --force`, which implies `--no-claim`: the I2C writes
go over the EP0 control pipe without claiming interface 0, so they run
alongside `rx888_stream` / `rx888d`. `fx3_cmd` conservatively marks the generic
`i2cw`/`i2cr` as stream-unsafe; `rx888_ppm` is the classified-safe wrapper —
it writes only MSNA fractional bytes (regs 31–33, or 28–33 on a carry tick) and
never the reset. Registers 26–33 sit outside the firmware's reserved-register
fence, so nothing here is blocked.

If no streamer is running you can pass `--claim` to use the normal exclusive
open instead.

---

## Build / requirements

Nothing to compile — it's a stock-Python-3 script. It needs `fx3_cmd` on your
`PATH` (or point at it):

```sh
make fx3_cmd
scripts/rx888_ppm.py --fx3-cmd ./fx3_cmd read
# or: export RX888_FX3_CMD=./fx3_cmd
```

---

## Commands

| Command | Purpose |
|---------|---------|
| `read` | Show the current PLL-A `a/b/c`, the ratio `N`, and the ppb-per-LSB resolution. If a baseline is set, also the current offset from it. |
| `baseline` | Capture the current setting as the 0 ppm reference (written to a small state file). |
| `nudge <±ppm>` | Bend the clock by a **relative** ppm amount. The everyday manual knob. |
| `set <±ppm>` | Set an **absolute** ppm offset relative to the captured baseline. |
| `zero` | Return to the captured baseline (0 ppm). |
| `sweep <start> <stop> <step>` | Step the trim across a range (bench characterization). |

Global options: `--fx3-cmd PATH`, `--claim`, `--state PATH`, `--max-ppm N`
(refuse wilder trims, default 200), `--dry-run`, `-v`. `nudge`/`set`/`zero`
accept `--check-lock` to verify PLL lock (`fx3_cmd stats_pll`) after the write;
`sweep` accepts `--check-lock` (poll after every step, fail on any drop) and
`--restore`.

### Examples

```sh
scripts/rx888_ppm.py read                  # inspect the current setting
scripts/rx888_ppm.py baseline              # mark "here" as 0 ppm
scripts/rx888_ppm.py nudge +0.05           # bend up 0.05 ppm, live
scripts/rx888_ppm.py nudge -0.05           # bend back down
scripts/rx888_ppm.py set 1.20              # go to +1.20 ppm vs baseline
scripts/rx888_ppm.py zero                  # back to baseline
scripts/rx888_ppm.py sweep -2 2 0.25 --check-lock   # characterization run
```

---

## Verifying it really is glitch-free

The PLL-slew argument says at most a small phase wobble, never a dropped edge —
but that's worth *measuring* on your actual part before trusting it, because a
few Si5351 libraries reset unconditionally precisely because some transitions
aren't clean. Two independent checks:

1. **PLL stayed locked.** `sweep --check-lock` polls `fx3_cmd stats_pll` after
   every step and fails loudly if lock ever drops. Cheap, and a lost lock is an
   immediate disqualifier.

2. **No samples slipped** — the real bar for a sample clock. With a stream
   running, nudge the trim while a continuity check watches the sample count.
   The counter/verify tooling already in `tests/` (`counter_cf32.py` +
   `verify_cf32.py`, or `hw_sample_check.py`) is built for exactly this:
   a dropped or duplicated edge shows up as a discontinuity. A clean count
   across a full sweep is the pass condition.

For the lowest-level look, watch the CLK output on a counter/scope through a
single-LSB step and confirm the transition is phase-continuous.

---

## What it deliberately will not do

- Change the output Multisynth divider (regs 42–65).
- Write the PLL reset register (177) or anything outside regs 26–33.
- Make large jumps that would need a divider change or a re-lock — that is the
  firmware's ADC-clock programming job, not a trim.

The hardware-free math (register decode/encode, ppm, minimal-write planning) is
importable and covered by `tests/rx888_ppm_selftest.py`, which runs in
`make check`.
