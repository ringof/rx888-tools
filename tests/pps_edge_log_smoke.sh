#!/usr/bin/env bash
# pps_edge_log_smoke.sh — exercise pps_edge_log's CLI without a PPS device.
#
# The tool self-guards: with <sys/timepps.h> (pps-tools) it is the full logger;
# without it, a stub that still answers --help. This test covers both — it
# detects the build and only asserts the runtime arg/exit conventions on the
# full build. No device required; suitable for CI.

set -euo pipefail

TOOL="${1:-./pps_edge_log}"
[[ -x "$TOOL" ]] || { echo "missing or non-executable: $TOOL" >&2; exit 2; }

failures=0
ok()   { echo "ok   $*"; }
fail() { echo "FAIL $*"; failures=$((failures+1)); }

run_rc() { set +e; "$@" >/dev/null 2>&1; local rc=$?; set -e; echo "$rc"; }
check_rc() {             # check_rc <want_rc> <label> <cmd...>
    local want="$1" label="$2"; shift 2
    local rc; rc="$(run_rc "$@")"
    if [[ "$rc" -eq "$want" ]]; then ok "$label (rc=$rc)"; else
        fail "$label: expected rc=$want, got rc=$rc"; fi
}

tmp_help=$(mktemp); tmp_err=$(mktemp)
trap 'rm -f "$tmp_help" "$tmp_err"' EXIT

# --help works on BOTH builds and exits 0.
if "$TOOL" -h >"$tmp_help" 2>&1; then ok "-h exits 0"; else fail "-h exited non-zero"; fi
check_rc 0 "--help exits 0" "$TOOL" --help
for tok in seconds --device --out; do
    if grep -q -- "$tok" "$tmp_help"; then ok "usage lists $tok"; else fail "usage missing $tok"; fi
done

# Detect the build: a normal run on the stub prints "built without".
set +e; "$TOOL" --device /nonexistent-pps-xyz 0.05 >/dev/null 2>"$tmp_err"; set -e
if grep -q "built without" "$tmp_err"; then
    ok "stub build (no <sys/timepps.h>) — skipping runtime checks"
else
    # Full build: arg validation is exit 2, before any device access.
    check_rc 2 "bad seconds"  "$TOOL" abc
    check_rc 2 "extra arg"    "$TOOL" 1 2
    check_rc 2 "unknown flag" "$TOOL" --bogus
    # A valid invocation against a missing device fails cleanly at open (exit 1).
    check_rc 1 "no such device" "$TOOL" --device /nonexistent-pps-xyz 0.05
fi

if [[ $failures -eq 0 ]]; then echo "ALL OK"; exit 0; else echo "FAILED ($failures)"; exit 1; fi
