#!/usr/bin/env bash
# ssd-preflight.sh — go/no-go that the capture target is on the mounted SSD,
# writable, and has room. Catches the classic footgun: if the SSD isn't
# mounted, the capture dir is just a plain folder on the SD card root, so
# captures silently land on (and fill) the SD card instead of the SSD.
#
#   DIR          capture dir (arg 1 or $CAPTURE_DIR, default /mnt/ssd/rx888)
#   MIN_FREE_GB  minimum free space to require (default 10)
#
# Exit: 0 PASS, 1 FAIL, 2 setup error.

set -uo pipefail
DIR="${1:-${CAPTURE_DIR:-/mnt/ssd/rx888}}"
MIN_FREE_GB="${MIN_FREE_GB:-10}"

fail=0
ok()  { printf '  ok   %s\n' "$*"; }
bad() { printf '  FAIL %s\n' "$*"; fail=1; }

echo "== SSD preflight: $DIR =="

# 1. exists
if [ ! -d "$DIR" ]; then
    bad "capture dir does not exist: $DIR (mount the SSD and mkdir it)"
    echo "  PREFLIGHT FAIL"; exit 1
fi

# 2. on a real mount, NOT the rootfs / SD card (df/stat give one clean line each)
root_src="$(df -P / 2>/dev/null | awk 'NR==2{print $1}')"
dir_src="$(df -P "$DIR" 2>/dev/null | awk 'NR==2{print $1}')"
dir_fs="$(stat -f -c %T "$DIR" 2>/dev/null)"
if [ -z "$dir_src" ]; then
    bad "cannot resolve the mount for $DIR"
elif [ "$dir_src" = "$root_src" ]; then
    bad "$DIR is on the ROOT filesystem ($dir_src) — the SSD is NOT mounted; captures would fill the SD card"
elif printf '%s' "$dir_src" | grep -q mmcblk; then
    bad "$DIR is on the SD card ($dir_src) — mount the SSD there instead"
else
    ok "on a separate mount: $dir_src ($dir_fs)"
fi

# 3. writable by whoever will capture
tf="$DIR/.ssd_preflight_$$"
if ( : > "$tf" ) 2>/dev/null; then
    rm -f "$tf"; ok "writable by $(id -un)"
else
    bad "not writable by $(id -un) (chown the dir, or capture with matching --user)"
fi

# 4. free space
avail_kb="$(df -Pk "$DIR" 2>/dev/null | awk 'NR==2{print $4}')"
avail_gb=$(( ${avail_kb:-0} / 1024 / 1024 ))
if [ "${avail_kb:-0}" -ge $(( MIN_FREE_GB * 1024 * 1024 )) ]; then
    ok "free space ${avail_gb} GB (>= ${MIN_FREE_GB} GB; iqlog burns ~1.5 GB/hr)"
else
    bad "only ${avail_gb} GB free (< ${MIN_FREE_GB} GB)"
fi

echo "  ================"
[ "$fail" = 0 ] && { echo "  PREFLIGHT PASS"; exit 0; } || { echo "  PREFLIGHT FAIL"; exit 1; }
