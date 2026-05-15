#!/usr/bin/env bash
# cli_smoke.sh — exercise rx888_stream's CLI without hardware.
#
# Verifies argparse coverage, error paths, and that the binary doesn't
# crash on bad inputs.  Suitable for CI; no device required.

set -euo pipefail

STREAM="${1:-./rx888_stream}"
[[ -x "$STREAM" ]] || { echo "missing or non-executable: $STREAM" >&2; exit 2; }

failures=0
ok()   { echo "ok   $*"; }
fail() { echo "FAIL $*"; failures=$((failures+1)); }

tmp_help=$(mktemp)
trap 'rm -f "$tmp_help"' EXIT

# -h prints help and exits 0
if "$STREAM" -h >"$tmp_help" 2>&1; then
    ok "-h exits 0"
else
    fail "-h exited non-zero"
fi

# Help mentions every documented flag.  If a future change drops one,
# this fails loudly instead of silently.
for flag in --firmware --verbose --dither --rand --fixup --samplerate \
            --gainmode --gain --att --queuedepth --reqsize \
            --ctrl-timeout --stream-timeout --watchdog-timeout --help; do
    if grep -q -- "$flag" "$tmp_help"; then
        ok "help mentions $flag"
    else
        fail "help missing $flag"
    fi
done

# Unknown flag exits non-zero (getopt convention: 2)
set +e
"$STREAM" --does-not-exist >/dev/null 2>&1
rc=$?
set -e
if [[ $rc -ne 0 ]]; then
    ok "unknown flag exits non-zero (rc=$rc)"
else
    fail "unknown flag should exit non-zero"
fi

# Without hardware, attempting to open a device exits 1 (not segfault, not 0).
# Skip this check if RX888_HW_PRESENT=1 (bench testing).
if [[ "${RX888_HW_PRESENT:-0}" != "1" ]]; then
    set +e
    "$STREAM" >/dev/null 2>&1
    rc=$?
    set -e
    if [[ $rc -eq 1 ]]; then
        ok "no-device run exits 1"
    else
        fail "no-device run expected rc=1, got rc=$rc"
    fi
fi

if [[ $failures -eq 0 ]]; then
    echo "ALL OK"
    exit 0
else
    echo "FAILED ($failures)"
    exit 1
fi
