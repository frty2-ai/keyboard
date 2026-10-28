# Security policy

`bwv4d` reads the raw HID stream of a keyboard, which includes everything the user types, and it can inject
keystrokes through uinput. We treat any bug that could leak keystrokes or clipboard contents, inject input an
attacker controls, or widen device permissions as a security issue.

## Reporting a vulnerability

Please **don't open a public issue.** Report it privately through GitHub:
[Report a vulnerability](https://github.com/frty2-ai/keyboard/security/advisories/new).

Include what you found, how to reproduce it and the version or commit. We aim to acknowledge reports within
7 days and to agree a disclosure date with you once a fix is ready.

## Supported versions

Only the latest commit on `main` gets security fixes while the project is at 0.x.

## Scope

In scope: the daemon (`src/`), the udev rule and systemd unit (`packaging/`), the install scripts and the probe tools.

Out of scope: the keyboard's firmware, Razer software, and the X11 security model itself. On X11 any client can read
input and clipboards; `bwv4d` does not make that better or worse.
