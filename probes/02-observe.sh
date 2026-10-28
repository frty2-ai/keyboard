#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 frty2.ai and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Probe P2/P5: passive capture of what the keyboard emits while you press the checklist in docs/PROBE_PLAN.md.
# Captures evdev events from every event node of the keyboard (evtest, non-grabbing) and raw HID
# reports from every hidraw node (hid-recorder, if installed). Nothing is written to the device.
# Usage: sudo ./02-observe.sh <label> [seconds=90]     e.g.  sudo ./02-observe.sh normal-mode 90
# NOTE: this logs every key you type on this keyboard during the window. Do not type passwords.
set -u
LABEL=${1:?usage: $0 <label> [seconds]}; SECS=${2:-90}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$HERE/out/$(date +%Y%m%d-%H%M%S)-observe-$LABEL; mkdir -p "$OUT"
DEV=$(for d in /sys/bus/usb/devices/*; do [ -f "$d/idProduct" ] || continue; [ "$(cat "$d/idVendor")" = 1532 ] && [ "$(cat "$d/idProduct")" = 028d ] && basename "$d"; done | head -1)
[ -n "$DEV" ] || { echo "keyboard not found"; exit 1; }
EVENTS=(); HIDRAWS=()
for i in 0 1 2 3 4; do
  H=$(ls -d /sys/bus/usb/devices/$DEV:1.$i/0003:* 2>/dev/null | head -1); [ -n "$H" ] || continue
  HR=$(ls "$H/hidraw" 2>/dev/null | head -1); [ -n "$HR" ] && HIDRAWS+=("/dev/$HR")
  for e in $(ls -d "$H"/input/input*/event* 2>/dev/null | xargs -rn1 basename); do EVENTS+=("/dev/input/$e"); echo "IF$i $e" >> "$OUT/nodes.txt"; done
  [ -n "$HR" ] && echo "IF$i $HR" >> "$OUT/nodes.txt"
done
cat "$OUT/nodes.txt"
pids=()
HIDREC=$(command -v hid-recorder || ls "$(getent passwd "${SUDO_USER:-$USER}" | cut -d: -f6)/.local/bin/hid-recorder" 2>/dev/null)
if command -v evtest >/dev/null; then
  for e in "${EVENTS[@]}"; do timeout -s INT "$SECS" stdbuf -oL evtest "$e" > "$OUT/$(basename "$e").evtest.txt" 2>&1 & pids+=($!); done
else echo "!! evtest not installed (sudo apt install evtest); evdev capture skipped"; fi
if [ -n "$HIDREC" ]; then
  PYTHONUNBUFFERED=1 timeout -s INT "$SECS" "$HIDREC" "${HIDRAWS[@]}" > "$OUT/hid-recorder.txt" 2>&1 & pids+=($!)
else echo "!! hid-recorder not installed (uv tool install hid-tools); raw HID capture skipped"; fi
[ ${#pids[@]} -gt 0 ] || { echo "nothing to capture with"; exit 1; }
echo; echo ">>> capturing for $SECS s into $OUT"
if [ "${CHECKLIST:-full}" = mkeys ]; then
echo ">>> press M1, wait 5 s, M2, wait 5 s, M3, M4, M5 (5 s apart). Nothing else. Keep a text editor focused-off (click the desktop)."
else
echo ">>> now press, one at a time with a 2 s pause, saying the name out loud or noting the clock:"
echo "    M1 M2 M3 M4 M5 | dial CW x3 | dial CCW x3 | dial press | side key 1 2 3 | media prev/play/next | mute vol- vol+ | Fn+F11 Fn+F12 | plain 'a'"
echo "    NEVER press Fn+F9: it starts on-board macro recording (persistent state)."
fi
wait
echo ">>> done. Files:"; ls -l "$OUT"
echo ">>> quick summary (event counts per node):"
for f in "$OUT"/*.evtest.txt; do printf '%-28s %s events\n' "$(basename "$f")" "$(grep -c '^Event:' "$f")"; done 2>/dev/null
[ -n "${SUDO_USER:-}" ] && chown -R "$SUDO_USER": "$OUT"
[ -f "$OUT/hid-recorder.txt" ] && printf '%-28s %s reports\n' hid-recorder.txt "$(grep -c '^E:' "$OUT/hid-recorder.txt")"
