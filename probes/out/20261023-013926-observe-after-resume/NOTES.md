# P6a suspend/resume, 2026-10-23 01:36-01:40 (INCONCLUSIVE BY DESIGN: suspend is masked)

Session: driver mode set 01:36:36, get-mode 03 at 01:39:26, restored 00 at 01:40:13. All OK.
`systemctl suspend` was refused twice: journal says "Unit suspend.target is masked, refusing operation".
/sys/power/suspend_stats/success = 0. So the machine never slept; this capture only shows driver mode
holding for about 3 minutes of normal use.

Capture: M1..M5 -> ID4 20..24, dial CW x3 -> IF2 pan +1 x3. Same as P5.

Verdict: suspend handling is not needed on this machine as configured (suspend.target masked).
The daemon should still re-apply driver mode on every (re)appearance of the device via udev/hotplug,
which also covers resume if suspend is ever unmasked.

Addendum: after the capture the user logged out from the Ubuntu menu as a suspend substitute. Logout does not
touch USB power, so it tests nothing about the keyboard. The session tool restored normal mode at 01:40:13
(log). Fix applied afterwards: razerctl.py now also treats SIGHUP (terminal closed / logout) as a stop signal,
so the restore runs in that case too. Daemon requirement: restore normal mode on exit, so the login screen
gets working media keys back.
