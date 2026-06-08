#!/usr/bin/env bash
# fx3_cmd_smoke.sh — exercise fx3_cmd's CLI without hardware.
#
# Verifies the usage text, help/exit-code conventions, and that the binary
# fails cleanly (no segfault) when no device is present.  No device required;
# suitable for CI.

set -euo pipefail

FX3="${1:-./fx3_cmd}"
[[ -x "$FX3" ]] || { echo "missing or non-executable: $FX3" >&2; exit 2; }

failures=0
ok()   { echo "ok   $*"; }
fail() { echo "FAIL $*"; failures=$((failures+1)); }

run_rc() { set +e; "$@" >/dev/null 2>&1; local rc=$?; set -e; echo "$rc"; }

tmp_help=$(mktemp)
trap 'rm -f "$tmp_help"' EXIT

# -h prints usage and exits 0.
if "$FX3" -h >"$tmp_help" 2>&1; then
    ok "-h exits 0"
else
    fail "-h exited non-zero"
fi
# --help and the bare 'help' command behave the same way.
[[ "$(run_rc "$FX3" --help)" -eq 0 ]] && ok "--help exits 0" || fail "--help non-zero"
[[ "$(run_rc "$FX3" help)"   -eq 0 ]] && ok "help exits 0"   || fail "help non-zero"

# Usage text lists every command.  If a future change adds or drops one
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

# No arguments prints usage and exits 2 (usage error).
[[ "$(run_rc "$FX3")" -eq 2 ]] && ok "no-arg exits 2" || fail "no-arg expected rc=2"

# Without hardware, a real command must fail cleanly (exit 1, device open
# fails) — not segfault, not hang, not 0.  Skip if a device is attached.
if [[ "${RX888_HW_PRESENT:-0}" != "1" ]]; then
    for c in test stats gpio reset; do
        rc="$(run_rc "$FX3" "$c" 0)"
        if [[ "$rc" -eq 1 ]]; then
            ok "no-device '$c' exits 1"
        else
            fail "no-device '$c' expected rc=1, got rc=$rc"
        fi
    done
fi

if [[ $failures -eq 0 ]]; then
    echo "ALL OK"
    exit 0
else
    echo "FAILED ($failures)"
    exit 1
fi
