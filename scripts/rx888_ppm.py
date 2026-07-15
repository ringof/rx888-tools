#!/usr/bin/env python3
"""rx888_ppm.py — online ppm trim of the RX888 sample clock.

The RX888 derives its ADC sample clock from an Si5351A (or the MS5351M
clone) synthesizing off the board reference. This tool nudges that clock's
frequency by a few ppm *while the receiver is running* — a manual "pitch
bend" you turn until a known carrier (WWV/WWVH/CHU, a broadcast pilot, a lab
signal generator) sits exactly on its mark.

It works by perturbing only the PLL-A **feedback fractional divider**
(MSNA, registers 26-33) and never issuing a PLL soft reset (register 177).
The PLL is an analog loop, so a small change to the fractional term slews the
VCO continuously; the output Multisynth divider is never touched, so the
output counter never reloads and no clock edge is dropped. This is the same
glitch-free technique QRP Labs radios use to keep a clean clock across live
frequency changes. Because the correction moves the *actual* clock, nulling
one reference calibrates the whole span — every tuned frequency shifts with it.

Transport is the FX3 vendor-command I2C path, reached through `fx3_cmd`. We
use `--force` (which implies `--no-claim`), so the writes go over the EP0
control pipe without claiming interface 0 and coexist with a running
`rx888_stream` / `rx888d`. `fx3_cmd` marks the generic `i2cw`/`i2cr` as
stream-unsafe out of caution; this tool is the classified-safe wrapper — it
only ever writes MSNA fractional bytes, and never the reset.

What it does NOT do: change the output Multisynth divider, pulse register 177,
or touch anything outside registers 26-33. If you need a clean re-lock or a
frequency step large enough to change the output divider, that's a job for the
firmware's ADC-clock programming, not this trim.

Examples:
    rx888_ppm.py read                 # show the current PLL-A ratio + resolution
    rx888_ppm.py baseline             # capture the current setting as 0 ppm
    rx888_ppm.py nudge +0.05          # bend up  0.05 ppm (relative)
    rx888_ppm.py nudge -0.05          # bend down 0.05 ppm
    rx888_ppm.py set 1.20             # go to +1.20 ppm relative to baseline
    rx888_ppm.py zero                 # return to the captured baseline
    rx888_ppm.py sweep -2 2 0.25 --check-lock   # bench: step the trim, verify lock

The pure math (decode/encode/apply) is importable and hardware-free; see
tests/rx888_ppm_selftest.py.
"""
import argparse
import json
import os
import re
import subprocess
import sys

# --- Si5351 / MS5351M constants -------------------------------------------
SI5351_ADDR = 0xC0          # 8-bit I2C address (0x60 << 1)
MSNA_BASE = 0x1A            # register 26: first PLL-A parameter byte
MSNA_LEN = 8               # registers 26..33
REG_P1_BASE = 0x1C         # register 28: P1 high byte (start of P1+P2 span)
REG_P2_BASE = 0x1F         # register 31: P2 high nibble (start of P2-only span)
PLL_RESET_REG = 177        # register we must NEVER write here

A_MIN, A_MAX = 15, 90      # valid PLL feedback integer range (VCO 600-900 MHz)

DEFAULT_MAX_PPM = 200.0    # guard: refuse trims wilder than this


# --- pure math: MSNA register <-> (a, b, c) --------------------------------
# VCO = f_ref * (a + b/c). Register packing per Skyworks AN619:
#   P1 = 128*a + floor(128*b/c) - 512
#   P2 = 128*b - c*floor(128*b/c)
#   P3 = c

def decode_msna(regs):
    """8 raw bytes (regs 26..33) -> (a, b, c)."""
    if len(regs) != MSNA_LEN:
        raise ValueError("expected 8 MSNA bytes, got %d" % len(regs))
    r26, r27, r28, r29, r30, r31, r32, r33 = regs
    p3 = ((r31 >> 4) << 16) | (r26 << 8) | r27
    p1 = ((r28 & 0x03) << 16) | (r29 << 8) | r30
    p2 = ((r31 & 0x0F) << 16) | (r32 << 8) | r33
    c = p3
    if c == 0:
        raise ValueError("decoded c==0 (register readback looks wrong)")
    # 0 <= floor(128*b/c) <= 127, and 128*a is a multiple of 128:
    a = (p1 + 512) // 128
    q = (p1 + 512) % 128
    b = (p2 + c * q) // 128
    return a, b, c


def encode_msna(a, b, c):
    """(a, b, c) -> 8 raw bytes (regs 26..33)."""
    if not (0 <= b < c):
        raise ValueError("b out of range 0 <= b < c (b=%d c=%d)" % (b, c))
    q = (128 * b) // c
    p1 = 128 * a + q - 512
    p2 = 128 * b - c * q
    p3 = c
    return [
        (p3 >> 8) & 0xFF,                                   # r26
        p3 & 0xFF,                                          # r27
        (p1 >> 16) & 0x03,                                  # r28
        (p1 >> 8) & 0xFF,                                   # r29
        p1 & 0xFF,                                          # r30
        (((p3 >> 16) & 0x0F) << 4) | ((p2 >> 16) & 0x0F),   # r31
        (p2 >> 8) & 0xFF,                                   # r32
        p2 & 0xFF,                                          # r33
    ]


def ratio(a, b, c):
    return a + b / c


def ppm_per_lsb(a, b, c):
    """Fractional change (in ppm) of moving b by one LSB."""
    return 1e6 / (c * ratio(a, b, c))


def apply_ppm(a, b, c, delta_ppm):
    """Return (a2, b2, applied_ppm) after bending the clock by delta_ppm.

    Keeps c fixed; moves b, carrying into a on the rare fractional wrap. The
    applied ppm is quantized to the nearest b LSB and reported so callers can
    show the real (not requested) correction.
    """
    n = ratio(a, b, c)
    delta_b = n * (delta_ppm * 1e-6) * c
    new_b = b + delta_b
    a2 = a
    while new_b >= c:
        new_b -= c
        a2 += 1
    while new_b < 0:
        new_b += c
        a2 -= 1
    b2 = int(round(new_b))
    if b2 >= c:                 # rounding pushed it onto the boundary
        b2 -= c
        a2 += 1
    applied = (ratio(a2, b2, c) / n - 1.0) * 1e6
    return a2, b2, applied


def plan_write(old_regs, new_regs):
    """Choose the minimal, reset-free register write for old->new MSNA.

    Returns (base_reg, data_bytes). Writes only what changed:
      * P2 alone  -> registers 31..33 (3 bytes) — the common small nudge
      * P1 and P2 -> registers 28..33 (6 bytes) — the occasional carry tick
    Never returns the P3 bytes (26,27) and never the reset register.
    """
    if old_regs[0:2] != new_regs[0:2]:
        # P3 (c) changed — this tool holds c fixed, so that must not happen.
        raise AssertionError("refusing to write: P3/c changed unexpectedly")
    p1_changed = old_regs[2:5] != new_regs[2:5]
    if p1_changed:
        return REG_P1_BASE, new_regs[2:8]      # regs 28..33
    return REG_P2_BASE, new_regs[5:8]          # regs 31..33


# --- transport: talk to the device through fx3_cmd -------------------------
class Fx3Error(RuntimeError):
    pass


class Device:
    def __init__(self, fx3_cmd, force=True, dry_run=False, verbose=False):
        self.fx3_cmd = fx3_cmd
        self.force = force
        self.dry_run = dry_run
        self.verbose = verbose

    def _run(self, cmd_args):
        argv = [self.fx3_cmd]
        if self.force:
            argv.append("--force")     # implies --no-claim: coexists with a stream
        argv += cmd_args
        if self.verbose:
            sys.stderr.write("+ " + " ".join(argv) + "\n")
        try:
            p = subprocess.run(argv, capture_output=True, text=True)
        except FileNotFoundError:
            raise Fx3Error(
                "could not run '%s' — build it (make fx3_cmd) or pass "
                "--fx3-cmd / set RX888_FX3_CMD" % self.fx3_cmd)
        return p

    def read_msna(self):
        p = self._run(["i2cr", hex(SI5351_ADDR), hex(MSNA_BASE), str(MSNA_LEN)])
        if p.returncode != 0:
            raise Fx3Error("i2cr failed:\n" + (p.stdout + p.stderr).strip())
        for line in p.stdout.splitlines():
            if line.startswith("PASS i2cr"):
                hexpart = line.rsplit(":", 1)[1]
                vals = [int(tok, 16) for tok in hexpart.split()]
                if len(vals) != MSNA_LEN:
                    raise Fx3Error("expected %d bytes, got %d: %r"
                                   % (MSNA_LEN, len(vals), line))
                return vals
        raise Fx3Error("could not parse i2cr output:\n" + p.stdout)

    def write_regs(self, base_reg, data):
        args = ["i2cw", hex(SI5351_ADDR), hex(base_reg)] + [hex(b) for b in data]
        if self.dry_run:
            sys.stderr.write("[dry-run] would write reg 0x%02X: %s\n"
                             % (base_reg, " ".join("%02X" % b for b in data)))
            return
        p = self._run(args)
        if p.returncode != 0:
            raise Fx3Error("i2cw failed:\n" + (p.stdout + p.stderr).strip())

    def pll_locked(self):
        """True if fx3_cmd stats_pll reports the Si5351 PLL locked."""
        p = self._run(["stats_pll"])
        return p.returncode == 0


# --- state file (baseline for absolute ppm) --------------------------------
def default_state_path():
    base = os.environ.get("XDG_STATE_HOME") or os.path.expanduser("~/.local/state")
    return os.path.join(base, "rx888-ppm.json")


def load_state(path):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def save_state(path, state):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        json.dump(state, f, indent=2, sort_keys=True)
        f.write("\n")


# --- command implementations ----------------------------------------------
def _write_new(dev, old_regs, a2, b2, c):
    new_regs = encode_msna(a2, b2, c)
    base, data = plan_write(old_regs, new_regs)
    assert base != PLL_RESET_REG and MSNA_BASE <= base <= 0x21, "unsafe target"
    dev.write_regs(base, data)
    return new_regs


def cmd_read(dev, args):
    regs = dev.read_msna()
    a, b, c = decode_msna(regs)
    print("MSNA (PLL-A) : a=%d  b=%d  c=%d" % (a, b, c))
    print("ratio N      : %.9f" % ratio(a, b, c))
    print("resolution   : %.4f ppb / b-LSB" % (ppm_per_lsb(a, b, c) * 1000.0))
    print("raw regs 26-33: " + " ".join("%02X" % r for r in regs))
    st = load_state(args.state)
    if st and st.get("c") == c:
        cur = (ratio(a, b, c) / ratio(st["a"], st["b"], st["c"]) - 1.0) * 1e6
        print("offset vs base: %+.4f ppm  (baseline b=%d)" % (cur, st["b"]))
    else:
        print("offset vs base: (no baseline — run 'baseline' to set 0 ppm)")
    return 0


def cmd_baseline(dev, args):
    regs = dev.read_msna()
    a, b, c = decode_msna(regs)
    save_state(args.state, {"a": a, "b": b, "c": c})
    print("baseline captured: a=%d b=%d c=%d  (now 0.00 ppm) -> %s"
          % (a, b, c, args.state))
    return 0


def _guard(applied_or_target, max_ppm):
    if abs(applied_or_target) > max_ppm:
        raise SystemExit("refusing %.3f ppm: exceeds --max-ppm %.1f "
                         "(raise it if you really mean to)"
                         % (applied_or_target, max_ppm))


def cmd_nudge(dev, args):
    regs = dev.read_msna()
    a, b, c = decode_msna(regs)
    _guard(args.ppm, args.max_ppm)
    a2, b2, applied = apply_ppm(a, b, c, args.ppm)
    if not (A_MIN <= a2 <= A_MAX):
        raise SystemExit("refusing: PLL integer a=%d would leave [%d,%d]"
                         % (a2, A_MIN, A_MAX))
    _write_new(dev, regs, a2, b2, c)
    print("nudge %+.4f ppm -> applied %+.4f ppm  (b %d -> %d)"
          % (args.ppm, applied, b, b2))
    _report_offset(args, a2, b2, c)
    _maybe_check_lock(dev, args)
    return 0


def cmd_set(dev, args):
    st = load_state(args.state)
    if not st:
        raise SystemExit("no baseline — run 'rx888_ppm.py baseline' first")
    regs = dev.read_msna()
    a, b, c = decode_msna(regs)
    if st["c"] != c:
        raise SystemExit("baseline c=%d != device c=%d (clock reprogrammed?); "
                         "re-run baseline" % (st["c"], c))
    _guard(args.ppm, args.max_ppm)
    # Target ratio is baseline * (1 + ppm); express as a delta from current.
    target_n = ratio(st["a"], st["b"], c) * (1.0 + args.ppm * 1e-6)
    delta_ppm = (target_n / ratio(a, b, c) - 1.0) * 1e6
    a2, b2, _ = apply_ppm(a, b, c, delta_ppm)
    if not (A_MIN <= a2 <= A_MAX):
        raise SystemExit("refusing: PLL integer a=%d would leave [%d,%d]"
                         % (a2, A_MIN, A_MAX))
    _write_new(dev, regs, a2, b2, c)
    achieved = (ratio(a2, b2, c) / ratio(st["a"], st["b"], c) - 1.0) * 1e6
    print("set %+.4f ppm -> achieved %+.4f ppm  (b %d -> %d)"
          % (args.ppm, achieved, b, b2))
    _maybe_check_lock(dev, args)
    return 0


def cmd_zero(dev, args):
    st = load_state(args.state)
    if not st:
        raise SystemExit("no baseline — run 'rx888_ppm.py baseline' first")
    regs = dev.read_msna()
    a, b, c = decode_msna(regs)
    if st["c"] != c:
        raise SystemExit("baseline c=%d != device c=%d; re-run baseline"
                         % (st["c"], c))
    _write_new(dev, regs, st["a"], st["b"], c)
    print("returned to baseline: a=%d b=%d c=%d (0.00 ppm)"
          % (st["a"], st["b"], c))
    _maybe_check_lock(dev, args)
    return 0


def cmd_sweep(dev, args):
    lo, hi, step = args.start, args.stop, args.step
    if step <= 0:
        raise SystemExit("step must be > 0")
    _guard(max(abs(lo), abs(hi)), args.max_ppm)
    regs0 = dev.read_msna()
    a0, b0, c0 = decode_msna(regs0)
    print("# sweeping %.3f..%.3f ppm step %.3f from a=%d b=%d c=%d (no reset)"
          % (lo, hi, step, a0, b0, c0))
    print("# %-10s %-12s %-12s %s" % ("ppm_req", "ppm_applied", "b", "lock"))
    n_steps = int(round((hi - lo) / step)) + 1
    ok = True
    for i in range(n_steps):
        ppm = lo + i * step
        a2, b2, applied = apply_ppm(a0, b0, c0, ppm)
        if not (A_MIN <= a2 <= A_MAX):
            print("# stop: a=%d out of range at %.3f ppm" % (a2, ppm))
            break
        _write_new(dev, regs0, a2, b2, c0)
        lock = ""
        if args.check_lock:
            locked = dev.pll_locked()
            lock = "LOCKED" if locked else "*** UNLOCKED ***"
            ok = ok and locked
        print("  %-10.3f %-12.4f %-12d %s" % (ppm, applied, b2, lock))
    if args.restore:
        base, data = plan_write(dev.read_msna(), regs0)
        dev.write_regs(base, data)
        print("# restored original setting (b=%d)" % b0)
    if args.check_lock and not ok:
        print("# FAIL: PLL lost lock during the sweep — this part/config is "
              "NOT glitch-free for no-reset slewing", file=sys.stderr)
        return 2
    return 0


def _report_offset(args, a, b, c):
    st = load_state(args.state)
    if st and st.get("c") == c:
        cur = (ratio(a, b, c) / ratio(st["a"], st["b"], st["c"]) - 1.0) * 1e6
        print("  now %+.4f ppm vs baseline" % cur)


def _maybe_check_lock(dev, args):
    if getattr(args, "check_lock", False):
        print("  PLL: " + ("locked" if dev.pll_locked() else "*** UNLOCKED ***"))


# --- CLI -------------------------------------------------------------------
def build_parser():
    p = argparse.ArgumentParser(
        prog="rx888_ppm.py",
        description="Online ppm trim of the RX888 sample clock via the "
                    "Si5351/MS5351M PLL-A fractional divider (glitch-free, "
                    "no PLL reset; safe while streaming).")
    p.add_argument("--fx3-cmd", default=os.environ.get("RX888_FX3_CMD", "fx3_cmd"),
                   help="path to the fx3_cmd binary (default: $RX888_FX3_CMD or "
                        "'fx3_cmd' on PATH)")
    p.add_argument("--claim", action="store_true",
                   help="claim interface 0 instead of --force/--no-claim; use "
                        "only when NO streamer is running")
    p.add_argument("--state", default=default_state_path(),
                   help="baseline state file (default: %(default)s)")
    p.add_argument("--max-ppm", type=float, default=DEFAULT_MAX_PPM,
                   help="refuse trims beyond this magnitude (default: %(default)s)")
    p.add_argument("--dry-run", action="store_true",
                   help="print the register writes without sending them")
    p.add_argument("-v", "--verbose", action="store_true",
                   help="echo each fx3_cmd invocation")

    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("read", help="show the current PLL-A ratio and resolution")
    sub.add_parser("baseline", help="capture the current setting as 0 ppm")

    sp = sub.add_parser("nudge", help="bend the clock by a relative ppm amount")
    sp.add_argument("ppm", type=float, help="signed ppm, e.g. +0.05 or -0.1")
    sp.add_argument("--check-lock", action="store_true",
                    help="verify PLL lock after the write (fx3_cmd stats_pll)")

    sp = sub.add_parser("set", help="set absolute ppm relative to the baseline")
    sp.add_argument("ppm", type=float, help="signed ppm relative to baseline")
    sp.add_argument("--check-lock", action="store_true")

    sp = sub.add_parser("zero", help="return to the captured baseline (0 ppm)")
    sp.add_argument("--check-lock", action="store_true")

    sp = sub.add_parser("sweep",
                        help="step the trim across a range (bench characterization)")
    sp.add_argument("start", type=float, help="start ppm")
    sp.add_argument("stop", type=float, help="stop ppm (inclusive)")
    sp.add_argument("step", type=float, help="ppm step (> 0)")
    sp.add_argument("--check-lock", action="store_true",
                    help="poll PLL lock after every step and fail if it drops")
    sp.add_argument("--restore", action="store_true",
                    help="restore the original setting when the sweep finishes")
    return p


DISPATCH = {
    "read": cmd_read,
    "baseline": cmd_baseline,
    "nudge": cmd_nudge,
    "set": cmd_set,
    "zero": cmd_zero,
    "sweep": cmd_sweep,
}


def main(argv=None):
    args = build_parser().parse_args(argv)
    dev = Device(args.fx3_cmd, force=not args.claim,
                 dry_run=args.dry_run, verbose=args.verbose)
    try:
        return DISPATCH[args.cmd](dev, args)
    except Fx3Error as e:
        sys.stderr.write("error: %s\n" % e)
        return 1


if __name__ == "__main__":
    sys.exit(main())
