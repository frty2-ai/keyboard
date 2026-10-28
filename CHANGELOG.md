# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.1.0] - 2026-10-28

First public release.

### Added

- `bwv4d` daemon for the Razer BlackWidow V4 Pro (`1532:028d`): driver mode on connect, normal mode on exit,
  crash and unplug.
- M1-M5 and side keys with app-aware defaults (terminals, VS Code, everything else) and per-key overrides.
- Command Dial profiles: Skills, Clipboard history, VS Code windows, App switcher, Tabs.
- Media keys, mute and volume roller translated in driver mode.
- Dial LED colour per profile through the standard HID LampArray interface.
- Click-through corner popup (no notification daemon, no sound).
- udev `uaccess` rule, systemd user unit, install and uninstall scripts.
- Probe scripts and an allow-listed vendor-channel tool, with the full verification record in `docs/PROBE_PLAN.md`.

[Unreleased]: https://github.com/frty2-ai/keyboard/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/frty2-ai/keyboard/releases/tag/v0.1.0
