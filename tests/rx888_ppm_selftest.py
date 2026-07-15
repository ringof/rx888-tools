#!/usr/bin/env python3
"""rx888_ppm_selftest.py — hardware-free checks for rx888_ppm.py math.

Verifies the MSNA register decode/encode round-trip, the ppm helpers, and
the minimal reset-free write planner. No device required; runs in CI.
"""
import importlib.util
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_MOD = os.path.join(_HERE, "..", "scripts", "rx888_ppm.py")


def load():
    spec = importlib.util.spec_from_file_location("rx888_ppm", _MOD)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def test_decode_encode_roundtrip(m):
    vectors = [
        (15, 0, 1_000_000),
        (30, 0, 1_048_575),
        (30, 524_287, 1_048_575),
        (32, 1, 1_000_000),
        (35, 999_999, 1_000_000),
        (88, 12_345, 1_048_575),
        (90, 1_048_574, 1_048_575),
    ]
    for a, b, c in vectors:
        regs = m.encode_msna(a, b, c)
        assert all(0 <= r <= 0xFF for r in regs), regs
        got = m.decode_msna(regs)
        assert got == (a, b, c), "roundtrip %r -> %r" % ((a, b, c), got)


def test_p3_nibble_preserved(m):
    # c that uses all 20 bits so P3[19:16] is non-zero; it must survive encode.
    a, b, c = 30, 100_000, 0xF_FFFF  # 1048575
    regs = m.encode_msna(a, b, c)
    assert (regs[5] >> 4) == ((c >> 16) & 0xF), regs


def test_ppm_resolution(m):
    a, b, c = 30, 500_000, 1_048_575
    r = m.ppm_per_lsb(a, b, c)
    # Tens of ppb per LSB for a typical config.
    assert 0.01 < r < 0.1, r


def test_apply_ppm_direction_and_magnitude(m):
    a, b, c = 30, 500_000, 1_048_575
    a2, b2, applied = m.apply_ppm(a, b, c, +1.0)
    assert b2 > b and applied > 0
    assert abs(applied - 1.0) < 0.05, applied      # quantization is tiny
    a3, b3, applied3 = m.apply_ppm(a, b, c, -1.0)
    assert b3 < b and applied3 < 0


def test_apply_ppm_carry_into_a(m):
    # A negative nudge at b=0 must borrow from a, not produce b<0.
    a, b, c = 30, 0, 1_000_000
    a2, b2, applied = m.apply_ppm(a, b, c, -0.5)
    assert 0 <= b2 < c, b2
    assert a2 == a - 1, (a2, a)
    assert applied < 0


def test_plan_write_p2_only(m):
    a, b, c = 30, 100_000, 1_048_575
    old = m.encode_msna(a, b, c)
    a2, b2, _ = m.apply_ppm(a, b, c, 0.01)   # tiny: q unchanged
    new = m.encode_msna(a2, b2, c)
    base, data = m.plan_write(old, new)
    assert base == m.REG_P2_BASE and len(data) == 3, (hex(base), data)
    assert base != m.PLL_RESET_REG
    assert m.MSNA_BASE <= base <= 0x21


def test_plan_write_p1_tick(m):
    a, b, c = 30, 100_000, 1_048_575
    old = m.encode_msna(a, b, c)
    # Jump b far enough that floor(128*b/c) increments -> P1 changes.
    new = m.encode_msna(a, b + 20_000, c)
    base, data = m.plan_write(old, new)
    assert base == m.REG_P1_BASE and len(data) == 6, (hex(base), data)


def test_plan_write_refuses_c_change(m):
    old = m.encode_msna(30, 100_000, 1_048_575)
    new = m.encode_msna(30, 100_000, 1_000_000)   # different c
    try:
        m.plan_write(old, new)
    except AssertionError:
        return
    raise AssertionError("plan_write should refuse a P3/c change")


def main():
    m = load()
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for t in tests:
        t(m)
        print("ok  %s" % t.__name__)
    print("PASS rx888_ppm_selftest (%d checks)" % len(tests))
    return 0


if __name__ == "__main__":
    sys.exit(main())
