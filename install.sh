#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 frty2.ai and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Build and install bwv4d for the current user. Asks for sudo once, for the udev rule only.
set -euo pipefail
cd "$(dirname "$0")"
make
install -Dm755 bwv4d ~/.local/bin/bwv4d
mkdir -p ~/.config/bwv4d
[ -f ~/.config/bwv4d/config ] || install -m644 packaging/config.example ~/.config/bwv4d/config
install -Dm644 packaging/bwv4d.service ~/.config/systemd/user/bwv4d.service
python3 packaging/merge-vscode-keybindings.py

echo "== udev rule (sudo)"
sudo install -m644 packaging/70-bwv4d.rules /etc/udev/rules.d/70-bwv4d.rules
sudo rm -f /etc/udev/rules.d/70-razer-bwv4pro.rules      # broader probe-phase rule, superseded
sudo udevadm control --reload
sudo udevadm trigger --subsystem-match=hidraw --subsystem-match=input --subsystem-match=misc
sleep 1

systemctl --user daemon-reload
systemctl --user enable --now bwv4d
sleep 1
systemctl --user --no-pager status bwv4d | head -12
echo
echo "Logs:      journalctl --user -u bwv4d -f"
echo "Config:    ~/.config/bwv4d/config   (then: systemctl --user restart bwv4d)"
echo "Stop:      systemctl --user stop bwv4d   (keyboard returns to normal mode)"
