#!/usr/bin/env bash
# tone_monitor_smoke.sh — exercise tone_monitor's CLI without hardware.
#
# Verifies help/exit-code conventions, argument validation (exit 2 before any
# device access — including the decim-must-divide-period rule), and that a valid
# invocation fails cleanly at device open (exit 1) when no device is present.
# No device required; suitable for CI.

set -euo pipefail

TM="${1:-./tone_monitor}"
[[ -x "$TM" ]] || { echo "missing or non-executable: $TM" >&2; exit 2; }

failures=0
ok()   { echo "ok   $*"; }
fail() { echo "FAIL $*"; failures=$((failures+1)); }

run_rc() { set +e; "$@" >/dev/null 2>&1; local rc=$?; set -e; echo "$rc"; }

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

if "$TM" -h >"$tmp_help" 2>&1; then
    ok "-h exits 0"
else
    fail "-h exited non-zero"
fi
check_rc 0 "--help exits 0" "$TM" --help

for tok in hours --rate --ftone --decim --statslog --iqlog; do
    if grep -q -- "$tok" "$tmp_help"; then
        ok "usage lists $tok"
    else
        fail "usage missing $tok"
    fi
done

# Argument validation happens before any device access (exit 2).
check_rc 2 "zero rate"     "$TM" --rate 0
check_rc 2 "bad rate"      "$TM" --rate abc
check_rc 2 "bad rate tail" "$TM" --rate 129.6x
check_rc 2 "bad ftone"     "$TM" --ftone abc
check_rc 2 "zero decim"    "$TM" --decim 0
check_rc 2 "bad decim"     "$TM" --decim 12x
check_rc 2 "bad hours"     "$TM" abc
check_rc 2 "extra arg"     "$TM" 1 2
check_rc 2 "unknown flag"  "$TM" --bogus

# decim must be a multiple of the period N = fs/gcd(fs,ftone). At the default
# rig (129.6 MSPS, 27 MHz) N=24, so 2400 is valid but 2401 is not — and this is
# checked before any device access (exit 2).
check_rc 2 "decim not multiple of N" "$TM" --decim 2401
if [[ "${RX888_HW_PRESENT:-0}" != "1" ]]; then
    check_rc 1 "valid decim, no device" "$TM" 0.001 --decim 2400
fi

# A valid invocation with no device must fail cleanly at open (exit 1).
if [[ "${RX888_HW_PRESENT:-0}" != "1" ]]; then
    check_rc 1 "no-device run" "$TM" 0.001
fi

if [[ $failures -eq 0 ]]; then
    echo "ALL OK"
    exit 0
else
    echo "FAILED ($failures)"
    exit 1
fi
