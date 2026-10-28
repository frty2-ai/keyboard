#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 frty2.ai and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""lamparray.py - probe the BlackWidow V4 Pro's standard HID LampArray (interface 4).

  lamparray.py info            read-only: array attributes + every lamp (position, purpose, key binding)
  lamparray.py --yes flash ID  host mode for 3 s: lamp ID green, others untouched-by-us, then autonomous again

Reports (from the interface-4 descriptor, see probes/out/baseline/if4-hidraw8.rdesc.txt):
  1 LampArrayAttributes (get)   2 LampAttributesRequest (set lamp id)   3 LampAttributesResponse (get)
  4 LampMultiUpdate (set)       5 LampRangeUpdate (set)                  6 LampArrayControl: AutonomousMode
All of it is volatile: replug or AutonomousMode=1 hands lighting back to the firmware.
"""
import fcntl, glob, json, os, struct, sys, time

def ioc(d, n, size): return (d << 30) | (size << 16) | (ord('H') << 8) | n
def node():
    for h in sorted(glob.glob('/sys/class/hidraw/hidraw*')):
        try:
            if '1532:0000028D' in open(f'{h}/device/uevent').read() and open(f'{h}/device/../bInterfaceNumber').read().strip() == '04':
                return '/dev/' + os.path.basename(h)
        except OSError: pass
    sys.exit('interface 4 not found')
def get(fd, rid, n):
    b = bytearray(n + 1); b[0] = rid
    fcntl.ioctl(fd, ioc(3, 0x07, len(b)), b, True); return bytes(b)
def put(fd, rid, payload):
    b = bytearray([rid]) + payload
    fcntl.ioctl(fd, ioc(3, 0x06, len(b)), b, True)
PURP = {1: 'control', 2: 'accent', 4: 'branding', 8: 'status', 16: 'illumination', 32: 'presentation'}

def info(fd):
    a = get(fd, 1, 22)
    count, w, h, d, kind, upd = struct.unpack_from('<H5I', a, 1)
    print(f'lamps={count} box={w}x{h}x{d} um kind={kind} min_update={upd} us')
    lamps = []
    for i in range(count):
        put(fd, 2, struct.pack('<H', i))
        r = get(fd, 3, 28)
        lid, x, y, z, lat, purp = struct.unpack_from('<H5I', r, 1)
        rc, gc, bc, ic, prog, key = r[23:29]
        lamps.append(dict(id=lid, x=x, y=y, z=z, purpose=purp, key=key, levels=(rc, gc, bc, ic), prog=prog))
    nonkey = [l for l in lamps if not l['key']]
    print(f'{len(lamps) - len(nonkey)} lamps bound to keys, {len(nonkey)} not bound to a key:')
    for l in nonkey:
        print(f"  id={l['id']:3} x={l['x']:7} y={l['y']:7} purpose={'+'.join(v for k, v in PURP.items() if l['purpose'] & k) or l['purpose']} levels={l['levels']}")
    k = lamps[:3]
    print('first key lamps:', [(l['id'], hex(l['key']), l['x'], l['y']) for l in k])
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'out', 'lamparray.json')
    json.dump(lamps, open(out, 'w'), indent=1); print('saved', os.path.normpath(out))

def flash(fd, lid):
    put(fd, 6, bytes([0]))                                # host mode
    try:
        put(fd, 4, bytes([1, 1]) + struct.pack('<8H', lid, *[0] * 7) + bytes([0, 255, 0, 255]) + bytes(28))
        time.sleep(3)
    finally:
        put(fd, 6, bytes([1]))                            # back to firmware lighting
        print('autonomous mode restored')

fd = os.open(node(), os.O_RDWR)
if len(sys.argv) > 1 and sys.argv[1] == 'info': info(fd)
elif len(sys.argv) > 3 and sys.argv[1] == '--yes' and sys.argv[2] == 'flash': flash(fd, int(sys.argv[3]))
else: print(__doc__)
