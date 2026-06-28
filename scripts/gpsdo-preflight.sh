#!/usr/bin/env bash
# gpsdo-preflight.sh — go/no-go check that the Leo Bodnar LBE-142x GPSDO is
# configured and locked for the RX888 PPS/tone campaign BEFORE a capture, so a
# misconfigured or unlocked GPSDO can't silently poison the data.
#
# This is a GATE, so PASS/FAIL is the point (unlike the measurement tools, which
# report raw numbers only). Checks, via the lbe-142x config tool (HID) + NMEA:
#   - GPS lock, PLL lock, antenna OK, outputs enabled   (an unlocked GPSDO is
#     the #1 silent data-poisoner)
#   - OUT1 1PPS enabled         (the timing reference for pps-gpio)
#   - OUT2 = 27 MHz             (the coherent tone for tone_quality)
#   - NMEA output enabled, AND sentences actually flowing with a GNSS fix
#
# Config tool: set $LBE142X to its path (default: lbe-142x on PATH), e.g.
#   export LBE142X=~/gps_pps/lbe-142x/build/bin/lbe-142x
# Expected OUT2: $FTONE_HZ (default 27000000).
# NMEA: read via gpsd (gpspipe) if running — gpsd owns the CDC port — else the
#   raw device $NMEA_DEV (default /dev/ttyACM0).
#
# Exit: 0 PASS, 1 FAIL, 2 setup error.

set -uo pipefail

LBE="${LBE142X:-lbe-142x}"
FTONE="${FTONE_HZ:-27000000}"
NMEA_DEV="${NMEA_DEV:-/dev/ttyACM0}"

fail=0
ok()  { printf '  ok   %s\n' "$*"; }
bad() { printf '  FAIL %s\n' "$*"; fail=1; }
has() { grep -qiE "$2" <<<"$1"; }     # has <text> <regex>

command -v "$LBE" >/dev/null 2>&1 || {
    echo "gpsdo-preflight: '$LBE' not found — set \$LBE142X to the lbe-142x binary" >&2
    exit 2; }

echo "== GPSDO preflight =="

# --- device status (HID) ---
st="$("$LBE" --status 2>&1)" || { echo "lbe-142x --status failed:"; echo "$st"; exit 2; }
echo "$st" | sed 's/^/    /'
echo "  ---- checks ----"

has "$st" 'GPS Lock:[[:space:]]*Yes'           && ok "GPS locked"        || bad "GPS NOT locked"
has "$st" 'PLL Lock:[[:space:]]*Yes'           && ok "PLL locked"        || bad "PLL NOT locked"
has "$st" 'Antenna:[[:space:]]*OK'             && ok "antenna OK"        || bad "antenna not OK"
has "$st" 'Output\(s\) Enabled:[[:space:]]*Yes' && ok "outputs enabled"  || bad "outputs disabled"
has "$st" '1PPS on OUT1:[[:space:]]*Enabled'   && ok "OUT1 = 1PPS"       || bad "OUT1 1PPS NOT enabled"
has "$st" 'NMEA output:[[:space:]]*Enabled'    && ok "NMEA enabled"      || bad "NMEA output disabled"

o2="$(grep -iE 'OUT2 Frequency:' <<<"$st" | sed -E 's/.*Frequency:[[:space:]]*([0-9]+).*/\1/')"
[ "${o2:-0}" = "$FTONE" ] && ok "OUT2 = $FTONE Hz" || bad "OUT2 = ${o2:-?} Hz (expected $FTONE)"

# --- NMEA actually flowing + fix (authoritative, regardless of the status flag) ---
read_nmea() {
    if command -v gpspipe >/dev/null 2>&1 && timeout 2 gpspipe -r -n 1 >/dev/null 2>&1; then
        timeout 6 gpspipe -r 2>/dev/null
    elif [ -r "$NMEA_DEV" ]; then
        timeout 6 cat "$NMEA_DEV" 2>/dev/null
    fi | tr -d '\r'
}
nm="$(read_nmea)"
if [ -z "$nm" ]; then
    bad "no NMEA (gpsd not running and $NMEA_DEV unreadable?)"
else
    has "$nm" '\$G[NP][A-Z]{3},'                 && ok "NMEA sentences flowing" || bad "no NMEA sentences"
    if has "$nm" '\$G[NP]RMC,[^,]*,A,' || has "$nm" '\$G[NP]GGA,([^,]*,){5}[1-9],'; then
        ok "GNSS fix present"
    else
        bad "no GNSS fix (RMC status not 'A' / GGA quality 0)"
    fi
fi

echo "  ================"
if [ "$fail" = 0 ]; then echo "  PREFLIGHT PASS"; exit 0; else echo "  PREFLIGHT FAIL"; exit 1; fi
