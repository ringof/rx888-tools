#!/usr/bin/env bash
# pps_integrity_smoke.sh — exercise pps_integrity's CLI without hardware.
#
# Verifies the usage text, help/exit-code conventions, argument validation
# (bad rate / bad hours / extra args -> exit 2, before any device access), and
# that a valid invocation fails cleanly at device open (exit 1) when no device
# is present. No device required; suitable for CI.

set -euo pipefail

PPS="${1:-./pps_integrity}"
[[ -x "$PPS" ]] || { echo "missing or non-executable: $PPS" >&2; exit 2; }

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

# -h / --help print usage and exit 0.
if "$PPS" -h >"$tmp_help" 2>&1; then
    ok "-h exits 0"
else
    fail "-h exited non-zero"
fi
check_rc 0 "--help exits 0" "$PPS" --help

# Usage text mentions the documented options. A future rename without updating
# usage() fails loudly here.
for tok in hours --rate --help; do
    if grep -q -- "$tok" "$tmp_help"; then
        ok "usage lists $tok"
    else
        fail "usage missing $tok"
    fi
done

# Argument validation happens before any device access (exit 2).
check_rc 2 "zero rate"        "$PPS" --rate 0
check_rc 2 "bad hours"        "$PPS" abc
check_rc 2 "extra arg"        "$PPS" 1 2
check_rc 2 "unknown flag"     "$PPS" --bogus

# A valid invocation with no device must fail cleanly at open (exit 1) — not
# segfault, not hang, not 0. A tiny duration keeps the run bounded if a device
# happens to be attached. Skip the assertion when hardware is present.
if [[ "${RX888_HW_PRESENT:-0}" != "1" ]]; then
    check_rc 1 "no-device run" "$PPS" 0.001 --rate 16
fi

if [[ $failures -eq 0 ]]; then
    echo "ALL OK"
    exit 0
else
    echo "FAILED ($failures)"
    exit 1
fi
