#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 frty2.ai and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""razerctl.py - allow-listed vendor-channel tool for the Razer BlackWidow V4 Pro (1532:028d).

It speaks to the vendor HID interface (bInterfaceNumber 3, the 90-byte feature
report) through /dev/hidrawN with HIDIOCSFEATURE / HIDIOCGFEATURE. That is the
same USB traffic OpenRazer generates (SET_REPORT / GET_REPORT, wValue 0x0300,
wIndex 3) without a kernel module and without detaching anything, so the
keyboard keeps typing while we talk to it.

Safety properties, on purpose:
  * only commands in ALLOWLIST can be sent; there is no --raw mode
  * every command is read-only except set-mode-*, which changes a VOLATILE
    setting (device mode) that an unplug/replug resets
  * mutating commands need --yes
  * `session` enters driver mode and ALWAYS restores normal mode when it ends
    (timer, Ctrl-C or error)
  * every request/response is appended to probes/out/razerctl.log.jsonl

Usage (needs root for /dev/hidraw until a udev rule is in place):
  razerctl.py --dry-run fw-version        # print the bytes, touch nothing
  sudo razerctl.py fw-version | serial | get-mode
  sudo razerctl.py --yes set-mode-driver | set-mode-normal
  sudo razerctl.py --yes session 120      # driver mode for 120 s, then revert
"""
import argparse, datetime, fcntl, glob, json, os, signal, sys, time

VID, PID = 0x1532, 0x028D
VENDOR_IFACE = 3            # confirmed from the report descriptors (22-byte, usage page 0xFF00)
TRANSACTION_ID = 0x1F       # OpenRazer razerkbd_driver.c, case USB_DEVICE_ID_RAZER_BLACKWIDOW_V4_PRO
REPORT_LEN = 90
WAIT_S = 0.0006             # RAZER_BLACKWIDOW_CHROMA_WAIT_US = 600
STATUS = {0x00: 'NEW', 0x01: 'BUSY', 0x02: 'OK', 0x03: 'FAILURE', 0x04: 'TIMEOUT', 0x05: 'NOT_SUPPORTED'}
HERE = os.path.dirname(os.path.abspath(__file__))
LOG = os.path.join(HERE, '..', 'out', 'razerctl.log.jsonl')

# name: (command_class, command_id, data_size, args, mutates_state, description)
ALLOWLIST = {
    'fw-version':      (0x00, 0x81, 0x02, b'',               False, 'read firmware version (compare with lsusb bcdDevice)'),
    'serial':          (0x00, 0x82, 0x16, b'',               False, 'read serial string'),
    'get-mode':        (0x00, 0x84, 0x02, b'',               False, 'read device mode: 00=normal, 03=driver'),
    'set-mode-normal': (0x00, 0x04, 0x02, bytes([0x00, 0]),  True,  'volatile: firmware handles M-keys/dial itself'),
    'set-mode-driver': (0x00, 0x04, 0x02, bytes([0x03, 0]),  True,  'volatile: M-keys/dial/side keys reported raw to host'),
}
# Deliberately NOT here: mode 0x02 (OpenRazer explicitly blocks it), anything with
# varstore=STORE (0x01), any lighting/profile/macro-storage command, any
# command class we have not seen OpenRazer use on this PID.


def _ioc(dir_, type_, nr, size):
    return (dir_ << 30) | (size << 16) | (ord(type_) << 8) | nr


def HIDIOCSFEATURE(n): return _ioc(3, 'H', 0x06, n)
def HIDIOCGFEATURE(n): return _ioc(3, 'H', 0x07, n)


def crc(report):
    c = 0
    for b in report[2:88]:
        c ^= b
    return c


def build(name):
    cls, cid, dsize, args, _, _ = ALLOWLIST[name]
    r = bytearray(REPORT_LEN)
    r[0] = 0x00                      # status: new command
    r[1] = TRANSACTION_ID
    r[2:4] = b'\x00\x00'             # remaining packets (big endian)
    r[4] = 0x00                      # protocol type
    r[5] = dsize
    r[6] = cls
    r[7] = cid
    r[8:8 + len(args)] = args
    r[88] = crc(r)
    r[89] = 0x00
    return bytes(r)


def find_hidraw():
    for dev in sorted(glob.glob('/sys/class/hidraw/hidraw*')):
        try:
            uevent = open(f'{dev}/device/uevent').read()
            if f'HID_ID=0003:{VID:08X}:{PID:08X}'.lower() not in uevent.lower():
                continue
            ifn = int(open(f'{dev}/device/../bInterfaceNumber').read(), 16)
            rdesc_len = len(open(f'{dev}/device/report_descriptor', 'rb').read())   # sysfs st_size lies (4096)
        except OSError:
            continue
        if ifn == VENDOR_IFACE:
            if rdesc_len != 22:
                sys.exit(f'{dev}: interface {ifn} but report descriptor is {rdesc_len} bytes, expected 22. Refusing.')
            return '/dev/' + os.path.basename(dev)
    sys.exit('BlackWidow V4 Pro vendor interface not found (is it plugged in?)')


def log(entry):
    os.makedirs(os.path.dirname(LOG), exist_ok=True)
    entry['ts'] = datetime.datetime.now().isoformat(timespec='seconds')
    with open(LOG, 'a') as f:
        f.write(json.dumps(entry) + '\n')


def transact(fd, name, retries=6):
    req = build(name)
    buf = bytearray(1) + req                      # byte 0 = report ID 0, then the 90-byte report
    fcntl.ioctl(fd, HIDIOCSFEATURE(len(buf)), buf, True)
    resp = None
    for attempt in range(retries):
        time.sleep(WAIT_S if attempt == 0 else 0.005)
        rbuf = bytearray(1 + REPORT_LEN)          # byte 0 = report ID 0 on input; data lands at [1:]
        fcntl.ioctl(fd, HIDIOCGFEATURE(len(rbuf)), rbuf, True)
        resp = bytes(rbuf[1:])
        if resp[0] != 0x01:                       # not BUSY
            break
    status = STATUS.get(resp[0], f'0x{resp[0]:02x}')
    problems = []
    if (resp[6], resp[7]) != (req[6], req[7]):
        problems.append(f'response class/id {resp[6]:02x}/{resp[7]:02x} != request {req[6]:02x}/{req[7]:02x}')
    if crc(resp) != resp[88]:
        problems.append(f'response crc {resp[88]:02x} != computed {crc(resp):02x}')
    log({'cmd': name, 'req': req.hex(), 'resp': resp.hex(), 'status': status, 'problems': problems})
    return req, resp, status, problems


def decode(name, resp):
    a = resp[8:88]
    if name == 'fw-version':
        return f'firmware v{a[0]}.{a[1]}'
    if name == 'serial':
        return 'serial ' + a[:22].split(b'\x00')[0].decode('ascii', 'replace')
    if name == 'get-mode':
        return f'device mode 0x{a[0]:02x} ({ {0: "normal", 3: "driver"}.get(a[0], "?") }), param 0x{a[1]:02x}'
    return 'ok'


def run(fd, name, quiet=False):
    req, resp, status, problems = transact(fd, name)
    if not quiet:
        print(f'> {name:16s} {req.hex()}')
        print(f'< {status:16s} {resp.hex()}')
    for p in problems:
        print('  ! ' + p)
    if status != 'OK':
        print(f'  ! status {status}; stop and re-check (see docs/PROBE_PLAN.md stop rules)')
        return None
    text = decode(name, resp)
    print('  = ' + text)
    return resp


def mode_of(fd):
    resp = run(fd, 'get-mode', quiet=True)
    return None if resp is None else resp[8]


def session(fd, seconds):
    print(f'entering driver mode for {seconds} s; Ctrl-C ends early. Normal mode is restored on exit no matter what.')
    before = mode_of(fd)
    if before != 0x00:
        print(f'  ! device mode is already 0x{before:02x}, not 0x00. Replug the keyboard and retry.')
        return 1
    rc = 0
    try:
        run(fd, 'set-mode-driver')
        if mode_of(fd) != 0x03:
            print('  ! device did not report driver mode; aborting session')
            return 1
        t_end = time.time() + seconds
        while time.time() < t_end:
            left = int(t_end - time.time())
            print(f'\r  driver mode active, {left:4d} s left   ', end='', flush=True)
            time.sleep(1)
        print()
    except KeyboardInterrupt:
        print('\n  interrupted')
    except Exception as e:                       # noqa: BLE001
        print(f'\n  ! error during session: {e!r}')
        rc = 1
    finally:
        for attempt in range(3):
            try:
                run(fd, 'set-mode-normal')
                if mode_of(fd) == 0x00:
                    print('  restored normal mode')
                    break
            except Exception as e:               # noqa: BLE001
                print(f'  ! restore attempt {attempt + 1} failed: {e!r}')
            time.sleep(0.5)
        else:
            print('  ! could not confirm normal mode. Unplug and replug the keyboard; that resets it.')
            rc = 1
    return rc


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('command', choices=list(ALLOWLIST) + ['session', 'list'])
    ap.add_argument('seconds', nargs='?', type=int, default=120, help='session length (session only)')
    ap.add_argument('--dry-run', action='store_true', help='print the request bytes, do not open the device')
    ap.add_argument('--yes', action='store_true', help='required for commands that change device state')
    ap.add_argument('--device', help='override the auto-detected /dev/hidrawN (must be the vendor interface)')
    args = ap.parse_args()

    if args.command == 'list':
        for k, (cls, cid, ds, a, mut, desc) in ALLOWLIST.items():
            print(f'{k:16s} class=0x{cls:02x} id=0x{cid:02x} size=0x{ds:02x} args={a.hex() or "-":6s} {"MUTATES" if mut else "read   "} {desc}')
        return 0
    if args.dry_run:
        names = list(ALLOWLIST) if args.command == 'session' else [args.command]
        for n in names:
            print(f'{n:16s} {build(n).hex()}')
        return 0
    mutating = args.command == 'session' or ALLOWLIST.get(args.command, (0, 0, 0, b'', False, ''))[4]
    if mutating and not args.yes:
        sys.exit(f'{args.command} changes device state (volatile). Re-run with --yes if you mean it.')

    dev = args.device or find_hidraw()
    try:
        fd = os.open(dev, os.O_RDWR)
    except PermissionError:
        sys.exit(f'{dev}: permission denied. Run with sudo, or install the udev rule from docs/PROBE_PLAN.md.')
    print(f'using {dev} (interface {VENDOR_IFACE}, transaction id 0x{TRANSACTION_ID:02x})')
    def _stop(*_): raise KeyboardInterrupt
    for sig in (signal.SIGTERM, signal.SIGHUP):   # SIGHUP = terminal closed / logout
        signal.signal(sig, _stop)
    try:
        if args.command == 'session':
            return session(fd, args.seconds)
        return 0 if run(fd, args.command) is not None else 2
    finally:
        os.close(fd)


if __name__ == '__main__':
    sys.exit(main())
