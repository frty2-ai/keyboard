# P2 normal-mode observation, 2026-10-23 01:15

Question: what does the firmware emit in normal mode (device mode 00), per control, per node?
Method: 02-observe.sh normal-mode 120, checklist pressed in order. hid-recorder t=0 is about evdev 1792698324.5.

## Observed (raw reports from hid-recorder; D:0 = IF0 hidraw4, D:1 = IF1 hidraw5)

| t (s) | IF | report | meaning | likely control |
|---|---|---|---|---|
| 0.0 (already present) | 0 | `00 00 04 e0 2a 00 00 00` | A + LeftCtrl(as array usage) + Backspace HELD | stale state from an earlier on-board macro |
| 2.19 / 2.30 | 0 | mod 01 + `e3` | Ctrl+Win tap | M1 (P2b) |
| 4.93 / 5.04 | 0 | mod 01 + `e3` | Ctrl+Win tap | M2 (P2b) |
| 13.87 - 14.86 | 0 | `04 2a` -> `2a` -> `2a 04` -> `2a 04 e0` -> `04 e0` -> `04 e0 2a` | macro replay of Ctrl+A / Backspace, ends with all three held | M5 (confirmed in P2b) |
| (none) | | | | M3, M4 silent (P2b) |
| 18.3 - 24.9 | 1 | ID5 `05 08 51/60/70/80/70/60/51` | on-device brightness level reports (dial in brightness mode) | dial CW x3, CCW x3 |
| (none) | | | dial press: no host report | dial press |
| 29.38 | 0 | mod 0a + `16` | Win+Shift+S | side key 1 |
| 31.39 | 0 | mod 08 + `0a` | Win+G | side key 2 |
| 33.89 | 0 | mod 08 + `2b` | Win+Tab | side key 3 |
| 41.8 - 44.3 | 1 | ID2 consumer b6 / cd / b5 | prev / play / next | media keys |
| 46.3 | 1 | ID2 consumer e2 | mute | mute |
| 50.2 - 52.9 | 1 | ID2 consumer e9 / ea alternating | vol+ / vol- per detent | volume roller |
| 62.7 - 71.3 | 1 | ID4 `04 01` / `04 00` x4 | Fn press/release (ID4 used even in normal mode) | Fn |
| 66.79 | 0 | all zero | stuck A/Ctrl/Backspace finally released | during Fn+F10 |
| 69.08 | 1 | ID5 `05 08 44` | brightness down | Fn+F11 |
| 70.94 | 1 | ID5 `05 08 51` | brightness up | Fn+F12 |
| (none) | | | Fn+F9 (on-the-fly macro RECORD), Fn+F10 (game mode): on-device | |
| 75.26 | 1 | ID1 NKRO bit 0x04 | 'a' | plain a |

evdev: event9 (IF0) carried all firmware-generated keys, including 417 Backspace autorepeats from
the stuck state (about 14.9 s to 66.8 s). event10 (IF1) carried media + plain typing. event11 (ABS_MISC)
and event12 (IF2 mouse) emitted NOTHING.

## Verdicts
- Physical keys go out on IF1 (NKRO). Firmware-generated output (on-board macros, side-key shortcuts) goes
  out on IF0 (boot interface), with modifiers sometimes encoded as array usages (e0) instead of mod bits.
- This keyboard has on-board macros/bindings stored (Ctrl+Win x2, a Ctrl+A/Backspace macro that leaves keys
  held, Win+Shift+S / Win+G / Win+Tab on the side keys). Almost certainly configured from Windows Synapse.
  Persistent, so they will keep firing in normal mode on Linux.
- Normal mode: M keys do NOT emit distinct codes; they replay whatever is stored. -> driver mode needed.
- Dial in normal mode is firmware-modal; it was in brightness mode, which never reaches the host as input.
- event11 is useless (ID4/ID5 produce no evdev events). A daemon must read ID4 from hidraw5 (= sees all
  keys) or use HID-BPF. Privacy decision moves to P7.
- Fn+F9 starts on-the-fly macro recording, which can write PERSISTENT on-board state. Removed from the
  checklist. A recording may have been left pending; replug cancels it (nothing saved without choosing an M key).

## Follow-ups
- Replug (clears stuck IF0 state and any pending recording).
- P2b: press M1..M5 one at a time, 5 s apart, to pin which key produces which output.
