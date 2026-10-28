# P2b per-M-key observation, 2026-10-23 01:22 (normal mode, after replug)

Pressed M5, M4, M3, M2, M1 in that order, about 4.5 s apart. hid-recorder t=0 is about evdev 1792698743.23.
User reports the macros were configured earlier with Razer Synapse on macOS (stored on-board).

| t (s) | IF1 NKRO report | evdev (event10) | key |
|---|---|---|---|
| 0.43 | A down | KEY_A 1 | M5 |
| 0.99 | A + Backspace down, never released | KEY_BACKSPACE 1, autorepeat x382 until 13.9 s | M5 |
| ~5, ~9 | nothing | nothing | M4, M3 (empty) |
| 13.90 / 14.06 | LCtrl tap (A+Bksp still held) | KEY_LEFTCTRL 1/0 | M2 |
| 17.63 / 17.76 | LCtrl tap | KEY_LEFTCTRL 1/0 | M1 |

A and Backspace were still held when the capture ended; the next physical key press clears them.

## Verdicts
- M5 = on-board macro "press A, press Backspace" with no releases (a truncated recording). In normal mode
  it holds Backspace down: destructive in any text field. Avoid M5 until driver mode is confirmed to bypass
  on-board macros (P5).
- M1, M2 = Ctrl+Cmd(Win) macro (macOS recording). In P2 the firmware sent them on IF0 as mod LCtrl + array
  usage 0xE3 (LWin). Here they went out on IF1 NKRO, whose bitmap only covers usages 0x00-0xA0, so the Win
  part was silently dropped and only Ctrl arrived.
- M3, M4 = empty.
- Corrects P2's guess: the Ctrl+A/Backspace macro there was M5, not M3 (timings line up: 2.2, 4.9, [7.5],
  [10], 13.9 s).
- Open question (low priority, driver mode makes it moot): firmware sent macro output on IF0 in P2 but on
  IF1 after a replug. Unknown trigger.
