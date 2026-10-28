#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 frty2.ai and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Remove bwv4d. Leaves ~/.config/bwv4d and the VS Code keybindings (harmless without the daemon).
set -u
systemctl --user disable --now bwv4d 2>/dev/null
~/.local/bin/bwv4d --restore 2>/dev/null
rm -f ~/.local/bin/bwv4d ~/.config/systemd/user/bwv4d.service
systemctl --user daemon-reload
sudo rm -f /etc/udev/rules.d/70-bwv4d.rules && sudo udevadm control --reload && sudo udevadm trigger
echo "removed; keyboard is in normal mode"
