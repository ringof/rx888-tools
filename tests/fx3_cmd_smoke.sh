#!/usr/bin/env bash
# fx3_cmd_smoke.sh — exercise fx3_cmd's CLI without hardware.
#
# Verifies the usage text, help/exit-code conventions, command-line validation
# (unknown command / wrong arity -> exit 2, before any device access), and that
# valid commands fail cleanly when no device is present. No device required;
# suitable for CI.

set -euo pipefail

FX3="${1:-./fx3_cmd}"
[[ -x "$FX3" ]] || { echo "missing or non-executable: $FX3" >&2; exit 2; }

failures=0
ok()   { echo "ok   $*"; }
fail() { echo "FAIL $*"; failures=$((failures+1)); }

run_rc() { set +e; "$@" >/dev/null 2>&1; local rc=$?; set -e; echo "$rc"; }

# Assert a command exits with an expected status.
check_rc() {             # check_rc <want_rc> <label> <cmd...>
    local want="$1" label="$2"; shift 2
    local rc; rc="$(run_rc "$@")"
    if [[ "$rc" -eq "$want" ]]; then
        ok "$label (rc=$rc)"
    else
        fail "$label: expected rc=$want, got rc=$rc"
    fi
}

tmp_help=$(mktemp)
trap 'rm -f "$tmp_help"' EXIT

# -h prints usage and exits 0.
if "$FX3" -h >"$tmp_help" 2>&1; then
    ok "-h exits 0"
else
    fail "-h exited non-zero"
fi
# --help and the bare 'help' command behave the same way.
check_rc 0 "--help exits 0" "$FX3" --help
check_rc 0 "help exits 0"   "$FX3" help

# Usage text lists every command. If a future change adds or drops one
# without updating usage(), this fails loudly.
for c in load reload test gpio adc att vga wdg_max start stop \
         i2cr i2cw reset usbreset debug raw stats stats_pll stats_shdn \
         stack_check; do
    if grep -qE "^[[:space:]]+$c\b" "$tmp_help"; then
        ok "usage lists $c"
    else
        fail "usage missing $c"
    fi
done
if grep -q -- "--no-claim" "$tmp_help"; then
    ok "usage lists --no-claim"
else
    fail "usage missing --no-claim"
fi

# No arguments prints usage and exits 2 (usage error).
check_rc 2 "no-arg" "$FX3"

# The command name and argument count are validated before any device access,
# so a typo or wrong arity is a usage error (exit 2) regardless of whether a
# device is attached or busy — not the device-open failure path (1).
check_rc 2 "unknown command"     "$FX3" bogus
check_rc 2 "missing arg (gpio)"  "$FX3" gpio
check_rc 2 "too few args (i2cr)" "$FX3" i2cr 0xC0 0
check_rc 2 "extra arg (test)"    "$FX3" test extra

# --no-claim is only allowed for read-only EP0 commands; a valid-arity
# write/recovery command with --no-claim is rejected by the allowlist with
# rc=2 (no device needed).
check_rc 2 "--no-claim gpio rejected" "$FX3" --no-claim gpio 0x20
for c in reset usbreset reload stats_shdn; do
    check_rc 2 "--no-claim $c rejected" "$FX3" --no-claim "$c"
done

# Without hardware, a valid command with correct arity must fail cleanly at
# device open (exit 1) — not segfault, not hang, not 0. (gpio needs an arg;
# the rest take none.) Skip if a device is attached.
if [[ "${RX888_HW_PRESENT:-0}" != "1" ]]; then
    check_rc 1 "no-device test"            "$FX3" test
    check_rc 1 "no-device stats"           "$FX3" stats
    check_rc 1 "no-device gpio"            "$FX3" gpio 0x20
    check_rc 1 "no-device reset"           "$FX3" reset
    check_rc 1 "no-device --no-claim test" "$FX3" --no-claim test
fi

if [[ $failures -eq 0 ]]; then
    echo "ALL OK"
    exit 0
else
    echo "FAILED ($failures)"
    exit 1
fi
