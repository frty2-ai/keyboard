/* SPDX-FileCopyrightText: 2026 frty2.ai and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
/* Device discovery (sysfs) and the vendor feature-report channel on interface 3. */
#define _GNU_SOURCE
#include "bwv4d.h"
#include <dirent.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define HID_ID "HID_ID=0003:00001532:0000028D"

static int slurp(const char *p, char *buf, size_t n)
{
	int fd = open(p, O_RDONLY | O_CLOEXEC);
	if (fd < 0) return -1;
	ssize_t r = read(fd, buf, n - 1);
	close(fd);
	if (r < 0) return -1;
	buf[r] = 0;
	return (int)r;
}

static int event_node(const char *hidraw, char *out, size_t n)
{
	char p[256];
	snprintf(p, sizeof p, "/sys/class/hidraw/%s/device/input", hidraw);
	DIR *d = opendir(p);
	if (!d) return -1;
	int ok = -1;
	for (struct dirent *e; ok && (e = readdir(d));) {
		if (strncmp(e->d_name, "input", 5)) continue;
		char q[512];
		snprintf(q, sizeof q, "%s/%s", p, e->d_name);
		DIR *d2 = opendir(q);
		if (!d2) continue;
		for (struct dirent *f; (f = readdir(d2));)
			if (!strncmp(f->d_name, "event", 5)) {
				snprintf(out, n, "/dev/input/%s", f->d_name);
				ok = 0;
				break;
			}
		closedir(d2);
	}
	closedir(d);
	return ok;
}

/* Fills the three paths; returns 0 only if all three were found. */
int device_find(char *kbd, char *vendor, char *mouse_ev, char *lamp, size_t n)
{
	DIR *d = opendir("/sys/class/hidraw");
	if (!d) return -1;
	kbd[0] = vendor[0] = mouse_ev[0] = lamp[0] = 0;
	char p[256], buf[1024];
	for (struct dirent *e; (e = readdir(d));) {
		if (strncmp(e->d_name, "hidraw", 6)) continue;
		snprintf(p, sizeof p, "/sys/class/hidraw/%s/device/uevent", e->d_name);
		if (slurp(p, buf, sizeof buf) < 0 || !strcasestr(buf, HID_ID)) continue;
		snprintf(p, sizeof p, "/sys/class/hidraw/%s/device/../bInterfaceNumber", e->d_name);
		if (slurp(p, buf, sizeof buf) < 0) continue;
		int ifn = (int)strtol(buf, NULL, 16);
		if (ifn == 1) snprintf(kbd, n, "/dev/%s", e->d_name);
		else if (ifn == 2) event_node(e->d_name, mouse_ev, n);
		else if (ifn == 4) snprintf(lamp, n, "/dev/%s", e->d_name);   /* optional: LampArray */
		else if (ifn == 3) {
			/* safety: only the 22-byte vendor descriptor seen in probing */
			snprintf(p, sizeof p, "/sys/class/hidraw/%s/device/report_descriptor", e->d_name);
			char rd[512];
			if (slurp(p, rd, sizeof rd) == 22) snprintf(vendor, n, "/dev/%s", e->d_name);
		}
	}
	closedir(d);
	return (kbd[0] && vendor[0] && mouse_ev[0]) ? 0 : -1;
}

static uint8_t crc(const uint8_t *r)
{
	uint8_t c = 0;
	for (int i = 2; i < 88; i++) c ^= r[i];
	return c;
}

/* Same framing as probes/tools/razerctl.py (verified on this unit in P4/P5). */
static int vendor_cmd(int fd, uint8_t cls, uint8_t id, uint8_t size, const uint8_t *args, int nargs, uint8_t *resp)
{
	uint8_t buf[91] = {0}, *r = buf + 1;
	r[1] = 0x1f;            /* transaction id for this PID */
	r[5] = size;
	r[6] = cls;
	r[7] = id;
	if (nargs) memcpy(r + 8, args, nargs);
	r[88] = crc(r);
	if (ioctl(fd, HIDIOCSFEATURE(sizeof buf), buf) < 0) return -1;
	for (int i = 0; i < 6; i++) {
		usleep(i ? 5000 : 600);
		uint8_t g[91] = {0};
		if (ioctl(fd, HIDIOCGFEATURE(sizeof g), g) < 0) return -1;
		memcpy(resp, g + 1, 90);
		if (resp[0] != 0x01) break;         /* not BUSY */
	}
	return (resp[0] == 0x02 && resp[6] == cls && resp[7] == id && crc(resp) == resp[88]) ? 0 : -1;
}

int vendor_set_mode(int fd, int mode)
{
	if (mode != 0 && mode != 3) return -1;  /* never 0x02 */
	uint8_t a[2] = {(uint8_t)mode, 0}, resp[90];
	return vendor_cmd(fd, 0x00, 0x04, 0x02, a, 2, resp);
}

int vendor_get_mode(int fd)
{
	uint8_t resp[90];
	return vendor_cmd(fd, 0x00, 0x84, 0x02, NULL, 0, resp) ? -1 : resp[8];
}
