# Razer BlackWidow V4 Pro on Linux: probe plan

Goal: make the M1–M5 macro keys and the Command Dial (rotate + press) programmable on
this Ubuntu box, via our own driver/daemon, without risking the keyboard's firmware or
the system's only keyboard. RGB is not a goal.

This is a *verification* program, not open-ended reverse engineering. Most of the
protocol is already documented by OpenRazer and by the author of `detentd`
(see §2.2). Each probe confirms one claim on *this* unit and firmware, in order of
increasing risk, and only then do we commit to an architecture (§6).

Status board (update as you go):

| Phase | What | Risk | Status |
|---|---|---|---|
| P0 | Setup: tools, log dirs, fallback input | none | **done 2026-10-23** |
| P1 | Passive USB/HID enumeration | none | **done 2026-10-23**, see `probes/out/*-enumerate/` |
| P2 | Observe events in *normal* mode | none | **done 2026-10-23**, incl. P2b per-M-key. See `probes/out/20261023-011524-observe-normal-mode/NOTES.md` |
| P3 | Prior-art review | none | **done**, summarized in §2.2 |
| P4 | Vendor channel, read-only commands | very low | **done 2026-10-23**, fw v1.4, all OK |
| P5 | Driver-mode session (volatile) + observe | low | **done 2026-10-23**, all codes match; clean revert. See `probes/out/20261023-013027-observe-driver-mode/NOTES.md` |
| P6 | Robustness: resume, replug, idle, Fn layer | low | **mostly done**: suspend N/A (suspend.target masked), Fn layer answered in P5, replug-reset still to confirm |
| P7 | Decision gate | — | **done 2026-10-23**: option B (module-free userspace daemon, hidraw + uinput, X11). Built as `bwv4d`, see README.md |

---

## 1. Goal and non-goals

Want:
- M1–M5 produce distinct, bindable events that a daemon or remapper can act on.
- Dial rotation and dial press produce distinct events; press cycles host-defined
  modes (volume, app switch, …), rotation acts per mode.
- Survives replug, suspend/resume, and login without manual steps.
- Normal typing is never degraded, and a crash of our code never takes the keyboard
  or the desktop with it.

Don't want (now):
- RGB control. (Note for later: interface 4 is a standard HID **LampArray**, usage
  page 0x59. If RGB ever matters it needs no vendor protocol at all.)
- Anything that writes persistent on-board state (profiles, stored macros, LED
  settings with varstore=STORE).
- Anything touching firmware update or bootloader paths.

## 2. What we already know

### 2.1 This machine (P1, 2026-10-23)

Device `1532:028d`, bcdDevice 1.01, USB 2.0 high speed, bus-powered, 5 HID interfaces,
all bound to `hid-generic`. OpenRazer is **not** installed (`razerkbd` absent). This is
the pristine state we want for probing.

| IF | Protocol | hidraw | event node(s) (by-id suffix) | Report descriptor says | Role |
|---|---|---|---|---|---|
| 0 | boot keyboard | hidraw4 | event9 (`-event-kbd`) | 8-byte boot report, 3 LEDs | BIOS/boot keyboard |
| 1 | keyboard | hidraw5 | event10 (`-if01-event-kbd`), event11 (`-event-if01`) | ID1 NKRO 160-key bitmap; ID2 Consumer (0..0x23C, 16-bit, incl. volume/mute/AC Pan/zoom); ID3 System control; **ID4, ID5: 21-byte arrays, GD usage 0x03 (Razer proprietary)** | main keyboard + media + **raw special-key channel** |
| 2 | boot mouse | hidraw6 | event12 (`-if02-event-mouse`) | 5 buttons, pad byte, **byte2 = Consumer AC Pan**, **byte3 = GD Wheel**, X/Y 16-bit | dial rotation and volume roller |
| 3 | none | hidraw7 | — | one 90-byte Feature report, usage page 0xFF00 | **vendor control channel** |
| 4 | keyboard | hidraw8 | — | HID LampArray (page 0x59), feature reports 1–6 | per-key RGB (standard) |

event11 was created by hid-generic for the ID4/ID5 collections and advertises only
`ABS_MISC`. Whether it actually carries the special-key codes is a P2/P5 question
(it matters for privacy, see §6).

Numbers above (hidraw4–8, event9–12) change across replug/boot. The scripts resolve
nodes dynamically; use the `/dev/input/by-id/usb-Razer_Razer_BlackWidow_V4_Pro-*`
names when typing commands by hand.

Full dumps: `probes/out/baseline/` (decoded report descriptors) and
`probes/out/<ts>-enumerate/`.

### 2.2 Prior art (P3)

**OpenRazer** `driver/razerkbd_driver.c`, `razercommon.c`, `razerchromacommon.c`
(master, fetched 2026-10-23), for `USB_DEVICE_ID_RAZER_BLACKWIDOW_V4_PRO 0x028D`:

- Vendor channel = SET_REPORT/GET_REPORT, wValue 0x0300, **wIndex 3**, 90 bytes,
  **transaction id 0x1F**, 600 µs between set and get. Matches our IF3 exactly.
- Device mode: class 0x00, cmd 0x04, args `[mode, 0x00]`, mode 0x00 = normal,
  0x03 = driver. OpenRazer **blocks mode 0x02** on purpose. The kernel module sets
  mode 0x00 at bind; the daemon later sets 0x03. Mode is volatile (replug resets).
- In driver mode, special keys arrive on IF1 as **report ID 0x04, 22 bytes** (with
  the ID byte), payload = list of codes: `0x20–0x24` = M1–M5, `0x60` = dial press,
  `0x63–0x65` = the three side keys, `0x50–0x55` = volume/media, `0x01` = Fn.
  OpenRazer rewrites these into F13–F17 / F24 / F18–F20.
- On IF2 OpenRazer remaps **GD Wheel → ABS_VOLUME** (the volume roller). The dial's
  AC Pan is left alone.
- Two real crashes were caused by *the kernel-side parser* of these raw reports
  (issues #2264 "X11 crash", #2377 "Kubuntu freezes on M1"; fixed by PR #2452 in
  2025). Lesson for us: **parse raw reports in userspace**, never in a module.

**OpenRazer issue #2228** plus the attached `protocol-reference.md` /
`supplement-to-previous-captures.md` (summary):

- In driver mode: press → `04 60 …` / release `04 00 …` on IF1; rotation → one 8-byte
  report per detent on IF2 with byte2 = `01` (CW) / `FF` (CCW) = AC Pan, which plain
  hid-input already delivers as `REL_HWHEEL ±1` (+ `REL_HWHEEL_HI_RES ±120`).
  No repeats on hold, no long-press report. **No mode logic in driver mode**: the
  dial LED never changes, rotation reports are identical regardless of presses.
- In *normal* mode the firmware itself is modal: pressing the dial cycles LED-colored
  modes and rotation is reported differently per mode (arrow keys, on-device
  brightness, vertical wheel, Alt-Tab sequences). This is what P2 will see.
- Gotcha: KDE binds F13 (`XF86Tools`) to System Settings by default, so M1 under
  OpenRazer "opens settings". Not a device problem.

**detentd** (gitlab.com/dustinwidmann/detentd, alpha, 2026): a userspace daemon
implementing exactly the Synapse-style dial policy on top of OpenRazer: grabs the IF2
node, listens for F24 without grabbing the keyboard nodes, re-injects via uinput,
sets the dial LED color per mode through OpenRazer/OpenRGB, D-Bus interface, udev
`uaccess` rule (deliberately *not* the `input` group).

Implication: the open questions are no longer "what does the device send" but
"does *this* unit behave as documented, and do we build on OpenRazer or go
module-free". That's what P2–P6 settle.

## 3. What can actually go wrong, and the rules that follow

| Risk | Reality | Rule |
|---|---|---|
| Bricking the firmware | Only firmware-update / bootloader commands can. We never send those, and we don't know them, which is the point. A malformed or wrong report gets status FAILURE/NOT_SUPPORTED, nothing more. | Only allow-listed commands (`razerctl.py list`). No `--raw` mode exists. Never send mode 0x02. Never send varstore=STORE. Never add a command we haven't seen OpenRazer send to this PID. |
| Persisting a bad on-board state | Only profile/macro/LED *store* commands persist. Device mode does not. | Out of scope until a separate, later plan. Replug is the universal reset for everything we do here. |
| Losing the only keyboard | Normal typing keeps working in driver mode (IF0 and IF1 ID1 are untouched; every OpenRazer user runs that way). The real risk is a *grabbing* daemon of ours misbehaving later. | Driver-mode experiments only via `razerctl.py session N`, which auto-reverts on timer, Ctrl-C or error. Keep an SSH session from the laptop/phone open during P5/P6. Never `EVIOCGRAB` the keyboard nodes; only ever grab the IF2 (mouse) node, and only in the daemon phase with a watchdog. |
| Crashing the kernel or X11 | Happened to others via OpenRazer's in-kernel parser. | All parsing in userspace. Don't install OpenRazer during probing. |
| Confounded observations | Two writers on IF3, or OpenRazer's bind-time mode set, or usb autosuspend. | One tool at a time on IF3. OpenRazer stays uninstalled through P6. Note `power/control` (currently `on`, good). |
| Privacy of a future daemon | Reading hidraw5 means seeing every keystroke. | Prefer event11 (ABS_MISC) or a dedicated path if P5 shows the codes there; otherwise minimize and document. No `input` group membership; use `uaccess` udev rules. |
| Unreadable experiments | Memory is not evidence. | Every probe writes to `probes/out/<timestamp>-<label>/`. `razerctl.py` appends every request/response to `probes/out/razerctl.log.jsonl`. |

Pre-flight before any **active** probe (P4+):
1. Previous probe reverted (`get-mode` says 00) or keyboard replugged.
2. Nothing else talks to the keyboard: `lsmod | grep razer` empty, no detentd, no
   OpenRGB, no input-remapper running.
3. A second way in exists (SSH from another device) or you accept replug as recovery.
4. You know the stop rules below.

Stop rules:
- An allow-listed command returns anything but OK **twice** → stop. Do not vary bytes.
  Re-read the OpenRazer source for this PID and compare with `--dry-run` output.
- Anything unexpected (LEDs change, a key stops working, the device re-enumerates) →
  replug, record what happened in the probe's notes file, stop for the day.
- Never press **Fn+F9**. It starts on-the-fly macro recording, which writes persistent
  on-board state if followed by an M key. (Learned in P2; it was on the original checklist.)
- Never "try a few more values" on the vendor channel. Fishing in command space is how
  you find the one command you didn't want.

## 4. Probe card format

Each probe records, in `probes/out/<ts>-<label>/NOTES.md`:
`question · hypothesis · method · expected · observed · verdict · follow-ups`.
Scripts: `probes/01-enumerate.sh`, `probes/02-observe.sh <label> [secs]`,
`probes/tools/razerctl.py`, `probes/tools/rdesc_decode.py`.

## P7 answers (2026-10-23)

1. M keys need driver mode? **Yes.** Normal mode replays stored macros (M5 holds Backspace).
2. event11 carries ID4 codes? **No.** Only hidraw5 (which also carries all typing), or HID-BPF.
3. Fn combos in driver mode? **No.** The firmware Fn layer is gone: Fn+F11/F12 send plain F11/F12 plus
   ID4 `01`; media keys, mute and side keys arrive only as ID4 codes; the volume roller becomes REL_WHEEL.
   The daemon must re-implement all of them.
4. Driver mode survives suspend? **Not applicable here:** suspend.target is masked on this machine.
   Re-apply on hotplug anyway.
5. Anything in normal mode worth keeping? Only on-device keyboard brightness (Fn+F11/F12, dial brightness mode).

## 5. The probes

### P0 Setup (no device interaction)

```bash
sudo apt install evtest libinput-tools python3-evdev usbutils
uv tool install hid-tools            # provides hid-recorder and hid-decode (uv is installed)
# optional, for whole-bus captures: sudo apt install tshark && sudo modprobe usbmon
mkdir -p probes/out
```
Optional, lets `razerctl.py` run without sudo later (seat-scoped, not the `input` group):
```
# /etc/udev/rules.d/70-razer-bwv4pro.rules   (must sort below 73-seat-late.rules)
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="1532", ATTRS{idProduct}=="028d", TAG+="uaccess"
```
Then `sudo udevadm control --reload && sudo udevadm trigger`.
Fallback input: there is no second real keyboard on this machine (the Basilisk and
BlackShark expose "keyboard" interfaces but aren't typing devices). Have SSH ready.

### P1 Passive enumeration — done

Re-run `probes/01-enumerate.sh` after any firmware change or when something looks
off, and diff `summary.txt` against the first run.

### P2 Observe everything in normal mode (zero risk)

Question: what does the firmware emit today, on which node, for each control?
Method: `sudo probes/02-observe.sh normal-mode 120`, then work through the checklist
it prints, one control at a time, 2 s apart. Repeat once for the dial press cycle
(press, rotate 3 clicks, press, rotate, … until the LED color returns to the start).

Hypotheses (from §2.2):

| Control | Expect in normal mode | Where |
|---|---|---|
| M1–M5 | nothing at all (firmware plays on-board macros; none assigned) *or* an ID4 report | event10/11, hidraw5 |
| Dial rotate | depends on current firmware mode: Consumer volume, GD Wheel (byte3 of IF2), arrow keys, Alt-Tab, or on-device brightness with no host event | event10, event12, hidraw5/6 |
| Dial press | no host event; LED color changes; subsequent rotation changes meaning | — (observe LED) |
| Side keys 1–3 | Consumer media usages | event10 |
| Volume roller + mute | Consumer volume up/down/mute or IF2 GD Wheel | event10 / event12 |
| Fn+F1..F12 | media/consumer usages | event10 |
| plain `a` | KEY_A on event9 **and** event10? (boot vs NKRO interface; expect only one) | event9/10 |

Verdict criteria: a filled-in table. Specifically answer: (a) do M-keys emit
*anything* in normal mode (if yes, a driver might not even need driver mode for
them); (b) does event11 ever fire, and with what value; (c) what the dial does
per firmware mode.

**P2 result (2026-10-23):** M keys replay on-board macros stored from Windows (stored from
Synapse on macOS: M1, M2 = Ctrl+Cmd; M3, M4 empty; **M5 = press A, press Backspace,
never released**, which holds Backspace down. Avoid M5 in normal mode.); side keys send
Win+Shift+S / Win+G / Win+Tab. All firmware output goes out on **IF0**; physical keys on
IF1. Dial was in on-device brightness mode (IF1 ID5 `05 08 <level>`), press is silent.
Volume roller = Consumer vol+/vol- on IF1. Fn = ID4 `04 01` even in normal mode.
**event11 and event12 emitted nothing.** So: driver mode is required for M keys, and a
daemon needs hidraw5 or HID-BPF for ID4 codes. Full table in that run's NOTES.md.

### P3 Prior-art review — done (§2.2)

Remaining optional reading: `detentd`'s `tools/` directory (their capture scripts) and
`docs/protocol-blackwidow-v4-pro.md`; OpenRazer daemon `hardware/keyboards.py` line
~31 (where driver mode is set) and PR #2452 (what the parser bug was).

### P4 Vendor channel, read-only (very low risk)

Question: can we talk to IF3 at all, with the OpenRazer parameters, from userspace
hidraw, and parse replies correctly?

1. `python3 probes/tools/razerctl.py --dry-run session` — prints the five request
   frames. Check by eye against §A: byte1 = 1f, bytes 5..7 = size/class/id, crc at
   byte 88. (Already verified: `00 1f 00 00 00 02 00 81 … 83 00` for fw-version.)
2. `sudo python3 probes/tools/razerctl.py fw-version` — expect status OK and a version
   consistent with bcdDevice 1.01 (likely `v1.1` or `v1.01`; a mismatch means we are
   parsing the wrong bytes, not that the device is odd).
3. `sudo python3 probes/tools/razerctl.py serial` — USB iSerial is empty on this unit,
   so this may legitimately return OK with an empty/odd string or NOT_SUPPORTED. Record.
4. `sudo python3 probes/tools/razerctl.py get-mode` — expect `00 00`.

Pass: three OK statuses, class/id echoed back, CRC verified by the tool, plausible
firmware version. Fail: BUSY forever or NOT_SUPPORTED → stop (stop rules). Do **not**
try other transaction ids by hand; if it comes to that, change `TRANSACTION_ID` in the
tool to `0xFF` (the generic one) once, rerun step 2, and record.

### P5 Driver-mode session (low risk, volatile, auto-reverting)

Question: in driver mode, does this unit emit what §2.2 says, and on which nodes?

Terminal A: `sudo python3 probes/tools/razerctl.py --yes session 180`
Terminal B (start right after A confirms driver mode): `sudo probes/02-observe.sh driver-mode 150`
Work through the same checklist as P2, plus: hold the dial 4 s; press dial then rotate
(expect *no* change in rotation reports and *no* LED change).

Expected:

| Control | Driver mode | Where |
|---|---|---|
| M1–M5 | `04 20..24 00…` press, `04 00…` release | hidraw5 (report ID 4); maybe event11 ABS_MISC |
| Dial press | `04 60 …` / `04 00 …`, single pair even when held | hidraw5 / event11 |
| Dial rotate | `00 00 01 00 00 00 00 00` CW, `… FF …` CCW; `REL_HWHEEL ±1` | hidraw6 / event12 |
| Side keys | `04 63/64/65` | hidraw5 |
| Volume roller | byte3 of IF2 = GD Wheel → `REL_WHEEL` on event12 (no OpenRazer to remap it) | hidraw6 / event12 |
| Fn | `04 01 …` appears; do Fn+F-key media functions still work? | hidraw5, event10 |
| plain typing | unchanged | event9/10 |

After the session ends, terminal A must print "restored normal mode". Then:
`sudo python3 probes/tools/razerctl.py get-mode` → `00`. Then replug and `get-mode`
again → `00` (confirms replug is a reset).

Pass: table filled, revert confirmed. Key answers to carry forward: whether event11
carries the ID4 codes (determines whether the daemon can avoid reading hidraw5), and
whether Fn combos survive driver mode (determines UX cost of driver mode).

### P6 Robustness (low risk; each is a short session)

- P6a Suspend/resume: enter driver mode (`session 600`), suspend the laptop/PC for a
  minute, resume, `get-mode` from a third terminal. Expect: unknown; OpenRazer users
  report needing re-apply. Outcome decides whether the daemon must re-send on resume.
- P6b Replug: already covered in P5. Record.
- P6c Idle decay: `session 1800` and do nothing for 30 min; `get-mode` at the end.
  Expect: still 03 (no keepalive known).
- P6d Interaction with libinput: in driver mode, does the desktop react to the dial
  (horizontal scroll in a browser) and to F-keys? Expect yes; this is what a daemon
  will have to grab or swallow.
- P6e USB autosuspend: set `power/control` to `auto` for the keyboard, repeat P5's dial
  test. If ghost events or lost events appear, the daemon ships a udev rule keeping it
  `on`. Reset to `on` afterwards.

### P7 Decision gate

Answer these from the NOTES files, then pick in §6:

1. Do M-keys need driver mode at all? (P2a)
2. Does event11 carry ID4 codes? (P5)
3. Do Fn combos still work in driver mode? (P5)
4. Does driver mode survive suspend? (P6a)
5. Is there anything in normal mode's firmware dial modes you'd rather keep (e.g. the
   on-device brightness mode)? (P2)

## 6. Architecture options (decide at P7)

| | A. OpenRazer + detentd | B. Own module-free daemon | C. HID-BPF + remapper |
|---|---|---|---|
| Code to write | ~0 (config only) | mode setter (done: razerctl core) + report parser + uinput emitter + dial state machine | a small BPF program rewriting report ID4 into keyboard usages, then keyd/input-remapper for policy |
| Kernel module | yes (DKMS, out-of-tree, had crash bugs) | no | no (BPF is sandboxed) |
| Privacy | daemon reads only F24 + IF2 | must read hidraw5 (all keys) unless event11 works | kernel rewrites; userspace sees keys only |
| Fragility | DKMS breaks on kernel updates (this machine runs 7.0) | low; depends only on hidraw/evdev/uinput ABIs | needs `udev-hid-bpf` tooling; newest path, least documented |
| Dial LED per mode | yes (via OpenRazer) | later, via LampArray on IF4 (standard HID, no vendor protocol) | later |

Recommendation, pending P5: **B**, with A as the fastest way to *experience* the end
state for an evening before committing (install, try, purge; it is reversible). B's
skeleton: udev `uaccess` rules for hidraw IF3, the IF1 special-key node and IF2;
a user-session daemon that (1) sets driver mode on start/hotplug/resume, (2) reads
ID4 codes from event11 or hidraw5, (3) grabs only the IF2 node and re-injects it
through uinput with per-mode semantics, (4) exposes mode state over D-Bus, (5) on
exit releases the grab and restores normal mode. Steal detentd's mode table and its
udev rule shape; don't steal its OpenRazer dependency.

## 7. Deferred, on purpose

- On-board profiles, stored macros, poll rate, game-mode LED: all vendor writes with
  persistence. Separate plan, separate allowlist, after B works.
- RGB / dial LED color: via IF4 LampArray (feature reports 1–6 decoded in
  `probes/out/baseline/if4-hidraw8.rdesc.txt`). Standard, but a different project.
- Wireless or dongle modes: this unit is wired; nothing here applies to a dongle.

## Appendix A: vendor report cheat sheet (IF3, 90 bytes)

```
byte 0     status      request: 0x00   response: 00 NEW 01 BUSY 02 OK 03 FAILURE 04 TIMEOUT 05 NOT_SUPPORTED
byte 1     transaction id   0x1F for this PID (OpenRazer)
bytes 2-3  remaining packets (BE)  0
byte 4     protocol type    0
byte 5     data size        per command
byte 6     command class
byte 7     command id
bytes 8-87 arguments (80)
byte 88    crc = XOR of bytes 2..87
byte 89    reserved 0
```
Transport: hidraw `HIDIOCSFEATURE` then `HIDIOCGFEATURE`, buffer = 1 report-ID byte (0)
+ 90 bytes; kernel turns that into SET_REPORT/GET_REPORT feature 0x0300 on wIndex 3.

| name | class | id | size | args | frame (dry-run) |
|---|---|---|---|---|---|
| fw-version | 00 | 81 | 02 | — | `00 1f 00 00 00 02 00 81 … crc 83` |
| serial | 00 | 82 | 16 | — | `… 16 00 82 … crc 94` |
| get-mode | 00 | 84 | 02 | — | `… 02 00 84 … crc 86` |
| set-mode-normal | 00 | 04 | 02 | 00 00 | `… 02 00 04 00 00 … crc 06` |
| set-mode-driver | 00 | 04 | 02 | 03 00 | `… 02 00 04 03 00 … crc 05` |

## Appendix B: special-key codes in report ID 4 (driver mode, from OpenRazer)

`01` Fn · `20..24` M1..M5 · `50` vol- · `51` vol+ · `52` mute · `53` next · `54` prev ·
`55` play/pause · `60` dial press · `63 64 65` side keys 1..3. Press = code present,
release = report with zeros. Rotation is never here; it is IF2 byte 2.

## Appendix C: files

```
docs/PROBE_PLAN.md                 this file
probes/01-enumerate.sh             P1, read-only, no root
probes/02-observe.sh               P2/P5 capture (evtest + hid-recorder), root
probes/tools/razerctl.py           P4/P5 allow-listed vendor tool, root (or udev rule)
probes/tools/rdesc_decode.py       HID report-descriptor decoder, no deps
probes/out/baseline/               report descriptors from 2026-10-23
probes/out/<ts>-*/                 one directory per probe run; add NOTES.md to each
probes/out/razerctl.log.jsonl      every vendor request/response ever sent (local only: holds the serial)

Raw captures (hid-recorder.txt, *.evtest.txt) contain whatever was typed during a capture, so they are
kept out of the public repository by .gitignore. The NOTES.md in each run directory summarizes them.
```

## Appendix D: references

- OpenRazer driver sources (master): https://github.com/openrazer/openrazer/tree/master/driver
- Issue #2228, dial in driver mode, with `protocol-reference.md` and supplement attached:
  https://github.com/openrazer/openrazer/issues/2228
- Issue #2264 (X11 crash) and #2377 (freeze on M1), fixed by PR #2452:
  https://github.com/openrazer/openrazer/issues/2377
- detentd: https://gitlab.com/dustinwidmann/detentd
- HID usage tables: Consumer page 0x0C (AC Pan 0x238), Lighting and Illumination page 0x59
