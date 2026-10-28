# P4 + P5 vendor channel and driver-mode session, 2026-10-23 01:30

## P4 read-only (razerctl.log.jsonl, 01:30:00)
fw-version OK -> v1.4 (args 01 04; USB bcdDevice says 1.01, so they are different counters).
serial OK (value kept out of notes). get-mode OK -> 00 00. CRC and class/id echo verified on every reply.
Transport confirmed: hidraw HIDIOCSFEATURE/GFEATURE on IF3, transaction id 0x1F. No retries needed.

## P5 session (01:30:19 -> 01:33:19)
set-mode-driver OK, get-mode -> 03 00, 180 s, set-mode-normal OK, get-mode -> 00 00. Clean revert.

| Control | Raw report | What the desktop got (evdev) |
|---|---|---|
| M1..M5 | IF1 ID4 `20` `21` `22` `23` `24`, then release | nothing (no on-board macros played) |
| Dial CW / CCW | IF2 byte2 = +1 / -1 per detent | event12 REL_HWHEEL +-1 (+ HI_RES +-120) |
| Dial press | IF1 ID4 `60`, single press/release | nothing |
| Side keys 1..3 | IF1 ID4 `63` `64` `65` | nothing |
| Prev / play / next | IF1 ID4 `54` `55` `53` | nothing (media keys dead) |
| Mute | IF1 ID4 `52` | nothing |
| Volume roller | IF2 byte3 = +-1 per detent | event12 REL_WHEEL +-1 (vertical scroll!) |
| Fn | IF1 ID4 `01` while held | nothing |
| Fn+F11 / Fn+F12 | ID4 `01` + NKRO F11 / F12 | KEY_F11 / KEY_F12 (firmware Fn layer gone; brightness unchanged) |
| plain a | NKRO A | KEY_A |

event9 (IF0) and event11 stayed silent. Every code matches OpenRazer's table and the #2228 protocol reference.

## Verdicts
- Hypothesis confirmed on this unit: driver mode turns every special control into a distinct raw code and bypasses
  the stored macros (M5 no longer holds Backspace).
- Cost of driver mode without a daemon: media keys, mute, M keys, side keys and dial press produce nothing; the
  volume roller scrolls vertically; Fn combos send plain F-keys. So a daemon must translate ALL of ID4 + IF2,
  not just the M keys and dial. Driver mode must never be left on without the daemon running.
- ID4 codes only reach userspace via hidraw5 (event11 never fires). That node also carries every keystroke.
- Privacy note: this capture also recorded about 60 s of unrelated typing (t=80..135 s) in hid-recorder.txt and
  event10.evtest.txt. Not needed for analysis; delete or trim before sharing these files.

## Still open (P6)
suspend/resume persistence of driver mode, idle persistence, replug reset (do now: replug, get-mode).
