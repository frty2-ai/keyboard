# Contributing

Thanks for helping. Bug reports, device reports for other Razer keyboards, documentation fixes and code are all
welcome.

## Ground rules

1. **Never risk someone's only keyboard.** This software runs on the device people type their passwords with.
2. **Volatile only.** The daemon may change settings that a replug resets, such as device mode or host-controlled
   lighting. It must not write anything the keyboard stores (profiles, on-board macros, `varstore=STORE` lighting),
   and it must not touch firmware-update or bootloader commands.
3. **Allow-listed vendor commands.** A new vendor-channel command needs (a) a public source that shows it, such as
   OpenRazer sending it to the same product ID, and (b) a probe-plan entry showing it was tried on real hardware
   with `probes/tools/razerctl.py --dry-run` first. No "send raw bytes" mode, ever.
4. **Always restore.** Every path out (normal exit, signal, crash via `ExecStopPost`, unplug) must leave the keyboard
   in normal mode.
5. **Keystrokes are never stored, logged or forwarded**, not even in debug output.
6. **Stay small.** The daemon is resident all day: one thread, event-driven (`epoll`), no polling loops, no new
   heavy dependencies. Mention resident memory (`ps -o rss`) before and after in performance-sensitive PRs.

## Build and test

```bash
sudo apt install build-essential pkg-config libx11-dev libxext-dev libxrandr-dev libxfixes-dev libcairo2-dev
make
./bwv4d --demo          # shows every popup, needs X11 but no keyboard
```

To test against the keyboard without installing, stop the service first (`systemctl --user stop bwv4d`), then run
`BWV4D_DEBUG=1 ./bwv4d` in a terminal. Ctrl-C restores normal mode. Keep a second way to type (SSH, another
keyboard) at hand while you work on input handling.

CI builds with GCC and Clang with `-Werror`, so please keep the build warning-free.

## Code style

- C (gnu11), tabs, about 120 columns, short comments that say *why*. Match the surrounding code.
- Python tools: standard library only, so they run on a bare system as root.
- Every source file starts with SPDX headers:

  ```c
  /* SPDX-FileCopyrightText: 2026 frty2.ai and contributors
   * SPDX-License-Identifier: GPL-2.0-or-later */
  ```

## Adding another keyboard

Start with the probes, not the daemon:

1. `probes/01-enumerate.sh`: passive, read-only. Attach `summary.txt` and the `*.rdesc.txt` files to a
   [device report](https://github.com/frty2-ai/keyboard/issues/new?template=device_report.yml).
2. `probes/02-observe.sh`: passive capture while you press the checklist. **It records everything you type
   during the capture**, so don't type anything private, and share only the `NOTES.md` you write from it, never the raw
   `hid-recorder.txt` / `*.evtest.txt` (`.gitignore` keeps them out of commits).
3. A short `razerctl.py session` in driver mode, which reverts on its own.

Follow [docs/PROBE_PLAN.md](docs/PROBE_PLAN.md) for the order and the stop rules.

## Pull requests

- One topic per PR, with a description of what you tested and on which hardware and firmware.
- Sign off your commits (`git commit -s`) to certify the
  [Developer Certificate of Origin](https://developercertificate.org/): you wrote the change, or have the right
  to submit it under this project's license.
- Don't paste code from projects with incompatible licenses. Credit what you learned from in
  [ACKNOWLEDGEMENTS.md](ACKNOWLEDGEMENTS.md).

## Conduct

This project follows the [Contributor Covenant](CODE_OF_CONDUCT.md).
