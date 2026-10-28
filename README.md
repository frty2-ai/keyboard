# keyboard: Command Dial and macro keys for the Razer BlackWidow V4 Pro on Linux

[![CI](https://github.com/frty2-ai/keyboard/actions/workflows/ci.yml/badge.svg)](https://github.com/frty2-ai/keyboard/actions/workflows/ci.yml)
[![License: GPL-2.0-or-later](https://img.shields.io/badge/license-GPL--2.0--or--later-blue.svg)](LICENSE)

`bwv4d` is a small userspace daemon, written in C, that makes the M1-M5 keys, the three side keys and the
Command Dial of a wired **Razer BlackWidow V4 Pro** (USB `1532:028d`) programmable on Linux/X11. It needs no
kernel module and no Razer software. It shows a quiet corner popup instead of desktop notifications.

- **Small:** one 70 KB binary, about 9 MB resident (mostly shared X11/cairo libraries), 0% CPU when idle.
  It sleeps in a single `epoll_wait` until the keyboard, a timer, a hotplug event or the X server wakes it.
- **Safe to stop:** stopping, crashing or unplugging always returns the keyboard to its normal firmware mode.
  It never writes persistent settings to the keyboard and never touches firmware.
- **No kernel code:** it talks to the keyboard through `hidraw`, `evdev` and `uinput` only, so kernel updates
  don't break it.

How every protocol detail was verified on real hardware, step by step and lowest risk first:
[docs/PROBE_PLAN.md](docs/PROBE_PLAN.md).

> **Status: early (0.1).** Tested on one wired BlackWidow V4 Pro, firmware v1.4, Ubuntu 24.04 (GNOME on X11),
> kernel 7.0. Other Razer keyboards with a Command Dial probably need only a few changes. See
> [Other keyboards](#other-keyboards).

## Requirements

- Razer BlackWidow V4 Pro, wired (`lsusb` shows `1532:028d`)
- An **X11** session (app awareness and the popup use X11; Wayland is not supported yet)
- systemd user session, udev
- **OpenRazer not loaded.** Its `razerkbd` module also switches the keyboard's mode; run one or the other.
- Build dependencies (Debian/Ubuntu):

  ```bash
  sudo apt install build-essential pkg-config libx11-dev libxext-dev libxrandr-dev libxfixes-dev libcairo2-dev
  ```

## Install

```bash
git clone https://github.com/frty2-ai/keyboard.git
cd keyboard
./install.sh          # builds, installs to ~/.local, asks for sudo once for the udev rule, starts the service
```

The installer:

1. installs `bwv4d` to `~/.local/bin` and the example config to `~/.config/bwv4d/config` (if you don't have one yet)
2. installs a systemd **user** service, `bwv4d.service`
3. adds six private keybindings to VS Code's `keybindings.json`, after backing it up (see [App awareness](#app-awareness))
4. installs a udev rule (with sudo) that gives your login seat access to exactly the nodes the daemon needs

Uninstall with `./uninstall.sh`. Stopping the service (`systemctl --user stop bwv4d`) always returns the keyboard
to normal mode. So does a crash, because the unit's `ExecStopPost` runs `bwv4d --restore`, and so does unplugging it.

## Default layout

| Control | Action |
|---|---|
| Side key 1 | Win+Shift+S (what the key sends in normal mode) |
| Side key 2 | Print (GNOME screenshot UI) |
| Side key 3 | Jump to the Skills profile; press again to go back |
| M1 | Select all + copy (app-aware) |
| M2 | Paste (app-aware) |
| M3 | Copy selection, raise the Claude desktop app, paste |
| M4 | Toggle between Claude and the window you came from |
| M5 | Raise VS Code; again = next VS Code window |
| Media keys, mute, volume roller | Same as the firmware does |
| Dial press | Next profile. Hold over 0.6 s = back to the first profile |

Each dial profile has its own colour on the dial LED and the popup. Pressing the dial cycles through them:

| Profile | Turning does |
|---|---|
| Skills | Types the current entry from your list (for example `/review `) into the focused app; each further click deletes it and types the next. Stop turning for 2.5 s, or type anything, and it stays. Only in the apps you list (default: Claude, VS Code); elsewhere the dial scrolls. |
| Clipboard | Opens a list of your last 30 copies (text and images, newest first) in the corner popup; each click moves the highlight. Nothing is typed while you browse. Stop turning for 1 s and the highlighted entry is pasted at the cursor. Any key cancels. |
| VS Code windows | Steps through VS Code windows; the popup shows the project name |
| App switcher | Alt+Tab / Alt+Shift+Tab, Alt held until 0.9 s after the last click |
| Tabs | Ctrl+PageDown / Ctrl+PageUp (VS Code, terminals, browsers) |

Any M key or side key can run a program or raise a window instead (`run_<key>`, `focus_<key>`). See
[packaging/config.example](packaging/config.example).

### App awareness

M1, M2 and M3 send what the focused app expects:

- **Terminals** (Ghostty, GNOME Terminal, kitty, ...): Ctrl+Shift+A/C/V
- **VS Code:** private chords (Ctrl+Alt+Shift+Insert/End/Home). The bindings in
  [packaging/vscode-keybindings.json](packaging/vscode-keybindings.json) map each chord to the editor or the
  terminal command, depending on `terminalFocus`. X11 can't tell VS Code's editor and terminal apart, but VS Code can.
- **Everything else:** Ctrl+A/C/V

## Configure

Edit `~/.config/bwv4d/config`: the skill list in dial order, the apps where the skill dial may type, timings,
colours, dial and roller direction, LED mode and key overrides. Every option is documented in
[packaging/config.example](packaging/config.example). Restart to apply:

```bash
systemctl --user restart bwv4d
```

## Operate

```bash
journalctl --user -u bwv4d -f         # logs (connect/disconnect, errors; never keystrokes)
bwv4d --demo                          # show every popup once, no keyboard access
bwv4d --restore                       # force the keyboard back to normal mode and firmware lighting
bwv4d --press m3                      # run a key's configured override without the keyboard
```

## How it works

```
 keyboard (USB 1532:028d)                     bwv4d (one thread, one epoll loop)              desktop
 ─────────────────────────                    ──────────────────────────────────              ───────
 IF1 hidraw  report 4: M keys, dial press,  ─▶ decode special-key codes        ─┐
             side keys, media keys              (typing: only "a key was hit")  │
 IF2 evdev   dial rotation, volume roller   ─▶ grabbed, read as REL_HWHEEL/WHEEL ├─▶ uinput keyboard + wheel
 IF3 hidraw  vendor channel                 ◀─ driver mode on connect,          │
                                                normal mode on exit              │
 IF4 hidraw  HID LampArray (standard)       ◀─ dial LED colour per profile      ─┘   X11: focused app, windows,
                                                                                      corner popup, clipboard
```

In its default ("normal") mode the keyboard handles the dial and M keys in firmware, and the host never sees
them as distinct keys. `bwv4d` switches it to **driver mode**, a volatile mode that resets on replug, in which
every special control sends its own code. The daemon then translates those codes, including the media keys and
the volume roller, which driver mode would otherwise leave dead. On exit it puts the keyboard back in normal mode.

The vendor channel is used for exactly two commands, set-mode-driver and set-mode-normal. The probe tool
[probes/tools/razerctl.py](probes/tools/razerctl.py) can send three more, all read-only. Neither has a "send raw
bytes" option. The dial LED uses the keyboard's **standard** HID LampArray interface, not a Razer protocol.

## Privacy

- Interface 1 carries every key you type. The daemon uses typing only to notice that a key was pressed, so it
  can finalize a typed skill or cancel clipboard browsing. It never stores, logs or forwards keystrokes.
- Clipboard history lives in RAM only and is gone when the daemon stops. Copies that password managers mark with
  `x-kde-passwordManagerHint` (KeePassXC and others) are never recorded.
- The udev rule grants the logged-in local seat (`uaccess`) access to interfaces 1, 3 and 4 and the dial's event
  node only. It does **not** add you to the `input` group, which would expose every input device.

## Other keyboards

Only `1532:028d` is supported today. Other Razer keyboards with a Command Dial (BlackWidow V4 Pro 75%, Huntsman V3
Pro, ...) very likely use the same scheme with a different product ID, transaction ID or key codes. The scripts in
[probes/](probes/) are how to find out safely: passive enumeration first, then observation, then a short,
self-reverting driver-mode session. If you have one of these keyboards, please open a
[device report](https://github.com/frty2-ai/keyboard/issues/new?template=device_report.yml).

## Known limitations

- X11 only.
- Keyboard brightness on Fn+F11/F12 doesn't work while the daemon runs (the firmware's Fn layer is off in driver mode).
- On-board macros saved from Razer Synapse stay in the keyboard and play again whenever the daemon is not running.
- The defaults assume the Claude desktop app and VS Code. Rebind M3-M5 in the config if you use something else.

## Contributing

Bug reports, device reports and pull requests are welcome. Please read [CONTRIBUTING.md](CONTRIBUTING.md),
especially the safety rules for anything that talks to the vendor channel. Report security issues privately as
described in [SECURITY.md](SECURITY.md). This project follows the [Contributor Covenant](CODE_OF_CONDUCT.md).

## Credits

All code in this repository was written from scratch. It would have taken far longer without the people who
documented this keyboard first, above all the **[OpenRazer](https://github.com/openrazer/openrazer)** project,
whose driver sources taught us the vendor channel, the device-mode command and the special-key code table. The
Command Dial's driver-mode protocol came from the captures and write-up that
[thedustinwidmann-lang](https://github.com/thedustinwidmann-lang) posted in
[OpenRazer issue #2228](https://github.com/openrazer/openrazer/issues/2228), and several design choices came from
the [detentd](https://gitlab.com/dustinwidmann/detentd) daemon. The full list, and exactly what we learned from
each, is in [ACKNOWLEDGEMENTS.md](ACKNOWLEDGEMENTS.md).

## License

Copyright © 2026 frty2.ai and contributors.

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
Public License as published by the Free Software Foundation, either version 2 of the License, or (at your option)
any later version. See [LICENSE](LICENSE).

## Trademarks

Razer, BlackWidow, Synapse and Chroma are trademarks of Razer Inc. This project is not affiliated with, endorsed
by or sponsored by Razer Inc. The names are used only to say which hardware this software works with.
