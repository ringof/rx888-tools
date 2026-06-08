#!/usr/bin/env bash
# rx888_present.sh — print "1" if an RX888 in *application* mode (VID 04b4,
# PID 00f1) is attached, else "0".
#
# A bootloader-mode device (04b4:00f3) counts as absent here on purpose: the
# no-device negative tests behave correctly against it — rx888_open returns
# NO_DEVICE, rx888_stream/fx3_cmd exit 1 — without uploading firmware or
# disturbing it, so they need not be skipped.  Only a *loaded* device makes
# those commands succeed (or claim a possibly-streaming interface), which is
# what we must detect.
#
# `make check` uses this to auto-set RX888_HW_PRESENT=1 so the device-disturbing
# negative checks are skipped when a usable device is plugged in.  Pure sysfs;
# no lsusb dependency.  Defaults to "0" on anything non-Linux or unreadable.

for d in /sys/bus/usb/devices/*; do
    v=$(cat "$d/idVendor" 2>/dev/null)  || continue
    p=$(cat "$d/idProduct" 2>/dev/null) || continue
    if [ "$v" = "04b4" ] && [ "$p" = "00f1" ]; then
        echo 1
        exit 0
    fi
done
echo 0
