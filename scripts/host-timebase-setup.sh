#!/usr/bin/env bash
# host-timebase-setup.sh — configure the Raspberry Pi HOST clock for the PPS
# timing test (Tier 1 in doc/pps_timing.md).
#
# This lives on the HOST, not in the container: chrony disciplines the kernel
# clock and pps-gpio loads a kernel module + device-tree overlay — both are
# host-level and cannot sensibly run inside Docker. The stream/analysis kit runs
# in the container; this just gives the host a GPSDO-locked clock and /dev/pps0.
#
# What it does (idempotently):
#   - installs chrony + pps-tools
#   - enables the pps-gpio device-tree overlay on a GPIO pin -> /dev/pps0
#   - loads the pps_gpio module at boot
#   - adds the PPS refclock (and an NMEA refclock for the integer second) to
#     chrony so the host clock locks to the SAME GPSDO that drives the RX888
#
# SAFE BY DEFAULT: prints the intended changes and exits. Pass --apply to make
# them. A reboot is required after --apply for the overlay to take effect.
#
# Usage: sudo ./host-timebase-setup.sh [--apply] [--gpio N] [--nmea DEV]
set -euo pipefail

GPIO=18
NMEA_DEV="/dev/ttyACM0"
APPLY=0

while [ $# -gt 0 ]; do
    case "$1" in
        --apply) APPLY=1 ;;
        --gpio)  GPIO="${2:?--gpio needs a pin}"; shift ;;
        --nmea)  NMEA_DEV="${2:?--nmea needs a device}"; shift ;;
        -h|--help)
            sed -n '2,/^set -euo/p' "$0" | sed 's/^# \{0,1\}//; /^set -euo/d'
            exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
    shift
done

# Raspberry Pi OS (bookworm) moved config.txt under /boot/firmware.
BOOTCFG=/boot/firmware/config.txt
[ -f "$BOOTCFG" ] || BOOTCFG=/boot/config.txt
CHRONY_DROPIN=/etc/chrony/conf.d/rx888-gpsdo.conf
OVERLAY_LINE="dtoverlay=pps-gpio,gpiopin=${GPIO}"

say() { printf '  %s\n' "$*"; }

echo "== host-timebase-setup =="
say "boot config : $BOOTCFG"
say "pps-gpio pin : GPIO${GPIO}  -> /dev/pps0"
say "NMEA device  : $NMEA_DEV"
say "chrony drop-in: $CHRONY_DROPIN"
echo

run() {                         # run <description> <cmd...>
    local desc="$1"; shift
    if [ "$APPLY" = 1 ]; then
        echo "[apply] $desc"
        "$@"
    else
        echo "[dry-run] $desc:  $*"
    fi
}

# 1. packages
run "install chrony + pps-tools" \
    bash -c 'apt-get update && apt-get install -y --no-install-recommends chrony pps-tools'

# 2. device-tree overlay (append only if absent)
if grep -qxF "$OVERLAY_LINE" "$BOOTCFG" 2>/dev/null; then
    say "overlay already present in $BOOTCFG"
else
    run "append '$OVERLAY_LINE' to $BOOTCFG" \
        bash -c "printf '\n# RX888 PPS timing test\n%s\n' '$OVERLAY_LINE' >> '$BOOTCFG'"
fi

# 3. load module at boot
if grep -qxF "pps_gpio" /etc/modules 2>/dev/null; then
    say "pps_gpio already in /etc/modules"
else
    run "add pps_gpio to /etc/modules" \
        bash -c "echo pps_gpio >> /etc/modules"
fi

# 4. chrony refclocks (PPS for the fraction, NMEA for the integer second)
read -r -d '' CHRONY_CONF <<EOF || true
# RX888 timing test — lock the host clock to the SAME GPSDO that drives the
# RX888, so host-vs-ADC clock drift does not masquerade as PPS extraction error.
refclock SHM 0 refid NMEA offset 0.0 precision 1e-3 poll 3   # gpsd feeds NMEA via SHM
refclock PPS /dev/pps0 lock NMEA refid PPS precision 1e-7 prefer
EOF
if [ "$APPLY" = 1 ]; then
    echo "[apply] write $CHRONY_DROPIN"
    mkdir -p "$(dirname "$CHRONY_DROPIN")"
    printf '%s\n' "$CHRONY_CONF" > "$CHRONY_DROPIN"
else
    echo "[dry-run] write $CHRONY_DROPIN:"
    printf '%s\n' "$CHRONY_CONF" | sed 's/^/    /'
    echo "          (NMEA via gpsd SHM from $NMEA_DEV; install/point gpsd at it,"
    echo "           or replace with chrony's own NMEA refclock if not using gpsd)"
fi

echo
if [ "$APPLY" = 1 ]; then
    echo "Applied. REBOOT for the pps-gpio overlay to take effect, then verify:"
    echo "    ls -l /dev/pps0 && sudo ppstest /dev/pps0     # see PPS asserts"
    echo "    chronyc sources -v && chronyc tracking         # PPS locked"
else
    echo "Dry run only. Re-run with --apply (as root) to make these changes."
fi
