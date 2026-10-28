# Acknowledgements

`bwv4d` and the probe tools were written from scratch for this project. No source code from the projects below
was copied into this repository. We learned facts about the hardware from them (protocol values, report layouts,
key codes) and design ideas. Every fact was then checked on a real keyboard before anything relied on it. The
lab notebook is [docs/PROBE_PLAN.md](docs/PROBE_PLAN.md).

Thank you to everyone below. If we have credited you wrongly or left you out, please open an issue.

## OpenRazer

<https://github.com/openrazer/openrazer>, GPL-2.0, by the OpenRazer contributors.

The open-source Razer driver for Linux, and the main reason this project took days rather than weeks. From
reading `driver/razerkbd_driver.c`, `razercommon.c` and `razerchromacommon.c` (master, October 2026) we learned:

- the vendor control channel: a 90-byte feature report on USB interface 3 (SET_REPORT/GET_REPORT, `wValue 0x0300`),
  its frame layout and XOR checksum
- the transaction ID for this keyboard, `0x1F`
- the device-mode command (class `0x00`, id `0x04`): `0x00` = normal, `0x03` = driver, and that mode `0x02` must
  never be sent
- the driver-mode special-key code table in report ID 4: `0x20`-`0x24` M1-M5, `0x60` dial press, `0x63`-`0x65`
  side keys, `0x50`-`0x55` media keys, `0x01` Fn
- that the volume roller arrives as the generic-desktop wheel on interface 2
- the lesson from issues [#2264](https://github.com/openrazer/openrazer/issues/2264) and
  [#2377](https://github.com/openrazer/openrazer/issues/2377): parse these raw reports in userspace, not in the kernel

We release this project under GPL-2.0-or-later, so it stays license-compatible with OpenRazer.

## OpenRazer issue #2228 and its contributors

<https://github.com/openrazer/openrazer/issues/2228>, "Razer Command Dial (on Razer BlackWidow v4 Pro) doesn't work
in Driver Mode", opened by [therob89](https://github.com/therob89).

[thedustinwidmann-lang](https://github.com/thedustinwidmann-lang) posted key captures and a driver-mode protocol
reference for the Command Dial: press arrives as `04 60` on interface 1, rotation as one Consumer AC Pan report per
detent on interface 2, and in driver mode the firmware applies no mode logic of its own, so dial "modes" are
entirely up to the host. That reference is why this project could start from verification instead of
reverse engineering. Thanks also to the OpenRazer maintainers in that thread, who asked for the right captures.

## detentd

<https://gitlab.com/dustinwidmann/detentd>, MIT, by Dustin Widmann.

A userspace Command Dial daemon built on top of OpenRazer. We used no code from it, but borrowed these ideas:

- put the dial's mode logic in a userspace daemon
- grab only the dial/roller event node and never the keyboard nodes, then re-inject through uinput
- grant device access with a udev `uaccess` rule, not membership of the `input` group
- a Synapse-style table of dial profiles, each with its own colour

## Tools and standards

- [hid-tools](https://gitlab.freedesktop.org/libevdev/hid-tools) (`hid-recorder`) and
  [evtest](https://gitlab.freedesktop.org/libevdev/evtest): every observation in the probe plan was captured with them.
- [USB HID Usage Tables](https://usb.org/document-library/hid-usage-tables-15): the Consumer page (AC Pan) and the
  Lighting and Illumination page (LampArray), which drive the dial LED with no vendor protocol at all.
- [Contributor Covenant](https://www.contributor-covenant.org/), version 2.1, for our code of conduct.
