#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 frty2.ai and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Probe P1: passive USB/HID enumeration of the BlackWidow V4 Pro. Read-only, no root needed.
# Output: probes/out/<timestamp>-enumerate/  (re-run any time; diff against baseline/ after firmware or mode changes)
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$HERE/out/$(date +%Y%m%d-%H%M%S)-enumerate; mkdir -p "$OUT"
DEV=$(for d in /sys/bus/usb/devices/*; do [ -f "$d/idProduct" ] || continue; [ "$(cat "$d/idVendor")" = 1532 ] && [ "$(cat "$d/idProduct")" = 028d ] && basename "$d"; done | head -1)
[ -n "$DEV" ] || { echo "keyboard not found"; exit 1; }
{
  echo "# $(date -Is) kernel=$(uname -r) usbdev=$DEV"
  echo "bcdDevice=$(cat /sys/bus/usb/devices/$DEV/bcdDevice) speed=$(cat /sys/bus/usb/devices/$DEV/speed) power/control=$(cat /sys/bus/usb/devices/$DEV/power/control) autosuspend_delay_ms=$(cat /sys/bus/usb/devices/$DEV/power/autosuspend_delay_ms)"
  for i in 0 1 2 3 4; do
    I=/sys/bus/usb/devices/$DEV:1.$i; H=$(ls -d $I/0003:* 2>/dev/null | head -1)
    printf 'IF%d class=%s sub=%s proto=%s driver=%s hid=%s hiddrv=%s hidraw=%s events=%s\n' $i "$(cat $I/bInterfaceClass)" "$(cat $I/bInterfaceSubClass)" "$(cat $I/bInterfaceProtocol)" "$(basename "$(readlink $I/driver)")" "$(basename "$H")" "$(basename "$(readlink $H/driver 2>/dev/null)" 2>/dev/null)" "$(ls $H/hidraw 2>/dev/null | tr '\n' ' ')" "$(ls -d $H/input/input*/event* 2>/dev/null | xargs -rn1 basename | tr '\n' ' ')"
    [ -n "$H" ] && cp "$H/report_descriptor" "$OUT/if$i.rdesc.bin" && python3 "$HERE/tools/rdesc_decode.py" "$OUT/if$i.rdesc.bin" > "$OUT/if$i.rdesc.txt"
  done
} | tee "$OUT/summary.txt"
lsusb -v -d 1532:028d > "$OUT/lsusb-v.txt" 2>&1
awk 'BEGIN{RS="";FS="\n"} /1532/ && /028d/' /proc/bus/input/devices > "$OUT/proc-bus-input.txt"
ls -l /dev/input/by-id/ 2>/dev/null | grep -i BlackWidow_V4_Pro > "$OUT/by-id.txt"
lsmod | grep -i razer > "$OUT/razer-modules.txt"; dpkg -l 2>/dev/null | grep -i -E 'razer|polychromatic' > "$OUT/razer-packages.txt"
echo "written to $OUT"; ls "$OUT"
