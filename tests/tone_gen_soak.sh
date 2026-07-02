#!/usr/bin/env bash
# tone_gen_soak.sh — drive the tone_monitor Goertzel stream processor and the
# tone_quality.py analyzer through the STDIN pipe with a synthetic stream, with
# no hardware. Exercises the streaming path (--source -), the iqlog ring under
# sustained load, and deterministic defect injection end-to-end.
#
# Requires python3 + numpy (for the analyzer); skips cleanly otherwise.

set -euo pipefail

TG="${1:-./tone_gen}"
TM="${2:-./tone_monitor}"
HERE="$(cd "$(dirname "$0")" && pwd)"
TQ="$HERE/tone_quality.py"

[[ -x "$TG" ]] || { echo "missing tone_gen: $TG" >&2; exit 2; }
[[ -x "$TM" ]] || { echo "missing tone_monitor: $TM" >&2; exit 2; }
if ! python3 -c 'import numpy' >/dev/null 2>&1; then
    echo "SKIP tone_gen_soak: python3+numpy not available"; exit 0
fi

work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
rc=0
N=2400000          # ~4.8 MB stream; fast but spans many baseband records

drive() {          # drive <name> <tone_gen-args...>
    local name="$1"; shift
    "$TG" --samples "$N" "$@" 2>/dev/null \
        | "$TM" --source - --iqlog "$work/$name.iq" --decim 2400 >/dev/null 2>&1
    python3 "$TQ" "$work/$name.iq"
}
check() { local cond="$1" msg="$2"; if [[ "$cond" == 1 ]]; then echo "ok   $msg"; else echo "FAIL $msg"; rc=1; fi; }
has()   { grep -Eq "$2" <<<"$1" && echo 1 || echo 0; }

out="$(drive clean)"
check "$(has "$out" 'NO grid slips')"           "clean pipe -> coherence intact, no slips"

# one drop every 600k over 2.4M -> slips at 600k/1200k/1800k (the 2.4M edge is
# the stream end, not emitted) = 3 slips.
out="$(drive drop --drop-every 600000)"
check "$(has "$out" '3 grid SLIP')"             "drop-every 600k -> exactly 3 grid slips"

out="$(drive garble --garble-every 800000 --garble-len 50)"
check "$(has "$out" '2 garble')"                "garble-every 800k -> 2 amplitude garbles"
check "$(has "$out" 'NO grid slips')"           "garble -> no false slips"

# A square reference (GPSDO output) must still work: the Goertzel nulls all
# harmonics except the few aliasing onto the bin (a small constant bias), so
# slip detection is unaffected.
out="$(drive sqdrop --square --drop-every 600000)"
check "$(has "$out" '3 grid SLIP')"             "square wave + drop-every 600k -> still 3 grid slips"
check "$([[ "$(has "$out" 'garble')" == 0 ]] && echo 1 || echo 0)" \
                                                "square -> no false garbles"

# Windowed streaming + per-window plotdata (the drift-robust path used on long /
# multi-GB captures): reuse the drop iqlog (3 slips). The per-window CSV must
# carry the 8 raw columns and its slip column must total the same 3 slips the
# whole-file path found — proving the constant-memory path agrees with it.
if [[ -f "$work/drop.iq" ]]; then
    python3 "$TQ" "$work/drop.iq" --window 0.005 --plotdata "$work/drop_win.dat" >/dev/null
    hdr="$(grep -m1 '^# window' "$work/drop_win.dat" || true)"
    check "$(has "$hdr" 'window .* t_s .* amp_mean .* resid_carrier_hz .* jitter_deg .* slips')" \
                                                    "windowed plotdata -> 8-column raw per-window CSV"
    slipsum="$(awk '!/^#/{s+=$8} END{print s+0}' "$work/drop_win.dat")"
    check "$([[ "$slipsum" == 3 ]] && echo 1 || echo 0)" \
                                                    "windowed plotdata slips total 3 (agrees with whole-file)"
fi

# +5 ppm detune -> residual carrier ~ 5e-6 * 27e6 = 135 Hz.
out="$(drive foff --freq-offset 5)"
rf="$(grep -Eo 'residual carrier [+-]?[0-9.]+' <<<"$out" | grep -Eo '[+-]?[0-9.]+$' || echo 0)"
check "$(awk -v x="$rf" 'BEGIN{print (x>130 && x<140)?1:0}')" \
                                                "freq-offset 5ppm -> residual carrier ${rf} Hz (~135)"

if [[ $rc -eq 0 ]]; then echo "ALL OK"; else echo "FAILED"; fi
exit $rc
