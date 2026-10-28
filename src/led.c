/* SPDX-FileCopyrightText: 2026 frty2.ai and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
/* Dial LED through the standard HID LampArray on interface 4 (feature reports 1, 4, 5, 6).
 * Volatile: AutonomousMode=1 (or a replug) hands lighting back to the firmware. */
#define _GNU_SOURCE
#include "bwv4d.h"
#include <fcntl.h>
#include <linux/hidraw.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int fd = -1, nlamps;

static int put(const uint8_t *b, size_t n) { return fd < 0 ? -1 : ioctl(fd, HIDIOCSFEATURE(n), b); }

int led_open(const char *path)
{
	if (fd >= 0) return 0;
	if ((fd = open(path, O_RDWR | O_CLOEXEC)) < 0) return -1;
	uint8_t a[23] = {1};
	if (ioctl(fd, HIDIOCGFEATURE(sizeof a), a) < 0) { close(fd); fd = -1; return -1; }
	nlamps = a[1] | (a[2] << 8);
	return nlamps > 0 ? 0 : -1;
}

void led_host(bool on)
{
	uint8_t b[2] = {6, on ? 0 : 1};                          /* AutonomousMode */
	put(b, sizeof b);
}

void led_fill(uint32_t rgb)
{
	uint8_t b[10] = {5, 1, 0, 0, (uint8_t)(nlamps - 1), (uint8_t)((nlamps - 1) >> 8),
	                 (uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb, 255};
	put(b, sizeof b);
	usleep(35000);                                            /* device minimum update interval: 33 ms */
}

void led_lamp(int id, uint32_t rgb)
{
	uint8_t b[51] = {4, 1, 1};
	b[3] = (uint8_t)id;
	b[4] = (uint8_t)(id >> 8);
	b[19] = (uint8_t)(rgb >> 16);
	b[20] = (uint8_t)(rgb >> 8);
	b[21] = (uint8_t)rgb;
	b[22] = 255;
	put(b, sizeof b);
}

void led_close(void)
{
	if (fd < 0) return;
	led_host(false);
	close(fd);
	fd = -1;
}
