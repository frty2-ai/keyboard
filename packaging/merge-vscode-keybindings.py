#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 frty2.ai and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Add bwv4d's six private chords to VS Code's keybindings.json (backup first, no duplicates)."""
import json, os, shutil, sys, time
here = os.path.dirname(os.path.abspath(__file__))
target = os.path.expanduser(sys.argv[1] if len(sys.argv) > 1 else '~/.config/Code/User/keybindings.json')
ours = json.load(open(os.path.join(here, 'vscode-keybindings.json')))
cur = []
if os.path.exists(target):
    text = open(target).read()
    try:
        cur = json.loads(text) if text.strip() else []
    except json.JSONDecodeError:
        sys.exit(f'{target} has comments or trailing commas; add packaging/vscode-keybindings.json entries by hand.')
    shutil.copy2(target, f'{target}.bak-{time.strftime("%Y%m%d-%H%M%S")}')
have = {(b.get('key'), b.get('command'), b.get('when')) for b in cur}
add = [b for b in ours if (b['key'], b['command'], b.get('when')) not in have]
os.makedirs(os.path.dirname(target), exist_ok=True)
with open(target, 'w') as f:
    json.dump(cur + add, f, indent=4)
    f.write('\n')
print(f'{target}: added {len(add)} bindings ({len(ours) - len(add)} already present)')
